// BULUT audio engine (engine.h).
//
// The BEADS engine follows Mutable Instruments Beads (MIT): its grain
// duration curve (GranularPlayer::ComputeGrainDuration), its grain envelope
// (GrainEnvelope: attack / sustain / release with smoothness from SHAPE), its
// density law (TriggerGenerator: regular pulses left of noon, random ones
// right of it, seeds only at noon), its grain amplitude (1 / sqrt(active + 1))
// and its media (buffer quantization here, wow and flutter in fx.cpp). The
// grains themselves are rendered by BULUT's own block kernels: Beads' full
// granular processor is several times the CPU of a channel here.
#include "ws/engine.h"

#include <cmath>
#include <cstring>

#include "fastrun.h"


#include "ws/prof.h"

namespace bulut {
#ifdef BULUT_PROF
double gProf[20], gProfMax[20], gProfBlock[20];
int gProfBlocks = 0;
uint32_t gProfLast = 0;
#endif

using lab::kBlock;
namespace dsp = lab::dsp;

namespace {

float gSine[1025];
bool gTables = false;

void buildSine() {
    if (gTables) return;
    gTables = true;
    for (int i = 0; i <= 1024; ++i) gSine[i] = sinf(dsp::kTwoPi * float(i) / 1024.0f);
}

// The window shapes, analytic (tables are built from it; tests read it directly).
float windowFn(int t, float x) {
    if (x <= 0.0f || x >= 1.0f) return 0.0f;
    const float hann = 0.5f - 0.5f * cosf(dsp::kTwoPi * x);
    if (t < 6) {   // Tukey: flat top, fades from 5 % (t = 0) to 50 % (t = 5, = Hann)
        const float f = 0.05f + 0.09f * float(t);
        if (x < f) return 0.5f - 0.5f * cosf(dsp::kPi * x / f);
        if (x > 1.0f - f) return 0.5f - 0.5f * cosf(dsp::kPi * (1.0f - x) / f);
        return 1.0f;
    }
    if (t < kTexWindows) {   // Hann -> narrowing gaussians under a Hann
        const float sigma = 0.30f - 0.022f * float(t - 6);
        const float d = (x - 0.5f) / sigma;
        return expf(-0.5f * d * d) * hann;
    }
    if (t == kWinGabor) {
        const float d = (x - 0.5f) / 0.16f;
        return expf(-0.5f * d * d) * hann;
    }
    if (t == kWinPerc) return x < 0.03f ? x / 0.03f : expf(-5.0f * (x - 0.03f)) * (1.0f - x) * 1.03f;
    if (t == kWinExpodec) return x < 0.01f ? x / 0.01f : expf(-6.0f * (x - 0.01f)) * (1.0f - x);
    // rexpodec: the mirror
    const float y = 1.0f - x;
    return y < 0.01f ? y / 0.01f : expf(-6.0f * (y - 0.01f)) * (1.0f - y);
}

inline float sineAt(uint32_t ph) {
    const uint32_t i = ph >> 22;
    const float f = float(ph & 0x3FFFFFu) * (1.0f / 4194304.0f);
    const float a = gSine[i];
    return a + (gSine[i + 1] - a) * f;
}

inline float winAt(const float* w, float ph) {
    const int i = int(ph);
    const float f = ph - float(i);
    const float a = w[i];
    return a + (w[i + 1] - a) * f;
}

inline int32_t sat16(int32_t v) {
#if defined(__ARM_FEATURE_SAT) && __ARM_FEATURE_SAT
    return __builtin_arm_ssat(v, 16);
#else
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
#endif
}
inline int16_t f2s16(float v) {
    const float c = v > 2.0e9f ? 2.0e9f : (v < -2.0e9f ? -2.0e9f : v);
    return int16_t(sat16(int32_t(c)));
}

// Gaussian-ish random (sum of 3 uniforms), mean 0, sd ~0.5.
inline float gauss(dsp::Rng& r) { return (r.uni() + r.uni() + r.uni() - 1.5f); }

// ---- grain renderers (inner loops) ------------------------------------------
// The envelope is evaluated once per 16-sample segment and ramped linearly in
// between. Buffer reads use integer interpolation and skip the ring mask when
// the segment does not wrap. Synthetic particles are quadrature oscillators
// (one complex rotation per sample), renormalised once per segment.
constexpr uint32_t kSeg = 16;
constexpr float kWinEnd = float(kWin) - 0.001f;

// Beads' grain envelope (GrainEnvelope::Render) as a function of the grain's
// progress x (0..1): attack t, or t^4 in the release, blended towards a
// smoothstep by the smoothness.
inline float beadsEnv(const Grain& g, float x) {
    float t, base;
    if (x < g.envA) {
        t = x / g.envA;
        base = t;
    } else if (x < g.envA + g.envS) {
        return 1.0f;
    } else {
        const float r = 1.0f - g.envA - g.envS;
        t = r > 1e-4f ? (1.0f - x) / r : 0.0f;
        t = t < 0.0f ? 0.0f : t;
        base = (t * t) * (t * t);
    }
    const float ss = t * t * (3.0f - 2.0f * t);
    return base + (ss - base) * g.envSm;
}

inline float envAt(const Grain& g, float ph) {
    if (ph > kWinEnd) ph = kWinEnd;
    return g.win ? winAt(g.win, ph) : beadsEnv(g, ph * (1.0f / float(kWin)));
}

inline float segSlope(const Grain& g, float& wph, uint32_t m, float w0) {
    wph += g.winc * float(m);
    return (envAt(g, wph) - w0) * (m == kSeg ? 1.0f / float(kSeg) : 1.0f / float(m));
}

inline void renorm(float& c, float& s) {
    const float k = 1.5f - 0.5f * (c * c + s * s);
    c *= k;
    s *= k;
}

OT_FASTRUN_FN void runBuf(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    const int16_t* __restrict src = g.src;
    const uint32_t mask = g.mask;
    uint32_t pos = g.pos;
    const int32_t inc = g.inc;
    float wph = g.wph;
    const float gl = g.gl, gr = g.gr;
    float wv = envAt(g, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(g, wph, m, wv);
        float* __restrict l = L + i0;
        float* __restrict r = R + i0;
        pos = (((pos >> 12) & mask) << 12) | (pos & 4095u);
        const int32_t span = (inc * int32_t(m)) >> 12;
        const int32_t i0x = int32_t(pos >> 12);
        if (i0x + span + 2 <= int32_t(mask) && i0x + span >= 0) {
            for (uint32_t i = 0; i < m; ++i) {
                const int16_t* p = src + (pos >> 12);
                const int32_t a = p[0];
                const int32_t d = p[1] - a;
                const int32_t v = a + ((d * int32_t(pos & 4095u)) >> 12);
                const float x = float(v) * wv;
                l[i] += x * gl;
                r[i] += x * gr;
                pos += uint32_t(inc);
                wv += dw;
            }
        } else {
            for (uint32_t i = 0; i < m; ++i) {
                const uint32_t idx = pos >> 12;
                const int32_t a = src[idx & mask];
                const int32_t d = src[(idx + 1) & mask] - a;
                const int32_t v = a + ((d * int32_t(pos & 4095u)) >> 12);
                const float x = float(v) * wv;
                l[i] += x * gl;
                r[i] += x * gr;
                pos += uint32_t(inc);
                wv += dw;
            }
        }
    }
    g.pos = pos;
    g.wph = wph;
}

// sine particle (Gabor, grainlet) and glisson (frequency glide per segment)
OT_FASTRUN_FN void runSine(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    float c = g.c, s = g.s, cr = g.cr, sr = g.sr;
    float wph = g.wph;
    const float gl = g.gl, gr = g.gr;
    float wv = envAt(g, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(g, wph, m, wv);
        float* __restrict l = L + i0;
        float* __restrict r = R + i0;
        for (uint32_t i = 0; i < m; ++i) {
            const float x = s * wv;
            l[i] += x * gl;
            r[i] += x * gr;
            const float t = c * cr - s * sr;
            s = s * cr + c * sr;
            c = t;
            wv += dw;
        }
        renorm(c, s);
        if (g.glide != 1.0f) {
            g.w *= g.glide;
            if (g.w > 2.9f) g.w = 2.9f;
            const uint32_t ph = uint32_t(g.w * (4294967296.0f / 6.2831853f));
            sr = sineAt(ph);
            cr = sineAt(ph + 0x40000000u);
        }
    }
    g.c = c;
    g.s = s;
    g.cr = cr;
    g.sr = sr;
    g.wph = wph;
}

// pulsar: a pulsaret (formant sine under a half-sine) at the start of each period
OT_FASTRUN_FN void runPulsar(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    float c = g.c, s = g.s, c2 = g.c2, s2 = g.s2;
    const float cr = g.cr, sr = g.sr, cr2 = g.cr2, sr2 = g.sr2;
    uint32_t ph = g.ph;
    const uint32_t inc = g.inc32, duty = g.duty;
    float wph = g.wph;
    const float gl = g.gl, gr = g.gr;
    float wv = envAt(g, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(g, wph, m, wv);
        float* __restrict l = L + i0;
        float* __restrict r = R + i0;
        for (uint32_t i = 0; i < m; ++i) {
            if (ph < duty) {
                const float x = s * s2 * wv;
                l[i] += x * gl;
                r[i] += x * gr;
                float t = c * cr - s * sr;
                s = s * cr + c * sr;
                c = t;
                t = c2 * cr2 - s2 * sr2;
                s2 = s2 * cr2 + c2 * sr2;
                c2 = t;
            }
            const uint32_t nph = ph + inc;
            if (nph < ph) {   // a new period: the pulsaret starts again
                c = 1.0f;
                s = 0.0f;
                c2 = 1.0f;
                s2 = 0.0f;
            }
            ph = nph;
            wv += dw;
        }
    }
    g.c = c;
    g.s = s;
    g.c2 = c2;
    g.s2 = s2;
    g.ph = ph;
    g.wph = wph;
}

// trainlet: band-limited pulse train sin((2N+1)y) / sin(y) - 1, y = half the phase,
// with its own exponential decay folded into the envelope ramp
OT_FASTRUN_FN void runTrain(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    float c = g.c, s = g.s, c2 = g.c2, s2 = g.s2;
    const float cr = g.cr, sr = g.sr, cr2 = g.cr2, sr2 = g.sr2;
    float wph = g.wph;
    const float gl = g.gl, gr = g.gr, top = float(g.plen);
    float env = g.env;
    const float decay = g.decay;
    float wv = envAt(g, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float w0 = wv;
        const float dwin = segSlope(g, wph, m, wv);
        const float e1 = env * (m == kSeg ? decay : 1.0f);
        float a = w0 * env * g.norm;
        const float da = ((w0 + dwin * float(m)) * e1 * g.norm - a) * (1.0f / float(m));
        float* __restrict l = L + i0;
        float* __restrict r = R + i0;
        for (uint32_t i = 0; i < m; ++i) {
            const float den = s2;
            const float d = (den > 0.003f || den < -0.003f) ? s / den : top;
            const float x = (d - 1.0f) * a;
            l[i] += x * gl;
            r[i] += x * gr;
            float t = c * cr - s * sr;
            s = s * cr + c * sr;
            c = t;
            t = c2 * cr2 - s2 * sr2;
            s2 = s2 * cr2 + c2 * sr2;
            c2 = t;
            a += da;
        }
        wv = w0 + dwin * float(m);
        env = e1;
        renorm(c, s);
        renorm(c2, s2);
    }
    g.c = c;
    g.s = s;
    g.c2 = c2;
    g.s2 = s2;
    g.env = env;
    g.wph = wph;
}

}  // namespace

void buildWindowTables(float* out) {
    for (int t = 0; t < kWindows; ++t)
        for (int i = 0; i <= kWin; ++i) out[t * (kWin + 1) + i] = windowFn(t, float(i) / float(kWin));
}

float windowValue(int table, float x) {
    if (table < 0) table = 0;
    if (table >= kWindows) table = kWindows - 1;
    return windowFn(table, x);
}

const float* Engine::windowTable(int i) const {
    return win_ + (i < 0 ? 0 : (i >= kWindows ? kWindows - 1 : i)) * (kWin + 1);
}

// Semitone offset snapped to a scale (FREE = unchanged).
float scaleSnap(float semis, int scale) {
    static const int8_t kMaj[7] = {0, 2, 4, 5, 7, 9, 11}, kMin[7] = {0, 2, 3, 5, 7, 8, 10}, kPen[5] = {0, 2, 4, 7, 9};
    if (scale <= kScaleFree || scale >= kScales) return semis;
    const float oct = floorf(semis / 12.0f);
    const float in = semis - oct * 12.0f;   // 0..12
    float best = 0.0f, bd = 99.0f;
    auto consider = [&](float c) {
        const float d = fabsf(in - c);
        if (d < bd) {
            bd = d;
            best = c;
        }
    };
    switch (scale) {
        case kScaleOct: consider(0.0f); consider(12.0f); break;
        case kScaleFifth: consider(0.0f); consider(7.0f); consider(12.0f); break;
        case kScaleMajor: for (int8_t s : kMaj) consider(float(s)); consider(12.0f); break;
        case kScaleMinor: for (int8_t s : kMin) consider(float(s)); consider(12.0f); break;
        default: for (int8_t s : kPen) consider(float(s)); consider(12.0f); break;
    }
    return oct * 12.0f + best;
}

// Column k (0..11) of the pad grid as a pitch step in a scale (QUANTA synthetic particles).
static float columnSemis(int col, int scale) {
    static const int8_t kMaj[7] = {0, 2, 4, 5, 7, 9, 11}, kMin[7] = {0, 2, 3, 5, 7, 8, 10}, kPen[5] = {0, 2, 4, 7, 9};
    switch (scale) {
        case kScaleOct: return float(12 * (col / 3) - 12);                 // 3 columns per octave, -1..+2 octaves
        case kScaleFifth: return float(12 * (col / 2) + 7 * (col & 1) - 12);   // root and fifth, from -1 octave
        case kScaleMajor: return float(12 * (col / 7) + kMaj[col % 7]);
        case kScaleMinor: return float(12 * (col / 7) + kMin[col % 7]);
        case kScalePent: return float(12 * (col / 5) + kPen[col % 5]);
        default: return float(col);
    }
}

const char* dlyDivName(int i) {
    static const char* const k[kDlyDivs] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
    return k[i < 0 ? 0 : (i >= kDlyDivs ? kDlyDivs - 1 : i)];
}
float dlyDivBeats(int i) {
    static const float k[kDlyDivs] = {0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    return k[i < 0 ? 0 : (i >= kDlyDivs ? kDlyDivs - 1 : i)];
}

void defaultEngineKnobs(int engine, float* k, int ch) {
    for (int i = 0; i < kKnobs; ++i) k[i] = 0.0f;
    switch (engine) {
        case kGranular:
            k[K::Pos] = 0.5f; k[K::Size] = 0.55f; k[K::Dens] = 0.55f; k[K::Pitch] = 0.5f; k[K::Spread] = 0.6f;
            k[K::Shape] = 0.45f; k[K::Scatter] = 0.15f; k[K::Feedback] = 0.0f; k[K::Mode] = 0.0f; k[K::Rand] = 0.0f;
            k[K::Scale] = 0.0f; k[K::Extra] = 0.1f;
            break;
        case kQuanta:
            k[K::Pos] = 0.5f; k[K::Size] = 0.45f; k[K::Dens] = 0.45f; k[K::Pitch] = 0.45f; k[K::Spread] = 0.7f;
            k[K::Shape] = 0.5f; k[K::Scatter] = 0.25f; k[K::Feedback] = 0.2f;
            k[K::Mode] = enumValue(ch & 1 ? kQGlisson : kQGabor, kQTypes); k[K::Rand] = enumValue(kLawPoisson, kLaws);
            k[K::Scale] = 0.0f; k[K::Extra] = 0.0f;
            break;
        case kBeads:
            k[K::Pos] = 0.7f; k[K::Size] = 0.6f; k[K::Dens] = 0.7f; k[K::Pitch] = 0.5f; k[K::Spread] = 0.5f;
            k[K::Shape] = 0.5f; k[K::Scatter] = 0.15f; k[K::Feedback] = 0.0f; k[K::Mode] = 0.0f; k[K::Rand] = 0.0f;
            k[K::Scale] = 0.1f; k[K::Extra] = 0.1f;
            break;
        default:   // TAPE
            k[K::Pos] = 0.0f; k[K::Size] = 0.45f; k[K::Dens] = 0.75f; k[K::Pitch] = 0.5f; k[K::Spread] = 0.3f;
            k[K::Shape] = 0.5f; k[K::Scatter] = 0.0f; k[K::Feedback] = 0.0f; k[K::Mode] = 1.0f; k[K::Rand] = 1.0f;
            k[K::Scale] = 0.0f; k[K::Extra] = 0.0f;
            break;
    }
}

// ---- init ---------------------------------------------------------------------
// DIRTY TAPE's recording format: Beads' mu-law round trip, tabulated on
// 12 bits (8 KB in SRAM) at init: a lookup per sample instead of the codec.
static int16_t sMuLaw[4096];
static void buildMuLaw() {
    for (int i = 0; i < 4096; ++i) {
        const int16_t v = int16_t(i * 16 - 32768 + 8);
        sMuLaw[i] = int16_t(beads::MuLaw2Lin(beads::Lin2MuLaw(v)) * 32768.0f);
    }
}

bool Engine::init(lab::Arena& arena, ChParams* params, ChState* st, ChSource* src, Gesture* gestures, FxState* fx) {
    buildMuLaw();
    buildSine();
    p_ = params;
    st_ = st;
    src_ = src;
    gest_ = gestures;
    fx_ = fx;
    win_ = static_cast<float*>(arena.alloc(kWindows * (kWin + 1) * sizeof(float)));
    if (!win_) return false;
    buildWindowTables(win_);
    // the diffusers' lines are touched every sample: SRAM (4 x 8 KB)
    static float sDiff[kCh][2048];
    for (int c = 0; c < kCh; ++c) {
        rt[c].buf = static_cast<int16_t*>(arena.alloc(kBufLen * 2));
        rt[c].frz = static_cast<int16_t*>(arena.alloc(kFrzLen * 2));
        if (!rt[c].buf || !rt[c].frz) return false;
        rt[c].logistic = 0.31f + 0.07f * float(c);
        if (!chain[c].init(arena, sDiff[c])) return false;
        gen[c].init(0x5EED0000u + uint32_t(c) * 7919u);
    }
    capBuf = static_cast<int16_t*>(arena.alloc(kBufLen * 2));
    mixHist = static_cast<int16_t*>(arena.alloc(kMixHist * 2));
    int16_t* dmem = static_cast<int16_t*>(arena.alloc(ot::audio::Delay::kMemorySamples * 2));
    rev_ = arena.make<ot::audio::Reverb>();
    int16_t* rmem = static_cast<int16_t*>(arena.alloc(ot::audio::Reverb::kMemorySamples * 2));
    if (!capBuf || !mixHist || !dmem || !rev_ || !rmem) return false;
    dly_.init(dmem);
    dly_.setPingPong(true);
    rev_->init(rmem);
    rev_->setAmount(1.0f);
    rev_->setInputGain(0.22f);
    rev_->setDiffusion(0.7f);
    return true;
}

// ---- emitters ------------------------------------------------------------------
void Engine::padDown(int c, int pad, int vel) {
    ChRt& r = rt[c];
    int slot = -1;
    for (int i = 0; i < kLiveEmitters; ++i)
        if (r.em[i].on && r.em[i].pad == pad) slot = i;
    for (int i = 0; i < kLiveEmitters && slot < 0; ++i)
        if (!r.em[i].on) slot = i;
    if (slot < 0) slot = 0;   // steal the first
    Emitter& e = r.em[slot];
    e.on = 0;
    e.pad = uint8_t(pad);
    e.vel = uint8_t(vel < 1 ? 1 : (vel > 127 ? 127 : vel));
    e.semis = 0.0f;
    e.acc = 0.0f;   // first grain right away
    e.fresh = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e.on = 1;
}
void Engine::padUp(int c, int pad) {
    for (int i = 0; i < kLiveEmitters; ++i)
        if (rt[c].em[i].pad == pad) rt[c].em[i].on = 0;
}
void Engine::noteOn(int c, int note, int vel) {
    ChRt& r = rt[c];
    int slot = -1;
    for (int i = 0; i < kLiveEmitters && slot < 0; ++i)
        if (!r.em[i].on) slot = i;
    if (slot < 0) slot = kLiveEmitters - 1;
    Emitter& e = r.em[slot];
    e.on = 0;
    e.pad = 0xFF;
    e.note = uint8_t(note & 127);
    e.vel = uint8_t(vel < 1 ? 1 : vel);
    e.semis = float(note - 60);
    e.acc = 0.0f;
    e.fresh = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e.on = 1;
}
void Engine::noteOff(int c, int note) {
    for (int i = 0; i < kLiveEmitters; ++i)
        if (rt[c].em[i].pad == 0xFF && rt[c].em[i].note == note) rt[c].em[i].on = 0;
}
void Engine::releaseAll(int c) {
    for (int i = 0; i < kEmitters; ++i) rt[c].em[i].on = 0;
}
void Engine::releaseGhosts(int c) {
    for (int i = kLiveEmitters; i < kEmitters; ++i) rt[c].em[i].on = 0;
}
void Engine::resetGestures() {
    for (int c = 0; c < kCh; ++c) {
        rt[c].gphase = 0.0f;
        rt[c].gidx = 0;
    }
}

// ---- grain allocation ------------------------------------------------------------
Grain* Engine::alloc(int c) {
    if (spawnsLeft_ <= 0 || nActive_ >= grainLimit || nActive_ >= kMaxGrains || rt[c].active >= cloudBudget) {
        ++rt[c].refused;
        refusedTotal = refusedTotal + 1;
        return nullptr;
    }
    --spawnsLeft_;
    Grain* g = &pool_[nActive_++];
    std::memset(g, 0, sizeof *g);
    g->cloud = uint8_t(c);
    ++rt[c].active;
    ++rt[c].spawned;
    return g;
}

void Engine::spark(int c, uint8_t kind, float pos, float semis, float amp, uint32_t len, bool live, int pad) {
    Spark s;
    s.cloud = uint8_t(c);
    s.kind = kind;
    s.pos = uint16_t(dsp::clamp(pos, 0.0f, 1.0f) * 65535.0f);
    s.semis = int8_t(dsp::clamp(semis, -48.0f, 48.0f));
    s.amp = uint8_t(dsp::clamp(amp, 0.0f, 1.0f) * 255.0f);
    const uint32_t l10 = len / 441u;
    s.life = uint8_t(l10 > 255 ? 255 : l10);
    s.live = uint8_t((live ? 0x80 : 0) | (pad >= 0 && pad < 48 ? pad / 12 : 7));
    sparks.push(s);
}

// The readable span of a channel's source: base index, length, buffer and mask.
// false = nothing to read (an empty sample, a buffer being replaced).
bool Engine::sourceSpan(int c, uint32_t& base, uint32_t& len, const int16_t*& src, uint32_t& mask) const {
    const ChRt& r = rt[c];
    if (r.busy) return false;
    if (st_[c].freeze && r.frzReady) {
        base = 0;
        len = kFrzLen;
        src = r.frz;
        mask = kFrzMask;
        return true;
    }
    src = r.buf;
    mask = kBufMask;
    if (p_[c].source == kSrcSample) {
        if (r.sampleLen < 64) return false;
        base = 0;
        len = r.sampleLen;
        return true;
    }
    base = (r.w - kLiveSpan) & kBufMask;
    len = kLiveSpan;
    return true;
}

// Region-mapped 0..1 position -> fraction of the source span.
static inline float regionMap(const ChParams& p, float x) {
    float a = p.regStart, b = p.regEnd;
    if (b < a) {
        const float t = a;
        a = b;
        b = t;
    }
    if (b - a < 0.002f) b = a + 0.002f;
    return dsp::clamp(a + x * (b - a), 0.0f, 1.0f);
}

// Inter-onset interval in samples for a mean rate (GRANULAR / QUANTA laws).
float Engine::nextInterval(int c, int engine, const float* k, float hz) {
    const float mean = dsp::kSr / (hz < 0.1f ? 0.1f : hz);
    if (engine != kQuanta) return mean * (0.55f + 0.9f * rng.uni());
    const float u = rng.uni();
    switch (enumIndex(k[K::Rand], kLaws)) {
        case kLawSync: return mean;
        case kLawPoisson: return -mean * 0.69314718f * flog2(u < 1e-4f ? 1e-4f : u);
        case kLawGauss: return mean * dsp::clamp(1.0f + 0.7f * gauss(rng), 0.1f, 3.0f);
        case kLawCauchy: {
            const uint32_t ph = uint32_t((dsp::clamp(u, 0.02f, 0.98f) - 0.5f) * 2147483648.0f);
            const float t = sineAt(ph) / sineAt(ph + 0x40000000u);   // tan(pi (u - 0.5))
            return mean * dsp::clamp(fabsf(1.0f + 0.6f * t), 0.04f, 10.0f);
        }
        default: {
            float& x = rt[c].logistic;
            x = 3.91f * x * (1.0f - x);
            if (x < 0.001f || x > 0.999f) x = 0.37f;
            return mean * (0.15f + 1.7f * x);
        }
    }
}

// Pan law shared by the spawners (equal power, *1.41 so the centre is unity).
static inline void panGains(float pan, float amp, float& gl, float& gr) {
    pan = dsp::clamp(pan, 0.0f, 1.0f);
    gl = amp * sqrtf(1.0f - pan) * 1.41f;
    gr = amp * sqrtf(pan) * 1.41f;
}

void Engine::spawnGranular(int c, const Emitter& e, int offset, bool live) {
    const float* k = p_[c].k;
    uint32_t base, slen, mask;
    const int16_t* src;
    if (!sourceSpan(c, base, slen, src, mask)) return;
    Grain* g = alloc(c);
    if (!g) return;
    const bool liveBuf = src == rt[c].buf && p_[c].source != kSrcSample;
    const float size = sizeSec(k[K::Size]);
    const uint32_t len = uint32_t(size * dsp::kSr) < 64 ? 64 : uint32_t(size * dsp::kSr);
    float semis = pitchSemi(k[K::Pitch]) + e.semis;
    const float pr = k[K::Rand];
    if (pr > 0.01f) semis += scaleSnap(gauss(rng) * 24.0f * pr, enumIndex(k[K::Scale], kScales));
    const float ratio = fexp2(semis * (1.0f / 12.0f));
    const bool rev = rng.uni() < k[K::Mode];
    float x = e.pad == 0xFF ? k[K::Pos] : padPos(e.pad);
    x = dsp::clamp(x + gauss(rng) * k[K::Scatter] * 0.25f, 0.0f, 1.0f);
    const float pos = regionMap(p_[c], x);
    uint32_t start;
    if (liveBuf) {
        float back = (1.0f - pos) * float(kLiveSpan);
        const float need = float(len) * (ratio > 1.0f && !rev ? ratio - 1.0f : 0.0f) + 256.0f;
        const float needRev = rev ? float(len) * ratio + 256.0f : 0.0f;
        if (back < need) back = need;
        if (back < needRev) back = needRev;
        start = (rt[c].w - uint32_t(back) - uint32_t(offset)) & kBufMask;
    } else {
        start = base + uint32_t(pos * float(slen - 1));
        if (rev && src != rt[c].frz) start = start + uint32_t(float(len) * ratio) < slen ? start + uint32_t(float(len) * ratio) : slen - 1;
    }
    g->src = src;
    g->mask = mask;
    g->kind = kGBuf;
    g->pos = (start & mask) << 12;
    const float incf = ratio * 4096.0f;
    g->inc = rev ? -int32_t(incf) : int32_t(incf);
    g->win = windowTable(enumIndex(k[K::Shape], kTexWindows));
    g->winc = float(kWin) / float(len);
    g->left = len;
    g->delay = uint32_t(offset);
    const float overlap = densHz(k[K::Dens]) * size;
    float amp = (float(e.vel) / 127.0f) / sqrtf(overlap < 1.0f ? 1.0f : overlap) * 1.5f;
    amp *= 1.0f - k[K::Extra] * rng.uni();
    float gl, gr;
    panGains(0.5f + (rng.uni() - 0.5f) * k[K::Spread], amp * (1.0f / 32768.0f), gl, gr);
    g->gl = gl;
    g->gr = gr;
    g->live = live ? 1 : 0;
    spark(c, src == rt[c].frz ? 1 : 0, x, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
}

void Engine::spawnQuanta(int c, const Emitter& e, int offset, bool live) {
    ChRt& r = rt[c];
    const float* k = p_[c].k;
    const int type = enumIndex(k[K::Mode], kQTypes);
    if (k[K::Extra] > 0.001f && rng.uni() < k[K::Extra]) return;   // MASK: stochastic masking
    const float dur = durSec(k[K::Size]) * (0.7f + 0.6f * rng.uni());
    uint32_t len = uint32_t(dur * dsp::kSr) < 16 ? 16 : uint32_t(dur * dsp::kSr);
    const float pan = 0.5f + (rng.uni() - 0.5f) * k[K::Spread];
    const float vel = float(e.vel) / 127.0f;
    float x = e.pad == 0xFF ? k[K::Pos] : padPos(e.pad);
    if (type == kQSample) {
        uint32_t base, slen, mask;
        const int16_t* src;
        if (!sourceSpan(c, base, slen, src, mask)) return;
        Grain* g = alloc(c);
        if (!g) return;
        const bool liveBuf = src == r.buf && p_[c].source != kSrcSample;
        x = dsp::clamp(x + gauss(rng) * k[K::Scatter] * 0.25f, 0.0f, 1.0f);
        const float pos = regionMap(p_[c], x);
        const float semis = (k[K::Shape] - 0.5f) * 48.0f + e.semis + gauss(rng) * 12.0f * bandOct(k[K::Scatter]) * 0.5f;
        const float ratio = fexp2(semis * (1.0f / 12.0f));
        uint32_t start;
        if (liveBuf) {
            float back = (1.0f - pos) * float(kLiveSpan);
            const float need = float(len) * (ratio > 1.0f ? ratio : 1.0f) + 256.0f;
            if (back < need) back = need;
            start = (r.w - uint32_t(back) - uint32_t(offset)) & kBufMask;
        } else {
            start = base + uint32_t(pos * float(slen - 1));
        }
        g->src = src;
        g->mask = mask;
        g->kind = kGBuf;
        g->pos = (start & mask) << 12;
        g->inc = int32_t(ratio * 4096.0f);
        g->win = windowTable(kWinGabor);
        g->winc = float(kWin) / float(len);
        g->left = len;
        g->delay = uint32_t(offset);
        float gl, gr;
        panGains(pan, vel * 1.1f * (1.0f / 32768.0f), gl, gr);
        g->gl = gl;
        g->gr = gr;
        g->live = live ? 1 : 0;
        spark(c, 2, x, semis, vel, len, live, e.pad == 0xFF ? -1 : e.pad);
        return;
    }
    Grain* g = alloc(c);
    if (!g) return;
    // frequency: base * column step * gaussian band scatter * drift
    const int scale = enumIndex(k[K::Scale], kScales);
    float semis = e.semis;
    if (e.pad != 0xFF) semis += columnSemis(e.pad % 12, scale);
    semis += gauss(rng) * 12.0f * bandOct(k[K::Scatter]);
    semis += r.drift * 12.0f;
    if (scale != kScaleFree && k[K::Scatter] > 0.01f) semis = scaleSnap(semis, scale);
    float hz = freqHz(k[K::Pitch]) * fexp2(semis * (1.0f / 12.0f));
    hz = dsp::clamp(hz, 15.0f, 15000.0f);
    const float wrad = dsp::kTwoPi * hz * dsp::kInvSr;   // radians per sample
    auto rot = [](float rad, float& cr, float& sr) {
        const uint32_t ph = uint32_t(rad * (4294967296.0f / 6.2831853f));
        sr = sineAt(ph);
        cr = sineAt(ph + 0x40000000u);
    };
    const float ch = k[K::Shape];
    if (type == kQGrainlet) {
        // the grainlet: its length is N cycles of its own frequency
        const float cycles = 2.0f + ch * 30.0f;
        float d = cycles / hz;
        d = dsp::clamp(d, 0.001f, 0.12f);
        len = uint32_t(d * dsp::kSr);
    }
    g->winc = float(kWin) / float(len);
    g->left = len;
    g->delay = uint32_t(offset);
    g->glide = 1.0f;
    float amp = vel * 0.42f;
    switch (type) {
        case kQGlisson:
        case kQGabor:
        case kQGrainlet: {
            g->kind = kGSine;
            const uint32_t ph0 = rng.next();
            g->c = sineAt(ph0 + 0x40000000u);
            g->s = sineAt(ph0);
            rot(wrad, g->cr, g->sr);
            g->w = wrad;
            if (type == kQGlisson) {
                const float glideOct = (ch - 0.5f) * 6.0f;   // -3 .. +3 octaves over the particle
                const float segs = float(len) / 16.0f;
                g->glide = fexp2(glideOct / (segs < 1.0f ? 1.0f : segs));
                g->win = windowTable(5);
            } else if (type == kQGrainlet) {
                g->win = windowTable(kWinGabor);
            } else {
                g->win = windowTable(6 + enumIndex(1.0f - ch, 10));   // CHAR = wider .. narrower gaussian
            }
            break;
        }
        case kQPulsar: {
            g->kind = kGPulsar;
            const float ratio = 1.0f + ch * 7.0f;          // formant / fundamental
            g->inc32 = uint32_t(hz * (4294967296.0f / dsp::kSr));
            g->duty = uint32_t(4294967295.0f / ratio);     // pulsaret = one formant cycle
            g->ph = 0;
            g->c = g->c2 = 1.0f;
            g->s = g->s2 = 0.0f;
            const float fr = dsp::clamp(wrad * ratio, 0.0f, 3.0f);
            rot(fr, g->cr, g->sr);
            rot(fr * 0.5f, g->cr2, g->sr2);
            g->win = windowTable(2);
            amp *= 0.9f;
            break;
        }
        default: {   // trainlet
            g->kind = kGTrain;
            int nh = 1 + int(ch * 15.0f);
            while (nh > 1 && hz * float(nh) > 15000.0f) --nh;
            const int m = 2 * nh + 1;
            g->plen = uint32_t(m);
            g->norm = 1.0f / float(2 * nh);
            g->env = 1.0f;
            g->decay = fexp(-4.0f * 16.0f / float(len));
            g->c = g->c2 = 1.0f;
            g->s = g->s2 = 0.0f;
            rot(wrad * 0.5f * float(m), g->cr, g->sr);
            rot(wrad * 0.5f, g->cr2, g->sr2);
            g->win = windowTable(kWinPerc);
            amp *= 0.8f;
            break;
        }
    }
    float gl, gr;
    panGains(pan, amp, gl, gr);
    g->gl = gl;
    g->gr = gr;
    g->live = live ? 1 : 0;
    const float sx = e.pad != 0xFF ? padPos(e.pad) + (semis - columnSemis(e.pad % 12, scale)) * 0.006f
                                   : flog2(hz / 30.0f) * (0.69314718f / 8.1f);
    spark(c, uint8_t(3 + (type > 5 ? 5 : type)), sx, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
}

// BEADS: Beads' duration curve, envelope, pitch / time randomization and amplitude law.
void Engine::spawnBeads(int c, const Emitter& e, int offset, bool live) {
    const float* k = p_[c].k;
    uint32_t base, slen, mask;
    const int16_t* src;
    if (!sourceSpan(c, base, slen, src, mask)) return;
    Grain* g = alloc(c);
    if (!g) return;
    const bool liveBuf = src == rt[c].buf && p_[c].source != kSrcSample;
    // SIZE (+ SIZE RND): ComputeGrainDuration -- left of 1/3 the grains play backwards
    float size = dsp::clamp(k[K::Size] + (rng.uni() - 0.5f) * k[K::Scale], 0.0f, 1.0f);
    const bool rev = size < 0.3333333f;
    const float clamped = rev ? size * 3.0f * (2.0f - size * 3.0f) : 1.0f - 1.5f * (size - 0.3333333f);
    const float shortness = 1.025f * clamped < 1.0f ? 1.025f * clamped : 1.0f;
    float durS = fexp2(shortness * -9.0f) * 4.0f;   // SemitonesToRatio(shortness * -108) * 4 s
    if (durS > 2.5f) durS = 2.5f;
    const uint32_t len = uint32_t(durS * dsp::kSr) < 96 ? 96 : uint32_t(durS * dsp::kSr);
    // PITCH (+ PITCH RND)
    const float semis = pitchSemi(k[K::Pitch]) + e.semis + gauss(rng) * 24.0f * k[K::Rand];
    const float ratio = fexp2(semis * (1.0f / 12.0f));
    // TIME (+ TIME RND)
    float x = e.pad == 0xFF ? k[K::Pos] : padPos(e.pad);
    x = dsp::clamp(x + (rng.uni() - 0.5f) * k[K::Scatter], 0.0f, 1.0f);
    const float pos = regionMap(p_[c], x);
    uint32_t start;
    if (liveBuf) {
        float back = (1.0f - pos) * float(kLiveSpan);
        const float need = rev ? float(len) * ratio + 256.0f : float(len) * (ratio > 1.0f ? ratio - 1.0f : 0.0f) + 256.0f;
        if (back < need) back = need;
        start = (rt[c].w - uint32_t(back) - uint32_t(offset)) & kBufMask;
    } else {
        start = base + uint32_t(pos * float(slen - 1));
    }
    g->src = src;
    g->mask = mask;
    g->kind = kGBuf;
    g->pos = (start & mask) << 12;
    g->inc = rev ? -int32_t(ratio * 4096.0f) : int32_t(ratio * 4096.0f);
    // SHAPE (+ SHAPE RND): GrainEnvelope::Start
    float shape = dsp::clamp(k[K::Shape] + (rng.uni() - 0.5f) * k[K::Extra], 0.0f, 1.0f);
    const float sustain = (1.0f - 3.0f * shape) > 0.0f ? 1.0f - 3.0f * shape : 0.0f;
    shape = (shape - 0.33333333f) * 1.5f;
    if (shape < 0.0f) shape = 0.0f;
    else if (shape < 0.5f) shape = shape * shape * shape * 4.0f;
    const float attack = shape;
    const float release = (1.0f - attack) * (1.0f - sustain);
    const float fl = float(len);
    const float aDur = fl * attack > 0.0005f * kSr * (1.0f + 3.0f * sustain) ? fl * attack : 0.0005f * kSr * (1.0f + 3.0f * sustain);
    const float sDur = fl * sustain > 2.0f ? fl * sustain : 2.0f;
    const float rDur = fl * release > 0.010f * kSr ? fl * release : 0.010f * kSr;
    const float tot = aDur + sDur + rDur;
    g->envA = aDur / tot;
    g->envS = sDur / tot;
    g->envSm = attack < 0.5f ? 2.0f * attack : 2.0f - 2.0f * attack;
    g->win = nullptr;
    g->winc = float(kWin) / tot;
    g->left = uint32_t(tot);
    g->delay = uint32_t(offset);
    // amplitude 1 / sqrt(active grains + 1), pan spread (ComputeGrainAmplitudeAndPan)
    const float amp = (float(e.vel) / 127.0f) / sqrtf(float(rt[c].active) + 1.0f) * 1.3f;
    const float spread = k[K::Spread];
    const float pan = 0.5f + (rng.uni() - 0.5f) * (spread * rng.uni() < 1.0f ? spread * rng.uni() : 1.0f);
    float gl, gr;
    panGains(pan, amp * (1.0f / 32768.0f), gl, gr);
    g->gl = gl;
    g->gr = gr;
    g->live = live ? 1 : 0;
    spark(c, src == rt[c].frz ? 1 : 0, x, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
}

// TAPE: a playhead that moves at SPEED through the region, read by two
// overlapping grains of GRAIN length at PITCH (time-stretch); pads jump to a
// slice and loop a LENGTH (rows: 1, 1/2, 1/4, 1/8 of it).
void Engine::spawnTape(int c, Emitter& e, int offset, bool live) {
    const float* k = p_[c].k;
    uint32_t base, slen, mask;
    const int16_t* src;
    if (!sourceSpan(c, base, slen, src, mask)) return;
    const bool liveBuf = src == rt[c].buf && p_[c].source != kSrcSample;
    const float glen = tapeGrainSec(k[K::Size]) * kSr;
    float semis = pitchSemi(k[K::Pitch]) + e.semis;
    const int scale = enumIndex(k[K::Scale], kScales);
    if (scale != kScaleFree) semis = scaleSnap(semis, scale);
    const float ratio = fexp2(semis * (1.0f / 12.0f));
    // the region in absolute indices
    const float a = regionMap(p_[c], 0.0f), b = regionMap(p_[c], 1.0f);
    const float rs = float(base) + a * float(slen), rl = (b - a) * float(slen) > 64.0f ? (b - a) * float(slen) : 64.0f;
    if (e.fresh) {
        e.fresh = 0;
        const float x = e.pad == 0xFF ? k[K::Pos] : padSlice(e.pad);
        e.head = rs + x * rl;
        e.loopStart = e.head;
    }
    Grain* g = alloc(c);
    if (!g) return;
    float startF = e.head + (rng.uni() - 0.5f) * k[K::Scatter] * glen * 2.0f;
    if (liveBuf) {
        // keep the grain behind the write head
        float back = float((rt[c].w - uint32_t(startF)) & kBufMask);
        const float need = glen * (ratio > 1.0f ? ratio : 1.0f) + 256.0f;
        if (back < need) startF -= need - back;
    }
    const uint32_t start = uint32_t(startF < 0.0f ? startF + float(kBufLen) : startF);
    g->src = src;
    g->mask = mask;
    g->kind = kGBuf;
    g->pos = (start & mask) << 12;
    g->inc = int32_t(ratio * 4096.0f);
    g->win = windowTable(5 + int(k[K::Shape] * 7.0f));
    const uint32_t len = uint32_t(glen);
    g->winc = float(kWin) / float(len);
    g->left = len;
    g->delay = uint32_t(offset);
    float gl, gr;
    panGains(0.5f + (rng.uni() - 0.5f) * k[K::Spread], (float(e.vel) / 127.0f) * 0.75f * (1.0f / 32768.0f), gl, gr);
    g->gl = gl;
    g->gr = gr;
    g->live = live ? 1 : 0;
    const float shown = slen ? (e.head - float(base)) / float(slen) : 0.0f;
    const float sx = (b - a) > 0.002f ? (shown - a) / (b - a) : 0.0f;
    spark(c, 6, sx, semis, 0.6f, len, live, e.pad == 0xFF ? -1 : e.pad);
}

void Engine::spawn(int c, Emitter& e, int offset, bool live) {
    switch (p_[c].engine) {
        case kQuanta: spawnQuanta(c, e, offset, live); break;
        case kBeads: spawnBeads(c, e, offset, live); break;
        case kTape: spawnTape(c, e, offset, live); break;
        default: spawnGranular(c, e, offset, live); break;
    }
}

// Advance one emitter over the block and spawn its grains (at most 8 per block).
void Engine::runEmitter(int c, Emitter& e, bool live) {
    const ChParams& p = p_[c];
    const float* k = p.k;
    const int eng = p.engine;
    if (eng == kTape) {
        // the playhead moves every block; grains every half grain length
        uint32_t base, slen, mask;
        const int16_t* src;
        if (!sourceSpan(c, base, slen, src, mask)) return;
        float speed = tapeSpeed(k[K::Dens]);
        if (k[K::Extra] > 0.001f) speed *= 1.0f + 0.03f * k[K::Extra] * sinf(float(sampleClock) * 2.4e-5f);
        const float glen = tapeGrainSec(k[K::Size]) * kSr;
        if (e.fresh) spawnTape(c, e, 0, live), e.acc = glen * 0.5f;
        const float a = regionMap(p, 0.0f), b = regionMap(p, 1.0f);
        const float rs = float(base) + a * float(slen), rl = (b - a) * float(slen) > 64.0f ? (b - a) * float(slen) : 64.0f;
        const float loopLen = rl * (k[K::Rand] < 0.02f ? 0.02f : k[K::Rand]) * (e.pad == 0xFF ? 1.0f : padTapeLen(e.pad));
        const bool loop = k[K::Mode] > 0.5f;
        e.head += speed * float(kBlock);
        // the source moved under a live playhead (the ring keeps its time): follow it
        const float ls = e.loopStart;
        if (speed >= 0.0f && e.head >= ls + loopLen) {
            if (loop) e.head -= loopLen;
            else if (e.head >= rs + rl) return;
        } else if (speed < 0.0f && e.head < ls) {
            if (loop) e.head += loopLen;
            else if (e.head < rs) return;
        }
        rt[c].tapeHead = slen ? ((e.head - float(base)) / float(slen) - a) / ((b - a) > 0.002f ? (b - a) : 0.002f) : -1.0f;
        float acc = e.acc;
        int n = 0;
        while (acc < float(kBlock) && n < 3) {
            const int off = acc < 0.0f ? 0 : int(acc);
            if (spawnsLeft_ > 0 && nActive_ < grainLimit && rt[c].active < cloudBudget) spawnTape(c, e, off, live);
            acc += glen * 0.5f;
            ++n;
        }
        if (acc < float(kBlock)) acc = float(kBlock);
        e.acc = acc - float(kBlock);
        return;
    }
    if (eng == kBeads && e.fresh) {
        // a pad press is a seed: one grain now, whatever the density
        e.fresh = 0;
        spawnBeads(c, e, 0, live);
    }
    if (e.acc >= float(kBlock)) {   // no grain due in this block
        e.acc -= float(kBlock);
        return;
    }
    float dv = k[K::Dens];
    float hz;
    bool regular = false;
    if (eng == kBeads) {
        // TriggerGenerator: regular pulses left of noon, random right of it, seeds only at noon
        if (dv < 0.48f) {
            hz = 130.81278f * fexp2(-dv * 16.0f);
            regular = true;
        } else if (dv > 0.52f) {
            hz = 0.5f * 130.81278f * fexp2(-(1.0f - dv) * 16.0f);
        } else {
            e.acc = float(kBlock);
            return;
        }
        if (e.pad != 0xFF) hz *= fexp2(padDensOff(e.pad) * 4.0f);
    } else {
        if (e.pad != 0xFF) dv += padDensOff(e.pad);
        dv = dsp::clamp(dv, 0.0f, 1.0f);
        hz = eng == kQuanta ? rateHz(dv) : densHz(dv);
    }
    float acc = e.acc;
    int n = 0;
    while (acc < float(kBlock) && n < 8) {
        const int off = acc < 0.0f ? 0 : int(acc);
        // a refused grain costs nothing but the interval (the budget check comes first)
        if (spawnsLeft_ > 0 && nActive_ < grainLimit && rt[c].active < cloudBudget) spawn(c, e, off, live);
        else {
            ++rt[c].refused;
            refusedTotal = refusedTotal + 1;
        }
        float iv;
        if (eng == kBeads) {
            if (regular) iv = dsp::kSr / hz;
            else {
                // ratio = -1 / LogOneMinusX(0.1 + 0.9 u)
                const float x = 0.1f + 0.9f * rng.uni();
                const float l1 = -x * (1.0f + x * (0.5f + x * (0.3333f + x * 0.25f)));
                iv = dsp::kSr / (hz * (-1.0f / l1));
            }
        } else {
            iv = n < 2 ? nextInterval(c, eng, k, hz) : dsp::kSr / hz;
        }
        acc += iv;
        ++n;
    }
    if (acc < float(kBlock)) acc = float(kBlock);
    e.acc = acc - float(kBlock);
}

// Gesture playback for one block: events in [phase, phase + block * speed).
void Engine::playGesture(int c) {
    ChRt& r = rt[c];
    if (r.pendG != 0xFF) {
        releaseGhosts(c);
        r.liveG = r.pendG;
        r.pendG = 0xFF;
        r.gphase = 0.0f;
        r.gidx = 0;
    }
    const ChState& s = st_[c];
    const Gesture& g = gest_[c * kGestBufs + r.liveG];
    if (!s.gestOn || g.len == 0 || g.count == 0) {
        if (r.em[kLiveEmitters].on | r.em[kLiveEmitters + 1].on | r.em[kLiveEmitters + 2].on | r.em[kLiveEmitters + 3].on)
            releaseGhosts(c);
        r.gestPos = 0.0f;
        return;
    }
    const float spd = speedMul(int(p_[c].gestSpeed + 0.5f));
    const float step = float(kBlock) * spd;
    float ph = r.gphase;
    float end = ph + step;
    for (int guard = 0; guard < 2; ++guard) {
        const float lim = end < float(g.len) ? end : float(g.len);
        while (r.gidx < g.count && float(g.ev[r.gidx].t) < lim) {
            const GestEvent& ev = g.ev[r.gidx++];
            if (ev.type == kGParam) {
                ParamEvent pe;
                pe.ch = uint8_t(c);
                pe.idx = ev.a;
                pe.v = ev.v;
                paramEvents.push(pe);
            } else if (ev.type == kGPadDown) {
                int k = -1;
                for (int i = 0; i < 4; ++i)
                    if (r.ghostPad[i] == ev.a || !r.em[kLiveEmitters + i].on) {
                        k = i;
                        break;
                    }
                if (k < 0) k = 0;
                Emitter& e = r.em[kLiveEmitters + k];
                r.ghostPad[k] = ev.a;
                e.pad = ev.a;
                e.vel = uint8_t(ev.v ? ev.v : 100);
                e.semis = 0.0f;
                e.acc = (float(ev.t) - ph) / spd;
                if (e.acc < 0.0f) e.acc = 0.0f;
                e.fresh = 1;
                e.on = 1;
            } else if (ev.type == kGPadUp) {
                for (int i = 0; i < 4; ++i)
                    if (r.ghostPad[i] == ev.a) {
                        r.em[kLiveEmitters + i].on = 0;
                        r.ghostPad[i] = 0xFF;
                    }
            }
        }
        if (end < float(g.len)) break;
        // wrap: the loop starts again; held ghosts carry over
        end -= float(g.len);
        ph = 0.0f;
        r.gidx = 0;
        r.gWrapped = 1;
        if (end <= 0.0f) break;
    }
    r.gphase = end >= float(g.len) ? 0.0f : end;
    r.gestPos = r.gphase / float(g.len);
}

// One block into a live buffer: gain, the channel's own feedback, Beads' media.
void Engine::writeBuffer(int c, const int16_t* x) {
    ChRt& r = rt[c];
    const ChParams& p = p_[c];
    const int32_t gin = int32_t(p.gain * 4096.0f);   // Q12
    const bool fb = r.fbOn && (p.engine != kQuanta) && p.k[K::Feedback] > 0.001f;
    int16_t* d = r.buf + r.w;   // kBufLen is a multiple of kBlock: a block never wraps
    int32_t pk = 0;
    if (!fb && gin == 4096) {
        std::memcpy(d, x, kBlock * sizeof(int16_t));
    } else if (!fb) {
        for (uint32_t i = 0; i < kBlock; ++i) d[i] = int16_t(sat16((int32_t(x[i]) * gin) >> 12));
    } else {
        const int32_t* fp = r.fbPrev;
        for (uint32_t i = 0; i < kBlock; ++i) d[i] = int16_t(sat16(((int32_t(x[i]) * gin) >> 12) + fp[i]));
    }
    // Beads' media: the recording format of the buffer
    if (p.engine == kBeads) {
        const int m = enumIndex(p.k[K::Mode], kMedias);
        if (m == kMediaLofi) {
            for (uint32_t i = 0; i < kBlock; ++i) d[i] = int16_t(d[i] & ~15);   // 12-bit
        } else if (m == kMediaDirtyTape) {
            for (uint32_t i = 0; i < kBlock; ++i) d[i] = sMuLaw[uint16_t(d[i] + 32768) >> 4];   // Beads' mu-law, tabulated
        } else if (m == kMediaCleanTape) {
            for (uint32_t i = 0; i < kBlock; ++i) {   // tape saturation: a soft knee above -6 dBFS
                const int32_t v = d[i];
                const int32_t a = v < 0 ? -v : v;
                if (a > 16384) d[i] = int16_t(v < 0 ? -(16384 + ((a - 16384) >> 1)) : 16384 + ((a - 16384) >> 1));
            }
        }
    }
    for (uint32_t i = 0; i < kBlock; i += 8) {
        const int32_t v = d[i] < 0 ? -d[i] : d[i];
        pk = v > pk ? v : pk;
    }
    const float lv = float(pk) * (1.0f / 32768.0f);
    r.bufLevel = lv > r.bufLevel ? lv : r.bufLevel * 0.97f;
    r.w = (r.w + kBlock) & kBufMask;
}

// The channel's modulation sources for this block.
void Engine::tickSources(int c) {
    ChRt& r = rt[c];
    const ChParams& p = p_[c];
    const float inc = p.lfoRate * float(kBlock) * dsp::kInvSr;
    float ph = r.lfoPh + inc;
    const bool wrapped = ph >= 1.0f;
    ph -= floorf(ph);
    r.lfoPh = ph;
    float v;
    switch (p.lfoShape) {
        case kLfoTri: v = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
        case kLfoSaw: v = 2.0f * ph - 1.0f; break;
        case kLfoSquare: v = ph < 0.5f ? 1.0f : -1.0f; break;
        case kLfoSh:
            if (wrapped) r.lfoHeld = rng.bi();
            v = r.lfoHeld;
            break;
        case kLfoDrift:
            if (wrapped) r.lfoHeld = rng.bi();
            r.lfoDrift += (r.lfoHeld - r.lfoDrift) * (inc * 4.0f > 1.0f ? 1.0f : inc * 4.0f);
            v = r.lfoDrift;
            break;
        default: v = sineAt(uint32_t(ph * 4294967295.0f)); break;
    }
    r.lfo = v;
    const float d = float(r.active) / float(cloudBudget > 0 ? cloudBudget : 1);
    r.density += ((d > 1.0f ? 1.0f : d) - r.density) * 0.2f;
}

// ---- the block --------------------------------------------------------------------
void Engine::render(const lab::Transport& t, const lab::AudioIn& in, float* outL, float* outR, lab::Context* ctx) {
    static float cl[kCh][kBlock], cr[kCh][kBlock];
    static float sendD[kBlock], sendR[kBlock], wetL[kBlock], wetR[kBlock];
    static float stemL[kStemCount][kBlock], stemR[kStemCount][kBlock];
    static int16_t inS16[kBlock], mixS16[kBlock], genS16[kBlock];
    PROF_BEGIN();
    if (t.startedThisBlock) resetGestures();
    spawnsLeft_ = kSpawnsPerBlock;
    playing = t.playing ? 1 : 0;
    beat = t.beat;
    // the clock the slicer / freezer follow: the transport, or a free one at its tempo
    const float bpm = t.bpm > 20.0f ? t.bpm : 120.0f;
    const float bps = bpm / 60.0f * dsp::kInvSr;
    if (t.playing) freeBeat_ = t.beat;
    clock_.beat = freeBeat_;
    clock_.beatsPerSample = bps;
    clock_.bpm = bpm;
    freeBeat_ += double(bps) * double(kBlock);

    if (in.frames) {
        for (uint32_t i = 0; i < kBlock; ++i)
            inS16[i] = int16_t((int32_t(in.frames[2 * i]) + int32_t(in.frames[2 * i + 1])) >> 1);
    } else {
        std::memset(inS16, 0, sizeof inS16);
    }
    std::memset(cl, 0, sizeof cl);
    std::memset(cr, 0, sizeof cr);

    // The CPU guard (config based, so it does not flicker with activity): every
    // channel's mixing, bed, drive / filter / media count; then the FX modes in
    // channel order and the generators, while they fit under kChainCap.
    float cfg = 0.0f;
    float base[kCh];
    for (int c = 0; c < kCh; ++c) {
        const ChParams& p = p_[c];
        const bool beadsTape = p.engine == kBeads && enumIndex(p.k[K::Mode], kMedias) == kMediaDirtyTape;
        base[c] = baseUnits(p, beadsTape) + kMixUnits + (st_[c].freeze && rt[c].frzReady && p.bed > 0.01f ? kBedUnits : 0.0f);
        cfg += base[c];
    }
    uint8_t bypass = 0, hold = 0;
    for (int c = 0; c < kCh; ++c) {
        const float u = fxUnits(p_[c].fxMode);
        if (u <= 0.0f) continue;
        if (cfg + u <= kChainCap) cfg += u;
        else bypass = uint8_t(bypass | (1u << c));
    }
    for (int c = 0; c < kCh; ++c) {
        const ChParams& p = p_[c];
        if (p.source != kSrcGen || st_[c].hold) continue;
        const float u = genUnits(p.genType);
        if (cfg + u <= kChainCap) cfg += u;
        else hold = uint8_t(hold | (1u << c));
    }
    cpuBypass = bypass;
    cpuHold = hold;
    // the grains get what the configuration leaves (whether the channels sound
    // yet or not: a grain spawned now still runs when a silent chain wakes up)
    const float units = cfg;
    unitsUsed = units;
    int lim = int(kUnits - units);
    if (lim > grainBudget) lim = grainBudget;
    if (lim < kMinGrains) lim = kMinGrains;
    if (lim > kMaxGrains) lim = kMaxGrains;
    grainLimit = lim;
    // The configuration got heavier than the running grains allow (an FX mode,
    // a generator switched on): the most advanced grains fade out within this
    // block, and what was not running last block waits one more block, so no
    // block pays for both.
    int alive = 0;
    uint8_t baseOff = 0;
    for (int i = 0; i < nActive_; ++i) alive += pool_[i].reserved ? 0 : 1;
    if (alive > lim) {
        for (int excess = alive - lim; excess > 0; --excess) {
            int best = -1;
            float bw = -1.0f;
            for (int i = 0; i < nActive_; ++i) {
                const Grain& g = pool_[i];
                if (g.reserved) continue;
                const float prog = g.delay > 0 ? 2.0f : g.wph * (1.0f / float(kWin));
                if (prog > bw) {
                    bw = prog;
                    best = i;
                }
            }
            if (best < 0) break;
            Grain& g = pool_[best];
            if (g.delay > 0) {   // not started yet: gone
                pool_[best] = pool_[--nActive_];
                continue;
            }
            g.reserved = 1;
            if (g.left > kBlock) g.left = kBlock;
            const float rest = kWinEnd - g.wph;
            g.winc = rest > 0.0f ? rest / float(g.left) : 0.0f;
        }
        bypass = uint8_t(bypass | (fxWanted_ & ~fxRan_));
        hold = uint8_t(hold | (genWanted_ & ~genRan_));
        // drive / filter / media that got heavier wait too
        for (int c = 0; c < kCh; ++c) {
            const ChParams& p = p_[c];
            const bool beadsTape = p.engine == kBeads && enumIndex(p.k[K::Mode], kMedias) == kMediaDirtyTape;
            const float now = baseUnits(p, beadsTape) + ((bypass >> c) & 1 ? 0.0f : fxUnits(p.fxMode));
            if (now > chainRan_[c] + 0.01f) {
                baseOff = uint8_t(baseOff | (1u << c));
                bypass = uint8_t(bypass | (1u << c));
            }
        }
        cpuBypass = bypass;
        cpuHold = hold;
    }
    for (int c = 0; c < kCh; ++c) {
        const ChParams& p = p_[c];
        const bool beadsTape = p.engine == kBeads && enumIndex(p.k[K::Mode], kMedias) == kMediaDirtyTape;
        chainRan_[c] = (baseOff >> c) & 1 ? 0.0f : baseUnits(p, beadsTape) + ((bypass >> c) & 1 ? 0.0f : fxUnits(p.fxMode));
    }
    {
        uint8_t fxW = 0, genW = 0;
        for (int c = 0; c < kCh; ++c) {
            if (p_[c].fxMode != kFxOff) fxW = uint8_t(fxW | (1u << c));
            if (p_[c].source == kSrcGen && !st_[c].hold) genW = uint8_t(genW | (1u << c));
        }
        fxWanted_ = fxW;
        genWanted_ = genW;
        fxRan_ = uint8_t(fxW & ~bypass);
        genRan_ = uint8_t(genW & ~hold);
    }

    // clouds take turns at the head of the spawn queue (the per-block spawn budget is shared)
    spawnRot_ = spawnRot_ + 1 >= kCh ? 0 : spawnRot_ + 1;
    for (int ci = 0; ci < kCh; ++ci) {
        const int c = (ci + spawnRot_) % kCh;
        ChRt& r = rt[c];
        const ChParams& p = p_[c];
        const ChState& s = st_[c];
        if (!r.busy && p.source != kSrcSample && !s.hold && !((hold >> c) & 1)) {
            PROF_MARK(2);
            if (p.source == kSrcInput) writeBuffer(c, inS16);
            else if (p.source == kSrcOutput) writeBuffer(c, mixS16);
            else {
                gen[c].render(p.genType, p.genA, p.genB, genS16);
                PROF_MARK(1);
                writeBuffer(c, genS16);
            }
            PROF_MARK(0);
        } else {
            r.bufLevel *= 0.97f;
        }
        tickSources(c);
        if (r.busy) {
            for (int i = 0; i < kEmitters; ++i) r.em[i].acc = 0.0f;
            continue;
        }
        // drift (QUANTA): a slow random walk of the base frequency
        const float dr = p.engine == kQuanta ? p.k[K::Feedback] : 0.0f;
        r.drift += (rng.uni() - 0.5f) * dr * 0.02f - r.drift * 0.002f;
        playGesture(c);
        r.tapeHead = -1.0f;
        for (int i = 0; i < kEmitters; ++i)
            if (r.em[i].on) runEmitter(c, r.em[i], true);
        if (s.drone) {
            Emitter& d = r.drone;
            if (d.pad != 0xFF || d.vel == 0) {
                d.pad = 0xFF;
                d.vel = 90;
                d.semis = 0.0f;
                d.fresh = 1;
            }
            runEmitter(c, d, false);
        } else {
            r.drone.vel = 0;
        }
    }

    PROF_MARK(2);
    // grains
    int n = nActive_;
    uint32_t touched = 0;
    for (int c = 0; c < kCh; ++c) rt[c].active = 0;
    for (int i = 0; i < n;) {
        Grain& g = pool_[i];
        if (rt[g.cloud].busy) {   // the main loop is replacing this channel's buffer
            pool_[i] = pool_[--n];
            continue;
        }
        const uint32_t d = g.delay < kBlock ? g.delay : kBlock - 1;
        uint32_t cnt = kBlock - d;
        if (cnt > g.left) cnt = g.left;
        float* L = cl[g.cloud] + d;
        float* R = cr[g.cloud] + d;
        touched |= 1u << g.cloud;
        switch (g.kind) {
            case kGBuf: runBuf(g, L, R, cnt); break;
            case kGSine: runSine(g, L, R, cnt); break;
            case kGPulsar: runPulsar(g, L, R, cnt); break;
            default: runTrain(g, L, R, cnt); break;
        }
        g.delay = 0;
        g.left -= cnt;
        if (g.left == 0) {
            pool_[i] = pool_[--n];
        } else {
            ++rt[g.cloud].active;
            ++i;
        }
    }
    nActive_ = n;
    totalActive = n;
    PROF_MARK(3);

    // per channel: freeze bed, insert chain, fader / pan, sends, stems, feedback, meters
    std::memset(sendD, 0, sizeof sendD);
    std::memset(sendR, 0, sizeof sendR);
    const bool stems = ctx != nullptr && ctx->shell != nullptr && ctx->shell->recording();
    if (stems) {
        std::memset(stemL, 0, sizeof stemL);
        std::memset(stemR, 0, sizeof stemR);
    }
    const uint8_t solo = soloMask;
    for (int c = 0; c < kCh; ++c) {
        ChRt& r = rt[c];
        const ChParams& p = p_[c];
        const ChState& s = st_[c];
        float* L = cl[c];
        float* R = cr[c];
        const bool bed = s.freeze && r.frzReady && p.bed > 0.01f && !r.busy;
        const bool on = bed || (touched & (1u << c));
        if (on) r.quiet = chain[c].tail() ? 520 : 1;
        else if (r.quiet > 0) --r.quiet;
        if (!on && r.quiet <= 0) {
            r.fbOn = 0;
            r.peak *= 0.93f;
            continue;
        }
        if (bed) {
            const float k = p.bed * p.bed * 0.5f * (1.0f / 32768.0f);
            const uint32_t q = r.bedPos;
            const int16_t* fa = r.frz + q;
            const int16_t* fb = r.frz + ((q + (kFrzLen / 2)) & kFrzMask);
            for (uint32_t i = 0; i < kBlock; ++i) {
                L[i] += float(fa[i]) * k;
                R[i] += float(fb[i]) * k;
            }
            r.bedPos = (q + kBlock) & kFrzMask;
        }
        const int med = p.engine == kBeads ? enumIndex(p.k[K::Mode], kMedias) : kMediaHifi;
        PROF_MARK(5);
        if (!((baseOff >> c) & 1)) chain[c].process(L, R, p, med, clock_, !((bypass >> c) & 1));
        PROF_MARK(4);
        const bool audible = !s.mute && (solo == 0 || (solo >> c) & 1);
        const float lvl = audible ? p.level * p.level * 1.6f : 0.0f;
        if (p.pan != r.panV) {
            r.panV = p.pan;
            const float pn = dsp::clamp(0.5f + 0.5f * p.pan, 0.0f, 1.0f);
            r.panL = sqrtf(1.0f - pn) * 1.41f;
            r.panR = sqrtf(pn) * 1.41f;
        }
        const float pl = lvl * r.panL, prr = lvl * r.panR;
        const float kd = 0.5f * p.dly * p.dly * lvl, kr = 0.5f * p.rev * p.rev * lvl;
        const bool fbOn = p.engine != kQuanta && p.k[K::Feedback] > 0.001f;
        r.fbOn = fbOn ? 1 : 0;
        const float fbK = 0.5f * p.k[K::Feedback] * 0.95f * 32767.0f;
        float pk = 0.0f;
        if (fbOn) {
            int32_t* fbp = r.fbPrev;
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float l = L[i], rr = R[i];
                const float m = l + rr;
                outL[i] += l * pl;
                outR[i] += rr * prr;
                sendD[i] += m * kd;
                sendR[i] += m * kr;
                fbp[i] = int32_t(m * fbK);
            }
        } else {
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float l = L[i], rr = R[i];
                const float m = l + rr;
                outL[i] += l * pl;
                outR[i] += rr * prr;
                sendD[i] += m * kd;
                sendR[i] += m * kr;
            }
        }
        for (uint32_t i = 0; i < kBlock; i += 8) {
            const float a = fabsf(L[i]) + fabsf(R[i]);
            pk = a > pk ? a : pk;
        }
        if (stems && lvl > 0.0f) {
            float* sl = stemL[s.stem & 3];
            float* sr = stemR[s.stem & 3];
            for (uint32_t i = 0; i < kBlock; ++i) {
                sl[i] += L[i] * pl;
                sr[i] += R[i] * prr;
            }
        }
        pk *= 0.5f * (pl > prr ? pl : prr);
        r.peak = pk > r.peak ? pk : r.peak * 0.93f;
    }

    PROF_MARK(5);
    // send effects
    const FxState& f = *fx_;
    if (dlyTone_ != f.dlyTone) {
        dlyTone_ = f.dlyTone;
        dly_.setFilter(400.0f * fexp(3.9f * f.dlyTone));
    }
    const float beats = dlyDivBeats(f.dlyDiv);
    dly_.setTimeSamples(uint32_t(beats * 60.0f / bpm * dsp::kSr));
    dly_.setFeedbackK(uint8_t(f.dlyFb * 0.92f * 255.0f));
    PROF_MARK(6);
    if (dly_.process(sendD, wetL, wetR)) {
        const float k = f.dlyRet;
        for (uint32_t i = 0; i < kBlock; ++i) {
            outL[i] += wetL[i] * k;
            outR[i] += wetR[i] * k;
            sendR[i] += 0.2f * (wetL[i] + wetR[i]) * k;   // echoes into the reverb
        }
    }
    if (revSize_ != f.revSize || revDamp_ != f.revDamp) {
        revSize_ = f.revSize;
        revDamp_ = f.revDamp;
        rev_->setSize(0.5f + 0.48f * f.revSize);
        rev_->setDamping(0.2f + 0.7f * f.revDamp);
    }
    PROF_MARK(8);
    if (rev_->process(sendR, wetL, wetR)) {
        const float k = f.revRet * 1.6f;
        for (uint32_t i = 0; i < kBlock; ++i) {
            outL[i] += wetL[i] * k;
            outR[i] += wetR[i] * k;
        }
    }

    PROF_MARK(9);
    // master: a soft knee above 0.75 (identity below it, C1-continuous, flat at 1.0
    // from 1.25), meter, the history for OUTPUT sources and freezes
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float a = fabsf(outL[i]) > fabsf(outR[i]) ? fabsf(outL[i]) : fabsf(outR[i]);
        pk = a > pk ? a : pk;
    }
    if (pk > 0.75f) {
        auto knee = [](float x) {
            const float a = fabsf(x);
            if (a <= 0.75f) return x;
            const float u = a - 0.75f;
            const float y = u >= 0.5f ? 1.0f : 0.75f + u - u * u;
            return x < 0.0f ? -y : y;
        };
        for (uint32_t i = 0; i < kBlock; ++i) {
            outL[i] = knee(outL[i]);
            outR[i] = knee(outR[i]);
        }
    }
    const uint32_t mw = mixW;
    int16_t* mh = mixHist + mw;   // kMixHist is a multiple of kBlock
    for (uint32_t i = 0; i < kBlock; ++i) {
        const int16_t v = int16_t(sat16(int32_t((outL[i] + outR[i]) * 16383.0f)));
        mixS16[i] = v;
        mh[i] = v;
    }
    mixW = (mw + kBlock) & (kMixHist - 1);
    const float pm = pk > 1.0f ? 1.0f : pk;
    meterL = pm > meterL ? pm : meterL * 0.93f;
    meterR = meterL;
    masterLevel = meterL;

    // resample capture (mono)
    if (capOn) {
        const int src = capSrc;
        uint32_t len = capLen;
        for (uint32_t i = 0; i < kBlock && len < kBufLen; ++i, ++len)
            capBuf[len] = src < 0 ? mixS16[i] : f2s16(0.5f * (cl[src][i] + cr[src][i]) * 32767.0f);
        capLen = len;
        if (len >= kBufLen) capOn = 0;
    }
    if (stems)
        for (int sIdx = 0; sIdx < kStemCount; ++sIdx) ctx->stem(sIdx, stemL[sIdx], stemR[sIdx]);
    sampleClock = sampleClock + kBlock;
    PROF_MARK(7);
    PROF_END();
}

}  // namespace bulut
