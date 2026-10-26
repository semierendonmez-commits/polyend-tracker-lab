// KONSOL send effects (fx.h).
#include "ws/fx.h"

#include <cmath>
#include <cstring>

#include "ws/dspx.h"

namespace lab {
namespace konsol {

using dx::clampf;
using dx::kInvSr;
using dx::kSr;

// ============================================================================
// SendDelay
// ============================================================================
bool SendDelay::init(Arena& a) {
    for (auto& b : buf_) {
        b = static_cast<float*>(a.alloc(kMax * sizeof(float)));
        if (!b) return false;
    }
    clear();
    return true;
}

void SendDelay::clear() {
    for (auto& b : buf_) std::memset(b, 0, kMax * sizeof(float));
    lp_[0] = lp_[1] = hpX_[0] = hpX_[1] = hpY_[0] = hpY_[1] = 0;
    quiet_ = 1u << 20;
}

bool SendDelay::process(const float* in, float* ol, float* orr) {
    float pk = 0;
    for (uint32_t i = 0; i < kBlock; i += 2) {
        const float a = in[i] < 0 ? -in[i] : in[i];
        if (a > pk) pk = a;
    }
    if (pk > 1e-5f) quiet_ = 0;
    else quiet_ += kBlock;
    // the echo tail of the longest feedback: time x repeats to -80 dB
    const float rep = fb_ > 0.01f ? -9.2f / logf(fb_ < 0.98f ? fb_ : 0.98f) : 1.0f;
    if (float(quiet_) > cur_ * (rep + 1.0f) + 4096.0f && cur_ == target_) return false;
    const float cut = 800.0f * dx::exp2fast(tone_ * 4.6f);
    const float lpA = 1.0f - expf(-dx::kTwoPi * (type_ == 1 ? cut * 0.6f : cut) * kInvSr);
    const float hpA = expf(-dx::kTwoPi * (type_ == 2 ? 300.0f : 60.0f) * kInvSr);
    const float slew = type_ == 1 ? 0.00006f : 0.0015f;
    const float fb = fb_ > 0.97f ? 0.97f : fb_;
    // lo-fi: sample-and-hold rate and bits from the amount
    const float crushInc = (22050.0f - 16000.0f * mod_) * kInvSr;
    const float levels = dx::exp2fast(12.0f - 5.0f * mod_);
    float* L = buf_[0];
    float* R = buf_[1];
    uint32_t w = w_;
    float d = cur_;
    float modMul = 1.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if ((i & 15) == 0) {
            d += (target_ - d) * slew * 16.0f;
            if (type_ == 1) {   // wow 0.55 Hz, flutter 6.5 Hz
                wow_ += 0.55f * 16.0f * kInvSr;
                if (wow_ >= 1.0f) wow_ -= 1.0f;
                flut_ += 6.5f * 16.0f * kInvSr;
                if (flut_ >= 1.0f) flut_ -= 1.0f;
                modMul = 1.0f + mod_ * (0.0045f * dx::sin2pi(wow_) + 0.0009f * dx::sin2pi(flut_));
            }
        }
        float rp = float(w) - d * modMul;
        if (rp < 0) rp += float(kMax);
        uint32_t i0 = uint32_t(rp);
        const float f = rp - float(i0);
        if (i0 >= kMax) i0 -= kMax;
        const uint32_t i1 = i0 + 1 < kMax ? i0 + 1 : 0;
        float yl = L[i0] + (L[i1] - L[i0]) * f;
        float yr = R[i0] + (R[i1] - R[i0]) * f;
        ol[i] = yl;
        orr[i] = yr;
        // feedback path: tone, low cut, colour
        lp_[0] += lpA * (yl - lp_[0]);
        lp_[1] += lpA * (yr - lp_[1]);
        float fl = lp_[0], fr = lp_[1];
        const float hl = hpA * (hpY_[0] + fl - hpX_[0]);
        hpX_[0] = fl;
        hpY_[0] = hl;
        const float hr = hpA * (hpY_[1] + fr - hpX_[1]);
        hpX_[1] = fr;
        hpY_[1] = hr;
        fl = hl;
        fr = hr;
        if (type_ == 1) {
            fl = dx::soft(fl * 1.4f) * 0.75f;
            fr = dx::soft(fr * 1.4f) * 0.75f;
        } else if (type_ == 2) {
            crush_ += crushInc;
            if (crush_ >= 1.0f) {
                crush_ -= 1.0f;
                hold_[0] = dx::ffloor(fl * levels + 0.5f) / levels;
                hold_[1] = dx::ffloor(fr * levels + 0.5f) / levels;
            }
            fl = hold_[0];
            fr = hold_[1];
        }
        const float x = in[i];
        if (pp_) {
            L[w] = x + fb * fr;
            R[w] = fb * fl;
        } else {
            L[w] = x + fb * fl;
            R[w] = x + fb * fr;
        }
        if (++w >= kMax) w = 0;
    }
    w_ = w;
    cur_ = d;
    if (cur_ != target_ && (cur_ - target_ < 0.01f && target_ - cur_ < 0.01f)) cur_ = target_;
    return true;
}

// ============================================================================
// FdnVerb
// ============================================================================
bool FdnVerb::init(Arena& a) {
    for (auto& l : line_) {
        l = static_cast<float*>(a.alloc(kLineMax * sizeof(float)));
        if (!l) return false;
    }
    for (auto& p : ap_) {
        p = static_cast<float*>(a.alloc(kApMax * sizeof(float)));
        if (!p) return false;
    }
    pre_buf_ = static_cast<float*>(a.alloc(kPreMax * sizeof(float)));
    sh_ = static_cast<float*>(a.alloc(kShift * sizeof(float)));
    if (!pre_buf_ || !sh_) return false;
    configure();
    return true;
}

void FdnVerb::clear() {
    for (auto& l : line_) std::memset(l, 0, kLineMax * sizeof(float));
    for (auto& p : ap_) std::memset(p, 0, kApMax * sizeof(float));
    std::memset(pre_buf_, 0, kPreMax * sizeof(float));
    std::memset(sh_, 0, kShift * sizeof(float));
    for (float& x : lp_) x = 0;
    quiet_ = 1u << 20;
}

void FdnVerb::configure() {
    static const uint32_t kLen[kLines] = {1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116};
    static const uint32_t kAp[4] = {142, 107, 379, 277};
    const float sc = type_ == 3 ? 0.35f + 0.4f * size_ : 0.45f + 1.0f * size_;
    for (int i = 0; i < kLines; ++i) {
        uint32_t n = uint32_t(float(kLen[i]) * sc);
        if (n >= kLineMax) n = kLineMax - 1;
        if (n < 64) n = 64;
        len_[i] = n;
        if (idx_[i] >= n) idx_[i] = 0;
    }
    for (int i = 0; i < 4; ++i) {
        uint32_t n = uint32_t(float(kAp[i]) * (0.6f + 0.6f * size_));
        apLen_[i] = n < kApMax ? n : kApMax - 1;
        if (apPos_[i] >= apLen_[i]) apPos_[i] = 0;
    }
    float rt;   // seconds to -60 dB
    switch (type_) {
        case 2: rt = 2.5f + 9.0f * size_; break;         // shimmer
        case 3: rt = 1.2f + 1.5f * size_; break;         // gated: dense, the gate cuts it
        default: rt = 0.7f + 7.5f * size_ * size_; break;   // hall
    }
    for (int i = 0; i < kLines; ++i) g_[i] = dx::exp2fast(-9.966f * float(len_[i]) / (rt * kSr));
    dampK_ = 0.08f + 0.85f * (1.0f - damp_) * (1.0f - damp_);
    preLen_ = uint32_t(pre_ * 0.12f * kSr);
    if (preLen_ >= kPreMax) preLen_ = kPreMax - 1;
    gateHoldLen_ = uint32_t((0.09f + 0.4f * size_) * kSr);
    dirty_ = false;
}

bool FdnVerb::process(const float* in, float* ol, float* orr) {
    if (dirty_) configure();
    float pk = 0;
    for (uint32_t i = 0; i < kBlock; i += 2) {
        const float a = in[i] < 0 ? -in[i] : in[i];
        if (a > pk) pk = a;
    }
    if (pk > 1e-5f) quiet_ = 0;
    else quiet_ += kBlock;
    const float tail = type_ == 2 ? 12.0f : (type_ == 3 ? 1.5f : 8.5f);
    if (float(quiet_) > tail * kSr) return false;
    uint32_t* idx = idx_;
    const float c = dampK_;
    const bool shimmer = type_ == 2, gated = type_ == 3;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = in[i];
        // pre-delay
        if (preLen_) {
            pre_buf_[preW_] = x;
            uint32_t r = preW_ + kPreMax - preLen_;
            if (r >= kPreMax) r -= kPreMax;
            x = pre_buf_[r];
            if (++preW_ >= kPreMax) preW_ = 0;
        }
        if (shimmer) x += shOut_ * 0.42f;
        // input diffusion: 4 series all-passes (g = 0.7)
        for (int k = 0; k < 4; ++k) {
            float* b = ap_[k];
            const uint32_t p = apPos_[k];
            const float d = b[p];
            const float v = x + 0.7f * d;
            b[p] = v;
            x = d - 0.7f * v;
            apPos_[k] = p + 1 < apLen_[k] ? p + 1 : 0;
        }
        // the network: read, damp, decay
        float o[kLines];
        for (int k = 0; k < kLines; ++k) {
            const float y = line_[k][idx[k]];
            lp_[k] += c * (y - lp_[k]);
            o[k] = lp_[k] * g_[k];
        }
        // 8-point Walsh-Hadamard (orthogonal with the 1/sqrt(8))
        float h[kLines];
        {
            const float a0 = o[0] + o[1], a1 = o[0] - o[1], a2 = o[2] + o[3], a3 = o[2] - o[3];
            const float a4 = o[4] + o[5], a5 = o[4] - o[5], a6 = o[6] + o[7], a7 = o[6] - o[7];
            const float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
            const float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
            const float s = 0.35355339f;
            h[0] = (b0 + b4) * s;
            h[1] = (b1 + b5) * s;
            h[2] = (b2 + b6) * s;
            h[3] = (b3 + b7) * s;
            h[4] = (b0 - b4) * s;
            h[5] = (b1 - b5) * s;
            h[6] = (b2 - b6) * s;
            h[7] = (b3 - b7) * s;
        }
        const float inj = x * 0.5f;
        for (int k = 0; k < kLines; ++k) {
            line_[k][idx[k]] = h[k] + ((k & 1) ? inj : -inj);
            if (++idx[k] >= len_[k]) idx[k] = 0;
        }
        float yl = o[0] - o[2] + o[4] - o[6];
        float yr = o[1] - o[3] + o[5] - o[7];
        if (shimmer) {
            // octave up: two read heads moving at twice the write speed, sin^2 windows
            const float m = 0.5f * (yl + yr);
            sh_[shW_] = m;
            shR_ += 1.0f / 2048.0f;
            if (shR_ >= 1.0f) shR_ -= 1.0f;
            float s = 0;
            for (int hd = 0; hd < 2; ++hd) {
                float ph = shR_ + 0.5f * float(hd);
                if (ph >= 1.0f) ph -= 1.0f;
                const float dly = (1.0f - ph) * 2048.0f + 2.0f;
                float rp = float(shW_) - dly;
                if (rp < 0) rp += float(kShift);
                uint32_t r0 = uint32_t(rp);
                const float f = rp - float(r0);
                if (r0 >= kShift) r0 -= kShift;
                const uint32_t r1 = r0 + 1 < kShift ? r0 + 1 : 0;
                const float w = dx::sin2pi(ph * 0.5f);
                s += (sh_[r0] + (sh_[r1] - sh_[r0]) * f) * w * w;
            }
            shOut_ = s;
            if (++shW_ >= kShift) shW_ = 0;
        }
        if (gated) {
            const float a = in[i] < 0 ? -in[i] : in[i];
            gateEnv_ = a > gateEnv_ ? a : gateEnv_ * 0.9995f;
            if (gateEnv_ > 0.015f) gateHold_ = gateHoldLen_;
            else if (gateHold_) --gateHold_;
            const float tgt = gateHold_ ? 1.0f : 0.0f;
            gate_ += (tgt - gate_) * (tgt > gate_ ? 0.02f : 0.004f);
            yl *= gate_ * 1.4f;
            yr *= gate_ * 1.4f;
        }
        ol[i] = yl * 0.6f;
        orr[i] = yr * 0.6f;
    }
    return true;
}

}  // namespace konsol
}  // namespace lab
