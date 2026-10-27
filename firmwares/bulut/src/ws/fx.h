// BULUT per-channel DSP blocks: the insert chain (drive -> filter -> FX mode ->
// Beads media) and the generator sources (rete, tone, noise).
//
// FX modes (one per channel, artifex-style: TIME, AMOUNT, FEEDBACK):
//   TEXTURE  Mutable Instruments Clouds' diffuser (MIT), with a feedback loop
//            and a tone control around it
//   SHIFTER  Clouds' pitch shifter (MIT), feedback = shimmer
//   FLANGER, PANNER, CRUSHER, SLICER, FREEZER
//            float ports after forsitan artifex's FX core (GPL-3.0,
//            forsitan modulare, artifex/core.hpp): the same controls and laws,
//            simplified (no trig input; the slicer and freezer follow the tempo)
// All of it runs in the audio interrupt on whole blocks; coefficients are
// computed once per block.
#pragma once

#include <cstdint>

#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/bulut.h"
#include "ws/tp.h"

namespace bulut {

// What a block needs to know about time.
struct BlockClock {
    double beat = 0.0;            // transport beat at the block start (free-running when stopped)
    float beatsPerSample = 0.0f;
    float bpm = 120.0f;
};

// CPU weights in grain units (one unit = one running grain, ~3,500 M4
// instructions a block; measured under QEMU with -DBULUT_PROF, see prof.h).
// engine.cpp: the grain budget is what is left of the block after these.
float baseUnits(const ChParams& p, bool beadsTape);   // drive + filter + DIRTY TAPE media
float fxUnits(int fxMode);                            // the FX mode
float genUnits(int genType);
constexpr float kMixUnits = 0.95f;   // a sounding channel's fader / sends / feedback
constexpr float kBedUnits = 0.55f;   // the freeze bed

class ChannelChain {
public:
    // Memory: SRAM for the diffuser (per-sample access), the arena for the rest.
    bool init(lab::Arena& a, float* diffuserSram);
    void reset();
    // In place on one channel's block (pre fader).
    // fxOn = false: the FX mode is bypassed (the CPU guard, engine.cpp).
    void process(float* L, float* R, const ChParams& p, int media, const BlockClock& bc, bool fxOn = true);
    bool tail() const { return tail_ > 0; }   // still ringing after the input went quiet
    // Display: the FX mode's own time read-out (Hz, ms, pattern...).
    float uiValue = 0.0f;

private:
    void drive(float* L, float* R, float amt);
    void filter(float* L, float* R, int mode, float hz, float reso);
    void texture(float* L, float* R, const ChParams& p);
    void shifter(float* L, float* R, const ChParams& p);
    void flanger(float* L, float* R, const ChParams& p);
    void panner(float* L, float* R, const ChParams& p);
    void crusher(float* L, float* R, const ChParams& p);
    void slicer(float* L, float* R, const ChParams& p, const BlockClock& bc);
    void freezer(float* L, float* R, const ChParams& p, const BlockClock& bc);
    void media(float* L, float* R, int m);

    // filter (TPT SVF per side)
    float ic1[2] = {}, ic2[2] = {};
    float fltHz_ = -1.0f, fltRes_ = -1.0f, g_ = 0.1f, k_ = 1.4f;
    int lastFx_ = -1;
    int tail_ = 0;
    // TEXTURE
    clouds::Diffuser diff_;
    float texFb_[2] = {}, texLp_[2] = {}, texPrev_[2] = {}, wowPrev_[2] = {};
    // SHIFTER
    clouds::PitchShifter shift_;
    float shFb_[2] = {};
    // FLANGER: float lines in the arena
    static constexpr uint32_t kFlgLen = 4096, kFlgMask = kFlgLen - 1;
    float* flg_[2] = {};
    uint32_t flgW_ = 0;
    float modPh_[2] = {0.0f, 0.25f};
    float flgDepth_ = 0.0f;
    // PANNER
    float panPh_ = 0.0f;
    // CRUSHER
    float crPh_[2] = {}, crHold_[2] = {}, crLp_[2] = {}, crY_[2] = {};
    // SLICER
    float slEnv_[2] = {}, slAtk_[2] = {};
    int slLastStep_ = -1;
    lab::dsp::Rng rng_{0xA27F1u};
    // FREEZER: an int16 stereo ring (the record head) and the loop state
    static constexpr uint32_t kFrzRing = 1u << 16;   // 1.49 s
    static constexpr uint32_t kFrzRingMask = kFrzRing - 1;
    int16_t* fz_[2] = {};
    uint32_t fzW_ = 0, fzRecorded_ = 0;
    bool fzFrozen_ = false, fzWasSilent_ = true;
    uint32_t fzEnd_ = 0;          // ring index where the frozen chunk ends
    float fzPos_ = 0.0f, fzLen_ = 0.0f;
    float fzOpen_ = 0.0f;
    // Beads media: wow and flutter (Beads' own), output low-pass
    beads::WowAndFlutter wow_;
    beads::FloatFrame frames_[lab::kBlock];
    float medLp_[2] = {};
};

// Generator sources: write one block of mono samples (int16) into a channel buffer.
class Generator {
public:
    void init(uint32_t seed);
    void reseed(uint32_t seed);
    // genA / genB 0..1 (ChParams), out: kBlock int16
    void render(int type, float a, float b, int16_t* out);
    uint32_t seed() const { return seed_; }
    // rete's node levels (display)
    float node[8] = {};

private:
    void rete(float leakK, float scaleK, float* out, uint32_t n);
    // rete (after forsitan modulare's rete.cpp, GPL-3.0; via RETICULUM's port)
    float matrix_[8][8] = {}, integ_[8] = {}, hpX_[8] = {}, hpY_[8] = {}, fb_[8] = {}, gain_[8] = {};
    uint32_t seed_ = 1, noise_ = 0x9d2c5680u;
    float exc_ = 0.0f, excT_ = 0.0f;
    uint32_t excCount_ = 0;
    float reteLeakK_ = -1.0f, reteScaleK_ = -1.0f, reteLeak_ = 0.0f, reteScale_ = 1.0f, reteDc_ = 0.99f;
    float half_[2] = {};
    // tone
    float ph_ = 0.0f, ph2_ = 0.0f;
    // noise
    float pink_[3] = {};
    float dust_ = 0.0f;
    lab::dsp::Rng rng_{0x5EEDu};
};

// The artifex rhythm table (forsitan modulare, shared/modulation.hpp, GPL-3.0).
uint16_t rhythmPattern(int i);

}  // namespace bulut
