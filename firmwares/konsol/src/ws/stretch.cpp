// KONSOL offline time-stretch / pitch-shift (stretch.h).
#include "ws/stretch.h"

#include <cmath>
#include <cstring>

#include "ws/dspx.h"

namespace lab {
namespace konsol {

const char* stretchModeName(int m) {
    static const char* const k[kStrModes] = {"Beats", "Tonal", "Repitch"};
    return k[m < 0 ? 0 : (m >= kStrModes ? kStrModes - 1 : m)];
}

namespace {
constexpr uint32_t kFade = 96;   // 2 ms segment fade-out
inline float wrapPi(float x) {
    x = fmodf(x + dx::kPi, dx::kTwoPi);
    if (x < 0) x += dx::kTwoPi;
    return x - dx::kPi;
}
}  // namespace

bool Stretcher::init(Arena& a) {
    for (int c = 0; c < 2; ++c) {
        ring_[c] = static_cast<float*>(a.alloc(kRing * sizeof(float)));
        re_[c] = static_cast<float*>(a.alloc(kN * sizeof(float)));
        im_[c] = static_cast<float*>(a.alloc(kN * sizeof(float)));
        lastPh_[c] = static_cast<float*>(a.alloc(kBins * sizeof(float)));
    }
    synPh_ = static_cast<float*>(a.alloc(kBins * sizeof(float)));
    mag_ = static_cast<float*>(a.alloc(2 * kBins * sizeof(float)));
    pvWin_ = static_cast<float*>(a.alloc(kN * sizeof(float)));
    if (!ring_[1] || !re_[1] || !im_[1] || !lastPh_[1] || !synPh_ || !mag_ || !pvWin_) return false;
    for (int i = 0; i < kGrain; ++i) win_[i] = 0.5f - 0.5f * cosf(dx::kTwoPi * float(i) / float(kGrain));
    for (int i = 0; i < kN; ++i) pvWin_[i] = 0.5f - 0.5f * cosf(dx::kTwoPi * float(i) / float(kN));
    return true;
}

void Stretcher::cancel() {
    if (busy_ && pool_ && out_ >= 0) pool_->release(out_);
    busy_ = false;
    out_ = -1;
}

bool Stretcher::start(SamplePool& pool, int src, uint32_t a, uint32_t e, float ratio, float pitch, int mode) {
    cancel();
    if (!pool.valid(src)) return false;
    const SampleSlot& s = pool.slot(src);
    if (e > s.frames) e = s.frames;
    if (a + 256 >= e) return false;
    ratio = dx::clampf(ratio, 0.25f, 4.0f);
    pool_ = &pool;
    src_ = src;
    a_ = a;
    e_ = e;
    mode_ = mode;
    ch_ = s.ch;
    ratio_ = ratio;
    pr_ = mode == kStrRepitch ? 1.0f : dx::semis(dx::clampf(pitch, -24.0f, 24.0f));
    tr_ = ratio_ * pr_;
    outLen_ = uint32_t(float(e - a) * ratio_);
    interLen_ = uint32_t(float(e - a) * tr_);
    out_ = pool.alloc(outLen_ + 2, ch_);
    if (out_ < 0) return false;
    written_ = 0;
    for (int c = 0; c < 2; ++c) std::memset(ring_[c], 0, kRing * sizeof(float));
    ringFinal_ = 0;
    ringBase_ = 0;
    readPos_ = 0;
    // segments at the source's onsets inside [a, e) (BEATS); one for the rest
    nSeg_ = 0;
    segS_[nSeg_++] = a;
    if (mode == kStrBeats)
        for (int i = 0; i < s.nOn && nSeg_ < kMaxOnsets; ++i)
            if (s.onset[i] > a + 512 && s.onset[i] + 256 < e) segS_[nSeg_++] = s.onset[i];
    segS_[nSeg_] = e;
    for (int i = 0; i <= nSeg_; ++i) segT_[i] = uint32_t(float(segS_[i] - a) * tr_);
    seg_ = 0;
    grain_ = 0;
    prevX_ = float(a);
    frame_ = -(kN / kH - 1);
    firstFrame_ = true;
    nextOnset_ = 0;
    busy_ = true;
    return true;
}

float Stretcher::readSrc(int c, float pos) const {
    if (pos < float(a_) || pos >= float(e_ - 1)) return 0.0f;
    const uint32_t i = uint32_t(pos);
    const float f = pos - float(i);
    const int16_t* d = (c && ch_ == 2) ? pool_->right(src_) : pool_->left(src_);
    return (float(d[i]) + (float(d[i + 1]) - float(d[i])) * f) * (1.0f / 32768.0f);
}

void Stretcher::emit(int c, uint32_t at, float v) { ring_[c][at % kRing] += v; }

// ---- WSOLA (BEATS) ------------------------------------------------------------------
bool Stretcher::produceWsola() {
    if (seg_ >= nSeg_) return false;
    const uint32_t s0 = segS_[seg_], s1 = segS_[seg_ + 1], t0 = segT_[seg_], t1 = segT_[seg_ + 1];
    const uint32_t og = t0 + uint32_t(grain_) * kHop;
    if (og >= t1 || t1 <= t0) {
        ++seg_;
        grain_ = 0;
        ringFinal_ = seg_ < nSeg_ ? segT_[seg_] : interLen_ + kFade;
        return true;
    }
    float x;
    const float hi = s1 > s0 + kGrain ? float(s1 - kGrain) : float(s0);
    if (grain_ == 0) {
        x = float(s0);
    } else {
        // nominal position, then the best match for the natural continuation
        const float xn = float(s0) + float(og - t0) / tr_;
        const float nat = prevX_ + float(kHop);
        float best = xn, bestC = -1e30f;
        for (int d = -kTol; d <= kTol; d += 4) {
            float cand = xn + float(d);
            if (cand < float(s0)) cand = float(s0);
            if (cand > hi) cand = hi;
            float acc = 0, e1 = 1e-9f;
            for (int i = 0; i < kGrain / 2; i += 4) {
                const uint32_t p = uint32_t(cand) + uint32_t(i), q = uint32_t(nat) + uint32_t(i);
                if (p + 1 >= e_ || q + 1 >= e_) break;
                const float u = pool_->mono(src_, p), w = pool_->mono(src_, q);
                acc += u * w;
                e1 += u * u;
            }
            const float score = acc / sqrtf(e1);
            if (score > bestC) {
                bestC = score;
                best = cand;
            }
        }
        x = best;
    }
    prevX_ = x;
    const uint32_t xi = uint32_t(x);
    for (int c = 0; c < ch_; ++c) {
        const int16_t* d = c ? pool_->right(src_) : pool_->left(src_);
        for (int i = 0; i < kGrain; ++i) {
            const uint32_t f = og + uint32_t(i);
            if (f >= t1 + kFade) break;
            const uint32_t sp = xi + uint32_t(i);
            if (sp >= s1 || sp >= e_) break;   // never into the next hit
            float w = (grain_ == 0 && i < kHop) ? 1.0f : win_[i];
            if (f >= t1) w *= 1.0f - float(f - t1) / float(kFade);
            emit(c, f, float(d[sp]) * (1.0f / 32768.0f) * w);
        }
    }
    ++grain_;
    const uint32_t fin = og + kHop < t1 ? og + kHop : t1;
    if (fin > ringFinal_) ringFinal_ = fin;
    return true;
}

// ---- phase vocoder (TONAL) -------------------------------------------------------------
bool Stretcher::producePv() {
    const int64_t og = int64_t(frame_) * kH;
    if (og >= int64_t(interLen_)) return false;
    const float ha = float(kH) / tr_;
    const float ia = float(a_) + float(og) / tr_;
    const SampleSlot& s = pool_->slot(src_);
    // phase reset when a strong onset enters this frame (transients stay sharp)
    bool reset = firstFrame_;
    while (nextOnset_ < s.nOn && float(s.onset[nextOnset_]) < ia + float(kN) * 0.25f) {
        if (float(s.onset[nextOnset_]) >= ia && s.strength[nextOnset_] > 0.35f) reset = true;
        ++nextOnset_;
    }
    for (int c = 0; c < ch_; ++c) {
        for (int i = 0; i < kN; ++i) {
            re_[c][i] = readSrc(c, ia + float(i)) * pvWin_[i];
            im_[c][i] = 0;
        }
        dx::fft(re_[c], im_[c], kN, false);
    }
    // phase advance from the mid channel, applied to both (keeps the stereo image)
    float* magM = mag_;
    float* phM = mag_ + kBins;
    for (int k = 0; k < kBins; ++k) {
        float r = re_[0][k], i = im_[0][k];
        if (ch_ == 2) {
            r += re_[1][k];
            i += im_[1][k];
        }
        magM[k] = r * r + i * i;
        phM[k] = atan2f(i, r);
    }
    for (int k = 0; k < kBins; ++k) {
        if (reset) {
            synPh_[k] = phM[k];
        } else {
            const float omega = dx::kTwoPi * float(k) / float(kN);
            const float dphi = wrapPi(phM[k] - lastPh_[0][k] - omega * ha);
            synPh_[k] = synPh_[k] + (omega + dphi / ha) * float(kH);
        }
        lastPh_[0][k] = phM[k];
    }
    // identity phase locking: every bin follows its nearest spectral peak
    if (!reset) {
        int lastPeak = -1;
        for (int k = 0; k < kBins;) {
            int p = -1;
            for (int j = k; j < kBins; ++j) {
                const float m = magM[j];
                const bool pk = (j < 1 || m > magM[j - 1]) && (j < 2 || m >= magM[j - 2]) &&
                                (j + 1 >= kBins || m >= magM[j + 1]) && (j + 2 >= kBins || m >= magM[j + 2]);
                if (pk) {
                    p = j;
                    break;
                }
            }
            if (p < 0) p = kBins - 1;
            // bins between the last peak and this one: split at the midpoint
            const int mid = lastPeak < 0 ? k : (lastPeak + p) / 2;
            for (int j = k; j < p; ++j) {
                const int q = (lastPeak >= 0 && j <= mid) ? lastPeak : p;
                if (q != j) synPh_[j] = synPh_[q] + (phM[j] - phM[q]);
            }
            lastPeak = p;
            k = p + 1;
        }
    }
    for (int k = 0; k < kBins; ++k) {
        float d = synPh_[k] - phM[k];
        d = wrapPi(d);
        synPh_[k] = phM[k] + d;   // keep the numbers small
        const float cs = cosf(d), sn = sinf(d);
        for (int c = 0; c < ch_; ++c) {
            const float r = re_[c][k], i = im_[c][k];
            re_[c][k] = r * cs - i * sn;
            im_[c][k] = r * sn + i * cs;
        }
    }
    const float norm = 1.0f / (float(kN) * 1.5f);
    for (int c = 0; c < ch_; ++c) {
        for (int k = 1; k < kN / 2; ++k) {
            re_[c][kN - k] = re_[c][k];
            im_[c][kN - k] = -im_[c][k];
        }
        dx::fft(re_[c], im_[c], kN, true);
        for (int i = 0; i < kN; ++i) {
            const int64_t f = og + i;
            if (f < 0 || f >= int64_t(interLen_)) continue;
            emit(c, uint32_t(f), re_[c][i] * pvWin_[i] * norm);
        }
    }
    firstFrame_ = false;
    ++frame_;
    const int64_t fin = og + kH;
    if (fin > int64_t(ringFinal_)) ringFinal_ = uint32_t(fin);
    return true;
}

bool Stretcher::produce() { return mode_ == kStrTonal ? producePv() : produceWsola(); }

void Stretcher::resampleOut() {
    // fixed point 32.32 read position in producer frames
    const uint64_t step = uint64_t(double(pr_) * 4294967296.0);
    uint64_t pos = uint64_t(readPos_ * 4294967296.0);
    int16_t* L = pool_->leftW(out_);
    int16_t* R = pool_->rightW(out_);
    const uint32_t lim = ringFinal_ < interLen_ ? ringFinal_ : interLen_;
    while (written_ < outLen_) {
        const uint32_t i = uint32_t(pos >> 32);
        if (i + 1 >= lim && lim < interLen_) break;
        const float f = float(uint32_t(pos & 0xFFFFFFFFu)) * (1.0f / 4294967296.0f);
        for (int c = 0; c < ch_; ++c) {
            const float y0 = i < interLen_ ? ring_[c][i % kRing] : 0.0f;
            const float y1 = i + 1 < interLen_ ? ring_[c][(i + 1) % kRing] : 0.0f;
            float v = (y0 + (y1 - y0) * f) * 32767.0f;
            v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
            (c ? R : L)[written_] = int16_t(v);
        }
        ++written_;
        pos += step;
        // frames below the read position are spent: clear them for reuse
        const uint32_t now = uint32_t(pos >> 32);
        while (ringBase_ + 1 < now) {
            for (int c = 0; c < 2; ++c) ring_[c][ringBase_ % kRing] = 0;
            ++ringBase_;
        }
    }
    readPos_ = double(pos) / 4294967296.0;
}

bool Stretcher::step(uint32_t frames) {
    if (!busy_) return false;
    const uint32_t target = written_ + frames;
    if (mode_ == kStrRepitch) {
        // speed = 1 / ratio: tape-style, cubic-free linear interpolation
        const float sp = 1.0f / ratio_;
        int16_t* L = pool_->leftW(out_);
        int16_t* R = pool_->rightW(out_);
        while (written_ < outLen_ && written_ < target) {
            const float p = float(a_) + float(written_) * sp;
            for (int c = 0; c < ch_; ++c) {
                const float v = readSrc(c, p) * 32767.0f;
                (c ? R : L)[written_] = int16_t(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }
            ++written_;
        }
    } else {
        while (written_ < outLen_ && written_ < target) {
            resampleOut();
            if (written_ >= outLen_) break;
            const uint32_t need = ringFinal_ + uint32_t(kN) + uint32_t(kN);
            if (need - ringBase_ >= kRing) continue;   // the resampler catches up first
            if (!produce()) {
                ringFinal_ = interLen_;
                resampleOut();
                while (written_ < outLen_) {   // rounding: pad
                    pool_->leftW(out_)[written_] = 0;
                    if (ch_ == 2) pool_->rightW(out_)[written_] = 0;
                    ++written_;
                }
            }
        }
    }
    if (written_ >= outLen_) {
        SampleSlot& o = pool_->slot(out_);
        o.derived = 1;
        o.frames = outLen_;
        // onsets of the source, time-scaled
        const SampleSlot& s = pool_->slot(src_);
        o.nOn = 0;
        for (int i = 0; i < s.nOn; ++i)
            if (s.onset[i] >= a_ && s.onset[i] < e_ && o.nOn < kMaxOnsets) {
                o.onset[o.nOn] = uint32_t(float(s.onset[i] - a_) * ratio_);
                o.cls[o.nOn] = s.cls[i];
                o.strength[o.nOn] = s.strength[i];
                ++o.nOn;
            }
        busy_ = false;
        return false;
    }
    return true;
}

}  // namespace konsol
}  // namespace lab
