// KONSOL input: pads, keys, encoder, per page (MANUAL.md has the full map).
#include <cstdio>
#include <cstring>

#include "ws/gen_glue.h"
#include "ws/konsol.h"

namespace lab {
namespace konsol {

void makamSetKey(Engine* e, int key);

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr int kGrids[5] = {kTps / 2, kTps, kTps * 2, kTps * 4, kTps * 16};   // 1/32 .. 1 bar
const char kNameChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
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
    c.lenSteps = uint8_t(steps);
    dirty_ = true;
}

void Konsol::undoSave(int t, int s) {
    std::memcpy(undo_, &P_->clip[t][s].notes(), sizeof(NoteBuf));
    undoT_ = t;
    undoS_ = s;
    undoLen_ = P_->clip[t][s].lenSteps;
}

void Konsol::undoRestore() {
    if (undoT_ < 0) {
        ctx_->toast("Nothing to undo");
        return;
    }
    Clip& c = P_->clip[undoT_][undoS_];
    NoteBuf cur;
    std::memcpy(&cur, &c.notes(), sizeof(NoteBuf));
    const int curLen = c.lenSteps;
    NoteBuf& b = c.beginEdit();
    std::memcpy(&b, undo_, sizeof(NoteBuf));
    c.lenSteps = uint8_t(undoLen_);
    c.commit();
    std::memcpy(undo_, &cur, sizeof(NoteBuf));   // undo again = redo
    undoLen_ = curLen;
    ctx_->toast("Undo");
}

int Konsol::keyPitch(int pad, int rows) const {
    const int row = padRow(pad), col = padCol(pad);
    const int fromBottom = (kPadRows - 1) - row;   // 0 = bottom row
    const int eng = P_->track[selT_].engine;
    int base;
    if (eng == kEngDrums) base = 36;
    else if (eng == kEngPerc) base = 60;
    else if (eng == kEngSampler && engine(selT_)->get(0) > 1.5f) base = 36;   // slices
    else base = 12 * (P_->track[selT_].octave + 1);
    (void)rows;
    const int p = base + keyOct_ * 12 + fromBottom * 12 + col;
    return p < 0 ? 0 : (p > 127 ? 127 : p);
}

void Konsol::scrollScenes(int d) {
    sceneTop_ = clampi(sceneTop_ + d, 0, kScenes - kPadRows);
    if (selS_ < sceneTop_) selS_ = sceneTop_;
    if (selS_ > sceneTop_ + kPadRows - 1) selS_ = sceneTop_ + kPadRows - 1;
}

// The roll shows the clip's notes (or the track's keyboard range when empty).
void Konsol::focusRoll() {
    const NoteBuf& nb = P_->clip[selT_][selS_].notes();
    int lo = 127, hi = 0;
    for (int i = 0; i < nb.count; ++i) {
        lo = nb.n[i].pitch < lo ? nb.n[i].pitch : lo;
        hi = nb.n[i].pitch > hi ? nb.n[i].pitch : hi;
    }
    if (hi < lo) {
        lo = keyPitch(padAt(0, 3), 4);
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
    help_ = false;
    // a hand-played pad always ends on the pad it started (page changes in between)
    if (!down && padPitch_[pad] >= 0) {
        playNote(padPitch_[pad], 0, false);
        padPitch_[pad] = -1;
        if (page_ == kPgSteps && heldStep_ < 0) return;
    }
    if (keysOverlay_ && page_ != kPgSteps) {
        if (down) keysPad(pad, true, e.velocity, 4);
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
        case kPgSound:
        case kPgSamples:
            if (down) keysPad(pad, true, e.velocity, 4);
            break;
        case kPgMixer:
            if (col < 8) mixerPad(col, row, down);
            else controlPad(col, row, down, e.velocity);
            break;
        case kPgSong:
            if (!down) break;
            if (col < 4) {   // scenes 0..15
                const int sc = row * 4 + col;
                if (e.shift || P_->songLen == 0) {
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
            break;
        default:
            if (down) keysPad(pad, true, e.velocity, 4);
            break;
    }
}

void Konsol::keysPad(int pad, bool down, int vel, int rows) {
    if (!down) return;
    const int p = keyPitch(pad, rows);
    padPitch_[pad] = int8_t(p);
    playNote(p, vel, true);
    if (page_ == kPgRoll) rollPitch_ = p;
    if (page_ == kPgSteps) stepNote_ = p;
}

void Konsol::sessionPad(int col, int row, bool down, int vel) {
    (void)vel;
    if (!down) return;
    const int t = col, s = sceneTop_ + row;
    Clip& c = P_->clip[t][s];
    const bool shift = false;
    (void)shift;
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
            case 2: dlyThrow_ = down; break;
            default: revThrow_ = down; break;
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
            if (nowMs_ - gridModeDownMs_ < 350) gridModeLatched_ = true;   // tapped: stays on
            else gridMode_ = kGmLaunch;                                  // held: momentary
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
    // col 10: stop all, scenes up, scenes down, record arm
    switch (row) {
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

// ---------------------------------------------------------------------------
// steps
// ---------------------------------------------------------------------------
void Konsol::toggleStep(int step, int vel) {
    const int t = selT_, s = selS_;
    ensureClip(t, s, 16);
    Clip& c = P_->clip[t][s];
    if (step >= c.lenSteps) return;
    if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3), 4);
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
        const int p = keyPitch(padAt(col, row), 2);
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
        padPitch_[padAt(col, row)] = int8_t(p);
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
// the lane note of a step (first note starting in it at `pitch`), -1
int laneNote(const NoteBuf& b, int step, int pitch) {
    for (int i = 0; i < b.count; ++i)
        if (b.n[i].start / kTps == step && b.n[i].pitch == pitch) return i;
    return -1;
}
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
        case 4: n.tone = int8_t(clampi(n.tone + d * 4, -64, 63)); break;
        case 5: n.flags = uint8_t(d > 0 ? (n.flags | kFlagSlide) : (n.flags & ~kFlagSlide)); break;
        default: break;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// piano roll
// ---------------------------------------------------------------------------
void Konsol::rollPad(int pad, bool down, int vel) {
    if (!down) return;
    const int p = keyPitch(pad, 4);
    rollPitch_ = p;
    if (rollPitch_ > rollTop_) rollTop_ = rollPitch_;
    if (rollPitch_ < rollTop_ - 15) rollTop_ = rollPitch_ + 15;
    padPitch_[pad] = int8_t(p);
    playNote(p, vel, true);
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------
void Konsol::button(const Event& e) {
    const uint8_t id = e.id;
    if (e.type == Event::kRelease) return;
    if (e.type == Event::kHold) {
        // held arrows repeat
        if (id == btn::Left || id == btn::Right || id == btn::Up || id == btn::Down) arrow(id, e.shift);
        return;
    }
    if (id <= btn::F8) {
        if (e.shift) {   // Shift + F1..F8: jump to a page from anywhere
            static const uint8_t kPg[8] = {kPgSession, kPgSteps, kPgRoll, kPgGen, kPgSound, kPgMixer, kPgSong, kPgProject};
            page_ = kPg[id];
            if (page_ == kPgRoll) focusRoll();
            help_ = false;
            confirm_ = false;
            if (page_ == kPgProject && projTab_ == 1) scanProjects();
            return;
        }
        fkey(id, false);
        return;
    }
    help_ = id == btn::Help ? !help_ : false;
    switch (id) {
        case btn::Pattern: page_ = kPgSession; break;
        case btn::Note: page_ = kPgSteps; break;
        case btn::Fx1:
            page_ = kPgRoll;
            focusRoll();
            break;
        case btn::Fx2: page_ = kPgGen; break;
        case btn::Instr:
        case btn::InstrParams: page_ = kPgSound; break;
        case btn::Master: page_ = kPgMixer; break;
        case btn::Song: page_ = kPgSong; break;
        case btn::File:
            page_ = kPgProject;
            if (projTab_ == 1) scanProjects();
            break;
        case btn::SampleLoader:
        case btn::SamplePlayback:
        case btn::SampleEditor:
        case btn::SampleRecorder:
            page_ = kPgSamples;
            scanSamples();
            break;
        case btn::Performance:
        case btn::Aux:
            keysOverlay_ = !keysOverlay_;
            ctx_->toast(keysOverlay_ ? "Pads: keyboard (selected track)" : "Pads: page layout");
            break;
        case btn::Rec:
            if (e.shift) break;   // the Shell's recorder
            recArm_ = !recArm_;
            if (recArm_ && rt_[selT_].play < 0) {
                if (!P_->clip[selT_][selS_].exists()) ensureClip(selT_, selS_, 16);
                launch(selT_, selS_);
                if (!ctx_->playing()) ctx_->setPlaying(true);
            }
            ctx_->toast(recArm_ ? "Record armed: play into the clip" : "Record off");
            break;
        case btn::Left:
        case btn::Right:
        case btn::Up:
        case btn::Down: arrow(id, e.shift); break;
        case btn::EncPush: encoder(0, e.shift); break;
        case btn::Copy:
            if (page_ == kPgSession || page_ == kPgGen) {
                // copy the selected clip into the next free scene of its track
                const Clip& c = P_->clip[selT_][selS_];
                if (!c.exists()) break;
                for (int s = selS_ + 1; s < kScenes; ++s)
                    if (!P_->clip[selT_][s].exists()) {
                        Clip& d = P_->clip[selT_][s];
                        d.lenSteps = c.lenSteps;
                        d.gen = c.gen;
                        d.live = c.live;
                        d.every = c.every;
                        std::memcpy(d.gp, c.gp, sizeof d.gp);
                        std::memcpy(&d.buf[0], &c.notes(), sizeof(NoteBuf));
                        d.cur = 0;
                        d.pending = 0;
                        selS_ = s;
                        if (s >= sceneTop_ + kPadRows) scrollScenes(s - (sceneTop_ + kPadRows - 1));
                        ctx_->toast("Clip duplicated");
                        dirty_ = true;
                        break;
                    }
            }
            break;
        case btn::Delete:
            if (page_ == kPgSession || page_ == kPgGen) {
                Clip& c = P_->clip[selT_][selS_];
                if (!c.exists()) break;
                if (rt_[selT_].play == selS_ || rt_[selT_].queued == selS_) {
                    // the audio must let go first: stop at once, delete on the next press
                    send(Cmd{Cmd::kStop, uint8_t(selT_), 0, 1});   // b = 1: immediately
                    ctx_->toast("Clip stopped - press Delete again to delete it");
                    break;
                }
                c.lenSteps = 0;
                c.live = 0;
                c.buf[0].clear();
                c.buf[1].clear();
                ctx_->toast("Clip deleted");
                dirty_ = true;
            } else if (page_ == kPgRoll) {
                Clip& c = P_->clip[selT_][selS_];
                const int i = c.notes().at(rollTick_, rollPitch_);
                if (i >= 0) {
                    undoSave(selT_, selS_);
                    NoteBuf& b = c.beginEdit();
                    b.remove(i);
                    c.commit();
                }
            } else if (page_ == kPgSong && songCur_ < P_->songLen) {
                fkey(1, false);
            }
            break;
        case btn::Insert:
            if (page_ == kPgRoll) encoder(0, false);
            else if (page_ == kPgSong) fkey(0, false);
            break;
        default: break;
    }
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
            const int pages = c.exists() ? (c.lenSteps + 15) / 16 : 1;
            if (dx) stepPage_ = clampi(stepPage_ + dx, 0, pages - 1);
            if (dy) {
                if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3), 4);
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
            Engine* en = engine(selT_);
            if (dx) {
                sndSec_ = clampi(sndSec_ + dx, 0, en->sections() - 1);
                sndRow_ = 0;
            }
            if (dy) {
                uint8_t ids[96];
                const int n = en->sectionParams(sndSec_, ids, 96);
                sndRow_ = clampi(sndRow_ + dy, -1, n - 1);   // -1 = the engine row
            }
            break;
        }
        case kPgMixer:
            if (mixFx_) fxRow_ = clampi(fxRow_ + dy, 0, 12);
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
            if (nameCur_ >= 0 && confirmKey_ == 99) {   // rename
                nameCur_ = clampi(nameCur_ + dx, 0, 11);
                break;
            }
            if (projTab_ == 0) {
                trkRow_ = clampi(trkRow_ + dy, 0, kTracks - 1);
                trkCol_ = clampi(trkCol_ + dx, 0, 3);
                selT_ = trkRow_;
            } else {
                listSel_ = clampi(listSel_ + dy, 0, listN_ ? listN_ - 1 : 0);
            }
            break;
        case kPgSamples:
            listSel_ = clampi(listSel_ + dy + dx * 8, 0, listN_ ? listN_ - 1 : 0);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// encoder (d == 0: the encoder push)
// ---------------------------------------------------------------------------
void Konsol::encoder(int d, bool shift) {
    const bool push = d == 0;
    switch (page_) {
        case kPgSession:
            if (push) {
                if (shift) launchScene(selS_);
                else if (P_->clip[selT_][selS_].exists()) launch(selT_, selS_);
                if (!ctx_->playing()) ctx_->setPlaying(true);
            } else if (shift) {
                ctx_->setBpm(ctx_->bpm() + float(d));
            } else {
                scrollScenes(d);
            }
            break;
        case kPgSteps: {
            Clip& c = P_->clip[selT_][selS_];
            if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3), 4);
            const int target = heldStep_ >= 0 ? heldStep_ : stepCursor_;
            if (push) {
                toggleStep(stepCursor_, 100);
                break;
            }
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
            if (push) {   // insert / remove at the cursor
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
            if (rollFn_ == 0 || i < 0) {   // cursor
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
            if (push) {
                fkey(0, false);
                break;
            }
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
                if (c.gen == kGenBBCut && pi == 0) {   // the procedure changes A / B
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
            Engine* en = engine(selT_);
            if (sndRow_ < 0) {   // the engine row
                if (push) break;
                setEngine(selT_, clampi(P_->track[selT_].engine + d, 0, kEngCount - 1));
                sndSec_ = 0;
                break;
            }
            uint8_t ids[96];
            const int n = en->sectionParams(sndSec_, ids, 96);
            if (sndRow_ >= n) sndRow_ = n - 1;
            if (n <= 0 || push) break;
            en->stepParam(ids[sndRow_], d, shift);
            if (P_->track[selT_].engine == kEngMakam) makamSetKey(en, P_->key);
            dirty_ = true;
            break;
        }
        case kPgMixer: {
            if (mixFx_) {
                FxCfg& f = P_->fx;
                const float k = (shift ? 0.1f : 0.01f) * float(d);
                auto adj = [&](float& v, float lo, float hi) { v = v + k < lo ? lo : (v + k > hi ? hi : v + k); };
                switch (fxRow_) {
                    case 0: f.delaySync = uint8_t(clampi(f.delaySync + d, 0, 5)); break;
                    case 1: adj(f.delayFb, 0.0f, 0.95f); break;
                    case 2: adj(f.delayTone, 0.0f, 1.0f); break;
                    case 3: f.pingPong = d > 0 ? 1 : 0; break;
                    case 4: adj(f.retA, 0.0f, 1.5f); break;
                    case 5: adj(f.revSize, 0.0f, 1.0f); break;
                    case 6: adj(f.revDamp, 0.0f, 1.0f); break;
                    case 7: adj(f.retB, 0.0f, 1.5f); break;
                    case 8: adj(f.compThr, 0.0f, 1.0f); break;
                    case 9: adj(f.compRatio, 0.0f, 1.0f); break;
                    case 10: adj(f.compMakeup, 0.0f, 1.0f); break;
                    case 11: f.limiter = d > 0 ? 1 : 0; break;
                    default: adj(f.master, 0.0f, 1.25f); break;
                }
                fxDirty_ = true;
                dirty_ = true;
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
                case 4: if (!push) m.stem = uint8_t(clampi(m.stem + d, 0, 4)); break;
                default: m.midiOut = push ? uint8_t(!m.midiOut) : uint8_t(d > 0); break;
            }
            if (push && mixRow_ < 4) m.mute ^= 1;
            dirty_ = true;
            break;
        }
        case kPgSong: {
            if (push) {   // preview the entry's scene live
                if (songCur_ < P_->songLen) {
                    launchScene(P_->song[songCur_].scene);
                    if (!ctx_->playing()) ctx_->setPlaying(true);
                }
                break;
            }
            if (songCur_ >= P_->songLen) break;
            SongEntry& en = P_->song[songCur_];
            if (shift) en.bars = uint8_t(clampi(en.bars + d, 1, 64));
            else en.scene = uint8_t(clampi(en.scene + d, 0, kScenes - 1));
            dirty_ = true;
            break;
        }
        case kPgProject: {
            if (confirmKey_ == 99) {   // rename
                if (push) {
                    confirmKey_ = -1;
                    // trim trailing spaces
                    for (int i = 15; i >= 0 && (P_->name[i] == ' ' || P_->name[i] == 0); --i) P_->name[i] = 0;
                    if (!P_->name[0]) std::snprintf(P_->name, sizeof P_->name, "SONG01");
                    ctx_->toast("Renamed - F3 saves");
                    break;
                }
                char& ch = P_->name[nameCur_];
                const int n = int(sizeof kNameChars) - 1;
                int idx = 0;
                while (idx < n && kNameChars[idx] != ch) ++idx;
                idx = ((idx + d) % n + n) % n;
                ch = kNameChars[idx];
                for (int i = 0; i < nameCur_; ++i)
                    if (!P_->name[i] || P_->name[i] == ' ') P_->name[i] = '_';
                break;
            }
            if (projTab_ == 1) {
                if (push) {
                    if (listN_ > 0) {
                        char nm[40];
                        copyStr(nm, sizeof nm, list_[listSel_]);
                        if (loadProject(nm)) ctx_->toast("Project loaded");
                        else ctx_->toast("Cannot load that project");
                    }
                } else {
                    listSel_ = clampi(listSel_ + d, 0, listN_ ? listN_ - 1 : 0);
                }
                break;
            }
            TrackCfg& tc = P_->track[trkRow_];
            if (push) break;
            switch (trkCol_) {
                case 0: setEngine(trkRow_, clampi(tc.engine + d, 0, kEngCount - 1)); break;
                case 1: tc.octave = uint8_t(clampi(tc.octave + d, 0, 7)); break;
                case 2: tc.mix.stem = uint8_t(clampi(tc.mix.stem + d, 0, 4)); break;
                default: tc.mix.midiOut = d > 0 ? 1 : 0; break;
            }
            dirty_ = true;
            break;
        }
        case kPgSamples:
            if (push) fkey(0, false);
            else listSel_ = clampi(listSel_ + d, 0, listN_ ? listN_ - 1 : 0);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// F1..F8 per page
// ---------------------------------------------------------------------------
void Konsol::fkey(int k, bool shift) {
    (void)shift;
    char msg[48] = {};
    switch (page_) {
        case kPgSession: {
            static const uint8_t kPg[8] = {kPgSession, kPgSteps, kPgRoll, kPgGen, kPgSound, kPgMixer, kPgSong, kPgProject};
            if (k == 0) {   // F1 on the session: stop all
                send(Cmd{Cmd::kStopAll, 0, 0, 0});
                break;
            }
            page_ = kPg[k];
            if (page_ == kPgRoll) focusRoll();
            if (page_ == kPgProject && projTab_ == 1) scanProjects();
            break;
        }
        case kPgSteps:
            if (k <= 5) stepLock_ = k;
            else if (k == 6) {   // clip length cycle
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
            } else {   // clear the lane (pitch)
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
            else if (k == 6) {   // quantise every note to the grid
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
            } else {
                undoRestore();
            }
            break;
        }
        case kPgGen: {
            Clip& c = P_->clip[selT_][selS_];
            switch (k) {
                case 0:   // write
                case 1:   // new seed + write
                case 4:   // write + open the roll
                    ensureClip(selT_, selS_, 16);
                    if (c.gen == kGenNotes) {
                        ctx_->toast("Choose a player first (row 1)");
                        break;
                    }
                    if (k == 1) c.gp[kGenParams - 1] = int16_t(1 + (nowMs_ * 7919u + uint32_t(c.gp[5]) * 31u) % 999u);
                    undoSave(selT_, selS_);
                    {
                        const uint8_t wasLive = c.live;
                        if (k == 4) c.live = 0;   // to the roll: a fixed take
                        if (runGenerator(selT_, selS_, 0, msg, sizeof msg)) ctx_->toast(msg[0] ? msg : "Written");
                        else ctx_->toast(msg[0] ? msg : "Generator failed");
                        if (k == 4) {
                            c.gen = kGenNotes;   // the take is now plain notes to edit
                            page_ = kPgRoll;
                            focusRoll();
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
                case 5: undoRestore(); break;
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
                default: page_ = kPgSteps; break;
            }
            dirty_ = true;
            break;
        }
        case kPgSound: {
            Engine* en = engine(selT_);
            switch (k) {
                case 0: sndSec_ = sndSec_ > 0 ? sndSec_ - 1 : en->sections() - 1; sndRow_ = 0; break;
                case 1: sndSec_ = (sndSec_ + 1) % en->sections(); sndRow_ = 0; break;
                case 2: sndRow_ = -1; break;
                case 3: en->reset(); ctx_->toast("Init parameters"); break;
                case 4: en->preset(0); ctx_->toast("KONSOL preset"); break;
                case 5:
                    page_ = kPgSamples;
                    scanSamples();
                    break;
                case 6: keyOct_ = keyOct_ >= 2 ? -2 : keyOct_ + 1; break;
                default:
                    keysOverlay_ = !keysOverlay_;
                    break;
            }
            dirty_ = true;
            break;
        }
        case kPgMixer: {
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
                case 0:   // insert after the cursor (a copy)
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
                case 3:   // append the selected scene
                    if (p.songLen < kSongMax) {
                        p.song[p.songLen] = SongEntry{uint8_t(selS_), 4};
                        songCur_ = p.songLen++;
                    }
                    break;
                case 4: p.songLoop = !p.songLoop; setLoop_ = p.songLoop; break;
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
                default:   // play the song from the cursor entry
                    if (!p.songLen) break;
                    if (ctx_->playing()) {
                        setSongMode(true);   // joins at the next bar line
                    } else {
                        songMode_ = true;
                        songReq_ = false;
                        ctx_->setPlaying(true);   // starts at the cursor entry
                    }
                    break;
            }
            dirty_ = true;
            break;
        }
        case kPgProject:
            switch (k) {
                case 0: projTab_ = 0; break;
                case 1:
                    projTab_ = 1;
                    scanProjects();
                    break;
                case 2:
                    if (!ctx_->fs) ctx_->toast("No SD card");
                    else if (saveProject(P_->name)) ctx_->toast("Saved");
                    else ctx_->toast("Save failed");
                    if (projTab_ == 1) scanProjects();
                    break;
                case 3: {   // save as a new name
                    if (!ctx_->fs) {
                        ctx_->toast("No SD card");
                        break;
                    }
                    char nm[16];
                    char path[96];
                    for (int i = 1; i < 100; ++i) {
                        std::snprintf(nm, sizeof nm, "SONG%02d", i);
                        std::snprintf(path, sizeof path, "%s/Projects/%s.kon", ctx_->home, nm);
                        if (!ctx_->fs->exists(path)) break;
                    }
                    copyStr(P_->name, sizeof P_->name, nm);
                    std::snprintf(msg, sizeof msg, saveProject(nm) ? "Saved as %s" : "Save failed", nm);
                    ctx_->toast(msg);
                    if (projTab_ == 1) scanProjects();
                    break;
                }
                case 4:
                case 5:
                    if (confirmKey_ != k) {
                        confirmKey_ = k;
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
                    ctx_->toast(k == 4 ? "New project" : "Demo loaded");
                    break;
                case 6:
                    confirmKey_ = 99;
                    nameCur_ = 0;
                    ctx_->toast("Rename: Left/Right + encoder, push = done");
                    break;
                default:
                    page_ = kPgSamples;
                    scanSamples();
                    break;
            }
            if (k != 4 && k != 5 && confirmKey_ != 99) confirmKey_ = -1;
            break;
        case kPgSamples: {
            switch (k) {
                case 0: {   // load / open
                    if (listN_ <= 0) break;
                    const char* nm = list_[listSel_];
                    if (nm[0] == '/') {   // folder
                        if (!std::strcmp(nm, "/..")) {
                            char* sl = std::strrchr(listDir_, '/');
                            if (sl && sl != listDir_) *sl = 0;
                            else listDir_[0] = 0;
                        } else {
                            char nd[48];
                            std::snprintf(nd, sizeof nd, "%s%s", listDir_[0] ? listDir_ : "", nm);
                            copyStr(listDir_, sizeof listDir_, nd);
                        }
                        scanSamples();
                        break;
                    }
                    char path[96];
                    std::snprintf(path, sizeof path, "%s/%s", listDir_[0] ? listDir_ + 1 : "", nm);
                    const char* p = path[0] == '/' ? path + 1 : path;
                    send(Cmd{Cmd::kPanic, 0, 0, 0});
                    loadSample(selT_, p);
                    const int eng = P_->track[selT_].engine;
                    if (eng != kEngSampler && eng != kEngGrain) setEngine(selT_, kEngSampler);
                    std::snprintf(msg, sizeof msg, "T%d: %s", selT_ + 1, nm);
                    ctx_->toast(msg);
                    break;
                }
                case 1: {
                    char* sl = std::strrchr(listDir_, '/');
                    if (sl && sl != listDir_) *sl = 0;
                    else listDir_[0] = 0;
                    scanSamples();
                    break;
                }
                case 2:
                    send(Cmd{Cmd::kPanic, 0, 0, 0});
                    buildBreak(smp_[selT_]);
                    ctx_->toast("Built-in break loaded");
                    break;
                case 3:
                    selT_ = (selT_ + 1) % kTracks;
                    break;
                default: page_ = kPgSound; break;
            }
            break;
        }
        default: break;
    }
}

}  // namespace konsol
}  // namespace lab
