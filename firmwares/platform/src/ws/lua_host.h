// PLATFORM -- the Lua script host (MANUAL.md "Lua API").
//
// One LuaSession runs one script: Lua 5.4 (32-bit numbers) on a capped heap
// (heap.h) carved from the session pool, the voice engine (engine.h) it
// controls, a recorded screen (redraw() records drawing commands; every
// frame replays the last committed list through the budgeted canvas), pad
// LEDs, MIDI, the clock (coroutines resumed ahead of time on the engine beat,
// so their engine commands carry sample-accurate timestamps), params with a
// params page, and file helpers in the script's data folder.
//
// Everything here runs in the main loop. Lua never runs in render().
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/budget_canvas.h"
#include "ws/pool.h"
#include "ws/engine.h"
#include "ws/heap.h"

struct lua_State;

namespace plat {

struct DrawOp {
    uint8_t op = 0, font = 0;
    uint16_t opt = 0;
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    uint32_t u = 0;
};

struct DrawList {
    DrawOp* ops = nullptr;
    float* verts = nullptr;
    char* text = nullptr;
    uint32_t n = 0, nv = 0, nt = 0;
    bool overflow = false;
    void clear() {
        n = nv = nt = 0;
        overflow = false;
    }
};

struct LuaConfig {
    uint32_t heapBytes = 3u * 1024u * 1024u;
    uint32_t callInsnLimit = 4000000;    // VM instructions per callback
    uint32_t initInsnLimit = 30000000;   // load + init()
};

class LuaSession {
public:
    static constexpr int kMaxOps = 2048, kMaxVerts = 4096, kMaxText = 12288;
    static constexpr int kConsoleLines = 64, kConsoleCols = 96;
    static constexpr int kMaxErrors = 5;              // errors in a window before the script stops
    static constexpr uint32_t kErrorWindowMs = 3000;
    static constexpr int kMaxPages = 12, kMaxSources = 8;

    // Creates the state in `pool` (the session's memory), runs the script and
    // its init(). Returns false when anything failed; failed() / error() tell.
    bool open(Pool& pool, lab::Context& ctx, Engine& engine, const char* scriptPath, const char* name, const LuaConfig& cfg);
    // The same with the script text in memory (bench, tests, built-in examples).
    bool openSource(Pool& pool, lab::Context& ctx, Engine& engine, const char* name, const char* src,
                    uint32_t len, const LuaConfig& cfg);
    // cleanup(), params saved; the state is gone afterwards (the pool is the caller's).
    void close();

    bool running() const { return L_ != nullptr && !failed_; }
    bool failed() const { return failed_; }
    const char* error() const { return error_; }
    const char* name() const { return name_; }
    Engine* engine() { return engine_; }

    void event(const lab::Event& e);
    void midi(const lab::MidiMsg& m);
    void update(uint32_t ms);
    void draw(BudgetCanvas& c);
    void leds(lab::Rgb* pads);
    void transport(bool playing);
    void save();

    // LABKIT: the pages the script's params live on (params:add{page = ...})
    int pageCount() const { return nPages_; }
    const char* pageName(int i) const { return (i >= 0 && i < nPages_) ? pageNames_[i] : ""; }
    int pageOf(const char* name);               // find or create (-1 when full)
    // REPL: evaluate one line in the script's state; the result is printed
    bool eval(const char* line);
    // the crash-safe runner: callback errors so far, the latest one
    int errorsTotal() const { return errorsTotal_; }
    const char* lastError() const { return lastError_; }
    uint32_t lastErrorMs() const { return lastErrorMs_; }
    // console history for the LOG view (0 = oldest kept line)
    int consoleTotal() const { return consoleN_; }
    // a copy of a string in the session pool (registry names live as long as the session)
    const char* keep(const char* s);
    float* modSlot(int i) { return (modSlots_ && i >= 0 && i < kMaxSources) ? &modSlots_[i] : nullptr; }
    int addModSlot() { return nModSlots_ < kMaxSources ? nModSlots_++ : -1; }
    bool isEnumParam(int rid) const { return rid >= 0 && rid < 512 && (enumBits_[rid >> 5] >> (rid & 31)) & 1u; }
    void markEnum(int rid) {
        if (rid >= 0 && rid < 512) enumBits_[rid >> 5] |= 1u << (rid & 31);
    }
    bool kitPath(char* out, uint32_t cap) const;   // Data/<script>/labkit.lkc

    // stats
    uint32_t heapUsed() const { return heap_.used(); }
    uint32_t heapCap() const { return heap_.capacity(); }
    uint32_t heapPeak() const { return heap_.peak(); }
    uint32_t screenDropped() const { return lastDropped_; }
    uint32_t sampleBytes() const { return sampleBytes_; }
    const char* consoleLine(int i) const;   // 0 = oldest of the shown lines
    int consoleCount() const { return consoleN_ < kConsoleLines ? consoleN_ : kConsoleLines; }

    // ---- used by the Lua bindings (lua_host.cpp) ----
    static LuaSession* from(lua_State* L);
    lab::Context* ctx() { return ctx_; }
    DrawList& back() { return lists_[back_]; }
    void commit();
    bool updatedFlag = false;
    uint8_t curFont = lab::font::kBody;
    uint16_t curAlign = 0;
    float fps = 15.0f;
    bool redrawRequested = false;
    double schedBeat = -1.0;     // > = 0 inside a clock coroutine
    uint8_t schedEpoch = 0;
    uint8_t padRgb[lab::kPads][3] = {};
    uint32_t gridColor = 0;
    char dataDir[64] = {};
    char scriptDir[48] = {};
    uint32_t insn = 0, insnLimit = 0;
    Pool* pool() { return pool_; }
    void addSampleBytes(uint32_t n) { sampleBytes_ += n; }
    void print(const char* s, uint32_t n);
    void printEnd();
    // MIDI with a timestamp: sent from update() when the engine reaches `beat`.
    void midiAt(const lab::MidiMsg& m);
    // clock snapshot
    double beatNow() const { return snapBeat_; }
    double bps() const { return snapBps_ * 44100.0; }   // beats per second
    uint8_t epochNow() const { return snapEpoch_; }

private:
    bool run(const char* src, uint32_t len);   // load + run the chunk
    bool start();                               // init(), params
    bool call(int nargs, uint32_t limit, bool fatal = true);   // pcall + traceback; fatal: stop the script
    bool callGlobal(const char* fn, int nargs); // global function (skipped when absent)
    void fail(const char* msg);
    void loadParams();
    void snapshot();
    void runClock();
    void flushMidi();

    lua_State* L_ = nullptr;
    lab::Context* ctx_ = nullptr;
    Pool* pool_ = nullptr;
    Heap heap_;
    Engine* engine_ = nullptr;
    DrawList lists_[2];
    int back_ = 0;
    char name_[32] = {};
    char path_[96] = {};
    bool failed_ = false;
    char error_[640] = {};
    char pageNames_[kMaxPages][16] = {};
    int nPages_ = 0;
    float* modSlots_ = nullptr;
    int nModSlots_ = 0;
    uint32_t enumBits_[16] = {};
    uint32_t lastPollMs_ = 0;
    int errInWindow_ = 0, errorsTotal_ = 0;
    uint32_t errWindowStart_ = 0, lastErrorMs_ = 0;
    char lastError_[160] = {};
    uint32_t lastRedrawMs_ = 0;
    uint32_t lastDropped_ = 0;
    uint32_t sampleBytes_ = 0;
    double snapBeat_ = 0.0, snapBps_ = 0.0;
    uint8_t snapEpoch_ = 0;
    uint32_t snapBlocks_ = 0;
    bool hasRedraw_ = false;
    // console
    char console_[kConsoleLines][kConsoleCols] = {};
    int consoleN_ = 0, consoleCol_ = 0;
    // scheduled MIDI
    struct TimedMidi {
        double beat;
        uint8_t epoch;
        lab::MidiMsg m;
    };
    static constexpr int kTimedMidi = 128;
    TimedMidi timed_[kTimedMidi];
    int nTimed_ = 0;
    LuaConfig cfg_;
};

// Script header ("-- name: ..." lines at the top of the file).
struct ScriptMeta {
    char name[24] = {}, author[24] = {}, desc[96] = {}, icon[16] = {};
};
void parseScriptHeader(const char* text, uint32_t len, const char* fileName, ScriptMeta& m);

const char* keyName(uint8_t id);

}  // namespace plat
