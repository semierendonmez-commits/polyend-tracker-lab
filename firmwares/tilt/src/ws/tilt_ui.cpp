// TILT -- input: global keys, the perform pads, every page, the FILE overlay.
//
// Only keys of the original Tracker's key matrix are used (NAV.md): no Aux,
// Help or encoder push. Insert stands in for the push ("confirm / apply"),
// Song nudges the table. LABKIT owns I.Param (PARAM pads, Shift: MACRO),
// Master (MAP, Shift: MOD) and Shift + Config (HELP).
// Registered fields are edited through fieldSet() (the registry, routes and
// MIDI learn stay in step); the matrix weights are written directly.
#include <cstdio>
#include <cstring>

#include "ws/tilt_ws.h"

namespace tilt {

using lab::Event;
namespace btn = lab::btn;

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int padR(int p) { return p / 12; }
inline int padC(int p) { return p % 12; }
// EVOLVE: the items of the page in order (params, meta cells, row locks).
constexpr int kEvParams = 13, kRndParams = 7, kMetaCells = 16, kLockRows = 10;
constexpr int kEvItems = kEvParams + kRndParams + kMetaCells + kLockRows;
}  // namespace

int Tilt::paramListOf(int page, int* out, int cap) const {
    if (page == kPgTable) return paramsOfPage(kPpTable, out, cap);
    if (page == kPgEvolve) {
        int n = paramsOfPage(kPpEvolve, out, cap);
        n += paramsOfPage(kPpRandom, out + n, cap - n);
        return n;
    }
    if (page == kPgMix) return paramsOfPage(kPpFx, out, cap);
    return 0;
}

// ---- events -------------------------------------------------------------------------------------
void Tilt::onEvent(const Event& e) {
    bool used = false;
    eventGlobal(e, used);
    if (!used) {
        if (file_) {
            eventFile(e);
        } else if ((e.type == Event::kPadDown || e.type == Event::kPadUp) && (perform_ || page_ == kPgGraph)) {
            eventPerform(e);
        } else {
            switch (page_) {
                case kPgGraph: eventGraph(e); break;
                case kPgMatrix: eventMatrix(e); break;
                case kPgSteps: eventSteps(e); break;
                case kPgTable:
                case kPgEvolve: eventParams(e); break;
                case kPgCombos: eventCombos(e); break;
                case kPgChannels: eventChannels(e); break;
                case kPgMix: eventMix(e); break;
                default: break;
            }
        }
    }
    placeChannels();
}

void Tilt::eventGlobal(const Event& e, bool& used) {
    if (e.type == Event::kPadDown) {
        padDown_[e.id] = true;
        padVel_[e.id] = e.velocity;
    } else if (e.type == Event::kPadUp) {
        padDown_[e.id] = false;
    }
    if (e.type == Event::kRelease) {
        if (e.id == btn::Shift) shift_ = false;
        if (e.id == btn::Performance) perform_ = false;
        return;
    }
    if (e.type != Event::kPress) return;
    used = true;
    switch (e.id) {
        case btn::Shift: shift_ = true; return;
        case btn::Performance: perform_ = true; return;
        case btn::File:
            file_ = !file_;
            if (file_) scanSlots();
            return;
        case btn::SampleLoader:   // the browser for the selected channel
            page_ = kPgChannels;
            chField_ = 14;
            file_ = false;
            openSampleBrowser(chSel_);
            return;
        case btn::Pattern: post(Cmd::kPage, (pat_->ev.page + 1) % kPages); return;
        case btn::Rec:
            if (eng_->replaying()) post(Cmd::kReplay, 0);
            else post(Cmd::kCapture, capBars_, 1);
            return;
        case btn::Song: post(Cmd::kNudge, (e.shift || shift_) ? 127 : 90); return;   // nudge the table
        case btn::Note: post(Cmd::kFlipper, 0, (e.shift || shift_) ? 127 : 100); return;
        case btn::Instr: post(Cmd::kFlipper, 1, (e.shift || shift_) ? 127 : 100); return;
        case btn::Fx1: post(Cmd::kFlipper, 2, (e.shift || shift_) ? 127 : 100); return;
        case btn::Fx2: post(Cmd::kFlipper, 3, (e.shift || shift_) ? 127 : 100); return;
        default: break;
    }
    if (e.id <= btn::F8 && !file_) {
        if (e.shift || shift_) {
            shiftAction(page_, e.id);
        } else {
            page_ = e.id;
            if (page_ == kPgMix) mixFx_ = false;
        }
        return;
    }
    used = false;
}

void Tilt::eventPerform(const Event& e) {
    if (e.type != Event::kPadDown) return;
    const int r = padR(e.id), c = padC(e.id), v = e.velocity;
    if (r <= 1 && c < 8) {
        post(Cmd::kFlip, r * 8 + c + 1, v);
    } else if (r == 0) {
        post(Cmd::kFlipper, c - 8, v);
    } else if (r == 1) {
        post(Cmd::kPage, c - 8);
    } else if (r == 2 && c < 8) {
        if (e.shift || shift_) fieldSet(&pat_->ch[c].mute, pat_->ch[c].mute ? 0 : 1);
        else auditionCh(c, pat_->ch[c].note, v);
    } else if (r == 2) {
        post(Cmd::kCombo, c - 8);
    } else {
        static const int kBars[4] = {1, 2, 4, 8};
        switch (c) {
            case 0: post(Cmd::kLaunch); break;
            case 1: post(Cmd::kHalt); break;
            case 2: post(Cmd::kBall); break;
            case 3: post(Cmd::kNudge, v); break;
            case 4:
            case 5:
            case 6:
            case 7:
                capBars_ = kBars[c - 4];
                post(Cmd::kCapture, capBars_, 1);
                break;
            case 8: post(Cmd::kReplay, eng_->replaying() ? 0 : 1); break;
            case 9: randomizeNow((e.shift || shift_) ? uint8_t(kRtSteps | kRtLanes) : uint8_t(kRtMatrix)); break;
            case 10: undoRandom(); break;
            case 11: post(Cmd::kTilt); break;
            default: break;
        }
    }
}

void Tilt::shiftAction(int page, int key) {
    switch (page) {
        case kPgGraph:
            switch (key) {
                case 0: post(Cmd::kLaunch); break;
                case 1: post(Cmd::kHalt); break;
                case 2: post(Cmd::kBall); break;
                case 3: post(Cmd::kNudge, 100); break;
                case 4: post(Cmd::kCapture, capBars_, 0); break;
                case 5: post(Cmd::kReplay, eng_->replaying() ? 0 : 1); break;
                case 6: randomizeNow(randTargets(pat_->rnd)); break;
                case 7: undoRandom(); break;
                default: break;
            }
            break;
        case kPgMatrix:
            if (key <= 5) mxField_ = key;
            else if (key == 6) post(Cmd::kPage, (pat_->ev.page + 1) % kPages);
            else mxLive_ = !mxLive_;
            break;
        case kPgSteps:
            switch (key) {
                case 0: randomizeTrigs(*pat_, rng_); break;
                case 1: clearSteps(*pat_); break;
                case 2:
                    if (stRow_ < kStepRows)
                        for (int s = 0; s < kSteps; ++s)
                            if (s != stStep_) {
                                const Step& src = pat_->steps[stStep_];
                                Step& d = pat_->steps[s];
                                switch (stRow_) {
                                    case kRowTrig: d.trig = src.trig; break;
                                    case kRowTrigP: d.trigP = src.trigP; break;
                                    case kRowDelay: d.delay = src.delay; break;
                                    case kRowDelayP: d.delayP = src.delayP; break;
                                    case kRowVel: d.vel = src.vel; break;
                                    case kRowLen: d.len = src.len; break;
                                    case kRowBumper: d.bumper = src.bumper; break;
                                    case kRowRamp: d.ramp = src.ramp; break;
                                    case kRowDegree: d.degree = src.degree; break;
                                    default: d.lock = src.lock; break;
                                }
                            }
                    break;
                case 3: fieldSet(&pat_->steps[stStep_].lock, pat_->steps[stStep_].lock ? 0 : 1); break;
                case 4:
                case 5: {
                    Step tmp[kSteps];
                    std::memcpy(tmp, pat_->steps, sizeof tmp);
                    const int d = key == 4 ? 1 : kSteps - 1;
                    for (int s = 0; s < kSteps; ++s) pat_->steps[s] = tmp[(s + d) % kSteps];
                    break;
                }
                case 6:
                    clipStep_ = pat_->steps[stStep_];
                    haveClipStep_ = true;
                    ctx_->toast("Step copied");
                    break;
                case 7:
                    if (haveClipStep_) pat_->steps[stStep_] = clipStep_;
                    break;
                default: break;
            }
            syncRegistry(true);
            break;
        case kPgTable:
            switch (key) {
                case 0: post(Cmd::kLaunch); break;
                case 1: post(Cmd::kHalt); break;
                case 2: post(Cmd::kBall); break;
                case 3: post(Cmd::kTilt); break;
                case 6: {
                    const Table t;
                    pat_->table = t;
                    for (int i = 0; i < kFlips; ++i) {
                        pat_->flips[i].step = uint8_t(1 + 4 * i);
                        pat_->flips[i].note = uint8_t(36 + i);
                    }
                    syncRegistry(true);
                    ctx_->toast("Table defaults");
                    break;
                }
                default: break;
            }
            break;
        case kPgEvolve:
            switch (key) {
                case 0: randomizeNow(randTargets(pat_->rnd)); ctx_->toast("Randomized"); break;
                case 1: undoRandom(); ctx_->toast("Undo"); break;
                case 2: randomizeNow(kRtMatrix); break;
                case 3: randomizeNow(kRtSteps); break;
                case 4: randomizeNow(kRtLanes); break;
                case 5: randomizeNow(kRtRules); break;
                case 6: randomizeNow(kRtSound); break;
                case 7: post(Cmd::kResetEvolve); ctx_->toast("Memory + mutation reset"); break;
                default: break;
            }
            break;
        case kPgCombos:
            if (key <= 3) post(Cmd::kCombo, key);
            else if (key == 4) fieldSet(&pat_->combos[cbSel_].path[0], 0);
            break;
        case kPgChannels: {
            static const uint8_t kTypes[6] = {kEnKick, kEnSnare, kEnHat, kEnAcid, kEnFm, kEnSample};
            static Channel clip;
            static bool have = false;
            if (key <= 5) {
                Channel& c = pat_->ch[chSel_];
                engineDefaults(c, kTypes[key]);
                if (kTypes[key] != kEnSample) c.sample[0] = 0;
                refreshSlots(chSel_);
                syncRegistry(true);
                auditionCh(chSel_, c.note, 110);
            } else if (key == 6) {
                clip = pat_->ch[chSel_];
                have = true;
                ctx_->toast("Channel copied");
            } else if (have) {
                pat_->ch[chSel_] = clip;
                sampleDirty_[chSel_] = true;
                refreshSlots(chSel_);
                syncRegistry(true);
            }
            break;
        }
        case kPgMix:
            if (key == 0) fieldSet(&pat_->ch[mixCh_].mute, pat_->ch[mixCh_].mute ? 0 : 1);
            else if (key == 1) fieldSet(&pat_->ch[mixCh_].solo, pat_->ch[mixCh_].solo ? 0 : 1);
            else if (key == 6) mixFx_ = !mixFx_;
            else if (key == 7) post(Cmd::kPanic);
            break;
        default: break;
    }
}

// ---- GRAPH ---------------------------------------------------------------------------------------
void Tilt::eventGraph(const Event& e) {
    if (e.type == Event::kEncoder) {
        const float b = ctx_->bpm() + float(e.delta) * ((e.shift || shift_) ? 0.1f : 1.0f);
        ctx_->setBpm(b);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left: kpSet(0, pat_->table.rate - 1); break;    // Rate is kParams[0]
        case btn::Right: kpSet(0, pat_->table.rate + 1); break;
        case btn::Up: post(Cmd::kPage, (pat_->ev.page + kPages - 1) % kPages); break;
        case btn::Down: post(Cmd::kPage, (pat_->ev.page + 1) % kPages); break;
        case btn::Insert: post(Cmd::kLaunch); break;
        case btn::Delete: post(Cmd::kHalt); break;
        case btn::Copy: post(Cmd::kBall); break;
        default: break;
    }
}

// ---- MATRIX ---------------------------------------------------------------------------------------
void Tilt::editMatrix(int delta) {
    const int pg = pat_->ev.page;
    uint8_t& w = pat_->w[pg][mxRow_][mxCol_];
    Edge& ed = pat_->e[pg][mxRow_][mxCol_];
    switch (mxField_) {
        case 0: w = uint8_t(clampi(w + delta, 0, 100)); break;
        case 1: ed.tr = int8_t(clampi(ed.tr + (delta > 0 ? 1 : -1), -12, 12)); break;
        case 2: ed.dly = uint8_t(clampi(ed.dly + (delta > 0 ? 1 : -1), 0, 11)); break;
        case 3: ed.vel = int8_t(clampi(ed.vel + delta, -64, 63)); break;
        case 4: mxShape_ = (mxShape_ + (delta > 0 ? 1 : kShapeCount - 1)) % kShapeCount; break;
        case 5: mxRowFn_ = (mxRowFn_ + (delta > 0 ? 1 : kRowFnCount - 1)) % kRowFnCount; break;
        default: break;
    }
}

void Tilt::eventMatrix(const Event& e) {
    const int pg = pat_->ev.page;
    const bool sh = e.shift || shift_;
    if (e.type == Event::kEncoder) {
        editMatrix(e.delta * (sh || mxField_ != 0 ? 1 : 5));
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if ((r <= 1 && c < 8) || (r == 0 && c == 8)) {
            const int col = c == 8 ? 0 : r * 8 + c + 1;
            mxCol_ = col;
            mxField_ = mxField_ > 3 ? 0 : mxField_;
            if (mxField_ == 0) {
                uint8_t& w = pat_->w[pg][mxRow_][col];
                w = w ? 0 : uint8_t(clampi(e.velocity * 100 / 127, 10, 100));
            }
        } else if (r >= 2 && c < 8) {
            mxRow_ = (r - 2) * 8 + c + 1;
        } else if (r == 2 && c == 8) {
            mxRow_ = 0;
        } else if (r == 3 && c == 8) {
            applyRowFn(*pat_, pg, mxRow_, kRfClear, rng_);
        } else if (r == 1 && c == 8) {
            applyRowFn(*pat_, pg, mxRow_, kRfEqual, rng_);
        } else if (c == 9) {
            mxField_ = r;
        } else if (c == 10) {
            static const int kSh[4] = {kShChain, kShDrunk, kShJumps, kShRandom};
            applyShape(*pat_, pg, kSh[r], rng_);
            ctx_->toast(kShapeNames[kSh[r]]);
        } else if (c == 11) {
            post(Cmd::kPage, r);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: mxRow_ = (mxRow_ + kStates - 1) % kStates; break;
        case btn::Down: mxRow_ = (mxRow_ + 1) % kStates; break;
        case btn::Left: mxCol_ = (mxCol_ + kStates - 1) % kStates; break;
        case btn::Right: mxCol_ = (mxCol_ + 1) % kStates; break;
        case btn::Insert:   // the push of the original encoder: toggle / apply
            if (sh) {
                pat_->w[pg][mxRow_][mxCol_] = 50;
            } else if (mxField_ == 4) {
                applyShape(*pat_, pg, mxShape_, rng_);
                ctx_->toast(kShapeNames[mxShape_]);
            } else if (mxField_ == 5) {
                applyRowFn(*pat_, pg, mxRow_, mxRowFn_, rng_);
            } else {
                uint8_t& w = pat_->w[pg][mxRow_][mxCol_];
                w = w ? 0 : 100;
            }
            break;
        case btn::Delete:
            if (mxField_ == 0) pat_->w[pg][mxRow_][mxCol_] = 0;
            else pat_->e[pg][mxRow_][mxCol_] = Edge();
            break;
        default: break;
    }
}

// ---- STEPS -----------------------------------------------------------------------------------------
void Tilt::editStep(int s, int row, int d) {
    Step& st = pat_->steps[clampi(s, 0, kSteps - 1)];
    switch (row) {
        case kRowTrig: fieldSet(&st.trig, clampi(fieldGet(&st.trig) + d, 0, 9)); break;
        case kRowTrigP: fieldSet(&st.trigP, clampi(fieldGet(&st.trigP) + d * 5, 0, 100)); break;
        case kRowDelay: fieldSet(&st.delay, clampi(fieldGet(&st.delay) + d, 0, 23)); break;
        case kRowDelayP: fieldSet(&st.delayP, clampi(fieldGet(&st.delayP) + d * 5, 0, 100)); break;
        case kRowVel: fieldSet(&st.vel, clampi(fieldGet(&st.vel) + d * 4, 1, 127)); break;
        case kRowLen: fieldSet(&st.len, clampi(fieldGet(&st.len) + d, 1, 64)); break;
        case kRowBumper: fieldSet(&st.bumper, clampi(fieldGet(&st.bumper) + d, 0, 4)); break;
        case kRowRamp: fieldSet(&st.ramp, clampi(fieldGet(&st.ramp) + d, 0, 16)); break;
        case kRowDegree: fieldSet(&st.degree, clampi(fieldGet(&st.degree) + d, -7, 21)); break;
        case kRowLock: fieldSet(&st.lock, d > 0 ? 1 : 0); break;
        default: break;
    }
}

void Tilt::eventSteps(const Event& e) {
    const bool sh = e.shift || shift_;
    if (e.type == Event::kEncoder) {
        if (stRow_ == kStepRows) {   // the melody strip
            int ids[8];
            const int n = paramsOfPage(kPpMelody, ids, 8);
            const int id = ids[clampi(melField_, 0, n - 1)];
            kpSet(id, paramGet(*pat_, id) + e.delta);
            return;
        }
        bool any = false;
        for (int s = 0; s < kSteps; ++s) {
            const int pad = (s / 8) * 12 + (s % 8);
            if (padDown_[pad]) {
                editStep(s, stRow_, e.delta * (sh ? 4 : 1));
                any = true;
            }
        }
        if (!any) editStep(stStep_, stRow_, e.delta * (sh ? 4 : 1));
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if (r <= 1 && c < 8) {
            stStep_ = r * 8 + c;
        } else if (r == 2 && c <= 9) {
            const int trig = c < 8 ? c + 1 : (c == 8 ? 0 : kTrigMute);
            bool any = false;
            for (int s = 0; s < kSteps; ++s)
                if (padDown_[(s / 8) * 12 + (s % 8)]) {
                    fieldSet(&pat_->steps[s].trig, trig);
                    any = true;
                }
            if (!any) fieldSet(&pat_->steps[stStep_].trig, trig);
            if (trig >= 1 && trig <= 8) auditionCh(trig - 1, pat_->ch[trig - 1].note, 100);
        } else if (r == 3 && c <= 9) {
            stRow_ = c;
        } else if (r == 3 && c == 10) {
            stRow_ = kStepRows;
        } else if (r == 3 && c == 11) {
            const Step& st = pat_->steps[stStep_];
            if (st.trig >= 1 && st.trig <= 8) auditionCh(st.trig - 1, pat_->ch[st.trig - 1].note, st.vel);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left:
            if (stRow_ == kStepRows) melField_ = clampi(melField_ - 1, 0, 3);
            else stStep_ = (stStep_ + kSteps - 1) % kSteps;
            break;
        case btn::Right:
            if (stRow_ == kStepRows) melField_ = clampi(melField_ + 1, 0, 3);
            else stStep_ = (stStep_ + 1) % kSteps;
            break;
        case btn::Up: stRow_ = clampi(stRow_ - 1, 0, kStepRows); break;
        case btn::Down: stRow_ = clampi(stRow_ + 1, 0, kStepRows); break;
        case btn::Insert:   // the encoder push: toggle / audition
            if (stRow_ == kRowLock) {
                fieldSet(&pat_->steps[stStep_].lock, pat_->steps[stStep_].lock ? 0 : 1);
            } else if (stRow_ == kStepRows) {
                int ids[8];
                paramsOfPage(kPpMelody, ids, 8);
                kpSet(ids[0], pat_->mel.on ? 0 : 1);
            } else {
                const Step& st = pat_->steps[stStep_];
                if (st.trig >= 1 && st.trig <= 8) auditionCh(st.trig - 1, pat_->ch[st.trig - 1].note, st.vel);
            }
            break;
        case btn::Delete: {
            const Step d;
            Step& st = pat_->steps[stStep_];
            switch (stRow_) {
                case kRowTrig: fieldSet(&st.trig, 0); break;
                case kRowTrigP: fieldSet(&st.trigP, d.trigP); break;
                case kRowDelay: fieldSet(&st.delay, 0); break;
                case kRowDelayP: fieldSet(&st.delayP, d.delayP); break;
                case kRowVel: fieldSet(&st.vel, d.vel); break;
                case kRowLen: fieldSet(&st.len, d.len); break;
                case kRowBumper: fieldSet(&st.bumper, 0); break;
                case kRowRamp: fieldSet(&st.ramp, 0); break;
                case kRowDegree: fieldSet(&st.degree, 0); break;
                case kRowLock: fieldSet(&st.lock, 0); break;
                default: break;
            }
            break;
        }
        case btn::Copy:
            if (sh) {
                if (haveClipStep_) {
                    pat_->steps[stStep_] = clipStep_;
                    syncRegistry(true);
                    ctx_->toast("Step pasted");
                }
            } else {
                clipStep_ = pat_->steps[stStep_];
                haveClipStep_ = true;
                ctx_->toast("Step copied (Shift + Copy pastes)");
            }
            break;
        default: break;
    }
}

// ---- TABLE / EVOLVE (parameter lists) --------------------------------------------------------------
void Tilt::eventParams(const Event& e) {
    int ids[48];
    const int n = paramListOf(page_, ids, 48);
    const bool ev = page_ == kPgEvolve;
    const int count = ev ? kEvItems : n;
    int& f = parFocus_[ev ? 1 : 0];
    f = clampi(f, 0, count - 1);
    const bool sh = e.shift || shift_;
    auto editItem = [&](int item, int d) {
        if (item < n) {
            const ParamDef& pd = kParams[ids[item]];
            const int span = pd.max - pd.min;
            const int step = sh && span > 20 ? 10 : 1;
            kpSet(ids[item], paramGet(*pat_, ids[item]) + d * step);
        } else if (item < n + kMetaCells) {
            const int k = item - n;
            uint8_t& m = pat_->ev.meta[k / 4][k % 4];
            fieldSet(&m, clampi(fieldGet(&m) + d * (sh ? 1 : 10), 0, 100));
        } else {
            const int row = item - n - kMetaCells;
            if (d > 0) pat_->rnd.rowLocks = uint16_t(pat_->rnd.rowLocks | (1u << row));
            else pat_->rnd.rowLocks = uint16_t(pat_->rnd.rowLocks & ~(1u << row));
        }
    };
    if (e.type == Event::kEncoder) {
        // hold a parameter pad + turn: edit that one
        for (int p = 0; p < 24; ++p)
            if (padDown_[p] && p < count && !ev) {
                editItem(p, e.delta);
                return;
            }
        editItem(f, e.delta);
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if (!ev) {
            if (r <= 1) {
                if (e.id < count) f = e.id;
            } else if (r == 3 && c < 4) {
                post(Cmd::kFlipper, c, e.velocity);
            } else if (r == 3) {
                switch (c) {
                    case 4: post(Cmd::kNudge, e.velocity); break;
                    case 5: post(Cmd::kLaunch); break;
                    case 6: post(Cmd::kHalt); break;
                    case 7: post(Cmd::kBall); break;
                    case 8: post(Cmd::kTilt); break;
                    default: break;
                }
            }
            return;
        }
        // EVOLVE: step locks, row locks, randomize actions
        if (r <= 1 && c < 8) {
            Step& st = pat_->steps[r * 8 + c];
            fieldSet(&st.lock, st.lock ? 0 : 1);
        } else if (r == 2 && c < kLockRows) {
            pat_->rnd.rowLocks = uint16_t(pat_->rnd.rowLocks ^ (1u << c));
        } else if (r == 3) {
            switch (c) {
                case 0: randomizeNow(randTargets(pat_->rnd)); break;
                case 1: undoRandom(); break;
                case 2: randomizeNow(kRtMatrix); break;
                case 3: randomizeNow(kRtSteps); break;
                case 4: randomizeNow(kRtLanes); break;
                case 5: randomizeNow(kRtRules); break;
                case 6: randomizeNow(kRtSound); break;
                case 7: post(Cmd::kResetEvolve); break;
                case 8:
                case 9:
                case 10:
                case 11: post(Cmd::kPage, c - 8); break;
                default: break;
            }
        } else if (r <= 1 && c >= 8) {
            f = n + (r * 4 + (c - 8));   // meta cells (rows A-B; C-D with the cursor)
        }
        return;
    }
    if (e.type != Event::kPress) return;
    // columns: TABLE by group; EVOLVE: [memory, mutation, rules] [meta + grid] [randomizer + locks]
    auto colOf = [&](int item) -> int {
        if (ev) {
            if (item < 6 || (item >= 9 && item < 13)) return 0;
            if ((item >= 6 && item < 9) || (item >= n && item < n + kMetaCells)) return 1;
            return 2;
        }
        const char* g = kParams[ids[item]].group;
        if (!std::strcmp(g, "CLOCK") || !std::strcmp(g, "TILT")) return 0;
        if (!std::strcmp(g, "FLIPPERS")) return 1;
        return 2;
    };
    switch (e.id) {
        case btn::Up:
        case btn::Down: {
            const int col = colOf(f);
            const int dir = e.id == btn::Up ? -1 : 1;
            for (int k = f + dir; k >= 0 && k < count; k += dir)
                if (colOf(k) == col) {
                    f = k;
                    break;
                }
            break;
        }
        case btn::Left:
        case btn::Right: {
            const int col = colOf(f) + (e.id == btn::Left ? -1 : 1);
            for (int k = 0; k < count; ++k)
                if (colOf(k) == col) {
                    f = k;
                    break;
                }
            break;
        }
        case btn::Insert:   // toggle an on / off row or a lock
            if (f < n && kParams[ids[f]].fmt == kFmtOnOff) kpSet(ids[f], !paramGet(*pat_, ids[f]));
            else if (f >= n + kMetaCells) pat_->rnd.rowLocks = uint16_t(pat_->rnd.rowLocks ^ (1u << (f - n - kMetaCells)));
            break;
        case btn::Delete:
            if (f < n) kpSet(ids[f], kParams[ids[f]].def);
            else if (f < n + kMetaCells) fieldSet(&pat_->ev.meta[(f - n) / 4][(f - n) % 4], 0);
            break;
        default: break;
    }
}

// ---- COMBOS ------------------------------------------------------------------------------------------
void Tilt::editCombo(int d) {
    Combo& c = pat_->combos[cbSel_];
    switch (cbField_) {
        case 0: fieldSet(&c.on, d > 0 ? 1 : 0); break;
        case 1:
        case 2:
        case 3:
        case 4: {
            const int k = cbField_ - 1;
            if (k >= c.len) {   // grow the path
                if (k == c.len && d > 0) fieldSet(&c.path[k], 1);
            } else {
                fieldSet(&c.path[k], clampi(c.path[k] + d, 0, 16));   // below 1: the path ends here
            }
            break;
        }
        case 5: fieldSet(&c.action, (c.action + (d > 0 ? 1 : kComboActions - 1)) % kComboActions); break;
        case 6: fieldSet(&c.a, clampi(c.a + d, 0, 127)); break;
        case 7: fieldSet(&c.b, clampi(c.b + d, 0, 127)); break;
        default: break;
    }
}

void Tilt::eventCombos(const Event& e) {
    if (e.type == Event::kEncoder) {
        editCombo(e.delta);
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        Combo& cb = pat_->combos[cbSel_];
        if (r <= 1 && c < 8) {
            const int s = r * 8 + c + 1;
            if (cb.len >= kComboLen) fieldSet(&cb.path[0], 0);
            fieldSet(&cb.path[cb.len], s);
            cbField_ = cb.len;
        } else if (r == 2 && c < 4) {
            cbSel_ = c;
        } else if (r == 2 && c >= 4 && c < 4 + kComboActions) {
            fieldSet(&cb.action, c - 4);
            cbField_ = 5;
        } else if (r == 3 && c < 4) {
            post(Cmd::kCombo, c);
        } else if (r == 3 && c == 11) {
            fieldSet(&cb.path[0], 0);
        } else if (r == 3 && c == 10) {
            fieldSet(&cb.on, cb.on ? 0 : 1);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: cbSel_ = (cbSel_ + kCombos - 1) % kCombos; break;
        case btn::Down: cbSel_ = (cbSel_ + 1) % kCombos; break;
        case btn::Left: cbField_ = clampi(cbField_ - 1, 0, 7); break;
        case btn::Right: cbField_ = clampi(cbField_ + 1, 0, 7); break;
        case btn::Insert: post(Cmd::kCombo, cbSel_); break;
        case btn::Delete: fieldSet(&pat_->combos[cbSel_].path[0], 0); break;
        default: break;
    }
}

// ---- CHANNELS -----------------------------------------------------------------------------------------
void Tilt::editChannel(int field, int d) {
    Channel& c = pat_->ch[chSel_];
    if (field == 0) {
        const int t = (c.type + (d > 0 ? 1 : kEngines - 1)) % kEngines;
        engineDefaults(c, t);
        if (t != kEnSample) c.sample[0] = 0;
        refreshSlots(chSel_);
        syncRegistry(true);
        return;
    }
    if (field >= 1 && field <= 8) {
        const EngParam& ep = kEngineParams[c.type][field - 1];
        if (!ep.name) return;
        const int span = ep.max - ep.min;
        const int step = (shift_ && span > 20) ? 8 : 1;
        fieldSet(&c.p[field - 1], clampi(fieldGet(&c.p[field - 1]) + d * step, ep.min, ep.max));
        return;
    }
    int ids[8];
    const int n = paramsOfPage(kPpChannel, ids, 8);
    if (field >= 9 && field < 9 + n) kpSet(ids[field - 9], paramGet(*pat_, ids[field - 9], chSel_) + d * (shift_ ? 12 : 1), chSel_);
}

void Tilt::eventChannels(const Event& e) {
    if (e.type == Event::kEncoder) {
        editChannel(chField_, e.delta);
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if (r == 3 && c < 8) {
            chSel_ = c;
        } else if (r == 3) {
            static const uint8_t kTypes[4] = {kEnKick, kEnSnare, kEnHat, kEnAcid};
            engineDefaults(pat_->ch[chSel_], kTypes[c - 8]);
            pat_->ch[chSel_].sample[0] = 0;
            refreshSlots(chSel_);
            syncRegistry(true);
        } else {
            const Channel& ch = pat_->ch[chSel_];
            const int idx = (2 - r) * 12 + c;
            int note = ch.note + idx;
            if (pat_->mel.on && ch.follow) note = ch.note + pat_->mel.root + scaleSemis(pat_->mel.scale, idx);
            auditionCh(chSel_, clampi(note, 0, 127), e.velocity);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    const int lastField = pat_->ch[chSel_].type == kEnSample ? 14 : 13;
    switch (e.id) {
        case btn::Up:
            do chField_ = (chField_ + lastField) % (lastField + 1);
            while (chField_ >= 1 && chField_ <= 8 && !kEngineParams[pat_->ch[chSel_].type][chField_ - 1].name);
            break;
        case btn::Down:
            do chField_ = (chField_ + 1) % (lastField + 1);
            while (chField_ >= 1 && chField_ <= 8 && !kEngineParams[pat_->ch[chSel_].type][chField_ - 1].name);
            break;
        case btn::Left: chSel_ = (chSel_ + kCh - 1) % kCh; break;
        case btn::Right: chSel_ = (chSel_ + 1) % kCh; break;
        case btn::Insert:   // on SAMPLE: the browser; elsewhere: audition
            if (chField_ == 14) openSampleBrowser(chSel_);
            else auditionCh(chSel_, pat_->ch[chSel_].note, 110);
            break;
        case btn::Delete:
            if (chField_ == 14) {
                pat_->ch[chSel_].sample[0] = 0;
                ctx_->toast("Built-in sample");
            } else if (chField_ >= 1 && chField_ <= 8) {
                Channel& c = pat_->ch[chSel_];
                fieldSet(&c.p[chField_ - 1], kEngineParams[c.type][chField_ - 1].def);
            }
            break;
        default: break;
    }
    if (pat_->ch[chSel_].type != kEnSample && chField_ > 13) chField_ = 13;
}

// ---- MIX ------------------------------------------------------------------------------------------------
void Tilt::editMix(int d) {
    if (mixFx_) {
        int ids[16];
        const int n = paramsOfPage(kPpFx, ids, 16);
        const int f = clampi(parFocus_[2], 0, n - 1);
        const ParamDef& pd = kParams[ids[f]];
        kpSet(ids[f], paramGet(*pat_, ids[f]) + d * ((shift_ && pd.max - pd.min > 20) ? 10 : 1));
        return;
    }
    int ids[16];
    const int n = paramsOfPage(kPpMix, ids, 16);
    const int id = ids[clampi(mixRow_, 0, n - 1)];
    const ParamDef& pd = kParams[id];
    const int step = (pd.max - pd.min > 20) ? (shift_ ? 1 : 4) : 1;
    kpSet(id, paramGet(*pat_, id, mixCh_) + d * step, mixCh_);
}

void Tilt::eventMix(const Event& e) {
    if (e.type == Event::kEncoder) {
        editMix(e.delta);
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if (c < 8) {
            Channel& ch = pat_->ch[c];
            if (r == 0) fieldSet(&ch.mute, ch.mute ? 0 : 1);
            else if (r == 1) fieldSet(&ch.solo, ch.solo ? 0 : 1);
            else if (r == 2) {
                mixCh_ = c;
                mixFx_ = false;
            } else auditionCh(c, ch.note, e.velocity);
        } else {
            // cols 9-12: the FX parameters 1-9 (rows 1-3), tap = focus
            const int f = r * 4 + (c - 8);
            if (f < 9) {
                mixFx_ = true;
                parFocus_[2] = f;
            }
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left:
            if (mixFx_) mixFx_ = false;
            else mixCh_ = clampi(mixCh_ - 1, 0, kCh - 1);
            break;
        case btn::Right:
            if (mixCh_ == kCh - 1 && !mixFx_) mixFx_ = true;
            else if (!mixFx_) mixCh_ = clampi(mixCh_ + 1, 0, kCh - 1);
            break;
        case btn::Up:
            if (mixFx_) parFocus_[2] = clampi(parFocus_[2] - 1, 0, 8);
            else mixRow_ = clampi(mixRow_ - 1, 0, 7);
            break;
        case btn::Down:
            if (mixFx_) parFocus_[2] = clampi(parFocus_[2] + 1, 0, 8);
            else mixRow_ = clampi(mixRow_ + 1, 0, 7);
            break;
        case btn::Insert:
            if (!mixFx_) fieldSet(&pat_->ch[mixCh_].mute, pat_->ch[mixCh_].mute ? 0 : 1);
            break;
        default: break;
    }
}

// ---- FILE overlay -----------------------------------------------------------------------------------------
// Pads: rows 1-2 cols 1-8 = factory 1-16, rows 1-2 cols 9-12 = kits 1-8,
// rows 3-4 cols 1-8 = user slots 1-16 (Shift + pad saves), row 3 cols 9-12 =
// JSON load, JSON save, MIDI file, replay; row 4 cols 9-12 = capture 1/2/4/8.
void Tilt::eventFile(const Event& e) {
    static const int kCounts[4] = {kFactoryCount, kUserSlots, kKitSlots, 4};
    const bool sh = e.shift || shift_;
    int& sel = fileSel_[fileCol_ < 3 ? fileCol_ : 0];
    if (e.type == Event::kEncoder) {
        if (fileCol_ == 3) {
            static const int kBars[4] = {1, 2, 4, 8};
            int k = 0;
            for (int i = 0; i < 4; ++i)
                if (kBars[i] == capBars_) k = i;
            capBars_ = kBars[clampi(k + e.delta, 0, 3)];
        } else {
            sel = clampi(sel + e.delta, 0, kCounts[fileCol_] - 1);
        }
        return;
    }
    if (e.type == Event::kPadDown) {
        const int r = padR(e.id), c = padC(e.id);
        if (r <= 1 && c < 8) {
            const int i = r * 8 + c;
            if (i < kFactoryCount) {
                loadFactory(i);
                fileSel_[0] = i;
                ctx_->toast(kFactoryNames[i]);
            }
        } else if (r <= 1) {
            const int k = r * 4 + (c - 8);
            if (sh) ctx_->toast(saveKit(k) ? "Kit saved" : "No card");
            else ctx_->toast(loadKit(k) ? "Kit loaded" : "Empty kit slot");
        } else if (c < 8) {
            const int slot = (r - 2) * 8 + c;
            if (sh) ctx_->toast(savePattern(slot) ? "Pattern saved" : "No card");
            else ctx_->toast(loadPattern(slot) ? "Pattern loaded" : "Empty slot");
        } else if (r == 2) {
            switch (c) {
                case 8: openJsonBrowser(); break;
                case 9: openJsonSave(); break;
                case 10:
                    if (exportMidi(lastMidi_, sizeof lastMidi_)) ctx_->toast("MIDI file saved");
                    else ctx_->toast(ctx_->fs ? "Capture first" : "No card");
                    break;
                default: post(Cmd::kReplay, eng_->replaying() ? 0 : 1); break;
            }
        } else {
            static const int kBars[4] = {1, 2, 4, 8};
            capBars_ = kBars[c - 8];
            post(Cmd::kCapture, capBars_, 1);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: sel = clampi(sel - 1, 0, kCounts[fileCol_] - 1); break;
        case btn::Down: sel = clampi(sel + 1, 0, kCounts[fileCol_] - 1); break;
        case btn::Left: fileCol_ = clampi(fileCol_ - 1, 0, 3); break;
        case btn::Right: fileCol_ = clampi(fileCol_ + 1, 0, 3); break;
        case btn::F1:
        case btn::Insert:
            if (fileCol_ == 0) {
                loadFactory(fileSel_[0]);
                ctx_->toast(kFactoryNames[fileSel_[0]]);
            } else if (fileCol_ == 1) {
                ctx_->toast(loadPattern(fileSel_[1]) ? "Pattern loaded" : "Empty slot");
            } else if (fileCol_ == 2) {
                ctx_->toast(loadKit(fileSel_[2]) ? "Kit loaded" : "Empty kit slot");
            } else {
                post(Cmd::kCapture, capBars_, 1);
            }
            break;
        case btn::F2: {
            const int slot = fileCol_ == 1 ? fileSel_[1] : (activeSlot_ >= 0 ? activeSlot_ : 0);
            ctx_->toast(savePattern(slot) ? "Pattern saved" : "No card");
            break;
        }
        case btn::F3:
            if (sh) ctx_->toast(loadKit(fileSel_[2]) ? "Kit loaded" : "Empty kit slot");
            else openJsonBrowser();
            break;
        case btn::F4:
            if (sh) ctx_->toast(saveKit(fileSel_[2]) ? "Kit saved" : "No card");
            else openJsonSave();
            break;
        case btn::F5: post(Cmd::kCapture, capBars_, 0); ctx_->toast("Captured"); break;
        case btn::F6: post(Cmd::kReplay, eng_->replaying() ? 0 : 1); break;
        case btn::F7:
            if (exportMidi(lastMidi_, sizeof lastMidi_)) ctx_->toast("MIDI file saved");
            else ctx_->toast(ctx_->fs ? "Capture first" : "No card");
            break;
        case btn::F8: file_ = false; break;
        default: break;
    }
}

}  // namespace tilt
