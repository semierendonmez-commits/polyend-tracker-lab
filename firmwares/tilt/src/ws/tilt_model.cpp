// TILT -- the data model: tables, the parameter table, defaults, factory
// patterns, matrix shapes, the randomizer and pattern bytes (tilt_model.h).
#include "ws/tilt_model.h"

#include <cstdio>
#include <cstring>

namespace tilt {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

const Rate kRates[kRateCount] = {
    {"1/64", 0.0625f}, {"1/32", 0.125f}, {"1/16", 0.25f}, {"1/8", 0.5f},
    {"1/4", 1.0f},     {"1/2", 2.0f},    {"1/16T", 1.0f / 6.0f}, {"1/8T", 1.0f / 3.0f},
};

const Scale kScales[kScaleCount] = {
    {"Major", 7, {0, 2, 4, 5, 7, 9, 11}},     {"Minor", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"Dorian", 7, {0, 2, 3, 5, 7, 9, 10}},    {"Phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"Mixolyd", 7, {0, 2, 4, 5, 7, 9, 10}},   {"HarmMin", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"PentMaj", 5, {0, 2, 4, 7, 9}},          {"PentMin", 5, {0, 3, 5, 7, 10}},
    {"Hijaz", 7, {0, 1, 4, 5, 7, 8, 10}},     {"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};

int scaleSemis(int scale, int deg) {
    const Scale& s = kScales[clampi(scale, 0, kScaleCount - 1)];
    const int n = s.n;
    int oct = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
    const int idx = deg - oct * n;
    return oct * 12 + s.semis[idx];
}

const char* const kEngineNames[kEngines] = {"SAMPLE", "KICK", "SNARE", "HAT", "ACID", "FM"};

const EngParam kEngineParams[kEngines][kParamsPerEngine] = {
    // SAMPLE
    {{"Source", 0, kBuiltins - 1, 0}, {"Slices", 1, 16, 1}, {"Slice by", 0, 3, 0}, {"Slice", 0, 15, 0},
     {"Pitch", 0, 48, 24}, {"Decay", 0, 127, 127}, {"Cutoff", 0, 127, 127}, {"Reso", 0, 127, 0}},
    // KICK (Plaits analog / synthetic bass drum)
    {{"Model", 0, 1, 0}, {"Tune", 0, 48, 24}, {"Decay", 0, 127, 70}, {"Tone", 0, 127, 50},
     {"Punch", 0, 127, 70}, {"Drive", 0, 127, 40}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    // SNARE (Plaits analog / synthetic snare drum)
    {{"Model", 0, 1, 0}, {"Tune", 0, 48, 24}, {"Decay", 0, 127, 60}, {"Tone", 0, 127, 64},
     {"Snappy", 0, 127, 70}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    // HAT (Plaits 808-style metallic noise)
    {{"Tune", 0, 48, 24}, {"Decay", 0, 127, 30}, {"Tone", 0, 127, 80}, {"Noise", 0, 127, 50},
     {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    // ACID (303-style)
    {{"Wave", 0, 1, 0}, {"Cutoff", 0, 127, 40}, {"Reso", 0, 127, 90}, {"Env mod", 0, 127, 70},
     {"Decay", 0, 127, 50}, {"Accent", 0, 127, 70}, {"Slide", 0, 127, 50}, {"Drive", 0, 127, 40}},
    // FM (two operators)
    {{"Ratio", 0, 8, 2}, {"Index", 0, 127, 60}, {"Idx decay", 0, 127, 50}, {"Decay", 0, 127, 70},
     {"Feedback", 0, 127, 20}, {"Attack", 0, 127, 0}, {"Detune", 0, 127, 64}, {"Tone", 0, 127, 100}},
};

const char* const kComboActionNames[kComboActions] = {"MULTIBALL", "FILL", "NOTE", "CC", "SHAKE", "SCENE"};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kDelayTimeNames[kDelayTimeCount] = {"1/32", "1/16", "1/16D", "1/8", "1/8D", "1/4", "1/4D", "1/2"};
const float kDelayTimeBeats[kDelayTimeCount] = {0.125f, 0.25f, 0.375f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
const char* const kBuiltinNames[kBuiltins] = {"BREAK", "CLAP", "RIM", "TOM", "BELL", "ZAP", "SHAKER", "CHORD"};
const char* const kShapeNames[kShapeCount] = {"CHAIN", "REVERSE", "PINGPONG", "DRUNK", "JUMPS",
                                              "EUCLID", "STAY", "HUB", "RANDOM", "CLEAR"};
const char* const kRowFnNames[kRowFnCount] = {"CLEAR", "NEXT", "EQUAL", "RANDOM", "STAY", "BACK"};

void noteName(int note, char* buf, int cap) {
    note = clampi(note, 0, 127);
    std::snprintf(buf, size_t(cap), "%s%d", kNoteNames[note % 12], note / 12 - 1);
}

static const char* const kRatioNames[9] = {"0.5", "1", "1.5", "2", "3", "3.5", "4", "5", "7"};
static const char* const kSliceBy[4] = {"Fixed", "Step", "Degree", "Random"};

void engineDefaults(Channel& c, int type) {
    c.type = uint8_t(clampi(type, 0, kEngines - 1));
    for (int i = 0; i < kParamsPerEngine; ++i) c.p[i] = kEngineParams[c.type][i].def;
}

void engineParamText(const Channel& c, int i, char* buf, int cap) {
    const EngParam& ep = kEngineParams[c.type][i];
    if (!ep.name) {
        std::snprintf(buf, size_t(cap), "-");
        return;
    }
    const int v = c.p[i];
    switch (c.type) {
        case kEnSample:
            if (i == 0) {
                std::snprintf(buf, size_t(cap), "%s", c.sample[0] ? "CARD" : kBuiltinNames[clampi(v, 0, kBuiltins - 1)]);
                return;
            }
            if (i == 2) {
                std::snprintf(buf, size_t(cap), "%s", kSliceBy[clampi(v, 0, 3)]);
                return;
            }
            if (i == 3) {
                std::snprintf(buf, size_t(cap), "%d", v + 1);
                return;
            }
            if (i == 4) {
                std::snprintf(buf, size_t(cap), "%+d", v - 24);
                return;
            }
            break;
        case kEnKick:
        case kEnSnare:
            if (i == 0) {
                std::snprintf(buf, size_t(cap), "%s", v ? "Synth" : "Analog");
                return;
            }
            if (i == 1) {
                std::snprintf(buf, size_t(cap), "%+d", v - 24);
                return;
            }
            break;
        case kEnHat:
            if (i == 0) {
                std::snprintf(buf, size_t(cap), "%+d", v - 24);
                return;
            }
            break;
        case kEnAcid:
            if (i == 0) {
                std::snprintf(buf, size_t(cap), "%s", v ? "Square" : "Saw");
                return;
            }
            break;
        case kEnFm:
            if (i == 0) {
                std::snprintf(buf, size_t(cap), "x%s", kRatioNames[clampi(v, 0, 8)]);
                return;
            }
            if (i == 6) {
                std::snprintf(buf, size_t(cap), "%+d", v - 64);
                return;
            }
            break;
        default: break;
    }
    std::snprintf(buf, size_t(cap), "%d", v);
}

void comboParamText(const Combo& c, int which, char* buf, int cap) {
    const int v = which ? c.b : c.a;
    switch (c.action) {
        case kCaMultiball:
            if (which) std::snprintf(buf, size_t(cap), "-");
            else std::snprintf(buf, size_t(cap), "+%d ball%s", clampi(v, 1, 3), v > 1 ? "s" : "");
            return;
        case kCaFill:
            if (which) std::snprintf(buf, size_t(cap), "x%d hits", clampi(v, 2, 4));
            else std::snprintf(buf, size_t(cap), "%d moves", clampi(v, 1, 32));
            return;
        case kCaNote:
            if (which) std::snprintf(buf, size_t(cap), "ch %d", clampi(v, 1, 8));
            else noteName(v, buf, cap);
            return;
        case kCaCc:
            std::snprintf(buf, size_t(cap), which ? "val %d" : "CC %d", v);
            return;
        case kCaShake:
            if (which) std::snprintf(buf, size_t(cap), "-");
            else std::snprintf(buf, size_t(cap), "%d moves", clampi(v, 1, 32));
            return;
        case kCaScene:
            if (which) std::snprintf(buf, size_t(cap), "-");
            else if (v >= kPages) std::snprintf(buf, size_t(cap), "next page");
            else std::snprintf(buf, size_t(cap), "page %c", 'A' + v);
            return;
        default: std::snprintf(buf, size_t(cap), "%d", v); return;
    }
}

uint8_t randTargets(const RandCfg& r) {
    return uint8_t((r.tMatrix ? kRtMatrix : 0) | (r.tSteps ? kRtSteps : 0) | (r.tLanes ? kRtLanes : 0) |
                   (r.tRules ? kRtRules : 0) | (r.tSound ? kRtSound : 0));
}

// ---- the parameter table --------------------------------------------------------------
namespace {
const char* const kRateNames[kRateCount] = {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/16T", "1/8T"};
const char* const kDrainNames[2] = {"REST", "HOLD"};
const char* const kTiltNames[3] = {"FREEZE", "SHAKE", "BOTH"};
const char* const kGridNames[3] = {"BAR", "HALF", "BEAT"};
const char* const kPageNames[kPages] = {"A", "B", "C", "D"};
const char* const kScaleNames[kScaleCount] = {"Major", "Minor", "Dorian", "Phrygian", "Mixolyd",
                                             "HarmMin", "PentMaj", "PentMin", "Hijaz", "Chromatic"};
const char* const kStemNames[4] = {"1", "2", "3", "4"};
}  // namespace

#define PO(field) uint16_t(offsetof(Pattern, field))
#define CHO(field) uint16_t(offsetof(Pattern, ch[0].field)), uint16_t(sizeof(Channel))
const ParamDef kParams[] = {
    // TABLE
    {"Rate", "CLOCK", kPpTable, kFmtNames, PO(table.rate), 0, 0, kRateCount - 1, 2, kRateNames, 1},
    {"Reset", "CLOCK", kPpTable, kFmtBars, PO(table.resetBars), 0, 0, 64, 0, nullptr, 1},
    {"Start step", "CLOCK", kPpTable, kFmtInt, PO(table.startStep), 0, 1, 16, 1, nullptr, 1},
    {"Swing", "CLOCK", kPpTable, kFmtPct, PO(table.swing), 0, 0, 75, 0, nullptr, 1},
    {"Drain", "CLOCK", kPpTable, kFmtNames, PO(table.drain), 0, 0, 1, 0, kDrainNames, 1},
    {"Flip 1 step", "FLIPPERS", kPpTable, kFmtInt, PO(flips[0].step), 0, 1, 16, 1, nullptr, 1},
    {"Flip 2 step", "FLIPPERS", kPpTable, kFmtInt, PO(flips[1].step), 0, 1, 16, 5, nullptr, 1},
    {"Flip 3 step", "FLIPPERS", kPpTable, kFmtInt, PO(flips[2].step), 0, 1, 16, 9, nullptr, 1},
    {"Flip 4 step", "FLIPPERS", kPpTable, kFmtInt, PO(flips[3].step), 0, 1, 16, 13, nullptr, 1},
    {"Flip 1 note", "FLIPPERS", kPpTable, kFmtNote, PO(flips[0].note), 0, 0, 127, 36, nullptr, 1},
    {"Flip 2 note", "FLIPPERS", kPpTable, kFmtNote, PO(flips[1].note), 0, 0, 127, 37, nullptr, 1},
    {"Flip 3 note", "FLIPPERS", kPpTable, kFmtNote, PO(flips[2].note), 0, 0, 127, 38, nullptr, 1},
    {"Flip 4 note", "FLIPPERS", kPpTable, kFmtNote, PO(flips[3].note), 0, 0, 127, 39, nullptr, 1},
    {"Step notes", "FLIPPERS", kPpTable, kFmtNote, PO(table.stepNoteBase), 0, 0, 112, 48, nullptr, 1},
    {"Max flips", "TILT", kPpTable, kFmtBars, PO(table.tiltFlips), 0, 0, 16, 6, nullptr, 1},
    {"Hard vel", "TILT", kPpTable, kFmtBars, PO(table.tiltVel), 0, 0, 127, 124, nullptr, 1},
    {"Action", "TILT", kPpTable, kFmtNames, PO(table.tiltAction), 0, 0, 2, 0, kTiltNames, 1},
    {"Length", "TILT", kPpTable, kFmtBeats, PO(table.tiltBeats), 0, 1, 16, 4, nullptr, 1},
    {"Ball save", "BALL", kPpTable, kFmtBars, PO(table.saveBars), 0, 0, 8, 2, nullptr, 1},
    {"Gravity", "BALL", kPpTable, kFmtPct, PO(table.gravity), 0, 0, 100, 0, nullptr, 1},
    {"Grav. grid", "BALL", kPpTable, kFmtNames, PO(table.gravityGrid), 0, 0, 2, 0, kGridNames, 1},
    {"Max balls", "BALL", kPpTable, kFmtInt, PO(table.mbMax), 0, 1, 4, 3, nullptr, 1},
    {"Ball life", "BALL", kPpTable, kFmtInt, PO(table.mbLife), 0, 4, 64, 16, nullptr, 1},
    // EVOLVE
    {"Fatigue", "MEMORY", kPpEvolve, kFmtPct, PO(ev.fatigue), 0, 0, 100, 0, nullptr, 1},
    {"Reinforce", "MEMORY", kPpEvolve, kFmtPct, PO(ev.reinforce), 0, 0, 100, 0, nullptr, 1},
    {"Recover", "MEMORY", kPpEvolve, kFmtPct, PO(ev.recover), 0, 0, 100, 20, nullptr, 1},
    {"Amount", "MUTATION", kPpEvolve, kFmtPct, PO(ev.mutate), 0, 0, 100, 0, nullptr, 1},
    {"Home", "MUTATION", kPpEvolve, kFmtPct, PO(ev.mutHome), 0, 0, 100, 15, nullptr, 1},
    {"Spread", "MUTATION", kPpEvolve, kFmtPct, PO(ev.mutSpread), 0, 0, 100, 0, nullptr, 1},
    {"Meta", "META", kPpEvolve, kFmtOnOff, PO(ev.metaOn), 0, 0, 1, 0, nullptr, 1},
    {"Every", "META", kPpEvolve, kFmtBars, PO(ev.metaBars), 0, 1, 16, 4, nullptr, 1},
    {"Page", "META", kPpEvolve, kFmtNames, PO(ev.page), 0, 0, kPages - 1, 0, kPageNames, 1},
    {"Anchor", "RULES", kPpEvolve, kFmtStepOff, PO(ev.anchorStep), 0, 0, 16, 0, nullptr, 1},
    {"Every", "RULES", kPpEvolve, kFmtInt, PO(ev.anchorEvery), 0, 1, 64, 8, nullptr, 1},
    {"Max repeat", "RULES", kPpEvolve, kFmtBars, PO(ev.maxRepeat), 0, 0, 8, 0, nullptr, 1},
    {"No bounce", "RULES", kPpEvolve, kFmtOnOff, PO(ev.noBounce), 0, 0, 1, 0, nullptr, 1},
    // RANDOMIZER (EVOLVE page, third column)
    {"Amount", "RANDOMIZER", kPpRandom, kFmtPct, PO(rnd.amount), 0, 0, 100, 40, nullptr, 1},
    {"Density", "RANDOMIZER", kPpRandom, kFmtPct, PO(rnd.density), 0, 0, 100, 40, nullptr, 1},
    {"Matrix", "RANDOMIZER", kPpRandom, kFmtOnOff, PO(rnd.tMatrix), 0, 0, 1, 1, nullptr, 1},
    {"Steps", "RANDOMIZER", kPpRandom, kFmtOnOff, PO(rnd.tSteps), 0, 0, 1, 1, nullptr, 1},
    {"Lanes", "RANDOMIZER", kPpRandom, kFmtOnOff, PO(rnd.tLanes), 0, 0, 1, 1, nullptr, 1},
    {"Rules", "RANDOMIZER", kPpRandom, kFmtOnOff, PO(rnd.tRules), 0, 0, 1, 0, nullptr, 1},
    {"Sound", "RANDOMIZER", kPpRandom, kFmtOnOff, PO(rnd.tSound), 0, 0, 1, 0, nullptr, 1},
    // MELODY (STEPS page strip)
    {"Melody", "MELODY", kPpMelody, kFmtOnOff, PO(mel.on), 0, 0, 1, 0, nullptr, 1},
    {"Scale", "MELODY", kPpMelody, kFmtNames, PO(mel.scale), 0, 0, kScaleCount - 1, 1, kScaleNames, 1},
    {"Root", "MELODY", kPpMelody, kFmtNames, PO(mel.root), 0, 0, 11, 0, kNoteNames, 1},
    {"Oct up", "MELODY", kPpMelody, kFmtPct, PO(mel.octUp), 0, 0, 100, 0, nullptr, 1},
    // MIX (per channel)
    {"Level", "MIX", kPpMix, kFmtInt, CHO(level), 0, 127, 100, nullptr, kCh},
    {"Pan", "MIX", kPpMix, kFmtSigned, CHO(pan), 0, 127, 64, nullptr, kCh},
    {"Send A", "MIX", kPpMix, kFmtInt, CHO(sendA), 0, 127, 0, nullptr, kCh},
    {"Send B", "MIX", kPpMix, kFmtInt, CHO(sendB), 0, 127, 0, nullptr, kCh},
    {"Choke", "MIX", kPpMix, kFmtBars, CHO(choke), 0, 4, 0, nullptr, kCh},
    {"Stem", "MIX", kPpMix, kFmtNames, CHO(stem), 0, 3, 0, kStemNames, kCh},
    {"Mute", "MIX", kPpMix, kFmtOnOff, CHO(mute), 0, 1, 0, nullptr, kCh},
    {"Solo", "MIX", kPpMix, kFmtOnOff, CHO(solo), 0, 1, 0, nullptr, kCh},
    // FX
    {"Dly time", "DELAY A", kPpFx, kFmtNames, PO(fx.dlyTime), 0, 0, kDelayTimeCount - 1, 3, kDelayTimeNames, 1},
    {"Feedback", "DELAY A", kPpFx, kFmtPct, PO(fx.dlyFb), 0, 0, 95, 45, nullptr, 1},
    {"Tone", "DELAY A", kPpFx, kFmtPct, PO(fx.dlyTone), 0, 0, 100, 70, nullptr, 1},
    {"Width", "DELAY A", kPpFx, kFmtPct, PO(fx.dlyWidth), 0, 0, 100, 60, nullptr, 1},
    {"Size", "REVERB B", kPpFx, kFmtPct, PO(fx.revSize), 0, 0, 100, 60, nullptr, 1},
    {"Damp", "REVERB B", kPpFx, kFmtPct, PO(fx.revDamp), 0, 0, 100, 50, nullptr, 1},
    {"Return", "REVERB B", kPpFx, kFmtPct, PO(fx.revLevel), 0, 0, 100, 80, nullptr, 1},
    {"Drive", "MASTER", kPpFx, kFmtPct, PO(fx.drive), 0, 0, 100, 0, nullptr, 1},
    {"Master", "MASTER", kPpFx, kFmtInt, PO(fx.master), 0, 0, 127, 90, nullptr, 1},
    // CHANNEL (per channel, the CHANNELS page)
    {"Note", "CHANNEL", kPpChannel, kFmtNote, CHO(note), 0, 127, 36, nullptr, kCh},
    {"MIDI ch", "CHANNEL", kPpChannel, kFmtMidiCh, CHO(midiCh), 0, 16, 1, nullptr, kCh},
    {"Follow", "CHANNEL", kPpChannel, kFmtOnOff, CHO(follow), 0, 1, 0, nullptr, kCh},
    {"Legato", "CHANNEL", kPpChannel, kFmtOnOff, CHO(legato), 0, 1, 0, nullptr, kCh},
    {"Choke", "CHANNEL", kPpChannel, kFmtBars, CHO(choke), 0, 4, 0, nullptr, kCh},
};
#undef PO
#undef CHO
const int kParamCount = int(sizeof kParams / sizeof kParams[0]);

int paramsOfPage(int page, int* out, int cap) {
    int n = 0;
    for (int i = 0; i < kParamCount && n < cap; ++i)
        if (kParams[i].page == page) out[n++] = i;
    return n;
}

int paramGet(const Pattern& p, int id, int index) {
    if (id < 0 || id >= kParamCount) return 0;
    const ParamDef& d = kParams[id];
    index = d.count > 1 ? clampi(index, 0, d.count - 1) : 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p) + d.offset + d.stride * index;
    return *b;
}

void paramSet(Pattern& p, int id, int v, int index) {
    if (id < 0 || id >= kParamCount) return;
    const ParamDef& d = kParams[id];
    index = d.count > 1 ? clampi(index, 0, d.count - 1) : 0;
    uint8_t* b = reinterpret_cast<uint8_t*>(&p) + d.offset + d.stride * index;
    *b = uint8_t(clampi(v, d.min, d.max));
}

void paramText(const Pattern& p, int id, int index, char* buf, int cap) {
    if (id < 0 || id >= kParamCount) {
        buf[0] = 0;
        return;
    }
    const ParamDef& d = kParams[id];
    const int v = paramGet(p, id, index);
    switch (d.fmt) {
        case kFmtPct: std::snprintf(buf, size_t(cap), "%d%%", v); break;
        case kFmtOnOff: std::snprintf(buf, size_t(cap), "%s", v ? "ON" : "off"); break;
        case kFmtNames: std::snprintf(buf, size_t(cap), "%s", d.names[clampi(v - d.min, 0, d.max - d.min)]); break;
        case kFmtStepOff:
        case kFmtBars:
            if (v == 0) std::snprintf(buf, size_t(cap), "off");
            else std::snprintf(buf, size_t(cap), "%d", v);
            break;
        case kFmtNote: noteName(v, buf, cap); break;
        case kFmtSigned: std::snprintf(buf, size_t(cap), "%+d", v - 64); break;
        case kFmtMidiCh:
            if (v == 0) std::snprintf(buf, size_t(cap), "off");
            else std::snprintf(buf, size_t(cap), "%d", v);
            break;
        case kFmtBeats: std::snprintf(buf, size_t(cap), "%d beat%s", v, v == 1 ? "" : "s"); break;
        default: std::snprintf(buf, size_t(cap), "%d", v); break;
    }
}

// ---- defaults -----------------------------------------------------------------------------
namespace {
void setChannel(Channel& c, int type, int note, int midiCh) {
    const Channel blank;
    c = blank;
    engineDefaults(c, type);
    c.note = uint8_t(note);
    c.midiCh = uint8_t(midiCh);
}
}  // namespace

void defaultKit(Pattern& p) {
    setChannel(p.ch[0], kEnKick, 36, 10);
    p.ch[0].stem = 0;
    setChannel(p.ch[1], kEnSnare, 38, 10);
    p.ch[1].stem = 0;
    p.ch[1].sendB = 20;
    setChannel(p.ch[2], kEnHat, 42, 10);
    p.ch[2].choke = 1;
    p.ch[2].level = 80;
    p.ch[2].pan = 52;
    p.ch[2].stem = 1;
    setChannel(p.ch[3], kEnHat, 46, 10);
    p.ch[3].p[1] = 85;   // open hat: long decay
    p.ch[3].p[2] = 70;
    p.ch[3].choke = 1;
    p.ch[3].level = 75;
    p.ch[3].pan = 78;
    p.ch[3].stem = 1;
    setChannel(p.ch[4], kEnSample, 39, 10);
    p.ch[4].p[0] = 1;   // CLAP
    p.ch[4].sendB = 35;
    p.ch[4].stem = 1;
    setChannel(p.ch[5], kEnAcid, 36, 2);
    p.ch[5].follow = 1;
    p.ch[5].legato = 1;
    p.ch[5].level = 85;
    p.ch[5].sendA = 25;
    p.ch[5].stem = 2;
    setChannel(p.ch[6], kEnFm, 60, 3);
    p.ch[6].follow = 1;
    p.ch[6].level = 70;
    p.ch[6].sendA = 40;
    p.ch[6].sendB = 50;
    p.ch[6].stem = 2;
    setChannel(p.ch[7], kEnSample, 48, 10);
    p.ch[7].p[0] = 0;    // BREAK
    p.ch[7].p[1] = 16;   // 16 slices
    p.ch[7].p[2] = 1;    // slice = step
    p.ch[7].level = 90;
    p.ch[7].stem = 3;
    p.fx = Fx();
}

namespace {
void chain(Pattern& p, int pg) {
    std::memset(p.w[pg], 0, sizeof p.w[pg]);
    for (int r = 0; r < kStates; ++r) p.w[pg][r][r >= kSteps ? 1 : r + 1] = 100;
}
inline uint8_t& W(Pattern& p, int pg, int r, int c) { return p.w[pg][r][c]; }
int wrapStep(int s) { return ((s - 1) % kSteps + kSteps) % kSteps + 1; }
}  // namespace

void defaultPattern(Pattern& p) {
    p = Pattern();
    std::snprintf(p.name, sizeof p.name, "INIT");
    for (int pg = 0; pg < kPages; ++pg) chain(p, pg);
    for (int i = 0; i < kFlips; ++i) {
        p.flips[i].step = uint8_t(1 + 4 * i);
        p.flips[i].note = uint8_t(36 + i);
    }
    // a plain two-step groove on the chain
    static const uint8_t kTrig[kSteps] = {1, 3, 3, 0, 2, 3, 1, 4, 3, 1, 3, 3, 2, 3, 5, 4};
    for (int s = 0; s < kSteps; ++s) {
        p.steps[s] = Step();
        p.steps[s].trig = kTrig[s];
        p.steps[s].vel = uint8_t(s % 4 == 0 ? 118 : (s % 2 ? 84 : 100));
    }
    defaultKit(p);
    p.size = uint16_t(sizeof(Pattern));
}

// ---- shapes ---------------------------------------------------------------------------------
void applyShape(Pattern& p, int pg, int shape, Rng32& r) {
    pg = clampi(pg, 0, kPages - 1);
    std::memset(p.w[pg], 0, sizeof p.w[pg]);
    const int start = clampi(p.table.startStep, 1, kSteps);
    if (shape != kShClear) W(p, pg, 0, start) = 100;
    for (int s = 1; s <= kSteps; ++s) {
        switch (shape) {
            case kShChain: W(p, pg, s, wrapStep(s + 1)) = 100; break;
            case kShReverse: W(p, pg, s, wrapStep(s - 1)) = 100; break;
            case kShPingPong:
                // up the odd steps, down the even ones: 1 3 5 .. 15 16 14 .. 2 1
                if (s % 2) W(p, pg, s, s == 15 ? 16 : s + 2) = 100;
                else W(p, pg, s, s == 2 ? 1 : s - 2) = 100;
                break;
            case kShDrunk:
                W(p, pg, s, wrapStep(s + 1)) = 45;
                W(p, pg, s, wrapStep(s - 1)) = 45;
                W(p, pg, s, s) = 10;
                break;
            case kShJumps:
                W(p, pg, s, wrapStep(s + 1)) = 70;
                W(p, pg, s, wrapStep(s + 4)) = 20;
                W(p, pg, s, wrapStep(s + 8)) = 10;
                break;
            case kShEuclid: {
                // towards the next quarter (1, 5, 9, 13), or one step on
                const int q = ((s - 1) / 4 + 1) * 4 + 1;
                W(p, pg, s, wrapStep(q)) = 60;
                W(p, pg, s, wrapStep(s + 1)) = uint8_t(W(p, pg, s, wrapStep(s + 1)) + 40);
                break;
            }
            case kShStay:
                W(p, pg, s, s) = 60;
                W(p, pg, s, wrapStep(s + 1)) = 40;
                break;
            case kShHub:
                if (s == 1) {
                    for (int c = 2; c <= kSteps; ++c) W(p, pg, s, c) = 20;
                } else {
                    W(p, pg, s, 1) = 50;
                    W(p, pg, s, wrapStep(s + 1)) = 50;
                }
                break;
            case kShRandom: {
                const int k = 1 + clampi(p.rnd.density, 0, 100) * 4 / 100;
                for (int i = 0; i < k; ++i) W(p, pg, s, 1 + r.below(kSteps)) = uint8_t(20 + r.below(81));
                if (r.pct(8)) W(p, pg, s, 0) = uint8_t(5 + r.below(20));
                break;
            }
            default: break;
        }
    }
}

void applyRowFn(Pattern& p, int pg, int row, int fn, Rng32& r) {
    pg = clampi(pg, 0, kPages - 1);
    row = clampi(row, 0, kStates - 1);
    uint8_t* w = p.w[pg][row];
    std::memset(w, 0, kStates);
    const int s = row == 0 ? 0 : row;
    switch (fn) {
        case kRfNext: w[row == 0 ? clampi(p.table.startStep, 1, kSteps) : wrapStep(s + 1)] = 100; break;
        case kRfEqual:
            for (int c = 1; c < kStates; ++c) w[c] = 50;
            break;
        case kRfRandom: {
            const int k = 1 + clampi(p.rnd.density, 0, 100) * 4 / 100;
            for (int i = 0; i < k; ++i) w[1 + r.below(kSteps)] = uint8_t(20 + r.below(81));
            break;
        }
        case kRfStay: w[row == 0 ? 1 : row] = 100; break;
        case kRfBack: w[row == 0 ? kSteps : wrapStep(s - 1)] = 100; break;
        default: break;
    }
}

// ---- randomizer -------------------------------------------------------------------------------
void randomizeTrigs(Pattern& p, Rng32& r) {
    for (auto& s : p.steps)
        if (!s.lock) s.trig = uint8_t(r.below(10));
}

void clearSteps(Pattern& p) {
    for (auto& s : p.steps) {
        const uint8_t lock = s.lock;
        s = Step();
        s.lock = lock;
    }
}

void randomize(Pattern& p, Rng32& r, uint8_t targets) {
    const int amt = clampi(p.rnd.amount, 0, 100);
    const float a = float(amt) * 0.01f;
    const uint16_t rl = p.rnd.rowLocks;
    auto rowFree = [rl](int row) { return !(rl & (1u << row)); };
    if (targets & kRtMatrix) {
        const int pg = clampi(p.ev.page, 0, kPages - 1);
        for (int row = 0; row < kStates; ++row) {
            if (row >= 1 && p.steps[row - 1].lock) continue;
            uint8_t rnd[kStates] = {};
            const int k = 1 + clampi(p.rnd.density, 0, 100) * 4 / 100;
            for (int i = 0; i < k; ++i) rnd[1 + r.below(kSteps)] = uint8_t(20 + r.below(81));
            if (row != 0 && r.pct(6)) rnd[0] = uint8_t(5 + r.below(15));
            uint8_t* w = p.w[pg][row];
            int sum = 0;
            for (int c = 0; c < kStates; ++c) {
                const float v = float(w[c]) + (float(rnd[c]) - float(w[c])) * a;
                w[c] = uint8_t(clampi(int(v + 0.5f), 0, 100));
                sum += w[c];
            }
            if (sum == 0) w[row == 0 ? 1 : wrapStep(row + 1)] = 100;
        }
    }
    for (int s = 0; s < kSteps; ++s) {
        Step& st = p.steps[s];
        if (st.lock) continue;
        if ((targets & kRtLanes) && rowFree(kRowTrig) && r.pct(amt)) {
            if (r.pct(72)) {
                int ch = r.below(kCh);
                for (int t = 0; t < kCh && p.ch[ch].mute; ++t) ch = (ch + 1) % kCh;
                st.trig = uint8_t(1 + ch);
            } else {
                st.trig = 0;
            }
        }
        if (!(targets & kRtSteps)) continue;
        if (rowFree(kRowTrigP) && r.pct(amt)) st.trigP = uint8_t(r.pct(55) ? 100 : 40 + r.below(61));
        if (rowFree(kRowDelay) && r.pct(amt)) st.delay = uint8_t(r.pct(70) ? 0 : 1 + r.below(6));
        if (rowFree(kRowDelayP) && r.pct(amt)) st.delayP = uint8_t(50 + r.below(51));
        if (rowFree(kRowVel) && r.pct(amt)) st.vel = uint8_t(50 + r.below(78));
        if (rowFree(kRowLen) && r.pct(amt)) st.len = uint8_t(1 + r.below(8));
        if (rowFree(kRowBumper) && r.pct(amt)) st.bumper = uint8_t(r.pct(78) ? 0 : 1 + r.below(2));
        if (rowFree(kRowRamp) && r.pct(amt)) st.ramp = uint8_t(r.pct(88) ? 0 : 1 + r.below(kSteps));
        if (rowFree(kRowDegree) && r.pct(amt)) st.degree = int8_t(r.below(12) - 2);
    }
    if (targets & kRtRules) {
        Evolve& ev = p.ev;
        if (r.pct(amt)) ev.anchorStep = uint8_t(r.pct(50) ? 0 : 1 + 4 * r.below(4));
        if (r.pct(amt)) ev.anchorEvery = uint8_t(4 << r.below(3));
        if (r.pct(amt)) ev.maxRepeat = uint8_t(r.below(4));
        if (r.pct(amt)) ev.noBounce = uint8_t(r.below(2));
        if (r.pct(amt)) p.table.gravity = uint8_t(r.below(51));
        if (r.pct(amt)) ev.mutate = uint8_t(r.below(41));
        if (r.pct(amt)) ev.fatigue = uint8_t(r.below(51));
        if (r.pct(amt)) ev.reinforce = uint8_t(r.below(31));
    }
    if (targets & kRtSound) {
        for (auto& c : p.ch) {
            for (int i = 0; i < kParamsPerEngine; ++i) {
                const EngParam& ep = kEngineParams[c.type][i];
                if (!ep.name || !r.pct(amt)) continue;
                if (c.type == kEnSample && i == 0) continue;   // keep the source
                const int span = ep.max - ep.min;
                const int d = int(float(span) * a * (r.uni() - 0.5f));
                c.p[i] = uint8_t(clampi(c.p[i] + d, ep.min, ep.max));
            }
        }
    }
}

// ---- bytes ------------------------------------------------------------------------------------
void sanitize(Pattern& p) {
    p.name[kNameLen - 1] = 0;
    for (int pg = 0; pg < kPages; ++pg)
        for (int r = 0; r < kStates; ++r)
            for (int c = 0; c < kStates; ++c) {
                p.w[pg][r][c] = uint8_t(clampi(p.w[pg][r][c], 0, 100));
                Edge& e = p.e[pg][r][c];
                e.tr = int8_t(clampi(e.tr, -12, 12));
                e.dly = uint8_t(clampi(e.dly, 0, 11));
            }
    for (auto& s : p.steps) {
        s.trig = uint8_t(clampi(s.trig, 0, 9));
        s.trigP = uint8_t(clampi(s.trigP, 0, 100));
        s.delay = uint8_t(clampi(s.delay, 0, 23));
        s.delayP = uint8_t(clampi(s.delayP, 0, 100));
        s.vel = uint8_t(clampi(s.vel, 1, 127));
        s.len = uint8_t(clampi(s.len, 1, 64));
        s.bumper = uint8_t(clampi(s.bumper, 0, 4));
        s.ramp = uint8_t(clampi(s.ramp, 0, 16));
        s.degree = int8_t(clampi(s.degree, -7, 21));
        s.lock = s.lock ? 1 : 0;
    }
    for (auto& c : p.combos) {
        c.len = uint8_t(c.len < 2 ? 0 : clampi(c.len, 2, kComboLen));
        for (auto& s : c.path) s = uint8_t(clampi(s, 1, 16));
        c.action = uint8_t(clampi(c.action, 0, kComboActions - 1));
    }
    for (auto& f : p.flips) f.step = uint8_t(clampi(f.step, 1, 16));
    for (int i = 0; i < kParamCount; ++i)
        for (int k = 0; k < kParams[i].count; ++k) paramSet(p, i, paramGet(p, i, k), k);
    for (auto& c : p.ch) {
        c.type = uint8_t(clampi(c.type, 0, kEngines - 1));
        for (int i = 0; i < kParamsPerEngine; ++i) {
            const EngParam& ep = kEngineParams[c.type][i];
            c.p[i] = ep.name ? uint8_t(clampi(c.p[i], ep.min, ep.max)) : 0;
        }
        c.sample[kPathLen - 1] = 0;
    }
    for (auto& row : p.ev.meta)
        for (auto& v : row) v = uint8_t(clampi(v, 0, 100));
    if (p.bpmX10 < 200 || p.bpmX10 > 4000) p.bpmX10 = 1700;
}

uint32_t patternToBytes(const Pattern& p, uint8_t* out, uint32_t cap) {
    if (cap < sizeof(Pattern)) return 0;
    std::memcpy(out, &p, sizeof(Pattern));
    Pattern* q = reinterpret_cast<Pattern*>(out);
    q->magic = kPatternMagic;
    q->version = kPatternVersion;
    q->size = uint16_t(sizeof(Pattern));
    return uint32_t(sizeof(Pattern));
}

bool patternFromBytes(Pattern& p, const uint8_t* data, uint32_t len) {
    if (len < 8) return false;
    uint32_t magic;
    uint16_t version, size;
    std::memcpy(&magic, data, 4);
    std::memcpy(&version, data + 4, 2);
    std::memcpy(&size, data + 6, 2);
    if (magic != kPatternMagic || version == 0 || size < 64) return false;
    defaultPattern(p);
    uint32_t n = size < len ? size : len;
    if (n > sizeof(Pattern)) n = sizeof(Pattern);
    std::memcpy(&p, data, n);
    p.magic = kPatternMagic;
    p.version = kPatternVersion;
    p.size = uint16_t(sizeof(Pattern));
    sanitize(p);
    return true;
}

// ---- factory patterns ------------------------------------------------------------------------
const char* const kFactoryNames[kFactoryCount] = {"CHAIN", "PINGPONG", "DRUNK", "FLIPPERS", "BUMPERS",
                                                  "RAMPS", "COMBOS", "GRAVITY", "MEMORY", "MUTATION",
                                                  "META", "EDGES", "RULES", "TILT"};
const char* const kFactoryIdeas[kFactoryCount] = {
    "Pinball's default: 1 > 2 > .. > 16, a plain groove",
    "Up the odd steps, down the even: an acid line in melody mode",
    "A drunk walk, one step left or right, FM in Hijaz",
    "The ball stays put: steer it with the flippers",
    "Jungle hats and snares: bumpers ratchet inside the move",
    "Ramps force the next move: 8 > 1 and 16 > 9 loops",
    "Hit 1-5-9 for MULTIBALL, 4-8 for a FILL, 13-16 for the next page",
    "Random walk, gravity pulls the ball home on every bar",
    "Fatigue: used edges tire, the ball finds new routes",
    "The weights drift every bar and are pulled back home",
    "Four pages A-D, the meta matrix picks one every 2 bars",
    "Edges transpose, delay and accent the notes they lead to",
    "Anchor on 1 every 8 moves, no bounce back, max 2 repeats",
    "Twitchy table: 3 flips a bar tilt it (shake, then freeze)",
};

namespace {
void steps(Pattern& p, const uint8_t* trig, const uint8_t* vel = nullptr) {
    for (int s = 0; s < kSteps; ++s) {
        p.steps[s] = Step();
        p.steps[s].trig = trig[s];
        if (vel) p.steps[s].vel = vel[s];
    }
}
}  // namespace

void factoryPattern(int i, Pattern& p) {
    defaultPattern(p);
    i = clampi(i, 0, kFactoryCount - 1);
    std::snprintf(p.name, sizeof p.name, "%s", kFactoryNames[i]);
    Rng32 r;
    r.seed(0x7117u + uint32_t(i) * 7919u);
    switch (i) {
        case 0:   // CHAIN: the default groove
            p.bpmX10 = 1240;
            p.table.swing = 12;
            break;
        case 1: {   // PINGPONG: acid in melody mode
            for (int pg = 0; pg < kPages; ++pg) applyShape(p, pg, kShPingPong, r);
            static const uint8_t t[16] = {6, 6, 1, 6, 6, 3, 6, 1, 6, 6, 2, 6, 6, 3, 6, 6};
            steps(p, t);
            static const int8_t deg[16] = {0, 2, 0, 4, 7, 0, 3, 0, 5, 7, 0, 2, 9, 0, 4, 7};
            for (int s = 0; s < kSteps; ++s) {
                p.steps[s].degree = deg[s];
                p.steps[s].len = uint8_t(s % 3 == 0 ? 6 : 3);
                p.steps[s].vel = uint8_t(s % 4 == 0 ? 120 : 92);
            }
            p.mel.on = 1;
            p.mel.scale = 1;
            p.mel.root = 2;
            p.bpmX10 = 1320;
            break;
        }
        case 2: {   // DRUNK: FM in Hijaz
            for (int pg = 0; pg < kPages; ++pg) applyShape(p, pg, kShDrunk, r);
            static const uint8_t t[16] = {7, 3, 7, 1, 7, 3, 7, 2, 7, 3, 7, 1, 7, 4, 7, 2};
            steps(p, t);
            for (int s = 0; s < kSteps; ++s) {
                p.steps[s].degree = int8_t(s);
                p.steps[s].len = 6;
            }
            p.ch[6].sendB = 70;
            p.mel.on = 1;
            p.mel.scale = 8;
            p.mel.octUp = 10;
            p.bpmX10 = 1050;
            break;
        }
        case 3: {   // FLIPPERS: a sticky table
            for (int pg = 0; pg < kPages; ++pg) applyShape(p, pg, kShStay, r);
            for (int s = 1; s <= kSteps; ++s) {
                W(p, 0, s, s) = 85;
                W(p, 0, s, wrapStep(s + 1)) = 15;
            }
            static const uint8_t t[16] = {1, 3, 3, 3, 2, 3, 4, 3, 1, 1, 3, 3, 2, 3, 5, 4};
            steps(p, t);
            p.table.tiltFlips = 10;
            p.bpmX10 = 1700;
            break;
        }
        case 4: {   // BUMPERS: jungle
            static const uint8_t t[16] = {1, 3, 8, 3, 2, 3, 8, 1, 3, 1, 8, 3, 2, 3, 2, 4};
            steps(p, t);
            p.steps[1].bumper = 1;
            p.steps[5].bumper = 2;
            p.steps[11].bumper = 1;
            p.steps[14].bumper = 3;
            p.steps[14].vel = 90;
            p.steps[2].trigP = 70;
            p.steps[6].trigP = 70;
            p.steps[10].trigP = 70;
            for (int s = 1; s <= kSteps; ++s) {
                W(p, 0, s, wrapStep(s + 1)) = 85;
                W(p, 0, s, wrapStep(s + 5)) = 15;
            }
            p.ch[7].p[2] = 1;
            p.table.swing = 18;
            p.bpmX10 = 1720;
            break;
        }
        case 5: {   // RAMPS
            p.steps[7].ramp = 1;
            p.steps[15].ramp = 9;
            p.steps[7].vel = 127;
            for (int s = 1; s <= kSteps; ++s) {
                W(p, 0, s, wrapStep(s + 1)) = 80;
                W(p, 0, s, wrapStep(s + 3)) = 20;
            }
            p.table.resetBars = 4;
            p.bpmX10 = 1360;
            break;
        }
        case 6: {   // COMBOS
            applyShape(p, 0, kShJumps, r);
            applyShape(p, 1, kShEuclid, r);
            p.combos[0] = Combo();
            p.combos[0].len = 3;
            p.combos[0].path[0] = 1;
            p.combos[0].path[1] = 5;
            p.combos[0].path[2] = 9;
            p.combos[0].action = kCaMultiball;
            p.combos[0].a = 2;
            p.combos[1].len = 2;
            p.combos[1].path[0] = 4;
            p.combos[1].path[1] = 8;
            p.combos[1].action = kCaFill;
            p.combos[1].a = 4;
            p.combos[1].b = 3;
            p.combos[2].len = 4;
            p.combos[2].path[0] = 13;
            p.combos[2].path[1] = 14;
            p.combos[2].path[2] = 15;
            p.combos[2].path[3] = 16;
            p.combos[2].action = kCaScene;
            p.combos[2].a = kPages;
            p.combos[3].len = 2;
            p.combos[3].path[0] = 3;
            p.combos[3].path[1] = 7;
            p.combos[3].action = kCaShake;
            p.combos[3].a = 3;
            p.table.mbMax = 4;
            p.bpmX10 = 1600;
            break;
        }
        case 7: {   // GRAVITY
            p.rnd.density = 60;
            applyShape(p, 0, kShRandom, r);
            p.table.gravity = 85;
            p.table.gravityGrid = 0;
            p.ev.noBounce = 1;
            p.bpmX10 = 1450;
            break;
        }
        case 8: {   // MEMORY
            applyShape(p, 0, kShHub, r);
            p.ev.fatigue = 60;
            p.ev.recover = 10;
            p.ev.reinforce = 0;
            static const uint8_t t[16] = {1, 3, 6, 3, 2, 6, 3, 4, 6, 1, 3, 6, 2, 3, 7, 4};
            steps(p, t);
            for (int s = 0; s < kSteps; ++s) p.steps[s].degree = int8_t((s * 3) % 8);
            p.mel.on = 1;
            p.mel.scale = 7;
            p.bpmX10 = 1260;
            break;
        }
        case 9: {   // MUTATION
            p.ev.mutate = 45;
            p.ev.mutHome = 10;
            p.ev.mutSpread = 40;
            for (int s = 1; s <= kSteps; ++s) W(p, 0, s, wrapStep(s + 2)) = 20;
            p.bpmX10 = 1380;
            break;
        }
        case 10: {   // META: four pages
            chain(p, 0);
            applyShape(p, 1, kShJumps, r);
            applyShape(p, 2, kShDrunk, r);
            applyShape(p, 3, kShReverse, r);
            p.ev.metaOn = 1;
            p.ev.metaBars = 2;
            static const uint8_t m[4][4] = {{40, 40, 10, 10}, {30, 30, 30, 10}, {50, 0, 25, 25}, {70, 10, 10, 10}};
            std::memcpy(p.ev.meta, m, sizeof m);
            p.bpmX10 = 1400;
            break;
        }
        case 11: {   // EDGES
            static const uint8_t t[16] = {6, 3, 6, 1, 6, 2, 6, 3, 6, 1, 6, 2, 6, 3, 6, 4};
            steps(p, t);
            for (int s = 1; s <= kSteps; ++s) {
                W(p, 0, s, wrapStep(s + 1)) = 70;
                W(p, 0, s, wrapStep(s + 3)) = 30;
                p.e[0][s][wrapStep(s + 3)].tr = int8_t((s % 3) * 5 - 5);
                p.e[0][s][wrapStep(s + 3)].vel = 20;
                if (s % 4 == 2) p.e[0][s][wrapStep(s + 1)].dly = 4;
            }
            p.mel.on = 1;
            p.mel.scale = 3;
            p.bpmX10 = 1250;
            break;
        }
        case 12: {   // RULES
            p.rnd.density = 75;
            applyShape(p, 0, kShRandom, r);
            p.ev.anchorStep = 1;
            p.ev.anchorEvery = 8;
            p.ev.noBounce = 1;
            p.ev.maxRepeat = 2;
            p.bpmX10 = 1500;
            break;
        }
        case 13: {   // TILT
            applyShape(p, 0, kShJumps, r);
            p.table.tiltFlips = 3;
            p.table.tiltAction = 2;
            p.table.tiltBeats = 8;
            p.table.saveBars = 4;
            static const uint8_t t[16] = {1, 3, 3, 2, 3, 1, 3, 4, 1, 3, 2, 3, 3, 1, 2, 4};
            steps(p, t);
            p.bpmX10 = 1740;
            break;
        }
        default: break;
    }
    sanitize(p);
}

}  // namespace tilt
