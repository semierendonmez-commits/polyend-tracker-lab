// FLIPPER -- input: buttons, pads, encoder per page; the editors' value rows.
#include <cstdio>

#include "ws/flip_ws.h"

namespace flip {

namespace btn = lab::btn;
using lab::Event;

namespace {

int clampNote(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

template <class T>
void bump(T& v, int d, int lo, int hi) {
    int x = int(v) + d;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    v = T(x);
}
void bumpInst(uint8_t& v, int d) {
    int x = v == kNone ? -1 : int(v);
    x += d;
    if (x < -1) x = -1;
    if (x > kInsts - 1) x = kInsts - 1;
    v = x < 0 ? kNone : uint8_t(x);
}

// Edit-page rows.
enum EditRow : uint8_t {
    eType, eInst, eNote, eVel, ePitch, eVelMode, eChance, ePoints, eWN, eWE, eWS, eWW, eDir, eFx, eAmount, eLen,
    eGroups, eEvent, eArg, eSide, ePower, eWall, eHold, eCount
};
const char* const kEditLabels[eCount] = {
    "Element", "Sound", "Note", "Velocity", "Pitch", "Velocity mode", "Chance", "Points", "Move up",
    "Move right", "Move down", "Move left", "Throw direction", "Effect", "Amount", "Sweep length", "Effect on",
    "Event", "Event table", "Side", "Power", "Shape", "Hold"};
const uint8_t kRowsEmpty[] = {eType, eWN, eWE, eWS, eWW};
const uint8_t kRowsBumper[] = {eType, eInst, eNote, eVel, ePitch, eVelMode, eChance, ePoints, eWN, eWE, eWS, eWW};
const uint8_t kRowsRamp[] = {eType, eFx, eAmount, eLen, eGroups, eDir, eInst, eNote, eVel, ePoints, eWN, eWE, eWS, eWW};
const uint8_t kRowsHole[] = {eType, eEvent, eArg, eHold, eInst, eNote, eVel, ePoints, eWN, eWE, eWS, eWW};
const uint8_t kRowsTarget[] = {eType, eEvent, eArg, eInst, eNote, eVel, ePoints, eWN, eWE, eWS, eWW};
const uint8_t kRowsFlipper[] = {eType, eSide, ePower, eInst, eNote, eVel, ePoints, eWN};
const uint8_t kRowsWall[] = {eType, eWall};
struct RowSet {
    const uint8_t* r;
    int n;
};
RowSet rowsFor(uint8_t type) {
    switch (type) {
        case kBumper: return {kRowsBumper, int(sizeof kRowsBumper)};
        case kRamp: return {kRowsRamp, int(sizeof kRowsRamp)};
        case kHole: return {kRowsHole, int(sizeof kRowsHole)};
        case kTarget: return {kRowsTarget, int(sizeof kRowsTarget)};
        case kFlipper: return {kRowsFlipper, int(sizeof kRowsFlipper)};
        case kWall: return {kRowsWall, int(sizeof kRowsWall)};
        default: return {kRowsEmpty, int(sizeof kRowsEmpty)};
    }
}

// Tables-page rows.
constexpr int kTabGraph = 18, kTabRails = kTabGraph + kTables, kTabPreset = kTabRails + 18, kTabFile = kTabPreset + 1,
              kTabRows = kTabFile + 1;
const char* const kTabLabels[kTabGraph] = {"Enabled",   "Colour",         "Step",          "Swing",
                                           "Scale",     "Root",           "Gravity",       "Balls",
                                           "Length",    "Sides",          "Ball save",     "Jackpot at",
                                           "On jackpot", "Jackpot table", "On multiball",  "Multiball table",
                                           "On game over", "Game over table"};
const char* const kRailLabels[6] = {"sound", "note", "hits", "steps", "rotate", "from heat"};
const char* const kColourNames[6] = {"Cyan", "Pink", "Yellow", "Green", "Violet", "Orange"};
const char* const kSidesNames[2] = {"Bounce", "Wrap around"};

// Run-page rows.
constexpr int kRunRows = 6;
const char* const kRunLabels[kRunRows] = {"Machine slot", "Capture", "Clip start", "Clip end", "Replay sound",
                                          "New machine"};
const char* const kCaptureNames[kCaptureCount] = {"Whole game", "Last 4 bars", "Last 8 bars", "Last 16 bars",
                                                  "Last 32 bars", "Last 64 bars"};

// Sounds-page rows.
constexpr int kSndRows = 14;
const char* const kSndLabels[kSndRows] = {"Engine",     "Level",       "Pan",        "Decay", "Tone",
                                          "Character A", "Character B", "Delay send", "Reverb send", "Group",
                                          "MIDI out",   "Root note",   "Colour",     "Sample"};
const char* const kCharA[kEnCount] = {"Punch", "Noise", "Metal", "Sweep", "Body", "Ratio", "Shape", "-"};
const char* const kCharB[kEnCount] = {"Sweep time", "-", "-", "FM", "-", "Index", "Resonance", "-"};

}  // namespace

// ---- rows: text ----------------------------------------------------------------------------
int Flipper::editRows() const { return rowsFor(m_->tab[game_.tableIndex()].cells[editCell_].type).n; }
int Flipper::editRowId(int i) const {
    const RowSet s = rowsFor(m_->tab[game_.tableIndex()].cells[editCell_].type);
    return i >= 0 && i < s.n ? int(s.r[i]) : int(eType);
}

void Flipper::editRowText(int i, char* label, char* value) const {
    const Cell& c = m_->tab[game_.tableIndex()].cells[editCell_];
    const int id = editRowId(i);
    lab::copyStr(label, 32, kEditLabels[id]);
    char nn[8];
    switch (id) {
        case eType: lab::copyStr(value, 32, kElemNames[c.type]); break;
        case eInst: lab::copyStr(value, 32, c.inst == kNone ? "None" : m_->inst[c.inst].name); break;
        case eNote: noteName(c.note, nn); std::snprintf(value, 32, "%s (%d)", nn, c.note); break;
        case eVel: std::snprintf(value, 32, "%d", c.vel); break;
        case ePitch: lab::copyStr(value, 32, kPitchNames[c.pitchMode]); break;
        case eVelMode: lab::copyStr(value, 32, kVelNames[c.velMode]); break;
        case eChance: std::snprintf(value, 32, "%d %%", c.chance); break;
        case ePoints: std::snprintf(value, 32, "%d", c.points * 10); break;
        case eWN: case eWE: case eWS: case eWW: std::snprintf(value, 32, "%d", c.w[id - eWN]); break;
        case eDir: lab::copyStr(value, 32, kDirNames[c.dir > 8 ? 8 : c.dir]); break;
        case eFx: lab::copyStr(value, 32, kFxNames[c.fx]); break;
        case eAmount: std::snprintf(value, 32, "%d %%", c.amount); break;
        case eLen: std::snprintf(value, 32, "%d steps", c.len); break;
        case eGroups:
            std::snprintf(value, 32, "%s%s%s", c.groups & 1 ? "Drm " : "", c.groups & 2 ? "Bas " : "",
                          c.groups & 4 ? "Key" : "");
            if (!c.groups) lab::copyStr(value, 32, "-");
            break;
        case eEvent: lab::copyStr(value, 32, kEventNames[c.event]); break;
        case eArg:
            if (c.event == kEvGoto) std::snprintf(value, 32, "%d %s", c.arg + 1, m_->tab[c.arg].name);
            else lab::copyStr(value, 32, "-");
            break;
        case eSide: lab::copyStr(value, 32, c.dir ? "Right" : "Left"); break;
        case ePower: std::snprintf(value, 32, "%d", c.amount); break;
        case eWall: lab::copyStr(value, 32, kWallNames[c.dir < kWallCount ? c.dir : 0]); break;
        case eHold: std::snprintf(value, 32, "%d steps", c.len); break;
        default: value[0] = 0; break;
    }
}

void Flipper::soundRowText(int i, char* label, char* value) const {
    const Instrument& in = m_->inst[sndInst_];
    if (i == 5) lab::copyStr(label, 32, kCharA[in.engine]);
    else if (i == 6) lab::copyStr(label, 32, kCharB[in.engine]);
    else lab::copyStr(label, 32, kSndLabels[i]);
    char nn[8];
    switch (i) {
        case 0: lab::copyStr(value, 32, kEngineNames[in.engine]); break;
        case 1: std::snprintf(value, 32, "%d", in.level); break;
        case 2: std::snprintf(value, 32, "%d", int(in.pan) - 50); break;
        case 3: std::snprintf(value, 32, in.engine == kEnSample && in.decay >= 99 ? "Hold" : "%d", in.decay); break;
        case 4: std::snprintf(value, 32, "%d", in.tone); break;
        case 5: std::snprintf(value, 32, "%d", in.charA); break;
        case 6: std::snprintf(value, 32, "%d", in.charB); break;
        case 7: std::snprintf(value, 32, "%d", in.sendDelay); break;
        case 8: std::snprintf(value, 32, "%d", in.sendReverb); break;
        case 9: lab::copyStr(value, 32, kGroupNames[in.group]); break;
        case 10:
            if (in.midi == 0) lab::copyStr(value, 32, "Off");
            else if (in.midi == 1) lab::copyStr(value, 32, "Base channel");
            else std::snprintf(value, 32, "Channel %d", in.midi - 1);
            break;
        case 11: noteName(in.root, nn); lab::copyStr(value, 32, nn); break;
        case 12: std::snprintf(value, 32, "%d", in.color + 1); break;
        case 13: {
            const char* p = m_->sample[sndInst_];
            const char* slash = p;
            for (const char* q = p; *q; ++q)
                if (*q == '/') slash = q + 1;
            if (!p[0]) lab::copyStr(value, 32, wavCount_ ? "None" : "No WAVs on the card");
            else lab::copyStr(value, 32, slash);
            break;
        }
        default: value[0] = 0; break;
    }
}

void Flipper::tableRowText(int i, char* label, char* value) const {
    const Table& t = m_->tab[tabSel_];
    char nn[8];
    if (i < kTabGraph) {
        lab::copyStr(label, 32, kTabLabels[i]);
        switch (i) {
            case 0: lab::copyStr(value, 32, t.enabled ? "On" : "Off"); break;
            case 1: lab::copyStr(value, 32, kColourNames[t.color % 6]); break;
            case 2: lab::copyStr(value, 32, kRateNames[t.stepRate]); break;
            case 3: std::snprintf(value, 32, "%d %%", t.swing); break;
            case 4: lab::copyStr(value, 32, kScaleNames[t.scale]); break;
            case 5: lab::copyStr(value, 32, kNoteNames[t.root % 12]); break;
            case 6: std::snprintf(value, 32, "%d", t.gravity); break;
            case 7: if (t.balls >= 10) lab::copyStr(value, 32, "Endless"); else std::snprintf(value, 32, "%d", t.balls); break;
            case 8:
                if (!kLengthBars[t.lengthBars]) lab::copyStr(value, 32, "Until game over");
                else std::snprintf(value, 32, "%d bars", kLengthBars[t.lengthBars]);
                break;
            case 9: lab::copyStr(value, 32, kSidesNames[t.sides]); break;
            case 10: std::snprintf(value, 32, "%d steps", t.ballSave); break;
            case 11: if (!t.jackpot) lab::copyStr(value, 32, "Off"); else std::snprintf(value, 32, "%d000", t.jackpot); break;
            case 12: lab::copyStr(value, 32, kActionNames[t.onJackpot]); break;
            case 13: std::snprintf(value, 32, "%d %s", t.jackpotArg + 1, m_->tab[t.jackpotArg].name); break;
            case 14: lab::copyStr(value, 32, kActionNames[t.onMultiball]); break;
            case 15: std::snprintf(value, 32, "%d %s", t.multiballArg + 1, m_->tab[t.multiballArg].name); break;
            case 16: lab::copyStr(value, 32, kActionNames[t.onGameOver]); break;
            case 17: std::snprintf(value, 32, "%d %s", t.gameOverArg + 1, m_->tab[t.gameOverArg].name); break;
            default: value[0] = 0; break;
        }
        return;
    }
    if (i < kTabRails) {
        const int k = i - kTabGraph;
        std::snprintf(label, 32, "Graph -> %d %s", k + 1, m_->tab[k].name);
        std::snprintf(value, 32, "%d", t.graph[k]);
        return;
    }
    if (i < kTabPreset) {
        const int r = (i - kTabRails) / 6, f = (i - kTabRails) % 6;
        const Rail& rl = t.rails[r];
        std::snprintf(label, 32, "Rail %d %s", r + 1, kRailLabels[f]);
        switch (f) {
            case 0: lab::copyStr(value, 32, rl.inst == kNone ? "None" : m_->inst[rl.inst].name); break;
            case 1: noteName(rl.note, nn); lab::copyStr(value, 32, nn); break;
            case 2: std::snprintf(value, 32, "%d", rl.hits); break;
            case 3: std::snprintf(value, 32, "%d", rl.steps); break;
            case 4: std::snprintf(value, 32, "%d", rl.rotate); break;
            default:
                if (rl.heat >= 5) lab::copyStr(value, 32, "Off");
                else std::snprintf(value, 32, "%d", rl.heat);
                break;
        }
        return;
    }
    if (i == kTabPreset) {
        lab::copyStr(label, 32, confirm_ == 3 ? "Push again: load preset" : "Factory preset (push)");
        static const char* const kP[8] = {"INTRO", "GROOVE", "BUILD", "DROP", "BREAK", "ACID", "SPACE", "OUTRO"};
        lab::copyStr(value, 32, kP[preset_ & 7]);
        return;
    }
    lab::copyStr(label, 32, "Table file (push loads)");
    if (!tabFileCount_) lab::copyStr(value, 32, "none in Tables/");
    else {
        const char* p = tabFiles_[tabFile_];
        const char* s = p;
        for (const char* q = p; *q; ++q)
            if (*q == '/') s = q + 1;
        lab::copyStr(value, 32, s);
    }
}

void Flipper::runRowText(int i, char* label, char* value) const {
    lab::copyStr(label, 32, kRunLabels[i]);
    switch (i) {
        case 0:
            if (confirm_ == 1) lab::copyStr(label, 32, "Push again to load");
            std::snprintf(value, 32, "%d%s", slot_ + 1, slotUsed_[slot_] ? " (saved)" : " (empty)");
            break;
        case 1: lab::copyStr(value, 32, kCaptureNames[captureWhat_]); break;
        case 2: std::snprintf(value, 32, "step %d", clipA_ + 1); break;
        case 3: std::snprintf(value, 32, "step %d of %lu", clipB_, static_cast<unsigned long>(m_->clipLen)); break;
        case 4: lab::copyStr(value, 32, replaySrc_ ? "Live table sounds" : "As recorded"); break;
        case 5:
            lab::copyStr(value, 32, confirm_ == 2 ? "Push again" : "Push: factory");
            break;
        default: value[0] = 0; break;
    }
}

// ---- rows: values ---------------------------------------------------------------------------
void Flipper::editValue(int row, int d) {
    Table& t = m_->tab[game_.tableIndex()];
    Cell& c = t.cells[editCell_];
    const int id = editRowId(row);
    switch (id) {
        case eType: {
            int ty = int(c.type) + d;
            ty = (ty % kElemCount + kElemCount) % kElemCount;
            c.type = uint8_t(ty);
            if (c.type == kFlipper) {
                c.dir = cellCol(editCell_) < 6 ? 0 : 1;
                if (c.amount < 1 || c.amount > 9) c.amount = 6;
            } else if (c.type == kWall) {
                c.dir = kWallBlock;
            } else if (c.type == kRamp) {
                c.dir = cellCol(editCell_) < 6 ? kNE : kNW;
                c.amount = 70;
                c.len = 8;
            } else if (c.type == kHole) {
                c.len = 4;
            }
            if ((c.type == kBumper || c.type == kTarget) && c.inst == kNone) c.inst = 5;
            editRow_ = 0;
            break;
        }
        case eInst: bumpInst(c.inst, d); break;
        case eNote: bump(c.note, d * (shiftHeld_ ? 12 : 1), 0, 127); break;
        case eVel: bump(c.vel, d * (shiftHeld_ ? 10 : 1), 1, 127); break;
        case ePitch: bump(c.pitchMode, d, 0, kPmCount - 1); break;
        case eVelMode: bump(c.velMode, d, 0, kVmCount - 1); break;
        case eChance: bump(c.chance, d * 5, 0, 100); break;
        case ePoints: bump(c.points, d, 0, 255); break;
        case eWN: case eWE: case eWS: case eWW: bump(c.w[id - eWN], d, 0, 9); break;
        case eDir: bump(c.dir, d, 0, 8); break;
        case eFx: bump(c.fx, d, 0, kFxCount - 1); break;
        case eAmount: bump(c.amount, d * 5, 0, 100); break;
        case eLen: case eHold: bump(c.len, d, 1, 64); break;
        case eGroups: bump(c.groups, d, 0, 7); break;
        case eEvent: bump(c.event, d, 0, kEvCount - 1); break;
        case eArg: bump(c.arg, d, 0, kTables - 1); break;
        case eSide: bump(c.dir, d, 0, 1); break;
        case ePower: bump(c.amount, d, 1, 9); break;
        case eWall: bump(c.dir, d, 0, kWallCount - 1); break;
        default: break;
    }
    cellChanged();
}

void Flipper::soundValue(int row, int d) {
    Instrument& in = m_->inst[sndInst_];
    const int k = shiftHeld_ ? 10 : 1;
    switch (row) {
        case 0: bump(in.engine, d, 0, kEnCount - 1); break;
        case 1: bump(in.level, d * k, 0, 100); break;
        case 2: bump(in.pan, d * k, 0, 100); break;
        case 3: bump(in.decay, d * k, 0, 100); break;
        case 4: bump(in.tone, d * k, 0, 100); break;
        case 5: bump(in.charA, d * k, 0, 100); break;
        case 6: bump(in.charB, d * k, 0, 100); break;
        case 7: bump(in.sendDelay, d * k, 0, 100); break;
        case 8: bump(in.sendReverb, d * k, 0, 100); break;
        case 9: bump(in.group, d, 0, kGroups - 1); break;
        case 10: bump(in.midi, d, 0, 17); break;
        case 11: bump(in.root, d * (shiftHeld_ ? 12 : 1), 0, 127); break;
        case 12: bump(in.color, d, 0, 7); break;
        case 13: {
            int s = sampleSel_[sndInst_] + d;
            s = s < 0 ? 0 : (s > wavCount_ ? wavCount_ : s);
            sampleSel_[sndInst_] = s;
            if (s == 0) m_->sample[sndInst_][0] = 0;
            else lab::copyStr(m_->sample[sndInst_], kPathLen, wavs_[s - 1]);
            sampleDirty_[sndInst_] = true;
            sampleTouchMs_ = now_;
            if (s > 0) in.engine = kEnSample;
            break;
        }
        default: break;
    }
}

void Flipper::tableValue(int row, int d) {
    Table& t = m_->tab[tabSel_];
    if (row < kTabGraph) {
        switch (row) {
            case 0: bump(t.enabled, d, 0, 1); break;
            case 1: bump(t.color, d, 0, 5); break;
            case 2: bump(t.stepRate, d, 0, kRateCount - 1); break;
            case 3: bump(t.swing, d * 5, 0, 75); break;
            case 4: bump(t.scale, d, 0, kScaleCount - 1); break;
            case 5: bump(t.root, d, 0, 11); break;
            case 6: bump(t.gravity, d, 0, 9); break;
            case 7: bump(t.balls, d, 1, 10); break;
            case 8: bump(t.lengthBars, d, 0, kLengthCount - 1); break;
            case 9: bump(t.sides, d, 0, 1); break;
            case 10: bump(t.ballSave, d * 4, 0, 64); break;
            case 11: bump(t.jackpot, d, 0, 99); break;
            case 12: bump(t.onJackpot, d, 0, kActCount - 1); break;
            case 13: bump(t.jackpotArg, d, 0, kTables - 1); break;
            case 14: bump(t.onMultiball, d, 0, kActCount - 1); break;
            case 15: bump(t.multiballArg, d, 0, kTables - 1); break;
            case 16: bump(t.onGameOver, d, 0, kActCount - 1); break;
            case 17: bump(t.gameOverArg, d, 0, kTables - 1); break;
            default: break;
        }
        bool any = false;
        for (const auto& tb : m_->tab) any = any || tb.enabled;
        if (!any) t.enabled = 1;
        if (row == 9 || row == 0) post(Cmd::kRebuild);
        return;
    }
    if (row < kTabRails) {
        bump(t.graph[row - kTabGraph], d, 0, 9);
        return;
    }
    if (row < kTabPreset) {
        const int r = (row - kTabRails) / 6, f = (row - kTabRails) % 6;
        Rail& rl = t.rails[r];
        switch (f) {
            case 0: bumpInst(rl.inst, d); break;
            case 1: bump(rl.note, d * (shiftHeld_ ? 12 : 1), 0, 127); break;
            case 2: bump(rl.hits, d, 0, 16); break;
            case 3: bump(rl.steps, d, 1, 16); break;
            case 4: bump(rl.rotate, d, 0, 15); break;
            default: bump(rl.heat, d, 0, 5); break;
        }
        return;
    }
    if (row == kTabPreset) {
        preset_ = (preset_ + d + 8) & 7;
        confirm_ = 0;
        return;
    }
    if (tabFileCount_) tabFile_ = (tabFile_ + d + tabFileCount_) % tabFileCount_;
}

void Flipper::runValue(int row, int d) {
    confirm_ = 0;
    switch (row) {
        case 0: slot_ = (slot_ + d + kSlots) % kSlots; break;
        case 1: captureWhat_ = (captureWhat_ + d + kCaptureCount) % kCaptureCount; break;
        case 2: {
            const int k = shiftHeld_ ? 16 : 1;
            clipA_ += d * k;
            if (clipA_ < 0) clipA_ = 0;
            if (clipA_ > clipB_ - 1) clipA_ = clipB_ > 0 ? clipB_ - 1 : 0;
            break;
        }
        case 3: {
            const int k = shiftHeld_ ? 16 : 1;
            clipB_ += d * k;
            if (clipB_ > int(m_->clipLen)) clipB_ = int(m_->clipLen);
            if (clipB_ < clipA_ + 1) clipB_ = clipA_ + 1;
            break;
        }
        case 4: replaySrc_ = replaySrc_ ? 0 : 1; game_.setReplayLive(replaySrc_ != 0); break;
        default: break;
    }
}

void Flipper::cellChanged() { post(Cmd::kRebuild); }

// ---- events ----------------------------------------------------------------------------------
void Flipper::onEvent(const Event& e) {
    if (e.type == Event::kPress || e.type == Event::kRelease) {
        if (e.id == btn::Shift) shiftHeld_ = e.type == Event::kPress;
    }
    if (e.type == Event::kPadDown) padDown_[e.id] = true;
    if (e.type == Event::kPadUp) padDown_[e.id] = false;
    if (e.type == Event::kPress && e.id == btn::Help) {
        help_ = !help_;
        return;
    }
    // page keys (F1..F5)
    if (e.type == Event::kPress && e.id <= btn::F5) {
        const int p = e.id - btn::F1;
        if (page_ != p) {
            page_ = p;
            confirm_ = 0;
            help_ = false;
            if (page_ == kPgTables) {
                tabSel_ = game_.tableIndex();
                scanTableFiles();
            }
            if (page_ == kPgSounds && e.shift) scanWavs();
            if (page_ == kPgRun) rescanSlots();
        }
        return;
    }
    // performance effects on every page: Note / Instr / FX1 / FX2 / Pattern held
    if ((e.type == Event::kPress || e.type == Event::kRelease) &&
        (e.id == btn::Note || e.id == btn::Instr || e.id == btn::Fx1 || e.id == btn::Fx2 || e.id == btn::Pattern)) {
        const int fx = e.id == btn::Note ? kFxFilter
                                         : (e.id == btn::Instr ? kFxCrush
                                                               : (e.id == btn::Fx1 ? kFxStutter
                                                                                   : (e.id == btn::Fx2 ? kFxDelay : kFxReverb)));
        post(Cmd::kPerf, fx, e.type == Event::kPress ? 85 : 0);
        return;
    }
    switch (page_) {
        case kPgPlay: eventPlay(e); break;
        case kPgEdit: eventEdit(e); break;
        case kPgSounds: eventSounds(e); break;
        case kPgTables: eventTables(e); break;
        default: eventRun(e); break;
    }
}

void Flipper::eventPlay(const Event& e) {
    const Table& t = game_.table();
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        const bool down = e.type == Event::kPadDown;
        const int col = lab::padCol(e.id), row = lab::padRow(e.id);
        const Cell& c = t.cells[e.id];
        if (row == kRows - 1 || c.type == kFlipper) {
            const int side = c.type == kFlipper ? (c.dir ? 1 : 0) : (col < 6 ? 0 : 1);
            if (col == t.launchCol && c.type != kFlipper && row == kRows - 1) {
                if (down) post(Cmd::kLaunch);
                return;
            }
            post(Cmd::kFlip, side, down ? 1 : 0, e.velocity);
            return;
        }
        if (down && e.shift) {
            post(Cmd::kDrop, e.id);
            return;
        }
        // magnet while held (the newest held pad wins)
        if (down) post(Cmd::kMagnet, e.id);
        else {
            int other = -1;
            for (int i = 0; i < lab::kPads; ++i)
                if (padDown_[i] && lab::padRow(i) < kRows - 1) other = i;
            post(Cmd::kMagnet, other);
        }
        return;
    }
    if (e.type == Event::kPress || e.type == Event::kRelease) {
        const bool down = e.type == Event::kPress;
        if (e.id == btn::Left) { post(Cmd::kFlip, 0, down, 110); return; }
        if (e.id == btn::Right) { post(Cmd::kFlip, 1, down, 110); return; }
        if (!down) return;
        switch (e.id) {
            case btn::Down: case btn::EncPush: post(Cmd::kLaunch); break;
            case btn::Up: post(Cmd::kTilt); break;
            case btn::F6:
                post(Cmd::kNewGame, game_.tableIndex());
                if (!ctx_->playing()) ctx_->setPlaying(true);
                ctx_->toast("New game");
                break;
            case btn::F7:
                m_->g.mode = m_->g.mode ? 0 : 1;
                ctx_->toast(m_->g.mode ? "Markov only: the ball steps on the beat" : "Hybrid: physics + Markov grid");
                break;
            case btn::F8:
                if (capture(captureWhat_)) {
                    char msg[48];
                    std::snprintf(msg, sizeof msg, "Captured %lu steps", static_cast<unsigned long>(m_->clipLen));
                    ctx_->toast(msg);
                } else {
                    ctx_->toast("Nothing to capture yet");
                }
                break;
            case btn::Delete: post(Cmd::kStop); break;
            default: break;
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        // cue the next table along the graph order
        int n = game_.pendingTable() >= 0 ? game_.pendingTable() : game_.tableIndex();
        for (int k = 0; k < kTables; ++k) {
            n = (n + (e.delta > 0 ? 1 : -1) + kTables) % kTables;
            if (m_->tab[n].enabled) break;
        }
        post(Cmd::kGoto, n);
    }
}

void Flipper::eventEdit(const Event& e) {
    Table& t = m_->tab[game_.tableIndex()];
    if (e.type == Event::kPadDown) {
        if (e.shift) {   // stamp the selected cell here
            t.cells[e.id] = t.cells[editCell_];
            if (t.cells[e.id].type == kFlipper) t.cells[e.id].dir = lab::padCol(e.id) < 6 ? 0 : 1;
            cellChanged();
        }
        editCell_ = e.id;
        if (editRow_ >= editRows()) editRow_ = 0;
        const Cell& c = t.cells[e.id];
        if (c.inst != kNone && (c.type == kBumper || c.type == kTarget || c.type == kRamp || c.type == kHole))
            audition(c.inst, c.note, c.vel);
        return;
    }
    if (e.type == Event::kEncoder) {
        editValue(editRow_, e.delta);
        return;
    }
    if (e.type != Event::kPress) return;
    if (e.shift && (e.id == btn::Left || e.id == btn::Right || e.id == btn::Up || e.id == btn::Down)) {
        int c = cellCol(editCell_), r = cellRow(editCell_);
        if (e.id == btn::Left) c = (c + kCols - 1) % kCols;
        if (e.id == btn::Right) c = (c + 1) % kCols;
        if (e.id == btn::Up) r = (r + kRows - 1) % kRows;
        if (e.id == btn::Down) r = (r + 1) % kRows;
        editCell_ = cellAt(c, r);
        if (editRow_ >= editRows()) editRow_ = 0;
        return;
    }
    switch (e.id) {
        case btn::Up: if (editRow_ > 0) --editRow_; break;
        case btn::Down: if (editRow_ + 1 < editRows()) ++editRow_; break;
        case btn::Left: editValue(editRow_, -1); break;
        case btn::Right: editValue(editRow_, 1); break;
        case btn::F6: editValue(0, e.shift ? -1 : 1); editRow_ = 0; break;
        case btn::F7: post(Cmd::kDrop, editCell_); if (!ctx_->playing()) ctx_->setPlaying(true); break;
        case btn::F8:
            if (confirm_ != 4) {
                confirm_ = 4;
                ctx_->toast("F8 again clears this table");
            } else {
                confirm_ = 0;
                clearTable(t);
                cellChanged();
                ctx_->toast("Table cleared");
            }
            break;
        case btn::Copy:
            if (e.shift) {
                *clipTable_ = t;
                haveClipTable_ = true;
                ctx_->toast("Table copied");
            } else {
                clipCell_ = t.cells[editCell_];
                haveClipCell_ = true;
                ctx_->toast("Cell copied");
            }
            break;
        case btn::Insert:
            if (e.shift && haveClipTable_) {
                char name[kNameLen];
                std::memcpy(name, t.name, kNameLen);
                t = *clipTable_;
                std::memcpy(t.name, name, kNameLen);
                cellChanged();
                ctx_->toast("Table pasted");
            } else if (haveClipCell_) {
                t.cells[editCell_] = clipCell_;
                cellChanged();
            }
            break;
        case btn::Delete: {
            Cell& c = t.cells[editCell_];
            const Cell keep = c;
            c = Cell();
            for (int i = 0; i < 4; ++i) c.w[i] = keep.w[i];
            editRow_ = 0;
            cellChanged();
            break;
        }
        case btn::EncPush: {
            const Cell& c = t.cells[editCell_];
            if (c.inst != kNone) audition(c.inst, c.note, c.vel);
            break;
        }
        default: break;
    }
    editTop_ = editRow_ < editTop_ ? editRow_ : (editRow_ >= editTop_ + 11 ? editRow_ - 10 : editTop_);
}

void Flipper::eventSounds(const Event& e) {
    const Instrument& in = m_->inst[sndInst_];
    if (e.type == Event::kPadDown) {
        if (e.id < kInsts) {
            sndInst_ = e.id;
            audition(sndInst_, in.root, 110);
        } else if (e.id >= 12) {
            audition(sndInst_, clampNote(int(m_->inst[sndInst_].root) - 12 + (e.id - 12)), e.velocity);
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        soundValue(sndRow_, e.delta);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: if (sndRow_ > 0) --sndRow_; break;
        case btn::Down: if (sndRow_ + 1 < kSndRows) ++sndRow_; break;
        case btn::Left: soundValue(sndRow_, -1); break;
        case btn::Right: soundValue(sndRow_, 1); break;
        case btn::F6: case btn::EncPush: audition(sndInst_, in.root, 110); break;
        case btn::F7: sndInst_ = (sndInst_ + kInsts - 1) % kInsts; break;
        case btn::F8:
            if (e.shift) {
                scanWavs();
                for (int i = 0; i < kInsts; ++i) sampleDirty_[i] = true;
                ctx_->toast("Samples reloaded");
            } else {
                sndInst_ = (sndInst_ + 1) % kInsts;
            }
            break;
        default: break;
    }
    sndTop_ = sndRow_ < sndTop_ ? sndRow_ : (sndRow_ >= sndTop_ + 11 ? sndRow_ - 10 : sndTop_);
}

void Flipper::eventTables(const Event& e) {
    if (e.type == Event::kPadDown) {
        const int col = lab::padCol(e.id), row = lab::padRow(e.id);
        if (col >= kTables) return;
        if (row == 0) {
            tabSel_ = col;
            if (e.shift) post(Cmd::kGotoNow, col);
        } else if (row == 1) {
            uint8_t& w = m_->tab[tabSel_].graph[col];
            w = uint8_t(w >= 9 ? 0 : (w < 3 ? 3 : (w < 6 ? 6 : 9)));
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        tableValue(tabRow_, e.delta);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: if (tabRow_ > 0) --tabRow_; confirm_ = 0; break;
        case btn::Down: if (tabRow_ + 1 < kTabRows) ++tabRow_; confirm_ = 0; break;
        case btn::Left: tableValue(tabRow_, -1); break;
        case btn::Right: tableValue(tabRow_, 1); break;
        case btn::F6:
            post(e.shift ? Cmd::kGotoNow : Cmd::kGoto, tabSel_);
            ctx_->toast(e.shift ? "Table now" : "Table cued");
            break;
        case btn::F7:
            ctx_->toast(saveTableFile(tabSel_) ? "Table saved to Tables/" : "Cannot save (card?)");
            break;
        case btn::F8: tabSel_ = (tabSel_ + 1) % kTables; break;
        case btn::EncPush:
            if (tabRow_ == kTabPreset) {
                if (confirm_ != 3) confirm_ = 3;
                else {
                    confirm_ = 0;
                    factoryTable(m_->tab[tabSel_], preset_);
                    post(Cmd::kRebuild);
                    ctx_->toast("Preset loaded");
                }
            } else if (tabRow_ == kTabFile && tabFileCount_) {
                ctx_->toast(loadTableFile(tabSel_, tabFiles_[tabFile_]) ? "Table loaded" : "Not a table file");
            }
            break;
        default: break;
    }
    tabTop_ = tabRow_ < tabTop_ ? tabRow_ : (tabRow_ >= tabTop_ + 11 ? tabRow_ - 10 : tabTop_);
}

void Flipper::eventRun(const Event& e) {
    if (e.type == Event::kEncoder) {
        runValue(runRow_, e.delta);
        return;
    }
    if (e.type == Event::kPadDown && m_->clipLen) {   // jump inside the clip: 48 pads = 48 slices
        const uint32_t at = uint32_t(e.id) * m_->clipLen / lab::kPads;
        clipA_ = int(at);
        if (clipB_ <= clipA_) clipB_ = int(m_->clipLen);
        if (game_.replaying()) post(Cmd::kReplay, 1, clipA_, clipB_);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: if (runRow_ > 0) --runRow_; confirm_ = 0; break;
        case btn::Down: if (runRow_ + 1 < kRunRows) ++runRow_; confirm_ = 0; break;
        case btn::Left: runValue(runRow_, -1); break;
        case btn::Right: runValue(runRow_, 1); break;
        case btn::F6:
            if (capture(captureWhat_)) {
                char msg[48];
                std::snprintf(msg, sizeof msg, "Captured %lu steps", static_cast<unsigned long>(m_->clipLen));
                ctx_->toast(msg);
            } else {
                ctx_->toast("Nothing to capture yet");
            }
            break;
        case btn::F7:
            if (!m_->clipLen) {
                ctx_->toast("Capture a run first (F6)");
                break;
            }
            post(Cmd::kReplay, game_.replaying() ? 0 : 1, clipA_, clipB_);
            if (!game_.replaying() && !ctx_->playing()) ctx_->setPlaying(true);
            break;
        case btn::F8: {
            char msg[48];
            std::snprintf(msg, sizeof msg, saveSlot(slot_) ? "Saved machine %d" : "Cannot save machine %d", slot_ + 1);
            ctx_->toast(msg);
            break;
        }
        case btn::EncPush:
            if (runRow_ == 0) {
                if (confirm_ != 1) confirm_ = 1;
                else {
                    confirm_ = 0;
                    char msg[48];
                    std::snprintf(msg, sizeof msg, loadSlot(slot_) ? "Loaded machine %d" : "Machine %d: nothing to load",
                                  slot_ + 1);
                    ctx_->toast(msg);
                    needSamples_ = true;
                }
            } else if (runRow_ == 5) {
                if (confirm_ != 2) confirm_ = 2;
                else {
                    confirm_ = 0;
                    if (!swapPending_) {
                        factoryMachine(*spare_);
                        swapPending_ = true;
                        post(Cmd::kSwap);
                        needSamples_ = true;
                        activeSlot_ = -1;   // no autosave over a slot until F8 saves it
                        ctx_->toast("Factory machine (F8 saves it)");
                    }
                }
            } else if (runRow_ == 4) {
                runValue(4, 1);
            }
            break;
        default: break;
    }
}

}  // namespace flip
