// GRAF -- keys, pads and encoder. Only keys that exist on the original
// Tracker are used (no Aux, Help or encoder push); Insert confirms / adds.
// LABKIT owns I.Param, Shift + I.Param, Master, Shift + Master, Shift + Config.
//
// Everywhere:
//   F1..F7 pages, F8 the page's action        Pattern undo, Shift + Pattern redo
//   Copy duplicate the module                  FX1 randomize (Shift: defaults)
//   FX2 scope the next output (Shift: mark it as a LABKIT source TAP)
//   Note / Instr: select upstream / downstream (follow the signal; Shift: next branch)
//   Song: song on / off                        Perform: the manual gate (MGAT modules)
//   S.Load: sample browser for a sampler       Rec: the module's action (Shift + Rec: recorder)
#include <cstdio>
#include <cstring>

#include "lab/browser.h"
#include "ws/graf_ws.h"

namespace graf {

namespace {
// Best jack pair for a new cable src -> dst: same signal kind first, CV into
// pitch next, else the first free ones.
void bestJacks(const Engine& e, int src, int dst, int* outJ, int* inJ) {
    const ModDef& ds = def(e.mod(src).type);
    const ModDef& dd = def(e.mod(dst).type);
    *outJ = 0;
    *inJ = -1;
    auto freeIn = [&](int i) {
        for (int c = 0; c < kMaxCables; ++c)
            if (e.cable(c).used && e.cable(c).dst == dst && e.cable(c).dstIn == i) return false;
        return true;
    };
    for (int pass = 0; pass < 3 && *inJ < 0; ++pass)
        for (int o = 0; o < ds.nOut && *inJ < 0; ++o)
            for (int i = 0; i < dd.nIn; ++i) {
                const bool kind = pass == 0 ? dd.in[i].sig == ds.out[o].sig && freeIn(i)
                                            : pass == 1 ? dd.in[i].sig == ds.out[o].sig
                                                        : (ds.out[o].sig == kCv && dd.in[i].sig == kPitch);
                if (kind) {
                    *outJ = o;
                    *inJ = i;
                    break;
                }
            }
    if (*inJ < 0) *inJ = 0;
}
}  // namespace

void Graf::onEvent(const Event& e) {
    now_ = e.ms ? e.ms : now_;
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        const bool down = e.type == Event::kPadDown;
        held_[e.id] = down;
        if (down) heldMs_[e.id] = e.ms;
        if (!down && heldNote_[e.id] >= 0) {
            keyPad(e.id, false, 0);
            return;
        }
        if (down && keyNote(e.id) >= 0 && !(page_ == kPgModule && lab::padRow(e.id) == 0)) {
            keyPad(e.id, true, e.velocity);
            return;
        }
    }
    if (globalKey(e)) return;
    switch (page_) {
        case kPgGraph: evGraph(e); break;
        case kPgMatrix: evMatrix(e); break;
        case kPgModule: evModule(e); break;
        case kPgAdd: evAdd(e); break;
        case kPgSnap: evSnap(e); break;
        case kPgPlay: evPlay(e); break;
        case kPgFile: evFile(e); break;
        default: break;
    }
}

bool Graf::globalKey(const Event& e) {
    using namespace lab;
    if (e.id == btn::Performance && (e.type == Event::kPress || e.type == Event::kRelease)) {
        eng_.manualGate = e.type == Event::kPress;
        return true;
    }
    if (e.type != Event::kPress) return false;
    if (e.id <= btn::F8) {
        fkey(e.id, e.shift);
        return true;
    }
    switch (e.id) {
        case btn::Pattern:
            if (e.shift ? redo() : undo()) {
                char m[40];
                std::snprintf(m, sizeof m, e.shift ? "Redo (%d more)" : "Undo (%d more)", e.shift ? nRedo_ : nUndo_);
                ctx_->toast(m);
            } else {
                ctx_->toast(e.shift ? "Nothing to redo" : "Nothing to undo");
            }
            return true;
        case btn::Copy:
            if (sel_ >= 0 && eng_.mod(sel_).used) {
                checkpoint(1);
                const int n = eng_.duplicate(sel_);
                if (n < 0) toastErr();
                else {
                    select(n);
                    ctx_->toast("Duplicated (with its input cables)");
                }
                rebuildJacks();
            }
            return true;
        case btn::Fx1:
            if (sel_ >= 0 && eng_.mod(sel_).used) {
                checkpoint(1);
                if (e.shift) {
                    eng_.resetParams(sel_);
                    ctx_->toast("Knobs to defaults");
                } else {
                    eng_.randomize(sel_, eng_.rng);
                    ctx_->toast("Randomized");
                }
            }
            return true;
        case btn::Fx2:
            if (sel_ >= 0 && eng_.mod(sel_).used) {
                const int n = def(eng_.mod(sel_).type).nOut;
                if (!n) return true;
                eng_.scopeSlot = sel_;
                if (e.shift) {
                    const int t = eng_.toggleTap(sel_, eng_.scopeOut % n);
                    char m[48], l[24];
                    jackLabel(l, sizeof l, Jack{uint8_t(sel_), uint8_t(eng_.scopeOut % n)}, true);
                    if (t >= 0) std::snprintf(m, sizeof m, "TAP%d = %s (LABKIT source)", t + 1, l);
                    else if (t == -1) std::snprintf(m, sizeof m, "%s: tap removed", l);
                    else std::snprintf(m, sizeof m, "All 8 taps in use");
                    ctx_->toast(m);
                } else {
                    eng_.scopeOut = (eng_.scopeOut + 1) % n;
                }
            }
            return true;
        case btn::Note:
        case btn::Instr:
            if (e.shift) followIdx_ = 1;
            follow(e.id == btn::Instr);
            return true;
        case btn::Song:
            eng_.songOn = !eng_.songOn;
            ctx_->toast(eng_.songOn ? "Song on (plays with the transport)" : "Song off");
            return true;
        case btn::SampleLoader: openSampleBrowser(); return true;
        case btn::Rec:
            if (sel_ >= 0 && eng_.mod(sel_).used && (page_ == kPgModule || page_ == kPgGraph)) {
                Module& m = eng_.mod(sel_);
                switch (m.type) {
                    case kPinb: pinbLayout(m, eng_.rng.next()); break;
                    case kMarkov: eng_.setParamK(sel_, 7, eng_.rng.uni()); break;
                    case kSeq:
                        checkpoint(1);
                        for (int i = 0; i < 16; ++i) {
                            seqNote(m, i) = int8_t(eng_.rng.range(10) - 2);
                            seqSetGate(m, i, eng_.rng.uni() < 0.7f);
                        }
                        break;
                    case kRete:
                    case kNet:
                    case kRval:
                    case kMg: m.ext[0] = uint8_t(m.ext[0] + 1); break;
                    default: return true;
                }
                ctx_->toast("New");
            }
            return true;
        default: return false;
    }
}

void Graf::fkey(int k, bool shift) {
    if (k < 7) {
        go(k);
        return;
    }
    action(shift);
}

// F8: the page's action.
void Graf::action(bool shift) {
    switch (page_) {
        case kPgAdd: addSelected(shift); return;
        case kPgSnap:
            eng_.storeSnap(snapSel_);
            {
                char m[40];
                std::snprintf(m, sizeof m, "Stored snapshot %d", snapSel_ + 1);
                ctx_->toast(m);
            }
            return;
        case kPgFile: {
            char path[96];
            std::snprintf(path, sizeof path, "%s/Patches/P%02d.grf", ctx_->home, fileSel_ + 1);
            if (!ctx_->fs) {
                ctx_->toast("No card");
                return;
            }
            if (fileExists_[fileSel_] && !(confirm_ && confirmWhat_ == 1)) {
                confirm_ = true;
                confirmWhat_ = 1;
                ctx_->toast("Exists: press F8 again to overwrite");
                return;
            }
            confirm_ = false;
            if (savePatch(path)) {
                ctx_->toast("Saved");
                fileExists_[fileSel_] = true;
            } else {
                ctx_->toast("Save failed");
            }
            return;
        }
        default: {   // next / previous canvas
            patch_ = (patch_ + (shift ? kPatches - 1 : 1)) % kPatches;
            sel_ = -1;
            for (int c = 0; c < kCells; ++c) {
                const int s = eng_.slotAtCell(patch_, c);
                if (s >= 0) {
                    select(s);
                    break;
                }
            }
            cursor_ = sel_ >= 0 ? eng_.mod(sel_).cell : 0;
            mRow_ = mCol_ = 0;
            rebuildJacks();
            pagesDirty_ = true;
            return;
        }
    }
}

void Graf::deleteSelected() {
    if (sel_ < 0 || !eng_.mod(sel_).used) return;
    checkpoint(1);
    char m[40], l[12];
    modLabel(l, sizeof l, sel_);
    std::snprintf(m, sizeof m, "Removed %s (Pattern: undo)", l);
    eng_.remove(sel_);
    sel_ = -1;
    rebuildJacks();
    ctx_->toast(m);
}

void Graf::moveCursor(int dx, int dy) {
    int c = lab::padCol(cursor_) + dx, r = lab::padRow(cursor_) + dy;
    if (c < 0) c = 0;
    if (c > 11) c = 11;
    if (r < 0) r = 0;
    if (r > 3) r = 3;
    cursor_ = lab::padAt(c, r);
}

void Graf::editKnob(int slot, int p, int delta, bool fine) {
    Module& m = eng_.mod(slot);
    const ModDef& d = def(m.type);
    if (p < 0 || p >= d.nParam || !delta) return;
    checkpoint(2, slot * 16 + p);
    const ParamDef& pd = d.p[p];
    if (pd.kind == kInt) {
        float mx = pd.max;
        if (m.type == kSmpl && p == 0) mx = float(eng_.sampleFileCount());
        const float v = clampf(m.v[p] + float(delta), pd.min, mx);
        eng_.setParamK(slot, p, Engine::unmapParam(pd, v));
        if (m.type == kMidi && p == 0) eng_.rebuild();
        if (m.type == kPlts && p == 0 && eng_.lastError()[0] && Engine::unmapParam(pd, v) != m.k[p]) toastErr();
    } else {
        eng_.setParamK(slot, p, m.k[p] + float(delta) * (fine ? 0.002f : 0.01f));
    }
    ctx_->shell->labkit().nav.hud(knobId(slot, p));
}

// ------------------------------------------------------------------ graph
void Graf::patchGesture(int src, int dst) {
    const ModDef& ds = def(eng_.mod(src).type);
    const ModDef& dd = def(eng_.mod(dst).type);
    if (!ds.nOut || !dd.nIn) {
        ctx_->toast(!ds.nOut ? "That module has no outputs" : "That module has no inputs");
        return;
    }
    // cables already between them: the first tap shows one, a second tap removes it
    int existing = -1;
    for (int c = 0; c < kMaxCables && existing < 0; ++c)
        if (eng_.cable(c).used && eng_.cable(c).src == src && eng_.cable(c).dst == dst) existing = c;
    if (existing >= 0) {
        if (gestCable_ >= 0 && eng_.cable(gestCable_).used && eng_.cable(gestCable_).src == src &&
            eng_.cable(gestCable_).dst == dst) {
            checkpoint(1);
            eng_.disconnect(gestCable_);
            gestCable_ = -1;
            ctx_->toast("Cable removed (Pattern: undo)");
            rebuildJacks();
            pagesDirty_ = true;
            return;
        }
        gestCable_ = existing;
        selCable_ = existing;
        eng_.scopeSlot = src;
        eng_.scopeOut = eng_.cable(existing).srcOut;
        return;
    }
    int o, i;
    bestJacks(eng_, src, dst, &o, &i);
    checkpoint(1);
    const float amt = ds.out[o].sig == kCv && dd.in[i].sig != kPitch ? 0.5f : 1.0f;
    const int c = eng_.connect(src, o, dst, i, amt);
    if (c < 0) {
        toastErr();
        return;
    }
    gestCable_ = c;
    selCable_ = c;
    eng_.scopeSlot = src;
    eng_.scopeOut = o;
    rebuildJacks();
    pagesDirty_ = true;
}

// While the source pad is held: Left / Right pick its output, Up / Down the input.
void Graf::cycleGestureJack(int dOut, int dIn) {
    if (gestCable_ < 0 || !eng_.cable(gestCable_).used) return;
    const Cable k = eng_.cable(gestCable_);
    const int no = def(eng_.mod(k.src).type).nOut, ni = def(eng_.mod(k.dst).type).nIn;
    int o = k.srcOut, in = k.dstIn;
    for (int tries = 0; tries < 16; ++tries) {
        o = (o + dOut + no) % no;
        in = (in + dIn + ni) % ni;
        if (eng_.cableFind(k.src, o, k.dst, in) < 0) break;
    }
    if (o == k.srcOut && in == k.dstIn) return;
    checkpoint(3, gestCable_);
    eng_.disconnect(gestCable_);
    gestCable_ = eng_.connect(k.src, o, k.dst, in, k.amount);
    selCable_ = gestCable_;
    eng_.scopeOut = o;
    rebuildJacks();
}

void Graf::evGraph(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        const int cell = e.id;
        const int at = eng_.slotAtCell(patch_, cell);
        const int heldSlot = holdCell_ >= 0 ? eng_.slotAtCell(patch_, holdCell_) : -1;
        if (heldSlot >= 0 && cell != holdCell_) {
            gestUsed_ = true;
            if (at >= 0) {
                patchGesture(heldSlot, at);   // hold A + tap B: a cable A > B
            } else {   // hold A + tap an empty pad: move A there
                checkpoint(1);
                eng_.move(heldSlot, cell);
                holdCell_ = cell;
                cursor_ = cell;
                rebuildJacks();
            }
            return;
        }
        if (e.shift && sel_ >= 0) {   // Shift + pad: move the selected module here
            checkpoint(1);
            eng_.move(sel_, cell);
            cursor_ = cell;
            rebuildJacks();
            return;
        }
        if (at >= 0) {
            select(at);
            holdCell_ = cell;
            gestUsed_ = false;
            gestCable_ = -1;
            emptyTap_ = -1;
        } else {
            if (emptyTap_ == cell) {   // second tap on the same empty cell: add here
                emptyTap_ = -1;
                go(kPgAdd);
                addCell_ = cell;
                return;
            }
            emptyTap_ = cell;
            cursor_ = cell;
            sel_ = -1;
            pagesDirty_ = true;
        }
        return;
    }
    if (e.type == Event::kPadUp) {
        if (e.id == holdCell_) holdCell_ = -1;
        return;
    }
    if (e.type == Event::kEncoder) {
        if (holdCell_ >= 0 && gestCable_ >= 0 && eng_.cable(gestCable_).used) {   // the new cable's amount
            checkpoint(3, gestCable_);
            Cable& k = eng_.cable(gestCable_);
            k.amount = clampf(k.amount + float(e.delta) * (e.shift ? 0.005f : 0.05f), -2.0f, 2.0f);
            if (fabsf(k.amount) < 0.004f) k.amount = 0.0f;
            return;
        }
        // step the selection through the canvas in signal order
        int order[kCells];
        const int n = eng_.signalOrder(patch_, order, kCells);
        if (!n) return;
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (order[i] == sel_) at = i;
        at = (at + (e.delta > 0 ? 1 : n - 1)) % n;
        select(order[at]);
        return;
    }
    if (e.type != Event::kPress) return;
    if (holdCell_ >= 0 && gestCable_ >= 0) {   // jack cycling while the source is held
        if (e.id == btn::Left || e.id == btn::Right) {
            cycleGestureJack(e.id == btn::Right ? 1 : -1, 0);
            return;
        }
        if (e.id == btn::Up || e.id == btn::Down) {
            cycleGestureJack(0, e.id == btn::Down ? 1 : -1);
            return;
        }
        if (e.id == btn::Delete) {
            checkpoint(1);
            eng_.disconnect(gestCable_);
            gestCable_ = -1;
            rebuildJacks();
            return;
        }
    }
    switch (e.id) {
        case btn::Left:
        case btn::Right:
        case btn::Up:
        case btn::Down: {
            const int dx = e.id == btn::Left ? -1 : (e.id == btn::Right ? 1 : 0);
            const int dy = e.id == btn::Up ? -1 : (e.id == btn::Down ? 1 : 0);
            if (e.shift && sel_ >= 0) {
                checkpoint(1);
                moveCursor(dx, dy);
                eng_.move(sel_, cursor_);
                rebuildJacks();
            } else {
                moveCursor(dx, dy);
                const int s = eng_.slotAtCell(patch_, cursor_);
                if (s >= 0) select(s);
                else {
                    sel_ = -1;
                    pagesDirty_ = true;
                }
            }
            break;
        }
        case btn::Insert:
            if (e.shift) {
                tidy(patch_);
                ctx_->toast("Canvas tidied (Pattern: undo)");
            } else if (sel_ >= 0) {
                go(kPgModule);
            } else {
                go(kPgAdd);
                addCell_ = cursor_;
            }
            break;
        case btn::Delete: deleteSelected(); break;
        default: break;
    }
}

// ------------------------------------------------------------------ matrix
void Graf::matrixPad(int pad, bool down) {
    const int r = mRow_ + lab::padRow(pad), c = mCol_ + lab::padCol(pad);
    if (r >= nSrc_ || c >= nDst_) return;
    if (down) {
        heldPin_ = pad;
        const int have = matrixCable(r, c);
        if (have >= 0) {
            selCable_ = have;
            gestUsed_ = false;   // released without turning: remove
        } else {
            const Jack& s = srcs_[r];
            const Jack& d = dsts_[c];
            const float amt = def(eng_.mod(s.slot).type).out[s.jack].sig == kCv &&
                                      def(eng_.mod(d.slot).type).in[d.jack].sig != kPitch
                                  ? 0.5f
                                  : 1.0f;
            checkpoint(1);
            const int k = eng_.connect(s.slot, s.jack, d.slot, d.jack, amt);
            if (k < 0) toastErr();
            selCable_ = k;
            gestUsed_ = true;   // a new cable stays
            pagesDirty_ = true;
        }
        if (selCable_ >= 0) {
            eng_.scopeSlot = srcs_[r].slot;
            eng_.scopeOut = srcs_[r].jack;
        }
    } else {
        if (heldPin_ == pad) heldPin_ = -1;
        const int have = matrixCable(r, c);
        if (have >= 0 && have == selCable_ && !gestUsed_) {
            checkpoint(1);
            eng_.disconnect(have);
            selCable_ = -1;
            pagesDirty_ = true;
        }
    }
}

void Graf::evMatrix(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        matrixPad(e.id, true);
        return;
    }
    if (e.type == Event::kPadUp) {
        matrixPad(e.id, false);
        return;
    }
    if (e.type == Event::kEncoder) {
        if (selCable_ >= 0 && eng_.cable(selCable_).used) {
            gestUsed_ = true;
            checkpoint(3, selCable_);
            Cable& k = eng_.cable(selCable_);
            k.amount = clampf(k.amount + float(e.delta) * (e.shift ? 0.005f : 0.05f), -2.0f, 2.0f);
            if (fabsf(k.amount) < 0.004f) k.amount = 0.0f;
        }
        return;
    }
    if (e.type != Event::kPress) return;
    const int stepR = e.shift ? 4 : 1, stepC = e.shift ? 12 : 1;
    switch (e.id) {
        case btn::Up: mRow_ -= stepR; break;
        case btn::Down: mRow_ += stepR; break;
        case btn::Left: mCol_ -= stepC; break;
        case btn::Right: mCol_ += stepC; break;
        case btn::Insert:   // flip the cable between + and -
            if (selCable_ >= 0 && eng_.cable(selCable_).used) {
                checkpoint(1);
                Cable& k = eng_.cable(selCable_);
                k.amount = k.amount > 0.0f ? -fabsf(k.amount) : fabsf(k.amount) > 0.0f ? fabsf(k.amount) : 1.0f;
            }
            break;
        case btn::Delete:
            if (selCable_ >= 0) {
                checkpoint(1);
                eng_.disconnect(selCable_);
                selCable_ = -1;
                pagesDirty_ = true;
            }
            break;
        default: break;
    }
    rebuildJacks();
}

// ------------------------------------------------------------------ module
void Graf::evModule(const Event& e) {
    using namespace lab;
    if (sel_ < 0 || !eng_.mod(sel_).used) {
        if (e.type == Event::kPress && e.id == btn::Insert) go(kPgAdd);
        return;
    }
    Module& m = eng_.mod(sel_);
    const ModDef& d = def(m.type);
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        const bool down = e.type == Event::kPadDown;
        const int row = padRow(e.id), c = padCol(e.id);
        if (m.type == kPinb) {
            if (down) {
                checkpoint(1);
                uint8_t& v = m.ext[e.id];
                if (e.shift) v ^= 2;
                else v ^= 1;
            }
            return;
        }
        if (m.type == kMgate && row >= 1) {   // its pads are the button
            m.ext[0] = down ? 1 : 0;
            return;
        }
        if (m.type == kSeq && row >= 2 && c < 8) {
            const int st = (row - 2) * 8 + c;
            if (down) {
                heldStep_ = st;
                stepUsed_ = false;
            } else if (heldStep_ == st) {
                if (!stepUsed_) {
                    checkpoint(1);
                    seqSetGate(m, st, !seqGate(m, st));
                }
                heldStep_ = -1;
            }
            return;
        }
        if (row == 0) {
            if (c < d.nParam) {
                if (down) {
                    param_ = c;
                    heldKnob_ = c;
                    knobUsed_ = false;
                } else if (heldKnob_ == c) {
                    heldKnob_ = -1;
                }
            } else if (down && c >= 10) {
                follow(c == 11);
            }
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        if (m.type == kSeq && heldStep_ >= 0) {
            stepUsed_ = true;
            checkpoint(2, 1000 + heldStep_);
            if (e.shift) {
                const int v = int(seqCv(m, heldStep_)) + e.delta * 8;
                seqCv(m, heldStep_) = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                int n = seqNote(m, heldStep_) + e.delta;
                if (n < -14) n = -14;
                if (n > 21) n = 21;
                seqNote(m, heldStep_) = int8_t(n);
                seqSetGate(m, heldStep_, true);
            }
            return;
        }
        const int p = heldKnob_ >= 0 ? heldKnob_ : param_;
        if (heldKnob_ >= 0) knobUsed_ = true;
        editKnob(sel_, p, e.delta, e.shift);
        return;
    }
    if (e.type != Event::kPress) return;
    const int per = 5;
    switch (e.id) {
        case btn::Left: param_ = param_ > 0 ? param_ - 1 : 0; break;
        case btn::Right: param_ = param_ + 1 < d.nParam ? param_ + 1 : param_; break;
        case btn::Up: param_ = param_ >= per ? param_ - per : param_; break;
        case btn::Down: param_ = param_ + per < d.nParam ? param_ + per : param_; break;
        case btn::Insert: go(kPgAdd); break;
        case btn::Delete:
            if (e.shift) {
                deleteSelected();
                go(kPgGraph);
            } else if (param_ < d.nParam) {
                checkpoint(1);
                eng_.setParamK(sel_, param_, Engine::unmapParam(d.p[param_], d.p[param_].def));
                ctx_->shell->labkit().nav.hud(knobId(sel_, param_));
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ add
int Graf::addOrderCount(int cat) const {
    if (cat >= kCatCount) return kTemplates;
    int n = 0;
    for (int t = 0; t < kTypeCount; ++t)
        if (kDefs[t].cat == cat) ++n;
    return n;
}

int Graf::addTypeAt(int cat, int i) const {
    int n = 0;
    for (int t = 0; t < kTypeCount; ++t)
        if (kDefs[t].cat == cat && n++ == i) return t;
    return -1;
}

void Graf::addSelected(bool patchFromSel) {
    const int from = (patchFromSel && sel_ >= 0 && eng_.mod(sel_).used) ? sel_ : -1;
    if (addCat_ >= kCatCount) {
        checkpoint(1);
        const int near = addCell_ >= 0 ? addCell_ : (sel_ >= 0 ? eng_.mod(sel_).cell : 0);
        const int n = addTemplate(addTpl_, patch_, near);
        char m[48];
        if (n > 0) std::snprintf(m, sizeof m, "%s: %d modules", templateName(addTpl_), n);
        else std::snprintf(m, sizeof m, "%s did not fit", templateName(addTpl_));
        ctx_->toast(m);
        rebuildJacks();
        if (n > 0) go(kPgGraph);
        return;
    }
    const int t = addType_;
    checkpoint(1);
    int cell = addCell_;
    if (cell < 0 && from >= 0) {   // next to the module it is patched from
        const int fc = eng_.mod(from).cell;
        cell = (fc % lab::kPadCols) < lab::kPadCols - 1 ? fc + 1 : fc;
    }
    const int s = eng_.add(t, patch_, cell);
    if (s < 0) {
        toastErr();
        return;
    }
    // remember it as recent
    int at = 5;
    for (int i = 0; i < 6; ++i)
        if (recent_[i] == t) at = i;
    for (int i = at; i > 0; --i) recent_[i] = recent_[i - 1];
    recent_[0] = uint8_t(t);
    char m[64];
    if (from >= 0) {
        int o, i;
        bestJacks(eng_, from, s, &o, &i);
        if (def(t).nIn && def(eng_.mod(from).type).nOut && eng_.connect(from, o, s, i, 1.0f) >= 0) {
            char a[24], b[24];
            jackLabel(a, sizeof a, Jack{uint8_t(from), uint8_t(o)}, true);
            jackLabel(b, sizeof b, Jack{uint8_t(s), uint8_t(i)}, false);
            std::snprintf(m, sizeof m, "%s > %s", a, b);
        } else {
            std::snprintf(m, sizeof m, "Added %s%d", def(t).tag, eng_.mod(s).number);
        }
    } else {
        std::snprintf(m, sizeof m, "Added %s%d", def(t).tag, eng_.mod(s).number);
    }
    select(s);
    rebuildJacks();
    ctx_->toast(m);
    go(kPgGraph);
}

void Graf::evAdd(const Event& e) {
    using namespace lab;
    const int n = addOrderCount(addCat_);
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row == 0) {
            if (c < kCatCount + 1) {
                addCat_ = c;
                if (addCat_ < kCatCount) addType_ = addTypeAt(addCat_, 0);
            } else if (c >= 9) {   // pads 10-12: the three most recent modules
                const int t = recent_[c - 9];
                addCat_ = kDefs[t].cat;
                addType_ = t;
            }
            return;
        }
        const int i = e.id - lab::kPadCols;
        if (i >= n) return;
        const bool same = addCat_ >= kCatCount ? addTpl_ == i : addType_ == addTypeAt(addCat_, i);
        if (same && addTapPad_ == e.id && e.ms - addTapMs_ < 600) {   // double tap: add
            addTapPad_ = -1;
            addSelected(e.shift);
            return;
        }
        addTapPad_ = e.id;
        addTapMs_ = e.ms;
        if (addCat_ >= kCatCount) addTpl_ = i;
        else addType_ = addTypeAt(addCat_, i);
        return;
    }
    if (e.type == Event::kEncoder) {
        if (addCat_ >= kCatCount) {
            addTpl_ = (addTpl_ + e.delta % kTemplates + kTemplates) % kTemplates;
            return;
        }
        int i = 0;
        for (int k = 0; k < n; ++k)
            if (addTypeAt(addCat_, k) == addType_) i = k;
        i += e.delta;
        while (i < 0) {   // into the previous category
            addCat_ = (addCat_ + kCatCount - 1) % kCatCount;
            i += addOrderCount(addCat_);
        }
        while (i >= addOrderCount(addCat_)) {
            i -= addOrderCount(addCat_);
            addCat_ = (addCat_ + 1) % kCatCount;
        }
        addType_ = addTypeAt(addCat_, i);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Insert: addSelected(e.shift); break;
        case btn::Up:
        case btn::Down:
            addCat_ = (addCat_ + (e.id == btn::Down ? 1 : kCatCount)) % (kCatCount + 1);
            if (addCat_ < kCatCount) addType_ = addTypeAt(addCat_, 0);
            break;
        case btn::Left:
        case btn::Right: {
            Event x;
            x.type = Event::kEncoder;
            x.delta = int8_t(e.id == btn::Right ? 1 : -1);
            evAdd(x);
            break;
        }
        default: break;
    }
}

// ------------------------------------------------------------------ snapshots
void Graf::evSnap(const Event& e) {
    using namespace lab;
    kit::Registry& P = ctx_->params();
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row == 0 && c < kSnaps) {
            snapSel_ = c;
            if (e.shift) {
                eng_.storeSnap(c);
                char m[40];
                std::snprintf(m, sizeof m, "Stored snapshot %d", c + 1);
                ctx_->toast(m);
            } else if (eng_.snap(c).valid) {
                eng_.songOn = false;
                if (xfOn_) P.set(xfOnId_, 0.0f);
                checkpoint(1);
                eng_.recallSnap(c, morphMs(), now_);
            }
            return;
        }
        if (row == 0 && c == 8) {   // crossfader on / off
            P.set(xfOnId_, xfOn_ ? 0.0f : 1.0f);
            if (xfOn_) {
                eng_.songOn = false;
                xfLast_ = -1.0f;
            }
            return;
        }
        if (row == 0 && (c == 9 || c == 10)) {   // A / B = the selected snapshot
            P.set(c == 9 ? xfAId_ : xfBId_, float(snapSel_));
            xfLast_ = -1.0f;
            return;
        }
        if (row == 0 && c == 11) {
            eng_.songOn = !eng_.songOn;
            if (eng_.songOn && xfOn_) P.set(xfOnId_, 0.0f);
            return;
        }
        if (row >= 1) {
            const int st = (row - 1) * 12 + c;
            if (st >= kSongSteps) return;
            songSel_ = st;
            if (st >= eng_.songLen) eng_.songLen = st + 1;
            if (e.shift) eng_.song[st].snap = uint8_t(snapSel_);
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        SongStep& s = eng_.song[songSel_];
        switch (songField_) {
            case 0: s.snap = uint8_t((s.snap + kSnaps + e.delta) % kSnaps); break;
            case 1: s.morphBars = uint8_t(clampf(float(s.morphBars + e.delta), 0.0f, 32.0f)); break;
            case 2: s.holdBars = uint8_t(clampf(float(s.holdBars + e.delta), 0.0f, 64.0f)); break;
            case 3: eng_.songLen = int(clampf(float(eng_.songLen + e.delta), 1.0f, float(kSongSteps))); break;
            case 4: P.nudge(xfAId_, e.delta); xfLast_ = -1.0f; break;
            case 5: P.nudge(xfBId_, e.delta); xfLast_ = -1.0f; break;
            case 6: P.nudge(xfId_, e.delta, e.shift); break;
            default: P.nudge(xfOnId_, e.delta); break;
        }
        if (s.morphBars + s.holdBars == 0) s.holdBars = 1;
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: songField_ = songField_ > 0 ? songField_ - 1 : 0; break;
        case btn::Down: songField_ = songField_ < 7 ? songField_ + 1 : 7; break;
        case btn::Left: songSel_ = songSel_ > 0 ? songSel_ - 1 : 0; break;
        case btn::Right: songSel_ = songSel_ + 1 < eng_.songLen ? songSel_ + 1 : songSel_; break;
        case btn::Insert: eng_.songOn = !eng_.songOn; break;
        case btn::Delete:
            if (eng_.songLen > 1) {
                for (int i = songSel_; i + 1 < kSongSteps; ++i) eng_.song[i] = eng_.song[i + 1];
                --eng_.songLen;
                if (songSel_ >= eng_.songLen) songSel_ = eng_.songLen - 1;
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ play
void Graf::evPlay(const Event& e) {
    using namespace lab;
    kit::Registry& P = ctx_->params();
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row != 0) return;
        if (c < kPatches) {
            if (e.shift) P.set(stripMute_[c], eng_.mix[c].mute ? 0.0f : 1.0f);
            else {
                stripSel_ = c;
                if (!eng_.rack) {
                    patch_ = c;
                    rebuildJacks();
                    pagesDirty_ = true;
                }
            }
        } else if (c == 10) {
            prefOctave_ = prefOctave_ > -2 ? prefOctave_ - 1 : -2;
        } else if (c == 11) {
            prefOctave_ = prefOctave_ < 2 ? prefOctave_ + 1 : 2;
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        P.nudge(stripField_ ? stripPan_[stripSel_] : stripLvl_[stripSel_], e.delta, e.shift);
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left: stripSel_ = stripSel_ > 0 ? stripSel_ - 1 : 0; break;
        case btn::Right: stripSel_ = stripSel_ + 1 < kPatches ? stripSel_ + 1 : stripSel_; break;
        case btn::Up:
        case btn::Down: stripField_ ^= 1; break;
        case btn::Insert: P.set(stripMute_[stripSel_], eng_.mix[stripSel_].mute ? 0.0f : 1.0f); break;
        case btn::Delete: P.reset(stripField_ ? stripPan_[stripSel_] : stripLvl_[stripSel_]); break;
        default: break;
    }
}

// ------------------------------------------------------------------ file
void Graf::evFile(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row <= 1) {
            fileSel_ = row * 12 + c;
            demoPicked_ = false;
            confirm_ = false;
        } else if (row == 3 && c < demoCount()) {
            demoSel_ = c;
            demoPicked_ = true;
            confirm_ = false;
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        if (demoPicked_) demoSel_ = (demoSel_ + e.delta % demoCount() + demoCount()) % demoCount();
        else fileSel_ = (fileSel_ + e.delta % 24 + 24) % 24;
        confirm_ = false;
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Insert: {
            if (demoPicked_) {
                checkpoint(1);
                loadDemo(demoSel_);
                ctx_->toast(demoName(demoSel_));
                return;
            }
            if (!fileExists_[fileSel_]) {
                ctx_->toast("Empty slot");
                return;
            }
            char path[96];
            std::snprintf(path, sizeof path, "%s/Patches/P%02d.grf", ctx_->home, fileSel_ + 1);
            if (loadPatch(path)) ctx_->toast("Loaded (Pattern: undo)");
            else toastErr();
            return;
        }
        case btn::Delete:
            if (confirm_ && confirmWhat_ == 2) {
                checkpoint(1);
                eng_.clear();
                for (int i = 0; i < kSnaps; ++i) eng_.clearSnap(i);
                clearAllRoutes();
                sel_ = -1;
                rebuildJacks();
                confirm_ = false;
                ctx_->toast("New empty patch");
            } else {
                confirm_ = true;
                confirmWhat_ = 2;
                ctx_->toast("Press Delete again to clear everything");
            }
            return;
        case btn::Rec:
            scanSamples();
            listFiles();
            {
                char m[40];
                std::snprintf(m, sizeof m, "%d samples found", eng_.sampleFileCount());
                ctx_->toast(m);
            }
            return;
        case btn::Left: fileSel_ = fileSel_ > 0 ? fileSel_ - 1 : 0; demoPicked_ = false; break;
        case btn::Right: fileSel_ = fileSel_ < 23 ? fileSel_ + 1 : 23; demoPicked_ = false; break;
        case btn::Up: fileSel_ = fileSel_ >= 12 ? fileSel_ - 12 : fileSel_; demoPicked_ = false; break;
        case btn::Down: fileSel_ = fileSel_ < 12 ? fileSel_ + 12 : fileSel_; demoPicked_ = false; break;
        default: break;
    }
    confirm_ = false;
}

// ------------------------------------------------------------------ samples
// S.Load: the LABKIT browser on /Lab/GRAF/Samples for the selected sampler
// (or a new one); the pick becomes that sampler's sound.
void Graf::openSampleBrowser() {
    if (!ctx_->fs) {
        ctx_->toast("No card");
        return;
    }
    if (sel_ < 0 || !eng_.mod(sel_).used || eng_.mod(sel_).type != kSmpl) {
        ctx_->toast("Select a SMPL module first (S.Load opens its samples)");
        return;
    }
    static char folder[64];
    std::snprintf(folder, sizeof folder, "%s/Samples", ctx_->home);
    kit::BrowserRequest q;
    q.title = "Sample for SMPL";
    q.folder = folder;
    q.exts = ".wav";
    q.ctx = this;
    q.onPick = [](void* c, const char* path) {
        Graf* g = static_cast<Graf*>(c);
        const char* name = std::strrchr(path, '/');
        name = name ? name + 1 : path;
        g->scanSamples();
        Engine& e = g->eng_;
        for (int i = 1; i <= e.sampleFileCount(); ++i)
            if (!std::strcmp(e.sampleFileName(i), name) && g->sel_ >= 0) {
                g->checkpoint(1);
                e.setParamK(g->sel_, 0, Engine::unmapParam(def(kSmpl).p[0], float(i)));
                return;
            }
        g->ctx_->toast("Only files in /Lab/GRAF/Samples can be used");
    };
    ctx_->browser().open(q);
}

}  // namespace graf
