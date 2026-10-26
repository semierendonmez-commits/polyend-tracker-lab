// GRAF -- workstation: init, events, MIDI, files, settings, demos, bench.
#include "ws/graf_ws.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace graf {

namespace {
const char* const kModelNames[] = {"Rack", "8 patches"};
const char* const kMorphNames[] = {"Cut", "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars"};
const float kMorphBars[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
const char* const kOctNames[] = {"-2", "-1", "0", "+1", "+2"};
const char* const kOffOnS[] = {"Off", "On"};

struct Prefs {
    uint32_t magic = 0x47524631u;   // "GRF1"
    int morph = 3, songPlay = 0, octave = 0, vel = 1, model = 0, sag = 1, patch = 0;
};

int cmpName(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = char(x - 32);
        if (y >= 'a' && y <= 'z') y = char(y - 32);
        if (x != y || !x) return int(uint8_t(x)) - int(uint8_t(y));
    }
}
}  // namespace

// ------------------------------------------------------------------ init
bool Graf::init(lab::Context& c) {
    ctx_ = &c;
    theme_.bg = col::kBg;
    theme_.panel = col::kPanel;
    theme_.fg = col::kFg;
    theme_.dim = col::kDim;
    theme_.accent = col::kAccent;
    theme_.accent2 = col::kSig[1];
    theme_.warn = col::kWarn;
    theme_.ok = col::kOk;
    if (!eng_.init(*c.arena)) return false;
    eng_.setConcurrent(true);
    fileBuf_ = static_cast<uint8_t*>(c.arena->alloc(kFileBuf));
    if (!fileBuf_) return false;
    for (int& n : heldNote_) n = -1;
    // ADD order: by category, then table order
    int n = 0;
    for (int cat = 0; cat < kCatCount; ++cat)
        for (int t = 0; t < kTypeCount; ++t)
            if (kDefs[t].cat == cat) addOrder_[n++] = uint8_t(t);
    loadPrefs();
    scanSamples();
    char path[96];
    std::snprintf(path, sizeof path, "%s/autosave.grf", c.home);
    if (!(c.fs && c.fs->exists(path) && loadPatch(path))) loadDemo(0);
    prefModel_ = eng_.rack ? 0 : 1;   // the patch file carries the model
    if (patch_ < 0 || patch_ >= kPatches) patch_ = 0;
    rebuildJacks();
    return true;
}

void Graf::loadPrefs() {
    if (!ctx_->fs) return;
    char path[96];
    std::snprintf(path, sizeof path, "%s/graf.cfg", ctx_->home);
    const lab::IStorage::Handle h = ctx_->fs->open(path, lab::IStorage::kRead);
    if (h < 0) return;
    Prefs p;
    if (ctx_->fs->read(h, &p, sizeof p) == int32_t(sizeof p) && p.magic == Prefs().magic) {
        prefMorph_ = p.morph >= 0 && p.morph <= 6 ? p.morph : 3;
        prefSongPlay_ = p.songPlay ? 1 : 0;
        prefOctave_ = p.octave >= -2 && p.octave <= 2 ? p.octave : 0;
        prefVel_ = p.vel ? 1 : 0;
        prefModel_ = p.model ? 1 : 0;
        prefSag_ = p.sag ? 1 : 0;
        patch_ = p.patch & 7;
    }
    ctx_->fs->close(h);
}

void Graf::savePrefs() {
    if (!ctx_->fs) return;
    char path[96];
    std::snprintf(path, sizeof path, "%s/graf.cfg", ctx_->home);
    const lab::IStorage::Handle h = ctx_->fs->open(path, lab::IStorage::kWrite);
    if (h < 0) return;
    Prefs p;
    p.morph = prefMorph_;
    p.songPlay = prefSongPlay_;
    p.octave = prefOctave_;
    p.vel = prefVel_;
    p.model = prefModel_;
    p.sag = prefSag_;
    p.patch = patch_;
    ctx_->fs->write(h, &p, sizeof p);
    ctx_->fs->close(h);
}

void Graf::scanSamples() {
    samplesScanned_ = true;
    SampleFile files[kMaxSampleFiles];
    int n = 0;
    if (ctx_->fs) {
        char dir[96];
        std::snprintf(dir, sizeof dir, "%s/Samples", ctx_->home);
        ctx_->fs->mkdir(dir);
        struct C {
            SampleFile* f;
            int* n;
        } cx{files, &n};
        ctx_->fs->listDir(
            dir,
            [](const lab::IStorage::DirEntry& e, void* p) {
                C* c = static_cast<C*>(p);
                if (e.isDir || !lab::endsWithCi(e.name, ".wav") || e.name[0] == '.') return true;
                if (std::strlen(e.name) >= sizeof c->f[0].name) return true;
                if (*c->n >= kMaxSampleFiles) return false;
                lab::copyStr(c->f[*c->n].name, sizeof c->f[0].name, e.name);
                ++*c->n;
                return true;
            },
            &cx);
        // sort by name (insertion sort, n <= 32)
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && cmpName(files[j].name, files[j - 1].name) < 0; --j) {
                SampleFile t = files[j];
                files[j] = files[j - 1];
                files[j - 1] = t;
            }
        std::snprintf(dir, sizeof dir, "%s/Patches", ctx_->home);
        ctx_->fs->mkdir(dir);
    }
    eng_.setSampleFiles(files, n);
    // samplers that named a file keep it when it is still on the card
    for (int s = 0; s < kMaxSlots; ++s) {
        Module& m = eng_.mod(s);
        if (!m.used || m.type != kSmpl) continue;
        if (eng_.sampleName[s][0])
            for (int i = 0; i < n; ++i)
                if (!cmpName(files[i].name, eng_.sampleName[s])) {
                    eng_.setParamK(s, 0, Engine::unmapParam(def(kSmpl).p[0], float(i + 1)));
                    break;
                }
        eng_.sampleLoaded[s] = -1;   // reload in update()
    }
}

// ------------------------------------------------------------------ helpers
void Graf::toastErr() {
    const char* e = eng_.lastError();
    if (e && *e) ctx_->toast(e);
}

void Graf::go(int p) {
    emptyTap_ = -1;
    if (p == page_) return;
    prevPage_ = page_;
    page_ = p;
    confirm_ = false;
    if (p == kPgMatrix || p == kPgModule) rebuildJacks();
    if (p == kPgModule && (sel_ < 0 || !eng_.mod(sel_).used)) {
        // pick the first module of this patch
        for (int c = 0; c < kCells; ++c) {
            const int s = eng_.slotAtCell(patch_, c);
            if (s >= 0) {
                select(s);
                break;
            }
        }
    }
    if (p == kPgAdd) addCell_ = eng_.slotAtCell(patch_, cursor_) < 0 ? cursor_ : -1;
    if (p == kPgFile) listFiles();
    // the Play page scopes the master, the others the selected module
    eng_.scopeSlot = p == kPgPlay ? -1 : sel_;
}

void Graf::select(int slot) {
    sel_ = slot;
    param_ = 0;
    if (slot >= 0) {
        cursor_ = eng_.mod(slot).cell;
        eng_.scopeSlot = slot;
        eng_.scopeOut = 0;
    }
}

void Graf::setModel(bool rack) {
    prefModel_ = rack ? 0 : 1;
    if (eng_.rack == rack) return;
    if (!rack) {   // 8 patches: cables between canvases are not possible any more
        int cut = 0;
        for (int c = 0; c < kMaxCables; ++c) {
            const Cable& k = eng_.cable(c);
            if (k.used && eng_.mod(k.src).patch != eng_.mod(k.dst).patch) {
                eng_.disconnect(c);
                ++cut;
            }
        }
        if (cut && ctx_) ctx_->toast("Cables between patches removed");
    }
    eng_.allNotesOff();
    eng_.rack = rack;
    eng_.rebuild();
    rebuildJacks();
}

void Graf::rebuildJacks() {
    nSrc_ = nDst_ = 0;
    // sources: this patch (8 patches) or every canvas (Rack), by canvas and cell
    for (int p = 0; p < kPatches; ++p) {
        if (!eng_.rack && p != patch_) continue;
        for (int c = 0; c < kCells; ++c) {
            const int s = eng_.slotAtCell(p, c);
            if (s < 0) continue;
            const ModDef& d = def(eng_.mod(s).type);
            for (int o = 0; o < d.nOut; ++o) srcs_[nSrc_++] = Jack{uint8_t(s), uint8_t(o)};
        }
    }
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(patch_, c);
        if (s < 0) continue;
        const ModDef& d = def(eng_.mod(s).type);
        for (int i = 0; i < d.nIn; ++i) dsts_[nDst_++] = Jack{uint8_t(s), uint8_t(i)};
    }
    const int maxR = nSrc_ > 4 ? nSrc_ - 4 : 0, maxC = nDst_ > 12 ? nDst_ - 12 : 0;
    if (mRow_ > maxR) mRow_ = maxR;
    if (mCol_ > maxC) mCol_ = maxC;
    if (mRow_ < 0) mRow_ = 0;
    if (mCol_ < 0) mCol_ = 0;
    if (selCable_ >= 0 && !eng_.cable(selCable_).used) selCable_ = -1;
}

int Graf::matrixCable(int r, int c) const {
    if (r < 0 || r >= nSrc_ || c < 0 || c >= nDst_) return -1;
    return eng_.cableFind(srcs_[r].slot, srcs_[r].jack, dsts_[c].slot, dsts_[c].jack);
}

void Graf::focusMatrix(int srcSlot, int dstSlot) {
    rebuildJacks();
    for (int r = 0; r < nSrc_; ++r)
        if (srcs_[r].slot == srcSlot) {
            mRow_ = r;
            break;
        }
    for (int c = 0; c < nDst_; ++c)
        if (dsts_[c].slot == dstSlot) {
            mCol_ = c;
            break;
        }
    rebuildJacks();   // clamps the window
}

void Graf::modLabel(char* buf, int cap, int slot) const {
    if (slot < 0 || !eng_.mod(slot).used) {
        lab::copyStr(buf, uint32_t(cap), "-");
        return;
    }
    const Module& m = eng_.mod(slot);
    std::snprintf(buf, size_t(cap), "%s%d", def(m.type).tag, m.number);
}

void Graf::jackLabel(char* buf, int cap, const Jack& j, bool out) const {
    char ml[12];
    modLabel(ml, sizeof ml, j.slot);
    const ModDef& d = def(eng_.mod(j.slot).type);
    std::snprintf(buf, size_t(cap), "%s %s", ml, out ? d.out[j.jack].name : d.in[j.jack].name);
}

void Graf::fmtParam(char* buf, int cap, int slot, int p) const {
    const Module& m = eng_.mod(slot);
    const ModDef& d = def(m.type);
    const ParamDef& pd = d.p[p];
    const float v = m.v[p];
    if (m.type == kSmpl && p == 0) {
        lab::copyStr(buf, uint32_t(cap), eng_.sampleFileName(int(v)));
        return;
    }
    if (pd.kind == kInt && pd.names) {
        int i = int(v - pd.min + 0.5f);
        lab::copyStr(buf, uint32_t(cap), pd.names[i < 0 ? 0 : i]);
        return;
    }
    if (pd.kind == kInt) {
        std::snprintf(buf, size_t(cap), "%d", int(v));
        return;
    }
    const char* u = pd.unit ? pd.unit : "";
    if (!std::strcmp(u, "Hz")) {
        if (v >= 1000.0f) std::snprintf(buf, size_t(cap), "%.2fk", double(v * 0.001f));
        else if (v >= 10.0f) std::snprintf(buf, size_t(cap), "%.0f Hz", double(v));
        else std::snprintf(buf, size_t(cap), "%.2f Hz", double(v));
    } else if (!std::strcmp(u, "s")) {
        if (v < 1.0f) std::snprintf(buf, size_t(cap), "%.0f ms", double(v * 1000.0f));
        else std::snprintf(buf, size_t(cap), "%.2f s", double(v));
    } else if (!std::strcmp(u, "%")) {
        std::snprintf(buf, size_t(cap), "%.0f%%", double(v * 100.0f));
    } else if (!std::strcmp(u, "st")) {
        std::snprintf(buf, size_t(cap), "%+.1f st", double(v));
    } else if (!std::strcmp(u, "oct")) {
        std::snprintf(buf, size_t(cap), "%+.1f oct", double(v));
    } else if (!std::strcmp(u, "x")) {
        std::snprintf(buf, size_t(cap), "x%.2f", double(v));
    } else {
        std::snprintf(buf, size_t(cap), "%.2f", double(v));
    }
}

uint32_t Graf::morphMs() const {
    const float bars = kMorphBars[prefMorph_ < 0 ? 0 : (prefMorph_ > 6 ? 6 : prefMorph_)];
    const float bpm = ctx_->bpm() > 20.0f ? ctx_->bpm() : 120.0f;
    return uint32_t(bars * 4.0f * 60000.0f / bpm);
}

// ------------------------------------------------------------------ keyboard
int Graf::keyBase() const { return 48 + 12 * prefOctave_; }

int Graf::keyNote(int pad) const {
    const int row = lab::padRow(pad), c = lab::padCol(pad);
    if (row < 1) return -1;
    if (page_ == kPgModule && sel_ >= 0) {
        const int t = eng_.mod(sel_).type;
        if (t == kPinb) return -1;
        if (t == kSeq && row >= 2 && c < 8) return -1;
    }
    if (page_ != kPgPlay && page_ != kPgModule) return -1;
    const int n = keyBase() + (3 - row) * 12 + c;
    return n > 127 ? 127 : n;
}

void Graf::keyPad(int pad, bool down, uint8_t vel) {
    const int vp = eng_.rack ? 0 : patch_;
    if (down) {
        const int n = keyNote(pad);
        if (n < 0) return;
        const uint8_t v = prefVel_ ? (vel ? vel : 100) : 100;
        heldNote_[pad] = n;
        eng_.noteOn(vp, uint8_t(n), v);
        ctx_->midiNote(true, uint8_t(n), v, uint8_t(eng_.rack ? 0 : patch_));
    } else if (heldNote_[pad] >= 0) {
        eng_.noteOff(vp, uint8_t(heldNote_[pad]));
        ctx_->midiNote(false, uint8_t(heldNote_[pad]), 0, uint8_t(eng_.rack ? 0 : patch_));
        heldNote_[pad] = -1;
    }
}

// ------------------------------------------------------------------ events
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
    // global keys
    if (e.type == Event::kPress) {
        if (e.id <= lab::btn::F8) {
            fkey(e.id, e.shift);
            return;
        }
        switch (e.id) {
            case lab::btn::Insert:
                if (page_ != kPgFile) {
                    go(kPgAdd);
                    return;
                }
                break;
            case lab::btn::Copy:
                if (sel_ >= 0 && (page_ == kPgGraph || page_ == kPgModule)) {
                    const int n = eng_.duplicate(sel_);
                    if (n < 0) toastErr();
                    else {
                        select(n);
                        ctx_->toast("Duplicated");
                    }
                    rebuildJacks();
                    return;
                }
                break;
            case lab::btn::Fx1:
                if (sel_ >= 0) {
                    eng_.randomize(sel_, eng_.rng);
                    ctx_->toast("Randomized");
                }
                return;
            case lab::btn::Fx2:
                if (sel_ >= 0) {
                    const int n = def(eng_.mod(sel_).type).nOut;
                    eng_.scopeSlot = sel_;
                    eng_.scopeOut = n ? (eng_.scopeOut + 1) % n : 0;
                }
                return;
            default: break;
        }
    }
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

void Graf::fkey(int k, bool shift) {
    static const uint8_t kPages[7] = {kPgGraph, kPgMatrix, kPgModule, kPgAdd, kPgSnap, kPgPlay, kPgFile};
    if (k < 7) {
        go(kPages[k]);
        return;
    }
    action(shift);
}

// F8: the page's action.
void Graf::action(bool shift) {
    switch (page_) {
        case kPgAdd: {
            const int t = addType_;
            const int s = eng_.add(t, patch_, addCell_);
            if (s < 0) {
                toastErr();
                return;
            }
            select(s);
            rebuildJacks();
            char m[48];
            std::snprintf(m, sizeof m, "Added %s%d", def(t).tag, eng_.mod(s).number);
            ctx_->toast(m);
            go(kPgGraph);
            return;
        }
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
                ctx_->toast("Exists: press Save again to overwrite");
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
        default: {   // patch / page switch
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
            return;
        }
    }
}

void Graf::deleteSelected() {
    if (sel_ < 0 || !eng_.mod(sel_).used) return;
    char m[40], l[12];
    modLabel(l, sizeof l, sel_);
    std::snprintf(m, sizeof m, "Removed %s", l);
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

void Graf::evGraph(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        const int cell = e.id;
        const int at = eng_.slotAtCell(patch_, cell);
        // another module pad already held: connect held -> this one
        if (holdCell_ >= 0 && holdCell_ != cell && at >= 0) {
            const int src = eng_.slotAtCell(patch_, holdCell_);
            usedHold_ = true;
            if (src >= 0) {
                const ModDef& ds = def(eng_.mod(src).type);
                const ModDef& dd = def(eng_.mod(at).type);
                bool any = false;
                for (int o = 0; o < ds.nOut && !any; ++o)
                    for (int i = 0; i < dd.nIn; ++i)
                        if (eng_.cableFind(src, o, at, i) >= 0) any = true;
                if (any || !ds.nOut || !dd.nIn) {
                    focusMatrix(src, at);
                    go(kPgMatrix);
                    return;
                }
                // first output, the first input of the same kind (else the first input)
                int bo = 0, bi = -1;
                for (int o = 0; o < ds.nOut && bi < 0; ++o)
                    for (int i = 0; i < dd.nIn; ++i)
                        if (dd.in[i].sig == ds.out[o].sig ||
                            (ds.out[o].sig == kCv && dd.in[i].sig == kPitch)) {
                            bo = o;
                            bi = i;
                            break;
                        }
                if (bi < 0) bi = 0;
                const int c = eng_.connect(src, bo, at, bi, 1.0f);
                if (c < 0) {
                    toastErr();
                    return;
                }
                selCable_ = c;
                rebuildJacks();
                char a[24], b[24], m[64];
                jackLabel(a, sizeof a, Jack{uint8_t(src), uint8_t(bo)}, true);
                jackLabel(b, sizeof b, Jack{uint8_t(at), uint8_t(bi)}, false);
                std::snprintf(m, sizeof m, "%s > %s", a, b);
                ctx_->toast(m);
            }
            return;
        }
        if (e.shift && sel_ >= 0) {   // move the selected module here
            eng_.move(sel_, cell);
            cursor_ = cell;
            rebuildJacks();
            return;
        }
        if (at >= 0) {
            select(at);
            holdCell_ = cell;
            usedHold_ = false;
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
        }
        return;
    }
    if (e.type == Event::kPadUp) {
        if (e.id == holdCell_) holdCell_ = -1;
        return;
    }
    if (e.type == Event::kEncoder) {
        // step the selection through the modules of this canvas
        int c = sel_ >= 0 ? eng_.mod(sel_).cell : cursor_;
        for (int k = 0; k < kCells; ++k) {
            c = (c + (e.delta > 0 ? 1 : kCells - 1)) % kCells;
            const int s = eng_.slotAtCell(patch_, c);
            if (s >= 0) {
                select(s);
                break;
            }
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left:
        case btn::Right:
        case btn::Up:
        case btn::Down: {
            const int dx = e.id == btn::Left ? -1 : (e.id == btn::Right ? 1 : 0);
            const int dy = e.id == btn::Up ? -1 : (e.id == btn::Down ? 1 : 0);
            if (e.shift && sel_ >= 0) {
                moveCursor(dx, dy);
                eng_.move(sel_, cursor_);
                rebuildJacks();
            } else {
                moveCursor(dx, dy);
                const int s = eng_.slotAtCell(patch_, cursor_);
                if (s >= 0) select(s);
                else sel_ = -1;
            }
            break;
        }
        case btn::EncPush:
            if (sel_ >= 0) go(kPgModule);
            else {
                go(kPgAdd);
                addCell_ = cursor_;
            }
            break;
        case btn::Delete: deleteSelected(); break;
        default: break;
    }
}

void Graf::matrixPad(int pad, bool down) {
    const int r = mRow_ + lab::padRow(pad), c = mCol_ + lab::padCol(pad);
    if (r >= nSrc_ || c >= nDst_) return;
    mCurR_ = r;
    mCurC_ = c;
    if (down) {
        const int have = matrixCable(r, c);
        if (have >= 0) {
            selCable_ = have;
            usedHold_ = false;   // released without turning: remove
        } else {
            const Jack& s = srcs_[r];
            const Jack& d = dsts_[c];
            // default amount: full for audio / gate / pitch, half for CV into audio-rate params
            const float amt = def(eng_.mod(s.slot).type).out[s.jack].sig == kCv ? 0.5f : 1.0f;
            const int k = eng_.connect(s.slot, s.jack, d.slot, d.jack, amt);
            if (k < 0) toastErr();
            selCable_ = k;
            usedHold_ = true;   // a new cable stays
        }
    } else {
        const int have = matrixCable(r, c);
        if (have >= 0 && have == selCable_ && !usedHold_) {
            eng_.disconnect(have);
            selCable_ = -1;
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
            usedHold_ = true;
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
        case btn::EncPush:
            if (selCable_ >= 0 && eng_.cable(selCable_).used) {
                Cable& k = eng_.cable(selCable_);
                k.amount = k.amount > 0.0f ? -1.0f : 1.0f;
            }
            break;
        case btn::Delete:
            if (selCable_ >= 0) {
                eng_.disconnect(selCable_);
                selCable_ = -1;
            }
            break;
        default: break;
    }
    rebuildJacks();
}

void Graf::evModule(const Event& e) {
    using namespace lab;
    if (sel_ < 0 || !eng_.mod(sel_).used) {
        if (e.type == Event::kPress && e.id == btn::EncPush) go(kPgAdd);
        return;
    }
    Module& m = eng_.mod(sel_);
    const ModDef& d = def(m.type);
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        const bool down = e.type == Event::kPadDown;
        const int row = padRow(e.id), c = padCol(e.id);
        if (m.type == kPinb) {
            if (down) {
                uint8_t& v = m.ext[e.id];
                if (e.shift) v ^= 2;
                else v ^= 1;
            }
            return;
        }
        if (m.type == kSeq && row >= 2 && c < 8) {
            const int st = (row - 2) * 8 + c;
            if (down) {
                heldStep_ = st;
                usedHold_ = false;
            } else if (heldStep_ == st) {
                if (!usedHold_) seqSetGate(m, st, !seqGate(m, st));
                heldStep_ = -1;
            }
            return;
        }
        if (row == 0 && down) {
            if (c < d.nParam) param_ = c;
            else if (c >= 10) {   // previous / next module on this canvas
                int cell = m.cell;
                for (int k = 0; k < kCells; ++k) {
                    cell = (cell + (c == 11 ? 1 : kCells - 1)) % kCells;
                    const int s = eng_.slotAtCell(patch_, cell);
                    if (s >= 0) {
                        select(s);
                        break;
                    }
                }
            }
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        if (m.type == kSeq && heldStep_ >= 0) {
            usedHold_ = true;
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
        if (param_ >= d.nParam) return;
        const ParamDef& pd = d.p[param_];
        if (pd.kind == kInt) {
            float v = m.v[param_] + float(e.delta);
            float mx = pd.max;
            if (m.type == kSmpl && param_ == 0) mx = float(eng_.sampleFileCount());
            v = clampf(v, pd.min, mx);
            eng_.setParamK(sel_, param_, Engine::unmapParam(pd, v));
            if (m.type == kMidi && param_ == 0) eng_.rebuild();
        } else {
            eng_.setParamK(sel_, param_, m.k[param_] + float(e.delta) * (e.shift ? 0.001f : 0.01f));
        }
        return;
    }
    if (e.type != Event::kPress) return;
    const int per = 5;
    switch (e.id) {
        case btn::Left: param_ = param_ > 0 ? param_ - 1 : 0; break;
        case btn::Right: param_ = param_ + 1 < d.nParam ? param_ + 1 : param_; break;
        case btn::Up: param_ = param_ >= per ? param_ - per : param_; break;
        case btn::Down: param_ = param_ + per < d.nParam ? param_ + per : param_; break;
        case btn::EncPush:
            if (param_ < d.nParam) eng_.setParamK(sel_, param_, Engine::unmapParam(d.p[param_], d.p[param_].def));
            break;
        case btn::Delete:
            deleteSelected();
            go(kPgGraph);
            break;
        case btn::Rec:
            if (m.type == kPinb) pinbLayout(m, eng_.rng.next());
            else if (m.type == kRete) m.ext[0] = uint8_t(m.ext[0] + 1);
            else if (m.type == kMarkov) eng_.setParamK(sel_, 7, eng_.rng.uni());
            else if (m.type == kSeq)
                for (int i = 0; i < 16; ++i) {
                    seqNote(m, i) = int8_t(eng_.rng.range(10) - 2);
                    seqSetGate(m, i, eng_.rng.uni() < 0.7f);
                }
            break;
        default: break;
    }
}

void Graf::evAdd(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        if (e.id >= kTypeCount) return;
        const int t = addOrder_[e.id];
        // a second tap on the same pad within 600 ms adds the module
        if (t == addType_ && addTapPad_ == e.id && e.ms - addTapMs_ < 600) {
            addTapPad_ = -1;
            action(false);
            return;
        }
        addTapPad_ = e.id;
        addTapMs_ = e.ms;
        addType_ = t;
        return;
    }
    if (e.type == Event::kEncoder) {
        int idx = 0;
        for (int i = 0; i < kTypeCount; ++i)
            if (addOrder_[i] == addType_) idx = i;
        idx = (idx + e.delta % kTypeCount + kTypeCount) % kTypeCount;
        addType_ = addOrder_[idx];
        return;
    }
    if (e.type == Event::kPress && e.id == btn::EncPush) action(false);
    if (e.type == Event::kPress && (e.id == btn::Left || e.id == btn::Right || e.id == btn::Up || e.id == btn::Down)) {
        int idx = 0;
        for (int i = 0; i < kTypeCount; ++i)
            if (addOrder_[i] == addType_) idx = i;
        const int d = e.id == btn::Left ? -1 : e.id == btn::Right ? 1 : e.id == btn::Up ? -12 : 12;
        idx += d;
        if (idx < 0) idx = 0;
        if (idx >= kTypeCount) idx = kTypeCount - 1;
        addType_ = addOrder_[idx];
    }
}

void Graf::evSnap(const Event& e) {
    using namespace lab;
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
                eng_.recallSnap(c, morphMs(), now_);
            }
            return;
        }
        if (row == 0 && c == 11) {
            eng_.songOn = !eng_.songOn;
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
        if (songField_ == 0) s.snap = uint8_t((s.snap + kSnaps + e.delta) % kSnaps);
        else if (songField_ == 1) s.morphBars = uint8_t(clampf(float(s.morphBars + e.delta), 0.0f, 32.0f));
        else if (songField_ == 2) s.holdBars = uint8_t(clampf(float(s.holdBars + e.delta), 0.0f, 64.0f));
        else eng_.songLen = int(clampf(float(eng_.songLen + e.delta), 1.0f, float(kSongSteps)));
        if (s.morphBars + s.holdBars == 0) s.holdBars = 1;
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Up: songField_ = songField_ > 0 ? songField_ - 1 : 0; break;
        case btn::Down: songField_ = songField_ < 3 ? songField_ + 1 : 3; break;
        case btn::Left: songSel_ = songSel_ > 0 ? songSel_ - 1 : 0; break;
        case btn::Right: songSel_ = songSel_ + 1 < eng_.songLen ? songSel_ + 1 : songSel_; break;
        case btn::EncPush: eng_.songOn = !eng_.songOn; break;
        case btn::Delete:
            if (eng_.songLen > 1) {
                for (int i = songSel_; i + 1 < kSongSteps; ++i) eng_.song[i] = eng_.song[i + 1];
                --eng_.songLen;
                if (songSel_ >= eng_.songLen) songSel_ = eng_.songLen - 1;
            }
            break;
        case btn::Insert: break;
        default: break;
    }
}

void Graf::evPlay(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row != 0) return;
        if (c < kPatches) {
            if (eng_.rack) {
                stripSel_ = c;
            } else {
                if (e.shift) eng_.mix[c].mute = !eng_.mix[c].mute;
                else {
                    patch_ = c;
                    stripSel_ = c;
                    rebuildJacks();
                }
            }
        } else if (c == 10) {
            prefOctave_ = prefOctave_ > -2 ? prefOctave_ - 1 : -2;
        } else if (c == 11) {
            prefOctave_ = prefOctave_ < 2 ? prefOctave_ + 1 : 2;
        }
        return;
    }
    // the strips: patches (8 patches) or the OUT modules (Rack)
    int outs[kPatches], nOut = 0;
    if (eng_.rack)
        for (int s = 0; s < kMaxSlots && nOut < kPatches; ++s)
            if (eng_.mod(s).used && eng_.mod(s).type == kOut) outs[nOut++] = s;
    const int nStrips = eng_.rack ? nOut : kPatches;
    if (e.type == Event::kEncoder) {
        if (stripSel_ >= nStrips) return;
        const float d = float(e.delta) * (e.shift ? 0.002f : 0.02f);
        if (eng_.rack) {
            const int s = outs[stripSel_];
            eng_.setParamK(s, stripField_ ? 1 : 0, eng_.mod(s).k[stripField_ ? 1 : 0] + d);
        } else {
            MixStrip& ms = eng_.mix[stripSel_];
            if (stripField_ == 0) ms.level = clampf(ms.level + d, 0.0f, 1.0f);
            else ms.pan = clampf(ms.pan + d * 2.0f, -1.0f, 1.0f);
        }
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::Left: stripSel_ = stripSel_ > 0 ? stripSel_ - 1 : 0; break;
        case btn::Right: stripSel_ = stripSel_ + 1 < nStrips ? stripSel_ + 1 : stripSel_; break;
        case btn::Up:
        case btn::Down: stripField_ ^= 1; break;
        case btn::EncPush:
            if (!eng_.rack && stripSel_ < kPatches) eng_.mix[stripSel_].mute = !eng_.mix[stripSel_].mute;
            break;
        default: break;
    }
}

void Graf::listFiles() {
    for (int i = 0; i < 24; ++i) {
        fileExists_[i] = false;
        if (!ctx_->fs) continue;
        char path[96];
        std::snprintf(path, sizeof path, "%s/Patches/P%02d.grf", ctx_->home, i + 1);
        fileExists_[i] = ctx_->fs->exists(path);
    }
}

void Graf::evFile(const Event& e) {
    using namespace lab;
    if (e.type == Event::kPadDown) {
        const int row = padRow(e.id), c = padCol(e.id);
        if (row <= 1) {
            fileSel_ = row * 12 + c;
            confirm_ = false;
        } else if (row == 3 && c < demoCount()) {
            demoSel_ = c;
            confirm_ = false;
        }
        return;
    }
    if (e.type == Event::kEncoder) {
        fileSel_ = (fileSel_ + e.delta % 24 + 24) % 24;
        confirm_ = false;
        return;
    }
    if (e.type != Event::kPress) return;
    switch (e.id) {
        case btn::EncPush: {
            if (!fileExists_[fileSel_]) {
                ctx_->toast("Empty slot");
                return;
            }
            char path[96];
            std::snprintf(path, sizeof path, "%s/Patches/P%02d.grf", ctx_->home, fileSel_ + 1);
            if (loadPatch(path)) ctx_->toast("Loaded");
            else toastErr();
            return;
        }
        case btn::Insert:
            loadDemo(demoSel_);
            ctx_->toast(demoName(demoSel_));
            return;
        case btn::Delete:
            if (confirm_ && confirmWhat_ == 2) {
                eng_.clear();
                for (int i = 0; i < kSnaps; ++i) eng_.clearSnap(i);
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
        case btn::Left: fileSel_ = fileSel_ > 0 ? fileSel_ - 1 : 0; break;
        case btn::Right: fileSel_ = fileSel_ < 23 ? fileSel_ + 1 : 23; break;
        case btn::Up: fileSel_ = fileSel_ >= 12 ? fileSel_ - 12 : fileSel_; break;
        case btn::Down: fileSel_ = fileSel_ < 12 ? fileSel_ + 12 : fileSel_; break;
        default: break;
    }
    confirm_ = false;
}

// ------------------------------------------------------------------ MIDI, update
void Graf::onMidi(const lab::MidiMsg& m) { eng_.midiIn(m); }

void Graf::onTransport(bool playing) {
    if (!playing) {
        eng_.allNotesOff();
        return;
    }
    if (prefSongPlay_) {
        bool any = false;
        for (int i = 0; i < eng_.songLen; ++i)
            if (eng_.snap(eng_.song[i].snap).valid) any = true;
        if (any) eng_.songOn = true;
    }
}

void Graf::update(uint32_t ms) {
    now_ = ms;
    eng_.service(ms);
    // MIDI notes from sequencers
    Engine::MidiOutEv ev;
    while (eng_.midiQ.pop(ev)) {
        lab::MidiMsg m;
        m.status = uint8_t((ev.on ? 0x90 : 0x80) | (ev.ch & 15));
        m.d1 = ev.note & 127;
        m.d2 = ev.on ? (ev.vel & 127) : 0;
        ctx_->midiOut(m);
    }
    // samplers whose Sample parameter changed (and then stayed for 250 ms)
    for (int s = 0; s < kMaxSlots; ++s) {
        const Module& m = eng_.mod(s);
        if (!m.used || m.type != kSmpl) continue;
        if (smplWant_[s] != int(m.v[0])) {
            smplWant_[s] = int(m.v[0]);
            smplSince_[s] = ms;
        }
        if (eng_.sampleLoaded[s] != smplWant_[s] && ms - smplSince_[s] >= 250) {
            char dir[96];
            std::snprintf(dir, sizeof dir, "%s/Samples", ctx_->home);
            if (!eng_.loadSample(s, ctx_->fs, dir)) toastErr();
            break;   // one per pass
        }
    }
    // snapshot morphs at ~100 Hz: a step walks every module and cable
    if (ms - lastMorph_ >= 10) {
        lastMorph_ = ms;
        if (eng_.songOn && ctx_->playing()) eng_.songApply(eng_.beat());
        else eng_.morphStep(ms);
    }
    if ((eng_.morphing() || eng_.songOn) && ms - lastJacks_ >= 200) {
        lastJacks_ = ms;
        rebuildJacks();
    }
    // meters (peak hold, decaying)
    for (int p = 0; p < kPatches; ++p) {
        const float v = eng_.meter[p];
        eng_.meter[p] = 0.0f;
        meterShow_[p] = v > meterShow_[p] ? v : meterShow_[p] * 0.9f;
    }
    const float mp = eng_.masterPeak;
    eng_.masterPeak = 0.0f;
    masterShow_ = mp > masterShow_ ? mp : masterShow_ * 0.9f;
}

void Graf::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    eng_.render(t, in, l, r, ctx_);
}

// ------------------------------------------------------------------ settings
int Graf::settings(lab::SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names,
                   void (*fn)(void*, int)) {
        if (n >= cap) return;
        lab::SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.onChange = fn;
        r.ctx = this;
    };
    add("GRAF patch model", &prefModel_, 0, 1, kModelNames,
        [](void* c, int v) { static_cast<Graf*>(c)->setModel(v == 0); });
    add("GRAF live morph time", &prefMorph_, 0, 6, kMorphNames, nullptr);
    add("GRAF song starts with Play", &prefSongPlay_, 0, 1, kOffOnS, nullptr);
    add("GRAF keyboard octave", &prefOctave_, -2, 2, kOctNames, nullptr);
    add("GRAF pad velocity", &prefVel_, 0, 1, kOffOnS, nullptr);
    add("GRAF cable sag", &prefSag_, 0, 1, kOffOnS, nullptr);
    return n;
}

void Graf::save() {
    savePrefs();
    if (!ctx_->fs) return;
    char path[96];
    std::snprintf(path, sizeof path, "%s/autosave.grf", ctx_->home);
    savePatch(path);
}

bool Graf::savePatch(const char* path) {
    if (!ctx_->fs) return false;
    const uint32_t n = eng_.serialize(fileBuf_, kFileBuf);
    if (!n) return false;
    char tmp[112];
    std::snprintf(tmp, sizeof tmp, "%s.tmp", path);
    const lab::IStorage::Handle h = ctx_->fs->open(tmp, lab::IStorage::kWrite);
    if (h < 0) return false;
    uint32_t put = 0;
    while (put < n) {
        const int32_t w = ctx_->fs->write(h, fileBuf_ + put, n - put);
        if (w <= 0) break;
        put += uint32_t(w);
    }
    const bool ok = put == n;
    ctx_->fs->close(h);
    if (!ok) return false;
    ctx_->fs->remove(path);
    return ctx_->fs->rename(tmp, path);
}

bool Graf::loadPatch(const char* path) {
    if (!ctx_->fs) return false;
    const lab::IStorage::Handle h = ctx_->fs->open(path, lab::IStorage::kRead);
    if (h < 0) return false;
    const int32_t size = ctx_->fs->size(h);
    bool ok = false;
    if (size > 0 && uint32_t(size) <= kFileBuf) {   // reads may come back short: loop
        uint32_t got = 0;
        while (got < uint32_t(size)) {
            const int32_t r = ctx_->fs->read(h, fileBuf_ + got, uint32_t(size) - got);
            if (r <= 0) break;
            got += uint32_t(r);
        }
        ok = got == uint32_t(size);
    }
    ctx_->fs->close(h);
    if (!ok) return false;
    if (!eng_.deserialize(fileBuf_, uint32_t(size))) return false;
    prefModel_ = eng_.rack ? 0 : 1;
    scanSamples();
    sel_ = -1;
    selCable_ = -1;
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(patch_, c);
        if (s >= 0) {
            select(s);
            break;
        }
    }
    rebuildJacks();
    return true;
}

// ------------------------------------------------------------------ demos
namespace {
struct Builder {
    Engine& e;
    int patch;
    int add(int type, int cell, std::initializer_list<float> vals = {}) {
        const int s = e.add(type, patch, cell);
        if (s < 0) return -1;
        int p = 0;
        for (float v : vals) {
            if (p < def(type).nParam && v == v) e.setParamK(s, p, Engine::unmapParam(def(type).p[p], v));
            ++p;
        }
        return s;
    }
    void c(int a, int ao, int b, int bi, float amt = 1.0f) {
        if (a >= 0 && b >= 0) e.connect(a, ao, b, bi, amt);
    }
};
constexpr float kNaN = __builtin_nanf("");
const char* const kDemoNames[] = {"Demo: First light", "Demo: Reticulum", "Demo: West coast", "Demo: Turing drums",
                                  "Demo: Poly keys", "Demo: 8 patches"};
}  // namespace

int Graf::demoCount() { return 6; }
const char* Graf::demoName(int i) { return kDemoNames[(i >= 0 && i < 6) ? i : 0]; }

void Graf::loadDemo(int i) {
    eng_.clear();
    eng_.quiesce();
    eng_.allNotesOff();
    for (int k = 0; k < kSnaps; ++k) eng_.clearSnap(k);
    eng_.rack = i != 5;
    prefModel_ = eng_.rack ? 0 : 1;
    for (int p = 0; p < kPatches; ++p) eng_.mix[p] = MixStrip();
    Builder b{eng_, 0};
    int varyA = -1, varyB = -1;
    switch (i) {
        default:
        case 0: {   // SEQ -> VCO -> LADR -> VCA <- ADSR -> DLY -> OUT
            const int seq = b.add(kSeq, 0, {8, 4, 0.45f, 2, 0, -1});
            const int vco = b.add(kVco, 2, {0, 0, 0.2f, 0.5f});
            const int lfo = b.add(kLfo, 12, {0.2f});
            const int lad = b.add(kLadder, 3, {500, 0.55f, 4, 1.6f});
            const int env = b.add(kAdsr, 14, {0.002f, 0.18f, 0.2f, 0.25f});
            const int vca = b.add(kVca, 4);
            const int dly = b.add(kDelay, 5, {0.35f, 5, 0.45f, 0.6f, 0.3f});
            const int out = b.add(kOut, 6, {0.8f});
            const int mi = b.add(kMidi, 24);   // keys / MIDI transpose the sequence and retrigger
            b.c(seq, 0, vco, 0);
            b.c(seq, 1, env, 0);
            b.c(mi, 0, vco, 0);
            b.c(mi, 1, env, 0);
            b.c(vco, 0, lad, 0);
            b.c(vco, 1, lad, 0, 0.5f);
            b.c(env, 0, lad, 2, 0.6f);
            b.c(lfo, 0, lad, 2, 0.15f);
            b.c(lad, 0, vca, 0);
            b.c(env, 0, vca, 1);
            b.c(vca, 0, dly, 0);
            b.c(dly, 0, out, 0);
            b.c(dly, 1, out, 1);
            varyA = lad;
            varyB = vco;
            break;
        }
        case 1: {   // RETICULUM: rete + two random sources + a manual-gate clock into granular + reverb
            const int clk = b.add(kClk, 0, {0, 0, 0.1f});
            const int eucl = b.add(kEucl, 12, {16, 3, 0, 3});
            const int snh = b.add(kSnh, 24, {0.2f});
            const int lfo = b.add(kLfo, 25, {0.07f});
            const int rete = b.add(kRete, 2, {0.8f, 0.82f, 180, 0.6f, 0.25f, 1, 0.3f, 0.45f, 0.8f});
            const int noi = b.add(kNoise, 36, {0.2f, 0.4f});
            const int grn = b.add(kGrain, 4, {0.18f, 8, 0.3f, 0, 0.5f, 0.2f, 0, 0.45f, 0.8f});
            const int vrb = b.add(kVerb, 5, {0.8f, 0.45f, 0.02f, 0.4f});
            const int out = b.add(kOut, 6, {0.8f});
            b.c(clk, 2, eucl, 1);
            b.c(eucl, 0, rete, 0);
            b.c(eucl, 0, snh, 1);
            b.c(snh, 0, rete, 2, 0.4f);
            b.c(lfo, 0, rete, 3, 0.4f);
            b.c(noi, 3, rete, 1, 0.5f);
            b.c(rete, 0, grn, 0);
            b.c(rete, 1, grn, 1);
            b.c(grn, 0, vrb, 0);
            b.c(grn, 1, vrb, 1);
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyA = rete;
            varyB = grn;
            break;
        }
        case 2: {   // PINB -> CPLX -> LPG (pinged) -> VERB; FUNC cycling into timbre
            const int pin = b.add(kPinb, 0, {4, 0.3f, 0.2f, 5, 2});
            const int cpx = b.add(kCplx, 2, {-1, 0, 1.5f, 0.8f, 0.2f, 0.3f, 0.1f});
            const int fn = b.add(kFunc, 13, {0.8f, 1.6f, 0.2f, 1});
            const int lpg = b.add(kLpg, 3, {2, 0, 0.5f, 0.3f});
            const int vrb = b.add(kVerb, 4, {0.85f, 0.35f, 0.03f, 0.35f});
            const int out = b.add(kOut, 5, {0.85f});
            b.c(pin, 0, cpx, 0);
            b.c(pin, 1, lpg, 2);
            b.c(fn, 0, cpx, 2, 0.5f);
            b.c(fn, 0, cpx, 3, 0.3f);
            b.c(cpx, 0, lpg, 0);
            b.c(lpg, 0, vrb, 0);
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyA = cpx;
            varyB = pin;
            break;
        }
        case 3: {   // clock, three euclideans into drums, Turing -> wavetable -> SVF
            const int k1 = b.add(kEucl, 0, {16, 4, 0, 4});
            const int k2 = b.add(kEucl, 12, {16, 3, 4, 4});
            const int k3 = b.add(kEucl, 24, {16, 9, 1, 4, 0.2f});
            const int kick = b.add(kPerc, 1, {0, 0, 0.35f, 0.4f, 0.9f});
            const int snr = b.add(kPerc, 13, {1, 0, 0.2f, 0.5f, 0.6f});
            const int hat = b.add(kPerc, 25, {2, 0, 0.06f, 0.6f, 0.4f});
            const int tur = b.add(kTuring, 36, {0.1f, 8, 6, 1.5f, 0, 4});
            const int wt = b.add(kWtbl, 37, {-1, 0, 1, 0.3f});
            const int lfo = b.add(kLfo, 38, {0.13f});
            const int svf = b.add(kSvf, 39, {900, 0.5f, 3});
            const int ad = b.add(kAd, 40, {0.003f, 0.25f});
            const int vca = b.add(kVca, 41);
            const int mix = b.add(kMix, 3, {0.9f, 0.7f, 0.5f, 0.6f, 1.3f});
            const int out = b.add(kOut, 4, {0.8f});
            b.c(k1, 0, kick, 0);
            b.c(k2, 0, snr, 0);
            b.c(k3, 0, hat, 0);
            b.c(k3, 2, hat, 2);
            b.c(tur, 0, wt, 0);
            b.c(lfo, 1, wt, 1, 0.5f);
            b.c(wt, 0, svf, 0);
            b.c(tur, 2, ad, 0);
            b.c(ad, 0, svf, 2, 0.5f);
            b.c(svf, 0, vca, 0);
            b.c(ad, 0, vca, 1);
            b.c(kick, 0, mix, 0);
            b.c(snr, 0, mix, 1);
            b.c(hat, 0, mix, 2);
            b.c(vca, 0, mix, 3);
            b.c(mix, 0, out, 0);
            varyA = tur;
            varyB = svf;
            break;
        }
        case 4: {   // 4-voice poly from MIDI / the pads
            const int vrb = b.add(kVerb, 10, {0.75f, 0.5f, 0.01f, 0.3f});
            const int out = b.add(kOut, 11, {0.8f});
            for (int v = 0; v < 4; ++v) {
                const int mi = b.add(kMidi, v * 12, {float(v + 1)});
                const int vco = b.add(kVco, v * 12 + 1, {0, v * 0.04f - 0.06f, 0, 0.4f});
                const int svf = b.add(kSvf, v * 12 + 2, {700, 0.35f, 4});
                const int env = b.add(kAdsr, v * 12 + 3, {0.004f, 0.4f, 0.5f, 0.6f});
                const int vca = b.add(kVca, v * 12 + 4);
                b.c(mi, 0, vco, 0);
                b.c(mi, 1, env, 0);
                b.c(vco, 0, svf, 0);
                b.c(env, 0, svf, 2, 0.5f);
                b.c(svf, 0, vca, 0);
                b.c(env, 0, vca, 1, 0.6f);
                b.c(vca, 0, vrb, 0, 0.7f);
                if (v == 0) varyA = svf;
            }
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyB = vrb;
            break;
        }
        case 5: {   // 8 patches: a bass on 1, a pluck on 2, rete on 3, drums on 4
            {
                b.patch = 0;
                const int seq = b.add(kSeq, 0, {8, 3, 0.4f, 6, 0, -2});
                const int vco = b.add(kVco, 1, {-1});
                const int svf = b.add(kSvf, 2, {300, 0.6f, 5, 2});
                const int env = b.add(kAdsr, 13, {0.002f, 0.2f, 0.1f, 0.1f});
                const int vca = b.add(kVca, 3);
                const int out = b.add(kOut, 4, {0.8f});
                b.c(seq, 0, vco, 0);
                b.c(seq, 1, env, 0);
                b.c(vco, 0, svf, 0);
                b.c(env, 0, svf, 2, 0.7f);
                b.c(svf, 0, vca, 0);
                b.c(env, 0, vca, 1);
                b.c(vca, 0, out, 0);
                varyA = svf;
            }
            {
                b.patch = 1;
                const int mrk = b.add(kMarkov, 0, {6, 0.2f, 5, 0, 0, 4});
                const int cpx = b.add(kCplx, 1, {0, 0, 2, 1.2f, 0.1f, 0.2f});
                const int lpg = b.add(kLpg, 2, {2, 0, 0.4f, 0.3f});
                const int out = b.add(kOut, 3, {0.7f, -0.3f});
                b.c(mrk, 0, cpx, 0);
                b.c(mrk, 1, lpg, 2);
                b.c(cpx, 0, lpg, 0);
                b.c(lpg, 0, out, 0);
                varyB = cpx;
            }
            {
                b.patch = 2;
                const int rete = b.add(kRete, 0, {0.7f, 0.7f, 300, 0.4f});
                const int out = b.add(kOut, 1, {0.5f, 0.3f});
                b.c(rete, 0, out, 0);
                b.c(rete, 1, out, 1);
            }
            {
                b.patch = 3;
                const int k1 = b.add(kEucl, 0, {16, 5, 0, 4});
                const int kick = b.add(kPerc, 1, {0, 0, 0.3f});
                const int hat = b.add(kPerc, 13, {2, 0, 0.05f, 0.7f, 0.4f});
                const int out = b.add(kOut, 2, {0.8f});
                b.c(k1, 0, kick, 0);
                b.c(k1, 1, hat, 0);   // the off-steps
                b.c(kick, 0, out, 0);
                b.c(hat, 0, out, 0);
            }
            break;
        }
    }
    eng_.rebuild();
    // snapshot 1 = as built, 2 = a variation, 3 = sparser; song 1 2 1 3
    eng_.storeSnap(0);
    Rand r;
    r.seed(0x5EED + uint32_t(i));
    if (varyA >= 0) eng_.randomize(varyA, r);
    if (varyB >= 0) eng_.randomize(varyB, r);
    eng_.storeSnap(1);
    if (varyA >= 0) eng_.randomize(varyA, r);
    eng_.storeSnap(2);
    eng_.recallSnap(0, 0, now_);
    static const uint8_t order[4] = {0, 1, 0, 2};
    for (int k = 0; k < 4; ++k) {
        eng_.song[k].snap = order[k];
        eng_.song[k].morphBars = 2;
        eng_.song[k].holdBars = 2;
    }
    eng_.songLen = 4;
    eng_.songOn = false;
    patch_ = 0;
    sel_ = -1;
    selCable_ = -1;
    mRow_ = mCol_ = 0;
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(0, c);
        if (s >= 0) {
            select(s);
            break;
        }
    }
    rebuildJacks();
}

// ------------------------------------------------------------------ bench
namespace {
const char* const kBenchNames[] = {"idle",      "first-light", "reticulum", "west-coast", "turing-drums",
                                   "poly-keys", "8-patches",   "fill-synth", "full-fx",   "fill-mixed",
                                   "fill-ctl",  "grain-max",   "rete-max"};
constexpr int kBench = 13;
// Fills the rack up to the CPU budget with the given module types (cycled),
// every input patched from the module before it -- the busiest patch the
// budget admits for that mix.
void fillRack(Engine& e, const int* types, int nTypes) {
    e.clear();
    e.quiesce();
    e.rack = true;
    const int out = e.add(kOut, 7, 47);
    const int clk = e.add(kClk, 7, 46);
    int prev = clk, cell = 0, misses = 0;
    for (int k = 0; misses < nTypes && cell < kMaxSlots - 3; ++k) {
        const int t = types[k % nTypes];
        const int s = e.add(t, cell / kCells, cell % kCells);
        if (s < 0) {
            ++misses;
            continue;
        }
        misses = 0;
        ++cell;
        const ModDef& d = def(t);
        for (int in = 0; in < d.nIn; ++in) {
            const ModDef& ps = def(e.mod(prev).type);
            if (e.connect(prev, in % (ps.nOut ? ps.nOut : 1), s, in, 0.6f) < 0) break;
        }
        if (d.nOut) prev = s;
    }
    if (out >= 0) e.connect(prev, 0, out, 0, 0.5f);
    e.rebuild();
}
}  // namespace

int Graf::benchScenarios() const {
#ifdef GRAF_CAL
    return 2 + kTypeCount + 3;
#else
    return kBench;
#endif
}

const char* Graf::benchName(int i) const {
#ifdef GRAF_CAL
    static char buf[24];
    if (i == 0) return "cal-base";
    if (i == 1) return "cal-cables";
    if (i == 2 + kTypeCount) return "cal-mix4";
    if (i == 3 + kTypeCount) return "cal-vco-saw";
    if (i == 4 + kTypeCount) return "cal-vco-4out";
    std::snprintf(buf, sizeof buf, "cal-%s", def(i - 2).tag);
    return buf;
#else
    return (i >= 0 && i < kBench) ? kBenchNames[i] : "?";
#endif
}

void Graf::benchSetup(int i) {
    benchScenario_ = i;
    eng_.setConcurrent(false);
#ifdef GRAF_CAL
    // base: one VCO. cables: a VCO into 4 CVMX x 3 inputs (12 cables, amount 0.5).
    // module T: a VCO feeding 4 copies of T on all their inputs (amount 0.5).
    eng_.clear();
    eng_.quiesce();
    eng_.rack = true;
    eng_.allOutputs = true;
    const int v = eng_.add(kVco, 0, 0);
    eng_.setParamK(v, 0, 0.5f);
    if (i >= 2 + kTypeCount) {   // VCO variants: pitch patched, no FM, into one MIX (4 cables)
        eng_.allOutputs = false;
        eng_.scopeSlot = -1;
        const int mx = eng_.add(kMix, 0, 9);
        eng_.connect(v, 0, mx, 0, 0.5f);
        eng_.connect(v, 0, mx, 1, 0.5f);
        eng_.connect(v, 0, mx, 2, 0.5f);
        eng_.connect(v, 0, mx, 3, 0.5f);
        if (i > 2 + kTypeCount) {
            eng_.disconnect(eng_.cableFind(v, 0, mx, 1));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 2));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 3));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 0));
            for (int k = 0; k < 4; ++k) {
                const int s = eng_.add(kVco, 0, k + 1);
                eng_.connect(v, 3, s, 0, 0.1f);
                const int no = i == 3 + kTypeCount ? 1 : 4;
                for (int o = 0; o < no; ++o) eng_.connect(s, o, mx, k, 0.25f);
            }
        }
        eng_.rebuild();
        return;
    }
    if (i >= 1) {
        const int t = i == 1 ? kCvmx : i - 2;
        for (int k = 0; k < 4; ++k) {
            const int s = eng_.add(t, 0, k + 1);
            if (s < 0) break;
            const int nIn = def(t).nIn;
            for (int in = 0; in < nIn; ++in) eng_.connect(v, 0, s, in, 0.5f);
        }
    }
    eng_.rebuild();
    for (int k = 0; k < 4; ++k) eng_.noteOn(0, uint8_t(48 + k * 7), 100);
    return;
#endif
    switch (i) {
        case 0:
            eng_.clear();
            eng_.quiesce();
            break;
        case 1: loadDemo(0); break;
        case 2: loadDemo(1); break;
        case 3: loadDemo(2); break;
        case 4: loadDemo(3); break;
        case 5:
            loadDemo(4);
            for (int k = 0; k < 4; ++k) eng_.noteOn(0, uint8_t(48 + k * 4), 100);
            break;
        case 6: {
            loadDemo(5);
            for (int p = 0; p < kPatches; ++p) eng_.noteOn(p, 48, 100);
            break;
        }
        case 7: {
            const int t[] = {kSeq, kVco, kCplx, kLadder, kSvf, kAdsr, kVca, kLfo, kFold, kWtbl, kLpg};
            fillRack(eng_, t, int(sizeof t / sizeof t[0]));
            break;
        }
        case 8: {   // effects: granular, reverbs, delays fed by noise
            eng_.clear();
            eng_.quiesce();
            eng_.rack = true;
            const int n = eng_.add(kNoise, 0, 0);
            const int clk = eng_.add(kClk, 0, 1);
            int prev = n;
            int cell = 2;
            const int fx[] = {kGrain, kVerb, kDelay, kVerb, kDelay, kGrain};
            for (int t : fx) {
                const int s = eng_.add(t, 0, cell++);
                if (s < 0) continue;
                eng_.connect(prev, 0, s, 0, 0.8f);
                eng_.connect(prev, prev == n ? 1 : 1, s, 1, 0.8f);
                if (t == kGrain) eng_.connect(clk, 0, s, 2, 1.0f);
                prev = s;
            }
            const int out = eng_.add(kOut, 0, cell++);
            eng_.connect(prev, 0, out, 0, 0.5f);
            eng_.connect(prev, 1, out, 1, 0.5f);
            eng_.rebuild();
            break;
        }
        case 9: {
            int t[kTypeCount], n = 0;
            for (int k = 0; k < kTypeCount; ++k)
                if (k != kOut) t[n++] = (k * 7) % kTypeCount == kOut ? kVco : (k * 7) % kTypeCount;
            fillRack(eng_, t, n);
            break;
        }
        case 10: {
            const int t[] = {kClk, kDiv, kSeq, kEucl, kTuring, kMarkov, kPinb, kSnh, kQuant, kLfo, kAdsr, kAd,
                             kFunc, kSlew, kLogic, kCvmx, kMidi, kPerc};
            fillRack(eng_, t, int(sizeof t / sizeof t[0]));
            break;
        }
        case 11: {   // granulars at full density and size, pitched up two octaves
            const int t[] = {kGrain};
            fillRack(eng_, t, 1);
            for (int s = 0; s < kMaxSlots; ++s)
                if (eng_.mod(s).used && eng_.mod(s).type == kGrain) {
                    eng_.setParamK(s, 0, 1.0f);
                    eng_.setParamK(s, 1, 1.0f);
                    eng_.setParamK(s, 3, 1.0f);
                    eng_.setParamK(s, 4, 1.0f);
                }
            const int n = eng_.add(kNoise, 6, 0);
            for (int s = 0; s < kMaxSlots; ++s)
                if (eng_.mod(s).used && eng_.mod(s).type == kGrain) eng_.connect(n, 0, s, 0, 1.0f);
            eng_.rebuild();
            break;
        }
        case 12: {
            const int t[] = {kRete};
            fillRack(eng_, t, 1);
            break;
        }
        default: break;
    }
    eng_.quiesce();
}

}  // namespace graf

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<graf::Graf>(); }
}  // namespace lab
