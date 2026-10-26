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

bool loadWavKit(Kit& kit, ot::IStorage& fs, const char* path) {
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
    const char* slash = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') slash = p + 1;
    lab::copyStr(kit.name, sizeof kit.name, slash);
    classifySlices(kit);
    return true;
}

}  // namespace makam
