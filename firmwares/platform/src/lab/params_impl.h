// LABKIT v2 -- Registry implementation. Compiled inside shell.cpp (the Shell's
// translation unit), so every build that links src/lab/shell.cpp -- firmware,
// simulator, host tests, the QEMU bench and PLATFORM's station apps -- gets
// LABKIT without new source files in its makefiles.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/lab.h"

namespace lab {
namespace kit {

namespace {

__attribute__((always_inline)) inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
__attribute__((always_inline)) inline int roundi(float v) { return int(v >= 0.0f ? v + 0.5f : v - 0.5f); }

// 2^x for x >= 0 (exp params in the audio block): bits + a cubic, ~1e-4 rel.
__attribute__((always_inline)) inline float fastExp2(float x) {
    if (x > 60.0f) x = 60.0f;
    const int i = int(x);
    const float f = x - float(i);
    const float p = 1.0f + f * (0.6951786f + f * (0.2261573f + f * 0.0786640f));
    union {
        uint32_t u;
        float f;
    } s;
    s.u = uint32_t(i + 127) << 23;
    return p * s.f;
}

// Little-endian chunk writer / reader (file or memory).
struct Out {
    uint8_t* buf = nullptr;
    uint32_t cap = 0, n = 0;
    IStorage* fs = nullptr;
    IStorage::Handle h = -1;
    uint8_t stage[128];
    uint32_t staged = 0;
    bool ok = true;
    void bytes(const void* p, uint32_t len) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        if (fs) {
            for (uint32_t i = 0; i < len; ++i) {
                stage[staged++] = b[i];
                if (staged == sizeof stage) flush();
            }
        } else if (buf) {
            if (n + len > cap) ok = false;
            else std::memcpy(buf + n, b, len);
        }
        n += len;
    }
    void flush() {
        if (fs && staged) {
            if (fs->write(h, stage, staged) != int32_t(staged)) ok = false;
            staged = 0;
        }
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        bytes(b, 2);
    }
    void u32(uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        bytes(b, 4);
    }
    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
};

struct In {
    const uint8_t* p;
    uint32_t len, at = 0;
    bool ok = true;
    bool need(uint32_t k) {
        if (at + k > len) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? p[at++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = uint16_t(p[at] | (p[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        const uint32_t v = uint32_t(p[at]) | (uint32_t(p[at + 1]) << 8) | (uint32_t(p[at + 2]) << 16) |
                           (uint32_t(p[at + 3]) << 24);
        at += 4;
        return v;
    }
    float f32() {
        const uint32_t u = u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    void bytes(void* d, uint32_t k) {
        if (need(k)) std::memcpy(d, p + at, k);
        at += k;
    }
};

const Rgb kGroupPalette[12] = {Rgb(0x2EC4F0), Rgb(0xFF8A3D), Rgb(0x6BE36B), Rgb(0xE45BD8), Rgb(0xF2D230), Rgb(0x6C8CFF),
                               Rgb(0xFF5A6A), Rgb(0x3DDBB0), Rgb(0xA77BFF), Rgb(0xB8F23C), Rgb(0xFF7FB0), Rgb(0xD9B38C)};

constexpr uint32_t kChunkMagic = 0x32564B4Cu;   // "LKV2"
constexpr uint16_t kChunkVersion = 1;

}  // namespace

// ---- Param (fluent settings) -------------------------------------------------
Param& Param::range(float lo, float hi) {
    if (r_ && r_->valid(id_)) {
        Registry::Bind& b = r_->bind_[id_];
        b.lo = lo;
        b.hi = hi > lo ? hi : lo + 1.0f;
        b.k = (b.curve == uint8_t(Curve::kExp) && b.lo > 0.0f) ? log2f(b.hi / b.lo) : b.hi - b.lo;
        r_->writeNow(id_, r_->toNorm(id_, r_->regValue(id_)));
    }
    return *this;
}
Param& Param::exp() {
    if (r_ && r_->valid(id_)) {
        Registry::Bind& b = r_->bind_[id_];
        if (b.lo <= 0.0f) b.lo = 0.001f;
        b.curve = uint8_t(Curve::kExp);
        b.k = log2f(b.hi / b.lo);
        r_->writeNow(id_, r_->toNorm(id_, r_->regValue(id_)));
    }
    return *this;
}
Param& Param::integer() {
    if (r_ && r_->valid(id_)) {
        Registry::Bind& b = r_->bind_[id_];
        b.curve = uint8_t(Curve::kInt);
        b.k = b.hi - b.lo;
        r_->writeNow(id_, r_->toNorm(id_, r_->regValue(id_)));
    }
    return *this;
}
Param& Param::names(const char* const* n, int count) {
    if (r_ && r_->valid(id_) && count > 0) {
        Registry::Bind& b = r_->bind_[id_];
        b.curve = uint8_t(Curve::kEnum);
        b.lo = 0.0f;
        b.hi = float(count > 1 ? count - 1 : 1);
        b.k = b.hi - b.lo;
        r_->meta_[id_].names = n;
        r_->meta_[id_].step = count > 1 ? 0.0f : 1.0f;
        r_->writeNow(id_, r_->toNorm(id_, r_->regValue(id_)));
    }
    return *this;
}
Param& Param::def(float v) {
    if (r_ && r_->valid(id_)) {
        r_->meta_[id_].def = v;
        r_->meta_[id_].flags |= Registry::kHasDef;
        if (r_->bind_[id_].kind == Registry::kBindSetter) r_->writeNow(id_, r_->toNorm(id_, v));
    }
    return *this;
}
Param& Param::step(float s) {
    if (r_ && r_->valid(id_)) r_->meta_[id_].step = s;
    return *this;
}
Param& Param::unit(const char* u) {
    if (r_ && r_->valid(id_)) r_->meta_[id_].unit = u ? u : "";
    return *this;
}
Param& Param::percent() {
    if (r_ && r_->valid(id_)) r_->meta_[id_].flags |= Registry::kPercent;
    return *this;
}
Param& Param::bipolar() {
    if (r_ && r_->valid(id_)) r_->meta_[id_].flags |= Registry::kBipolar;
    return *this;
}
Param& Param::page(int p) {
    if (r_ && r_->valid(id_)) r_->meta_[id_].page = int16_t(p);
    return *this;
}
Param& Param::group(const char* name, Rgb color) {
    if (r_ && r_->valid(id_)) {
        const int g = r_->group(name, color);
        if (g >= 0) r_->meta_[id_].group = int8_t(g);
    }
    return *this;
}
Param& Param::pad(int p) {
    if (r_ && r_->valid(id_) && p >= 0 && p < kPadParams) r_->meta_[id_].pad = int8_t(p);
    return *this;
}
Param& Param::noMod() {
    if (r_ && r_->valid(id_)) r_->meta_[id_].flags |= Registry::kNoMod;
    return *this;
}

// ---- registration ----------------------------------------------------------------
void Registry::reserve(int capacity) {
    if (meta_ || !arena_) return;
    cap_ = capacity < 8 ? 8 : (capacity > 4096 ? 4096 : capacity);
    meta_ = arena_->make<Meta>(uint32_t(cap_));
    bind_ = arena_->make<Bind>(uint32_t(cap_));
    setCtx_ = static_cast<void**>(arena_->alloc(uint32_t(sizeof(void*)) * uint32_t(cap_)));
    base_ = static_cast<float*>(arena_->alloc(4u * uint32_t(cap_)));
    eff_ = static_cast<float*>(arena_->alloc(4u * uint32_t(cap_)));
    dirty_ = static_cast<uint32_t*>(arena_->alloc(4u * uint32_t((cap_ + 31) / 32)));
    if (!meta_ || !bind_ || !setCtx_ || !base_ || !eff_ || !dirty_) {
        meta_ = nullptr;
        cap_ = 0;
    }
}

uint32_t Registry::hash(const char* key) {
    uint32_t h = 2166136261u;
    for (const char* p = key; p && *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
    return h;
}

int Registry::group(const char* name, Rgb color) {
    if (!name) name = "";
    for (int i = 0; i < groupCount_; ++i)
        if (std::strcmp(groups_[i].name, name) == 0) {
            if (color.r | color.g | color.b) groups_[i].color = color;
            return i;
        }
    if (groupCount_ >= kMaxGroups) return -1;
    groups_[groupCount_].name = name;
    groups_[groupCount_].color = (color.r | color.g | color.b) ? color : kGroupPalette[groupCount_ % 12];
    return groupCount_++;
}

void Registry::setGroup(const char* name, Rgb color) {
    const int g = group(name, color);
    curGroup_ = int8_t(g < 0 ? 0 : g);
}

Param& Registry::add(const char* key, const char* name, float* value) {
    return addCommon(key, name, kBindFloat, value, nullptr, nullptr, value ? *value : 0.0f);
}
Param& Registry::add(const char* key, const char* name, int* value) {
    Param& p = addCommon(key, name, kBindInt, value, nullptr, nullptr, value ? float(*value) : 0.0f);
    if (valid(p.id_)) {
        bind_[p.id_].curve = uint8_t(Curve::kInt);
        bind_[p.id_].hi = float(value && *value > 1 ? *value : 1);
        bind_[p.id_].k = bind_[p.id_].hi;
        // Only the base: never write the field through this provisional 0..max range
        // (a negative int, e.g. an octave of -3, would be clamped before range() runs).
        base_[p.id_] = eff_[p.id_] = clamp01(toNorm(p.id_, regValue(p.id_)));
    }
    return p;
}
Param& Registry::add(const char* key, const char* name, Setter set, void* ctx, float initial) {
    return addCommon(key, name, kBindSetter, nullptr, set, ctx, initial);
}

Param& Registry::addCommon(const char* key, const char* name, uint8_t kind, void* ptr, Setter set, void* ctx,
                           float initial) {
    cursor_.r_ = this;
    cursor_.id_ = kNoParam;
    if (!meta_) reserve(cap_);
    if (!meta_ || count_ >= cap_) return cursor_;
    const ParamId id = ParamId(count_++);
    Meta& m = meta_[id];
    m = Meta();
    m.key = hash(key);
    m.name = name ? name : "";
    m.page = curPage_;
    m.group = curGroup_;
    m.def = initial;
    Bind& b = bind_[id];
    b = Bind();
    b.kind = kind;
    b.ptr = ptr;
    b.set = set;
    setCtx_[id] = ctx;
    if (groupCount_ == 0) group("", Rgb());
    const float n = clamp01(initial);
    base_[id] = eff_[id] = n;
    if (kit_ && !kit_->registeringShell) ++kit_->wsParams;
    cursor_.id_ = id;
    return cursor_;
}

// ---- lookup ----------------------------------------------------------------------------
ParamId Registry::find(const char* key) const { return findHash(hash(key)); }
ParamId Registry::findHash(uint32_t h) const {
    for (int i = 0; i < count_; ++i)
        if (meta_[i].key == h) return ParamId(i);
    return kNoParam;
}
const char* Registry::name(ParamId id) const { return valid(id) ? meta_[id].name : ""; }
const char* Registry::unit(ParamId id) const { return valid(id) ? meta_[id].unit : ""; }
int Registry::page(ParamId id) const { return valid(id) ? meta_[id].page : -1; }
int Registry::groupOf(ParamId id) const { return valid(id) ? meta_[id].group : 0; }
const char* Registry::groupName(int g) const { return (g >= 0 && g < groupCount_) ? groups_[g].name : ""; }
Rgb Registry::groupColor(int g) const { return (g >= 0 && g < groupCount_) ? groups_[g].color : kGroupPalette[0]; }
Curve Registry::curve(ParamId id) const { return valid(id) ? Curve(bind_[id].curve) : Curve::kLinear; }
bool Registry::isBipolar(ParamId id) const { return valid(id) && (meta_[id].flags & kBipolar); }
bool Registry::modulatable(ParamId id) const { return valid(id) && !(meta_[id].flags & kNoMod); }
int Registry::enumCount(ParamId id) const {
    return (valid(id) && bind_[id].curve == uint8_t(Curve::kEnum)) ? int(bind_[id].hi) + 1 : 0;
}
float Registry::minimum(ParamId id) const { return valid(id) ? bind_[id].lo : 0.0f; }
float Registry::maximum(ParamId id) const { return valid(id) ? bind_[id].hi : 1.0f; }

int Registry::padLayout(int pg, ParamId out[kPadParams]) const {
    for (int i = 0; i < kPadParams; ++i) out[i] = kNoParam;
    int used = 0;
    for (int i = 0; i < count_; ++i)
        if (meta_[i].page == pg && meta_[i].pad >= 0 && out[meta_[i].pad] == kNoParam) {
            out[meta_[i].pad] = ParamId(i);
            if (meta_[i].pad + 1 > used) used = meta_[i].pad + 1;
        }
    int slot = 0;
    for (int i = 0; i < count_; ++i) {
        if (meta_[i].page != pg) continue;
        if (meta_[i].pad >= 0 && out[meta_[i].pad] == ParamId(i)) continue;
        while (slot < kPadParams && out[slot] != kNoParam) ++slot;
        if (slot >= kPadParams) break;
        out[slot] = ParamId(i);
        if (slot + 1 > used) used = slot + 1;
    }
    return used;
}

int Registry::onPage(int pg, ParamId* out, int cap) const {
    int n = 0;
    for (int i = 0; i < count_ && n < cap; ++i)
        if (meta_[i].page == pg) out[n++] = ParamId(i);
    return n;
}

// ---- conversions --------------------------------------------------------------------------
float Registry::toNorm(ParamId id, float v) const {
    if (!valid(id)) return 0.0f;
    const Bind& b = bind_[id];
    if (b.curve == uint8_t(Curve::kExp)) {
        if (v <= b.lo) return 0.0f;
        return b.k > 0.0f ? clamp01(log2f(v / b.lo) / b.k) : 0.0f;
    }
    const float span = b.hi - b.lo;
    if (span <= 0.0f) return 0.0f;
    float n = clamp01((v - b.lo) / span);
    if (b.curve == uint8_t(Curve::kInt) || b.curve == uint8_t(Curve::kEnum)) n = float(roundi(n * span)) / span;
    return n;
}

float Registry::fromNorm(ParamId id, float n) const {
    if (!valid(id)) return 0.0f;
    const Bind& b = bind_[id];
    n = clamp01(n);
    switch (Curve(b.curve)) {
        case Curve::kExp: return b.lo * exp2f(n * b.k);
        case Curve::kInt:
        case Curve::kEnum: return float(roundi(b.lo + n * (b.hi - b.lo)));
        default: return b.lo + n * (b.hi - b.lo);
    }
}

float Registry::regValue(ParamId id) const {
    return bind_[id].kind == kBindSetter ? meta_[id].def : readBinding(id);
}

float Registry::readBinding(ParamId id) const {
    const Bind& b = bind_[id];
    if (b.kind == kBindFloat && b.ptr) return *static_cast<const float*>(b.ptr);
    if (b.kind == kBindInt && b.ptr) return float(*static_cast<const int*>(b.ptr));
    return fromNorm(id, base_[id]);
}

// Main loop: the base (and, unmodulated, the effective value and a pointer
// binding) right away; the audio block writes it again (setters run there).
void Registry::writeNow(ParamId id, float n) {
    if (!valid(id)) return;
    n = clamp01(n);
    const Bind& b = bind_[id];
    if (b.curve == uint8_t(Curve::kInt) || b.curve == uint8_t(Curve::kEnum)) {
        const float span = b.hi - b.lo;
        if (span > 0.0f) n = float(roundi(n * span)) / span;
    }
    base_[id] = n;
    if (meta_[id].mods == 0) {
        eff_[id] = n;
        const float v = fromNorm(id, n);
        if (b.kind == kBindFloat && b.ptr) *static_cast<float*>(b.ptr) = v;
        else if (b.kind == kBindInt && b.ptr) *static_cast<int*>(b.ptr) = roundi(v);
    }
    markDirty(id);
}

void Registry::markDirty(ParamId id) {
    if (!valid(id)) return;
    __atomic_fetch_or(&dirty_[id >> 5], 1u << (id & 31), __ATOMIC_RELAXED);
}

void Registry::setModCount(ParamId id, int n) {
    if (!valid(id)) return;
    meta_[id].mods = uint8_t(n < 0 ? 0 : (n > 255 ? 255 : n));
    if (n == 0) writeNow(id, base_[id]);
}

// ---- values ---------------------------------------------------------------------------------
float Registry::get(ParamId id) const { return valid(id) ? fromNorm(id, base_[id]) : 0.0f; }
float Registry::norm(ParamId id) const { return valid(id) ? base_[id] : 0.0f; }
float Registry::effective(ParamId id) const { return valid(id) ? fromNorm(id, eff_[id]) : 0.0f; }
float Registry::effectiveNorm(ParamId id) const { return valid(id) ? eff_[id] : 0.0f; }
float Registry::defaultValue(ParamId id) const { return valid(id) ? meta_[id].def : 0.0f; }

void Registry::changed(ParamId id, bool hud, bool fromShell) {
    if (!kit_) return;
    if (hud) kit_->nav.hud(id);
    if (fromShell && kit_->ws && !kit_->registeringShell) kit_->ws->navParamChanged(id);
}

void Registry::set(ParamId id, float value, bool hud) {
    if (!valid(id)) return;
    writeNow(id, toNorm(id, value));
    changed(id, hud, false);
}
void Registry::setNorm(ParamId id, float n, bool hud) {
    if (!valid(id)) return;
    writeNow(id, n);
    changed(id, hud, false);
}
void Registry::setNormFromShell(ParamId id, float n, bool hud) {
    if (!valid(id)) return;
    writeNow(id, n);
    changed(id, hud, true);
}

void Registry::nudge(ParamId id, int delta, bool fine, bool hud) {
    if (!valid(id) || !delta) return;
    const Meta& m = meta_[id];
    const Bind& b = bind_[id];
    float n = base_[id];
    if (b.curve == uint8_t(Curve::kInt) || b.curve == uint8_t(Curve::kEnum)) {
        const float st = m.step > 0.0f ? m.step : 1.0f;
        const float v = fromNorm(id, n) + float(delta) * st;
        n = toNorm(id, v < b.lo ? b.lo : (v > b.hi ? b.hi : v));
    } else if (m.step > 0.0f && b.curve == uint8_t(Curve::kLinear)) {
        const float v = fromNorm(id, n) + float(delta) * m.step * (fine ? 0.1f : 1.0f);
        n = toNorm(id, v);
    } else {
        n += float(delta) * (fine ? 0.002f : 0.01f);
    }
    if (kit_ && !kit_->inShellUi) {
        writeNow(id, n);
        changed(id, hud, false);
    } else {
        setNormFromShell(id, n, hud);
    }
}

void Registry::reset(ParamId id, bool hud) {
    if (!valid(id)) return;
    if (kit_ && kit_->inShellUi) setNormFromShell(id, toNorm(id, meta_[id].def), hud);
    else set(id, meta_[id].def, hud);
}

void Registry::formatNorm(ParamId id, float n, char* buf, int cap) const {
    if (!buf || cap <= 0) return;
    buf[0] = 0;
    if (!valid(id)) return;
    const Meta& m = meta_[id];
    const Bind& b = bind_[id];
    const float v = fromNorm(id, n);
    const char* u = m.unit ? m.unit : "";
    const char* sp = (*u && std::strcmp(u, "%") != 0) ? " " : "";
    if (b.curve == uint8_t(Curve::kEnum)) {
        const int i = roundi(v);
        if (m.names) copyStr(buf, uint32_t(cap), m.names[i < 0 ? 0 : i]);
        else std::snprintf(buf, size_t(cap), "%d", i);
        return;
    }
    if (b.curve == uint8_t(Curve::kInt)) {
        std::snprintf(buf, size_t(cap), (m.flags & kBipolar) && v > 0.0f ? "+%d%s%s" : "%d%s%s", roundi(v), sp, u);
        return;
    }
    if (m.flags & kPercent) {
        const float pc = v * 100.0f;
        std::snprintf(buf, size_t(cap), (m.flags & kBipolar) && pc > 0.5f ? "+%d%%" : "%d%%", roundi(pc));
        return;
    }
    const float a = fabsf(v);
    const char* sign = ((m.flags & kBipolar) && v > 0.0f) ? "+" : "";
    if (a >= 10000.0f) std::snprintf(buf, size_t(cap), "%s%.1fk%s%s", sign, double(v * 0.001f), sp, u);
    else if (a >= 1000.0f) std::snprintf(buf, size_t(cap), "%s%.2fk%s%s", sign, double(v * 0.001f), sp, u);
    else if (a >= 100.0f) std::snprintf(buf, size_t(cap), "%s%d%s%s", sign, roundi(v), sp, u);
    else if (a >= 10.0f) std::snprintf(buf, size_t(cap), "%s%.1f%s%s", sign, double(v), sp, u);
    else std::snprintf(buf, size_t(cap), "%s%.2f%s%s", sign, double(v), sp, u);
}

void Registry::format(ParamId id, char* buf, int cap, bool effective) const {
    if (!valid(id)) {
        if (buf && cap > 0) buf[0] = 0;
        return;
    }
    formatNorm(id, effective ? eff_[id] : base_[id], buf, cap);
}

void Registry::sync() {
    for (int i = 0; i < count_; ++i) {
        const Bind& b = bind_[i];
        if (meta_[i].mods || !b.ptr || (b.kind != kBindFloat && b.kind != kBindInt)) continue;
        if (dirty_[i >> 5] & (1u << (i & 31))) continue;
        const float v = readBinding(ParamId(i));
        const float cur = fromNorm(ParamId(i), base_[i]);
        const float tol = 1e-5f * (1.0f + fabsf(v));
        if (fabsf(v - cur) > tol) base_[i] = eff_[i] = toNorm(ParamId(i), v);
    }
}

// ---- audio -------------------------------------------------------------------------------------
LABKIT_HOT void Registry::writeNorm(ParamId id, float n) {
    const Bind& b = bind_[id];
    float v;
    switch (Curve(b.curve)) {
        case Curve::kExp: v = b.lo * fastExp2(n * b.k); break;
        case Curve::kInt:
        case Curve::kEnum: v = float(roundi(b.lo + n * b.k)); break;
        default: v = b.lo + n * b.k; break;
    }
    switch (b.kind) {
        case kBindFloat: *static_cast<float*>(b.ptr) = v; break;
        case kBindInt: *static_cast<int*>(b.ptr) = roundi(v); break;
        case kBindSetter: b.set(setCtx_[id], v); break;
        default: break;
    }
}

LABKIT_HOT void Registry::applyBlock() {
    if (!dirty_) return;
    // At most kWritesPerBlock params per block (a preset load marks all of
    // them): the rest stay marked for the next blocks, so a block never pays
    // for hundreds of writes at once.
    constexpr int kWritesPerBlock = 32;
    int budget = kWritesPerBlock;
    const int words = (count_ + 31) >> 5;
    for (int w = 0; w < words && budget > 0; ++w) {
        if (!dirty_[w]) continue;
        uint32_t bits = __atomic_exchange_n(&dirty_[w], 0u, __ATOMIC_ACQ_REL);
        while (bits && budget > 0) {
            const int id = (w << 5) + __builtin_ctz(bits);
            bits &= bits - 1;
            if (meta_[id].mods) continue;   // the mod plan writes it
            const float n = base_[id];
            eff_[id] = n;
            writeNorm(ParamId(id), n);
            --budget;
        }
        if (bits) __atomic_fetch_or(&dirty_[w], bits, __ATOMIC_RELAXED);   // the rest: next block
    }
}

void Registry::applyNow() {
    for (int i = 0; i < (count_ + 31) / 32 + kMaxParams / 32; ++i) applyBlock();
}

// ---- save / load ------------------------------------------------------------------------------------
namespace {
void writeChunk(const Registry& r, const Kit& k, Out& o) {
    o.u32(kChunkMagic);
    o.u16(kChunkVersion);
    o.u16(0);
    o.u16(uint16_t(r.count()));
    for (int i = 0; i < r.count(); ++i) {
        o.u32(r.meta(ParamId(i)).key);
        o.f32(r.get(ParamId(i)));
    }
    o.u8(kMacros);
    for (int m = 0; m < kMacros; ++m) {
        char nm[12] = {};
        copyStr(nm, sizeof nm, k.mod.macroName(m));
        o.bytes(nm, sizeof nm);
    }
    o.u16(uint16_t(k.mod.routeCount()));
    for (int i = 0; i < k.mod.routeCount(); ++i) {
        const Route& rt = k.mod.routeAt(i);
        const uint32_t src = rt.src < kSrcWs ? uint32_t(rt.src) : (Registry::hash(k.mod.sourceName(rt.src)) | 0x100u);
        o.u32(src);
        o.u32(r.meta(rt.param).key);
        o.f32(rt.depth);
        o.f32(rt.lo);
        o.f32(rt.hi);
        o.u8(rt.window);
        o.u8(0);
        o.u16(0);
    }
    const int nl = k.nav.learnCount();
    o.u16(uint16_t(nl));
    for (int i = 0; i < Nav::kMaxLearn; ++i) {
        const Nav::Learn& l = k.nav.learn_[i];
        if (!l.status || !r.valid(l.param)) continue;
        o.u8(l.status);
        o.u8(l.cc);
        o.u16(0);
        o.u32(r.meta(l.param).key);
    }
}
}  // namespace

uint32_t Registry::chunkSize() const {
    if (!kit_) return 0;
    Out o;
    writeChunk(*this, *kit_, o);
    return o.n;
}

uint32_t Registry::saveChunk(uint8_t* buf, uint32_t cap) const {
    if (!kit_ || !buf) return 0;
    Out o;
    o.buf = buf;
    o.cap = cap;
    writeChunk(*this, *kit_, o);
    return o.ok ? o.n : 0;
}

bool Registry::loadChunk(const uint8_t* buf, uint32_t len) {
    if (!kit_ || !buf) return false;
    In in{buf, len};
    if (in.u32() != kChunkMagic) return false;
    const uint16_t ver = in.u16();
    in.u16();
    if (!in.ok || ver == 0 || ver > kChunkVersion) return false;
    Kit& k = *kit_;
    k.mod.registerShellParams();   // macros / LFO settings exist before their values load
    const bool wasUi = k.inShellUi;
    k.inShellUi = true;
    const int n = in.u16();
    for (int i = 0; i < n && in.ok; ++i) {
        const uint32_t key = in.u32();
        const float v = in.f32();
        const ParamId id = findHash(key);
        if (valid(id) && in.ok && std::isfinite(v)) setNormFromShell(id, toNorm(id, v), false);
    }
    const int nm = in.u8();
    for (int m = 0; m < nm && in.ok; ++m) {
        char name[13] = {};
        in.bytes(name, 12);
        if (m < kMacros && in.ok && name[0]) k.mod.setMacroName(m, name);
    }
    k.mod.clearRoutes();
    const int nr = in.u16();
    for (int i = 0; i < nr && in.ok; ++i) {
        const uint32_t srcKey = in.u32();
        const uint32_t pkey = in.u32();
        const float depth = in.f32(), lo = in.f32(), hi = in.f32();
        const uint8_t window = in.u8();
        in.u8();
        in.u16();
        if (!in.ok) break;
        int src = -1;
        if (srcKey < uint32_t(kSrcWs)) src = int(srcKey);
        else
            for (int s = kSrcWs; s < k.mod.sourceCount(); ++s)
                if ((hash(k.mod.sourceName(s)) | 0x100u) == srcKey) src = s;
        const ParamId p = findHash(pkey);
        if (src < 0 || !valid(p)) continue;
        const int r = k.mod.route(src, p, depth);
        if (r >= 0 && window) k.mod.setWindow(r, true, lo, hi);
    }
    k.nav.clearLearn();
    const int nl = in.u16();
    for (int i = 0; i < nl && in.ok; ++i) {
        const uint8_t st = in.u8(), cc = in.u8();
        in.u16();
        const ParamId p = findHash(in.u32());
        if (in.ok && valid(p)) k.nav.learnAssign(st, cc, p);
    }
    k.inShellUi = wasUi;
    return in.ok;
}

bool Registry::saveFile(IStorage* fs, const char* path) const {
    if (!fs || !kit_ || !path) return false;
    const IStorage::Handle h = fs->open(path, IStorage::kWrite);
    if (h < 0) return false;
    Out o;
    o.fs = fs;
    o.h = h;
    writeChunk(*this, *kit_, o);
    o.flush();
    fs->close(h);
    return o.ok;
}

bool Registry::loadFile(IStorage* fs, const char* path) {
    if (!fs || !kit_ || !path || !arena_) return false;
    const IStorage::Handle h = fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    const int32_t size = fs->size(h);
    bool ok = false;
    // The chunk is small (12 B per param, 24 B per route): read it in pieces
    // into a stack buffer when it fits, else refuse (a corrupt file).
    uint8_t buf[8192];
    if (size > 0 && size <= int32_t(sizeof buf) && fs->read(h, buf, uint32_t(size)) == size)
        ok = loadChunk(buf, uint32_t(size));
    fs->close(h);
    return ok;
}

}  // namespace kit
}  // namespace lab
