// TILT -- the sound: eight channels, each one voice of one engine (SAMPLE, the
// Plaits analog / synthetic KICK and SNARE, the Plaits 808-style HAT, a
// 303-style ACID, a two-operator FM), a mixer with level / pan / two sends /
// choke groups / mute / solo, a tempo delay (send A), an FDN reverb (send B),
// master drive, and four recorder stems.
//
// Main loop -> audio: the Pattern's plain fields (read every block) and the
// event queue (post()). Everything here runs in the audio interrupt except
// init(), renderBuiltins() and the sample loading helpers.
#pragma once

#include <cstdint>

#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/tilt_model.h"

namespace tilt {

struct SampleSlot {
    int16_t* data = nullptr;
    uint32_t cap = 0;
    volatile uint32_t frames = 0;
    volatile uint8_t ready = 0;
};

// One scheduled event, in absolute samples.
struct Pending {
    uint32_t at = 0;
    uint32_t len = 0;        // samples (notes)
    uint8_t kind = 0;        // Hit::Kind
    uint8_t ch = 0, note = 60, vel = 100, step = 0, chokeMask = 0, ball = 0, used = 0;
};

class Voice;

class Audio {
public:
    static constexpr int kQueue = 256;
    static constexpr uint32_t kUserFrames = 441000;   // 10 s per channel
    static constexpr uint32_t kDelayFrames = 2 * 44100 + 512;

    bool init(lab::Arena& a);
    void attach(Pattern* p) { p_ = p; }
    void renderBuiltins();         // the built-in samples (init, main loop)
    SampleSlot& userSlot(int ch) { return user_[ch]; }
    const SampleSlot& builtin(int i) const { return builtin_[i]; }
    const SampleSlot* sourceOf(int ch) const;   // what a SAMPLE channel plays

    // ---- audio side ---------------------------------------------------------------
    // Queue an event (audio side only: the workstation posts main-loop events
    // through its command ring).
    bool post(const Pending& e);
    // Render one block starting at absolute sample `now`; events due in this block
    // fire at their sample. Fired notes are listed in fired[] for MIDI out.
    void render(uint32_t now, float bpm, float* l, float* r);
    void panic();                  // everything off
    // stems (recorder): after render(), stemL/R[i] hold stem i
    float stemL[4][lab::kBlock], stemR[4][lab::kBlock];
    bool stemUsed[4] = {};
    bool stemsOn = false;          // set by the workstation: the recorder takes stems
    Pending fired[64];
    int nFired = 0;

    // ---- meters (screen) -----------------------------------------------------------
    float chPeak[kCh] = {};
    volatile uint32_t chHits[kCh] = {};
    float outPeak = 0.0f;
    bool active(int ch) const;

private:
    void fire(const Pending& e);
    Pattern* p_ = nullptr;
    Voice* v_ = nullptr;           // kCh voices
    Pending q_[kQueue];
    SampleSlot user_[kCh];
    SampleSlot builtin_[kBuiltins];
    // FX
    float* dlL_ = nullptr;
    float* dlR_ = nullptr;
    uint32_t dlW_ = 0;
    float dlTime_ = 11025.0f, dlLpL_ = 0.0f, dlLpR_ = 0.0f;
    static constexpr int kRevLines = 4;
    float* rev_[kRevLines] = {};
    uint32_t revLen_[kRevLines] = {}, revW_[kRevLines] = {};
    float revLp_[kRevLines] = {};
    float* ap_[2] = {};
    uint32_t apLen_[2] = {}, apW_[2] = {};
    lab::dsp::DcBlock dcL_, dcR_;
    int queuedN_ = 0;
    float fxPrevL_ = 0.0f, fxPrevR_ = 0.0f;
    int16_t panOf_[kCh] = {-1, -1, -1, -1, -1, -1, -1, -1};
    float panL_[kCh] = {}, panR_[kCh] = {};
    uint32_t dlIdle_ = 100000, revIdle_ = 100000;
    float sendA_[lab::kBlock], sendB_[lab::kBlock], chBuf_[lab::kBlock];
};

}  // namespace tilt
