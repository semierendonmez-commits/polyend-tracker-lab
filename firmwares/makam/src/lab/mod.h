// LABKIT v2 -- modulation: sources, routes and macros (lab::kit::Mod).
//
// Sources (ids):
//   0..3   LFO 1-4  (sine / tri / saw / square / S&H / smooth random; free Hz or
//                    synced to the transport; phase; retrigger on Play), bipolar
//   4..5   RND 1-2  (stepped or drunk walk, synced), bipolar
//   6..13  MACRO 1-8 (the macro knobs, 0..1)
//   14..   the workstation's own sources: addSource("ENV", &envLevel_)
//
// A route adds depth * source to a param's normalized value (depth -1..+1);
// a *window* route (macro targets) instead sweeps the param from lo to hi as
// the source goes 0..1. Up to 64 routes; a macro has up to 8 targets.
// Routes are applied once per audio block (Shell::kitBlock) from a compiled
// plan, so the cost is a few instructions per route.
#pragma once

#include "lab/lab.h"
#include "lab/params.h"

namespace lab {
namespace kit {

constexpr int kLfos = 4, kRands = 2, kMacros = 8, kMacroTargets = 8, kMaxRoutes = 64;
constexpr int kSrcLfo = 0, kSrcRnd = 4, kSrcMacro = 6, kSrcWs = 14, kMaxWsSources = 18;
constexpr int kMaxSources = kSrcWs + kMaxWsSources;

enum class Shape : uint8_t { kSine, kTri, kSaw, kSquare, kSampleHold, kSmooth };

struct Route {
    int8_t src = -1;
    uint8_t window = 0;     // 1: param = lo + (hi - lo) * source (macro target window)
    ParamId param = kNoParam;
    float depth = 0.0f;     // -1..+1 of the param's range (add routes)
    float lo = 0.0f, hi = 1.0f;
};

class Mod {
public:
    // ---- sources ---------------------------------------------------------
    // A workstation source (init): a float the workstation updates itself
    // (envelope, sequencer lane, ball position...). bipolar: -1..1, else 0..1.
    int addSource(const char* name, const float* value, bool bipolar = false);
    int registerSource(const char* name, const float* value, bool bipolar = false) { return addSource(name, value, bipolar); }
    int sourceCount() const { return kSrcWs + wsCount_; }
    int findSource(const char* name) const;
    const char* sourceName(int s) const;
    float sourceValue(int s) const;
    bool sourceBipolar(int s) const;
    Rgb sourceColor(int s) const;
    static bool isMacro(int s) { return s >= kSrcMacro && s < kSrcMacro + kMacros; }

    // ---- routes (main loop) ---------------------------------------------------
    // Add a route or change its depth; returns its index or -1 (table full,
    // the macro already has 8 targets, the param refuses modulation).
    int route(int src, ParamId p, float depth);
    int findRoute(int src, ParamId p) const;
    bool unroute(int src, ParamId p);
    void removeRoute(int i);
    void clearRoutes();
    int routeCount() const { return count_; }
    const Route& routeAt(int i) const { return routes_[i]; }
    void setDepth(int i, float depth);
    void setWindow(int i, bool on, float lo = 0.0f, float hi = 1.0f);
    int routesTo(ParamId p, int* out, int cap) const;
    int routesFrom(int src, int* out, int cap) const;

    // ---- macros ------------------------------------------------------------------
    float macro(int m) const;
    void setMacro(int m, float v);
    const char* macroName(int m) const { return (m >= 0 && m < kMacros) ? macroName_[m] : ""; }
    void setMacroName(int m, const char* name);
    ParamId macroParam(int m) const { return (m >= 0 && m < kMacros) ? macroParam_[m] : kNoParam; }
    ParamId lfoParam(int lfo, int field) const { return lfoParam_[lfo][field]; }   // field 0..4
    ParamId rndParam(int rnd, int field) const { return rndParam_[rnd][field]; }   // field 0..2

    // ---- audio (Shell::kitBlock) ----------------------------------------------------
    void tick(const Transport& t);   // sources for this block
    void applyRoutes();              // the plan -> registry writes
    // Compiles the routes into the audio plan when they changed (main loop).
    void update(bool audioRunning);

    // ---- internals ---------------------------------------------------------------------
    void attach(Kit* kit, Registry* reg) { kit_ = kit; reg_ = reg; }
    void registerShellParams();      // MACRO + MOD pages (when the nav turns on)
    bool shellParamsRegistered() const { return macroParam_[0] != kNoParam; }
    void markDirty() { dirty_ = true; }
    struct Lfo {
        float shape = 0, rate = 1.0f, sync = 0, phase = 0, retrig = 0;   // registry-bound
        float ph = 0.0f, out = 0.0f, a = 0.0f, b = 0.0f;
        uint32_t seed = 0;
    };
    struct Rnd {
        float mode = 0, sync = 4, rate = 2.0f;   // registry-bound
        float out = 0.0f;
        int32_t lastStep = -1;
        float ph = 0.0f;
        uint32_t seed = 0;
    };
    const Lfo& lfo(int i) const { return lfo_[i]; }
    const Rnd& rnd(int i) const { return rnd_[i]; }
    static float divisionBeats(int sync);   // 0 = free

private:
    // The audio plan: routes grouped by param, each param with its write
    // precomputed (float linear / float exp / int linear inline, else the registry).
    struct PlanRoute {
        const float* src;
        float depth, lo, hi;
        uint8_t window, bipolar, pad_[2];
    };
    struct PlanParam {
        ParamId id;
        uint8_t windows, adds;  // window routes first, then the add routes
        uint8_t mode, pad_[3];  // mode: 0 registry, 1 float lin, 2 float exp, 3 int lin
        float lo, k;
        void* ptr;
    };
    struct Plan {
        uint32_t gen = 0;
        int nRoutes = 0, nParams = 0;
        PlanRoute r[kMaxRoutes];
        PlanParam p[kMaxRoutes];
    };
    const float* sourcePtr(int s) const;
    void rebuild(Plan& plan);
    void recount();
    float rand01(uint32_t& s);

    Kit* kit_ = nullptr;
    Registry* reg_ = nullptr;
    Route routes_[kMaxRoutes];
    int count_ = 0;
    bool dirty_ = false;
    Plan plans_[2];
    Plan* volatile live_ = nullptr;      // read by the audio block
    volatile uint32_t audioGen_ = 0;     // plan generation the audio block last used
    uint32_t gen_ = 0;
    Lfo lfo_[kLfos];
    Rnd rnd_[kRands];
    float lfoOut_[kLfos] = {}, rndOut_[kRands] = {};
    float b64_ = 0.0f;          // the beat inside a 64-beat cycle, advanced in float
    uint32_t resync_ = 0;       // blocks until the next exact resync from the double beat
    float zero_ = 0.0f;
    char macroName_[kMacros][12] = {};
    ParamId macroParam_[kMacros] = {kNoParam, kNoParam, kNoParam, kNoParam, kNoParam, kNoParam, kNoParam, kNoParam};
    ParamId lfoParam_[kLfos][5] = {};
    ParamId rndParam_[kRands][3] = {};
    float macroVal_[kMacros] = {};       // registry-bound macro values
    struct WsSource {
        const char* name;
        const float* value;
        bool bipolar;
    };
    WsSource ws_[kMaxWsSources] = {};
    int wsCount_ = 0;
};

}  // namespace kit
}  // namespace lab
