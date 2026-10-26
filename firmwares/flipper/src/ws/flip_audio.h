// FLIPPER -- the sound: 12 voices of eight small engines (kick, snare, hat,
// zap, pluck, bell, lead/bass, sampler), three group buses (drums / bass /
// keys) with the ramp effects (filter sweep, crush, stutter) as inserts, a
// tempo-synced ping-pong delay and a small FDN reverb as sends.
// Everything runs in the audio interrupt; memory comes from the arena.
#pragma once

#include <cstdint>

#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/flip_game.h"
#include "ws/flip_model.h"

namespace flip {

constexpr int kVoices = 10;
constexpr uint32_t kSampleMaxFrames = 44100 * 6;   // 6 s per sampler slot
constexpr uint32_t kPluckLen = 2048;

struct SampleSlot {
    int16_t* data = nullptr;
    volatile uint32_t frames = 0;   // 0 while (re)loading
};

struct Voice {
    bool on = false;
    uint8_t inst = 0, engine = 0, group = 0, note = 60;
    uint32_t age = 0;
    int32_t wait = 0;            // samples before the voice starts (sample-accurate onsets)
    float amp = 0.0f, env = 0.0f, envK = 0.999f;
    float gl = 0.7f, gr = 0.7f, sendD = 0.0f, sendR = 0.0f;
    float ph = 0.0f, inc = 0.0f, ph2 = 0.0f, inc2 = 0.0f;
    float pitchEnv = 0.0f, pitchK = 0.99f, pitchAmt = 0.0f;
    float modEnv = 0.0f, modK = 0.999f, modAmt = 0.0f;
    float tone = 0.5f, a = 0.5f, b = 0.5f;
    float z1 = 0.0f, z2 = 0.0f, z3 = 0.0f;
    uint32_t noise = 1;
    lab::dsp::Svf svf;
    float cut = 1000.0f, cutEnv = 0.0f;
    int ctr = 0;
    // pluck (Karplus-Strong)
    float* ks = nullptr;
    uint32_t ksLen = 0, ksPos = 0;
    // sampler
    float pos = 0.0f, rate = 1.0f;
};

struct GroupFx {
    // envelopes run in samples
    uint32_t fT[kFxCount] = {}, fLen[kFxCount] = {};
    float fAmt[kFxCount] = {};
    lab::dsp::Svf svfL, svfR;
    float crushHoldL = 0.0f, crushHoldR = 0.0f, crushPh = 0.0f;
    float* stut = nullptr;       // stereo interleaved ring
    uint32_t stutW = 0, stutLen = 0, stutPos = 0, stutCap = 0;
    bool stutting = false;
    float level[kFxCount] = {};  // current envelope values (UI)
};

class Audio {
public:
    bool init(lab::Arena& arena);
    void setMachine(const Machine* m) { m_ = m; }
    void setTempo(float bpm, float stepSamples) { bpm_ = bpm; stepSamples_ = stepSamples; }

    // audio interrupt
    void trigger(int inst, int note, int vel, int offset);
    void fx(const FxOut& f, int offset);
    void perf(int fx, float amount);   // momentary performance effect on all groups (0 = off)
    void render(float* l, float* r);
    void panic();

    // stems (after render): drums, bass, keys, effect returns
    const float* stemL(int i) const { return i < kGroups ? busL_[i] : fxL_; }
    const float* stemR(int i) const { return i < kGroups ? busR_[i] : fxR_; }

    SampleSlot samples[kInsts];
    float groupLevel[kGroups] = {};  // UI meters
    float fxLevel(int group, int fx) const { return g_[group].level[fx]; }
    int voicesOn() const;

private:
    void startVoice(Voice& v, const Instrument& in, int inst, int note, int vel);
    void renderVoice(Voice& v, int i0);
    void groupFx(int g);
    void delay(int i0);
    void reverb();

    const Machine* m_ = nullptr;
    Voice v_[kVoices];
    uint32_t age_ = 0;
    float bpm_ = 120.0f, stepSamples_ = 5512.0f;
    GroupFx g_[kGroups];
    float perfFx_[kFxCount] = {};
    float busL_[kGroups][lab::kBlock], busR_[kGroups][lab::kBlock];
    float sendD_[lab::kBlock], sendR_[lab::kBlock];
    float fxL_[lab::kBlock], fxR_[lab::kBlock];   // delay + reverb returns
    // delay
    float* dl_ = nullptr;
    float* dr_ = nullptr;
    uint32_t dLen_ = 0, dW_ = 0;
    float dTime_ = 10000.0f, dLpL_ = 0.0f, dLpR_ = 0.0f;
    // reverb (4-line FDN + 2 input all-passes)
    float* rv_[4] = {};
    uint32_t rvLen_[4] = {}, rvW_[4] = {};
    float rvLp_[4] = {};
    float* ap_[2] = {};
    uint32_t apLen_[2] = {}, apW_[2] = {};
    lab::dsp::DcBlock dcL_, dcR_;
    uint32_t noise_ = 0x1234567u;
    uint32_t dQuiet_ = 0, rQuiet_ = 0;   // blocks of silence in / out (bypass)
};

}  // namespace flip
