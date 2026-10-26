// KONSOL DSP helpers (dspx.h): the FFT.
#include "ws/dspx.h"

#include "sdram_bss.h"

namespace lab {
namespace konsol {
namespace dx {

namespace {
constexpr int kMaxN = 4096;
OT_SDRAM_BSS float gCos[kMaxN / 2];   // main loop only: SDRAM, not SRAM
OT_SDRAM_BSS float gSin[kMaxN / 2];
bool gReady = false;
void tables() {
    for (int i = 0; i < kMaxN / 2; ++i) {
        const float a = kTwoPi * float(i) / float(kMaxN);
        gCos[i] = cosf(a);
        gSin[i] = sinf(a);
    }
    gReady = true;
}
}  // namespace

void fft(float* re, float* im, int n, bool inverse) {
    if (!gReady) tables();
    if (n < 2 || n > kMaxN) return;
    // bit reversal
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = kMaxN / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = gCos[k * step];
                const float wi = inverse ? gSin[k * step] : -gSin[k * step];
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

}  // namespace dx
}  // namespace konsol
}  // namespace lab
