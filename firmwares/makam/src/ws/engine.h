// MAKAM -- the audio engine: usul sequencer, break slicer with cut
// procedures and phrase-end fills, bass, lead, dub sends (own delay, the
// OpenTracker reverb), ducking, stems, MIDI out, modulation sources.
// render() runs in the audio interrupt; everything else in the main loop.
#pragma once

#include <cstdint>

#include "audio/effects.h"
#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/kit.h"
#include "ws/song.h"
#include "ws/theory.h"

namespace makam {

// Main loop -> audio: live notes (pads, MIDI in) and performance edits.
struct LiveEv {
    enum Type : uint8_t { kLeadOn, kLeadOff, kBassOn, kBassOff, kSlice, kAllOff };
    uint8_t type = kLeadOn;
    int8_t deg = 0;      // makam degree (notes) / slice (kSlice)
    uint8_t vel = 100;
    uint8_t id = 0;      // pad id for note off
};

// Audio -> main loop: what is playing (single-word fields).
struct PlayState {
    volatile int32_t unit = -1;     // units since Play
    volatile int32_t step = 0;      // unit in the usul cycle
    volatile int32_t cycle = 0;     // cycles since Play
    volatile int32_t section = 0;   // section index (song mode)
    volatile int32_t secCycle = 0;  // cycle inside the section
    volatile int32_t slice = -1;    // last break slice
    volatile int32_t sliceHit = 0;  // counts break hits
    volatile int32_t leadDeg = -100, bassDeg = -100;   // sounding degrees (-100 none)
    volatile int32_t leadComma = 0;
    volatile int32_t phraseStep = 0;
    volatile uint32_t phraseWraps = 0;
    volatile int32_t fill = 0;       // phrase-end fill level now: 0 none, 1 quarter, 2 half, 3 phrase / section end
    volatile int32_t fillStyle = 0;  // FillStyle of the current fill
    volatile int32_t cut = 0;        // CutMode playing
    volatile float meter[4] = {};    // channel peaks (break, bass, lead, fx)
    volatile float slicePos = 0.0f;  // 0..1 position in the break (play cursor)
};
enum FillStyle : int { kFillRolls, kFillStutter, kFillReverse, kFillDrop, kFillBuild, kFillStyles };
extern const char* const kFillStyleNames[kFillStyles];
extern const char* const kCutNames[kCutModes];

struct SliceVoice {
    const int16_t* data = nullptr;
    uint32_t frames = 0;
    float pos = 0, rate = 1, start = 0, end = 0, loopLen = 0;
    float gain = 0, fade = 0, fadeStep = 0;
    bool active = false, rev = false, stutter = false;
};

struct BassVoice {
    lab::dsp::Osc o1, o2, o3;
    lab::dsp::Svf f, f2, f3;
    lab::dsp::Adsr amp;
    float freq = 55, target = 55, glide = 0.999f, glide16 = 0.98f;
    float fenv = 0, lfo = 0, vel = 1;
    float pc = 0, pm = 0, fb = 0, subPh = 0, dive = 1;
    bool gate = false;
};

struct LeadVoice {
    float freq = 220, target = 220, glideK = 0.0f, glide16 = 1.0f;
    float ph = 0, ph2 = 0, vibPh = 0, vibAmt = 0;
    lab::dsp::Adsr amp;
    lab::dsp::Svf f;
    float* ks = nullptr;            // Karplus-Strong line (kKsLen)
    float* ks2 = nullptr;
    uint32_t ksW = 0;
    float ksLp = 0, ksLp2 = 0, ksDecay = 0.996f;
    int32_t burst = 0;              // samples of excitation left
    float burstLp = 0, burstK = 0.5f;
    int32_t graceLeft = 0;          // samples of the grace note
    float graceFreq = 0;
    float vel = 1, age = 0;
    uint8_t padId = 255;
    bool gate = false, active = false;
    int deg = 0;
};

// Stereo dub delay (float lines in the arena): Clean, Tape (wow + flutter,
// saturated dark feedback), Lo-fi (decimated, 6-bit feedback), Ping-pong.
class DubDelay {
public:
    static constexpr uint32_t kLen = 131072;   // 2.97 s per side
    bool init(lab::Arena& a);
    void setTime(float samples) { target_ = samples < 16.0f ? 16.0f : (samples > float(kLen - 64) ? float(kLen - 64) : samples); }
    void setFeedback(float fb) { fb_ = fb; }
    void setType(int t) { type_ = t; }
    // mono in, stereo out, kBlock frames; false = bypassed (outputs zero)
    bool process(const float* in, float* outL, float* outR);

private:
    float* l_ = nullptr;
    float* r_ = nullptr;
    uint32_t w_ = 0;
    float time_ = 11025.0f, target_ = 11025.0f, fb_ = 0.4f;
    int type_ = 0;
    float lpL_ = 0, lpR_ = 0, hpL_ = 0, hpR_ = 0, holdL_ = 0, holdR_ = 0;
    float wow_ = 0, flutter_ = 0;
    uint32_t hold_ = 0, quiet_ = kLen;
};

class Engine {
public:
    static constexpr int kLeadVoices = 3;
    static constexpr uint32_t kKsLen = 2048;

    bool init(lab::Arena& a, lab::Context* ctx);
    // Pointers set by the workstation (main loop).
    Song* song = nullptr;
    Kit* kits = nullptr;            // kKits + 1 kits
    volatile bool wavReady = false; // kit 3 holds a loaded WAV

    // Performance (held pads on the Break page), written by the main loop.
    volatile int perfRoll = 0;      // 0, 2, 3, 4, 6
    volatile bool perfChaos = false, perfRev = false, perfStutter = false, perfHalf = false, perfMute = false;

    lab::SpscRing<LiveEv, 64> live;
    PlayState st;

    // Modulation sources (LABKIT, read once per block by the Shell).
    float srcUsul = 0, srcDum = 0, srcTek = 0, srcSlice = 0, srcEnergy = 0, srcSeyir = 0, srcSection = 0, srcFill = 0;

    void render(const lab::Transport& t, float* l, float* r);
    void reset() { resetReq = true; }
    volatile bool resetReq = false;
    // Pitch of a degree (absolute commas, practice intonation applied) and its frequency.
    int degreeToComma(int deg, int octaveShift) const;
    float commaToHz(int comma) const;

private:
    struct CutStep {
        int8_t src;     // source unit in the cycle (-1: let the last cut play on)
        uint8_t roll;   // hits in the unit
        int8_t pitch;
        uint8_t rev;
    };
    void stepEvent(int32_t unit, int len);
    void subTick(int32_t unit, int sub, int swingTicks);
    void triggerSlice(int slice, int pitch, bool rev, bool stutter, float vel);
    void triggerPos(float pos, float sliceLen, int pitch, bool rev, bool stutter, float vel);
    void planCut(int mode, int len, int chaos);
    void learnDna();
    void bassNote(int deg, int flags, float vel);
    void bassOff();
    void leadNote(int deg, int flags, float vel, uint8_t padId);
    void leadOff(uint8_t padId);
    void allOff();
    void chokeBreak();
    void sendNote(bool on, int chOff, int comma, uint8_t vel, uint8_t* lastNote);
    void renderSegment(uint32_t i0, uint32_t i1);
    void updateFx(float bpm);
    const Kit& kit() const;

    lab::Context* ctx_ = nullptr;
    DubDelay delay_;
    ot::audio::Reverb reverb_;
    SliceVoice sv_[2];
    int svNext_ = 0;
    BassVoice bass_;
    LeadVoice lead_[kLeadVoices];
    int leadNext_ = 0;
    lab::dsp::Svf brkF_;
    lab::dsp::Rng rng_{0xA5A5A5u};
    float duck_ = 0;
    // per-block working buffers
    float bL_[lab::kBlock], bR_[lab::kBlock];    // break stem
    float sL_[lab::kBlock], sR_[lab::kBlock];    // bass stem
    float lL_[lab::kBlock], lR_[lab::kBlock];    // lead stem
    float fL_[lab::kBlock], fR_[lab::kBlock];    // fx stem
    float dIn_[lab::kBlock], rIn_[lab::kBlock];
    float brk_[lab::kBlock], bas_[lab::kBlock], led_[lab::kBlock];
    float pk_[4] = {};
    // current step state (sequencer)
    int curRoll_ = 1, curSlice_ = -1, curPitch_ = 0;
    float curPos_ = -1.0f, curLen_ = 0.0f;   // >= 0: the hit was a position cut (BBCut)
    bool curRev_ = false, curStutter_ = false, curOn_ = false;
    int32_t lastTick_ = -1;
    int section_ = 0, secStart_ = 0;
    int lastFxTime_ = -1, lastBpmX10_ = -1, lastRvbType_ = -1;
    float lastSize_ = -1, lastDamp_ = -1;
    float tempoRate_ = 1.0f;
    float bpm_ = 172.0f;
    uint8_t midiBass_ = 255, midiLead_ = 255;
    // cut procedures
    CutStep plan_[kSteps];
    int planMode_ = -1;
    int fillStyle_ = 0, fillSrc_ = -1;
    float fillPos_ = -1.0f;
    int lastHitSlice_ = 0;
    // DNA (break groove learned as a class Markov chain)
    const Kit* dnaKit_ = nullptr;
    uint32_t dnaFrames_ = 0;
    uint8_t dnaNext_[4][4] = {};     // class transition counts
    int dnaPrev_ = 0;
    // gated reverb
    float gate_ = 0;
    int32_t gateHold_ = 0;
    float energy_ = 0;
    // cached per block
    float bassCut_ = 1000, bassQ_ = 1;
};

}  // namespace makam
