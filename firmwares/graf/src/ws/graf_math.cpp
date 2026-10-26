// GRAF -- math tables and scales (graf_math.h).
#include "ws/graf_math.h"

namespace graf {

float gSin[kSinN + 1];

void initMath() {
    // One quarter by sinf, the rest by symmetry (exact zeros / ones).
    for (int i = 0; i <= kSinN / 4; ++i) {
        const float s = sinf(kTwoPi * float(i) / float(kSinN));
        gSin[i] = s;
        gSin[kSinN / 2 - i] = s;
        gSin[kSinN / 2 + i] = -s;
        gSin[kSinN - i] = -s;
    }
    gSin[0] = 0.0f;
    gSin[kSinN / 2] = 0.0f;
    gSin[kSinN] = 0.0f;
}

// bit n = semitone n above the root
const uint16_t kScaleMask[kScales] = {
    0xFFF,   // chromatic
    0xAB5,   // major            0 2 4 5 7 9 11
    0x5AD,   // minor (natural)  0 2 3 5 7 8 10
    0x6AD,   // dorian           0 2 3 5 7 9 10
    0x5AB,   // phrygian         0 1 3 5 7 8 10
    0x295,   // major pentatonic 0 2 4 7 9
    0x4A9,   // minor pentatonic 0 3 5 7 10
    0x4E9,   // blues            0 3 5 6 7 10
    0x9AD,   // harmonic minor   0 2 3 5 7 8 11
    0x555,   // whole tone       0 2 4 6 8 10
    0x18D,   // hirajoshi-ish    0 2 3 7 8
    0x891,   // fifths+          0 4 7 11
};
const char* const kScaleNames[kScales] = {"Chrom", "Major", "Minor", "Dorian", "Phryg", "Pent+",
                                          "Pent-", "Blues", "HarmM", "Whole", "Hira", "Maj7"};

float quantizeSemi(float semi, int sc) {
    if (sc <= 0 || sc >= kScales) return floorf(semi + 0.5f);
    const uint16_t mask = kScaleMask[sc];
    const float fl = floorf(semi / 12.0f);
    const float oct = fl * 12.0f;
    const float r = semi - oct;   // 0..12
    float best = 0.0f, bestD = 100.0f;
    for (int n = 0; n <= 12; ++n) {
        if (!(mask & (1u << (n % 12)))) continue;
        const float d = fabsf(r - float(n));
        if (d < bestD) {
            bestD = d;
            best = float(n);
        }
    }
    return oct + best;
}

int scaleDegree(int sc, int degree) {
    if (sc < 0 || sc >= kScales) sc = 0;
    const uint16_t mask = kScaleMask[sc];
    int notes[12], n = 0;
    for (int i = 0; i < 12; ++i)
        if (mask & (1u << i)) notes[n++] = i;
    if (!n) return degree;
    int oct = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
    int idx = degree - oct * n;
    return oct * 12 + notes[idx];
}

}  // namespace graf
