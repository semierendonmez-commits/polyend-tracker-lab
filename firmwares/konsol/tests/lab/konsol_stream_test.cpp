// KONSOL SD streaming host tests (src/ws/stream.*): ring refill, underrun
// fade, loop wrap, the SAMPLER on a streamed file, the conversion of a
// 24-bit 48 kHz file and the project round trip of the Stream switch.
#include <cmath>
#include <cstring>
#include <vector>

#include "../mem_storage.h"
#include "app/storage_stream_reader.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "ws/konsol.h"
#include "ws/stream.h"

using namespace lab;
using namespace lab::konsol;

namespace {

// MemStorage reads at most 1000 bytes a call (it exercises short reads); the
// stream readers expect a card's whole reads, like the simulator's host files.
struct WholeStorage : MemStorage {
    int32_t read(Handle h, void* dst, uint32_t len) override {
        uint32_t got = 0;
        while (got < len) {
            const int32_t k = MemStorage::read(h, static_cast<uint8_t*>(dst) + got, len - got);
            if (k <= 0) break;
            got += uint32_t(k);
        }
        return int32_t(got);
    }
};

// A head of `frames` frames of FakeStreamReader's data (planar, like a pool slot).
struct Head {
    std::vector<int16_t> l, r;
    Head(uint8_t sid, uint8_t ch, uint32_t frames) : l(frames), r(frames) {
        for (uint32_t f = 0; f < frames; ++f) {
            l[f] = FakeStreamReader::sample(sid, f * ch);
            r[f] = FakeStreamReader::sample(sid, f * ch + (ch == 2 ? 1 : 0));
        }
    }
};

struct StreamRig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(8u * 1024u * 1024u);
    Arena arena;
    Streams st;
    FakeStreamReader rd;
    SamplePool pool;   // unused by the rings (service takes it for its signature)
    StreamRig() {
        arena.attach(mem.data(), uint32_t(mem.size()));
        CHECK(st.init(arena));
        st.setReader(&rd);
    }
    int service(int max = 64) { return st.service(pool, nullptr, nullptr, 0, max); }
};

int16_t want(uint8_t sid, uint8_t ch, uint32_t file, int c) { return FakeStreamReader::sample(sid, file * ch + uint32_t(c)); }

std::vector<uint8_t> wav16(int ch, uint32_t frames, uint32_t rate = 44100, int bits = 16) {
    const uint32_t bps = uint32_t(bits / 8);
    std::vector<uint8_t> w(44 + size_t(frames) * bps * uint32_t(ch));
    auto u32 = [&](int at, uint32_t v) { std::memcpy(&w[size_t(at)], &v, 4); };
    auto u16 = [&](int at, uint16_t v) { std::memcpy(&w[size_t(at)], &v, 2); };
    std::memcpy(&w[0], "RIFF", 4);
    u32(4, uint32_t(w.size() - 8));
    std::memcpy(&w[8], "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, uint16_t(ch));
    u32(24, rate);
    u32(28, rate * bps * uint32_t(ch));
    u16(32, uint16_t(bps * uint32_t(ch)));
    u16(34, uint16_t(bits));
    std::memcpy(&w[36], "data", 4);
    u32(40, frames * bps * uint32_t(ch));
    for (uint32_t i = 0; i < frames; ++i) {
        // a sustained tone (no decay): every part of the file is loud
        const float v = std::sin(6.2831853f * 220.0f * float(i) / float(rate)) * 0.6f;
        for (int c = 0; c < ch; ++c) {
            const int32_t s = int32_t(v * (c ? 20000.0f : 26000.0f));
            uint8_t* p = &w[44 + (size_t(i) * uint32_t(ch) + uint32_t(c)) * bps];
            if (bits == 16) {
                const int16_t x = int16_t(s);
                std::memcpy(p, &x, 2);
            } else {   // 24-bit
                const int32_t x = s * 256;
                p[0] = uint8_t(x);
                p[1] = uint8_t(x >> 8);
                p[2] = uint8_t(x >> 16);
            }
        }
    }
    return w;
}

struct KRig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    WholeStorage fs;
    ot::StorageStreamReader reader{fs};   // what the simulator's Shell hands KONSOL
    Shell shell;
    Konsol* k = nullptr;
    Transport tr;
    float l[kBlock], r[kBlock];
    float peak = 0.0f;
    KRig() {
        Hal h;
        h.fs = &fs;
        h.streamReader = &reader;
        shell.init(h, mem.data(), uint32_t(mem.size()), "KONSOL");
        k = static_cast<Konsol*>(shell.workstation());
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
        tr.playing = true;
        Project& p = k->project();
        for (int t = 0; t < kTracks; ++t)
            for (int s = 0; s < kScenes; ++s) p.clip[t][s].lenSteps = 0;
        for (int t = 0; t < kTracks; ++t) k->rt(t) = TrackRt();
    }
    void run(int n) {
        for (int b = 0; b < n; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = 0.0f;
            AudioIn in;
            k->render(tr, in, l, r);
            for (uint32_t i = 0; i < kBlock; ++i) peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            k->update(uint32_t(b * 3));
        }
    }
};

}  // namespace

LAB_TEST(stream_ring_refill_reads_ahead_in_order) {
    StreamRig g;
    const uint8_t sid = 3, ch = 2;
    const uint32_t head = 4096;
    Head hd(sid, ch, head);
    StreamGeom geo;
    geo.a = 0;
    geo.e = 1000000;   // a one-shot far longer than the ring
    uint32_t gen = 0;
    const int ri = g.st.acquire(5, ch, sid, head, geo, &gen);
    CHECK(ri >= 0 && g.st.owns(ri, gen));
    StreamRing& r = g.st.ring(ri);
    // the main loop fills from the end of the head up to the ring's size
    CHECK(g.service() > 0);
    CHECK(r.fillGen == gen && r.base <= head && r.base + r.avail >= r.frames() / 2);
    CHECK(g.service() == 0);   // full: nothing more until the voice moves
    // a window across the head / ring border is exact
    int16_t L[300], R[300];
    CHECK(streamFetch(&r, gen, geo, ch, hd.l.data(), hd.r.data(), head, head - 100, 300, L, R));
    bool same = true;
    for (uint32_t i = 0; i < 300; ++i)
        same = same && L[i] == want(sid, ch, head - 100 + i, 0) && R[i] == want(sid, ch, head - 100 + i, 1);
    CHECK(same);
    // the voice moves on: the ring slides forward, in order, many times over
    uint32_t pos = head;
    int bad = 0;
    for (int step = 0; step < 200; ++step) {
        pos += 3000;
        r.need = pos;
        g.service();
        if (!streamFetch(&r, gen, geo, ch, hd.l.data(), hd.r.data(), head, pos, 256, L, R)) ++bad;
        for (uint32_t i = 0; i < 256; ++i)
            if (L[i] != want(sid, ch, pos + i, 0) || R[i] != want(sid, ch, pos + i, 1)) {
                if (!bad) std::fprintf(stderr, "    step %d pos %u i %u base %u avail %u\n", step, pos, i, r.base, r.avail);
                ++bad;
            }
    }
    CHECK(bad == 0);
    CHECK(r.underruns == 0 && g.st.underruns() == 0);
    CHECK(g.rd.reads > 10);
    // releasing the ring: the service leaves it alone
    g.st.release(ri, gen);
    CHECK(!g.st.owns(ri, gen) && g.st.active() == 0 && g.service() == 0);
}

LAB_TEST(stream_underrun_fades_out_and_back_in) {
    StreamRig g;
    const uint8_t sid = 1, ch = 1;
    const uint32_t head = 2048;
    Head hd(sid, ch, head);
    StreamGeom geo;
    geo.e = 200000;
    uint32_t gen = 0;
    const int ri = g.st.acquire(2, ch, sid, head, geo, &gen);
    StreamRing& r = g.st.ring(ri);
    // nothing read yet: the window runs from the head into missing frames
    int16_t L[200], R[200];
    const uint32_t v0 = head - 40;
    CHECK(!streamFetch(&r, gen, geo, ch, hd.l.data(), hd.r.data(), head, v0, 200, L, R));
    CHECK(r.underruns == 1);
    for (uint32_t i = 0; i < 40; ++i) CHECK(L[i] == hd.l[v0 + i]);   // the head plays as it is
    // the gap: a linear decay from the last frame read, never a jump, then silence
    const int32_t last = hd.l[head - 1];
    for (uint32_t i = 40; i < 40 + kStreamFade; ++i) {
        CHECK(std::abs(int32_t(L[i])) <= std::abs(last));
        CHECK(std::abs(int32_t(L[i])) <= std::abs(int32_t(L[i - 1])) + 1);
    }
    for (uint32_t i = 40 + kStreamFade; i < 200; ++i) CHECK(L[i] == 0);
    // the card catches up: the next frames fade back in over kStreamFade
    g.service();
    CHECK(streamFetch(&r, gen, geo, ch, hd.l.data(), hd.r.data(), head, head + 160, 200, L, R));
    for (uint32_t i = 0; i < kStreamFade; ++i) {
        const int32_t full = want(sid, ch, head + 160 + i, 0);
        CHECK(std::abs(int32_t(L[i])) <= std::abs(full));
        CHECK(L[i] == int16_t(full * int32_t(i + 1) / int32_t(kStreamFade)));
    }
    for (uint32_t i = kStreamFade; i < 200; ++i) CHECK(L[i] == want(sid, ch, head + 160 + i, 0));
    CHECK(r.underruns == 1);
    // a read error: counted, the ring stays consistent and plays what it has
    g.rd.fail = true;
    r.need = head + 200000 / 2;
    g.service();
    CHECK(g.st.stats.readErrors >= 1);
}

LAB_TEST(stream_loop_wraps_without_waiting) {
    StreamRig g;
    const uint8_t sid = 7, ch = 2;
    const uint32_t head = 4096;
    Head hd(sid, ch, head);
    StreamGeom geo;
    geo.a = 1000;              // inside the head
    geo.e = 1000 + 50000;      // the loop end far past it
    geo.loop = true;
    CHECK(streamFileOf(geo, geo.e - 1) == geo.e - 1);
    CHECK(streamFileOf(geo, geo.e) == geo.a);
    CHECK(streamFileOf(geo, geo.e + 50000 + 7) == geo.a + 7);
    uint32_t gen = 0;
    const int ri = g.st.acquire(4, ch, sid, head, geo, &gen);
    StreamRing& r = g.st.ring(ri);
    // play 6 loops in 2,000-frame steps: every window equals the file through the wrap
    int16_t L[2100], R[2100];
    uint32_t v = geo.a;
    int bad = 0, misses = 0;
    for (int step = 0; step < 6 * 26; ++step) {
        r.need = v;
        g.service();
        if (!streamFetch(&r, gen, geo, ch, hd.l.data(), hd.r.data(), head, v, 2000, L, R)) ++misses;
        for (uint32_t i = 0; i < 2000; ++i) {
            const uint32_t f = streamFileOf(geo, v + i);
            if (L[i] != want(sid, ch, f, 0) || R[i] != want(sid, ch, f, 1)) ++bad;
        }
        v += 2000;
    }
    CHECK(v > geo.e + 5 * (geo.e - geo.a));   // several wraps
    CHECK(misses == 0 && bad == 0 && r.underruns == 0);
    // a short loop entirely in the head needs no ring at all
    StreamGeom tiny;
    tiny.a = 100;
    tiny.e = 900;
    tiny.loop = true;
    CHECK(streamFetch(nullptr, 0, tiny, ch, hd.l.data(), hd.r.data(), head, tiny.e - 10, 30, L, R));
    for (uint32_t i = 0; i < 30; ++i) CHECK(L[i] == hd.l[streamFileOf(tiny, tiny.e - 10 + i)]);
    // one-shot end: silence after e, not an underrun
    StreamGeom one;
    one.e = 1000;
    CHECK(streamFetch(nullptr, 0, one, ch, hd.l.data(), hd.r.data(), head, 990, 20, L, R));
    for (uint32_t i = 10; i < 20; ++i) CHECK(L[i] == 0 && R[i] == 0);
}

LAB_TEST(stream_steal_oldest_ring_when_all_busy) {
    StreamRig g;
    StreamGeom geo;
    geo.e = 300000;
    uint32_t gens[kStreamRings + 1];
    int rings[kStreamRings + 1];
    for (int i = 0; i <= kStreamRings; ++i) rings[i] = g.st.acquire(i, 2, uint8_t(i), 1024, geo, &gens[i]);
    CHECK(rings[kStreamRings] == rings[0]);   // the ninth note took the first note's ring
    CHECK(!g.st.owns(rings[0], gens[0]) && g.st.owns(rings[kStreamRings], gens[kStreamRings]));
    CHECK(g.st.active() == kStreamRings);
    // the service never mixes notes: the stolen ring fills for the new note only
    g.service(200);
    StreamRing& r = g.st.ring(rings[0]);
    CHECK(r.fillGen == gens[kStreamRings] && r.sid == uint8_t(kStreamRings));
}

LAB_TEST(stream_sampler_plays_a_long_wav_from_the_card) {
    KRig g;
    Konsol& k = *g.k;
    // 40 s stereo 16-bit 44.1 kHz: Auto streams it as it is (no conversion)
    g.fs.files["Lab/KONSOL/Samples/stem.wav"] = wav16(2, 44100u * 40u);
    CHECK(k.streams() && k.streams()->ready() && k.streams()->reader());
    CHECK(k.loadSample(1, "Lab/KONSOL/Samples/stem.wav"));
    TrackSample& ts = k.trackSample(1);
    SamplePool& pool = k.pool();
    CHECK(pool.streamed(ts.src));
    CHECK(pool.slot(ts.src).frames == kHeadFrames && pool.length(ts.src) == 44100u * 40u);
    CHECK(pool.slot(ts.src).ch == 2);
    // the SAMPLER from 50 % (far past the resident head): it plays from the ring
    k.setEngine(1, kEngSampler);
    Engine* e = k.engine(1);
    e->set(0, 0.0f);    // one-shot
    e->set(1, 0.5f);    // start 50 %
    e->set(5, 1.0f);    // Warp Beats: ignored on a streamed sample
    e->noteOn(60, 1.0f, 0, 0, 0);
    CHECK(k.streams()->active() == 1);
    g.peak = 0.0f;
    g.run(1);           // the first block: the card has not been read yet (fades in)
    g.run(400);         // ~1.2 s: several chunks
    CHECK(g.peak > 0.2f);
    // after the first block the ring stays ahead: no more underruns
    const uint32_t u0 = k.streams()->underruns();
    g.run(600);
    CHECK(k.streams()->underruns() == u0);
    CHECK(e->active() == 1);
    // Off loads it into memory (up to 60 s); the stream file is released
    ts.stream = kStreamOff;
    CHECK(k.loadSample(1, "Lab/KONSOL/Samples/stem.wav"));
    CHECK(pool.valid(ts.src) && !pool.streamed(ts.src) && pool.slot(ts.src).frames == 44100u * 40u);
    g.run(4);
    CHECK(k.streams()->active() == 0);
    // a short file never streams on Auto
    ts.stream = kStreamAuto;
    g.fs.files["Lab/KONSOL/Samples/short.wav"] = wav16(1, 44100u * 5u);
    CHECK(k.loadSample(2, "Lab/KONSOL/Samples/short.wav"));
    CHECK(!pool.streamed(k.trackSample(2).src));
}

LAB_TEST(stream_converts_a_24bit_48k_file_once) {
    KRig g;
    Konsol& k = *g.k;
    g.fs.files["Lab/KONSOL/Samples/bounce.wav"] = wav16(2, 48000u * 32u, 48000, 24);
    CHECK(k.loadSample(3, "Lab/KONSOL/Samples/bounce.wav"));
    CHECK(k.convProgress(3) >= 0.0f);   // queued / converting: the track keeps its sample meanwhile
    for (int i = 0; i < 400 && k.convProgress(3) >= 0.0f; ++i) k.update(uint32_t(i));
    CHECK(k.convProgress(3) < 0.0f);
    const TrackSample& ts = k.trackSample(3);
    SamplePool& pool = k.pool();
    CHECK(pool.streamed(ts.src));
    CHECK(std::strcmp(pool.slot(ts.src).path, "Lab/KONSOL/Samples/bounce.wav") == 0);
    // ~32 s at 44.1 kHz
    CHECK(pool.length(ts.src) > 44100u * 32u - 64u && pool.length(ts.src) < 44100u * 32u + 64u);
    // the copy: one 16-bit 44.1 kHz WAV in <home>/Stream, no temporary file left
    int copies = 0, tmps = 0;
    for (auto& kv : g.fs.files) {
        if (kv.first.find("/Stream/") == std::string::npos) continue;
        if (kv.first.find(".tmp") != std::string::npos) ++tmps;
        else if (kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".wav") == 0) {
            ++copies;
            CHECK(kv.second.size() == 44u + size_t(pool.length(ts.src)) * 4u);
        }
    }
    CHECK(copies == 1 && tmps == 0);
    // its head is the converted signal (not silence)
    int pk = 0;
    for (uint32_t i = 0; i < kHeadFrames; ++i) pk = std::max(pk, std::abs(int(pool.left(ts.src)[i])));
    CHECK(pk > 12000);
    // loading it again (a project open) reuses the copy at once
    const size_t files = g.fs.files.size();
    k.gcSamples();
    CHECK(k.loadSample(4, "Lab/KONSOL/Samples/bounce.wav"));
    CHECK(pool.streamed(k.trackSample(4).src) && k.convProgress(4) < 0.0f);
    CHECK(g.fs.files.size() == files);
}

LAB_TEST(stream_switch_survives_save_and_load) {
    KRig g;
    Konsol& k = *g.k;
    g.fs.files["Lab/KONSOL/Samples/pad.wav"] = wav16(1, 44100u * 12u);
    k.trackSample(5).stream = kStreamOn;   // 12 s streams only when asked
    CHECK(k.loadSample(5, "Lab/KONSOL/Samples/pad.wav"));
    CHECK(k.pool().streamed(k.trackSample(5).src));
    k.trackSample(6).stream = kStreamOff;
    CHECK(k.saveProject("STRM"));
    k.newProject(false);
    CHECK(k.trackSample(5).stream == kStreamAuto);
    CHECK(k.loadProject("STRM"));
    CHECK(k.trackSample(5).stream == kStreamOn && k.trackSample(6).stream == kStreamOff);
    CHECK(k.pool().streamed(k.trackSample(5).src));
    CHECK(k.pool().length(k.trackSample(5).src) == 44100u * 12u);
}
