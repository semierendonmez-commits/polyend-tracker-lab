// The OpenTracker synth engines (ACD, VAP, FAT, WTFM, PERC) driven standalone:
// one SynthPatch + SynthParams + a few SynthVoice records per track, no
// SynthBank. Parameter names, ranges, formats and steps come from the
// OpenTracker synth UI tables (ui/screen_synth_tables.cpp).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/engines/acd.h"
#include "audio/engines/fat.h"
#include "audio/engines/perc.h"
#include "audio/engines/vap.h"
#include "audio/engines/wtfm.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "ui/screens.h"
#include "ws/engines.h"
#include "ws/model.h"

namespace lab {
namespace konsol {

Engine* makeSampler(Arena& a, SampleBuf* b);
Engine* makeGrain(Arena& a, SampleBuf* b);
Engine* makeDrums(Arena& a);
Engine* makeIkeda(Arena& a);
Engine* makeMakam(Arena& a);

namespace {

using namespace ot;
using namespace ot::audio;

// Estimated M4 instructions per voice per block (tests/perf_baseline.txt and
// audio_bench: acd.max 18k, vapworst 17.5k, fatworst 18.8k, wtfm 22k without /
// 30.6k with oversampling; PERC per kit from its own cost model).
int32_t voiceCost(int32_t engine, bool oversample) {
    switch (engine) {
        case kSynthEngineAcd: return 14500;
        case kSynthEngineVap: return 15000;
        case kSynthEngineFat: return 18000;
        case kSynthEngineWtfm: return oversample ? 34000 : 25000;
        default: return 20000;
    }
}

class SynthEng final : public Engine {
public:
    static constexpr int kMaxVoices = 4;

    SynthEng(int kind, int32_t engineId, ISynthEngine* e) : kind_(kind), id_(engineId), eng_(e) {
        patchInit(engineId, &patch_);
        nParams_ = synthParamCount(engineId);
        volIdx_ = patchVolumeParam(engineId);
        nsec_ = 0;
        const synthui::Section* sec = nullptr;
        const int ns = synthui::sections(engineId, &sec);
        for (int s = 0; s < ns && nsec_ < 12; ++s)
            if (sec[s].macro < 0 && sec[s].count > 0) secs_[nsec_++] = &sec[s];
        for (auto& v : v_) v.v.slotScratch = &scratch_;
        poly_ = (engineId == kSynthEngineAcd) ? 1 : (engineId == kSynthEnginePerc ? 1 : 3);
        preset(0);
    }
    int kind() const override { return kind_; }
    int sections() const override { return nsec_ + 1; }   // + "Voice" (polyphony)
    const char* sectionName(int s) const override {
        if (s == nsec_) return "Voices";
        return (s >= 0 && s < nsec_) ? secs_[s]->name : "";
    }
    int sectionParams(int s, uint8_t* out, int cap) const override {
        if (s == nsec_) {
            if (cap > 0) out[0] = uint8_t(nParams_);   // the polyphony row
            return 1;
        }
        if (s < 0 || s >= nsec_) return 0;
        int n = 0;
        for (int i = 0; i < secs_[s]->count && n < cap; ++i)
            if (synthui::paramUi(id_, secs_[s]->params[i])) out[n++] = secs_[s]->params[i];
        return n;
    }
    int paramCount() const override { return nParams_ + 1; }
    void paramName(int i, char* out, int cap) const override {
        if (i == nParams_) {
            std::snprintf(out, size_t(cap), "Polyphony");
            return;
        }
        const synthui::ParamUi* u = synthui::paramUi(id_, i);
        std::snprintf(out, size_t(cap), "%s", u ? u->label : "-");
    }
    float get(int i) const override {
        if (i == nParams_) return float(poly_);
        return synthParamGet(patch_, i);
    }
    void set(int i, float v) override {
        if (i == nParams_) {
            int p = int(v + 0.5f);
            poly_ = p < 1 ? 1 : (p > maxPoly() ? maxPoly() : p);
            return;
        }
        synthParamSet(patch_, i, v);
        stage();
    }
    void stepParam(int i, int d, bool coarse) override {
        if (i == nParams_) {
            set(i, float(poly_ + d));
            return;
        }
        const float v = synthParamGet(patch_, i);
        set(i, synthui::stepValue(id_, i, v, coarse ? d * 10 : d));
    }
    void format(int i, char* out, int cap) const override {
        if (i == nParams_) {
            std::snprintf(out, size_t(cap), "%d", poly_);
            return;
        }
        synthui::formatValue(id_, i, synthParamGet(patch_, i), out, cap);
    }
    float frac(int i) const override {
        if (i == nParams_) return float(poly_) / float(maxPoly());
        return synthui::valueFrac(id_, i, synthParamGet(patch_, i));
    }
    void reset() override {
        patchInit(id_, &patch_);
        stage();
    }
    void preset(int variant) override {
        patchInit(id_, &patch_);
        switch (id_) {
            case kSynthEngineAcd:   // a squelchy 303-style mono line
                synthParamSet(patch_, acd::kCutoff, 380.0f);
                synthParamSet(patch_, acd::kResonance, 1.05f);
                synthParamSet(patch_, acd::kFilterType, float(acd::kSvf24));
                synthParamSet(patch_, acd::kFilterEnv, 0.42f);
                synthParamSet(patch_, acd::kModA, 0.0f);
                synthParamSet(patch_, acd::kModD, 0.22f);
                synthParamSet(patch_, acd::kModS, 0.0f);
                synthParamSet(patch_, acd::kAmpA, 0.002f);
                synthParamSet(patch_, acd::kAmpD, 0.3f);
                synthParamSet(patch_, acd::kAmpS, 0.6f);
                synthParamSet(patch_, acd::kAmpR, 0.05f);
                synthParamSet(patch_, acd::kGlideMode, float(acd::kLegato));
                synthParamSet(patch_, acd::kGlideTime, 0.07f);
                synthParamSet(patch_, acd::kSubMix, 0.25f);
                break;
            case kSynthEngineVap:   // warm chord pad
                synthParamSet(patch_, vap::kCutoff, variant ? 2400.0f : 1400.0f);
                synthParamSet(patch_, vap::kDetune, 14.0f);
                synthParamSet(patch_, vap::kOscMix, 0.0f);
                synthParamSet(patch_, vap::kAmpA, 0.03f);
                synthParamSet(patch_, vap::kAmpR, 0.6f);
                synthParamSet(patch_, vap::kAmpS, 0.8f);
                synthParamSet(patch_, vap::kFilterEnvAmt, 0.2f);
                break;
            case kSynthEngineFat:   // supersaw stab
                synthParamSet(patch_, fat::kFatness, 0.45f);
                synthParamSet(patch_, fat::kCutoff, 3200.0f);
                synthParamSet(patch_, fat::kAmpR, 0.35f);
                synthParamSet(patch_, fat::kAmpS, 0.7f);
                break;
            case kSynthEngineWtfm:   // bell-ish FM lead
                synthParamSet(patch_, wtfm::kFm, 0.35f);
                synthParamSet(patch_, wtfm::kCutoff, 6000.0f);
                synthParamSet(patch_, wtfm::kAmpD, 0.6f);
                synthParamSet(patch_, wtfm::kAmpS, 0.3f);
                synthParamSet(patch_, wtfm::kAmpR, 0.4f);
                break;
            default: break;
        }
        stage();
    }

    // --- audio ----------------------------------------------------------------
    void noteOn(int pitch, float vel, int, int tone, int flags) override {
        sync();
        if (tone != tone_) {   // tone lock: only the cutoff moves (cheap)
            tone_ = tone;
            applyTone();
        }
        SynthNote n;
        n.note = uint8_t(pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch));
        n.pitch = float(n.note);
        n.velocity = vel;
        int r = -1;
        if (id_ == kSynthEnginePerc || poly_ == 1) {
            r = 0;
            V& V0 = v_[0];
            n.reused = V0.on;
            n.overlap = V0.on && V0.v.gate && ((flags & kFlagSlide) || id_ == kSynthEnginePerc);
            if (!V0.on) std::memset(V0.v.state, 0, sizeof(V0.v.state));
        } else {
            for (int i = 0; i < poly_; ++i)
                if (v_[i].on && v_[i].v.note == n.note) r = i;   // retrigger the same note
            if (r < 0)
                for (int i = 0; i < poly_; ++i)
                    if (!v_[i].on) { r = i; break; }
            if (r < 0) {   // steal: a released voice first, else the oldest
                for (int i = 0; i < poly_; ++i)
                    if (!v_[i].v.gate && (r < 0 || v_[i].age < v_[r].age)) r = i;
                if (r < 0)
                    for (int i = 0; i < poly_; ++i)
                        if (r < 0 || v_[i].age < v_[r].age) r = i;
            }
            n.reused = v_[r].on;
            if (!v_[r].on) std::memset(v_[r].v.state, 0, sizeof(v_[r].v.state));
        }
        V& V = v_[r];
        V.v.slot = 0;
        V.v.note = n.note;
        V.v.gate = true;
        V.v.fading = false;
        V.v.bend = V.v.bendStart = 0.0f;
        if (!V.on) V.g = 0.0f;
        V.vel = vel;
        V.on = true;
        V.age = ++age_;
        eng_->noteOn(V.v, p_, n);
    }
    void noteOff(int pitch) override {
        if (id_ == kSynthEnginePerc) return;
        for (auto& V : v_)
            if (V.on && V.v.gate && V.v.note == pitch) {
                V.v.gate = false;
                eng_->noteOff(V.v, p_);
            }
    }
    void allOff(bool hard) override {
        for (auto& V : v_) {
            if (!V.on) continue;
            if (hard) {
                V.v.fading = true;
                V.fade = true;
            } else if (V.v.gate) {
                V.v.gate = false;
                eng_->noteOff(V.v, p_);
            }
        }
    }
    void setTempo(float bpm) override { bpm_ = bpm; }
    void render(float* l, float* r) override {
        bool any = false;
        for (auto& V : v_) any = any || V.on;
        if (!any) return;
        sync();
        eng_->setTempo(bpm_);
        eng_->prepare(scratch_, p_);
        for (auto& V : v_) {
            if (!V.on) continue;
            float target = V.fade ? 0.0f : gainOf(V.vel);
            V.v.fading = V.fade;
            const float g0 = V.g;
            const float inc = (target - g0) * (1.0f / float(kBlock));
            V.g = target;
            const bool alive = eng_->render(V.v, p_, g0, inc, l, r);
            if (!alive || V.fade) {
                V.on = false;
                V.fade = false;
            }
        }
    }
    int active() const override {
        int n = 0;
        for (auto& V : v_) n += V.on;
        return n;
    }
    int32_t cost() const override {
        if (id_ == kSynthEnginePerc) {
            if (!v_[0].on) return 0;
            const int32_t c = perc::kitCost(const_cast<SynthSlotScratch&>(scratch_), p_);
            return (c > 0 ? c : 4000) + 1500;
        }
        const int n = active();
        return n ? n * voiceCost(id_, oversample()) + 1200 : 0;
    }
    int32_t noteCost() const override {
        if (id_ == kSynthEnginePerc) return v_[0].on ? 6000 : 12000;
        if (poly_ == 1 && v_[0].on) return 0;
        // a free voice adds one; a full slot steals (no extra)
        for (int i = 0; i < poly_; ++i)
            if (!v_[i].on) return voiceCost(id_, oversample()) + (active() ? 0 : 1200);
        return 0;
    }
    bool steal() override {
        int r = -1;
        for (int i = 0; i < kMaxVoices; ++i)
            if (v_[i].on && !v_[i].fade && (r < 0 || v_[i].age < v_[r].age)) r = i;
        if (r < 0) return false;
        v_[r].fade = true;   // one block fade, then free
        return true;
    }

private:
    int maxPoly() const { return id_ == kSynthEnginePerc ? 1 : kMaxVoices; }
    bool oversample() const { return id_ == kSynthEngineWtfm && p_.end[wtfm::kOversample] >= 0.5f; }
    float gainOf(float vel) const {
        if (volIdx_ < 0) return vel;   // PERC: velocity scales the kit
        return p_.end[volIdx_] * vel;
    }
    // Main loop: the effective parameters of the patch into the free stage
    // buffer, then publish it (the audio copies it at its next block).
    void stage() {
        const int k = pub_ ^ 1;
        const uint8_t macros[kSynthSeqMacros] = {0, 0, 0, 0, 0, 0};
        stageN_[k] = synthEffectiveParams(patch_, macros, 1.0f, stage_[k]);
        pub_ = uint8_t(k);
        dirty_ = true;
    }
    // Audio: take the published parameters (a copy, no computation).
    void sync() {
        if (!dirty_) return;
        dirty_ = false;
        const int k = pub_;
        p_.engine = id_;
        p_.count = stageN_[k];
        std::memcpy(p_.end, stage_[k], sizeof(float) * uint32_t(p_.count));
        const int ci = cutoffIndex();
        if (ci >= 0) baseCut_ = p_.end[ci];
        applyTone();
    }
    void applyTone() {
        const int ci = cutoffIndex();
        if (ci >= 0) {
            float c = baseCut_;
            if (tone_) {
                c *= exp2f(float(tone_) * (2.0f / 64.0f));
                c = c < 20.0f ? 20.0f : (c > 20000.0f ? 20000.0f : c);
            }
            p_.end[ci] = c;
        }
        std::memcpy(p_.start, p_.end, sizeof(float) * uint32_t(p_.count));
        p_.moving = false;
        ++p_.serial;
    }
    int cutoffIndex() const {
        switch (id_) {
            case kSynthEngineAcd: return acd::kCutoff;
            case kSynthEngineVap: return vap::kCutoff;
            case kSynthEngineFat: return fat::kCutoff;
            case kSynthEngineWtfm: return wtfm::kCutoff;
            default: return -1;
        }
    }

    struct V {
        SynthVoice v;
        bool on = false, fade = false;
        float g = 0.0f, vel = 1.0f;
        uint32_t age = 0;
    };
    int kind_;
    int32_t id_;
    ISynthEngine* eng_;
    SynthPatch patch_;
    SynthParams p_;
    SynthSlotScratch scratch_;
    V v_[kMaxVoices];
    const synthui::Section* secs_[12] = {};
    int nsec_ = 0, nParams_ = 0, volIdx_ = -1;
    int poly_ = 1;
    int tone_ = 0;
    float baseCut_ = 1000.0f;
    float stage_[2][kSynthParamMax] = {};
    int stageN_[2] = {};
    volatile uint8_t pub_ = 0;
    uint32_t age_ = 0;
    float bpm_ = 120.0f;
    volatile bool dirty_ = true;

public:
    // Raw patch access for the project file.
    SynthPatch& patch() { return patch_; }
};

const char* const kNames[kEngCount] = {"SAMPLER", "GRAIN", "DRUMS", "IKEDA", "MAKAM",
                                       "ACD",     "VAP",   "FAT",   "WTFM",  "PERC"};
const char* const kInfo[kEngCount] = {
    "WAV from the card: one-shot, loop or slices",
    "Granular cloud over the track's sample",
    "Analog-style kit: 8 drums on notes C2..G2",
    "Sine tones, clicks, noise, data bursts",
    "Makam voices in 53-TET / 24-tone tuning",
    "OpenTracker ACD: acid mono synth",
    "OpenTracker VAP: virtual analog poly",
    "OpenTracker FAT: supersaw",
    "OpenTracker WTFM: wavetable FM",
    "OpenTracker PERC: synthesized drum kit",
};

template <class T, class... A>
T* make(Arena& a, A... args) {
    void* m = a.alloc(sizeof(T), 16);
    return m ? new (m) T(args...) : nullptr;
}

}  // namespace

const char* engineName(int k) { return kNames[k < 0 ? 0 : (k >= kEngCount ? kEngCount - 1 : k)]; }
const char* engineInfo(int k) { return kInfo[k < 0 ? 0 : (k >= kEngCount ? kEngCount - 1 : k)]; }

int Engine::sectionParams(int s, uint8_t* out, int cap) const {
    if (s != 0) return 0;
    int n = 0;
    for (int i = 0; i < paramCount() && n < cap; ++i) out[n++] = uint8_t(i);
    return n;
}

bool TrackEngines::init(Arena& a, SampleBuf* sample, int track) {
    (void)track;
    e[kEngSampler] = makeSampler(a, sample);
    e[kEngGrain] = makeGrain(a, sample);
    e[kEngDrums] = makeDrums(a);
    e[kEngIkeda] = makeIkeda(a);
    e[kEngMakam] = makeMakam(a);
    e[kEngAcd] = make<SynthEng>(a, int(kEngAcd), int32_t(kSynthEngineAcd), synthAcdEngine());
    e[kEngVap] = make<SynthEng>(a, int(kEngVap), int32_t(kSynthEngineVap), vap::engine());
    e[kEngFat] = make<SynthEng>(a, int(kEngFat), int32_t(kSynthEngineFat), fat::engine());
    e[kEngWtfm] = make<SynthEng>(a, int(kEngWtfm), int32_t(kSynthEngineWtfm), wtfm::engine());
    e[kEngPerc] = make<SynthEng>(a, int(kEngPerc), int32_t(kSynthEnginePerc), percEngine());
    for (auto* x : e)
        if (!x) return false;
    return true;
}

}  // namespace konsol
}  // namespace lab
