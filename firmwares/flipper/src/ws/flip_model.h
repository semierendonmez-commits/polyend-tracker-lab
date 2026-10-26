// FLIPPER -- the data model: a machine (the song) of up to 8 tables (sections),
// 8 instruments, the global play settings and the captured run (clip).
// Plain POD structs: shared by the main loop (editing) and the audio interrupt
// (playing). Every byte field is range-checked by sanitize() after a load.
#pragma once

#include <cstdint>
#include <cstring>

namespace ot {
class IStorage;
}

namespace flip {

constexpr int kCols = 12, kRows = 4, kCells = 48;
constexpr int kTables = 8, kInsts = 8, kMaxBalls = 4, kRails = 3, kGroups = 3;
constexpr int kClipMax = 4096;   // captured steps
constexpr int kNameLen = 12;
constexpr int kPathLen = 64;
constexpr uint8_t kNone = 255;

inline int cellCol(int c) { return c % kCols; }
inline int cellRow(int c) { return c / kCols; }
inline int cellAt(int col, int row) { return row * kCols + col; }

// ---- table elements ---------------------------------------------------------
enum Elem : uint8_t { kEmpty, kBumper, kRamp, kHole, kTarget, kFlipper, kWall, kElemCount };
// Eight directions, clockwise from north (north = row 0 = the far end of the table).
enum Dir : uint8_t { kN, kNE, kE, kSE, kS, kSW, kW, kNW, kDirCount, kRadial = 8 };
inline int dirDx(int d) { static const int8_t k[8] = {0, 1, 1, 1, 0, -1, -1, -1}; return k[d & 7]; }
inline int dirDy(int d) { static const int8_t k[8] = {-1, -1, 0, 1, 1, 1, 0, -1}; return k[d & 7]; }
inline int dirOpp(int d) { return (d + 4) & 7; }

// Ramp effects. Filter / Crush / Stutter are inserts on the target groups,
// Delay / Reverb are send throws from the target groups.
enum FxType : uint8_t { kFxFilter, kFxCrush, kFxStutter, kFxDelay, kFxReverb, kFxCount };
// Hole / target events.
enum EventType : uint8_t {
    kEvNone, kEvGraph, kEvGoto, kEvMultiball, kEvBallSave, kEvMultiplier, kEvMutate, kEvExtraBall, kEvJackpot,
    kEvCount
};
enum PitchMode : uint8_t { kPmFixed, kPmWalk, kPmCombo, kPmBall, kPmRandom, kPmCount };
enum VelMode : uint8_t { kVmFixed, kVmSpeed, kVmAccent, kVmRandom, kVmCount };
// Table-level actions (jackpot, multiball, game over).
enum Action : uint8_t { kActNone, kActGraph, kActGoto, kActMultiball, kActCount };
// Wall shapes (physics): a block, or a slope that guides the ball.
enum WallShape : uint8_t { kWallBlock, kWallSlopeR, kWallSlopeL, kWallCount };   // slope R: "\" rolls right

struct Cell {
    uint8_t type = kEmpty;
    uint8_t w[4] = {2, 2, 2, 2};   // Markov weights N E S W, 0..9
    uint8_t inst = kNone;          // 0..7 or kNone (silent)
    uint8_t note = 60;
    uint8_t vel = 100;             // 1..127
    uint8_t pitchMode = kPmFixed;
    uint8_t velMode = kVmFixed;
    uint8_t dir = kRadial;         // ramp / kick direction (0..7, 8 radial); flipper side (0 L, 1 R); wall shape
    uint8_t fx = kFxFilter;        // ramp effect
    uint8_t amount = 70;           // ramp amount 0..100; flipper power 1..9
    uint8_t len = 8;               // ramp sweep length (steps); hole hold (steps)
    uint8_t event = kEvNone;       // hole / target
    uint8_t arg = 0;               // event argument (table 0..7)
    uint8_t points = 10;           // x10 points
    uint8_t chance = 100;          // % the element plays when hit
    uint8_t groups = 7;            // ramp effect target groups (bit mask, drums / bass / keys)
    uint8_t reserved[2] = {};
};

// A backbeat lane: a Euclidean rhythm that joins once the heat reaches `heat`.
struct Rail {
    uint8_t inst = kNone;
    uint8_t note = 36;
    uint8_t vel = 100;
    uint8_t hits = 4;     // 0..16
    uint8_t steps = 16;   // 1..16
    uint8_t rotate = 0;   // 0..15
    uint8_t heat = 1;     // 0..4, 5 = off
    uint8_t reserved = 0;
};

enum StepRate : uint8_t { kRate4, kRate8, kRate16, kRate32, kRate8T, kRate16T, kRateCount };

struct Table {
    char name[kNameLen] = "TABLE";
    uint8_t enabled = 1;
    uint8_t color = 0;        // neon palette 0..5
    uint8_t stepRate = kRate16;
    uint8_t swing = 0;        // 0..75 %
    uint8_t scale = 0;        // flip::kScales
    uint8_t root = 0;         // 0 = C
    uint8_t gravity = 4;      // 0..9
    uint8_t balls = 3;        // 1..9, 10 = endless
    uint8_t lengthBars = 0;   // index into kLengthBars (0 = no time limit)
    uint8_t sides = 0;        // 0 bounce, 1 wrap around
    uint8_t launchCol = 11;   // plunger lane column
    uint8_t ballSave = 16;    // steps after a launch in which a drain is forgiven
    uint8_t jackpot = 10;     // score target on this table, x1000 points (0 = off)
    uint8_t onJackpot = kActGraph, jackpotArg = 0;
    uint8_t onMultiball = kActNone, multiballArg = 0;
    uint8_t onGameOver = kActGraph, gameOverArg = 0;
    uint8_t graph[kTables] = {};   // Markov graph: weight to every table, 0..9
    uint8_t reserved[3] = {};
    Rail rails[kRails];
    Cell cells[kCells];
};

enum Engine : uint8_t { kEnKick, kEnSnare, kEnHat, kEnZap, kEnPluck, kEnBell, kEnLead, kEnSample, kEnCount };
enum Group : uint8_t { kGrDrums, kGrBass, kGrKeys };

struct Instrument {
    char name[kNameLen] = "INST";
    uint8_t engine = kEnBell;
    uint8_t level = 80;       // 0..100
    uint8_t pan = 50;         // 0 left .. 100 right
    uint8_t decay = 40;       // 0..100
    uint8_t tone = 50;        // 0..100
    uint8_t charA = 50;       // engine character 1
    uint8_t charB = 30;       // engine character 2
    uint8_t sendDelay = 0;    // 0..100
    uint8_t sendReverb = 15;  // 0..100
    uint8_t group = kGrKeys;
    uint8_t midi = 1;         // 0 off, 1 base channel (Settings), 2..17 = channel 1..16
    uint8_t root = 60;        // the note that plays the sound at its own pitch
    uint8_t color = 0;        // LED colour index
    uint8_t reserved[3] = {};
};

// Global play settings (Settings rows; ints because SettingRow takes int*).
struct Global {
    int32_t mode = 0;         // 0 hybrid, 1 Markov only
    int32_t autoOn = 1;       // auto-player
    int32_t autoAfter = 3;    // seconds without player input
    int32_t autoSkill = 70;   // 0..100 %
    int32_t follow = 70;      // hybrid: how hard the grid ball chases the real ball, %
    int32_t ghost = 1;        // hybrid: ghost notes while the grid ball wanders
    int32_t quant = 0;        // table changes on 0: next bar, 1: 2 bars, 2: 4 bars
    int32_t midiNotes = 1;    // send bumper notes as MIDI
    int32_t midiIn = 1;       // MIDI notes in act as pads
    int32_t trails = 1;       // ball trails on screen
    int32_t startTable = 0;   // table on a new game
    int32_t ledLevel = 2;     // pad brightness 0..2
};

// One step of a captured run: where every ball was and what it played.
struct ClipStep {
    uint8_t table = 0;
    uint8_t heat = 0;
    uint8_t flags = 0;   // bit 0: tilt
    uint8_t fxCell = kNone;         // ramp whose effect fired on this step
    uint8_t cell[kMaxBalls] = {kNone, kNone, kNone, kNone};
    uint8_t inst[kMaxBalls] = {kNone, kNone, kNone, kNone};
    uint8_t note[kMaxBalls] = {};   // 0 = nothing played
    uint8_t vel[kMaxBalls] = {};
};

struct Machine {
    Global g;
    Instrument inst[kInsts];
    Table tab[kTables];
    char sample[kInsts][kPathLen] = {};   // WAV path per instrument (sampler engine)
    uint32_t clipLen = 0;
    ClipStep clip[kClipMax];
};

// ---- names and tables (UI) --------------------------------------------------
extern const char* const kElemNames[kElemCount];
extern const char* const kElemShort[kElemCount];
extern const char* const kDirNames[9];
extern const char* const kFxNames[kFxCount];
extern const char* const kEventNames[kEvCount];
extern const char* const kPitchNames[kPmCount];
extern const char* const kVelNames[kVmCount];
extern const char* const kActionNames[kActCount];
extern const char* const kEngineNames[kEnCount];
extern const char* const kGroupNames[kGroups];
extern const char* const kRateNames[kRateCount];
extern const char* const kNoteNames[12];
extern const char* const kWallNames[kWallCount];
constexpr int kScaleCount = 12;
extern const char* const kScaleNames[kScaleCount];
extern const uint16_t kScaleMask[kScaleCount];
constexpr int kLengthCount = 7;
extern const uint8_t kLengthBars[kLengthCount];   // 0 (off), 4, 8, 16, 32, 64, 128

// Steps per bar of a rate (4/4).
int stepsPerBar(int rate);
// Beats per step.
float beatsPerStep(int rate);
// Note `degrees` scale steps above `base` (base snapped down into the scale).
int scaleNote(int base, int degrees, int root, int scale);
// "C#3"-style name into buf (>= 5 chars).
void noteName(int note, char* buf);

// ---- factory content, checks, files -------------------------------------------
void factoryMachine(Machine& m);
void factoryTable(Table& t, int preset);    // presets 0..7
void clearTable(Table& t);
void sanitize(Machine& m);

// Versioned chunk file (.flp): "FLPR", version, then tagged chunks
// [tag 4][length u32][payload]; unknown chunks are skipped, short chunks keep
// their defaults, so newer firmwares read older files and the other way round.
constexpr uint16_t kFileVersion = 1;
bool saveMachine(ot::IStorage& fs, const char* path, const Machine& m);
// Loads into `m` (which must hold defaults); false = no valid file.
bool loadMachine(ot::IStorage& fs, const char* path, Machine& m);

}  // namespace flip
