// BULUT TAPE FILE host tests (src/ws/stream.*, src/ws/tape.cpp): ring refill,
// underrun fade, loop wrap, varispeed, jump / stop fades, a long WAV feeding a
// channel's buffer like an input (play, stop, HOLD, jump, LOOP off, speed),
// the conversion of a 24-bit 48 kHz file, the project round trip, old
// projects' scenes, and BULUT without a stream reader.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/storage_stream_reader.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "mem_storage.h"
#include "ws/bulut_ws.h"
#include "ws/stream.h"

using namespace bulut;
namespace kit = lab::kit;

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

// The mono value tape `sid` reads at file frame f (FakeStreamReader's data).
int16_t want(uint8_t sid, uint8_t ch, uint32_t f) {
    if (ch == 1) return FakeStreamReader::sample(sid, f);
    return int16_t((int32_t(FakeStreamReader::sample(sid, 2 * f)) + int32_t(FakeStreamReader::sample(sid, 2 * f + 1))) >> 1);
}

// Tapes on FakeStreamReader, without a workstation.
struct TapeRig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(4u * 1024u * 1024u);
    lab::Arena arena;
    Tapes tp;
    FakeStreamReader rd;
    int16_t out[lab::kBlock];
    TapeRig() {
        arena.attach(mem.data(), uint32_t(mem.size()));
        CHECK(tp.init(arena));
        tp.setReader(&rd);
    }
    void open(int c, uint8_t ch, uint32_t frames, uint32_t headFrom) {
        CHECK(tp.bind(c, "fake.wav", 44, ch, frames, nullptr));
        if (headFrom != 0xFFFFFFFFu) CHECK(tp.loadHead(c, headFrom));
    }
    bool block(int c, float speed = 1.0f) { return tp.render(c, speed, out); }
    int service(int max = 8) { return tp.service(nullptr, nullptr, 0, max); }
};

std::vector<uint8_t> wavFile(int ch, uint32_t frames, uint32_t rate = 44100, int bits = 16) {
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

// BULUT with a card and the simulator's stream reader.
struct BRig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    WholeStorage fs;
    ot::StorageStreamReader reader{fs};
    lab::Shell shell;
    bool ok = false;
    lab::Transport tr;
    uint32_t ms = 0;
    float peak = 0.0f;
    explicit BRig(bool withReader = true) {
        lab::Hal h;
        h.fs = &fs;
        if (withReader) h.streamReader = &reader;
        ok = shell.init(h, mem.data(), uint32_t(mem.size()), "BULUT");
        tr.playing = true;
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
    }
    Bulut& ws() { return *static_cast<Bulut*>(shell.workstation()); }
    kit::Registry& P() { return shell.context().params(); }
    // n blocks, the main loop after each (a silent input)
    void run(int n) {
        for (int b = 0; b < n; ++b) {
            int16_t in[lab::kBlock * 2] = {};
            lab::AudioIn ai;
            ai.frames = in;
            float l[lab::kBlock] = {}, r[lab::kBlock] = {};
            shell.kitBlock(tr);
            ws().render(tr, ai, l, r);
            tr.beat += tr.beatsPerSample * lab::kBlock;
            for (uint32_t i = 0; i < lab::kBlock; ++i) peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            ms += 3;
            ws().tick(ms);
        }
    }
};

// Everything quiet except channel `keep`, no drones.
void soloCh(Bulut& b, int keep) {
    for (int c = 0; c < kCh; ++c) {
        b.setParam(c, CP::Level, c == keep ? 0.7f : 0.0f, false);
        b.state(c).drone = 0;
    }
}

}  // namespace

// ---- the rings ---------------------------------------------------------------------
LAB_TEST(tape_ring_refill_reads_ahead_in_order) {
    TapeRig g;
    const uint32_t from = 1000, frames = 44100u * 600u;
    g.open(1, 2, frames, from);
    Tape& t = g.tp.tape(1);
    CHECK(t.bound && t.headOk && t.headBase == from && t.headLen == kHeadFrames);
    g.tp.play(1, from, from, false);
    CHECK(g.tp.requestPending(1));
    // the first block plays the resident head at once (fading in over kStreamFade)
    CHECK(g.block(1));
    CHECK(t.playing && !g.tp.requestPending(1));
    for (uint32_t i = kStreamFade; i < lab::kBlock; ++i) CHECK(g.out[i] == want(1, 2, from + i));
    CHECK(std::abs(int(g.out[0])) <= std::abs(int(want(1, 2, from))) / 16 + 1);
    // the main loop fills from the end of the head; then it is full until the run moves
    CHECK(g.service(64) > 0);
    CHECK(t.fillGen == t.gen && t.base <= from + kHeadFrames && t.base + t.avail >= t.ringFrames() / 2);
    CHECK(g.service(64) == 0);
    // ~3 s through the head / ring border and many ring wraps, in order, at 1 x
    int bad = 0;
    uint32_t pos = from + lab::kBlock;
    for (int b = 1; b < 1100; ++b) {
        if ((b & 3) == 0) g.service();
        CHECK(g.block(1));
        for (uint32_t i = 0; i < lab::kBlock; ++i)
            if (g.out[i] != want(1, 2, pos + i)) {
                if (!bad) std::fprintf(stderr, "    block %d pos %u i %u base %u avail %u\n", b, pos, i, t.base, t.avail);
                ++bad;
            }
        pos += lab::kBlock;
    }
    CHECK(bad == 0);
    CHECK(t.underruns == 0 && g.tp.underruns() == 0);
    CHECK(g.tp.position(1) == pos);
    CHECK(g.rd.reads > 10 && g.tp.stats.readErrors == 0);
    // stopping: one decaying block, then nothing to write (the buffer holds)
    g.tp.stop(1);
    CHECK(g.block(1));
    CHECK(g.out[lab::kBlock - 1] == 0);
    CHECK(!g.block(1));
    CHECK(g.service() == 0);   // a stopped tape reads nothing
}

LAB_TEST(tape_underrun_fades_out_and_back_in) {
    TapeRig g;
    const uint32_t frames = 44100u * 120u;
    g.open(0, 1, frames, 0);
    Tape& t = g.tp.tape(0);
    // a run far past the head: the first block has nothing (the card was not read yet)
    const uint32_t v0 = 1000000;
    g.tp.play(0, v0, 0, false);
    CHECK(g.block(0));
    CHECK(t.underruns == 1);
    for (uint32_t i = 0; i < lab::kBlock; ++i) CHECK(g.out[i] == 0);   // nothing was read before: silence
    // the card catches up: the next frames fade back in over kStreamFade
    g.service();
    CHECK(g.block(0));
    const uint32_t p = v0 + lab::kBlock;
    for (uint32_t i = 0; i < kStreamFade; ++i)
        CHECK(g.out[i] == int16_t(int32_t(want(0, 1, p + i)) * int32_t(i + 1) / int32_t(kStreamFade)));
    for (uint32_t i = kStreamFade; i < lab::kBlock; ++i) CHECK(g.out[i] == want(0, 1, p + i));
    CHECK(t.underruns == 1);
    // the card stops: the ring runs dry, the gap decays linearly from the last frame, never a jump
    uint32_t pos = p + lab::kBlock;
    bool sawGap = false;
    for (int b = 0; b < 1200 && !sawGap; ++b) {   // a mono ring holds 1,024 blocks
        const uint32_t end = t.base + t.avail;   // the first frame not in the ring
        CHECK(g.block(0));
        if (end < pos + lab::kBlock) {
            const uint32_t j = end > pos ? end - pos : 0;
            CHECK(j > 0);
            if (j == 0) break;
            const int32_t last = g.out[j - 1];
            CHECK(last == want(0, 1, pos + j - 1));
            for (uint32_t i = j; i < lab::kBlock; ++i) {
                const uint32_t k = i - j + 1;
                const int32_t exp = k < kStreamFade ? last * int32_t(kStreamFade - k) / int32_t(kStreamFade) : 0;
                CHECK(g.out[i] == exp);
            }
            sawGap = true;
        }
        pos += lab::kBlock;
    }
    if (!(sawGap && t.underruns == 2)) std::fprintf(stderr, "    sawGap %d underruns %u\n", int(sawGap), unsigned(t.underruns));
    CHECK(sawGap && t.underruns == 2);
    // a read error is counted; the tape stays consistent
    g.rd.fail = true;
    CHECK(g.service() == 0);
    CHECK(g.tp.stats.readErrors >= 1);
}

LAB_TEST(tape_loop_wraps_without_waiting) {
    TapeRig g;
    const uint32_t frames = 200000, from = 50000;
    StreamGeom geo;
    geo.a = from;
    geo.e = frames;
    geo.loop = true;
    CHECK(streamFileOf(geo, frames - 1) == frames - 1);
    CHECK(streamFileOf(geo, frames) == from);
    CHECK(streamFileOf(geo, frames + 150000 + 7) == from + 7);
    g.open(2, 2, frames, from);
    Tape& t = g.tp.tape(2);
    g.tp.play(2, frames - 3000, from, true);   // starts near the end: wraps to FROM after 3,000 frames
    CHECK(g.block(2));   // nothing read yet: an underrun
    g.service(64);
    CHECK(g.block(2));   // fades in
    t.underruns = 0;
    // six loops: every frame equals the file through the wrap, no underrun
    uint32_t v = frames - 3000 + 2 * lab::kBlock;
    int bad = 0;
    const int blocks = int(6u * (frames - from) / lab::kBlock);
    for (int b = 0; b < blocks; ++b) {
        if ((b & 3) == 0) g.service();
        CHECK(g.block(2));
        for (uint32_t i = 0; i < lab::kBlock; ++i)
            if (g.out[i] != want(2, 2, streamFileOf(geo, v + i))) ++bad;
        v += lab::kBlock;
    }
    CHECK(bad == 0 && t.underruns == 0);
    CHECK(t.playing && !t.ended);
    // LOOP off: the run ends at the end of the file; the frames past it are silence
    g.tp.play(2, frames - 300, from, false);
    int blocksPlayed = 0;
    for (int b = 0; b < 8 && g.block(2); ++b) {
        ++blocksPlayed;
        g.service();
    }
    CHECK(!t.playing && t.ended);
    CHECK(blocksPlayed == 3);   // 300 frames: blocks 1-2 play, block 3 runs past the end
    CHECK(g.out[lab::kBlock - 1] == 0);
}

LAB_TEST(tape_speed_is_varispeed) {
    TapeRig g;
    const uint32_t frames = 44100u * 300u;
    g.open(3, 1, frames, 0);
    Tape& t = g.tp.tape(3);
    struct Case {
        float speed;
        uint32_t perBlock;   // frames per block after the clamp
    } cases[] = {{2.0f, 256}, {1.5f, 192}, {1.0f, 128}, {0.5f, 64}, {0.25f, 32}, {3.0f, 256}, {0.1f, 32}};
    for (const Case& k : cases) {
        const uint32_t v0 = 100000;
        g.tp.play(3, v0, 0, false);
        g.block(3, k.speed);   // past the head: the first block waits for the card (an underrun)
        g.service(64);
        g.block(3, k.speed);   // fades in
        t.underruns = 0;
        const uint32_t n0 = t.need;
        CHECK(n0 == v0 + 2 * k.perBlock);
        int bad = 0;
        for (int b = 0; b < 100; ++b) {
            const uint32_t start = t.need;
            const float frac = float(t.frac) / 65536.0f;
            if ((b & 3) == 0) g.service();
            CHECK(g.block(3, k.speed));
            // every output sample is the file read at start + frac + speed * i (linear interpolation)
            const float sp = k.speed < kTapeMinSpeed ? kTapeMinSpeed : (k.speed > kTapeMaxSpeed ? kTapeMaxSpeed : k.speed);
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                const float x = frac + sp * float(i);
                const uint32_t f = uint32_t(x);
                const float a = float(want(3, 1, start + f)), bb = float(want(3, 1, start + f + 1));
                const float e = a + (bb - a) * (x - float(f));
                if (std::fabs(float(g.out[i]) - e) > 1.01f) ++bad;
            }
        }
        CHECK(bad == 0);
        // the pitch follows the speed: the position moves speed x the time
        CHECK(t.need == n0 + 100u * k.perBlock);
        CHECK(t.underruns == 0);
    }
    // a fractional speed keeps its fraction across blocks (no drift)
    g.tp.play(3, 0, 0, false);
    for (int b = 0; b < 1000; ++b) {
        if ((b & 3) == 0) g.service();
        g.block(3, 1.37f);
    }
    const double moved = double(t.need) + double(t.frac) / 65536.0;
    CHECK(std::fabs(moved - double(uint32_t(1.37f * 65536.0f + 0.5f)) / 65536.0 * 128.0 * 1000.0) < 1e-3);
    CHECK(std::fabs(moved - 1.37 * 128.0 * 1000.0) < 2.0);   // 16-bit speed: 1.37 x within 1e-5
}

LAB_TEST(tape_jump_and_stop_never_click) {
    TapeRig g;
    const uint32_t frames = 44100u * 60u;
    g.open(0, 2, frames, 0);
    g.tp.play(0, 0, 0, true);
    for (int b = 0; b < 40; ++b) {
        g.service();
        g.block(0);
    }
    // jump far away while playing: the old signal decays under the new one
    const int32_t before = g.out[lab::kBlock - 1];
    g.tp.play(0, 44100u * 30u, 0, true);
    g.service(64);   // (the jump applies at the next block; the service still sees the old run)
    CHECK(g.block(0));
    const int32_t first = g.out[0];
    CHECK(std::abs(first - before) <= std::abs(before) / 16 + 2048);   // no step from the last sample
    // stop: one block decays to zero
    for (int b = 0; b < 10; ++b) {
        g.service();
        g.block(0);
    }
    const int32_t last = g.out[lab::kBlock - 1];
    g.tp.stop(0);
    CHECK(g.block(0));
    CHECK(std::abs(int32_t(g.out[0]) - last) <= std::abs(last) / 16 + 1);
    for (uint32_t i = kStreamFade; i < lab::kBlock; ++i) CHECK(g.out[i] == 0);
    CHECK(!g.block(0));
}

// ---- a long WAV into a channel ----------------------------------------------------------
LAB_TEST(bulut_tape_file_feeds_a_channel_like_an_input) {
    BRig g;
    CHECK(g.ok);
    if (!g.ok) return;
    Bulut& b = g.ws();
    CHECK(b.tapeAvailable());
    g.fs.files["Samples/field.wav"] = wavFile(2, 44100u * 60u);
    soloCh(b, 0);
    CHECK(b.loadTape(0, "Samples/field.wav"));
    CHECK(b.params(0).source == kSrcTape);
    CHECK(b.tapeSlot(0).state == kTapeOpen && b.tapeSlot(0).frames == 44100u * 60u);
    CHECK(b.tapePlaying(0));
    Engine& e = b.engine();
    const uint32_t w0 = e.rt[0].w;
    g.run(345);   // ~1 s
    CHECK(e.rt[0].bufLevel > 0.2f);
    CHECK(e.rt[0].w == ((w0 + 345u * lab::kBlock) & kBufMask));   // written every block, like an input
    CHECK_NEAR(b.tapePos(0), 1.0f / 60.0f, 0.004f);
    const uint32_t u0 = b.tapes()->underruns();
    // the grains play the last seconds of the tape
    g.peak = 0.0f;
    e.padDown(0, 11, 110);
    g.run(345);
    e.padUp(0, 11);
    CHECK(g.peak > 0.05f);
    CHECK(b.tapes()->underruns() == u0);   // the read-ahead keeps up
    // a jump: 50 %
    b.tapeJump(0, 0.5f, true);
    g.run(4);
    CHECK_NEAR(b.tapePos(0), 0.5f, 0.003f);
    // SPEED 2 x: the tape moves twice as fast
    g.P().set(b.tapePid(0, kTpSpeed), 2.0f, false);
    const float p0 = b.tapePos(0);
    g.run(345);
    CHECK_NEAR(b.tapePos(0) - p0, 2.0f / 60.0f, 0.004f);
    // HOLD: the tape runs on, the buffer does not move
    b.state(0).hold = 1;
    uint32_t w1 = e.rt[0].w;
    const float p1 = b.tapePos(0);
    g.run(50);
    CHECK(e.rt[0].w == w1 && b.tapePos(0) > p1);
    b.state(0).hold = 0;
    // stop: the buffer holds, Play continues where it stopped
    b.tapePlay(0, false);
    g.run(3);
    CHECK(!b.tapePlaying(0));
    w1 = e.rt[0].w;
    const float p2 = b.tapePos(0);
    g.run(100);
    CHECK(e.rt[0].w == w1);
    CHECK_NEAR(b.tapePos(0), p2, 1e-6f);
    b.tapePlay(0, true);
    g.run(10);
    CHECK(b.tapePlaying(0) && b.tapePos(0) > p2 && b.tapePos(0) < p2 + 0.01f);
    // LOOP off near the end: it stops at the end, Play starts again at FROM
    g.P().set(b.tapePid(0, kTpSpeed), 1.0f, false);
    g.P().set(b.tapePid(0, kTpLoop), 0.0f, false);
    g.P().set(b.tapePid(0, kTpFrom), 0.25f, false);
    g.run(80);   // FROM applies after a short rest
    CHECK(b.tapeSlot(0).fromFrame == 44100u * 15u);
    CHECK(b.tapeSlot(0).headFrom == b.tapeSlot(0).fromFrame);
    b.tapeJump(0, 0.995f, true);
    g.run(200);   // 0.3 s past the end
    CHECK(!b.tapePlaying(0) && b.tapeSlot(0).ended);
    CHECK_NEAR(b.tapePos(0), 1.0f, 0.001f);
    b.tapePlay(0, true);
    g.run(2);
    CHECK(b.tapePlaying(0));
    CHECK_NEAR(b.tapePos(0), 0.25f, 0.002f);
    // another source: the tape pauses where it is
    b.setSource(0, kSrcInput);
    const float p3 = b.tapePos(0);
    g.run(30);
    CHECK_NEAR(b.tapePos(0), p3, 1e-6f);
    b.setSource(0, kSrcTape);
    g.run(30);
    CHECK(b.tapePos(0) > p3);
}

LAB_TEST(bulut_tape_survives_save_and_load) {
    BRig g;
    if (!g.ok) return;
    Bulut& b = g.ws();
    g.fs.files["Samples/long.wav"] = wavFile(1, 44100u * 20u);
    CHECK(b.loadTape(2, "Samples/long.wav"));
    g.P().set(b.tapePid(2, kTpSpeed), 1.5f, false);
    g.P().set(b.tapePid(2, kTpFrom), 0.25f, false);
    g.P().set(b.tapePid(2, kTpLoop), 0.0f, false);
    g.run(400);
    b.tapeJump(2, 0.6f, true);
    g.run(3);
    b.tapePlay(2, false);
    g.run(2);
    const float pos = b.tapePos(2);
    CHECK(b.saveProjectTo("Lab/BULUT/Projects/T.blt"));
    b.tapePlay(2, true);
    g.run(2);
    // a fresh project forgets the tapes
    b.loadProjectFrom("Lab/BULUT/Projects/T.blt");
    g.run(2);
    CHECK(b.params(2).source == kSrcTape);
    CHECK(std::strcmp(b.tapeSlot(2).path, "Samples/long.wav") == 0);
    CHECK(b.tapeSlot(2).state == kTapeOpen);
    CHECK(!b.tapePlaying(2));
    CHECK_NEAR(b.tapePos(2), pos, 0.001f);
    CHECK_NEAR(g.P().get(b.tapePid(2, kTpSpeed)), 1.5f, 0.01f);
    CHECK_NEAR(g.P().get(b.tapePid(2, kTpFrom)), 0.25f, 0.001f);
    CHECK(g.P().get(b.tapePid(2, kTpLoop)) < 0.5f);
    // a playing tape plays again after loading
    b.tapePlay(2, true);
    CHECK(b.saveProjectTo("Lab/BULUT/Projects/U.blt"));
    b.loadProjectFrom("Lab/BULUT/Projects/T.blt");
    CHECK(!b.tapePlaying(2));
    b.loadProjectFrom("Lab/BULUT/Projects/U.blt");
    g.run(2);
    CHECK(b.tapePlaying(2));
    // a missing tape file: a message, the channel just holds
    g.fs.files.erase("Samples/long.wav");
    b.loadProjectFrom("Lab/BULUT/Projects/U.blt");
    g.run(20);
    CHECK(b.tapeSlot(2).state == kTapeFail && !b.tapePlaying(2));
}

LAB_TEST(bulut_tape_converts_a_24bit_48k_file_once) {
    BRig g;
    if (!g.ok) return;
    Bulut& b = g.ws();
    g.fs.files["Samples/bounce.wav"] = wavFile(2, 48000u * 12u, 48000, 24);
    CHECK(b.loadTape(1, "Samples/bounce.wav"));
    CHECK(b.tapeSlot(1).state == kTapeConv && b.tapeConvProgress(1) >= 0.0f);
    CHECK(b.tapePlaying(1));   // it will play once ready
    for (int i = 0; i < 400 && b.tapeConvProgress(1) >= 0.0f; ++i) g.run(1);
    CHECK(b.tapeConvProgress(1) < 0.0f);
    CHECK(b.tapeSlot(1).state == kTapeOpen);
    CHECK(b.tapeSlot(1).frames > 44100u * 12u - 64u && b.tapeSlot(1).frames < 44100u * 12u + 64u);
    CHECK(std::strstr(b.tapeSlot(1).file, "Lab/BULUT/Stream/bounce_") == b.tapeSlot(1).file);
    // the copy: one 16-bit 44.1 kHz WAV in /Lab/BULUT/Stream, no temporary file left
    int copies = 0, tmps = 0;
    for (auto& kv : g.fs.files) {
        if (kv.first.find("/Stream/") == std::string::npos) continue;
        if (kv.first.find(".tmp") != std::string::npos) ++tmps;
        else ++copies;
    }
    CHECK(copies == 1 && tmps == 0);
    CHECK(b.tapePlaying(1));
    g.run(200);
    CHECK(b.engine().rt[1].bufLevel > 0.2f);
    // loading it again reuses the copy at once
    const size_t files = g.fs.files.size();
    CHECK(b.loadTape(3, "Samples/bounce.wav"));
    CHECK(b.tapeSlot(3).state == kTapeOpen && g.fs.files.size() == files);
    // a short file is refused (load it as a SAMPLE)
    g.fs.files["Samples/short.wav"] = wavFile(1, 44100u * 2u);
    CHECK(!b.loadTape(0, "Samples/short.wav"));
}

LAB_TEST(bulut_tape_without_a_reader_is_not_available) {
    for (int card = 0; card < 2; ++card) {
        // card 1: a card but no stream reader (a PLATFORM station); card 0: no card at all
        lab::Hal h;
        std::vector<uint8_t> mem(64u * 1024u * 1024u);
        MemStorage fs;
        if (card) h.fs = &fs;
        lab::Shell shell;
        CHECK(shell.init(h, mem.data(), uint32_t(mem.size()), "BULUT"));
        Bulut& b = *static_cast<Bulut*>(shell.workstation());
        CHECK(!b.tapeAvailable());
        if (card) fs.files["Samples/long.wav"] = wavFile(1, 44100u * 10u);
        CHECK(!b.loadTape(0, "Samples/long.wav"));
        b.setSource(0, kSrcTape);   // allowed: the page says "not available here"
        CHECK(b.params(0).source == kSrcTape);
        b.tapePlay(0, true);
        CHECK(!b.tapePlaying(0));
        // BULUT works as before: CH4's generator sounds
        lab::Transport tr;
        float pk = 0.0f;
        b.engine().padDown(3, 47, 120);
        for (int k = 0; k < 600; ++k) {
            float l[lab::kBlock] = {}, r[lab::kBlock] = {};
            lab::AudioIn in;
            shell.kitBlock(tr);
            b.render(tr, in, l, r);
            for (uint32_t i = 0; i < lab::kBlock; ++i) pk = std::fmax(pk, std::fabs(l[i]));
            if ((k & 7) == 0) b.tick(uint32_t(k));
        }
        CHECK(pk > 0.01f && std::isfinite(pk));
        CHECK(b.engine().rt[0].bufLevel < 1e-3f);
    }
}

// Projects from before TAPE FILE counted 4 sources in their scenes.
LAB_TEST(bulut_old_project_scenes_keep_their_source) {
    BRig g;
    if (!g.ok) return;
    Bulut& b = g.ws();
    b.setSource(0, kSrcGen);
    b.storeScene(0);
    CHECK(b.saveProjectTo("Lab/BULUT/Projects/OLD.blt"));
    std::vector<uint8_t>& f = g.fs.files["Lab/BULUT/Projects/OLD.blt"];
    CHECK(f.size() > 16 && f[6] == kProjectMinor);
    // make it a minor-0 file: GEN was 3 of 0..3 (normalized 1.0)
    f[6] = 0;
    f[7] = 0;
    bool patched = false;
    for (size_t i = 8; i + 8 < f.size();) {
        uint32_t len = 0;
        std::memcpy(&len, &f[i + 4], 4);
        if (std::memcmp(&f[i], "SCN2", 4) == 0) {
            const size_t at = i + 8 + 4 + size_t(CP::Source) * 4;   // scene index, then norm[0][Source]
            float v = 0.0f;
            std::memcpy(&v, &f[at], 4);
            CHECK_NEAR(v, 0.75f, 1e-4f);   // GEN of 5 sources today
            v = 1.0f;
            std::memcpy(&f[at], &v, 4);
            patched = true;
        }
        i += 8 + len;
    }
    CHECK(patched);
    b.setSource(0, kSrcInput);
    CHECK(b.loadProjectFrom("Lab/BULUT/Projects/OLD.blt"));
    b.setSource(0, kSrcInput);
    b.recallScene(0, false);
    CHECK(b.params(0).source == kSrcGen);
}
