// DATAMATICS -- workstation logic: input, pages, data loader, pieces (dm_ws.h).
// Drawing lives in dm_draw.cpp.
#include "ws/dm_ws.h"

#include <cstdio>
#include <cstring>

namespace dm {

using namespace lab;

namespace {
const char* const kArr[] = {"Free run", "Sections"};
const char* const kEnd[] = {"Loop", "Stop"};
const char* const kMidiModes[] = {"Off", "One channel", "Channel per stream"};
const char* const kJump[] = {"Next bar", "Immediate"};
const char* const kSpeed[] = {"Slow", "Normal", "Fast"};
const char* const kOffOn[] = {"Off", "On"};

// Stream page parameter order (what Up/Down walks).
const uint8_t kStreamOrder[] = {P_SLOT, P_UNIT, P_ORDER, P_START, P_LEN, P_STRIDE, P_SYNC, P_RATE_MS, P_RATE_DIV,
                                P_SOUND, P_THR, P_LO, P_HI, P_SCALE, P_DUR, P_DURMOD, P_SHAPE, P_LEVEL, P_VEL,
                                P_PANMODE, P_PAN, P_STEM, P_MIDI};
constexpr int kStreamRows = int(sizeof kStreamOrder);

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool isFile(const char* name) { return name && name[0] && name[0] != '.'; }
}  // namespace

int streamParamAt(int row) { return kStreamOrder[clampi(row, 0, kStreamRows - 1)]; }
int streamParamRows() { return kStreamRows; }

// ---------------------------------------------------------------------------------
bool Datamatics::init(Context& c) {
    ctx_ = &c;
    theme_.bg = Rgb(0x000000);
    theme_.panel = Rgb(0x0C0C0C);
    theme_.fg = Rgb(0xFFFFFF);
    theme_.dim = Rgb(0x5A5A5A);
    theme_.accent = Rgb(0xFF2A00);    // the single accent: signal red
    theme_.accent2 = Rgb(0xB0B0B0);
    theme_.warn = Rgb(0xFF2A00);
    theme_.ok = Rgb(0xFFFFFF);
    Arena& a = *c.arena;
    if (!eng_.init(a)) return false;
    eng_.bindMidi(&c);
    ent_ = a.make<Entry>(kMaxEntries);
    pieces_ = static_cast<char(*)[24]>(a.alloc(kMaxPieces * 24));
    stage_ = static_cast<uint8_t*>(a.alloc(kStageBytes, 32));
    midiScratch_ = static_cast<MidiEv*>(a.alloc(sizeof(MidiEv) * kMaxMidiEvents));
    wf_ = static_cast<uint8_t*>(a.alloc(kWfW * kWfH));
    pv_ = static_cast<uint8_t*>(a.alloc(kPvW * kPvH));
    mx_ = static_cast<uint8_t*>(a.alloc(kMxW * kMxH));
    builtinBuf_ = static_cast<uint8_t*>(a.alloc(65536));
    if (!ent_ || !pieces_ || !stage_ || !midiScratch_ || !wf_ || !pv_ || !mx_ || !builtinBuf_) return false;
    // built-in data in every slot: the demo piece plays without a card
    loadBuiltinNow(0, 1, kAuto);
    loadBuiltinNow(1, 0, kAuto);
    loadBuiltinNow(2, 2, kAuto);
    loadBuiltinNow(3, 3, kAuto);
    copyStr(dir_, sizeof dir_, "");
    copyStr(pieceName_, sizeof pieceName_, "DEMO");
    if (c.fs) {
        char p[96];
        std::snprintf(p, sizeof p, "%s/Pieces", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/Data", c.home);
        c.fs->mkdir(p);
        // UI settings (waterfall speed, reload at boot): "DMUI <speed> <autoload>"
        std::snprintf(p, sizeof p, "%s/ui.cfg", c.home);
        const int32_t h = c.fs->open(p, IStorage::kRead);
        if (h >= 0) {
            char b[32] = {};
            const int32_t n = c.fs->read(h, b, sizeof b - 1);
            c.fs->close(h);
            if (n >= 8 && !std::strncmp(b, "DMUI ", 5) && b[5] >= '0' && b[5] <= '2' && (b[7] == '0' || b[7] == '1')) {
                setSpeed_ = b[5] - '0';
                setAutoload_ = b[7] - '0';
            }
        }
        // the last piece comes back at boot
        std::snprintf(p, sizeof p, "%s/last.dmx", c.home);
        if (setAutoload_ && c.fs->exists(p)) loadPiece(p);
    }
    setArrange_ = eng_.arrange;
    setEnd_ = eng_.endMode;
    setMidi_ = eng_.midiMode;
    setJump_ = eng_.jumpQuant;
    return true;
}

void Datamatics::loadBuiltinNow(int slot, int b, int mode) {
    // A built-in set read in AUTO keeps its own form; a forced mode reads its
    // values as a byte file through that mode (e.g. @LFSR as BITS).
    Slot& sl = eng_.slot[slot];
    __atomic_store_n(&sl.n, 0u, __ATOMIC_RELEASE);
    DecodeOut o;
    o.cap = kMaxValues;
    if (mode == kAuto || !builtinBuf_) {
        o.v = sl.v;
        o.aux = sl.aux;
        makeBuiltin(b, o);
    } else {
        o.v = builtinBuf_;   // own buffer: a card load may be using stage_
        o.aux = nullptr;
        o.cap = 65536;
        const uint32_t n = makeBuiltin(b, o);
        DecodeOut d;
        d.v = sl.v;
        d.aux = sl.aux;
        d.cap = kMaxValues;
        decode(builtinName(b), builtinBuf_, n, Mode(mode), d, midiScratch_);
        o = d;
    }
    sl.width = o.width ? o.width : 256;
    sl.mode = o.mode;
    sl.srcW = o.srcW;
    sl.srcH = o.srcH;
    sl.wantMode = uint8_t(mode);
    copyStr(sl.path, sizeof sl.path, builtinName(b));
    __atomic_store_n(&sl.n, o.n, __ATOMIC_RELEASE);
    ++slotGen_[slot];
}

// ---- loader: a file is read in chunks across main-loop passes, then decoded --------
void Datamatics::requestLoad(int slot, const char* path, int mode) {
    if (slot < 0 || slot >= kSlots || !path || !path[0]) return;
    const int b = builtinIndex(path);
    if (b >= 0) {
        loadBuiltinNow(slot, b, mode);
        return;
    }
    if (jobCount_ >= kJobQ) return;
    LoadJob& j = jobs_[jobCount_++];
    copyStr(j.path, sizeof j.path, path);
    j.slot = slot;
    j.mode = mode;
}

void Datamatics::pumpLoader() {
    if (!job_.active) {
        if (!jobCount_) return;
        const LoadJob j = jobs_[0];
        for (int i = 1; i < jobCount_; ++i) jobs_[i - 1] = jobs_[i];
        --jobCount_;
        Slot& sl = eng_.slot[j.slot];
        copyStr(sl.path, sizeof sl.path, j.path);
        sl.wantMode = uint8_t(j.mode);
        if (!ctx_->fs) {
            std::snprintf(status_, sizeof status_, "NO CARD: %s", j.path);
            statusUntil_ = nowMs_ + 3000;
            return;
        }
        const int32_t h = ctx_->fs->open(j.path, IStorage::kRead);
        if (h < 0) {
            std::snprintf(status_, sizeof status_, "MISSING %s", j.path);
            statusUntil_ = nowMs_ + 3000;
            return;
        }
        job_.active = true;
        copyStr(job_.path, sizeof job_.path, j.path);
        job_.slot = j.slot;
        job_.mode = j.mode;
        job_.handle = h;
        const int32_t sz = ctx_->fs->size(h);
        job_.size = sz > 0 ? uint32_t(sz) : 0;
        job_.limit = job_.size < kStageBytes ? job_.size : kStageBytes;
        job_.pos = 0;
    }
    // ~32 KB per pass keeps the UI and MIDI moving while a big file loads
    uint32_t budget = 32768;
    while (job_.pos < job_.limit && budget) {
        uint32_t want = job_.limit - job_.pos;
        if (want > budget) want = budget;
        const int32_t got = ctx_->fs->read(job_.handle, stage_ + job_.pos, want);
        if (got <= 0) {
            job_.limit = job_.pos;
            break;
        }
        job_.pos += uint32_t(got);
        budget -= uint32_t(got) < budget ? uint32_t(got) : budget;
    }
    if (job_.pos >= job_.limit) finishJob();
}

void Datamatics::finishJob() {
    ctx_->fs->close(job_.handle);
    job_.active = false;
    Slot& sl = eng_.slot[job_.slot];
    __atomic_store_n(&sl.n, 0u, __ATOMIC_RELEASE);   // audio reads nothing while we rewrite
    DecodeOut o;
    o.v = sl.v;
    o.aux = sl.aux;
    o.cap = kMaxValues;
    const char* name = job_.path;
    for (const char* p = job_.path; *p; ++p)
        if (*p == '/') name = p + 1;
    decode(name, stage_, job_.pos, Mode(job_.mode), o, midiScratch_);
    sl.width = o.width ? o.width : 256;
    sl.mode = o.mode;
    sl.srcW = o.srcW;
    sl.srcH = o.srcH;
    __atomic_store_n(&sl.n, o.n, __ATOMIC_RELEASE);
    ++slotGen_[job_.slot];
    std::snprintf(status_, sizeof status_, "S%d < %.30s  %s %lu", job_.slot + 1, name, modeName(o.mode),
                  static_cast<unsigned long>(o.n));
    statusUntil_ = nowMs_ + 3000;
    if (job_.size > kStageBytes) {
        std::snprintf(status_, sizeof status_, "S%d < %.24s first 4 MB", job_.slot + 1, name);
    }
}

void Datamatics::finishLoadsNow() {
    for (int guard = 0; guard < 100000 && loadBusy(); ++guard) pumpLoader();
}

// ---- browser ---------------------------------------------------------------------
void Datamatics::scanDir() {
    entCount_ = 0;
    // built-ins first at the card root
    if (!dir_[0])
        for (int b = 0; b < kBuiltinCount && entCount_ < kMaxEntries; ++b) {
            Entry& e = ent_[entCount_++];
            copyStr(e.name, sizeof e.name, builtinName(b));
            e.size = 0;
            e.dir = false;
        }
    else {
        Entry& e = ent_[entCount_++];
        copyStr(e.name, sizeof e.name, "..");
        e.size = 0;
        e.dir = true;
    }
    if (ctx_->fs) {
        struct C {
            Datamatics* w;
        } c{this};
        ctx_->fs->listDir(dir_[0] ? dir_ : "/",
                          [](const IStorage::DirEntry& d, void* p) {
                              Datamatics* w = static_cast<C*>(p)->w;
                              if (!isFile(d.name)) return true;
                              if (w->entCount_ >= kMaxEntries) return false;
                              Entry& e = w->ent_[w->entCount_++];
                              copyStr(e.name, sizeof e.name, d.name);
                              e.size = d.size;
                              e.dir = d.isDir;
                              return true;
                          },
                          &c);
    }
    // folders first, then files; each alphabetical (insertion sort, <=160)
    const int first = dir_[0] ? 1 : kBuiltinCount;
    for (int i = first + 1; i < entCount_; ++i) {
        Entry t = ent_[i];
        int j = i - 1;
        auto less = [](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir;
            for (int k = 0;; ++k) {
                char x = a.name[k], y = b.name[k];
                if (x >= 'a' && x <= 'z') x = char(x - 32);
                if (y >= 'a' && y <= 'z') y = char(y - 32);
                if (x != y) return x < y;
                if (!x) return false;
            }
        };
        while (j >= first && less(t, ent_[j])) {
            ent_[j + 1] = ent_[j];
            --j;
        }
        ent_[j + 1] = t;
    }
    if (entSel_ >= entCount_) entSel_ = entCount_ ? entCount_ - 1 : 0;
}

void Datamatics::enterEntry() {
    if (entSel_ < 0 || entSel_ >= entCount_) return;
    const Entry& e = ent_[entSel_];
    if (e.dir) {
        if (!std::strcmp(e.name, "..")) {
            char* slash = std::strrchr(dir_, '/');
            if (slash) *slash = 0;
            else dir_[0] = 0;
        } else {
            char nd[128];
            if (dir_[0]) std::snprintf(nd, sizeof nd, "%s/%s", dir_, e.name);
            else std::snprintf(nd, sizeof nd, "%s", e.name);
            copyStr(dir_, sizeof dir_, nd);
        }
        entSel_ = 0;
        entTop_ = 0;
        scanDir();
        return;
    }
    if (e.name[0] == '@') {
        requestLoad(target_, e.name, loadMode_);
    } else {
        char path[128];
        if (dir_[0]) std::snprintf(path, sizeof path, "%s/%s", dir_, e.name);
        else std::snprintf(path, sizeof path, "%s", e.name);
        requestLoad(target_, path, loadMode_);
    }
}

// ---- pieces ------------------------------------------------------------------------
void Datamatics::scanPieces() {
    pieceCount_ = 0;
    if (!ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/Pieces", ctx_->home);
    struct C {
        Datamatics* w;
    } c{this};
    ctx_->fs->listDir(p,
                      [](const IStorage::DirEntry& d, void* q) {
                          Datamatics* w = static_cast<C*>(q)->w;
                          if (d.isDir || !endsWithCi(d.name, ".dmx")) return true;
                          if (w->pieceCount_ >= kMaxPieces) return false;
                          char* n = w->pieces_[w->pieceCount_++];
                          copyStr(n, 24, d.name);
                          const size_t l = std::strlen(n);
                          if (l >= 4) n[l - 4] = 0;
                          return true;
                      },
                      &c);
    if (pieceSel_ >= pieceCount_) pieceSel_ = pieceCount_ ? pieceCount_ - 1 : 0;
}

bool Datamatics::savePiece(const char* path) {
    if (!ctx_->fs) return false;
    static char buf[4096];
    const uint32_t n = writePiece(eng_, ctx_->bpm(), buf, sizeof buf);
    if (!n) return false;
    const int32_t h = ctx_->fs->open(path, IStorage::kWrite);
    if (h < 0) return false;
    const bool ok = ctx_->fs->write(h, buf, n) == int32_t(n);
    ctx_->fs->close(h);
    return ok;
}

bool Datamatics::loadPiece(const char* path) {
    if (!ctx_->fs) return false;
    const int32_t h = ctx_->fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    static char buf[8192];
    uint32_t n = 0;
    for (;;) {
        const int32_t got = ctx_->fs->read(h, buf + n, uint32_t(sizeof buf - 1 - n));
        if (got <= 0) break;
        n += uint32_t(got);
        if (n >= sizeof buf - 1) break;
    }
    ctx_->fs->close(h);
    buf[n] = 0;
    float bpm = ctx_->bpm();
    char oldPaths[kSlots][128];
    uint8_t oldModes[kSlots];
    for (int i = 0; i < kSlots; ++i) {
        copyStr(oldPaths[i], sizeof oldPaths[i], eng_.slot[i].path);
        oldModes[i] = eng_.slot[i].wantMode;
        eng_.slot[i].path[0] = 0;
    }
    if (!readPiece(eng_, buf, n, &bpm)) {
        for (int i = 0; i < kSlots; ++i) copyStr(eng_.slot[i].path, sizeof eng_.slot[i].path, oldPaths[i]);
        return false;
    }
    if (bpm >= 20.0f && bpm <= 400.0f) ctx_->setBpm(bpm);
    for (int i = 0; i < kSlots; ++i) {
        char p[128];
        copyStr(p, sizeof p, eng_.slot[i].path);
        const int m = eng_.slot[i].wantMode;
        if (!p[0]) {
            eng_.slot[i].n = 0;
            ++slotGen_[i];
            continue;
        }
        if (!std::strcmp(p, oldPaths[i]) && m == oldModes[i] && eng_.slot[i].n) continue;   // already in memory
        requestLoad(i, p, m);
    }
    setArrange_ = eng_.arrange;
    setEnd_ = eng_.endMode;
    setMidi_ = eng_.midiMode;
    setJump_ = eng_.jumpQuant;
    eng_.reset();
    return true;
}

void Datamatics::newPiece() {
    eng_.defaults();
    loadBuiltinNow(0, 1, kAuto);
    loadBuiltinNow(1, 0, kAuto);
    loadBuiltinNow(2, 2, kAuto);
    loadBuiltinNow(3, 3, kAuto);
    copyStr(pieceName_, sizeof pieceName_, "DEMO");
}

void Datamatics::save() {
    if (!ctx_ || !ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/last.dmx", ctx_->home);
    savePiece(p);
    std::snprintf(p, sizeof p, "%s/ui.cfg", ctx_->home);
    const int32_t h = ctx_->fs->open(p, IStorage::kWrite);
    if (h >= 0) {
        char b[16];
        const int n = std::snprintf(b, sizeof b, "DMUI %d %d\n", setSpeed_, setAutoload_);
        ctx_->fs->write(h, b, uint32_t(n));
        ctx_->fs->close(h);
    }
}

// ---- settings rows ----------------------------------------------------------------------
int Datamatics::settings(SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, void (*fn)(void*, int)) {
        if (n >= cap) return;
        SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.onChange = fn;
        r.ctx = this;
    };
    add("Arrangement", &setArrange_, 0, 1, kArr, [](void* c, int v) { static_cast<Datamatics*>(c)->eng_.arrange = v; });
    add("At the end of the piece", &setEnd_, 0, 1, kEnd,
        [](void* c, int v) { static_cast<Datamatics*>(c)->eng_.endMode = v; });
    add("MIDI out of the streams", &setMidi_, 0, 2, kMidiModes,
        [](void* c, int v) { static_cast<Datamatics*>(c)->eng_.midiMode = v; });
    add("Section jump", &setJump_, 0, 1, kJump, [](void* c, int v) { static_cast<Datamatics*>(c)->eng_.jumpQuant = v; });
    add("Waterfall speed", &setSpeed_, 0, 2, kSpeed, nullptr);
    add("Reload last piece at boot", &setAutoload_, 0, 1, kOffOn, nullptr);
    return n;
}

// ---- transport / audio --------------------------------------------------------------------
void Datamatics::onTransport(bool playing) {
    if (!playing) eng_.wantStop = false;
}

void Datamatics::render(const Transport& t, const AudioIn&, float* l, float* r) {
    eng_.render(t, l, r, stemL_, stemR_);
    for (int i = 0; i < 4; ++i) ctx_->stem(i, stemL_[i], stemR_[i]);
}

void Datamatics::onMidi(const MidiMsg& m) {
    // Notes 36..43 audition streams 1..8; 48.. jump to sections; CC 1..8 = stream levels.
    if (m.type() == 0x90 && m.d2) {
        if (m.d1 >= 36 && m.d1 < 36 + kStreams) eng_.st[m.d1 - 36].audition = m.d2;
        else if (m.d1 >= 48 && m.d1 < 48 + kMaxSections && m.d1 - 48 < eng_.secCount) {
            if (ctx_->playing()) eng_.jumpReq = m.d1 - 48;
            else eng_.startSec = m.d1 - 48;
        }
    } else if (m.type() == 0xB0 && m.d1 >= 1 && m.d1 <= kStreams) {
        eng_.st[m.d1 - 1].p[P_LEVEL] = m.d2 * 100 / 127;
    }
}

void Datamatics::update(uint32_t ms) {
    nowMs_ = ms;
    pumpLoader();
    if (eng_.wantStop && ctx_->playing()) ctx_->setPlaying(false);
    drainEvents(ms);
    uploadVisuals();
    if (page_ == PG_DATA && !entCount_) scanDir();
}

// ---- input --------------------------------------------------------------------------------
void Datamatics::setPage(int p) {
    if (p != page_) lastPage_ = page_;
    page_ = p;
    delArm_ = false;
    if (p == PG_DATA) {
        target_ = eng_.st[sel_].p[P_SLOT] & 3;
        scanDir();
    }
    if (p == PG_PIECE) scanPieces();
    wfAllDirty_ = true;
}

void Datamatics::editParam(int s, int p, int delta, bool coarse) {
    const ParamInfo& pi = paramInfo(p);
    int step = 1;
    int v = eng_.st[s].p[p];
    if (!pi.names) {
        if (p == P_LO || p == P_HI) {   // frequencies: proportional steps
            step = v / (coarse ? 8 : 64);
            if (step < 1) step = 1;
        } else if (p == P_DUR || p == P_RATE_MS) {
            step = v / (coarse ? 4 : 20);
            if (step < 1) step = 1;
        } else if (coarse) {
            step = 10;
        }
    }
    v = clampi(v + delta * step, pi.min, pi.max);
    eng_.st[s].p[p] = v;
}

void Datamatics::onEvent(const Event& e) {
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        padEvent(e);
        return;
    }
    if (e.type == Event::kPress) {
        if (e.id <= btn::F6) {
            static const uint8_t map[] = {PG_MATRIX, PG_STREAM, PG_DATA, PG_SECTIONS, PG_MIXER, PG_PIECE};
            setPage(map[e.id]);
            return;
        }
        // module keys as shortcuts
        if (e.id == btn::File || e.id == btn::SampleLoader) { setPage(PG_DATA); return; }
        if (e.id == btn::Song) { setPage(PG_SECTIONS); return; }
        if (e.id == btn::Master) { setPage(PG_MIXER); return; }
        if (e.id == btn::Pattern) { setPage(PG_MATRIX); return; }
        if (e.id == btn::InstrParams) { setPage(PG_STREAM); return; }
        if (e.id == btn::Performance) { setPage(page_ == PG_VIEW ? lastPage_ : PG_VIEW); return; }
    }
    switch (page_) {
        case PG_MATRIX: matrixEvent(e); break;
        case PG_STREAM: streamEvent(e); break;
        case PG_DATA: dataEvent(e); break;
        case PG_SECTIONS: sectionsEvent(e); break;
        case PG_MIXER: mixerEvent(e); break;
        case PG_PIECE: pieceEvent(e); break;
        default:
            if (e.type == Event::kPress && (e.id == btn::F8 || e.id == btn::F7)) setPage(lastPage_);
            break;
    }
}

void Datamatics::padEvent(const Event& e) {
    // Pads (all pages): rows 0..3 = the 4 streams of the bank; cols 0..7 = the next
    // 8 units of that stream's read head (press = audition that unit). Col 8 =
    // mute, col 9 = solo, col 10 = select (+ open Stream page with Shift),
    // col 11 = section launch (row 0..3 = section bank) / bank switch on Shift.
    if (e.type != Event::kPadDown) return;
    const int row = padRow(e.id), col = padCol(e.id);
    const int s = bank_ * 4 + row;
    if (col < 8) {
        eng_.st[s].audOff = uint16_t(col);
        eng_.st[s].audition = uint8_t(e.velocity ? e.velocity : 100);
        sel_ = s;
        return;
    }
    if (col == 8) { eng_.st[s].p[P_MUTE] ^= 1; return; }
    if (col == 9) { eng_.st[s].p[P_SOLO] ^= 1; return; }
    if (col == 10) {
        sel_ = s;
        if (e.shift) setPage(PG_STREAM);
        return;
    }
    if (col == 11) {
        if (e.shift) {
            bank_ ^= 1;
            return;
        }
        // section launch: rows 0..3 = sections (secSel_ page of 4)
        const int base = (secSel_ / 4) * 4;
        const int target = base + row;
        if (target < eng_.secCount) {
            if (ctx_->playing()) eng_.jumpReq = target;
            else eng_.startSec = target;
            secSel_ = target;
        }
    }
}

void Datamatics::matrixEvent(const Event& e) {
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: sel_ = (sel_ + kStreams - 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Down: sel_ = (sel_ + 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Left: editParam(sel_, P_START, -1, e.shift); break;
            case btn::Right: editParam(sel_, P_START, 1, e.shift); break;
            case btn::EncPush: setPage(PG_STREAM); break;
            case btn::F7: eng_.st[sel_].p[P_MUTE] ^= 1; break;
            case btn::F8: setPage(PG_VIEW); break;
            case btn::Delete: eng_.st[sel_].cue = 0; break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        // encoder: density (threshold) of the selected stream; Shift + encoder: its rate
        if (e.shift) editParam(sel_, eng_.st[sel_].p[P_SYNC] ? P_RATE_DIV : P_RATE_MS, e.delta, false);
        else editParam(sel_, P_THR, -e.delta * 4, false);
    }
}

void Datamatics::streamEvent(const Event& e) {
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: streamParam_ = (streamParam_ + kStreamRows - 1) % kStreamRows; break;
            case btn::Down: streamParam_ = (streamParam_ + 1) % kStreamRows; break;
            case btn::Left: editParam(sel_, kStreamOrder[streamParam_], -1, e.shift); break;
            case btn::Right: editParam(sel_, kStreamOrder[streamParam_], 1, e.shift); break;
            case btn::F7: sel_ = (sel_ + kStreams - 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::F8: sel_ = (sel_ + 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Copy:
                for (int p = 0; p < P_COUNT; ++p) clip_[p] = eng_.st[sel_].p[p];
                clipValid_ = true;
                ctx_->toast("Stream copied");
                break;
            case btn::Insert:
                if (clipValid_) {
                    for (int p = 0; p < P_COUNT; ++p) eng_.st[sel_].p[p] = clip_[p];
                    ctx_->toast("Stream pasted");
                }
                break;
            case btn::Delete: {
                const int p = kStreamOrder[streamParam_];
                eng_.st[sel_].p[p] = paramInfo(p).def;
                break;
            }
            case btn::EncPush: eng_.st[sel_].audition = 110; break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        editParam(sel_, kStreamOrder[streamParam_], e.delta, e.shift);
    }
}

void Datamatics::dataEvent(const Event& e) {
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: entSel_ = entSel_ > 0 ? entSel_ - 1 : 0; break;
            case btn::Down: entSel_ = entSel_ + 1 < entCount_ ? entSel_ + 1 : entSel_; break;
            case btn::Left: target_ = (target_ + kSlots - 1) % kSlots; break;
            case btn::Right: target_ = (target_ + 1) % kSlots; break;
            case btn::EncPush:
            case btn::F8: enterEntry(); break;
            case btn::F7: loadMode_ = (loadMode_ + 1) % kModeCount; break;
            case btn::Delete: {   // reload the slot with the current mode
                Slot& sl = eng_.slot[target_];
                if (sl.path[0]) requestLoad(target_, sl.path, loadMode_);
                break;
            }
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        entSel_ = clampi(entSel_ + e.delta * (e.shift ? 8 : 1), 0, entCount_ ? entCount_ - 1 : 0);
    }
    const int vis = 12;
    if (entSel_ < entTop_) entTop_ = entSel_;
    if (entSel_ >= entTop_ + vis) entTop_ = entSel_ - vis + 1;
}

void Datamatics::sectionsEvent(const Event& e) {
    Engine& g = eng_;
    Section& c = g.sec[secSel_ < g.secCount ? secSel_ : 0];
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: secSel_ = secSel_ > 0 ? secSel_ - 1 : 0; break;
            case btn::Right: secSel_ = secSel_ + 1 < g.secCount ? secSel_ + 1 : secSel_; break;
            case btn::Up: secField_ = (secField_ + 3) % 4; break;
            case btn::Down: secField_ = (secField_ + 1) % 4; break;
            case btn::Insert:
                if (g.secCount < kMaxSections) {
                    const int at = g.secCount ? secSel_ + 1 : 0;
                    for (int i = g.secCount; i > at; --i) g.sec[i] = g.sec[i - 1];
                    g.sec[at] = g.secCount ? g.sec[secSel_] : Section();
                    g.secCount = g.secCount + 1;
                    secSel_ = at;
                }
                break;
            case btn::Delete:
                if (g.secCount > 1) {
                    for (int i = secSel_; i + 1 < g.secCount; ++i) g.sec[i] = g.sec[i + 1];
                    g.secCount = g.secCount - 1;
                    if (secSel_ >= g.secCount) secSel_ = g.secCount - 1;
                    if (g.curSec >= g.secCount) g.jumpReq = 0;
                }
                break;
            case btn::EncPush:
            case btn::F7:
                if (ctx_->playing()) g.jumpReq = secSel_;
                else g.startSec = secSel_;
                break;
            case btn::F8: {
                g.arrange ^= 1;
                setArrange_ = g.arrange;
                break;
            }
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        const int d = e.delta;
        switch (secField_) {
            case 0: c.type = (c.type + T_COUNT + d) % T_COUNT; break;
            case 1: c.bars = clampi(c.bars + d * (e.shift ? 4 : 1), 1, 64); break;
            case 2: {   // mask: encoder walks the bit, Shift+encoder toggles
                if (e.shift) c.mask ^= 1 << maskBit_;
                else maskBit_ = clampi(maskBit_ + d, 0, 7);
                break;
            }
            default: c.depth = clampi(c.depth + d * (e.shift ? 10 : 1), 0, 100); break;
        }
    }
    if (e.type == Event::kPress && e.id == btn::Note && secField_ == 2) c.mask ^= 1 << maskBit_;
}

void Datamatics::mixerEvent(const Event& e) {
    static const uint8_t fields[] = {P_LEVEL, P_PAN, P_PANMODE, P_STEM, P_MUTE, P_SOLO, P_MIDI};
    constexpr int nf = int(sizeof fields);
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: sel_ = (sel_ + kStreams - 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Right: sel_ = (sel_ + 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Up: mixField_ = (mixField_ + nf - 1) % nf; break;
            case btn::Down: mixField_ = (mixField_ + 1) % nf; break;
            case btn::F7: eng_.st[sel_].p[P_MUTE] ^= 1; break;
            case btn::F8: eng_.st[sel_].p[P_SOLO] ^= 1; break;
            case btn::EncPush: eng_.st[sel_].audition = 110; break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        editParam(sel_, fields[mixField_], e.delta, e.shift);
    }
}

void Datamatics::pieceEvent(const Event& e) {
    char path[128];
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: pieceSel_ = pieceSel_ > 0 ? pieceSel_ - 1 : 0; delArm_ = false; break;
            case btn::Down: pieceSel_ = pieceSel_ + 1 < pieceCount_ ? pieceSel_ + 1 : pieceSel_; delArm_ = false; break;
            case btn::F7: {   // save as a new numbered piece
                if (!ctx_->fs) { ctx_->toast("No card"); break; }
                int n = 1;
                for (; n < 1000; ++n) {
                    std::snprintf(path, sizeof path, "%s/Pieces/DM-%03d.dmx", ctx_->home, n);
                    if (!ctx_->fs->exists(path)) break;
                }
                if (savePiece(path)) {
                    std::snprintf(pieceName_, sizeof pieceName_, "DM-%03d", n);
                    char m[48];
                    std::snprintf(m, sizeof m, "Saved %s", pieceName_);
                    ctx_->toast(m);
                } else {
                    ctx_->toast("Cannot write to the card");
                }
                scanPieces();
                break;
            }
            case btn::F8:
            case btn::EncPush:
                if (pieceSel_ < pieceCount_) {
                    std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                    if (loadPiece(path)) {
                        copyStr(pieceName_, sizeof pieceName_, pieces_[pieceSel_]);
                        ctx_->toast("Piece loaded");
                    } else {
                        ctx_->toast("Not a DATAMATICS piece");
                    }
                }
                break;
            case btn::Copy:   // overwrite the selected piece
                if (pieceSel_ < pieceCount_) {
                    std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                    if (savePiece(path)) {
                        copyStr(pieceName_, sizeof pieceName_, pieces_[pieceSel_]);
                        ctx_->toast("Overwritten");
                    }
                }
                break;
            case btn::Delete:
                if (pieceSel_ < pieceCount_ && ctx_->fs) {
                    if (!delArm_) {
                        delArm_ = true;
                        ctx_->toast("Delete again to remove the file");
                    } else {
                        std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                        ctx_->fs->remove(path);
                        delArm_ = false;
                        scanPieces();
                    }
                }
                break;
            case btn::Insert:
                newPiece();
                ctx_->toast("New piece (demo data)");
                break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        pieceSel_ = clampi(pieceSel_ + e.delta, 0, pieceCount_ ? pieceCount_ - 1 : 0);
        delArm_ = false;
    }
}

// ---- bench ------------------------------------------------------------------------------
const char* Datamatics::benchName(int i) const {
    static const char* const n[] = {"demo", "dense_sine", "ms_clicks", "noise_wall", "max_events"};
    return (i >= 0 && i < 5) ? n[i] : "?";
}

void Datamatics::benchSetup(int i) {
    newPiece();
    eng_.reset();
    eng_.midiMode = 0;
    if (i == 0) return;
    eng_.arrange = 0;   // free run: every stream sounds
    for (int s = 0; s < kStreams; ++s) {
        int* p = eng_.st[s].p;
        p[P_SLOT] = 2;   // LFSR: dense random bytes
        p[P_UNIT] = U_BYTE;
        p[P_THR] = 0;
        p[P_MUTE] = p[P_SOLO] = 0;
        p[P_SHAPE] = SH_DECAY;
        p[P_PANMODE] = PM_DATA;
        p[P_PAN] = 100;
        p[P_LEVEL] = 30;
        p[P_SYNC] = 0;
        p[P_STEM] = s & 3;
        switch (i) {
            case 1:   // 8 streams x 4 sine voices always sounding (long sines retriggered every 2 ms)
                p[P_SOUND] = S_SINE; p[P_RATE_MS] = 2; p[P_DUR] = 20000; p[P_LO] = 40; p[P_HI] = 18000;
                break;
            case 2:   // 1 ms clicks on every stream (8 events per ms, 3 ms clicks)
                p[P_SOUND] = S_CLICK; p[P_RATE_MS] = 1; p[P_DUR] = 30; p[P_SHAPE] = SH_FADE;
                break;
            case 3:   // noise everywhere, long
                p[P_SOUND] = S_NOISE; p[P_RATE_MS] = 3; p[P_DUR] = 20000; p[P_LO] = 200; p[P_HI] = 20000;
                break;
            default:   // maximum event density: 1 ms grid, mixed sounds, cut section
                p[P_SOUND] = s & 3; p[P_RATE_MS] = 1; p[P_DUR] = 20000; p[P_DURMOD] = 100;
                break;
        }
    }
    if (i == 4) {
        eng_.arrange = 1;
        eng_.secCount = 1;
        eng_.sec[0] = Section{T_CUT, 64, 0xFF, 10};
    }
}

}  // namespace dm
