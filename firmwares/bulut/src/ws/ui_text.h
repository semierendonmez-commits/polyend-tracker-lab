// BULUT names and value text: engines, knobs, enums (UI, registry names, HUD).
#pragma once

#include <cstdio>

#include "ws/bulut.h"

namespace bulut {

inline const char* engineName(int e) {
    static const char* const k[kEngines] = {"GRANULAR", "QUANTA", "BEADS", "TAPE"};
    return k[e < 0 ? 0 : (e >= kEngines ? kEngines - 1 : e)];
}
inline const char* engineShort(int e) {
    static const char* const k[kEngines] = {"GRAN", "QNTA", "BEAD", "TAPE"};
    return k[e < 0 ? 0 : (e >= kEngines ? kEngines - 1 : e)];
}
inline const char* srcName(int s) {
    static const char* const k[kSrcCount] = {"INPUT", "OUTPUT", "SAMPLE", "GENERATOR", "TAPE FILE"};
    return k[s < 0 ? 0 : (s >= kSrcCount ? kSrcCount - 1 : s)];
}
inline const char* genName(int g) {
    static const char* const k[kGenCount] = {"RETE", "TONE", "NOISE"};
    return k[g < 0 ? 0 : (g >= kGenCount ? kGenCount - 1 : g)];
}
inline const char* qTypeName(int i) {
    static const char* const k[kQTypes] = {"SAMPLE", "GLISSON", "GABOR", "GRAINLET", "PULSAR", "TRAINLET"};
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
inline const char* mediaName(int i) {
    static const char* const k[kMedias] = {"HI-FI", "CLEAN TAPE", "LO-FI", "DIRTY TAPE"};
    return k[i < 0 ? 0 : (i >= kMedias ? kMedias - 1 : i)];
}
inline const char* fltName(int i) {
    static const char* const k[kFltModes] = {"OFF", "LOW-PASS", "BAND-PASS", "HIGH-PASS"};
    return k[i < 0 ? 0 : (i >= kFltModes ? kFltModes - 1 : i)];
}
inline const char* fxName(int i) {
    static const char* const k[kFxModes] = {"OFF", "TEXTURE", "SHIFTER", "FLANGER", "PANNER", "CRUSHER", "SLICER", "FREEZER"};
    return k[i < 0 ? 0 : (i >= kFxModes ? kFxModes - 1 : i)];
}
inline const char* lfoShapeName(int i) {
    static const char* const k[kLfoShapes] = {"SINE", "TRI", "SAW", "SQUARE", "S&H", "DRIFT"};
    return k[i < 0 ? 0 : (i >= kLfoShapes ? kLfoShapes - 1 : i)];
}

// Engine knob names (12 per engine, slot order of K::).
inline const char* knobName(int engine, int k) {
    static const char* const n[kEngines][kKnobs] = {
        {"POSITION", "SIZE", "DENSITY", "PITCH", "SPREAD", "TEXTURE", "JITTER", "FEEDBACK", "REVERSE", "P.RAND",
         "SCALE", "AMP RND"},
        {"POSITION", "DURATION", "RATE", "FREQ", "SPREAD", "CHAR", "BAND", "DRIFT", "TYPE", "LAW", "SCALE", "MASK"},
        {"TIME", "SIZE", "DENSITY", "PITCH", "SPREAD", "SHAPE", "TIME RND", "FEEDBACK", "MEDIA", "PITCH RND",
         "SIZE RND", "SHAPE RND"},
        {"POSITION", "GRAIN", "SPEED", "PITCH", "SPREAD", "SMOOTH", "JITTER", "FEEDBACK", "LOOP", "LENGTH", "SCALE",
         "WOW"},
    };
    return n[engine < 0 ? 0 : (engine >= kEngines ? kEngines - 1 : engine)][(k >= 0 && k < kKnobs) ? k : 0];
}

// Discrete knobs (0 = continuous).
inline int knobSteps(int engine, int k) {
    if (k == K::Scale && engine != kBeads) return kScales;
    if (engine == kQuanta && k == K::Mode) return kQTypes;
    if (engine == kQuanta && k == K::Rand) return kLaws;
    if (engine == kBeads && k == K::Mode) return kMedias;
    if (engine == kTape && k == K::Mode) return 2;   // LOOP: one-shot / loop
    if (engine == kGranular && k == K::Shape) return 16;
    return 0;
}

inline void knobText(int engine, int k, float v, char* out, int cap) {
    const size_t c = size_t(cap);
    const int steps = knobSteps(engine, k);
    if (k == K::Scale && steps) { std::snprintf(out, c, "%s", scaleName(enumIndex(v, kScales))); return; }
    switch (engine) {
        case kGranular:
            switch (k) {
                case K::Size: {
                    const float s = sizeSec(v);
                    if (s < 0.1f) std::snprintf(out, c, "%d ms", int(s * 1000.0f + 0.5f));
                    else std::snprintf(out, c, "%.2f s", double(s));
                    return;
                }
                case K::Dens: std::snprintf(out, c, "%.1f /s", double(densHz(v))); return;
                case K::Pitch: std::snprintf(out, c, "%+.1f st", double(pitchSemi(v))); return;
                case K::Shape: {
                    const int t = enumIndex(v, 16);
                    std::snprintf(out, c, "%s %d", t < 5 ? "HARD" : (t == 5 ? "HANN" : "SOFT"), t);
                    return;
                }
                default: break;
            }
            break;
        case kQuanta:
            switch (k) {
                case K::Mode: std::snprintf(out, c, "%s", qTypeName(enumIndex(v, kQTypes))); return;
                case K::Rand: std::snprintf(out, c, "%s", lawName(enumIndex(v, kLaws))); return;
                case K::Dens: std::snprintf(out, c, "%d /s", int(rateHz(v) + 0.5f)); return;
                case K::Size: std::snprintf(out, c, "%.1f ms", double(durSec(v) * 1000.0f)); return;
                case K::Pitch: std::snprintf(out, c, "%d Hz", int(freqHz(v) + 0.5f)); return;
                case K::Scatter: std::snprintf(out, c, "%.2f oct", double(bandOct(v))); return;
                default: break;
            }
            break;
        case kBeads:
            switch (k) {
                case K::Mode: std::snprintf(out, c, "%s", mediaName(enumIndex(v, kMedias))); return;
                case K::Pitch: std::snprintf(out, c, "%+.1f st", double(pitchSemi(v))); return;
                case K::Dens:
                    if (v < 0.48f) std::snprintf(out, c, "REG %d", int((0.48f - v) * 208.0f));
                    else if (v > 0.52f) std::snprintf(out, c, "RND %d", int((v - 0.52f) * 208.0f));
                    else std::snprintf(out, c, "SEEDS");
                    return;
                case K::Size:
                    if (v < 0.3333f) std::snprintf(out, c, "REV %d", int((0.3333f - v) * 300.0f));
                    else std::snprintf(out, c, "%d", int((v - 0.3333f) * 150.0f));
                    return;
                default: break;
            }
            break;
        default:   // TAPE
            switch (k) {
                case K::Size: std::snprintf(out, c, "%d ms", int(tapeGrainSec(v) * 1000.0f + 0.5f)); return;
                case K::Dens: std::snprintf(out, c, "%+.2f x", double(tapeSpeed(v))); return;
                case K::Pitch: std::snprintf(out, c, "%+.1f st", double(pitchSemi(v))); return;
                case K::Mode: std::snprintf(out, c, "%s", v > 0.5f ? "LOOP" : "ONCE"); return;
                default: break;
            }
            break;
    }
    std::snprintf(out, c, "%d", int(v * 100.0f + 0.5f));
}

}  // namespace bulut
