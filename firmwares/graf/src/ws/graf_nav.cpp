// GRAF -- LABKIT v2: every knob of the patch in the registry, the pages, the
// sources, focus and edits from the Shell's views.
//
// Registry layout:
//   64 slots x 10 knobs ("s07.k3"), bound to Module::ke (the effective knob,
//   0..1): routes and macros reach any knob in the patch. A knob param
//   follows its slot: name "VCO1 Tune", group = the module's category,
//   no modulation when the slot is empty. Which page a knob sits on is
//   decided live (reassignPages): the MODULE page holds the selected module
//   (pads 1-10), the module feeding it (13-22) and the one it feeds (25-34)
//   -- following the signal --; the GRAPH page holds every other module's
//   main knob on the pad of its cell (the canvas as a knob grid). Other knobs
//   are off the pages but stay routable.
//   PLAY: the 8 strips (level, pan, mute). SNAPS: the crossfader (A, B, X-FADE, on).
// Sources: TAP1-8 (outputs you mark: Shift + FX2), LANE1-4 (the first four
// sequencers' lanes), BAR, BEAT, LEVEL.
#include <cstdio>
#include <cstring>

#include "ws/graf_ws.h"

namespace graf {

namespace {
using kit::ParamId;
using kit::Registry;
// The knob shown on the GRAPH page for each module type.
const uint8_t kMainKnob[kTypeCount] = {
    1,  // VCO Tune
    4,  // CPLX Timbre
    3,  // WTBL Position
    0,  // NOIS Density
    1,  // SMPL Tune
    0,  // AIN Gain
    2,  // MIDI Glide
    0,  // RETE Gain
    1,  // PINB Chaos
    1,  // CLK Swing
    0,  // DIV N
    2,  // SEQ Gate len
    1,  // EUCL Hits
    0,  // TURG Prob
    1,  // MRKV Entropy
    0,  // S&H Slew
    0,  // QNT Scale
    0,  // LFO Rate
    1,  // ADSR Decay
    1,  // AD Decay
    0,  // FUNC Rise
    0,  // SLEW Rise
    0,  // LOGC Thresh
    3,  // CVMX Offset
    0,  // LADR Cutoff
    0,  // SVF Cutoff
    2,  // LPG Decay
    0,  // FOLD Fold
    0,  // VCA Level 1
    4,  // MIX Drive
    0,  // RING Mix
    0,  // CRSH Bits
    2,  // DLY Feedback
    3,  // VERB Mix
    2,  // GRN Position
    0,  // OUT Level
    2,  // PERC Decay
    4,  // PLTS Timbre
    3,  // RESO Structure
    4,  // EXCT Strike
    0,  // NET Gain
    2,  // RVAL Scale
    0,  // MG Range
    0,  // MGAT Length
    0,  // CHAO Rate
    0,  // BERN Prob
    1,  // ACID Cutoff
    3,  // FLTR Tone
    3,  // PART Density
};
static_assert(sizeof kMainKnob == kTypeCount, "a main knob per type");

bool noModParam(int type, int p) {
    const char* n = def(type).p[p].name;
    if ((type == kPlts || type == kPart) && p == 0) return true;   // model / type: they set the CPU cost
    return !std::strcmp(n, "Sample") || !std::strcmp(n, "Voice") || !std::strcmp(n, "MIDI out") ||
           !std::strcmp(n, "Stem") || !std::strcmp(n, "Seed");
}

char gKeys[kMaxSlots][kMaxParam][8];
const char* const kSnapNames[kSnaps] = {"Snap 1", "Snap 2", "Snap 3", "Snap 4", "Snap 5", "Snap 6", "Snap 7", "Snap 8"};
const char* const kOffOn2[2] = {"Off", "On"};
const char* const kTapNames[kTaps] = {"TAP1", "TAP2", "TAP3", "TAP4", "TAP5", "TAP6", "TAP7", "TAP8"};
const char* const kLaneNames[kLanes] = {"LANE1", "LANE2", "LANE3", "LANE4"};
char gStripName[3][kPatches][10];
}  // namespace

void Graf::registerParams() {
    Registry& P = ctx_->params();
    P.reserve(kMaxSlots * kMaxParam + 48);
    for (int c = 0; c < kCatCount; ++c) catGroup_[c] = P.group(kCatNames[c], col::kCat[c]);
    P.setPage(-1);
    for (int s = 0; s < kMaxSlots; ++s)
        for (int p = 0; p < kMaxParam; ++p) {
            std::snprintf(gKeys[s][p], sizeof gKeys[s][p], "s%02d.k%d", s & 63, p & 15);
            knobName_[s][p][0] = 0;
            const ParamId id = P.add(gKeys[s][p], knobName_[s][p], &eng_.mod(s).ke[p]).range(0.0f, 1.0f).percent().def(0.0f);
            if (s == 0 && p == 0) knobBase_ = id;
        }
    // mixer strips
    P.setPage(kPgPlay);
    P.setGroup("STRIPS", Rgb(0x38C8FF));
    for (int i = 0; i < kPatches; ++i) {
        std::snprintf(gStripName[0][i], 10, "LEVEL %d", (i & 7) + 1);
        std::snprintf(gStripName[1][i], 10, "PAN %d", (i & 7) + 1);
        std::snprintf(gStripName[2][i], 10, "MUTE %d", (i & 7) + 1);
        static char keys[3][kPatches][12];
        std::snprintf(keys[0][i], 12, "mix%d.lvl", (i & 7) + 1);
        std::snprintf(keys[1][i], 12, "mix%d.pan", (i & 7) + 1);
        std::snprintf(keys[2][i], 12, "mix%d.mute", (i & 7) + 1);
        stripLvl_[i] = P.add(keys[0][i], gStripName[0][i], &eng_.mix[i].level).percent().pad(i);
        stripPan_[i] = P.add(keys[1][i], gStripName[1][i], &eng_.mix[i].pan).range(-1.0f, 1.0f).bipolar().pad(12 + i);
        stripMute_[i] = P.add(keys[2][i], gStripName[2][i], &eng_.mix[i].mute).names(kOffOn2, 2).noMod().pad(24 + i);
    }
    // the crossfader between two snapshots
    P.setPage(kPgSnap);
    P.setGroup("X-FADE", Rgb(0xFFB000));
    xfAId_ = P.add("xf.a", "XF A", &xfA_).names(kSnapNames, kSnaps).noMod().pad(0);
    xfBId_ = P.add("xf.b", "XF B", &xfB_).names(kSnapNames, kSnaps).noMod().pad(1);
    xfId_ = P.add("xf.pos", "X-FADE", &xf_).percent().pad(2);
    xfOnId_ = P.add("xf.on", "XF ON", &xfOn_).names(kOffOn2, 2).noMod().pad(3);
    // sources
    kit::Mod& M = ctx_->mod();
    for (int t = 0; t < kTaps; ++t) M.addSource(kTapNames[t], &eng_.tapVal[t], true);
    for (int l = 0; l < kLanes; ++l) M.addSource(kLaneNames[l], &eng_.laneVal[l], true);
    M.addSource("BAR", &eng_.barPhase, false);
    M.addSource("BEAT", &eng_.beatPhase, false);
    M.addSource("LEVEL", &eng_.masterEnv, false);
    // every knob starts as "no module here"
    for (int s = 0; s < kMaxSlots; ++s) {
        for (int p = 0; p < kMaxParam; ++p) {
            Registry::Meta& mt = P.meta(knobId(s, p));
            mt.flags |= Registry::kNoMod;
            mt.page = -1;
        }
    }
}

// Names, groups and modulation flags follow the slots.
void Graf::modulesChanged() {
    if (knobBase_ < 0) return;
    Registry& P = ctx_->params();
    for (int s = 0; s < kMaxSlots; ++s) {
        const Module& m = eng_.mod(s);
        const ModDef& d = def(m.type);
        for (int p = 0; p < kMaxParam; ++p) {
            Registry::Meta& mt = P.meta(knobId(s, p));
            const bool live = m.used && p < d.nParam;
            if (live) {
                std::snprintf(knobName_[s][p], sizeof knobName_[s][p], "%s%d %s", d.tag, m.number, d.p[p].name);
                mt.group = int8_t(catGroup_[d.cat]);
                if (noModParam(m.type, p)) mt.flags |= Registry::kNoMod;
                else mt.flags &= uint8_t(~Registry::kNoMod);
            } else {
                knobName_[s][p][0] = 0;
                mt.flags |= Registry::kNoMod;
            }
        }
        if (!m.used) dropRoutes(s);
    }
    eng_.refreshLanes();
    pagesDirty_ = true;
}

// A knob's base value changed in GRAF (editing, morphs, files): the registry follows.
void Graf::knobChanged(int slot, int p) {
    if (knobBase_ < 0 || inNav_) return;
    ctx_->params().setNorm(knobId(slot, p), eng_.mod(slot).k[p], false);
}

void Graf::dropRoutes(int slot) {
    kit::Mod& M = ctx_->mod();
    for (int p = 0; p < kMaxParam; ++p) {
        const ParamId id = knobId(slot, p);
        int r[8];
        for (int guard = 0; guard < 64; ++guard) {
            const int n = M.routesTo(id, r, 8);
            if (n <= 0) break;
            M.removeRoute(r[0]);
        }
        auto& nav = ctx_->shell->labkit().nav;
        for (auto& l : nav.learn_)
            if (l.param == id) l = kit::Nav::Learn();
    }
}

void Graf::clearAllRoutes() {
    ctx_->mod().clearRoutes();
    ctx_->shell->labkit().nav.clearLearn();
}

// Which knobs are on which page (and pad).
void Graf::reassignPages() {
    pagesDirty_ = false;
    if (knobBase_ < 0) return;
    Registry& P = ctx_->params();
    for (int s = 0; s < kMaxSlots; ++s)
        for (int p = 0; p < kMaxParam; ++p) {
            Registry::Meta& mt = P.meta(knobId(s, p));
            mt.page = -1;
            mt.pad = -1;
        }
    // MODULE page: the selected module, the one feeding it, the one it feeds
    int trio[3] = {-1, -1, -1};
    if (sel_ >= 0 && eng_.mod(sel_).used) {
        trio[0] = sel_;
        int up[8], dn[8];
        if (eng_.upstream(sel_, up, 8) > 0) trio[1] = up[0];
        const int nd = eng_.downstream(sel_, dn, 8);
        for (int i = 0; i < nd; ++i)
            if (dn[i] != trio[1]) {
                trio[2] = dn[i];
                break;
            }
    }
    for (int r = 0; r < 3; ++r) {
        const int s = trio[r];
        if (s < 0) continue;
        const int np = def(eng_.mod(s).type).nParam;
        for (int p = 0; p < np; ++p) {
            Registry::Meta& mt = P.meta(knobId(s, p));
            mt.page = kPgModule;
            mt.pad = int8_t(r * 12 + p);
        }
    }
    // GRAPH page: every other module's main knob on its cell's pad (rows 1-3)
    for (int c = 0; c < 36; ++c) {
        const int s = eng_.slotAtCell(patch_, c);
        if (s < 0 || s == trio[0] || s == trio[1] || s == trio[2]) continue;
        const int mk = kMainKnob[eng_.mod(s).type];
        if (mk >= def(eng_.mod(s).type).nParam) continue;
        Registry::Meta& mt = P.meta(knobId(s, mk));
        mt.page = kPgGraph;
        mt.pad = int8_t(c);
    }
}

// ---------------------------------------------------------------- pages
int Graf::navPages(kit::PageInfo* out, int cap) {
    static const char* const kNames[kPgCount] = {"GRAPH", "MATRIX", "MODULE", "ADD", "SNAPS", "PLAY", "FILE"};
    static const char* const kHelp[kPgCount] = {
        "The canvas: modules on the 12 x 4 grid of the pads. Hold A + tap B: cable A > B (encoder: amount).",
        "The pin matrix: rows = outputs, columns = inputs. Tap a pin to patch, hold + turn for the amount.",
        "One module: big knobs; pads 1-10 are its knobs (hold + turn), 11 / 12 walk the signal.",
        "Add a module: pad row 1 = category, rows 2-4 = modules. Insert / F8 adds, Shift + F8 adds and patches.",
        "Snapshots 1-8, the song timeline, the A / B crossfader (LABKIT can move X-FADE).",
        "Mixer strips (patch n or OUT n), master scope, pad keyboard.",
        "24 patch slots, demos, sample rescan.",
    };
    static const char* const kF[kPgCount][8] = {
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Next canvas (Shift: previous)"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Next canvas (Shift: previous)"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Next canvas (Shift: previous)"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Add (Shift: add + patch from the selected)"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Store the selected snapshot"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Next canvas (Shift: previous)"},
        {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", "Save into the slot (twice: overwrite)"},
    };
    static const Rgb kCol[kPgCount] = {Rgb(0xFFB000), Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0x3CE07A),
                                       Rgb(0xFF4FA0), Rgb(0xA060F0), Rgb(0x9AA4B0)};
    int n = 0;
    for (; n < kPgCount && n < cap; ++n) {
        out[n].name = kNames[n];
        out[n].help = kHelp[n];
        out[n].color = kCol[n];
        for (int k = 0; k < 8; ++k) out[n].fkeys[k] = kF[n][k];
    }
    return n;
}

void Graf::navGoto(int page) {
    if (page >= 0 && page < kPgCount) go(page);
}

void Graf::navFocus(kit::ParamId id) {
    if (knobBase_ < 0 || id < knobBase_ || id >= knobBase_ + kMaxSlots * kMaxParam) return;
    const int s = (id - knobBase_) / kMaxParam, p = (id - knobBase_) % kMaxParam;
    if (!eng_.mod(s).used) return;
    if (s != sel_ && page_ == kPgModule) return;   // the trio rows: keep the selection
    param_ = p < def(eng_.mod(s).type).nParam ? p : 0;
}

void Graf::navParamChanged(kit::ParamId id) {
    if (knobBase_ < 0 || id < knobBase_ || id >= knobBase_ + kMaxSlots * kMaxParam) return;
    const int s = (id - knobBase_) / kMaxParam, p = (id - knobBase_) % kMaxParam;
    Module& m = eng_.mod(s);
    if (!m.used || p >= def(m.type).nParam) return;
    checkpoint(2, s * 16 + p);
    inNav_ = true;
    const float k = ctx_->params().norm(id);
    eng_.setParamK(s, p, k);   // base + v; the registry keeps writing ke while modulated
    inNav_ = false;
}

}  // namespace graf
