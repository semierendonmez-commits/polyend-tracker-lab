// PLATFORM -- native app sessions (native.h).
#include "ws/native.h"

#include <cstdio>
#include <cstring>

#include "ws/lab_layout.h"

#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
#include "hal/sdram_cache.h"
#endif
#if !defined(__arm__)
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace plat {

static_assert(sizeof(PlatSetting) == sizeof(lab::SettingRow), "PlatSetting must mirror lab::SettingRow");
static_assert(sizeof(PlatEvent) == 12, "PlatEvent layout");
static_assert(sizeof(PlatPage) == sizeof(lab::kit::PageInfo) && offsetof(PlatPage, help) == offsetof(lab::kit::PageInfo, help) &&
                  offsetof(PlatPage, fkeys) == offsetof(lab::kit::PageInfo, fkeys) &&
                  offsetof(PlatPage, r) == offsetof(lab::kit::PageInfo, color),
              "PlatPage must mirror lab::kit::PageInfo");

#if defined(__arm__)
// The fast segment buffer: in .noinit, which the device linker script places
// right after .data in SRAM_L (code bus) -- checked at run time; the segment
// falls back to the SDRAM when the buffer is elsewhere or too small.
namespace {
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
__attribute__((aligned(16), section(".noinit.platfast"))) uint8_t gFastRam[kFastRamBytes];
#else   // QEMU test build (tests/bench_m4/mps2.ld has no .noinit: an orphan would land in the libc heap)
__attribute__((aligned(16))) uint8_t gFastRam[kFastRamBytes];
#endif
bool fastRamIsCodeBus() {
    const uintptr_t a = reinterpret_cast<uintptr_t>(gFastRam);
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
    return a >= 0x1FFF0000u && a + kFastRamBytes <= 0x20000000u;
#else
    return a != 0;   // QEMU test build: any RAM
#endif
}
}  // namespace

// Instruction / data synchronisation after writing code (ARMv7-M: DSB + ISB),
// plus the K66 LMEM caches: the system cache over the SDRAM (write-through,
// invalidated so no line from an earlier app at the same address survives) and
// the code cache that serves SRAM_L (whole-cache invalidate, PCCCR).
extern "C" void platSyncCode(const void* p, uint32_t len) {
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
    ot::hal::cache::invalidate(p, len);
    volatile uint32_t& pcccr = *reinterpret_cast<volatile uint32_t*>(0xE0082000u);
    pcccr = (pcccr & 3u) | (1u << 31) | (1u << 24) | (1u << 26);   // GO + INVW0 + INVW1
    for (uint32_t n = 0; n < 100000 && (pcccr & (1u << 31)); ++n) {}
#else
    (void)p;
    (void)len;
#endif
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}
#endif

namespace {

NativeSession* N(void* c) { return static_cast<NativeSession*>(c); }
BudgetCanvas* C(void* c) { return N(c)->canvas(); }

// ---- host table thunks ---------------------------------------------------------------
void* hAlloc(void* c, uint32_t bytes, uint32_t align) {
    if (align < 8) align = 8;
    if (align & (align - 1)) return nullptr;
    return N(c)->pool()->alloc(bytes, align);
}
uint32_t hMemFree(void* c) { return N(c)->pool()->free(); }
void hColor(void* c, uint32_t rgb) { if (C(c)) C(c)->color(rgb); }
void hAlpha(void* c, uint8_t a) { if (C(c)) C(c)->alpha(a); }
void hLineWidth(void* c, float px) { if (C(c)) C(c)->lineWidth(px); }
void hPointSize(void* c, float px) { if (C(c)) C(c)->pointSize(px); }
void hClip(void* c, int x, int y, int w, int h) { if (C(c)) C(c)->clip(x, y, w, h); }
void hNoClip(void* c) { if (C(c)) C(c)->noClip(); }
void hFill(void* c, int x, int y, int w, int h) { if (C(c)) C(c)->fill(x, y, w, h); }
void hFillRound(void* c, int x, int y, int w, int h, float r) { if (C(c)) C(c)->fillRound(x, y, w, h, r); }
void hFrame(void* c, int x, int y, int w, int h) { if (C(c)) C(c)->frame(x, y, w, h); }
void hLine(void* c, float a, float b, float d, float e) { if (C(c)) C(c)->line(a, b, d, e); }
void hPoint(void* c, float x, float y, float r) { if (C(c)) C(c)->point(x, y, r); }
void hRing(void* c, float x, float y, float r, int s) { if (C(c)) C(c)->ring(x, y, r, s); }
void hArc(void* c, float x, float y, float r, float a0, float a1, int s) { if (C(c)) C(c)->arc(x, y, r, a0, a1, s); }
void hBegin(void* c, int kind) { if (C(c)) C(c)->beginOpen(kind); }
void hVtx(void* c, float x, float y) { if (C(c)) C(c)->vtx(x, y); }
void hEnd(void* c) { if (C(c)) C(c)->endBatch(); }
void hText(void* c, int x, int y, uint8_t f, const char* s, uint16_t o) { if (C(c)) C(c)->text(x, y, f, s, o); }
void hNumber(void* c, int x, int y, uint8_t f, int32_t n, uint16_t o) { if (C(c)) C(c)->number(x, y, f, n, o); }
void hBitmap(void* c, int x, int y, uint32_t a, int w, int h, int s) {
    if (C(c) && a < 0x06CC00u) C(c)->bitmap332(x, y, a, w, h, s);
}
bool hWriteRamG(void* c, uint32_t addr, const void* src, uint32_t len) {
    // the first free RAM_G band (WORKSTATION_GUIDE.md): 0x000000..0x06CBFF
    if (addr + uint64_t(len) > 0x06CC00u) return false;
    lab::Shell* sh = N(c)->ctx()->shell;
    (void)sh;
    lab::IDisplay* d = N(c)->canvas() ? N(c)->canvas()->canvas().display() : nullptr;
    return d && d->writeRamG(addr, src, len);
}
uint32_t hWords(void* c) { return C(c) ? C(c)->used() : 0; }
ot::IStorage* fs(void* c) { return N(c)->ctx()->fs; }
int32_t hOpen(void* c, const char* p, uint8_t m) {
    return fs(c) && p && m >= 1 && m <= 3 ? fs(c)->open(p, ot::IStorage::OpenMode(m)) : -1;
}
int32_t hRead(void* c, int32_t h, void* d, uint32_t n) { return fs(c) ? fs(c)->read(h, d, n) : -1; }
int32_t hWrite(void* c, int32_t h, const void* s, uint32_t n) { return fs(c) ? fs(c)->write(h, s, n) : -1; }
bool hSeek(void* c, int32_t h, uint32_t p) { return fs(c) && fs(c)->seek(h, p); }
int32_t hSize(void* c, int32_t h) { return fs(c) ? fs(c)->size(h) : -1; }
void hClose(void* c, int32_t h) { if (fs(c)) fs(c)->close(h); }
bool hExists(void* c, const char* p) { return fs(c) && p && fs(c)->exists(p); }
bool hMkdir(void* c, const char* p) { return fs(c) && p && fs(c)->mkdir(p); }
bool hRemove(void* c, const char* p) { return fs(c) && p && fs(c)->remove(p); }
bool hRename(void* c, const char* a, const char* b) { return fs(c) && a && b && fs(c)->rename(a, b); }
int32_t hList(void* c, const char* p, PlatDirFn fn, void* u) {
    if (!fs(c) || !p || !fn) return -1;
    struct L {
        PlatDirFn fn;
        void* u;
    } l{fn, u};
    return fs(c)->listDir(
        p, [](const ot::IStorage::DirEntry& e, void* x) {
            L* l = static_cast<L*>(x);
            return l->fn(e.name, e.isDir, e.size, l->u);
        },
        &l);
}
uint32_t hMillis(void* c) { return N(c)->ctx()->millis(); }
bool hPlaying(void* c) { return N(c)->ctx()->playing(); }
float hBpm(void* c) { return N(c)->ctx()->bpm(); }
void hSetBpm(void* c, float b) { N(c)->ctx()->setBpm(b); }
void hSetPlaying(void* c, bool on) { N(c)->ctx()->setPlaying(on); }
void hMidi(void* c, uint8_t s, uint8_t a, uint8_t b) {
    lab::MidiMsg m;
    m.status = s;
    m.d1 = a & 127;
    m.d2 = b & 127;
    N(c)->ctx()->midiOut(m);
}
uint8_t hMidiCh(void* c) { return N(c)->ctx()->midiChannel(); }
void hMidiNote(void* c, bool on, uint8_t n, uint8_t v, uint8_t o) { N(c)->ctx()->midiNote(on, n, v, o); }
void hToast(void* c, const char* m) { N(c)->ctx()->toast(m ? m : ""); }
void hRedraw(void* c) { N(c)->ctx()->requestRedraw(); }
void hLog(void* c, const char* m) { N(c)->log(m); }
void hStem(void* c, int i, const float* l, const float* r) { N(c)->ctx()->stem(i, l, r); }
bool hRecording(void* c) { return N(c)->ctx()->shell->recording(); }
uint32_t hCpu(void* c) { return N(c)->ctx()->shell->cpuPermille(); }
int hInput(void* c) { return N(c)->ctx()->shell->inputSource(); }
float hRadioMHz(void* c) { return N(c)->ctx()->shell->radioMHz(); }
ot::IRadio* radio(void* c) { return N(c)->ctx()->radio; }
void hRadioBand(void* c, float a, float b, float s) { if (radio(c)) radio(c)->setBand(a, b, s); }
bool hRadioTune(void* c, float f) { return radio(c) && radio(c)->tune(f); }
bool hRadioSeek(void* c, bool up) { return radio(c) && radio(c)->seek(up); }
bool hRadioBusy(void* c) { return radio(c) && radio(c)->busy(); }
float hRadioFreq(void* c) { return radio(c) ? radio(c)->frequency() : 0.0f; }

// ---- LABKIT (main loop). The registry is grown with the kit's audio block
// off: Registry::add counts the param before its binding is complete.
int hParamAdd(void* c, const char* key, const char* name, float* value, float lo, float hi, float def, const char* unit,
              int page, uint32_t flags) {
    NativeSession* n = N(c);
    lab::Context* ctx = n->ctx();
    if (!ctx->shell || !key || !key[0] || !value) return -1;
    lab::kit::Kit& k = ctx->shell->labkit();
    lab::kit::Registry& R = k.reg;
    if (!(hi > lo)) hi = lo + 1.0f;
    if ((flags & PLAT_PARAM_EXP) && lo <= 0.0f) flags &= ~uint32_t(PLAT_PARAM_EXP);
    def = def < lo ? lo : (def > hi ? hi : def);
    const bool was = k.active;
    k.active = false;
    R.setPage(page < 0 ? 0 : (page >= lab::kit::kMaxPages ? lab::kit::kMaxPages - 1 : page));
    *value = def;
    lab::kit::Param& P = R.add(key, name && name[0] ? name : key, value).range(lo, hi).def(def);
    const lab::kit::ParamId id = P;
    if (id != lab::kit::kNoParam) {
        if (flags & PLAT_PARAM_INT) P.integer();
        if (flags & PLAT_PARAM_EXP) P.exp();
        if (flags & PLAT_PARAM_PERCENT) P.percent();
        if (flags & PLAT_PARAM_BIPOLAR) P.bipolar();
        if (flags & PLAT_PARAM_NOMOD) P.noMod();
        if (unit && unit[0]) P.unit(unit);
        n->noteParam();
    }
    k.active = was;
    return id;
}
float hParamGet(void* c, int id, bool effective) {
    lab::kit::Registry& R = N(c)->ctx()->params();
    if (!R.valid(lab::kit::ParamId(id))) return 0.0f;
    return effective ? R.effective(lab::kit::ParamId(id)) : R.get(lab::kit::ParamId(id));
}
void hParamSet(void* c, int id, float v) {
    lab::kit::Registry& R = N(c)->ctx()->params();
    if (R.valid(lab::kit::ParamId(id))) R.set(lab::kit::ParamId(id), v);
}
int hModSource(void* c, const char* name, const float* value, bool bipolar) {
    if (!name || !value || !N(c)->ctx()->shell) return -1;
    return N(c)->ctx()->mod().addSource(name, value, bipolar);
}

bool validApp(const PlatApp* a) {
    return a && a->abi == PLAT_ABI_VERSION && a->size >= offsetof(PlatApp, close) + sizeof(void*);
}

}  // namespace

void NativeSession::log(const char* s) {
    lab::copyStr(log_[logN_ % kLog], sizeof log_[0], s ? s : "");
    ++logN_;
}

void NativeSession::fail(const char* msg) {
    failed_ = true;
    lab::copyStr(error_, sizeof error_, msg);
}

void NativeSession::buildHost() {
    PlatHost& h = host_;
    h = PlatHost();
    h.abi = PLAT_ABI_VERSION;
    h.size = sizeof(PlatHost);
    h.ctx = this;
    h.home = home_;
    h.fwName = ctx_->shell ? ctx_->shell->fwName() : "PLATFORM";
    h.alloc = hAlloc;
    h.memFree = hMemFree;
    h.color = hColor;
    h.alpha = hAlpha;
    h.lineWidth = hLineWidth;
    h.pointSize = hPointSize;
    h.clip = hClip;
    h.noClip = hNoClip;
    h.fill = hFill;
    h.fillRound = hFillRound;
    h.frame = hFrame;
    h.line = hLine;
    h.point = hPoint;
    h.ring = hRing;
    h.arc = hArc;
    h.beginBatch = hBegin;
    h.vtx = hVtx;
    h.endBatch = hEnd;
    h.text = hText;
    h.number = hNumber;
    h.bitmap332 = hBitmap;
    h.writeRamG = hWriteRamG;
    h.ramgBase = 0;
    h.ramgSize = 0x06CC00u;
    h.canvasWords = hWords;
    h.canvasBudget = 1680;
    h.fileOpen = hOpen;
    h.fileRead = hRead;
    h.fileWrite = hWrite;
    h.fileSeek = hSeek;
    h.fileSize = hSize;
    h.fileClose = hClose;
    h.fileExists = hExists;
    h.mkdir = hMkdir;
    h.fileRemove = hRemove;
    h.fileRename = hRename;
    h.listDir = hList;
    h.hasCard = ctx_->fs != nullptr;
    h.millis = hMillis;
    h.playing = hPlaying;
    h.bpm = hBpm;
    h.setBpm = hSetBpm;
    h.setPlaying = hSetPlaying;
    h.midiOut = hMidi;
    h.midiChannel = hMidiCh;
    h.toast = hToast;
    h.requestRedraw = hRedraw;
    h.log = hLog;
    h.stem = hStem;
    h.recording = hRecording;
    h.cpuPermille = hCpu;
    h.inputSource = hInput;
    h.radioMHz = hRadioMHz;
    h.hasRadio = ctx_->radio != nullptr;
    h.radioSetBand = hRadioBand;
    h.radioTune = hRadioTune;
    h.radioSeek = hRadioSeek;
    h.radioBusy = hRadioBusy;
    h.radioFrequency = hRadioFreq;
    h.labShell = ctx_->shell;
    h.labCanvas = nullptr;
    h.labStorage = ctx_->fs;
    h.labRadio = ctx_->radio;
    h.labLayout = kKitLayout;
    h.midiNote = hMidiNote;
    if (ctx_->shell) {
        h.paramAdd = hParamAdd;
        h.paramGet = hParamGet;
        h.paramSet = hParamSet;
        h.modSource = hModSource;
    }
}

bool NativeSession::hasNav() const {
    return running() && app_->size >= offsetof(PlatApp, labTheme) + sizeof(void*);
}

bool NativeSession::kitPath(char* out, uint32_t cap) const {
    if (!home_[0]) return false;
    std::snprintf(out, cap, "%s/labkit.lkc", home_);
    return true;
}

int NativeSession::navPages(lab::kit::PageInfo* out, int cap) {
    if (!hasNav() || !app_->navPages) return 0;
    const int n = app_->navPages(app_->self, reinterpret_cast<PlatPage*>(out), cap);
    return n < 0 ? 0 : (n > cap ? cap : n);
}
int NativeSession::navPage() const { return hasNav() && app_->navPage ? app_->navPage(app_->self) : -1; }
void NativeSession::navGoto(int page) {
    if (hasNav() && app_->navGoto) app_->navGoto(app_->self, page);
}
void NativeSession::navFocus(int id) {
    if (hasNav() && app_->navFocus) app_->navFocus(app_->self, id);
}
void NativeSession::navParamChanged(int id) {
    if (hasNav() && app_->navParamChanged) app_->navParamChanged(app_->self, id);
}
void NativeSession::navKeys(lab::kit::NavKeys& keys) {
    if (hasNav() && app_->navKeys && (info_.flags & PLAT_INFO_WORKSTATION)) app_->navKeys(app_->self, &keys);
}
const lab::Theme* NativeSession::labTheme() const {
    if (!hasNav() || !app_->labTheme || !(info_.flags & PLAT_INFO_WORKSTATION)) return nullptr;
    return static_cast<const lab::Theme*>(app_->labTheme(app_->self));
}

bool readAppInfo(lab::Context& ctx, const char* path, PlatInfo& info) {
    std::memset(&info, 0, sizeof info);
    if (!ctx.fs) return false;
    const ot::IStorage::Handle h = ctx.fs->open(path, ot::IStorage::kRead);
    if (h < 0) return false;
#if defined(__arm__)
    PappHeader hd;
    const bool ok = ctx.fs->read(h, &hd, sizeof hd) == int32_t(sizeof hd) && !std::memcmp(hd.magic, "PAPP", 4) &&
                    !std::memcmp(hd.info.magic, "PLATINF", 8);
    ctx.fs->close(h);
    if (ok) {
        info = hd.info;
        info.name[sizeof info.name - 1] = info.author[sizeof info.author - 1] = 0;
        info.desc[sizeof info.desc - 1] = info.icon[sizeof info.icon - 1] = 0;
    }
    return ok;
#else
    // host .so: scan the file for the PlatInfo record ("PLATINF\0" + fields)
    uint8_t buf[4096 + sizeof(PlatInfo)];
    uint32_t carry = 0;
    bool found = false;
    for (;;) {
        const int32_t n = ctx.fs->read(h, buf + carry, 4096);
        if (n <= 0) break;
        const uint32_t total = carry + uint32_t(n);
        for (uint32_t i = 0; i + sizeof(PlatInfo) <= total; ++i) {
            if (buf[i] == 'P' && !std::memcmp(buf + i, "PLATINF", 8)) {
                std::memcpy(&info, buf + i, sizeof info);
                found = true;
                break;
            }
        }
        if (found) break;
        carry = total < sizeof(PlatInfo) ? total : uint32_t(sizeof(PlatInfo));
        std::memmove(buf, buf + total - carry, carry);
    }
    ctx.fs->close(h);
    if (found) {
        info.name[sizeof info.name - 1] = info.author[sizeof info.author - 1] = 0;
        info.desc[sizeof info.desc - 1] = info.icon[sizeof info.icon - 1] = 0;
    }
    return found;
#endif
}

bool NativeSession::loadImage(const char* path) {
    ot::IStorage* f = ctx_->fs;
    if (!f) {
        fail("no SD card");
        return false;
    }
    const ot::IStorage::Handle h = f->open(path, ot::IStorage::kRead);
    if (h < 0) {
        fail("cannot open the app file");
        return false;
    }
    const int32_t sz = f->size(h);
#if defined(__arm__)
    // Streamed straight into place: header -> fast segment (SRAM_L or pool) ->
    // main segment (its final address in the pool) -> relocations (into a
    // scratch area at the pool's top, released afterwards). The CRC is
    // computed on the way, so a damaged file never runs.
    PappHeader hd;
    auto readAll = [&](void* dst, uint32_t n, uint32_t& crc) {
        uint8_t* d = static_cast<uint8_t*>(dst);
        uint32_t got = 0;
        while (got < n) {
            const int32_t k = f->read(h, d + got, n - got);
            if (k <= 0) return false;
            got += uint32_t(k);
        }
        crc = pappCrc32(d, n, crc);
        return true;
    };
    uint32_t crc = 0, dummy = 0;
    if (sz < int32_t(sizeof hd) || !readAll(&hd, sizeof hd, dummy)) {
        f->close(h);
        fail("card read error");
        return false;
    }
    PappStatus st = PappStatus::Ok;
    {
        // header checks against the real file size (no body in memory yet)
        static uint8_t probe[sizeof(PappHeader)];
        std::memcpy(probe, &hd, sizeof hd);
        st = pappParseHeader(probe, uint32_t(sz), hd);
    }
    if (st != PappStatus::Ok) {
        f->close(h);
        fail(pappStatusText(st));
        return false;
    }
    info_ = hd.info;
    const uint64_t need = uint64_t(hd.imageSize) + hd.bssSize + hd.imageAlign + uint64_t(hd.relocCount) * 4u + 64u +
                          (hd.fastSize > kFastRamBytes ? hd.fastSize : 0u);
    if (need > pool_->free()) {
        f->close(h);
        fail("not enough memory for the app image");
        return false;
    }
    const bool fastSram = hd.fastSize && hd.fastSize <= kFastRamBytes && fastRamIsCodeBus();
    uint8_t* fast = nullptr;
    if (hd.fastSize) fast = fastSram ? gFastRam : static_cast<uint8_t*>(pool_->alloc(hd.fastSize, 16));
    uint8_t* main = static_cast<uint8_t*>(pool_->alloc(hd.imageSize + hd.bssSize, hd.imageAlign));
    // relocation words: borrowed from the free pool above (not kept)
    const uint32_t mark = pool_->used();
    uint32_t* rel = static_cast<uint32_t*>(pool_->alloc(hd.relocCount * 4u + 4u, 4));
    bool ok = main && rel && (!hd.fastSize || fast);
    ok = ok && (!hd.fastSize || readAll(fast, hd.fastSize, crc));
    ok = ok && readAll(main, hd.imageSize, crc);
    ok = ok && readAll(rel, hd.relocCount * 4u, crc);
    f->close(h);
    if (!ok) {
        fail(main ? "card read error" : "not enough memory for the app image");
        return false;
    }
    st = pappRelocate(hd, main, fast, rel, uint32_t(uintptr_t(main)), uint32_t(uintptr_t(fast)), crc);
    pool_->rewind(mark);   // the relocation words are no longer needed
    if (st != PappStatus::Ok) {
        fail(pappStatusText(st));
        return false;
    }
    platSyncCode(main, hd.imageSize + hd.bssSize);
    if (fast) platSyncCode(fast, hd.fastSize);
    fastInSram_ = fastSram;
    imageBytes_ = hd.imageSize + hd.bssSize;
    fastBytes_ = hd.fastSize;
    // constructors (.init_array), then the entry point
    for (uint32_t i = 0; i < hd.initCount; ++i) {
        uint32_t fnAddr;
        std::memcpy(&fnAddr, main + hd.initArray + i * 4u, 4);
        if (fnAddr && fnAddr != 0xFFFFFFFFu) reinterpret_cast<void (*)()>(uintptr_t(fnAddr))();
    }
    const PlatEntryFn entry = reinterpret_cast<PlatEntryFn>(uintptr_t(pappEntryAddr(hd, uint32_t(uintptr_t(main)))));
    app_ = entry();
    return true;
#else
    // host: an in-memory file (memfd) holding the .so, then dlopen
    if (sz <= 0 || uint32_t(sz) > pool_->free()) {
        f->close(h);
        fail(sz <= 0 ? "empty app file" : "app file too big for the memory");
        return false;
    }
    const uint32_t mark = pool_->used();
    uint8_t* file = static_cast<uint8_t*>(pool_->alloc(uint32_t(sz), 16));
    int32_t got = 0;
    while (file && got < sz) {
        const int32_t n = f->read(h, file + got, uint32_t(sz - got));
        if (n <= 0) break;
        got += n;
    }
    f->close(h);
    if (!file || got != sz) {
        fail("card read error");
        return false;
    }
    fd_ = int(memfd_create("platform-app", MFD_CLOEXEC));
    if (fd_ < 0 || ::write(fd_, file, size_t(sz)) != ssize_t(sz)) {
        fail("memfd failed");
        return false;
    }
    char p[64];
    std::snprintf(p, sizeof p, "/proc/self/fd/%d", fd_);
    dl_ = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!dl_) {
        const char* e = dlerror();
        fail(e ? e : "dlopen failed");
        return false;
    }
    const PlatInfo* inf = reinterpret_cast<const PlatInfo*>(dlsym(dl_, "plat_app_info"));
    if (inf) info_ = *inf;
    const PlatEntryFn entry = reinterpret_cast<PlatEntryFn>(dlsym(dl_, "plat_app_entry"));
    if (!entry) {
        fail("no plat_app_entry in the app");
        return false;
    }
    imageBytes_ = uint32_t(sz);
    pool_->rewind(mark);   // dlopen keeps its own mapping
    app_ = entry();
    return true;
#endif
}

bool NativeSession::open(Pool& pool, lab::Context& ctx, const char* path, const char* dataDir) {
    ctx_ = &ctx;
    pool_ = &pool;
    app_ = nullptr;
    failed_ = false;
    error_[0] = 0;
    logN_ = 0;
    params_ = 0;
    fastInSram_ = false;
    imageBytes_ = fastBytes_ = 0;
    std::memset(&info_, 0, sizeof info_);
    lab::copyStr(home_, sizeof home_, dataDir);
    if (ctx.fs) {
        char d[80];
        std::snprintf(d, sizeof d, "%s/Data", ctx.home);
        ctx.fs->mkdir(d);
        ctx.fs->mkdir(home_);
    }
    buildHost();
    if (!loadImage(path)) {
        app_ = nullptr;
        return false;
    }
    if (uint64_t(info_.memKB) * 1024u > pool.free()) {
        char m[128];
        std::snprintf(m, sizeof m, "%s needs %lu KB of memory, %lu KB are free", info_.name,
                      static_cast<unsigned long>(info_.memKB), static_cast<unsigned long>(pool.free() / 1024));
        app_ = nullptr;
        fail(m);
        return false;
    }
    if (!validApp(app_)) {
        app_ = nullptr;
        fail("the app was built for another PLATFORM ABI (rebuild it with this SDK)");
        return false;
    }
    if (app_->init && !app_->init(app_->self, &host_)) {
        const PlatApp* a = app_;
        app_ = nullptr;
        char m[160];
        std::snprintf(m, sizeof m, "%s: init failed%s%s", a->name ? a->name : "app", logN_ ? " - " : "",
                      logN_ ? log_[(logN_ - 1) % kLog] : "");
        fail(m);
        return false;
    }
    char kp[96];
    if (params_ && !(info_.flags & PLAT_INFO_WORKSTATION) && ctx.fs && kitPath(kp, sizeof kp) && ctx.fs->exists(kp))
        ctx.params().loadFile(ctx.fs, kp);
    return true;
}

void NativeSession::saveKit() {
    char kp[96];
    if (params_ && !(info_.flags & PLAT_INFO_WORKSTATION) && ctx_->fs && ctx_->shell && kitPath(kp, sizeof kp))
        ctx_->params().saveFile(ctx_->fs, kp);
}

void NativeSession::close() {
    if (app_ && !failed_) {
        saveKit();
        if (app_->save) app_->save(app_->self);
        if (app_->close) app_->close(app_->self);
    }
    app_ = nullptr;
#if !defined(__arm__)
    if (dl_) dlclose(dl_);
    dl_ = nullptr;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
}

void NativeSession::event(const lab::Event& e) {
    if (!running() || !app_->event) return;
    PlatEvent pe{};
    pe.type = e.type;
    pe.id = e.id;
    pe.velocity = e.velocity;
    pe.delta = e.delta;
    pe.shift = e.shift ? 1 : 0;
    pe.ms = e.ms;
    app_->event(app_->self, &pe);
}
void NativeSession::midi(const lab::MidiMsg& m) {
    if (running() && app_->midi) app_->midi(app_->self, m.status, m.d1, m.d2);
}
void NativeSession::update(uint32_t ms) {
    if (running() && app_->update) app_->update(app_->self, ms);
}
void NativeSession::draw(lab::Canvas& g, BudgetCanvas& c) {
    if (!running() || !app_->draw) return;
    canvas_ = &c;
    host_.labCanvas = &g;
    app_->draw(app_->self);
    c.endBatch();
    canvas_ = nullptr;
    host_.labCanvas = nullptr;
}
void NativeSession::drawIntro(lab::Canvas& g, BudgetCanvas& c, uint32_t ms) {
    if (!running() || !app_->drawIntro) return;
    canvas_ = &c;
    host_.labCanvas = &g;
    app_->drawIntro(app_->self, ms);
    c.endBatch();
    canvas_ = nullptr;
    host_.labCanvas = nullptr;
}
void NativeSession::leds(lab::Rgb* pads) {
    if (!running() || !app_->leds) return;
    uint8_t rgb[lab::kPads * 3];
    std::memset(rgb, 0, sizeof rgb);
    app_->leds(app_->self, rgb);
    for (int i = 0; i < lab::kPads; ++i) pads[i] = lab::Rgb(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]);
}
void NativeSession::transport(bool playing) {
    if (running() && app_->transport) app_->transport(app_->self, playing);
}
int NativeSession::settings(lab::SettingRow* rows, int cap) {
    if (!running() || !app_->settings) return 0;
    int n = app_->settings(app_->self, reinterpret_cast<PlatSetting*>(rows), cap);
    return n < 0 ? 0 : (n > cap ? cap : n);
}
void NativeSession::save() {
    if (running()) saveKit();
    if (running() && app_->save) app_->save(app_->self);
}
static bool hasBench(const PlatApp* a) {
    return a && a->size >= offsetof(PlatApp, benchSetup) + sizeof(void*) && a->benchScenarios && a->benchSetup;
}
int NativeSession::benchScenarios() const { return running() && hasBench(app_) ? app_->benchScenarios(app_->self) : 0; }
const char* NativeSession::benchName(int i) const {
    return running() && hasBench(app_) && app_->benchName ? app_->benchName(app_->self, i) : "?";
}
void NativeSession::benchSetup(int i) {
    if (running() && hasBench(app_)) app_->benchSetup(app_->self, i);
}

void NativeSession::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    const PlatApp* a = app_;
    if (!a || failed_ || !a->render) return;
    PlatTransport pt;
    pt.playing = t.playing;
    pt.bpm = t.bpm;
    pt.beat = t.beat;
    pt.beatsPerSample = t.beatsPerSample;
    pt.startedThisBlock = t.startedThisBlock;
    pt.stoppedThisBlock = t.stoppedThisBlock;
    a->render(a->self, &pt, in.frames, l, r);
}

}  // namespace plat
