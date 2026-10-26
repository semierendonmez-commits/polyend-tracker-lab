// KONSOL DSP helpers shared by the engines, the effects and the sample tools.
// Float only (Cortex-M4F single-precision FPU).
#pragma once

#include <cmath>
#include <cstdint>

namespace lab {
namespace konsol {
namespace dx {

constexpr float kSr = 44100.0f;
constexpr float kInvSr = 1.0f / 44100.0f;
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// sin(2 pi t) for t in [0, 1): parabola + one refinement (< 0.1 % error).
inline float sin2pi(float t) {
    const float x = t < 0.5f ? t : t - 1.0f;
    const float ax = x < 0 ? -x : x;
    const float y = 8.0f * x - 16.0f * x * ax;
    const float ay = y < 0 ? -y : y;
    return 0.225f * (y * ay - y) + y;
}
// floor without a libm call (the M4 FPU has no VRINT): |x| < 2^31
inline float ffloor(float x) {
    const float t = float(int32_t(x));
    return t > x ? t - 1.0f : t;
}
// Wraps any phase into [0, 1).
inline float wrap(float p) { return p - ffloor(p); }

// 2^x, |x| < 30, 0.01 % (exponent bits + cubic).
inline float exp2fast(float x) {
    const float fl = ffloor(x);
    const float f = x - fl;
    const float p = 1.0f + f * (0.6951786f + f * (0.2261871f + f * (0.0782354f)));
    union {
        float f;
        int32_t i;
    } u;
    u.f = p;
    u.i += int32_t(fl) << 23;
    return u.f;
}
inline float semis(float s) { return exp2fast(s * (1.0f / 12.0f)); }
inline float mtof(float n) { return 440.0f * exp2fast((n - 69.0f) * (1.0f / 12.0f)); }

// soft clip (tanh-like)
inline float soft(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// per-sample decay multiplier for a -60 dB time in ms
inline float decayK(float ms) { return expf(-6.9f / (clampf(ms, 0.5f, 60000.0f) * 0.001f * kSr)); }
// per-sample one-pole coefficient (time constant in seconds)
inline float tauK(float sec) { return sec <= 0.0f ? 0.0f : expf(-1.0f / (sec * kSr)); }

struct Noise {
    uint32_t s = 0x2545F491u;
    float bi() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(int32_t(s)) * (1.0f / 2147483648.0f);
    }
    float uni() { return bi() * 0.5f + 0.5f; }
};

// TPT state-variable filter (Zavalishin); q = 1/k.
struct Svf {
    float ic1 = 0, ic2 = 0, g = 0.1f, k = 1.4f, a1 = 0.5f, a2 = 0, a3 = 0;
    void set(float hz, float q) {
        g = tanf(kPi * clampf(hz, 10.0f, 19000.0f) * kInvSr);
        k = 1.0f / (q < 0.3f ? 0.3f : q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0; }
    inline void tick(float x, float& lp, float& bp, float& hp) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
    inline float low(float x) {
        float l, b, h;
        tick(x, l, b, h);
        return l;
    }
    inline float band(float x) {
        float l, b, h;
        tick(x, l, b, h);
        return b;
    }
    inline float high(float x) {
        float l, b, h;
        tick(x, l, b, h);
        return h;
    }
};

struct OnePole {
    float z = 0, a = 0.9f;
    void setHz(float hz) { a = expf(-kTwoPi * clampf(hz, 1.0f, 20000.0f) * kInvSr); }
    inline float lp(float x) { return z = x + a * (z - x); }
    inline float hp(float x) { return x - lp(x); }
};

// polyBLEP correction
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
inline float sawBlep(float p, float dt) { return 2.0f * p - 1.0f - blep(p, dt); }

// In-place radix-2 complex FFT (n a power of two, <= 4096). inverse: unscaled.
void fft(float* re, float* im, int n, bool inverse);

}  // namespace dx
}  // namespace konsol
}  // namespace lab
