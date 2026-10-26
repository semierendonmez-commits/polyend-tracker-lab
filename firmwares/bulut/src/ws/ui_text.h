// BULUT parameter names, steps and value text (UI, gestures, morph).
#pragma once

#include <cstdio>

#include "ws/bulut.h"

namespace bulut {

inline const char* qTypeName(int i) {
    static const char* const k[kQTypes] = {"SAMPLE", "GLISSON", "GABOR", "PULSAR", "TRAINLET"};
    return k[i < 0 ? 0 : (i >= kQTypes ? kQTypes - 1 : i)];
}
inline const char* lawName(int i) {
    static const char* const k[kLaws] = {"SYNC", "POISSON", "GAUSS", "CAUCHY", "LOGISTIC"};
    return k[i < 0 ? 0 : (i >= kLaws ? kLaws - 1 : i)];
}
inline const char* scaleName(int i) {
    static const char* const k[kScales] = {"FREE", "OCTAVE", "FIFTH", "MAJOR", "MINOR", "PENTA"};
    return k[i < 0 ? 0 : (i >= kScales ? kScales - 1 : i)];
}

// Names of the 16 slots for each engine.
inline const char* paramName(int eng, int slot) {
    static const char* const g[kParams] = {"POSITION", "SIZE",  "DENSITY", "PITCH",  "SPREAD", "TEXTURE",
                                           "FEEDBACK", "REVERB", "ENGINE", "JITTER", "P.RAND", "REVERSE",
                                           "LEVEL",    "DELAY", "SCALE",   "TONE"};
    static const char* const q[kParams] = {"TYPE",  "RATE",   "DURATION", "FREQ",  "BAND",  "LAW",
                                           "CHAR",  "REVERB", "ENGINE",   "POSITION", "DRIFT", "SPREAD",
                                           "LEVEL", "DELAY",  "SCALE",    "TONE"};
    return (eng ? q : g)[slot & 15];
}

// Number of discrete steps (0 = continuous).
inline int paramSteps(int eng, int slot) {
    if (slot == P::Engine) return 2;
    if (slot == P::Scale) return kScales;
    if (eng == kQuanta) {
        if (slot == P::QType) return kQTypes;
        if (slot == P::QLaw) return kLaws;
    }
    return 0;
}

inline void paramText(int eng, int slot, float v, char* out, int cap) {
    if (slot == P::Engine) { std::snprintf(out, size_t(cap), "%s", v > 0.5f ? "QUANTA" : "GRANULAR"); return; }
    if (slot == P::Scale) { std::snprintf(out, size_t(cap), "%s", scaleName(enumIndex(v, kScales))); return; }
    if (slot == P::Level || slot == P::Delay || slot == P::Reverb) {
        std::snprintf(out, size_t(cap), "%d%%", int(v * 100.0f + 0.5f));
        return;
    }
    if (slot == P::Tone) {
        const int t = int((v - 0.5f) * 200.0f);
        if (t < -4) std::snprintf(out, size_t(cap), "DARK %d", -t);
        else if (t > 4) std::snprintf(out, size_t(cap), "BRIGHT %d", t);
        else std::snprintf(out, size_t(cap), "FLAT");
        return;
    }
    if (eng == kGranular) {
        switch (slot) {
            case P::Size: {
                const float s = sizeSec(v);
                if (s < 0.1f) std::snprintf(out, size_t(cap), "%d ms", int(s * 1000.0f + 0.5f));
                else std::snprintf(out, size_t(cap), "%.2f s", double(s));
                return;
            }
            case P::Dens: std::snprintf(out, size_t(cap), "%.1f /s", double(densHz(v))); return;
            case P::Pitch: {
                const float p = pitchSemi(v);
                std::snprintf(out, size_t(cap), "%+.1f st", double(p));
                return;
            }
            case P::Texture: {
                const int t = enumIndex(v, 16);
                std::snprintf(out, size_t(cap), "%s %d", t < 5 ? "FLAT" : (t == 5 ? "HANN" : "SOFT"), t);
                return;
            }
            default: break;
        }
    } else {
        switch (slot) {
            case P::QType: std::snprintf(out, size_t(cap), "%s", qTypeName(enumIndex(v, kQTypes))); return;
            case P::QLaw: std::snprintf(out, size_t(cap), "%s", lawName(enumIndex(v, kLaws))); return;
            case P::QRate: std::snprintf(out, size_t(cap), "%d /s", int(rateHz(v) + 0.5f)); return;
            case P::QDur: std::snprintf(out, size_t(cap), "%.1f ms", double(durSec(v) * 1000.0f)); return;
            case P::QFreq: std::snprintf(out, size_t(cap), "%d Hz", int(freqHz(v) + 0.5f)); return;
            case P::QBand: std::snprintf(out, size_t(cap), "%.2f oct", double(bandOct(v))); return;
            default: break;
        }
    }
    std::snprintf(out, size_t(cap), "%d", int(v * 100.0f + 0.5f));
}

}  // namespace bulut
