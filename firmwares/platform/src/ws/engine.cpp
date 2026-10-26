// PLATFORM -- native voice engine (engine.h).
#include "ws/engine.h"

#include <cmath>
#include <cstring>

#include "core/fastrun.h"
#include "lab/dsp.h"

namespace plat {

using lab::kBlock;
constexpr int kCtl = 64;   // control-rate segment (envelope / pitch / filter updates)

const char* const kParamNames[pParamCount] = {
    "wave", "amp", "pan", "pw", "fm_ratio", "fm_index", "cutoff", "res", "ftype",
    "attack", "decay", "sustain", "release", "env_mode", "pitch_env", "pitch_decay",
    "sample", "rate", "start", "length", "loop",
    "grain_size", "grain_density", "grain_pos", "grain_spread",
    "send_delay", "send_reverb", "glide", "detune", "drive", "fenv",
};
const char* const kFxNames[fFxCount] = {
    "delay_time", "delay_feedback", "delay_level", "delay_tone", "reverb_size", "reverb_damp", "reverb_level", "level",
};
const char* const kWaveNames[kWaveCount] = {"sine", "tri", "saw", "square", "noise", "fm", "sample", "grain", "click"};

namespace {

// Parameter defaults and ranges.
struct Range {
    float def, lo, hi;
};
const Range kRange[pParamCount] = {
    {0, 0, kWaveCount - 1},  // wave
    {0.5f, 0, 2},            // amp
    {0, -1, 1},              // pan
    {0.5f, 0.02f, 0.98f},    // pw
    {2, 0.0625f, 16},        // fm_ratio
    {1, 0, 16},              // fm_index
    {18000, 20, 18000},      // cutoff
    {0.7f, 0.5f, 20},        // res (Q)
    {0, 0, 3},               // ftype: lp bp hp off
    {0.005f, 0.0005f, 20},   // attack
    {0.2f, 0.001f, 20},      // decay
    {0.7f, 0, 1},            // sustain
    {0.3f, 0.001f, 30},      // release
    {0, 0, 1},               // env_mode: adsr perc
    {0, -48, 48},            // pitch_env (semitones)
    {0.05f, 0.001f, 10},     // pitch_decay
    {0, 0, kSlots - 1},      // sample slot
    {1, -8, 8},              // rate
    {0, 0, 1},               // start
    {1, 0, 1},               // length
    {0, 0, 1},               // loop
    {0.08f, 0.005f, 1},      // grain_size (s)
    {20, 0.5f, 200},         // grain_density (grains / s)
    {0, 0, 1},               // grain_pos
    {0.1f, 0, 1},            // grain_spread
    {0, 0, 1},               // send_delay
    {0, 0, 1},               // send_reverb
    {0, 0, 10},              // glide (s)
    {0, -1200, 1200},        // detune (cents)
    {0, 0, 1},               // drive
    {0, -8, 8},              // fenv (octaves at full envelope)
};
const Range kFxRange[fFxCount] = {
    {0.375f, 0.001f, 2}, {0.4f, 0, 0.98f}, {0.3f, 0, 1}, {0.6f, 0, 1}, {0.7f, 0, 1}, {0.4f, 0, 1}, {0.3f, 0, 1}, {1, 0, 2},
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool eqName(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (x == '-') x = '_';
        if (x != y) return false;
        if (!x) return true;
    }
}

constexpr int kSinN = 1024;
float gSin[kSinN + 1];
bool gSinReady = false;
void initSin() {
    if (gSinReady) return;
    for (int i = 0; i <= kSinN; ++i) gSin[i] = sinf(6.28318531f * float(i) / float(kSinN));
    gSinReady = true;
}

inline float sinTab(float ph) {   // ph in [0, 1)
    const float x = ph * float(kSinN);
    const int i = int(x);
    const float f = x - float(i);
    const float a = gSin[i & (kSinN - 1)];
    return a + (gSin[(i & (kSinN - 1)) + 1] - a) * f;
}

inline float polyBlep(float t, float dt) {
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

inline uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
inline float noise(uint32_t& s) { return float(int32_t(xorshift(s))) * (1.0f / 2147483648.0f); }

// Control-rate approximations (no libm in the voice path):
// 2^x, relative error < 1e-4 for |x| < 30; tan(x) for 0 <= x < 1.29 (Pade), < 5e-5.
inline float fexp2(float x) {
    if (x < -30.0f) x = -30.0f;
    if (x > 30.0f) x = 30.0f;
    const float fl = floorf(x);
    const float f = x - fl;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    int32_t i;
    std::memcpy(&i, &p, 4);
    i += int32_t(fl) * (1 << 23);
    std::memcpy(&p, &i, 4);
    return p;
}
inline float fexp(float x) { return fexp2(x * 1.44269504f); }
inline float ftan(float x) {
    const float x2 = x * x;
    return x * (135135.0f + x2 * (-17325.0f + x2 * 378.0f)) / (135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2)));
}

// Envelope: advance `n` samples from (stage, env); returns the new level.
// Stages: 0 idle, 1 attack, 2 decay, 3 sustain, 4 release.
float advanceEnv(uint8_t& st, float e, const float* p, int n) {
    const float sr = lab::dsp::kSr;
    float left = float(n);
    while (left > 0.0f) {
        switch (st) {
            case 1: {
                const float rate = 1.0f / (p[pAttack] * sr);
                const float need = (1.0f - e) / rate;
                if (need > left) return e + rate * left;
                left -= need;
                e = 1.0f;
                st = 2;
                break;
            }
            case 2: {
                const bool perc = p[pEnvMode] >= 0.5f;
                const float floor = perc ? 0.0f : p[pSustain];
                // exponential-ish decay toward the floor: -60 dB over `decay`
                const float k = fexp(-6.9f * left / (p[pDecay] * sr));
                const float ne = floor + (e - floor) * k;
                if (perc && ne < 0.0005f) {
                    st = 0;
                    return 0.0f;
                }
                if (!perc && ne - floor < 0.0005f) {
                    st = 3;
                    return floor;
                }
                return ne;
            }
            case 3: return p[pSustain];
            case 4: {
                const float k = fexp(-6.9f * left / (p[pRelease] * sr));
                const float ne = e * k;
                if (ne < 0.0005f) {
                    st = 0;
                    return 0.0f;
                }
                return ne;
            }
            default: return 0.0f;
        }
    }
    return e;
}

}  // namespace

float fastSin(float phase01) {
    phase01 -= floorf(phase01);
    return sinTab(phase01);
}

int paramIndex(const char* name) {
    for (int i = 0; i < pParamCount; ++i)
        if (eqName(name, kParamNames[i])) return i;
    return -1;
}
int fxIndex(const char* name) {
    for (int i = 0; i < fFxCount; ++i)
        if (eqName(name, kFxNames[i])) return i;
    return -1;
}
int waveIndex(const char* name) {
    for (int i = 0; i < kWaveCount; ++i)
        if (eqName(name, kWaveNames[i])) return i;
    return -1;
}

bool Engine::init(lab::Arena& arena) {
    initSin();
    ring_ = arena.make<lab::SpscRing<Cmd, 512>>();
    voices_ = arena.make<Voice>(kVoices);
    pending_ = arena.make<Cmd>(kPending);
    if (!ring_ || !voices_ || !pending_) return false;
    for (int v = 0; v < kVoices; ++v) {
        for (int p = 0; p < pParamCount; ++p) voices_[v].p[p] = kRange[p].def;
        voices_[v].rng = 0x9E3779B9u * uint32_t(v + 1);
    }
    for (int f = 0; f < fFxCount; ++f) fx_[f] = kFxRange[f].def;
    dlyLen_ = uint32_t(2.05f * lab::dsp::kSr);
    dlyL_ = static_cast<float*>(arena.alloc(dlyLen_ * 4));
    dlyR_ = static_cast<float*>(arena.alloc(dlyLen_ * 4));
    if (!dlyL_ || !dlyR_) return false;
    // half-rate lengths (Freeverb's at 22.05 kHz) and per-side allpasses
    static const uint32_t kComb[kCombs] = {557, 593, 641, 677};
    static const uint32_t kAp[2][kAps] = {{277, 191}, {293, 173}};
    for (int c = 0; c < kCombs; ++c) {
        combLen_[c] = kComb[c];
        comb_[c] = static_cast<float*>(arena.alloc(combLen_[c] * 4));
        if (!comb_[c]) return false;
    }
    for (int s = 0; s < 2; ++s)
        for (int a = 0; a < kAps; ++a) {
            apLen_[s][a] = kAp[s][a];
            ap_[s][a] = static_cast<float*>(arena.alloc(apLen_[s][a] * 4));
            if (!ap_[s][a]) return false;
        }
    return true;
}

// ---- main loop side ------------------------------------------------------------
bool Engine::push(const Cmd& c0) {
    Cmd c = c0;
    c.epoch = stampEpoch_;
    if (!ring_ || !ring_->push(c)) {
        ++dropped_;
        return false;
    }
    return true;
}

void Engine::noteOn(int voice, float hz, float vel, double when, bool freq) {
    Cmd c;
    c.op = freq ? Cmd::kOnFreq : Cmd::kOn;
    c.a = uint8_t(voice);
    c.v = hz;
    c.frames = uint32_t(clampf(vel, 0.0f, 1.0f) * 65535.0f);
    c.when = when;
    push(c);
}
void Engine::noteOff(int voice, double when) {
    Cmd c;
    c.op = Cmd::kOff;
    c.a = uint8_t(voice);
    c.when = when;
    push(c);
}
void Engine::set(int voice, int param, float v, double when) {
    Cmd c;
    c.op = Cmd::kSet;
    c.a = uint8_t(voice);
    c.b = uint8_t(param);
    c.v = v;
    c.when = when;
    push(c);
}
void Engine::fx(int param, float v, double when) {
    Cmd c;
    c.op = Cmd::kFx;
    c.b = uint8_t(param);
    c.v = v;
    c.when = when;
    push(c);
}
void Engine::panic() {
    Cmd c;
    c.op = Cmd::kPanic;
    push(c);
}
void Engine::requestReset() { resetReq_ = resetReq_ + 1; }
void Engine::publishSlot(int slot, const int16_t* data, uint32_t frames, int channels) {
    Cmd c;
    c.op = Cmd::kSlot;
    c.a = uint8_t(slot);
    c.b = uint8_t(channels);
    c.ptr = data;
    c.frames = frames;
    push(c);
}

void Engine::clock(double& beat, double& bps, uint8_t& epoch, uint32_t& blocks) const {
    for (int tries = 0; tries < 100; ++tries) {
        const uint32_t s0 = seq_;
        if (s0 & 1u) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        beat = pubBeat_;
        bps = pubBps_;
        epoch = pubEpoch_;
        blocks = pubBlocks_;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq_ == s0) return;
    }
}

float Engine::paramValue(int voice, int p) const {
    if (voice < 0 || voice >= kVoices || p < 0 || p >= pParamCount) return 0.0f;
    return voices_[voice].p[p];
}

// ---- audio side ------------------------------------------------------------------
void Engine::applyNow(const Cmd& c) { apply(c); }

void Engine::trigger(Voice& v, float hz, float vel, bool freq) {
    const float f = freq ? hz : lab::dsp::mtof(hz);
    const bool legato = v.p[pGlide] > 0.0f && v.active;
    v.hzTarget = clampf(f, 0.5f, 20000.0f);
    if (!legato) v.hz = v.hzTarget;
    v.vel = vel;
    v.gate = true;
    v.active = true;
    v.stage = 1;
    if (!legato) {
        v.env = 0.0f;
        // the shortest attack starts at full level: percussion hits land on their sample
        if (v.p[pAttack] <= 0.0006f) {
            v.env = 1.0f;
            v.stage = 2;
        }
        v.phase = 0.0f;
        v.mphase = 0.0f;
    }
    v.pitchEnv = v.p[pPitchEnv];
    v.clickEnv = 1.0f;
    v.grainTimer = 0.0f;
    const SampleSlot& s = slots_[int(v.p[pSample]) & (kSlots - 1)];
    if (s.data && s.frames) {
        const float st = v.p[pRate] >= 0.0f ? v.p[pStart] : v.p[pStart] + v.p[pLength];
        v.sidx = uint32_t(clampf(st, 0.0f, 0.9999f) * float(s.frames));
        v.sfrac = 0.0f;
    }
    if (int(v.p[pWave]) == kClick) v.ic1 = v.ic2 = v.ic1r = v.ic2r = 0.0f;
}

void Engine::apply(const Cmd& c) {
    switch (c.op) {
        case Cmd::kOn:
        case Cmd::kOnFreq:
            if (c.a < kVoices) trigger(voices_[c.a], c.v, float(c.frames) * (1.0f / 65535.0f), c.op == Cmd::kOnFreq);
            break;
        case Cmd::kOff:
            if (c.a < kVoices) {
                Voice& v = voices_[c.a];
                v.gate = false;
                if (v.stage && v.p[pEnvMode] < 0.5f) v.stage = 4;
            }
            break;
        case Cmd::kSet:
            if (c.a < kVoices && c.b < pParamCount) voices_[c.a].p[c.b] = clampf(c.v, kRange[c.b].lo, kRange[c.b].hi);
            break;
        case Cmd::kFx:
            if (c.b < fFxCount) fx_[c.b] = clampf(c.v, kFxRange[c.b].lo, kFxRange[c.b].hi);
            break;
        case Cmd::kSlot:
            if (c.a < kSlots) {
                // voices reading the old data stop first
                for (int i = 0; i < kVoices; ++i) {
                    Voice& v = voices_[i];
                    const int w = int(v.p[pWave]);
                    if (v.active && (w == kSample || w == kGrain) && (int(v.p[pSample]) & (kSlots - 1)) == c.a) {
                        v.active = false;
                        v.stage = 0;
                        v.env = 0.0f;
                    }
                }
                slots_[c.a].data = c.ptr;
                slots_[c.a].frames = c.frames;
                slots_[c.a].channels = c.b;
            }
            break;
        case Cmd::kPanic:
            for (int i = 0; i < kVoices; ++i) {
                voices_[i].active = false;
                voices_[i].gate = false;
                voices_[i].stage = 0;
                voices_[i].env = 0.0f;
            }
            break;
        default: break;
    }
}

namespace {

struct Mix {
    float g, k, a1;            // SVF
    float e, de;               // envelope ramp
    float gl, gr, sd, sv;      // pan gains, sends
    float pre;                 // drive gain
};

// filter (FT: 0 lp, 1 bp, 2 hp, 3 none) + envelope + pan + sends, mono source.
template <int FT, bool SENDS>
inline void mixMono(const float* __restrict buf, int n, float& ic1, float& ic2, Mix& m, float* __restrict L,
                    float* __restrict R, float* __restrict D, float* __restrict V) {
    const float g = m.g, k = m.k, a1 = m.a1, ga1 = m.g * m.a1, de = m.de, gl = m.gl, gr = m.gr, sd = m.sd, sv = m.sv;
    float e = m.e, s1 = ic1, s2 = ic2;
    for (int i = 0; i < n; ++i) {
        float x = buf[i];
        if (FT != 3) {
            const float v1 = a1 * s1 + ga1 * (x - s2);
            const float v2 = s2 + g * v1;
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;
            x = FT == 0 ? v2 : (FT == 1 ? v1 : x - k * v1 - v2);
        }
        x *= e;
        e += de;
        L[i] += x * gl;
        R[i] += x * gr;
        if (SENDS) {
            D[i] += x * sd;
            V[i] += x * sv;
        }
    }
    ic1 = s1;
    ic2 = s2;
    m.e = e;
}

template <int FT>
inline void filterInPlace(float* __restrict buf, int n, float& ic1, float& ic2, const Mix& m) {
    const float g = m.g, k = m.k, a1 = m.a1, ga1 = m.g * m.a1;
    float s1 = ic1, s2 = ic2;
    for (int i = 0; i < n; ++i) {
        const float x = buf[i];
        const float v1 = a1 * s1 + ga1 * (x - s2);
        const float v2 = s2 + g * v1;
        s1 = 2.0f * v1 - s1;
        s2 = 2.0f * v2 - s2;
        buf[i] = FT == 0 ? v2 : (FT == 1 ? v1 : x - k * v1 - v2);
    }
    ic1 = s1;
    ic2 = s2;
}

void filterAny(int ft, float* buf, int n, float& ic1, float& ic2, const Mix& m) {
    switch (ft) {
        case 0: filterInPlace<0>(buf, n, ic1, ic2, m); break;
        case 1: filterInPlace<1>(buf, n, ic1, ic2, m); break;
        case 2: filterInPlace<2>(buf, n, ic1, ic2, m); break;
        default: break;
    }
}

void driveInPlace(float* buf, int n, float pre) {
    for (int i = 0; i < n; ++i) {
        float x = buf[i] * pre;
        x = x > 3.0f ? 3.0f : (x < -3.0f ? -3.0f : x);
        const float x2 = x * x;
        buf[i] = x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }
}

template <bool SENDS>
void mixAny(int ft, const float* buf, int n, float& ic1, float& ic2, Mix& m, float* L, float* R, float* D, float* V) {
    switch (ft) {
        case 0: mixMono<0, SENDS>(buf, n, ic1, ic2, m, L, R, D, V); break;
        case 1: mixMono<1, SENDS>(buf, n, ic1, ic2, m, L, R, D, V); break;
        case 2: mixMono<2, SENDS>(buf, n, ic1, ic2, m, L, R, D, V); break;
        default: mixMono<3, SENDS>(buf, n, ic1, ic2, m, L, R, D, V); break;
    }
}

}  // namespace

OT_FASTRUN_FN void Engine::renderVoice(Voice& v, int from, int to) {
    const int n = to - from;
    if (n <= 0) return;
    const float* p = v.p;
    const int wave = int(p[pWave]);
    // ---- control rate: envelope ramp, pitch, filter, pan ----
    const float e0 = v.env;
    const float e1 = advanceEnv(v.stage, e0, p, n);
    v.env = e1;
    if (p[pGlide] > 0.0f && v.hz != v.hzTarget) {
        v.hz += (v.hzTarget - v.hz) * (1.0f - fexp(-float(n) / (p[pGlide] * lab::dsp::kSr)));
        if (fabsf(v.hz - v.hzTarget) < 0.01f) v.hz = v.hzTarget;
    } else {
        v.hz = v.hzTarget;
    }
    if (v.pitchEnv != 0.0f) {
        v.pitchEnv *= fexp(-6.9f * float(n) / (p[pPitchDecay] * lab::dsp::kSr));
        if (fabsf(v.pitchEnv) < 0.01f) v.pitchEnv = 0.0f;
    }
    const float semis = v.pitchEnv + p[pDetune] * 0.01f;
    const float hz = semis != 0.0f ? v.hz * fexp2(semis * (1.0f / 12.0f)) : v.hz;
    const float inc = clampf(hz * lab::dsp::kInvSr, 0.0f, 0.49f);
    int ftype = int(p[pFtype]);
    float cut = p[pCutoff];
    if (p[pFenv] != 0.0f) cut *= fexp2(p[pFenv] * e1);
    cut = clampf(cut, 20.0f, 18000.0f);
    if (ftype == 0 && cut >= 17999.0f) ftype = 3;
    Mix m;
    if (ftype != 3) {
        if (cut != v.cutCached || p[pRes] != v.resCached) {
            v.cutCached = cut;
            v.resCached = p[pRes];
            v.g = ftan(lab::dsp::kPi * cut * lab::dsp::kInvSr);
            v.k = 1.0f / p[pRes];
        }
        m.g = v.g;
        m.k = v.k;
        m.a1 = 1.0f / (1.0f + v.g * (v.g + v.k));
    } else {
        m.g = m.k = m.a1 = 0.0f;
    }
    const float pan = (p[pPan] + 1.0f) * 0.125f;   // 0..0.25 of a turn: equal power
    const float amp = p[pAmp] * v.vel;
    m.gl = sinTab(pan + 0.25f) * amp;
    m.gr = sinTab(pan) * amp;
    m.sd = p[pSendDelay];
    m.sv = p[pSendReverb];
    m.e = e0;
    m.de = (e1 - e0) / float(n);
    m.pre = 1.0f + p[pDrive] * 8.0f;
    const bool drive = p[pDrive] > 0.0f;
    const bool sends = m.sd > 0.0f || m.sv > 0.0f;
    if (sends) sendsUsed_ = true;

    // ---- oscillator ----
    float buf[kBlock];
    float bufR[kBlock];
    bool stereo = false;
    float ph = v.phase;
    switch (wave) {
        case kSine:
            for (int i = 0; i < n; ++i) {
                buf[i] = sinTab(ph);
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        case kTri:
            for (int i = 0; i < n; ++i) {
                buf[i] = 4.0f * (ph < 0.5f ? ph : 1.0f - ph) - 1.0f;
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        case kSaw: {
            const float idt = 1.0f / (inc > 1e-6f ? inc : 1e-6f);
            for (int i = 0; i < n; ++i) {
                float s0 = 2.0f * ph - 1.0f;
                if (ph < inc) {
                    const float t = ph * idt;
                    s0 -= t + t - t * t - 1.0f;
                } else if (ph > 1.0f - inc) {
                    const float t = (ph - 1.0f) * idt;
                    s0 -= t * t + t + t + 1.0f;
                }
                buf[i] = s0;
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        }
        case kSquare: {
            const float pw = p[pPw];
            const float idt = 1.0f / (inc > 1e-6f ? inc : 1e-6f);
            for (int i = 0; i < n; ++i) {
                float s0 = ph < pw ? 1.0f : -1.0f;
                if (ph < inc) {
                    const float t = ph * idt;
                    s0 += t + t - t * t - 1.0f;
                } else if (ph > 1.0f - inc) {
                    const float t = (ph - 1.0f) * idt;
                    s0 += t * t + t + t + 1.0f;
                }
                float q = ph - pw;
                if (q < 0.0f) q += 1.0f;
                if (q < inc) {
                    const float t = q * idt;
                    s0 -= t + t - t * t - 1.0f;
                } else if (q > 1.0f - inc) {
                    const float t = (q - 1.0f) * idt;
                    s0 -= t * t + t + t + 1.0f;
                }
                buf[i] = s0;
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        }
        case kNoise: {
            uint32_t r = v.rng;
            for (int i = 0; i < n; ++i) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                buf[i] = float(int32_t(r)) * (1.0f / 2147483648.0f);
            }
            v.rng = r;
            break;
        }
        case kFm: {
            const float minc = clampf(inc * p[pFmRatio], 0.0f, 0.49f);
            const float idx = p[pFmIndex] * (1.0f / 6.2831853f);
            float mp = v.mphase;
            for (int i = 0; i < n; ++i) {
                float cp = ph + sinTab(mp) * idx;
                cp -= float(int32_t(cp + 64.0f) - 64);   // wrap to [0, 1)
                buf[i] = sinTab(cp);
                ph += inc;
                if (ph >= 1.0f) ph -= 1.0f;
                mp += minc;
                if (mp >= 1.0f) mp -= 1.0f;
            }
            v.mphase = mp;
            break;
        }
        case kClick: {
            float ce = v.clickEnv;
            uint32_t r = v.rng;
            for (int i = 0; i < n; ++i) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                buf[i] = (ce >= 0.999f ? 1.0f : 0.0f) + float(int32_t(r)) * (0.5f / 2147483648.0f) * ce;
                ce *= 0.995f;
            }
            v.rng = r;
            v.clickEnv = ce < 1e-4f ? 0.0f : ce;
            break;
        }
        case kSample: {
            const SampleSlot& s = slots_[int(p[pSample]) & (kSlots - 1)];
            if (!s.data || !s.frames) {
                v.stage = 0;
                v.active = false;
                v.env = 0.0f;
                return;
            }
            const float rate = p[pRate] * hz * (1.0f / 261.6256f);
            const bool fwd = rate >= 0.0f;
            // 16.16 fixed-point position step
            const float arate = fabsf(rate) > 64.0f ? 64.0f : fabsf(rate);
            const uint32_t step = uint32_t(arate * 65536.0f);
            const uint32_t st = uint32_t(clampf(p[pStart], 0.0f, 0.9999f) * float(s.frames));
            uint32_t en = uint32_t(clampf(p[pStart] + p[pLength], 0.0f, 1.0f) * float(s.frames));
            if (en <= st + 1) en = st + 2 < s.frames ? st + 2 : s.frames;
            const bool loop = p[pLoop] >= 0.5f;
            const int16_t* SL = s.data;
            const int16_t* SR = s.channels == 2 ? s.data + s.frames : nullptr;
            stereo = SR != nullptr;
            uint32_t idx = v.sidx;
            uint32_t fr = uint32_t(v.sfrac * 65536.0f);
            const float kS = 1.0f / 32768.0f;
            const uint32_t last = s.frames - 1;
            int i = 0;
            for (; i < n; ++i) {
                if (idx >= en || idx < st) {
                    if (!loop) break;
                    idx = fwd ? st + (idx - en) % (en - st) : en - 1;
                    if (idx >= en) idx = st;
                }
                const uint32_t j = idx < last ? idx + 1 : idx;
                const float fk = float(fr) * (1.0f / 65536.0f);
                const float a = float(SL[idx]), b = float(SL[j]);
                buf[i] = (a + (b - a) * fk) * kS;
                if (SR) {
                    const float c = float(SR[idx]), d = float(SR[j]);
                    bufR[i] = (c + (d - c) * fk) * kS;
                }
                fr += step;
                const uint32_t adv = fr >> 16;
                fr &= 0xFFFFu;
                idx = fwd ? idx + adv : idx - adv;
            }
            if (i < n) {   // one-shot ended
                for (; i < n; ++i) buf[i] = bufR[i] = 0.0f;
                v.stage = 0;
            }
            v.sidx = idx;
            v.sfrac = float(fr) * (1.0f / 65536.0f);
            break;
        }
        case kGrain: {
            const SampleSlot& s = slots_[int(p[pSample]) & (kSlots - 1)];
            const bool haveS = s.data && s.frames > 64;
            const float glen = p[pGrainSize] * lab::dsp::kSr;
            const float every = lab::dsp::kSr / p[pGrainDensity];
            const float rate = p[pRate] * hz * (1.0f / 261.6256f);
            for (int i = 0; i < n; ++i) buf[i] = 0.0f;
            v.grainTimer -= float(n);
            if (v.grainTimer <= 0.0f) {
                v.grainTimer += every;
                if (v.grainTimer < 0.0f) v.grainTimer = every;
                for (int gi = 0; gi < grainCap_; ++gi) {
                    Grain& gr0 = v.grains[gi];
                    if (gr0.on) continue;
                    gr0.on = true;
                    gr0.age = 0.0f;
                    gr0.len = glen;
                    gr0.inc = fabsf(rate) > 0.01f ? (fabsf(rate) < 8.0f ? fabsf(rate) : 8.0f) : 0.01f;
                    gr0.frac = 0.0f;
                    if (haveS) {
                        float pos = p[pGrainPos] +
                                    p[pGrainSpread] * (float(xorshift(v.rng) >> 8) * (1.0f / 16777216.0f) - 0.5f);
                        pos -= floorf(pos);
                        gr0.idx = uint32_t(pos * float(s.frames - 2));
                    } else {
                        gr0.idx = xorshift(v.rng) & 1023u;
                    }
                    break;
                }
            }
            const float kS = 1.0f / 32768.0f;
            for (int gi = 0; gi < kGrains; ++gi) {
                Grain& gr0 = v.grains[gi];
                if (!gr0.on) continue;
                if (gi >= grainCap_) {   // governor: drop the extra grains
                    gr0.on = false;
                    continue;
                }
                // window: 4x(1-x) evaluated incrementally (second differences)
                const float invLen = 1.0f / gr0.len;
                float age = gr0.age;
                int cnt = int(gr0.len - age);
                if (cnt > n) cnt = n;
                if (cnt < 0) cnt = 0;
                const float x0 = age * invLen;
                float w = 4.0f * x0 * (1.0f - x0);
                float dw = 4.0f * invLen * (1.0f - 2.0f * x0 - invLen);
                const float ddw = -8.0f * invLen * invLen;
                if (haveS) {
                    uint32_t idx = gr0.idx;
                    uint32_t fr = uint32_t(gr0.frac * 65536.0f);
                    const uint32_t stp = uint32_t(gr0.inc * 65536.0f);
                    const uint32_t lim = s.frames - 1;
                    const int16_t* SD = s.data;
                    for (int i = 0; i < cnt; ++i) {
                        if (idx >= lim) idx = 0;
                        const float a = float(SD[idx]), b = float(SD[idx + 1]);
                        buf[i] += (a + (b - a) * float(fr) * (1.0f / 65536.0f)) * kS * w;
                        w += dw;
                        dw += ddw;
                        fr += stp;
                        idx += fr >> 16;
                        fr &= 0xFFFFu;
                    }
                    gr0.idx = idx;
                    gr0.frac = float(fr) * (1.0f / 65536.0f);
                } else {
                    float gp = gr0.frac;
                    for (int i = 0; i < cnt; ++i) {
                        buf[i] += sinTab(gp) * w;
                        w += dw;
                        dw += ddw;
                        gp += inc;
                        if (gp >= 1.0f) gp -= 1.0f;
                    }
                    gr0.frac = gp;
                }
                gr0.age = age + float(cnt);
                if (gr0.age >= gr0.len) gr0.on = false;
            }
            break;
        }
        default:
            for (int i = 0; i < n; ++i) buf[i] = 0.0f;
            break;
    }
    v.phase = ph;

    // ---- filter, drive, envelope, mix ----
    float* L = busL_ + from;
    float* R = busR_ + from;
    float* D = sendD_ + from;
    float* V = sendR_ + from;
    if (!stereo) {
        if (drive) {
            filterAny(ftype, buf, n, v.ic1, v.ic2, m);
            driveInPlace(buf, n, m.pre);
            if (sends) mixMono<3, true>(buf, n, v.ic1, v.ic2, m, L, R, D, V);
            else mixMono<3, false>(buf, n, v.ic1, v.ic2, m, L, R, D, V);
        } else if (sends) {
            mixAny<true>(ftype, buf, n, v.ic1, v.ic2, m, L, R, D, V);
        } else {
            mixAny<false>(ftype, buf, n, v.ic1, v.ic2, m, L, R, D, V);
        }
    } else {
        filterAny(ftype, buf, n, v.ic1, v.ic2, m);
        filterAny(ftype, bufR, n, v.ic1r, v.ic2r, m);
        if (drive) {
            driveInPlace(buf, n, m.pre);
            driveInPlace(bufR, n, m.pre);
        }
        float e = m.e;
        const float gl = m.gl * 1.4142f, gr = m.gr * 1.4142f, sd = m.sd * 0.5f, sv = m.sv * 0.5f, de = m.de;
        for (int i = 0; i < n; ++i) {
            const float x = buf[i] * e, y = bufR[i] * e;
            e += de;
            L[i] += x * gl;
            R[i] += y * gr;
            if (sends) {
                D[i] += (x + y) * sd;
                V[i] += (x + y) * sv;
            }
        }
    }
    // keep the filter finite (very high resonance + drive can run away)
    if (!(fabsf(v.ic1) < 1e4f) || !(fabsf(v.ic2) < 1e4f)) v.ic1 = v.ic2 = 0.0f;
    if (!(fabsf(v.ic1r) < 1e4f) || !(fabsf(v.ic2r) < 1e4f)) v.ic1r = v.ic2r = 0.0f;
    if (v.stage == 0) {
        v.active = false;
        v.env = 0.0f;
    }
}

OT_FASTRUN_FN void Engine::renderFx(float* l, float* r) {
    // ---- delay (ping-pong): skipped while idle (no send, echoes died away) ----
    const bool inD = sendsUsed_ || dlyEnergy_ > 1e-5f;
    const float lvlD = fx_[fDelayLevel], lvlV = fx_[fReverbLevel];
    if (inD) {
        const float target = fx_[fDelayTime] * lab::dsp::kSr;
        const float fb = fx_[fDelayFeedback];
        const float tone = 0.05f + 0.9f * fx_[fDelayTone];
        float dt = dlyTime_ * lab::dsp::kSr;
        const float dstep = (target - dt) * (1.0f / float(kBlock)) * 0.05f;
        float lp = dlyLp_, lpR = dlyLpR_, en = 0.0f;
        uint32_t w = dlyW_;
        const uint32_t len = dlyLen_;
        float* __restrict DL = dlyL_;
        float* __restrict DR = dlyR_;
        for (uint32_t i = 0; i < kBlock; ++i) {
            dt += dstep;
            const uint32_t di = uint32_t(dt);
            const float f = dt - float(di);
            // read position w - dt - 1 .. w - dt (wrap with one compare: dt < len - 2)
            int32_t i0 = int32_t(w) - int32_t(di) - 1;
            if (i0 < 0) i0 += int32_t(len);
            int32_t i1 = i0 - 1;
            if (i1 < 0) i1 += int32_t(len);
            const float dl = DL[i0] + (DL[i1] - DL[i0]) * f;
            const float dr = DR[i0] + (DR[i1] - DR[i0]) * f;
            lp += tone * (dl - lp);
            lpR += tone * (dr - lpR);
            DL[w] = sendD_[i] + lpR * fb;   // ping-pong: R feeds L
            DR[w] = lp * fb;
            if (++w >= len) w = 0;
            l[i] += dl * lvlD;
            r[i] += dr * lvlD;
            en += fabsf(dl) + fabsf(dr);
        }
        dlyTime_ = dt * lab::dsp::kInvSr;
        dlyLp_ = lp;
        dlyLpR_ = lpR;
        dlyW_ = w;
        dlyEnergy_ = en * (1.0f / float(kBlock)) + (sendsUsed_ ? 1.0f : 0.0f);
    } else {
        dlyTime_ = fx_[fDelayTime];
    }
    // ---- reverb at half rate: 4 shared combs, 2 allpasses per side ----
    if (sendsUsed_ || revEnergy_ > 1e-5f) {
        const float fb = 0.7f + 0.28f * fx_[fReverbSize];
        const float damp = fx_[fReverbDamp] * 0.7f;
        float en = 0.0f;
        float* cb[kCombs];
        uint32_t cw[kCombs], cl[kCombs];
        float clp[kCombs];
        for (int c = 0; c < kCombs; ++c) {
            cb[c] = comb_[c];
            cw[c] = combW_[c];
            cl[c] = combLen_[c];
            clp[c] = combLp_[c];
        }
        float pl = revPrevL_, pr = revPrevR_;
        for (uint32_t i = 0; i < kBlock; i += 2) {
            const float x = (sendR_[i] + sendR_[i + 1]) * 0.015f;   // average + Freeverb-like input gain
            float acc = 0.0f;
            for (int c = 0; c < kCombs; ++c) {
                const float y = cb[c][cw[c]];
                clp[c] = y + (clp[c] - y) * damp;
                cb[c][cw[c]] = x + clp[c] * fb;
                if (++cw[c] >= cl[c]) cw[c] = 0;
                acc += y;
            }
            float outs[2];
            for (int s = 0; s < 2; ++s) {
                float v = acc;
                for (int a = 0; a < kAps; ++a) {
                    float* ab = ap_[s][a];
                    uint32_t& aw = apW_[s][a];
                    const float bo = ab[aw];
                    ab[aw] = v + bo * 0.5f;
                    v = bo - v;
                    if (++aw >= apLen_[s][a]) aw = 0;
                }
                outs[s] = v * lvlV;
            }
            // linear upsampling
            l[i] += (pl + outs[0]) * 0.5f;
            r[i] += (pr + outs[1]) * 0.5f;
            l[i + 1] += outs[0];
            r[i + 1] += outs[1];
            pl = outs[0];
            pr = outs[1];
            en += fabsf(acc);
        }
        revPrevL_ = pl;
        revPrevR_ = pr;
        for (int c = 0; c < kCombs; ++c) {
            combW_[c] = cw[c];
            combLp_[c] = clp[c];
        }
        revEnergy_ = en * (2.0f / float(kBlock)) + (sendsUsed_ ? 1.0f : 0.0f);
    } else {
        revPrevL_ = revPrevR_ = 0.0f;
    }
}

uint32_t Engine::voiceCost(int wave, int ftype, bool drive, bool stereo, bool sends, int grains) {
    // M4 instructions per 128-sample block, measured under QEMU -icount (marginal
    // cost of one more voice, 2 control segments) plus ~10 % margin;
    // tests/lab/platform_test.cpp checks the governor keeps blocks under budget.
    static const uint16_t kWave[kWaveCount] = {4900, 4400, 4900, 6800, 3500, 8600, 6800, 2700, 4500};
    uint32_t c = kWave[wave < 0 || wave >= kWaveCount ? 0 : wave];
    if (wave == kGrain) c += uint32_t(grains) * 3900u;
    if (ftype != 3) c += stereo ? 2400 : 1200;
    if (drive) c += stereo ? 5200 : 3000;
    if (sends) c += 900;
    if (stereo) c += 4100;
    return c;
}

OT_FASTRUN_FN void Engine::render(const lab::Transport& t, float* l, float* r) {
    if (!voices_) return;
    // reset request from the main loop (app switch): silence, forget slots
    if (resetAck_ != resetReq_) {
        Cmd c;
        while (ring_->pop(c)) {}
        nPending_ = 0;
        for (int i = 0; i < kVoices; ++i) {
            Voice& v = voices_[i];
            v = Voice();
            for (int p = 0; p < pParamCount; ++p) v.p[p] = kRange[p].def;
            v.rng = 0x9E3779B9u * uint32_t(i + 1);
        }
        for (auto& s : slots_) s = SampleSlot();
        for (int f = 0; f < fFxCount; ++f) fx_[f] = kFxRange[f].def;
        std::memset(dlyL_, 0, dlyLen_ * 4);
        std::memset(dlyR_, 0, dlyLen_ * 4);
        for (int c = 0; c < kCombs; ++c) {
            std::memset(comb_[c], 0, combLen_[c] * 4);
            combLp_[c] = 0.0f;
        }
        for (int s = 0; s < 2; ++s)
            for (int a = 0; a < kAps; ++a) std::memset(ap_[s][a], 0, apLen_[s][a] * 4);
        dlyEnergy_ = revEnergy_ = 0.0f;
        revPrevL_ = revPrevR_ = 0.0f;
        resetAck_ = resetReq_;
    }

    // engine clock
    if (t.startedThisBlock) ++epoch_;
    const double bps = t.beatsPerSample > 0.0 ? t.beatsPerSample : 120.0 / 60.0 / 44100.0;
    const double beat = t.playing ? t.beat : freeBeat_;
    seq_ = seq_ + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    pubBeat_ = beat;
    pubBps_ = bps;
    pubEpoch_ = epoch_;
    pubBlocks_ = pubBlocks_ + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    seq_ = seq_ + 1;
    const double blockEnd = beat + bps * double(kBlock);
    freeBeat_ = blockEnd;

    // collect this block's events: pending + new commands
    struct Ev {
        int at;
        int idx;
    };
    Ev evs[64];
    Cmd now[64];
    int nev = 0;
    auto offsetOf = [&](const Cmd& c) -> int {
        if (c.when < 0.0 || c.epoch != epoch_ || c.when <= beat) return 0;
        const double s = (c.when - beat) / bps;
        return s < double(kBlock) ? int(s) : -1;
    };
    // pending first (they are older)
    int keep = 0;
    for (int i = 0; i < nPending_; ++i) {
        const Cmd& c = pending_[i];
        const int at = offsetOf(c);
        if (at >= 0 && nev < 64) {
            now[nev] = c;
            evs[nev] = Ev{at, nev};
            ++nev;
        } else {
            pending_[keep++] = c;
        }
    }
    nPending_ = keep;
    Cmd c;
    while (nev < 64 && ring_->pop(c)) {
        const int at = offsetOf(c);
        if (at >= 0) {
            now[nev] = c;
            evs[nev] = Ev{at, nev};
            ++nev;
        } else if (nPending_ < kPending) {
            pending_[nPending_++] = c;
        } else {
            ++dropped_;   // too far ahead with a full queue
        }
    }
    // stable insertion sort by time
    for (int i = 1; i < nev; ++i) {
        const Ev x = evs[i];
        int j = i - 1;
        while (j >= 0 && evs[j].at > x.at) {
            evs[j + 1] = evs[j];
            --j;
        }
        evs[j + 1] = x;
    }
    // Global commands (fx, sample slots, panic) take effect at the block start;
    // voice commands at their sample: each voice is split only at its own events.
    for (int i = 0; i < nev; ++i) {
        const Cmd& cm = now[evs[i].idx];
        if (cm.op == Cmd::kFx || cm.op == Cmd::kSlot || cm.op == Cmd::kPanic) apply(cm);
    }

    // ---- CPU governor: estimate this block, limit grains, then voices ----
    uint32_t cost = 0;
    int nActive = 0;
    bool anyEvents = false;
    bool starts[kVoices] = {};
    for (int i = 0; i < nev; ++i) {
        const Cmd& cm = now[evs[i].idx];
        if ((cm.op == Cmd::kOn || cm.op == Cmd::kOnFreq) && cm.a < kVoices) {
            anyEvents = true;
            starts[cm.a] = true;
        }
    }
    uint32_t vcost[kVoices];
    int nGrain = 0;
    for (int vi = 0; vi < kVoices; ++vi) {
        const Voice& v = voices_[vi];
        vcost[vi] = 0;
        if (!v.active && !starts[vi]) continue;
        const int w = int(v.p[pWave]);
        const bool st = (w == kSample || w == kGrain) && slots_[int(v.p[pSample]) & (kSlots - 1)].channels == 2 && w == kSample;
        const int ft = int(v.p[pFtype]) == 0 && v.p[pCutoff] >= 17999.0f && v.p[pFenv] == 0.0f ? 3 : int(v.p[pFtype]);
        vcost[vi] = voiceCost(w, ft, v.p[pDrive] > 0.0f, st, v.p[pSendDelay] > 0.0f || v.p[pSendReverb] > 0.0f, kGrains);
        cost += vcost[vi];
        ++nActive;
        if (w == kGrain) ++nGrain;
    }
    const uint32_t fxCost = (sendsUsed_ || dlyEnergy_ > 1e-5f ? 10000u : 0u) + (sendsUsed_ || revEnergy_ > 1e-5f ? 10500u : 0u);
    cost += fxCost + 5200u;   // + command handling, bus clear, output
    // grains first: fewer simultaneous grains per grain voice
    int cap = kGrains;
    while (cost > kBudgetInsn && cap > 1 && nGrain) {
        --cap;
        cost -= uint32_t(nGrain) * 3900u;
        for (int vi = 0; vi < kVoices; ++vi)
            if (vcost[vi] && int(voices_[vi].p[pWave]) == kGrain) vcost[vi] -= 3900u;
    }
    grainCap_ = cap;
    bool limited = cap < kGrains;
    // then the quietest sounding voices stop (a voice starting in this block is kept)
    while (cost > kBudgetInsn) {
        int q = -1;
        float lv = 2.0f;
        for (int vi = 0; vi < kVoices; ++vi) {
            const Voice& v = voices_[vi];
            if (v.active && !starts[vi] && v.env * v.p[pAmp] < lv) {
                lv = v.env * v.p[pAmp];
                q = vi;
            }
        }
        if (q < 0) break;
        Voice& v = voices_[q];
        v.active = false;
        v.stage = 0;
        v.env = 0.0f;
        cost -= vcost[q];
        vcost[q] = 0;
        limited = true;
    }
    if (limited) ++overloads_;
    lastCost_ = cost;

    const bool fxBusy = sendsUsed_ || dlyEnergy_ > 1e-5f || revEnergy_ > 1e-5f;
    if (!nActive && !anyEvents && !fxBusy) {   // nothing sounding: only parameter changes
        for (int i = 0; i < nev; ++i) {
            const Cmd& cm = now[evs[i].idx];
            if (cm.op == Cmd::kSet || cm.op == Cmd::kOff) apply(cm);
        }
        for (int vi = 0; vi < kVoices; ++vi) meter_[vi] = 0.0f;
        outPeak_ = 0.0f;
        sendsUsed_ = false;
        return;
    }
    for (uint32_t i = 0; i < kBlock; ++i) busL_[i] = busR_[i] = sendD_[i] = sendR_[i] = 0.0f;
    sendsUsed_ = false;
    for (int vi = 0; vi < kVoices; ++vi) {
        Voice& v = voices_[vi];
        int pos = 0;
        for (int i = 0; i < nev; ++i) {
            const Cmd& cm = now[evs[i].idx];
            if (cm.a != vi || !(cm.op == Cmd::kOn || cm.op == Cmd::kOnFreq || cm.op == Cmd::kOff || cm.op == Cmd::kSet))
                continue;
            const int at = evs[i].at;
            while (pos < at && v.active) {   // control rate: at most kCtl samples a segment
                const int end = pos + kCtl < at ? pos + kCtl : at;
                renderVoice(v, pos, end);
                pos = end;
            }
            pos = at;
            apply(cm);
        }
        while (pos < int(kBlock) && v.active) {
            const int end = pos + kCtl < int(kBlock) ? pos + kCtl : int(kBlock);
            renderVoice(v, pos, end);
            pos = end;
        }
    }

    const float lvl = fx_[fLevel];
    if (lvl != 1.0f)
        for (uint32_t i = 0; i < kBlock; ++i) {
            busL_[i] *= lvl;
            busR_[i] *= lvl;
        }
    renderFx(busL_, busR_);
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float a = busL_[i], b = busR_[i];
        l[i] += a;
        r[i] += b;
        const float m = fabsf(a) + fabsf(b);
        if (m > pk) pk = m;
    }
    outPeak_ = pk * 0.5f;
    for (int vi = 0; vi < kVoices; ++vi) meter_[vi] = voices_[vi].active ? voices_[vi].env : 0.0f;
}

}  // namespace plat
