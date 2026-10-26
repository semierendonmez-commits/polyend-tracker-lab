// PLATFORM -- the native voice engine Lua scripts play (MANUAL.md "Voice engine").
//
// 16 voices (sine, triangle, saw, square, noise, 2-op FM, sample, grain, click)
// with a state-variable filter, an ADSR / percussive envelope, a pitch
// envelope, pan and two sends, plus a global ping-pong delay and a reverb.
//
// Threads: the main loop (Lua) only pushes Cmd records into an SPSC ring;
// render() (audio interrupt) pops them and applies each at its sample inside
// the block. A command's `when` is a beat on the engine clock: the Shell
// transport beat while playing, a free-running beat at the same tempo while
// stopped. The clock publishes its position to the main loop through a
// sequence lock (beat(), bps(), epoch()); a transport start re-zeroes the
// beat and bumps the epoch, and commands of an older epoch play at once.
#pragma once

#include <cstdint>

#include "lab/lab.h"

namespace plat {

constexpr int kVoices = 16;
constexpr int kSlots = 32;           // sample slots
constexpr int kGrains = 4;           // grains per grain voice

enum Wave : uint8_t { kSine, kTri, kSaw, kSquare, kNoise, kFm, kSample, kGrain, kClick, kWaveCount };

// Voice parameters (engine.set(voice, name, value)). Keep in sync with kParamNames.
enum Param : uint8_t {
    pWave, pAmp, pPan, pPw, pFmRatio, pFmIndex, pCutoff, pRes, pFtype,
    pAttack, pDecay, pSustain, pRelease, pEnvMode, pPitchEnv, pPitchDecay,
    pSample, pRate, pStart, pLength, pLoop,
    pGrainSize, pGrainDensity, pGrainPos, pGrainSpread,
    pSendDelay, pSendReverb, pGlide, pDetune, pDrive, pFenv,
    pParamCount
};
// Global effect parameters (engine.fx(name, value)).
enum FxParam : uint8_t {
    fDelayTime, fDelayFeedback, fDelayLevel, fDelayTone,
    fReverbSize, fReverbDamp, fReverbLevel, fLevel,
    fFxCount
};
extern const char* const kParamNames[pParamCount];
extern const char* const kFxNames[fFxCount];
extern const char* const kWaveNames[kWaveCount];
int paramIndex(const char* name);   // -1 if unknown
int fxIndex(const char* name);
int waveIndex(const char* name);

struct Cmd {
    enum Op : uint8_t { kNone, kOn, kOff, kSet, kFx, kSlot, kPanic, kReset, kOnFreq };
    uint8_t op = kNone, a = 0, b = 0, epoch = 0;
    float v = 0.0f;
    double when = -1.0;            // engine beat, < 0 = at the next block start
    const int16_t* ptr = nullptr;  // kSlot: planar int16 (L then R when stereo)
    uint32_t frames = 0;
};

struct SampleSlot {
    const int16_t* data = nullptr;
    uint32_t frames = 0;
    uint8_t channels = 0;
};

class Engine {
public:
    // Allocates voices, delay and reverb lines from the arena.
    bool init(lab::Arena& arena);

    // ---- main loop ----
    bool push(const Cmd& c);                  // false when the ring is full
    void noteOn(int voice, float hz, float vel, double when, bool freq = true);
    void noteOff(int voice, double when);
    void set(int voice, int param, float v, double when);
    void fx(int param, float v, double when);
    void panic();                             // every voice off now (hard)
    // Silence and forget the sample slots; done when resetDone() is true.
    void requestReset();
    bool resetDone() const { return resetAck_ == resetReq_; }
    void publishSlot(int slot, const int16_t* data, uint32_t frames, int channels);
    uint32_t dropped() const { return dropped_; }
    // Epoch stamped on the next pushed commands (the clock snapshot their
    // `when` belongs to); the host sets it after each clock() snapshot.
    void setStampEpoch(uint8_t e) { stampEpoch_ = e; }
    // Clock as last published by render() (consistent snapshot).
    void clock(double& beat, double& bps, uint8_t& epoch, uint32_t& blocks) const;
    uint8_t epoch() const { return pubEpoch_; }
    uint32_t blocks() const { return pubBlocks_; }
    // Main-loop view of activity (for the UI): envelope level per voice, 0..1.
    float meter(int v) const { return (v >= 0 && v < kVoices) ? meter_[v] : 0.0f; }
    float outPeak() const { return outPeak_; }
    // CPU governor (render): blocks in which voices were limited, the last
    // block's estimated cost in M4 instructions.
    uint32_t overloadBlocks() const { return overloads_; }
    uint32_t lastCost() const { return lastCost_; }
    static constexpr uint32_t kBudgetInsn = 150000;   // engine share of the 180k render budget
    static uint32_t voiceCost(int wave, int ftype, bool drive, bool stereo, bool sends, int grains);

    // ---- audio interrupt ----
    void render(const lab::Transport& t, float* l, float* r);

    // ---- tests / bench ----
    // Applies a command directly (no ring): only when render() is not running.
    void applyNow(const Cmd& c);
    float paramValue(int voice, int p) const;

private:
    struct Grain {
        uint32_t idx = 0;
        float frac = 0.0f, inc = 1.0f, age = 0.0f, len = 0.0f;
        bool on = false;
    };
    struct Voice {
        float p[pParamCount];
        // state
        bool gate = false, active = false;
        uint8_t stage = 0;   // 0 idle 1 attack 2 decay 3 sustain 4 release
        float env = 0.0f, vel = 1.0f;
        float hz = 261.63f, hzTarget = 261.63f, phase = 0.0f, mphase = 0.0f;
        float pitchEnv = 0.0f;        // current semitone offset
        float lastInc = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f, ic1r = 0.0f, ic2r = 0.0f, g = 0.1f, k = 1.0f, cutCached = -1.0f, resCached = -1.0f;
        uint32_t sidx = 0;
        float sfrac = 0.0f;
        float grainTimer = 0.0f;
        Grain grains[kGrains];
        uint32_t rng = 1;
        float clickEnv = 0.0f;
        float gl = 0.7f, gr = 0.7f;
    };

    void apply(const Cmd& c);
    void trigger(Voice& v, float hz, float vel, bool freq);
    void renderVoice(Voice& v, int from, int to);
    void renderFx(float* l, float* r);

    lab::SpscRing<Cmd, 512>* ring_ = nullptr;
    Voice* voices_ = nullptr;
    SampleSlot slots_[kSlots];
    float fx_[fFxCount];
    // pending (audio side): commands whose time is beyond the current block
    static constexpr int kPending = 256;
    Cmd* pending_ = nullptr;
    int nPending_ = 0;

    // buses for the current block
    float busL_[lab::kBlock], busR_[lab::kBlock], sendD_[lab::kBlock], sendR_[lab::kBlock];

    // delay
    float* dlyL_ = nullptr;
    float* dlyR_ = nullptr;
    uint32_t dlyLen_ = 0, dlyW_ = 0;
    float dlyTime_ = 0.375f, dlyLp_ = 0.0f, dlyLpR_ = 0.0f;
    float dlyEnergy_ = 0.0f;
    // reverb at half rate (22.05 kHz): 4 shared combs, 2 allpasses per side
    static constexpr int kCombs = 4, kAps = 2;
    float* comb_[kCombs] = {};
    uint32_t combLen_[kCombs] = {}, combW_[kCombs] = {};
    float combLp_[kCombs] = {};
    float* ap_[2][kAps] = {};
    uint32_t apLen_[2][kAps] = {}, apW_[2][kAps] = {};
    float revPrevL_ = 0.0f, revPrevR_ = 0.0f;
    bool sendsUsed_ = false;
    float revEnergy_ = 0.0f;
    float level_ = 1.0f;

    // clock
    double freeBeat_ = 0.0;
    uint8_t epoch_ = 0;
    volatile uint32_t seq_ = 0;
    double pubBeat_ = 0.0, pubBps_ = 0.0;
    volatile uint8_t pubEpoch_ = 0;
    volatile uint32_t pubBlocks_ = 0;

    volatile uint32_t resetReq_ = 0, resetAck_ = 0;
    uint32_t dropped_ = 0;
    uint8_t stampEpoch_ = 0;
    uint32_t overloads_ = 0, lastCost_ = 0;
    int grainCap_ = kGrains;
    float meter_[kVoices] = {};
    float outPeak_ = 0.0f;
};

// Sine table shared by the engine and the examples (1024 points + guard).
float fastSin(float phase01);

}  // namespace plat
