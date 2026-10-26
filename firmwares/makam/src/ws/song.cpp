// MAKAM -- song defaults and the versioned project file (song.h).
#include "ws/song.h"

#include <cstring>

#include "formats/crc32.h"
#include "ws/gen.h"
#include "ws/theory.h"
#include "ws/usul.h"

namespace makam {

const char* const kDelaySyncNames[kDelaySyncCount] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
const float kDelaySyncBeats[kDelaySyncCount] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
const char* const kSectionNames[kSecTypes] = {"Intro", "Build", "Drop", "Break", "Taksim", "Drop 2", "Outro"};

namespace {
// Every persistent int of the song, in file order. New fields go at the end
// (older files load them as defaults).
template <class F>
void visit(Song& s, F&& f) {
    f(s.bpm); f(s.usul); f(s.unit); f(s.swing); f(s.makam); f(s.tuning); f(s.transpose); f(s.kit);
    f(s.breakResample); f(s.songMode); f(s.sectionCount); f(s.curSection); f(s.editVar); f(s.editPhrase);
    for (auto& v : s.brk)
        for (auto& st : v.s) { f(st.stroke); f(st.slice); f(st.roll); f(st.pitch); f(st.rev); f(st.stutter); f(st.choke); }
    for (auto& b : s.bass)
        for (auto& st : b.s) { f(st.deg); f(st.flags); }
    for (auto& p : s.phr) {
        f(p.cycles);
        for (auto& st : p.s) { f(st.deg); f(st.flags); }
    }
    for (auto& sec : s.sec) { f(sec.type); f(sec.cycles); f(sec.brk); f(sec.bass); f(sec.lead); f(sec.chaos); f(sec.filter); }
    for (auto& c : s.ch) { f(c.level); f(c.pan); f(c.dly); f(c.rvb); f(c.mute); }
    f(s.dlyTime); f(s.dlyFb); f(s.rvbSize); f(s.rvbDamp); f(s.duck); f(s.djFilter); f(s.drive); f(s.editDensity);
    f(s.gen.density); f(s.gen.range); f(s.gen.ornament); f(s.gen.start); f(s.gen.live); f(s.gen.seed);
    f(s.bp.model); f(s.bp.octave); f(s.bp.detune); f(s.bp.cutoff); f(s.bp.reso); f(s.bp.env); f(s.bp.decay);
    f(s.bp.drive); f(s.bp.glide);
    f(s.lp.model); f(s.lp.octave); f(s.lp.tone); f(s.lp.decay); f(s.lp.vibrato); f(s.lp.glide);
    f(s.midiBend); f(s.midiOut);
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void sanitize(Song& s) {
    s.bpm = clampi(s.bpm, 60, 220);
    s.usul = clampi(s.usul, 0, kUsulCount - 1);
    s.unit = clampi(s.unit, 0, 1);
    s.swing = clampi(s.swing, 0, 50);
    s.makam = clampi(s.makam, 0, kMakamCount - 1);
    s.tuning = clampi(s.tuning, 0, 1);
    s.transpose = clampi(s.transpose, -12, 12);
    s.kit = clampi(s.kit, 0, kKits);
    s.sectionCount = clampi(s.sectionCount, 1, kSections);
    s.curSection = clampi(s.curSection, 0, s.sectionCount - 1);
    s.editVar = clampi(s.editVar, 0, kVars - 1);
    s.editPhrase = clampi(s.editPhrase, 0, kPhrases - 1);
    for (auto& v : s.brk)
        for (auto& st : v.s) {
            st.stroke = clampi(st.stroke, 0, 3);
            st.slice = clampi(st.slice, -1, kSlices - 1);
            st.roll = clampi(st.roll, 1, 4);
            st.pitch = clampi(st.pitch, -12, 12);
        }
    for (auto& b : s.bass)
        for (auto& st : b.s) st.deg = clampi(st.deg, -7, 14);
    for (auto& p : s.phr) {
        p.cycles = clampi(p.cycles, 1, 4);
        for (auto& st : p.s) st.deg = clampi(st.deg, -7, 21);
    }
    for (auto& sec : s.sec) {
        sec.type = clampi(sec.type, 0, kSecTypes - 1);
        sec.cycles = clampi(sec.cycles, 1, 64);
        sec.brk = clampi(sec.brk, 0, 4);
        sec.bass = clampi(sec.bass, 0, 4);
        sec.lead = clampi(sec.lead, 0, 4);
        sec.chaos = clampi(sec.chaos, 0, 100);
        sec.filter = clampi(sec.filter, -100, 100);
    }
    s.dlyTime = clampi(s.dlyTime, 0, kDelaySyncCount - 1);
    s.gen.range = clampi(s.gen.range, 4, 14);
    s.gen.start = clampi(s.gen.start, 0, 3);
    s.gen.seed = clampi(s.gen.seed, 1, 999);
    s.bp.model = clampi(s.bp.model, 0, 2);
    s.bp.octave = clampi(s.bp.octave, -4, -1);
    s.lp.model = clampi(s.lp.model, 0, 3);
    s.lp.octave = clampi(s.lp.octave, -2, 1);
    s.midiBend = clampi(s.midiBend, 1, 24);
    s.midiOut = clampi(s.midiOut, 0, 1);
    s.wav[sizeof s.wav - 1] = 0;
}

void put16(uint8_t* p, int v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
}
int get16(const uint8_t* p) { return int(int16_t(uint16_t(p[0] | (p[1] << 8)))); }
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

void songApplyUsul(Song& s) {
    for (int v = 0; v < kVars; ++v) genBreakVar(s, v, uint32_t(101 + v * 7));
    for (int v = 0; v < kVars; ++v) genBass(s, v);
}

void songApplyMakam(Song& s) {
    const int saveSeed = s.gen.seed;
    for (int p = 0; p < kPhrases; ++p) {
        GenParams g = s.gen;
        // Four characters: full seyir, sparse taksim, dense run, ornamented.
        if (p == 1) { g.density = 30; g.ornament = 60; }
        if (p == 2) { g.density = 85; g.ornament = 20; }
        if (p == 3) { g.density = 55; g.ornament = 80; g.start = 3; }
        genPhrase(kMakams[s.makam], g, s.usul, s.phr[p], uint32_t(saveSeed * 31 + p * 977));
    }
}

void songDefaults(Song& s) {
    s = Song();
    struct Def { int type, cycles, brk, bass, lead, chaos, filter; };
    const Def d[6] = {
        {kSecIntro, 16, 1, 0, 0, 0, -55},  {kSecBuild, 16, 2, 1, 0, 10, -20}, {kSecDrop, 32, 3, 2, 1, 25, 0},
        {kSecTaksim, 16, 0, 1, 2, 0, 0},   {kSecDrop2, 32, 4, 3, 3, 45, 0},   {kSecOutro, 16, 1, 1, 4, 10, 40},
    };
    for (int i = 0; i < 6; ++i) {
        Section& x = s.sec[i];
        x.type = d[i].type;
        x.cycles = d[i].cycles;
        x.brk = d[i].brk;
        x.bass = d[i].bass;
        x.lead = d[i].lead;
        x.chaos = d[i].chaos;
        x.filter = d[i].filter;
    }
    s.sectionCount = 6;
    s.ch[0] = Channel{85, 0, 0, 8, 0};    // break
    s.ch[1] = Channel{80, 0, 0, 0, 0};    // bass
    s.ch[2] = Channel{62, 0, 30, 35, 0};  // lead
    s.ch[3] = Channel{70, 0, 0, 0, 0};    // FX return
    songApplyUsul(s);
    songApplyMakam(s);
}

uint32_t songCycles(const Song& s) {
    uint32_t n = 0;
    for (int i = 0; i < s.sectionCount && i < kSections; ++i) n += uint32_t(s.sec[i].cycles);
    return n ? n : 1;
}

int songSectionAt(const Song& s, uint32_t cycle, uint32_t* startCycle) {
    const uint32_t total = songCycles(s);
    cycle %= total;
    uint32_t at = 0;
    for (int i = 0; i < s.sectionCount && i < kSections; ++i) {
        const uint32_t len = uint32_t(s.sec[i].cycles);
        if (cycle < at + len) {
            if (startCycle) *startCycle = at;
            return i;
        }
        at += len;
    }
    if (startCycle) *startCycle = 0;
    return 0;
}

size_t songSave(const Song& sIn, uint8_t* out, size_t cap) {
    Song& s = const_cast<Song&>(sIn);   // visit() only reads here
    size_t fields = 0;
    visit(s, [&](int&) { ++fields; });
    const size_t payload = fields * 2 + sizeof s.wav;
    const size_t total = 12 + payload + 4;
    if (cap < total) return 0;
    std::memcpy(out, "MKMJ", 4);
    out[4] = uint8_t(kFileVersion & 0xFF);
    out[5] = uint8_t(kFileVersion >> 8);
    out[6] = out[7] = 0;
    put32(out + 8, uint32_t(payload));
    uint8_t* p = out + 12;
    std::memcpy(p, s.wav, sizeof s.wav);
    p += sizeof s.wav;
    visit(s, [&](int& v) {
        put16(p, v);
        p += 2;
    });
    put32(out + 12 + payload, ot::crc32(out + 12, payload));
    return total;
}

bool songLoad(Song& s, const uint8_t* data, size_t len) {
    if (len < 16 || std::memcmp(data, "MKMJ", 4) != 0) return false;
    const uint16_t ver = uint16_t(data[4] | (data[5] << 8));
    if (ver == 0 || ver > kFileVersion) return false;
    const uint32_t payload = get32(data + 8);
    if (payload < sizeof s.wav || 12 + size_t(payload) + 4 > len) return false;
    if (ot::crc32(data + 12, payload) != get32(data + 12 + payload)) return false;
    Song t;
    songDefaults(t);
    const uint8_t* p = data + 12;
    const uint8_t* end = p + payload;
    std::memcpy(t.wav, p, sizeof t.wav);
    p += sizeof t.wav;
    visit(t, [&](int& v) {
        if (p + 2 <= end) {
            v = get16(p);
            p += 2;
        }
    });
    sanitize(t);
    s = t;
    return true;
}

}  // namespace makam
