// GRAF -- fast single-precision helpers for the per-sample module code.
// Everything here is float only (Cortex-M4F) and cheap: table sine, a
// polynomial exp2, a rational tanh, Schmitt triggers and small smoothers.
#pragma once

#include <cmath>
#include <cstdint>

namespace graf {

constexpr float kSr = 44100.0f;
constexpr float kInvSr = 1.0f / 44100.0f;
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;
constexpr float kC4 = 261.625565f;   // pitch 0.0 (octaves) = C4

// Sine table: one cycle in kSinN points (+1 guard), filled by initMath().
constexpr int kSinN = 1024;
extern float gSin[kSinN + 1];
void initMath();

// sin(2 pi phase) for any phase (wrapped), linear interpolation.
inline float sin01(float phase) {
    phase -= float(int32_t(phase));
    if (phase < 0.0f) phase += 1.0f;
    const float x = phase * float(kSinN);
    const int32_t xi = int32_t(x);
    const float f = x - float(xi);
    const int32_t i = xi & (kSinN - 1);   // phase may round up to exactly 1.0
    return gSin[i] + (gSin[i + 1] - gSin[i]) * f;
}
// Same for a phase already in [0, 1).
inline float sinP(float phase) {
    const float x = phase * float(kSinN);
    const int32_t i = int32_t(x) & (kSinN - 1);
    const float f = x - float(int32_t(x));
    return gSin[i] + (gSin[i + 1] - gSin[i]) * f;
}

// 2^x, |x| < 30, ~1e-4 relative error (good for pitch: < 0.2 cent).
inline float fexp2(float x) {
    if (x < -30.0f) x = -30.0f;
    if (x > 30.0f) x = 30.0f;
    int32_t ii = int32_t(x);
    if (float(ii) > x) --ii;
    const float fl = float(ii);
    const float f = x - fl;
    // minimax-ish polynomial for 2^f on [0, 1)
    const float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    uint32_t bits;
    __builtin_memcpy(&bits, &p, 4);
    bits += uint32_t(ii) << 23;
    float r;
    __builtin_memcpy(&r, &bits, 4);
    return r;
}

// Pitch (octaves, 0 = C4) to Hz.
inline float pitchHz(float oct) { return kC4 * fexp2(oct); }

// Soft clip / tanh-like saturation, smooth up to |x| = 3.
inline float sat(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Cheap cubic soft clip (no divide): unity slope at 0, flat at +-1.5 -> +-1.
inline float soft(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - 0.148148148f * x * x * x;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// tan(pi * hz / sr) for the TPT filters, valid up to ~0.45 sr (Pade 5/4).
inline float tanPi(float hz) {
    float x = hz * (kPi * kInvSr);
    if (x > 1.45f) x = 1.45f;
    if (x < 0.0f) x = 0.0f;
    const float x2 = x * x;
    return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}

// xorshift32 random.
struct Rand {
    uint32_t s = 0x2545F491u;
    void seed(uint32_t v) { s = v ? v : 0x2545F491u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    float bi() { return uni() * 2.0f - 1.0f; }
    int range(int n) { return n > 0 ? int(next() % uint32_t(n)) : 0; }
};

// Schmitt trigger for gate / trigger inputs (high above 0.5, low below 0.25).
struct Schmitt {
    bool hi = false;
    bool rise(float x) {
        if (!hi && x > 0.5f) {
            hi = true;
            return true;
        }
        if (hi && x < 0.25f) hi = false;
        return false;
    }
};

// Pulse generator: 1.0 for `n` samples after fire().
struct Pulse {
    int32_t left = 0;
    void fire(int32_t n = 220) { left = n; }
    float step() {
        if (left > 0) {
            --left;
            return 1.0f;
        }
        return 0.0f;
    }
};

// One-pole coefficient for a time constant in seconds.
inline float coefSec(float sec) {
    if (sec < 0.00005f) return 1.0f;
    return 1.0f - expf(-1.0f / (sec * kSr));
}

// Scales: 12-bit masks of allowed semitones (bit 0 = root).
constexpr int kScales = 12;
extern const uint16_t kScaleMask[kScales];
extern const char* const kScaleNames[kScales];
// Quantize semitones (relative to root) to the nearest note of scale `sc`.
float quantizeSemi(float semi, int sc);
// n-th degree (any integer, may be negative) of scale `sc`, in semitones.
int scaleDegree(int sc, int degree);

}  // namespace graf
