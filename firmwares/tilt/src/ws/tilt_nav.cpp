// TILT x LABKIT v2: every parameter of the pattern in the registry (PARAM pads,
// MAP, macros, LFO / random routes, MIDI learn), the pages for MAP / HELP, the
// modulation sources, and the glue that keeps the registry and the pattern in
// step. Main loop only, except refSetter (the audio block calls it).
//
// The pattern is packed bytes (the file format), so every param is bound with
// a setter and a FieldRef (the byte offset in the Pattern): the setter writes
// the effective value into that byte once per block. The matrix weights are
// NOT registered: modulating them would fight mutation and memory.
//
// The 8 channels' engine knobs are "slots" (normalized 0..1, mapped to the
// engine's range when written): the selected channel's slots and settings sit
// on the CHANNELS page, the other channels' on hidden pages, so routes and
// macros keep working on every channel.
#pragma GCC optimize("Os")
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "ws/tilt_ws.h"

namespace tilt {

using lab::Rgb;
namespace kit = lab::kit;

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char* const kOffOn[2] = {"off", "ON"};
const char* const kTrigNames[10] = {"-", "1", "2", "3", "4", "5", "6", "7", "8", "M"};
const char* const kCbOn[2] = {"off", "on"};
constexpr uint32_t kStepsOff = uint32_t(offsetof(Pattern, steps));
constexpr uint32_t kCombosOff = uint32_t(offsetof(Pattern, combos));
constexpr uint32_t kChOff = uint32_t(offsetof(Pattern, ch));
constexpr uint32_t kMetaOff = uint32_t(offsetof(Pattern, ev.meta));
const Rgb kPageColors[kNavPages] = {Rgb(0xFF2E88), Rgb(0xB0309A), Rgb(0xFFC83A), Rgb(0x2EE6FF), Rgb(0x7B5CFF),
                                    Rgb(0xFF5FD2), Rgb(0x4DFFA6), Rgb(0xE8E2D0), Rgb(0xC6FF3D), Rgb(0x2E9AFF),
                                    Rgb(0x4DFFA6), Rgb(0xC6FF3D), Rgb(0x9AA6BF)};
}  // namespace

// ---- refs ------------------------------------------------------------------------------------
void Tilt::refSetter(void* ctx, float v) {
    const FieldRef& r = *static_cast<const FieldRef*>(ctx);
    uint8_t* b = reinterpret_cast<uint8_t*>(r.t->pat_) + r.off;
    const int iv = int(v < 0.0f ? v - 0.5f : v + 0.5f);
    switch (r.kind) {
        case kRefU8: *b = uint8_t(clampi(iv, 0, 255)); break;
        case kRefI8: *reinterpret_cast<int8_t*>(b) = int8_t(clampi(iv, -128, 127)); break;
        case kRefSlot: {
            Channel& c = r.t->pat_->ch[r.ch];
            const EngParam& ep = kEngineParams[c.type][r.slot];
            if (!ep.name) return;
            const float x = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            c.p[r.slot] = uint8_t(ep.min + int(x * float(ep.max - ep.min) + 0.5f));
            break;
        }
        case kRefPath: {
            Combo& cb = r.t->pat_->combos[r.ch];
            cb.path[r.slot] = uint8_t(clampi(iv, 0, kSteps));
            comboFixLen(cb);
            break;
        }
        default: break;
    }
}

namespace {
// The value of a ref in the registry's units, read from the pattern.
float refValue(const Pattern& p, uint8_t kind, uint16_t off, int ch, int slot) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p) + off;
    switch (kind) {
        case 1: return float(*reinterpret_cast<const int8_t*>(b));
        case 2: {
            const Channel& c = p.ch[ch];
            const EngParam& ep = kEngineParams[c.type][slot];
            if (!ep.name || ep.max <= ep.min) return 0.0f;
            return float(c.p[slot] - ep.min) / float(ep.max - ep.min);
        }
        default: return float(*b);
    }
}
}  // namespace

kit::ParamId Tilt::paramOf(const void* field) const {
    if (!offRef_ || !field) return kit::kNoParam;
    const intptr_t off = static_cast<const uint8_t*>(field) - reinterpret_cast<const uint8_t*>(pat_);
    if (off < 0 || off >= intptr_t(sizeof(Pattern))) return kit::kNoParam;
    const int r = offRef_[off];
    return r ? refId_[r - 1] : kit::kNoParam;
}

kit::ParamId Tilt::addRef(const char* key, const char* name, const void* field, uint8_t kind, uint8_t ch, uint8_t slot) {
    kit::Registry& R = ctx_->params();
    if (nRefs_ >= kMaxRefs) return kit::kNoParam;
    const uint16_t off = uint16_t(static_cast<const uint8_t*>(field) - reinterpret_cast<const uint8_t*>(pat_));
    FieldRef& r = refs_[nRefs_];
    r = FieldRef{this, off, kind, ch, slot};
    lab::copyStr(refName_[nRefs_], 16, name);
    const float init = refValue(*pat_, kind, off, ch, slot);
    kit::Param& p = R.add(key, refName_[nRefs_], &Tilt::refSetter, &r, init);
    refId_[nRefs_] = p.id();
    if (kind != kRefSlot) offRef_[off] = uint16_t(nRefs_ + 1);
    ++nRefs_;
    return p.id();
}

void Tilt::registerParams() {
    kit::Registry& R = ctx_->params();
    refs_ = ctx_->arena->make<FieldRef>(kMaxRefs);
    refId_ = ctx_->arena->make<kit::ParamId>(kMaxRefs);
    offRef_ = static_cast<uint16_t*>(ctx_->arena->alloc(sizeof(Pattern) * sizeof(uint16_t)));
    refName_ = static_cast<char(*)[16]>(ctx_->arena->alloc(kMaxRefs * 16));
    if (!refs_ || !refId_ || !offRef_ || !refName_) return;
    char key[24], name[24];
    // a kParams entry: range, names and modulation from the table
    auto addDef = [&](int id, int index, const char* nm) {
        const ParamDef& d = kParams[id];
        const uint8_t* f = reinterpret_cast<const uint8_t*>(pat_) + d.offset + d.stride * index;
        std::snprintf(key, sizeof key, "o%u", unsigned(d.offset + d.stride * index));
        const uint16_t off = uint16_t(f - reinterpret_cast<const uint8_t*>(pat_));
        if (nRefs_ >= kMaxRefs) return;
        FieldRef& r = refs_[nRefs_];
        r = FieldRef{this, off, kRefU8, 0, 0};
        lab::copyStr(refName_[nRefs_], 16, nm ? nm : d.name);
        kit::Param& p = R.add(key, refName_[nRefs_], &Tilt::refSetter, &r, float(*f));
        switch (d.fmt) {
            case kFmtNames:
                if (d.min == 0) p.names(d.names, d.max + 1).noMod();
                else p.range(float(d.min), float(d.max)).integer();
                break;
            case kFmtOnOff: p.names(kOffOn, 2).noMod(); break;
            case kFmtPct: p.range(float(d.min), float(d.max)).integer().unit("%"); break;
            case kFmtSigned: p.range(float(d.min), float(d.max)).integer().bipolar(); break;
            case kFmtMidiCh: p.range(float(d.min), float(d.max)).integer().noMod(); break;
            case kFmtBeats: p.range(float(d.min), float(d.max)).integer().unit("beats"); break;
            default: p.range(float(d.min), float(d.max)).integer(); break;
        }
        refId_[nRefs_] = p.id();
        offRef_[off] = uint16_t(nRefs_ + 1);
        ++nRefs_;
    };
    auto addField = [&](const char* nm, const void* f, uint8_t kind, float lo, float hi) -> kit::Param* {
        const uint16_t off = uint16_t(static_cast<const uint8_t*>(f) - reinterpret_cast<const uint8_t*>(pat_));
        std::snprintf(key, sizeof key, "o%u", unsigned(off));
        if (nRefs_ >= kMaxRefs) return nullptr;
        FieldRef& r = refs_[nRefs_];
        r = FieldRef{this, off, kind, 0, 0};
        lab::copyStr(refName_[nRefs_], 16, nm);
        kit::Param& p = R.add(key, refName_[nRefs_], &Tilt::refSetter, &r, refValue(*pat_, kind, off, 0, 0));
        p.range(lo, hi).integer();
        refId_[nRefs_] = p.id();
        offRef_[off] = uint16_t(nRefs_ + 1);
        ++nRefs_;
        return &p;
    };
    int ids[48];

    // ---- STEPS: velocity, trig % -----------------------------------------------------------
    R.setPage(kPgSteps);
    R.setGroup("VELO", Rgb(0xFFC83A));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Velo", s + 1);
        addField(name, &pat_->steps[s].vel, kRefU8, 1, 127);
    }
    R.setGroup("TRIG %", Rgb(0xFF2E88));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Trig%%", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].trigP, kRefU8, 0, 100)) p->unit("%");
    }
    // ---- LANES: the trig lanes, melody, locks ------------------------------------------------
    R.setPage(kNpLanes);
    R.setGroup("LANE", Rgb(0xC6FF3D));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Lane", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].trig, kRefU8, 0, 9)) p->names(kTrigNames, 10).noMod();
    }
    R.setGroup("MELODY", Rgb(0xFF5FD2));
    for (int i = 0, n = paramsOfPage(kPpMelody, ids, 48); i < n; ++i) addDef(ids[i], 0, nullptr);
    R.setGroup("LOCK", Rgb(0x9AA6BF));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Lock", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].lock, kRefU8, 0, 1)) p->names(kOffOn, 2).noMod();
    }
    // ---- TIMING: delay, delay % ---------------------------------------------------------------
    R.setPage(kNpTiming);
    R.setGroup("DELAY", Rgb(0x2EE6FF));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Delay", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].delay, kRefU8, 0, 23)) p->unit("/12");
    }
    R.setGroup("DELAY %", Rgb(0x2E9AFF));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Dly%%", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].delayP, kRefU8, 0, 100)) p->unit("%");
    }
    // ---- LENGTH: length, bumper ---------------------------------------------------------------
    R.setPage(kNpLength);
    R.setGroup("LENGTH", Rgb(0x4DFFA6));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Len", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].len, kRefU8, 1, 64)) p->unit("/4");
    }
    R.setGroup("BUMPER", Rgb(0xFFC83A));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Bump", s + 1);
        addField(name, &pat_->steps[s].bumper, kRefU8, 0, 4);
    }
    // ---- PITCH: degree, ramp ------------------------------------------------------------------
    R.setPage(kNpPitch);
    R.setGroup("DEGREE", Rgb(0xC6FF3D));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Degree", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].degree, kRefI8, -7, 21)) p->bipolar();
    }
    R.setGroup("RAMP", Rgb(0x2EE6FF));
    for (int s = 0; s < kSteps; ++s) {
        std::snprintf(name, sizeof name, "%d Ramp", s + 1);
        if (kit::Param* p = addField(name, &pat_->steps[s].ramp, kRefU8, 0, 16)) p->noMod();
    }
    // ---- TABLE --------------------------------------------------------------------------------
    R.setPage(kPgTable);
    {
        const char* last = nullptr;
        for (int i = 0, n = paramsOfPage(kPpTable, ids, 48); i < n; ++i) {
            const char* g = kParams[ids[i]].group;
            if (!last || std::strcmp(last, g)) R.setGroup(g, Rgb(0x2EE6FF));
            last = g;
            addDef(ids[i], 0, nullptr);
        }
    }
    // ---- EVOLVE: memory, mutation, meta, rules, randomizer, the meta matrix --------------------
    R.setPage(kPgEvolve);
    {
        const char* last = nullptr;
        for (int i = 0, n = paramsOfPage(kPpEvolve, ids, 48); i < n; ++i) {
            const char* g = kParams[ids[i]].group;
            if (!last || std::strcmp(last, g)) R.setGroup(g, Rgb(0x7B5CFF));
            last = g;
            addDef(ids[i], 0, nullptr);
        }
        R.setGroup("RANDOMIZER", Rgb(0xFF2E88));
        for (int i = 0, n = paramsOfPage(kPpRandom, ids, 48); i < n; ++i) addDef(ids[i], 0, nullptr);
        R.setGroup("META", Rgb(0x7B5CFF));
        for (int r = 0; r < kPages; ++r)
            for (int c = 0; c < kPages; ++c) {
                std::snprintf(name, sizeof name, "Meta %c>%c", 'A' + r, 'A' + c);
                if (kit::Param* p = addField(name, &pat_->ev.meta[r][c], kRefU8, 0, 100)) p->unit("%");
            }
    }
    // ---- COMBOS --------------------------------------------------------------------------------
    R.setPage(kPgCombos);
    for (int i = 0; i < kCombos; ++i) {
        std::snprintf(name, sizeof name, "COMBO %d", i + 1);
        R.setGroup(i == 0 ? "COMBO 1" : (i == 1 ? "COMBO 2" : (i == 2 ? "COMBO 3" : "COMBO 4")), Rgb(0xFF5FD2));
        Combo& cb = pat_->combos[i];
        std::snprintf(name, sizeof name, "%d On", i + 1);
        if (kit::Param* p = addField(name, &cb.on, kRefU8, 0, 1)) p->names(kCbOn, 2).noMod();
        for (int k = 0; k < kComboLen; ++k) {
            std::snprintf(name, sizeof name, "%d Path %d", i + 1, k + 1);
            if (kit::Param* p = addField(name, &cb.path[k], kRefU8, 0, 16)) p->noMod();
            refs_[nRefs_ - 1].kind = kRefPath;
            refs_[nRefs_ - 1].ch = uint8_t(i);
            refs_[nRefs_ - 1].slot = uint8_t(k);
        }
        std::snprintf(name, sizeof name, "%d Action", i + 1);
        if (kit::Param* p = addField(name, &cb.action, kRefU8, 0, kComboActions - 1))
            p->names(kComboActionNames, kComboActions).noMod();
        std::snprintf(name, sizeof name, "%d A", i + 1);
        if (kit::Param* p = addField(name, &cb.a, kRefU8, 0, 127)) p->noMod();
        std::snprintf(name, sizeof name, "%d B", i + 1);
        if (kit::Param* p = addField(name, &cb.b, kRefU8, 0, 127)) p->noMod();
    }
    // ---- CHANNELS: engine, its 8 knobs (slots), note and lane settings -------------------------
    int chIds[8];
    const int nCh = paramsOfPage(kPpChannel, chIds, 8);
    for (int c = 0; c < kCh; ++c) {
        R.setPage(kHiddenChPage + c);
        std::snprintf(name, sizeof name, "CH %d", c + 1);
        static const char* const kChGroups[kCh] = {"CH 1", "CH 2", "CH 3", "CH 4", "CH 5", "CH 6", "CH 7", "CH 8"};
        R.setGroup(kChGroups[c], chColor(c));
        std::snprintf(name, sizeof name, "%d Engine", c + 1);
        if (kit::Param* p = addField(name, &pat_->ch[c].type, kRefU8, 0, kEngines - 1))
            p->names(kEngineNames, kEngines).noMod();
        chRef_[c][0] = nRefs_ - 1;
        for (int k = 0; k < kParamsPerEngine; ++k) {
            std::snprintf(key, sizeof key, "c%d.s%d", c + 1, k);
            if (nRefs_ >= kMaxRefs) break;
            FieldRef& r = refs_[nRefs_];
            r = FieldRef{this, uint16_t(kChOff + sizeof(Channel) * uint32_t(c) + offsetof(Channel, p) + uint32_t(k)),
                         kRefSlot, uint8_t(c), uint8_t(k)};
            refName_[nRefs_][0] = 0;
            kit::Param& p = R.add(key, refName_[nRefs_], &Tilt::refSetter, &r, refValue(*pat_, kRefSlot, r.off, c, k));
            p.range(0.0f, 1.0f).percent().pad(1 + k);
            refId_[nRefs_] = p.id();
            offRef_[r.off] = uint16_t(nRefs_ + 1);
            slotRef_[c][k] = nRefs_;
            ++nRefs_;
        }
        for (int i = 0; i < nCh && i < 5; ++i) {
            std::snprintf(name, sizeof name, "%d %s", c + 1, kParams[chIds[i]].name);
            addDef(chIds[i], c, name);
            chRef_[c][1 + i] = nRefs_ - 1;
        }
    }
    // ---- MIX: level, pan, sends ------------------------------------------------------------------
    R.setPage(kPgMix);
    {
        int mix[16];
        const int nm = paramsOfPage(kPpMix, mix, 16);
        static const char* const kMixGroups[4] = {"LEVEL", "PAN", "SEND A", "SEND B"};
        static const Rgb kMixColors[4] = {Rgb(0x4DFFA6), Rgb(0xE8E2D0), Rgb(0x2EE6FF), Rgb(0x7B5CFF)};
        for (int k = 0; k < 4 && k < nm; ++k) {
            R.setGroup(kMixGroups[k], kMixColors[k]);
            for (int c = 0; c < kCh; ++c) {
                std::snprintf(name, sizeof name, "%d %s", c + 1, kParams[mix[k]].name);
                addDef(mix[k], c, name);
            }
        }
        // ---- FX / MUTES: the effects, mutes, solos, stems ----------------------------------------
        R.setPage(kNpFxMutes);
        R.setGroup("FX", Rgb(0xE8E2D0));
        for (int i = 0, n = paramsOfPage(kPpFx, ids, 48); i < n; ++i) addDef(ids[i], 0, nullptr);
        static const char* const kFlagGroups[3] = {"MUTE", "SOLO", "STEM"};
        static const Rgb kFlagColors[3] = {Rgb(0xFF4A3D), Rgb(0xFFC83A), Rgb(0x9AA6BF)};
        for (int k = 0; k < 3; ++k) {
            R.setGroup(kFlagGroups[k], kFlagColors[k]);
            const int id = mix[k == 0 ? 6 : (k == 1 ? 7 : 5)];
            for (int c = 0; c < kCh; ++c) {
                std::snprintf(name, sizeof name, "%d %s", c + 1, kParams[id].name);
                addDef(id, c, name);
            }
        }
    }
    R.setPage(0);
    // ---- modulation sources ------------------------------------------------------------------------
    kit::Mod& M = ctx_->mod();
    static const char* const kSrc[kModSources] = {"BALL", "BALL X", "BALL Y", "HEAT", "ENTROPY",
                                                  "BALLS", "TILT", "PAGE", "VELO"};
    for (int i = 0; i < kModSources; ++i) M.addSource(kSrc[i], &mod_[i], false);
    kitReady_ = true;
    for (int c = 0; c < kCh; ++c) refreshSlots(c);
    placeChannels();
}

// ---- keeping the registry and the pattern in step ----------------------------------------------
void Tilt::refreshSlots(int c) {
    if (!kitReady_) return;
    kit::Registry& R = ctx_->params();
    const Channel& ch = pat_->ch[c];
    for (int k = 0; k < kParamsPerEngine; ++k) {
        const int ri = slotRef_[c][k];
        const kit::ParamId id = refId_[ri];
        const EngParam& ep = kEngineParams[ch.type][k];
        if (ep.name) {
            std::snprintf(refName_[ri], 16, "%d %s", c + 1, ep.name);
            R.meta(id).flags = uint8_t(R.meta(id).flags & ~kit::Registry::kNoMod);
        } else {
            refName_[ri][0] = 0;
            R.meta(id).flags = uint8_t(R.meta(id).flags | kit::Registry::kNoMod);
        }
        const float n = refValue(*pat_, kRefSlot, refs_[ri].off, c, k);
        R.baseArray()[id] = n;
        if (!R.meta(id).mods) R.effArray()[id] = n;
    }
    placedFor_ = -1;
}

void Tilt::placeChannels() {
    if (!kitReady_ || placedFor_ == chSel_) return;
    kit::Registry& R = ctx_->params();
    for (int c = 0; c < kCh; ++c) {
        const bool sel = c == chSel_;
        for (int k = 0; k < 6; ++k)
            R.meta(refId_[chRef_[c][k]]).page = int16_t(sel ? kPgChannels : kHiddenChPage + c);
        for (int k = 0; k < kParamsPerEngine; ++k) {
            const int ri = slotRef_[c][k];
            const bool used = refName_[ri][0] != 0;
            R.meta(refId_[ri]).page = int16_t(sel && used ? kPgChannels : kHiddenChPage + c);
        }
    }
    placedFor_ = chSel_;
}

void Tilt::syncRegistry(bool all) {
    if (!kitReady_) return;
    kit::Registry& R = ctx_->params();
    float* base = R.baseArray();
    float* eff = R.effArray();
    for (int i = 0; i < nRefs_; ++i) {
        const kit::ParamId id = refId_[i];
        const bool mod = R.meta(id).mods != 0;
        if (mod && !all) continue;   // modulated: the registry owns the field
        const FieldRef& r = refs_[i];
        const float n = R.toNorm(id, refValue(*pat_, uint8_t(r.kind == kRefPath ? uint8_t(kRefU8) : r.kind), r.off, r.ch, r.slot));
        if (fabsf(base[id] - n) > 1e-5f) {
            if (mod) {
                R.setNorm(id, n, false);   // the audio block writes base + modulation
            } else {
                base[id] = n;
                eff[id] = n;
            }
        }
    }
}

int Tilt::fieldGet(const void* field) const {
    const kit::ParamId id = paramOf(field);
    const uint8_t* b = static_cast<const uint8_t*>(field);
    const int off = int(b - reinterpret_cast<const uint8_t*>(pat_));
    if (id != kit::kNoParam && ctx_->params().meta(id).mods) {
        const FieldRef& r = refs_[offRef_[off] - 1];
        const float v = ctx_->params().get(id);
        if (r.kind == kRefSlot) {
            const EngParam& ep = kEngineParams[pat_->ch[r.ch].type][r.slot];
            return ep.min + int(v * float(ep.max - ep.min) + 0.5f);
        }
        return int(v < 0.0f ? v - 0.5f : v + 0.5f);
    }
    // signed fields: the step degree and the edge properties
    if (off >= int(kStepsOff) && off < int(kStepsOff + sizeof pat_->steps) &&
        (off - int(kStepsOff)) % int(sizeof(Step)) == int(offsetof(Step, degree)))
        return *reinterpret_cast<const int8_t*>(b);
    return *b;
}

void Tilt::fieldSet(void* field, int v) {
    uint8_t* b = static_cast<uint8_t*>(field);
    const int off = int(b - reinterpret_cast<const uint8_t*>(pat_));
    const kit::ParamId id = paramOf(field);
    if (id == kit::kNoParam) {
        *b = uint8_t(v);
        return;
    }
    const FieldRef& r = refs_[offRef_[off] - 1];
    float units = float(v);
    if (r.kind == kRefSlot) {
        const EngParam& ep = kEngineParams[pat_->ch[r.ch].type][r.slot];
        if (!ep.name || ep.max <= ep.min) return;
        units = float(clampi(v, ep.min, ep.max) - ep.min) / float(ep.max - ep.min);
    }
    refSetter(&refs_[offRef_[off] - 1], units);   // at once (the screen and the next sync see it)
    ctx_->params().set(id, units, false);
}

void Tilt::kpSet(int kparam, int v, int index) {
    if (kparam < 0 || kparam >= kParamCount) return;
    const ParamDef& d = kParams[kparam];
    index = d.count > 1 ? clampi(index, 0, d.count - 1) : 0;
    uint8_t* f = reinterpret_cast<uint8_t*>(pat_) + d.offset + d.stride * index;
    fieldSet(f, clampi(v, d.min, d.max));
}

// ---- pages ---------------------------------------------------------------------------------------
int Tilt::navPages(kit::PageInfo* out, int cap) {
    static const char* const kNames[kNavPages] = {"GRAPH", "MATRIX", "STEPS", "TABLE",  "EVOLVE", "COMBOS", "CHANNEL",
                                                  "MIX",   "LANES",  "TIMING", "LENGTH", "PITCH", "FX"};
    static const char* const kHelp[kNavPages] = {
        "The ball on the 16 steps: arrows as thick as the weights, odds, entropy. Pads: perform.",
        "17 x 17 weights of the page played (row: where the ball is; D = drain). Insert: toggle / apply.",
        "Each step's channel, chances, timing, bumper, ramp and degree; the melody strip at the bottom.",
        "Clock, flippers (Note, Instr, FX1, FX2), tilt, ball save, gravity, multiball.",
        "Memory, mutation, the meta matrix A-D, rules and the randomizer with its locks.",
        "Four paths of 2-4 steps; landing them fires MULTIBALL, FILL, NOTE, CC, SHAKE or SCENE.",
        "The selected channel: engine and its 8 knobs, note, MIDI channel, follow, legato, choke.",
        "Levels, pan and the two sends of the 8 channels.",
        "Params only: the trig lane of each step, the melody, the randomizer locks (STEPS page).",
        "Params only: each step's delay and delay chance (STEPS page).",
        "Params only: each step's length and bumper hits (STEPS page).",
        "Params only: each step's scale degree and ramp (STEPS page).",
        "Params only: delay A, reverb B, master; mutes, solos, stems (MIX page).",
    };
    static char lines[kPgCount][8][40];
    static bool built = false;
    if (!built) {
        for (int p = 0; p < kPgCount; ++p)
            for (int k = 0; k < 8; ++k) {
                const char* sh = kPages_[p].shift[k];
                if (sh && sh[0]) std::snprintf(lines[p][k], 40, "%s;  Shift: %s", kPages_[k].name, sh);
                else std::snprintf(lines[p][k], 40, "%s", kPages_[k].name);
            }
        built = true;
    }
    int n = 0;
    for (; n < kNavPages && n < cap; ++n) {
        out[n].name = kNames[n];
        out[n].help = kHelp[n];
        out[n].color = kPageColors[n];
        const int src = n < kPgCount ? n : (n == kNpFxMutes ? int(kPgMix) : int(kPgSteps));
        for (int k = 0; k < 8; ++k) out[n].fkeys[k] = lines[src][k];
    }
    return n;
}

int Tilt::navPage() const {
    if (page_ == kPgSteps) {
        switch (stRow_) {
            case kRowTrig:
            case kRowLock:
            case kStepRows: return kNpLanes;
            case kRowDelay:
            case kRowDelayP: return kNpTiming;
            case kRowLen:
            case kRowBumper: return kNpLength;
            case kRowDegree:
            case kRowRamp: return kNpPitch;
            default: return kPgSteps;
        }
    }
    if (page_ == kPgMix && (mixFx_ || mixRow_ >= 5)) return kNpFxMutes;
    return page_;
}

void Tilt::navGoto(int page) {
    file_ = false;
    if (page >= 0 && page < kPgCount) {
        page_ = page;
        if (page == kPgMix) mixFx_ = false;
        return;
    }
    switch (page) {
        case kNpLanes: page_ = kPgSteps; stRow_ = kRowTrig; break;
        case kNpTiming: page_ = kPgSteps; stRow_ = kRowDelay; break;
        case kNpLength: page_ = kPgSteps; stRow_ = kRowLen; break;
        case kNpPitch: page_ = kPgSteps; stRow_ = kRowDegree; break;
        case kNpFxMutes: page_ = kPgMix; mixFx_ = true; break;
        default: break;
    }
}

void Tilt::navFocus(kit::ParamId id) {
    int ri = -1;
    for (int i = 0; i < nRefs_; ++i)
        if (refId_[i] == id) ri = i;
    if (ri < 0) return;
    const FieldRef& r = refs_[ri];
    const uint32_t off = r.off;
    if (r.kind == kRefSlot) {   // a channel knob
        chSel_ = r.ch;
        chField_ = 1 + r.slot;
        page_ = kPgChannels;
        return;
    }
    if (off >= kStepsOff && off < kStepsOff + sizeof pat_->steps) {
        const uint32_t s = (off - kStepsOff) / sizeof(Step), f = (off - kStepsOff) % sizeof(Step);
        static const uint8_t kRowOf[sizeof(Step)] = {kRowTrig, kRowTrigP, kRowDelay, kRowDelayP, kRowVel,
                                                     kRowLen,  kRowBumper, kRowRamp, kRowDegree, kRowLock};
        stStep_ = int(s);
        stRow_ = f < sizeof kRowOf ? kRowOf[f] : 0;
        page_ = kPgSteps;
        return;
    }
    if (off >= kCombosOff && off < kCombosOff + sizeof pat_->combos) {
        const uint32_t c = (off - kCombosOff) / sizeof(Combo), f = (off - kCombosOff) % sizeof(Combo);
        cbSel_ = int(c);
        if (f == offsetof(Combo, on)) cbField_ = 0;
        else if (f >= offsetof(Combo, path) && f < offsetof(Combo, path) + kComboLen) cbField_ = 1 + int(f - offsetof(Combo, path));
        else if (f == offsetof(Combo, action)) cbField_ = 5;
        else if (f == offsetof(Combo, a)) cbField_ = 6;
        else cbField_ = 7;
        page_ = kPgCombos;
        return;
    }
    if (off >= kMetaOff && off < kMetaOff + 16) {
        int lst[48];
        parFocus_[1] = paramListOf(kPgEvolve, lst, 48) + int(off - kMetaOff);
        page_ = kPgEvolve;
        return;
    }
    // kParams entries: which page and which row
    for (int k = 0; k < kParamCount; ++k) {
        const ParamDef& d = kParams[k];
        for (int i = 0; i < d.count; ++i) {
            if (uint32_t(d.offset + d.stride * i) != off) continue;
            int lst[48];
            switch (d.page) {
                case kPpTable:
                case kPpEvolve:
                case kPpRandom: {
                    const int pg = d.page == kPpTable ? kPgTable : kPgEvolve;
                    const int n = paramListOf(pg, lst, 48);
                    for (int j = 0; j < n; ++j)
                        if (lst[j] == k) parFocus_[pg == kPgTable ? 0 : 1] = j;
                    page_ = pg;
                    return;
                }
                case kPpMelody: {
                    const int n = paramsOfPage(kPpMelody, lst, 48);
                    for (int j = 0; j < n; ++j)
                        if (lst[j] == k) melField_ = j;
                    stRow_ = kStepRows;
                    page_ = kPgSteps;
                    return;
                }
                case kPpMix: {
                    const int n = paramsOfPage(kPpMix, lst, 48);
                    for (int j = 0; j < n; ++j)
                        if (lst[j] == k) mixRow_ = j;
                    mixCh_ = i;
                    mixFx_ = false;
                    page_ = kPgMix;
                    return;
                }
                case kPpFx: {
                    const int n = paramsOfPage(kPpFx, lst, 48);
                    for (int j = 0; j < n; ++j)
                        if (lst[j] == k) parFocus_[2] = j;
                    mixFx_ = true;
                    page_ = kPgMix;
                    return;
                }
                case kPpChannel: {
                    const int n = paramsOfPage(kPpChannel, lst, 48);
                    for (int j = 0; j < n; ++j)
                        if (lst[j] == k) chField_ = 9 + j;
                    chSel_ = i;
                    page_ = kPgChannels;
                    return;
                }
                default: return;
            }
        }
    }
    // the engine type
    for (int c = 0; c < kCh; ++c)
        if (refId_[chRef_[c][0]] == id) {
            chSel_ = c;
            chField_ = 0;
            page_ = kPgChannels;
        }
}

void Tilt::navParamChanged(kit::ParamId id) {
    // a Shell view (PARAM pads, MAP, MIDI learn) set a base value: write it now, so the
    // screen and the next sync see it; an engine change loads that engine's defaults
    for (int i = 0; i < nRefs_; ++i) {
        if (refId_[i] != id) continue;
        kit::Registry& R = ctx_->params();
        const FieldRef& r = refs_[i];
        for (int c = 0; c < kCh; ++c)
            if (chRef_[c][0] == i) {
                const int t = clampi(int(R.get(id) + 0.5f), 0, kEngines - 1);
                if (t != pat_->ch[c].type) {
                    engineDefaults(pat_->ch[c], t);
                    if (t != kEnSample) pat_->ch[c].sample[0] = 0;
                    refreshSlots(c);
                    placedFor_ = -1;
                    placeChannels();
                }
                return;
            }
        if (!R.meta(id).mods) refSetter(&refs_[i], R.get(id));
        (void)r;
        return;
    }
}

}  // namespace tilt
