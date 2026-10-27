// GRAF -- workstation: init, prefs, samples, files, settings, update, MIDI.
#include "ws/graf_ws.h"

#include <cstdio>
#include <cstring>

namespace graf {

const char* const kCatNames[kCatCount] = {"SOURCES", "SHAPERS", "MODULATE", "SEQUENCE", "EFFECTS", "IN / OUT", "MACRO"};

namespace {
const char* const kModelNames[] = {"Rack", "8 patches"};
const char* const kMorphNames[] = {"Cut", "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars"};
const float kMorphBars[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
const char* const kOctNames[] = {"-2", "-1", "0", "+1", "+2"};
const char* const kOffOnS[] = {"Off", "On"};

struct Prefs {
    uint32_t magic = 0x47524632u;   // "GRF2"
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
    undoMem_ = static_cast<uint8_t*>(c.arena->alloc(kUndoBytes * kUndo));
    redoMem_ = static_cast<uint8_t*>(c.arena->alloc(kUndoBytes * kUndo));
    stateBuf_ = static_cast<uint8_t*>(c.arena->alloc(kUndoBytes));
    knobName_ = static_cast<char(*)[kMaxParam][14]>(c.arena->alloc(sizeof(char[kMaxParam][14]) * kMaxSlots));
    if (!fileBuf_ || !undoMem_ || !redoMem_ || !stateBuf_ || !knobName_) return false;
    for (int& n : heldNote_) n = -1;
    // the engine tells us about knob and module changes (LABKIT follows them)
    eng_.onKnobCtx = this;
    eng_.onKnob = [](void* x, int slot, int p) { static_cast<Graf*>(x)->knobChanged(slot, p); };
    eng_.onModules = [](void* x) { static_cast<Graf*>(x)->modulesChanged(); };
    registerParams();
    ctx_->browser().reserve(128, 3.0f);
    loadPrefs();
    scanSamples();
    char path[96];
    std::snprintf(path, sizeof path, "%s/autosave.grf", c.home);
    if (!(c.fs && c.fs->exists(path) && loadPatch(path))) loadDemo(0);
    prefModel_ = eng_.rack ? 0 : 1;   // the patch file carries the model
    if (patch_ < 0 || patch_ >= kPatches) patch_ = 0;
    nUndo_ = nRedo_ = 0;
    rebuildJacks();
    reassignPages();
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
    holdCell_ = -1;
    heldKnob_ = -1;
    if (p == page_) return;
    prevPage_ = page_;
    page_ = p;
    confirm_ = false;
    if (p == kPgMatrix || p == kPgModule) rebuildJacks();
    if (p == kPgModule && (sel_ < 0 || !eng_.mod(sel_).used)) {
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
    if (slot != sel_) param_ = 0;
    sel_ = slot;
    if (slot >= 0) {
        cursor_ = eng_.mod(slot).cell;
        eng_.scopeSlot = slot;
        eng_.scopeOut = 0;
        // the first patched output is the most telling one
        for (int c = 0; c < kMaxCables; ++c)
            if (eng_.cable(c).used && eng_.cable(c).src == slot) {
                eng_.scopeOut = eng_.cable(c).srcOut;
                break;
            }
    }
    pagesDirty_ = true;
}

void Graf::setModel(bool rack) {
    prefModel_ = rack ? 0 : 1;
    if (eng_.rack == rack) return;
    checkpoint(1);
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
    // sources: this patch (8 patches) or every canvas (Rack), canvas by canvas, cell by cell
    for (int pp = 0; pp < kPatches; ++pp) {
        const int p = (patch_ + pp) % kPatches;   // this canvas first
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
    if (gestCable_ >= 0 && !eng_.cable(gestCable_).used) gestCable_ = -1;
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
    } else if (!std::strcmp(u, "ms")) {
        std::snprintf(buf, size_t(cap), "%.0f ms", double(v));
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
        if (t == kPinb || t == kMgate) return -1;
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

void Graf::listFiles() {
    for (int i = 0; i < 24; ++i) {
        fileExists_[i] = false;
        if (!ctx_->fs) continue;
        char path[96];
        std::snprintf(path, sizeof path, "%s/Patches/P%02d.grf", ctx_->home, i + 1);
        fileExists_[i] = ctx_->fs->exists(path);
    }
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
    static int want[kMaxSlots];
    static uint32_t since[kMaxSlots];
    for (int s = 0; s < kMaxSlots; ++s) {
        const Module& m = eng_.mod(s);
        if (!m.used || m.type != kSmpl) continue;
        if (want[s] != int(m.v[0])) {
            want[s] = int(m.v[0]);
            since[s] = ms;
        }
        if (eng_.sampleLoaded[s] != want[s] && ms - since[s] >= 250) {
            char dir[96];
            std::snprintf(dir, sizeof dir, "%s/Samples", ctx_->home);
            if (!eng_.loadSample(s, ctx_->fs, dir)) toastErr();
            break;   // one per pass
        }
    }
    // snapshot morphs / song / crossfader at ~100 Hz: a step walks every module and cable
    if (ms - lastMorph_ >= 10) {
        lastMorph_ = ms;
        if (xfOn_) {
            if (xfId_ >= 0) xf_ = ctx_->params().effective(xfId_);   // macros / LFOs move it too
            if (fabsf(xf_ - xfLast_) > 0.002f) {
                xfLast_ = xf_;
                eng_.crossfade(xfA_, xfB_, xf_);
            }
        } else if (eng_.songOn && ctx_->playing()) {
            eng_.songApply(eng_.beat());
        } else {
            eng_.morphStep(ms);
        }
    }
    if ((eng_.morphing() || eng_.songOn || xfOn_) && ms - lastJacks_ >= 200) {
        lastJacks_ = ms;
        rebuildJacks();
    }
    if (pagesDirty_) reassignPages();
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

// ------------------------------------------------------------------ files
// A patch file: the engine's "GRAF" block, then (optional) the LABKIT chunk
// (macros, routes, MIDI learn) behind the tag "LKIT" + u32 length.
bool Graf::savePatch(const char* path) {
    if (!ctx_->fs) return false;
    uint32_t n = eng_.serialize(fileBuf_, kFileBuf);
    if (!n) return false;
    if (n + 8 < kFileBuf) {
        const uint32_t k = ctx_->params().saveChunk(fileBuf_ + n + 8, kFileBuf - n - 8);
        if (k) {
            std::memcpy(fileBuf_ + n, "LKIT", 4);
            fileBuf_[n + 4] = uint8_t(k);
            fileBuf_[n + 5] = uint8_t(k >> 8);
            fileBuf_[n + 6] = uint8_t(k >> 16);
            fileBuf_[n + 7] = uint8_t(k >> 24);
            n += 8 + k;
        }
    }
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
    ctx_->fs->close(h);
    if (put != n) return false;
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
    // the engine block ends with its checksum; find the LABKIT tag after it
    uint32_t engLen = uint32_t(size), chunkAt = 0, chunkLen = 0;
    for (uint32_t i = 16; i + 8 <= uint32_t(size); ++i)
        if (fileBuf_[i] == 'L' && std::memcmp(fileBuf_ + i, "LKIT", 4) == 0) {
            const uint32_t k = uint32_t(fileBuf_[i + 4]) | (uint32_t(fileBuf_[i + 5]) << 8) |
                               (uint32_t(fileBuf_[i + 6]) << 16) | (uint32_t(fileBuf_[i + 7]) << 24);
            if (i + 8 + k == uint32_t(size)) {
                engLen = i;
                chunkAt = i + 8;
                chunkLen = k;
                break;
            }
        }
    checkpoint(1);
    if (!eng_.deserialize(fileBuf_, engLen)) return false;
    clearAllRoutes();
    if (chunkLen) ctx_->params().loadChunk(fileBuf_ + chunkAt, chunkLen);
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
    pagesDirty_ = true;
    return true;
}

}  // namespace graf

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<graf::Graf>(); }
}  // namespace lab
