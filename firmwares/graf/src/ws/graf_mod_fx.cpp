// GRAF -- shapers and effects: ladder VCF, SVF, low-pass gate, wavefolder,
// VCA, mixer, ring mod, bitcrusher, delay, plate reverb, granular, output.
#include "core/fastrun.h"
#include "ws/graf_modules.h"

namespace graf {

namespace {
inline float cutoffHz(float base, float pitch, float mod, float depth) {
    return clampf(base * fexp2(pitch + mod * depth), 16.0f, 19000.0f);
}
// Denormal / NaN guard for filter state.
inline float guard(float x) {
    if (!(x == x)) return 0.0f;
    if (x > 1e4f) return 1e4f;
    if (x < -1e4f) return -1e4f;
    return x;
}
}  // namespace

// ---------------------------------------------------------------- LADR
// Four one-pole TPT stages with a saturated global feedback (a cheap Moog-ish
// ladder): resonance self-oscillates near 1.0, Drive saturates the input.
struct LadS {
    float s1, s2, s3, s4;
};
OT_FASTRUN_FN void pLadder(Module& m, Proc& p) {
    auto& s = m.s<LadS>();
    const float base = m.v[0], res0 = m.v[1], depth = m.v[2], drive = m.v[3];
    const float *in = p.in[0], *fr = p.in[1], *md = p.in[2], *rs = p.in[3];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    const bool mod = p.has(1) || p.has(2);
    float G = 0.0f, g = 0.0f, ig = 1.0f, G4 = 0.0f, G2 = 0.0f, G3 = 0.0f;
    float s1 = s.s1, s2 = s.s2, s3 = s.s3, s4 = s.s4;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (i == 0 || (mod && (i & 3) == 0)) {
            g = tanPi(cutoffHz(base, fr[i], md[i], depth));
            ig = 1.0f / (1.0f + g);
            G = g * ig;
            G2 = G * G;
            G3 = G2 * G;
            G4 = G2 * G2;
        }
        const float k = clampf(res0 + rs[i], 0.0f, 1.1f) * 4.0f;
        // zero-delay feedback estimate from the stage states
        const float S = (G3 * s1 + G2 * s2 + G * s3 + s4) * ig;
        float u = (soft(in[i] * drive) - k * S) / (1.0f + k * G4);
        u = soft(u);
        float v = (u - s1) * G;
        const float y1 = v + s1;
        s1 = y1 + v;
        v = (y1 - s2) * G;
        const float y2 = v + s2;
        s2 = y2 + v;
        v = (y2 - s3) * G;
        const float y3 = v + s3;
        s3 = y3 + v;
        v = (y3 - s4) * G;
        const float y4 = v + s4;
        s4 = y4 + v;
        const float comp = 1.0f + k * 0.25f;   // keep the pass band level with resonance up
        o0[i] = y4 * comp;
        o1[i] = y2 * comp;
        o2[i] = (y2 - y4) * 2.0f * comp;
        o3[i] = (u - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4);
    }
    s.s1 = guard(s1);
    s.s2 = guard(s2);
    s.s3 = guard(s3);
    s.s4 = guard(s4);
}

// ---------------------------------------------------------------- SVF
struct SvfS {
    float ic1, ic2;
};
OT_FASTRUN_FN void pSvf(Module& m, Proc& p) {
    auto& s = m.s<SvfS>();
    const float base = m.v[0], res0 = m.v[1], depth = m.v[2], drive = m.v[3];
    const float *in = p.in[0], *fr = p.in[1], *md = p.in[2], *rs = p.in[3];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    const bool mod = p.has(1) || p.has(2) || p.has(3);
    float g = 0, k = 0, a1 = 0, a2 = 0;
    float ic1 = s.ic1, ic2 = s.ic2;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (i == 0 || (mod && (i & 3) == 0)) {
            g = tanPi(cutoffHz(base, fr[i], md[i], depth));
            k = 2.0f - 1.98f * clampf(res0 + rs[i], 0.0f, 1.0f);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
        }
        const float x = drive > 1.01f ? sat(in[i] * drive) : in[i];
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        o0[i] = v2;
        o1[i] = v1;
        const float hp = x - k * v1 - v2;
        o2[i] = hp;
        o3[i] = hp + v2;
    }
    s.ic1 = guard(ic1);
    s.ic2 = guard(ic2);
}

// ---------------------------------------------------------------- LPG
// Vactrol model: the control (CV + offset, or a ping) charges a "light"
// quickly and it discharges slowly and non-linearly; the light opens a
// 2-pole low-pass and a VCA together (Mode: LP, VCA or both).
struct LpgS {
    float light, ic1, ic2;
    Schmitt ping;
};
OT_FASTRUN_FN void pLpg(Module& m, Proc& p) {
    auto& s = m.s<LpgS>();
    const int mode = int(m.v[0]);
    const float ofs = m.v[1];
    const float dec = coefSec(m.v[2] * 0.35f);
    const float k = 2.0f - 1.6f * m.v[3];
    const float *in = p.in[0], *cv = p.in[1], *pg = p.in[2];
    float* o = m.out[0];
    float g = 0, a1 = 0, a2 = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(2) && s.ping.rise(pg[i])) s.light = 1.0f;
        const float c = clampf(cv[i] + ofs, 0.0f, 1.0f);
        if (c > s.light) s.light += (c - s.light) * 0.02f;
        else s.light += (c - s.light) * dec * (0.4f + s.light);   // slower as it darkens
        if ((i & 3) == 0) {
            const float l = s.light;
            g = tanPi(mode == 1 ? 18000.0f : 40.0f * fexp2(l * 9.0f));
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
        }
        const float v3 = in[i] - s.ic2;
        const float v1 = a1 * s.ic1 + a2 * v3;
        const float v2 = s.ic2 + g * v1;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        const float amp = mode == 0 ? 1.0f : s.light * s.light * (2.0f - s.light);
        o[i] = (mode == 1 ? in[i] : v2) * amp;
    }
    s.ic1 = guard(s.ic1);
    s.ic2 = guard(s.ic2);
}

// ---------------------------------------------------------------- FOLD
void pFold(Module& m, Proc& p) {
    const float f0 = m.v[0], sy0 = m.v[1], lvl = m.v[2];
    float* o = m.out[0];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float amt = clampf(f0 + p.in[1][i], 0.0f, 1.0f);
        const float sym = clampf(sy0 + p.in[2][i], -1.0f, 1.0f);
        const float x = p.in[0][i] * (1.0f + amt * 7.0f) + sym;
        o[i] = sin01(x * 0.25f) * lvl;
    }
}

// ---------------------------------------------------------------- VCA
void pVca(Module& m, Proc& p) {
    const float l1 = m.v[0], l2 = m.v[1];
    const bool ex = m.v[2] > 0.5f;
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2];
    const bool c1 = p.has(1), c2 = p.has(3);
    for (uint32_t i = 0; i < kBlock; ++i) {
        float g1 = c1 ? clampf(p.in[1][i], 0.0f, 2.0f) : 1.0f;
        float g2 = c2 ? clampf(p.in[3][i], 0.0f, 2.0f) : 1.0f;
        if (ex) {
            g1 *= g1;
            g2 *= g2;
        }
        const float a = p.in[0][i] * g1 * l1, b = p.in[2][i] * g2 * l2;
        o0[i] = a;
        o1[i] = b;
        o2[i] = a + b;
    }
}

// ---------------------------------------------------------------- MIX
void pMix(Module& m, Proc& p) {
    const float a = m.v[0], b = m.v[1], c = m.v[2], d = m.v[3], dr = m.v[4];
    float *o0 = m.out[0], *o1 = m.out[1];
    const bool drive = dr > 1.01f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = p.in[0][i] * a + p.in[1][i] * b + p.in[2][i] * c + p.in[3][i] * d;
        if (drive) x = sat(x * dr) / sat(dr);
        o0[i] = x;
        o1[i] = -x;
    }
}

// ---------------------------------------------------------------- RING
void pRing(Module& m, Proc& p) {
    const float mix = m.v[0], bias = m.v[1];
    float *o0 = m.out[0], *o1 = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = p.in[0][i], y = p.in[1][i];
        const float rm = x * (y + bias);
        o0[i] = x + (rm - x) * mix;
        o1[i] = x * (0.5f + 0.5f * y);
    }
}

// ---------------------------------------------------------------- CRSH
struct CrshS {
    float acc, hold;
};
void pCrush(Module& m, Proc& p) {
    auto& s = m.s<CrshS>();
    const float mix = m.v[2];
    float* o = m.out[0];
    const float bits = clampf(m.v[0] + p.in[2][0] * 8.0f, 1.0f, 16.0f);
    const float steps = fexp2(bits - 1.0f), inv = 1.0f / steps;
    const float rate = clampf(m.v[1] * fexp2(p.in[1][0] * 4.0f), 50.0f, 44100.0f) * kInvSr;
    for (uint32_t i = 0; i < kBlock; ++i) {
        s.acc += rate;
        if (s.acc >= 1.0f) {
            s.acc -= float(int32_t(s.acc));
            s.hold = floorf(clampf(p.in[0][i], -1.0f, 1.0f) * steps + 0.5f) * inv;
        }
        o[i] = p.in[0][i] + (s.hold - p.in[0][i]) * mix;
    }
}

// ---------------------------------------------------------------- DLY
// Two 131072-sample lines in the slab (2.97 s each).
struct DlyS {
    uint32_t w;
    float time, lpL, lpR, wowPh, hpL, hpR, lastWow;
};
OT_FASTRUN_FN void pDelay(Module& m, Proc& p) {
    auto& s = m.s<DlyS>();
    constexpr uint32_t kLen = kSlabFloats / 2, kMask = kLen - 1;
    float *oL = m.out[0], *oR = m.out[1];
    if (!m.slab) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            oL[i] = p.in[0][i];
            oR[i] = p.has(1) ? p.in[1][i] : p.in[0][i];
        }
        return;
    }
    float* L = m.slab;
    float* R = m.slab + kLen;
    static const float kSyncB[9] = {0, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f};
    const int sync = int(m.v[1]);
    float tsec = sync ? kSyncB[sync] * 60.0f / (p.eng->tempoBpm > 20.0f ? p.eng->tempoBpm : 120.0f) : m.v[0];
    tsec *= fexp2(p.in[2][0] * 2.0f);
    const float target = clampf(tsec * kSr, 16.0f, float(kLen - 600));
    if (s.time < 1.0f) s.time = target;
    const float fb = clampf(m.v[2] + p.in[3][0], 0.0f, 1.1f);
    const float tone = 0.05f + m.v[3] * 0.9f;
    const float mix = m.v[4];
    const bool pp = m.v[5] > 0.5f;
    const float wow = m.v[6] * 80.0f;
    const bool stIn = p.has(1);
    uint32_t w = s.w;
    s.wowPh += 0.6f * kInvSr * float(kBlock);
    if (s.wowPh >= 1.0f) s.wowPh -= 1.0f;
    const float d0 = s.time + wow * (1.0f + s.lastWow);
    s.time += (target - s.time) * 0.06f;
    s.lastWow = sinP(s.wowPh);
    const float d1 = s.time + wow * (1.0f + s.lastWow);
    const float dd = (d1 - d0) * (1.0f / float(kBlock));
    float d = d0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        d += dd;
        float rp = float(w) - d;
        if (rp < 0.0f) rp += float(kLen);
        const uint32_t i0 = uint32_t(rp) & kMask, i1 = (i0 + 1) & kMask;
        const float f = rp - float(int32_t(rp));
        const float dl = L[i0] + (L[i1] - L[i0]) * f;
        const float dr = R[i0] + (R[i1] - R[i0]) * f;
        s.lpL += (dl - s.lpL) * tone;
        s.lpR += (dr - s.lpR) * tone;
        // a gentle high-pass in the loop keeps the low end from piling up
        s.hpL += (s.lpL - s.hpL) * 0.002f;
        s.hpR += (s.lpR - s.hpR) * 0.002f;
        const float yl = s.lpL - s.hpL, yr = s.lpR - s.hpR;
        const float xl = p.in[0][i], xr = stIn ? p.in[1][i] : xl;
        if (pp) {
            L[w] = soft(0.5f * (xl + xr) + yr * fb);
            R[w] = soft(yl * fb);
        } else {
            L[w] = soft(xl + yl * fb);
            R[w] = soft(xr + yr * fb);
        }
        w = (w + 1) & kMask;
        oL[i] = xl + (dl - xl) * mix;
        oR[i] = xr + (dr - xr) * mix;
    }
    s.w = w;
}

// ---------------------------------------------------------------- VERB
// Dattorro plate (J. Dattorro, "Effect Design Part 1", JAES 1997): input
// band-limit, four input diffusers, a figure-eight tank of two modulated
// all-passes, two delays with damping and two decay diffusers; four output
// taps per side. Delay lengths are the paper's (29761 Hz) scaled to 44.1
// kHz. All lines share one 65536-sample ring with a single decrementing
// write pointer (the Clouds FxEngine addressing): a line of length L at base
// B is written at (p + B) and read `t` samples back at (p + B + t), masked.
namespace {
// The plate runs at half rate (22.05 kHz): input pairs averaged, outputs
// linearly interpolated -- half the CPU, the top octave is damped anyway.
constexpr float kDs = 22050.0f / 29761.0f;
constexpr uint32_t kVMask = 65535;
constexpr int kVd[] = {int(142 * kDs), int(107 * kDs), int(379 * kDs), int(277 * kDs),             // diffusers
                       int(672 * kDs) + 40, int(4453 * kDs), int(1800 * kDs), int(3720 * kDs),      // tank A
                       int(908 * kDs) + 40, int(4217 * kDs), int(2656 * kDs), int(3163 * kDs),      // tank B
                       5513};                                                                       // predelay
constexpr int kVn = 13;
}  // namespace
struct VerbS {
    uint32_t base[kVn];
    uint32_t p;
    float bw, dA, dB, tankA, tankB, lfo, pl, pr;
    bool init;
};
OT_FASTRUN_FN void pVerb(Module& m, Proc& p) {
    auto& s = m.s<VerbS>();
    float *oL = m.out[0], *oR = m.out[1];
    if (!m.slab) {
        for (uint32_t i = 0; i < kBlock; ++i) oL[i] = oR[i] = 0.0f;
        return;
    }
    if (!s.init) {
        uint32_t o = 0;
        for (int i = 0; i < kVn; ++i) {
            s.base[i] = o;
            o += uint32_t(kVd[i]) + 1;
        }
        s.init = true;
    }
    float* M = m.slab;
    const uint32_t* B = s.base;
    uint32_t P = s.p;
#define RD(d, t) M[(P + B[d] + uint32_t(t)) & kVMask]
#define WR(d, x) M[(P + B[d]) & kVMask] = (x)
    const float decay = m.v[0];
    const float damp = 1.0f - m.v[1];
    const uint32_t pre = uint32_t(clampf(m.v[2], 0.0f, 0.24f) * (kSr * 0.5f)) + 1;
    const float mix = clampf(m.v[3] + p.in[2][0], 0.0f, 1.0f);
    const float diff = m.v[4];
    const float modd = m.v[5] * 12.0f;
    const float id1 = 0.75f * diff, id2 = 0.625f * diff;
    const float dd1 = 0.7f * diff, dd2 = clampf(decay + 0.15f, 0.25f, 0.5f);
    const bool st = p.has(1);
    // LFO once per block (the tap moves slowly)
    s.lfo += 0.9f * kInvSr * float(kBlock);
    if (s.lfo >= 1.0f) s.lfo -= 1.0f;
    const float mo = sinP(s.lfo) * modd * 0.5f;
    const float rpa = float(kVd[4]) - 20.0f - mo, rpb = float(kVd[8]) - 20.0f + mo;
    const uint32_t ta = uint32_t(rpa), tb = uint32_t(rpb);
    const float fa = rpa - float(ta), fb = rpb - float(tb);
    float tankA = s.tankA, tankB = s.tankB, dA = s.dA, dB = s.dB, bw = s.bw;
    const float* inL = p.in[0];
    const float* inR = st ? p.in[1] : p.in[0];
    float pl = s.pl, pr = s.pr;
    for (uint32_t i = 0; i < kBlock; i += 2) {
        WR(12, (inL[i] + inR[i] + inL[i + 1] + inR[i + 1]) * 0.25f);
        float x = RD(12, pre);
        bw += (x - bw) * 0.75f;
        x = bw;
        // input diffusers: all-pass y = z + g v, v = x - g z (z = line end)
        for (int d = 0; d < 4; ++d) {
            const float g = d < 2 ? id1 : id2;
            const float z = RD(d, kVd[d]);
            const float v = x - g * z;
            WR(d, v);
            x = z + g * v;
        }
        // tank A
        {
            const float z = RD(4, ta) + (RD(4, ta + 1) - RD(4, ta)) * fa;
            const float v = x + tankB * decay + dd1 * z;
            WR(4, v);
            WR(5, z - dd1 * v);
            dA += (RD(5, kVd[5]) - dA) * damp;
            const float z2 = RD(6, kVd[6]);
            const float v2 = dA * decay - dd2 * z2;
            WR(6, v2);
            WR(7, z2 + dd2 * v2);
            tankA = RD(7, kVd[7]);
        }
        // tank B
        {
            const float z = RD(8, tb) + (RD(8, tb + 1) - RD(8, tb)) * fb;
            const float v = x + tankA * decay + dd1 * z;
            WR(8, v);
            WR(9, z - dd1 * v);
            dB += (RD(9, kVd[9]) - dB) * damp;
            const float z2 = RD(10, kVd[10]);
            const float v2 = dB * decay - dd2 * z2;
            WR(10, v2);
            WR(11, z2 + dd2 * v2);
            tankB = RD(11, kVd[11]);
        }
        const float yl = 0.5f * (RD(9, int(266 * kDs)) + RD(9, int(2974 * kDs)) - RD(10, int(1913 * kDs)) +
                                 RD(11, int(1996 * kDs)) - RD(5, int(1990 * kDs)));
        const float yr = 0.5f * (RD(5, int(353 * kDs)) + RD(5, int(3627 * kDs)) - RD(6, int(1228 * kDs)) +
                                 RD(7, int(2673 * kDs)) - RD(9, int(2111 * kDs)));
        const float dry = 1.0f - mix;
        oL[i] = inL[i] * dry + (pl + yl) * 0.5f * mix;
        oR[i] = inR[i] * dry + (pr + yr) * 0.5f * mix;
        oL[i + 1] = inL[i + 1] * dry + yl * mix;
        oR[i + 1] = inR[i + 1] * dry + yr * mix;
        pl = yl;
        pr = yr;
        P = (P - 1) & kVMask;
    }
    s.pl = guard(pl);
    s.pr = guard(pr);
#undef RD
#undef WR
    s.p = P;
    s.tankA = guard(tankA);
    s.tankB = guard(tankB);
    s.dA = guard(dA);
    s.dB = guard(dB);
    s.bw = guard(bw);
}

// ---------------------------------------------------------------- GRN
// Beads-like: a 2.97 s stereo record buffer (frozen with Freeze), up to 12
// grains with a raised-cosine window; grains start on Trig or at Density per
// second, at Position (+ spread jitter) behind the write head, pitched.
struct Grain {
    float pos, inc, ph, dph, panL, panR;
    bool on;
};
struct GrnS {
    uint32_t w;
    float next;
    Grain g[8];
    Schmitt trig;
    float fbL, fbR;
};
OT_FASTRUN_FN void pGrain(Module& m, Proc& p) {
    auto& s = m.s<GrnS>();
    constexpr uint32_t kLen = kSlabFloats / 2, kMask = kLen - 1;
    float *oL = m.out[0], *oR = m.out[1];
    if (!m.slab) {
        for (uint32_t i = 0; i < kBlock; ++i) oL[i] = oR[i] = 0.0f;
        return;
    }
    float* L = m.slab;
    float* R = m.slab + kLen;
    const float size = clampf(m.v[0] * fexp2(p.in[4][0] * 2.0f), 0.005f, 1.0f);
    const float dens = m.v[1];
    const float posK = clampf(m.v[2] + p.in[3][0], 0.0f, 1.0f);
    const float pitch = m.v[3] * (1.0f / 12.0f) + p.in[5][0];
    const float spread = m.v[4], fbk = m.v[5];
    const bool freeze = m.v[6] > 0.5f;
    const float mix = m.v[7], width = m.v[8];
    const bool st = p.has(1);
    const bool trigIn = p.has(2);
    const float glen = size * kSr;
    Rand& r = p.eng->rng;
    uint32_t w = s.w;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float xl = p.in[0][i], xr = st ? p.in[1][i] : xl;
        if (!freeze) {
            L[w] = xl + s.fbL * fbk;
            R[w] = xr + s.fbR * fbk;
            w = (w + 1) & kMask;
        }
        bool spawn = false;
        if (trigIn) {
            spawn = s.trig.rise(p.in[2][i]);
        } else {
            s.next -= dens * kInvSr;
            if (s.next <= 0.0f) {
                s.next += 1.0f + (r.uni() - 0.5f) * spread;
                spawn = true;
            }
        }
        if (spawn)
            for (int k = 0; k < 8; ++k)
                if (!s.g[k].on) {
                    Grain& g = s.g[k];
                    const float jit = r.uni() * spread * 0.3f;
                    const float rate = fexp2(pitch + r.bi() * spread * 0.05f);
                    float back = (posK + jit) * float(kLen - 4096) + glen * (rate > 1.0f ? rate : 1.0f) + 64.0f;
                    if (back > float(kLen - 64)) back = float(kLen - 64);
                    float sp = float(w) - back;
                    if (sp < 0.0f) sp += float(kLen);
                    g.pos = sp;
                    g.inc = rate;
                    g.ph = 0.0f;
                    g.dph = 1.0f / (glen > 64.0f ? glen : 64.0f);
                    const float pan = r.bi() * width;
                    g.panL = 0.7f * (1.0f - pan);
                    g.panR = 0.7f * (1.0f + pan);
                    g.on = true;
                    break;
                }
        float yl = 0.0f, yr = 0.0f;
        for (int k = 0; k < 8; ++k) {
            Grain& g = s.g[k];
            if (!g.on) continue;
            const uint32_t i0 = uint32_t(g.pos) & kMask, i1 = (i0 + 1) & kMask;
            const float f = g.pos - float(int32_t(g.pos));
            const float a = L[i0] + (L[i1] - L[i0]) * f;
            const float b = R[i0] + (R[i1] - R[i0]) * f;
            const float win = 0.5f - 0.5f * sinP(g.ph < 0.75f ? g.ph + 0.25f : g.ph - 0.75f);   // 0.5 - 0.5 cos
            yl += a * win * g.panL;
            yr += b * win * g.panR;
            g.pos += g.inc;
            if (g.pos >= float(kLen)) g.pos -= float(kLen);
            g.ph += g.dph;
            if (g.ph >= 1.0f) g.on = false;
        }
        s.fbL = sat(yl);
        s.fbR = sat(yr);
        oL[i] = xl + (yl - xl) * mix;
        oR[i] = xr + (yr - xr) * mix;
    }
    s.w = w;
}

// ---------------------------------------------------------------- OUT
// Sums into its patch's strip; the engine applies strip level / pan / mute,
// the master and the stems.
OT_FASTRUN_FN void pOut(Module& m, Proc& p) {
    Engine& e = *p.eng;
    const int slot = int(&m - &e.mod(0));
    const int patch = e.voicePatch(slot);
    const MixStrip& ms = e.mix[e.rack ? 0 : patch];
    const float pan = clampf(m.v[1], -1.0f, 1.0f);
    float gl = m.v[0] * (pan > 0.0f ? 1.0f - pan : 1.0f);
    float gr = m.v[0] * (pan < 0.0f ? 1.0f + pan : 1.0f);
    if (!e.rack) {
        const float sp = ms.pan;
        gl *= ms.level * (sp > 0.0f ? 1.0f - sp : 1.0f);
        gr *= ms.level * (sp < 0.0f ? 1.0f + sp : 1.0f);
        if (ms.mute) gl = gr = 0.0f;
    }
    int stem = int(m.v[2]);   // 0 auto, 1 off, 2..5 = stems 1..4
    stem = stem == 0 ? (e.rack ? (m.number - 1) & 3 : patch & 3) : (stem == 1 ? -1 : stem - 2);
    const bool st = p.has(1), lv = p.has(2);
    float* sl = stem >= 0 ? e.stemL[stem] : nullptr;
    float* sr = stem >= 0 ? e.stemR[stem] : nullptr;
    if (stem >= 0) e.stemUsed[stem] = true;
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float g = lv ? clampf(p.in[2][i], 0.0f, 1.0f) : 1.0f;
        const float l = p.in[0][i] * gl * g;
        const float r = (st ? p.in[1][i] : p.in[0][i]) * gr * g;
        e.mixL[i] += l;
        e.mixR[i] += r;
        if (sl) {
            sl[i] += l;
            sr[i] += r;
        }
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (a > pk) pk = a;
    }
    float& mt = e.meter[e.rack ? (m.number - 1) & 7 : patch];
    mt = pk > mt ? pk : mt;
}

}  // namespace graf
