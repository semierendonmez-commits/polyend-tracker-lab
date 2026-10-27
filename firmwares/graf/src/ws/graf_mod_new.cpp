// GRAF -- round-2 modules:
//   RESO   Rings-style modal resonator (12 partials, own code after the idea of
//          Mutable Instruments Rings: structure / brightness / damping / position)
//   EXCT   Elements-style exciter (bow, blow, strike) -- own code, same idea
//   NET    the user's RETICULUM patch: forsitan rete (8 leaky integrators in a
//          random 8x8 matrix, DC block, hard clip; GPL-3, via /home/claude/reticulum)
//          with Random Values on the node gains and the Manual Gate re-rolls
//   RVAL   Random Values (VCV Fundamental, GPL-3, via reticulum): new values on a trigger
//   MG     Modulation Generator MG-16 (TinyTricks, GPL-3, via reticulum), 4 outputs
//   MGAT   Manual Gate (Count Modula, GPL-3, via reticulum): the Perform key
//   CHAO   Lorenz attractor + logistic map
//   BERN   Bernoulli gate
//   ACID   303 voice / filter: Open303 (Robin Schmidt, MIT) in float, as ported in KONSOL
//   FLTR   the user's FLOATER kick / snare / perc (via KONSOL's port), 1x rate
//   PART   the user's QUANTA microsound particles (via KONSOL's port)
#include <cstring>

#include "core/fastrun.h"
#include "ws/graf_modules.h"

namespace graf {

namespace {
inline float wrap01(float p) {
    p -= float(int32_t(p));
    return p < 0.0f ? p + 1.0f : p;
}
// one-pole coefficient for a -60 dB decay time
inline float decayK(float sec) { return expf(-6.9f / ((sec > 0.0005f ? sec : 0.0005f) * kSr)); }
// time constant (1/e) coefficient
inline float tauK(float sec) { return expf(-1.0f / ((sec > 0.00002f ? sec : 0.00002f) * kSr)); }
inline float semis(float st) { return fexp2(st * (1.0f / 12.0f)); }
inline float fflo(float x) {
    float t = float(int32_t(x));
    return t > x ? t - 1.0f : t;
}
// Simple TPT SVF with low / band / high outputs.
struct SvfS {
    float ic1, ic2, a1, a2, a3, k;
    void set(float hz, float q) {
        const float g = tanPi(clampf(hz, 10.0f, 18000.0f));
        k = 1.0f / (q > 0.3f ? q : 0.3f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(float x, float& lp, float& bp) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
    }
    float low(float x) {
        float l, b;
        tick(x, l, b);
        return l;
    }
    float band(float x) {
        float l, b;
        tick(x, l, b);
        return b;
    }
    float high(float x) {
        float l, b;
        tick(x, l, b);
        return x - k * b - l;
    }
};
float gWin[6][257];   // QUANTA particle windows
}  // namespace

bool initModuleTables(lab::Arena& a) {
    (void)a;
    for (int w = 0; w < 6; ++w)
        for (int i = 0; i <= 256; ++i) {
            const float x = float(i) / 256.0f;
            float v = 1.0f;
            switch (w) {
                case 0: {   // gauss
                    const float e = expf(-0.5f * (0.5f / 0.15f) * (0.5f / 0.15f));
                    v = (expf(-0.5f * ((x - 0.5f) / 0.15f) * ((x - 0.5f) / 0.15f)) - e) / (1.0f - e);
                    break;
                }
                case 1: v = 0.5f - 0.5f * sinf(kTwoPi * (x + 0.25f)); break;   // hann
                case 2:
                case 3: {   // expodec / rexpodec
                    const float xx = w == 2 ? x : 1.0f - x;
                    if (xx < 0.005f) v = xx / 0.005f;
                    else {
                        const float u = (xx - 0.005f) / 0.995f, e = expf(-5.0f);
                        v = (expf(-5.0f * u) - e) / (1.0f - e);
                    }
                    break;
                }
                case 4: {   // trapezoid
                    v = x / 0.1f;
                    if ((1.0f - x) / 0.1f < v) v = (1.0f - x) / 0.1f;
                    v = v > 1.0f ? 1.0f : v;
                    break;
                }
                default: v = 1.0f; break;
            }
            gWin[w][i] = v < 0.0f ? 0.0f : v;
        }
    return true;
}

int laneOutput(int type) {
    switch (type) {
        case kSeq: return 2;      // CV
        case kTuring: return 1;   // CV
        case kMarkov: return 2;   // State
        case kPinb: return 2;     // X
        case kClk: return 1;      // Beat
        case kDiv: return 2;      // /N
        case kEucl: return 0;
        default: return 0;
    }
}

// ---------------------------------------------------------------- RESO
// Twelve two-pole resonators. Model Modal: partial ratios move from harmonic
// (Structure 0) towards a free bar's (1); String: harmonic with stiffness
// (dispersion grows with Structure); Bell: bell partials stretched by
// Structure. Position weights the partials like a pluck point (sin(pi k p)),
// Brightness tilts them, Damping sets the decay (higher partials decay faster
// when dark). Odd / Even split the partials like Rings' two outputs.
namespace {
constexpr int kModes = 12;
const float kBar[kModes] = {1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.81f, 31.87f, 39.81f, 48.63f, 58.33f, 68.92f};
const float kBell[kModes] = {0.5f, 1.0f, 1.19f, 1.56f, 2.0f, 2.51f, 2.66f, 3.01f, 4.1f, 5.42f, 6.0f, 8.0f};
}  // namespace
struct ResoS {
    float y1[kModes], y2[kModes], c1[kModes], c2[kModes], g[kModes];
    float burst, lp;
    Schmitt strum;
    Rand r;
};
OT_FASTRUN_FN void pReso(Module& m, Proc& p) {
    auto& s = m.s<ResoS>();
    if (!s.r.s) s.r.seed(0x51A7u + m.cell);
    const int model = int(m.v[0]);
    const float f0 = pitchHz(m.v[1] + m.v[2] * (1.0f / 12.0f) + p.in[2][0]);
    const float st = clampf(m.v[3], 0.0f, 1.0f);
    const float bright = clampf(m.v[4] + p.in[3][0], 0.0f, 1.0f);
    const float damp = clampf(m.v[5] + p.in[4][0], 0.0f, 1.0f);
    const float pos = clampf(m.v[6] + p.in[5][0], 0.0f, 1.0f);
    const float strumLvl = m.v[7], lvl = m.v[8];
    const float T = 0.05f * fexp2(damp * 7.5f);   // 0.05 .. 9 s
    // coefficients once per block
    for (int k = 0; k < kModes; ++k) {
        float ratio;
        const float n = float(k + 1);
        if (model == 0) ratio = lerpf(n, kBar[k], st);
        else if (model == 1) ratio = n * (1.0f + st * st * 0.02f * n * n);
        else ratio = kBell[k] * (1.0f + st * 0.25f * float(k) * 0.1f) * 2.0f;
        const float hz = f0 * ratio;
        if (hz > 18000.0f) {
            s.g[k] = 0.0f;
            s.c1[k] = 0.0f;
            s.c2[k] = 0.0f;
            continue;
        }
        const float Tk = T / (1.0f + float(k) * (1.0f - bright) * 0.8f);
        const float rr = decayK(Tk);
        const float w = hz * kInvSr;
        s.c1[k] = 2.0f * rr * sinP(wrap01(w + 0.25f));
        s.c2[k] = -rr * rr;
        const float place = sinP(wrap01(0.5f * n * (pos * 0.98f + 0.01f)));
        const float tilt = fexp2(-float(k) * (1.0f - bright) * 1.5f);
        s.g[k] = (1.0f - rr) * 6.0f * (place < 0.0f ? -place : place) * tilt;
    }
    const float *in = p.in[0], *sg = p.in[1];
    const bool hasStrum = p.has(1);
    const float bcoef = 0.05f + bright * 0.9f;
    float *oO = m.out[0], *oE = m.out[1], *oM = m.out[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (hasStrum && s.strum.rise(sg[i])) s.burst = strumLvl;
        float x = in[i];
        if (s.burst > 0.0005f) {
            s.lp += bcoef * (s.r.bi() - s.lp);
            x += s.lp * s.burst * 4.0f;
            s.burst *= 0.995f;
        }
        float odd = 0.0f, even = 0.0f;
        for (int k = 0; k < kModes; k += 2) {
            const float a = s.c1[k] * s.y1[k] + s.c2[k] * s.y2[k] + s.g[k] * x;
            s.y2[k] = s.y1[k];
            s.y1[k] = a;
            odd += a;
            const float b = s.c1[k + 1] * s.y1[k + 1] + s.c2[k + 1] * s.y2[k + 1] + s.g[k + 1] * x;
            s.y2[k + 1] = s.y1[k + 1];
            s.y1[k + 1] = b;
            even += b;
        }
        oO[i] = sat(odd * lvl);
        oE[i] = sat(even * lvl);
        oM[i] = sat((odd + even) * 0.7f * lvl);
    }
    for (int k = 0; k < kModes; ++k)
        if (!(s.y1[k] == s.y1[k]) || fabsf(s.y1[k]) > 1e4f) s.y1[k] = s.y2[k] = 0.0f;
}

// ---------------------------------------------------------------- EXCT
struct ExctS {
    float env, strike, mal, bowLp, blowLp, lastG;
    SvfS blow;
    Rand r;
    bool gate;
};
OT_FASTRUN_FN void pExct(Module& m, Proc& p) {
    auto& s = m.s<ExctS>();
    if (!s.r.s) s.r.seed(0xE1E7u + m.cell);
    const float bow = clampf(m.v[0] + p.in[1][0], 0.0f, 1.0f), bowT = m.v[1];
    const float blow = clampf(m.v[2] + p.in[2][0], 0.0f, 1.0f), flow = m.v[3];
    const float strike = clampf(m.v[4] + p.in[3][0], 0.0f, 1.0f), mallet = m.v[5];
    const float contour = m.v[6], lvl = m.v[7];
    s.blow.set(200.0f * fexp2(flow * 6.0f), 0.8f + flow * 2.0f);
    // contour: 0 = percussive (fast attack, short decay), 1 = sustained bowing (slow attack, hold)
    const float atk = 1.0f - tauK(0.001f + contour * 0.25f);
    const float rel = 1.0f - tauK(0.02f + contour * 0.6f);
    const float malK = tauK(0.0005f + (1.0f - mallet) * 0.01f);
    const float bowC = 0.02f + bowT * 0.5f;
    const float *gt = p.in[0];
    const bool hasGate = p.has(0);
    float *o = m.out[0], *oe = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const bool g = hasGate ? gt[i] > 0.4f : false;
        if (g && !s.gate) {
            s.strike = 1.0f;
            s.mal = 1.0f;
        }
        s.gate = g;
        const float tgt = g ? 1.0f : 0.0f;
        s.env += (tgt - s.env) * (tgt > s.env ? atk : rel);
        const float n = s.r.bi();
        // bow: stick-slip-ish: rectified, low-passed noise pulses
        s.bowLp += bowC * ((n > 0.6f - bowT * 0.5f ? 1.0f : -0.3f) - s.bowLp);
        const float bw = s.bowLp * bow;
        const float bl = s.blow.band(n) * blow * 2.0f;
        s.mal *= malK;
        const float st = (s.mal * s.mal * 2.0f + n * s.mal * mallet * 0.5f) * strike;
        o[i] = (bw + bl) * s.env * lvl + st * lvl;
        oe[i] = s.env;
    }
}

// ---------------------------------------------------------------- NET (forsitan rete)
// The rete module of the user's patch, from its VCV source as ported in
// RETICULUM: eight leaky integrators, a fixed random 8 x 8 matrix (from Seed),
// DC-blocking high-pass, hard clip, fed back one sample later. The node gains
// come from GAIN plus eight Random Values (spread by SPREAD), re-rolled by a
// trigger on Roll -- the Manual Gate hitting the Random Values in the patch;
// a trigger on Rnd draws a new matrix (rete's RND button).
namespace {
constexpr int kNetN = 8;
}
struct NetS {
    float matrix[kNetN][kNetN];
    float integ[kNetN], hpX[kNetN], hpY[kNetN], fb[kNetN], rv[kNetN];
    float panL[kNetN], panR[kNetN];
    float env;
    uint32_t seed, noise;
    int lastSeed;
    Schmitt roll, rnd;
    Rand r;
};
static void netSeed(NetS& s, uint32_t seed) {
    s.seed = seed ? seed : 1u;
    uint32_t r = s.seed;
    auto bip = [&r]() {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return float(r >> 8) * (2.0f / 16777216.0f) - 1.0f;
    };
    for (int i = 0; i < kNetN; ++i)
        for (int j = 0; j < kNetN; ++j) s.matrix[i][j] = bip();
    for (int i = 0; i < kNetN; ++i) {
        s.integ[i] = 0.01f * bip();
        s.hpX[i] = s.hpY[i] = s.fb[i] = 0.0f;
    }
}
static void netRoll(NetS& s) {
    for (int i = 0; i < kNetN; ++i) s.rv[i] = s.r.uni();
}
void rNet(Module& m) {
    auto& s = m.s<NetS>();
    for (int i = 0; i < kNetN; ++i) {
        const float q = float(i) / float(kNetN - 1);
        s.panR[i] = sqrtf(q);
        s.panL[i] = sqrtf(1.0f - q);
    }
    s.r.seed(0x5EEDu + m.cell * 31u);
    s.noise = 0x9d2c5680u;
    s.lastSeed = -1;
    netRoll(s);
}
float netNode(const Module& m, int i) {
    const NetS& s = *reinterpret_cast<const NetS*>(m.st.bytes);
    return fabsf(s.fb[i & 7]);
}
OT_FASTRUN_FN void pNet(Module& m, Proc& p) {
    auto& s = m.s<NetS>();
    const int seedK = int(m.v[5]);
    if (seedK != s.lastSeed) {
        s.lastSeed = seedK;
        netSeed(s, uint32_t(seedK) * 2654435761u + 0x5eedf00du);
    }
    const float *rl = p.in[0], *rn = p.in[1], *ex = p.in[2];
    if (p.has(0) || p.has(1))
        for (uint32_t i = 0; i < kBlock; i += 2) {
            if (p.has(0) && s.roll.rise(rl[i])) netRoll(s);
            if (p.has(1) && s.rnd.rise(rn[i])) netSeed(s, s.r.next());
        }
    if (m.ext[0] != m.ext[1]) {   // Rec on the module page: a manual re-roll
        m.ext[1] = m.ext[0];
        netRoll(s);
    }
    // block-rate constants (the source computes them per sample from the knobs)
    const float leakK = clampf(m.v[2] + p.in[4][0], 0.0f, 1.0f);
    const float scaleK = clampf(m.v[3] + p.in[5][0], 0.0f, 1.0f);
    const float srRatio = 48000.0f / 44100.0f;
    const float leak48 = 1.0f - fexp2((-1.0f - 4.0f * leakK) * 3.3219281f);
    const float leak = fexp2(log2f(leak48) * srRatio);
    const float dcR = fexp2(log2f(0.995f) * srRatio);
    const float scale = fexp2(3.0f * scaleK * 3.3219281f);
    const float gain = clampf(m.v[0] + p.in[3][0], 0.0f, 1.0f), spread = m.v[1];
    float gi[kNetN];
    for (int i = 0; i < kNetN; ++i) gi[i] = clampf(gain + spread * (s.rv[i] * 2.0f - 1.0f) * 0.5f, 0.0f, 1.0f) * scale;
    const float exLvl = m.v[4] * 0.5f;   // excite: volts * 0.1 * level, with audio +-1 = +-5 V
    const float outK = m.v[6] * 0.2f * 5.0f / (float(kNetN) * 0.5f);
    float *oL = m.out[0], *oR = m.out[1], *oM = m.out[2], *oE = m.out[3];
    uint32_t ns = s.noise;
    for (uint32_t n = 0; n < kBlock; ++n) {
        const float excite = ex[n] * exLvl;
        ns ^= ns << 13;
        ns ^= ns >> 17;
        ns ^= ns << 5;
        const float life = 1.0e-5f * (float(ns >> 8) * (2.0f / 16777216.0f) - 1.0f);
        float g[kNetN];
        for (int i = 0; i < kNetN; ++i) {
            const float in = s.fb[i] + ((i & 1) ? -excite : excite) + life;
            s.integ[i] = leak * s.integ[i] + in;
            g[i] = gi[i] * s.integ[i];
        }
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < kNetN; ++i) {
            const float* row = s.matrix[i];
            const float mm = row[0] * g[0] + row[1] * g[1] + row[2] * g[2] + row[3] * g[3] + row[4] * g[4] +
                             row[5] * g[5] + row[6] * g[6] + row[7] * g[7];
            const float y = mm - s.hpX[i] + dcR * s.hpY[i];
            s.hpX[i] = mm;
            s.hpY[i] = y;
            const float c = y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
            s.fb[i] = c;
            l += c * s.panL[i];
            r += c * s.panR[i];
        }
        l *= outK;
        r *= outK;
        oL[n] = l;
        oR[n] = r;
        oM[n] = 0.5f * (l + r);
        const float a = fabsf(l) + fabsf(r);
        s.env += (a > s.env ? 0.05f : 0.001f) * (a - s.env);
        oE[n] = s.env;
    }
    s.noise = ns;
    for (int i = 0; i < kNetN; ++i)
        if (!(s.hpY[i] == s.hpY[i]) || !(s.integ[i] == s.integ[i]) || fabsf(s.integ[i]) > 1e9f) {
            s.hpX[i] = s.hpY[i] = s.integ[i] = s.fb[i] = 0.0f;
        }
}

// ---------------------------------------------------------------- RVAL
struct RvalS {
    float val[4], out[4];
    Schmitt t;
    Rand r;
};
OT_FASTRUN_FN void pRval(Module& m, Proc& p) {
    auto& s = m.s<RvalS>();
    if (!s.r.s) s.r.seed(0xBA11u + m.cell * 7u);
    const bool bi = m.v[0] > 0.5f;
    const float sc = m.v[2];
    const float sl = coefSec(m.v[1]);
    bool fire = false;
    if (m.ext[0] != m.ext[1]) {
        m.ext[1] = m.ext[0];
        fire = true;
    }
    float* o[4] = {m.out[0], m.out[1], m.out[2], m.out[3]};
    for (uint32_t i = 0; i < kBlock; ++i) {
        if ((p.has(0) && s.t.rise(p.in[0][i])) || fire) {
            fire = false;
            for (int k = 0; k < 4; ++k) s.val[k] = (bi ? s.r.bi() : s.r.uni()) * sc;
        }
        for (int k = 0; k < 4; ++k) {
            s.out[k] += (s.val[k] - s.out[k]) * sl;
            o[k][i] = s.out[k];
        }
    }
}

// ---------------------------------------------------------------- MG (MG-16)
struct MgSrc {
    float phase, freq, wave, hold;
    bool holding;
};
struct MgS {
    MgSrc src[4];
    Schmitt t;
    Rand r;
    bool init;
};
static void mgRegen(MgS& s, float range, float bias, bool sh) {
    for (auto& q : s.src) {
        q.holding = sh && s.r.uni() >= 0.5f;
        if (q.holding) {
            const float lo = clampf(bias - range, -1.0f, 1.0f), hi = clampf(bias + range, -1.0f, 1.0f);
            q.hold = lo + s.r.uni() * (hi - lo);
        } else {
            q.wave = s.r.uni() * 3.0f;
            const float spread = 8.0f * range, b = bias * 8.0f;
            const float lo = clampf(b - spread, -8.0f, 8.0f), hi = clampf(b + spread, -8.0f, 8.0f);
            q.freq = fexp2(lo + s.r.uni() * (hi - lo));
            q.phase = 0.0f;
        }
    }
}
OT_FASTRUN_FN void pMg(Module& m, Proc& p) {
    auto& s = m.s<MgS>();
    const float range = clampf(m.v[0] + p.in[1][0], 0.0f, 1.0f), bias = m.v[1];
    const bool sh = m.v[2] > 0.5f, uni = m.v[3] > 0.5f;
    const float lvl = m.v[4];
    if (!s.init) {
        s.r.seed(0x316u + m.cell);
        mgRegen(s, range, bias, sh);
        s.init = true;
    }
    if (m.ext[0] != m.ext[1]) {
        m.ext[1] = m.ext[0];
        mgRegen(s, range, bias, sh);
    }
    if (p.has(0))
        for (uint32_t i = 0; i < kBlock; i += 2)
            if (s.t.rise(p.in[0][i])) mgRegen(s, range, bias, sh);
    for (int k = 0; k < 4; ++k) {
        MgSrc& q = s.src[k];
        float* o = m.out[k];
        if (q.holding) {
            const float v = (uni ? 0.5f + 0.5f * q.hold : q.hold) * lvl;
            for (uint32_t i = 0; i < kBlock; ++i) o[i] = v;
            continue;
        }
        float inc = q.freq * kInvSr;
        if (inc > 0.5f) inc = 0.5f;
        const float w = q.wave;
        const float ws = clampf(1.0f - fabsf(w), 0.0f, 1.0f), wt = clampf(1.0f - fabsf(w - 1.0f), 0.0f, 1.0f),
                    ww = clampf(1.0f - fabsf(w - 2.0f), 0.0f, 1.0f), wq = clampf(1.0f - fabsf(w - 3.0f), 0.0f, 1.0f);
        float ph = q.phase;
        for (uint32_t i = 0; i < kBlock; ++i) {
            ph += inc;
            if (ph >= 1.0f) ph -= 1.0f;
            const float sn = sinP(ph);
            const float tr = 4.0f * (ph < 0.5f ? ph : 1.0f - ph) - 1.0f;
            const float sw = 2.0f * ph - 1.0f;
            const float sq = ph < 0.5f ? 1.0f : -1.0f;
            const float v = sn * ws + tr * wt + sw * ww + sq * wq;
            o[i] = (uni ? 0.5f + 0.5f * v : v) * lvl;
        }
        q.phase = ph;
    }
}

// ---------------------------------------------------------------- MGAT
struct MgatS {
    bool high, latch;
    int32_t trig;
    float ext;
};
OT_FASTRUN_FN void pMgate(Module& m, Proc& p) {
    auto& s = m.s<MgatS>();
    const bool button = p.eng->manualGate || m.ext[0] != 0;
    const float len = m.v[0];
    float *oG = m.out[0], *oT = m.out[1], *oL = m.out[2], *oX = m.out[3];
    const float* in = p.in[0];
    const bool hasIn = p.has(0);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const bool b = button || (hasIn && in[i] > 0.4f);
        if (b && !s.high) {
            s.latch = !s.latch;
            s.trig = 44;   // 1 ms
        }
        s.high = b;
        if (b) s.ext = len;
        else if (s.ext > 0.0f) s.ext -= kInvSr;
        oG[i] = b ? 1.0f : 0.0f;
        oT[i] = s.trig > 0 ? 1.0f : 0.0f;
        if (s.trig > 0) --s.trig;
        oL[i] = s.latch ? 1.0f : 0.0f;
        oX[i] = (b || s.ext > 0.0f) ? 1.0f : 0.0f;
    }
}

// ---------------------------------------------------------------- CHAO
struct ChaoS {
    float x, y, z, lg;
    Schmitt t;
    bool init;
};
OT_FASTRUN_FN void pChaos(Module& m, Proc& p) {
    auto& s = m.s<ChaoS>();
    if (!s.init) {
        s.x = 0.1f;
        s.y = 0.0f;
        s.z = 20.0f;
        s.lg = 0.37f;
        s.init = true;
    }
    const float rate = m.v[0] * fexp2(p.in[1][0] * 4.0f);
    const float rho = m.v[1];
    const float rr = m.v[2];
    const float lvl = m.v[3];
    const float dt = clampf(rate * 0.0004f, 0.0f, 0.02f);   // Lorenz time per sample
    const float sigma = 10.0f, beta = 8.0f / 3.0f;
    float *oX = m.out[0], *oY = m.out[1], *oZ = m.out[2], *oL = m.out[3];
    const bool clk = p.has(0);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float dx = sigma * (s.y - s.x), dy = s.x * (rho - s.z) - s.y, dz = s.x * s.y - beta * s.z;
        s.x += dx * dt;
        s.y += dy * dt;
        s.z += dz * dt;
        if (clk && s.t.rise(p.in[0][i])) s.lg = rr * s.lg * (1.0f - s.lg);
        oX[i] = clampf(s.x * 0.05f, -1.0f, 1.0f) * lvl;
        oY[i] = clampf(s.y * 0.037f, -1.0f, 1.0f) * lvl;
        oZ[i] = clampf((s.z - 25.0f) * 0.04f, -1.0f, 1.0f) * lvl;
        oL[i] = s.lg * lvl;
    }
    if (!(s.x == s.x) || fabsf(s.x) > 1e3f) s.init = false;
    if (!clk && p.playing) {   // no clock patched: the logistic map steps every 1/16
        const float pos = (p.beat + float(kBlock) * p.bps) * 4.0f;
        if (int32_t(pos) != int32_t(p.beat * 4.0f)) s.lg = rr * s.lg * (1.0f - s.lg);
    }
    if (s.lg <= 0.0f || s.lg >= 1.0f || !(s.lg == s.lg)) s.lg = 0.37f;
}

// ---------------------------------------------------------------- BERN
struct BernS {
    Schmitt t;
    bool a, b, fall;
    int32_t pa, pb;
    Rand r;
};
OT_FASTRUN_FN void pBern(Module& m, Proc& p) {
    auto& s = m.s<BernS>();
    if (!s.r.s) s.r.seed(0xBE7u + m.cell * 13u);
    const int mode = int(m.v[1]);   // 0 trig, 1 gate, 2 latch
    float *oA = m.out[0], *oB = m.out[1];
    const float* in = p.in[0];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = in[i];
        if (s.t.rise(x)) {
            const float prob = clampf(m.v[0] + p.in[1][i], 0.0f, 1.0f);
            const bool heads = s.r.uni() < prob;
            s.a = heads;
            s.b = !heads;
            if (heads) s.pa = 220;
            else s.pb = 220;
        }
        float a, b;
        if (mode == 0) {
            a = s.pa > 0 ? 1.0f : 0.0f;
            b = s.pb > 0 ? 1.0f : 0.0f;
        } else if (mode == 1) {
            const bool hi = x > 0.25f;
            a = (hi && s.a) ? 1.0f : 0.0f;
            b = (hi && s.b) ? 1.0f : 0.0f;
        } else {
            a = s.a ? 1.0f : 0.0f;
            b = s.b ? 1.0f : 0.0f;
        }
        if (s.pa > 0) --s.pa;
        if (s.pb > 0) --s.pb;
        oA[i] = a;
        oB[i] = b;
    }
}

// ---------------------------------------------------------------- ACID (Open303)
// The 303 chain of KONSOL's TB303 engine (Open303 by Robin Schmidt, MIT,
// TB_303 filter coefficients by mystran & kunn) at 1x rate, driven by gates:
// a rising Gate starts a note (Accent high = accent, Slide high = slide).
// With In patched the oscillator is replaced by the input (a 303 filter).
struct AcidS {
    float phase, freq, target;
    float y1, y2, y3, y4, fbX, fbY, hp1X, hp1Y, hp2X, hp2Y, apX, apY;
    float mainEnv, mainK, rc2, accentGain, amp, ampRelK, declick;
    int32_t ampPhase;
    bool noteOn, init;
    Schmitt gate;
    float hp1A, hp2A, apA, rc2K, declickK;
};
OT_FASTRUN_FN void pAcid(Module& m, Proc& p) {
    auto& s = m.s<AcidS>();
    if (!s.init) {
        s.hp1A = expf(-kTwoPi * 44.486f * kInvSr);
        s.hp2A = expf(-kTwoPi * 24.167f * kInvSr);
        const float t = tanPi(14.008f);
        s.apA = (t - 1.0f) / (t + 1.0f);
        s.rc2K = expf(-1.0f / (kSr * 0.001f * 15.0f));
        s.declickK = expf(-kTwoPi * 200.0f * kInvSr);
        s.freq = s.target = 110.0f;
        s.init = true;
    }
    const float *in = p.in[0], *pit = p.in[1], *gt = p.in[2], *acc = p.in[3], *sld = p.in[4], *cutIn = p.in[5];
    const float wave = m.v[0];
    const float cutK = clampf(m.v[1] + cutIn[0], 0.0f, 1.0f);
    const float cut = 314.0f * fexp2(cutK * 2.9305f);   // 314 .. 2394 Hz
    const float e = m.v[3], c = cutK;
    const float envScaler = (1.0f - c) * (3.773996f * e + 0.736966f) + c * (4.194549f * e + 0.864345f);
    const float envOffset = 0.0482929f * c + 0.2943912f;
    const float rraw = m.v[2];
    const float rsk = (1.0f - expf(-3.0f * rraw)) / (1.0f - expf(-3.0f));
    const float slideK = tauK(m.v[6] * 0.001f * 0.2f > 0.0001f ? m.v[6] * 0.001f * 0.2f : 0.0001f);
    const float fbA = expf(-kTwoPi * 150.0f * kInvSr);
    const float vol = m.v[8] * m.v[8] * 1.2f;
    const float drive = m.v[7];
    const float dpre = 1.0f + 6.0f * drive, dpost = 1.0f / (1.0f + 2.0f * drive);
    const float ampDecK = expf(-1.0f / (kSr * 0.001f * 1230.0f * 0.25f));
    const float tune = m.v[9];
    const bool ext = p.has(0);
    s.target = pitchHz(pit[0] + tune * (1.0f / 12.0f));
    float b0 = 0, k = 0, g = 1;
    float* o = m.out[0];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.gate.rise(gt[i])) {
            const bool accent = acc[i] > 0.5f;
            const bool slide = sld[i] > 0.5f;
            s.target = pitchHz(pit[i] + tune * (1.0f / 12.0f));
            if (!slide) {
                s.freq = s.target;
                s.mainEnv = 1.0f;
                s.noteOn = true;
                s.ampPhase = 0;
            }
            s.accentGain = accent ? m.v[5] : 0.0f;
            const float decMs = accent ? 200.0f : 200.0f * fexp2(m.v[4] * 3.3219281f);
            s.mainK = expf(-1.0f / (kSr * 0.001f * decMs * 0.25f));
            s.ampRelK = expf(-1.0f / (kSr * 0.001f * (accent ? 50.0f : 1.0f)));
        } else if (s.noteOn && !s.gate.hi) {
            s.noteOn = false;
        }
        s.freq = s.target + slideK * (s.freq - s.target);
        const float mainOut = s.mainEnv;
        s.mainEnv *= s.mainK;
        s.rc2 = (s.accentGain > 0 ? mainOut : 0.0f) + s.rc2K * (s.rc2 - (s.accentGain > 0 ? mainOut : 0.0f));
        if ((i & 3) == 0) {
            const float t1 = envScaler * (mainOut - envOffset);
            const float t2 = s.accentGain * s.rc2;
            const float fc = clampf(cut * fexp2(t1 + t2), 200.0f, 20000.0f);
            const float fx = fc * kInvSr * 0.70710678f;
            b0 = (0.00045522346f + 6.1922189f * fx) / (1.0f + 12.358354f * fx + 4.4156345f * (fx * fx));
            k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f) - 476.47308f) + 614.95611f) + 213.87126f) +
                16.998792f;
            g = k * (1.0f / 17.0f);
            g = (g - 1.0f) * rsk + 1.0f;
            g = g * (1.0f + rsk);
            k = k * rsk;
        }
        if (s.noteOn) {
            s.amp = s.amp + (1.0f - s.amp) * (s.ampPhase < 3 ? 1.0f : 0.0f);
            ++s.ampPhase;
            s.amp *= ampDecK;
        } else {
            s.amp *= s.ampRelK > 0.0f ? s.ampRelK : 0.9f;
        }
        float ampOut = s.amp;
        if (s.noteOn) ampOut += 0.45f * mainOut + s.accentGain * 4.0f * mainOut;
        s.declick = ampOut + s.declickK * (s.declick - ampOut);
        float x;
        if (ext) {
            x = in[i];
        } else {
            const float dt = clampf(s.freq * kInvSr, 0.0f, 0.45f);
            s.phase += dt;
            if (s.phase >= 1.0f) s.phase -= 1.0f;
            float saw = 2.0f * s.phase - 1.0f;
            // polyBLEP
            if (s.phase < dt) {
                const float t = s.phase / dt;
                saw -= t + t - t * t - 1.0f;
            } else if (s.phase > 1.0f - dt) {
                const float t = (s.phase - 1.0f) / dt;
                saw -= t * t + t + t + 1.0f;
            }
            const float sq = s.phase < 0.5f ? 1.0f : -1.0f;
            x = -(saw + (sq - saw) * wave);
        }
        const float h = s.hp1A * (s.hp1Y + x - s.hp1X);
        s.hp1X = x;
        s.hp1Y = h;
        const float fbIn = k * s.y4;
        const float fb = fbA * (s.fbY + fbIn - s.fbX);
        s.fbX = fbIn;
        s.fbY = fb;
        const float y0 = h - fb;
        s.y1 += 2.0f * b0 * (y0 - s.y1 + s.y2);
        s.y2 += b0 * (s.y1 - 2.0f * s.y2 + s.y3);
        s.y3 += b0 * (s.y2 - 2.0f * s.y3 + s.y4);
        s.y4 += b0 * (s.y3 - 2.0f * s.y4);
        const float tmp = 2.0f * g * s.y4;
        const float ap = s.apA * tmp + s.apX - s.apA * s.apY;
        s.apX = tmp;
        s.apY = ap;
        const float h2 = s.hp2A * (s.hp2Y + ap - s.hp2X);
        s.hp2X = ap;
        s.hp2Y = h2;
        float y = h2 * (ext ? 1.0f : s.declick) * vol;
        if (drive > 0.01f) y = sat(y * dpre) * dpost;
        o[i] = y;
    }
    if (fabsf(s.y4) > 1e6f || !(s.y4 == s.y4)) s.y1 = s.y2 = s.y3 = s.y4 = s.fbX = s.fbY = 0.0f;
}

// ---------------------------------------------------------------- FLTR (FLOATER drums)
struct FltrS {
    bool on;
    float vel, amp, pe, ce, ph, lastX, dcX, dcY;           // kick
    float body, noise, snap, sph[3], pk0, pk1, pk2;         // snare
    bool attacking;
    float pb0, pb1, pb2;
    float impulse, burst, held, crush;                      // perc
    SvfS hp1, bp1, bp2;
    Schmitt t;
    Rand r;
    float note;
};
OT_FASTRUN_FN void pFltr(Module& m, Proc& p) {
    auto& s = m.s<FltrS>();
    if (!s.r.s) s.r.seed(0xF10Au + m.cell);
    const int voice = int(m.v[0]);
    const float tuneK = m.v[1], dec = m.v[2], tone = m.v[3], grit = m.v[4], color = m.v[5], lvl = m.v[6];
    const float *tr = p.in[0], *pit = p.in[1], *acc = p.in[2];
    float* o = m.out[0];
    const float pitchOct = pit[0];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.t.rise(tr[i])) {
            s.on = true;
            s.vel = clampf(1.0f + (p.has(2) ? acc[i] : 0.0f) * 0.5f, 0.2f, 1.6f);
            s.amp = 1.0f;
            s.pe = s.ce = 1.0f;
            s.ph = 0.0f;
            s.body = 1.0f;
            s.snap = 1.0f;
            s.attacking = true;
            s.noise = 0.0f;
            s.sph[0] = s.sph[1] = s.sph[2] = 0.0f;
            s.pk0 = s.pk1 = s.pk2 = 1.0f;
            s.impulse = 1.0f;
            s.burst = 1.0f;
            s.note = pitchOct;
        }
        o[i] = 0.0f;
    }
    if (!s.on) return;
    // the render (as in KONSOL's FlKit, the FLOATER voices), block-start triggers
    if (voice == 0) {   // kick: sine / triangle with pitch env, click, overdrive
        const float baseHz = (30.0f + tuneK * 90.0f) * fexp2(s.note);
        const float pEnvOct = (8.0f + tone * 40.0f) * (1.0f / 12.0f);
        const float ampK = decayK(0.02f + dec * 3.0f);
        const float pitchK = tauK(0.04f);
        const float clickK = tauK(0.004f);
        const float click = color * 0.5f, drive = grit;
        const float g2 = 1.0f + 12.0f * drive * drive, bias = 0.35f * drive;
        const float sb = sat(bias), odNorm = 1.0f / (1.0f + 0.6f * drive);
        const float shape = clampf(tone - 0.5f, 0.0f, 0.5f) * 2.0f;
        const float dcA = expf(-kTwoPi * 12.0f * kInvSr);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float hz = baseHz * fexp2(pEnvOct * s.pe);
            float step = hz * kInvSr;
            if (step > 0.45f) step = 0.45f;
            s.ph += step;
            if (s.ph >= 1.0f) s.ph -= 1.0f;
            const float sine = sinP(s.ph);
            float x = sine + (-(1.0f - 4.0f * fabsf(s.ph - 0.5f)) - sine) * shape;
            x *= s.amp;
            if (s.ce > 1e-3f) x += s.ce * s.ce * click * (0.6f * s.r.bi() + 0.8f);
            x = (sat(x * g2 + bias) - sb) * odNorm;
            const float hp = dcA * (s.dcY + x - s.dcX);
            s.dcX = x;
            s.dcY = hp;
            o[i] = hp * s.vel * lvl;
            s.amp *= ampK;
            s.pe *= pitchK;
            s.ce *= clickK;
        }
        if (s.amp < 1e-4f) s.on = false;
    } else if (voice == 1) {   // snare: three modal sines + white / pink noise through a high-pass, snap
        const float tune = (100.0f + tuneK * 300.0f) * fexp2(s.note);
        s.hp1.set(200.0f + color * 2000.0f, 0.7071f);
        const float bodyK = tauK(0.01f + tone * 0.3f);
        const float noiseK = tauK(0.01f + dec * 0.6f), snapK = tauK(0.012f);
        const float attackStep = 1.0f / 44.0f;
        const float bodyAmt = tone, noiseAmt = 1.0f - 0.6f * bodyAmt;
        const float k1 = bodyK * bodyK * 0.99f, k2 = k1 * bodyK;
        const float inc0 = tune * kInvSr, inc1 = tune * 1.59f * kInvSr, inc2 = tune * 2.14f * kInvSr;
        const float snapBend = 0.5f * grit;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float bend = 1.0f + snapBend * s.snap;
            s.sph[0] = wrap01(s.sph[0] + inc0 * bend);
            s.sph[1] = wrap01(s.sph[1] + inc1 * bend);
            s.sph[2] = wrap01(s.sph[2] + inc2 * bend);
            float body = sinP(s.sph[0]) * s.pk0;
            if (s.pk1 > 1e-4f) body += sinP(s.sph[1]) * 0.55f * s.pk1 + sinP(s.sph[2]) * 0.3f * s.pk2;
            if (s.attacking) {
                s.noise += attackStep;
                if (s.noise >= 1.0f) {
                    s.noise = 1.0f;
                    s.attacking = false;
                }
            } else {
                s.noise *= noiseK;
            }
            const float white = s.r.bi();
            s.pb0 = 0.99765f * s.pb0 + white * 0.0990460f;
            s.pb1 = 0.96300f * s.pb1 + white * 0.2965164f;
            s.pb2 = 0.57000f * s.pb2 + white * 1.0526913f;
            const float pink = clampf((s.pb0 + s.pb1 + s.pb2 + white * 0.1848f) * 0.75f, -1.5f, 1.5f);
            const float nz = s.hp1.high(white + (pink - white) * color);
            const float snap = s.snap * s.snap * s.r.bi() * grit * 0.6f;
            o[i] = sat((body * bodyAmt * 0.7f + nz * s.noise * noiseAmt + snap) * s.vel) * lvl;
            s.pk0 *= bodyK;
            s.pk1 *= k1;
            s.pk2 *= k2;
            s.snap *= snapK;
        }
        if (s.noise < 1e-4f && s.pk0 < 1e-4f && !s.attacking) s.on = false;
    } else {   // perc: an impulse into two high-Q band-passes, bit / rate crush
        const float hz = clampf(110.0f * semis(48.0f * tuneK) * fexp2(s.note), 30.0f, kSr * 0.4f);
        const float r3 = tone * tone * tone;
        const float q = 0.7f + 2000.0f * r3 * r3;
        s.bp1.set(hz, q);
        s.bp2.set(hz, q < 60.0f ? q : 60.0f);
        const float ampK = tauK(0.01f + dec * 2.0f), burstK = tauK(0.0008f);
        const float bits = 16.0f - grit * 13.0f;
        const float levels = fexp2(bits - 1.0f);
        const float rate = 44100.0f * fexp2(-color * 6.0f);
        const bool crushRate = color > 0.01f;
        const float makeUp = 1.0f + 6.0f * (1.0f - tone);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float x = s.impulse * 4.0f + s.burst * s.burst * s.r.bi() * 0.5f;
            s.impulse = 0;
            s.burst *= burstK;
            float y = s.bp2.band(sat(s.bp1.band(x))) * makeUp;
            y = sat(y) * s.amp * s.vel;
            s.amp *= ampK;
            float z = y;
            if (crushRate) {
                s.crush += rate * kInvSr;
                if (s.crush >= 1.0f) {
                    s.crush -= fflo(s.crush);
                    s.held = y;
                }
                z = s.held;
            }
            if (bits < 15.99f) z = fflo(z * levels + 0.5f) / levels;
            o[i] = z * lvl;
        }
        if (s.amp < 1e-4f) s.on = false;
    }
}

// ---------------------------------------------------------------- PART (QUANTA)
namespace {
constexpr int kParts = 6;
}
struct Part {
    bool on;
    uint8_t type, wave, win;
    int32_t age, len, pulseLen, wait, K;
    float phase, inc, incMul, amp, gl, gr, a, norm;
};
struct PartS {
    Part pt[kParts];
    int32_t next;
    int cursor;
    float env;
    Rand r;
};
static float partOsc(Part& q, Rand& r) {
    switch (q.wave) {
        case 1: return 2.0f * q.phase - 1.0f;
        case 2: return q.phase < 0.5f ? 1.0f : -1.0f;
        case 3: return r.bi();
        default: return sinP(q.phase);
    }
}
static float partTrain(Part& q) {
    const float c2 = 2.0f * sinP(wrap01(q.phase + 0.25f));
    float s1 = sinP(q.phase), s0 = 0.0f, sum = s1, a = q.a;
    for (int k = 2; k <= q.K; ++k) {
        const float sk = c2 * s1 - s0;
        s0 = s1;
        s1 = sk;
        sum += a * sk;
        a *= q.a;
    }
    return sum * q.norm;
}
OT_FASTRUN_FN void pPart(Module& m, Proc& p) {
    auto& s = m.s<PartS>();
    if (!s.r.s) s.r.seed(0x9A47u + m.cell * 17u);
    const int type = int(m.v[0]), wave = int(m.v[1]), win = int(m.v[2]);
    const float dens = clampf(m.v[3] * fexp2(p.in[2][0] * 3.0f), 0.2f, 400.0f);
    const float durMs = m.v[4];
    const float jit = m.v[5], spread = m.v[6], glide = m.v[7];
    const float lvl = m.v[9];
    const bool gated = p.has(0);
    const float hz0 = pitchHz(m.v[8] + p.in[1][0]);
    // cloud envelope: on while the gate is high (always when unpatched)
    const bool on = !gated || p.in[0][0] > 0.4f || p.in[0][kBlock - 1] > 0.4f;
    s.env += ((on ? 1.0f : 0.0f) - s.env) * 0.2f;
    float *oL = m.out[0], *oR = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) oL[i] = oR[i] = 0.0f;
    if (on) {
        const float period = 1.0f / dens;
        const int32_t perS = int32_t(period * kSr);
        s.next -= int32_t(kBlock);
        int guard = 0;
        while (s.next <= 0 && guard++ < 4) {
            Part* q = nullptr;
            for (auto& x : s.pt)
                if (!x.on) {
                    q = &x;
                    break;
                }
            if (!q) q = &s.pt[(s.cursor++) % kParts];
            Part& n = *q;
            n = Part();
            n.on = true;
            n.type = uint8_t(type);
            n.wave = uint8_t(wave);
            n.win = uint8_t(win);
            float hz = hz0 * (jit > 0.0f ? semis(jit * 12.0f * s.r.bi()) : 1.0f);
            hz = clampf(hz, 1.0f, 16000.0f);
            float dur;
            switch (type) {
                case 2: dur = clampf(8.0f / hz, 0.0005f, 1.0f); break;   // grainlet: 8 cycles
                case 3: dur = clampf(12.0f / hz, 0.0005f, 1.0f); break;  // trainlet
                case 4: dur = period; break;                              // pulsar
                default: dur = durMs * 0.001f; break;
            }
            n.len = int32_t(dur * kSr);
            if (n.len < 2) n.len = 2;
            if (n.len > int32_t(kSr)) n.len = int32_t(kSr);
            n.pulseLen = type == 4 ? n.len / 3 : n.len;
            if (type == 4) n.len = n.pulseLen > 2 ? n.pulseLen : 2;
            n.inc = hz * kInvSr;
            n.incMul = 1.0f;
            if (type == 1 && glide != 0.0f) n.incMul = fexp2(glide / 12.0f / float(n.len));
            if (type == 3) {
                n.a = 0.6f;
                n.K = int32_t(0.45f * kSr / hz);
                n.K = n.K < 1 ? 1 : (n.K > 4 ? 4 : n.K);
                float sum = 0, a = 1;
                for (int k = 0; k < n.K; ++k) {
                    sum += a;
                    a *= n.a;
                }
                n.norm = 1.0f / sum;
            }
            n.amp = 1.0f;
            const float pan = s.r.bi() * spread;
            n.gl = pan > 0 ? 1.0f - pan : 1.0f;
            n.gr = pan < 0 ? 1.0f + pan : 1.0f;
            n.wait = int32_t(kBlock) + s.next;
            if (n.wait < 0 || n.wait >= int32_t(kBlock)) n.wait = 0;
            const int32_t j = int32_t(float(perS) * 0.3f * jit * s.r.bi());
            s.next += perS + j > 32 ? perS + j : 32;
        }
    }
    const float gain = lvl * s.env * 0.6f;
    for (auto& qq : s.pt) {
        if (!qq.on) continue;
        Part q = qq;
        const float* wt = gWin[q.win < 6 ? q.win : 0];
        const float invLen = 256.0f / float(q.len);
        const uint32_t i0 = uint32_t(q.wait);
        int32_t n = int32_t(kBlock - i0);
        if (n > q.len - q.age) n = q.len - q.age;
        float wpos = float(q.age) * invLen;
        const float gl = q.amp * gain * q.gl, gr = q.amp * gain * q.gr;
        for (int32_t j = 0; j < n; ++j) {
            const int wi = int(wpos);
            const float w = wt[wi] + (wt[wi + 1] - wt[wi]) * (wpos - float(wi));
            wpos += invLen;
            float sm;
            if (q.type == 3) sm = partTrain(q);
            else if (q.type == 4 && q.age + j >= q.pulseLen) sm = 0.0f;
            else sm = partOsc(q, s.r);
            q.phase += q.inc;
            if (q.phase >= 1.0f) q.phase -= fflo(q.phase);
            if (q.incMul != 1.0f) {
                q.inc *= q.incMul;
                if (q.inc > 0.45f) q.inc = 0.45f;
            }
            const float v = sm * w;
            oL[i0 + uint32_t(j)] += v * gl;
            oR[i0 + uint32_t(j)] += v * gr;
        }
        q.age += n;
        if (q.age >= q.len) q.on = false;
        q.wait = 0;
        qq = q;
    }
}

}  // namespace graf
