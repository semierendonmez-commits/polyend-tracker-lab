// GRAF -- the patch engine: module types, module slots, cables, the compiled
// routing plan the audio interrupt runs, MIDI voices, snapshots + morphing,
// the song timeline and the patch file format.
//
// Threading: the main loop owns every structure here. The audio interrupt
// reads the published Plan (double buffered, swapped by an index store), the
// module parameter values `v[]` (floats written by the main loop), cable
// amounts (floats) and the voice table. A module slot whose module was
// removed is only reused after the audio side has started a block with a plan
// that no longer contains it.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/graf_math.h"

namespace graf {

using lab::kBlock;

constexpr int kMaxIn = 6, kMaxOut = 4, kMaxParam = 10;
constexpr int kMaxSlots = 64;      // module slots (all patches)
constexpr int kMaxCables = 160;
constexpr int kCells = 48;         // canvas cells per patch = pads (12 x 4)
constexpr int kPatches = 8;
constexpr int kSnaps = 8;
constexpr int kSongSteps = 32;
constexpr int kSlabs = 6;                  // big float buffers (delay, reverb, grains)
constexpr uint32_t kSlabFloats = 262144;   // 1 MB each
constexpr int kSampleBanks = 8;            // sampler memory, one per SMPL module
constexpr uint32_t kBankSamples = 262144;  // int16, 512 KB each (5.9 s mono)
constexpr int kMaxSampleFiles = 32;
constexpr int kExtBytes = 64;              // per-module extra data (sequencer steps)
// CPU budget in M4 instructions per block (the gate limit is 180,000; the
// rest covers the engine itself, cable sums and the shell's work).
#ifdef GRAF_CAL
constexpr int32_t kCpuBudget = 100000000;   // calibration builds: no guard
#else
constexpr int32_t kCpuBudget = 168000;
#endif
constexpr int32_t kCableCost = 950;         // a cable summed into an input that already has one
constexpr int32_t kEngineCost = 6000;      // fixed per block

// Signal kinds (cable colours, default amounts).
enum Sig : uint8_t { kAudio, kCv, kGate, kPitch };
// Module categories (colours, the ADD page).
enum Cat : uint8_t { kCatOsc, kCatShape, kCatMod, kCatSeq, kCatFx, kCatIo, kCatMacro, kCatCount };

enum ParamKind : uint8_t { kLin, kExp, kInt };

struct ParamDef {
    const char* name;
    float min, max, def;
    uint8_t kind;
    const char* unit;                 // "Hz", "s", "%", "st", "oct", ...
    const char* const* names;         // kInt with names (max - min + 1 entries)
};

struct JackDef {
    const char* name;
    uint8_t sig;
};

struct Module;
struct Proc;
using ProcessFn = void (*)(Module& m, Proc& p);
using ResetFn = void (*)(Module& m);

struct ModDef {
    const char* tag;       // short name on boxes: "VCO"
    const char* title;     // "Oscillator"
    const char* about;     // one line
    uint8_t cat;
    uint8_t nIn, nOut, nParam;
    JackDef in[kMaxIn];
    JackDef out[kMaxOut];
    ParamDef p[kMaxParam];
    int32_t cost;          // M4 instructions per block (measured, tests/lab bench)
    uint8_t slab;          // 1 = needs a big buffer
    ProcessFn process;
    ResetFn reset;         // state init after creation (may be null)
};

// Type ids are stored in patch files: append only, never renumber.
enum ModType : uint8_t {
    kVco, kCplx, kWtbl, kNoise, kSmpl, kAudIn, kMidi, kRete, kPinb, kClk, kDiv, kSeq, kEucl, kTuring, kMarkov,
    kSnh, kQuant, kLfo, kAdsr, kAd, kFunc, kSlew, kLogic, kCvmx, kLadder, kSvf, kLpg, kFold, kVca, kMix, kRing,
    kCrush, kDelay, kVerb, kGrain, kOut, kPerc,
    // round 2
    kPlts, kReso, kExct, kNet, kRval, kMg, kMgate, kChaos, kBern, kAcid, kFltr, kPart, kTypeCount
};
extern const ModDef kDefs[kTypeCount];
const ModDef& def(int type);

// Per-module DSP state: one union, sized for the biggest module.
struct StateMem {
    alignas(8) uint8_t bytes[1536];
};

struct Module {
    bool used = false;
    uint8_t type = 0;
    uint8_t patch = 0;     // 0..7
    uint8_t cell = 0;      // canvas cell 0..47 (= pad)
    uint8_t number = 1;    // display number: VCO 1, VCO 2...
    float k[kMaxParam] = {};   // knob positions 0..1: the user's (base) value (UI, snapshots, files)
    float ke[kMaxParam] = {};  // effective knob 0..1 (base + LABKIT modulation; the registry writes it)
    float kv[kMaxParam] = {};  // the ke that v[] was last mapped from (audio)
    float v[kMaxParam] = {};   // mapped values (audio reads)
    void* big = nullptr;       // pooled engine memory (PLTS)
    int8_t bigIdx = -1;
    float* out[kMaxOut] = {};
    float* slab = nullptr;
    int8_t slabIdx = -1;
    int16_t* bank = nullptr;   // sampler memory
    int8_t bankIdx = -1;
    uint8_t ext[kExtBytes] = {};
    StateMem st;
    template <class T> T& s() {
        static_assert(sizeof(T) <= sizeof(StateMem), "module state too big");
        return *reinterpret_cast<T*>(st.bytes);
    }
};

class Engine;

// What a module's process() sees for one block.
struct Proc {
    const float* in[kMaxIn];
    uint8_t conn = 0;          // bit i: input i has at least one cable
    uint8_t used = 0xFF;       // bit o: output o is read by a cable or the scope
    const lab::Transport* tr = nullptr;
    float beat = 0.0f;         // transport beat at block start, modulo 1024
    float bps = 0.0f;          // beats per sample
    bool playing = false;
    bool started = false;      // transport started in this block
    const lab::AudioIn* ain = nullptr;
    Engine* eng = nullptr;
    bool has(int i) const { return (conn >> i) & 1; }
};

struct Cable {
    bool used = false;
    uint8_t src = 0, srcOut = 0, dst = 0, dstIn = 0;
    float amount = 1.0f;
};

// The compiled routing the audio side runs.
struct Plan {
    uint8_t order[kMaxSlots];
    uint8_t count = 0;
    struct Src {
        const float* buf;
        const float* amt;
    };
    Src src[kMaxCables];
    uint16_t first[kMaxSlots][kMaxIn];
    uint8_t n[kMaxSlots][kMaxIn];
    uint8_t used[kMaxSlots];   // outputs read by cables
};

// MIDI voices per patch (main loop writes, MIDI modules read).
struct Voice {
    float pitch = 0.0f;   // octaves, 0 = C4
    float vel = 0.0f;
    bool gate = false;
    uint8_t note = 60;
    uint32_t trig = 0;    // increments on every note-on
    uint32_t age = 0;
};
struct PatchVoices {
    Voice v[8];
    int count = 1;              // voices in use (max Voice param of the MIDI modules)
    uint8_t stack[16];          // mono: held notes, last = newest
    uint8_t stackVel[16];
    int held = 0;
    uint32_t clock = 0;
    float bend = 0.0f;          // -1..1
    float cc[128];
};

// A mixer strip: patch n (8 patches) or OUT n (Rack).
struct MixStrip {
    float level = 1.0f;
    float pan = 0.0f;
    int mute = 0;
};

constexpr int kTaps = 8, kLanes = 4;
constexpr int kPlaitsPool = 4;
constexpr int32_t kPartTrainCost = 74000;   // PART with Type = trainlet (measured, GRAF_CAL)

// A snapshot holds a whole patch state. Modules are keyed by where they sit
// (patch * 48 + cell), so morphing matches modules by position.
struct SnapMod {
    uint8_t type = 0xFF;   // 0xFF = none
    uint8_t patch = 0, cell = 0, number = 1;
    float k[kMaxParam];
    uint8_t ext[kExtBytes];
    uint16_t key() const { return uint16_t(patch * kCells + cell); }
};
struct SnapCable {
    uint16_t src, dst;     // keys
    uint8_t srcOut, dstIn;
    float amount;
};
struct Snapshot {
    bool valid = false;
    uint8_t nMod = 0;
    uint16_t nCab = 0;
    SnapMod mod[kMaxSlots];
    SnapCable cab[kMaxCables];
    float level[kPatches];
    int find(uint16_t key) const {
        for (int i = 0; i < nMod; ++i)
            if (mod[i].key() == key) return i;
        return -1;
    }
};

struct SongStep {
    uint8_t snap = 0;
    uint8_t morphBars = 2;
    uint8_t holdBars = 2;
};

// Per-sample-file entry of the card's Samples folder.
struct SampleFile {
    char name[40];
};

class Engine {
public:
    // Memory from the arena; false when it does not fit.
    bool init(lab::Arena& a);
    void setConcurrent(bool on) { concurrent_ = on; }

    // ---- graph editing (main loop) ----
    int add(int type, int patch, int cell = -1);    // slot or -1 (full / no CPU / no memory)
    int addAt(int slot, int type, int patch, int cell);   // a given slot (snapshots, files)
    bool remove(int slot);
    void clear();
    int duplicate(int slot);
    bool move(int slot, int cell);
    int cableFind(int src, int srcOut, int dst, int dstIn) const;
    int connect(int src, int srcOut, int dst, int dstIn, float amount);   // cable index or -1
    bool disconnect(int cable);
    void setParamK(int slot, int p, float k);       // knob 0..1, clamped; updates v (and tells onKnob)
    // Main loop: called after a knob's base value changed (LABKIT registry follows it).
    void (*onKnob)(void* ctx, int slot, int p) = nullptr;
    void* onKnobCtx = nullptr;
    // Main loop: a module was added / removed / moved / renumbered (names, pages).
    void (*onModules)(void* ctx) = nullptr;
    // Cell remap for snapshots when modules move: every key `from` becomes `to`.
    void remapSnapKeys(const uint16_t* from, const uint16_t* to, int n);
    // Places a module in a cell without swapping (the cell must be free); snapshots follow.
    bool place(int slot, int cell);
    // Upstream / downstream modules of a slot (in cable order); returns the count.
    int upstream(int slot, int* out, int cap) const;
    int downstream(int slot, int* out, int cap) const;
    // Modules in signal order (the plan's order) of one canvas; returns the count.
    int signalOrder(int patch, int* out, int cap) const;
    void refresh(int slot);                         // k -> v for all params
    void resetParams(int slot);
    void randomize(int slot, Rand& r);
    void rebuild();                                 // compile + publish the plan
    void service(uint32_t nowMs);                   // pending frees etc.
    void quiesce();                                 // single-threaded: free now

    // ---- queries ----
    Module& mod(int slot) { return mods_[slot]; }
    const Module& mod(int slot) const { return mods_[slot]; }
    Cable& cable(int i) { return cables_[i]; }
    const Cable& cable(int i) const { return cables_[i]; }
    int slotAtCell(int patch, int cell) const;
    int freeCell(int patch, int preferred = -1) const;
    int moduleCount(int patch = -1) const;          // -1 = all
    int cableCount() const;
    int32_t cpuCost() const;                        // estimated M4 insn per block
    int32_t modCost(int slot, bool allOuts = false) const;
    int32_t cpuCostOfType(int type) const { return def(type).cost; }
    bool fits(int32_t extra) const { return cpuCost() + extra <= kCpuBudget; }
    static float mapParam(const ParamDef& d, float k);
    static float unmapParam(const ParamDef& d, float v);
    // Patch whose MIDI voices / mixer strip a slot uses (Rack mode: all share 0).
    int voicePatch(int slot) const { return rack ? 0 : mods_[slot].patch; }
    const char* lastError() const { return err_; }

    // ---- audio (interrupt) ----
    void render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r, lab::Context* ctx);
    uint32_t renders() const { return renders_; }

    // ---- MIDI / keyboard (main loop) ----
    void noteOn(int patch, uint8_t note, uint8_t vel);
    void noteOff(int patch, uint8_t note);
    void allNotesOff();
    void midiIn(const lab::MidiMsg& m);
    PatchVoices& voices(int patch) { return voices_[patch]; }

    // ---- snapshots / song (main loop) ----
    void storeSnap(int i);
    void clearSnap(int i);
    const Snapshot& snap(int i) const { return snaps_[i]; }
    // Morph from the current state to snapshot i over `ms` (0 = now).
    void recallSnap(int i, uint32_t ms, uint32_t nowMs);
    void morphStep(uint32_t nowMs);              // live recall progress
    void crossfade(int snapA, int snapB, float t); // the X-FADE parameter
    bool morphing() const { return morphTo_ >= 0; }
    int morphTarget() const { return morphTo_; }
    float morphPos() const { return morphPos_; }
    // Song: position the snapshots for transport beat `beat`.
    void songApply(double beat);
    int songStepAt(double beat, float* t, double* stepStartBar) const;
    int songBars() const;

    // ---- files ----
    // withSnaps = false: the structure only (modules, cables, strips) -- undo states.
    uint32_t serialize(uint8_t* buf, uint32_t cap, bool withSnaps = true) const;
    // keepState: modules already in place (same slot, type) keep their DSP state
    // (undo / redo); otherwise the patch is rebuilt from scratch.
    bool deserialize(const uint8_t* buf, uint32_t len, bool keepState = false);

    // ---- samples ----
    void setSampleFiles(const SampleFile* files, int n);
    int sampleFileCount() const { return nFiles_; }
    const char* sampleFileName(int i) const { return (i > 0 && i <= nFiles_) ? files_[i - 1].name : "Built-in"; }
    // Loads the module's sample (by its Sample parameter) into its bank.
    bool loadSample(int slot, lab::IStorage* fs, const char* dir);
    uint32_t bankFrames(int slot) const;
    void builtinSample(int16_t* dst, uint32_t n, uint32_t* frames);

    // public state (main loop writes, audio reads)
    bool rack = true;               // Rack (one patch) or 8 patches
    MixStrip mix[kPatches];
    Snapshot snaps_[kSnaps];
    SongStep song[kSongSteps];
    int songLen = 4;
    bool songOn = false;
    char sampleName[kMaxSlots][40] = {};    // file loaded per SMPL slot
    uint32_t sampleFrames[kMaxSlots] = {};
    bool sampleStereo[kMaxSlots] = {};
    int sampleLoaded[kMaxSlots] = {};       // param value the bank holds (-1 = none)

    // Scope: one output of one module, copied every block.
    volatile int scopeSlot = -1;
    volatile int scopeOut = 0;
    float scope[kBlock] = {};
    volatile uint32_t scopeSeq = 0;
    // Output meters per patch (peak, decays), master.
    float meter[kPatches] = {};
    float masterPeak = 0.0f;

    // Stems / master accumulation (audio).
    float* stemL[4] = {};
    float* stemR[4] = {};
    bool stemUsed[4] = {};
    float* mixL = nullptr;
    float* mixR = nullptr;

    // LABKIT sources: user-marked outputs (taps) and the sequencer lanes.
    int8_t tapSlot[kTaps] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint8_t tapOut[kTaps] = {};
    float tapVal[kTaps] = {};
    int8_t laneSlot[kLanes] = {-1, -1, -1, -1};
    float laneVal[kLanes] = {};
    float masterEnv = 0.0f, barPhase = 0.0f, beatPhase = 0.0f;
    int tapOf(int slot, int out) const;            // tap index or -1
    int toggleTap(int slot, int out);              // new tap index, -1 = removed (or full: -2)
    void refreshLanes();                           // main loop: pick the lane modules
    volatile bool manualGate = false;              // the Perform key (MGATE modules)
    float tempoBpm = 120.0f;   // last transport tempo seen by render
    volatile uint32_t beatQ8 = 0;   // transport beat x 256 at the last block (atomic for the main loop)
    double beat() const { return double(beatQ8) * (1.0 / 256.0); }
    bool allOutputs = false;   // compute every output (CPU calibration)
    Rand rng;

    // MIDI notes from the sequencer modules (audio -> main loop).
    struct MidiOutEv {
        uint8_t ch, note, vel;
        bool on;
    };
    lab::SpscRing<MidiOutEv, 256> midiQ;

private:
    void compile(Plan& p);
    void freeSlot(int slot);
    void collectVoices();
    void morphBetween(const Snapshot& a, const Snapshot& b, float t);
    void captureLive(Snapshot& s) const;
    const char* err_ = "";
    bool dirtyPlan_ = false;
    int newNumber(int type) const;

    Module* mods_ = nullptr;
    Cable* cables_ = nullptr;
    Plan* plans_ = nullptr;          // three: published, being read by audio, being written
    volatile int pub_ = 0;           // published plan index
    volatile int reading_ = 0;       // plan the audio side is running
    volatile uint32_t pubSeq_ = 0, ackSeq_ = 0;
    float* bufs_ = nullptr;          // kMaxSlots * kMaxOut * kBlock
    float* slabs_[kSlabs] = {};
    bool slabUsed_[kSlabs] = {};
    int16_t* banks_[kSampleBanks] = {};
    bool bankUsed_[kSampleBanks] = {};
    void* plaits_[kPlaitsPool] = {};
    bool plaitsUsed_[kPlaitsPool] = {};
    bool concurrent_ = false;
    struct Pending {
        uint8_t slot;
        uint32_t seq;
    };
    Pending pending_[kMaxSlots];
    int nPending_ = 0;
    bool reserved_[kMaxSlots] = {};  // removed, waiting for the audio side
    volatile uint32_t renders_ = 0;
    PatchVoices voices_[kPatches];
    // live morph
    Snapshot* morphFrom_ = nullptr;
    int morphTo_ = -1;
    uint32_t morphStart_ = 0, morphMs_ = 0;
    float morphPos_ = 0.0f;
    int songLastStep_ = -1;
    float songLastT_ = -1.0f;
    SampleFile files_[kMaxSampleFiles];
    int nFiles_ = 0;
};

}  // namespace graf
