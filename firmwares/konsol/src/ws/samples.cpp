// KONSOL sample pool, WAV loading, onsets and hit classes (samples.h).
#include "ws/samples.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "sdram_bss.h"
#include "ws/dspx.h"

namespace lab {
namespace konsol {

const char* hitClassName(int c) {
    static const char* const k[4] = {"PERC", "KICK", "SNARE", "HAT"};
    return k[c & 3];
}

bool SamplePool::init(Arena& a, uint32_t samples) {
    data_ = static_cast<int16_t*>(a.alloc(samples * sizeof(int16_t)));
    cap_ = data_ ? samples : 0;
    top_ = 0;
    for (auto& s : slot_) s = SampleSlot();
    return data_ != nullptr;
}

int SamplePool::alloc(uint32_t frames, uint8_t ch) {
    if (frames == 0) frames = 1;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    const uint32_t need = frames * ch;
    if (top_ + need > cap_) return -1;
    for (int i = 0; i < kSlots; ++i)
        if (!slot_[i].used) {
            SampleSlot& s = slot_[i];
            s = SampleSlot();
            s.used = 1;
            s.ch = ch;
            s.cap = frames;
            s.off = top_;
            s.frames = 0;
            top_ += need;
            return i;
        }
    return -1;
}

void SamplePool::release(int i) {
    if (i < 0 || i >= kSlots || !slot_[i].used) return;
    slot_[i].frames = 0;
    slot_[i].used = 0;
    // the space at the very top comes back at once; the rest on compact()
    uint32_t top = 0;
    for (const auto& s : slot_)
        if (s.used && s.off + s.cap * s.ch > top) top = s.off + s.cap * s.ch;
    top_ = top;
}

void SamplePool::compact() {
    // slots in ascending offset order, each moved down to the running top
    uint32_t top = 0;
    for (;;) {
        int next = -1;
        for (int i = 0; i < kSlots; ++i)
            if (slot_[i].used && slot_[i].off >= top && (next < 0 || slot_[i].off < slot_[next].off)) next = i;
        if (next < 0) break;
        SampleSlot& s = slot_[next];
        const uint32_t n = s.cap * s.ch;
        if (s.off != top) {
            std::memmove(data_ + top, data_ + s.off, n * sizeof(int16_t));
            s.off = top;
        }
        top += n;
    }
    top_ = top;
}

float SamplePool::peak(int s) const {
    if (!valid(s)) return 0.0f;
    const SampleSlot& sl = slot_[s];
    int32_t pk = 0;
    for (int c = 0; c < sl.ch; ++c) {
        const int16_t* d = c ? right(s) : left(s);
        for (uint32_t i = 0; i < sl.frames; ++i) {
            const int32_t v = d[i] < 0 ? -int32_t(d[i]) : d[i];
            if (v > pk) pk = v;
        }
    }
    return float(pk) / 32768.0f;
}

int SamplePool::loadWav(IStorage& fs, const char* path, uint32_t maxFrames, char* err, int errCap) {
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) {
        std::snprintf(err, size_t(errCap), "Cannot open the file");
        return -1;
    }
    ot::WavInfo info;
    ot::StorageReader rd(fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    fs.close(h);
    if (!ok) {
        std::snprintf(err, size_t(errCap), "Not a WAV the Tracker reads");
        return -1;
    }
    const uint8_t ch = info.channels >= 2 ? 2 : 1;
    ot::WavImportOptions opt;
    opt.stereo = ch == 2;
    const uint32_t full = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
    const uint32_t room = freeFrames(ch);
    const uint32_t lim = (maxFrames < room ? maxFrames : room) > 8 ? (maxFrames < room ? maxFrames : room) - 8 : 0;
    if (lim < 64) {
        std::snprintf(err, size_t(errCap), "Sample memory is full");
        return -1;
    }
    if (full > lim) opt.endMarker = uint16_t(uint64_t(65535) * lim / (uint64_t(full) + 1));   // the start that fits
    const ot::WavImportPlan plan = ot::wavPlanImport(info, opt);
    const int s = alloc(plan.outFrames + 4, ch);
    if (s < 0) {
        std::snprintf(err, size_t(errCap), "Sample memory is full");
        return -1;
    }
    ot::WavImportResult res;
    const ot::FmtStatus st = ot::wavImportFile(fs, path, opt, leftW(s), size_t(slot_[s].cap) * ch, res);
    if (st != ot::FmtStatus::Ok || res.frames < 16) {
        release(s);
        std::snprintf(err, size_t(errCap), "Cannot read that WAV");
        return -1;
    }
    // the importer writes planar: L, then R at plan.outFrames; the pool keeps R at cap
    if (ch == 2 && plan.outFrames != slot_[s].cap)
        std::memmove(leftW(s) + slot_[s].cap, leftW(s) + plan.outFrames, res.frames * sizeof(int16_t));
    copyStr(slot_[s].path, sizeof slot_[s].path, path);
    slot_[s].frames = res.frames;
    return s;
}

int SamplePool::builtinBreak() {
    // 2 bars at 172 BPM: a jungle break (kick, snare, ghosts, ride / hats),
    // stereo: hats a little right, ghosts a little left.
    const float bpm = 172.0f;
    const uint32_t n = uint32_t(2.0f * 4.0f * 60.0f / bpm * 44100.0f);
    const int s = alloc(n, 2);
    if (s < 0) return -1;
    int16_t* L = leftW(s);
    int16_t* R = rightW(s);
    std::memset(L, 0, n * sizeof(int16_t));
    std::memset(R, 0, n * sizeof(int16_t));
    dx::Noise nz;
    // per 16th over 2 bars: K kick, S snare, g ghost snare, h hat, o open hat
    static const char kPat[33] = "K.h.S.hgh.KhS.ghK.hgS.h.hgK.S.og";
    const float step = 60.0f / bpm / 4.0f * 44100.0f;
    for (int k = 0; k < 32; ++k) {
        const char c = kPat[k];
        if (c == '.') continue;
        const uint32_t at = uint32_t(float(k) * step);
        const float len = c == 'K' ? 0.32f : (c == 'S' ? 0.28f : (c == 'o' ? 0.22f : (c == 'g' ? 0.12f : 0.06f)));
        const uint32_t m = uint32_t(len * 44100.0f);
        float hz1 = 0, hx = 0, ph = 0, bp1 = 0, bp2 = 0;
        for (uint32_t i = 0; i < m && at + i < n; ++i) {
            const float t = float(i) * dx::kInvSr;
            float v = 0, pan = 0.0f;
            if (c == 'K') {
                const float f = 48.0f + 130.0f * expf(-t * 38.0f);
                ph += f * dx::kInvSr;
                v = dx::sin2pi(ph - floorf(ph)) * expf(-t * 7.5f) * 0.95f + nz.bi() * expf(-t * 300.0f) * 0.2f;
            } else if (c == 'S' || c == 'g') {
                const float a = c == 'S' ? 0.85f : 0.28f;
                ph += 185.0f * dx::kInvSr;
                // band-passed noise for the snare wires
                const float x = nz.bi();
                bp1 += 0.45f * (x - bp1);
                bp2 += 0.08f * (bp1 - bp2);
                v = (dx::sin2pi(ph - floorf(ph)) * expf(-t * 30.0f) * 0.55f + (bp1 - bp2) * 1.6f * expf(-t * 16.0f)) * a;
                pan = c == 'g' ? -0.3f : 0.0f;
            } else {   // hats
                const float x = nz.bi();
                hz1 = 0.55f * (hz1 + x - hx);
                hx = x;
                v = hz1 * expf(-t * (c == 'o' ? 14.0f : 85.0f)) * (c == 'o' ? 0.3f : 0.38f);
                pan = 0.35f;
            }
            const float gl = pan > 0 ? 1.0f - pan : 1.0f, gr = pan < 0 ? 1.0f + pan : 1.0f;
            int32_t ol = int32_t(L[at + i]) + int32_t(v * gl * 21000.0f);
            int32_t orr = int32_t(R[at + i]) + int32_t(v * gr * 21000.0f);
            L[at + i] = int16_t(ol > 32767 ? 32767 : (ol < -32768 ? -32768 : ol));
            R[at + i] = int16_t(orr > 32767 ? 32767 : (orr < -32768 ? -32768 : orr));
        }
    }
    slot_[s].path[0] = 0;
    slot_[s].frames = n;
    return s;
}

// ---------------------------------------------------------------------------
// Onsets: spectral flux (after the semi fork's "Breaks" auto slice, the
// user's own code): N = 256, hop 128 (2.9 ms), compressed magnitudes,
// high bins weighted, peak picking over +-2 frames with an adaptive threshold,
// 46 ms minimum spacing, the onset refined to a 32-frame energy jump.
// ---------------------------------------------------------------------------
namespace {
constexpr int kN = 256, kHop = 128, kBins = kN / 2 + 1, kMaxCand = 160;
struct OnsetScratch {
    float win[kN];
    float re[kN], im[kN];
    float prev[kBins];
    uint32_t candPos[kMaxCand];
    float candVal[kMaxCand];
};
OT_SDRAM_BSS OnsetScratch gOn;
}  // namespace

void SamplePool::detectOnsets(int s, float sens) {
    if (!valid(s)) return;
    SampleSlot& sl = slot_[s];
    const uint32_t len = sl.frames;
    sl.nOn = 0;
    if (len < 2048) {
        sl.onset[0] = 0;
        sl.strength[0] = 1.0f;
        sl.nOn = 1;
        return;
    }
    for (int i = 0; i < kN; ++i) gOn.win[i] = 0.5f - 0.5f * cosf(dx::kTwoPi * (float(i) + 0.5f) / float(kN));
    for (float& p : gOn.prev) p = 0;
    float hist[16] = {};
    float peakFlux = 0;
    int nCand = 0;
    uint32_t lastCand = 0;
    const uint32_t spacing = 2048;   // 46 ms
    const float kSens = 2.1f - 1.2f * dx::clampf(sens, 0.0f, 1.0f);   // threshold factor 0.9 .. 2.1
    for (uint32_t f = 0;; ++f) {
        const uint32_t f0 = f * kHop;
        if (f0 + kN > len) break;
        for (int i = 0; i < kN; ++i) {
            gOn.re[i] = mono(s, f0 + uint32_t(i)) * gOn.win[i];
            gOn.im[i] = 0;
        }
        dx::fft(gOn.re, gOn.im, kN, false);
        float flux = 0;
        for (int k = 1; k < kBins; ++k) {
            const float m = sqrtf(sqrtf(gOn.re[k] * gOn.re[k] + gOn.im[k] * gOn.im[k]));
            const float d = m - gOn.prev[k];
            gOn.prev[k] = m;
            if (d > 0) flux += d * (1.0f + float(k) / float(kBins));
        }
        for (int i = 0; i < 15; ++i) hist[i] = hist[i + 1];
        hist[15] = flux;
        if (flux > peakFlux) peakFlux = flux;
        if (f < 4) continue;
        const float c = hist[13];
        bool peak = c > 0;
        for (int i = 11; i <= 15; ++i)
            if (i != 13 && hist[i] >= c) peak = false;
        float mean = 0;
        for (int i = 0; i < 16; ++i) mean += hist[i];
        mean *= 1.0f / 16.0f;
        if (!peak || c <= kSens * 0.72f * mean + 0.03f * peakFlux) continue;
        // refine: the first 32-frame block whose energy jumps over twice the one before
        uint32_t pos = (f - 2) * kHop;
        {
            float prevE = 0;
            for (uint32_t b = 0; b * 32 < uint32_t(kN) && pos + (b + 1) * 32 <= len; ++b) {
                float e = 1e-9f;
                for (uint32_t i = 0; i < 32; ++i) {
                    const float x = mono(s, pos + b * 32 + i);
                    e += x * x;
                }
                if (b > 0 && e > 2.0f * prevE && e > 1e-4f) {
                    pos += (b > 1 ? b - 1 : 0) * 32;
                    break;
                }
                prevE = e;
            }
        }
        if (nCand > 0 && pos < lastCand + spacing) {
            if (c > gOn.candVal[nCand - 1]) {
                gOn.candPos[nCand - 1] = pos;
                gOn.candVal[nCand - 1] = c;
                lastCand = pos;
            }
        } else if (nCand < kMaxCand) {
            gOn.candPos[nCand] = pos;
            gOn.candVal[nCand] = c;
            ++nCand;
            lastCand = pos;
        }
    }
    // slice 0 at the start, then the strongest onsets in time order
    int idx[kMaxCand];
    int n = 0;
    for (int i = 0; i < nCand; ++i)
        if (gOn.candPos[i] >= spacing) idx[n++] = i;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && gOn.candVal[idx[j]] > gOn.candVal[idx[j - 1]]; --j) {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    if (n > kMaxOnsets - 1) n = kMaxOnsets - 1;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && gOn.candPos[idx[j]] < gOn.candPos[idx[j - 1]]; --j) {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    sl.onset[0] = 0;
    sl.strength[0] = 1.0f;
    sl.nOn = 1;
    const float norm = peakFlux > 0 ? 1.0f / peakFlux : 1.0f;
    for (int i = 0; i < n; ++i) {
        sl.onset[sl.nOn] = gOn.candPos[idx[i]];
        sl.strength[sl.nOn] = gOn.candVal[idx[i]] * norm;
        ++sl.nOn;
    }
    classify(s);
}

// Hit classes from the first 60 ms of each segment: low-band energy share
// (one-pole below ~150 Hz), high-band share (above ~5 kHz), zero-crossing
// rate and decay. Kick: mostly low, few crossings. Hat: mostly high, short.
// Snare: broadband noise with a body. Everything else: PERC.
void SamplePool::classify(int s) {
    if (!valid(s)) return;
    SampleSlot& sl = slot_[s];
    const float aLow = 1.0f - expf(-dx::kTwoPi * 150.0f * dx::kInvSr);
    const float aHigh = expf(-dx::kTwoPi * 5000.0f * dx::kInvSr);
    for (int k = 0; k < sl.nOn; ++k) {
        const uint32_t a = sl.onset[k];
        uint32_t e = k + 1 < sl.nOn ? sl.onset[k + 1] : sl.frames;
        const uint32_t win = 2646;   // 60 ms
        if (e > a + win) e = a + win;
        if (e <= a + 64) {
            sl.cls[k] = kClsOther;
            continue;
        }
        float lp = 0, hpY = 0, hpX = 0, eAll = 1e-9f, eLow = 0, eHigh = 0, eHead = 0, eTail = 0;
        int zc = 0;
        float prev = 0;
        const uint32_t mid = a + (e - a) / 2;
        for (uint32_t i = a; i < e; ++i) {
            const float x = mono(s, i);
            lp += aLow * (x - lp);
            hpY = aHigh * (hpY + x - hpX);
            hpX = x;
            eAll += x * x;
            eLow += lp * lp;
            eHigh += hpY * hpY;
            if ((x >= 0) != (prev >= 0)) ++zc;
            prev = x;
            if (i < mid) eHead += x * x;
            else eTail += x * x;
        }
        const float low = eLow / eAll, high = eHigh / eAll;
        const float zcr = float(zc) / float(e - a);
        const float decay = eTail / (eHead + 1e-9f);
        uint8_t c = kClsOther;
        if (low > 0.45f && zcr < 0.06f) c = kClsKick;
        else if (high > 0.35f && zcr > 0.18f && decay < 0.6f) c = kClsHat;
        else if (zcr > 0.08f && low < 0.45f && high > 0.08f) c = kClsSnare;
        else if (low > 0.3f) c = kClsKick;
        sl.cls[k] = c;
    }
}

void SamplePool::equalOnsets(int s, int n) {
    if (!valid(s)) return;
    SampleSlot& sl = slot_[s];
    n = n < 1 ? 1 : (n > kMaxOnsets ? kMaxOnsets : n);
    for (int k = 0; k < n; ++k) {
        sl.onset[k] = uint32_t(uint64_t(sl.frames) * uint32_t(k) / uint32_t(n));
        sl.strength[k] = 1.0f;
    }
    sl.nOn = uint8_t(n);
    classify(s);
}

int SamplePool::addOnset(int s, uint32_t at) {
    if (!valid(s)) return -1;
    SampleSlot& sl = slot_[s];
    if (sl.nOn >= kMaxOnsets || at >= sl.frames) return -1;
    int k = 0;
    while (k < sl.nOn && sl.onset[k] < at) ++k;
    if (k < sl.nOn && sl.onset[k] == at) return k;
    for (int i = sl.nOn; i > k; --i) {
        sl.onset[i] = sl.onset[i - 1];
        sl.strength[i] = sl.strength[i - 1];
        sl.cls[i] = sl.cls[i - 1];
    }
    sl.onset[k] = at;
    sl.strength[k] = 0.5f;
    sl.cls[k] = kClsOther;
    ++sl.nOn;
    return k;
}

void SamplePool::removeOnset(int s, int k) {
    if (!valid(s)) return;
    SampleSlot& sl = slot_[s];
    if (k <= 0 || k >= sl.nOn) return;
    for (int i = k; i + 1 < sl.nOn; ++i) {
        sl.onset[i] = sl.onset[i + 1];
        sl.strength[i] = sl.strength[i + 1];
        sl.cls[i] = sl.cls[i + 1];
    }
    --sl.nOn;
}

void SamplePool::moveOnset(int s, int k, int32_t d) {
    if (!valid(s)) return;
    SampleSlot& sl = slot_[s];
    if (k <= 0 || k >= sl.nOn) return;
    const int64_t lo = int64_t(sl.onset[k - 1]) + 64;
    const int64_t hi = (k + 1 < sl.nOn ? int64_t(sl.onset[k + 1]) : int64_t(sl.frames)) - 64;
    int64_t v = int64_t(sl.onset[k]) + d;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (hi > lo) sl.onset[k] = uint32_t(v);
}

bool SamplePool::sliceRange(int s, int k, uint32_t& a, uint32_t& e) const {
    if (!valid(s)) return false;
    const SampleSlot& sl = slot_[s];
    if (sl.nOn == 0) {
        if (k != 0) return false;
        a = 0;
        e = sl.frames;
        return true;
    }
    if (k < 0 || k >= sl.nOn) return false;
    a = sl.onset[k];
    e = k + 1 < sl.nOn ? sl.onset[k + 1] : sl.frames;
    if (e > sl.frames) e = sl.frames;
    return e > a + 2;
}

float loopBpm(uint32_t frames, int& bars, float hint) {
    const float sec = float(frames) * dx::kInvSr;
    if (sec < 0.05f) {
        if (bars <= 0) bars = 1;
        return 120.0f;
    }
    if (bars > 0) return 240.0f * float(bars) / sec;
    static const int kB[6] = {1, 2, 4, 8, 16, 32};
    float best = 0;
    float bestScore = 1e9f;
    for (int b : kB) {
        const float bpm = 240.0f * float(b) / sec;
        if (bpm < 60.0f || bpm > 200.0f) continue;
        // close to the hint (the project tempo) or its half / double
        float d = fabsf(bpm - hint);
        const float d2 = fabsf(bpm * 2.0f - hint), d3 = fabsf(bpm * 0.5f - hint);
        if (d2 < d) d = d2 * 1.5f;
        if (d3 < d) d = d3 * 1.5f;
        if (bpm < 85.0f || bpm > 180.0f) d += 30.0f;
        if (d < bestScore) {
            bestScore = d;
            best = bpm;
            bars = b;
        }
    }
    if (best == 0) {   // a one-shot or an odd length: 1 bar
        bars = 1;
        best = 240.0f / sec;
    }
    return best;
}

}  // namespace konsol
}  // namespace lab
