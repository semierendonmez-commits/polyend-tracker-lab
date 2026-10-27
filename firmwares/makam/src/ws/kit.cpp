// MAKAM -- break kits (kit.h). Synthesis runs once at init (main loop).
#include "ws/kit.h"

#include <cmath>
#include <cstring>

#include "formats/wav.h"
#include "lab/dsp.h"

namespace makam {

const char* const kKitNames[kKits + 1] = {"Kirik Break", "Sert Funk", "Darbuka", "Card WAV"};

namespace {
using lab::dsp::kTwoPi;
using lab::dsp::kSr;

// Two-bar patterns, one char per 16th note (32). Separate lanes so hats
// ride over the drums.
//   K kick  S snare  g ghost snare  D doum  T tek  k ka  . nothing
//   h closed hat  o open hat  r ride  c crash
struct Pattern {
    const char* drums;
    const char* tops;
};
const Pattern kPatterns[kKits] = {
    // Kirik: the classic broken break -- kick, kick, snare, the late snare.
    {"K.K.S..S.SKKS..SK.K.S..S.SK..S.g", "r.r.r.r.r.r.r.r.r.r.r.r.r.r.r.c."},
    // Sert: tight funk, 16th hats, ghosts between.
    {"K..KS.gK.gKKS.g.K..KS.gK..K.SgS.", "hhhhhhhohhhhhhhhhhhhhhhohhhhhhoh"},
    // Darbuka: doum / tek / ka laid out like a break.
    {"D.k.T.kkD.k.TkT.D.kDT.kkD.kkTkTk", "................................"},
};

float rnd(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// One-pole high-pass / low-pass helpers for the synthesis.
struct Hp {
    float a, x1 = 0, y1 = 0;
    explicit Hp(float hz) : a(1.0f / (1.0f + kTwoPi * hz / kSr)) {}
    float p(float x) {
        const float y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }
};
struct Lp {
    float a, z = 0;
    explicit Lp(float hz) : a(1.0f - expf(-kTwoPi * hz / kSr)) {}
    float p(float x) { return z += a * (x - z); }
};

// sin(2 pi p), p in [0, 1): cubic shaped triangle (drum synthesis only).
inline float qsin(float p) {
    const float x = p < 0.5f ? p * 4.0f - 1.0f : 3.0f - p * 4.0f;
    return -(x * (1.5f - 0.5f * x * x));
}
// exp(-k / sr) per sample.
inline float dk(float k) { return expf(-k / kSr); }

void hit(float* acc, uint32_t frames, uint32_t at, char c, uint32_t& seed, float vel) {
    const uint32_t maxLen = uint32_t(0.6f * kSr);
    float ph = 0.0f, ph2 = 0.0f;
    Hp hp7(7000.0f), hp2(1500.0f), hp5(5000.0f);
    lab::dsp::Svf bp;
    float metal[6] = {};
    const float mf[6] = {3200.0f, 4410.0f, 5130.0f, 6250.0f, 7870.0f, 8520.0f};
    // e1 main envelope, e2 pitch / tone envelope, both recursive.
    float e1 = 1.0f, e2 = 1.0f, k1 = 1.0f, k2 = 1.0f, g = 1.0f;
    switch (c) {
        case 'K': k1 = dk(7.5f); k2 = dk(38.0f); break;
        case 'S': bp.set(1900.0f, 0.9f); k1 = dk(15.0f); k2 = dk(28.0f); break;
        case 'g': bp.set(1900.0f, 0.9f); k1 = dk(27.0f); k2 = dk(50.0f); g = 0.32f; break;
        case 'D': k1 = dk(5.0f); k2 = dk(25.0f); break;
        case 'T': bp.set(3100.0f, 2.5f); k1 = dk(55.0f); k2 = dk(70.0f); g = 0.95f; break;
        case 'k': bp.set(2500.0f, 2.0f); k1 = dk(55.0f); k2 = dk(70.0f); g = 0.45f; break;
        case 'h': k1 = dk(70.0f); g = 0.32f; break;
        case 'o': k1 = dk(9.0f); g = 0.26f; break;
        case 'r': k1 = dk(7.0f); g = 0.35f; break;
        case 'c': k1 = dk(2.2f); g = 0.5f; break;
        default: return;
    }
    for (uint32_t i = 0; i < maxLen && at + i < frames; ++i) {
        float v = 0.0f;
        switch (c) {
            case 'K': {
                ph += (46.0f + 120.0f * e2) / kSr;
                if (ph >= 1.0f) ph -= 1.0f;
                v = qsin(ph) * e1 * 1.1f;
                if (i < 160) v += rnd(seed) * 0.35f * (1.0f - float(i) / 160.0f);
                break;
            }
            case 'S':
            case 'g': {
                ph += 182.0f / kSr;
                if (ph >= 1.0f) ph -= 1.0f;
                ph2 += 331.0f / kSr;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                const float tone = (qsin(ph) * 0.55f + qsin(ph2) * 0.3f) * e2;
                bp.process(rnd(seed));
                v = tone + (bp.bp * 1.6f + hp2.p(rnd(seed)) * 0.5f) * e1;
                break;
            }
            case 'D': {   // doum: deep membrane with a pitch drop
                const float f = 82.0f + 30.0f * e2;
                ph += f / kSr;
                if (ph >= 1.0f) ph -= 1.0f;
                ph2 += f * 1.59f / kSr;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                v = (qsin(ph) + 0.25f * qsin(ph2) * e2) * e1 * 1.05f;
                if (i < 90) v += rnd(seed) * 0.2f;
                break;
            }
            case 'T':
            case 'k': {   // tek / ka: rim crack
                ph += (c == 'T' ? 910.0f : 760.0f) / kSr;
                if (ph >= 1.0f) ph -= 1.0f;
                bp.process(rnd(seed));
                v = bp.bp * 2.2f * e1 + qsin(ph) * 0.5f * e2;
                break;
            }
            case 'h':
            case 'o': v = hp7.p(rnd(seed)) * e1; break;
            default: {   // r / c
                float m = 0.0f;
                for (int k = 0; k < 6; ++k) {
                    metal[k] += mf[k] / kSr;
                    if (metal[k] >= 1.0f) metal[k] -= 1.0f;
                    m += metal[k] < 0.5f ? 1.0f : -1.0f;
                }
                v = (hp5.p(m * 0.12f) + hp7.p(rnd(seed)) * 0.4f) * e1;
                break;
            }
        }
        e1 *= k1;
        e2 *= k2;
        acc[at + i] += v * vel * g;
        if (e1 < 0.0003f && e2 < 0.0003f) break;
    }
}
}  // namespace

void Kit::index() {
    std::memset(count, 0, sizeof count);
    for (int i = 0; i < kSlices; ++i) {
        const int c = (cls[i] >= 1 && cls[i] <= 3) ? int(cls[i]) : int(kClsGhost);
        list[c][count[c]++] = uint8_t(i);
    }
    // Every class needs one slice: borrow from the others.
    if (!count[kClsKick]) list[kClsKick][count[kClsKick]++] = 0;
    if (!count[kClsSnare]) list[kClsSnare][count[kClsSnare]++] = uint8_t(kSlices / 4);
    if (!count[kClsGhost]) list[kClsGhost][count[kClsGhost]++] = uint8_t(kSlices / 2 + 1);
    for (int c = 0; c < 200; ++c) {
        const uint32_t a = uint32_t(uint64_t(frames) * uint32_t(c) / 200u);
        const uint32_t b = uint32_t(uint64_t(frames) * uint32_t(c + 1) / 200u);
        int pk = 0;
        for (uint32_t i = a; i < b && data; i += 4) {
            const int v = data[i] < 0 ? -data[i] : data[i];
            if (v > pk) pk = v;
        }
        peak[c] = int8_t(pk >> 8);
    }
}

void synthKit(Kit& kit, int k, float* scratch) {
    if (!kit.data || !scratch || k < 0 || k >= kKits) return;
    kit.bpm = 170.0f;
    const float sixteenth = 60.0f / kit.bpm / 4.0f * kSr;
    kit.frames = uint32_t(sixteenth * 32.0f + 0.5f);
    if (kit.frames > kit.cap) kit.frames = kit.cap;
    float* a = scratch;   // kKitFrames floats from the arena (SDRAM)
    std::memset(a, 0, sizeof(float) * kit.frames);
    uint32_t seed = 0xC0FFEEu + uint32_t(k) * 7919u;
    const Pattern& p = kPatterns[k];
    for (int s = 0; s < 32; ++s) {
        const uint32_t at = uint32_t(float(s) * sixteenth);
        const float accent = (s % 4 == 0) ? 1.0f : 0.85f;
        hit(a, kit.frames, at, p.drums[s], seed, accent);
        hit(a, kit.frames, at, p.tops[s], seed, (s % 2 == 0) ? 1.0f : 0.7f);
    }
    // Tape-ish glue: soft saturation and a gentle top roll-off.
    Lp lp(11000.0f);
    for (uint32_t i = 0; i < kit.frames; ++i) {
        const float v = lab::dsp::softclip(lp.p(a[i]) * 1.4f) * 0.9f;
        kit.data[i] = int16_t(v * 32000.0f);
    }
    for (int i = 0; i <= kSlices; ++i) kit.start[i] = uint32_t(uint64_t(kit.frames) * uint32_t(i) / kSlices);
    for (int i = 0; i < kSlices; ++i) {
        const char c = p.drums[2 * i];
        kit.cls[i] = (c == 'K' || c == 'D') ? kClsKick : (c == 'S' || c == 'T') ? kClsSnare : kClsGhost;
    }
    lab::copyStr(kit.name, sizeof kit.name, kKitNames[k]);
    kit.index();
}

void classifySlices(Kit& kit) {
    // Band energies over the first 25 ms of each slice: low (< 150 Hz) vs high (> 1.5 kHz).
    float lowE[kSlices], highE[kSlices], allE[kSlices];
    float maxAll = 1e-9f;
    for (int s = 0; s < kSlices; ++s) {
        Lp lp(150.0f);
        Hp hp(1500.0f);
        float l = 0, h = 0, t = 0;
        const uint32_t a = kit.start[s];
        const uint32_t n = uint32_t(0.025f * kSr);
        for (uint32_t i = 0; i < n && a + i < kit.frames; ++i) {
            const float x = kit.data[a + i] * (1.0f / 32768.0f);
            const float lo = lp.p(x), hi = hp.p(x);
            l += lo * lo;
            h += hi * hi;
            t += x * x;
        }
        lowE[s] = l;
        highE[s] = h;
        allE[s] = t;
        if (t > maxAll) maxAll = t;
    }
    for (int s = 0; s < kSlices; ++s) {
        const float loud = allE[s] / maxAll;
        if (lowE[s] > 0.35f * allE[s] && loud > 0.2f) kit.cls[s] = kClsKick;
        else if (highE[s] > 0.2f * allE[s] && loud > 0.25f) kit.cls[s] = kClsSnare;
        else kit.cls[s] = kClsGhost;
    }
    kit.cls[0] = kit.cls[0] == kClsSnare ? kClsSnare : kClsKick;   // breaks start on the one
    kit.index();
}

// ---- onset slicer -------------------------------------------------------------
// Spectral-flux onsets after the semi fork's "Breaks" auto-slice detector
// (OpenTracker semi fork, src/core/app/auto_slice.cpp, GPL-3.0 per the lab):
// 256-point Hann frames every 128 samples (2.9 ms), the half-wave rectified
// rise of the square-rooted magnitudes, high bins weighted more, a peak over
// its two neighbours each side and over 1.5 x the local mean, 46 ms spacing
// (the stronger of two close onsets), the onset refined to the 32-sample
// block where the energy doubles. Written again here for MAKAM's kit.
namespace {
constexpr int kFftN = 256, kHop = 128, kBins = kFftN / 2 + 1;
void fft256(float* re, float* im, const float* cosT, const float* sinT) {
    for (int i = 1, j = 0; i < kFftN; ++i) {   // bit reversal
        int bit = kFftN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= kFftN; len <<= 1) {
        const int step = kFftN / len;
        for (int i = 0; i < kFftN; i += len)
            for (int k = 0; k < len / 2; ++k) {
                const float wr = cosT[k * step], wi = -sinT[k * step];
                const int a = i + k, b = a + len / 2;
                const float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}
}  // namespace

int onsetSlices(Kit& kit, float* scratch) {
    for (int i = 0; i <= kSlices; ++i) kit.start[i] = uint32_t(uint64_t(kit.frames) * uint32_t(i) / kSlices);
    if (!scratch || !kit.data || kit.frames < 8192) return 0;
    // scratch layout: win, cos, sin, re, im, prev, candidates
    float* win = scratch;
    float* cosT = win + kFftN;
    float* sinT = cosT + kFftN;
    float* re = sinT + kFftN;
    float* im = re + kFftN;
    float* prev = im + kFftN;
    constexpr int kMaxCand = 96;
    float* candVal = prev + kBins + 3;
    uint32_t* candPos = reinterpret_cast<uint32_t*>(candVal + kMaxCand);
    for (int i = 0; i < kFftN; ++i) {
        const float a = 6.2831853f * float(i) / float(kFftN);
        cosT[i] = cosf(a);
        sinT[i] = sinf(a);
        win[i] = 0.5f - 0.5f * cosf(6.2831853f * (float(i) + 0.5f) / float(kFftN));
    }
    for (int k = 0; k < kBins; ++k) prev[k] = 0.0f;
    float hist[16] = {};
    float peakMax = 0.0f;
    int nCand = 0;
    uint32_t lastCand = 0;
    const uint32_t spacing = 2048;
    uint32_t frame = 0;
    for (uint32_t f0 = 0; f0 + kFftN <= kit.frames; f0 += kHop, ++frame) {
        for (int i = 0; i < kFftN; ++i) {
            re[i] = float(kit.data[f0 + uint32_t(i)]) * win[i];
            im[i] = 0.0f;
        }
        fft256(re, im, cosT, sinT);
        float flux = 0.0f;
        for (int k = 0; k < kBins; ++k) {
            const float m = sqrtf(sqrtf(re[k] * re[k] + im[k] * im[k]));
            const float d = m - prev[k];
            prev[k] = m;
            if (k >= 1 && d > 0.0f) flux += d * (1.0f + float(k) / float(kBins));
        }
        for (int i = 0; i < 15; ++i) hist[i] = hist[i + 1];
        hist[15] = flux;
        if (flux > peakMax) peakMax = flux;
        if (frame < 4) continue;
        const float c = hist[13];
        bool peak = c > 0.0f;
        for (int i = 11; i <= 15; ++i)
            if (i != 13 && hist[i] >= c) peak = false;
        float mean = 0.0f;
        for (int i = 0; i < 16; ++i) mean += hist[i];
        mean *= 1.0f / 16.0f;
        if (!peak || c <= 1.5f * mean + 0.04f * peakMax) continue;
        // refine: the first 32-sample block whose energy doubles, one block early
        uint32_t pos = (frame - 2) * kHop + kFftN / 2;
        {
            const uint32_t fr = (frame - 2) * kHop;
            float prevE = 0.0f;
            for (uint32_t b = 0; b * 32 < kFftN && fr + (b + 1) * 32 <= kit.frames; ++b) {
                float e = 1.0f;
                for (uint32_t i = 0; i < 32; ++i) {
                    const float x = float(kit.data[fr + b * 32 + i]);
                    e += x * x;
                }
                if (b > 0 && e > 2.0f * prevE && e > 2000.0f * 32.0f) {
                    pos = fr + (b > 1 ? b - 1 : 0) * 32;
                    break;
                }
                prevE = e;
            }
        }
        if (nCand > 0 && pos < lastCand + spacing) {
            if (c > candVal[nCand - 1]) {
                candVal[nCand - 1] = c;
                candPos[nCand - 1] = pos;
                lastCand = pos;
            }
        } else if (nCand < kMaxCand) {
            candVal[nCand] = c;
            candPos[nCand] = pos;
            ++nCand;
            lastCand = pos;
        }
    }
    // the strongest 15 after the start, back in time order
    int idx[kMaxCand];
    int n = 0;
    for (int i = 0; i < nCand; ++i)
        if (candPos[i] >= spacing && candPos[i] + spacing < kit.frames) idx[n++] = i;
    const int found = n;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && candVal[idx[j]] > candVal[idx[j - 1]]; --j) {
            const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    if (n > kSlices - 1) n = kSlices - 1;
    uint32_t pts[kSlices + 1];
    int np = 0;
    pts[np++] = 0;
    for (int i = 0; i < n; ++i) pts[np++] = candPos[idx[i]];
    for (int i = 1; i < np; ++i)
        for (int j = i; j > 1 && pts[j] < pts[j - 1]; --j) {
            const uint32_t t = pts[j]; pts[j] = pts[j - 1]; pts[j - 1] = t;
        }
    // fewer than 16: split the longest gap until there are 16 slices
    while (np < kSlices) {
        int best = 0;
        uint32_t gap = 0;
        for (int i = 0; i < np; ++i) {
            const uint32_t end = i + 1 < np ? pts[i + 1] : kit.frames;
            if (end - pts[i] > gap) { gap = end - pts[i]; best = i; }
        }
        for (int i = np; i > best + 1; --i) pts[i] = pts[i - 1];
        pts[best + 1] = pts[best] + gap / 2;
        ++np;
    }
    for (int i = 0; i < kSlices; ++i) kit.start[i] = pts[i];
    kit.start[kSlices] = kit.frames;
    return found;
}

bool loadWavKit(Kit& kit, ot::IStorage& fs, const char* path, bool autoSlice, float* scratch) {
    if (!kit.data) return false;
    ot::WavImportOptions opt;
    ot::WavImportResult res;
    kit.frames = 0;
    const ot::FmtStatus st = ot::wavImportFile(fs, path, opt, kit.data, kit.cap, res);
    if (st != ot::FmtStatus::Ok || res.frames < 4410) return false;
    kit.frames = res.frames;
    // 1, 2 or 4 bars: pick the tempo nearest to 170 BPM.
    const float secs = float(kit.frames) / kSr;
    float best = 0.0f, bestD = 1e9f;
    for (int bars = 1; bars <= 8; bars *= 2) {
        const float bpm = 60.0f * 4.0f * float(bars) / secs;
        const float d = fabsf(bpm - 170.0f);
        if (d < bestD) {
            bestD = d;
            best = bpm;
        }
    }
    kit.bpm = best;
    for (int i = 0; i <= kSlices; ++i) kit.start[i] = uint32_t(uint64_t(kit.frames) * uint32_t(i) / kSlices);
    if (autoSlice && scratch) onsetSlices(kit, scratch);
    const char* slash = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') slash = p + 1;
    lab::copyStr(kit.name, sizeof kit.name, slash);
    classifySlices(kit);
    return true;
}

}  // namespace makam
