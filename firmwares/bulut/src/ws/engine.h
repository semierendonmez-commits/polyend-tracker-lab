// BULUT audio engine: four channels of grains / particles, their buffers and
// sources, the spectral freeze loops, gesture playback, the insert chains, the
// send effects, resample capture and the modulation sources BULUT exports.
//
// Threads: render() runs in the audio interrupt. The LABKIT registry writes the
// effective parameter values (ChParams) before each block; the main loop writes
// ChState, emitters (pads / MIDI), sources and requests. Gesture playback posts
// its parameter moves to the main loop (paramEvents), which applies them
// through the registry. Gestures are triple-buffered (gestIdle / postGesture).
#pragma once

#include <cstdint>

#include "audio/effects.h"
#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/bulut.h"
#include "ws/fx.h"

namespace bulut {

// One emitter = one source of grains: a held pad, a MIDI note, a gesture
// "ghost" pad, or the cloud's own drone.
struct Emitter {
    volatile uint8_t on;
    uint8_t pad;          // 0..47, 0xFF = MIDI note / drone
    uint8_t vel;          // 1..127
    uint8_t note;         // MIDI note (pad == 0xFF)
    float semis;          // pitch offset (MIDI note)
    float acc;            // samples until the next grain
    float head;           // TAPE: playhead (absolute buffer index, fractional)
    float loopStart;      // TAPE: where the playhead loops back to
    uint8_t fresh;        // TAPE / BEADS: just pressed (seed)
    uint8_t reserved[3];
};

// Pad grid -> emitter (also used by the UI and the tests):
//   column 0..11 -> position inside the region (left = start / oldest, right = end / newest);
//                   QUANTA synthetic particles: pitch steps over FREQ; TAPE: slice start
//   row 0 (top) .. 3 -> density offset +0.30, +0.12, -0.08, -0.25;
//                   TAPE: loop length 1, 1/2, 1/4, 1/8 of LENGTH (stutter)
inline float padPos(int pad) { return (float(pad % 12) + 0.5f) / 12.0f; }
inline float padSlice(int pad) { return float(pad % 12) / 12.0f; }
inline float padDensOff(int pad) {
    static const float k[4] = {0.30f, 0.12f, -0.08f, -0.25f};
    return k[(pad / 12) & 3];
}
inline float padTapeLen(int pad) {
    static const float k[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    return k[(pad / 12) & 3];
}

enum GrainKind : uint8_t { kGBuf = 0, kGSine, kGPulsar, kGTrain };

struct Grain {
    const int16_t* src;    // kGBuf: channel buffer or freeze loop
    const float* win;      // window table (kWin + 1), null = Beads envelope
    uint32_t mask;         // index mask of src
    uint32_t pos;          // read position, 12 fractional bits
    int32_t inc;           // per sample, same fixed point (negative = reverse)
    float wph, winc;       // envelope phase 0..kWin
    float gl, gr;          // channel gains (include 1/32768 for buffers)
    uint32_t left;         // samples to go
    uint32_t delay;        // samples to wait in its first block
    float c, s, cr, sr;    // synthetic: quadrature oscillator and its rotation
    float c2, s2, cr2, sr2;   // trainlet denominator / pulsar envelope
    float glide, w;        // glisson
    uint32_t ph, inc32;    // pulsar period phase
    uint32_t duty, plen;   // pulsar duty / trainlet harmonics
    float env, decay, norm;   // trainlet
    float envA, envS, envSm;   // Beads envelope: attack, sustain fractions, smoothness
    uint8_t kind, cloud, live, reserved;
};

constexpr int kWin = 512;           // window table length (+1 guard)
constexpr int kTexWindows = 16;     // texture morph: tukey (narrow fades) -> hann -> narrowing gauss
constexpr int kWinGabor = 16, kWinPerc = 17, kWinExpodec = 18, kWinRexpodec = 19, kWindows = 20;
constexpr uint32_t kLiveSpan = 8u * 44100u;   // live buffer: the region spans the last 8 s
constexpr uint32_t kMixHist = 16384;          // master history ring (freeze from the mix)
// CPU budget (make lab-gate). Everything a block may cost beyond the fixed
// part is counted in grain units: each running grain is one, the insert
// chains and generators are worth their measured cost (fx.cpp chainUnits).
constexpr int kMaxGrains = 24;                // running grains, hard cap
constexpr int kDefaultGrains = 20;
constexpr int kMaxCloudGrains = 14, kDefaultCloudGrains = 10;
constexpr float kUnits = 30.0f;               // grains + chains + generators + mixing, per block
constexpr int kMinGrains = 6;                 // never fewer than this, whatever the chains cost
// The CPU guard: what the channels are set to (whether they sound or not) may
// cost at most this; past it the FX of the last channels is bypassed and
// generators stop writing (their buffer holds), shown as CPU on the pages.
constexpr float kChainCap = kUnits - float(kMinGrains);
constexpr int kSpawnsPerBlock = 8;
constexpr int kGestBufs = 3;                  // per channel (see Engine::gestIdle)

struct ChRt {
    int16_t* buf = nullptr;          // kBufLen: live ring or the loaded sample
    int16_t* frz = nullptr;          // kFrzLen: spectral freeze loop
    volatile uint32_t w = 0;         // live write index
    volatile uint32_t sampleLen = 0; // loaded sample length (frames)
    volatile uint8_t busy = 0;       // main loop is replacing the buffer: silent
    volatile uint8_t frzReady = 0;   // the freeze loop holds a spectrum
    volatile uint8_t liveG = 0;      // gesture buffer audio plays
    volatile uint8_t pendG = 0xFF;   // gesture buffer posted by the main loop
    Emitter em[kEmitters];           // 0..3 pads / MIDI, 4..7 gesture ghosts
    uint8_t ghostPad[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    Emitter drone;
    float gphase = 0.0f;             // gesture loop phase (samples)
    uint16_t gidx = 0;
    uint8_t gWrapped = 0;            // audio -> UI: the loop wrapped (flash)
    uint8_t fbOn = 0;
    int active = 0;                  // running grains
    float peak = 0.0f;               // output meter (decaying)
    float logistic = 0.37f, drift = 0.0f;
    uint32_t bedPos = 0;
    uint32_t spawned = 0, refused = 0;
    int quiet = 0;                   // blocks the chain may still ring
    float panV = -2.0f, panL = 1.0f, panR = 1.0f;
    int32_t fbPrev[lab::kBlock];     // the channel's last output x feedback, int16 units
    // modulation sources (exported to LABKIT)
    float lfo = 0.0f, lfoPh = 0.0f, lfoHeld = 0.0f, lfoDrift = 0.0f;
    float bufLevel = 0.0f;           // envelope of what is written into the buffer
    float density = 0.0f;            // running grains / per-channel budget
    float gestPos = 0.0f;            // gesture loop position 0..1
    float media = 0.0f;              // (unused slot, kept for alignment)
    // TAPE: the playhead shown on screen (0..1 of the region), -1 = none
    float tapeHead = -1.0f;
};

// A spawn report for the particle display (audio -> main loop).
struct Spark {
    uint8_t cloud, kind;
    uint16_t pos;          // 0..65535 across the field
    int8_t semis;          // pitch (-48..48)
    uint8_t amp;           // 0..255
    uint8_t life;          // grain length, 10 ms units (clamped)
    uint8_t live;          // bit 7: from a pad / note / gesture (else drone); bits 0..2: pad row, 7 = none
};

// A gesture's parameter move, for the main loop (it owns the registry).
struct ParamEvent {
    uint8_t ch, idx;       // channel, channel param index 0..35
    uint16_t v;            // normalized 0..65535
};

class Engine {
public:
    // gestures: kGestBufs * kCh buffers, [ch * kGestBufs + i]
    bool init(lab::Arena& arena, ChParams* params, ChState* st, ChSource* src, Gesture* gestures, FxState* fx);
    void render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r, lab::Context* ctx);

    // main loop helpers
    void padDown(int ch, int pad, int vel);
    void padUp(int ch, int pad);
    void noteOn(int ch, int note, int vel);
    void noteOff(int ch, int note);
    void releaseAll(int ch);
    void releaseGhosts(int ch);
    void resetGestures();
    Gesture& gestLive(int c) {
        const uint8_t p = rt[c].pendG;
        return gest_[c * kGestBufs + (p != 0xFF ? p : rt[c].liveG)];
    }
    // The buffer to fill; remembered, so postGesture() posts exactly this one.
    Gesture& gestIdle(int c) {
        idle_[c] = freeGest(c);
        return gest_[c * kGestBufs + idle_[c]];
    }
    Gesture& gestFilling(int c) { return gest_[c * kGestBufs + idle_[c]]; }
    void postGesture(int c) { rt[c].pendG = idle_[c]; }
    uint8_t freeGest(int c) const {
        const uint8_t p = rt[c].pendG, l = rt[c].liveG;
        for (uint8_t i = 0; i < kGestBufs; ++i)
            if (i != p && i != l) return i;
        return 0;
    }

    // capture (resample): src -1 = the mix, 0..3 = one channel (post fader)
    void startCapture(int src) { capLen = 0; capSrc = int8_t(src); capOn = 1; }
    void stopCapture() { capOn = 0; }

    int grainBudget = kDefaultGrains;       // global (Settings)
    int cloudBudget = kDefaultCloudGrains;  // per channel (Settings)
    volatile int totalActive = 0;
    volatile int grainLimit = kDefaultGrains;   // after the chains' units (display)
    volatile float unitsUsed = 0.0f;            // chains + generators this block (display)
    volatile uint8_t cpuBypass = 0;             // CPU guard: channels whose FX mode is bypassed
    volatile uint8_t cpuHold = 0;               // CPU guard: generators that do not write
    volatile uint32_t refusedTotal = 0;
    volatile double beat = 0.0;            // transport beat at the last block
    volatile uint32_t sampleClock = 0;     // samples rendered
    volatile uint8_t soloMask = 0;
    volatile uint8_t playing = 0;

    ChRt rt[kCh];
    ChannelChain chain[kCh];
    Generator gen[kCh];
    lab::SpscRing<Spark, 512> sparks;
    lab::SpscRing<ParamEvent, 256> paramEvents;
    int16_t* capBuf = nullptr;             // kBufLen mono
    volatile uint32_t capLen = 0;
    volatile int8_t capSrc = -1;
    volatile uint8_t capOn = 0;
    int16_t* mixHist = nullptr;            // kMixHist-sample mono ring of the master
    volatile uint32_t mixW = 0;
    float meterL = 0.0f, meterR = 0.0f;
    float masterLevel = 0.0f;              // mod source: the mix's envelope

    const float* windowTable(int i) const;
    float nextInterval(int c, int engine, const float* k, float rateHz);   // public for tests
    lab::dsp::Rng rng{0xB01u};

private:
    void spawn(int c, Emitter& e, int offset, bool live);
    void spawnGranular(int c, const Emitter& e, int offset, bool live);
    void spawnQuanta(int c, const Emitter& e, int offset, bool live);
    void spawnBeads(int c, const Emitter& e, int offset, bool live);
    void spawnTape(int c, Emitter& e, int offset, bool live);
    void playGesture(int c);
    void runEmitter(int c, Emitter& e, bool live);
    void writeBuffer(int c, const int16_t* x);
    void tickSources(int c);
    Grain* alloc(int c);
    void spark(int c, uint8_t kind, float pos, float semis, float amp, uint32_t len, bool live, int pad);
    // region helpers: where a 0..1 position reads in the channel's source
    bool sourceSpan(int c, uint32_t& base, uint32_t& len, const int16_t*& src, uint32_t& mask) const;

    ChParams* p_ = nullptr;
    ChState* st_ = nullptr;
    ChSource* src_ = nullptr;
    Gesture* gest_ = nullptr;
    FxState* fx_ = nullptr;
    ot::audio::Delay dly_;
    ot::audio::Reverb* rev_ = nullptr;
    float dlyTone_ = -1.0f, revSize_ = -1.0f, revDamp_ = -1.0f;
    int spawnRot_ = 0;
    int spawnsLeft_ = 0;
    uint8_t idle_[kCh] = {1, 1, 1, 1};
    uint8_t fxWanted_ = 0, fxRan_ = 0, genWanted_ = 0, genRan_ = 0;   // CPU guard, last block
    float chainRan_[kCh] = {};             // chain units each channel ran last block
    Grain pool_[kMaxGrains];
    int nActive_ = 0;
    float* win_ = nullptr;                 // kWindows * (kWin + 1), in the arena
    BlockClock clock_;
    double freeBeat_ = 0.0;                // the slicer / freezer clock while stopped
};

float scaleSnap(float semis, int scale);
float windowValue(int table, float x);   // x 0..1 (tests)
void buildWindowTables(float* out);      // kWindows * (kWin + 1)

}  // namespace bulut
