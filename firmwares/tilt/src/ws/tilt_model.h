// TILT -- the data model: one Pattern holds everything the user edits (the four
// 17 x 17 matrix pages, edge properties, the 16 steps, flippers, combos, rules,
// evolution, melody, randomizer settings, the 8 sound channels and the mix).
// Plain fields with clear ranges; the parameter table (kParams) describes the
// scalar ones for the pages (and, later, for the LABKIT parameter registry).
//
// Pinball 2 vocabulary (nouserid, as decoded in FLOATDAT's Pinball.h):
//   state 0 = S (before step 1), states 1..16 = the steps.
//   matrix row = where the ball is, column = where it goes; column 0 = D, the drain.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tilt {

constexpr int kSteps = 16, kStates = 17, kPages = 4, kCh = 8, kBalls = 4, kCombos = 4, kFlips = 4;
constexpr int kComboLen = 4;
constexpr int kTrigNone = 0, kTrigMute = 9;   // 1..8 = channel
constexpr int kParamsPerEngine = 8;
constexpr int kNameLen = 12, kPathLen = 128;   // sample paths (v1 files: 48)
constexpr int kPathLenV1 = 48;

// ---- the steps ------------------------------------------------------------------
// One Pinball step: the device's rows plus TILT's bumper / ramp / degree.
struct Step {
    uint8_t trig = 0;      // 0 none, 1..8 channel, 9 mute (chokes every choke group)
    uint8_t trigP = 100;   // % chance the trig fires
    uint8_t delay = 0;     // 0..23 twelfths of a move
    uint8_t delayP = 100;  // % chance the delay applies
    uint8_t vel = 100;     // 1..127
    uint8_t len = 4;       // 1..64 quarter moves (4 = one move)
    uint8_t bumper = 0;    // 0..4 extra ratchet hits inside the move, each at 0.8 x the last velocity
    uint8_t ramp = 0;      // 0 off, 1..16: the next move is forced to this step
    int8_t degree = 0;     // -7..21 scale degree (melody mode)
    uint8_t lock = 0;      // randomizer lock
};
enum StepRow : uint8_t { kRowTrig, kRowTrigP, kRowDelay, kRowDelayP, kRowVel, kRowLen, kRowBumper, kRowRamp,
                         kRowDegree, kRowLock, kStepRows };

// Per matrix cell (row, column) of a page: what happens when the ball takes that edge.
struct Edge {
    int8_t tr = 0;     // -12..12 semitones
    uint8_t dly = 0;   // 0..11 twelfths of a move
    int8_t vel = 0;    // -64..63
    uint8_t pad = 0;
};

struct Flipper {
    uint8_t step = 1;   // 1..16
    uint8_t note = 36;  // MIDI note that flips it
    uint8_t pad[2] = {};
};

enum ComboAction : uint8_t { kCaMultiball, kCaFill, kCaNote, kCaCc, kCaShake, kCaScene, kComboActions };
struct Combo {
    uint8_t len = 0;                 // 0 (unused) or 2..4
    uint8_t path[kComboLen] = {};    // steps 1..16 (0 = the path ends here)
    uint8_t action = kCaMultiball;
    uint8_t a = 1, b = 100;          // action parameters (see comboParamText)
    uint8_t on = 1;
};

// ---- the machine ------------------------------------------------------------------
struct Table {
    uint8_t rate = 2;          // index into kRates (move length)
    uint8_t resetBars = 0;     // 0 off, 1..64: relaunch from the start step
    uint8_t startStep = 1;     // 1..16
    uint8_t swing = 0;         // 0..75 %
    uint8_t drain = 0;         // 0 REST (one silent move, then from S), 1 HOLD (until the next relaunch)
    uint8_t tiltFlips = 6;     // flips per bar before TILT (0 off)
    uint8_t tiltVel = 124;     // a flip / nudge this hard tilts (0 off)
    uint8_t tiltAction = 0;    // 0 FREEZE, 1 SHAKE, 2 BOTH
    uint8_t tiltBeats = 4;     // 1..16 beats
    uint8_t saveBars = 2;      // ball save window after a launch (0 off)
    uint8_t gravity = 0;       // 0..100 %
    uint8_t gravityGrid = 0;   // 0 bar, 1 half bar, 2 beat
    uint8_t mbMax = 3;         // 1..4 balls in play
    uint8_t mbLife = 16;       // moves an extra ball lives (4..64)
    uint8_t stepNoteBase = 48; // MIDI notes base..base+15 flip to steps 1..16
    uint8_t pad = 0;
};

struct Evolve {
    uint8_t fatigue = 0;       // 0..100: a used edge gets tired
    uint8_t reinforce = 0;     // 0..100: a used edge gets stronger
    uint8_t recover = 20;      // 0..100: memory back to neutral
    uint8_t mutate = 0;        // 0..100: weights drift every bar
    uint8_t mutHome = 15;      // 0..100: drift pulled back home every bar
    uint8_t mutSpread = 0;     // 0..100: drift reaches empty cells too
    uint8_t metaOn = 0;
    uint8_t metaBars = 4;      // 1..16
    uint8_t meta[kPages][kPages] = {{50, 50, 0, 0}, {0, 50, 50, 0}, {0, 0, 50, 50}, {50, 0, 0, 50}};
    uint8_t page = 0;          // the page being played (and edited)
    // rules
    uint8_t anchorStep = 0;    // 0 off, 1..16
    uint8_t anchorEvery = 8;   // moves
    uint8_t maxRepeat = 0;     // 0 off, 1..8
    uint8_t noBounce = 0;      // never straight back to the step the ball came from
    uint8_t pad[3] = {};
};

struct Melody {
    uint8_t on = 0;
    uint8_t scale = 1;         // index into kScales
    uint8_t root = 0;          // 0..11 key (added to every following channel's note)
    uint8_t octUp = 0;         // 0..100 % chance a note jumps an octave
};

enum RandTarget : uint8_t { kRtMatrix = 1, kRtSteps = 2, kRtLanes = 4, kRtRules = 8, kRtSound = 16 };
struct RandCfg {
    uint8_t amount = 40;       // 0..100
    uint8_t density = 40;      // matrix: how many cells a random row gets
    uint8_t tMatrix = 1, tSteps = 1, tLanes = 1, tRules = 0, tSound = 0;   // targets of RANDOMIZE
    uint8_t pad = 0;
    uint16_t rowLocks = 0;     // bit per StepRow: those step rows are never randomized
    uint16_t pad2 = 0;
};
uint8_t randTargets(const RandCfg& r);

// ---- sound -------------------------------------------------------------------------
enum EngineType : uint8_t { kEnSample, kEnKick, kEnSnare, kEnHat, kEnAcid, kEnFm, kEngines };
struct Channel {
    uint8_t type = kEnSample;
    uint8_t p[kParamsPerEngine] = {};   // 0..127, meaning per engine (kEngineParams)
    uint8_t level = 100, pan = 64, sendA = 0, sendB = 0;
    uint8_t choke = 0;      // 0 off, 1..4 choke group
    uint8_t midiCh = 1;     // 0 off, 1..16
    uint8_t note = 36;      // the channel's note (MIDI out, pitch reference, melody base)
    uint8_t follow = 0;     // melody mode: play the step's scale degree
    uint8_t legato = 0;     // overlapping notes glide (ACID / FM)
    uint8_t stem = 0;       // recorder stem 0..3
    uint8_t mute = 0, solo = 0;
    uint8_t pad[2] = {};
    char sample[kPathLen] = {};   // SAMPLE: card path; empty = the built-in sample p[0]
};

struct Fx {
    uint8_t dlyTime = 3;    // index into kDelayTimes
    uint8_t dlyFb = 45, dlyTone = 70, dlyWidth = 60;
    uint8_t revSize = 60, revDamp = 50, revLevel = 80;
    uint8_t drive = 0;      // master drive 0..100
    uint8_t master = 90;    // 0..127
    uint8_t pad[3] = {};
};

constexpr uint32_t kPatternMagic = 0x504C5454u;   // "TTLP"
constexpr uint16_t kPatternVersion = 2;   // 2: 128-character sample paths

struct Pattern {
    uint32_t magic = kPatternMagic;
    uint16_t version = kPatternVersion;
    uint16_t size = 0;           // sizeof(Pattern) when written
    char name[kNameLen] = {};
    uint16_t bpmX10 = 1700;
    uint16_t pad0 = 0;
    uint8_t w[kPages][kStates][kStates] = {};   // weights 0..100
    Edge e[kPages][kStates][kStates] = {};
    Step steps[kSteps];
    Flipper flips[kFlips];
    Combo combos[kCombos];
    Table table;
    Evolve ev;
    Melody mel;
    RandCfg rnd;
    Channel ch[kCh];
    Fx fx;
};

// ---- tables ----------------------------------------------------------------------------
struct Rate {
    const char* name;
    float beats;   // one move in quarter notes
};
constexpr int kRateCount = 8;
extern const Rate kRates[kRateCount];
constexpr int kScaleCount = 10;
struct Scale {
    const char* name;
    uint8_t n;
    int8_t semis[12];
};
extern const Scale kScales[kScaleCount];
// Semitones of scale degree `deg` (any integer; wraps into octaves).
int scaleSemis(int scale, int deg);
extern const char* const kEngineNames[kEngines];
struct EngParam {
    const char* name;   // nullptr = unused slot
    uint8_t min, max, def;
};
extern const EngParam kEngineParams[kEngines][kParamsPerEngine];
void engineDefaults(Channel& c, int type);
void engineParamText(const Channel& c, int i, char* buf, int cap);
extern const char* const kComboActionNames[kComboActions];
extern const char* const kNoteNames[12];
constexpr int kDelayTimeCount = 8;
extern const char* const kDelayTimeNames[kDelayTimeCount];
extern const float kDelayTimeBeats[kDelayTimeCount];
constexpr int kBuiltins = 8;
extern const char* const kBuiltinNames[kBuiltins];
void noteName(int note, char* buf, int cap);
// "A" / "x 2 balls" etc. for a combo's parameters.
void comboParamText(const Combo& c, int which, char* buf, int cap);

// ---- the parameter table (scalar parameters of the pages) ------------------------------
enum ParamFmt : uint8_t {
    kFmtInt, kFmtPct, kFmtOnOff, kFmtNames, kFmtStepOff, kFmtBars, kFmtNote, kFmtSigned, kFmtMidiCh, kFmtBeats
};
enum ParamPage : uint8_t { kPpTable, kPpEvolve, kPpMelody, kPpRandom, kPpMix, kPpFx, kPpChannel, kPpCount };
struct ParamDef {
    const char* name;
    const char* group;
    uint8_t page;        // ParamPage
    uint8_t fmt;         // ParamFmt
    uint16_t offset;     // byte offset in Pattern
    uint16_t stride;     // per index (channels / flippers); 0 = scalar
    int16_t min, max, def;
    const char* const* names;   // kFmtNames
    uint8_t count;              // indexed parameters: how many (1 = scalar)
};
extern const ParamDef kParams[];
extern const int kParamCount;
// The parameters of a page, in order (indices into kParams); returns the count.
int paramsOfPage(int page, int* out, int cap);
int paramGet(const Pattern& p, int id, int index = 0);
void paramSet(Pattern& p, int id, int v, int index = 0);
void paramText(const Pattern& p, int id, int index, char* buf, int cap);

// ---- building patterns ---------------------------------------------------------------
void defaultPattern(Pattern& p);
void defaultKit(Pattern& p);
constexpr int kFactoryCount = 15;   // the 10 ideas (flippers ... melody), then 5 more
extern const char* const kFactoryNames[kFactoryCount];
extern const char* const kFactoryIdeas[kFactoryCount];
void factoryPattern(int i, Pattern& p);

// Matrix shapes (functions) on one page.
enum Shape : uint8_t {
    kShChain, kShReverse, kShPingPong, kShDrunk, kShJumps, kShEuclid, kShStay, kShHub, kShRandom, kShClear,
    kShapeCount
};
extern const char* const kShapeNames[kShapeCount];
struct Rng32 {
    uint32_t s = 0x2545F491u;
    void seed(uint32_t v) { s = v ? v : 0x2545F491u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    int below(int n) { return n <= 1 ? 0 : int(next() % uint32_t(n)); }
    bool pct(int p) { return uni() * 100.0f < float(p); }
};
void applyShape(Pattern& p, int page, int shape, Rng32& r);
// Row functions on one row of a page.
enum RowFn : uint8_t { kRfClear, kRfNext, kRfEqual, kRfRandom, kRfStay, kRfBack, kRowFnCount };
extern const char* const kRowFnNames[kRowFnCount];
void applyRowFn(Pattern& p, int page, int row, int fn, Rng32& r);

// The randomizer: amount, targets and locks from p.rnd; step locks from Step::lock.
void randomize(Pattern& p, Rng32& r, uint8_t targets);
// Pinball's "Randomize Trigs": every unlocked step gets random(0..9).
void randomizeTrigs(Pattern& p, Rng32& r);
void clearSteps(Pattern& p);

// Bytes of a pattern (save / load with version and size: older files load, the
// rest defaults; a newer file's extra bytes are ignored).
void sanitize(Pattern& p);
bool patternFromBytes(Pattern& p, const uint8_t* data, uint32_t len);
// A kit: the 8 channels and the FX (version 2: 128-character sample paths; v1 converts).
uint32_t kitToBytes(const Pattern& p, uint8_t* out, uint32_t cap);
bool kitFromBytes(Pattern& p, const uint8_t* data, uint32_t len);
// Combo path helpers: the length is the number of leading non-zero steps.
void comboFixLen(Combo& c);
uint32_t patternToBytes(const Pattern& p, uint8_t* out, uint32_t cap);

}  // namespace tilt
