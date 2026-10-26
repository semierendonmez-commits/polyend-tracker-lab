// TILT -- the sound (tilt_audio.h).
#include "ws/tilt_audio.h"

#include <cmath>
#include <cstring>

#include "core/fastrun.h"
#include "ws/mi/plaits/dsp/drums/analog_bass_drum.h"
#include "ws/mi/plaits/dsp/drums/analog_snare_drum.h"
#include "ws/mi/plaits/dsp/drums/hi_hat.h"
#include "ws/mi/plaits/dsp/drums/synthetic_bass_drum.h"
#include "ws/mi/plaits/dsp/drums/synthetic_snare_drum.h"
#include "ws/mi/plaits/resources.h"
#include "ws/tilt_engine.h"

namespace tilt {

using lab::kBlock;
namespace dsp = lab::dsp;

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float k01(int v) { return float(v) * (1.0f / 127.0f); }
// Plaits' sine table (512 points + a guard), phase as 32 bits.
inline float sine32(uint32_t ph) {
    const uint32_t i = ph >> 23;
    const float f = float(ph & 0x7FFFFFu) * (1.0f / 8388608.0f);
    const float a = plaits::lut_sine[i], b = plaits::lut_sine[i + 1];
    return a + (b - a) * f;
}
inline float decayK(float seconds) { return expf(-6.9f / (clampf(seconds, 0.001f, 60.0f) * dsp::kSr)); }
const float kRatios[9] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 3.5f, 4.0f, 5.0f, 7.0f};
float gTemp1[kBlock], gTemp2[kBlock];
// 2^x for control-rate pitch / cutoff (about 0.1 % error).
inline float fastExp2(float x) {
    x = x < -30.0f ? -30.0f : (x > 30.0f ? 30.0f : x);
    const float fl = float(int32_t(x + 64.0f) - 64);   // floor
    const float f = x - fl;
    const float p = 1.0f + f * (0.6951786f + f * (0.2261642f + f * 0.0782396f));
    union {
        uint32_t i;
        float v;
    } u;
    u.i = uint32_t(int32_t(fl) + 127) << 23;
    return u.v * p;
}
// Cheap soft clip: cubic inside +-1.5, flat outside.
inline float clip3(float x) {
    x = x > 1.5f ? 1.5f : (x < -1.5f ? -1.5f : x);
    return x - x * x * x * (1.0f / 6.75f);
}
}  // namespace

// ---- one channel's voice -------------------------------------------------------------
class Voice {
public:
    using Hat = plaits::HiHat<plaits::SquareNoise, plaits::SwingVCA, true, false>;
    plaits::AnalogBassDrum abd;
    plaits::SyntheticBassDrum sbd;
    plaits::AnalogSnareDrum asd;
    plaits::SyntheticSnareDrum ssd;
    Hat hat;

    bool on = false, trig = false, choking = false;
    uint8_t type = kEnSample;
    int quiet = 0;              // blocks below the silence threshold
    uint32_t age = 0;
    float accent = 0.8f, f0 = 0.001f, declick = 0.0f, last = 0.0f, chokeGain = 1.0f;
    int note = 60, semis = 0;
    uint32_t gateLeft = 0;
    // sample
    const SampleSlot* src = nullptr;
    float pos = 0.0f, inc = 1.0f, endPos = 0.0f, env = 1.0f, envK = 1.0f;
    dsp::Svf svf;
    bool filt = false;
    // acid
    float freq = 110.0f, tfreq = 110.0f, slideK = 0.01f, phase = 0.0f, fenv = 0.0f, fenvK = 0.999f, amp = 0.0f;
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0, cutA = 0.1f, accAmt = 1.0f;
    bool sliding = false, acc = false;
    int ctl = 0;
    // fm
    uint32_t mph = 0, cph = 0, minc = 0, cinc = 0, tcinc = 0;
    float idx = 0.0f, idxK = 0.999f, aenv = 0.0f, attackInc = 1.0f, ampK = 0.999f, prevMod = 0.0f, lp = 0.0f;
    bool attacking = false;

    void init() {
        abd.Init();
        sbd.Init();
        asd.Init();
        ssd.Init();
        hat.Init();
    }

    void choke() {
        if (on) choking = true;
    }

    void noteOn(const Channel& c, const Audio& au, const Pattern& p, int ch, int nt, int vel, uint32_t len, int step,
                Rng32& rng) {
        declick = last;   // the jump from the last sample fades out (no click)
        type = c.type;
        note = nt;
        semis = nt - c.note;
        accent = k01(vel);
        trig = true;
        on = true;
        choking = false;
        chokeGain = 1.0f;
        quiet = 0;
        age = 0;
        switch (type) {
            case kEnKick:
                f0 = dsp::mtof(float(36 + (c.p[1] - 24) + semis)) * dsp::kInvSr;
                break;
            case kEnSnare:
                f0 = dsp::mtof(float(50 + (c.p[1] - 24) + semis)) * dsp::kInvSr;
                break;
            case kEnHat:
                f0 = dsp::mtof(float(56 + (c.p[0] - 24) + semis)) * dsp::kInvSr;
                break;
            case kEnSample: {
                src = au.sourceOf(ch);
                if (!src || !src->ready || src->frames < 64) {
                    on = false;
                    return;
                }
                const uint32_t frames = src->frames;
                const int slices = clampi(c.p[1], 1, 16);
                int slice = 0;
                switch (c.p[2]) {
                    case 0: slice = c.p[3]; break;
                    case 1: slice = step > 0 ? step - 1 : 0; break;
                    case 2: slice = step > 0 ? p.steps[step - 1].degree : 0; break;
                    default: slice = rng.below(slices); break;
                }
                slice = ((slice % slices) + slices) % slices;
                pos = float(uint64_t(frames) * uint32_t(slice) / uint32_t(slices));
                endPos = float(uint64_t(frames) * uint32_t(slice + 1) / uint32_t(slices)) - 2.0f;
                inc = exp2f(float(c.p[4] - 24 + semis) * (1.0f / 12.0f));
                env = accent;
                envK = c.p[5] >= 127 ? 1.0f : decayK(0.02f * powf(150.0f, float(c.p[5]) / 126.0f));
                filt = c.p[6] < 127 || c.p[7] > 0;
                if (filt) svf.set(40.0f * exp2f(float(c.p[6]) * (9.0f / 127.0f)), 0.6f + k01(c.p[7]) * 9.0f);
                break;
            }
            case kEnAcid: {
                tfreq = dsp::mtof(float(nt));
                acc = vel >= 110;
                slideK = 1.0f - decayK(0.01f + k01(c.p[6]) * 0.25f);
                if (c.legato && gateLeft > 0 && amp > 0.01f) {
                    sliding = true;   // 303 slide: no new envelope
                    gateLeft = len;
                    trig = false;
                    declick = 0.0f;
                    return;
                }
                sliding = false;
                freq = tfreq;
                fenv = 1.0f;
                fenvK = decayK((0.08f + k01(c.p[4]) * 1.6f) * (acc ? 0.6f : 1.0f));
                accAmt = acc ? 1.0f + k01(c.p[5]) : 1.0f;
                gateLeft = len;
                break;
            }
            case kEnFm: {
                const float hz = dsp::mtof(float(nt)) * exp2f(float(c.p[6] - 64) * (0.5f / 64.0f / 12.0f));
                tcinc = uint32_t(hz * dsp::kInvSr * 4294967296.0f);
                if (c.legato && gateLeft > 0 && aenv > 0.01f) {
                    gateLeft = len;
                    trig = false;
                    declick = 0.0f;
                    return;
                }
                cinc = tcinc;
                minc = uint32_t(float(cinc) * kRatios[clampi(c.p[0], 0, 8)]);
                idx = k01(c.p[1]) * 5.0f * (0.6f + 0.4f * accent);
                idxK = decayK(0.02f + k01(c.p[2]) * 2.5f);
                attackInc = 1.0f / ((0.001f + k01(c.p[5]) * 0.6f) * dsp::kSr);
                attacking = true;
                ampK = decayK(0.05f + k01(c.p[3]) * 4.0f);
                gateLeft = len;
                break;
            }
            default: break;
        }
    }

    // n samples into out (overwrites).
    void render(const Channel& c, float* out, int n) {
        if (!on) {
            for (int i = 0; i < n; ++i) out[i] = 0.0f;
            return;
        }
        const bool t = trig;
        trig = false;
        switch (type) {
            case kEnKick:
                if (c.p[0] == 0)
                    abd.Render(false, t, accent, f0, k01(c.p[3]), k01(c.p[2]), k01(c.p[4]), k01(c.p[5]), out, size_t(n));
                else
                    sbd.Render(false, t, accent, f0, k01(c.p[3]), k01(c.p[2]), k01(c.p[5]) * 0.6f, k01(c.p[4]),
                               k01(c.p[2]) * 0.8f, out, size_t(n));
                break;
            case kEnSnare:
                if (c.p[0] == 0)
                    asd.Render(false, t, accent, f0, k01(c.p[3]), k01(c.p[2]), k01(c.p[4]), out, size_t(n));
                else
                    ssd.Render(false, t, accent, f0, k01(c.p[3]), k01(c.p[2]), k01(c.p[4]), out, size_t(n));
                break;
            case kEnHat:
                hat.Render(false, t, accent, f0, k01(c.p[2]), k01(c.p[1]), k01(c.p[3]), gTemp1, gTemp2, out, size_t(n));
                break;
            case kEnSample: renderSample(out, n); break;
            case kEnAcid: renderAcid(c, out, n); break;
            case kEnFm: renderFm(c, out, n); break;
            default:
                for (int i = 0; i < n; ++i) out[i] = 0.0f;
                break;
        }
        // declick and choke run only while they are needed
        if (declick != 0.0f) {
            float d = declick;
            for (int i = 0; i < n; ++i) {
                out[i] += d;
                d *= 0.985f;
            }
            declick = (d > 1e-4f || d < -1e-4f) ? d : 0.0f;
        }
        if (choking) {
            float g = chokeGain;
            for (int i = 0; i < n; ++i) {
                g *= 0.993f;
                out[i] *= g;
            }
            chokeGain = g;
            if (g < 0.002f) stop();
        }
        last = out[n - 1];
        age += uint32_t(n);
    }

    // After the block is mixed: the block's peak (pre-fader) decides when the voice sleeps.
    void blockDone(float pk) {
        if (pk < 2.0e-5f && age > 2205) {
            if (++quiet > 4) stop();
        } else {
            quiet = 0;
        }
    }
    float outGain() const { return type == kEnKick ? 1.4f : (type == kEnHat ? 1.6f : 1.0f); }

    void stop() {
        on = false;
        choking = false;
        declick = 0.0f;
        last = 0.0f;
        gateLeft = 0;
        amp = 0.0f;
        aenv = 0.0f;
    }

    // The render loops keep their state in locals: `out` could alias the
    // members otherwise, and every sample would reload and store them.
    void renderSample(float* __restrict out, int n) {
        if (!src || !src->ready) {
            for (int i = 0; i < n; ++i) out[i] = 0.0f;
            stop();
            return;
        }
        const int16_t* __restrict d = src->data;
        float ps = pos, e = env;
        const float in = inc, ek = envK, end = endPos;
        int i = 0;
        if (filt) {
            float ic1 = svf.ic1, ic2 = svf.ic2;
            const float g = svf.g, k = svf.k;
            const float a1 = 1.0f / (1.0f + g * (g + k));
            for (; i < n && ps < end; ++i) {
                const uint32_t j = uint32_t(ps);
                const float f = ps - float(j);
                float x = (float(d[j]) + float(d[j + 1] - d[j]) * f) * (1.0f / 32768.0f);
                const float rem = end - ps;
                if (rem < 96.0f) x *= rem * (1.0f / 96.0f);
                ps += in;
                x *= e;
                e *= ek;
                const float v3 = x - ic2;
                const float v1 = a1 * ic1 + g * a1 * v3;
                const float v2 = ic2 + g * v1;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                out[i] = v2;
            }
            svf.ic1 = ic1;
            svf.ic2 = ic2;
        } else {
            for (; i < n && ps < end; ++i) {
                const uint32_t j = uint32_t(ps);
                const float f = ps - float(j);
                float x = (float(d[j]) + float(d[j + 1] - d[j]) * f) * (1.0f / 32768.0f);
                const float rem = end - ps;
                if (rem < 96.0f) x *= rem * (1.0f / 96.0f);
                ps += in;
                out[i] = x * e;
                e *= ek;
            }
        }
        if (i < n) {
            for (; i < n; ++i) out[i] = 0.0f;
            on = false;
        }
        pos = ps;
        env = e;
    }

    void renderAcid(const Channel& c, float* __restrict out, int n) {
        const float cutBase = 50.0f * exp2f(k01(c.p[1]) * 7.0f);
        const float envMod = k01(c.p[3]) * 4.5f * accAmt;
        const float k = k01(c.p[2]) * 3.7f;
        const float drive = 1.0f + k01(c.p[7]) * 4.0f;
        const bool square = c.p[0] != 0;
        const float rel = 1.0f - 1.0f / (0.006f * dsp::kSr);
        const float gain = (acc ? 0.62f : 0.48f) / (0.6f + 0.4f * drive);
        const float comp = (1.0f + k * 0.35f) * drive;
        float fr = freq, ph = phase, fe = fenv, am = amp, a = cutA;
        float y1 = s1, y2 = s2, y3 = s3, y4 = s4;
        const float tf = tfreq, sk = slideK, fk = fenvK;
        const bool sl = sliding;
        uint32_t gate = gateLeft;
        int cnt = ctl;
        for (int i = 0; i < n; ++i) {
            if (sl) fr += (tf - fr) * sk;
            if ((cnt++ & 7) == 0) {
                float fc = cutBase * fastExp2(envMod * fe);
                if (fc > 16000.0f) fc = 16000.0f;
                const float w = dsp::kTwoPi * fc * dsp::kInvSr;
                a = w / (1.0f + w);
            }
            const float dt = fr * dsp::kInvSr;
            ph += dt;
            if (ph >= 1.0f) ph -= 1.0f;
            float o;
            if (square) {
                o = (ph < 0.5f ? 1.0f : -1.0f) + dsp::Osc::blep(ph, dt);
                float q = ph - 0.5f;
                if (q < 0.0f) q += 1.0f;
                o -= dsp::Osc::blep(q, dt);
            } else {
                o = 2.0f * ph - 1.0f - dsp::Osc::blep(ph, dt);
            }
            const float x = clip3(o - k * y4);
            y1 += a * (x - y1);
            y2 += a * (y1 - y2);
            y3 += a * (y2 - y3);
            y4 += a * (y3 - y4);
            fe *= fk;
            if (gate > 0) {
                --gate;
                am += (1.0f - am) * 0.02f;
            } else {
                am *= rel;
            }
            out[i] = clip3(y4 * comp) * am * gain;
        }
        if (am < 1e-4f && gate == 0) am = 0.0f;
        freq = fr;
        phase = ph;
        fenv = fe;
        amp = am;
        cutA = a;
        s1 = y1;
        s2 = y2;
        s3 = y3;
        s4 = y4;
        gateLeft = gate;
        ctl = cnt;
        if (gateLeft == 0 && amp == 0.0f) on = false;
    }

    void renderFm(const Channel& c, float* __restrict out, int n) {
        const float fbk = k01(c.p[4]) * 0.25f * 4294967296.0f * 0.5f;
        const float tone = 0.05f + k01(c.p[7]) * 0.95f;
        const float ratio = kRatios[clampi(c.p[0], 0, 8)];
        float ix = idx * 4294967296.0f * 0.08f;
        const float ik = idxK, ak = ampK, ai = attackInc;
        uint32_t m = mph, cp = cph, ci = cinc, mi = minc;
        const uint32_t tci = tcinc;
        float pm = prevMod, ae = aenv, y = lp;
        bool att = attacking;
        if (ci != tci) {   // legato glide, once per block
            const int32_t d = int32_t(tci - ci);
            ci += uint32_t(d / 8 + (d > 0 ? 1 : (d < 0 ? -1 : 0)));
            mi = uint32_t(float(ci) * ratio);
        }
        for (int i = 0; i < n; ++i) {
            m += mi;
            const float mod = sine32(m + uint32_t(int32_t(pm * fbk)));
            pm = mod;
            cp += ci;
            const float sv = sine32(cp + uint32_t(int32_t(mod * ix)));
            ix *= ik;
            if (att) {
                ae += ai;
                if (ae >= 1.0f) {
                    ae = 1.0f;
                    att = false;
                }
            } else {
                ae *= ak;
            }
            y += tone * (sv * ae - y);
            out[i] = y * 0.55f;
        }
        idx = ix * (1.0f / (4294967296.0f * 0.08f));
        mph = m;
        cph = cp;
        cinc = ci;
        minc = mi;
        prevMod = pm;
        aenv = ae;
        lp = y;
        attacking = att;
        gateLeft = gateLeft > uint32_t(n) ? gateLeft - uint32_t(n) : 0;
        if (!att && ae < 1e-4f) on = false;
    }
};

// ---- Audio ---------------------------------------------------------------------------------
bool Audio::init(lab::Arena& a) {
    v_ = a.make<Voice>(kCh);
    if (!v_) return false;
    for (int i = 0; i < kCh; ++i) v_[i].init();
    for (int i = 0; i < kCh; ++i) {
        user_[i].data = static_cast<int16_t*>(a.alloc((kUserFrames + 4) * sizeof(int16_t)));
        if (!user_[i].data) return false;
        user_[i].cap = kUserFrames;
    }
    static const uint32_t kBuiltinFrames[kBuiltins] = {70000, 16000, 4000, 18000, 60000, 8000, 6000, 30000};
    for (int i = 0; i < kBuiltins; ++i) {
        builtin_[i].data = static_cast<int16_t*>(a.alloc((kBuiltinFrames[i] + 4) * sizeof(int16_t)));
        if (!builtin_[i].data) return false;
        builtin_[i].cap = kBuiltinFrames[i];
    }
    dlL_ = static_cast<float*>(a.alloc(kDelayFrames * sizeof(float)));
    dlR_ = static_cast<float*>(a.alloc(kDelayFrames * sizeof(float)));
    static const uint32_t kRev[kRevLines] = {1687, 1601, 2053, 2251};
    for (int i = 0; i < kRevLines; ++i) {
        revLen_[i] = kRev[i];
        rev_[i] = static_cast<float*>(a.alloc(kRev[i] * sizeof(float)));
        if (!rev_[i]) return false;
    }
    static const uint32_t kAp[2] = {142, 379};
    for (int i = 0; i < 2; ++i) {
        apLen_[i] = kAp[i];
        ap_[i] = static_cast<float*>(a.alloc(kAp[i] * sizeof(float)));
        if (!ap_[i]) return false;
    }
    return dlL_ && dlR_;
}

const SampleSlot* Audio::sourceOf(int ch) const {
    if (!p_ || ch < 0 || ch >= kCh) return nullptr;
    const Channel& c = p_->ch[ch];
    if (c.sample[0] && user_[ch].ready && user_[ch].frames) return &user_[ch];
    return &builtin_[clampi(c.p[0], 0, kBuiltins - 1)];
}

bool Audio::active(int ch) const { return v_ && ch >= 0 && ch < kCh && v_[ch].on; }

bool Audio::post(const Pending& e) {
    for (auto& q : q_)
        if (!q.used) {
            q = e;
            q.used = 1;
            ++queuedN_;
            return true;
        }
    return false;
}

void Audio::panic() {
    for (auto& q : q_) q.used = 0;
    queuedN_ = 0;
    if (v_)
        for (int i = 0; i < kCh; ++i) v_[i].stop();
}

OT_FASTRUN_FN void Audio::render(uint32_t now, float bpm, float* l, float* r) {
    nFired = 0;
    if (!p_ || !v_) return;
    const Pattern& p = *p_;
    // due events, sorted by offset
    struct Due {
        int off;
        int qi;
    } due[64];
    int nd = 0;
    if (queuedN_ > 0) {
        for (int i = 0; i < kQueue && nd < 64; ++i) {
            Pending& q = q_[i];
            if (!q.used) continue;
            const int32_t d = int32_t(q.at - now);
            if (d >= int32_t(kBlock)) continue;
            int off = d < 0 ? 0 : d;
            int j = nd++;
            while (j > 0 && due[j - 1].off > off) {
                due[j] = due[j - 1];
                --j;
            }
            due[j] = Due{off, i};
        }
    }
    bool anySolo = false;
    for (const auto& c : p.ch) anySolo |= c.solo != 0;
    bool anyA = false, anyB = false;
    for (int s = 0; s < 4; ++s) stemUsed[s] = false;
    static Rng32 rng;
    for (int ch = 0; ch < kCh; ++ch) {
        const Channel& c = p.ch[ch];
        Voice& v = v_[ch];
        const bool audible = !c.mute && (!anySolo || c.solo);
        int at = 0;
        bool touched = v.on;
        for (int k = 0; k < nd; ++k) {
            const Pending& e = q_[due[k].qi];
            const bool mine = (e.kind == Hit::kNote && e.ch == ch) || (e.kind == Hit::kChoke && (e.chokeMask >> ch) & 1);
            if (!mine) continue;
            if (due[k].off > at) {
                v.render(c, chBuf_ + at, due[k].off - at);
                at = due[k].off;
            }
            if (e.kind == Hit::kChoke) {
                v.choke();
            } else if (audible) {
                v.noteOn(c, *this, p, ch, e.note, e.vel, e.len, e.step, rng);
                touched = true;
                chHits[ch] = chHits[ch] + 1;
            }
        }
        if (!touched) {
            chPeak[ch] *= 0.9f;
            continue;
        }
        if (at < int(kBlock)) v.render(c, chBuf_ + at, int(kBlock) - at);
        // peak (every 4th sample is enough for meters and sleep)
        float pk = 0.0f;
        for (uint32_t i = 0; i < kBlock; i += 4) {
            const float a = fabsf(chBuf_[i]);
            pk = a > pk ? a : pk;
        }
        v.blockDone(pk);
        // mix: level, pan (constant power), sends, stems
        const float lv = audible ? k01(c.level) * k01(c.level) * v.outGain() : 0.0f;
        chPeak[ch] = pk * lv > chPeak[ch] ? pk * lv : chPeak[ch] * 0.9f;
        if (lv <= 0.0f) continue;
        if (panOf_[ch] != c.pan) {   // constant power, recomputed when the pan moves
            panOf_[ch] = c.pan;
            const float a = k01(c.pan) * 1.5707963f;
            panL_[ch] = cosf(a) * 1.41421356f;
            panR_[ch] = sinf(a) * 1.41421356f;
        }
        const float gl = lv * panL_[ch], gr = lv * panR_[ch];
        const float* x = chBuf_;
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] += x[i] * gl;
            r[i] += x[i] * gr;
        }
        if (c.sendA) {
            const float sa = k01(c.sendA) * lv;
            for (uint32_t i = 0; i < kBlock; ++i) sendA_[i] = (anyA ? sendA_[i] : 0.0f) + x[i] * sa;
            anyA = true;
        }
        if (c.sendB) {
            const float sb = k01(c.sendB) * lv;
            for (uint32_t i = 0; i < kBlock; ++i) sendB_[i] = (anyB ? sendB_[i] : 0.0f) + x[i] * sb;
            anyB = true;
        }
        if (stemsOn) {
            const int st = clampi(c.stem, 0, 3);
            float* SL = stemL[st];
            float* SR = stemR[st];
            if (!stemUsed[st]) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    SL[i] = x[i] * gl;
                    SR[i] = x[i] * gr;
                }
                stemUsed[st] = true;
            } else {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    SL[i] += x[i] * gl;
                    SR[i] += x[i] * gr;
                }
            }
        }
    }
    for (int k = 0; k < nd; ++k) {
        Pending& e = q_[due[k].qi];
        if (e.kind == Hit::kNote) {
            const Channel& c = p.ch[e.ch];
            if (!c.mute && (!anySolo || c.solo) && nFired < 64) fired[nFired++] = e;
        } else if (e.kind == Hit::kCc && nFired < 64) {
            fired[nFired++] = e;
        }
        e.used = 0;
        --queuedN_;
    }

    // ---- the sends run at half rate (22.05 kHz) and are interpolated back ----
    const Fx& fx = p.fx;
    if (anyA) dlIdle_ = 0;
    if (anyB) revIdle_ = 0;
    const bool runA = dlIdle_ < 400, runB = revIdle_ < 1100;   // tails: ~1.2 s / ~3 s after the last send
    if (runA || runB) {
        constexpr uint32_t kHalf = kBlock / 2;
        float hl[kHalf], hr[kHalf];
        for (uint32_t i = 0; i < kHalf; ++i) hl[i] = hr[i] = 0.0f;
        if (runA) {   // send A: tempo delay, ping-pong by width
            ++dlIdle_;
            const float target = clampf(kDelayTimeBeats[clampi(fx.dlyTime, 0, kDelayTimeCount - 1)] * 60.0f /
                                            (bpm > 20.0f ? bpm : 120.0f) * (dsp::kSr * 0.5f),
                                        32.0f, float(kDelayFrames - 4));
            const float fb = float(fx.dlyFb) * 0.01f;
            const float tone = 0.15f + float(fx.dlyTone) * 0.0085f;
            const float wdt = float(fx.dlyWidth) * 0.01f;
            const float inL = (1.0f - 0.5f * wdt) * 0.5f, inR = 0.25f * wdt;
            float* const DL = dlL_;
            float* const DR = dlR_;
            uint32_t w = dlW_;
            float lpL = dlLpL_, lpR = dlLpR_;
            dlTime_ += (target - dlTime_) * 0.06f;   // the time glides once per block
            float rp = float(w) - dlTime_;
            if (rp < 0.0f) rp += float(kDelayFrames);
            uint32_t i0 = uint32_t(rp);
            const float f = rp - float(i0);
            for (uint32_t i = 0; i < kHalf; ++i) {
                const uint32_t i1 = i0 + 1 >= kDelayFrames ? 0 : i0 + 1;
                const float tl = DL[i0] + (DL[i1] - DL[i0]) * f;
                const float tr = DR[i0] + (DR[i1] - DR[i0]) * f;
                lpL += tone * (tl - lpL);
                lpR += tone * (tr - lpR);
                const float in = anyA ? sendA_[2 * i] + sendA_[2 * i + 1] : 0.0f;
                DL[w] = clip3(in * inL + fb * (lpR * wdt + lpL * (1.0f - wdt)));
                DR[w] = clip3(in * inR + fb * (lpL * wdt + lpR * (1.0f - wdt)));
                if (++w >= kDelayFrames) w = 0;
                i0 = i1;
                hl[i] = lpL;
                hr[i] = lpR;
            }
            dlW_ = w;
            dlLpL_ = lpL;
            dlLpR_ = lpR;
        }
        if (runB) {   // send B: four-line FDN reverb
            ++revIdle_;
            const float g = (0.62f + float(fx.revSize) * 0.0034f) * 0.5f;
            const float damp = 0.2f + (100.0f - float(fx.revDamp)) * 0.0075f;
            const float ret = float(fx.revLevel) * 0.006f;
            float* const A0 = ap_[0];
            float* const A1 = ap_[1];
            float* const R0 = rev_[0];
            float* const R1 = rev_[1];
            float* const R2 = rev_[2];
            float* const R3 = rev_[3];
            uint32_t a0 = apW_[0], a1 = apW_[1], w0 = revW_[0], w1 = revW_[1], w2 = revW_[2], w3 = revW_[3];
            const uint32_t la0 = apLen_[0], la1 = apLen_[1], l0 = revLen_[0], l1 = revLen_[1], l2 = revLen_[2],
                           l3 = revLen_[3];
            float d0 = revLp_[0], d1 = revLp_[1], d2 = revLp_[2], d3 = revLp_[3];
            for (uint32_t i = 0; i < kHalf; ++i) {
                float x = anyB ? (sendB_[2 * i] + sendB_[2 * i + 1]) * 0.5f : 0.0f;
                float d = A0[a0];
                float wv = x + 0.6f * d;
                A0[a0] = wv;
                if (++a0 >= la0) a0 = 0;
                x = d - 0.6f * wv;
                d = A1[a1];
                wv = x + 0.6f * d;
                A1[a1] = wv;
                if (++a1 >= la1) a1 = 0;
                x = d - 0.6f * wv;
                d0 += damp * (R0[w0] - d0);
                d1 += damp * (R1[w1] - d1);
                d2 += damp * (R2[w2] - d2);
                d3 += damp * (R3[w3] - d3);
                const float s1 = d0 + d1, e1 = d0 - d1, s2 = d2 + d3, e2 = d2 - d3;
                R0[w0] = x + g * (s1 + s2);
                R1[w1] = x + g * (e1 + e2);
                R2[w2] = x + g * (s1 - s2);
                R3[w3] = x + g * (e1 - e2);
                if (++w0 >= l0) w0 = 0;
                if (++w1 >= l1) w1 = 0;
                if (++w2 >= l2) w2 = 0;
                if (++w3 >= l3) w3 = 0;
                hl[i] += (d0 + d2) * ret;
                hr[i] += (d1 + d3) * ret;
            }
            apW_[0] = a0;
            apW_[1] = a1;
            revW_[0] = w0;
            revW_[1] = w1;
            revW_[2] = w2;
            revW_[3] = w3;
            revLp_[0] = d0;
            revLp_[1] = d1;
            revLp_[2] = d2;
            revLp_[3] = d3;
        }
        float pl = fxPrevL_, pr = fxPrevR_;
        for (uint32_t i = 0; i < kHalf; ++i) {   // back to 44.1 kHz (linear)
            l[2 * i] += 0.5f * (pl + hl[i]);
            r[2 * i] += 0.5f * (pr + hr[i]);
            l[2 * i + 1] += hl[i];
            r[2 * i + 1] += hr[i];
            pl = hl[i];
            pr = hr[i];
        }
        fxPrevL_ = pl;
        fxPrevR_ = pr;
    }
    // ---- master: drive, DC block, level ----
    const float drive = float(fx.drive) * 0.04f;
    const float mg = k01(fx.master) * 1.1f;
    if (drive > 0.0f) {
        const float post = 1.0f / (1.0f + drive * 0.3f);
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] = clip3(l[i] * (1.0f + drive)) * post;
            r[i] = clip3(r[i] * (1.0f + drive)) * post;
        }
    }
    float xl = dcL_.x1, yl = dcL_.y1, xr = dcR_.x1, yr = dcR_.y1;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float a = l[i], b = r[i];
        yl = a - xl + 0.995f * yl;
        xl = a;
        yr = b - xr + 0.995f * yr;
        xr = b;
        l[i] = yl * mg;
        r[i] = yr * mg;
    }
    dcL_.x1 = xl;
    dcL_.y1 = yl;
    dcR_.x1 = xr;
    dcR_.y1 = yr;
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; i += 4) {
        const float m = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
        pk = m > pk ? m : pk;
    }
    outPeak = pk > outPeak ? pk : outPeak * 0.95f;
}

// ---- built-in samples (main loop, at init) -----------------------------------------------------
namespace {
inline int16_t s16(float v) {
    v = clampf(v, -1.0f, 1.0f) * 32000.0f;
    return int16_t(v);
}
void finish(SampleSlot& s, uint32_t n) {
    s.frames = n < s.cap ? n : s.cap;
    s.data[s.frames] = s.data[s.frames + 1] = 0;
    s.ready = 1;
}
}  // namespace

void Audio::renderBuiltins() {
    dsp::Rng rng(0xB17u);
    // BREAK: one bar at 170 BPM from the Plaits drums, 16 slices of a 16th each
    {
        SampleSlot& s = builtin_[0];
        s.ready = 0;
        const uint32_t sixteenth = uint32_t(44100.0f * 60.0f / 170.0f / 4.0f);
        const uint32_t n = sixteenth * 16 < s.cap ? sixteenth * 16 : s.cap;
        static const char kPat[16] = {'K', 'h', 'K', 'h', 'S', 'h', 'g', 'S', 'h', 'S', 'K', 'K', 'S', 'g', 'h', 'S'};
        Voice* tmp = v_;   // borrow the voices (audio is not running yet)
        Channel kc, sc, hc;
        engineDefaults(kc, kEnKick);
        engineDefaults(sc, kEnSnare);
        engineDefaults(hc, kEnHat);
        sc.p[4] = 90;
        Pattern dummy;
        Rng32 r2;
        float buf[kBlock];
        float out[kBlock];
        uint32_t w = 0;
        for (int st = 0; st < 16; ++st) {
            const char c = kPat[st];
            if (c == 'K') tmp[0].noteOn(kc, *this, dummy, 0, 36, 120, 4000, 0, r2);
            if (c == 'S') tmp[1].noteOn(sc, *this, dummy, 1, 38, 118, 4000, 0, r2);
            if (c == 'g') tmp[1].noteOn(sc, *this, dummy, 1, 38, 50, 4000, 0, r2);
            if (c == 'h' || st % 2 == 0) tmp[2].noteOn(hc, *this, dummy, 2, 42, st % 4 == 0 ? 110 : 70, 2000, 0, r2);
            uint32_t left = sixteenth;
            while (left && w < n) {
                const int k = int(left < kBlock ? left : kBlock);
                for (int i = 0; i < k; ++i) out[i] = 0.0f;
                tmp[0].render(kc, buf, k);
                for (int i = 0; i < k; ++i) out[i] += buf[i] * 0.8f * 1.4f;
                tmp[1].render(sc, buf, k);
                for (int i = 0; i < k; ++i) out[i] += buf[i] * 0.7f;
                tmp[2].render(hc, buf, k);
                for (int i = 0; i < k; ++i) out[i] += buf[i] * 0.45f * 1.6f;
                for (int i = 0; i < k && w < n; ++i) s.data[w++] = s16(dsp::softclip(out[i] * 1.3f));
                left -= uint32_t(k);
            }
        }
        for (int i = 0; i < 3; ++i) tmp[i].stop();
        finish(s, w);
    }
    // CLAP: three noise bursts and a tail through a band-pass
    {
        SampleSlot& s = builtin_[1];
        dsp::Svf bp;
        bp.set(1150.0f, 2.2f);
        const uint32_t n = s.cap;
        for (uint32_t i = 0; i < n; ++i) {
            const float t = float(i) * dsp::kInvSr;
            float env = 0.0f;
            for (int b = 0; b < 3; ++b) {
                const float tb = t - 0.011f * float(b);
                if (tb >= 0.0f && tb < 0.011f) env = expf(-tb * 260.0f);
            }
            if (t >= 0.033f) env = 0.75f * expf(-(t - 0.033f) * 16.0f);
            bp.process(rng.bi() * env);
            s.data[i] = s16(bp.bp * 2.4f);
        }
        finish(s, n);
    }
    // RIM: a short ping and a click
    {
        SampleSlot& s = builtin_[2];
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            const float v = sinf(dsp::kTwoPi * 1720.0f * t) * expf(-t * 90.0f) * 0.8f +
                            (i < 40 ? rng.bi() * 0.7f : 0.0f) + sinf(dsp::kTwoPi * 520.0f * t) * expf(-t * 140.0f) * 0.5f;
            s.data[i] = s16(v);
        }
        finish(s, s.cap);
    }
    // TOM: a falling sine
    {
        SampleSlot& s = builtin_[3];
        float ph = 0.0f;
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            const float f = 95.0f + 110.0f * expf(-t * 18.0f);
            ph += f * dsp::kInvSr;
            if (ph >= 1.0f) ph -= 1.0f;
            s.data[i] = s16(sinf(dsp::kTwoPi * ph) * expf(-t * 7.5f) * 0.95f);
        }
        finish(s, s.cap);
    }
    // BELL: two-operator FM, ratio 3.5
    {
        SampleSlot& s = builtin_[4];
        float pc = 0.0f, pm = 0.0f;
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            pc += 523.25f * dsp::kInvSr;
            pm += 523.25f * 3.5f * dsp::kInvSr;
            if (pc >= 1.0f) pc -= 1.0f;
            if (pm >= 1.0f) pm -= 1.0f;
            const float m = sinf(dsp::kTwoPi * pm) * 3.0f * expf(-t * 3.0f);
            s.data[i] = s16(sinf(dsp::kTwoPi * pc + m) * expf(-t * 2.6f) * 0.7f);
        }
        finish(s, s.cap);
    }
    // ZAP: a fast falling sweep
    {
        SampleSlot& s = builtin_[5];
        float ph = 0.0f;
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            ph += (60.0f + 2400.0f * expf(-t * 45.0f)) * dsp::kInvSr;
            if (ph >= 1.0f) ph -= 1.0f;
            s.data[i] = s16((ph < 0.5f ? 0.6f : -0.6f) * expf(-t * 22.0f));
        }
        finish(s, s.cap);
    }
    // SHAKER: high-passed noise, two bumps
    {
        SampleSlot& s = builtin_[6];
        dsp::Svf hp;
        hp.set(6500.0f, 0.9f);
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            const float env = t < 0.02f ? t / 0.02f : expf(-(t - 0.02f) * 55.0f);
            hp.process(rng.bi() * env);
            s.data[i] = s16(hp.hp * 0.8f);
        }
        finish(s, s.cap);
    }
    // CHORD: a minor-7 saw stab through a closing low-pass
    {
        SampleSlot& s = builtin_[7];
        static const float kSemis[4] = {0.0f, 3.0f, 7.0f, 10.0f};
        float ph[4] = {};
        dsp::Svf lpf;
        for (uint32_t i = 0; i < s.cap; ++i) {
            const float t = float(i) * dsp::kInvSr;
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                ph[k] += 261.63f * exp2f(kSemis[k] / 12.0f) * dsp::kInvSr * (k == 1 ? 1.003f : 1.0f);
                if (ph[k] >= 1.0f) ph[k] -= 1.0f;
                v += 2.0f * ph[k] - 1.0f;
            }
            if ((i & 31) == 0) lpf.set(300.0f + 5000.0f * expf(-t * 9.0f), 1.4f);
            lpf.process(v * 0.25f);
            s.data[i] = s16(lpf.lp * expf(-t * 4.0f) * 1.2f);
        }
        finish(s, s.cap);
    }
}

}  // namespace tilt
