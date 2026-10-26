// The user's own voices, ported to the Tracker (float, no allocation):
//   FLOATER  kick / snare / perc of the FLOATER drum machine (Voices.cpp)
//   TB303    Open303 (Robin Schmidt, MIT) signal chain as in schwung-303 /
//            Schwung Acid, rewritten in float: blend saw-square oscillator,
//            TeeBee filter (TB_303 mode, mystran & kunn coefficients),
//            feedback high-pass, accent RC, de-clicked amp envelope, slides
//   NEURO    the NEUROSTACK neuro-bass models (12) on one master phase
//   QUANTA   the QUANTA microsound particles (Curtis Roads' types)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "ws/dspx.h"
#include "ws/engine_table.h"
#include "ws/model.h"

namespace lab {
namespace konsol {

namespace {
using dx::clampf;
using dx::kInvSr;
using dx::kSr;

// =============================================================================
// FLOATER kit
// =============================================================================
const char* const kDriveMode[2] = {"Overdrive", "Folder"};
const char* const kSlope[2] = {"12 dB", "24 dB"};
const char* const kPercScale[5] = {"Chromatic", "24-tone", "19-tone", "Min pent", "Maj pent"};
enum FlP {
    fKTune, fKShape, fKPEnv, fKPDec, fKFmRatio, fKFmIdx, fKClick, fKDecay, fKDrive, fKMode, fKFold,
    fSTune, fSBody, fSBodyDec, fSColor, fSHpf, fSSlope, fSAttack, fSDecay, fSSnap,
    fPPitch, fPScale, fPRes, fPDecay, fPBits, fPRate, fGain, fFlN
};
const PDesc kFlDesc[fFlN] = {
    {"Tune", 30, 120, 48, 1, kFHz, nullptr},
    {"Shape", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Pitch Env", 0, 48, 24, 1, kFSemi, nullptr},
    {"Pitch Dec", 3, 300, 40, 1, kFMs, nullptr},
    {"FM Ratio", 0.5f, 4, 1.5f, 0.05f, kFNum, nullptr},
    {"FM Index", 0, 1, 0.3f, 0.01f, kFPct, nullptr},
    {"Click", 0, 1, 0.3f, 0.01f, kFPct, nullptr},
    {"Decay", 20, 3000, 450, 10, kFMs, nullptr},
    {"Drive", 0, 1, 0.3f, 0.01f, kFPct, nullptr},
    {"Drive Mode", 0, 1, 0, 1, kFList, kDriveMode},
    {"Fold Table", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Tune", 100, 400, 190, 1, kFHz, nullptr},
    {"Body", 0, 1, 0.45f, 0.01f, kFPct, nullptr},
    {"Body Decay", 10, 1000, 120, 5, kFMs, nullptr},
    {"Color", 0, 1, 0.3f, 0.01f, kFPct, nullptr},
    {"HPF", 40, 4000, 400, 1, kFHz, nullptr},
    {"Slope", 0, 1, 0, 1, kFList, kSlope},
    {"Attack", 0.1f, 20, 1, 0.1f, kFMs, nullptr},
    {"Decay", 10, 2000, 180, 5, kFMs, nullptr},
    {"Snap", 0, 1, 0.4f, 0.01f, kFPct, nullptr},
    {"Pitch", 0, 1, 0.5f, 0.01f, kFPct, nullptr},
    {"Scale", 0, 4, 3, 1, kFList, kPercScale},
    {"Resonance", 0, 1, 0.95f, 0.005f, kFPct, nullptr},
    {"Decay", 10, 4000, 300, 10, kFMs, nullptr},
    {"Bits", 1, 16, 16, 1, kFInt, nullptr},
    {"Rate", 400, 44100, 44100, 100, kFHz, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kFlSec[4] = {{"Kick", 0, 11}, {"Snare", 11, 9}, {"Perc", 20, 6}, {"Out", 26, 1}};

float foldTransfer(float u, float table) {
    u = clampf(u, -1.0f, 1.0f);
    const float a = dx::sin2pi(dx::wrap(0.75f * u));
    const float u2 = u * u, u3 = u2 * u, u5 = u3 * u2, u7 = u5 * u2;
    const float t3 = 4.0f * u3 - 3.0f * u;
    const float t5 = 16.0f * u5 - 20.0f * u3 + 5.0f * u;
    const float t7 = 64.0f * u7 - 112.0f * u5 + 56.0f * u3 - 7.0f * u;
    const float b = 0.5f * t3 + 0.3f * t5 + 0.2f * t7;
    return a + (b - a) * clampf(table, 0.0f, 1.0f);
}
float percQuantize(float semis, int scale) {
    switch (scale) {
        case 1: return dx::ffloor(semis * 2.0f + 0.5f) * 0.5f;
        case 2: {
            const float st = 12.0f / 19.0f;
            return dx::ffloor(semis / st + 0.5f) * st;
        }
        case 3:
        case 4: {
            static const float mi[5] = {0, 3, 5, 7, 10}, ma[5] = {0, 2, 4, 7, 9};
            const float* d = scale == 3 ? mi : ma;
            const float oct = dx::ffloor(semis / 12.0f);
            float best = 0, dist = 1e9f;
            for (int o = -1; o <= 1; ++o)
                for (int k = 0; k < 5; ++k) {
                    const float c = (oct + float(o)) * 12.0f + d[k];
                    const float dd = fabsf(c - semis);
                    if (dd < dist) {
                        dist = dd;
                        best = c;
                    }
                }
            return best;
        }
        default: return dx::ffloor(semis + 0.5f);
    }
}

class FlKit final : public TableEngine {
public:
    FlKit() {
        bind(kFlDesc, fFlN, kFlSec, 4);
        dcA_ = expf(-dx::kTwoPi * 12.0f * kInvSr);
    }
    int kind() const override { return kEngFloater; }
    void noteOn(int pitch, float vel, int offset, int tone, int) override {
        const int w = offset > 0 && offset < int(kBlock) ? offset : 0;
        if (pitch == 36 || pitch == 35 || pitch == 24 || pitch == 48) {
            K& k = k_;
            k.on = true;
            k.wait = w;
            k.vel = vel;
            k.amp = 1;
            k.pe = 1;
            k.fe = 1;
            k.ce = 1;
            k.ph = 0;
            k.fph = 0;
            k.tone = float(tone) * 0.1f;
        } else if (pitch == 38 || pitch == 40 || pitch == 37) {
            S& s = s_;
            s.on = true;
            s.wait = w;
            s.vel = vel;
            s.body = 1;
            s.snap = 1;
            s.attacking = true;
            s.ph[0] = s.ph[1] = s.ph[2] = 0;
            s.tone = float(tone) * 0.1f;
        } else {
            P& c = c_;
            c.on = true;
            c.wait = w;
            c.vel = vel;
            c.impulse = 1;
            c.burst = 1;
            c.amp = 1;
            c.note = float(pitch - 60) + float(tone) * 0.1f;
        }
    }
    void noteOff(int) override {}
    void allOff(bool hard) override {
        if (hard) k_.on = s_.on = c_.on = false;
    }
    void render(float* l, float* r) override {
        float buf[kBlock];
        bool any = false;
        for (uint32_t i = 0; i < kBlock; ++i) buf[i] = 0;
        if (k_.on) {
            renderKick(buf);
            any = true;
        }
        if (s_.on) {
            renderSnare(buf);
            any = true;
        }
        if (c_.on) {
            renderPerc(buf);
            any = true;
        }
        if (!any) return;
        const float g = p_[fGain] * 0.8f;
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] += buf[i] * g;
            r[i] += buf[i] * g;
        }
    }
    int active() const override { return int(k_.on) + int(s_.on) + int(c_.on); }
    // QEMU-measured (tests: eprobe): kick 39k, snare 30k, perc 11k per block
    int32_t cost() const override {
        return (k_.on ? 39000 : 0) + (s_.on ? 30000 : 0) + (c_.on ? 11000 : 0) + (active() ? 700 : 0);
    }
    int32_t noteCost() const override { return 39000; }
    bool steal() override {
        if (c_.on) return !(c_.on = false);
        if (s_.on) return !(s_.on = false);
        if (k_.on) return !(k_.on = false);
        return false;
    }

private:
    struct K {
        bool on = false;
        int wait = 0;
        float vel = 1, amp = 0, pe = 0, fe = 0, ce = 0, ph = 0, fph = 0, lastX = 0, tone = 0, dcX = 0, dcY = 0;
    } k_;
    struct S {
        bool on = false, attacking = false;
        int wait = 0;
        float vel = 1, body = 0, noise = 0, snap = 0, ph[3] = {}, tone = 0, pk0 = 0, pk1 = 0, pk2 = 0;
        dx::Svf hp1, hp2;
    } s_;
    struct P {
        bool on = false;
        int wait = 0;
        float vel = 1, impulse = 0, burst = 0, amp = 0, note = 0, held = 0, crush = 0;
        dx::Svf bp1, bp2, aa1, aa2;
    } c_;
    dx::Noise rng_;
    float dcA_ = 0.998f;

    void renderKick(float* out) {
        K k = k_;   // locals: the output stores cannot alias the state
        dx::Noise rng = rng_;
        const float* p = p_;
        const float ampK = dx::decayK(p[fKDecay]);
        const float pitchK = dx::tauK(p[fKPDec] * 0.001f);
        const float fmK = dx::tauK(0.025f), clickK = dx::tauK(0.004f);
        const float drive = p[fKDrive];
        const bool folder = p[fKMode] > 0.5f;
        const float baseHz = p[fKTune] * dx::semis(k.tone);
        const float shape = p[fKShape], fmR = p[fKFmRatio], fmI = p[fKFmIdx] * 1.6f, click = p[fKClick] * 0.5f;
        const float g1 = 1.0f + 5.0f * drive, g2 = 1.0f + 12.0f * drive * drive, bias = 0.35f * drive;
        const float sb = dx::soft(bias), odNorm = 1.0f / (1.0f + 0.6f * drive);
        const float foldMix = 0.45f + 0.35f * drive, foldTab = p[fKFold];
        const float pEnvOct = p[fKPEnv] * (1.0f / 12.0f);
        const bool tri = shape > 0.001f;
        for (uint32_t i = uint32_t(k.wait); i < kBlock; ++i) {
            const float hz = baseHz * dx::exp2fast(pEnvOct * k.pe);
            float step = hz * (0.5f * kInvSr);
            if (step > 0.45f) step = 0.45f;
            const bool fm = k.fe > 1e-4f, clk = k.ce > 1e-3f;
            const float fmAmt = fmI * k.fe, clkAmt = k.ce * k.ce * click;
            float y = 0;
            for (int s2 = 0; s2 < 2; ++s2) {
                k.ph += step;
                if (k.ph >= 1.0f) k.ph -= 1.0f;
                float fr = k.ph;
                if (fm) {
                    k.fph += step * fmR;
                    if (k.fph >= 1.0f) k.fph -= dx::ffloor(k.fph);
                    fr = dx::wrap(k.ph + dx::sin2pi(k.fph) * fmAmt);
                }
                const float sine = dx::sin2pi(fr);
                float x = sine;
                if (tri) x += (-(1.0f - 4.0f * fabsf(fr - 0.5f)) - sine) * shape;
                x *= k.amp;
                if (clk) x += clkAmt * (0.6f * rng.bi() + 0.8f);
                if (folder) {
                    float u = x * g1;
                    u = 4.0f * fabsf(0.25f * u + 0.25f - dx::ffloor(0.25f * u + 0.75f) + 0.5f) - 1.0f;
                    x = dx::soft(0.6f * x + foldMix * foldTransfer(u, foldTab) * k.amp);
                } else {
                    x = (dx::soft(x * g2 + bias) - sb) * odNorm;
                }
                y += 0.5f * (x + k.lastX);
                k.lastX = x;
            }
            y *= 0.5f;
            // DC high-pass (12 Hz)
            const float hp = dcA_ * (k.dcY + y - k.dcX);
            k.dcX = y;
            k.dcY = hp;
            out[i] += hp * k.vel;
            k.amp *= ampK;
            k.pe *= pitchK;
            k.fe *= fmK;
            k.ce *= clickK;
        }
        k.wait = 0;
        if (k.amp < 1e-4f) k.on = false;
        k_ = k;
        rng_ = rng;
    }
    void renderSnare(float* out) {
        S s = s_;
        dx::Noise rng = rng_;
        float pb0 = pinkB0_, pb1 = pinkB1_, pb2 = pinkB2_;
        const float* p = p_;
        const float hz = clampf(p[fSHpf], 20.0f, 12000.0f);
        s.hp1.set(hz, 0.7071f);
        s.hp2.set(hz, 0.7071f);
        const float bodyK = dx::tauK(p[fSBodyDec] * 0.001f);
        const float noiseK = dx::tauK(p[fSDecay] * 0.001f), snapK = dx::tauK(0.012f);
        const float attackStep = 1.0f / (p[fSAttack] * 0.001f * kSr > 1.0f ? p[fSAttack] * 0.001f * kSr : 1.0f);
        static const float ratio[3] = {1.0f, 1.59f, 2.14f}, gain[3] = {1.0f, 0.55f, 0.3f};
        const float bodyAmt = p[fSBody], noiseAmt = 1.0f - 0.6f * bodyAmt;
        // the upper modes die faster: body^(1 + 0.7 k) per mode
        const float k1 = powf(bodyK, 1.7f), k2 = powf(bodyK, 2.4f);
        if (s.pk0 == 0 && s.body >= 1.0f) {
            s.pk0 = s.pk1 = s.pk2 = 1.0f;
        }
        const float tune = p[fSTune] * dx::semis(s.tone);
        const bool slope24 = p[fSSlope] > 0.5f;
        float pk0 = s.pk0, pk1 = s.pk1, pk2 = s.pk2;
        if (s.body >= 1.0f) pk0 = pk1 = pk2 = 1.0f;
        const float inc0 = tune * ratio[0] * kInvSr, inc1 = tune * ratio[1] * kInvSr, inc2 = tune * ratio[2] * kInvSr;
        const float snapBend = 0.5f * p[fSSnap];
        for (uint32_t i = uint32_t(s.wait); i < kBlock; ++i) {
            const float bend = 1.0f + snapBend * s.snap;
            s.ph[0] += inc0 * bend;
            s.ph[1] += inc1 * bend;
            s.ph[2] += inc2 * bend;
            if (s.ph[0] >= 1.0f) s.ph[0] -= 1.0f;
            if (s.ph[1] >= 1.0f) s.ph[1] -= 1.0f;
            if (s.ph[2] >= 1.0f) s.ph[2] -= 1.0f;
            float body = dx::sin2pi(s.ph[0]) * gain[0] * pk0;
            if (pk1 > 1e-4f) body += dx::sin2pi(s.ph[1]) * gain[1] * pk1 + dx::sin2pi(s.ph[2]) * gain[2] * pk2;
            if (s.attacking) {
                s.noise += attackStep;
                if (s.noise >= 1.0f) {
                    s.noise = 1.0f;
                    s.attacking = false;
                }
            } else {
                s.noise *= noiseK;
            }
            const float white = rng.bi();
            pb0 = 0.99765f * pb0 + white * 0.0990460f;
            pb1 = 0.96300f * pb1 + white * 0.2965164f;
            pb2 = 0.57000f * pb2 + white * 1.0526913f;
            const float pink = clampf((pb0 + pb1 + pb2 + white * 0.1848f) * 0.75f, -1.5f, 1.5f);
            float noise = white + (pink - white) * p[fSColor];
            noise = s.hp1.high(noise);
            if (slope24) noise = s.hp2.high(noise);
            const float snap = s.snap * s.snap * rng.bi() * p[fSSnap] * 0.6f;
            const float y = (body * bodyAmt * 0.7f + noise * s.noise * noiseAmt + snap) * s.vel;
            pk0 *= bodyK;
            pk1 *= k1;
            pk2 *= k2;
            s.body = pk0;
            s.snap *= snapK;
            out[i] += dx::soft(y);
        }
        s.pk0 = pk0;
        s.pk1 = pk1;
        s.pk2 = pk2;
        s.wait = 0;
        if (s.noise < 1e-4f && pk0 < 1e-4f && !s.attacking) {
            s.on = false;
            s.pk0 = 0;
        }
        s_ = s;
        rng_ = rng;
        pinkB0_ = pb0;
        pinkB1_ = pb1;
        pinkB2_ = pb2;
    }
    void renderPerc(float* out) {
        P c = c_;
        dx::Noise rng = rng_;
        const float* p = p_;
        const float semis = percQuantize(48.0f * p[fPPitch] + c.note, int(p[fPScale] + 0.5f));
        const float hz = clampf(110.0f * dx::semis(semis), 30.0f, kSr * 0.4f);
        const float res = p[fPRes];
        const float r3 = res * res * res;
        const float q = 0.7f + 2000.0f * r3 * r3;
        c.bp1.set(hz, q);
        c.bp2.set(hz, q < 60.0f ? q : 60.0f);
        const float ampK = dx::tauK(p[fPDecay] * 0.001f), burstK = dx::tauK(0.0008f);
        const float bits = p[fPBits];
        const float levels = dx::exp2fast(bits - 1.0f);
        const float rate = p[fPRate];
        const bool crushRate = rate < kSr * 0.99f;
        if (crushRate) {
            c.aa1.set(rate * 0.45f, 0.54f);
            c.aa2.set(rate * 0.45f, 1.31f);
        }
        const float makeUp = 1.0f + 6.0f * (1.0f - res);
        for (uint32_t i = uint32_t(c.wait); i < kBlock; ++i) {
            const float x = c.impulse * 4.0f + c.burst * c.burst * rng.bi() * 0.5f;
            c.impulse = 0;
            c.burst *= burstK;
            float y = c.bp2.band(dx::soft(c.bp1.band(x))) * makeUp;
            y = dx::soft(y) * c.amp * c.vel;
            c.amp *= ampK;
            float z = y;
            if (crushRate) {
                y = c.aa2.low(c.aa1.low(y));
                c.crush += rate * kInvSr;
                if (c.crush >= 1.0f) {
                    c.crush -= dx::ffloor(c.crush);
                    c.held = y;
                }
                z = c.held;
            }
            if (bits < 15.99f) z = dx::ffloor(z * levels + 0.5f) / levels;
            out[i] += z;
        }
        c.wait = 0;
        if (c.amp < 1e-4f) c.on = false;
        c_ = c;
        rng_ = rng;
    }
    float pinkB0_ = 0, pinkB1_ = 0, pinkB2_ = 0;
};

// =============================================================================
// TB303 (Open303 in float)
// =============================================================================
enum TbP { tWave, tCutoff, tReso, tEnvMod, tDecay, tAccent, tSlide, tAccDecay, tFbHp, tTune, tDrive, tVolume, tTbN };
const PDesc kTbDesc[tTbN] = {
    {"Waveform", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Cutoff", 0, 1, 0.45f, 0.005f, kFPct, nullptr},
    {"Resonance", 0, 1, 0.7f, 0.005f, kFPct, nullptr},
    {"Env Mod", 0, 1, 0.45f, 0.005f, kFPct, nullptr},
    {"Decay", 0, 1, 0.4f, 0.005f, kFPct, nullptr},
    {"Accent", 0, 1, 0.6f, 0.01f, kFPct, nullptr},
    {"Slide", 5, 300, 60, 1, kFMs, nullptr},
    {"Acc Decay", 30, 3000, 200, 10, kFMs, nullptr},
    {"Fb HPF", 100, 350, 150, 1, kFHz, nullptr},
    {"Tuning", -12, 12, 0, 1, kFSemi, nullptr},
    {"Drive", 0, 1, 0.15f, 0.01f, kFPct, nullptr},
    {"Volume", 0, 1, 0.8f, 0.01f, kFPct, nullptr},
};
const SecDesc kTbSec[3] = {{"Voice", 0, 6}, {"Devilfish", 6, 3}, {"Out", 9, 3}};

class Tb303 final : public TableEngine {
public:
    Tb303() {
        bind(kTbDesc, tTbN, kTbSec, 3);
        // fixed filters of the chain (Open303 "tweakables")
        hp1A_ = expf(-dx::kTwoPi * 44.486f / (2.0f * kSr));   // at the 2x rate
        hp2A_ = expf(-dx::kTwoPi * 24.167f * kInvSr);
        const float t = tanf(dx::kPi * 14.008f * kInvSr);
        apA_ = (t - 1.0f) / (t + 1.0f);
        rc2K_ = expf(-1.0f / (kSr * 0.001f * 15.0f));
        declickK_ = expf(-dx::kTwoPi * 200.0f * kInvSr);
    }
    int kind() const override { return kEngTb303; }
    void noteOn(int pitch, float vel, int, int tone, int flags) override {
        const bool accent = vel > 0.86f || (flags & kFlagAccent);
        const bool slide = (flags & kFlagSlide) && held_ > 0;
        const float hz = 440.0f * dx::exp2fast((float(pitch) + p_[tTune] - 69.0f) * (1.0f / 12.0f));
        target_ = hz;
        toneOct_ = float(tone) * (1.0f / 48.0f);
        if (!slide) {
            if (idle_) {   // reset only from silence (no clicks)
                phase_ = 0;
                y1_ = y2_ = y3_ = y4_ = 0;
                fbX_ = fbY_ = 0;
                hp1X_ = hp1Y_ = hp2X_ = hp2Y_ = apX_ = apY_ = 0;
            }
            freq_ = hz;
            mainEnv_ = 1.0f;   // Open303: the decay envelope restarts
            ampNoteOn_ = true;
            ampPhase_ = 0;
        }
        accentGain_ = accent ? p_[tAccent] : 0.0f;
        const float decMs = accent ? p_[tAccDecay] : 200.0f * expf(p_[tDecay] * logf(10.0f));   // 200..2000 ms
        mainK_ = expf(-1.0f / (kSr * 0.001f * decMs * 0.25f));
        ampRelK_ = expf(-1.0f / (kSr * 0.001f * (accent ? 50.0f : 1.0f)));
        ++held_;
        lastPitch_ = pitch;
        idle_ = false;
    }
    void noteOff(int pitch) override {
        if (held_ > 0) --held_;
        if (pitch == lastPitch_ || held_ == 0) {
            held_ = 0;
            ampNoteOn_ = false;
        }
    }
    void allOff(bool hard) override {
        held_ = 0;
        ampNoteOn_ = false;
        if (hard) {
            idle_ = true;
            amp_ = 0;
        }
    }
    void render(float* l, float* r) override {
        if (idle_) return;
        const float* p = p_;
        // Open303: cutoff 314..2394 Hz (exp), env mod mapping from measurements
        const float cut = 314.0f * expf(p[tCutoff] * logf(2394.0f / 314.0f));
        const float e = p[tEnvMod];
        const float c = p[tCutoff];
        const float envScaler = (1.0f - c) * (3.773996f * e + 0.736966f) + c * (4.194549f * e + 0.864345f);
        const float envOffset = 0.0482929f * c + 0.2943912f;
        const float rraw = p[tReso];
        const float rsk = (1.0f - expf(-3.0f * rraw)) / (1.0f - expf(-3.0f));
        const float slideK = expf(-1.0f / (kSr * 0.001f * (0.2f * p[tSlide] > 0.1f ? 0.2f * p[tSlide] : 0.1f)));
        const float wave = p[tWave];
        const float fbA = expf(-dx::kTwoPi * p[tFbHp] / (2.0f * kSr));
        const float vol = p[tVolume] * p[tVolume] * 0.9f;
        const float drive = p[tDrive];
        const float dpre = 1.0f + 6.0f * drive, dpost = 1.0f / (1.0f + 2.0f * drive);
        const float ampDecK = expf(-1.0f / (kSr * 0.001f * 1230.0f * 0.25f));
        const float invSr2 = 0.5f * kInvSr;
        float b0 = 0, k = 0, g = 1;
        float ob[kBlock];   // a local output: the member state stays in registers
        for (uint32_t i = 0; i < kBlock; ++i) {
            freq_ = target_ + slideK * (freq_ - target_);
            // filter envelope: main decay + accent RC
            const float mainOut = mainEnv_;
            mainEnv_ *= mainK_;
            rc2_ = (accentGain_ > 0 ? mainOut : 0.0f) + rc2K_ * (rc2_ - (accentGain_ > 0 ? mainOut : 0.0f));
            if ((i & 3) == 0) {   // filter coefficients at a quarter of the rate
                const float t1 = envScaler * (mainOut - envOffset);
                const float t2 = accentGain_ * rc2_;
                float fc = cut * dx::exp2fast(t1 + t2 + toneOct_);
                fc = clampf(fc, 200.0f, 20000.0f);
                // TB_303 coefficients (mystran & kunn) at the 2x rate
                const float fx = fc * invSr2 * 0.70710678f;
                b0 = (0.00045522346f + 6.1922189f * fx) / (1.0f + 12.358354f * fx + 4.4156345f * (fx * fx));
                k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f) - 476.47308f) + 614.95611f) + 213.87126f) +
                    16.998792f;
                g = k * (1.0f / 17.0f);
                g = (g - 1.0f) * rsk + 1.0f;
                g = g * (1.0f + rsk);
                k = k * rsk;
            }
            // amp envelope (attack 0, decay 1230 ms, sustain 0) + the main env push
            if (ampNoteOn_) {
                amp_ = amp_ + (1.0f - amp_) * (ampPhase_ < 3 ? 1.0f : 0.0f);
                ++ampPhase_;
                amp_ *= ampDecK;
            } else {
                amp_ *= ampRelK_;
            }
            float ampOut = amp_;
            if (ampNoteOn_) ampOut += 0.45f * mainOut + accentGain_ * 4.0f * mainOut;
            declick_ = ampOut + declickK_ * (declick_ - ampOut);
            float tmp = 0;
            const float dt = freq_ * invSr2;
            for (int o = 0; o < 2; ++o) {
                phase_ += dt;
                if (phase_ >= 1.0f) phase_ -= 1.0f;
                const float saw = dx::sawBlep(phase_, dt);
                float x = -saw;
                if (wave > 0.001f) {
                    float sq = phase_ < 0.5f ? 1.0f : -1.0f;
                    sq += dx::blep(phase_, dt);
                    float q2 = phase_ - 0.5f;
                    if (q2 < 0) q2 += 1.0f;
                    sq -= dx::blep(q2, dt);
                    x = -(saw + (sq - saw) * wave);
                }
                // pre-filter high-pass
                const float h = hp1A_ * (hp1Y_ + x - hp1X_);
                hp1X_ = x;
                hp1Y_ = h;
                // TeeBee filter (TB_303 mode): feedback through a one-pole HP
                const float fbIn = k * y4_;
                const float fb = fbA * (fbY_ + fbIn - fbX_);
                fbX_ = fbIn;
                fbY_ = fb;
                const float y0 = h - fb;
                y1_ += 2.0f * b0 * (y0 - y1_ + y2_);
                y2_ += b0 * (y1_ - 2.0f * y2_ + y3_);
                y3_ += b0 * (y2_ - 2.0f * y3_ + y4_);
                y4_ += b0 * (y3_ - 2.0f * y4_);
                tmp = 2.0f * g * y4_;
            }
            // post: all-pass, high-pass
            const float ap = apA_ * tmp + apX_ - apA_ * apY_;
            apX_ = tmp;
            apY_ = ap;
            const float h2 = hp2A_ * (hp2Y_ + ap - hp2X_);
            hp2X_ = ap;
            hp2Y_ = h2;
            float y = h2 * declick_ * vol;
            if (drive > 0.01f) y = dx::soft(y * dpre) * dpost;
            ob[i] = y;
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] += ob[i];
            r[i] += ob[i];
        }
        if (!ampNoteOn_ && declick_ < 1e-4f && amp_ < 1e-4f) idle_ = true;
        if (fabsf(y4_) > 1e6f || !(y4_ == y4_)) {   // safety
            y1_ = y2_ = y3_ = y4_ = 0;
        }
    }
    int active() const override { return idle_ ? 0 : 1; }
    int32_t cost() const override { return idle_ ? 0 : 30500; }   // QEMU: 29.7k (drive on)
    int32_t noteCost() const override { return idle_ ? 30500 : 0; }
    bool steal() override {
        if (idle_) return false;
        idle_ = true;
        return true;
    }

private:
    float phase_ = 0, freq_ = 110, target_ = 110, toneOct_ = 0;
    float y1_ = 0, y2_ = 0, y3_ = 0, y4_ = 0, fbX_ = 0, fbY_ = 0;
    float hp1A_ = 0.99f, hp1X_ = 0, hp1Y_ = 0, hp2A_ = 0.99f, hp2X_ = 0, hp2Y_ = 0, apA_ = 0, apX_ = 0, apY_ = 0;
    float mainEnv_ = 0, mainK_ = 0.999f, rc2_ = 0, rc2K_ = 0.99f, accentGain_ = 0;
    float amp_ = 0, ampRelK_ = 0.9f, declick_ = 0, declickK_ = 0.97f;
    int ampPhase_ = 0;
    bool ampNoteOn_ = false, idle_ = true;
    int held_ = 0, lastPitch_ = -1;
};

// =============================================================================
// NEURO (NEUROSTACK neuro-bass models)
// =============================================================================
const char* const kNeuroModels[12] = {"Reese", "Wobble", "Growl", "FM Stab", "Hoover", "Talk",
                                      "Screech", "Snarl", "808 Drop", "Solid FM", "Hollow", "Semi Sines"};
const char* const kLfoSync[8] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "Free"};
const char* const kSubOct[2] = {"0", "-1 oct"};
enum NeP { nModel, nTone, nMotion, nGrit, nLfo, nLfoHz, nEnvDec, nSub, nSubOct, nSubDrive, nDrive, nGlide, nAttack,
           nRelease, nGain, nNeN };
const PDesc kNeDesc[nNeN] = {
    {"Model", 0, 11, 0, 1, kFList, kNeuroModels},
    {"Tone", 0, 1, 0.5f, 0.01f, kFPct, nullptr},
    {"Motion", 0, 1, 0.5f, 0.01f, kFPct, nullptr},
    {"Grit", 0, 1, 0.3f, 0.01f, kFPct, nullptr},
    {"LFO Sync", 0, 7, 2, 1, kFList, kLfoSync},
    {"LFO Free", 0.1f, 20, 2, 0.1f, kFNum, nullptr},
    {"Env Decay", 20, 2000, 250, 10, kFMs, nullptr},
    {"Sub", 0, 1, 0.7f, 0.01f, kFPct, nullptr},
    {"Sub Oct", 0, 1, 0, 1, kFList, kSubOct},
    {"Sub Drive", 0, 1, 0.15f, 0.01f, kFPct, nullptr},
    {"Drive", 0, 1, 0.25f, 0.01f, kFPct, nullptr},
    {"Glide", 0, 500, 0, 5, kFMs, nullptr},
    {"Attack", 0, 200, 2, 1, kFMs, nullptr},
    {"Release", 5, 2000, 80, 5, kFMs, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kNeSec[4] = {{"Model", 0, 4}, {"Motion", 4, 3}, {"Sub/Drive", 7, 4}, {"Amp", 11, 4}};

class Neuro final : public TableEngine {
public:
    Neuro() { bind(kNeDesc, nNeN, kNeSec, 4); }
    int kind() const override { return kEngNeuro; }
    void setTempo(float bpm) override { bpm_ = bpm; }
    void noteOn(int pitch, float vel, int, int tone, int flags) override {
        const float hz = dx::mtof(float(pitch));
        target_ = hz;
        if (!(flags & kFlagSlide) || !gate_ || p_[nGlide] < 1.0f) f0_ = hz;
        if (!gate_ || !(flags & kFlagSlide)) e_ = 1.0f;   // the trigger envelope
        gate_ = true;
        vel_ = vel;
        tone_ = float(tone) * (1.0f / 128.0f);
        pitch_ = pitch;
        on_ = true;
    }
    void noteOff(int pitch) override {
        if (pitch == pitch_) gate_ = false;
    }
    void allOff(bool hard) override {
        gate_ = false;
        if (hard) {
            on_ = false;
            env_ = 0;
        }
    }
    void render(float* l, float* r) override {
        if (!on_) return;
        const float* p = p_;
        const int model = int(p[nModel] + 0.5f);
        const float tone = clampf(p[nTone] + tone_, 0.0f, 1.0f), motion = p[nMotion], grit = p[nGrit];
        static const float kBeats[7] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        const int ls = int(p[nLfo] + 0.5f);
        const float lfoHz = ls >= 7 ? p[nLfoHz] : (bpm_ / 60.0f) / kBeats[ls];
        const float glideK = p[nGlide] < 1.0f ? 0.0f : expf(-1.0f / (kSr * 0.001f * p[nGlide]));
        const float atk = 1.0f / (p[nAttack] * 0.001f * kSr + 1.0f);
        const float rel = 1.0f / (p[nRelease] * 0.001f * kSr + 1.0f);
        const float eK = dx::tauK(p[nEnvDec] * 0.001f);
        const float sub = p[nSub], subDrive = p[nSubDrive], drive = p[nDrive];
        const float subMul = p[nSubOct] > 0.5f ? 0.5f : 1.0f;
        const float subPre = 1.0f + 3.0f * subDrive, subNorm = 1.0f / dx::soft(subPre);
        const float drvPre = 1.0f + 3.0f * drive, drvNorm = 1.0f / (1.0f + drive);
        const float gain = p[nGain] * vel_ * 0.5f;
        float ob[kBlock];
        // control rate (every 16 samples): filters
        for (uint32_t i0 = 0; i0 < kBlock; i0 += 16) {
            f0_ = target_ + glideK * (f0_ - target_);
            lfo_ += lfoHz * 16.0f * kInvSr;
            lfo_ -= dx::ffloor(lfo_);
            const float lfoSin = dx::sin2pi(lfo_);
            const float lfoUni = 0.5f + 0.5f * lfoSin;
            const float f0 = f0_;
            setFilters(model, f0, tone, motion, lfoUni);
            const float dphi = f0 * kInvSr;
            for (uint32_t i = i0; i < i0 + 16; ++i) {
                if (gate_) env_ += (1.0f - env_) * atk;
                else env_ -= env_ * rel * 4.0f;
                phi_ += dphi;
                if (phi_ >= 1.0f) phi_ -= 1.0f;
                float y = modelSample(model, dphi, tone, motion, grit, lfoUni, lfoSin);
                e_ *= eK;
                // the sub: a sine on the master phase (or an octave down)
                subPh_ += dphi * subMul;
                if (subPh_ >= 1.0f) subPh_ -= 1.0f;
                float s = dx::sin2pi(subPh_);
                if (subDrive > 0.01f) s = dx::soft(s * subPre) * subNorm;
                y = dx::soft(y * drvPre) * drvNorm;
                ob[i] = (y * 0.8f + s * sub * 0.7f) * env_ * gain;
            }
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] += ob[i];
            r[i] += ob[i];
        }
        if (!gate_ && env_ < 1e-4f) on_ = false;
    }
    int active() const override { return on_ ? 1 : 0; }
    int32_t cost() const override { return on_ ? modelCost() : 0; }
    int32_t noteCost() const override { return on_ ? 0 : modelCost(); }
    bool steal() override {
        if (!on_) return false;
        on_ = false;
        return true;
    }

private:
    int32_t modelCost() const {
        // QEMU-measured M4 instructions per block, per model (+ a margin)
        static const int32_t kC[12] = {27400, 23600, 31300, 27000, 38500, 28300, 24800, 29500, 23800, 27700, 21200, 28100};
        const int m = int(p_[nModel] + 0.5f);
        return kC[m < 0 ? 0 : (m > 11 ? 11 : m)] + 1000;
    }
    void setFilters(int m, float f0, float tone, float motion, float lfoUni) {
        switch (m) {
            case 0: f1_.set(f0 * (1.5f + 46.0f * tone * tone), 0.7f + 0.8f * motion); break;
            case 1: f1_.set(f0 * (1.2f + (2.0f + 60.0f * tone) * lfoUni * lfoUni), 2.2f); break;
            case 2: {
                const float v = lfoUni;
                const float fA = v < 0.5f ? 730.0f + (570.0f - 730.0f) * v * 2.0f : 570.0f + (300.0f - 570.0f) * (v * 2.0f - 1.0f);
                const float fB = v < 0.5f ? 1090.0f + (840.0f - 1090.0f) * v * 2.0f : 840.0f + (870.0f - 840.0f) * (v * 2.0f - 1.0f);
                f1_.set(fA, 5.0f);
                f2_.set(fB, 6.0f);
                break;
            }
            case 4: f1_.set(f0 * 14.0f, 0.7f); break;
            case 5: {
                const float t = 1.0f - e_, sp = 0.6f + 0.8f * tone;
                f1_.set(570.0f + (270.0f - 570.0f) * t, 5.0f * sp);
                f2_.set(840.0f + (2290.0f - 840.0f) * t, 7.0f * sp);
                f3_.set(2410.0f + (3010.0f - 2410.0f) * t, 8.0f * sp);
                break;
            }
            case 6: {
                const float ratio = 1.0f + (1.0f + 7.0f * tone) * (0.35f + 0.65f * lfoUni);
                f1_.set(f0 * 2.0f, 0.7f);
                f2_.set(f0 * ratio < kSr * 0.4f ? f0 * ratio : kSr * 0.4f, 3.0f + 4.0f * motion);
                break;
            }
            case 7: f1_.set(f0 * (3.0f + 10.0f * motion), 4.0f); break;
            case 10: f1_.set(f0 * (1.4f + 30.0f * tone * e_), 4.5f); break;
            default: break;
        }
    }
    float modelSample(int m, float dphi, float tone, float motion, float grit, float lfoUni, float lfoSin) {
        const float phi = phi_;
        float y = 0;
        switch (m) {
            case 0: {   // Reese: three saws, two detuned around the master
                const float d = 0.002f + 0.022f * motion * motion;
                p2_ = dx::wrap(p2_ + dphi * (1.0f + d));
                p3_ = dx::wrap(p3_ + dphi * (1.0f - d * 0.93f));
                const float x = (dx::sawBlep(phi, dphi) + dx::sawBlep(p2_, dphi) + dx::sawBlep(p3_, dphi)) * 0.36f;
                y = dx::soft(f1_.low(x) * (1.0f + 6.0f * grit));
                break;
            }
            case 1: {   // Wobble
                const float x = 0.62f * dx::sawBlep(phi, dphi) +
                                0.38f * (dx::sawBlep(phi, dphi) - dx::sawBlep(dx::wrap(phi + 0.5f), dphi));
                y = dx::soft(f1_.low(x) * 1.1f * (1.0f + 6.0f * (0.15f + grit)));
                break;
            }
            case 2: {   // Growl: PM through moving formants
                const float idx = 0.6f + tone * 5.5f * (0.4f + 0.6f * lfoUni);
                const float x = dx::sin2pi(dx::wrap(phi + idx * dx::sin2pi(phi) * 0.159f));
                y = dx::soft((0.35f * x + 1.6f * f1_.band(x) + 1.3f * f2_.band(x)) * (1.0f + 6.0f * (0.2f + grit)));
                break;
            }
            case 3: {   // FM stab
                pm_ = dx::wrap(pm_ + dphi * 2.0f);
                const float idx = (0.8f + 14.0f * tone) * (0.15f + 0.85f * e_);
                const float x = dx::sin2pi(dx::wrap(phi + (idx * dx::sin2pi(pm_) + grit * 1.4f * fb_) * 0.159f));
                fb_ = x;
                y = dx::soft(x * (1.0f + 2.0f * grit));
                break;
            }
            case 4: {   // Hoover
                const float rr = dx::exp2fast(tone * e_);
                p1_ = dx::wrap(p1_ + dphi * rr);
                p2_ = dx::wrap(p2_ + dphi * rr * 1.0071f);
                p3_ = dx::wrap(p3_ + dphi * rr * 0.9932f);
                const float w = 0.5f + 0.35f * lfoSin;
                const float dt = dphi * rr;
                float x = 0;
                x += dx::sawBlep(p1_, dt) - dx::sawBlep(dx::wrap(p1_ + w), dt);
                x += dx::sawBlep(p2_, dt) - dx::sawBlep(dx::wrap(p2_ + w), dt);
                x += dx::sawBlep(p3_, dt) - dx::sawBlep(dx::wrap(p3_ + w), dt);
                y = dx::soft(f1_.low(x * 0.3f) * (1.0f + 6.0f * grit));
                break;
            }
            case 5: {   // Talk
                const float x = dx::sawBlep(phi, dphi);
                y = dx::soft((0.12f * x + 1.4f * f1_.band(x) + 1.2f * f2_.band(x) + 0.7f * f3_.band(x)) *
                             (1.0f + 6.0f * (0.15f + grit)));
                break;
            }
            case 6: {   // Screech: hard-synced saw
                const float ratio = 1.0f + (1.0f + 7.0f * tone) * (0.35f + 0.65f * lfoUni);
                p1_ += dphi * ratio;
                if (phi < dphi) p1_ = phi * ratio;
                else if (p1_ >= 1.0f) p1_ -= dx::ffloor(p1_);
                const float x = 2.0f * p1_ - 1.0f;
                const float h = f1_.high(x);
                y = dx::soft((0.6f * h + 0.8f * f2_.band(h)) * (1.0f + 6.0f * (0.25f + grit)));
                break;
            }
            case 8: {   // 808 drop
                const float rr = dx::exp2fast(tone * 2.0f * e_ * e_);
                p1_ = dx::wrap(p1_ + dphi * rr);
                y = dx::soft(dx::sin2pi(p1_) * (0.85f + 0.3f * e_) * (1.0f + 6.0f * (0.1f + grit)));
                break;
            }
            case 9: {   // Solid FM (DX100 "Solid Bass")
                pm_ = dx::wrap(pm_ + dphi * (motion > 0.5f ? 2.0f : 1.0f));
                const float mod = dx::sin2pi(dx::wrap(pm_ + grit * 1.2f * fb_ * 0.159f));
                fb_ = mod;
                const float idx = 0.4f + 2.6f * tone + 5.0f * e_;
                y = dx::soft(dx::sin2pi(dx::wrap(phi + idx * mod * 0.159f)) * 1.9f);
                break;
            }
            case 10: {   // Hollow: pulse into a resonant LP, dies away
                const float pw = 0.5f - 0.42f * grit;
                const float x = dx::sawBlep(phi, dphi) - dx::sawBlep(dx::wrap(phi + pw), dphi);
                y = f1_.low(x) * 0.9f * e_;
                break;
            }
            case 11: {   // Semi sines
                const float det = 0.05f + 0.95f * motion;
                p2_ = dx::wrap(p2_ + dphi * dx::semis(det));
                const float l2 = 0.3f + 0.7f * tone;
                y = dx::soft((dx::sin2pi(phi) + l2 * dx::sin2pi(p2_)) / (1.0f + l2) * (1.0f + 6.0f * grit));
                break;
            }
            default: {   // Snarl: folded sine + band noise, crushed
                const float amt = 1.0f + 6.0f * tone * (0.4f + 0.6f * lfoUni);
                float x = dx::sin2pi(dx::wrap(amt * dx::sin2pi(phi) * 0.159f));
                x += 0.45f * f1_.band(nz_.bi());
                const int hold = 1 + int(grit * grit * 14.0f);
                if (++holdCount_ >= hold) {
                    holdCount_ = 0;
                    hold_ = x;
                }
                const float steps = 8.0f + (1.0f - grit) * 120.0f;
                y = dx::ffloor(hold_ * steps + 0.5f) / steps;
                break;
            }
        }
        return y;
    }
    dx::Svf f1_, f2_, f3_;
    dx::Noise nz_;
    float phi_ = 0, p1_ = 0, p2_ = 0, p3_ = 0, pm_ = 0, fb_ = 0, subPh_ = 0, lfo_ = 0, hold_ = 0;
    int holdCount_ = 0;
    float f0_ = 55, target_ = 55, e_ = 0, env_ = 0, vel_ = 1, tone_ = 0, bpm_ = 120;
    bool gate_ = false, on_ = false;
    int pitch_ = -1;
};

// =============================================================================
// QUANTA particles
// =============================================================================
const char* const kPType[5] = {"Grain", "Glisson", "Grainlet", "Trainlet", "Pulsar"};
const char* const kPWave[4] = {"Sine", "Saw", "Square", "Noise"};
const char* const kPWin[6] = {"Gauss", "Hann", "Expodec", "Rexpodec", "Trapez", "Rect"};
const char* const kPSync[10] = {"Free", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T", "1/32T"};
enum QnP { qType, qDensity, qSync, qDur, qWave, qWin, qDuty, qGlide, qCycles, qImpulses, qChroma, qJitPitch, qJitTime,
           qJitAmp, qSpread, qMask, qAttack, qRelease, qGain, qQnN };
const PDesc kQnDesc[qQnN] = {
    {"Type", 0, 4, 4, 1, kFList, kPType},
    {"Density", 1, 400, 40, 1, kFNum, nullptr},
    {"Sync", 0, 9, 0, 1, kFList, kPSync},
    {"Duration", 1, 200, 25, 1, kFMs, nullptr},
    {"Wave", 0, 3, 0, 1, kFList, kPWave},
    {"Window", 0, 5, 0, 1, kFList, kPWin},
    {"Duty", 1, 100, 25, 1, kFNum, nullptr},
    {"Glide", -24, 24, 7, 1, kFSemi, nullptr},
    {"Cycles", 1, 64, 8, 1, kFNum, nullptr},
    {"Impulses", 1, 32, 8, 1, kFNum, nullptr},
    {"Chroma", 0, 1, 0.5f, 0.01f, kFPct, nullptr},
    {"Jit Pitch", 0, 12, 0, 0.1f, kFNum, nullptr},
    {"Jit Time", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Jit Amp", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Spread", 0, 1, 0.5f, 0.01f, kFPct, nullptr},
    {"Mask", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Attack", 1, 2000, 5, 1, kFMs, nullptr},
    {"Release", 5, 4000, 300, 5, kFMs, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kQnSec[4] = {{"Emitter", 0, 4}, {"Particle", 4, 7}, {"Jitter", 11, 5}, {"Amp", 16, 3}};

class Quanta final : public TableEngine {
public:
    static constexpr int kParts = 12;
    static constexpr int kWin = 256;
    Quanta() {
        bind(kQnDesc, qQnN, kQnSec, 4);
        for (int w = 0; w < 6; ++w)
            for (int i = 0; i <= kWin; ++i) win_[w][i] = shape(w, float(i) / float(kWin));
    }
    int kind() const override { return kEngQuanta; }
    void setTempo(float bpm) override { bpm_ = bpm; }
    void noteOn(int pitch, float vel, int, int tone, int) override {
        hz_ = dx::mtof(float(pitch) + float(tone) * 0.05f);
        vel_ = vel;
        gate_ = true;
        on_ = true;
        pitch_ = pitch;
        next_ = 0;
    }
    void noteOff(int pitch) override {
        if (pitch == pitch_) gate_ = false;
    }
    void allOff(bool hard) override {
        gate_ = false;
        if (hard) {
            on_ = false;
            env_ = 0;
            for (auto& p : pt_) p.on = false;
        }
    }
    void render(float* l, float* r) override {
        bool any = false;
        for (auto& p : pt_) any = any || p.on;
        if (!on_ && !any) return;
        const float* p = p_;
        const float atk = 1.0f / (p[qAttack] * 0.001f * kSr + 1.0f) * 1.0f;
        const float rel = 1.0f / (p[qRelease] * 0.001f * kSr + 1.0f);
        // envelope at block rate (good enough for clouds)
        for (int s = 0; s < int(kBlock); s += 32) {
            if (gate_) env_ += (1.0f - env_) * atk * 32.0f * 0.25f;
            else env_ -= env_ * rel * 32.0f * 0.25f;
            env_ = clampf(env_, 0.0f, 1.0f);
        }
        if (on_ && env_ < 1e-4f && !gate_) on_ = false;
        // emissions inside this block
        if (on_) {
            static const float kSyncB[10] = {0, 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.6667f, 0.3333f, 0.1667f, 0.0833f};
            const int sync = int(p[qSync] + 0.5f);
            const float period = sync > 0 ? kSyncB[sync] * 60.0f / (bpm_ > 1 ? bpm_ : 120.0f) : 1.0f / p[qDensity];
            const int32_t perS = int32_t(period * kSr);
            next_ -= int32_t(kBlock);
            int guard = 0;
            while (next_ <= 0 && guard++ < 8) {
                spawn(period, int(kBlock) + next_);
                int32_t jit = int32_t(float(perS) * p[qJitTime] * nz_.bi() * 0.5f);
                next_ += perS + jit > 32 ? perS + jit : 32;
            }
        }
        const float gain = p[qGain] * vel_ * env_ * 0.6f;
        float al[kBlock], ar[kBlock];
        for (uint32_t i = 0; i < kBlock; ++i) al[i] = ar[i] = 0.0f;
        for (auto& qq : pt_) {
            if (!qq.on) continue;
            Part q = qq;
            const float* wt = win_[q.win];
            const float invLen = float(kWin) / float(q.len);
            const uint32_t i0 = uint32_t(q.wait);
            int n = int(kBlock - i0);
            if (n > q.len - q.age) n = q.len - q.age;
            float wpos = float(q.age) * invLen;
            const float g = q.amp * gain, gl = g * q.gl, gr = g * q.gr;
            const bool glide = q.incMul != 1.0f;
            for (int j = 0; j < n; ++j) {
                const int wi = int(wpos);
                const float w = wt[wi] + (wt[wi + 1] - wt[wi]) * (wpos - float(wi));
                wpos += invLen;
                float sm;
                if (q.type == 3) sm = trainlet(q);
                else if (q.type == 4 && q.age + j >= q.pulseLen) sm = 0;
                else sm = osc(q);
                q.phase += q.inc;
                if (q.phase >= 1.0f) q.phase -= dx::ffloor(q.phase);
                if (glide) {
                    q.inc *= q.incMul;
                    if (q.inc > 0.45f) q.inc = 0.45f;
                }
                const float v = sm * w;
                al[i0 + uint32_t(j)] += v * gl;
                ar[i0 + uint32_t(j)] += v * gr;
            }
            q.age += n;
            if (q.age >= q.len) q.on = false;
            q.wait = 0;
            qq = q;
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            l[i] += al[i];
            r[i] += ar[i];
        }
    }
    int active() const override {
        int n = 0;
        for (auto& q : pt_) n += q.on;
        return n + (on_ ? 1 : 0);
    }
    // QEMU: per particle 7.4k (grain, glisson), 8k (grainlet), 20k (trainlet), 7k (pulsar)
    int32_t partCost() const {
        static const int32_t kC[5] = {7400, 7500, 8000, 20000, 7000};
        const int t = int(p_[qType] + 0.5f);
        return kC[t < 0 ? 0 : (t > 4 ? 4 : t)];
    }
    int32_t cost() const override {
        int n = 0;
        for (auto& q : pt_) n += q.on;
        return (on_ || n) ? 3000 + n * partCost() : 0;
    }
    int32_t noteCost() const override {
        // the whole cloud: density x duration particles overlap
        float ov = p_[qDensity] * p_[qDur] * 0.001f + 1.0f;
        if (int(p_[qType] + 0.5f) == 4) ov = 1.0f;
        if (ov > kParts) ov = kParts;
        return on_ ? 0 : int32_t(3000.0f + ov * float(partCost()));
    }
    bool steal() override {
        for (auto& q : pt_)
            if (q.on) {
                q.on = false;
                return true;
            }
        if (on_) {
            on_ = false;
            return true;
        }
        return false;
    }

private:
    struct Part {
        bool on = false;
        uint8_t type = 0, wave = 0, win = 0;
        int age = 0, len = 1, pulseLen = 1, wait = 0;
        float phase = 0, inc = 0.01f, incMul = 1, amp = 1, gl = 1, gr = 1, a = 0.5f, norm = 1;
        int K = 1;
    };
    static float shape(int t, float x) {
        switch (t) {
            case 0: {
                const float e = expf(-0.5f * (0.5f / 0.15f) * (0.5f / 0.15f));
                return (expf(-0.5f * ((x - 0.5f) / 0.15f) * ((x - 0.5f) / 0.15f)) - e) / (1.0f - e);
            }
            case 1: return 0.5f - 0.5f * cosf(dx::kTwoPi * x);
            case 2: {
                if (x < 0.005f) return x / 0.005f;
                const float u = (x - 0.005f) / 0.995f, e = expf(-5.0f);
                return (expf(-5.0f * u) - e) / (1.0f - e);
            }
            case 3: return shape(2, 1.0f - x);
            case 4: {
                float v = x / 0.1f;
                if ((1.0f - x) / 0.1f < v) v = (1.0f - x) / 0.1f;
                return v > 1.0f ? 1.0f : v;
            }
            default: return 1.0f;
        }
    }
    float osc(Part& q) {
        switch (q.wave) {
            case 1: return 2.0f * q.phase - 1.0f;
            case 2: return q.phase < 0.5f ? 1.0f : -1.0f;
            case 3: return nz_.bi();
            default: return dx::sin2pi(q.phase);
        }
    }
    float trainlet(Part& q) {
        // sum of K harmonics with a geometric tilt (Roads' trainlet), by the
        // Chebyshev recurrence: one sine and one cosine per sample
        const float c2 = 2.0f * dx::sin2pi(dx::wrap(q.phase + 0.25f));
        float s1 = dx::sin2pi(q.phase), s0 = 0.0f, sum = s1, a = q.a;
        for (int k = 2; k <= q.K; ++k) {
            const float sk = c2 * s1 - s0;
            s0 = s1;
            s1 = sk;
            sum += a * sk;
            a *= q.a;
        }
        return sum * q.norm;
    }
    void spawn(float period, int at) {
        const float* p = p_;
        if (p[qMask] > 0 && nz_.uni() < p[qMask]) return;
        Part* q = nullptr;
        for (auto& x : pt_)
            if (!x.on) {
                q = &x;
                break;
            }
        if (!q) q = &pt_[cursor_++ % kParts];
        Part& n = *q;
        n = Part();
        n.on = true;
        n.type = uint8_t(p[qType] + 0.5f);
        n.wave = uint8_t(p[qWave] + 0.5f);
        n.win = uint8_t(p[qWin] + 0.5f);
        float hz = hz_;
        if (p[qJitPitch] > 0) hz *= dx::semis(p[qJitPitch] * nz_.bi());
        hz = clampf(hz, 1.0f, 16000.0f);
        float dur;
        switch (n.type) {
            case 2: dur = clampf(p[qCycles] / hz, 0.0005f, 1.0f); break;
            case 3: dur = clampf(p[qImpulses] / hz, 0.0005f, 2.0f); break;
            case 4: dur = period; break;
            default: dur = p[qDur] * 0.001f; break;
        }
        n.len = int(dur * kSr) > 2 ? int(dur * kSr) : 2;
        if (n.len > int(kSr)) n.len = int(kSr);
        n.pulseLen = n.type == 4 ? int(float(n.len) * clampf(p[qDuty], 1.0f, 100.0f) * 0.01f) : n.len;
        if (n.type == 4) n.len = n.pulseLen > 2 ? n.pulseLen : 2;
        n.inc = hz * kInvSr;
        if (n.type == 1 && p[qGlide] != 0) n.incMul = dx::exp2fast(p[qGlide] / 12.0f / float(n.len));
        if (n.type == 3) {
            n.a = 0.3f + 0.65f * p[qChroma];
            n.K = int(0.45f * kSr / hz);
            if (n.K < 1) n.K = 1;
            if (n.K > 8) n.K = 8;
            float sum = 0, a = 1;
            for (int k = 0; k < n.K; ++k) {
                sum += a;
                a *= n.a;
            }
            n.norm = 1.0f / sum;
        }
        n.amp = 1.0f - p[qJitAmp] * nz_.uni();
        const float pan = nz_.bi() * p[qSpread];
        n.gl = pan > 0 ? 1.0f - pan : 1.0f;
        n.gr = pan < 0 ? 1.0f + pan : 1.0f;
        n.wait = at > 0 && at < int(kBlock) ? at : 0;
    }
    Part pt_[kParts];
    float win_[6][kWin + 1];
    dx::Noise nz_;
    float hz_ = 220, vel_ = 1, env_ = 0, bpm_ = 120;
    bool gate_ = false, on_ = false;
    int pitch_ = -1, cursor_ = 0;
    int32_t next_ = 0;
};

}  // namespace

Engine* makeFloater(Arena& a) {
    void* m = a.alloc(sizeof(FlKit), 16);
    return m ? new (m) FlKit() : nullptr;
}
Engine* makeTb303(Arena& a) {
    void* m = a.alloc(sizeof(Tb303), 16);
    return m ? new (m) Tb303() : nullptr;
}
Engine* makeNeuro(Arena& a) {
    void* m = a.alloc(sizeof(Neuro), 16);
    return m ? new (m) Neuro() : nullptr;
}
Engine* makeQuanta(Arena& a) {
    void* m = a.alloc(sizeof(Quanta), 16);
    return m ? new (m) Quanta() : nullptr;
}

}  // namespace konsol
}  // namespace lab
