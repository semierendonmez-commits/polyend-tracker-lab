// FLIPPER -- the sound (flip_audio.h).
#include "ws/flip_audio.h"

#include <cmath>
#include <cstring>

#include "core/fastrun.h"

namespace flip {

using lab::kBlock;
using lab::dsp::clamp;
using lab::dsp::kInvSr;
using lab::dsp::kSr;

namespace {

// sin(2 pi p) for p in [0, 1), parabolic approximation with one refinement (~0.1 % error).
inline float fsin(float p) {
    float t = p - float(int(p));
    if (t < 0.0f) t += 1.0f;
    t = t < 0.5f ? t : t - 1.0f;   // -0.5 .. 0.5
    const float y = 8.0f * t - 16.0f * t * fabsf(t);
    return 0.225f * (y * fabsf(y) - y) + y;
}
inline float wrap(float p) { return p >= 1.0f ? p - 1.0f : p; }
inline float noise(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(int32_t(s)) * (1.0f / 2147483648.0f);
}
// SVF coefficients without tanf (fc < ~0.23 sr).
inline void svfFast(lab::dsp::Svf& f, float hz, float q) {
    hz = clamp(hz, 20.0f, 16000.0f);
    const float x = lab::dsp::kPi * hz * kInvSr;
    const float x2 = x * x;
    f.g = x * (1.0f + x2 * (0.33333f + x2 * (0.13333f + x2 * 0.05397f)));
    f.k = 1.0f / clamp(q, 0.5f, 40.0f);
}
inline float svfLp(lab::dsp::Svf& f, float x) {
    const float a1 = 1.0f / (1.0f + f.g * (f.g + f.k));
    const float v3 = x - f.ic2;
    const float v1 = a1 * f.ic1 + f.g * a1 * v3;
    const float v2 = f.ic2 + f.g * v1;
    f.ic1 = 2.0f * v1 - f.ic1;
    f.ic2 = 2.0f * v2 - f.ic2;
    return v2;
}
// Coefficient for an exponential decay to -60 dB in `sec` seconds.
inline float decayK(float sec) { return expf(-6.9f / (clamp(sec, 0.003f, 30.0f) * kSr)); }
inline float decaySec(uint8_t d, float lo, float range) { return lo * powf(range, float(d) * 0.01f); }

const float kBellRatio[8] = {1.0f, 1.4142f, 2.0f, 2.76f, 3.5f, 4.2f, 5.4f, 7.0f};

}  // namespace

bool Audio::init(lab::Arena& arena) {
    for (auto& v : v_) {
        v.ks = static_cast<float*>(arena.alloc(kPluckLen * sizeof(float)));
        if (!v.ks) return false;
    }
    for (auto& s : samples) {
        s.data = static_cast<int16_t*>(arena.alloc(kSampleMaxFrames * sizeof(int16_t)));
        if (!s.data) return false;
        s.frames = 0;
    }
    for (auto& g : g_) {
        g.stutCap = 44100;
        g.stut = static_cast<float*>(arena.alloc(g.stutCap * 2 * sizeof(float)));
        if (!g.stut) return false;
    }
    dLen_ = 44100 * 2;
    dl_ = static_cast<float*>(arena.alloc(dLen_ * sizeof(float)));
    dr_ = static_cast<float*>(arena.alloc(dLen_ * sizeof(float)));
    static const uint32_t kRv[4] = {2203, 2687, 3163, 3511};
    static const uint32_t kAp[2] = {347, 773};
    for (int i = 0; i < 4; ++i) {
        rvLen_[i] = kRv[i];
        rv_[i] = static_cast<float*>(arena.alloc(kRv[i] * sizeof(float)));
        if (!rv_[i]) return false;
    }
    for (int i = 0; i < 2; ++i) {
        apLen_[i] = kAp[i];
        ap_[i] = static_cast<float*>(arena.alloc(kAp[i] * sizeof(float)));
        if (!ap_[i]) return false;
    }
    return dl_ && dr_;
}

int Audio::voicesOn() const {
    int n = 0;
    for (const auto& v : v_) n += v.on ? 1 : 0;
    return n;
}

void Audio::panic() {
    for (auto& v : v_) v.on = false;
    for (auto& g : g_) {
        for (int f = 0; f < kFxCount; ++f) g.fLen[f] = 0;
        g.stutting = false;
    }
    for (auto& p : perfFx_) p = 0.0f;
}

void Audio::startVoice(Voice& v, const Instrument& in, int inst, int note, int vel) {
    const float vf = float(vel) * (1.0f / 127.0f);
    v.on = true;
    v.inst = uint8_t(inst);
    v.engine = in.engine;
    v.group = in.group;
    v.note = uint8_t(note);
    v.age = ++age_;
    v.ctr = 0;
    v.tone = float(in.tone) * 0.01f;
    v.a = float(in.charA) * 0.01f;
    v.b = float(in.charB) * 0.01f;
    v.amp = vf * vf * float(in.level) * 0.01f * 0.55f;
    const float pan = float(in.pan) * 0.01f;
    v.gl = cosf(pan * 1.5707963f) * 1.41421f * 0.7071f;
    v.gr = sinf(pan * 1.5707963f) * 1.41421f * 0.7071f;
    v.sendD = float(in.sendDelay) * 0.01f;
    v.sendR = float(in.sendReverb) * 0.01f;
    v.env = 1.0f;
    v.z1 = v.z2 = v.z3 = 0.0f;
    v.svf.ic1 = v.svf.ic2 = 0.0f;
    v.noise = 0x9E3779B9u ^ (v.age * 2654435761u);
    const float hz = lab::dsp::mtof(float(note));
    v.ph = v.ph2 = 0.0f;
    v.inc = hz * kInvSr;
    v.pitchEnv = v.modEnv = 1.0f;
    v.pitchAmt = v.modAmt = 0.0f;
    switch (in.engine) {
        case kEnKick:
            v.envK = decayK(decaySec(in.decay, 0.08f, 15.0f));
            v.pitchK = decayK(0.03f + 0.1f * v.b);
            v.pitchAmt = 2.0f + 8.0f * v.a;
            v.inc = lab::dsp::mtof(float(note) - 12.0f) * kInvSr * 1.4f;
            break;
        case kEnSnare:
            v.envK = decayK(decaySec(in.decay, 0.06f, 12.0f));
            v.modK = decayK(0.05f);   // tone body
            v.inc = (160.0f + 2.0f * float(note - 50 > -40 ? note - 50 : -40) * 4.0f) * kInvSr;
            svfFast(v.svf, 1200.0f + 7000.0f * v.tone, 0.8f);
            break;
        case kEnHat:
            v.envK = decayK(decaySec(in.decay, 0.02f, 40.0f));
            svfFast(v.svf, 5000.0f + 7000.0f * v.tone, 0.9f);
            v.inc = hz * kInvSr * 2.0f;
            v.inc2 = v.inc * 1.4837f;
            break;
        case kEnZap:
            v.envK = decayK(decaySec(in.decay, 0.04f, 25.0f));
            v.pitchK = decayK(0.02f + 0.25f * v.tone);
            v.pitchAmt = 3.0f + 12.0f * v.a;
            v.modAmt = v.b * 0.6f;
            v.inc2 = v.inc * 1.5f;
            break;
        case kEnPluck: {
            v.envK = decayK(decaySec(in.decay, 0.3f, 25.0f));
            float period = kSr / hz;
            if (period > float(kPluckLen - 4)) period = float(kPluckLen - 4);
            if (period < 2.0f) period = 2.0f;
            v.pos = period - 0.5f;   // read distance (the averaging filter adds half a sample)
            const float t60 = decaySec(in.decay, 0.3f, 25.0f);
            v.rate = powf(0.001f, 1.0f / (hz * t60));   // loop gain
            v.z3 = 0.2f + 0.75f * v.tone;                // brightness of the excitation
            // only the last `period` samples behind the write position are ever read
            float lp = 0.0f;
            const uint32_t n = uint32_t(period) + 2;
            for (uint32_t i = 0; i < n; ++i) {
                lp += v.z3 * (noise(v.noise) - lp);
                v.ks[i] = lp * (1.0f + v.a);
            }
            v.ksLen = kPluckLen;
            v.ksPos = n;
            break;
        }
        case kEnBell:
            v.envK = decayK(decaySec(in.decay, 0.15f, 40.0f));
            v.modK = decayK(0.05f + 1.5f * v.tone);
            v.inc2 = v.inc * kBellRatio[int(v.a * 7.99f)];
            v.modAmt = 0.2f + 2.5f * v.b;
            break;
        case kEnLead:
            v.envK = decayK(decaySec(in.decay, 0.08f, 30.0f));
            v.modK = decayK(0.05f + 0.6f * (float(in.decay) * 0.01f));   // filter envelope
            v.cut = 80.0f * powf(60.0f, v.tone);
            v.cutEnv = 6.0f * vf * vf;
            v.modAmt = v.a;   // saw <-> square
            break;
        case kEnSample: {
            v.envK = in.decay >= 99 ? 1.0f : decayK(decaySec(in.decay, 0.05f, 100.0f));
            v.pos = 0.0f;
            v.rate = exp2f(float(note - int(in.root)) * (1.0f / 12.0f));
            v.cut = 0.05f + 0.95f * v.tone * v.tone;
            break;
        }
        default: v.on = false; break;
    }
}

void Audio::trigger(int inst, int note, int vel, int offset) {
    if (!m_ || inst < 0 || inst >= kInsts) return;
    const Instrument& in = m_->inst[inst];
    if (in.engine == kEnSample && samples[inst].frames == 0) return;
    // the same drum restarts its own voice (choke); otherwise steal the oldest or quietest
    Voice* pick = nullptr;
    if (in.group == kGrDrums || in.engine == kEnKick || in.engine == kEnSnare || in.engine == kEnHat) {
        for (auto& v : v_)
            if (v.on && v.inst == inst) {
                pick = &v;
                break;
            }
    }
    if (!pick && in.group == kGrBass) {   // monophonic bass
        for (auto& v : v_)
            if (v.on && v.inst == inst) {
                pick = &v;
                break;
            }
    }
    if (!pick)
        for (auto& v : v_)
            if (!v.on) {
                pick = &v;
                break;
            }
    if (!pick) {
        float best = 1e9f;
        for (auto& v : v_) {
            const float score = v.env * v.amp * 4.0f + float(v.age - (age_ > 4096 ? age_ - 4096 : 0)) * 1e-4f;
            if (score < best) {
                best = score;
                pick = &v;
            }
        }
    }
    if (!pick) return;
    // a stolen voice that is still loud fades through the new attack (the attack ramp hides the cut)
    startVoice(*pick, in, inst, note, vel);
    pick->wait = offset < 0 ? 0 : (offset >= int(kBlock) ? int(kBlock) - 1 : offset);
}

void Audio::fx(const FxOut& f, int offset) {
    (void)offset;
    if (f.fx >= kFxCount) return;
    const uint32_t len = uint32_t(float(f.steps < 1 ? 1 : f.steps) * stepSamples_);
    for (int g = 0; g < kGroups; ++g) {
        if (!(f.groups & (1u << g))) continue;
        GroupFx& G = g_[g];
        G.fT[f.fx] = 0;
        G.fLen[f.fx] = len;
        G.fAmt[f.fx] = float(f.amount) * 0.01f;
        if (f.fx == kFxStutter) G.stutting = false;   // grab a fresh slice
    }
}

void Audio::perf(int f, float amount) {
    if (f < 0 || f >= kFxCount) return;
    perfFx_[f] = clamp(amount, 0.0f, 1.0f);
}

OT_FASTRUN_FN void Audio::renderVoice(Voice& v, int i0) {
    // The voice renders into a local mono buffer (no aliasing with the voice state),
    // then one pass pans it into its group bus and the two sends.
    float o[kBlock];
    for (int i = 0; i < i0; ++i) o[i] = 0.0f;
    int last = int(kBlock);
    float env = v.env;
    const float envK = v.envK, amp = v.amp;
    float ph = v.ph, ph2 = v.ph2;
    int ctr = v.ctr;
    switch (v.engine) {
        case kEnKick: {
            float pe = v.pitchEnv;
            const float pk = v.pitchK, pa = v.pitchAmt, inc = v.inc, drive = 1.0f + 3.0f * v.tone;
            for (int i = i0; i < int(kBlock); ++i) {
                ph = wrap(ph + inc * (1.0f + pa * pe));
                pe *= pk;
                float s = fsin(ph) * drive;
                s = s / (1.0f + fabsf(s));
                const float atk = ctr < 32 ? float(ctr++) * (1.0f / 32.0f) : 1.0f;
                s *= env * atk * 1.6f;
                env *= envK;
                o[i] = s;
            }
            v.pitchEnv = pe;
            break;
        }
        case kEnSnare: {
            float me = v.modEnv;
            const float mk = v.modK, inc = v.inc, mixN = 0.35f + 0.65f * v.a;
            for (int i = i0; i < int(kBlock); ++i) {
                ph = wrap(ph + inc);
                const float body = fsin(ph) * me;
                me *= mk;
                v.svf.process(noise(v.noise));
                const float n = v.svf.bp * 1.6f + v.svf.hp * 0.4f;
                const float atk = ctr < 16 ? float(ctr++) * (1.0f / 16.0f) : 1.0f;
                const float s = (body * (1.0f - mixN) + n * mixN) * env * atk;
                env *= envK;
                o[i] = s;
            }
            v.modEnv = me;
            break;
        }
        case kEnHat: {
            const float inc = v.inc, inc2 = v.inc2, metal = v.a;
            for (int i = i0; i < int(kBlock); ++i) {
                ph = wrap(ph + inc);
                ph2 = wrap(ph2 + inc2);
                const float sq = ((ph < 0.5f) ? 1.0f : -1.0f) * ((ph2 < 0.5f) ? 1.0f : -1.0f);
                v.svf.process(noise(v.noise) * (1.0f - metal) + sq * metal * 0.7f);
                const float atk = ctr < 8 ? float(ctr++) * 0.125f : 1.0f;
                const float s = v.svf.hp * env * atk;
                env *= envK;
                o[i] = s;
            }
            break;
        }
        case kEnZap: {
            float pe = v.pitchEnv;
            const float pk = v.pitchK, pa = v.pitchAmt, inc = v.inc, inc2 = v.inc2, fm = v.modAmt;
            for (int i = i0; i < int(kBlock); ++i) {
                const float f = 1.0f + pa * pe;
                pe *= pk;
                ph2 = wrap(ph2 + inc2 * f);
                ph = wrap(ph + inc * f);
                const float atk = ctr < 16 ? float(ctr++) * (1.0f / 16.0f) : 1.0f;
                const float s = fsin(ph + fm * fsin(ph2)) * env * atk;
                env *= envK;
                o[i] = s;
            }
            v.pitchEnv = pe;
            break;
        }
        case kEnPluck: {
            float* ks = v.ks;
            uint32_t w = v.ksPos;
            const float dist = v.pos, g = v.rate;
            const uint32_t di = uint32_t(dist);
            const float fr = dist - float(di);
            float prev = v.z1;
            for (int i = i0; i < int(kBlock); ++i) {
                const uint32_t r0 = (w - di + kPluckLen) & (kPluckLen - 1);
                const uint32_t r1 = (r0 - 1 + kPluckLen) & (kPluckLen - 1);
                const float x = ks[r0] + (ks[r1] - ks[r0]) * fr;
                const float y = 0.5f * (x + prev) * g;
                prev = x;
                ks[w] = y;
                w = (w + 1) & (kPluckLen - 1);
                const float s = x * env;
                env *= envK;
                o[i] = s;
            }
            v.z1 = prev;
            v.ksPos = w;
            break;
        }
        case kEnBell: {
            float me = v.modEnv;
            const float mk = v.modK, inc = v.inc, inc2 = v.inc2, idx = v.modAmt;
            for (int i = i0; i < int(kBlock); ++i) {
                ph = wrap(ph + inc);
                ph2 = wrap(ph2 + inc2);
                const float m = fsin(ph2) * idx * me;
                me *= mk;
                const float atk = ctr < 24 ? float(ctr++) * (1.0f / 24.0f) : 1.0f;
                const float s = fsin(ph + m) * env * atk;
                env *= envK;
                o[i] = s;
            }
            v.modEnv = me;
            break;
        }
        case kEnLead: {
            float me = v.modEnv;
            const float mk = v.modK, inc = v.inc, sqMix = v.modAmt, q = 0.7f + 9.0f * v.b;
            int i = i0;
            while (i < int(kBlock)) {
                const int end = (i + 16 < int(kBlock)) ? i + 16 : int(kBlock);
                svfFast(v.svf, v.cut * (1.0f + v.cutEnv * me * 8.0f), q);
                for (; i < end; ++i) {
                    ph += inc;
                    if (ph >= 1.0f) ph -= 1.0f;
                    // square = saw(ph) - saw(ph + 1/2): x = saw + (square - saw) * mix = saw1 - mix * saw2
                    float p2 = ph + 0.5f;
                    if (p2 >= 1.0f) p2 -= 1.0f;
                    const float saw1 = 2.0f * ph - 1.0f - lab::dsp::Osc::blep(ph, inc);
                    const float saw2 = 2.0f * p2 - 1.0f - lab::dsp::Osc::blep(p2, inc);
                    const float x = saw1 - sqMix * saw2;
                    me *= mk;
                    const float atk = ctr < 32 ? float(ctr++) * (1.0f / 32.0f) : 1.0f;
                    const float s = svfLp(v.svf, x) * env * atk * 0.8f;
                    env *= envK;
                    o[i] = s;
                }
            }
            v.modEnv = me;
            break;
        }
        case kEnSample: {
            const SampleSlot& sl = samples[v.inst];
            const uint32_t n = sl.frames;
            const int16_t* d = sl.data;
            float pos = v.pos, lp = v.z1;
            const float rate = v.rate, k = v.cut;
            for (int i = i0; i < int(kBlock); ++i) {
                const uint32_t p0 = uint32_t(pos);
                if (n < 2 || p0 + 1 >= n) {
                    env = 0.0f;
                    last = i;
                    break;
                }
                const float fr = pos - float(p0);
                const float x = (float(d[p0]) + (float(d[p0 + 1]) - float(d[p0])) * fr) * (1.0f / 32768.0f);
                lp += k * (x - lp);
                pos += rate;
                const float atk = ctr < 16 ? float(ctr++) * (1.0f / 16.0f) : 1.0f;
                const float s = lp * env * atk * 1.4f;
                env *= envK;
                o[i] = s;
            }
            v.pos = pos;
            v.z1 = lp;
            break;
        }
        default:
            env = 0.0f;
            last = i0;
            break;
    }
    v.ph = ph;
    v.ph2 = ph2;
    v.ctr = ctr;
    v.env = env;
    if (env < 0.0003f) v.on = false;
    float* __restrict bl = busL_[v.group];
    float* __restrict br = busR_[v.group];
    float* __restrict dd = sendD_;
    float* __restrict rr = sendR_;
    const float gl = v.gl * amp, gr = v.gr * amp, sd = v.sendD * amp, sr = v.sendR * amp;
    if (sd > 0.0f || sr > 0.0f) {
        for (int i = i0; i < last; ++i) {
            const float x = o[i];
            bl[i] += x * gl;
            br[i] += x * gr;
            dd[i] += x * sd;
            rr[i] += x * sr;
        }
    } else {
        for (int i = i0; i < last; ++i) {
            const float x = o[i];
            bl[i] += x * gl;
            br[i] += x * gr;
        }
    }
}

OT_FASTRUN_FN void Audio::groupFx(int gi) {
    GroupFx& G = g_[gi];
    float* l = busL_[gi];
    float* r = busR_[gi];
    float e[kFxCount];
    for (int f = 0; f < kFxCount; ++f) {
        float v = 0.0f;
        if (G.fLen[f]) {
            const float t = float(G.fT[f]) / float(G.fLen[f]);
            v = (t < 0.1f ? t * 10.0f : 1.0f - (t - 0.1f) * (1.0f / 0.9f)) * G.fAmt[f];
            G.fT[f] += kBlock;
            if (G.fT[f] >= G.fLen[f]) G.fLen[f] = 0;
        }
        if (perfFx_[f] > v) v = perfFx_[f];
        e[f] = v < 0.0f ? 0.0f : v;
        G.level[f] = e[f];
    }
    // filter sweep: low-pass closing down with the envelope
    if (e[kFxFilter] > 0.002f) {
        const float hz = 16000.0f * powf(0.0125f, e[kFxFilter]);
        svfFast(G.svfL, hz, 1.2f + 4.0f * e[kFxFilter]);
        G.svfR.g = G.svfL.g;
        G.svfR.k = G.svfL.k;
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] = svfLp(G.svfL, l[i]);
            r[i] = svfLp(G.svfR, r[i]);
        }
    } else {
        G.svfL.ic1 = G.svfL.ic2 = G.svfR.ic1 = G.svfR.ic2 = 0.0f;
    }
    // crush: sample-rate reduction + quantisation
    if (e[kFxCrush] > 0.002f) {
        const float step = 1.0f / (1.0f + e[kFxCrush] * 24.0f);
        const float q = 4.0f + (1.0f - e[kFxCrush]) * 120.0f, iq = 1.0f / q;
        for (uint32_t i = 0; i < kBlock; ++i) {
            G.crushPh += step;
            if (G.crushPh >= 1.0f) {
                G.crushPh -= 1.0f;
                G.crushHoldL = float(int(l[i] * q)) * iq;
                G.crushHoldR = float(int(r[i] * q)) * iq;
            }
            l[i] = G.crushHoldL;
            r[i] = G.crushHoldR;
        }
    }
    // stutter: repeat the last slice while the envelope is up
    float* st = G.stut;
    const uint32_t cap = G.stutCap;
    if (e[kFxStutter] > 0.05f) {
        if (!G.stutting) {
            G.stutting = true;
            const float frac = e[kFxStutter] > 0.66f ? 0.25f : (e[kFxStutter] > 0.33f ? 0.5f : 1.0f);
            uint32_t len = uint32_t(stepSamples_ * frac);
            if (len < 256) len = 256;
            if (len > cap - kBlock) len = cap - kBlock;
            G.stutLen = len;
            G.stutPos = 0;
        }
        const uint32_t start = (G.stutW + cap - G.stutLen) % cap;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint32_t p = (start + G.stutPos) % cap;
            float w = 1.0f;
            if (G.stutPos < 32) w = float(G.stutPos) * (1.0f / 32.0f);
            else if (G.stutPos + 32 > G.stutLen) w = float(G.stutLen - G.stutPos) * (1.0f / 32.0f);
            l[i] = st[2 * p] * w;
            r[i] = st[2 * p + 1] * w;
            if (++G.stutPos >= G.stutLen) G.stutPos = 0;
        }
    } else {
        G.stutting = false;
        uint32_t w = G.stutW;
        for (uint32_t i = 0; i < kBlock; ++i) {
            st[2 * w] = l[i];
            st[2 * w + 1] = r[i];
            if (++w >= cap) w = 0;
        }
        G.stutW = w;
    }
    // throws into the delay / reverb
    const float td = e[kFxDelay] * 0.9f, tr = e[kFxReverb] * 1.2f;
    if (td > 0.002f || tr > 0.002f)
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float m = (l[i] + r[i]) * 0.5f;
            sendD_[i] += m * td;
            sendR_[i] += m * tr;
        }
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; i += 4) {
        const float a = fabsf(l[i]) + fabsf(r[i]);
        if (a > pk) pk = a;
    }
    groupLevel[gi] = groupLevel[gi] * 0.9f + pk * 0.1f > pk ? groupLevel[gi] * 0.9f + pk * 0.1f : pk;
}

OT_FASTRUN_FN void Audio::delay(int) {
    // ping-pong, dotted eighth, low-passed feedback
    float t = 0.75f * 60.0f / (bpm_ > 20.0f ? bpm_ : 20.0f) * kSr;
    if (t > float(dLen_ - 2)) t = float(dLen_ - 2);
    float* dl = dl_;
    float* dr = dr_;
    const uint32_t len = dLen_;
    uint32_t w = dW_;
    float lpL = dLpL_, lpR = dLpR_;
    for (uint32_t i = 0; i < kBlock; ++i) {
        dTime_ += (t - dTime_) * 0.0005f;
        uint32_t rp = w + len - uint32_t(dTime_);
        if (rp >= len) rp -= len;
        const float yl = dl[rp], yr = dr[rp];
        lpL += 0.45f * (yl - lpL);
        lpR += 0.45f * (yr - lpR);
        dl[w] = sendD_[i] * 0.8f + lpR * 0.5f;
        dr[w] = lpL * 0.5f;
        if (++w >= len) w = 0;
        fxL_[i] += yl;
        fxR_[i] += yr;
    }
    dW_ = w;
    dLpL_ = lpL;
    dLpR_ = lpR;
}

OT_FASTRUN_FN void Audio::reverb() {
    float* d0 = rv_[0];
    float* d1 = rv_[1];
    float* d2 = rv_[2];
    float* d3 = rv_[3];
    uint32_t w0 = rvW_[0], w1 = rvW_[1], w2 = rvW_[2], w3 = rvW_[3];
    const uint32_t L0 = rvLen_[0], L1 = rvLen_[1], L2 = rvLen_[2], L3 = rvLen_[3];
    float* a0 = ap_[0];
    float* a1 = ap_[1];
    uint32_t aw0 = apW_[0], aw1 = apW_[1];
    float lp0 = rvLp_[0], lp1 = rvLp_[1], lp2 = rvLp_[2], lp3 = rvLp_[3];
    const float fb = 0.42f, damp = 0.35f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = sendR_[i] * 0.5f;
        // two input all-passes
        float y = a0[aw0];
        float v = x + 0.6f * y;
        a0[aw0] = v;
        x = y - 0.6f * v;
        if (++aw0 >= apLen_[0]) aw0 = 0;
        y = a1[aw1];
        v = x + 0.6f * y;
        a1[aw1] = v;
        x = y - 0.6f * v;
        if (++aw1 >= apLen_[1]) aw1 = 0;
        const float o0 = d0[w0], o1 = d1[w1], o2 = d2[w2], o3 = d3[w3];
        lp0 += damp * (o0 - lp0);
        lp1 += damp * (o1 - lp1);
        lp2 += damp * (o2 - lp2);
        lp3 += damp * (o3 - lp3);
        // Hadamard feedback
        const float s01 = lp0 + lp1, d01 = lp0 - lp1, s23 = lp2 + lp3, d23 = lp2 - lp3;
        d0[w0] = x + fb * (s01 + s23);
        d1[w1] = x + fb * (d01 + d23);
        d2[w2] = fb * (s01 - s23);
        d3[w3] = fb * (d01 - d23);
        if (++w0 >= L0) w0 = 0;
        if (++w1 >= L1) w1 = 0;
        if (++w2 >= L2) w2 = 0;
        if (++w3 >= L3) w3 = 0;
        fxL_[i] += (o0 + o2) * 0.6f;
        fxR_[i] += (o1 + o3) * 0.6f;
    }
    rvW_[0] = w0;
    rvW_[1] = w1;
    rvW_[2] = w2;
    rvW_[3] = w3;
    apW_[0] = aw0;
    apW_[1] = aw1;
    rvLp_[0] = lp0;
    rvLp_[1] = lp1;
    rvLp_[2] = lp2;
    rvLp_[3] = lp3;
}

OT_FASTRUN_FN void Audio::render(float* l, float* r) {
    for (int g = 0; g < kGroups; ++g)
        for (uint32_t i = 0; i < kBlock; ++i) busL_[g][i] = busR_[g][i] = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        sendD_[i] = sendR_[i] = 0.0f;
        fxL_[i] = fxR_[i] = 0.0f;
    }
    for (auto& v : v_) {
        if (!v.on) continue;
        const int start = v.wait;
        v.wait = 0;
        renderVoice(v, start);
    }
    for (int g = 0; g < kGroups; ++g) groupFx(g);
    // the sends idle once their input and tails are silent
    float inD = 0.0f, inR = 0.0f;
    for (uint32_t i = 0; i < kBlock; i += 2) {
        inD += fabsf(sendD_[i]);
        inR += fabsf(sendR_[i]);
    }
    dQuiet_ = inD > 1e-5f ? 0 : dQuiet_ + 1;
    rQuiet_ = inR > 1e-5f ? 0 : rQuiet_ + 1;
    if (dQuiet_ < 1400) delay(0);   // ~4 s: the echoes have died away (0.5 feedback, low-passed)
    if (rQuiet_ < 1000) reverb();   // ~3 s
    for (uint32_t i = 0; i < kBlock; ++i) {
        float a = busL_[0][i] + busL_[1][i] + busL_[2][i] + fxL_[i];
        float b = busR_[0][i] + busR_[1][i] + busR_[2][i] + fxR_[i];
        l[i] += dcL_.process(a);
        r[i] += dcR_.process(b);
    }
}

}  // namespace flip
