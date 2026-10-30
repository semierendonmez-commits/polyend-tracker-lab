// KONSOL SD streaming: rings, the audio-side window fetch and the main-loop
// read-ahead (stream.h). Conversion and the readers: stream_io.cpp.
#include "ws/stream.h"

#include <cstring>

namespace lab {
namespace konsol {

namespace {
inline void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline uint32_t minu(uint32_t a, uint32_t b) { return a < b ? a : b; }
}  // namespace

const char* streamModeName(int m) {
    static const char* const k[kStreamModes] = {"Auto", "On", "Off"};
    return k[m >= 0 && m < kStreamModes ? m : 0];
}

bool Streams::init(Arena& a) {
    mem_ = static_cast<int16_t*>(a.alloc(uint32_t(kStreamRings) * kRingSamples * sizeof(int16_t)));
    if (!mem_) return false;
    for (int i = 0; i < kStreamRings; ++i) {
        ring_[i] = StreamRing();
        ring_[i].buf = mem_ + uint32_t(i) * kRingSamples;
    }
    return true;
}

// ---- audio side ---------------------------------------------------------------

int Streams::acquire(int slot, uint8_t ch, uint8_t sid, uint32_t head, const StreamGeom& g, uint32_t* gen) {
    if (!mem_) return -1;
    int best = -1;
    for (int i = 0; i < kStreamRings; ++i)
        if (!ring_[i].busy) {
            best = i;
            break;
        }
    if (best < 0) {   // all busy: the oldest note loses its ring (its voice stops)
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < kStreamRings; ++i)
            if (ring_[i].age < oldest) {
                oldest = ring_[i].age;
                best = i;
            }
    }
    StreamRing& r = ring_[best];
    r.busy = 1;
    r.slot = int8_t(slot);
    r.ch = ch == 2 ? 2 : 1;
    r.sid = sid;
    r.g = g;
    r.head = head;
    r.age = ++age_;
    r.need = g.a;
    r.lastL = r.lastR = 0;
    r.fadeOut = 0;
    r.fadeIn = kStreamFade;
    barrier();
    r.gen = ++gen_;   // last: the main loop sees a complete note
    *gen = r.gen;
    return best;
}

void Streams::release(int r, uint32_t gen) {
    if (r >= 0 && r < kStreamRings && ring_[r].gen == gen) ring_[r].busy = 0;
}

bool streamFetch(StreamRing* ring, uint32_t gen, const StreamGeom& g, uint8_t ch, const int16_t* hl,
                 const int16_t* hr, uint32_t head, uint32_t v0, uint32_t n, int16_t* l, int16_t* r) {
    bool ok = true;
    // the ring's window, sampled once (the main loop cannot run inside the interrupt)
    const bool ringOk = ring && ring->busy && ring->gen == gen && ring->fillGen == gen;
    const uint32_t rb = ringOk ? ring->base : 0, ra = ringOk ? ring->avail : 0;
    const uint32_t rf = ring ? ring->frames() : 1, mask = rf - 1;
    uint32_t fin = ring ? ring->fadeIn : kStreamFade, fout = ring ? ring->fadeOut : 0;
    int32_t lastL = ring ? ring->lastL : 0, lastR = ring ? ring->lastR : 0;
    if (!hr) hr = hl;
    uint32_t i = 0;
    while (i < n) {
        const uint32_t v = v0 + i;
        if (!g.loop && v >= g.e) {   // past a one-shot's end: silence, not an underrun
            for (; i < n; ++i) l[i] = r[i] = 0;
            break;
        }
        const uint32_t f = streamFileOf(g, v);
        uint32_t k = minu(n - i, g.e - f);   // a run never crosses the loop point
        const int16_t* src = nullptr;
        bool inter = false;
        if (f < head) {
            k = minu(k, head - f);
            src = hl + f;
        } else if (ra && v >= rb && v < rb + ra) {
            k = minu(minu(k, rb + ra - v), rf - (v & mask));
            src = ring->buf + (v & mask) * ch;
            inter = ch == 2;
        }
        if (!src) {   // missing: a linear decay from the last frame read, then silence
            ok = false;
            fin = 0;
            if (fout) --fout;
            l[i] = int16_t(lastL * int32_t(fout) / int32_t(kStreamFade));
            r[i] = int16_t(lastR * int32_t(fout) / int32_t(kStreamFade));
            ++i;
            continue;
        }
        const int16_t* srcR = inter ? src + 1 : (f < head ? hr + f : src);
        const uint32_t step = inter ? 2u : 1u;
        if (fin >= kStreamFade) {   // the common case: a plain copy
            if (!inter) {
                std::memcpy(l + i, src, k * 2);
                std::memcpy(r + i, srcR, k * 2);
            } else {
                for (uint32_t j = 0; j < k; ++j) {
                    l[i + j] = src[2 * j];
                    r[i + j] = src[2 * j + 1];
                }
            }
        } else {   // back after an underrun: fade in
            for (uint32_t j = 0; j < k; ++j) {
                if (fin < kStreamFade) ++fin;
                l[i + j] = int16_t(int32_t(src[j * step]) * int32_t(fin) / int32_t(kStreamFade));
                r[i + j] = int16_t(int32_t(srcR[j * step]) * int32_t(fin) / int32_t(kStreamFade));
            }
        }
        lastL = l[i + k - 1];
        lastR = r[i + k - 1];
        fout = kStreamFade;
        i += k;
    }
    if (ring) {
        ring->fadeIn = uint8_t(fin);
        ring->fadeOut = uint8_t(fout);
        ring->lastL = int16_t(lastL);
        ring->lastR = int16_t(lastR);
        if (!ok) ring->underruns = ring->underruns + 1;
    }
    return ok;
}

// ---- main loop ----------------------------------------------------------------

namespace {
// The first virtual frame the ring has to hold for a voice at `pos`: past the
// resident head while the note is still in its first pass through it.
uint32_t coverOf(const StreamRing& r, uint32_t pos) {
    if (pos < r.head && pos < r.g.e) return r.head < r.g.e || r.g.loop ? r.head : r.g.e;
    return pos;
}
// Frames ready ahead of the voice (head included); 0xFFFFFFFF: nothing more to read.
uint32_t leadOf(const StreamRing& r) {
    const uint32_t pos = r.need;
    uint32_t end = coverOf(r, pos);
    if (r.fillGen == r.gen && r.avail && r.base <= end && r.base + r.avail > end) end = r.base + r.avail;
    if (!r.g.loop && end >= r.g.e) return 0xFFFFFFFFu;
    return end > pos ? end - pos : 0;
}
}  // namespace

int Streams::fill(StreamRing& r, const SamplePool& pool) {
    (void)pool;
    if (!r.busy || !reader_) return 0;
    const uint32_t g = r.gen;
    const StreamGeom geo = r.g;
    const uint8_t ch = r.ch, sid = r.sid;
    const uint32_t pos = r.need;
    const uint32_t rf = r.frames(), cf = kChunkSamples / ch;
    const uint32_t cover = coverOf(r, pos);
    const uint32_t keep = pos > kStreamMargin ? pos - kStreamMargin : 0;
    const uint32_t protect = cover > keep ? cover : keep;
    bool reset = r.fillGen != g;
    if (!reset) reset = cover < r.base || cover > r.base + r.avail + cf;   // the voice ran away (underruns)
    if (reset) {
        r.avail = 0;
        barrier();
        r.base = cover - cover % cf;
        r.fillGen = g;
        barrier();
    }
    const uint32_t next = r.base + r.avail;
    if (!geo.loop && next >= geo.e) return 0;                      // the rest is in the ring
    if (next + cf > rf && next + cf - rf > protect) return 0;      // full: the oldest chunk is still needed
    const uint32_t n = geo.loop ? cf : minu(cf, geo.e - next);
    if (r.avail + cf > rf) {   // the chunk reuses the oldest one's slots: shrink the window first
        r.avail = r.avail - cf;
        barrier();
        r.base = r.base + cf;
        barrier();
    }
    int16_t* dst = r.buf + (next & (rf - 1)) * ch;   // chunk-aligned: never wraps inside the ring
    ot::audio::StreamSource src;
    src.instrument = sid;
    uint32_t k = 0;
    while (k < n) {
        const uint32_t f = streamFileOf(geo, next + k);
        const uint32_t run = minu(n - k, geo.e - f);
        if (!reader_->read(src, f * ch, run * ch, dst + k * ch, nullptr)) {
            ++stats.readErrors;
            return -1;
        }
        k += run;
    }
    barrier();
    if (r.gen != g || !r.busy) return 0;   // a new note took the ring meanwhile
    r.avail = r.avail + n;
    ++stats.chunks;
    return 1;
}

int Streams::service(const SamplePool& pool, uint32_t (*millis)(void*), void* ctx, uint32_t budgetMs, int maxChunks) {
    if (!mem_ || !reader_) return 0;
    ++stats.passes;
    const uint32_t t0 = millis ? millis(ctx) : 0;
    uint32_t skip = 0;   // rings done for this pass (full, finished or failed)
    int done = 0;
    while (done < maxChunks) {
        if (millis && done > 0 && millis(ctx) - t0 >= budgetMs) break;
        int best = -1;
        uint32_t bestLead = 0xFFFFFFFFu;
        for (int i = 0; i < kStreamRings; ++i) {
            if (!ring_[i].busy || ((skip >> i) & 1u)) continue;
            const uint32_t lead = leadOf(ring_[i]);
            if (lead == 0xFFFFFFFFu) continue;
            if (lead < stats.minLead) stats.minLead = lead;
            if (best < 0 || lead < bestLead) {
                best = i;
                bestLead = lead;
            }
        }
        if (best < 0) break;
        const uint32_t r0 = millis ? millis(ctx) : 0;
        const int k = fill(ring_[best], pool);
        if (millis) {
            const uint32_t ms = millis(ctx) - r0;
            if (ms > stats.maxReadMs) stats.maxReadMs = ms;
        }
        if (k <= 0) {
            skip |= 1u << best;
            continue;
        }
        done += k;
    }
    return done;
}

void Streams::dropSlot(int s) {
    for (auto& r : ring_)
        if (r.slot == s) {
            r.fillGen = r.fillGen + 0x80000000u;   // never the voice's gen: the window is empty
            r.avail = 0;
        }
}

uint32_t Streams::underruns() const {
    uint32_t u = 0;
    for (const auto& r : ring_) u += r.underruns;
    return u;
}

int Streams::active() const {
    int n = 0;
    for (const auto& r : ring_) n += r.busy ? 1 : 0;
    return n;
}

}  // namespace konsol
}  // namespace lab
