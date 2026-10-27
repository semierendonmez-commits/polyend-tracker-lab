// KONSOL input: pads, keys, encoder, per page (MANUAL.md has the full map).
// Every function sits on a key the original Tracker has: no Aux, Help or
// encoder push (LABKIT NAV.md). Insert = the page's "do it" (launch, toggle,
// add); Delete = remove; Copy = duplicate; Shift + Copy = undo, Shift +
// Delete = redo. I.Param / Master / Shift + Config belong to LABKIT.
// Main loop only (no audio-interrupt code here): built for size.
#pragma GCC optimize("Os")
#include <cstdio>
#include <cstring>

#include "ws/gen_glue.h"
#include "ws/kit.h"
#include "ws/konsol.h"
#include "ws/stretch.h"

namespace lab {
namespace konsol {

void makamSetKey(Engine* e, int key);

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr int kGrids[5] = {kTps / 2, kTps, kTps * 2, kTps * 4, kTps * 16};   // 1/32 .. 1 bar
enum Pick : int { kPickSample = 0, kPickKitFolder, kPickPadSample, kPickKit, kPickProject };
enum Name : int { kNameSaveAs = 10, kNameRename, kNameKit };
}  // namespace

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
void Konsol::ensureClip(int t, int s, int steps) {
    Clip& c = P_->clip[t][s];
    if (c.exists()) return;
    c.cur = 0;
    c.pending = 0;
    c.buf[0].clear();
    c.buf[1].clear();
    c.gen = kGenNotes;
    c.live = 0;
    c.every = 1;
    c.launchQ = 0;
    c.lenSteps = uint8_t(steps);
    dirty_ = true;
}

void Konsol::undoSave(int t, int s) {
    if (!undo_) return;
    if (undoN_ == kUndo) {   // full: forget the oldest
        std::memmove(&undo_[0], &undo_[1], sizeof(UndoRec) * (kUndo - 1));
        --undoN_;
    }
    UndoRec& u = undo_[undoN_++];
    u.t = int8_t(t);
    u.s = int8_t(s);
    u.len = P_->clip[t][s].lenSteps;
    std::memcpy(&u.nb, &P_->clip[t][s].notes(), sizeof(NoteBuf));
    redoN_ = 0;   // a new edit drops the redo history
}

namespace {
// swap the clip with the record (undo <-> redo)
void swapIn(Project& p, int t, int s, uint8_t& len, NoteBuf& nb) {
    Clip& c = p.clip[t][s];
    static NoteBuf cur;
    std::memcpy(&cur, &c.notes(), sizeof(NoteBuf));
    const uint8_t curLen = c.lenSteps;
    NoteBuf& b = c.beginEdit();
    std::memcpy(&b, &nb, sizeof(NoteBuf));
    c.lenSteps = len;
    c.commit();
    std::memcpy(&nb, &cur, sizeof(NoteBuf));
    len = curLen;
}
}  // namespace

void Konsol::undoRestore() {
    if (!undoN_) {
        ctx_->toast("Nothing to undo");
        return;
    }
    UndoRec& u = undo_[--undoN_];
    swapIn(*P_, u.t, u.s, u.len, u.nb);
    if (redoN_ == kUndo) {
        std::memmove(&redo_[0], &redo_[1], sizeof(UndoRec) * (kUndo - 1));
        --redoN_;
    }
    std::memcpy(&redo_[redoN_++], &u, sizeof(UndoRec));
    char m[32];
    std::snprintf(m, sizeof m, "Undo (%d more)", undoN_);
    ctx_->toast(m);
    dirty_ = true;
}

void Konsol::redoRestore() {
    if (!redoN_) {
        ctx_->toast("Nothing to redo");
        return;
    }
    UndoRec& u = redo_[--redoN_];
    swapIn(*P_, u.t, u.s, u.len, u.nb);
    if (undoN_ < kUndo) std::memcpy(&undo_[undoN_++], &u, sizeof(UndoRec));
    ctx_->toast("Redo");
    dirty_ = true;
}

bool Konsol::drumLike(int t) const {
    const int e = P_->track[t].engine;
    if (e == kEngDrums || e == kEngKit || e == kEngFloater || e == kEngPerc) return true;
    return e == kEngSampler && eng_[t].e[kEngSampler]->get(0) > 1.5f;   // slices
}

// Pads as a keyboard of the selected track: 4 rows of 12 from the bottom.
// Drum-like engines: 48 consecutive notes from C2 (kits, slices). Melodic:
// a row per octave, chromatic or "in key" (the scale's notes only).
int Konsol::keyPitch(int pad) const {
    const int row = padRow(pad), col = padCol(pad);
    const int fromBottom = (kPadRows - 1) - row;
    const int eng = P_->track[selT_].engine;
    int p;
    if (drumLike(selT_)) {
        const int base = eng == kEngPerc ? 60 : 36;
        p = base + keyOct_ * 12 + fromBottom * 12 + col;
    } else if (P_->keyLayout == 1) {
        const uint16_t mask = scaleMask(selT_);
        int deg[12], n = 0;
        for (int k = 0; k < 12; ++k)
            if ((mask >> k) & 1) deg[n++] = k;
        if (!n) {
            deg[0] = 0;
            n = 1;
        }
        const int root = 12 * (P_->track[selT_].octave + 1) + P_->key;
        p = root + keyOct_ * 12 + fromBottom * 12 + (col / n) * 12 + deg[col % n];
    } else {
        p = 12 * (P_->track[selT_].octave + 1) + keyOct_ * 12 + fromBottom * 12 + col;
    }
    return p < 0 ? 0 : (p > 127 ? 127 : p);
}

void Konsol::scrollScenes(int d) {
    sceneTop_ = clampi(sceneTop_ + d, 0, kScenes - kPadRows);
    if (selS_ < sceneTop_) selS_ = sceneTop_;
    if (selS_ > sceneTop_ + kPadRows - 1) selS_ = sceneTop_ + kPadRows - 1;
}

void Konsol::focusRoll() {
    const NoteBuf& nb = P_->clip[selT_][selS_].notes();
    int lo = 127, hi = 0;
    for (int i = 0; i < nb.count; ++i) {
        lo = nb.n[i].pitch < lo ? nb.n[i].pitch : lo;
        hi = nb.n[i].pitch > hi ? nb.n[i].pitch : hi;
    }
    if (hi < lo) {
        lo = keyPitch(padAt(0, 3));
        hi = lo + 12;
    }
    rollTop_ = clampi((lo + hi) / 2 + 8, 15, 127);
    if (hi > rollTop_) rollTop_ = hi;
    if (rollPitch_ > rollTop_ || rollPitch_ < rollTop_ - 15) rollPitch_ = nb.count ? nb.n[0].pitch : lo;
    if (nb.count) {
        rollTick_ = nb.n[0].start;
        rollPitch_ = nb.n[0].pitch;
    }
}

void Konsol::gotoPage(int p) {
    if (p < 0 || p >= kPgCount) return;
    if (p != page_) prevPage_ = page_;
    page_ = uint8_t(p);
    if (page_ == kPgRoll) focusRoll();
    if (page_ == kPgKit && kitPads(eng_[selT_].e[kEngKit])) kitSelect(eng_[selT_].e[kEngKit], kitSel_);
    confirmKey_ = -1;
    sndEngine_ = false;
}

// ---------------------------------------------------------------------------
// browser / keyboard (LABKIT modals; the result is applied in update())
// ---------------------------------------------------------------------------
void Konsol::openBrowser(int what) {
    if (!ctx_->fs) {
        ctx_->toast("No SD card");
        return;
    }
    kit::BrowserRequest q;
    static char folder[64];
    switch (what) {
        case kPickKitFolder:
            q.title = "Kit from a folder: pick any WAV in it";
            std::snprintf(folder, sizeof folder, "%s/Samples", ctx_->home);
            break;
        case kPickPadSample:
            q.title = "Sample for the selected pad";
            std::snprintf(folder, sizeof folder, "%s/Samples", ctx_->home);
            break;
        case kPickKit:
            q.title = "Load a kit";
            q.exts = ".kit";
            q.preview = false;
            std::snprintf(folder, sizeof folder, "%s/Kits", ctx_->home);
            break;
        case kPickProject:
            q.title = "Open a project";
            q.exts = ".kon";
            q.preview = false;
            std::snprintf(folder, sizeof folder, "%s/Projects", ctx_->home);
            break;
        default:
            q.title = "Load a sample into the track";
            std::snprintf(folder, sizeof folder, "%s/Samples", ctx_->home);
            break;
    }
    q.folder = folder;
    q.ctx = this;
    pickWhat_ = -1;
    pickPath_[0] = 0;
    static int sWhat;
    sWhat = what;
    q.onPick = [](void* c, const char* path) {
        Konsol* k = static_cast<Konsol*>(c);
        std::snprintf(k->pickPath_, sizeof k->pickPath_, "%s", path);
        k->pickWhat_ = sWhat;
    };
    ctx_->browser().open(q);
}

void Konsol::openKeyboard(int what) {
    kit::KeyboardRequest q;
    static int sWhat;
    sWhat = what;
    q.title = what == kNameKit ? "Kit name" : (what == kNameRename ? "Project name" : "Save the project as");
    q.initial = what == kNameKit ? "KIT01" : P_->name;
    q.maxLen = what == kNameKit ? 15 : 15;
    q.ctx = this;
    q.onDone = [](void* c, const char* text) {
        Konsol* k = static_cast<Konsol*>(c);
        std::snprintf(k->pickPath_, sizeof k->pickPath_, "%s", text);
        k->pickWhat_ = sWhat;
    };
    ctx_->keyboard().open(q);
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------
void Konsol::onEvent(const Event& e) {
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        padEvent(e);
        return;
    }
    if (e.type == Event::kEncoder) {
        encoder(e.delta, e.shift);
        return;
    }
    if (e.type == Event::kPress || e.type == Event::kRelease || e.type == Event::kHold) button(e);
}

void Konsol::padEvent(const Event& e) {
    const bool down = e.type == Event::kPadDown;
    const int pad = e.id, col = padCol(pad), row = padRow(pad);
    // a hand-played pad always ends on the pad (and track) it started
    if (!down && padPitch_[pad] >= 0) {
        playNoteOn(padTrack_[pad] >= 0 ? padTrack_[pad] : selT_, padPitch_[pad], 0, false);
        padPitch_[pad] = -1;
        padTrack_[pad] = -1;
        if (page_ == kPgSteps && heldStep_ < 0) return;
    }
    if (keysOverlay_ && page_ != kPgSteps) {
        if (down) keysPad(pad, true, e.velocity);
        return;
    }
    switch (page_) {
        case kPgSession:
        case kPgGen:
            if (col < 8) sessionPad(col, row, down, e.velocity);
            else controlPad(col, row, down, e.velocity);
            break;
        case kPgSteps: stepsPad(col, row, down, e.velocity); break;
        case kPgRoll: rollPad(pad, down, e.velocity); break;
        case kPgMixer:
            if (col < 8) mixerPad(col, row, down);
            else controlPad(col, row, down, e.velocity);
            break;
        case kPgSong: songPad(col, row, down, e.shift); break;
        case kPgSample: samplePad(pad, down, e.velocity); break;
        case kPgKit: kitPad(pad, down, e.velocity); break;
        default:
            if (down) keysPad(pad, true, e.velocity);
            break;
    }
}

void Konsol::keysPad(int pad, bool down, int vel) {
    if (!down) return;
    const int p = keyPitch(pad);
    padPitch_[pad] = int8_t(p);
    padTrack_[pad] = int8_t(selT_);
    playNote(p, vel, true);
    if (page_ == kPgRoll) rollPitch_ = p;
    if (page_ == kPgSteps) stepNote_ = p;
}

void Konsol::sessionPad(int col, int row, bool down, int vel) {
    (void)vel;
    if (!down) return;
    const int t = col, s = sceneTop_ + row;
    Clip& c = P_->clip[t][s];
    switch (gridMode_) {
        case kGmStop: stopTrack(t); break;
        case kGmMute: P_->track[t].mix.mute ^= 1; break;
        case kGmSolo: P_->track[t].mix.solo ^= 1; break;
        case kGmSelect:
            selT_ = t;
            selS_ = s;
            break;
        default:
            selT_ = t;
            selS_ = s;
            if (c.exists()) {
                launch(t, s);
                if (!ctx_->playing()) ctx_->setPlaying(true);
            } else if (rt_[t].play >= 0) {
                stopTrack(t);   // an empty slot stops the track (session convention)
            }
            break;
    }
}

void Konsol::controlPad(int col, int row, bool down, int vel) {
    (void)vel;
    if (col == 11) {   // performance: held effects
        switch (row) {
            case 0: stutHeld_ = down; break;
            case 1: hpHeld_ = down; break;
            case 2:   // one throw: delay and reverb together
                dlyThrow_ = down;
                revThrow_ = down;
                break;
            default: fillHeld_ = down; break;   // FILL: notes with a Fill condition play
        }
        return;
    }
    if (col == 9) {   // grid modes: hold = momentary, tap = latch
        static const uint8_t kM[4] = {kGmStop, kGmMute, kGmSolo, kGmSelect};
        const uint8_t m = kM[row];
        if (down) {
            if (gridMode_ == m && gridModeLatched_) {
                gridMode_ = kGmLaunch;
                gridModeLatched_ = false;
            } else {
                gridMode_ = m;
                gridModeLatched_ = false;
                gridModeDownMs_ = nowMs_;
            }
        } else if (gridMode_ == m) {
            if (nowMs_ - gridModeDownMs_ < 350) gridModeLatched_ = true;
            else gridMode_ = kGmLaunch;
        }
        return;
    }
    if (!down) return;
    if (col == 8) {   // scene launch
        const int s = sceneTop_ + row;
        selS_ = s;
        launchScene(s);
        if (!ctx_->playing()) ctx_->setPlaying(true);
        return;
    }
    switch (row) {   // col 10: stop all, scenes up, scenes down, record arm
        case 0: send(Cmd{Cmd::kStopAll, 0, 0, 0}); break;
        case 1: scrollScenes(-1); break;
        case 2: scrollScenes(1); break;
        default: {
            Event e;
            e.type = Event::kPress;
            e.id = btn::Rec;
            button(e);
            break;
        }
    }
}

void Konsol::mixerPad(int col, int row, bool down) {
    if (!down) return;
    Mix& m = P_->track[col].mix;
    switch (row) {
        case 0: m.mute ^= 1; break;
        case 1: m.solo ^= 1; break;
        case 2:
            selT_ = col;
            mixCol_ = col;
            mixFx_ = false;
            break;
        default: m.stem = uint8_t((m.stem + 1) % 5); break;
    }
    dirty_ = true;
}

void Konsol::songPad(int col, int row, bool down, bool shift) {
    if (!down) return;
    if (col < 4) {   // scenes 0..15
        const int sc = row * 4 + col;
        if (shift || P_->songLen == 0) {
            if (P_->songLen < kSongMax) {
                P_->song[P_->songLen].scene = uint8_t(sc);
                P_->song[P_->songLen].bars = 4;
                songCur_ = P_->songLen++;
            }
        } else if (songCur_ < P_->songLen) {
            P_->song[songCur_].scene = uint8_t(sc);
        }
        dirty_ = true;
    } else {   // entries 0..31
        const int i = row * 8 + (col - 4);
        if (i < P_->songLen) songCur_ = i;
    }
}

// Sample page pads (rows 1-3): the slices of the track's sample; tap =
// select + hear it (through the track's SAMPLER / KIT), row 4: keyboard.
void Konsol::samplePad(int pad, bool down, int vel) {
    const int row = padRow(pad);
    if (row == 3) {
        if (down) keysPad(pad, true, vel);
        return;
    }
    if (!down) return;
    const int k = pad;   // 0..35
    const TrackSample& ts = ts_[selT_];
    const int slot = smpTab_ == 1 ? ts.src : ts.slot;
    if (!pool_.valid(slot) || k >= pool_.slot(slot).nOn) return;
    sliceSel_ = k;
    const int eng = P_->track[selT_].engine;
    if (eng == kEngSampler && engine(selT_)->get(0) > 1.5f) {
        padPitch_[pad] = int8_t(36 + k);
        padTrack_[pad] = int8_t(selT_);
        playNote(36 + k, vel, true);
    }
}

// Kit page pads: a 4 x 4 grid (pad 1 bottom left, like a drum machine) on
// columns 1-4 plays and selects; columns 5-12 play the selected pad chromatically.
void Konsol::kitPad(int pad, bool down, int vel) {
    const int row = padRow(pad), col = padCol(pad);
    if (!down) return;
    if (col < 4) {
        const int k = (3 - row) * 4 + col;
        kitSel_ = k;
        kitSelect(eng_[selT_].e[kEngKit], k);
        padPitch_[pad] = int8_t(kKitBase + k);
        padTrack_[pad] = int8_t(selT_);
        playNote(kKitBase + k, vel, true);
        return;
    }
    // chromatic: the selected pad's tune follows the key (a quick "tune by ear")
    KitPad* pads = kitPads(eng_[selT_].e[kEngKit]);
    if (!pads) return;
    const int semi = (3 - row) * 8 + (col - 4) - 12;
    pads[kitSel_].tune = float(clampi(semi, -24, 24));
    kitRefresh(eng_[selT_].e[kEngKit]);
    padPitch_[pad] = int8_t(kKitBase + kitSel_);
    padTrack_[pad] = int8_t(selT_);
    playNote(kKitBase + kitSel_, vel, true);
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// steps
// ---------------------------------------------------------------------------
void Konsol::toggleStep(int step, int vel) {
    const int t = selT_, s = selS_;
    ensureClip(t, s, 16);
    Clip& c = P_->clip[t][s];
    if (step >= c.lenSteps) return;
    if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3));
    undoSave(t, s);
    NoteBuf& b = c.beginEdit();
    int found = -1;
    for (int i = 0; i < b.count; ++i)
        if (b.n[i].pitch == stepNote_ && b.n[i].start / kTps == step) found = i;
    if (found >= 0) {
        b.remove(found);
    } else {
        Note n;
        n.start = uint16_t(step * kTps);
        n.len = uint16_t(kTps - 1);
        n.pitch = uint8_t(stepNote_);
        n.vel = uint8_t(vel > 0 ? vel : 100);
        b.add(n);
    }
    c.commit();
    dirty_ = true;
}

void Konsol::stepsPad(int col, int row, bool down, int vel) {
    if (row >= 2) {   // keyboard: sets the lane, plays; with a held step: that note's pitch
        if (!down) return;
        const int pad = padAt(col, row);
        const int p = keyPitch(pad);
        if (heldStep_ >= 0) {
            Clip& c = P_->clip[selT_][selS_];
            NoteBuf& b = c.beginEdit();
            for (int i = 0; i < b.count; ++i)
                if (b.n[i].start / kTps == heldStep_ && b.n[i].pitch == stepNote_) b.n[i].pitch = uint8_t(p);
            b.sort();
            c.commit();
            stepEdited_ = true;
        }
        stepNote_ = p;
        padPitch_[pad] = int8_t(p);
        padTrack_[pad] = int8_t(selT_);
        playNote(p, vel, true);
        return;
    }
    if (col >= 8) {   // step pages
        if (!down) return;
        const int pg = row * 4 + (col - 8);
        Clip& c = P_->clip[selT_][selS_];
        if (c.exists() && pg * 16 < c.lenSteps) stepPage_ = pg;
        return;
    }
    const int step = stepPage_ * 16 + row * 8 + col;
    if (down) {
        heldStep_ = step;
        stepEdited_ = false;
        stepCursor_ = step;
    } else if (heldStep_ == step) {
        if (!stepEdited_) toggleStep(step, vel ? vel : 100);
        heldStep_ = -1;
    }
}

namespace {
int laneNote(const NoteBuf& b, int step, int pitch) {
    for (int i = 0; i < b.count; ++i)
        if (b.n[i].start / kTps == step && b.n[i].pitch == pitch) return i;
    return -1;
}
// step locks: 0 velocity, 1 length, 2 chance, 3 ratchet, 4 condition, 5 tone, 6 slide
void editLock(Note& n, int lock, int d) {
    switch (lock) {
        case 0: n.vel = uint8_t(clampi(n.vel + d * 4, 1, 127)); break;
        case 1: n.len = uint16_t(clampi(n.len + d * (kTps / 4), 1, kTps * 64)); break;
        case 2: n.chance = uint8_t(clampi(n.chance + d * 5, 0, 100)); break;
        case 3: {
            static const int kR[7] = {0, 2, 3, 4, 6, 8, 16};
            int i = 0;
            while (i < 6 && kR[i] < n.roll) ++i;
            n.roll = uint8_t(kR[clampi(i + (d > 0 ? 1 : -1), 0, 6)]);
            break;
        }
        case 4: n.cond = uint8_t(clampi(n.cond + (d > 0 ? 1 : -1), 0, kCondCount - 1)); break;
        case 5: n.tone = int8_t(clampi(n.tone + d * 4, -64, 63)); break;
        default: n.flags = uint8_t(d > 0 ? (n.flags | kFlagSlide) : (n.flags & ~kFlagSlide)); break;
    }
}
}  // namespace

void Konsol::rollPad(int pad, bool down, int vel) {
    if (!down) return;
    const int p = keyPitch(pad);
    rollPitch_ = p;
    if (rollPitch_ > rollTop_) rollTop_ = rollPitch_;
    if (rollPitch_ < rollTop_ - 15) rollTop_ = rollPitch_ + 15;
    padPitch_[pad] = int8_t(p);
    padTrack_[pad] = int8_t(selT_);
    playNote(p, vel, true);
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------
void Konsol::button(const Event& e) {
    const uint8_t id = e.id;
    if (e.type == Event::kRelease) return;
    if (e.type == Event::kHold) {
        if (id == btn::Left || id == btn::Right || id == btn::Up || id == btn::Down) arrow(id, e.shift);
        return;
    }
    if (id <= btn::F8) {
        if (e.shift && !(page_ == kPgRoll && id == btn::F8)) {   // Shift + F1..F8: jump from anywhere
            static const uint8_t kPg[8] = {kPgSession, kPgSteps, kPgRoll, kPgGen, kPgSound, kPgMixer, kPgSong, kPgProject};
            gotoPage(kPg[id]);
            return;
        }
        fkey(id, e.shift);
        return;
    }
    switch (id) {
        case btn::Pattern: gotoPage(kPgSession); break;
        case btn::Note: gotoPage(kPgSteps); break;
        case btn::Fx1: gotoPage(kPgRoll); break;
        case btn::Fx2: gotoPage(kPgGen); break;
        case btn::Instr: gotoPage(kPgSound); break;
        case btn::SampleLoader:
            gotoPage(kPgSample);
            smpTab_ = 0;
            break;
        case btn::SampleEditor:
            gotoPage(kPgSample);
            smpTab_ = 1;
            break;
        case btn::SamplePlayback: gotoPage(kPgKit); break;
        case btn::SampleRecorder: gotoPage(kPgMixer); break;
        case btn::Song: gotoPage(kPgSong); break;
        case btn::File: gotoPage(kPgProject); break;
        case btn::Performance:
            keysOverlay_ = !keysOverlay_;
            ctx_->toast(keysOverlay_ ? "Pads: keyboard of the selected track" : "Pads: page layout");
            break;
        case btn::Rec:
            if (e.shift) break;   // the Shell's recorder
            recArm_ = !recArm_;
            if (recArm_ && rt_[selT_].play < 0) {
                if (!P_->clip[selT_][selS_].exists()) ensureClip(selT_, selS_, 16);
                launch(selT_, selS_);
                if (!ctx_->playing()) ctx_->setPlaying(true);
            }
            ctx_->toast(recArm_ ? (drumLike(selT_) && P_->recHits ? "Record armed: hits (no note-offs)"
                                                                   : "Record armed: play into the clip")
                                : "Record off");
            break;
        case btn::Left:
        case btn::Right:
        case btn::Up:
        case btn::Down: arrow(id, e.shift); break;
        case btn::Insert: insertKey(e.shift); break;
        case btn::Delete: deleteKey(e.shift); break;
        case btn::Copy: copyKey(e.shift); break;
        default: break;
    }
}

// Insert: the page's "do it" (the encoder push of other Trackers).
void Konsol::insertKey(bool shift) {
    switch (page_) {
        case kPgSession:
        case kPgGen:
            if (shift) launchScene(selS_);
            else if (P_->clip[selT_][selS_].exists()) launch(selT_, selS_);
            if (!ctx_->playing()) ctx_->setPlaying(true);
            break;
        case kPgSteps: toggleStep(stepCursor_, 100); break;
        case kPgRoll: {
            ensureClip(selT_, selS_, 16);
            Clip& c = P_->clip[selT_][selS_];
            const int g = kGrids[rollGrid_];
            const int i = c.notes().at(rollTick_, rollPitch_);
            undoSave(selT_, selS_);
            NoteBuf& b = c.beginEdit();
            if (i >= 0) b.remove(i);
            else {
                Note n;
                n.start = uint16_t((rollTick_ / g) * g);
                n.len = uint16_t(g > 1 ? g - 1 : 1);
                n.pitch = uint8_t(rollPitch_);
                n.vel = 100;
                b.add(n);
            }
            c.commit();
            dirty_ = true;
            break;
        }
        case kPgSound:
            if (sndEngine_) {   // confirm the chooser
                send(Cmd{Cmd::kPanic, 0, 0, 0});
                setEngine(selT_, sndPick_);
                sndEngine_ = false;
                sndSec_ = 0;
                sndCol_ = 0;
            }
            break;
        case kPgSample:
            if (smpTab_ == 1) fkey(4, false);   // add a slice point
            else fkey(4, false);                // stretch
            break;
        case kPgKit: fkey(0, shift); break;
        case kPgSong:
            if (songCur_ < P_->songLen) {   // hear the entry's scene
                launchScene(P_->song[songCur_].scene);
                if (!ctx_->playing()) ctx_->setPlaying(true);
            }
            break;
        case kPgMixer:
            if (!mixFx_) P_->track[mixCol_].mix.mute ^= 1;
            break;
        default: break;
    }
}

void Konsol::deleteKey(bool shift) {
    if (shift) {
        redoRestore();
        return;
    }
    switch (page_) {
        case kPgSession:
        case kPgGen: {
            Clip& c = P_->clip[selT_][selS_];
            if (!c.exists()) break;
            if (rt_[selT_].play == selS_ || rt_[selT_].queued == selS_) {
                send(Cmd{Cmd::kStop, uint8_t(selT_), 0, 1});   // b = 1: at once; delete on the next press
                ctx_->toast("Clip stopped - press Delete again to delete it");
                break;
            }
            undoSave(selT_, selS_);
            c.lenSteps = 0;
            c.live = 0;
            c.buf[0].clear();
            c.buf[1].clear();
            ctx_->toast("Clip deleted (Shift + Copy: undo)");
            dirty_ = true;
            break;
        }
        case kPgSteps: {
            Clip& c = P_->clip[selT_][selS_];
            const int i = c.exists() ? laneNote(c.notes(), stepCursor_, stepNote_) : -1;
            if (i < 0) break;
            undoSave(selT_, selS_);
            NoteBuf& b = c.beginEdit();
            b.remove(i);
            c.commit();
            break;
        }
        case kPgRoll: {
            Clip& c = P_->clip[selT_][selS_];
            const int i = c.notes().at(rollTick_, rollPitch_);
            if (i >= 0) {
                undoSave(selT_, selS_);
                NoteBuf& b = c.beginEdit();
                b.remove(i);
                c.commit();
            }
            break;
        }
        case kPgSound:
            if (!sndEngine_) {   // the parameter to its default
                Engine* en = engine(selT_);
                uint8_t ids[64];
                const int n = en->sectionParams(sndSec_, ids, 64);
                if (sndCol_ < n) {
                    const kit::ParamId id = slotId(selT_, ids[sndCol_]);
                    if (id != kit::kNoParam) ctx_->params().reset(id);
                }
            }
            break;
        case kPgSample:
            if (smpTab_ == 1) fkey(5, false);
            break;
        case kPgKit: fkey(7, false); break;
        case kPgSong: fkey(1, false); break;
        default: break;
    }
}

void Konsol::copyKey(bool shift) {
    if (shift) {
        undoRestore();
        return;
    }
    if (page_ != kPgSession && page_ != kPgGen) return;
    // duplicate the selected clip into the next free scene of its track
    const Clip& c = P_->clip[selT_][selS_];
    if (!c.exists()) return;
    for (int s = selS_ + 1; s < kScenes; ++s)
        if (!P_->clip[selT_][s].exists()) {
            Clip& d = P_->clip[selT_][s];
            d.lenSteps = c.lenSteps;
            d.gen = c.gen;
            d.live = c.live;
            d.every = c.every;
            d.launchQ = c.launchQ;
            std::memcpy(d.gp, c.gp, sizeof d.gp);
            std::memcpy(&d.buf[0], &c.notes(), sizeof(NoteBuf));
            d.cur = 0;
            d.pending = 0;
            selS_ = s;
            if (s >= sceneTop_ + kPadRows) scrollScenes(s - (sceneTop_ + kPadRows - 1));
            ctx_->toast("Clip duplicated");
            dirty_ = true;
            return;
        }
    ctx_->toast("No free scene below on this track");
}

void Konsol::arrow(int id, bool shift) {
    const int dx = id == btn::Left ? -1 : (id == btn::Right ? 1 : 0);
    const int dy = id == btn::Up ? -1 : (id == btn::Down ? 1 : 0);
    switch (page_) {
        case kPgSession:
        case kPgGen:
            if (page_ == kPgGen && dy) {
                genRow_ = clampi(genRow_ + dy, 0, 9);
                break;
            }
            selT_ = clampi(selT_ + dx, 0, kTracks - 1);
            if (dy) {
                selS_ = clampi(selS_ + dy, 0, kScenes - 1);
                if (selS_ < sceneTop_) sceneTop_ = selS_;
                if (selS_ >= sceneTop_ + kPadRows) sceneTop_ = selS_ - kPadRows + 1;
            }
            break;
        case kPgSteps: {
            const Clip& c = P_->clip[selT_][selS_];
            const int L = c.exists() ? c.lenSteps : 16;
            if (dx) {
                stepCursor_ = clampi(stepCursor_ + dx * (shift ? 16 : 1), 0, L - 1);
                stepPage_ = stepCursor_ / 16;
            }
            if (dy) {
                if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3));
                stepNote_ = clampi(stepNote_ - dy * (shift ? 12 : 1), 0, 127);
            }
            break;
        }
        case kPgRoll: {
            const Clip& c = P_->clip[selT_][selS_];
            const int L = (c.exists() ? c.lenSteps : 16) * kTps;
            const int g = kGrids[rollGrid_];
            if (dx) rollTick_ = clampi((rollTick_ / g + dx) * g, 0, L - 1);
            if (dy) rollPitch_ = clampi(rollPitch_ - dy * (shift ? 12 : 1), 0, 127);
            if (rollPitch_ > rollTop_) rollTop_ = rollPitch_;
            if (rollPitch_ < rollTop_ - 15) rollTop_ = rollPitch_ + 15;
            break;
        }
        case kPgSound: {
            if (sndEngine_) {
                sndPick_ = clampi(sndPick_ + dx + dy * 4, 0, kEngCount - 1);
                break;
            }
            Engine* en = engine(selT_);
            if (dy) {
                sndSec_ = clampi(sndSec_ + dy, 0, en->sections() - 1);
                sndCol_ = 0;
            }
            if (dx) {
                uint8_t ids[64];
                const int n = en->sectionParams(sndSec_, ids, 64);
                sndCol_ = clampi(sndCol_ + dx, 0, n - 1);
            }
            break;
        }
        case kPgSample:
            if (smpTab_ == 1) {
                const int s = ts_[selT_].src;
                const int n = pool_.valid(s) ? pool_.slot(s).nOn : 0;
                sliceSel_ = clampi(sliceSel_ + dx + dy * 8, 0, n > 0 ? n - 1 : 0);
            } else {
                smpRow_ = clampi(smpRow_ + dy, 0, 2);
            }
            break;
        case kPgKit:
            if (dx) {
                kitSel_ = clampi(kitSel_ + dx, 0, kKitPads - 1);
                kitSelect(eng_[selT_].e[kEngKit], kitSel_);
            }
            if (dy) kitRow_ = clampi(kitRow_ + dy, 0, 8);
            break;
        case kPgMixer:
            if (mixFx_) fxRow_ = clampi(fxRow_ + dy + dx * 8, 0, 15);
            else {
                mixCol_ = clampi(mixCol_ + dx, 0, kTracks - 1);
                selT_ = mixCol_;
                mixRow_ = clampi(mixRow_ + dy, 0, 5);
            }
            break;
        case kPgSong:
            if (dx) songCur_ = clampi(songCur_ + dx, 0, P_->songLen ? P_->songLen - 1 : 0);
            if (dy && songCur_ < P_->songLen) {
                SongEntry& en = P_->song[songCur_];
                en.bars = uint8_t(clampi(en.bars - dy * (shift ? 4 : 1), 1, 64));
                dirty_ = true;
            }
            break;
        case kPgProject:
            trkRow_ = clampi(trkRow_ + dy, 0, kTracks - 1);
            trkCol_ = clampi(trkCol_ + dx, 0, 4);
            selT_ = trkRow_;
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// encoder
// ---------------------------------------------------------------------------
void Konsol::encoder(int d, bool shift) {
    switch (page_) {
        case kPgSession:
            if (shift) ctx_->setBpm(ctx_->bpm() + float(d));
            else scrollScenes(d);
            break;
        case kPgSteps: {
            Clip& c = P_->clip[selT_][selS_];
            if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3));
            const int target = heldStep_ >= 0 ? heldStep_ : stepCursor_;
            if (heldStep_ >= 0 || shift) {   // edit the lock of the step's lane note
                if (!c.exists()) break;
                const int i = laneNote(c.notes(), target, stepNote_);
                if (i < 0) break;
                NoteBuf& b = c.beginEdit();
                editLock(b.n[i], stepLock_, d);
                c.commit();
                stepEdited_ = true;
                dirty_ = true;
            } else {
                const int L = c.exists() ? c.lenSteps : 16;
                stepCursor_ = clampi(stepCursor_ + d, 0, L - 1);
                stepPage_ = stepCursor_ / 16;
            }
            break;
        }
        case kPgRoll: {
            ensureClip(selT_, selS_, 16);
            Clip& c = P_->clip[selT_][selS_];
            const int L = c.lenSteps * kTps;
            const int g = kGrids[rollGrid_];
            const int i = c.notes().at(rollTick_, rollPitch_);
            if (rollFn_ == 0 || i < 0) {
                const int step = shift ? 1 : g;
                rollTick_ = clampi(rollTick_ + d * step, 0, L - 1);
                if (!shift) rollTick_ = (rollTick_ / g) * g;
                break;
            }
            NoteBuf& b = c.beginEdit();
            Note& n = b.n[i];
            switch (rollFn_) {
                case 1: n.len = uint16_t(clampi(n.len + d * (shift ? 1 : g), 1, L)); break;
                case 2: n.vel = uint8_t(clampi(n.vel + d * (shift ? 1 : 6), 1, 127)); break;
                case 3: {
                    const int ns = clampi(n.start + d * (shift ? 1 : g), 0, L - 1);
                    n.start = uint16_t(ns);
                    rollTick_ = ns;
                    break;
                }
                case 4:
                    n.pitch = uint8_t(clampi(n.pitch + d * (shift ? 12 : 1), 0, 127));
                    rollPitch_ = n.pitch;
                    break;
                default: break;
            }
            b.sort();
            c.commit();
            dirty_ = true;
            break;
        }
        case kPgGen: {
            Clip& c = P_->clip[selT_][selS_];
            if (genRow_ == 0) {
                ensureClip(selT_, selS_, 16);
                const int k = clampi(c.gen + d, 0, kGenCount - 1);
                if (k != c.gen) defaultGen(c, k);
                if (k == kGenUsul) c.lenSteps = uint8_t(usulSteps(c.gp[0], c.gp[1]));
            } else if (genRow_ <= 6) {
                const int pi = genRow_ - 1;
                const ot::gen::ParamDesc& pd = genParam(c.gen, pi, c.gp);
                if (!pd.name) break;
                c.gp[pi] = int16_t(clampi(c.gp[pi] + d * (shift ? 10 : 1), pd.min, pd.max));
                if (c.gen == kGenBBCut && pi == 0) {
                    for (int k = 1; k <= 2; ++k) {
                        const ot::gen::ParamDesc& q = genParam(c.gen, k, c.gp);
                        if (q.name) c.gp[k] = q.def;
                    }
                }
                if (c.gen == kGenUsul && pi <= 1) c.lenSteps = uint8_t(usulSteps(c.gp[0], c.gp[1]));
            } else if (genRow_ == 7) {
                ensureClip(selT_, selS_, 16);
                static const int kLens[9] = {4, 8, 12, 16, 24, 32, 48, 64, 128};
                int k = 0;
                while (k < 8 && kLens[k] < c.lenSteps) ++k;
                c.lenSteps = uint8_t(shift ? clampi(c.lenSteps + d, 1, kMaxSteps) : kLens[clampi(k + d, 0, 8)]);
            } else if (genRow_ == 8) {
                c.live = d > 0 ? 1 : 0;
            } else {
                c.every = uint8_t(clampi(c.every + d, 1, 8));
            }
            dirty_ = true;
            break;
        }
        case kPgSound: {
            if (sndEngine_) {
                sndPick_ = clampi(sndPick_ + d, 0, kEngCount - 1);
                break;
            }
            Engine* en = engine(selT_);
            uint8_t ids[64];
            const int n = en->sectionParams(sndSec_, ids, 64);
            if (sndCol_ >= n) sndCol_ = n - 1;
            if (n <= 0) break;
            en->stepParam(ids[sndCol_], d, shift);
            slotEdited(selT_, ids[sndCol_]);
            if (P_->track[selT_].engine == kEngMakam) makamSetKey(en, P_->key);
            dirty_ = true;
            break;
        }
        case kPgSample: {
            TrackSample& ts = ts_[selT_];
            if (smpTab_ == 1) {   // move the selected slice point
                const int s = ts.src;
                if (!pool_.valid(s)) break;
                const uint32_t fr = pool_.slot(s).frames;
                const int32_t step = shift ? 16 : int32_t(fr / 800 > 64 ? fr / 800 : 64);
                pool_.moveOnset(s, sliceSel_, d * step);
                dirty_ = true;
                break;
            }
            switch (smpRow_) {
                case 0: {   // bars (0 = detect)
                    ts.bars = uint8_t(clampi(ts.bars + d, 0, 32));
                    int bars = ts.bars;
                    if (pool_.valid(ts.src)) ts.srcBpm = loopBpm(pool_.slot(ts.src).frames, bars, ctx_->bpm(), &pool_.slot(ts.src));
                    if (!ts.stretched) ts.outBpm = ts.srcBpm;
                    break;
                }
                case 1: ts.mode = uint8_t(clampi(ts.mode + d, 0, kStrModes - 1)); break;
                default: ts.pitch = int8_t(clampi(ts.pitch + d, -24, 24)); break;
            }
            dirty_ = true;
            break;
        }
        case kPgKit: {
            Engine* k = eng_[selT_].e[kEngKit];
            const int pi = kpTune + kitRow_;   // rows: tune .. reverse
            k->stepParam(pi, d, shift);
            if (P_->track[selT_].engine == kEngKit) slotEdited(selT_, pi);
            dirty_ = true;
            break;
        }
        case kPgMixer: {
            if (mixFx_) {
                FxCfg& f = P_->fx;
                const float k = (shift ? 0.1f : 0.01f) * float(d);
                auto adj = [&](float& v, float lo, float hi) { v = v + k < lo ? lo : (v + k > hi ? hi : v + k); };
                switch (fxRow_) {
                    case 0: f.dlyType = uint8_t(clampi(f.dlyType + d, 0, kDlyTypes - 1)); break;
                    case 1: f.delaySync = uint8_t(clampi(f.delaySync + d, 0, 5)); break;
                    case 2: adj(f.delayFb, 0.0f, 0.95f); break;
                    case 3: adj(f.delayTone, 0.0f, 1.0f); break;
                    case 4: adj(f.dlyMod, 0.0f, 1.0f); break;
                    case 5: f.pingPong = d > 0 ? 1 : 0; break;
                    case 6: adj(f.retA, 0.0f, 1.5f); break;
                    case 7: f.revType = uint8_t(clampi(f.revType + d, 0, kRevTypes - 1)); break;
                    case 8: adj(f.revSize, 0.0f, 1.0f); break;
                    case 9: adj(f.revDamp, 0.0f, 1.0f); break;
                    case 10: adj(f.revPre, 0.0f, 1.0f); break;
                    case 11: adj(f.retB, 0.0f, 1.5f); break;
                    case 12: adj(f.compThr, 0.0f, 1.0f); break;
                    case 13: adj(f.compRatio, 0.0f, 1.0f); break;
                    case 14: adj(f.compMakeup, 0.0f, 1.0f); break;
                    default: adj(f.master, 0.0f, 1.25f); break;
                }
                fxDirty_ = true;
                dirty_ = true;
                refreshFx();
                break;
            }
            Mix& m = P_->track[mixCol_].mix;
            const float k = (shift ? 0.1f : 0.01f) * float(d);
            auto adj = [&](float& v, float lo, float hi) { v = v + k < lo ? lo : (v + k > hi ? hi : v + k); };
            switch (mixRow_) {
                case 0: adj(m.level, 0.0f, 1.25f); break;
                case 1: adj(m.pan, -1.0f, 1.0f); break;
                case 2: adj(m.sendA, 0.0f, 1.0f); break;
                case 3: adj(m.sendB, 0.0f, 1.0f); break;
                case 4: m.stem = uint8_t(clampi(m.stem + d, 0, 4)); break;
                default: m.midiOut = uint8_t(d > 0); break;
            }
            dirty_ = true;
            break;
        }
        case kPgSong: {
            if (songCur_ >= P_->songLen) break;
            SongEntry& en = P_->song[songCur_];
            if (shift) en.bars = uint8_t(clampi(en.bars + d, 1, 64));
            else en.scene = uint8_t(clampi(en.scene + d, 0, kScenes - 1));
            dirty_ = true;
            break;
        }
        case kPgProject: {
            TrackCfg& tc = P_->track[trkRow_];
            switch (trkCol_) {
                case 0:
                    send(Cmd{Cmd::kPanic, 0, 0, 0});
                    setEngine(trkRow_, clampi(tc.engine + d, 0, kEngCount - 1));
                    break;
                case 1: tc.octave = uint8_t(clampi(tc.octave + d, 0, 7)); break;
                case 2: tc.mix.stem = uint8_t(clampi(tc.mix.stem + d, 0, 4)); break;
                case 3: tc.mix.midiOut = d > 0 ? 1 : 0; break;
                default: tc.midiCh = uint8_t(clampi(tc.midiCh + d, 0, 16)); break;
            }
            dirty_ = true;
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------
// F1..F8 per page
// ---------------------------------------------------------------------------
void Konsol::fkey(int k, bool shift) {
    char msg[48] = {};
    switch (page_) {
        case kPgSession: {
            static const uint8_t kPg[8] = {kPgSession, kPgSteps, kPgRoll, kPgGen, kPgSound, kPgMixer, kPgSong, kPgProject};
            if (k == 0) {
                send(Cmd{Cmd::kStopAll, 0, 0, 0});
                break;
            }
            gotoPage(kPg[k]);
            break;
        }
        case kPgSteps:
            if (k <= 4) stepLock_ = k;
            else if (k == 5) stepLock_ = stepLock_ == 5 ? 6 : 5;   // tone / slide
            else if (k == 6) {
                ensureClip(selT_, selS_, 16);
                Clip& c = P_->clip[selT_][selS_];
                static const int kL[6] = {8, 16, 32, 64, 96, 128};
                int i = 0;
                while (i < 5 && kL[i] <= c.lenSteps) ++i;
                if (kL[i] <= c.lenSteps) i = 0;
                c.lenSteps = uint8_t(kL[i]);
                if (stepPage_ * 16 >= c.lenSteps) stepPage_ = 0;
                std::snprintf(msg, sizeof msg, "Clip length %d steps", c.lenSteps);
                ctx_->toast(msg);
            } else {
                Clip& c = P_->clip[selT_][selS_];
                if (!c.exists()) break;
                undoSave(selT_, selS_);
                NoteBuf& b = c.beginEdit();
                for (int i = b.count - 1; i >= 0; --i)
                    if (b.n[i].pitch == stepNote_) b.remove(i);
                c.commit();
                ctx_->toast("Lane cleared");
            }
            break;
        case kPgRoll: {
            Clip& c = P_->clip[selT_][selS_];
            if (k <= 4) rollFn_ = k;
            else if (k == 5) rollGrid_ = (rollGrid_ + 1) % 5;
            else if (k == 6) {
                if (!c.exists()) break;
                undoSave(selT_, selS_);
                NoteBuf& b = c.beginEdit();
                const int g = kGrids[rollGrid_];
                for (int i = 0; i < b.count; ++i) {
                    int s = ((b.n[i].start + g / 2) / g) * g;
                    if (s >= c.lenSteps * kTps) s = 0;
                    b.n[i].start = uint16_t(s);
                }
                b.sort();
                c.commit();
                ctx_->toast("Quantised to the grid");
            } else if (shift) {
                redoRestore();
            } else {
                undoRestore();
            }
            break;
        }
        case kPgGen: {
            Clip& c = P_->clip[selT_][selS_];
            switch (k) {
                case 0:
                case 1:
                case 4:
                    ensureClip(selT_, selS_, 16);
                    if (c.gen == kGenNotes) {
                        ctx_->toast("Choose a player first (row 1)");
                        break;
                    }
                    if (k == 1) c.gp[kGenParams - 1] = int16_t(1 + (nowMs_ * 7919u + uint32_t(c.gp[5]) * 31u) % 999u);
                    undoSave(selT_, selS_);
                    {
                        const uint8_t wasLive = c.live;
                        if (k == 4) c.live = 0;
                        if (runGenerator(selT_, selS_, 0, msg, sizeof msg)) ctx_->toast(msg[0] ? msg : "Written");
                        else ctx_->toast(msg[0] ? msg : "Generator failed");
                        if (k == 4) {
                            c.gen = kGenNotes;
                            gotoPage(kPgRoll);
                        } else {
                            c.live = wasLive;
                        }
                    }
                    break;
                case 2:
                    c.live = !c.live;
                    ctx_->toast(c.live ? "Live: varies every loop" : "Live off: notes stay");
                    break;
                case 3: c.every = uint8_t(c.every % 8 + 1); break;
                case 5:
                    if (shift) redoRestore();
                    else undoRestore();
                    break;
                case 6:
                    if (!c.exists()) break;
                    undoSave(selT_, selS_);
                    {
                        NoteBuf& b = c.beginEdit();
                        b.clear();
                        c.commit();
                    }
                    ctx_->toast("Clip notes cleared");
                    break;
                default: {   // the clip's own launch quantise
                    if (!c.exists()) break;
                    c.launchQ = uint8_t((c.launchQ + 1) % 6);
                    std::snprintf(msg, sizeof msg, "Clip launch: %s",
                                  c.launchQ ? launchQNames()[c.launchQ - 1] : "project setting");
                    ctx_->toast(msg);
                    break;
                }
            }
            dirty_ = true;
            break;
        }
        case kPgSound: {
            Engine* en = engine(selT_);
            switch (k) {
                case 0:
                    sndSec_ = sndSec_ > 0 ? sndSec_ - 1 : en->sections() - 1;
                    sndCol_ = 0;
                    break;
                case 1:
                    sndSec_ = (sndSec_ + 1) % en->sections();
                    sndCol_ = 0;
                    break;
                case 2:
                    sndEngine_ = !sndEngine_;
                    sndPick_ = P_->track[selT_].engine;
                    if (sndEngine_) ctx_->toast("Encoder / arrows: engine   Insert: use it   F3: cancel");
                    break;
                case 3:
                    en->reset();
                    refreshSlots(selT_);
                    ctx_->toast("Init parameters");
                    break;
                case 4:
                    en->preset(0);
                    refreshSlots(selT_);
                    ctx_->toast("KONSOL preset");
                    break;
                case 5: gotoPage(kPgSample); break;
                case 6: keyOct_ = keyOct_ >= 2 ? -2 : keyOct_ + 1; break;
                default: keysOverlay_ = !keysOverlay_; break;
            }
            dirty_ = true;
            break;
        }
        case kPgSample: {
            TrackSample& ts = ts_[selT_];
            switch (k) {
                case 0: smpTab_ = 0; break;
                case 1: smpTab_ = 1; break;
                case 2: openBrowser(kPickSample); break;
                case 3:
                    send(Cmd{Cmd::kPanic, 0, 0, 0});
                    setTrackBreak(selT_);
                    ctx_->toast("Built-in break (172 BPM, 2 bars)");
                    break;
                case 4:
                    if (smpTab_ == 1) {   // add a slice point in the middle of the selected slice
                        uint32_t a, e;
                        if (pool_.sliceRange(ts.src, sliceSel_, a, e)) {
                            const int nk = pool_.addOnset(ts.src, a + (e - a) / 2);
                            if (nk >= 0) sliceSel_ = nk;
                        }
                    } else if (stretchTrack(selT_)) {
                        ctx_->toast(str_->busy() ? "Stretching..." : "Already at the tempo");
                    }
                    break;
                case 5:
                    if (smpTab_ == 1) {
                        pool_.removeOnset(ts.src, sliceSel_);
                        if (sliceSel_ > 0) --sliceSel_;
                    } else {
                        unstretch(selT_);
                        ctx_->toast("The original sample plays");
                    }
                    break;
                case 6: {
                    const int n = kitFromSlices(selT_, shift);
                    setEngine(selT_, kEngKit);
                    refreshSlots(selT_);
                    std::snprintf(msg, sizeof msg, "Kit: %d pads from the slices", n);
                    ctx_->toast(msg);
                    break;
                }
                default:
                    if (smpTab_ == 1 && shift) {   // re-detect at another sensitivity
                        sliceSens_ = sliceSens_ >= 0.85f ? 0.2f : sliceSens_ + 0.15f;
                        pool_.detectOnsets(ts.src, sliceSens_);
                        std::snprintf(msg, sizeof msg, "Sensitivity %d%%: %d slices", int(sliceSens_ * 100.0f + 0.5f),
                                      pool_.valid(ts.src) ? pool_.slot(ts.src).nOn : 0);
                        ctx_->toast(msg);
                    } else if (smpTab_ == 1) {
                        pool_.detectOnsets(ts.src, sliceSens_);
                        std::snprintf(msg, sizeof msg, "%d slices at the transients", pool_.valid(ts.src) ? pool_.slot(ts.src).nOn : 0);
                        ctx_->toast(msg);
                    } else {
                        setEngine(selT_, kEngSampler);
                        refreshSlots(selT_);
                        ctx_->toast("Track engine: SAMPLER");
                    }
                    break;
            }
            dirty_ = true;
            break;
        }
        case kPgKit: {
            switch (k) {
                case 0:
                case 1: {
                    const int n = kitFromSlices(selT_, k == 1);
                    if (P_->track[selT_].engine != kEngKit) setEngine(selT_, kEngKit);
                    refreshSlots(selT_);
                    std::snprintf(msg, sizeof msg, "Kit: %d pads%s", n, k == 1 ? ", kick / snare / hat first" : "");
                    ctx_->toast(msg);
                    break;
                }
                case 2: openBrowser(kPickKitFolder); break;
                case 3: openBrowser(kPickPadSample); break;
                case 4: openBrowser(kPickKit); break;
                case 5: openKeyboard(kNameKit); break;
                case 6:
                    kitFollow_ = !kitFollow_;
                    ctx_->toast(kitFollow_ ? "The selected pad follows what plays" : "Pad selection: by hand");
                    break;
                default: {
                    KitPad* pads = kitPads(eng_[selT_].e[kEngKit]);
                    if (!pads) break;
                    pads[kitSel_] = KitPad();
                    std::snprintf(pads[kitSel_].name, sizeof pads[kitSel_].name, "PAD %d", kitSel_ + 1);
                    kitRefresh(eng_[selT_].e[kEngKit]);
                    gcSamples();
                    break;
                }
            }
            dirty_ = true;
            break;
        }
        case kPgMixer: {
            if (mixFx_ && k >= 2) {
                FxCfg& f = P_->fx;
                switch (k) {
                    case 2: f.revType = uint8_t((f.revType + 1) % kRevTypes); break;
                    case 3: f.dlyType = uint8_t((f.dlyType + 1) % kDlyTypes); break;
                    case 4: f.pingPong ^= 1; break;
                    case 5: f.limiter ^= 1; break;
                    default: break;
                }
                fxDirty_ = true;
                dirty_ = true;
                refreshFx();
                break;
            }
            Mix& m = P_->track[mixCol_].mix;
            switch (k) {
                case 0: mixFx_ = false; break;
                case 1: mixFx_ = true; break;
                case 2: m.mute ^= 1; break;
                case 3: m.solo ^= 1; break;
                case 4: m.midiOut ^= 1; break;
                case 5:
                    m.level = 0.8f;
                    m.pan = 0.0f;
                    break;
                case 6:
                    for (auto& tc : P_->track) tc.mix.solo = 0;
                    break;
                default:
                    for (auto& tc : P_->track) tc.mix.mute = 0;
                    break;
            }
            dirty_ = true;
            break;
        }
        case kPgSong: {
            Project& p = *P_;
            switch (k) {
                case 0:
                    if (p.songLen < kSongMax) {
                        const int at = p.songLen ? songCur_ + 1 : 0;
                        for (int i = p.songLen; i > at; --i) p.song[i] = p.song[i - 1];
                        p.song[at] = p.songLen ? p.song[songCur_] : SongEntry{uint8_t(selS_), 4};
                        ++p.songLen;
                        songCur_ = at;
                    }
                    break;
                case 1:
                    if (songCur_ < p.songLen) {
                        for (int i = songCur_; i + 1 < p.songLen; ++i) p.song[i] = p.song[i + 1];
                        --p.songLen;
                        if (songCur_ >= p.songLen && songCur_ > 0) --songCur_;
                    }
                    break;
                case 2:
                    setSongMode(!songMode_);
                    ctx_->toast(songMode_ ? "Song mode: the arrangement plays" : "Session mode: launch clips");
                    break;
                case 3:
                    if (p.songLen < kSongMax) {
                        p.song[p.songLen] = SongEntry{uint8_t(selS_), 4};
                        songCur_ = p.songLen++;
                    }
                    break;
                case 4:
                    p.songLoop = !p.songLoop;
                    setLoop_ = p.songLoop;
                    break;
                case 5:
                    if (songCur_ > 0 && songCur_ < p.songLen) {
                        const SongEntry x = p.song[songCur_];
                        p.song[songCur_] = p.song[songCur_ - 1];
                        p.song[--songCur_] = x;
                    }
                    break;
                case 6:
                    if (songCur_ + 1 < p.songLen) {
                        const SongEntry x = p.song[songCur_];
                        p.song[songCur_] = p.song[songCur_ + 1];
                        p.song[++songCur_] = x;
                    }
                    break;
                default:
                    if (!p.songLen) break;
                    if (ctx_->playing()) {
                        setSongMode(true);
                    } else {
                        songMode_ = true;
                        songReq_ = false;
                        ctx_->setPlaying(true);
                    }
                    break;
            }
            dirty_ = true;
            break;
        }
        case kPgProject:
            switch (k) {
                case 0: trkRow_ = selT_; break;
                case 1: openBrowser(kPickProject); break;
                case 2:
                    if (!ctx_->fs) ctx_->toast("No SD card");
                    else if (saveProject(P_->name)) ctx_->toast("Saved");
                    else ctx_->toast("Save failed");
                    break;
                case 3: openKeyboard(kNameSaveAs); break;
                case 4:
                case 5:
                    if (confirmKey_ != k || nowMs_ - confirmMs_ > 3000) {
                        confirmKey_ = k;
                        confirmMs_ = nowMs_;
                        ctx_->toast(k == 4 ? "Press again: new empty project" : "Press again: load the demo");
                        break;
                    }
                    confirmKey_ = -1;
                    ctx_->setPlaying(false);
                    send(Cmd{Cmd::kPanic, 0, 0, 0});
                    newProject(k == 5);
                    if (k == 4) {
                        for (int t = 0; t < kTracks; ++t)
                            for (int s = 0; s < kScenes; ++s) P_->clip[t][s].lenSteps = 0;
                        P_->songLen = 0;
                    }
                    refreshRegistry();
                    undoN_ = redoN_ = 0;
                    ctx_->toast(k == 4 ? "New project" : "Demo loaded");
                    break;
                case 6: openKeyboard(kNameRename); break;
                default: {
                    const uint32_t before = pool_.usedSamples();
                    gcSamples();
                    compactPool();
                    std::snprintf(msg, sizeof msg, "Sample memory: %lu KB freed",
                                  static_cast<unsigned long>((before - pool_.usedSamples()) / 512u));
                    ctx_->toast(msg);
                    break;
                }
            }
            if (k != 4 && k != 5) confirmKey_ = -1;
            break;
        default: break;
    }
}

// Results of the browser / keyboard modals (main loop, after the Shell closed them).
void Konsol::applyPick() {
    if (pickWhat_ < 0) return;
    const int what = pickWhat_;
    pickWhat_ = -1;
    char msg[64];
    const char* path = pickPath_;
    switch (what) {
        case kPickSample: {
            send(Cmd{Cmd::kPanic, 0, 0, 0});
            if (loadSample(selT_, path)) {
                const int eng = P_->track[selT_].engine;
                if (eng != kEngSampler && eng != kEngGrain && eng != kEngKit) {
                    setEngine(selT_, kEngSampler);
                    refreshSlots(selT_);
                }
                const TrackSample& ts = ts_[selT_];
                std::snprintf(msg, sizeof msg, "T%d: %d bars at %d BPM, %d slices", selT_ + 1, ts.bars,
                              int(ts.srcBpm + 0.5f), pool_.valid(ts.src) ? pool_.slot(ts.src).nOn : 0);
                ctx_->toast(msg);
            }
            break;
        }
        case kPickKitFolder: {
            char dir[128];
            std::snprintf(dir, sizeof dir, "%s", path);
            char* sl = std::strrchr(dir, '/');
            if (sl) *sl = 0;
            else dir[0] = 0;
            send(Cmd{Cmd::kPanic, 0, 0, 0});
            const int n = kitFromFolder(selT_, dir);
            if (n > 0) {
                if (P_->track[selT_].engine != kEngKit) setEngine(selT_, kEngKit);
                refreshSlots(selT_);
                std::snprintf(msg, sizeof msg, "Kit: %d one-shots from %.30s", n, dir);
                ctx_->toast(msg);
            }
            break;
        }
        case kPickPadSample:
            if (padSample(selT_, kitSel_, path)) {
                std::snprintf(msg, sizeof msg, "Pad %d: %.40s", kitSel_ + 1, path);
                ctx_->toast(msg);
            }
            break;
        case kPickKit:
            send(Cmd{Cmd::kPanic, 0, 0, 0});
            if (loadKit(selT_, path)) {
                if (P_->track[selT_].engine != kEngKit) setEngine(selT_, kEngKit);
                refreshSlots(selT_);
                ctx_->toast("Kit loaded");
            }
            break;
        case kPickProject: {
            // "Lab/KONSOL/Projects/NAME.kon" -> NAME
            const char* b = std::strrchr(path, '/');
            char nm[24];
            std::snprintf(nm, sizeof nm, "%s", b ? b + 1 : path);
            char* dot = std::strrchr(nm, '.');
            if (dot) *dot = 0;
            if (loadProject(nm)) ctx_->toast("Project loaded");
            else ctx_->toast("Cannot load that project");
            break;
        }
        case kNameSaveAs:
        case kNameRename: {
            char nm[16];
            int n = 0;
            for (const char* q = path; *q && n < 15; ++q) {
                const char c = *q;
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') nm[n++] = c;
                else if (c >= 'a' && c <= 'z') nm[n++] = char(c - 32);
                else if (c == ' ') nm[n++] = '_';
            }
            nm[n] = 0;
            if (!n) break;
            copyStr(P_->name, sizeof P_->name, nm);
            if (what == kNameSaveAs) {
                std::snprintf(msg, sizeof msg, saveProject(nm) ? "Saved as %s" : "Save failed", nm);
                ctx_->toast(msg);
            } else {
                ctx_->toast("Renamed - F3 saves");
            }
            dirty_ = true;
            break;
        }
        case kNameKit: {
            char nm[16];
            int n = 0;
            for (const char* q = path; *q && n < 15; ++q) {
                const char c = *q;
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                    nm[n++] = c;
                else if (c == ' ') nm[n++] = '_';
            }
            nm[n] = 0;
            if (!n) break;
            std::snprintf(msg, sizeof msg, saveKit(selT_, nm) ? "Kit saved: %s" : "Kit save failed", nm);
            ctx_->toast(msg);
            break;
        }
        default: break;
    }
}

}  // namespace konsol
}  // namespace lab
