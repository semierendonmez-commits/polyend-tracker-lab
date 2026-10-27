// KONSOL-native engines: DRUMS, IKEDA, MAKAM (the sample engines: engines_sample.cpp).
// Float only, no allocation after init, no libm calls per sample (the fast
// sine below), costs estimated per block for the voice governor.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/dsp.h"
#include "ws/engine_table.h"
#include "ws/model.h"

namespace lab {
namespace konsol {

namespace {

using dsp::kInvSr;
using dsp::kSr;

// sin(2 pi t), t any phase in [0, 1): parabola + one refinement (< 0.1 %).
inline float fsin(float t) {
    float x = t < 0.5f ? t : t - 1.0f;          // -0.5 .. 0.5
    const float ax = x < 0 ? -x : x;
    float y = 8.0f * x - 16.0f * x * ax;
    const float ay = y < 0 ? -y : y;
    return 0.225f * (y * ay - y) + y;
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float semis(float s) { return exp2f(s * (1.0f / 12.0f)); }
inline float msToInc(float ms) { return 1.0f / (clampf(ms, 0.2f, 60000.0f) * 0.001f * kSr); }
// Per-sample multiplier that falls by 60 dB over `ms`.
inline float decayK(float ms) { return expf(-6.9f / (clampf(ms, 1.0f, 60000.0f) * 0.001f * kSr)); }

struct Noise {
    uint32_t s = 0x2545F491u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(int32_t(s)) * (1.0f / 2147483648.0f);
    }
};

// Chamberlin-style SVF with stable TPT coefficients, mono (cheap).
struct Filt {
    float g = 0.5f, k = 1.4f, a1 = 0.5f, ic1 = 0, ic2 = 0;
    void set(float hz, float res01) {
        g = tanf(dsp::kPi * clampf(hz, 20.0f, 18000.0f) * kInvSr);
        k = 2.0f - 1.9f * clampf(res01, 0.0f, 1.0f);
        a1 = 1.0f / (1.0f + g * (g + k));
    }
    inline float lp(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + g * a1 * v3;
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }
    inline float bp(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + g * a1 * v3;
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v1;
    }
    inline float hp(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + g * a1 * v3;
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return x - k * v1 - v2;
    }
    void clear() { ic1 = ic2 = 0; }
};

// One-pole highpass (cheap).
struct Hp1 {
    float a = 0.9f, x1 = 0, y1 = 0;
    void set(float hz) { a = expf(-dsp::kTwoPi * clampf(hz, 10.0f, 16000.0f) * kInvSr); }
    inline float run(float x) {
        const float y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }
};

// =============================================================================
// DRUMS (8-voice analog-style kit: notes 36..43)
// =============================================================================
enum DrP {
    dKickTune, dKickDecay, dKickPunch, dKickClick,
    dSnTune, dSnDecay, dSnSnappy, dSnTone,
    dClapDecay, dClapTone,
    dChDecay, dOhDecay, dHatTone,
    dTomTune, dTomDecay,
    dRimTune, dCowTune, dCowDecay,
    dDrive, dPitch, dGain, dDrN
};
const PDesc kDrDesc[dDrN] = {
    {"Kick Tune", 30, 90, 48, 1, kFHz, nullptr},
    {"Kick Decay", 50, 1500, 420, 10, kFMs, nullptr},
    {"Kick Punch", 0, 1, 0.6f, 0.01f, kFPct, nullptr},
    {"Kick Click", 0, 1, 0.35f, 0.01f, kFPct, nullptr},
    {"Snare Tune", 120, 400, 190, 1, kFHz, nullptr},
    {"Snare Decay", 40, 800, 180, 5, kFMs, nullptr},
    {"Snappy", 0, 1, 0.6f, 0.01f, kFPct, nullptr},
    {"Snare Tone", 800, 9000, 2500, 1, kFHz, nullptr},
    {"Clap Decay", 50, 800, 220, 5, kFMs, nullptr},
    {"Clap Tone", 500, 4000, 1300, 1, kFHz, nullptr},
    {"CHat Decay", 10, 300, 45, 1, kFMs, nullptr},
    {"OHat Decay", 60, 1500, 380, 5, kFMs, nullptr},
    {"Hat Tone", 3000, 14000, 7500, 1, kFHz, nullptr},
    {"Tom Tune", 60, 300, 110, 1, kFHz, nullptr},
    {"Tom Decay", 60, 1200, 300, 5, kFMs, nullptr},
    {"Rim Tune", 800, 3000, 1700, 1, kFHz, nullptr},
    {"Cow Tune", 300, 1200, 560, 1, kFHz, nullptr},
    {"Cow Decay", 30, 800, 160, 5, kFMs, nullptr},
    {"Drive", 0, 1, 0.15f, 0.01f, kFPct, nullptr},
    {"Kit Pitch", -12, 12, 0, 1, kFSemi, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kDrSec[6] = {{"Kick", 0, 4}, {"Snare/Clap", 4, 6}, {"Hats", 10, 3}, {"Tom/Rim/Cow", 13, 5}, {"Kit", 18, 3},
                           {"", 0, 0}};

class Drums final : public TableEngine {
public:
    Drums() { bind(kDrDesc, dDrN, kDrSec, 5); }
    int kind() const override { return kEngDrums; }
    void noteOn(int pitch, float vel, int offset, int tone, int) override {
        const int d = ((pitch - 36) % 8 + 8) % 8;
        const float pm = semis(p_[dPitch] + float(tone) * 0.1f);
        D& v = d_[d];
        v.on = true;
        v.wait = uint32_t(offset > 0 && offset < int(kBlock) ? offset : 0);
        v.vel = vel;
        v.ph = 0;
        v.ph2 = 0;
        v.t = 0;
        switch (d) {
            case 0:   // kick
                v.f = p_[dKickTune] * pm;
                v.amp = 1.0f;
                v.k = decayK(p_[dKickDecay]);
                v.pe = 1.0f;
                v.pk = decayK(25.0f + 60.0f * (1.0f - p_[dKickPunch]));
                v.click = p_[dKickClick];
                break;
            case 1:   // snare
                v.f = p_[dSnTune] * pm;
                v.amp = 1.0f;
                v.k = decayK(p_[dSnDecay] * 0.6f);
                v.n = 1.0f;
                v.nk = decayK(p_[dSnDecay]);
                v.pe = 1.0f;
                v.pk = decayK(20.0f);
                v.hp.set(p_[dSnTone]);
                break;
            case 2:   // clap
                v.n = 1.0f;
                v.nk = decayK(p_[dClapDecay]);
                v.f2.set(p_[dClapTone] * pm, 0.55f);
                break;
            case 3:   // closed hat chokes the open one
                d_[4].on = false;
                v.n = 1.0f;
                v.nk = decayK(p_[dChDecay]);
                v.hp.set(p_[dHatTone] * pm);
                break;
            case 4:
                v.n = 1.0f;
                v.nk = decayK(p_[dOhDecay]);
                v.hp.set(p_[dHatTone] * pm * 0.9f);
                break;
            case 5:   // tom (pitch follows the note's octave)
                v.f = p_[dTomTune] * pm * semis(float(((pitch - 36) / 8) * 5));
                v.amp = 1.0f;
                v.k = decayK(p_[dTomDecay]);
                v.pe = 1.0f;
                v.pk = decayK(60.0f);
                break;
            case 6:   // rim
                v.f = p_[dRimTune] * pm;
                v.amp = 1.0f;
                v.k = decayK(28.0f);
                v.n = 0.6f;
                v.nk = decayK(6.0f);
                break;
            default:   // cowbell
                v.f = p_[dCowTune] * pm;
                v.amp = 1.0f;
                v.k = decayK(p_[dCowDecay]);
                v.f2.set(v.f * 1.6f, 0.4f);
                break;
        }
    }
    void noteOff(int) override {}
    void allOff(bool hard) override {
        if (hard)
            for (auto& d : d_) d.on = false;
    }
    void render(float* l, float* r) override {
        float acc[kBlock];
        bool any = false;
        for (int d = 0; d < 8; ++d) {
            D& v = d_[d];
            if (!v.on) continue;
            if (!any) {
                for (uint32_t i = 0; i < kBlock; ++i) acc[i] = 0;
                any = true;
            }
            const float g = v.vel;
            const uint32_t w0 = v.wait;
            v.wait = 0;
            switch (d) {
                case 0:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        const float f = v.f * (1.0f + 3.5f * v.pe * p_[dKickPunch]);
                        v.ph += f * kInvSr;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        float s = fsin(v.ph) * v.amp;
                        if (v.t < 90) s += v.click * (1.0f - float(v.t) * (1.0f / 90.0f)) * noise_.next();
                        acc[i] += s * g;
                        v.amp *= v.k;
                        v.pe *= v.pk;
                        ++v.t;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
                case 1: {
                    const float sn = p_[dSnSnappy];
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        const float f = v.f * (1.0f + 0.6f * v.pe);
                        v.ph += f * kInvSr;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        v.ph2 += f * 1.47f * kInvSr;
                        if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                        const float body = (fsin(v.ph) + 0.5f * fsin(v.ph2)) * v.amp * (1.0f - 0.5f * sn);
                        const float nz = v.hp.run(noise_.next()) * v.n * sn * 1.4f;
                        acc[i] += (body + nz) * g;
                        v.amp *= v.k;
                        v.n *= v.nk;
                        v.pe *= v.pk;
                    }
                    if (v.n < 1e-4f && v.amp < 1e-4f) v.on = false;
                    break;
                }
                case 2:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        // three fast bursts, then the tail
                        float e = v.n;
                        if (v.t < 1800) {
                            const uint32_t m = v.t % 600;
                            e = 1.0f - float(m) * (1.0f / 600.0f);
                        }
                        acc[i] += v.f2.bp(noise_.next()) * e * g * 2.2f;
                        if (v.t >= 1800) v.n *= v.nk;
                        ++v.t;
                    }
                    if (v.n < 1e-4f) v.on = false;
                    break;
                case 3:
                case 4:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        acc[i] += v.hp.run(noise_.next()) * v.n * g * 0.8f;
                        v.n *= v.nk;
                    }
                    if (v.n < 1e-4f) v.on = false;
                    break;
                case 5:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        v.ph += v.f * (1.0f + 0.8f * v.pe) * kInvSr;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        acc[i] += fsin(v.ph) * v.amp * g;
                        v.amp *= v.k;
                        v.pe *= v.pk;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
                case 6:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        v.ph += v.f * kInvSr;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        acc[i] += (fsin(v.ph) * v.amp + noise_.next() * v.n) * g * 0.7f;
                        v.amp *= v.k;
                        v.n *= v.nk;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
                default:
                    for (uint32_t i = w0; i < kBlock; ++i) {
                        v.ph += v.f * kInvSr;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        v.ph2 += v.f * 1.48f * kInvSr;
                        if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                        const float sq = (v.ph < 0.5f ? 0.5f : -0.5f) + (v.ph2 < 0.5f ? 0.5f : -0.5f);
                        acc[i] += v.f2.bp(sq) * v.amp * g * 1.2f;
                        v.amp *= v.k;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
            }
        }
        if (!any) return;
        const float drive = p_[dDrive], gain = p_[dGain] * 0.8f;
        if (drive > 0.01f) {
            const float pre = 1.0f + drive * 5.0f, post = gain / (1.0f + drive * 1.2f);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float x = dsp::softclip(acc[i] * pre) * post;
                l[i] += x;
                r[i] += x;
            }
        } else {
            for (uint32_t i = 0; i < kBlock; ++i) {
                l[i] += acc[i] * gain;
                r[i] += acc[i] * gain;
            }
        }
    }
    int active() const override {
        int n = 0;
        for (auto& d : d_) n += d.on;
        return n;
    }
    int32_t cost() const override {
        int32_t c = 0;
        static const int32_t kC[8] = {5500, 11500, 6500, 5000, 5000, 4800, 6500, 9000};
        for (int d = 0; d < 8; ++d)
            if (d_[d].on) c += kC[d];
        return c ? c + 1600 : 0;
    }
    int32_t noteCost() const override { return 11500; }
    bool steal() override {
        // the quietest ringing drum
        int q = -1;
        float lv = 9;
        for (int d = 0; d < 8; ++d) {
            if (!d_[d].on) continue;
            const float a = d_[d].amp > d_[d].n ? d_[d].amp : d_[d].n;
            if (a < lv) { lv = a; q = d; }
        }
        if (q < 0) return false;
        d_[q].on = false;
        return true;
    }

private:
    struct D {
        bool on = false;
        float vel = 1, f = 100, ph = 0, ph2 = 0, amp = 0, k = 0.999f, pe = 0, pk = 0.99f, n = 0, nk = 0.99f,
              click = 0;
        uint32_t t = 0, wait = 0;
        Hp1 hp;
        Filt f2;
    };
    D d_[8];
    Noise noise_;
};

// =============================================================================
// IKEDA (sine tones, clicks, noise, data bursts)
// =============================================================================
const char* const kIkMode[5] = {"By pitch", "Sine", "Click", "Noise", "Data"};
enum IkP { iMode, iSineDecay, iSineOct, iClickLen, iClickTone, iNoiseDecay, iNoiseHp, iDataRate, iDataDecay, iBits,
           iSpread, iGain, iIkN };
const PDesc kIkDesc[iIkN] = {
    {"Mode", 0, 4, 0, 1, kFList, kIkMode},
    {"Sine Decay", 5, 4000, 260, 5, kFMs, nullptr},
    {"Sine Oct", -2, 3, 1, 1, kFInt, nullptr},
    {"Click Len", 1, 400, 24, 1, kFInt, nullptr},
    {"Click Tone", 200, 18000, 9000, 1, kFHz, nullptr},
    {"Noise Decay", 2, 2000, 60, 2, kFMs, nullptr},
    {"Noise HP", 40, 16000, 5000, 1, kFHz, nullptr},
    {"Data Rate", 0.25f, 16, 2, 0.25f, kFNum, nullptr},
    {"Data Decay", 5, 2000, 140, 5, kFMs, nullptr},
    {"Bits", 1, 16, 16, 1, kFInt, nullptr},
    {"Spread", 0, 1, 0.8f, 0.01f, kFPct, nullptr},
    {"Gain", 0, 2, 0.9f, 0.01f, kFPct, nullptr},
};
const SecDesc kIkSec[4] = {{"Mode/Sine", 0, 3}, {"Click/Noise", 3, 4}, {"Data", 7, 3}, {"Out", 10, 2}};

class Ikeda final : public TableEngine {
public:
    Ikeda() { bind(kIkDesc, iIkN, kIkSec, 4); }
    int kind() const override { return kEngIkeda; }
    void noteOn(int pitch, float vel, int, int tone, int) override {
        int type = ip(iMode);
        if (type == 0) type = pitch < 48 ? 2 : (pitch < 72 ? 1 : (pitch < 84 ? 4 : 3));
        V* v = &v_[0];
        for (auto& x : v_)
            if (!x.on) { v = &x; break; }
        if (v->on) {   // all busy: oldest
            for (auto& x : v_)
                if (x.age < v->age) v = &x;
        }
        V& x = *v;
        x.on = true;
        x.type = uint8_t(type);
        x.vel = vel;
        x.age = ++age_;
        x.t = 0;
        x.ph = 0;
        side_ = !side_;
        const float sp = p_[iSpread];
        x.gl = side_ ? 1.0f : 1.0f - sp;
        x.gr = side_ ? 1.0f - sp : 1.0f;
        const float hz = dsp::mtof(float(pitch)) * semis(12.0f * float(ip(iSineOct)) + float(tone) * 0.25f);
        switch (type) {
            case 1:
                x.inc = clampf(hz, 20.0f, 19000.0f) * kInvSr;
                x.amp = 0.6f;
                x.k = decayK(p_[iSineDecay]);
                break;
            case 2:
                x.len = uint32_t(p_[iClickLen]);
                x.amp = 1.0f;
                x.f.set(p_[iClickTone], 0.2f);
                break;
            case 3:
                x.amp = 0.7f;
                x.k = decayK(p_[iNoiseDecay]);
                x.hp.set(p_[iNoiseHp]);
                break;
            default:   // data: random bits clocked at the note frequency x rate
                x.inc = clampf(hz * p_[iDataRate] * 0.25f, 10.0f, 22000.0f) * kInvSr;
                x.amp = 0.45f;
                x.k = decayK(p_[iDataDecay]);
                x.bit = 1.0f;
                break;
        }
    }
    void noteOff(int) override {}
    void allOff(bool hard) override {
        if (hard)
            for (auto& v : v_) v.on = false;
    }
    void render(float* l, float* r) override {
        const int bits = ip(iBits);
        const float q = bits < 16 ? float(1 << (bits - 1)) : 0.0f;
        const float gain = p_[iGain];
        for (auto& v : v_) {
            if (!v.on) continue;
            const float gl = v.gl * v.vel * gain, gr = v.gr * v.vel * gain;
            switch (v.type) {
                case 1:
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        v.ph += v.inc;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        float s = fsin(v.ph) * v.amp;
                        if (q > 0.0f) s = floorf(s * q) / q;
                        l[i] += s * gl;
                        r[i] += s * gr;
                        v.amp *= v.k;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
                case 2:
                    for (uint32_t i = 0; i < kBlock && v.t < v.len; ++i, ++v.t) {
                        const float s = v.f.lp((v.t & 1) ? -v.amp : v.amp) * 1.5f;
                        l[i] += s * gl;
                        r[i] += s * gr;
                    }
                    if (v.t >= v.len) v.on = false;
                    break;
                case 3:
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        float s = v.hp.run(noise_.next()) * v.amp;
                        if (q > 0.0f) s = floorf(s * q) / q;
                        l[i] += s * gl;
                        r[i] += s * gr;
                        v.amp *= v.k;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
                default:
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        v.ph += v.inc;
                        if (v.ph >= 1.0f) {
                            v.ph -= 1.0f;
                            v.bit = noise_.next() > 0.0f ? 1.0f : -1.0f;
                        }
                        const float s = v.bit * v.amp;
                        l[i] += s * gl;
                        r[i] += s * gr;
                        v.amp *= v.k;
                    }
                    if (v.amp < 1e-4f) v.on = false;
                    break;
            }
        }
    }
    int active() const override {
        int n = 0;
        for (auto& v : v_) n += v.on;
        return n;
    }
    int32_t cost() const override { return active() * 6000 + (active() ? 600 : 0); }
    int32_t noteCost() const override { return 6000; }
    bool steal() override {
        V* o = nullptr;
        for (auto& v : v_)
            if (v.on && (!o || v.age < o->age)) o = &v;
        if (!o) return false;
        o->on = false;
        return true;
    }

private:
    struct V {
        bool on = false;
        uint8_t type = 1;
        uint32_t age = 0, t = 0, len = 0;
        float vel = 1, ph = 0, inc = 0.01f, amp = 0, k = 0.999f, gl = 1, gr = 1, bit = 1;
        Filt f;
        Hp1 hp;
    };
    V v_[6];
    Noise noise_;
    uint32_t age_ = 0;
    bool side_ = false;
};

// =============================================================================
// MAKAM (53-TET / 24-tone / 12-TET tuned voices: Kanun, Ney, Saz, Rebab)
// =============================================================================
struct MakamDef {
    const char* name;
    uint8_t commas[7];   // degree offsets in Holdrian commas (53 per octave)
};
const MakamDef kMakams[kMakamCount] = {
    {"Rast", {0, 9, 17, 22, 31, 40, 48}},     {"Ussak", {0, 8, 13, 22, 31, 35, 44}},
    {"Hicaz", {0, 5, 17, 22, 31, 39, 44}},    {"Huseyni", {0, 8, 13, 22, 31, 39, 44}},
    {"Nihavend", {0, 9, 13, 22, 31, 35, 44}}, {"Kurdi", {0, 4, 13, 22, 31, 35, 44}},
    {"Saba", {0, 8, 13, 18, 23, 35, 40}},     {"Segah", {0, 5, 14, 22, 31, 36, 44}},
};
const char* kMakamNames[kMakamCount] = {"Rast", "Ussak", "Hicaz", "Huseyni", "Nihavend", "Kurdi", "Saba", "Segah"};
const char* const kTuning[3] = {"12-TET", "24-tone", "53-TET"};
const char* const kTimbre[4] = {"Kanun", "Ney", "Saz", "Rebab"};
int degreeSemi(int m, int d) { return int(float(kMakams[m].commas[d]) * 12.0f / 53.0f + 0.5f); }

enum MkP { mMakam, mTuning, mTimbre, mBright, mAttack, mDecay, mRelease, mVib, mVibRate, mDetune, mGlide, mGain, mMkN };
const PDesc kMkDesc[mMkN] = {
    {"Makam", 0, kMakamCount - 1, 2, 1, kFList, kMakamNames},
    {"Tuning", 0, 2, 2, 1, kFList, kTuning},
    {"Timbre", 0, 3, 0, 1, kFList, kTimbre},
    {"Bright", 0, 1, 0.55f, 0.01f, kFPct, nullptr},
    {"Attack", 1, 2000, 6, 2, kFMs, nullptr},
    {"Decay", 50, 8000, 1400, 20, kFMs, nullptr},
    {"Release", 10, 4000, 250, 10, kFMs, nullptr},
    {"Vibrato", 0, 60, 12, 1, kFInt, nullptr},
    {"Vib Rate", 0.5f, 9, 5.2f, 0.1f, kFNum, nullptr},
    {"Detune", 0, 30, 6, 1, kFInt, nullptr},
    {"Glide", 0, 500, 0, 5, kFMs, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kMkSec[3] = {{"Makam", 0, 4}, {"Envelope", 4, 3}, {"Motion", 7, 5}};

class Makam final : public TableEngine {
public:
    static constexpr int kV = 4;
    static constexpr uint32_t kKs = 2048;
    explicit Makam(Arena& a) {
        bind(kMkDesc, mMkN, kMkSec, 3);
        for (auto& v : v_) v.ks = static_cast<float*>(a.alloc(kKs * sizeof(float)));
    }
    bool ok() const { return v_[kV - 1].ks != nullptr; }
    int kind() const override { return kEngMakam; }
    int key = 9;
    // Semitones (fractional) of a played note in the chosen makam / tuning.
    float tune(int note) const {
        const int t = ip(mTuning);
        if (t == 0) return float(note);
        const int m = ip(mMakam);
        const int rel = note - key;
        const int oct = rel >= 0 ? rel / 12 : -((11 - rel) / 12);
        const int pc = rel - oct * 12;
        for (int d = 0; d < 7; ++d)
            if (degreeSemi(m, d) == pc) {
                float s = float(kMakams[m].commas[d]) * 12.0f / 53.0f;
                if (t == 1) s = floorf(s * 2.0f + 0.5f) * 0.5f;
                return float(key + oct * 12) + s;
            }
        return float(note);
    }
    void noteOn(int pitch, float vel, int, int tone, int flags) override {
        V* v = nullptr;
        for (auto& x : v_)
            if (x.on && x.pitch == pitch) v = &x;
        if (!v)
            for (auto& x : v_)
                if (!x.on) { v = &x; break; }
        if (!v) {
            v = &v_[0];
            for (auto& x : v_)
                if (x.age < v->age) v = &x;
        }
        V& x = *v;
        const float semi = tune(pitch);
        const float hz = dsp::mtof(semi);
        const bool slide = (flags & kFlagSlide) && x.on;
        x.target = hz;
        if (!slide || p_[mGlide] < 1.0f) x.hz = hz;
        x.pitch = pitch;
        x.vel = vel;
        x.age = ++age_;
        x.rel = false;
        x.on = true;
        x.bright = clampf(p_[mBright] + float(tone) * (1.0f / 128.0f), 0.0f, 1.0f);
        const int tim = ip(mTimbre);
        x.timbre = uint8_t(tim);
        if (!slide) {
            x.env = 0;
            x.att = msToInc(p_[mAttack]);
            x.vph = 0;
            if (tim == 0 || tim == 2) {   // excite the string
                // one period of filtered noise; the write pointer starts after it
                const float len = kSr / (hz > 40.0f ? hz : 40.0f);
                const uint32_t n = uint32_t(len) + 3 < kKs ? uint32_t(len) + 3 : kKs;
                float lp = 0;
                const float b = tim == 2 ? 0.85f : 0.55f + 0.4f * x.bright;
                for (uint32_t i = 0; i < n; ++i) {
                    lp += (noise_.next() - lp) * b;
                    x.ks[i] = lp;
                }
                x.w = n & (kKs - 1);
            }
            x.f.set(400.0f + 5000.0f * x.bright * x.bright, 0.2f);
            x.f2.set(tim == 3 ? 1100.0f : 2000.0f, 0.6f);
        }
        x.decayK = decayK(p_[mDecay]);
        if (!slide) x.sus = 1.0f;
        x.relInc = msToInc(p_[mRelease]);
    }
    void noteOff(int pitch) override {
        for (auto& v : v_)
            if (v.on && v.pitch == pitch) v.rel = true;
    }
    void allOff(bool hard) override {
        for (auto& v : v_) {
            if (hard) v.on = false;
            else v.rel = true;
        }
    }
    void render(float* l, float* r) override {
        const float vib = p_[mVib] * (1.0f / 1200.0f), vibInc = p_[mVibRate] * kInvSr * float(kBlock);
        const float det = p_[mDetune] * (1.0f / 1200.0f);
        const float gain = p_[mGain] * 0.5f;
        const float glideK = p_[mGlide] < 1.0f ? 1.0f : 1.0f - expf(-float(kBlock) / (p_[mGlide] * 0.001f * kSr));
        for (auto& v : v_) {
            if (!v.on) continue;
            v.hz += (v.target - v.hz) * glideK;
            v.vph += vibInc;
            if (v.vph >= 1.0f) v.vph -= 1.0f;
            const float mod = 1.0f + vib * 0.69f * fsin(v.vph) * (v.env > 0.5f ? 1.0f : v.env * 2.0f);
            const float hz = v.hz * mod;
            float g = v.vel * gain;
            switch (v.timbre) {
                case 0:
                case 2: {   // Karplus-Strong string
                    const float len = clampf(kSr / hz, 2.0f, float(kKs - 2));
                    // Decay = the string's -60 dB time: feedback per period.
                    float fb = powf(v.decayK, len);
                    if (fb > 0.9995f) fb = 0.9995f;
                    const float damp = v.timbre == 2 ? 0.75f : 0.35f + 0.5f * v.bright;
                    float* ks = v.ks;
                    uint32_t w = v.w;
                    const uint32_t li = uint32_t(len);
                    const float lf = len - float(li);
                    float env = v.env;
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        uint32_t r0 = (w + kKs - li) & (kKs - 1);
                        uint32_t r1 = (r0 + kKs - 1) & (kKs - 1);
                        const float y = ks[r0] + (ks[r1] - ks[r0]) * lf;
                        const float prev = ks[(w + kKs - 1) & (kKs - 1)];
                        ks[w] = (y * (1.0f - damp) + prev * damp) * fb;
                        w = (w + 1) & (kKs - 1);
                        if (v.rel) {
                            env -= v.relInc;
                            if (env < 0) env = 0;
                        } else {
                            env = env < 1.0f ? env + v.att : 1.0f;
                            if (env > 1.0f) env = 1.0f;
                        }
                        const float s = y * env * g * 1.6f;
                        l[i] += s;
                        r[i] += s;
                    }
                    v.w = w;
                    v.env = env;
                    if (v.rel && env <= 0.0f) v.on = false;
                    v.sus *= powK(v.decayK);   // the string dies out by itself
                    if (v.sus < 0.001f) v.on = false;
                    break;
                }
                case 1: {   // Ney: sine + octave + breath
                    const float inc = hz * kInvSr, inc2 = hz * (1.0f + det) * 2.0f * kInvSr;
                    float env = v.env;
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        v.ph += inc;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        v.ph2 += inc2;
                        if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                        const float breath = v.f2.bp(noise_.next()) * (0.15f + 0.25f * v.bright);
                        const float s = (fsin(v.ph) + 0.18f * fsin(v.ph2) + breath) * env * g;
                        l[i] += s;
                        r[i] += s;
                        if (v.rel) {
                            env -= v.relInc;
                            if (env < 0) env = 0;
                        } else if (env < 1.0f) {
                            env += v.att;
                            if (env > 1.0f) env = 1.0f;
                        }
                    }
                    v.env = env;
                    if (v.rel && env <= 0.0f) v.on = false;
                    break;
                }
                default: {   // Rebab: two detuned saws through a body formant
                    const float inc = hz * kInvSr, inc2 = hz * (1.0f + det) * kInvSr;
                    float env = v.env;
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        v.ph += inc;
                        if (v.ph >= 1.0f) v.ph -= 1.0f;
                        v.ph2 += inc2;
                        if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                        const float saw = (v.ph + v.ph2 - 1.0f);
                        const float body = v.f.lp(saw) * 0.8f + v.f2.bp(saw) * 0.9f;
                        const float s = body * env * g;
                        l[i] += s;
                        r[i] += s;
                        if (v.rel) {
                            env -= v.relInc;
                            if (env < 0) env = 0;
                        } else if (env < 1.0f) {
                            env += v.att;
                            if (env > 1.0f) env = 1.0f;
                        }
                    }
                    v.env = env;
                    if (v.rel && env <= 0.0f) v.on = false;
                    break;
                }
            }
        }
    }
    int active() const override {
        int n = 0;
        for (auto& v : v_) n += v.on;
        return n;
    }
    int32_t cost() const override {
        int32_t c = 0;
        for (auto& v : v_)
            if (v.on) c += v.timbre == 3 ? 9000 : (v.timbre == 1 ? 7500 : 6200);
        return c ? c + 800 : 0;
    }
    int32_t noteCost() const override { return 9000; }
    bool steal() override {
        V* o = nullptr;
        for (auto& v : v_)
            if (v.on && (!o || v.age < o->age)) o = &v;
        if (!o) return false;
        o->on = false;
        return true;
    }

private:
    static float powK(float k) {   // k^kBlock by squaring (kBlock = 128 = 2^7)
        for (int i = 0; i < 7; ++i) k *= k;
        return k;
    }
    struct V {
        bool on = false, rel = false;
        uint8_t timbre = 0;
        int pitch = 0;
        uint32_t age = 0, w = 0;
        float hz = 220, target = 220, vel = 1, env = 0, att = 0.01f, relInc = 0.001f, ph = 0, ph2 = 0, vph = 0,
              bright = 0.5f, decayK = 0.9999f, sus = 1.0f;
        float* ks = nullptr;
        Filt f, f2;
    };
    V v_[kV];
    Noise noise_;
    uint32_t age_ = 0;
};

}  // namespace

// --- factory hooks used by engines_common.cpp ----------------------------------
Engine* makeDrums(Arena& a) {
    void* m = a.alloc(sizeof(Drums), 16);
    return m ? new (m) Drums() : nullptr;
}
Engine* makeIkeda(Arena& a) {
    void* m = a.alloc(sizeof(Ikeda), 16);
    return m ? new (m) Ikeda() : nullptr;
}
Engine* makeMakam(Arena& a) {
    void* m = a.alloc(sizeof(Makam), 16);
    if (!m) return nullptr;
    Makam* k = new (m) Makam(a);
    return k->ok() ? k : nullptr;
}
void makamSetKey(Engine* e, int key) {
    if (e && e->kind() == kEngMakam) static_cast<Makam*>(e)->key = key;
}
float makamTune(Engine* e, int note) {
    return (e && e->kind() == kEngMakam) ? static_cast<Makam*>(e)->tune(note) : float(note);
}

const char* makamName(int m) { return kMakams[m < 0 ? 0 : (m >= kMakamCount ? kMakamCount - 1 : m)].name; }
uint16_t makamMask(int m) {
    m = m < 0 ? 0 : (m >= kMakamCount ? kMakamCount - 1 : m);
    uint16_t mask = 0;
    for (int d = 0; d < 7; ++d) mask |= uint16_t(1u << (degreeSemi(m, d) % 12));
    return mask;
}

}  // namespace konsol
}  // namespace lab
