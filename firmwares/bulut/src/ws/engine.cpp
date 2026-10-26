// BULUT audio engine (engine.h).
#include "ws/engine.h"

#include <cmath>
#include <cstring>

#include "fastrun.h"


namespace bulut {

using lab::kBlock;
namespace dsp = lab::dsp;

namespace {

// Window tables: 0..15 texture morph, 16 Gabor (gaussian), 17 percussive.
float gWin[kWindows][kWin + 1];
float gSine[1025];
bool gTables = false;

void buildTables() {
    if (gTables) return;
    gTables = true;
    for (int i = 0; i <= 1024; ++i) gSine[i] = sinf(dsp::kTwoPi * float(i) / 1024.0f);
    for (int t = 0; t < kWindows; ++t) {
        for (int i = 0; i <= kWin; ++i) {
            const float x = float(i) / float(kWin);   // 0..1
            const float hann = 0.5f - 0.5f * cosf(dsp::kTwoPi * x);
            float w;
            if (t < 6) {
                // Tukey: flat top, fades from 5 % (t = 0) to 50 % (t = 5, = Hann)
                const float f = 0.05f + 0.09f * float(t);
                if (x < f) w = 0.5f - 0.5f * cosf(dsp::kPi * x / f);
                else if (x > 1.0f - f) w = 0.5f - 0.5f * cosf(dsp::kPi * (1.0f - x) / f);
                else w = 1.0f;
            } else if (t < kTexWindows) {
                // Hann -> narrowing gaussians under a Hann (t = 6..15)
                const float sigma = 0.30f - 0.022f * float(t - 6);
                const float d = (x - 0.5f) / sigma;
                w = expf(-0.5f * d * d) * hann;
            } else if (t == kWinGabor) {
                const float d = (x - 0.5f) / 0.16f;
                w = expf(-0.5f * d * d) * hann;
            } else {
                // percussive: 3 % attack, exponential decay
                w = x < 0.03f ? x / 0.03f : expf(-5.0f * (x - 0.03f)) * (1.0f - x) * 1.03f;
            }
            gWin[t][i] = w;
        }
        gWin[t][0] = 0.0f;
        gWin[t][kWin] = 0.0f;
    }
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
// float (already scaled to int16 range) -> int16 with saturation; the float -> int
// conversion itself saturates on the M4, so only the 16-bit clamp is needed.
inline int16_t f2s16(float v) {
    const float c = v > 2.0e9f ? 2.0e9f : (v < -2.0e9f ? -2.0e9f : v);
    return int16_t(sat16(int32_t(c)));
}
inline int16_t toS16(float v) { return f2s16(v * 32767.0f); }

// Gaussian-ish random (sum of 3 uniforms), mean 0, sd ~0.5.
inline float gauss(dsp::Rng& r) { return (r.uni() + r.uni() + r.uni() - 1.5f); }

// ---- grain renderers (inner loops) ------------------------------------------
// The window is evaluated once per 16-sample segment and ramped linearly in
// between (the tables have 512 points, a grain is >= 22 samples). Buffer reads
// use integer interpolation and skip the ring mask when the segment does not
// wrap. Synthetic particles are quadrature oscillators (one complex rotation
// per sample), renormalised once per segment.
constexpr uint32_t kSeg = 16;
constexpr float kWinEnd = float(kWin) - 0.001f;

inline float winEnd(const float* w, float ph) { return winAt(w, ph < kWinEnd ? ph : kWinEnd); }

// Next segment: window value at its start (w0, carried) and its slope.
inline float segSlope(const float* w, float& wph, float winc, uint32_t m, float w0) {
    wph += winc * float(m);
    return (winEnd(w, wph) - w0) * (m == kSeg ? 1.0f / float(kSeg) : 1.0f / float(m));
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
    const float* w = g.win;
    float wph = g.wph;
    const float winc = g.winc, gl = g.gl, gr = g.gr;
    float wv = winEnd(w, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(w, wph, winc, m, wv);
        float* __restrict l = L + i0;
        float* __restrict r = R + i0;
        pos = (((pos >> 12) & mask) << 12) | (pos & 4095u);
        const int32_t span = (inc * int32_t(m)) >> 12;
        const int32_t i0x = int32_t(pos >> 12);
        if (i0x + span + 2 <= int32_t(mask) && i0x + span >= 0) {
            // contiguous: no masking
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

// sine particle (Gabor) and glisson (frequency glide per segment)
OT_FASTRUN_FN void runSine(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    float c = g.c, s = g.s, cr = g.cr, sr = g.sr;
    const float* w = g.win;
    float wph = g.wph;
    const float winc = g.winc, gl = g.gl, gr = g.gr;
    float wv = winEnd(w, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(w, wph, winc, m, wv);
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
    const uint32_t inc = g.inc32, duty = g.cnt;
    const float* w = g.win;
    float wph = g.wph;
    const float winc = g.winc, gl = g.gl, gr = g.gr;
    float wv = winEnd(w, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float dw = segSlope(w, wph, winc, m, wv);
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
// with its own exponential decay folded into the window ramp
OT_FASTRUN_FN void runTrain(Grain& g, float* __restrict L, float* __restrict R, uint32_t n) {
    float c = g.c, s = g.s, c2 = g.c2, s2 = g.s2;
    const float cr = g.cr, sr = g.sr, cr2 = g.cr2, sr2 = g.sr2;
    const float* w = g.win;
    float wph = g.wph;
    const float winc = g.winc, gl = g.gl, gr = g.gr, top = float(g.plen);
    float env = g.env;
    const float decay = g.decay;
    float wv = winEnd(w, wph);
    for (uint32_t i0 = 0; i0 < n; i0 += kSeg) {
        const uint32_t m = n - i0 < kSeg ? n - i0 : kSeg;
        const float w0 = wv;
        const float dwin = segSlope(w, wph, winc, m, wv);
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

float windowValue(int table, float x) {
    buildTables();
    if (table < 0) table = 0;
    if (table >= kWindows) table = kWindows - 1;
    return winAt(gWin[table], dsp::clamp(x, 0.0f, 0.9999f) * float(kWin));
}

const float* Engine::windowTable(int i) const { return gWin[i < 0 ? 0 : (i >= kWindows ? kWindows - 1 : i)]; }

// Semitone offset snapped to a scale (FREE = unchanged).
float scaleSnap(float semis, int scale) {
    static const int8_t kMaj[7] = {0, 2, 4, 5, 7, 9, 11}, kMin[7] = {0, 2, 3, 5, 7, 8, 10}, kPen[5] = {0, 2, 4, 7, 9};
    if (scale <= kScaleFree || scale >= kScales) return semis;
    const float oct = floorf(semis / 12.0f);
    const float in = semis - oct * 12.0f;   // 0..12
    float best = 0.0f, bd = 99.0f;
    auto consider = [&](float c) {
        const float d = fabsf(in - c);
        if (d < bd) { bd = d; best = c; }
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
        case kScaleOct: return float(12 * (col / 3) - 12);   // 3 columns per octave, -1..+2 octaves
        case kScaleFifth: return float(12 * (col / 2) + 7 * (col & 1) - 12);          // root and fifth, from -1 octave
        case kScaleMajor: return float(12 * (col / 7) + kMaj[col % 7]);
        case kScaleMinor: return float(12 * (col / 7) + kMin[col % 7]);
        case kScalePent: return float(12 * (col / 5) + kPen[col % 5]);
        default: return float(col);
    }
}

// ---- reverb / delay settings ------------------------------------------------
const char* dlyDivName(int i) {
    static const char* const k[kDlyDivs] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
    return k[i < 0 ? 0 : (i >= kDlyDivs ? kDlyDivs - 1 : i)];
}
float dlyDivBeats(int i) {
    static const float k[kDlyDivs] = {0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    return k[i < 0 ? 0 : (i >= kDlyDivs ? kDlyDivs - 1 : i)];
}
void defaultFx(FxState& f) {
    f.dlyDiv = enumValue(5, kDlyDivs);
    f.dlyFb = 0.45f;
    f.dlyTone = 0.55f;
    f.dlyRet = 0.7f;
    f.revSize = 0.75f;
    f.revDamp = 0.45f;
    f.revRet = 0.8f;
    f.reserved = 0.0f;
}

void defaultCloud(CloudState& c, int index) {
    std::memset(&c, 0, sizeof c);
    float* g = c.p[0];
    g[P::Pos] = 0.5f;
    g[P::Size] = 0.55f;
    g[P::Dens] = 0.55f;
    g[P::Pitch] = 0.5f;
    g[P::Spread] = 0.6f;
    g[P::Texture] = 0.45f;
    g[P::Feedback] = 0.0f;
    g[P::Reverb] = 0.35f;
    g[P::Engine] = 0.0f;
    g[P::Jitter] = 0.15f;
    g[P::PRand] = 0.0f;
    g[P::Reverse] = 0.0f;
    g[P::Level] = 0.7f;
    g[P::Delay] = 0.1f;
    g[P::Scale] = 0.0f;
    g[P::Tone] = 0.5f;
    float* q = c.p[1];
    q[P::QType] = enumValue(index % 2 ? kQGlisson : kQGabor, kQTypes);
    q[P::QRate] = 0.45f;
    q[P::QDur] = 0.45f;
    q[P::QFreq] = 0.45f;
    q[P::QBand] = 0.25f;
    q[P::QLaw] = enumValue(kLawPoisson, kLaws);
    q[P::QChar] = 0.5f;
    q[P::Reverb] = g[P::Reverb];
    q[P::Engine] = 0.0f;
    q[P::QPos] = 0.5f;
    q[P::QSpread] = 0.7f;
    q[P::Level] = g[P::Level];
    q[P::Delay] = g[P::Delay];
    q[P::QDrift] = 0.2f;
    q[P::Scale] = g[P::Scale];
    q[P::Tone] = 0.5f;
    c.pan = 0.5f;
    c.bed = 0.5f;
    c.freezeSlot = 0xFF;
    c.stem = uint8_t(index & 3);
    c.gestOn = 1;
}

// ---- init ---------------------------------------------------------------------
bool Engine::init(lab::Arena& arena, CloudState* states, CloudSource* sources, Gesture* gestures, FxState* fx) {
    buildTables();
    st_ = states;
    src_ = sources;
    gest_ = gestures;
    fx_ = fx;
    for (int c = 0; c < kClouds; ++c) {
        rt[c].buf = static_cast<int16_t*>(arena.alloc(kBufLen * 2));
        rt[c].frz = static_cast<int16_t*>(arena.alloc(kFrzLen * 2));
        if (!rt[c].buf || !rt[c].frz) return false;
        rt[c].logistic = 0.31f + 0.07f * float(c);
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
    CloudRt& r = rt[c];
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
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e.on = 1;
}
void Engine::padUp(int c, int pad) {
    for (int i = 0; i < kLiveEmitters; ++i)
        if (rt[c].em[i].pad == pad) rt[c].em[i].on = 0;
}
void Engine::noteOn(int c, int note, int vel) {
    CloudRt& r = rt[c];
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
    for (int c = 0; c < kClouds; ++c) {
        rt[c].gphase = 0.0f;
        rt[c].gidx = 0;
    }
}

// ---- grain allocation ------------------------------------------------------------

Grain* Engine::alloc(int c) {
    if (spawnsLeft <= 0 || nActive_ >= grainBudget || nActive_ >= kMaxGrains || rt[c].active >= cloudBudget) {
        ++rt[c].refused;
        refusedTotal = refusedTotal + 1;
        return nullptr;
    }
    --spawnsLeft;
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

// Inter-onset interval in samples for a mean rate.
float Engine::nextInterval(int c, const CloudState& s, float hz) {
    const float mean = dsp::kSr / (hz < 0.1f ? 0.1f : hz);
    if (s.engine() == kGranular) return mean * (0.55f + 0.9f * rng.uni());
    const float u = rng.uni();
    switch (enumIndex(s.p[1][P::QLaw], kLaws)) {
        case kLawSync: return mean;
        case kLawPoisson: return -mean * 0.69314718f * flog2(u < 1e-4f ? 1e-4f : u);
        case kLawGauss: return mean * dsp::clamp(1.0f + 0.7f * gauss(rng), 0.1f, 3.0f);
        case kLawCauchy: {
            // tan(pi (u - 0.5)) from the sine table
            const uint32_t ph = uint32_t((dsp::clamp(u, 0.02f, 0.98f) - 0.5f) * 2147483648.0f);
            const float t = sineAt(ph) / sineAt(ph + 0x40000000u);
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

void Engine::spawnGranular(int c, const CloudState& s, const Emitter& e, int offset, bool live) {
    CloudRt& r = rt[c];
    const float* p = s.p[0];
    const bool frz = s.freeze && r.frzReady;
    const bool sample = !frz && src_[c].type == kSrcSample;
    if (sample && r.sampleLen < 64) return;
    Grain* g = alloc(c);
    if (!g) return;
    const float size = sizeSec(p[P::Size]);
    const uint32_t len = uint32_t(size * dsp::kSr) < 64 ? 64 : uint32_t(size * dsp::kSr);
    // pitch
    float semis = pitchSemi(p[P::Pitch]) + e.semis;
    const float pr = p[P::PRand];
    if (pr > 0.01f) semis += scaleSnap(gauss(rng) * 24.0f * pr, enumIndex(p[P::Scale], kScales));
    const float ratio = fexp2(semis * (1.0f / 12.0f));
    const bool rev = rng.uni() < p[P::Reverse];
    // position
    float pos = e.pad == 0xFF ? p[P::Pos] : padPos(e.pad);
    pos += gauss(rng) * p[P::Jitter] * 0.25f;
    pos = dsp::clamp(pos, 0.0f, 1.0f);
    uint32_t start;
    if (frz) {
        start = uint32_t(pos * float(kFrzLen - 1));
        g->src = r.frz;
        g->mask = kFrzMask;
    } else if (sample) {
        const uint32_t sl = r.sampleLen;
        start = uint32_t(pos * float(sl - 1));
        if (rev) start = start + len < sl ? start + uint32_t(len * ratio) : sl - 1;
        g->src = r.buf;
        g->mask = kBufMask;
    } else {
        // live ring: pos 1 = now, 0 = kLiveSpan ago; forward grains faster than
        // the write head start far enough back not to overtake it
        float back = (1.0f - pos) * float(kLiveSpan);
        const float need = float(len) * (ratio > 1.0f && !rev ? ratio - 1.0f : 0.0f) + 256.0f;
        const float needRev = rev ? float(len) * ratio + 256.0f : 0.0f;
        if (back < need) back = need;
        if (back < needRev) back = needRev;
        start = (r.w - uint32_t(back) - uint32_t(offset)) & kBufMask;
        g->src = r.buf;
        g->mask = kBufMask;
    }
    g->kind = kGBuf;
    g->pos = start << 12;
    const float incf = ratio * 4096.0f;
    g->inc = rev ? -int32_t(incf) : int32_t(incf);
    const int tex = enumIndex(p[P::Texture], kTexWindows);
    g->win = gWin[tex];
    g->wph = 0.0f;
    g->winc = float(kWin) / float(len);
    g->left = len;
    g->delay = uint32_t(offset);
    // amplitude: velocity, normalised by the expected overlap
    const float overlap = densHz(p[P::Dens]) * size;
    // overlap-normalised: dense clouds stay as loud as sparse ones
    float amp = (float(e.vel) / 127.0f) / sqrtf(overlap < 1.0f ? 1.0f : overlap);
    amp *= 1.5f;
    const float pan = 0.5f + (rng.uni() - 0.5f) * p[P::Spread];
    const float k = amp * (1.0f / 32768.0f);
    g->gl = k * sqrtf(1.0f - pan) * 1.41f;
    g->gr = k * sqrtf(pan) * 1.41f;
    g->live = live ? 1 : 0;
    spark(c, frz ? 1 : 0, pos, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
}

void Engine::spawnQuanta(int c, CloudState& s, const Emitter& e, int offset, bool live) {
    CloudRt& r = rt[c];
    const float* q = s.p[1];
    const int type = enumIndex(q[P::QType], kQTypes);
    const bool frz = s.freeze && r.frzReady;
    const float dur = durSec(q[P::QDur]) * (0.7f + 0.6f * rng.uni());
    const uint32_t len = uint32_t(dur * dsp::kSr) < 16 ? 16 : uint32_t(dur * dsp::kSr);
    const float pan = 0.5f + (rng.uni() - 0.5f) * q[P::QSpread];
    const float vel = float(e.vel) / 127.0f;
    float pos = e.pad == 0xFF ? q[P::QPos] : padPos(e.pad);
    if (type == kQSample) {
        const bool sample = !frz && src_[c].type == kSrcSample;
        if (sample && r.sampleLen < 64) return;
        Grain* g = alloc(c);
        if (!g) return;
        pos = dsp::clamp(pos + gauss(rng) * q[P::QBand] * 0.25f, 0.0f, 1.0f);
        float semis = (q[P::QChar] - 0.5f) * 48.0f + e.semis + gauss(rng) * 12.0f * bandOct(q[P::QBand]) * 0.5f;
        const float ratio = fexp2(semis * (1.0f / 12.0f));
        uint32_t start;
        if (frz) {
            start = uint32_t(pos * float(kFrzLen - 1));
            g->src = r.frz;
            g->mask = kFrzMask;
        } else if (sample) {
            start = uint32_t(pos * float(r.sampleLen - 1));
            g->src = r.buf;
            g->mask = kBufMask;
        } else {
            float back = (1.0f - pos) * float(kLiveSpan);
            const float need = float(len) * (ratio > 1.0f ? ratio : 1.0f) + 256.0f;
            if (back < need) back = need;
            start = (r.w - uint32_t(back) - uint32_t(offset)) & kBufMask;
            g->src = r.buf;
            g->mask = kBufMask;
        }
        g->kind = kGBuf;
        g->pos = start << 12;
        g->inc = int32_t(ratio * 4096.0f);
        g->win = gWin[kWinGabor];
        g->winc = float(kWin) / float(len);
        g->left = len;
        g->delay = uint32_t(offset);
        const float amp = vel * 1.1f;
        g->gl = amp * (1.0f / 32768.0f) * sqrtf(1.0f - pan) * 1.41f;
        g->gr = amp * (1.0f / 32768.0f) * sqrtf(pan) * 1.41f;
        g->live = live ? 1 : 0;
        spark(c, 2, pos, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
        return;
    }
    Grain* g = alloc(c);
    if (!g) return;
    // frequency: base * column step * gaussian band scatter * drift
    const int scale = enumIndex(s.p[0][P::Scale], kScales);
    float semis = e.semis;
    if (e.pad != 0xFF) semis += columnSemis(e.pad % 12, scale);
    semis += gauss(rng) * 12.0f * bandOct(q[P::QBand]);
    semis += r.drift * 12.0f;
    if (scale != kScaleFree && q[P::QBand] > 0.01f) semis = scaleSnap(semis, scale);
    float hz = freqHz(q[P::QFreq]) * fexp2(semis * (1.0f / 12.0f));
    hz = dsp::clamp(hz, 15.0f, 15000.0f);
    const float wrad = dsp::kTwoPi * hz * dsp::kInvSr;   // radians per sample
    auto rot = [](float rad, float& cr, float& sr) {
        const uint32_t ph = uint32_t(rad * (4294967296.0f / 6.2831853f));
        sr = sineAt(ph);
        cr = sineAt(ph + 0x40000000u);
    };
    g->winc = float(kWin) / float(len);
    g->left = len;
    g->delay = uint32_t(offset);
    g->glide = 1.0f;
    float amp = vel * 0.42f;
    const float ch = q[P::QChar];
    switch (type) {
        case kQGlisson:
        case kQGabor: {
            g->kind = kGSine;
            const uint32_t ph0 = rng.next();
            g->c = sineAt(ph0 + 0x40000000u);
            g->s = sineAt(ph0);
            rot(wrad, g->cr, g->sr);
            g->w = wrad;
            if (type == kQGlisson) {
                // -3 .. +3 octaves over the particle, applied per 16-sample segment
                const float glideOct = (ch - 0.5f) * 6.0f;
                const float segs = float(len) / 16.0f;
                g->glide = fexp2(glideOct / (segs < 1.0f ? 1.0f : segs));
                g->win = gWin[5];
            } else {
                g->win = gWin[6 + enumIndex(1.0f - ch, 10)];   // Char = wider .. narrower gaussian
            }
            break;
        }
        case kQPulsar: {
            g->kind = kGPulsar;
            const float ratio = 1.0f + ch * 7.0f;          // formant / fundamental
            g->inc32 = uint32_t(hz * (4294967296.0f / dsp::kSr));
            g->cnt = uint32_t(4294967295.0f / ratio);     // pulsaret = one formant cycle
            g->ph = 0;
            g->c = g->c2 = 1.0f;
            g->s = g->s2 = 0.0f;
            const float fr = dsp::clamp(wrad * ratio, 0.0f, 3.0f);
            rot(fr, g->cr, g->sr);
            rot(fr * 0.5f, g->cr2, g->sr2);
            g->win = gWin[2];
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
            // numerator sin(m y), denominator sin(y), y advancing at half the fundamental
            g->c = g->c2 = 1.0f;
            g->s = g->s2 = 0.0f;
            rot(wrad * 0.5f * float(m), g->cr, g->sr);
            rot(wrad * 0.5f, g->cr2, g->sr2);
            g->win = gWin[kWinPerc];
            amp *= 0.8f;
            break;
        }
    }
    g->gl = amp * sqrtf(1.0f - pan) * 1.41f;
    g->gr = amp * sqrtf(pan) * 1.41f;
    g->live = live ? 1 : 0;
    // display: from a pad, under its column (scattered by the band); else by log frequency
    const float x = e.pad != 0xFF ? padPos(e.pad) + (semis - columnSemis(e.pad % 12, scale)) * 0.006f
                                  : flog2(hz / 30.0f) * (0.69314718f / 8.1f);
    spark(c, uint8_t(3 + type), x, semis, amp, len, live, e.pad == 0xFF ? -1 : e.pad);
}

void Engine::spawn(int c, Emitter& e, int offset, bool live) {
    CloudState& s = st_[c];
    if (s.engine() == kGranular) spawnGranular(c, s, e, offset, live);
    else spawnQuanta(c, s, e, offset, live);
}

// Advance one emitter over the block and spawn its grains (at most 12 per block).
void Engine::runEmitter(int c, Emitter& e, bool live) {
    if (e.acc >= float(kBlock)) {   // no grain due in this block
        e.acc -= float(kBlock);
        return;
    }
    CloudState& s = st_[c];
    const bool gran = s.engine() == kGranular;
    float dv = gran ? s.p[0][P::Dens] : s.p[1][P::QRate];
    if (e.pad != 0xFF) dv += padDensOff(e.pad);
    dv = dsp::clamp(dv, 0.0f, 1.0f);
    const float hz = gran ? densHz(dv) : rateHz(dv);
    float acc = e.acc;
    int n = 0;
    while (acc < float(kBlock) && n < 8) {
        const int off = acc < 0.0f ? 0 : int(acc);
        // a refused grain costs nothing but the interval (the budget check comes first)
        if (spawnsLeft > 0 && nActive_ < grainBudget && rt[c].active < cloudBudget) spawn(c, e, off, live);
        else {
            ++rt[c].refused;
            refusedTotal = refusedTotal + 1;
        }
        acc += n < 2 ? nextInterval(c, s, hz) : dsp::kSr / hz;
        ++n;
    }
    if (acc < float(kBlock)) acc = float(kBlock);
    e.acc = acc - float(kBlock);
}

// Gesture playback for one block: events in [phase, phase + block * speed).
void Engine::playGesture(int c) {
    CloudRt& r = rt[c];
    if (r.pendG != 0xFF) {
        releaseGhosts(c);
        r.liveG = r.pendG;
        r.pendG = 0xFF;
        r.gphase = 0.0f;
        r.gidx = 0;
    }
    CloudState& s = st_[c];
    const Gesture& g = gest_[c * kGestBufs + r.liveG];
    if (!s.gestOn || g.len == 0 || g.count == 0) {
        if (r.em[kLiveEmitters].on | r.em[kLiveEmitters + 1].on | r.em[kLiveEmitters + 2].on | r.em[kLiveEmitters + 3].on)
            releaseGhosts(c);
        return;
    }
    const float step = float(kBlock) * speedMul(g.speed);
    float ph = r.gphase;
    float end = ph + step;
    for (int guard = 0; guard < 2; ++guard) {
        const float lim = end < float(g.len) ? end : float(g.len);
        while (r.gidx < g.count && float(g.ev[r.gidx].t) < lim) {
            const GestEvent& ev = g.ev[r.gidx++];
            if (ev.type == kGParam) {
                const int slot = ev.a & 15, eng = ev.a >> 4;
                s.set(eng, slot, float(ev.v) * (1.0f / 65535.0f));
            } else if (ev.type == kGPadDown) {
                int k = -1;
                for (int i = 0; i < 4; ++i)
                    if (r.ghostPad[i] == ev.a || !r.em[kLiveEmitters + i].on) { k = i; break; }
                if (k < 0) k = 0;
                Emitter& e = r.em[kLiveEmitters + k];
                r.ghostPad[k] = ev.a;
                e.pad = ev.a;
                e.vel = uint8_t(ev.v ? ev.v : 100);
                e.semis = 0.0f;
                e.acc = float(g.ev[r.gidx - 1].t) - ph;
                if (e.acc < 0.0f) e.acc = 0.0f;
                e.acc /= speedMul(g.speed);
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
}

// ---- the per-cloud mix: freeze bed, tone filter, feedback copy, fader / pan, sends ------
// One pass per cloud. Output and sends accumulate (both arrive zeroed). The
// filtered cloud is written back to L/R only when a stem or a cloud capture needs it.
namespace {
struct MixArgs {
    float* L;
    float* R;
    float* outL;
    float* outR;
    float* sendD;
    float* sendR;
    int32_t* fb;            // feedback copy (pre fader, int16 units, gain applied) or null
    float fbK;
    const int16_t* bedA;    // freeze bed taps (kBed)
    const int16_t* bedB;
    float bedK;
    float pl, pr, kd, kr, ta, zl, zr;
};

template <int kMode, bool kBed, bool kStore>
void mixLoop(MixArgs& a) {
    float* __restrict L = a.L;
    float* __restrict R = a.R;
    float* __restrict oL = a.outL;
    float* __restrict oR = a.outR;
    float* __restrict sD = a.sendD;
    float* __restrict sR = a.sendR;
    int32_t* __restrict fb = a.fb;
    const float fbK = a.fbK;
    const int16_t* __restrict ba = a.bedA;
    const int16_t* __restrict bb = a.bedB;
    const float pl = a.pl, pr = a.pr, kd = a.kd, kr = a.kr, ta = a.ta, bk = a.bedK;
    float zl = a.zl, zr = a.zr;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float l = L[i], r = R[i];
        if (kBed) {
            l += float(ba[i]) * bk;
            r += float(bb[i]) * bk;
        }
        if (kMode == 1) {
            zl += ta * (l - zl);
            zr += ta * (r - zr);
            l = zl;
            r = zr;
        } else if (kMode == 2) {
            zl += ta * (l - zl);
            zr += ta * (r - zr);
            l -= zl;
            r -= zr;
        }
        if (kStore) {
            L[i] = l;
            R[i] = r;
        }
        const float m = l + r;
        if (fb) fb[i] = int32_t(m * fbK);
        oL[i] += l * pl;
        oR[i] += r * pr;
        sD[i] += m * kd;
        sR[i] += m * kr;
    }
    a.zl = zl;
    a.zr = zr;
}

template <int kMode>
void mixMode(MixArgs& a, bool bed, bool store) {
    if (bed) store ? mixLoop<kMode, true, true>(a) : mixLoop<kMode, true, false>(a);
    else store ? mixLoop<kMode, false, true>(a) : mixLoop<kMode, false, false>(a);
}

void mixCloud(MixArgs& a, int mode, bool bed, bool store) {
    if (mode == 1) mixMode<1>(a, bed, store);
    else if (mode == 2) mixMode<2>(a, bed, store);
    else mixMode<0>(a, bed, store);
}

// Peak of |l| + |r| over every 8th sample (meters only).
float sparsePeak(const float* L, const float* R) {
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; i += 8) {
        const float v = fabsf(L[i]) + fabsf(R[i]);
        pk = v > pk ? v : pk;
    }
    return pk;
}
}  // namespace

// ---- the block --------------------------------------------------------------------
void Engine::render(const lab::Transport& t, const lab::AudioIn& in, float* outL, float* outR,
                                  lab::Context* ctx) {
    static float cl[kClouds][kBlock], cr[kClouds][kBlock];
    static float sendD[kBlock], sendR[kBlock], wetL[kBlock], wetR[kBlock];
    static float stemL[kStemCount][kBlock], stemR[kStemCount][kBlock];
    if (t.startedThisBlock) resetGestures();
    spawnsLeft = kSpawnsPerBlock;
    playing = t.playing ? 1 : 0;
    beat = t.beat;

    // input (mono int16) for the live buffers
    static int16_t inS16[kBlock], mixS16[kBlock];
    if (in.frames) {
        for (uint32_t i = 0; i < kBlock; ++i)
            inS16[i] = int16_t((int32_t(in.frames[2 * i]) + int32_t(in.frames[2 * i + 1])) >> 1);
    } else {
        std::memset(inS16, 0, sizeof inS16);
    }

    std::memset(cl, 0, sizeof cl);
    std::memset(cr, 0, sizeof cr);

    // clouds take turns at the head of the spawn queue (the per-block spawn budget is shared)
    spawnRot_ = spawnRot_ + 1 >= kClouds ? 0 : spawnRot_ + 1;
    for (int ci = 0; ci < kClouds; ++ci) {
        const int c = (ci + spawnRot_) % kClouds;
        CloudRt& r = rt[c];
        CloudState& s = st_[c];
        // live buffer write
        const CloudSource& so = src_[c];
        if (!r.busy && so.type != kSrcSample && !so.hold) {
            const int16_t* x = so.type == kSrcLiveIn ? inS16 : mixS16;
            const int32_t gin = int32_t(so.inGain * 4096.0f);   // Q12
            const bool fb = r.fbOn && s.p[0][P::Feedback] > 0.001f;
            int16_t* buf = r.buf;
            const uint32_t w = r.w;
            int16_t* d = buf + w;   // kBufLen is a multiple of kBlock: a block never wraps
            if (!fb && gin == 4096) {
                std::memcpy(d, x, kBlock * sizeof(int16_t));
            } else if (!fb) {
                for (uint32_t i = 0; i < kBlock; ++i) d[i] = int16_t(sat16((int32_t(x[i]) * gin) >> 12));
            } else {
                // fbPrev already holds the feedback in int16 units (scaled in the mix pass)
                const int32_t* fp = r.fbPrev;
                for (uint32_t i = 0; i < kBlock; ++i) d[i] = int16_t(sat16(((int32_t(x[i]) * gin) >> 12) + fp[i]));
            }
            r.w = (w + kBlock) & kBufMask;
        }
        if (r.busy) {
            for (int i = 0; i < kEmitters; ++i) r.em[i].acc = 0.0f;
            continue;
        }
        // drift (QUANTA): a slow random walk of the base frequency
        const float dr = s.p[1][P::QDrift];
        r.drift += (rng.uni() - 0.5f) * dr * 0.02f - r.drift * 0.002f;
        playGesture(c);
        for (int i = 0; i < kEmitters; ++i)
            if (r.em[i].on) runEmitter(c, r.em[i], true);
        if (s.drone) {
            r.drone.pad = 0xFF;
            r.drone.vel = 90;
            r.drone.semis = 0.0f;
            runEmitter(c, r.drone, false);
        }
    }

    // grains
    int n = nActive_;
    uint32_t touched = 0;
    for (int c = 0; c < kClouds; ++c) rt[c].active = 0;
    for (int i = 0; i < n;) {
        Grain& g = pool_[i];
        if (rt[g.cloud].busy) {   // the main loop is replacing this cloud's buffer
            pool_[i] = pool_[--n];
            continue;
        }
        uint32_t d = g.delay < kBlock ? g.delay : kBlock - 1;
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

    // per cloud: bed, tone, level / pan, sends, stems, feedback, meters
    std::memset(sendD, 0, sizeof sendD);
    std::memset(sendR, 0, sizeof sendR);
    const bool stems = ctx != nullptr && ctx->shell != nullptr && ctx->shell->recording();
    if (stems) {
        std::memset(stemL, 0, sizeof stemL);
        std::memset(stemR, 0, sizeof stemR);
    }
    const uint8_t solo = soloMask;
    for (int c = 0; c < kClouds; ++c) {
        CloudRt& r = rt[c];
        CloudState& s = st_[c];
        float* L = cl[c];
        float* R = cr[c];
        const bool bed = s.freeze && r.frzReady && s.bed > 0.01f && !r.busy;
        if (!bed && !(touched & (1u << c))) {
            // silent this block: nothing to filter, send or meter (filters rest at 0)
            r.fbOn = 0;
            r.peak *= 0.93f;
            r.lpL = r.lpR = r.hpL = r.hpR = 0.0f;
            continue;
        }
        // tone: tilt low-pass (< 0.5) / high-pass (> 0.5), coefficient cached per value
        const float tone = s.p[0][P::Tone];
        if (tone != r.toneV) {
            r.toneV = tone;
            float hz = tone < 0.5f ? 150.0f * fexp(9.8f * tone) : 20.0f * fexp(10.6f * (tone - 0.5f));
            r.toneA = 1.0f - fexp(-dsp::kTwoPi * hz * dsp::kInvSr);
        }
        const int mode = tone < 0.48f ? 1 : (tone > 0.52f ? 2 : 0);
        const bool audible = !s.mute && (solo == 0 || (solo >> c) & 1);
        const float lvl = audible ? s.p[0][P::Level] * s.p[0][P::Level] * 1.6f : 0.0f;
        if (s.pan != r.panV) {
            r.panV = s.pan;
            r.panL = sqrtf(1.0f - s.pan) * 1.41f;
            r.panR = sqrtf(s.pan) * 1.41f;
        }
        const float pl = lvl * r.panL, prr = lvl * r.panR;
        r.fbOn = s.p[0][P::Feedback] > 0.001f ? 1 : 0;
        MixArgs ma;
        ma.L = L;
        ma.R = R;
        ma.outL = outL;
        ma.outR = outR;
        ma.sendD = sendD;
        ma.sendR = sendR;
        ma.fb = r.fbOn ? r.fbPrev : nullptr;
        ma.fbK = 0.5f * s.p[0][P::Feedback] * 0.95f * 32767.0f;
        // freeze bed: the loop itself, two taps half a loop apart (kFrzLen is a multiple of kBlock)
        if (bed) {
            const uint32_t p = r.bedPos;
            ma.bedA = r.frz + p;
            ma.bedB = r.frz + ((p + (kFrzLen / 2)) & kFrzMask);
            ma.bedK = s.bed * s.bed * 0.5f * (1.0f / 32768.0f);
            r.bedPos = (p + kBlock) & kFrzMask;
        } else {
            ma.bedA = ma.bedB = nullptr;
            ma.bedK = 0.0f;
        }
        ma.pl = pl;
        ma.pr = prr;
        ma.kd = 0.5f * s.p[0][P::Delay] * s.p[0][P::Delay] * lvl;
        ma.kr = 0.5f * s.p[0][P::Reverb] * s.p[0][P::Reverb] * lvl;
        ma.ta = r.toneA;
        ma.zl = mode == 1 ? r.lpL : r.hpL;
        ma.zr = mode == 1 ? r.lpR : r.hpR;
        const bool store = (stems && lvl > 0.0f) || (capOn && capSrc == c);
        mixCloud(ma, mode, bed, store);
        if (mode == 1) {
            r.lpL = ma.zl;
            r.lpR = ma.zr;
        } else if (mode == 2) {
            r.hpL = ma.zl;
            r.hpR = ma.zr;
        }
        if (stems && lvl > 0.0f) {
            float* sl = stemL[s.stem & 3];
            float* sr = stemR[s.stem & 3];
            for (uint32_t i = 0; i < kBlock; ++i) {
                sl[i] += L[i] * pl;
                sr[i] += R[i] * prr;
            }
        }
        float pk = sparsePeak(L, R) * 0.5f * (pl > prr ? pl : prr);
        if (bed) pk += s.bed * s.bed * 0.35f * (pl > prr ? pl : prr);
        r.peak = pk > r.peak ? pk : r.peak * 0.93f;
    }

    // send effects
    const FxState& f = *fx_;
    if (dlyTone_ != f.dlyTone) {
        dlyTone_ = f.dlyTone;
        dly_.setFilter(400.0f * expf(3.9f * f.dlyTone));
    }
    const float beats = dlyDivBeats(enumIndex(f.dlyDiv, kDlyDivs));
    const float bpm = t.bpm > 20.0f ? t.bpm : 120.0f;
    dly_.setTimeSamples(uint32_t(beats * 60.0f / bpm * dsp::kSr));
    dly_.setFeedbackK(uint8_t(f.dlyFb * 0.92f * 255.0f));
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
    if (rev_->process(sendR, wetL, wetR)) {
        const float k = f.revRet * 1.6f;
        for (uint32_t i = 0; i < kBlock; ++i) {
            outL[i] += wetL[i] * k;
            outR[i] += wetR[i] * k;
        }
    }

    // master: a soft knee above 0.75 (identity below it, C1-continuous, flat at 1.0 from 1.25),
    // so a block that stays under the knee skips the curve without changing the sound;
    // meter and the history for OUTPUT sources and freezes in the same pass
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
    uint32_t mw = mixW;
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

    // resample capture (mono)
    if (capOn) {
        const int src = capSrc;
        uint32_t len = capLen;
        for (uint32_t i = 0; i < kBlock && len < kBufLen; ++i, ++len)
            capBuf[len] = src < 0 ? mixS16[i] : toS16(0.5f * (cl[src][i] + cr[src][i]));
        capLen = len;
        if (len >= kBufLen) capOn = 0;
    }
    if (stems)
        for (int sIdx = 0; sIdx < kStemCount; ++sIdx) ctx->stem(sIdx, stemL[sIdx], stemR[sIdx]);
    sampleClock = sampleClock + kBlock;
}

}  // namespace bulut
