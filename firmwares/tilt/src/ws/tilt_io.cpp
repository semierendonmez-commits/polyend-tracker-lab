// TILT -- files on the card: patterns, kits, samples, the last session and the
// capture as a MIDI file (tilt_ws.h). Main loop only; every path handles a
// missing card (ctx_->fs == nullptr).
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "ws/tilt_ws.h"

namespace tilt {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool writeFile(ot::IStorage& fs, const char* path, const void* data, uint32_t n) {
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kWrite);
    if (h < 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t done = 0;
    while (done < n) {   // the card may write less than asked
        const int32_t w = fs.write(h, p + done, n - done);
        if (w <= 0) break;
        done += uint32_t(w);
    }
    fs.close(h);
    return done == n;
}
}  // namespace

// Reads a whole file (short reads are normal on the card); -1 = no file.
int32_t readFile(ot::IStorage& fs, const char* path, void* data, uint32_t cap) {
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return -1;
    uint8_t* p = static_cast<uint8_t*>(data);
    uint32_t done = 0;
    while (done < cap) {
        const int32_t r = fs.read(h, p + done, cap - done);
        if (r <= 0) break;
        done += uint32_t(r);
    }
    fs.close(h);
    return int32_t(done);
}

void Tilt::slotPath(bool kit, int slot, char* buf, int cap) const {
    if (kit) std::snprintf(buf, size_t(cap), "%s/Kits/KIT%02d.TLK", ctx_->home, slot + 1);
    else std::snprintf(buf, size_t(cap), "%s/Patterns/PAT%02d.TLT", ctx_->home, slot + 1);
}

void Tilt::scanSlots() {
    for (int i = 0; i < kUserSlots; ++i) {
        slotUsed_[i] = false;
        slotName_[i][0] = 0;
    }
    for (int i = 0; i < kKitSlots; ++i) kitUsed_[i] = false;
    if (!ctx_->fs) return;
    char p[96];
    for (int i = 0; i < kUserSlots; ++i) {
        slotPath(false, i, p, sizeof p);
        uint8_t head[8 + kNameLen];
        const int32_t n = readFile(*ctx_->fs, p, head, sizeof head);
        if (n == int32_t(sizeof head)) {
            slotUsed_[i] = true;
            std::memcpy(slotName_[i], head + 8, kNameLen);
            slotName_[i][kNameLen - 1] = 0;
        }
    }
    for (int i = 0; i < kKitSlots; ++i) {
        slotPath(true, i, p, sizeof p);
        kitUsed_[i] = ctx_->fs->exists(p);
    }
}

bool Tilt::savePattern(int slot) {
    if (!ctx_->fs || slot < 0 || slot >= kUserSlots) return false;
    char p[96];
    slotPath(false, slot, p, sizeof p);
    pat_->bpmX10 = uint16_t(ctx_->bpm() * 10.0f + 0.5f);
    if (!pat_->name[0] || std::strncmp(pat_->name, "U", 1) == 0) std::snprintf(pat_->name, kNameLen, "USER %02d", slot + 1);
    uint32_t n = patternToBytes(*pat_, io_, kIoBytes);
    if (n) {   // + the LABKIT chunk: routes, macros, MIDI learn
        syncRegistry(false);
        n += ctx_->params().saveChunk(io_ + n, kIoBytes - n);
    }
    const bool ok = n && writeFile(*ctx_->fs, p, io_, n);
    if (ok) activeSlot_ = slot;
    scanSlots();
    return ok;
}

void Tilt::swapIn(const Pattern& p) {
    // copied here (the audio may see one torn block: harmless bytes), then the
    // registry follows, then the audio side restarts the walk
    std::memcpy(pat_, &p, sizeof(Pattern));
    for (int i = 0; i < kCh; ++i) sampleDirty_[i] = true;
    ctx_->setBpm(float(pat_->bpmX10) * 0.1f);
    for (int c = 0; c < kCh; ++c) refreshSlots(c);
    syncRegistry(true);
    post(Cmd::kSwap);
}

bool Tilt::loadPattern(int slot) {
    if (!ctx_->fs || slot < 0 || slot >= kUserSlots) return false;
    char p[96];
    slotPath(false, slot, p, sizeof p);
    const int32_t n = readFile(*ctx_->fs, p, io_, kIoBytes);
    if (n <= 0 || !patternFromBytes(*staged_, io_, uint32_t(n))) return false;
    activeSlot_ = slot;
    swapIn(*staged_);
    uint16_t size;
    std::memcpy(&size, io_ + 6, 2);
    if (uint32_t(n) > size + 8u && ctx_->params().loadChunk(io_ + size, uint32_t(n) - size))
        syncRegistry(true);   // routes / macros / MIDI learn from the chunk, the values from the pattern
    return true;
}

void Tilt::loadFactory(int i) {
    factoryPattern(i, *staged_);
    activeSlot_ = -1;
    swapIn(*staged_);
}

bool Tilt::saveKit(int slot) {
    if (!ctx_->fs || slot < 0 || slot >= kKitSlots) return false;
    char p[96];
    slotPath(true, slot, p, sizeof p);
    const uint32_t n = kitToBytes(*pat_, io_, kIoBytes);
    const bool ok = n && writeFile(*ctx_->fs, p, io_, n);
    scanSlots();
    return ok;
}

bool Tilt::loadKit(int slot) {
    if (!ctx_->fs || slot < 0 || slot >= kKitSlots) return false;
    char p[96];
    slotPath(true, slot, p, sizeof p);
    const int32_t n = readFile(*ctx_->fs, p, io_, kIoBytes);
    if (n < 8) return false;
    std::memcpy(staged_, pat_, sizeof(Pattern));
    if (!kitFromBytes(*staged_, io_, uint32_t(n))) return false;
    // only the sound changes (the walk keeps going)
    std::memcpy(pat_->ch, staged_->ch, sizeof(Channel) * kCh);
    pat_->fx = staged_->fx;
    for (int i = 0; i < kCh; ++i) {
        sampleDirty_[i] = true;
        refreshSlots(i);
    }
    syncRegistry(true);
    return true;
}

void Tilt::save() {
    if (!ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/state.tlt", ctx_->home);
    pat_->bpmX10 = uint16_t(ctx_->bpm() * 10.0f + 0.5f);
    const uint32_t n = patternToBytes(*pat_, io_, kIoBytes);
    if (n) writeFile(*ctx_->fs, p, io_, n);
    syncRegistry(false);
    std::snprintf(p, sizeof p, "%s/labkit-patch.bin", ctx_->home);
    ctx_->params().saveFile(ctx_->fs, p);
    std::snprintf(p, sizeof p, "%s/tilt.cfg", ctx_->home);
    const uint8_t cfg[4] = {uint8_t(setAutoload_), uint8_t(setMidiOut_), uint8_t(setStepNotes_), uint8_t(setTrail_)};
    writeFile(*ctx_->fs, p, cfg, 4);
}

// ---- samples ------------------------------------------------------------------------------------------
void Tilt::loadSample(int ch) {
    SampleSlot& s = audio_.userSlot(ch);
    const char* path = pat_->ch[ch].sample;
    if (!path[0]) {
        loaded_[ch][0] = 0;
        return;
    }
    if (s.ready && std::strcmp(loaded_[ch], path) == 0) return;
    s.ready = 0;
    s.frames = 0;
    loaded_[ch][0] = 0;
    if (!ctx_->fs) return;
    const ot::IStorage::Handle h = ctx_->fs->open(path, ot::IStorage::kRead);
    if (h < 0) return;
    ot::WavInfo info;
    ot::StorageReader rd(*ctx_->fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    ctx_->fs->close(h);
    if (!ok) return;
    ot::WavImportOptions opt;
    const uint32_t src = ot::wavSourceFrames(info);
    const uint32_t out = ot::wavResampledFrames(src, info.sampleRate);
    if (out > s.cap - 4 && out) opt.endMarker = uint16_t(uint64_t(65535) * (s.cap - 4) / out);
    ot::WavImportResult res;
    if (ot::wavImportFile(*ctx_->fs, path, opt, s.data, s.cap, res) == ot::FmtStatus::Ok && res.frames > 64) {
        s.frames = res.frames > s.cap ? s.cap : res.frames;
        s.data[s.frames] = s.data[s.frames + 1] = 0;
        s.ready = 1;
        lab::copyStr(loaded_[ch], kPathLen, path);
    }
}

void Tilt::loadSamplesFor() {
    for (int i = 0; i < kCh; ++i) {
        sampleDirty_[i] = false;
        if (pat_->ch[i].type == kEnSample) loadSample(i);
    }
}

// ---- the capture as a standard MIDI file ----------------------------------------------------------------
namespace {
struct MEv {
    uint32_t t;
    uint8_t st, d1, d2, pad;
};
uint8_t* vlq(uint8_t* p, uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = uint8_t(v & 0x7F);
    while (v >>= 7) tmp[n++] = uint8_t(0x80 | (v & 0x7F));
    while (n) *p++ = tmp[--n];
    return p;
}
void be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
}  // namespace

bool Tilt::exportMidi(char* pathOut, int cap) {
    if (pathOut && cap) pathOut[0] = 0;
    if (!ctx_->fs) return false;
    const int n = eng_->clipCount();
    if (n <= 0) return false;
    const CapEvent* clip = eng_->clip();
    // events into the second half of the io buffer, the file into the first
    const int maxEv = int((kIoBytes / 2) / sizeof(MEv));
    MEv* ev = reinterpret_cast<MEv*>(io_ + kIoBytes / 2);
    int m = 0;
    const uint32_t end96 = uint32_t(eng_->clipBars()) * 4u * 96u;
    for (int i = 0; i < n && m + 2 <= maxEv; ++i) {
        const Channel& c = pat_->ch[clip[i].ch];
        const uint8_t mc = uint8_t((c.midiCh ? c.midiCh : 10) - 1);
        uint32_t off = clip[i].t96 + clip[i].len96;
        if (off > end96) off = end96;
        ev[m++] = MEv{clip[i].t96, uint8_t(0x90 | mc), clip[i].note, clip[i].vel, 0};
        ev[m++] = MEv{off, uint8_t(0x80 | mc), clip[i].note, 0, 0};
    }
    // sort by time, note-offs first at equal times (insertion sort: the clip is nearly sorted)
    for (int i = 1; i < m; ++i) {
        const MEv x = ev[i];
        int j = i;
        while (j > 0 && (ev[j - 1].t > x.t || (ev[j - 1].t == x.t && (ev[j - 1].st & 0xF0) == 0x90 && (x.st & 0xF0) == 0x80))) {
            ev[j] = ev[j - 1];
            --j;
        }
        ev[j] = x;
    }
    uint8_t* b = io_;
    std::memcpy(b, "MThd", 4);
    be32(b + 4, 6);
    b[8] = 0;
    b[9] = 0;   // format 0
    b[10] = 0;
    b[11] = 1;  // one track
    b[12] = 0;
    b[13] = 96; // ppq
    std::memcpy(b + 14, "MTrk", 4);
    uint8_t* p = b + 22;
    const uint32_t usPerQ = uint32_t(60000000.0f / (ctx_->bpm() > 20.0f ? ctx_->bpm() : 120.0f));
    p = vlq(p, 0);
    *p++ = 0xFF;
    *p++ = 0x51;
    *p++ = 0x03;
    *p++ = uint8_t(usPerQ >> 16);
    *p++ = uint8_t(usPerQ >> 8);
    *p++ = uint8_t(usPerQ);
    uint32_t last = 0;
    uint8_t* const limit = io_ + kIoBytes / 2 - 16;
    for (int i = 0; i < m && p < limit; ++i) {
        p = vlq(p, ev[i].t - last);
        last = ev[i].t;
        *p++ = ev[i].st;
        *p++ = ev[i].d1 & 0x7F;
        *p++ = ev[i].d2 & 0x7F;
    }
    p = vlq(p, end96 > last ? end96 - last : 0);
    *p++ = 0xFF;
    *p++ = 0x2F;
    *p++ = 0x00;
    be32(b + 18, uint32_t(p - (b + 22)));
    char path[96];
    for (int k = 1; k < 1000; ++k) {
        std::snprintf(path, sizeof path, "%s/Midi/CAP-%03d.MID", ctx_->home, k);
        if (!ctx_->fs->exists(path)) break;
    }
    const bool ok = writeFile(*ctx_->fs, path, io_, uint32_t(p - b));
    if (ok && pathOut) lab::copyStr(pathOut, uint32_t(cap), path);
    if (ok) ++midiFiles_;
    return ok;
}

void Tilt::randomizeNow(uint8_t targets) {
    std::memcpy(undo_, pat_, sizeof(Pattern));
    haveUndo_ = true;
    randomize(*pat_, rng_, targets);
    syncRegistry(true);
    if (targets & kRtSound)
        for (int c = 0; c < kCh; ++c) refreshSlots(c);
}

void Tilt::undoRandom() {
    if (!haveUndo_) return;
    std::memcpy(staged_, pat_, sizeof(Pattern));
    std::memcpy(pat_, undo_, sizeof(Pattern));
    std::memcpy(undo_, staged_, sizeof(Pattern));
    for (int c = 0; c < kCh; ++c) refreshSlots(c);
    syncRegistry(true);
}

// ---- JSON (MANUAL.md "Pattern JSON") ------------------------------------------------------------
bool Tilt::saveJson(const char* path) {
    if (!ctx_->fs) return false;
    pat_->bpmX10 = uint16_t(ctx_->bpm() * 10.0f + 0.5f);
    const uint32_t n = patternToJson(*pat_, reinterpret_cast<char*>(io_), kIoBytes);
    return n && writeFile(*ctx_->fs, path, io_, n);
}

bool Tilt::loadJson(const char* path, char* err, int errCap) {
    if (err && errCap) err[0] = 0;
    if (!ctx_->fs) return false;
    const int32_t n = readFile(*ctx_->fs, path, io_, kIoBytes);
    if (n <= 0) {
        if (err) std::snprintf(err, size_t(errCap), "cannot read the file");
        return false;
    }
    if (uint32_t(n) >= kIoBytes) {
        if (err) std::snprintf(err, size_t(errCap), "file too big");
        return false;
    }
    if (!patternFromJson(*staged_, reinterpret_cast<const char*>(io_), uint32_t(n), toks_, kJsonTokens, err, errCap))
        return false;
    activeSlot_ = -1;
    swapIn(*staged_);
    return true;
}

int Tilt::writeFactoryJson() {
    if (!ctx_->fs) return 0;
    char dir[64], p[128];
    std::snprintf(dir, sizeof dir, "%s/Patterns/Factory", ctx_->home);
    ctx_->fs->mkdir(dir);
    int written = 0;
    for (int i = 0; i < kFactoryCount; ++i) {
        std::snprintf(p, sizeof p, "%s/%02d-%s.json", dir, i + 1, kFactoryNames[i]);
        if (ctx_->fs->exists(p)) continue;
        factoryPattern(i, *staged_);
        const uint32_t n = patternToJson(*staged_, reinterpret_cast<char*>(io_), kIoBytes);
        if (n && writeFile(*ctx_->fs, p, io_, n)) ++written;
    }
    return written;
}

void Tilt::openJsonBrowser() {
    if (!ctx_->fs) {
        ctx_->toast("No card");
        return;
    }
    static char folder[48];
    std::snprintf(folder, sizeof folder, "%s/Patterns", ctx_->home);
    lab::kit::BrowserRequest q;
    q.title = "Load a pattern (JSON)";
    q.folder = folder;
    q.exts = ".json";
    q.preview = false;
    q.ctx = this;
    q.onPick = [](void* c, const char* path) {
        Tilt* t = static_cast<Tilt*>(c);
        lab::copyStr(t->pendingPick_, sizeof t->pendingPick_, path);
        t->pendingKind_ = 2;
    };
    ctx_->browser().open(q);
}

void Tilt::openJsonSave() {
    if (!ctx_->fs) {
        ctx_->toast("No card");
        return;
    }
    lab::kit::KeyboardRequest q;
    q.title = "Save the pattern as JSON";
    q.initial = pat_->name;
    q.maxLen = 24;
    q.ctx = this;
    q.onDone = [](void* c, const char* text) {
        Tilt* t = static_cast<Tilt*>(c);
        if (!text || !text[0]) return;
        char p[128];
        std::snprintf(p, sizeof p, "%s/Patterns/%s.json", t->ctx_->home, text);
        std::snprintf(t->pat_->name, sizeof t->pat_->name, "%.11s", text);
        t->ctx_->toast(t->saveJson(p) ? "Saved as JSON" : "Cannot write the file");
    };
    ctx_->keyboard().open(q);
}

void Tilt::openSampleBrowser(int ch) {
    if (!ctx_->fs) {
        ctx_->toast("No card: built-in samples only");
        return;
    }
    static char folder[48];
    std::snprintf(folder, sizeof folder, "%s/Samples", ctx_->home);
    lab::kit::BrowserRequest q;
    static char title[32];
    std::snprintf(title, sizeof title, "Sample for channel %d", ch + 1);
    q.title = title;
    q.folder = pat_->ch[ch].sample[0] ? "" : folder;
    q.exts = ".wav";
    q.ctx = this;
    pickCh_ = ch;
    q.onPick = [](void* c, const char* path) {
        Tilt* t = static_cast<Tilt*>(c);
        lab::copyStr(t->pendingPick_, sizeof t->pendingPick_, path);
        t->pendingKind_ = 1;
    };
    ctx_->browser().open(q);
}

}  // namespace tilt
