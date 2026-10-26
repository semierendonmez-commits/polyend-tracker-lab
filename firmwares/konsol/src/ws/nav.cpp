// KONSOL x LABKIT v2: the parameter registry (engine slots per track, the
// mixer and the send effects), the modulation sources, the page descriptions
// for MAP / HELP, and the glue that keeps the registry and KONSOL's own pages
// in step.
//
// Engine parameters: every track has 36 "slots" (normalized 0..1, bound with
// a setter) that follow the track's current engine in the Sound page's order;
// the selected track's slots are on the SOUND page (PARAM pads, MAP), the
// others on hidden pages, so routes and macros keep working on every track.
#include <cstdio>
#include <cstring>

#include "lab/mod.h"
#include "lab/nav.h"
#include "lab/params.h"
#include "ws/konsol.h"

namespace lab {
namespace konsol {

namespace {
constexpr int kHiddenPage = 40;   // + track: slots of the tracks that are not selected
enum FxWhich : uint8_t {
    kFxMaster, kFxRetA, kFxRetB, kFxComp, kFxDlyFb, kFxDlyTone, kFxDlyTime, kFxDlyType, kFxRevSize, kFxRevDamp,
    kFxRevType, kFxRevPre, kFxN
};
const char* const kSyncNames[6] = {"1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
const char* const kRevNames[kRevTypes] = {"Plate", "Hall", "Shimmer", "Gated"};
const char* const kDlyNames[kDlyTypes] = {"Digital", "Tape", "Lo-fi"};
const char* const kTrackGroups[kTracks] = {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8"};
const char* const kLvlNames[kTracks] = {"1 Level", "2 Level", "3 Level", "4 Level",
                                        "5 Level", "6 Level", "7 Level", "8 Level"};
const char* const kDlyNamesT[kTracks] = {"1 Delay", "2 Delay", "3 Delay", "4 Delay",
                                         "5 Delay", "6 Delay", "7 Delay", "8 Delay"};
const char* const kRevNamesT[kTracks] = {"1 Reverb", "2 Reverb", "3 Reverb", "4 Reverb",
                                         "5 Reverb", "6 Reverb", "7 Reverb", "8 Reverb"};
const char* const kSrcNames[kTracks] = {"T1 ENV", "T2 ENV", "T3 ENV", "T4 ENV", "T5 ENV", "T6 ENV", "T7 ENV", "T8 ENV"};
}  // namespace

const char* const kPageName[kPgCount] = {"SESSION", "STEPS", "ROLL", "PLAYER", "SOUND",
                                         "SAMPLE",  "KIT",   "MIXER", "SONG",   "PROJECT"};
const char* const kPageKey[kPgCount] = {"Pattern", "Note", "FX1", "FX2", "Instr", "S.Load", "S.Play", "S.Rec", "Song", "File"};
Rgb pageColor(int p) {
    static const Rgb k[kPgCount] = {Rgb(0xFFB000), Rgb(0xFF6A4D), Rgb(0xFF8FB1), Rgb(0xB58CFF), Rgb(0x4DB8FF),
                                    Rgb(0x3FD6C2), Rgb(0x8BE36B), Rgb(0xE8E2D0), Rgb(0xFFD23F), Rgb(0x9AA6BF)};
    return k[p < 0 || p >= kPgCount ? 0 : p];
}

// ---------------------------------------------------------------------------
// registration (init)
// ---------------------------------------------------------------------------
void Konsol::registerParams() {
    kit::Registry& R = ctx_->params();
    R.reserve(kTracks * kSlotsPerTrack + 64);
    for (int t = 0; t < kTracks; ++t) {
        R.setPage(kHiddenPage + t);
        R.setGroup(kTrackGroups[t], trackColor(t));
        for (int s = 0; s < kSlotsPerTrack; ++s) {
            char key[12];
            std::snprintf(key, sizeof key, "t%d.s%02d", t + 1, s);
            std::snprintf(slotName_[t][s], sizeof slotName_[t][s], "-");
            slotRef_[t][s] = SlotRef{this, uint8_t(t), uint8_t(s)};
            slotIdx_[t][s] = -1;
            slotParam_[t][s] = R.add(key, slotName_[t][s], &Konsol::slotSet, &slotRef_[t][s], 0.5f)
                                   .range(0.0f, 1.0f)
                                   .percent()
                                   .pad(s);
        }
    }
    // the mixer: row 1 levels + master / returns / comp, row 2 delay sends + delay,
    // row 3 reverb sends + reverb
    R.setPage(kPgMixer);
    for (int t = 0; t < kTracks; ++t) {
        Mix& m = P_->track[t].mix;
        R.setGroup(kTrackGroups[t], trackColor(t));
        char key[12];
        std::snprintf(key, sizeof key, "t%d.lvl", t + 1);
        mixId_[t][0] = R.add(key, kLvlNames[t], &m.level).range(0.0f, 1.25f).def(0.8f).pad(t);
        mixId_[t][1] = kit::kNoParam;   // pan: on the mixer page only
        std::snprintf(key, sizeof key, "t%d.dly", t + 1);
        mixId_[t][2] = R.add(key, kDlyNamesT[t], &m.sendA).percent().def(0.0f).pad(12 + t);
        std::snprintf(key, sizeof key, "t%d.rev", t + 1);
        mixId_[t][3] = R.add(key, kRevNamesT[t], &m.sendB).percent().def(0.0f).pad(24 + t);
    }
    static const struct {
        const char* key;
        const char* name;
        float lo, hi;
        int pad;
        const char* const* names;
        int nNames;
    } kFx[kFxN] = {
        {"fx.master", "Master", 0.0f, 1.25f, 8, nullptr, 0},       {"fx.reta", "Dly Return", 0.0f, 1.5f, 9, nullptr, 0},
        {"fx.retb", "Rev Return", 0.0f, 1.5f, 10, nullptr, 0},     {"fx.comp", "Comp", 0.0f, 1.0f, 11, nullptr, 0},
        {"fx.dfb", "Dly Fdbk", 0.0f, 0.95f, 20, nullptr, 0},       {"fx.dtone", "Dly Tone", 0.0f, 1.0f, 21, nullptr, 0},
        {"fx.dtime", "Dly Time", 0.0f, 5.0f, 22, kSyncNames, 6},   {"fx.dtype", "Dly Type", 0.0f, 2.0f, 23, kDlyNames, 3},
        {"fx.rsize", "Rev Size", 0.0f, 1.0f, 32, nullptr, 0},      {"fx.rdamp", "Rev Damp", 0.0f, 1.0f, 33, nullptr, 0},
        {"fx.rtype", "Rev Type", 0.0f, 3.0f, 34, kRevNames, 4},    {"fx.rpre", "Pre-delay", 0.0f, 1.0f, 35, nullptr, 0},
    };
    R.setGroup("FX", Rgb(0xE8E2D0));
    for (int i = 0; i < kFxN; ++i) {
        fxRef_[i] = FxRef{this, uint8_t(i)};
        const FxCfg& f = P_->fx;
        float init = 0;
        switch (i) {
            case kFxMaster: init = f.master; break;
            case kFxRetA: init = f.retA; break;
            case kFxRetB: init = f.retB; break;
            case kFxComp: init = f.compRatio; break;
            case kFxDlyFb: init = f.delayFb; break;
            case kFxDlyTone: init = f.delayTone; break;
            case kFxDlyTime: init = f.delaySync; break;
            case kFxDlyType: init = f.dlyType; break;
            case kFxRevSize: init = f.revSize; break;
            case kFxRevDamp: init = f.revDamp; break;
            case kFxRevType: init = f.revType; break;
            default: init = f.revPre; break;
        }
        kit::Param& p = R.add(kFx[i].key, kFx[i].name, &Konsol::fxSet, &fxRef_[i], init).range(kFx[i].lo, kFx[i].hi);
        if (kFx[i].names) p.names(kFx[i].names, kFx[i].nNames).noMod();
        else if (kFx[i].hi <= 1.0f) p.percent();
        p.pad(kFx[i].pad);
    }
    R.setPage(0);
    // modulation sources: a decaying envelope per track's notes, bar and beat ramps
    for (int t = 0; t < kTracks; ++t) ctx_->mod().addSource(kSrcNames[t], &noteEnv_[t], false);
    ctx_->mod().addSource("BAR", &barPh_, false);
    ctx_->mod().addSource("BEAT", &beatPh_, false);
    kitReady_ = true;
    for (int t = 0; t < kTracks; ++t) refreshSlots(t);
    placeSlots();
}

void Konsol::slotSet(void* ctx, float v) {
    const SlotRef& r = *static_cast<const SlotRef*>(ctx);
    Konsol& k = *r.k;
    const int idx = k.slotIdx_[r.t][r.s];
    if (idx < 0 || k.hold_) return;
    k.engine(r.t)->setFrac(idx, v);
}

void Konsol::fxSet(void* ctx, float v) {
    const FxRef& r = *static_cast<const FxRef*>(ctx);
    Konsol& k = *r.k;
    FxCfg& f = k.P_->fx;
    const auto ix = [](float x, int hi) {
        const int i = int(x + 0.5f);
        return uint8_t(i < 0 ? 0 : (i > hi ? hi : i));
    };
    switch (r.which) {
        case kFxMaster: f.master = v; break;
        case kFxRetA: f.retA = v; break;
        case kFxRetB: f.retB = v; break;
        case kFxComp: f.compRatio = v; break;
        case kFxDlyFb: f.delayFb = v; break;
        case kFxDlyTone: f.delayTone = v; break;
        case kFxDlyTime: f.delaySync = ix(v, 5); break;
        case kFxDlyType: f.dlyType = ix(v, kDlyTypes - 1); break;
        case kFxRevSize: f.revSize = v; break;
        case kFxRevDamp: f.revDamp = v; break;
        case kFxRevType: f.revType = ix(v, kRevTypes - 1); break;
        default: f.revPre = v; break;
    }
    k.fxDirty_ = true;
}

// The slots of track t follow its engine, in the Sound page's section order.
void Konsol::refreshSlots(int t) {
    if (!kitReady_) return;
    kit::Registry& R = ctx_->params();
    Engine* e = engine(t);
    int n = 0;
    uint8_t ids[64];
    for (int sec = 0; sec < e->sections() && n < kSlotsPerTrack; ++sec) {
        const int k = e->sectionParams(sec, ids, 64);
        for (int i = 0; i < k && n < kSlotsPerTrack; ++i) {
            bool dup = false;
            for (int j = 0; j < n; ++j) dup = dup || slotIdx_[t][j] == ids[i];
            if (!dup) slotIdx_[t][n++] = int8_t(ids[i]);
        }
    }
    for (int s = n; s < kSlotsPerTrack; ++s) slotIdx_[t][s] = -1;
    for (int s = 0; s < kSlotsPerTrack; ++s) {
        const kit::ParamId id = slotParam_[t][s];
        char nm[32];
        if (slotIdx_[t][s] >= 0) {
            e->paramName(slotIdx_[t][s], nm, sizeof nm);
            std::snprintf(slotName_[t][s], sizeof slotName_[t][s], "%s", nm);
            const float f = e->frac(slotIdx_[t][s]);
            R.baseArray()[id] = f;
            R.effArray()[id] = f;
            R.meta(id).flags = uint8_t(R.meta(id).flags & ~kit::Registry::kNoMod);
        } else {
            slotName_[t][s][0] = 0;
            R.meta(id).flags = uint8_t(R.meta(id).flags | kit::Registry::kNoMod);
        }
    }
    placedFor_ = -1;   // re-place (the empty slots leave the pads)
}

void Konsol::placeSlots() {
    if (!kitReady_ || placedFor_ == selT_) return;
    kit::Registry& R = ctx_->params();
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kSlotsPerTrack; ++s) {
            const bool used = slotIdx_[t][s] >= 0;
            R.meta(slotParam_[t][s]).page = int16_t(t == selT_ && used ? kPgSound : kHiddenPage + t);
        }
    placedFor_ = selT_;
}

void Konsol::slotEdited(int t, int param) {
    if (!kitReady_) return;
    for (int s = 0; s < kSlotsPerTrack; ++s)
        if (slotIdx_[t][s] == param) {
            kit::Registry& R = ctx_->params();
            const kit::ParamId id = slotParam_[t][s];
            const float f = engine(t)->frac(param);
            R.baseArray()[id] = f;
            if (!R.meta(id).mods) R.effArray()[id] = f;
            return;
        }
}

kit::ParamId Konsol::slotId(int t, int param) const {
    for (int s = 0; s < kSlotsPerTrack; ++s)
        if (slotIdx_[t][s] == param) return slotParam_[t][s];
    return kit::kNoParam;
}

// ---------------------------------------------------------------------------
// pages (MAP, HELP)
// ---------------------------------------------------------------------------
int Konsol::navPages(kit::PageInfo* out, int cap) {
    static const char* const kHelp[kPgCount] = {
        "Clips of 8 tracks x 16 scenes. Pads: launch; col 9 scenes, 10 modes, 11 stop/scroll/rec, 12 perform.",
        "16 steps of the selected clip on pad rows 1-2, a keyboard on rows 3-4. Hold a step + turn = edit.",
        "Piano roll of the selected clip. Arrows move, Insert adds or removes, F1-F5 pick what the encoder edits.",
        "Players and generators of the selected clip: write a take, or let it vary live while it plays.",
        "The selected track's engine. Up/Down: section, Left/Right: parameter, encoder: value. F3: engine.",
        "The track's sample: load, tempo, time-stretch to the project (F5); F2: the slice editor.",
        "16-pad kit: from slices, by class, from a folder of one-shots or pad by pad. Pads 4x4 play / select.",
        "8 channels: level, pan, delay and reverb sends, mute, solo. F2: the send effects and the master bus.",
        "The arrangement: scenes for N bars. Pads 1-4: scenes, 5-12: entries. F8 plays the song.",
        "Name, save, open, new; the track table (engine, octave, stem, MIDI out / in).",
    };
    static const char* const kF[kPgCount][8] = {
        {"Stop all clips", "Steps", "Piano roll", "Player", "Sound", "Mixer", "Song", "Project"},
        {"Lock: velocity", "Lock: length", "Lock: chance", "Lock: ratchet", "Lock: condition (A:B, Fill)",
         "Lock: tone / slide", "Clip length", "Clear the lane"},
        {"Encoder: cursor", "Encoder: length", "Encoder: velocity", "Encoder: move", "Encoder: pitch", "Grid",
         "Quantise", "Undo (Shift: redo)"},
        {"Write a take", "New seed + write", "Live on / off", "Every N loops", "Write + open the roll", "Undo",
         "Clear the clip", "Clip launch quantise"},
        {"Section <", "Section >", "Choose the engine", "Init", "KONSOL preset", "Sample page", "Keyboard octave",
         "Pads: keyboard"},
        {"Tempo / stretch", "Slice editor", "Load a WAV (browser)", "Built-in break", "Stretch to the tempo",
         "Back to the original", "Kit from the slices", "Track: SAMPLER"},
        {"Kit from slices", "Kit sorted by class", "Kit from a folder", "Sample on this pad", "Load a kit",
         "Save the kit", "Follow played pad", "Clear the pad"},
        {"Channels", "Send FX + master", "Mute", "Solo", "MIDI out", "Reset the channel", "Unsolo all",
         "Unmute all"},
        {"Insert an entry", "Delete the entry", "Song mode", "Append the scene", "Loop on / off", "Move left",
         "Move right", "Play the song"},
        {"Track table", "Open a project", "Save", "Save as (name)", "New (press twice)", "Demo (press twice)",
         "Rename (keyboard)", "Remove unused samples"},
    };
    int n = 0;
    for (; n < kPgCount && n < cap; ++n) {
        out[n].name = kPageName[n];
        out[n].help = kHelp[n];
        out[n].color = pageColor(n);
        for (int k = 0; k < 8; ++k) out[n].fkeys[k] = kF[n][k];
    }
    return n;
}

void Konsol::navGoto(int page) {
    if (page >= 0 && page < kPgCount) gotoPage(page);
}

void Konsol::navFocus(kit::ParamId id) {
    // follow the focused param with our own cursors
    for (int s = 0; s < kSlotsPerTrack; ++s)
        if (slotParam_[selT_][s] == id && slotIdx_[selT_][s] >= 0) {
            Engine* e = engine(selT_);
            uint8_t ids[64];
            for (int sec = 0; sec < e->sections(); ++sec) {
                const int k = e->sectionParams(sec, ids, 64);
                for (int i = 0; i < k; ++i)
                    if (ids[i] == slotIdx_[selT_][s]) {
                        sndSec_ = sec;
                        sndCol_ = i;
                        return;
                    }
            }
        }
    for (int t = 0; t < kTracks; ++t)
        for (int k = 0; k < 4; ++k)
            if (mixId_[t][k] == id) {
                mixCol_ = t;
                mixRow_ = k;
                mixFx_ = false;
            }
}

void Konsol::navParamChanged(kit::ParamId id) {
    (void)id;
    dirty_ = true;
}

}  // namespace konsol
}  // namespace lab
