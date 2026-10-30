// KONSOL sample engines on the shared pool: SAMPLER (stereo; one-shot, loop,
// equal or transient slices; realtime WARP: Beats = transient-locked
// re-triggering, Texture = two-grain overlap-add), GRAIN (stereo granular
// cloud) and KIT (16 pads).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "ws/dspx.h"
#include "ws/engine_table.h"
#include "ws/kit.h"
#include "ws/model.h"
#include "ws/stream.h"

namespace lab {
namespace konsol {

namespace {
using dx::clampf;
using dx::kInvSr;
using dx::kSr;

inline float msToInc(float ms) { return 1.0f / (clampf(ms, 0.2f, 60000.0f) * 0.001f * kSr); }
inline float rd(const int16_t* d, uint32_t p, float f, uint32_t len) {
    const int32_t s0 = d[p];
    const int32_t s1 = d[p + 1 < len ? p + 1 : p];
    return float(s0) + float(s1 - s0) * f;
}
inline float resQ(float r) { return 0.55f + 9.0f * r * r; }

// =============================================================================
// SAMPLER
// =============================================================================
const char* const kSmpMode[3] = {"One-shot", "Loop", "Slices"};
const char* const kSliceMode[2] = {"Equal", "Transient"};
const char* const kWarpMode[3] = {"Off", "Beats", "Texture"};
enum SmpP { sMode, sStart, sEnd, sSliceMode, sSlices, sWarp, sPitch, sFine, sAttack, sRelease, sVoices, sCutoff, sRes,
            sDrive, sWidth, sGain, sSmpN };
const PDesc kSmpDesc[sSmpN] = {
    {"Mode", 0, 2, 2, 1, kFList, kSmpMode},
    {"Start", 0, 1, 0, 0.005f, kFPct, nullptr},
    {"End", 0, 1, 1, 0.005f, kFPct, nullptr},
    {"Slice By", 0, 1, 1, 1, kFList, kSliceMode},
    {"Slices", 2, 64, 16, 1, kFInt, nullptr},
    {"Warp", 0, 2, 0, 1, kFList, kWarpMode},
    {"Pitch", -24, 24, 0, 1, kFSemi, nullptr},
    {"Fine", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Attack", 0, 500, 1, 1, kFMs, nullptr},
    {"Release", 5, 4000, 60, 5, kFMs, nullptr},
    {"Voices", 1, 4, 4, 1, kFInt, nullptr},
    {"Cutoff", 40, 18000, 18000, 1, kFHz, nullptr},
    {"Resonance", 0, 1, 0.1f, 0.01f, kFPct, nullptr},
    {"Drive", 0, 1, 0, 0.01f, kFPct, nullptr},
    {"Width", 0, 1, 1, 0.01f, kFPct, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kSmpSec[3] = {{"Sample", 0, 6}, {"Pitch/Amp", 6, 5}, {"Tone", 11, 5}};

class Sampler final : public TableEngine {
public:
    Sampler(SamplePool* pool, TrackSample* ts) : pool_(pool), ts_(ts) { bind(kSmpDesc, sSmpN, kSmpSec, 3); }
    int kind() const override { return kEngSampler; }
    void preset(int v) override {
        reset();
        if (v == 1) p_[sMode] = 1;
    }
    void setTempo(float bpm) override { bpm_ = bpm; }
    void noteOn(int pitch, float vel, int offset, int tone, int flags) override {
        const int slot = ts_->slot;
        if (!pool_->valid(slot)) return;
        const SampleSlot& sl = pool_->slot(slot);
        const int maxV = ip(sVoices);
        int vi = -1;
        for (int i = 0; i < maxV; ++i)
            if (!v_[i].on) {
                vi = i;
                break;
            }
        if (vi < 0) {
            uint32_t best = 0xFFFFFFFFu;
            for (int i = 0; i < maxV; ++i)
                if (v_[i].age < best) {
                    best = v_[i].age;
                    vi = i;
                }
        }
        V& v = v_[vi];
        dropRing(v);
        const int mode = ip(sMode);
        // SD streaming: a streamed slot plays its whole file (One-shot / Loop,
        // Equal slices; no Warp, no reverse, at most kStreamMaxRate).
        const bool str = sl.stream && pool_->streams();
        const uint32_t len = str ? sl.sframes : sl.frames;
        uint32_t a = uint32_t(p_[sStart] * float(len)), e = uint32_t(p_[sEnd] * float(len));
        if (e > len) e = len;
        if (e <= a + 64) {
            a = 0;
            e = len;
        }
        float rate = dx::semis(p_[sPitch] + p_[sFine]);
        v.warp = 0;
        if (mode == 2) {
            const int note = pitch - 36;
            if (ip(sSliceMode) == 1 && sl.nOn > 1 && !str) {
                int k0 = 0;
                while (k0 < sl.nOn && sl.onset[k0] < a) ++k0;
                int n = 0;
                while (k0 + n < sl.nOn && sl.onset[k0 + n] < e) ++n;
                if (n > 0) {
                    const int k = k0 + ((note % n) + n) % n;
                    const uint32_t sa = sl.onset[k];
                    const uint32_t se = k + 1 < sl.nOn && sl.onset[k + 1] < e ? sl.onset[k + 1] : e;
                    a = sa;
                    e = se > sa + 64 ? se : sa + 64;
                }
            } else {
                const int n = ip(sSlices);
                const int k = ((note % n) + n) % n;
                const uint32_t span = (e - a) / uint32_t(n);
                a = a + span * uint32_t(k);
                e = a + span;
            }
        } else {
            rate *= dx::semis(float(pitch - 60));
            if (ip(sWarp) && ts_->outBpm > 1.0f && !str) {
                v.warp = uint8_t(ip(sWarp));
                rate = dx::semis(p_[sPitch] + p_[sFine] + float(pitch - 60));   // pitch only, the tempo moves time
            }
        }
        v.slot = int16_t(slot);
        v.rev = (flags & kFlagReverse) != 0 && !v.warp && !str;
        v.str = str;
        if (str) {
            if (rate > kStreamMaxRate) rate = kStreamMaxRate;
            v.g.a = a;
            v.g.e = e;
            v.g.loop = mode == 1;
            // a note whose whole range is resident needs no ring
            if (e > sl.frames) v.ring = int8_t(pool_->streams()->acquire(slot, sl.ch, sl.sid, sl.frames, v.g, &v.rgen));
        }
        v.a = a;
        v.e = e;
        v.loop = mode == 1;
        v.pos = v.rev ? e - 2 : a;
        v.frac = 0;
        v.inc = rate;
        v.vel = vel;
        v.env = p_[sAttack] < 1.0f ? 1.0f : 0.0f;
        v.att = msToInc(p_[sAttack]);
        v.rel = false;
        v.relInc = msToInc(p_[sRelease]);
        v.pitch = pitch;
        v.wait = uint32_t(offset > 0 && offset < int(kBlock) ? offset : 0);
        v.age = ++age_;
        v.vpos = float(a);
        v.hgain = 1;
        v.tgain = 0;
        v.nextB = boundary(sl, v, a);
        v.gph[0] = 0.5f;   // texture: one grain half way, one starting
        v.gph[1] = 1.0f;
        v.gpos[0] = v.gpos[1] = a;
        v.gfrac[0] = v.gfrac[1] = 0;
        v.gnext = 0;
        v.on = true;
        toneMul_ = tone ? dx::semis(float(tone) * 0.5f) : 1.0f;
        filtDirty_ = true;
    }
    void noteOff(int pitch) override {
        if (ip(sMode) != 1) return;   // one-shots and slices play to their end
        for (auto& v : v_)
            if (v.on && v.pitch == pitch) v.rel = true;
    }
    void allOff(bool hard) override {
        for (auto& v : v_) {
            if (hard) v.on = false;
            else v.rel = true;
        }
    }
    void render(float* l, float* r) override {
        const int slot = ts_->slot;
        if (!pool_->valid(slot)) {
            for (auto& v : v_) v.on = false;
            return;
        }
        const SampleSlot& sl = pool_->slot(slot);
        const uint32_t len = sl.frames;
        const uint32_t full = pool_->length(slot);
        const int16_t* dl = pool_->left(slot);
        const int16_t* dr = pool_->right(slot);
        for (auto& v : v_)
            if (!v.on) dropRing(v);
        if (filtDirty_) {
            const float hz = p_[sCutoff] * toneMul_;
            fl_.set(hz, resQ(p_[sRes]));
            fr_.set(hz, resQ(p_[sRes]));
            filtDirty_ = false;
        }
        alignas(16) float al[kBlock], ar[kBlock];
        bool any = false;
        const float ratio = ts_->outBpm > 1.0f ? bpm_ / ts_->outBpm : 1.0f;
        for (auto& v : v_) {
            if (!v.on) continue;
            if (v.slot != slot || v.e > (v.str ? full : len) || v.a + 2 >= v.e || v.str != (sl.stream != 0)) {
                v.on = false;   // the sample changed under the voice
                dropRing(v);
                continue;
            }
            if (!any) {
                for (uint32_t i = 0; i < kBlock; ++i) al[i] = ar[i] = 0.0f;
                any = true;
            }
            bool alive;
            if (v.str) alive = renderStream(v, sl, dl, dr, al, ar);
            else if (v.warp == 1) alive = renderBeats(v, sl, dl, dr, al, ar, ratio);
            else if (v.warp == 2) alive = renderTexture(v, dl, dr, al, ar, ratio);
            else alive = renderPlain(v, dl, dr, al, ar, len);
            if (!alive) {
                v.on = false;
                dropRing(v);
            }
        }
        if (!any) return;
        const float gain = p_[sGain] * (1.0f / 32768.0f);
        const float drive = p_[sDrive];
        const bool filt = p_[sCutoff] * toneMul_ < 17000.0f || p_[sRes] > 0.15f;
        const float w = p_[sWidth];
        const float pre = 1.0f + drive * 6.0f, post = 1.0f / (1.0f + drive * 1.5f);
        for (uint32_t i = 0; i < kBlock; ++i) {
            float xl = al[i] * gain, xr = ar[i] * gain;
            if (w < 0.99f) {
                const float m = 0.5f * (xl + xr), s = 0.5f * (xl - xr) * w;
                xl = m + s;
                xr = m - s;
            }
            if (filt) {
                xl = fl_.low(xl);
                xr = fr_.low(xr);
            }
            if (drive > 0.01f) {
                xl = dx::soft(xl * pre) * post;
                xr = dx::soft(xr * pre) * post;
            }
            l[i] += xl;
            r[i] += xr;
        }
    }
    int active() const override {
        int n = 0;
        for (auto& v : v_) n += v.on;
        return n;
    }
    // QEMU-measured per voice: plain 9.1k, Beats 12.8k, Texture 25.4k; the filter 4k
    int32_t base() const { return 2500 + ((p_[sCutoff] < 17000.0f || p_[sRes] > 0.15f) ? 4000 : 0); }
    int32_t cost() const override {
        int32_t c = 0;
        for (auto& v : v_)
            if (v.on) c += v.warp == 2 ? 25500 : (v.warp == 1 ? 13000 : (v.str ? kStreamCost : 9300));
        return c ? c + base() : 0;
    }
    int32_t noteCost() const override {
        int32_t one = ip(sMode) != 2 && ip(sWarp) == 2 ? 25500 : (ip(sMode) != 2 && ip(sWarp) == 1 ? 13000 : 9300);
        if (pool_->streamed(ts_->slot)) one = kStreamCost;
        return active() ? one : one + base();
    }
    bool steal() override {
        int vi = -1;
        uint32_t best = 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i)
            if (v_[i].on && v_[i].age < best) {
                best = v_[i].age;
                vi = i;
            }
        if (vi < 0) return false;
        v_[vi].on = false;
        return true;
    }

private:
    struct V {
        bool on = false, rel = false, loop = false, rev = false;
        uint8_t warp = 0;
        int16_t slot = -1;
        uint32_t pos = 0, a = 0, e = 0, age = 0, wait = 0;
        float frac = 0, inc = 1, vel = 1, env = 0, att = 1, relInc = 0.001f;
        int pitch = 0;
        // warp
        float vpos = 0, hgain = 1, tgain = 0, tfrac = 0;
        uint32_t nextB = 0, tpos = 0;
        uint32_t gpos[2] = {};
        float gfrac[2] = {}, gph[2] = {};
        int32_t gnext = 0;
        // SD streaming: virtual positions (pos), the note's geometry, its ring
        bool str = false;
        int8_t ring = -1;
        uint32_t rgen = 0;
        StreamGeom g;
    };
    // QEMU-measured streamed voice (window fetch + the plain loop)
    static constexpr int32_t kStreamCost = 11500;
    void dropRing(V& v) {
        if (v.ring >= 0 && pool_->streams()) pool_->streams()->release(v.ring, v.rgen);
        v.ring = -1;
    }
    // A streamed voice: each block copies its window (head / ring, underruns
    // fade) and runs the plain linear interpolation over it.
    bool renderStream(V& v, const SampleSlot& sl, const int16_t* dl, const int16_t* dr, float* al, float* ar) {
        Streams* st = pool_->streams();
        StreamRing* ring = nullptr;
        if (v.ring >= 0) {
            if (!st->owns(v.ring, v.rgen)) return false;   // another note took the ring
            ring = &st->ring(v.ring);
        }
        const uint32_t i0 = v.wait;
        v.wait = 0;
        const float inc = v.inc;
        float frac = v.frac, env = v.env;
        constexpr uint32_t kWin = uint32_t(kStreamMaxRate) * kBlock + 4;
        uint32_t n = uint32_t(frac + inc * float(kBlock - i0)) + 2;
        if (n > kWin) n = kWin;
        int16_t wl[kWin], wr[kWin];
        const uint32_t pos = v.pos;
        streamFetch(ring, v.rgen, v.g, sl.ch, dl, dr, sl.frames, pos, n, wl, wr);
        const float g = v.vel;
        uint32_t j = 0;
        bool alive = true;
        for (uint32_t i = i0; i < kBlock; ++i) {
            if (!envStep(v, env)) {
                alive = false;
                break;
            }
            if (!v.g.loop && pos + j + 1 >= v.g.e) {
                alive = false;
                break;
            }
            const float ge = g * env;
            const uint32_t j1 = j + 1 < n ? j + 1 : j;
            al[i] += (float(wl[j]) + float(wl[j1] - wl[j]) * frac) * ge;
            ar[i] += (float(wr[j]) + float(wr[j1] - wr[j]) * frac) * ge;
            frac += inc;
            const uint32_t k = uint32_t(frac);
            frac -= float(k);
            j += k;
            if (j + 1 >= n) j = n - 2;   // never past the window (rounding)
        }
        v.pos = pos + j;
        v.frac = frac;
        v.env = env;
        if (ring) ring->need = v.pos;
        return alive;
    }
    void changed(int i) override {
        if (i == sCutoff || i == sRes) filtDirty_ = true;
    }
    // amp envelope step; false = released to silence
    static inline bool envStep(V& v, float& env) {
        if (v.rel) {
            env -= v.relInc;
            if (env <= 0.0f) {
                env = 0.0f;
                return false;
            }
        } else if (env < 1.0f) {
            env += v.att;
            if (env > 1.0f) env = 1.0f;
        }
        return true;
    }
    // next segment boundary after `from` (onsets, or a 1/16 grid of the tempo)
    uint32_t boundary(const SampleSlot& sl, const V& v, uint32_t from) const {
        if (sl.nOn > 1) {
            for (int k = 0; k < sl.nOn; ++k)
                if (sl.onset[k] > from) return sl.onset[k] < v.e ? sl.onset[k] : v.e;
            return v.e;
        }
        const float bpm = ts_->outBpm > 1.0f ? ts_->outBpm : 120.0f;
        const uint32_t grid = uint32_t(60.0f / bpm / 4.0f * kSr);
        if (grid < 64) return v.e;
        const uint32_t k = (from - v.a) / grid + 1;
        const uint32_t b = v.a + k * grid;
        return b < v.e ? b : v.e;
    }
    bool renderPlain(V& v, const int16_t* dl, const int16_t* dr, float* al, float* ar, uint32_t len) {
        const float g = v.vel;
        uint32_t pos = v.pos;
        float frac = v.frac, env = v.env;
        const float inc = v.inc;
        const uint32_t e = v.e, a = v.a;
        const uint32_t i0 = v.wait;
        v.wait = 0;
        bool alive = true;
        for (uint32_t i = i0; i < kBlock; ++i) {
            if (!envStep(v, env)) {
                alive = false;
                break;
            }
            const float ge = g * env;
            al[i] += rd(dl, pos, frac, len) * ge;
            ar[i] += rd(dr, pos, frac, len) * ge;
            frac += inc;
            const uint32_t k = uint32_t(frac);
            frac -= float(k);
            if (!v.rev) {
                pos += k;
                if (pos + 1 >= e) {
                    if (v.loop) pos = a + (pos + 1 - e) % ((e - a) ? (e - a) : 1);
                    else {
                        alive = false;
                        break;
                    }
                }
            } else {
                if (pos < a + k + 1) {
                    if (v.loop) pos = e - 2;
                    else {
                        alive = false;
                        break;
                    }
                } else {
                    pos -= k;
                }
            }
        }
        v.pos = pos;
        v.frac = frac;
        v.env = env;
        return alive;
    }
    // Beats: the virtual position runs at the tempo ratio; at each onset the
    // head jumps there (the old one fades out over 64 samples); a head that
    // runs past its segment (slower tempo) fades out: hits stay crisp.
    bool renderBeats(V& v, const SampleSlot& sl, const int16_t* dl, const int16_t* dr, float* al, float* ar,
                     float ratio) {
        const float g = v.vel;
        float env = v.env;
        const uint32_t e = v.e, a = v.a, len = sl.frames;
        const uint32_t i0 = v.wait;
        v.wait = 0;
        uint32_t segEnd = v.nextB;
        for (uint32_t i = i0; i < kBlock; ++i) {
            if (!envStep(v, env)) {
                v.env = 0;
                return false;
            }
            v.vpos += ratio;
            if (v.vpos >= float(v.nextB)) {
                uint32_t start = v.nextB;
                if (start >= e) {
                    if (!v.loop) return false;
                    v.vpos -= float(e - a);
                    if (v.vpos < float(a)) v.vpos = float(a);
                    start = a;
                }
                v.tpos = v.pos;
                v.tfrac = v.frac;
                v.tgain = v.hgain;
                v.pos = start;
                v.frac = 0;
                v.hgain = 1;
                v.nextB = boundary(sl, v, start);
                segEnd = v.nextB;
            }
            float sl0 = 0, sr0 = 0;
            if (v.hgain > 0.0f) {
                sl0 = rd(dl, v.pos, v.frac, len) * v.hgain;
                sr0 = rd(dr, v.pos, v.frac, len) * v.hgain;
                v.frac += v.inc;
                const uint32_t k = uint32_t(v.frac);
                v.frac -= float(k);
                v.pos += k;
                if (v.pos + 1 >= len) v.pos = len - 2;
                if (v.pos >= segEnd) v.hgain -= 1.0f / 64.0f;
            }
            if (v.tgain > 0.0f) {
                sl0 += rd(dl, v.tpos, v.tfrac, len) * v.tgain;
                sr0 += rd(dr, v.tpos, v.tfrac, len) * v.tgain;
                v.tfrac += v.inc;
                const uint32_t k = uint32_t(v.tfrac);
                v.tfrac -= float(k);
                v.tpos += k;
                if (v.tpos + 1 >= len) v.tpos = len - 2;
                v.tgain -= 1.0f / 64.0f;
            }
            const float ge = g * env;
            al[i] += sl0 * ge;
            ar[i] += sr0 * ge;
        }
        v.env = env;
        return true;
    }
    // Texture: two sin^2 grains of 2048 frames at 50 % overlap, each started
    // at the virtual position and read at the pitch rate.
    bool renderTexture(V& v, const int16_t* dl, const int16_t* dr, float* al, float* ar, float ratio) {
        constexpr int32_t kG = 2048;
        constexpr float kInvG = 1.0f / float(kG);
        const float g = v.vel;
        float env = v.env;
        const uint32_t e = v.e, a = v.a;
        const uint32_t len = e;   // grains never read past the range end
        const uint32_t i0 = v.wait;
        v.wait = 0;
        for (uint32_t i = i0; i < kBlock; ++i) {
            if (!envStep(v, env)) {
                v.env = 0;
                return false;
            }
            v.vpos += ratio;
            if (v.vpos >= float(e)) {
                if (!v.loop) return false;
                v.vpos -= float(e - a);
            }
            if (--v.gnext <= 0) {
                const int gi = v.gph[0] >= v.gph[1] ? 0 : 1;   // the older grain
                v.gpos[gi] = uint32_t(v.vpos);
                v.gfrac[gi] = 0;
                v.gph[gi] = 0;
                v.gnext = kG / 2;
            }
            float sl0 = 0, sr0 = 0;
            for (int k = 0; k < 2; ++k) {
                if (v.gph[k] >= 1.0f) continue;
                const float s = dx::sin2pi(v.gph[k] * 0.5f);
                const float w = s * s;
                uint32_t p = v.gpos[k];
                if (p + 2 >= len) p = a + (p - a) % ((len - a > 2) ? len - a - 2 : 1);
                sl0 += rd(dl, p, v.gfrac[k], len) * w;
                sr0 += rd(dr, p, v.gfrac[k], len) * w;
                v.gfrac[k] += v.inc;
                const uint32_t n = uint32_t(v.gfrac[k]);
                v.gfrac[k] -= float(n);
                v.gpos[k] = p + n;
                v.gph[k] += kInvG;
            }
            const float ge = g * env;
            al[i] += sl0 * ge;
            ar[i] += sr0 * ge;
        }
        v.env = env;
        return true;
    }
    SamplePool* pool_;
    TrackSample* ts_;
    V v_[4];
    uint32_t age_ = 0;
    dx::Svf fl_, fr_;
    float toneMul_ = 1.0f, bpm_ = 120.0f;
    volatile bool filtDirty_ = true;
};

// =============================================================================
// GRAIN (granular cloud over the track's sample, stereo)
// =============================================================================
enum GrP { gPos, gScan, gSpray, gSize, gDensity, gPitch, gPSpread, gReverse, gWidth, gAttack, gRelease, gGain, gGrN };
const PDesc kGrDesc[gGrN] = {
    {"Position", 0, 1, 0.25f, 0.005f, kFPct, nullptr},
    {"Scan", -1, 1, 0.1f, 0.01f, kFBi, nullptr},
    {"Spray", 0, 1, 0.1f, 0.01f, kFPct, nullptr},
    {"Size", 10, 500, 90, 2, kFMs, nullptr},
    {"Density", 1, 60, 18, 1, kFInt, nullptr},
    {"Pitch", -24, 24, 0, 1, kFSemi, nullptr},
    {"P.Spread", 0, 12, 0, 0.5f, kFNum, nullptr},
    {"Reverse", 0, 1, 0.15f, 0.01f, kFPct, nullptr},
    {"Width", 0, 1, 0.7f, 0.01f, kFPct, nullptr},
    {"Attack", 1, 3000, 40, 5, kFMs, nullptr},
    {"Release", 5, 6000, 600, 10, kFMs, nullptr},
    {"Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kGrSec[3] = {{"Cloud", 0, 5}, {"Pitch/Space", 5, 4}, {"Amp", 9, 3}};

class Grain final : public TableEngine {
public:
    static constexpr int kGrains = 8;
    Grain(SamplePool* pool, TrackSample* ts) : pool_(pool), ts_(ts) { bind(kGrDesc, gGrN, kGrSec, 3); }
    int kind() const override { return kEngGrain; }
    void noteOn(int pitch, float vel, int, int, int) override {
        gate_ = true;
        pitch_ = pitch;
        vel_ = vel;
        scanPos_ = p_[gPos];
        if (env_ <= 0.0f) next_ = 0;
    }
    void noteOff(int pitch) override {
        if (pitch == pitch_) gate_ = false;
    }
    void allOff(bool hard) override {
        gate_ = false;
        if (hard) {
            env_ = 0;
            for (auto& g : g_) g.on = false;
        }
    }
    void render(float* l, float* r) override {
        const int slot = ts_->slot;
        if (!pool_->valid(slot)) {
            for (auto& g : g_) g.on = false;
            env_ = 0;
            return;
        }
        const uint32_t len = pool_->slot(slot).frames;
        const int16_t* dl = pool_->left(slot);
        const int16_t* dr = pool_->right(slot);
        const float blockSec = float(kBlock) * kInvSr;
        if (gate_) env_ += blockSec * 1000.0f / p_[gAttack];
        else env_ -= blockSec * 1000.0f / p_[gRelease];
        env_ = clampf(env_, 0.0f, 1.0f);
        bool any = false;
        for (auto& g : g_) any = any || g.on;
        if (env_ <= 0.0f && !any) return;
        scanPos_ += p_[gScan] * blockSec * (44100.0f / float(len));
        if (scanPos_ >= 1.0f) scanPos_ -= 1.0f;
        if (scanPos_ < 0.0f) scanPos_ += 1.0f;
        if (env_ > 0.0f) {
            next_ -= int32_t(kBlock);
            int guard = 0;
            while (next_ <= 0 && guard++ < 3) {
                spawn(len);
                const float per = kSr / p_[gDensity];
                next_ += int32_t(per * (0.75f + 0.25f * rng_.bi() + 0.25f));
            }
        }
        const float master = p_[gGain] * env_ * vel_ * (1.0f / 32768.0f) * 0.6f;
        for (auto& g : g_) {
            if (!g.on) continue;
            if (g.slot != slot || g.pos + 2 >= len) g.pos = 0;
            g.slot = int16_t(slot);
            uint32_t pos = g.pos;
            float frac = g.frac, ph = g.ph;
            const float inc = g.inc, phInc = g.phInc;
            const float gl = g.gl * master, gr = g.gr * master;
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float w = 4.0f * ph * (1.0f - ph);
                l[i] += rd(dl, pos, frac, len) * w * gl;
                r[i] += rd(dr, pos, frac, len) * w * gr;
                ph += phInc;
                frac += inc;
                const uint32_t k = uint32_t(frac);
                frac -= float(k);
                if (g.rev) pos = pos >= k + 1 ? pos - k : len - 2;
                else {
                    pos += k;
                    if (pos + 1 >= len) pos = 0;
                }
                if (ph >= 1.0f) {
                    g.on = false;
                    break;
                }
            }
            g.pos = pos;
            g.frac = frac;
            g.ph = ph;
        }
    }
    int active() const override {
        int n = 0;
        for (auto& g : g_) n += g.on;
        return n + ((env_ > 0.0f || gate_) ? 1 : 0);
    }
    int32_t cost() const override {
        int n = 0;
        for (auto& g : g_) n += g.on;
        return (env_ > 0.0f || gate_ || n) ? 2500 + n * 4800 : 0;
    }
    int32_t noteCost() const override {
        float ov = p_[gDensity] * p_[gSize] * 0.001f + 1.0f;
        if (ov > kGrains) ov = kGrains;
        return env_ > 0.0f ? 0 : int32_t(2500 + ov * 4800);
    }
    bool steal() override {
        for (auto& g : g_)
            if (g.on) {
                g.on = false;
                return true;
            }
        if (env_ > 0.0f) {
            env_ = 0;
            gate_ = false;
            return true;
        }
        return false;
    }

private:
    void spawn(uint32_t len) {
        G* g = nullptr;
        for (auto& x : g_)
            if (!x.on) {
                g = &x;
                break;
            }
        if (!g) return;
        float p = scanPos_ + p_[gSpray] * rng_.bi() * 0.5f;
        p -= dx::ffloor(p);
        g->pos = uint32_t(p * float(len - 2));
        g->frac = 0;
        const float sp = p_[gPSpread] * rng_.bi();
        g->inc = dx::semis(p_[gPitch] + float(pitch_ - 60) + sp);
        g->phInc = 1.0f / (p_[gSize] * 0.001f * kSr);
        g->ph = 0;
        g->rev = rng_.uni() < p_[gReverse];
        const float pan = rng_.bi() * p_[gWidth];
        g->gl = 0.75f - 0.5f * pan;
        g->gr = 0.75f + 0.5f * pan;
        g->on = true;
    }
    struct G {
        bool on = false, rev = false;
        int16_t slot = -1;
        uint32_t pos = 0;
        float frac = 0, inc = 1, ph = 0, phInc = 0.001f, gl = 0.5f, gr = 0.5f;
    };
    SamplePool* pool_;
    TrackSample* ts_;
    G g_[kGrains];
    dx::Noise rng_;
    bool gate_ = false;
    int pitch_ = 60;
    float vel_ = 1, env_ = 0, scanPos_ = 0.25f;
    int32_t next_ = 0;
};

// =============================================================================
// KIT
// =============================================================================
const char* const kChoke[5] = {"Off", "A", "B", "C", "D"};
const char* const kOnOff[2] = {"Off", "On"};
const PDesc kKitDesc[kpN] = {
    {"Pad", 1, 16, 1, 1, kFInt, nullptr},
    {"Tune", -24, 24, 0, 1, kFSemi, nullptr},
    {"Fine", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Level", 0, 2, 1, 0.01f, kFPct, nullptr},
    {"Pan", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Decay", 0.02f, 1, 1, 0.01f, kFPct, nullptr},
    {"Filter", -1, 1, 0, 0.01f, kFBi, nullptr},
    {"Resonance", 0, 1, 0.15f, 0.01f, kFPct, nullptr},
    {"Choke", 0, 4, 0, 1, kFList, kChoke},
    {"Reverse", 0, 1, 0, 1, kFList, kOnOff},
    {"Voices", 1, 8, 6, 1, kFInt, nullptr},
    {"Vel Sens", 0, 1, 0.7f, 0.01f, kFPct, nullptr},
    {"Kit Gain", 0, 2, 1, 0.01f, kFPct, nullptr},
};
const SecDesc kKitSec[3] = {{"Pad", 0, 6}, {"Pad Tone", 6, 4}, {"Kit", 10, 3}};

class KitEng final : public TableEngine {
public:
    static constexpr int kVoices = 8;
    explicit KitEng(SamplePool* pool) : pool_(pool) {
        bind(kKitDesc, kpN, kKitSec, 3);
        for (int i = 0; i < kKitPads; ++i) std::snprintf(pad_[i].name, sizeof pad_[i].name, "PAD %d", i + 1);
    }
    int kind() const override { return kEngKit; }
    void reset() override {
        TableEngine::reset();
        for (auto& p : pad_) {
            const int16_t s = p.slot;
            const uint32_t a = p.a, e = p.e;
            char nm[sizeof p.name];
            std::memcpy(nm, p.name, sizeof nm);
            p = KitPad();
            p.slot = s;
            p.a = a;
            p.e = e;
            std::memcpy(p.name, nm, sizeof nm);
        }
        load();
    }
    void noteOn(int pitch, float vel, int offset, int tone, int flags) override {
        const int pi = (((pitch - kKitBase) % kKitPads) + kKitPads) % kKitPads;
        const KitPad& pd = pad_[pi];
        last_ = pi;
        if (!pool_->valid(pd.slot)) return;
        const SampleSlot& sl = pool_->slot(pd.slot);
        uint32_t a = pd.a, e = pd.e ? pd.e : sl.frames;
        if (e > sl.frames) e = sl.frames;
        if (a + 32 >= e) return;
        // choke: other pads of the group (and this pad's old voices) fade out
        for (auto& v : v_)
            if (v.on && (v.pad == pi || (pd.choke && pad_[v.pad].choke == pd.choke))) v.fade = true;
        int vi = -1;
        const int maxV = ip(kpVoices);
        for (int i = 0; i < maxV; ++i)
            if (!v_[i].on) {
                vi = i;
                break;
            }
        if (vi < 0) {
            uint32_t best = 0xFFFFFFFFu;
            for (int i = 0; i < maxV; ++i)
                if (v_[i].age < best) {
                    best = v_[i].age;
                    vi = i;
                }
        }
        V& v = v_[vi];
        v = V();
        v.on = true;
        v.pad = uint8_t(pi);
        v.slot = pd.slot;
        v.rev = pd.rev || (flags & kFlagReverse);
        v.a = a;
        v.e = e;
        v.pos = v.rev ? e - 2 : a;
        v.inc = dx::semis(pd.tune + pd.fine + float(tone) * 0.05f);
        const float vs = p_[kpVelSens];
        v.vel = (1.0f - vs) + vs * vel;
        v.vel *= pd.gain;
        v.gl = pd.pan > 0 ? 1.0f - pd.pan : 1.0f;
        v.gr = pd.pan < 0 ? 1.0f + pd.pan : 1.0f;
        v.amp = 1.0f;
        // decay: -60 dB over `decay` of the hit's length (1 = no decay)
        v.decK = pd.decay >= 0.995f ? 1.0f : dx::decayK(float(e - a) * pd.decay * 1000.0f * kInvSr);
        v.wait = uint32_t(offset > 0 && offset < int(kBlock) ? offset : 0);
        v.age = ++age_;
        const float f = pd.filter;
        v.filt = f < -0.02f ? 1 : (f > 0.02f ? 2 : 0);
        if (v.filt == 1) {
            const float hz = 18000.0f * dx::exp2fast(f * 9.0f);
            v.fl.set(hz, resQ(pd.res));
            v.fr.set(hz, resQ(pd.res));
        } else if (v.filt == 2) {
            const float hz = 30.0f * dx::exp2fast(f * 9.0f);
            v.fl.set(hz, resQ(pd.res));
            v.fr.set(hz, resQ(pd.res));
        }
    }
    void noteOff(int) override {}
    void allOff(bool hard) override {
        for (auto& v : v_) {
            if (hard) v.on = false;
            else v.fade = true;
        }
    }
    void render(float* l, float* r) override {
        const float kg = p_[kpKitGain] * (1.0f / 32768.0f);
        for (auto& v : v_) {
            if (!v.on) continue;
            if (!pool_->valid(v.slot) || v.e > pool_->slot(v.slot).frames) {
                v.on = false;
                continue;
            }
            const uint32_t len = pool_->slot(v.slot).frames;
            const int16_t* dl = pool_->left(v.slot);
            const int16_t* dr = pool_->right(v.slot);
            const float gl = v.gl * v.vel * kg, gr = v.gr * v.vel * kg;
            uint32_t pos = v.pos;
            float frac = v.frac, amp = v.amp;
            for (uint32_t i = v.wait; i < kBlock; ++i) {
                float xl = rd(dl, pos, frac, len) * amp, xr = rd(dr, pos, frac, len) * amp;
                if (v.filt == 1) {
                    xl = v.fl.low(xl);
                    xr = v.fr.low(xr);
                } else if (v.filt == 2) {
                    xl = v.fl.high(xl);
                    xr = v.fr.high(xr);
                }
                l[i] += xl * gl;
                r[i] += xr * gr;
                amp *= v.decK;
                if (v.fade) amp -= amp * 0.02f + 0.0005f;
                frac += v.inc;
                const uint32_t k = uint32_t(frac);
                frac -= float(k);
                if (!v.rev) {
                    pos += k;
                    if (pos + 1 >= v.e) {
                        v.on = false;
                        break;
                    }
                } else {
                    if (pos < v.a + k + 1) {
                        v.on = false;
                        break;
                    }
                    pos -= k;
                }
                if (amp < 1e-4f) {
                    v.on = false;
                    break;
                }
            }
            v.wait = 0;
            v.pos = pos;
            v.frac = frac;
            v.amp = amp;
        }
    }
    int active() const override {
        int n = 0;
        for (auto& v : v_) n += v.on;
        return n;
    }
    int32_t cost() const override {
        int32_t c = 0;
        for (auto& v : v_)
            if (v.on) c += v.filt ? 11200 : 7400;   // QEMU: 7.2k, 11k filtered
        return c ? c + 800 : 0;
    }
    int32_t noteCost() const override { return 11200; }
    bool steal() override {
        int vi = -1;
        uint32_t best = 0xFFFFFFFFu;
        for (int i = 0; i < kVoices; ++i)
            if (v_[i].on && v_[i].age < best) {
                best = v_[i].age;
                vi = i;
            }
        if (vi < 0) return false;
        v_[vi].on = false;
        return true;
    }
    KitPad* pads() { return pad_; }
    int lastHit() const { return last_; }
    void select(int p) {
        p_[kpPad] = float((p & 15) + 1);
        load();
    }
    void load() {
        const KitPad& pd = pad_[(ip(kpPad) - 1) & 15];
        p_[kpTune] = pd.tune;
        p_[kpFine] = pd.fine;
        p_[kpGain] = pd.gain;
        p_[kpPan] = pd.pan;
        p_[kpDecay] = pd.decay;
        p_[kpFilter] = pd.filter;
        p_[kpRes] = pd.res;
        p_[kpChoke] = pd.choke;
        p_[kpRev] = pd.rev;
    }

private:
    void changed(int i) override {
        if (i == kpPad) {
            load();
            return;
        }
        KitPad& pd = pad_[(ip(kpPad) - 1) & 15];
        switch (i) {
            case kpTune: pd.tune = p_[i]; break;
            case kpFine: pd.fine = p_[i]; break;
            case kpGain: pd.gain = p_[i]; break;
            case kpPan: pd.pan = p_[i]; break;
            case kpDecay: pd.decay = p_[i]; break;
            case kpFilter: pd.filter = p_[i]; break;
            case kpRes: pd.res = p_[i]; break;
            case kpChoke: pd.choke = uint8_t(ip(i)); break;
            case kpRev: pd.rev = uint8_t(ip(i)); break;
            default: break;
        }
    }
    struct V {
        bool on = false, rev = false, fade = false;
        uint8_t pad = 0, filt = 0;
        int16_t slot = -1;
        uint32_t pos = 0, a = 0, e = 0, age = 0, wait = 0;
        float frac = 0, inc = 1, vel = 1, gl = 1, gr = 1, amp = 1, decK = 1;
        dx::Svf fl, fr;
    };
    SamplePool* pool_;
    KitPad pad_[kKitPads];
    V v_[kVoices];
    uint32_t age_ = 0;
    volatile int last_ = -1;
};

}  // namespace

Engine* makeSampler(Arena& a, SamplePool* pool, TrackSample* ts) {
    void* m = a.alloc(sizeof(Sampler), 16);
    return m ? new (m) Sampler(pool, ts) : nullptr;
}
Engine* makeGrain(Arena& a, SamplePool* pool, TrackSample* ts) {
    void* m = a.alloc(sizeof(Grain), 16);
    return m ? new (m) Grain(pool, ts) : nullptr;
}
Engine* makeKit(Arena& a, SamplePool* pool) {
    void* m = a.alloc(sizeof(KitEng), 16);
    return m ? new (m) KitEng(pool) : nullptr;
}
KitPad* kitPads(Engine* k) { return k && k->kind() == kEngKit ? static_cast<KitEng*>(k)->pads() : nullptr; }
int kitLastHit(const Engine* k) { return k && k->kind() == kEngKit ? static_cast<const KitEng*>(k)->lastHit() : -1; }
void kitSelect(Engine* k, int pad) {
    if (k && k->kind() == kEngKit) static_cast<KitEng*>(k)->select(pad);
}
void kitRefresh(Engine* k) {
    if (k && k->kind() == kEngKit) static_cast<KitEng*>(k)->load();
}

}  // namespace konsol
}  // namespace lab
