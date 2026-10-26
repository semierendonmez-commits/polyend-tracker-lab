// FLIPPER -- model: names, scales, factory machine, the .flp chunk file.
#include "ws/flip_model.h"

#include <cstdio>

#include "hal_api.h"
#include "lab/dsp.h"

namespace flip {

const char* const kElemNames[kElemCount] = {"Empty", "Bumper", "Ramp", "Hole", "Target", "Flipper", "Wall"};
const char* const kElemShort[kElemCount] = {"", "BMP", "RMP", "HOL", "TGT", "FLP", "WAL"};
const char* const kDirNames[9] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW", "Keep"};
const char* const kFxNames[kFxCount] = {"Filter", "Crush", "Stutter", "Delay", "Reverb"};
const char* const kEventNames[kEvCount] = {"None",      "Graph",  "Go to",      "Multiball", "Ball save",
                                           "Multiply", "Mutate", "Extra ball", "Jackpot"};
const char* const kPitchNames[kPmCount] = {"Fixed", "Walk", "Combo", "Ball", "Random"};
const char* const kVelNames[kVmCount] = {"Fixed", "Speed", "Accent", "Random"};
const char* const kActionNames[kActCount] = {"Stay", "Graph", "Go to", "Multiball"};
const char* const kEngineNames[kEnCount] = {"Kick", "Snare", "Hat", "Zap", "Pluck", "Bell", "Lead", "Sampler"};
const char* const kGroupNames[kGroups] = {"Drums", "Bass", "Keys"};
const char* const kRateNames[kRateCount] = {"1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kWallNames[kWallCount] = {"Block", "Slope \\", "Slope /"};
const char* const kScaleNames[kScaleCount] = {"Chromatic", "Major",    "Minor",  "Dorian", "Phrygian", "Lydian",
                                              "Mixolyd.",  "Harm.min", "Pent.maj", "Pent.min", "Blues", "Whole"};
// bit n = semitone n above the root
const uint16_t kScaleMask[kScaleCount] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x5AB, 0xAD5,
                                          0x6B5, 0x9AD, 0x295, 0x4A9, 0x4E9, 0x555};
const uint8_t kLengthBars[kLengthCount] = {0, 4, 8, 16, 32, 64, 128};

int stepsPerBar(int rate) {
    static const uint8_t k[kRateCount] = {4, 8, 16, 32, 12, 24};
    return k[rate < 0 || rate >= kRateCount ? kRate16 : rate];
}
float beatsPerStep(int rate) { return 4.0f / float(stepsPerBar(rate)); }

int scaleNote(int base, int degrees, int root, int scale) {
    if (scale <= 0 || scale >= kScaleCount) return base + degrees;
    const uint16_t mask = kScaleMask[scale];
    // snap base down into the scale
    int n = base;
    for (int i = 0; i < 12; ++i) {
        const int pc = ((n - root) % 12 + 12) % 12;
        if (mask & (1u << pc)) break;
        --n;
    }
    int step = degrees > 0 ? 1 : -1;
    for (int d = degrees; d != 0; d -= step) {
        for (int i = 0; i < 12; ++i) {
            n += step;
            const int pc = ((n - root) % 12 + 12) % 12;
            if (mask & (1u << pc)) break;
        }
    }
    return n;
}

void noteName(int note, char* buf) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    std::snprintf(buf, 6, "%s%d", kNoteNames[note % 12], note / 12 - 1);
}

// ---- factory content ------------------------------------------------------------
namespace {

void setName(char* dst, const char* s) {
    std::memset(dst, 0, kNameLen);
    for (int i = 0; i < kNameLen - 1 && s[i]; ++i) dst[i] = s[i];
}

void inst(Instrument& in, const char* name, uint8_t engine, uint8_t group, uint8_t root, uint8_t decay, uint8_t tone,
          uint8_t a, uint8_t b, uint8_t dly, uint8_t rev, uint8_t level, uint8_t pan, uint8_t color) {
    setName(in.name, name);
    in.engine = engine;
    in.group = group;
    in.root = root;
    in.decay = decay;
    in.tone = tone;
    in.charA = a;
    in.charB = b;
    in.sendDelay = dly;
    in.sendReverb = rev;
    in.level = level;
    in.pan = pan;
    in.color = color;
    in.midi = 1;
}

// Instruments of the factory machine.
enum { kKick, kSnare, kHat, kBass, kPluck, kBell, kZap, kLead };

struct Style {
    const char* name;
    uint8_t color, rate, swing, scale, root, gravity, balls, lenIdx;
    uint8_t bumpers, ramps, holes, targets;
    uint8_t pal[4];        // instruments used by bumpers / targets
    uint8_t lo, hi;        // note range (bumper notes)
    uint8_t pitchMode, velMode;
    uint8_t railKickHits, railKickHeat, railHatHits, railHatHeat, railSnareHeat;
    uint8_t rampFx[2];
    uint8_t graph[kTables];
    uint8_t outlanes;      // drains at col 0 / 11 on the bottom row
};

const Style kStyles[kTables] = {
    // name     col rate    sw scale root grav balls len  bmp rmp hol tgt  palette                    lo  hi  pitch   vel
    {"INTRO", 0, kRate16, 0, 2, 9, 3, 3, 3, 9, 2, 1, 2, {kBell, kPluck, kBell, kHat}, 57, 76, kPmWalk, kVmSpeed,
     4, 0, 4, 2, 3, {kFxReverb, kFxDelay}, {2, 6, 2, 0, 1, 0, 0, 0}, 0},
    {"GROOVE", 1, kRate16, 20, 2, 9, 4, 3, 4, 11, 2, 2, 3, {kSnare, kHat, kBass, kPluck}, 45, 69, kPmFixed,
     kVmAccent, 4, 0, 8, 1, 1, {kFxFilter, kFxDelay}, {0, 2, 5, 2, 0, 1, 0, 0}, 0},
    {"BUILD", 2, kRate16, 10, 2, 9, 5, 3, 3, 12, 3, 1, 3, {kZap, kPluck, kSnare, kBell}, 52, 76, kPmCombo, kVmSpeed,
     4, 0, 16, 0, 1, {kFxFilter, kFxStutter}, {0, 0, 1, 6, 0, 1, 0, 0}, 1},
    {"DROP", 3, kRate16, 0, 7, 9, 6, 4, 4, 14, 2, 2, 2, {kBass, kSnare, kZap, kLead}, 33, 57, kPmBall, kVmAccent,
     5, 0, 8, 0, 0, {kFxCrush, kFxStutter}, {0, 1, 1, 2, 4, 2, 0, 1}, 1},
    {"BREAK", 4, kRate8, 0, 3, 9, 2, 3, 2, 8, 2, 2, 2, {kBell, kLead, kPluck, kZap}, 50, 74, kPmRandom, kVmRandom,
     2, 1, 3, 2, 3, {kFxReverb, kFxFilter}, {1, 2, 0, 3, 0, 0, 2, 0}, 0},
    {"ACID", 5, kRate16, 15, 10, 9, 5, 3, 3, 12, 3, 1, 2, {kLead, kLead, kHat, kSnare}, 33, 52, kPmWalk, kVmAccent,
     4, 0, 12, 0, 1, {kFxFilter, kFxDelay}, {0, 2, 0, 3, 1, 3, 1, 0}, 0},
    {"SPACE", 0, kRate8T, 0, 8, 2, 1, 3, 3, 10, 3, 2, 2, {kBell, kPluck, kZap, kBell}, 60, 84, kPmWalk, kVmSpeed,
     3, 2, 6, 3, 9, {kFxDelay, kFxReverb}, {2, 1, 0, 0, 1, 0, 3, 3}, 1},
    {"OUTRO", 2, kRate16, 10, 2, 9, 3, 2, 3, 8, 2, 1, 2, {kPluck, kBell, kHat, kKick}, 45, 69, kPmFixed, kVmSpeed, 4,
     0, 8, 2, 3, {kFxReverb, kFxFilter}, {6, 1, 0, 0, 0, 0, 1, 0}, 0},
};

}  // namespace

void clearTable(Table& t) {
    char name[kNameLen];
    std::memcpy(name, t.name, kNameLen);
    t = Table();
    std::memcpy(t.name, name, kNameLen);
    for (auto& r : t.rails) r = Rail();
}

void factoryTable(Table& t, int preset) {
    const Style& s = kStyles[preset & 7];
    t = Table();
    setName(t.name, s.name);
    t.enabled = 1;
    t.color = s.color;
    t.stepRate = s.rate;
    t.swing = s.swing;
    t.scale = s.scale;
    t.root = s.root;
    t.gravity = s.gravity;
    t.balls = s.balls;
    t.lengthBars = s.lenIdx;
    t.launchCol = 11;
    t.ballSave = 16;
    t.jackpot = 12;
    t.onJackpot = kActGraph;
    t.onMultiball = kActNone;
    t.onGameOver = kActGraph;
    std::memcpy(t.graph, s.graph, kTables);
    lab::dsp::Rng rng(0xF11B0000u + uint32_t(preset) * 7919u);

    // bottom row: slopes guide the ball to a flipper pair in the middle
    for (int c = 0; c < kCols; ++c) {
        Cell& cl = t.cells[cellAt(c, 3)];
        cl.type = kWall;
        cl.dir = c < 6 ? kWallSlopeR : kWallSlopeL;
        cl.inst = kNone;
    }
    if (s.outlanes) {
        t.cells[cellAt(0, 3)].type = kEmpty;
        t.cells[cellAt(11, 3)].type = kEmpty;
    }
    t.cells[cellAt(11, 3)].type = kEmpty;   // plunger lane
    Cell& fl = t.cells[cellAt(5, 3)];
    Cell& fr = t.cells[cellAt(6, 3)];
    fl = Cell();
    fr = Cell();
    fl.type = fr.type = kFlipper;
    fl.dir = 0;
    fr.dir = 1;
    fl.amount = fr.amount = 6;
    fl.inst = fr.inst = kKick;
    fl.note = fr.note = 36;
    fl.w[0] = fr.w[0] = 5;

    // Markov weights for the open field
    for (int c = 0; c < kCells; ++c) {
        Cell& cl = t.cells[c];
        if (cl.type == kWall || cl.type == kFlipper) continue;
        for (int d = 0; d < 4; ++d) cl.w[d] = uint8_t(1 + rng.range(5));
        if (cellRow(c) == 2 && (cellCol(c) == 5 || cellCol(c) == 6)) cl.w[2] = 6;   // feed the flippers
    }

    auto freeCell = [&](int rowLo, int rowHi) {
        for (int tries = 0; tries < 200; ++tries) {
            const int c = cellAt(rng.range(kCols), rowLo + rng.range(rowHi - rowLo + 1));
            if (t.cells[c].type == kEmpty && cellCol(c) != 11) return c;
        }
        return -1;
    };
    auto scaleN = [&](int lo, int hi) {
        const int n = lo + rng.range(hi - lo + 1);
        return uint8_t(scaleNote(n, 0, s.root, s.scale));
    };
    for (int i = 0; i < s.bumpers; ++i) {
        const int c = freeCell(0, 2);
        if (c < 0) break;
        Cell& cl = t.cells[c];
        cl.type = kBumper;
        cl.inst = s.pal[i & 3];
        const bool drum = cl.inst == kSnare || cl.inst == kHat || cl.inst == kKick;
        cl.note = drum ? uint8_t(cl.inst == kKick ? 36 : (cl.inst == kSnare ? 50 : 66)) : scaleN(s.lo, s.hi);
        if (cl.inst == kBass || cl.inst == kLead) cl.note = scaleN(s.lo, s.lo + 14);
        cl.vel = uint8_t(80 + rng.range(40));
        cl.pitchMode = drum ? uint8_t(kPmFixed) : s.pitchMode;
        cl.velMode = s.velMode;
        cl.points = 10;
        cl.chance = uint8_t(rng.range(4) == 0 ? 70 : 100);
    }
    for (int i = 0; i < s.ramps; ++i) {
        const int c = freeCell(0, 2);
        if (c < 0) break;
        Cell& cl = t.cells[c];
        cl.type = kRamp;
        cl.fx = s.rampFx[i & 1];
        cl.dir = uint8_t(cellCol(c) < 6 ? kNE : kNW);
        cl.amount = uint8_t(55 + rng.range(40));
        cl.len = uint8_t(i & 1 ? 8 : 16);
        cl.groups = cl.fx == kFxStutter || cl.fx == kFxCrush ? 1 : 7;
        cl.inst = kZap;
        cl.note = scaleN(s.lo + 12, s.hi);
        cl.vel = 70;
        cl.points = 25;
    }
    static const uint8_t kHoleEv[4] = {kEvGraph, kEvMultiball, kEvMutate, kEvBallSave};
    for (int i = 0; i < s.holes; ++i) {
        const int c = freeCell(0, 1);
        if (c < 0) break;
        Cell& cl = t.cells[c];
        cl.type = kHole;
        cl.event = kHoleEv[(i + preset) & 3];
        cl.len = 4;
        cl.inst = kBell;
        cl.note = scaleN(s.lo, s.hi);
        cl.vel = 90;
        cl.points = 50;
    }
    static const uint8_t kTgtEv[4] = {kEvMultiplier, kEvNone, kEvExtraBall, kEvNone};
    for (int i = 0; i < s.targets; ++i) {
        const int c = freeCell(0, 2);
        if (c < 0) break;
        Cell& cl = t.cells[c];
        cl.type = kTarget;
        cl.event = kTgtEv[i & 3];
        cl.inst = s.pal[3];
        cl.note = scaleN(s.lo, s.hi);
        cl.vel = 100;
        cl.points = 30;
    }
    // rails: kick / hat / snare backbone, gated by heat
    Rail& rk = t.rails[0];
    rk.inst = kKick;
    rk.note = 36;
    rk.vel = 115;
    rk.hits = s.railKickHits;
    rk.steps = 16;
    rk.heat = s.railKickHeat > 5 ? 5 : s.railKickHeat;
    Rail& rh = t.rails[1];
    rh.inst = kHat;
    rh.note = 66;
    rh.vel = 70;
    rh.hits = s.railHatHits;
    rh.steps = 16;
    rh.rotate = 2;
    rh.heat = s.railHatHeat > 5 ? 5 : s.railHatHeat;
    Rail& rs = t.rails[2];
    rs.inst = kSnare;
    rs.note = 50;
    rs.vel = 100;
    rs.hits = 2;
    rs.steps = 16;
    rs.rotate = 4;
    rs.heat = s.railSnareHeat > 5 ? 5 : s.railSnareHeat;
    if (s.rate == kRate8 || s.rate == kRate8T) {   // fewer steps per bar: shorter rails
        rk.steps = rh.steps = rs.steps = uint8_t(stepsPerBar(s.rate));
        if (rk.hits > rk.steps) rk.hits = rk.steps;
        rs.rotate = uint8_t(rs.steps / 4);
    }
}

void factoryMachine(Machine& m) {
    m.g = Global();
    inst(m.inst[0], "KICK", kEnKick, kGrDrums, 36, 45, 40, 60, 0, 0, 0, 90, 50, 0);
    inst(m.inst[1], "SNARE", kEnSnare, kGrDrums, 50, 35, 60, 55, 0, 5, 18, 72, 52, 1);
    inst(m.inst[2], "HAT", kEnHat, kGrDrums, 66, 12, 70, 40, 0, 0, 5, 55, 60, 2);
    inst(m.inst[3], "BASS", kEnLead, kGrBass, 36, 35, 35, 70, 20, 0, 0, 75, 50, 3);
    inst(m.inst[4], "PLUCK", kEnPluck, kGrKeys, 60, 55, 60, 40, 0, 25, 20, 65, 35, 4);
    inst(m.inst[5], "BELL", kEnBell, kGrKeys, 60, 60, 50, 40, 45, 30, 30, 55, 65, 5);
    inst(m.inst[6], "ZAP", kEnZap, kGrDrums, 60, 25, 50, 60, 0, 15, 15, 55, 45, 6);
    inst(m.inst[7], "LEAD", kEnLead, kGrKeys, 60, 30, 65, 50, 40, 20, 15, 55, 50, 7);
    for (int i = 0; i < kTables; ++i) factoryTable(m.tab[i], i);
    std::memset(m.sample, 0, sizeof m.sample);
    m.clipLen = 0;
}

// ---- sanitize ---------------------------------------------------------------------
namespace {
template <class T>
void clampv(T& v, int lo, int hi) {
    if (int(v) < lo) v = T(lo);
    if (int(v) > hi) v = T(hi);
}
void fixName(char* s) {
    s[kNameLen - 1] = 0;
    for (int i = 0; i < kNameLen - 1 && s[i]; ++i)
        if (s[i] < 32 || s[i] > 126) s[i] = '?';
}
}  // namespace

void sanitize(Machine& m) {
    Global& g = m.g;
    clampv(g.mode, 0, 1);
    clampv(g.autoOn, 0, 1);
    clampv(g.autoAfter, 1, 10);
    clampv(g.autoSkill, 0, 100);
    clampv(g.follow, 0, 100);
    clampv(g.ghost, 0, 1);
    clampv(g.quant, 0, 2);
    clampv(g.midiNotes, 0, 1);
    clampv(g.midiIn, 0, 1);
    clampv(g.trails, 0, 1);
    clampv(g.startTable, 0, kTables - 1);
    clampv(g.ledLevel, 0, 2);
    for (auto& in : m.inst) {
        fixName(in.name);
        clampv(in.engine, 0, kEnCount - 1);
        clampv(in.level, 0, 100);
        clampv(in.pan, 0, 100);
        clampv(in.decay, 0, 100);
        clampv(in.tone, 0, 100);
        clampv(in.charA, 0, 100);
        clampv(in.charB, 0, 100);
        clampv(in.sendDelay, 0, 100);
        clampv(in.sendReverb, 0, 100);
        clampv(in.group, 0, kGroups - 1);
        clampv(in.midi, 0, 17);
        clampv(in.root, 0, 127);
        clampv(in.color, 0, 7);
    }
    bool any = false;
    for (auto& t : m.tab) {
        fixName(t.name);
        clampv(t.enabled, 0, 1);
        clampv(t.color, 0, 5);
        clampv(t.stepRate, 0, kRateCount - 1);
        clampv(t.swing, 0, 75);
        clampv(t.scale, 0, kScaleCount - 1);
        clampv(t.root, 0, 11);
        clampv(t.gravity, 0, 9);
        clampv(t.balls, 1, 10);
        clampv(t.lengthBars, 0, kLengthCount - 1);
        clampv(t.sides, 0, 1);
        clampv(t.launchCol, 0, kCols - 1);
        clampv(t.ballSave, 0, 64);
        clampv(t.onJackpot, 0, kActCount - 1);
        clampv(t.onMultiball, 0, kActCount - 1);
        clampv(t.onGameOver, 0, kActCount - 1);
        clampv(t.jackpotArg, 0, kTables - 1);
        clampv(t.multiballArg, 0, kTables - 1);
        clampv(t.gameOverArg, 0, kTables - 1);
        for (auto& w : t.graph) clampv(w, 0, 9);
        for (auto& r : t.rails) {
            if (r.inst != kNone) clampv(r.inst, 0, kInsts - 1);
            clampv(r.note, 0, 127);
            clampv(r.vel, 1, 127);
            clampv(r.steps, 1, 16);
            clampv(r.hits, 0, 16);
            clampv(r.rotate, 0, 15);
            clampv(r.heat, 0, 5);   // 5 = off
        }
        for (auto& c : t.cells) {
            clampv(c.type, 0, kElemCount - 1);
            for (auto& w : c.w) clampv(w, 0, 9);
            if (c.inst != kNone) clampv(c.inst, 0, kInsts - 1);
            clampv(c.note, 0, 127);
            clampv(c.vel, 1, 127);
            clampv(c.pitchMode, 0, kPmCount - 1);
            clampv(c.velMode, 0, kVmCount - 1);
            clampv(c.dir, 0, 8);
            clampv(c.fx, 0, kFxCount - 1);
            clampv(c.amount, 0, 100);
            clampv(c.len, 1, 64);
            clampv(c.event, 0, kEvCount - 1);
            clampv(c.arg, 0, kTables - 1);
            clampv(c.chance, 0, 100);
            clampv(c.groups, 0, 7);
            if (c.type == kFlipper) {
                clampv(c.dir, 0, 1);
                clampv(c.amount, 1, 9);
            }
            if (c.type == kWall) clampv(c.dir, 0, kWallCount - 1);
        }
        any = any || t.enabled;
    }
    if (!any) m.tab[0].enabled = 1;
    for (auto& s : m.sample) {
        s[kPathLen - 1] = 0;
        for (int i = 0; i < kPathLen && s[i]; ++i)
            if (s[i] < 32 || s[i] > 126) {
                s[0] = 0;
                break;
            }
    }
    if (m.clipLen > uint32_t(kClipMax)) m.clipLen = kClipMax;
    for (uint32_t i = 0; i < m.clipLen; ++i) {
        ClipStep& st = m.clip[i];
        clampv(st.table, 0, kTables - 1);
        clampv(st.heat, 0, 4);
        if (st.fxCell != kNone && st.fxCell >= kCells) st.fxCell = kNone;
        for (int b = 0; b < kMaxBalls; ++b) {
            if (st.cell[b] != kNone && st.cell[b] >= kCells) st.cell[b] = kNone;
            if (st.inst[b] != kNone && st.inst[b] >= kInsts) st.inst[b] = kNone;
            clampv(st.note[b], 0, 127);
            clampv(st.vel[b], 0, 127);
        }
    }
}

// ---- file -------------------------------------------------------------------------
namespace {
constexpr uint32_t tag(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
           (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t kMagic = tag('F', 'L', 'P', 'R');
constexpr uint32_t kGlob = tag('G', 'L', 'O', 'B'), kInst = tag('I', 'N', 'S', 'T'), kSmpl = tag('S', 'M', 'P', 'L'),
                   kTabl = tag('T', 'A', 'B', 'L'), kClip = tag('C', 'L', 'I', 'P'), kEnd = tag('E', 'N', 'D', ' ');

struct Writer {
    ot::IStorage& fs;
    ot::IStorage::Handle h;
    bool ok = true;
    void raw(const void* p, uint32_t n) {
        if (ok && n && fs.write(h, p, n) != int32_t(n)) ok = false;
    }
    void u32(uint32_t v) { raw(&v, 4); }
    void chunk(uint32_t t, uint8_t index, const void* p, uint32_t n) {
        u32(t);
        u32(n + 1);
        raw(&index, 1);
        raw(p, n);
    }
};

bool readAll(ot::IStorage& fs, ot::IStorage::Handle h, void* dst, uint32_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n) {
        const int32_t got = fs.read(h, d, n);
        if (got <= 0) return false;
        d += got;
        n -= uint32_t(got);
    }
    return true;
}
bool skip(ot::IStorage& fs, ot::IStorage::Handle h, uint32_t n) {
    uint8_t buf[64];
    while (n) {
        const uint32_t k = n < sizeof buf ? n : uint32_t(sizeof buf);
        if (!readAll(fs, h, buf, k)) return false;
        n -= k;
    }
    return true;
}
}  // namespace

bool saveMachine(ot::IStorage& fs, const char* path, const Machine& m) {
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kWrite);
    if (h < 0) return false;
    Writer w{fs, h};
    w.u32(kMagic);
    const uint32_t ver = kFileVersion;
    w.u32(ver);
    w.chunk(kGlob, 0, &m.g, sizeof m.g);
    for (int i = 0; i < kInsts; ++i) w.chunk(kInst, uint8_t(i), &m.inst[i], sizeof m.inst[i]);
    for (int i = 0; i < kInsts; ++i)
        if (m.sample[i][0]) w.chunk(kSmpl, uint8_t(i), m.sample[i], kPathLen);
    for (int i = 0; i < kTables; ++i) w.chunk(kTabl, uint8_t(i), &m.tab[i], sizeof m.tab[i]);
    if (m.clipLen) {
        w.u32(kClip);
        w.u32(1 + 4 + m.clipLen * uint32_t(sizeof(ClipStep)));
        const uint8_t idx = 0;
        w.raw(&idx, 1);
        w.u32(m.clipLen);
        w.raw(m.clip, m.clipLen * uint32_t(sizeof(ClipStep)));
    }
    w.u32(kEnd);
    w.u32(0);
    fs.close(h);
    return w.ok;
}

bool loadMachine(ot::IStorage& fs, const char* path, Machine& m) {
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return false;
    uint32_t hdr[2] = {};
    if (!readAll(fs, h, hdr, 8) || hdr[0] != kMagic || hdr[1] == 0) {
        fs.close(h);
        return false;
    }
    bool sawTable = false;
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t ch[2];
        if (!readAll(fs, h, ch, 8)) break;
        const uint32_t t = ch[0], len = ch[1];
        if (t == kEnd) break;
        if (len == 0) continue;
        uint8_t idx = 0;
        if (!readAll(fs, h, &idx, 1)) break;
        uint32_t left = len - 1;
        auto take = [&](void* dst, uint32_t size) {
            const uint32_t n = left < size ? left : size;
            if (n && !readAll(fs, h, dst, n)) return false;
            left -= n;
            return true;
        };
        bool ok = true;
        if (t == kGlob) ok = take(&m.g, sizeof m.g);
        else if (t == kInst && idx < kInsts) ok = take(&m.inst[idx], sizeof m.inst[idx]);
        else if (t == kSmpl && idx < kInsts) ok = take(m.sample[idx], kPathLen);
        else if (t == kTabl && idx < kTables) {
            ok = take(&m.tab[idx], sizeof m.tab[idx]);
            sawTable = true;
        } else if (t == kClip) {
            uint32_t n = 0;
            ok = take(&n, 4);
            if (n > uint32_t(kClipMax)) n = kClipMax;
            if (ok) ok = take(m.clip, n * uint32_t(sizeof(ClipStep)));
            m.clipLen = n;
        }
        if (!ok || !skip(fs, h, left)) break;
    }
    fs.close(h);
    sanitize(m);
    return sawTable;
}

}  // namespace flip
