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

// SVF with its coefficients cached at set() (lab::dsp::Svf divides every
// sample); the same TPT structure, so the same sound.
struct FastSvf {
    float ic1 = 0, ic2 = 0, k = 1.4f, a1 = 0.9f, a2 = 0.09f, a3 = 0.009f;
    float lp = 0, bp = 0, hp = 0;
    void set(float hz, float q) {
        hz = hz < 20.0f ? 20.0f : (hz > 16000.0f ? 16000.0f : hz);
        const float x = 3.14159265f * hz * (1.0f / 44100.0f);
        const float x2 = x * x;
        const float g = x * (1.0f + x2 * (0.33333f + x2 * 0.13333f)) / (1.0f - x2 * 0.04f);   // tan(x)
        k = 1.0f / (q < 0.5f ? 0.5f : q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void process(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
};

// Linear ADSR (times in seconds) advanced at control rate: advance(n) moves
// it n samples at once; the voices interpolate between the control points.
struct CtlEnv {
    enum Stage : uint8_t { kIdle, kAttack, kDecay, kSustain, kRelease };
    Stage st = kIdle;
    float v = 0.0f, a = 0.005f, d = 0.2f, s = 0.7f, r = 0.3f;
    void gate(bool on) { st = on ? kAttack : (st == kIdle ? kIdle : kRelease); }
    bool active() const { return st != kIdle; }
    float advance(int n) {
        float t = float(n);
        while (t > 0.0f) {
            switch (st) {
                case kAttack: {
                    const float rate = 1.0f / ((a > 0.0005f ? a : 0.0005f) * 44100.0f);
                    const float need = (1.0f - v) / rate;
                    if (need > t) { v += rate * t; t = 0.0f; } else { v = 1.0f; t -= need; st = kDecay; }
                    break;
                }
                case kDecay: {
                    const float rate = (1.0f - s) / ((d > 0.0005f ? d : 0.0005f) * 44100.0f);
                    if (rate <= 0.0f || v <= s) { v = s; st = kSustain; break; }
                    const float need = (v - s) / rate;
                    if (need > t) { v -= rate * t; t = 0.0f; } else { v = s; t -= need; st = kSustain; }
                    break;
                }
                case kRelease: {
                    const float rate = 1.0f / ((r > 0.0005f ? r : 0.0005f) * 44100.0f);
                    const float need = v / rate;
                    if (need > t) { v -= rate * t; t = 0.0f; } else { v = 0.0f; t = 0.0f; st = kIdle; }
                    break;
                }
                case kSustain: t = 0.0f; break;
                default: v = 0.0f; t = 0.0f; break;
            }
        }
        return v;
    }
};

struct SliceVoice {
    const int16_t* data = nullptr;
    uint32_t frames = 0;
    float pos = 0, rate = 1, start = 0, end = 0, loopLen = 0;
    float gain = 0, fade = 0, fadeStep = 0;
    bool active = false, rev = false, stutter = false;
};

struct BassVoice {
    lab::dsp::Osc o1, o2, o3;
    FastSvf f, f2, f3;
    CtlEnv amp;
    float e = 0, de = 0;   // the envelope between control points
    float freq = 55, target = 55, glide = 0.999f, glide16 = 0.98f;
    float fenv = 0, lfo = 0, vel = 1;
    float pc = 0, pm = 0, fb = 0, subPh = 0, dive = 1;
    bool gate = false;
};

struct LeadVoice {
    float freq = 220, target = 220, glideK = 0.0f, glide16 = 1.0f;
    float ph = 0, ph2 = 0, vibPh = 0, vibAmt = 0;
    CtlEnv amp;
    float e = 0, de = 0;
    FastSvf f;
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
    FastSvf brkF_;
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
