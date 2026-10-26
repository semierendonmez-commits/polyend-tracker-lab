// polyend-tracker-lab -- small DSP building blocks shared by the workstations.
// Header-only, float only (the Cortex-M4F has a single-precision FPU: never use
// double in per-sample code, and call the f-suffixed libm functions: sinf, expf...).
#pragma once

#include <cmath>
#include <cstdint>

namespace lab {
namespace dsp {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;
constexpr float kSr = 44100.0f;
constexpr float kInvSr = 1.0f / 44100.0f;

inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float mtof(float note) { return 440.0f * exp2f((note - 69.0f) * (1.0f / 12.0f)); }
inline float dbToGain(float db) { return powf(10.0f, db * 0.05f); }
// Soft clip, |x| < ~3 smooth, odd harmonics.
inline float softclip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
// Wavefolder (sine fold).
inline float fold(float x) { return sinf(x * 1.5707963f); }

// xorshift32 noise / random.
struct Rng {
    uint32_t s = 0x12345678u;
    explicit Rng(uint32_t seed = 0x12345678u) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }       // 0..1
    float bi() { return uni() * 2.0f - 1.0f; }                                // -1..1
    int range(int n) { return n > 0 ? int(next() % uint32_t(n)) : 0; }       // 0..n-1
    bool chance(float p) { return uni() < p; }
};

// One-pole low-pass / smoother.
struct OnePole {
    float z = 0.0f, a = 0.1f;
    void setHz(float hz) { a = 1.0f - expf(-kTwoPi * hz * kInvSr); }
    float process(float x) { return z += a * (x - z); }
};

// DC blocker.
struct DcBlock {
    float x1 = 0.0f, y1 = 0.0f;
    float process(float x) {
        const float y = x - x1 + 0.995f * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// TPT state-variable filter (Zavalishin): lp / bp / hp in one go.
struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f, g = 0.1f, k = 1.4f;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;
    void set(float hz, float q) {
        hz = clamp(hz, 10.0f, 18000.0f);
        g = tanf(kPi * hz * kInvSr);
        k = 1.0f / clamp(q, 0.5f, 40.0f);
    }
    void process(float x) {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + g * a1 * v3;
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
};

// Phase accumulator oscillator with polyBLEP saw / square, sine, triangle.
struct Osc {
    float phase = 0.0f, inc = 0.01f;
    void setHz(float hz) { inc = hz * kInvSr; }
    static float blep(float t, float dt) {
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
    float step() {
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        return phase;
    }
    float saw() {
        const float p = step();
        return 2.0f * p - 1.0f - blep(p, inc);
    }
    float square(float pw = 0.5f) {
        const float p = step();
        float v = p < pw ? 1.0f : -1.0f;
        v += blep(p, inc);
        float q = p - pw;
        if (q < 0.0f) q += 1.0f;
        v -= blep(q, inc);
        return v;
    }
    float sine() { return sinf(kTwoPi * step()); }
    float tri() {
        const float p = step();
        return 4.0f * (p < 0.5f ? p : 1.0f - p) - 1.0f;
    }
};

// Linear ADSR, times in seconds.
struct Adsr {
    enum Stage : uint8_t { kIdle, kAttack, kDecay, kSustain, kRelease };
    Stage st = kIdle;
    float v = 0.0f, a = 0.005f, d = 0.2f, s = 0.7f, r = 0.3f;
    void gate(bool on) { st = on ? kAttack : (st == kIdle ? kIdle : kRelease); }
    bool active() const { return st != kIdle; }
    float process() {
        switch (st) {
            case kAttack:
                v += kInvSr / (a > 0.0005f ? a : 0.0005f);
                if (v >= 1.0f) { v = 1.0f; st = kDecay; }
                break;
            case kDecay:
                v -= (1.0f - s) * kInvSr / (d > 0.0005f ? d : 0.0005f);
                if (v <= s) { v = s; st = kSustain; }
                break;
            case kSustain: break;
            case kRelease:
                v -= kInvSr / (r > 0.0005f ? r : 0.0005f);
                if (v <= 0.0f) { v = 0.0f; st = kIdle; }
                break;
            default: v = 0.0f; break;
        }
        return v;
    }
};

// Exponential decay envelope (percussion), time = -60 dB time in seconds.
struct Decay {
    float v = 0.0f, k = 0.999f;
    void set(float seconds) { k = expf(-6.9f / (clamp(seconds, 0.001f, 30.0f) * kSr)); }
    void trigger(float level = 1.0f) { v = level; }
    float process() { return v *= k; }
};

// Delay line over caller-provided memory (allocate from the Arena).
struct DelayLine {
    float* buf = nullptr;
    uint32_t size = 0, w = 0;
    void attach(float* mem, uint32_t n) { buf = mem; size = n; w = 0; }
    void write(float x) {
        buf[w] = x;
        if (++w >= size) w = 0;
    }
    // Fractional read, `samples` behind the last write.
    float read(float samples) const {
        if (!size) return 0.0f;
        float rp = float(w) - 1.0f - samples;
        while (rp < 0.0f) rp += float(size);
        const uint32_t i0 = uint32_t(rp) % size;
        const uint32_t i1 = (i0 + 1) % size;
        const float f = rp - floorf(rp);
        return buf[i0] + (buf[i1] - buf[i0]) * f;
    }
};

// Euclidean rhythm: is step `i` of `steps` a hit for `hits` pulses, rotated.
inline bool euclid(int i, int steps, int hits, int rotate = 0) {
    if (steps <= 0 || hits <= 0) return false;
    if (hits >= steps) return true;
    const int k = ((i + rotate) % steps + steps) % steps;
    return ((k * hits) % steps) < hits;
}

}  // namespace dsp
}  // namespace lab
