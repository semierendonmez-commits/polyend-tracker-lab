// LABKIT v2 -- the parameter registry (lab::kit::Registry).
//
// A workstation registers its parameters in init(): a stable key (saves), a
// short name, a page and a group, the range and curve, a unit, and a binding
// to a field of its own state (float*, int*) or a setter. The registry keeps
// the *base* value (what the user set) and writes the *effective* value
// (base + modulation, clamped) to the binding once per audio block, before
// render(), so the DSP code keeps reading its own fields.
//
//   kit::Registry& P = ctx.params();
//   P.setPage(0);  P.setGroup("FILTER", Rgb(0xFF8A3D));
//   cutoff_ = P.add("flt.cut", "Cutoff", &cut_).range(20, 18000).exp().unit("Hz");
//   P.add("flt.res", "Reso", &res_).percent();
//   P.add("osc.wave", "Wave", &wave_).names(kWaveNames, 4);
//
// Values are kept normalized (0..1) inside; get()/set() speak units. UI edits
// (set / nudge / reset) are main loop only. A workstation may keep writing its
// own fields directly: unmodulated pointer-bound params follow them (sync()).
#pragma once

#include "lab/lab.h"

namespace lab {
namespace kit {

constexpr ParamId kNoParam = -1;
constexpr int kMaxParams = 512;
constexpr int kMaxGroups = 32;
constexpr int kPadParams = 36;          // pads per page in the PARAM layer (3 rows)
constexpr int kPageMacro = 100;         // the Shell's own pages (after the workstation's)
constexpr int kPageMod = 101;

enum class Curve : uint8_t { kLinear, kExp, kInt, kEnum };

class Registry;

// Returned by Registry::add(): chain the settings; converts to the ParamId.
class Param {
public:
    Param& range(float lo, float hi);
    Param& exp();                                    // exponential: lo > 0 (Hz, ms, ratios)
    Param& integer();                                // whole numbers (int binding: implied)
    Param& names(const char* const* n, int count);   // enum 0..count-1 with labels
    Param& def(float v);                             // reset value (default: the bound value at add())
    Param& step(float s);                            // encoder step in units (0: 1 % of the range)
    Param& unit(const char* u);                      // shown after the value: "Hz", "ms", "dB"
    Param& percent();                                // 0..1 shown as 0..100 %
    Param& bipolar();                                // drawn from the centre (pan, detune)
    Param& page(int p);
    Param& group(const char* name, Rgb color = Rgb());
    Param& pad(int p);                               // fixed pad 0..35 on its page
    Param& noMod();                                  // never a modulation target
    ParamId id() const { return id_; }
    operator ParamId() const { return id_; }

private:
    friend class Registry;
    Registry* r_ = nullptr;
    ParamId id_ = kNoParam;
};

class Registry {
public:
    using Setter = void (*)(void* ctx, float value);

    // ---- registration (init) --------------------------------------------
    // Optional, before the first add(): the capacity (default kMaxParams).
    void reserve(int capacity);
    Param& add(const char* key, const char* name, float* value);
    Param& add(const char* key, const char* name, int* value);
    // A setter binding runs in the audio interrupt once per block when the
    // value changed (and every block while modulated): keep it short.
    Param& add(const char* key, const char* name, Setter set, void* ctx, float initial);
    // Defaults for the following add() calls.
    void setPage(int page) { curPage_ = int16_t(page); }
    int currentPage() const { return curPage_; }
    int currentGroup() const { return curGroup_; }
    void restoreGroup(int g) { curGroup_ = int8_t(g); }
    void setGroup(const char* name, Rgb color = Rgb());
    int group(const char* name, Rgb color = Rgb());   // find or create; -1 if full

    // ---- lookup -----------------------------------------------------------
    int count() const { return count_; }
    bool valid(ParamId id) const { return id >= 0 && id < count_; }
    ParamId find(const char* key) const;
    ParamId findHash(uint32_t keyHash) const;
    static uint32_t hash(const char* key);
    const char* name(ParamId id) const;
    const char* unit(ParamId id) const;
    int page(ParamId id) const;
    int groupOf(ParamId id) const;
    const char* groupName(int g) const;
    Rgb groupColor(int g) const;
    Rgb color(ParamId id) const { return groupColor(groupOf(id)); }
    Curve curve(ParamId id) const;
    bool isBipolar(ParamId id) const;
    bool modulatable(ParamId id) const;
    int enumCount(ParamId id) const;   // 0 = not an enum
    float minimum(ParamId id) const;
    float maximum(ParamId id) const;
    // Params of a page in pad order (fixed pads first, then registration
    // order). out[i] = param on pad i, kNoParam = empty pad. Returns the
    // highest used pad + 1 (0..kPadParams); `all` may be null (pads only).
    int padLayout(int page, ParamId out[kPadParams]) const;
    // All params of a page in registration order (the MAP lists them).
    int onPage(int page, ParamId* out, int cap) const;

    // ---- values (main loop) ------------------------------------------------
    float get(ParamId id) const;               // base, in units
    float norm(ParamId id) const;              // base, 0..1
    float effective(ParamId id) const;         // after modulation, in units
    float effectiveNorm(ParamId id) const;     // after modulation, 0..1
    float defaultValue(ParamId id) const;
    void set(ParamId id, float value, bool hud = true);
    void setNorm(ParamId id, float n, bool hud = true);
    void nudge(ParamId id, int delta, bool fine = false, bool hud = true);
    void reset(ParamId id, bool hud = true);
    // Text of the value ("1.20k Hz", "Saw", "45 %"); effective = after modulation.
    void format(ParamId id, char* buf, int cap, bool effective = false) const;
    void formatNorm(ParamId id, float n, char* buf, int cap) const;
    // Re-read unmodulated pointer bindings (the workstation wrote its fields).
    void sync();

    // ---- save / load ----------------------------------------------------------
    // One versioned chunk: base values (by key hash), the 8 macros (names,
    // values), the mod routes and the MIDI learn table. Unknown keys are
    // skipped on load. saveChunk returns the bytes written (0 = no room).
    uint32_t chunkSize() const;
    uint32_t saveChunk(uint8_t* buf, uint32_t cap) const;
    bool loadChunk(const uint8_t* buf, uint32_t len);
    // The chunk as a file on the card (main loop; false without a card).
    bool saveFile(IStorage* fs, const char* path) const;
    bool loadFile(IStorage* fs, const char* path);

    // ---- audio side (the Shell) --------------------------------------------------
    // Writes changed and modulated params to their bindings. Called by the
    // Shell once per audio block before render(); tests may call it directly.
    void applyBlock();

    // ---- internals shared with the kit ------------------------------------------
    struct Meta {
        uint32_t key = 0;
        const char* name = "";
        const char* unit = "";
        const char* const* names = nullptr;
        float def = 0.0f, step = 0.0f;
        int16_t page = 0;
        int8_t group = 0;
        int8_t pad = -1;
        uint8_t flags = 0;
        uint8_t mods = 0;   // routes targeting this param
    };
    struct Bind {           // what the audio block needs, kept compact
        float lo = 0.0f, hi = 1.0f, k = 1.0f;   // k: hi - lo, or log2(hi / lo) for kExp
        void* ptr = nullptr;
        Setter set = nullptr;
        uint8_t curve = 0, kind = 0;
    };
    enum Flags : uint8_t { kPercent = 1, kBipolar = 2, kNoMod = 4, kHasDef = 8 };
    enum Kind : uint8_t { kBindNone, kBindFloat, kBindInt, kBindSetter };
    void attach(Kit* kit, Arena* arena) { kit_ = kit; arena_ = arena; }
    Meta& meta(ParamId id) { return meta_[id]; }
    const Bind& binding(ParamId id) const { return bind_[id]; }
    const Meta& meta(ParamId id) const { return meta_[id]; }
    float* baseArray() { return base_; }
    float* effArray() { return eff_; }
    void setModCount(ParamId id, int n);
    float toNorm(ParamId id, float value) const;
    float fromNorm(ParamId id, float n) const;
    // Called by the audio block (Mod plan): write param `id` at normalized n.
    void writeNorm(ParamId id, float n);
    // Set from a Shell UI (pads, MAP, macro page, MIDI learn): notifies the workstation.
    void setNormFromShell(ParamId id, float n, bool hud = true);
    void applyNow();   // main loop: write all pending values now (tests, no audio)
    bool reserved() const { return meta_ != nullptr; }

private:
    friend class Param;
    Param& addCommon(const char* key, const char* name, uint8_t kind, void* ptr, Setter set, void* ctx, float initial);
    void markDirty(ParamId id);
    void changed(ParamId id, bool hud, bool fromShell);
    float readBinding(ParamId id) const;
    float regValue(ParamId id) const;   // registration: the bound value (setters: the default)
    void writeNow(ParamId id, float n);

    Kit* kit_ = nullptr;
    Arena* arena_ = nullptr;
    Meta* meta_ = nullptr;
    Bind* bind_ = nullptr;
    void** setCtx_ = nullptr;
    float* base_ = nullptr;   // normalized, written by the main loop
    float* eff_ = nullptr;    // normalized, written by the audio block (or set() when unmodulated)
    uint32_t* dirty_ = nullptr;
    int cap_ = kMaxParams, count_ = 0;
    int16_t curPage_ = 0;
    int8_t curGroup_ = 0;
    struct Group {
        const char* name;
        Rgb color;
    };
    Group groups_[kMaxGroups] = {};
    int groupCount_ = 0;
    Param cursor_;
};

}  // namespace kit
}  // namespace lab
