// BULUT spectral freeze: analyse a moment (4096-point FFT magnitudes,
// averaged over overlapping frames) and resynthesise it as a seamless
// 65,536-sample loop (one inverse FFT with random phases: the loop is exactly
// periodic, so it never clicks, and its spectrum never moves). Main loop only.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/bulut.h"

namespace bulut {

class Spectral {
public:
    static constexpr int kAna = 4096;          // analysis frame
    static constexpr int kAnaFrames = 4;       // averaged frames (hop kAna / 2)
    static constexpr int kAnaSpan = kAna + (kAnaFrames - 1) * (kAna / 2);   // 10,240 samples
    bool init(lab::Arena& a);
    // Analyse kAnaSpan samples read through `get(i)` into a library slot.
    // Returns false (slot untouched) when the moment is silence (peak < -72 dBFS).
    template <class Fn> bool analyse(Fn get, FreezeSlot& out) {
        float pk = 0.0f;
        for (int i = 0; i < kAnaSpan; ++i) {
            const float v = get(i);
            span_[i] = v;
            pk = v > pk ? v : (-v > pk ? -v : pk);
        }
        if (pk < 2.5e-4f) return false;
        analyseSpan(out);
        return true;
    }
    // Resynthesise a slot into `loop` (kFrzLen int16). `seed` picks the phases.
    void synth(const FreezeSlot& in, int16_t* loop, uint32_t seed);
    // In-place radix-2 complex FFT, n a power of two, inverse when `inv`.
    static void fft(float* re, float* im, int n, bool inv);

private:
    void analyseSpan(FreezeSlot& out);
    float* span_ = nullptr;
    float* re_ = nullptr;      // kFrzLen
    float* im_ = nullptr;      // kFrzLen
};

}  // namespace bulut
