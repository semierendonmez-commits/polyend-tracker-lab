// GRAF -- sound sources: VCO, complex oscillator, wavetable, noise, sampler,
// audio in, MIDI to CV, drum voice and the RETICULUM macro.
//
// Signal conventions: audio +-1, CV +-1 (unipolar 0..1), pitch in octaves
// (1.0 = one octave, 0 = C4 at the oscillators), gates 0 / 1.
#include "core/fastrun.h"
#include "ws/graf_modules.h"

namespace graf {

float gZero[kBlock] = {};
float* gWt = nullptr;

namespace {
inline float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
inline float wrap01(float p) {
    p -= float(int32_t(p));
    return p < 0.0f ? p + 1.0f : p;
}
}  // namespace

// ---------------------------------------------------------------- VCO
// The phase (and its increment) is computed once into scratch arrays, then
// only the patched waveforms are rendered from it.
struct VcoS {
    float ph;
    Schmitt sync;
};
namespace {
float gPh[kBlock], gInc[kBlock];
}
OT_FASTRUN_FN void pVco(Module& m, Proc& p) {
    auto& s = m.s<VcoS>();
    const float base = m.v[0] + m.v[1] * (1.0f / 12.0f);
    const float fmd = m.v[2] * 3.0f, pw0 = m.v[3];
    const float *pit = p.in[0], *fm = p.in[1], *pwm = p.in[2], *sy = p.in[3];
    const bool hasFm = p.has(1), hasPit = p.has(0);
    const bool hasSync = p.has(3), hasPwm = p.has(2);
    float inc = clampf(pitchHz(base + pit[0]) * kInvSr, 0.0f, 0.45f);
    float ph = s.ph;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (hasFm) inc = clampf(pitchHz(base + pit[i] + fm[i] * fmd) * kInvSr, 0.0f, 0.45f);
        else if (hasPit && (i & 7) == 0) inc = clampf(pitchHz(base + pit[i]) * kInvSr, 0.0f, 0.45f);
        if (hasSync && s.sync.rise(sy[i])) ph = 0.0f;
        ph += inc;
        if (ph >= 1.0f) ph -= 1.0f;
        gPh[i] = ph;
        gInc[i] = inc;
    }
    s.ph = ph;
    const uint8_t u = p.used;
    if (u & 1) {
        float* o = m.out[0];
        for (uint32_t i = 0; i < kBlock; ++i) o[i] = 2.0f * gPh[i] - 1.0f - blep(gPh[i], gInc[i]);
    }
    if (u & 2) {
        float* o = m.out[1];
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float pw = hasPwm ? clampf(pw0 + pwm[i] * 0.45f, 0.03f, 0.97f) : pw0;
            const float q = gPh[i];
            float v = q < pw ? 1.0f : -1.0f;
            v += blep(q, gInc[i]);
            float t = q - pw;
            if (t < 0.0f) t += 1.0f;
            o[i] = v - blep(t, gInc[i]);
        }
    }
    if (u & 4) {
        float* o = m.out[2];
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float q = gPh[i];
            o[i] = 4.0f * (q < 0.5f ? q : 1.0f - q) - 1.0f;
        }
    }
    if (u & 8) {
        float* o = m.out[3];
        for (uint32_t i = 0; i < kBlock; ++i) o[i] = sinP(gPh[i]);
    }
}

// ---------------------------------------------------------------- CPLX
// Buchla 259-style: a sine modulator FMs the principal oscillator (through
// zero), the principal's waveshape morphs sine -> triangle-ish -> square by
// Timbre, a sine wavefolder adds harmonics, Order biases odd / even.
struct CplxS {
    float ph, mph;
};
OT_FASTRUN_FN void pCplx(Module& m, Proc& p) {
    auto& s = m.s<CplxS>();
    const float base = m.v[0] + m.v[1] * (1.0f / 12.0f);
    const float ratio = m.v[2], index0 = m.v[3], timbre0 = m.v[4], fold0 = m.v[5], order = m.v[6];
    const float *pit = p.in[0], *fm = p.in[1], *tim = p.in[2], *fol = p.in[3], *idx = p.in[4];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2];
    const bool hasPit = p.has(0);
    float hz = pitchHz(base);
    float inc = hz * kInvSr, minc = inc * ratio;
    float ph = s.ph, mph = s.mph;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (hasPit && (i & 3) == 0) {
            hz = pitchHz(base + pit[i]);
            inc = hz * kInvSr;
            minc = inc * ratio;
        }
        mph += minc;
        if (mph >= 1.0f) mph -= 1.0f;
        const float mod = sinP(mph);
        const float index = index0 + idx[i] * 2.0f;
        ph += inc * (1.0f + (mod * index) + fm[i] * 2.0f);
        ph = wrap01(ph);
        const float sn = sinP(ph);
        // timbre: sine -> clipped (square-ish) by driving a soft clip
        const float tmb = clampf(timbre0 + tim[i], 0.0f, 1.0f);
        float w = soft(sn * (1.0f + tmb * 2.0f));
        // order: add the 2nd harmonic (even) or emphasise the 3rd (odd)
        if (order > 0.0f) w += order * 0.5f * (sn * sn * 2.0f - 1.0f);
        else w += order * 0.5f * (w * w * w - w);
        const float fd = clampf(fold0 + fol[i], 0.0f, 1.0f);
        const float out = sin01(w * (0.25f + fd * 1.25f));
        o0[i] = out;
        o1[i] = sn;
        o2[i] = mod;
    }
    s.ph = ph;
    s.mph = mph;
}

// ---------------------------------------------------------------- WTBL
bool initWavetables(lab::Arena& a) {
    // Built into each engine's arena (one engine on the device; tests make many).
    gWt = static_cast<float*>(a.alloc(sizeof(float) * kWtTables * kWtFrames * (kWtLen + 1)));
    if (!gWt) return false;
    for (int t = 0; t < kWtTables; ++t)
        for (int f = 0; f < kWtFrames; ++f) {
            float* w = gWt + (t * kWtFrames + f) * (kWtLen + 1);
            float peak = 1e-6f;
            for (int i = 0; i < kWtLen; ++i) {
                const float x = float(i) / float(kWtLen);
                float v = 0.0f;
                const float ff = float(f) / float(kWtFrames - 1);
                switch (t) {
                    case 0:   // harmonic sweep: more partials per frame
                        for (int h = 1; h <= 2 + f * 3; ++h) v += sin01(x * float(h)) / float(h) * (h & 1 ? 1.0f : ff);
                        break;
                    case 1: {   // vocal-ish formants
                        const float f1 = 2.0f + ff * 6.0f, f2 = 7.0f + ff * 10.0f;
                        for (int h = 1; h <= 24; ++h) {
                            const float a1 = expf(-(float(h) - f1) * (float(h) - f1) * 0.3f);
                            const float a2 = 0.6f * expf(-(float(h) - f2) * (float(h) - f2) * 0.15f);
                            v += sin01(x * float(h)) * (a1 + a2 + (h == 1 ? 0.4f : 0.0f));
                        }
                        break;
                    }
                    case 2: {   // digital: stepped / bit-reduced sines
                        const float steps = 32.0f - ff * 28.0f;
                        v = floorf(sin01(x + 0.25f * sin01(x * (1.0f + float(f)))) * steps) / steps;
                        break;
                    }
                    default: {   // pulse width sweep, band-limited sum
                        const float pw = 0.5f - ff * 0.45f;
                        for (int h = 1; h <= 20; ++h)
                            v += 2.0f / (float(h) * kPi) * sinf(kPi * float(h) * pw) * sin01(x * float(h) + 0.25f);
                        break;
                    }
                }
                w[i] = v;
                if (fabsf(v) > peak) peak = fabsf(v);
            }
            for (int i = 0; i < kWtLen; ++i) w[i] /= peak;
            w[kWtLen] = w[0];
        }
    return true;
}

struct WtS {
    float ph;
};
OT_FASTRUN_FN void pWtbl(Module& m, Proc& p) {
    auto& s = m.s<WtS>();
    const float base = m.v[0] + m.v[1] * (1.0f / 12.0f);
    const int table = int(m.v[2]) & 3;
    const float pos0 = m.v[3], fmd = m.v[4] * 3.0f;
    const float *pit = p.in[0], *pos = p.in[1], *fm = p.in[2];
    float* o = m.out[0];
    const bool hasFm = p.has(2), hasPit = p.has(0);
    float inc = clampf(pitchHz(base + pit[0]) * kInvSr, 0.0f, 0.45f);
    float ph = s.ph;
    const float* tb = gWt + table * kWtFrames * (kWtLen + 1);
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (hasFm) inc = clampf(pitchHz(base + pit[i] + fm[i] * fmd) * kInvSr, 0.0f, 0.45f);
        else if (hasPit && (i & 7) == 0) inc = clampf(pitchHz(base + pit[i]) * kInvSr, 0.0f, 0.45f);
        ph += inc;
        if (ph >= 1.0f) ph -= 1.0f;
        const float fp = clampf(pos0 + pos[i], 0.0f, 0.9999f) * float(kWtFrames - 1);
        const int f0 = int(fp);
        const float ft = fp - float(f0);
        const float x = ph * float(kWtLen);
        const int i0 = int(x);
        const float xt = x - float(i0);
        const float* a = tb + f0 * (kWtLen + 1);
        const float* b = f0 < kWtFrames - 1 ? a + (kWtLen + 1) : a;
        const float va = a[i0] + (a[i0 + 1] - a[i0]) * xt;
        const float vb = b[i0] + (b[i0 + 1] - b[i0]) * xt;
        o[i] = va + (vb - va) * ft;
    }
    s.ph = ph;
}

// ---------------------------------------------------------------- NOIS
struct NoiseS {
    Rand r;
    float b0, b1, b2, brown;
    float crack;
};
OT_FASTRUN_FN void pNoise(Module& m, Proc& p) {
    auto& s = m.s<NoiseS>();
    if (!s.r.s) s.r.seed(0x1234567u + m.cell * 7919u);
    const float dens = m.v[0], lvl = m.v[1];
    const float cprob = dens * dens * 0.05f;
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    const uint8_t u = p.used;
    if (!(u & 14)) {   // white only
        for (uint32_t i = 0; i < kBlock; ++i) o0[i] = s.r.bi() * lvl;
        return;
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float w = s.r.bi();
        o0[i] = w * lvl;
        // Paul Kellet's economy pink
        s.b0 = 0.99765f * s.b0 + w * 0.0990460f;
        s.b1 = 0.96300f * s.b1 + w * 0.2965164f;
        s.b2 = 0.57000f * s.b2 + w * 1.0526913f;
        o1[i] = (s.b0 + s.b1 + s.b2 + w * 0.1848f) * 0.25f * lvl;
        s.brown = clampf(s.brown * 0.998f + w * 0.04f, -1.0f, 1.0f);
        o2[i] = s.brown * 1.8f * lvl;
        if (s.r.uni() < cprob) s.crack = s.r.bi();
        o3[i] = s.crack * lvl;
        s.crack *= 0.6f;
    }
}

// ---------------------------------------------------------------- SMPL
struct SmplS {
    float pos;
    bool play;
    float env;
    Schmitt trig;
};
void pSmpl(Module& m, Proc& p) {
    auto& s = m.s<SmplS>();
    float *oL = m.out[0], *oR = m.out[1];
    const uint32_t frames = p.eng->sampleFrames[&m - &p.eng->mod(0)];
    const bool stereo = p.eng->sampleStereo[&m - &p.eng->mod(0)];
    if (!m.bank || frames < 4) {
        for (uint32_t i = 0; i < kBlock; ++i) oL[i] = oR[i] = 0.0f;
        return;
    }
    const int16_t* L = m.bank;
    const int16_t* R = stereo ? m.bank + frames : m.bank;
    const float tune = m.v[1] * (1.0f / 12.0f);
    const float len = float(frames - 2);
    const bool loop = m.v[4] > 0.5f, gateMode = m.v[5] > 0.5f;
    const float lvl = m.v[6] * (1.0f / 32768.0f);
    if (s.pos >= len || s.pos < 0.0f) {   // the sample changed under a playing voice
        s.pos = 0.0f;
        s.play = false;
    }
    const float *tr = p.in[0], *pit = p.in[1], *stc = p.in[2];
    const bool hasTrig = p.has(0);
    float rate = fexp2(tune + pit[0]);
    for (uint32_t i = 0; i < kBlock; ++i) {
        if ((i & 7) == 0 && p.has(1)) rate = fexp2(tune + pit[i]);
        const float st = clampf(m.v[2] + stc[i], 0.0f, 0.99f) * len;
        const float en = clampf(st + m.v[3] * len, st + 4.0f, len);
        if (hasTrig && s.trig.rise(tr[i])) {
            s.pos = st;
            s.play = true;
        }
        const bool gateOn = !gateMode || tr[i] > 0.25f;
        const float tgt = (s.play && gateOn) ? 1.0f : 0.0f;
        s.env += (tgt - s.env) * (tgt > s.env ? 0.05f : 0.004f);
        if (!s.play) {
            oL[i] = oR[i] = 0.0f;
            continue;
        }
        const int32_t ip = int32_t(s.pos);
        const float f = s.pos - float(ip);
        const float a = float(L[ip]) + float(L[ip + 1] - L[ip]) * f;
        const float b = float(R[ip]) + float(R[ip + 1] - R[ip]) * f;
        const float g = lvl * s.env;
        oL[i] = a * g;
        oR[i] = b * g;
        s.pos += rate;
        if (s.pos >= en) {
            if (loop) s.pos = st + (s.pos - en);
            else s.play = false;
        }
        if (gateMode && s.env < 0.0005f && !gateOn) s.play = false;
    }
    if (!hasTrig && !s.play && !gateMode) {   // free-running when nothing triggers it
        s.play = loop;
        s.pos = m.v[2] * len;
    }
}

// Built-in sound: an FM bell, 1.5 s.
void makeBuiltinSample(int16_t* dst, uint32_t n, uint32_t* frames) {
    uint32_t len = 66150;
    if (len > n) len = n;
    float ph = 0.0f, mph = 0.0f;
    for (uint32_t i = 0; i < len; ++i) {
        const float t = float(i) * kInvSr;
        const float env = expf(-t * 2.6f) * (t < 0.002f ? t * 500.0f : 1.0f);
        const float ienv = expf(-t * 6.0f);
        mph += 440.0f * 3.5f * kInvSr;
        if (mph >= 1.0f) mph -= 1.0f;
        ph += 440.0f * kInvSr * (1.0f + 2.5f * ienv * sinP(mph));
        ph = wrap01(ph);
        dst[i] = int16_t(sinP(ph) * env * 26000.0f);
    }
    *frames = len;
}

// ---------------------------------------------------------------- AIN
struct AinS {
    float env;
};
void pAudIn(Module& m, Proc& p) {
    auto& s = m.s<AinS>();
    float *oL = m.out[0], *oR = m.out[1], *oE = m.out[2];
    const float g = m.v[0];
    const float rel = coefSec(m.v[1]);
    const lab::AudioIn& in = *p.ain;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float l = in.l(i) * g, r = in.r(i) * g;
        oL[i] = l;
        oR[i] = r;
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        s.env += (a > s.env ? 0.3f : rel) * (a - s.env);
        oE[i] = s.env;
    }
}

// ---------------------------------------------------------------- MIDI
struct MidiS {
    float pitch;
    uint32_t trig;
    int gap;
};
void pMidi(Module& m, Proc& p) {
    auto& s = m.s<MidiS>();
    PatchVoices& pv = p.eng->voices(p.eng->voicePatch(int(&m - &p.eng->mod(0))));
    int vi = int(m.v[0]) - 1;
    if (vi < 0) vi = 0;
    if (vi > 7) vi = 7;
    const Voice& v = pv.v[vi];
    const float target = v.pitch + pv.bend * m.v[3] * (1.0f / 12.0f);
    const float gl = coefSec(m.v[2]);
    if (v.trig != s.trig) {
        s.trig = v.trig;
        s.gap = 64;   // a short low gate so envelopes retrigger
        if (m.v[2] < 0.001f) s.pitch = target;
    }
    const float cc = pv.cc[int(m.v[1]) & 127];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        s.pitch += (target - s.pitch) * gl;
        o0[i] = s.pitch;
        float g = v.gate ? 1.0f : 0.0f;
        if (s.gap > 0) {
            --s.gap;
            g = 0.0f;
        }
        o1[i] = g;
        o2[i] = v.vel;
        o3[i] = cc;
    }
}

// ---------------------------------------------------------------- PERC
struct PercS {
    Rand r;
    Schmitt trig;
    float ph, env, penv, nenv, z1, z2, hp;
    int clapN;
    float clapT;
};
void pPerc(Module& m, Proc& p) {
    auto& s = m.s<PercS>();
    if (!s.r.s) s.r.seed(0xBEEF + m.cell);
    const int type = int(m.v[0]);
    const float tune = fexp2(m.v[1] * (1.0f / 12.0f) + p.in[1][0]);
    const float dk = expf(-6.9f / (m.v[2] * kSr));
    const float tone = m.v[3];
    float* o = m.out[0];
    const float *tr = p.in[0], *acc = p.in[2];
    const float lvl = m.v[4];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.trig.rise(tr[i])) {
            s.env = 1.0f + (p.has(2) ? acc[i] : 0.0f);
            s.penv = 1.0f;
            s.nenv = 1.0f;
            s.clapN = 3;
            s.clapT = 0.0f;
        }
        float y = 0.0f;
        const float n = s.r.bi();
        switch (type) {
            case 0: {   // kick
                const float hz = 45.0f * tune * (1.0f + s.penv * (2.0f + tone * 6.0f));
                s.ph = wrap01(s.ph + hz * kInvSr);
                s.penv *= 0.9965f;
                y = sat(sinP(s.ph) * s.env * (1.0f + tone)) + n * s.nenv * 0.15f;
                s.nenv *= 0.95f;
                break;
            }
            case 1: {   // snare
                s.ph = wrap01(s.ph + 185.0f * tune * kInvSr);
                s.hp = n - s.z1;
                s.z1 = n;
                y = sinP(s.ph) * s.penv * 0.6f + s.hp * s.env * (0.3f + tone * 0.6f);
                s.penv *= 0.9985f;
                break;
            }
            case 2: {   // hat: high-passed noise
                s.z1 += 0.7f * (n - s.z1);
                s.z2 += (0.2f + tone * 0.6f) * (s.z1 - s.z2);
                y = (s.z1 - s.z2) * s.env * 1.4f;
                break;
            }
            case 3: {   // clap: noise bursts through a band
                s.clapT += kInvSr;
                if (s.clapN > 0 && s.clapT > 0.011f) {
                    s.clapT = 0.0f;
                    --s.clapN;
                    s.nenv = 1.0f;
                }
                s.z1 += 0.25f * (n - s.z1);
                s.z2 += 0.12f * (s.z1 - s.z2);
                const float bp = s.z1 - s.z2;
                y = bp * (s.nenv * 2.0f + s.env * (0.6f + tone)) * 1.5f;
                s.nenv *= 0.996f;
                break;
            }
            case 4: {   // tom
                const float hz = 110.0f * tune * (1.0f + s.penv * (0.6f + tone));
                s.ph = wrap01(s.ph + hz * kInvSr);
                s.penv *= 0.9992f;
                y = sinP(s.ph) * s.env;
                break;
            }
            default: {   // zap
                const float hz = 60.0f * tune * (1.0f + s.penv * 40.0f * (0.2f + tone));
                s.ph = wrap01(s.ph + hz * kInvSr);
                s.penv *= 0.997f;
                y = sinP(s.ph) * s.env;
                break;
            }
        }
        s.env *= dk;
        o[i] = y * lvl;
    }
}

// ---------------------------------------------------------------- RETE
// RETICULUM: four resonant band-pass nodes in a recurrent net. Each node
// hears noise, the Excite input and the saturated outputs of all nodes
// through a weight matrix; Gain scales the matrix, Leak sets the resonance,
// Scale the base frequency, Level the output. The modulation (random steps,
// a smooth walk or a drunk walk) moves node gains and frequencies; a gate
// re-rolls the matrix, the node ratios, the pans and the walk targets --
// the Manual Gate of the patch hitting rete RND and the Random Values.
struct ReteS {
    uint8_t rollSeen;
    float ic1[4], ic2[4], y[4];
    float w[4][4];
    float ratio[4], pan[4], gainMod[4], freqMod[4], walkT[4];
    float modPh;
    float env;
    float act[4];
    Rand r;
    Schmitt gate;
};
static void reteRoll(ReteS& s) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) s.w[i][j] = s.r.bi() * (i == j ? 0.3f : 1.0f);
        s.ratio[i] = 1.0f + float(i) * (0.5f + s.r.uni() * 0.9f) + s.r.uni() * 0.3f;
        s.pan[i] = s.r.bi();
        s.walkT[i] = s.r.bi();
    }
}
void rRete(Module& m) {
    auto& s = m.s<ReteS>();
    s.r.seed(0xA5A5u + m.cell * 131u + m.number * 7u);
    reteRoll(s);
}
float reteNode(const Module& m, int i) {
    const ReteS& s = *reinterpret_cast<const ReteS*>(m.st.bytes);
    return clampf(s.act[i & 3], 0.0f, 1.0f);
}
OT_FASTRUN_FN void pRete(Module& m, Proc& p) {
    auto& s = m.s<ReteS>();
    const float *gt = p.in[0], *ex = p.in[1], *scc = p.in[2], *gnc = p.in[3];
    // control rate: modulation and coefficients, once per block
    if (p.has(0))
        for (uint32_t i = 0; i < kBlock; i += 4)
            if (s.gate.rise(gt[i])) reteRoll(s);
    if (m.ext[0] != s.rollSeen) {   // Rec on the module page
        s.rollSeen = m.ext[0];
        reteRoll(s);
    }
    const int mode = int(m.v[5]);
    const float rate = m.v[6], depth = m.v[7];
    s.modPh += rate * float(kBlock) * kInvSr;
    const bool step = s.modPh >= 1.0f;
    if (step) s.modPh -= float(int(s.modPh));
    for (int i = 0; i < 4; ++i) {
        if (mode == 0) {   // random: new value each period
            if (step) s.walkT[i] = s.r.bi();
            s.gainMod[i] += (s.walkT[i] - s.gainMod[i]) * 0.3f;
        } else if (mode == 1) {   // walk: glide towards targets that move each period
            if (step) s.walkT[i] = clampf(s.walkT[i] + s.r.bi() * 0.6f, -1.0f, 1.0f);
            s.gainMod[i] += (s.walkT[i] - s.gainMod[i]) * rate * 0.004f;
        } else {   // drunk: small random steps every block, pulled back
            s.gainMod[i] = clampf(s.gainMod[i] * 0.995f + s.r.bi() * rate * 0.004f, -1.0f, 1.0f);
        }
        s.freqMod[i] = s.gainMod[(i + 1) & 3];
    }
    const float scale = m.v[2] * fexp2(scc[0] * 2.0f);
    const float leak = clampf(m.v[1], 0.0f, 1.0f);
    const float k = 2.0f - 1.97f * leak * leak;   // damping: 2 (dead) .. 0.03 (ringing)
    const float gain = m.v[0] * (1.0f + gnc[0]);
    float g[4], a1[4], a2[4], wl[4], wr[4], ng[4];
    const float spread = m.v[8];
    for (int i = 0; i < 4; ++i) {
        const float hz = clampf(scale * s.ratio[i] * fexp2(s.freqMod[i] * depth), 20.0f, 16000.0f);
        g[i] = tanPi(hz);
        a1[i] = 1.0f / (1.0f + g[i] * (g[i] + k));
        a2[i] = g[i] * a1[i];
        const float pn = clampf(s.pan[i] * spread, -1.0f, 1.0f);
        wl[i] = 0.5f * (1.0f - pn);
        wr[i] = 0.5f * (1.0f + pn);
        ng[i] = clampf(1.0f + s.gainMod[i] * depth, 0.0f, 2.0f) * gain;
    }
    const float noise = m.v[4] * 0.5f, lvl = m.v[3] * 1.5f;
    float *oL = m.out[0], *oR = m.out[1], *oM = m.out[2], *oE = m.out[3];
    float y0 = s.y[0], y1 = s.y[1], y2 = s.y[2], y3 = s.y[3];
    float act0 = 0, act1 = 0, act2 = 0, act3 = 0;
    // weights pre-scaled by node gain
    const float w00 = s.w[0][0] * ng[0], w01 = s.w[0][1] * ng[0], w02 = s.w[0][2] * ng[0], w03 = s.w[0][3] * ng[0];
    const float w10 = s.w[1][0] * ng[1], w11 = s.w[1][1] * ng[1], w12 = s.w[1][2] * ng[1], w13 = s.w[1][3] * ng[1];
    const float w20 = s.w[2][0] * ng[2], w21 = s.w[2][1] * ng[2], w22 = s.w[2][2] * ng[2], w23 = s.w[2][3] * ng[2];
    const float w30 = s.w[3][0] * ng[3], w31 = s.w[3][1] * ng[3], w32 = s.w[3][2] * ng[3], w33 = s.w[3][3] * ng[3];
    float a0c1 = s.ic1[0], a0c2 = s.ic2[0], a1c1 = s.ic1[1], a1c2 = s.ic2[1];
    float a2c1 = s.ic1[2], a2c2 = s.ic2[2], a3c1 = s.ic1[3], a3c2 = s.ic2[3];
    const float A0 = a1[0], A1 = a1[1], A2 = a1[2], A3 = a1[3];
    const float B0 = a2[0], B1 = a2[1], B2 = a2[2], B3 = a2[3];
    const float G0 = g[0], G1 = g[1], G2 = g[2], G3 = g[3];
    const float L0 = wl[0] * lvl, L1 = wl[1] * lvl, L2 = wl[2] * lvl, L3 = wl[3] * lvl;
    const float R0 = wr[0] * lvl, R1 = wr[1] * lvl, R2 = wr[2] * lvl, R3 = wr[3] * lvl;
    uint32_t rs = s.r.s;
    float nprev = 0.0f;
    float env = s.env;
#define RETE_NODE(c1, c2, A, Bc, G, x, y)          \
    {                                               \
        const float v3 = (x) - c2;                  \
        const float v1 = A * c1 + Bc * v3;          \
        const float v2 = c2 + G * v1;               \
        c1 = 2.0f * v1 - c1;                        \
        c2 = 2.0f * v2 - c2;                        \
        y = v1 * k;                                 \
    }
    for (uint32_t n = 0; n < kBlock; ++n) {
        const float sy0 = soft(y0 * 1.5f), sy1 = soft(y1 * 1.5f), sy2 = soft(y2 * 1.5f), sy3 = soft(y3 * 1.5f);
        rs ^= rs << 13;
        rs ^= rs >> 17;
        rs ^= rs << 5;
        const float nz = float(int32_t(rs)) * (noise * (1.0f / 2147483648.0f));
        const float e = ex[n];
        RETE_NODE(a0c1, a0c2, A0, B0, G0, nz + e + w00 * sy0 + w01 * sy1 + w02 * sy2 + w03 * sy3, y0)
        RETE_NODE(a1c1, a1c2, A1, B1, G1, nprev + e + w10 * sy0 + w11 * sy1 + w12 * sy2 + w13 * sy3, y1)
        RETE_NODE(a2c1, a2c2, A2, B2, G2, e - nz + w20 * sy0 + w21 * sy1 + w22 * sy2 + w23 * sy3, y2)
        RETE_NODE(a3c1, a3c2, A3, B3, G3, e - nprev + w30 * sy0 + w31 * sy1 + w32 * sy2 + w33 * sy3, y3)
        nprev = nz;
        act0 += fabsf(y0);
        act1 += fabsf(y1);
        act2 += fabsf(y2);
        act3 += fabsf(y3);
        const float l = soft(y0 * L0 + y1 * L1 + y2 * L2 + y3 * L3);
        const float r = soft(y0 * R0 + y1 * R1 + y2 * R2 + y3 * R3);
        oL[n] = l;
        oR[n] = r;
        oM[n] = 0.5f * (l + r);
        const float a = fabsf(l + r) * 0.5f;
        env += (a > env ? 0.05f : 0.0008f) * (a - env);
        oE[n] = env * 2.0f;
    }
#undef RETE_NODE
    s.r.s = rs ? rs : 1u;
    s.env = env;
    s.ic1[0] = a0c1;
    s.ic2[0] = a0c2;
    s.ic1[1] = a1c1;
    s.ic2[1] = a1c2;
    s.ic1[2] = a2c1;
    s.ic2[2] = a2c2;
    s.ic1[3] = a3c1;
    s.ic2[3] = a3c2;
    const float act[4] = {act0, act1, act2, act3};
    s.y[0] = y0;
    s.y[1] = y1;
    s.y[2] = y2;
    s.y[3] = y3;
    for (int i = 0; i < 4; ++i) {
        s.act[i] = act[i] * (2.0f / float(kBlock));
        if (!(s.ic1[i] == s.ic1[i]) || fabsf(s.ic1[i]) > 1e6f) s.ic1[i] = s.ic2[i] = 0.0f;   // NaN guard
        if (!(s.ic2[i] == s.ic2[i]) || fabsf(s.ic2[i]) > 1e6f) s.ic1[i] = s.ic2[i] = 0.0f;
    }
}

}  // namespace graf
