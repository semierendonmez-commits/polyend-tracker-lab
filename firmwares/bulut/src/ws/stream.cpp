// BULUT TAPE FILE: the rings, the audio-side window fetch and varispeed, the
// main-loop read-ahead (stream.h). Conversion, head reads and the bench
// reader: stream_io.cpp.
#include "ws/stream.h"

#include <cstring>

namespace bulut {

namespace {
inline void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline uint32_t minu(uint32_t a, uint32_t b) { return a < b ? a : b; }
}  // namespace

bool Tapes::init(lab::Arena& a) {
    mem_ = static_cast<int16_t*>(a.alloc(uint32_t(kTapes) * (kRingSamples + kHeadFrames) * sizeof(int16_t)));
    scratch_ = static_cast<int16_t*>(a.alloc(kChunkSamples * sizeof(int16_t)));
    if (!mem_ || !scratch_) {
        mem_ = nullptr;
        return false;
    }
    for (int i = 0; i < kTapes; ++i) {
        tape_[i] = Tape();
        tape_[i].buf = mem_ + uint32_t(i) * kRingSamples;
        tape_[i].head = mem_ + uint32_t(kTapes) * kRingSamples + uint32_t(i) * kHeadFrames;
        tape_[i].sid = uint8_t(i);
    }
    return true;
}

// ---- audio side ---------------------------------------------------------------

bool tapeFetch(Tape& t, uint32_t gen, uint32_t v0, uint32_t n, int16_t* out) {
    bool ok = true;
    // the ring's window and the head, sampled once (the main loop cannot run inside the interrupt)
    const bool ringOk = t.fillGen == gen;
    const uint32_t rb = ringOk ? t.base : 0, ra = ringOk ? t.avail : 0;
    const uint8_t ch = t.ch;
    const uint32_t rf = kRingSamples / ch, mask = rf - 1;
    const bool hOk = t.headOk != 0;
    const uint32_t hb = t.headBase, he = hb + t.headLen;
    const StreamGeom g = t.g;
    uint32_t fin = t.fadeIn, fout = t.fadeOut;
    int32_t last = t.last;
    uint32_t i = 0;
    while (i < n) {
        const uint32_t v = v0 + i;
        if (!g.loop && v >= g.e) {   // past a LOOP-off end: silence, not an underrun
            std::memset(out + i, 0, (n - i) * sizeof(int16_t));
            break;
        }
        const uint32_t f = streamFileOf(g, v);
        uint32_t k = minu(n - i, g.e - f);   // a run never crosses the loop point
        const int16_t* src = nullptr;
        bool ring = false;
        if (hOk && f >= hb && f < he) {
            k = minu(k, he - f);
            src = t.head + (f - hb);
        } else if (ra && v >= rb && v < rb + ra) {
            k = minu(minu(k, rb + ra - v), rf - (v & mask));
            src = t.buf + (v & mask) * ch;
            ring = true;
        }
        if (!src) {   // missing: a linear decay from the last frame read, then silence
            if (ra && v < rb) k = minu(k, rb - v);              // up to where the ring window starts
            if (hOk && f < hb) k = minu(k, hb - f);             // ... or the head
            ok = false;
            fin = 0;
            uint32_t j = 0;
            for (; j < k && fout; ++j) {
                --fout;
                out[i + j] = int16_t(last * int32_t(fout) / int32_t(kStreamFade));
            }
            if (j < k) std::memset(out + i + j, 0, (k - j) * sizeof(int16_t));
            i += k;
            continue;
        }
        int16_t* o = out + i;
        if (!ring || ch == 1) {
            if (fin >= kStreamFade) {
                std::memcpy(o, src, k * sizeof(int16_t));
            } else {
                for (uint32_t j = 0; j < k; ++j) {
                    if (fin < kStreamFade) ++fin;
                    o[j] = int16_t(int32_t(src[j]) * int32_t(fin) / int32_t(kStreamFade));
                }
            }
        } else {   // stereo ring: the mono mix
            if (fin >= kStreamFade) {
                for (uint32_t j = 0; j < k; ++j) o[j] = int16_t((int32_t(src[2 * j]) + int32_t(src[2 * j + 1])) >> 1);
            } else {
                for (uint32_t j = 0; j < k; ++j) {
                    if (fin < kStreamFade) ++fin;
                    const int32_t m = (int32_t(src[2 * j]) + int32_t(src[2 * j + 1])) >> 1;
                    o[j] = int16_t(m * int32_t(fin) / int32_t(kStreamFade));
                }
            }
        }
        last = o[k - 1];
        fout = kStreamFade;
        i += k;
    }
    t.fadeIn = uint8_t(fin);
    t.fadeOut = uint8_t(fout);
    t.last = int16_t(last);
    if (!ok) t.underruns = t.underruns + 1;
    return ok;
}

bool Tapes::render(int c, float speed, int16_t* out) {
    Tape& t = tape_[c];
    if (!mem_ || !t.bound) {
        t.playing = 0;
        t.xFade = 0;
        return false;
    }
    const uint32_t seq = t.reqSeq;
    if (seq != t.seen) {   // a command from the main loop
        t.seen = seq;
        const TapeReq& q = t.req[seq & 1u];
        if (t.playing) {   // the old signal decays under whatever comes next
            t.xLast = t.outLast;
            t.xFade = kStreamFade;
        }
        if (q.play && q.g.e > q.g.a && q.v0 < q.g.e) {
            t.g = q.g;
            t.need = q.v0;
            t.frac = 0;
            t.last = 0;
            t.fadeOut = 0;
            t.fadeIn = 0;   // fade in (from the head as from the ring)
            t.ended = 0;
            barrier();
            t.gen = t.gen + 1;   // the ring's old content is not this run's
            barrier();
            t.playing = 1;
        } else {
            t.playing = 0;
        }
    }
    if (!t.playing) {
        if (!t.xFade) return false;
        // stopped: one last block that decays from the last sample, then the buffer holds
        uint32_t f = t.xFade;
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            if (f) --f;
            out[i] = int16_t(int32_t(t.xLast) * int32_t(f) / int32_t(kStreamFade));
        }
        t.xFade = 0;
        t.outLast = 0;
        return true;
    }
    float sp = speed;
    if (!(sp == sp)) sp = 1.0f;
    sp = sp < kTapeMinSpeed ? kTapeMinSpeed : (sp > kTapeMaxSpeed ? kTapeMaxSpeed : sp);
    // the position in 16.16 fixed point from `need`: the window is frames
    // need .. need + n - 1 (linear interpolation reads one past the last)
    const uint32_t step = uint32_t(sp * 65536.0f + 0.5f);
    static int16_t win[kTapeFetchMax];
    const uint32_t last = t.frac + step * (lab::kBlock - 1);
    const uint32_t n = (last >> 16) + 2u;   // <= kTapeFetchMax (step <= 2.0)
    tapeFetch(t, t.gen, t.need, n, win);
    if (step == 65536u && t.frac == 0) {   // 1 x on a whole frame: the window as it is
        std::memcpy(out, win, lab::kBlock * sizeof(int16_t));
    } else {
        uint32_t x = t.frac;
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            const uint32_t k = x >> 16;
            const int32_t a = win[k];
            out[i] = int16_t(a + (((int32_t(win[k + 1]) - a) * int32_t((x & 0xFFFFu) >> 1)) >> 15));
            x += step;
        }
    }
    const uint32_t end = t.frac + step * lab::kBlock;
    t.frac = end & 0xFFFFu;
    t.need = t.need + (end >> 16);
    if (t.xFade) {   // a jump: the old signal decays under the new one
        uint32_t f = t.xFade;
        const int32_t xl = t.xLast;
        for (uint32_t i = 0; i < lab::kBlock && f; ++i) {
            --f;
            const int32_t v = int32_t(out[i]) + xl * int32_t(f) / int32_t(kStreamFade);
            out[i] = int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        t.xFade = 0;
    }
    t.outLast = out[lab::kBlock - 1];
    if (!t.g.loop && t.need >= t.g.e) {   // the end of a LOOP-off run: the buffer holds from here
        t.playing = 0;
        t.ended = 1;
    }
    return true;
}

// ---- main loop ----------------------------------------------------------------

void Tapes::post(int c, const TapeReq& q) {
    Tape& t = tape_[c];
    const uint32_t s = t.reqSeq + 1u;
    t.req[s & 1u] = q;   // never the slot the published sequence points to
    barrier();
    t.reqSeq = s;
}

void Tapes::play(int c, uint32_t from, uint32_t loopStart, bool loop) {
    const Tape& t = tape_[c];
    TapeReq q;
    q.play = 1;
    q.g.e = t.frames;
    q.g.a = loopStart < t.frames ? loopStart : 0;
    q.g.loop = loop;
    q.v0 = from < t.frames ? from : q.g.a;
    post(c, q);
}

void Tapes::stop(int c) {
    TapeReq q;
    q.play = 0;
    post(c, q);
}

uint32_t Tapes::position(int c) const {
    const Tape& t = tape_[c];
    const StreamGeom g = t.g;
    const uint32_t v = t.need;
    if (g.e <= g.a) return 0;
    const uint32_t f = streamFileOf(g, v);
    return f < t.frames ? f : t.frames;
}

void Tapes::unbind(int c) {
    Tape& t = tape_[c];
    t.bound = 0;   // from here on the audio side never touches the file, the head or the ring
    barrier();
    t.headOk = 0;
    t.avail = 0;
    t.fillGen = t.gen + 0x80000000u;   // never the run's gen: the window is empty
    if (reader_) reader_->unbind(t.sid);
}

namespace {
// The first virtual frame the ring has to hold for a run at `pos`: past the
// resident head while the run is still in its first pass through it.
uint32_t coverOf(const Tape& t, const StreamGeom& g, uint32_t pos) {
    if (t.headOk && pos >= t.headBase && pos < t.headBase + t.headLen && pos < g.e) return t.headBase + t.headLen;
    return pos;
}
}  // namespace

uint32_t Tapes::lead(int c) const {
    const Tape& t = tape_[c];
    const StreamGeom g = t.g;
    const uint32_t pos = t.need;
    uint32_t end = coverOf(t, g, pos);
    if (t.fillGen == t.gen && t.avail && t.base <= end && t.base + t.avail > end) end = t.base + t.avail;
    if (!g.loop && end >= g.e) return 0xFFFFFFFFu;
    return end > pos ? end - pos : 0;
}

int Tapes::fill(Tape& t) {
    if (!t.bound || !t.playing || !reader_) return 0;
    const uint32_t gen = t.gen;
    barrier();
    const StreamGeom geo = t.g;
    const uint8_t ch = t.ch, sid = t.sid;
    const uint32_t pos = t.need;
    const uint32_t rf = t.ringFrames(), cf = kChunkSamples / ch;
    const uint32_t cover = coverOf(t, geo, pos);
    const uint32_t keep = pos > kStreamMargin ? pos - kStreamMargin : 0;
    const uint32_t protect = cover > keep ? cover : keep;
    bool reset = t.fillGen != gen;
    if (!reset) reset = cover < t.base || cover > t.base + t.avail + cf;   // the run ran away (a jump, underruns)
    if (reset) {
        t.avail = 0;
        barrier();
        t.base = cover - cover % cf;
        t.fillGen = gen;
        barrier();
    }
    const uint32_t next = t.base + t.avail;
    if (!geo.loop && next >= geo.e) return 0;                      // the rest is in the ring
    if (next + cf > rf && next + cf - rf > protect) return 0;      // full: the oldest chunk is still needed
    const uint32_t n = geo.loop ? cf : minu(cf, geo.e - next);
    if (t.avail + cf > rf) {   // the chunk reuses the oldest one's slots: shrink the window first
        t.avail = t.avail - cf;
        barrier();
        t.base = t.base + cf;
        barrier();
    }
    int16_t* dst = t.buf + (next & (rf - 1)) * ch;   // chunk-aligned: never wraps inside the ring
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
    if (t.gen != gen || !t.bound) return 0;   // a jump took the ring meanwhile
    t.avail = t.avail + n;
    ++stats.chunks;
    return 1;
}

int Tapes::service(uint32_t (*millis)(void*), void* ctx, uint32_t budgetMs, int maxChunks) {
    if (!mem_ || !reader_) return 0;
    ++stats.passes;
    const uint32_t t0 = millis ? millis(ctx) : 0;
    uint32_t skip = 0;   // tapes done for this pass (full, finished or failed)
    int done = 0;
    while (done < maxChunks) {
        if (millis && done > 0 && millis(ctx) - t0 >= budgetMs) break;
        int best = -1;
        uint32_t bestLead = 0xFFFFFFFFu;
        for (int i = 0; i < kTapes; ++i) {
            const Tape& t = tape_[i];
            if (!t.bound || !t.playing || ((skip >> i) & 1u)) continue;
            const uint32_t ld = lead(i);
            if (ld == 0xFFFFFFFFu) continue;
            if (ld < stats.minLead) stats.minLead = ld;
            if (best < 0 || ld < bestLead) {
                best = i;
                bestLead = ld;
            }
        }
        if (best < 0) break;
        const uint32_t r0 = millis ? millis(ctx) : 0;
        const int k = fill(tape_[best]);
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

uint32_t Tapes::underruns() const {
    uint32_t u = 0;
    for (const auto& t : tape_) u += t.underruns;
    return u;
}

}  // namespace bulut
