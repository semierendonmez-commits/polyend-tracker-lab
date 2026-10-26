// BULUT spectral freeze (spectral.h).
#include "ws/spectral.h"

#include <cmath>
#include <cstring>

#include "lab/dsp.h"

namespace bulut {

bool Spectral::init(lab::Arena& a) {
    span_ = static_cast<float*>(a.alloc(kAnaSpan * sizeof(float)));
    re_ = static_cast<float*>(a.alloc(kFrzLen * sizeof(float)));
    im_ = static_cast<float*>(a.alloc(kFrzLen * sizeof(float)));
    return span_ && re_ && im_;
}

void Spectral::fft(float* re, float* im, int n, bool inv) {
    // bit reversal
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = (inv ? 2.0f : -2.0f) * lab::dsp::kPi / float(len);
        const float wr0 = cosf(ang), wi0 = sinf(ang);
        const int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < half; ++k) {
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float t = wr * wr0 - wi * wi0;
                wi = wr * wi0 + wi * wr0;
                wr = t;
            }
        }
    }
}

void Spectral::analyseSpan(FreezeSlot& out) {
    // average magnitude over kAnaFrames hann-windowed frames (re_/im_ as scratch)
    float* acc = re_ + kAna;   // kAna / 2 bins (re_ has kFrzLen >= 2 * kAna floats)
    for (int b = 0; b < kSpecBins; ++b) acc[b] = 0.0f;
    for (int f = 0; f < kAnaFrames; ++f) {
        const float* x = span_ + f * (kAna / 2);
        for (int i = 0; i < kAna; ++i) {
            const float w = 0.5f - 0.5f * cosf(lab::dsp::kTwoPi * float(i) / float(kAna));
            re_[i] = x[i] * w;
            im_[i] = 0.0f;
        }
        // a second scratch row for im: im_ is kFrzLen long
        fft(re_, im_, kAna, false);
        for (int b = 0; b < kSpecBins; ++b) acc[b] += sqrtf(re_[b] * re_[b] + im_[b] * im_[b]);
    }
    float mx = 1e-9f;
    for (int b = 0; b < kSpecBins; ++b) mx = acc[b] > mx ? acc[b] : mx;
    out.used = 1;
    out.scale = mx / float(kAnaFrames);
    acc[0] = 0.0f;   // no DC
    for (int b = 0; b < kSpecBins; ++b) {
        const float v = acc[b] / mx;
        out.mag[b] = uint16_t(v * 65535.0f + 0.5f);
    }
}

void Spectral::synth(const FreezeSlot& in, int16_t* loop, uint32_t seed) {
    // kFrzLen-point spectrum: bin k of the loop = frequency k * sr / kFrzLen;
    // analysis bin j = j * sr / kAna -> loop bin j * (kFrzLen / kAna).
    constexpr int kUp = int(kFrzLen) / Spectral::kAna;   // 16
    lab::dsp::Rng rng(seed ? seed : 1u);
    std::memset(re_, 0, kFrzLen * sizeof(float));
    std::memset(im_, 0, kFrzLen * sizeof(float));
    for (int k = 1; k < int(kFrzLen) / 2; ++k) {
        const float pos = float(k) / float(kUp);
        const int j = int(pos);
        if (j >= kSpecBins - 1) break;
        const float f = pos - float(j);
        const float m = (float(in.mag[j]) * (1.0f - f) + float(in.mag[j + 1]) * f) * (1.0f / 65535.0f);
        if (m <= 0.0f) continue;
        const float ph = rng.uni() * lab::dsp::kTwoPi;
        re_[k] = m * cosf(ph);
        im_[k] = m * sinf(ph);
        // Hermitian mirror for a real signal
        re_[kFrzLen - k] = re_[k];
        im_[kFrzLen - k] = -im_[k];
    }
    fft(re_, im_, int(kFrzLen), true);
    float mx = 1e-9f;
    for (uint32_t i = 0; i < kFrzLen; ++i) {
        const float a = fabsf(re_[i]);
        mx = a > mx ? a : mx;
    }
    // normalise to -3 dBFS peak: a frozen moment always plays at a usable level
    const float g = 0.7f * 32767.0f / mx;
    for (uint32_t i = 0; i < kFrzLen; ++i) loop[i] = int16_t(re_[i] * g);
}

}  // namespace bulut
