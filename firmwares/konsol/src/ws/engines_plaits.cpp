// PLAITS: Mutable Instruments Plaits (Emilie Gillet, MIT) as a KONSOL engine.
// One mono voice per track, all 24 models, rendered as two 64-frame calls per
// 128-frame block. Pluck mode = Plaits' internal low-pass gate pinged by every
// note; Gate mode drives the LPG with an attack / release envelope (sustain).
#include <cstdio>
#include <cstring>
#include <new>

#include "ws/dspx.h"
#include "ws/engine_table.h"
#include "ws/mi/plaits/dsp/voice.h"
#include "ws/model.h"

namespace lab {
namespace konsol {

namespace {
using dx::clampf;

const char* const kPlModels[24] = {"VA VCF",     "Phase Dist", "6-Op FM A",  "6-Op FM B",  "6-Op FM C",  "Wave Terrain",
                                   "String Mach", "Chiptune",  "Virt Analog", "Waveshaper", "2-Op FM",    "Formant",
                                   "Harmonic",   "Wavetable",  "Chords",     "Speech",     "Swarm",      "Noise",
                                   "Particle",   "String",     "Modal",      "Bass Drum",  "Snare Drum", "Hi-Hat"};
// QEMU-measured M4 instructions per 128 frames (2 x 64, LPG on), per model.
const int32_t kPlCost[24] = {38500, 51400, 32300, 31700, 31900, 41600, 57000, 18600, 41000, 33000, 54300, 53400,
                             83900, 49300, 49800, 14900, 42000, 39300, 62700, 67000, 85600, 44300, 54000, 46900};
const char* const kPlEnv[2] = {"Pluck", "Gate"};
enum PlP { pModel, pHarm, pTimbre, pMorph, pDecay, pColour, pEnv, pAttack, pRelease, pFmEnv, pTimEnv, pMorEnv, pAux,
           pOct, pFine, pGain, pPlN };
const PDesc kPlDesc[pPlN] = {
    {"Model", 0, 23, 8, 1, kFList, kPlModels},
    {"Harmonics", 0, 1, 0.5f, 0.005f, kFPct, nullptr},
    {"Timbre", 0, 1, 0.5f, 0.005f, kFPct, nullptr},
    {"Morph", 0, 1, 0.5f, 0.005f, kFPct, nullptr},
    {"Decay", 0, 1, 0.5f, 0.005f, kFPct, nullptr},
    {"LPG Colour", 0, 1, 0.5f, 0.005f, kFPct, nullptr},
    {"Envelope", 0, 1, 0, 1, kFList, kPlEnv},
    {"Attack", 0, 2000, 2, 1, kFMs, nullptr},
    {"Release", 5, 4000, 200, 5, kFMs, nullptr},
    {"FM Env", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Timbre Env", -1, 1, 0.3f, 0.01f, kFBi, nullptr},
    {"Morph Env", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Aux Mix", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Octave", -3, 3, 0, 1, kFInt, nullptr},
    {"Fine", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kPlSec[4] = {{"Model", 0, 4}, {"Envelope", 4, 5}, {"Mod", 9, 3}, {"Out", 12, 4}};

class PlaitsEng final : public TableEngine {
public:
    PlaitsEng(plaits::Voice* v) : v_(v) { bind(kPlDesc, pPlN, kPlSec, 4); }
    int kind() const override { return kEngPlaits; }
    void preset(int variant) override {
        reset();
        if (variant == 1) {   // a pad
            p_[pModel] = 6;
            p_[pEnv] = 1;
            p_[pAttack] = 300;
            p_[pRelease] = 1200;
        }
    }
    void noteOn(int pitch, float vel, int offset, int tone, int) override {
        note_ = float(pitch);
        vel_ = vel;
        tone_ = float(tone) * (1.0f / 128.0f);
        pend_ = true;
        pendHalf_ = offset >= 64 ? 1 : 0;
        gate_ = true;
        pitch_ = pitch;
        idle_ = false;
        quiet_ = 0;
        age_ = 0;
    }
    void noteOff(int pitch) override {
        if (pitch == pitch_) gate_ = false;
    }
    void allOff(bool hard) override {
        gate_ = false;
        if (hard) {
            idle_ = true;
            env_ = 0;
            pend_ = false;
        }
    }
    void render(float* l, float* r) override {
        if (idle_) return;
        const float* p = p_;
        plaits::Patch pa;
        pa.note = note_ + 12.0f * float(int(p[pOct] + (p[pOct] >= 0 ? 0.5f : -0.5f))) + p[pFine];
        pa.harmonics = p[pHarm];
        pa.timbre = clampf(p[pTimbre] + tone_, 0.0f, 1.0f);
        pa.morph = p[pMorph];
        pa.frequency_modulation_amount = p[pFmEnv];
        pa.timbre_modulation_amount = p[pTimEnv];
        pa.morph_modulation_amount = p[pMorEnv];
        pa.engine = int(p[pModel] + 0.5f);
        pa.decay = p[pDecay];
        pa.lpg_colour = p[pColour];
        plaits::Modulations m;
        std::memset(&m, 0, sizeof m);
        m.trigger_patched = true;
        const bool gateMode = p[pEnv] > 0.5f;
        m.level_patched = gateMode;
        const float aInc = 64.0f / (p[pAttack] * 0.001f * dx::kSr + 1.0f);
        const float rInc = 64.0f / (p[pRelease] * 0.001f * dx::kSr + 1.0f);
        const float aux = p[pAux];
        const float g = p[pGain] * (gateMode ? 1.0f : vel_) * (1.0f / 32768.0f);
        float pk = 0;
        for (int k = 0; k < 2; ++k) {
            float trig = 0;
            if (pend_ && k >= pendHalf_ && !lastTrig_) {
                trig = 1.0f;
                pend_ = false;
                if (gateMode && env_ < 0.001f) env_ = 0;
            }
            lastTrig_ = trig > 0;
            m.trigger = trig;
            if (gateMode) {
                if (gate_) env_ = env_ + aInc > 1.0f ? 1.0f : env_ + aInc;
                else env_ = env_ - rInc < 0.0f ? 0.0f : env_ - rInc;
                m.level = env_ * vel_;
            }
            v_->Render(pa, m, fr_, 64);
            float* ol = l + k * 64;
            float* orr = r + k * 64;
            for (int i = 0; i < 64; ++i) {
                const float o = float(fr_[i].out), a = float(fr_[i].aux);
                const float x = (o + (a - o) * aux) * g;
                ol[i] += x;
                orr[i] += x;
                const float ax = x < 0 ? -x : x;
                if (ax > pk) pk = ax;
            }
        }
        // idle when it has been silent for a while (the LPG closed)
        if (age_ < 1000) ++age_;
        if (pk < 2e-4f && age_ > 4 && !pend_ && (!gateMode || !gate_)) {
            if (++quiet_ > 12) idle_ = true;
        } else {
            quiet_ = 0;
        }
    }
    int active() const override { return idle_ ? 0 : 1; }
    int32_t cost() const override { return idle_ ? 0 : modelCost(); }
    int32_t noteCost() const override { return idle_ ? modelCost() : 0; }
    bool steal() override {
        if (idle_) return false;
        idle_ = true;
        return true;
    }

private:
    int32_t modelCost() const {
        const int e = int(p_[pModel] + 0.5f);
        return kPlCost[e < 0 ? 0 : (e > 23 ? 23 : e)] * 11 / 10 + 1500;
    }
    plaits::Voice* v_;
    plaits::Voice::Frame fr_[64];
    float note_ = 60, vel_ = 1, tone_ = 0, env_ = 0;
    bool pend_ = false, lastTrig_ = false, gate_ = false, idle_ = true;
    int pendHalf_ = 0, pitch_ = -1, quiet_ = 0, age_ = 0;
};

}  // namespace

Engine* makePlaits(Arena& a) {
    constexpr uint32_t kBuf = 32768;
    void* vm = a.alloc(sizeof(plaits::Voice), 16);
    char* buf = static_cast<char*>(a.alloc(kBuf, 16));
    void* em = a.alloc(sizeof(PlaitsEng), 16);
    if (!vm || !buf || !em) return nullptr;
    plaits::Voice* v = new (vm) plaits::Voice();
    stmlib::BufferAllocator alloc(buf, kBuf);
    v->Init(&alloc);
    return new (em) PlaitsEng(v);
}

}  // namespace konsol
}  // namespace lab
