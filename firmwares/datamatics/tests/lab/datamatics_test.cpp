// DATAMATICS host tests: reading modes on synthetic files, sample-accurate
// scheduling, section transitions, piece save / load roundtrip.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../mem_storage.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "ws/dm_data.h"
#include "ws/dm_engine.h"
#include "ws/dm_ws.h"

using namespace dm;

namespace {

struct Buf {
    std::vector<uint8_t> v = std::vector<uint8_t>(kMaxValues), a = std::vector<uint8_t>(kMaxValues);
    std::vector<MidiEv> ev = std::vector<MidiEv>(kMaxMidiEvents);
    DecodeOut out() {
        DecodeOut o;
        o.v = v.data();
        o.aux = a.data();
        o.cap = kMaxValues;
        return o;
    }
};

void le16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void le32(std::vector<uint8_t>& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
void be16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void be32(std::vector<uint8_t>& b, uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }

std::vector<uint8_t> makeBmp24(int w, int h) {   // bottom-up; pixel (x, y) grey = x * 16 + y
    std::vector<uint8_t> f = {'B', 'M'};
    const int stride = (w * 3 + 3) & ~3;
    le32(f, 54 + stride * h);
    le32(f, 0);
    le32(f, 54);
    le32(f, 40);
    le32(f, uint32_t(w));
    le32(f, uint32_t(h));
    le16(f, 1);
    le16(f, 24);
    le32(f, 0);
    le32(f, uint32_t(stride * h));
    le32(f, 2835); le32(f, 2835); le32(f, 0); le32(f, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t g = uint8_t(x * 16 + y);
            f.push_back(g); f.push_back(g); f.push_back(g);
        }
        for (int p = w * 3; p < stride; ++p) f.push_back(0);
    }
    return f;
}

std::vector<uint8_t> makeWav(int frames) {   // mono 16-bit: 64 frames loud, 64 silent, ...
    std::vector<uint8_t> f = {'R', 'I', 'F', 'F'};
    le32(f, uint32_t(36 + frames * 2));
    for (char c : std::string("WAVEfmt ")) f.push_back(uint8_t(c));
    le32(f, 16); le16(f, 1); le16(f, 1); le32(f, 44100); le32(f, 88200); le16(f, 2); le16(f, 16);
    for (char c : std::string("data")) f.push_back(uint8_t(c));
    le32(f, uint32_t(frames * 2));
    for (int i = 0; i < frames; ++i) {
        const int16_t s = ((i / 64) % 2 == 0) ? int16_t((i & 1) ? 16384 : -16384) : 0;
        le16(f, uint16_t(s));
    }
    return f;
}

std::vector<uint8_t> makeMidi() {   // type 1, two tracks, 96 ppq
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd'};
    be32(f, 6); be16(f, 1); be16(f, 2); be16(f, 96);
    auto track = [&](std::vector<uint8_t> ev) {
        for (char c : std::string("MTrk")) f.push_back(uint8_t(c));
        be32(f, uint32_t(ev.size()));
        f.insert(f.end(), ev.begin(), ev.end());
    };
    // track 1: note 60 at 0, note 64 at 96 (a quarter later: 3 rests on the 1/16 grid)
    track({0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,   // tempo meta
           0x00, 0x90, 60, 100, 0x30, 0x80, 60, 0, 0x30, 0x90, 64, 50, 0x18, 0x90, 64, 0, 0x00, 0xFF, 0x2F, 0x00});
    // track 2: note 72 at tick 24 (running status), sysex before it
    track({0x00, 0xF0, 0x02, 0x7E, 0xF7, 0x18, 0x90, 72, 127, 0x10, 72, 0, 0x00, 0xFF, 0x2F, 0x00});
    return f;
}

lab::Transport playing(float bpm, double beat = 0.0) {
    lab::Transport t;
    t.playing = true;
    t.bpm = bpm;
    t.beat = beat;
    t.beatsPerSample = double(bpm) / 60.0 / 44100.0;
    return t;
}

// An engine with one free-running click stream on a constant slot.
struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(4u << 20);
    lab::Arena arena;
    Engine e;
    float l[lab::kBlock], r[lab::kBlock];
    float sl[4][lab::kBlock], sr[4][lab::kBlock];
    Rig() {
        arena.attach(mem.data(), uint32_t(mem.size()));
        e.init(arena);
        e.arrange = 0;
        for (int s = 0; s < kStreams; ++s) e.st[s].p[P_MUTE] = 1;
        for (int i = 0; i < kSlots; ++i) {
            std::memset(e.slot[i].v, 255, 1024);
            std::memset(e.slot[i].aux, 255, 1024);
            e.slot[i].n = 1024;
        }
        int* p = e.st[0].p;
        p[P_MUTE] = 0;
        p[P_SLOT] = 0;
        p[P_SOUND] = S_IMPULSE;
        p[P_THR] = 0;
        p[P_LEVEL] = 100;
        p[P_PANMODE] = PM_FIX;
        p[P_PAN] = 0;
        p[P_SYNC] = 0;
        p[P_RATE_MS] = 1;
    }
    // Run `blocks` blocks; return the absolute sample positions of non-zero samples (left).
    std::vector<uint32_t> run(int blocks, float bpm = 120.0f) {
        std::vector<uint32_t> hits;
        lab::Transport t = playing(bpm);
        for (int b = 0; b < blocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            e.render(t, l, r, sl, sr);
            for (uint32_t i = 0; i < lab::kBlock; ++i)
                if (std::fabs(l[i]) > 1e-6f) hits.push_back(uint32_t(b) * lab::kBlock + i);
            t.beat += t.beatsPerSample * lab::kBlock;
        }
        return hits;
    }
};

}  // namespace

// ---- reading modes ----------------------------------------------------------------------
LAB_TEST(dm_detect_modes) {
    const uint8_t riff[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    CHECK(detectMode("a.WAV", nullptr, 0) == kWav);
    CHECK(detectMode("x.bin", riff, 12) == kWav);
    CHECK(detectMode("song.mid", nullptr, 0) == kMidi);
    CHECK(detectMode("pic.bmp", nullptr, 0) == kImage);
    CHECK(detectMode("pic.pgm", nullptr, 0) == kImage);
    CHECK(detectMode("readme.txt", nullptr, 0) == kText);
    const char* txt = "plain words only\n";
    CHECK(detectMode("noext", reinterpret_cast<const uint8_t*>(txt), uint32_t(std::strlen(txt))) == kText);
    const uint8_t bin[4] = {0, 1, 2, 0xFF};
    CHECK(detectMode("blob", bin, 4) == kRaw);
}

LAB_TEST(dm_mode_raw_and_bits) {
    Buf b;
    const uint8_t f[3] = {0xA5, 0x00, 0xFF};
    DecodeOut o = b.out();
    CHECK(decode("x.dat", f, 3, kRaw, o, b.ev.data()));
    CHECK(o.n == 3 && o.mode == kRaw && o.v[0] == 0xA5 && o.v[2] == 0xFF);
    o = b.out();
    CHECK(decode("x.dat", f, 3, kBits, o, b.ev.data()));
    CHECK(o.n == 24 && o.mode == kBits);
    const uint8_t want[8] = {255, 0, 255, 0, 0, 255, 0, 255};   // 0xA5 MSB first
    for (int i = 0; i < 8; ++i) CHECK(o.v[i] == want[i]);
    for (int i = 8; i < 16; ++i) CHECK(o.v[i] == 0);
    for (int i = 16; i < 24; ++i) CHECK(o.v[i] == 255);
}

LAB_TEST(dm_mode_text) {
    Buf b;
    const char* t = "a b\r\n~!";
    DecodeOut o = b.out();
    CHECK(decode("t.txt", reinterpret_cast<const uint8_t*>(t), uint32_t(std::strlen(t)), kAuto, o, b.ev.data()));
    CHECK(o.mode == kText);
    CHECK(o.n == 6);   // \r dropped
    CHECK(o.v[0] > 0 && o.v[1] == 0 && o.v[2] > 0 && o.v[3] == 0);
    CHECK(o.v[4] == 255);   // '~'
    CHECK(o.v[5] == 3);     // '!'
    CHECK(o.aux[0] == 'a');
}

LAB_TEST(dm_mode_image_bmp_and_pnm) {
    Buf b;
    const std::vector<uint8_t> bmp = makeBmp24(5, 3);   // odd width: row padding
    DecodeOut o = b.out();
    CHECK(decode("p.bmp", bmp.data(), uint32_t(bmp.size()), kAuto, o, b.ev.data()));
    CHECK(o.mode == kImage && o.n == 15 && o.width == 5 && o.srcW == 5 && o.srcH == 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) CHECK_NEAR(o.v[y * 5 + x], x * 16 + y, 1);   // top row first
    // binary PGM with a comment
    std::string pgm = "P5\n# c\n4 2\n255\n";
    const uint8_t px[8] = {0, 10, 20, 30, 40, 50, 60, 70};
    std::vector<uint8_t> f(pgm.begin(), pgm.end());
    f.insert(f.end(), px, px + 8);
    o = b.out();
    CHECK(decode("p.pgm", f.data(), uint32_t(f.size()), kAuto, o, b.ev.data()));
    CHECK(o.mode == kImage && o.n == 8 && o.width == 4 && o.v[5] == 50);
    // ASCII PBM: 1 = black
    const char* pbm = "P1 3 1 101";
    o = b.out();
    CHECK(decode("p.pbm", reinterpret_cast<const uint8_t*>(pbm), uint32_t(std::strlen(pbm)), kAuto, o, b.ev.data()));
    CHECK(o.n == 3 && o.v[0] == 0 && o.v[1] == 255 && o.v[2] == 0);
    // a huge image is downsampled to fit
    std::string big = "P5 2048 512 255\n";
    std::vector<uint8_t> g(big.begin(), big.end());
    g.resize(g.size() + 2048u * 512u, 128);
    o = b.out();
    CHECK(decode("big.pgm", g.data(), uint32_t(g.size()), kAuto, o, b.ev.data()));
    CHECK(o.n <= kMaxValues && o.n > kMaxValues / 4 && o.srcW == 2048 && o.v[7] == 128);
    // not an image: falls back to raw
    const uint8_t junk[4] = {1, 2, 3, 4};
    o = b.out();
    CHECK(decode("junk", junk, 4, kImage, o, b.ev.data()));
    CHECK(o.mode == kRaw && o.n == 4);
}

LAB_TEST(dm_mode_wav) {
    Buf b;
    const std::vector<uint8_t> w = makeWav(64 * 8);
    DecodeOut o = b.out();
    CHECK(decode("w.wav", w.data(), uint32_t(w.size()), kAuto, o, b.ev.data()));
    CHECK(o.mode == kWav && o.n == 8);
    CHECK(o.v[0] == 128 && o.v[1] == 0 && o.v[2] == 128);   // peak 16384 -> 128
    CHECK(o.aux[0] == 255 && o.aux[1] == 0);                 // alternating sign: every sample crosses
}

LAB_TEST(dm_mode_midi) {
    Buf b;
    const std::vector<uint8_t> m = makeMidi();
    DecodeOut o = b.out();
    CHECK(decode("m.mid", m.data(), uint32_t(m.size()), kAuto, o, b.ev.data()));
    CHECK(o.mode == kMidi);
    // 60 @0, 72 @24 (no rest: one 1/16 apart), then 64 @96: 3 sixteenths later -> 2 rests
    CHECK(o.n == 5);
    CHECK(o.v[0] == 120 && o.aux[0] == 200);
    CHECK(o.v[1] == 144 && o.aux[1] == 254);
    CHECK(o.v[2] == 0 && o.v[3] == 0);
    CHECK(o.v[4] == 128 && o.aux[4] == 100);
}

LAB_TEST(dm_builtins) {
    Buf b;
    for (int i = 0; i < kBuiltinCount; ++i) {
        DecodeOut o = b.out();
        CHECK(makeBuiltin(i, o) > 1000);
        CHECK(builtinIndex(builtinName(i)) == i);
    }
    DecodeOut o = b.out();
    makeBuiltin(0, o);   // primes
    CHECK(o.v[2] == 255 && o.v[3] == 255 && o.v[4] == 0 && o.v[97] == 255 && o.v[91] == 0);
}

// ---- scheduling ----------------------------------------------------------------------------
LAB_TEST(dm_ms_grid_is_sample_accurate) {
    Rig g;
    g.e.st[0].p[P_RATE_MS] = 10;   // 441 samples
    const std::vector<uint32_t> h = g.run(100);
    CHECK(h.size() >= 28);
    for (size_t i = 0; i < h.size(); ++i) CHECK(h[i] == uint32_t(i) * 441u);
    // impulses are single samples of full scale
    CHECK(h.size() > 1 && h[1] - h[0] == 441);
}

LAB_TEST(dm_beat_grid_exact_over_time) {
    Rig g;
    g.e.st[0].p[P_SYNC] = 1;
    g.e.st[0].p[P_RATE_DIV] = 5;   // 1/16 at 125 BPM: 5292 samples
    const std::vector<uint32_t> h = g.run(3000, 125.0f);
    CHECK(h.size() > 60);
    for (size_t i = 0; i < h.size(); ++i) CHECK(h[i] == uint32_t(i) * 5292u);
    // 1/12 at 120 BPM = 7350 samples exactly
    Rig k;
    k.e.st[0].p[P_SYNC] = 1;
    k.e.st[0].p[P_RATE_DIV] = 6;
    const std::vector<uint32_t> q = k.run(1000, 120.0f);
    for (size_t i = 0; i < q.size(); ++i) CHECK(q[i] == uint32_t(i) * 7350u);
}

LAB_TEST(dm_threshold_and_bits) {
    Rig g;
    // slot 0: bytes 0, 255, 0, 255 ... ; bit unit on 0xF0 = 4 hits 4 rests
    for (int i = 0; i < 1024; ++i) g.e.slot[0].v[i] = (i & 1) ? 255 : 0;
    g.e.st[0].p[P_THR] = 128;
    const std::vector<uint32_t> h = g.run(40);   // 1 ms grid: events at 0, 44.1, 88.2... samples
    for (uint32_t s : h) {
        const uint32_t k = (s * 1000u) / 44100u;   // event k sits at ceil(k * 44.1)
        CHECK(k % 2 == 1);
    }
    Rig b;
    std::memset(b.e.slot[0].v, 0xF0, 1024);
    b.e.st[0].p[P_UNIT] = U_BIT;
    b.e.st[0].p[P_THR] = 128;
    b.e.st[0].p[P_RATE_MS] = 10;
    const std::vector<uint32_t> hb = b.run(55);   // 7040 samples: steps 0..15
    CHECK(hb.size() == 8);
    CHECK(hb[0] == 0 && hb[3] == 3 * 441 && hb[4] == 8 * 441);
}

LAB_TEST(dm_click_duration_and_hard_stop) {
    Rig g;
    int* p = g.e.st[0].p;
    p[P_SOUND] = S_CLICK;
    p[P_DUR] = 10;   // 1.0 ms = 44 samples
    p[P_RATE_MS] = 100;
    const std::vector<uint32_t> h = g.run(10);
    CHECK(h.size() == 44);
    CHECK(h.front() == 0 && h.back() == 43);
    // transport stop silences the next block entirely
    lab::Transport t;   // not playing
    p[P_DUR] = 20000;
    Rig k;
    k.e.st[0].p[P_SOUND] = S_SINE;
    k.e.st[0].p[P_DUR] = 20000;
    k.run(2);
    for (uint32_t i = 0; i < lab::kBlock; ++i) k.l[i] = k.r[i] = 0.0f;
    k.e.render(t, k.l, k.r, k.sl, k.sr);
    float pk = 0.0f;
    for (uint32_t i = 0; i < lab::kBlock; ++i) pk = std::fmax(pk, std::fabs(k.l[i]));
    CHECK(pk == 0.0f);
}

LAB_TEST(dm_pan_and_stems) {
    Rig g;
    int* p = g.e.st[0].p;
    p[P_PAN] = 100;   // hard right
    p[P_STEM] = 2;
    g.run(1);
    float l = 0, r = 0, s2 = 0, s0 = 0;
    for (uint32_t i = 0; i < lab::kBlock; ++i) {
        l += std::fabs(g.l[i]);
        r += std::fabs(g.r[i]);
        s2 += std::fabs(g.sr[2][i]);
        s0 += std::fabs(g.sr[0][i]);
    }
    CHECK(l < 1e-3f && r > 0.5f);
    CHECK(s2 > 0.5f && s0 == 0.0f);
}

// ---- sections -----------------------------------------------------------------------------
LAB_TEST(dm_sections_advance_and_silence) {
    Rig g;
    g.e.arrange = 1;
    g.e.secCount = 3;
    g.e.sec[0] = Section{T_PLAY, 1, 0x01, 50};
    g.e.sec[1] = Section{T_SILENCE, 1, 0xFF, 0};
    g.e.sec[2] = Section{T_PLAY, 1, 0x00, 50};   // stream 0 masked out
    // 120 BPM: a bar = 88200 samples
    const std::vector<uint32_t> h = g.run(int(3 * 88200 / 128) + 4);
    CHECK(!h.empty());
    bool inSilence = false, afterMask = false;
    uint32_t last0 = 0;
    for (uint32_t s : h) {
        if (s >= 88200 && s < 2 * 88200) inSilence = true;
        if (s >= 2 * 88200 && s < 3 * 88200) afterMask = true;   // section 3 (stream masked out)
        if (s < 88200) last0 = s;
    }
    CHECK(!inSilence);
    CHECK(!afterMask);
    CHECK(last0 > 88200 - 50);   // played up to the boundary (1 ms grid)
    // wraps to section 0 (loop) and plays again
    std::vector<uint32_t> more = g.run(int(88200 / 128) + 10);
    CHECK(g.e.curSec == 0 || !more.empty());
}

LAB_TEST(dm_section_boundary_is_sample_exact) {
    Rig g;
    g.e.arrange = 1;
    g.e.secCount = 2;
    g.e.sec[0] = Section{T_SILENCE, 1, 0, 0};
    g.e.sec[1] = Section{T_PLAY, 1, 0x01, 50};
    g.e.st[0].p[P_RATE_MS] = 7;   // 308.7 samples: not aligned to the bar
    const std::vector<uint32_t> h = g.run(int(2 * 88200 / 128) + 2);
    CHECK(!h.empty());
    // first event of section 2 is the first 7 ms grid point at or after 88200
    uint32_t want = 0;
    for (uint32_t k = 0;; ++k) {
        const uint32_t s = uint32_t((uint64_t(k) * 7u * 44100u + 999u) / 1000u);
        if (s >= 88200) { want = s; break; }
    }
    CHECK(h.front() == want);
}

LAB_TEST(dm_densify_and_jump) {
    Rig g;
    g.e.arrange = 1;
    g.e.secCount = 2;
    g.e.sec[0] = Section{T_DENSIFY, 2, 0xFF, 100};
    g.e.sec[1] = Section{T_PLAY, 2, 0xFF, 50};
    for (int s = 0; s < kStreams; ++s) {
        g.e.st[s] = g.e.st[0];
    }
    // densify: at 10 % only stream 0 may sound, at 95 % all
    g.run(int(0.1 * 2 * 88200 / 128));
    int allow = 0;
    for (int s = 0; s < kStreams; ++s) allow += g.e.sectionAllows(s);
    CHECK(allow == 1);
    g.run(int(0.85 * 2 * 88200 / 128));
    allow = 0;
    for (int s = 0; s < kStreams; ++s) allow += g.e.sectionAllows(s);
    CHECK(allow == 8);
    // immediate jump
    g.e.jumpQuant = 1;
    g.e.jumpReq = 1;
    g.run(2);
    CHECK(g.e.curSec == 1 && g.e.jumpReq == -1);
    // end mode stop: the engine asks the transport to stop at the end
    g.e.endMode = 1;
    g.run(int(2 * 88200 / 128) + 4);
    CHECK(g.e.wantStop);
}

// ---- pieces -----------------------------------------------------------------------------
LAB_TEST(dm_piece_roundtrip) {
    Rig a;
    a.e.defaults();
    a.e.st[3].p[P_LO] = 17;
    a.e.st[5].p[P_PAN] = -42;
    a.e.secCount = 3;
    a.e.sec[2] = Section{T_FREEZE, 13, 0xA5, 77};
    a.e.midiMode = 2;
    lab::copyStr(a.e.slot[1].path, sizeof a.e.slot[1].path, "Data/my file (1).bmp");
    a.e.slot[1].wantMode = kBits;
    a.e.slot[3].path[0] = 0;
    static char text[4096];
    const uint32_t n = writePiece(a.e, 133.7f, text, sizeof text);
    CHECK(n > 100);
    Rig b;
    float bpm = 0;
    CHECK(readPiece(b.e, text, n, &bpm));
    CHECK_NEAR(bpm, 133.7, 0.05);
    for (int s = 0; s < kStreams; ++s)
        for (int p = 0; p < P_COUNT; ++p) CHECK(a.e.st[s].p[p] == b.e.st[s].p[p]);
    CHECK(b.e.secCount == 3 && b.e.sec[2].type == T_FREEZE && b.e.sec[2].bars == 13 && b.e.sec[2].mask == 0xA5 &&
          b.e.sec[2].depth == 77);
    CHECK(b.e.midiMode == 2);
    CHECK(!std::strcmp(b.e.slot[1].path, "Data/my file (1).bmp") && b.e.slot[1].wantMode == kBits);
    CHECK(b.e.slot[3].path[0] == 0);
    // rejects other files, clamps out-of-range values
    CHECK(!readPiece(b.e, "hello", 5, &bpm));
    const char* bad = "DATAMATICS 9\nstream 0 7 9 9\nsection 99 500 999 -5\n";
    CHECK(readPiece(b.e, bad, uint32_t(std::strlen(bad)), &bpm));
    CHECK(b.e.st[0].p[P_SLOT] == 3 && b.e.st[0].p[P_UNIT] == 1);
    CHECK(b.e.secCount == 1 && b.e.sec[0].type == T_COUNT - 1 && b.e.sec[0].bars == 64 && b.e.sec[0].depth == 0);
}

LAB_TEST(dm_workstation_files_on_card) {
    MemStorage fs;
    std::vector<uint8_t> f = makeBmp24(16, 8);
    fs.files["Data/pic.bmp"] = f;
    fs.files["Data/notes.txt"] = std::vector<uint8_t>({'h', 'i', ' ', 'x'});
    std::vector<uint8_t> mem(64u << 20);
    lab::Hal h;
    h.fs = &fs;
    lab::Shell sh;
    CHECK(sh.init(h, mem.data(), uint32_t(mem.size()), "DATAMATICS"));
    Datamatics* w = static_cast<Datamatics*>(sh.workstation());
    w->requestLoad(2, "Data/pic.bmp", kAuto);
    w->requestLoad(3, "Data/notes.txt", kBits);
    w->requestLoad(1, "Data/missing.bin", kAuto);
    w->finishLoadsNow();
    Engine& e = w->engine();
    CHECK(e.slot[2].n == 128 && e.slot[2].mode == kImage && e.slot[2].width == 16);
    CHECK(e.slot[3].n == 32 && e.slot[3].mode == kBits);
    CHECK(!std::strcmp(e.slot[1].path, "Data/missing.bin"));
    // save, change, load back (the pic slot keeps its data, paths come back)
    e.st[6].p[P_RATE_MS] = 777;
    CHECK(w->savePiece("Lab/DATAMATICS/Pieces/T.dmx"));
    e.st[6].p[P_RATE_MS] = 5;
    e.slot[2].path[0] = 0;
    CHECK(w->loadPiece("Lab/DATAMATICS/Pieces/T.dmx"));
    w->finishLoadsNow();
    CHECK(e.st[6].p[P_RATE_MS] == 777);
    CHECK(!std::strcmp(e.slot[2].path, "Data/pic.bmp") && e.slot[2].n == 128);
    // power off writes last.dmx
    sh.powerOff();
    CHECK(fs.files.count("Lab/DATAMATICS/last.dmx") == 1);
}
