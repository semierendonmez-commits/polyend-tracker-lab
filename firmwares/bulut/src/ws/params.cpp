// BULUT's LABKIT registration: every user-facing value is a registry
// parameter (PARAM pads, MAP, macros, routes, MIDI learn, the save chunk),
// and the LABKIT navigation hooks (pages, focus, changes).
//
// Engine knobs: each channel has twelve knob params whose meaning is the
// engine's (ui_text.h). Switching the engine stores the knobs into the old
// engine's memory, loads the new engine's and renames the params, so a
// route or a macro on a knob stays on the knob.
#include <cstdio>
#include <cstring>

#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

namespace bulut {

using lab::Rgb;
namespace kit = lab::kit;

namespace {
const char* const kLfoNames[kLfoShapes] = {"Sine", "Tri", "Saw", "Square", "S&H", "Drift"};
const char* const kFltNames[kFltModes] = {"Off", "Low-pass", "Band-pass", "High-pass"};
const char* const kFxNames[kFxModes] = {"Off", "Texture", "Shifter", "Flanger", "Panner", "Crusher", "Slicer", "Freezer"};
const char* const kEngNames[kEngines] = {"Granular", "Quanta", "Beads", "Tape"};
const char* const kSrcNames[kSrcCount] = {"Input", "Output", "Sample", "Generator", "Tape file"};
const char* const kGenNames[kGenCount] = {"Rete", "Tone", "Noise"};
const char* const kSpeedNames[kSpeeds] = {"1/4 x", "1/2 x", "1 x", "2 x", "4 x"};
const char* const kLoopNames[2] = {"Off", "On"};
const char* const kDivNames[kDlyDivs] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
const char* const kRowNames[24] = {"GAIN", "START", "END", "GEN A", "GEN B", "BED", "LFO RATE", "LFO SHAPE",
                                   "LEVEL", "PAN", "DELAY", "REVERB", "DRIVE", "FILTER", "CUTOFF", "RESO",
                                   "FX MODE", "FX TIME", "FX AMOUNT", "FX FDBK", "GEST SPEED", "ENGINE",
                                   "SOURCE", "GEN TYPE"};
const Rgb kChCol[kCh] = {Rgb(0x8EC9F0), Rgb(0xF2A7C0), Rgb(0x9BE3C3), Rgb(0xF5C49A)};
}  // namespace

Rgb Bulut::chColor(int c) const { return kChCol[c < 0 ? 0 : (c >= kCh ? kCh - 1 : c)]; }

const char* Bulut::paramName(int c, int idx) const {
    if (idx < kKnobs) return knobNames_[c][idx];
    return kRowNames[idx - kKnobs];
}

void Bulut::registerParams() {
    kit::Registry& P = ctx_->params();
    P.reserve(240);
    char key[24];
    for (int c = 0; c < kCh; ++c) {
        ChParams& p = par_[c];
        P.setPage(kNavCh1 + c);
        const Rgb col = kChCol[c];
        char gname[4][12];
        std::snprintf(gname[0], 12, "C%d SOUND", c + 1);
        std::snprintf(gname[1], 12, "C%d SOURCE", c + 1);
        std::snprintf(gname[2], 12, "C%d MIX", c + 1);
        std::snprintf(gname[3], 12, "C%d FX", c + 1);
        // group names must outlive the registry: keep them in static storage
        static char sGroup[kCh][4][12];
        for (int k = 0; k < 4; ++k) std::memcpy(sGroup[c][k], gname[k], 12);
        static char sKeys[kCh][kChParams][24];
        auto K = [&](int idx, const char* suffix) {
            std::snprintf(sKeys[c][idx], sizeof sKeys[c][idx], "c%d.%.12s", c + 1, suffix);
            return sKeys[c][idx];
        };
        (void)key;
        P.setGroup(sGroup[c][0], col);
        for (int i = 0; i < kKnobs; ++i) {
            char sfx[16];
            std::snprintf(sfx, sizeof sfx, "k%d", i + 1);
            std::snprintf(knobNames_[c][i], 12, "%s", knobName(p.engine, i));
            pid_[c][i] = P.add(K(i, sfx), knobNames_[c][i], &p.k[i]).range(0.0f, 1.0f).percent().pad(i);
        }
        P.setGroup(sGroup[c][1], lab::mix(col, Rgb(0xFFFFFF), 0.35f));
        pid_[c][CP::Gain] = P.add(K(CP::Gain, "gain"), "GAIN", &p.gain).range(0.0f, 2.0f).percent().pad(CP::Gain);
        pid_[c][CP::Start] = P.add(K(CP::Start, "start"), "START", &p.regStart).percent().pad(CP::Start);
        pid_[c][CP::End] = P.add(K(CP::End, "end"), "END", &p.regEnd).percent().pad(CP::End);
        pid_[c][CP::GenA] = P.add(K(CP::GenA, "gena"), "GEN A", &p.genA).percent().pad(CP::GenA);
        pid_[c][CP::GenB] = P.add(K(CP::GenB, "genb"), "GEN B", &p.genB).percent().pad(CP::GenB);
        pid_[c][CP::Bed] = P.add(K(CP::Bed, "bed"), "BED", &p.bed).percent().pad(CP::Bed);
        pid_[c][CP::LfoRate] =
            P.add(K(CP::LfoRate, "lfo.rate"), "LFO RATE", &p.lfoRate).range(0.02f, 20.0f).exp().unit("Hz").pad(CP::LfoRate);
        pid_[c][CP::LfoShape] =
            P.add(K(CP::LfoShape, "lfo.shape"), "LFO SHAPE", &p.lfoShape).names(kLfoNames, kLfoShapes).pad(CP::LfoShape);
        P.setGroup(sGroup[c][2], lab::scale(col, 0.75f));
        pid_[c][CP::Level] = P.add(K(CP::Level, "level"), "LEVEL", &p.level).percent().pad(CP::Level);
        pid_[c][CP::Pan] = P.add(K(CP::Pan, "pan"), "PAN", &p.pan).range(-1.0f, 1.0f).bipolar().pad(CP::Pan);
        pid_[c][CP::Dly] = P.add(K(CP::Dly, "dly"), "DELAY", &p.dly).percent().pad(CP::Dly);
        pid_[c][CP::Rev] = P.add(K(CP::Rev, "rev"), "REVERB", &p.rev).percent().pad(CP::Rev);
        P.setGroup(sGroup[c][3], lab::mix(col, Rgb(0xB9A8F0), 0.5f));
        pid_[c][CP::Drive] = P.add(K(CP::Drive, "drive"), "DRIVE", &p.drive).percent().pad(CP::Drive);
        pid_[c][CP::Flt] = P.add(K(CP::Flt, "flt"), "FILTER", &p.fltMode).names(kFltNames, kFltModes).noMod().pad(CP::Flt);
        pid_[c][CP::Cutoff] =
            P.add(K(CP::Cutoff, "cut"), "CUTOFF", &p.cutoff).range(20.0f, 18000.0f).exp().unit("Hz").pad(CP::Cutoff);
        pid_[c][CP::Reso] = P.add(K(CP::Reso, "res"), "RESO", &p.reso).percent().pad(CP::Reso);
        pid_[c][CP::FxMode] = P.add(K(CP::FxMode, "fx"), "FX MODE", &p.fxMode).names(kFxNames, kFxModes).noMod().pad(CP::FxMode);
        pid_[c][CP::FxTime] = P.add(K(CP::FxTime, "fx.time"), "FX TIME", &p.fxTime).percent().pad(CP::FxTime);
        pid_[c][CP::FxAmt] = P.add(K(CP::FxAmt, "fx.amt"), "FX AMOUNT", &p.fxAmt).percent().pad(CP::FxAmt);
        pid_[c][CP::FxFb] = P.add(K(CP::FxFb, "fx.fb"), "FX FDBK", &p.fxFb).percent().pad(CP::FxFb);
        pid_[c][CP::GestSpeed] =
            P.add(K(CP::GestSpeed, "gest.spd"), "GEST SPEED", &p.gestSpeed).names(kSpeedNames, kSpeeds).pad(CP::GestSpeed);
        pid_[c][CP::Engine] = P.add(K(CP::Engine, "engine"), "ENGINE", &p.engine).names(kEngNames, kEngines).noMod().pad(CP::Engine);
        pid_[c][CP::Source] = P.add(K(CP::Source, "source"), "SOURCE", &p.source).names(kSrcNames, kSrcCount).noMod().pad(CP::Source);
        pid_[c][CP::GenType] = P.add(K(CP::GenType, "gen"), "GEN TYPE", &p.genType).names(kGenNames, kGenCount).noMod().pad(CP::GenType);
    }
    // the send effects (CLOUDS page) and the morph time (MOTION page)
    P.setPage(kNavClouds);
    P.setGroup("SEND FX", Rgb(0x9CD6F2));
    mid_[kMDlyDiv] = P.add("fx.dly.time", "DLY TIME", &fx_.dlyDiv).names(kDivNames, kDlyDivs).pad(0);
    mid_[kMDlyFb] = P.add("fx.dly.fb", "DLY FDBK", &fx_.dlyFb).percent().pad(1);
    mid_[kMDlyTone] = P.add("fx.dly.tone", "DLY TONE", &fx_.dlyTone).percent().pad(2);
    mid_[kMDlyRet] = P.add("fx.dly.ret", "DLY RETURN", &fx_.dlyRet).percent().pad(3);
    mid_[kMRevSize] = P.add("fx.rev.size", "REV SIZE", &fx_.revSize).percent().pad(4);
    mid_[kMRevDamp] = P.add("fx.rev.damp", "REV DAMP", &fx_.revDamp).percent().pad(5);
    mid_[kMRevRet] = P.add("fx.rev.ret", "REV RETURN", &fx_.revRet).percent().pad(6);
    P.setPage(kNavMotion);
    P.setGroup("MOTION", Rgb(0xF2B6C8));
    mid_[kMMorph] = P.add("motion.morph", "MORPH TIME", &fx_.morphSec).range(0.0f, 8.0f).step(0.1f).unit("s").noMod().pad(0);
    P.setPage(kNavFreeze);
    P.setGroup("SPECTRAL", Rgb(0xB9A8F0));
    tid_[0] = P.add("spec.blur", "BLUR", &tools_.blur).percent().noMod().pad(0);
    tid_[1] = P.add("spec.shift", "SHIFT", &tools_.shift).range(-24.0f, 24.0f).step(1.0f).bipolar().unit("st").noMod().pad(1);
    tid_[2] = P.add("spec.tilt", "TILT", &tools_.tilt).range(-1.0f, 1.0f).bipolar().noMod().pad(2);
    tid_[3] = P.add("spec.sparse", "SPARSE", &tools_.sparse).percent().noMod().pad(3);
    tid_[4] = P.add("spec.morph", "MORPH", &tools_.morph).percent().noMod().pad(4);
    tid_[5] = P.add("spec.mslot", "MORPH SLOT", &tools_.morphSlot).range(0, kFreezeSlots - 1).noMod().pad(5);
    // TAPE FILE: each channel's SPEED, FROM and LOOP (page TAPE; not in the
    // channel pages / scenes: those are full). In the channel's SOURCE group.
    P.setPage(kNavTape);
    static char sTapeKey[kCh][kTapeParams][24];
    static char sTapeName[kCh][kTapeParams][16];
    static const char* const kTpKey[kTapeParams] = {"spd", "from", "loop"};
    static const char* const kTpName[kTapeParams] = {"SPEED", "FROM", "LOOP"};
    for (int c = 0; c < kCh; ++c) {
        char gname[16];
        std::snprintf(gname, sizeof gname, "C%d SOURCE", c + 1);
        P.setGroup(P.groupName(P.group(gname)), lab::mix(kChCol[c], Rgb(0xFFFFFF), 0.35f));
        for (int k = 0; k < kTapeParams; ++k) {
            std::snprintf(sTapeKey[c][k], sizeof sTapeKey[c][k], "c%d.tape.%s", c + 1, kTpKey[k]);
            std::snprintf(sTapeName[c][k], sizeof sTapeName[c][k], "C%d %s", c + 1, kTpName[k]);
        }
        ChParams& p = par_[c];
        tapePid_[c][kTpSpeed] = P.add(sTapeKey[c][kTpSpeed], sTapeName[c][kTpSpeed], &p.tapeSpeed)
                                    .range(kTapeMinSpeed, kTapeMaxSpeed)
                                    .exp()
                                    .def(1.0f)
                                    .unit("x")
                                    .pad(c * 3);
        tapePid_[c][kTpFrom] = P.add(sTapeKey[c][kTpFrom], sTapeName[c][kTpFrom], &p.tapeFrom).percent().noMod().pad(c * 3 + 1);
        tapePid_[c][kTpLoop] =
            P.add(sTapeKey[c][kTpLoop], sTapeName[c][kTpLoop], &p.tapeLoop).names(kLoopNames, 2).def(1.0f).noMod().pad(c * 3 + 2);
    }
    for (int c = 0; c < kCh; ++c) {
        engNow_[c] = par_[c].engine;
        srcNow_[c] = par_[c].source;
    }
}

void Bulut::renameKnobs(int c) {
    for (int i = 0; i < kKnobs; ++i) std::snprintf(knobNames_[c][i], 12, "%s", knobName(par_[c].engine, i));
}

// The engine param changed (an encoder, the PARAM pads, a scene, a project):
// park the old engine's knobs, bring the new engine's back.
void Bulut::syncEngines() {
    kit::Registry& P = ctx_->params();
    for (int c = 0; c < kCh; ++c) {
        const int now = par_[c].engine;
        if (now == engNow_[c]) continue;
        const int old = engNow_[c];
        if (old >= 0 && old < kEngines)
            for (int i = 0; i < kKnobs; ++i) st_[c].engMem[old][i] = P.norm(pid_[c][i]);
        engNow_[c] = now;
        for (int i = 0; i < kKnobs; ++i) P.setNorm(pid_[c][i], st_[c].engMem[now][i], false);
        renameKnobs(c);
        eng_->releaseGhosts(c);
    }
}

int Bulut::chOf(kit::ParamId id, int* idx) const {
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kChParams; ++i)
            if (pid_[c][i] == id) {
                if (idx) *idx = i;
                return c;
            }
    return -1;
}

float Bulut::norm(int c, int idx) const { return ctx_->params().norm(pid_[c][idx]); }

void Bulut::setNorm(int c, int idx, float n, bool record) {
    kit::Registry& P = ctx_->params();
    P.setNorm(pid_[c][idx], clamp01(n), false);
    if (!record) return;
    if (rec_->state() == GestureRecorder::kArmed && rec_->cloud() == c) waitGesture(c);
    if (rec_->state() == GestureRecorder::kRecording && rec_->cloud() == c && idx != CP::Engine && idx != CP::Source)
        rec_->param(idx, P.norm(pid_[c][idx]), eng_->sampleClock);
}

void Bulut::setParam(int c, int idx, float v, bool record) {
    kit::Registry& P = ctx_->params();
    setNorm(c, idx, P.toNorm(pid_[c][idx], v), record);
}

void Bulut::paramText(int c, int idx, char* out, int cap) const {
    if (idx < kKnobs) {
        knobText(par_[c].engine, idx, ctx_->params().norm(pid_[c][idx]), out, cap);
        return;
    }
    ctx_->params().format(pid_[c][idx], out, cap);
}

// ---- LABKIT pages -------------------------------------------------------------------
int Bulut::navPages(kit::PageInfo* out, int cap) {
    static char chNames[kCh][16];
    int n = 0;
    auto add = [&](const char* name, const char* help, Rgb col) -> kit::PageInfo* {
        if (n >= cap) return nullptr;
        kit::PageInfo& p = out[n++];
        p = kit::PageInfo();
        p.name = name;
        p.help = help;
        p.color = col;
        return &p;
    };
    // the F-keys are the same on every page: pages F1..F7, F8 = the page's action
    static const char* const kPagesKeys[7] = {"CLOUDS: the four channels + mixer", "SOUND: the engine's 12 knobs",
                                              "SOURCE: source, sample, region", "FX: drive, filter, FX mode, LFO",
                                              "MOTION: gestures, scenes, timeline", "FREEZE: the frozen moments",
                                              "FILE: projects, resample"};
    static const char* const kAct[kPages] = {"F8: gesture REC on the selected channel",
                                             "F8: next engine (Shift+F1..F4: pick one)",
                                             "F8: REGION pads (two taps = start / end)",
                                             "F8: add a route (channel LFO -> the focused param)",
                                             "F8: gesture REC (Shift+F8: store scene)",
                                             "F8: capture a moment (Insert: recall it)",
                                             "F8: save the project (Insert: load)"};
    add("CLOUDS", "All four channels: what plays, records, is frozen. Up/Down: channel, "
                                     "Left/Right: LEVEL PAN DELAY REVERB, encoder: value.",
                           Rgb(0x9CD6F2));
    for (int c = 0; c < kCh; ++c) {
        std::snprintf(chNames[c], sizeof chNames[c], "CH %d", c + 1);
        kit::PageInfo* q = add(chNames[c],
                               "One channel's 36 params: row 1 the engine's knobs, row 2 source / LFO / mixer, "
                               "row 3 the insert chain and switches.",
                               kChCol[c]);
        if (q) q->fkeys[7] = kAct[page_ == kPgSound || page_ == kPgSource || page_ == kPgFx ? page_ : kPgSound];
    }
    add("MOTION", "Gestures (recorded movements that loop), 8 scenes, the timeline. Up/Down: focus, Left/Right: "
                  "select, Insert: store / morph / add.",
        Rgb(0xF2B6C8));
    add("FREEZE", "The library of frozen moments and the spectral tools (blur, shift, tilt, sparse, morph), "
                  "applied when a moment is recalled.",
        Rgb(0xB9A8F0));
    add("FILE", "16 project slots and the resample loop (record the mix or a channel into another channel).",
        Rgb(0xE6EAF4));
    add("TAPE", "TAPE FILE of each channel: SPEED (varispeed, pitch follows), FROM (start / loop point), LOOP. "
                "Play / stop, the file and the position: SOURCE page.",
        Rgb(0xF5C49A));
    for (int i = 0; i < n; ++i) {
        for (int f = 0; f < 7; ++f) out[i].fkeys[f] = kPagesKeys[f];
        if (!out[i].fkeys[7]) {
            const int ui = i == 0 ? kPgClouds
                                  : (i == kNavMotion ? kPgMotion
                                                     : (i == kNavFreeze ? kPgFreeze : (i == kNavTape ? kPgSource : kPgFile)));
            out[i].fkeys[7] = kAct[ui];
        }
    }
    return n;
}

int Bulut::navPage() const {
    switch (page_) {
        case kPgClouds: return kNavClouds;
        case kPgSound:
        case kPgSource:
        case kPgFx: return kNavCh1 + sel_;
        case kPgMotion: return kNavMotion;
        case kPgFreeze: return kNavFreeze;
        default: return kNavFile;
    }
}

void Bulut::navGoto(int page) {
    if (page >= kNavCh1 && page < kNavCh1 + kCh) {
        selectChannel(page - kNavCh1);
        if (page_ != kPgSound && page_ != kPgSource && page_ != kPgFx) setPage(kPgSound);
        return;
    }
    if (page == kNavClouds) setPage(kPgClouds);
    else if (page == kNavMotion) setPage(kPgMotion);
    else if (page == kNavFreeze) setPage(kPgFreeze);
    else if (page == kNavFile) setPage(kPgFile);
    else if (page == kNavTape) setPage(kPgSource);
}

void Bulut::navFocus(kit::ParamId id) {
    for (int c = 0; c < kCh; ++c)   // a TAPE FILE param: its row on the SOURCE page
        for (int k = 0; k < kTapeParams; ++k)
            if (tapePid_[c][k] == id) {
                selectChannel(c);
                if (par_[c].source == kSrcTape) cur_[kPgSource] = 6 + k;
                return;
            }
    int idx = 0;
    const int c = chOf(id, &idx);
    if (c < 0) return;
    selectChannel(c);
    if (idx < kKnobs) {
        cur_[kPgSound] = idx;
    } else if (idx >= CP::Drive && idx <= CP::FxFb) {
        cur_[kPgFx] = idx - CP::Drive;
    } else if (idx == CP::LfoRate || idx == CP::LfoShape) {
        cur_[kPgFx] = 8 + (idx - CP::LfoRate);
    }
}

void Bulut::navParamChanged(kit::ParamId id) {
    int idx = 0;
    const int c = chOf(id, &idx);
    if (c < 0) return;
    // a Shell view (PARAM pads, MAP, macro, MIDI learn) moved it: a gesture records it too
    if (rec_->state() == GestureRecorder::kArmed && rec_->cloud() == c) waitGesture(c);
    if (rec_->state() == GestureRecorder::kRecording && rec_->cloud() == c && idx != CP::Engine && idx != CP::Source)
        rec_->param(idx, ctx_->params().norm(id), eng_->sampleClock);
}

}  // namespace bulut
