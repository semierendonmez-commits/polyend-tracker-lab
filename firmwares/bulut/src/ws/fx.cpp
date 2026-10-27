// BULUT per-channel insert chain and generators (fx.h).
//
// The FLANGER, PANNER, CRUSHER, SLICER and FREEZER modes follow forsitan
// artifex's FX core (forsitan modulare, GPL-3.0, artifex/core.hpp): their
// ranges, laws and the rhythm table are taken from it; the code is a float,
// block-rate rewrite for the Cortex-M4 (no double, no per-sample libm).
// The generator RETE follows forsitan modulare's rete (GPL-3.0) as ported in
// RETICULUM (Source/engine/Modules.h), run at half rate.
#include "ws/fx.h"

#include <cmath>
#include <cstring>

#include "fastrun.h"
#include "ws/prof.h"

namespace bulut {

using lab::kBlock;
namespace dsp = lab::dsp;

namespace {
clouds::FloatFrame gFrames[kBlock];   // one channel at a time (the audio interrupt is not re-entrant)
constexpr float kEdgeFade = 0.004f;   // freezer splice
constexpr float kSliceAttack = 0.002f;

inline float fastSigmoid(float x, float h) { return x * (1.0f + h) / (1.0f + h * fabsf(x)); }
inline float soft(float x) {   // |x| < 3 smooth, odd harmonics (lab::dsp::softclip, inlined)
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
inline void renorm(float& c, float& s) {
    const float k = 1.5f - 0.5f * (c * c + s * s);
    c *= k;
    s *= k;
}
// sin / cos of an angle in turns (block rate only)
inline void sincosTurns(float turns, float& s, float& c) {
    const float a = turns * dsp::kTwoPi;
    s = sinf(a);
    c = cosf(a);
}
}  // namespace

uint16_t rhythmPattern(int i) {
    static const uint16_t hand[22] = {0x8888, 0x0808, 0x2222, 0xAAAA, 0xFFFF, 0x9292, 0x9228, 0x9128,
                                      0x9224, 0xA94A, 0x9999, 0xB2B2, 0x8000, 0x8080, 0x9249, 0x9632,
                                      0x8100, 0x9222, 0x9294, 0x922A, 0x2A2A, 0xB4B4};
    static const int dens[10] = {3, 5, 7, 9, 10, 11, 12, 13, 14, 15};
    i = i < 0 ? 0 : (i > 31 ? 31 : i);
    if (i < 22) return hand[i];
    const int k = dens[i - 22];
    uint16_t p = 0;
    for (int s = 0; s < 16; ++s)
        if ((s * k) % 16 < k) p = uint16_t(p | (0x8000u >> s));
    return p;
}

// Grain units (fx.h): instructions per block / 3,500, measured with the
// BULUT_PROF bench build (TEXTURE 10.4k, SHIFTER 11.3k, FLANGER 12.3k,
// CRUSHER 4.9k, FREEZER 4.0k, drive 3.9k, filter 3.3k, DIRTY TAPE media +
// mu-law 9.5k, RETE 17.5k, TONE / NOISE ~9.5k a block).
float baseUnits(const ChParams& p, bool beadsTape) {
    float u = 0.0f;
    if (p.drive > 0.01f) u += 1.15f;
    if (p.fltMode != kFltOff) u += 1.0f;
    if (beadsTape) u += 2.75f;
    return u;
}
float fxUnits(int m) {
    static const float kFx[kFxModes] = {0.0f, 3.05f, 3.3f, 3.6f, 1.0f, 1.45f, 0.8f, 1.2f};
    return (m > 0 && m < kFxModes) ? kFx[m] : 0.0f;
}
float genUnits(int genType) { return genType == kGenRete ? 5.1f : 2.8f; }

// ---- chain ---------------------------------------------------------------------
bool ChannelChain::init(lab::Arena& a, float* diffuserSram) {
    uint16_t* shBuf = static_cast<uint16_t*>(a.alloc(4096 * sizeof(uint16_t)));
    flg_[0] = static_cast<float*>(a.alloc(kFlgLen * sizeof(float)));
    flg_[1] = static_cast<float*>(a.alloc(kFlgLen * sizeof(float)));
    fz_[0] = static_cast<int16_t*>(a.alloc(kFrzRing * sizeof(int16_t)));
    fz_[1] = static_cast<int16_t*>(a.alloc(kFrzRing * sizeof(int16_t)));
    if (!shBuf || !flg_[0] || !flg_[1] || !fz_[0] || !fz_[1] || !diffuserSram) return false;
    diff_.Init(diffuserSram);
    shift_.Init(shBuf);
    wow_.Init(kSr * 0.5f);   // run at half rate (media())
    wow_.set_lp(0.8f);
    wow_.set_amount(0.5f);   // the same depth in time as at full rate
    reset();
    return true;
}

void ChannelChain::reset() {
    for (int c = 0; c < 2; ++c) {
        texPrev_[c] = wowPrev_[c] = 0.0f;
        ic1[c] = ic2[c] = 0.0f;
        texFb_[c] = texLp_[c] = shFb_[c] = 0.0f;
        crPh_[c] = crHold_[c] = crLp_[c] = crY_[c] = 0.0f;
        slEnv_[c] = slAtk_[c] = 0.0f;
        medLp_[c] = 0.0f;
    }
    std::memset(flg_[0], 0, kFlgLen * sizeof(float));
    std::memset(flg_[1], 0, kFlgLen * sizeof(float));
    fzFrozen_ = false;
    fzRecorded_ = 0;
    fzOpen_ = 0.0f;
}

void ChannelChain::drive(float* L, float* R, float amt) {
    // a cubic soft clip (flat at +-1 from |x| = 1.5): a few instructions a sample
    const float g = 1.0f + amt * amt * 24.0f;
    const float mk = 1.0f / (1.0f + 0.6f * amt);
    const float k3 = 1.0f / 6.75f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float l = L[i] * g, r = R[i] * g;
        l = l > 1.5f ? 1.5f : (l < -1.5f ? -1.5f : l);
        r = r > 1.5f ? 1.5f : (r < -1.5f ? -1.5f : r);
        L[i] = (l - l * l * l * k3) * mk;
        R[i] = (r - r * r * r * k3) * mk;
    }
}

void ChannelChain::filter(float* L, float* R, int mode, float hz, float reso) {
    if (hz != fltHz_ || reso != fltRes_) {
        fltHz_ = hz;
        fltRes_ = reso;
        const float f = dsp::clamp(hz, 20.0f, 18000.0f);
        g_ = tanf(dsp::kPi * f * dsp::kInvSr);
        k_ = 2.0f - 1.95f * dsp::clamp(reso, 0.0f, 1.0f);
    }
    const float g = g_, k = k_;
    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1;
    float* io[2] = {L, R};
    for (int c = 0; c < 2; ++c) {
        float s1 = ic1[c], s2 = ic2[c];
        float* x = io[c];
        // one loop per mode (no branch per sample)
#define BULUT_SVF_STEP                         \
    const float v3 = x[i] - s2;                \
    const float v1 = a1 * s1 + a2 * v3;        \
    const float v2 = s2 + g * v1;              \
    s1 = 2.0f * v1 - s1;                       \
    s2 = 2.0f * v2 - s2;
        if (mode == kFltLp) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                BULUT_SVF_STEP
                x[i] = v2;
            }
        } else if (mode == kFltBp) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                BULUT_SVF_STEP
                x[i] = v1 * k;
            }
        } else {
            for (uint32_t i = 0; i < kBlock; ++i) {
                BULUT_SVF_STEP
                x[i] = x[i] - k * v1 - v2;
            }
        }
#undef BULUT_SVF_STEP
        if (!(s1 > -1e6f && s1 < 1e6f) || !(s2 > -1e6f && s2 < 1e6f)) s1 = s2 = 0.0f;
        ic1[c] = s1;
        ic2[c] = s2;
    }
}

// TEXTURE: Clouds' diffuser, run fully wet at half rate (22.05 kHz: half the
// cost, a band-limited smear) and mixed with the dry signal at full rate.
// FEEDBACK feeds the diffused signal back into it (a smear towards reverb),
// TIME is the loop's tone (dark .. bright), AMOUNT the wet mix.
void ChannelChain::texture(float* L, float* R, const ChParams& p) {
    constexpr uint32_t kHalf = kBlock / 2;
    const float fb = p.fxFb * 0.85f;
    const float lp = 0.05f + 0.9f * p.fxTime * p.fxTime;
    const float fl = texFb_[0] * fb, fr = texFb_[1] * fb;
    for (uint32_t i = 0; i < kHalf; ++i) {
        gFrames[i].l = 0.5f * (L[2 * i] + L[2 * i + 1]) + fl;
        gFrames[i].r = 0.5f * (R[2 * i] + R[2 * i + 1]) + fr;
    }
    diff_.set_amount(1.0f);
    diff_.Process(gFrames, kHalf);
    const float wet = 0.2f + 0.8f * p.fxAmt;
    float pl = texPrev_[0], pr = texPrev_[1];
    float zl = texLp_[0], zr = texLp_[1];
    for (uint32_t i = 0; i < kHalf; ++i) {
        const float wl = gFrames[i].l, wr = gFrames[i].r;
        const float ml = 0.5f * (pl + wl), mr = 0.5f * (pr + wr);
        L[2 * i] += wet * (ml - L[2 * i]);
        R[2 * i] += wet * (mr - R[2 * i]);
        L[2 * i + 1] += wet * (wl - L[2 * i + 1]);
        R[2 * i + 1] += wet * (wr - R[2 * i + 1]);
        pl = wl;
        pr = wr;
        zl += lp * (wl - zl);
        zr += lp * (wr - zr);
    }
    texPrev_[0] = pl;
    texPrev_[1] = pr;
    texLp_[0] = zl;
    texLp_[1] = zr;
    texFb_[0] = soft(zl);
    texFb_[1] = soft(zr);
    uiValue = p.fxAmt;
}

// SHIFTER: Clouds' pitch shifter, at half rate (22.05 kHz, like TEXTURE: half
// the cost) and fully wet. AMOUNT = -24 .. +24 semitones (centre 0, whole
// semitones), TIME = window, FEEDBACK = shimmer (the shifted signal back in).
void ChannelChain::shifter(float* L, float* R, const ChParams& p) {
    constexpr uint32_t kHalf = kBlock / 2;
    const float semis = floorf((p.fxAmt - 0.5f) * 48.0f + 0.5f);
    shift_.set_ratio(fexp2(semis * (1.0f / 12.0f)));
    shift_.set_size(0.15f + 0.85f * p.fxTime);
    const float fb = p.fxFb * 0.8f;
    const float fl = shFb_[0] * fb, fr = shFb_[1] * fb;
    for (uint32_t i = 0; i < kHalf; ++i) {
        gFrames[i].l = 0.5f * (L[2 * i] + L[2 * i + 1]) + fl;
        gFrames[i].r = 0.5f * (R[2 * i] + R[2 * i + 1]) + fr;
    }
    shift_.Process(gFrames, kHalf);
    float pl = texPrev_[0], pr = texPrev_[1];
    for (uint32_t i = 0; i < kHalf; ++i) {
        const float wl = gFrames[i].l, wr = gFrames[i].r;
        L[2 * i] = 0.5f * (pl + wl);
        R[2 * i] = 0.5f * (pr + wr);
        L[2 * i + 1] = wl;
        R[2 * i + 1] = wr;
        pl = wl;
        pr = wr;
    }
    texPrev_[0] = pl;
    texPrev_[1] = pr;
    shFb_[0] = soft(gFrames[kHalf - 1].l);
    shFb_[1] = soft(gFrames[kHalf - 1].r);
    uiValue = semis;
}

// FLANGER (artifex 2): a 5.5 ms line swept +-5 ms by a sine at 0.02 Hz .. 50 Hz.
void ChannelChain::flanger(float* L, float* R, const ChParams& p) {
    const float hz = 0.02f * fexp2(11.287712f * p.fxTime);   // 0.02 * 2500^t
    const float amt = p.fxAmt;
    flgDepth_ += (amt - flgDepth_) * 0.15f;
    const float base = 5.5f * 0.001f * kSr, depth = 5.0f * flgDepth_ * 0.001f * kSr;
    const float fb = p.fxFb * 0.95f;
    const float w = hz * dsp::kInvSr;
    // the sweep moves in 16-sample segments (a linear ramp of the delay inside)
    constexpr uint32_t kSeg = 16;
    float sw, cw;
    sincosTurns(w * float(kSeg), sw, cw);
    const float dry = 1.0f - 0.5f * amt;
    const float k3 = 1.0f / 6.75f;
    float* io[2] = {L, R};
    const uint32_t wpos = flgW_;
    for (int c = 0; c < 2; ++c) {
        float* x = io[c];
        float* line = flg_[c];
        float s, co;
        sincosTurns(modPh_[c], s, co);
        uint32_t wp = wpos;
        float d0 = base + depth * s;
        for (uint32_t seg = 0; seg < kBlock; seg += kSeg) {
            const float t = co * cw - s * sw;
            s = s * cw + co * sw;
            co = t;
            const float d1 = base + depth * s;
            const float dd = (d1 - d0) * (1.0f / float(kSeg));
            float d = d0;
            for (uint32_t i = seg; i < seg + kSeg; ++i) {
                const float rp = float(wp + kFlgLen) - d;   // always positive
                const uint32_t i0 = uint32_t(rp);
                const float fr = rp - float(i0);
                const float a = line[i0 & kFlgMask], bb = line[(i0 + 1) & kFlgMask];
                const float wet = a + (bb - a) * fr;
                float v = x[i] + wet * fb;
                v = v > 1.5f ? 1.5f : (v < -1.5f ? -1.5f : v);
                line[wp] = v - v * v * v * k3;
                wp = (wp + 1) & kFlgMask;
                x[i] = x[i] * dry + wet * amt;
                d += dd;
            }
            d0 = d1;
        }
        modPh_[c] += w * float(kBlock);
        modPh_[c] -= floorf(modPh_[c]);
    }
    flgW_ = (wpos + kBlock) & kFlgMask;
    uiValue = hz;
}

// PANNER (artifex 4): a sine at 0.1 Hz .. 10 kHz moves the image; AMOUNT
// deepens it, its top half squares the sine (auto-pan -> ring modulation).
void ChannelChain::panner(float* L, float* R, const ChParams& p) {
    float hz = 0.1f * fexp2(16.609640f * p.fxTime);   // 0.1 * 100000^t
    if (hz > 10000.0f) hz = 10000.0f;
    const float depth = dsp::clamp(p.fxAmt * 2.0f, 0.0f, 1.0f);
    const float hard = dsp::clamp(p.fxAmt * 2.0f - 1.0f, 0.0f, 1.0f) * 30.0f;
    const float w = hz * dsp::kInvSr;
    float sw, cw, s, c;
    sincosTurns(w, sw, cw);
    sincosTurns(panPh_, s, c);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float m = fastSigmoid(s, hard);
        L[i] *= 1.0f - depth * 0.5f * (1.0f - m);
        R[i] *= 1.0f - depth * 0.5f * (1.0f + m);
        const float t = c * cw - s * sw;
        s = s * cw + c * sw;
        c = t;
    }
    panPh_ += w * float(kBlock);
    panPh_ -= floorf(panPh_);
    uiValue = hz;
}

// CRUSHER (artifex 5): sample-and-hold at 7.5 Hz .. 44.1 kHz, 12 .. 3 bits,
// Gray-code mangling in AMOUNT's top half, a DC-blocked loop around the hold.
void ChannelChain::crusher(float* L, float* R, const ChParams& p) {
    const float rate = 7.5f * fexp2(12.522f * p.fxTime);   // 7.5 * (44100 / 7.5)^t
    const float amt = p.fxAmt;
    const float crush = dsp::clamp((amt - 0.1f) / 0.4f, 0.0f, 1.0f);
    const int levels = int(fexp2(12.0f - 9.0f * crush) + 0.5f);
    const float xorAmt = dsp::clamp(amt * 2.0f - 1.0f, 0.0f, 1.0f);
    const float wet = dsp::clamp(amt * 10.0f, 0.0f, 1.0f);
    const float fb = p.fxFb * 1.25f;
    const float inc = (rate > kSr ? kSr : rate) * dsp::kInvSr;
    const float corner = dsp::clamp(rate * 0.012f, 4.0f, 400.0f);
    const float hpC = dsp::clamp(1.0f - fexp(-dsp::kTwoPi * corner / (rate < 1.0f ? 1.0f : rate)), 0.0f, 1.0f);
    const float inv = 1.0f / float(levels);
    float* io[2] = {L, R};
    for (int c = 0; c < 2; ++c) {
        float* x = io[c];
        float ph = crPh_[c], hold = crHold_[c], lp = crLp_[c], y = crY_[c];
        for (uint32_t i = 0; i < kBlock; ++i) {
            ph += inc;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                lp += (y - lp) * hpC;
                const float s = soft(x[i] + (y - lp) * fb);
                const int k = int(fabsf(s) * float(levels) + 0.5f);
                float q = float(k) * inv;
                if (xorAmt > 0.001f) {
                    const int gcode = (k ^ (k >> 1)) % (levels + 1);
                    q += (float(gcode) * inv - q) * xorAmt;
                }
                q = q > 1.0f ? 1.0f : q;
                hold = s < 0.0f ? -q : q;
            }
            y = x[i] * (1.0f - wet) + hold * wet;
            x[i] = y;
        }
        crPh_[c] = ph;
        crHold_[c] = hold;
        crLp_[c] = lp;
        crY_[c] = y;
    }
    uiValue = rate;
}

// SLICER (artifex 6): the rhythm table on the amplitude, 16ths of the tempo.
// TIME = pattern 1..32, AMOUNT = decay (and wet), FEEDBACK = chance a step flips.
void ChannelChain::slicer(float* L, float* R, const ChParams& p, const BlockClock& bc) {
    const int pat = int(p.fxTime * 31.999f);
    const float amt = p.fxAmt;
    const float slice = dsp::clamp((amt - 0.1f) / 0.9f, 0.0f, 1.0f);
    const float decay = fexp2(-4.0588937f * slice);   // 0.06^slice
    const float coef = fexp(-dsp::kInvSr / (decay * 0.5f));
    const float wet = dsp::clamp(amt * 10.0f, 0.0f, 1.0f);
    const float makeup = 1.0f + 0.6f * slice;
    const uint16_t bits = rhythmPattern(pat);
    // 16th-note steps of the block
    const double s16 = bc.beat * 4.0;
    const int64_t first = int64_t(s16);
    float f = float(s16 - double(first));
    const float df = bc.beatsPerSample * 4.0f;
    int step = int(first & 15);
    float* io[2] = {L, R};
    float env0 = slEnv_[0], env1 = slEnv_[1], atk0 = slAtk_[0], atk1 = slAtk_[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (step != slLastStep_) {
            slLastStep_ = step;
            bool hit = (bits & uint16_t(0x8000u >> step)) != 0;
            if (p.fxFb > 0.001f && rng_.uni() < p.fxFb * 0.7f) hit = !hit;
            if (hit) atk0 = atk1 = kSliceAttack;
        }
        if (atk0 > 0.0f) {
            atk0 -= dsp::kInvSr;
            env0 = env0 + dsp::kInvSr / kSliceAttack;
            env0 = env0 > 1.0f ? 1.0f : env0;
        } else {
            env0 *= coef;
        }
        atk1 = atk0;
        env1 = env0;
        const float g = (1.0f - wet) + env0 * makeup * wet;
        io[0][i] *= g;
        io[1][i] *= g;
        f += df;
        if (f >= 1.0f) {
            f -= 1.0f;
            step = (step + 1) & 15;
        }
    }
    slEnv_[0] = env0;
    slEnv_[1] = env1;
    slAtk_[0] = atk0;
    slAtk_[1] = atk1;
    uiValue = float(pat + 1);
}

// FREEZER (artifex 3): AMOUNT rising from zero catches the last loop; TIME =
// length (left half: bar / 1..16 at the tempo, right half: 20 .. 3.4 ms, a
// pitch); FEEDBACK = roll: the chance that a loop pass halves the length.
void ChannelChain::freezer(float* L, float* R, const ChParams& p, const BlockClock& bc) {
    const float t = p.fxTime, amt = p.fxAmt;
    float len;
    if (t < 0.5f) {
        static const float div[8] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f};
        const int i = int(t * 16.0f) > 7 ? 7 : int(t * 16.0f);
        float bar = 240.0f / (bc.bpm > 20.0f ? bc.bpm : 120.0f);
        while (bar > 1.2f) bar *= 0.5f;
        len = bar / div[i];
        uiValue = div[i];
    } else {
        len = 0.020f * fexp2(-2.5564f * (t - 0.5f) * 2.0f);   // 20 ms * 0.17^x
        uiValue = len * 1000.0f;
    }
    const float lenS = len * kSr;
    const float splice = kEdgeFade * kSr;
    const bool ready = float(fzRecorded_) >= lenS + splice;
    if (amt > 0.02f && fzWasSilent_ && ready && !fzFrozen_) {
        fzFrozen_ = true;
        fzEnd_ = fzW_;
        fzPos_ = 0.0f;
        fzLen_ = lenS;
        fzOpen_ = 0.0f;
    }
    if (amt <= 0.02f && fzFrozen_) fzFrozen_ = false;
    fzWasSilent_ = amt <= 0.02f;
    if (!fzFrozen_) {
        // record (the head runs while nothing is frozen)
        int16_t* a = fz_[0];
        int16_t* b = fz_[1];
        uint32_t w = fzW_;
        for (uint32_t i = 0; i < kBlock; ++i) {
            a[w] = int16_t(dsp::clamp(L[i], -1.0f, 1.0f) * 32767.0f);
            b[w] = int16_t(dsp::clamp(R[i], -1.0f, 1.0f) * 32767.0f);
            w = (w + 1) & kFrzRingMask;
        }
        fzW_ = w;
        if (fzRecorded_ < kFrzRing) fzRecorded_ += kBlock;
        return;
    }
    // play the loop: [end - len, end), its wrap faded into the lead-in before it
    const float wet = amt;
    const float k = 1.0f / 32768.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float ln = fzLen_;
        const float fade = splice < 0.25f * ln ? splice : 0.25f * ln;
        const float base = float(fzEnd_) - ln;
        float pos = base + fzPos_;
        if (pos < 0.0f) pos += float(kFrzRing);
        uint32_t i0 = uint32_t(pos) & kFrzRingMask;
        float v0 = float(fz_[0][i0]) * k, v1 = float(fz_[1][i0]) * k;
        const float into = fzPos_ - (ln - fade);
        if (into > 0.0f && fade >= 1.0f) {
            const float x = into / fade;
            float lp = pos - ln;
            if (lp < 0.0f) lp += float(kFrzRing);
            const uint32_t j = uint32_t(lp) & kFrzRingMask;
            v0 += (float(fz_[0][j]) * k - v0) * x;
            v1 += (float(fz_[1][j]) * k - v1) * x;
        }
        fzOpen_ = fzOpen_ + 1.0f / splice;
        const float o = (fzOpen_ > 1.0f ? 1.0f : fzOpen_) * wet;
        L[i] += (v0 - L[i]) * o;
        R[i] += (v1 - R[i]) * o;
        fzPos_ += 1.0f;
        if (fzPos_ >= ln) {
            fzPos_ -= ln;
            // the next pass takes the new length; FEEDBACK may roll it in half
            float nl = lenS;
            if (p.fxFb > 0.001f && rng_.uni() < p.fxFb * 0.6f) nl = (ln * 0.5f > 64.0f) ? ln * 0.5f : ln;
            const float maxLen = float(fzRecorded_) - splice;
            fzLen_ = nl < maxLen ? nl : maxLen;
            if (fzLen_ < 32.0f) fzLen_ = 32.0f;
            if (fzPos_ >= fzLen_) fzPos_ = 0.0f;
        }
    }
}

// Beads' media on the channel output (the buffer side is in engine.cpp):
// CLEAN TAPE and LO-FI soften the top, DIRTY TAPE adds Beads' wow and flutter.
void ChannelChain::media(float* L, float* R, int m) {
    if (m == kMediaDirtyTape) {
        // Beads' wow and flutter at half rate (it is a slow, band-limited wobble)
        constexpr uint32_t kHalf = kBlock / 2;
        for (uint32_t i = 0; i < kHalf; ++i) {
            frames_[i].l = 0.5f * (L[2 * i] + L[2 * i + 1]);
            frames_[i].r = 0.5f * (R[2 * i] + R[2 * i + 1]);
        }
        wow_.Process(frames_, kHalf);
        float pl = wowPrev_[0], pr = wowPrev_[1];
        for (uint32_t i = 0; i < kHalf; ++i) {
            const float wl = frames_[i].l, wr = frames_[i].r;
            L[2 * i] = 0.5f * (pl + wl);
            R[2 * i] = 0.5f * (pr + wr);
            L[2 * i + 1] = wl;
            R[2 * i + 1] = wr;
            pl = wl;
            pr = wr;
        }
        wowPrev_[0] = pl;
        wowPrev_[1] = pr;
    }
    const float a = m == kMediaCleanTape ? 0.75f : (m == kMediaLofi ? 0.85f : 0.55f);
    float zl = medLp_[0], zr = medLp_[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        zl += a * (L[i] - zl);
        zr += a * (R[i] - zr);
        L[i] = zl;
        R[i] = zr;
    }
    medLp_[0] = zl;
    medLp_[1] = zr;
}

OT_FASTRUN_FN void ChannelChain::process(float* L, float* R, const ChParams& p, int med, const BlockClock& bc, bool fxOn) {
    PROF_MARK(4);
    if (p.drive > 0.01f) drive(L, R, p.drive);
    PROF_MARK(10);
    if (p.fltMode != kFltOff) filter(L, R, p.fltMode, p.cutoff, p.reso);
    PROF_MARK(11);
    const int m = fxOn ? p.fxMode : int(kFxOff);
    if (m != lastFx_) {
        // a new mode starts from silence (no stale lines), the freezer re-arms
        lastFx_ = m;
        fzFrozen_ = false;
        fzWasSilent_ = true;
        texFb_[0] = texFb_[1] = shFb_[0] = shFb_[1] = 0.0f;
    }
    switch (m) {
        case kFxTexture: texture(L, R, p); break;
        case kFxShifter: shifter(L, R, p); break;
        case kFxFlanger: flanger(L, R, p); break;
        case kFxPanner: panner(L, R, p); break;
        case kFxCrusher: crusher(L, R, p); break;
        case kFxSlicer: slicer(L, R, p, bc); break;
        case kFxFreezer: freezer(L, R, p, bc); break;
        default: break;
    }
    PROF_MARK(12 + (m & 7));
    if (med != kMediaHifi) media(L, R, med);
    PROF_MARK(4);
    // tails: modes with memory keep running ~1.5 s after the input went quiet
    tail_ = (m == kFxTexture || m == kFxShifter || m == kFxFlanger || m == kFxFreezer || med == kMediaDirtyTape) ? 520 : 0;
}

// ---- generators ------------------------------------------------------------------
void Generator::init(uint32_t seed) { reseed(seed); }

void Generator::reseed(uint32_t s) {
    seed_ = s ? s : 1u;
    uint32_t r = seed_;
    auto bip = [&r]() {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return float(r >> 8) * (2.0f / 16777216.0f) - 1.0f;
    };
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) matrix_[i][j] = bip();
    for (int i = 0; i < 8; ++i) {
        integ_[i] = 0.01f * bip();
        hpX_[i] = hpY_[i] = fb_[i] = 0.0f;
        gain_[i] = 0.45f + 0.5f * (0.5f + 0.5f * bip());
    }
}

// rete: eight leaky integrators -> the seeded 8 x 8 matrix -> DC block -> clip
// -> fed back one step later; excite = a slow random walk (MG-16's role in the
// RETICULUM patch). Quarter rate (11.025 kHz), linear interpolation to 44.1 kHz;
// the state lives in locals for the block (registers, not memory).
void Generator::rete(float leakK, float scaleK, float* out, uint32_t n) {
    // laws of rete.cpp at 48 kHz, rescaled for the 11.025 kHz step
    const float srRatio = 48000.0f / 11025.0f;
    if (leakK != reteLeakK_ || scaleK != reteScaleK_) {
        reteLeakK_ = leakK;
        reteScaleK_ = scaleK;
        const float leak48 = 1.0f - fexp2(-3.3219281f * (1.0f + 4.0f * dsp::clamp(leakK, 0.0f, 1.0f)));
        reteLeak_ = fexp2(srRatio * flog2(leak48));
        reteScale_ = fexp2(9.9657843f * dsp::clamp(scaleK, 0.0f, 1.0f));   // 10^(3k)
        reteDc_ = fexp2(srRatio * flog2(0.995f));
    }
    const float leak = reteLeak_, scale = reteScale_, dcR = reteDc_;
    // excite: a held random value every ~0.4 s, glided
    if (++excCount_ >= 140) {
        excCount_ = 0;
        excT_ = rng_.bi() * 2.0f;
    }
    float in[8], fb[8], hx[8], hy[8], gs[8];
    for (int i = 0; i < 8; ++i) {
        in[i] = integ_[i];
        fb[i] = fb_[i];
        hx[i] = hpX_[i];
        hy[i] = hpY_[i];
        gs[i] = gain_[i] * scale;
    }
    float last = half_[0];
    uint32_t nz = noise_;
    float exc = exc_;
    const float excT = excT_;
    for (uint32_t s = 0; s < n; s += 4) {
        exc += (excT - exc) * 0.004f;
        const float excite = exc * 0.04f;
        nz ^= nz << 13;
        nz ^= nz >> 17;
        nz ^= nz << 5;
        const float life = 1.0e-5f * (float(nz >> 8) * (2.0f / 16777216.0f) - 1.0f);
        float gi[8];
        for (int i = 0; i < 8; ++i) {
            in[i] = leak * in[i] + fb[i] + ((i & 1) ? -excite : excite) + life;
            gi[i] = gs[i] * in[i];
        }
        float sum = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float* row = matrix_[i];
            const float m = row[0] * gi[0] + row[1] * gi[1] + row[2] * gi[2] + row[3] * gi[3] + row[4] * gi[4] +
                            row[5] * gi[5] + row[6] * gi[6] + row[7] * gi[7];
            const float y = m - hx[i] + dcR * hy[i];
            hx[i] = m;
            hy[i] = y;
            const float c = y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
            fb[i] = c;
            sum += c;
        }
        const float v = sum * 0.25f;   // +-2 at most; the buffer write soft-clips
        const float d = (v - last) * 0.25f;
        out[s] = last + d;
        out[s + 1] = last + 2.0f * d;
        out[s + 2] = last + 3.0f * d;
        out[s + 3] = v;
        last = v;
    }
    // a blown-up state (NaN / huge) restarts quietly
    bool bad = false;
    for (int i = 0; i < 8; ++i) bad = bad || !(hy[i] > -1e6f && hy[i] < 1e6f) || !(in[i] > -1e9f && in[i] < 1e9f);
    for (int i = 0; i < 8; ++i) {
        integ_[i] = bad ? 0.0f : in[i];
        fb_[i] = bad ? 0.0f : fb[i];
        hpX_[i] = bad ? 0.0f : hx[i];
        hpY_[i] = bad ? 0.0f : hy[i];
        node[i] = fb_[i];
    }
    half_[0] = bad ? 0.0f : last;
    noise_ = nz;
    exc_ = exc;
}

OT_FASTRUN_FN void Generator::render(int type, float a, float b, int16_t* out) {
    float buf[kBlock];
    if (type == kGenRete) {
        rete(a, b, buf, kBlock);
    } else if (type == kGenTone) {
        // a drone: two detuned saws (polyBLEP) and a sine, B blends sine -> saws
        const float note = 24.0f + a * 72.0f;
        const float inc = dsp::mtof(note) * dsp::kInvSr, inc2 = inc * 1.0058f;
        const float mixS = b;
        for (uint32_t i = 0; i < kBlock; ++i) {
            ph_ += inc;
            if (ph_ >= 1.0f) ph_ -= 1.0f;
            ph2_ += inc2;
            if (ph2_ >= 1.0f) ph2_ -= 1.0f;
            const float s1 = 2.0f * ph_ - 1.0f - dsp::Osc::blep(ph_, inc);
            const float s2 = 2.0f * ph2_ - 1.0f - dsp::Osc::blep(ph2_, inc2);
            const float tri = 4.0f * (ph_ < 0.5f ? ph_ : 1.0f - ph_) - 1.0f;
            buf[i] = 0.5f * ((s1 + s2) * 0.5f * mixS + tri * (1.0f - mixS));
        }
        for (int i = 0; i < 8; ++i) node[i] *= 0.9f;
    } else {
        // noise: A = colour (white .. pink .. brown), B = density (dust .. continuous)
        const float col = a;
        const float pdust = b * b;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float w = rng_.bi();
            pink_[0] = 0.99765f * pink_[0] + w * 0.0990460f;
            pink_[1] = 0.96300f * pink_[1] + w * 0.2965164f;
            pink_[2] = 0.57000f * pink_[2] + w * 1.0526913f;
            const float pk = (pink_[0] + pink_[1] + pink_[2] + w * 0.1848f) * 0.25f;
            dust_ = dust_ * 0.995f + w * 0.02f;
            float v = col < 0.5f ? w + (pk - w) * col * 2.0f : pk + (dust_ * 4.0f - pk) * (col - 0.5f) * 2.0f;
            if (pdust < 0.98f && rng_.uni() > pdust) v *= 0.03f;   // sparse: dust impulses
            buf[i] = v * 0.5f;
        }
        for (int i = 0; i < 8; ++i) node[i] *= 0.9f;
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float v = soft(buf[i]) * 32000.0f;
        out[i] = int16_t(v);
    }
}

}  // namespace bulut
