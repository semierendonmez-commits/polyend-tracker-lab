// PLATFORM -- launcher + host (platform.h, MANUAL.md).
#include "ws/platform.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/examples_data.h"

namespace plat {

using lab::Canvas;
using lab::Event;
using lab::Rgb;
namespace font = lab::font;
namespace btn = lab::btn;

namespace {

constexpr const char* kScriptsDir = "Scripts";
constexpr const char* kAppsDir = "Apps";
constexpr const char* kStateFile = "state.txt";

const char* const kOffOn[] = {"Off", "On"};
const char* const kHeap[] = {"1 MB", "2 MB", "3 MB", "4 MB", "5 MB", "6 MB", "7 MB", "8 MB"};

lab::Theme makeTheme() {
    lab::Theme t;
    t.bg = Rgb(0x07090D);
    t.panel = Rgb(0x121820);
    t.fg = Rgb(0xE8ECF0);
    t.dim = Rgb(0x66707C);
    t.accent = Rgb(0x39E6B4);    // phosphor mint
    t.accent2 = Rgb(0xFFB347);   // amber
    t.warn = Rgb(0xFF4D5E);
    t.ok = Rgb(0x39E6B4);
    return t;
}

bool startsWith(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

void baseName(char* dst, uint32_t cap, const char* file) {
    uint32_t i = 0;
    for (; file[i] && file[i] != '.' && i + 1 < cap; ++i) dst[i] = file[i];
    dst[i] = 0;
}

// A small embedded "Bench" script for the CPU gate / host tests.
constexpr const char kBenchScript[] = R"LUA(
function init()
  for v = 1, 16 do
    engine.voice(v, {wave = ({"saw", "square", "fm", "sine", "tri", "noise"})[(v - 1) % 6 + 1],
      cutoff = 900 + v * 150, res = 2, fenv = 2, attack = 0.002, decay = 0.4, sustain = 0.6, release = 0.5,
      send_delay = 0.4, send_reverb = 0.5, drive = 0.2, amp = 0.08, pan = (v - 8.5) / 8})
  end
  engine.fx("delay_feedback", 0.6)
  clock.run(function()
    local n = 0
    while true do
      clock.sync(1 / 16)
      n = n + 1
      for v = 1, 16 do
        if (n + v) % 3 == 0 then engine.note(v, 36 + (n * 7 + v * 5) % 36, 0.8) end
        if (n + v) % 5 == 0 then engine.off(v) end
      end
    end
  end)
end
)LUA";

}  // namespace

// The LABKIT objects as they were outside any session (after the Shell's init:
// its MACRO / MOD params, no routes): sessions register on top, and closing
// one puts this back (MANUAL.md "LABKIT").
static_assert(Platform::kKitParams == LuaSession::kRegCap, "one registry size");

struct Platform::KitSnap {
    alignas(8) unsigned char reg[sizeof(lab::kit::Registry)];
    alignas(8) unsigned char mod[sizeof(lab::kit::Mod)];
    alignas(8) unsigned char nav[sizeof(lab::kit::Nav)];
    float base[lab::kit::kMaxParams];
    float eff[lab::kit::kMaxParams];
    uint32_t wsParams = 0;
    int count = 0;
    bool active = false;
    bool taken = false;
};

bool Platform::init(lab::Context& c) {
    ctx_ = &c;
    theme_ = makeTheme();
    engine_ = c.arena->make<Engine>();
    lua_ = c.arena->make<LuaSession>();
    native_ = c.arena->make<NativeSession>();
    snap_ = c.arena->make<KitSnap>();
    if (!engine_ || !lua_ || !native_ || !snap_ || !engine_->init(*c.arena)) return false;
    if (c.shell) c.params().reserve(kKitParams);   // sessions register into it (stations up to ~700 params)
    // The session pool takes the rest of the SDRAM arena (scripts, samples, native apps).
    uint32_t poolBytes = c.arena->free() > kArenaReserve + kMinPool ? c.arena->free() - kArenaReserve : 0;
    poolMem_ = static_cast<uint8_t*>(c.arena->alloc(poolBytes, 64));
    if (!poolMem_) return false;
    pool_.attach(poolMem_, poolBytes);
    if (c.fs) {
        char p[64];
        std::snprintf(p, sizeof p, "%s/%s", c.home, kScriptsDir);
        if (!c.fs->exists(p)) installExamples(false);
        std::snprintf(p, sizeof p, "%s/%s", c.home, kAppsDir);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/Data", c.home);
        c.fs->mkdir(p);
    }
    readState();
    installed_ = installApps();
    rescan();
    return true;
}

// Apps shipped next to the firmware image: release/Apps/ is copied to
// /Firmware/Apps/ on the card together with the .ptf. Every .papp there that
// is missing from /Lab/PLATFORM/Apps, or has another size, is copied in.
int Platform::installApps() {
    if (!ctx_->fs) return 0;
    struct List {
        char names[24][48];
        uint32_t sizes[24];
        int n;
    };
    static List src;
    src.n = 0;
    ctx_->fs->listDir(
        "Firmware/Apps",
        [](const ot::IStorage::DirEntry& e, void* u) {
            List* l = static_cast<List*>(u);
            if (e.isDir || !lab::endsWithCi(e.name, kAppExt) || std::strlen(e.name) >= 48) return true;
            if (l->n >= 24) return false;
            lab::copyStr(l->names[l->n], 48, e.name);
            l->sizes[l->n++] = e.size;
            return true;
        },
        &src);
    int copied = 0;
    static uint8_t buf[4096];
    for (int i = 0; i < src.n; ++i) {
        char from[96], to[96];
        std::snprintf(from, sizeof from, "Firmware/Apps/%s", src.names[i]);
        std::snprintf(to, sizeof to, "%s/%s/%s", ctx_->home, kAppsDir, src.names[i]);
        const ot::IStorage::Handle old = ctx_->fs->open(to, ot::IStorage::kRead);
        if (old >= 0) {
            const int32_t sz = ctx_->fs->size(old);
            ctx_->fs->close(old);
            if (sz == int32_t(src.sizes[i])) continue;   // already installed
        }
        const ot::IStorage::Handle in = ctx_->fs->open(from, ot::IStorage::kRead);
        if (in < 0) continue;
        char tmp[100];
        std::snprintf(tmp, sizeof tmp, "%s.part", to);
        const ot::IStorage::Handle out = ctx_->fs->open(tmp, ot::IStorage::kWrite);
        bool ok = out >= 0;
        uint32_t total = 0;
        while (ok) {
            const int32_t n = ctx_->fs->read(in, buf, sizeof buf);
            if (n <= 0) break;
            ok = ctx_->fs->write(out, buf, uint32_t(n)) == n;
            total += uint32_t(n);
        }
        ctx_->fs->close(in);
        if (out >= 0) ctx_->fs->close(out);
        ok = ok && total == src.sizes[i];
        if (ok) {
            ctx_->fs->remove(to);
            ok = ctx_->fs->rename(tmp, to);
        }
        if (!ok) {
            ctx_->fs->remove(tmp);
            continue;
        }
        ++copied;
    }
    return copied;
}

// ---- card ---------------------------------------------------------------------------
void Platform::installExamples(bool force) {
    if (!ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/%s", ctx_->home, kScriptsDir);
    ctx_->fs->mkdir(p);
    for (int i = 0; i < kExampleScriptCount; ++i) {
        std::snprintf(p, sizeof p, "%s/%s/%s", ctx_->home, kScriptsDir, kExampleScripts[i].file);
        if (!force && ctx_->fs->exists(p)) continue;
        const ot::IStorage::Handle h = ctx_->fs->open(p, ot::IStorage::kWrite);
        if (h < 0) continue;
        const uint32_t n = uint32_t(std::strlen(kExampleScripts[i].text));
        ctx_->fs->write(h, kExampleScripts[i].text, n);
        ctx_->fs->close(h);
    }
}

void Platform::scanScripts() {
    if (!ctx_->fs) return;
    char dir[64];
    std::snprintf(dir, sizeof dir, "%s/%s", ctx_->home, kScriptsDir);
    struct Ctx {
        Platform* p;
        char names[kMaxEntries][48];
        int n;
    };
    static Ctx c;   // main loop only; keeps 3 KB off the stack
    c.p = this;
    c.n = 0;
    ctx_->fs->listDir(
        dir,
        [](const ot::IStorage::DirEntry& e, void* u) {
            Ctx* c = static_cast<Ctx*>(u);
            if (e.isDir || !lab::endsWithCi(e.name, ".lua") || std::strlen(e.name) >= 48) return true;
            if (c->n >= kMaxEntries) return false;
            lab::copyStr(c->names[c->n++], 48, e.name);
            return true;
        },
        &c);
    for (int i = 0; i < c.n && nEntries_ < kMaxEntries; ++i) {
        char path[128];
        std::snprintf(path, sizeof path, "%s/%s", dir, c.names[i]);
        char head[1024];
        int32_t got = 0;
        const ot::IStorage::Handle h = ctx_->fs->open(path, ot::IStorage::kRead);
        if (h >= 0) {
            got = ctx_->fs->read(h, head, sizeof head);
            ctx_->fs->close(h);
        }
        ScriptMeta m;
        parseScriptHeader(head, got > 0 ? uint32_t(got) : 0, c.names[i], m);
        Entry& en = entries_[nEntries_++];
        en = Entry();
        en.kind = Entry::kScript;
        lab::copyStr(en.file, sizeof en.file, c.names[i]);
        lab::copyStr(en.name, sizeof en.name, m.name);
        lab::copyStr(en.author, sizeof en.author, m.author);
        lab::copyStr(en.desc, sizeof en.desc, m.desc[0] ? m.desc : "Lua script");
        lab::copyStr(en.icon, sizeof en.icon, m.icon);
    }
}

void Platform::scanApps() {
    if (!ctx_->fs) return;
    char dir[64];
    std::snprintf(dir, sizeof dir, "%s/%s", ctx_->home, kAppsDir);
    struct Ctx {
        char names[kMaxEntries][48];
        int n;
    };
    static Ctx c;
    c.n = 0;
    ctx_->fs->listDir(
        dir,
        [](const ot::IStorage::DirEntry& e, void* u) {
            Ctx* c = static_cast<Ctx*>(u);
            if (e.isDir || !lab::endsWithCi(e.name, kAppExt) || std::strlen(e.name) >= 48) return true;
            if (c->n >= kMaxEntries) return false;
            lab::copyStr(c->names[c->n++], 48, e.name);
            return true;
        },
        &c);
    for (int i = 0; i < c.n && nEntries_ < kMaxEntries; ++i) {
        char path[128];
        std::snprintf(path, sizeof path, "%s/%s", dir, c.names[i]);
        PlatInfo info;
        Entry& en = entries_[nEntries_];
        en = Entry();
        lab::copyStr(en.file, sizeof en.file, c.names[i]);
        if (readAppInfo(*ctx_, path, info)) {
            en.kind = (info.flags & PLAT_INFO_WORKSTATION) ? Entry::kStation : Entry::kApp;
            lab::copyStr(en.name, sizeof en.name, info.name);
            lab::copyStr(en.author, sizeof en.author, info.author);
            lab::copyStr(en.desc, sizeof en.desc, info.desc);
            lab::copyStr(en.icon, sizeof en.icon, info.icon[0] ? info.icon : "app");
            en.color = info.color & 0xFFFFFFu;
        } else {
            en.kind = Entry::kApp;
            baseName(en.name, sizeof en.name, c.names[i]);
            lab::copyStr(en.desc, sizeof en.desc, "(no app info - not built with the PLATFORM SDK?)");
            lab::copyStr(en.icon, sizeof en.icon, "app");
        }
        ++nEntries_;
    }
}

void Platform::rescan() {
    nEntries_ = 0;
    previewFor_ = -1;
    scanApps();
    scanScripts();
    // sort: stations, apps, scripts; then by name
    for (int i = 1; i < nEntries_; ++i) {
        Entry e = entries_[i];
        int j = i - 1;
        auto before = [](const Entry& a, const Entry& b) {
            const int ka = a.kind == Entry::kStation ? 0 : (a.kind == Entry::kApp ? 1 : 2);
            const int kb = b.kind == Entry::kStation ? 0 : (b.kind == Entry::kApp ? 1 : 2);
            if (ka != kb) return ka < kb;
            return std::strcmp(a.name, b.name) < 0;
        };
        while (j >= 0 && before(e, entries_[j])) {
            entries_[j + 1] = entries_[j];
            --j;
        }
        entries_[j + 1] = e;
    }
    nView_ = 0;
    for (int i = 0; i < nEntries_; ++i) {
        const bool script = entries_[i].kind == Entry::kScript;
        if (filter_ == 0 || (filter_ == 1 && script) || (filter_ == 2 && !script)) view_[nView_++] = i;
    }
    if (sel_ >= nView_) sel_ = nView_ ? nView_ - 1 : 0;
    // select the last opened entry
    if (lastFile_[0])
        for (int v = 0; v < nView_; ++v)
            if (!std::strcmp(entries_[view_[v]].file, lastFile_)) sel_ = v;
}

// state.txt: last=<file>\nopen=<file being opened, cleared after kCrashGuardMs>
void Platform::readState() {
    lastFile_[0] = 0;
    if (!ctx_->fs) return;
    char p[80];
    std::snprintf(p, sizeof p, "%s/%s", ctx_->home, kStateFile);
    const ot::IStorage::Handle h = ctx_->fs->open(p, ot::IStorage::kRead);
    if (h < 0) return;
    char buf[256] = {};
    const int32_t n = ctx_->fs->read(h, buf, sizeof buf - 1);
    ctx_->fs->close(h);
    if (n <= 0) return;
    buf[n] = 0;
    char crashed[48] = {};
    for (char* line = buf; line && *line;) {
        char* nl = std::strchr(line, '\n');
        if (nl) *nl = 0;
        if (startsWith(line, "last=")) lab::copyStr(lastFile_, sizeof lastFile_, line + 5);
        if (startsWith(line, "open=")) lab::copyStr(crashed, sizeof crashed, line + 5);
        if (startsWith(line, "auto=")) autoOpen_ = line[5] == '1';
        if (startsWith(line, "heap=")) {
            const int v = line[5] - '0';
            if (v >= 1 && v <= 8) scriptHeapMB_ = v;
        }
        if (startsWith(line, "bar=")) sessionBar_ = line[4] == '1';
        line = nl ? nl + 1 : nullptr;
    }
    if (crashed[0]) {
        // the previous session did not get past its first seconds: do not reopen it
        lab::copyStr(openFile_, sizeof openFile_, crashed);
        guardSet_ = true;   // remembered until writeState() clears it
    }
}

void Platform::writeState() {
    if (!ctx_->fs) return;
    char p[80];
    std::snprintf(p, sizeof p, "%s/%s", ctx_->home, kStateFile);
    const ot::IStorage::Handle h = ctx_->fs->open(p, ot::IStorage::kWrite);
    if (h < 0) return;
    char buf[200];
    const int n = std::snprintf(buf, sizeof buf, "last=%s\nopen=%s\nauto=%d\nheap=%d\nbar=%d\n", lastFile_,
                                guardSet_ ? openFile_ : "", autoOpen_, scriptHeapMB_, sessionBar_);
    ctx_->fs->write(h, buf, uint32_t(n));
    ctx_->fs->close(h);
}

void Platform::setGuard(const char* file, bool on) {
    guardSet_ = on;
    lab::copyStr(openFile_, sizeof openFile_, on ? file : "");
    writeState();
}

bool Platform::audioRunning() const {
    // The Shell's audio callback sets the transport rate on its first block;
    // without audio (host tests, the CPU bench) nothing waits for it.
    return ctx_->shell && ctx_->shell->transport().beatsPerSample > 0.0;
}

// ---- LABKIT state ---------------------------------------------------------------------
void Platform::waitBlock() {
    // One audio block: a kit block that was running when the flag changed has
    // ended (device: the main loop never sees half an interrupt; simulator:
    // the audio thread). Bounded -- without audio nothing waits.
    if (!audioRunning()) return;
    // Bounded twice: by time, and by spins -- the scripted simulator's clock
    // and audio only advance between main-loop passes.
    const uint32_t b0 = blocks_, t0 = ctx_->millis();
    for (uint32_t spins = 0; blocks_ == b0 && ctx_->millis() - t0 < 20u && spins < 400000u; ++spins) {
    }
}

void Platform::ensureSnapshot() {
    if (!snap_ || snap_->taken || !ctx_->shell) return;
    lab::kit::Kit& k = ctx_->shell->labkit();
    std::memcpy(static_cast<void*>(snap_->reg), static_cast<const void*>(&k.reg), sizeof snap_->reg);
    std::memcpy(static_cast<void*>(snap_->mod), static_cast<const void*>(&k.mod), sizeof snap_->mod);
    std::memcpy(static_cast<void*>(snap_->nav), static_cast<const void*>(&k.nav), sizeof snap_->nav);
    snap_->count = k.reg.count() < lab::kit::kMaxParams ? k.reg.count() : lab::kit::kMaxParams;   // the Shell's ~34
    if (k.reg.baseArray() && snap_->count > 0) {
        std::memcpy(snap_->base, k.reg.baseArray(), sizeof(float) * uint32_t(snap_->count));
        std::memcpy(snap_->eff, k.reg.effArray(), sizeof(float) * uint32_t(snap_->count));
    }
    snap_->wsParams = k.wsParams;
    snap_->active = k.active;
    snap_->taken = true;
}

void Platform::kitOff() {
    if (!ctx_->shell) return;
    ctx_->shell->labkit().active = false;
    waitBlock();
}

void Platform::kitOn() {
    if (!ctx_->shell || !snap_ || !snap_->taken) return;
    lab::kit::Kit& k = ctx_->shell->labkit();
    k.active = snap_->active;
    if (k.active) k.nav.refreshPages();
}

void Platform::restoreKit() {
    if (!ctx_->shell || !snap_ || !snap_->taken) return;
    lab::kit::Kit& k = ctx_->shell->labkit();
    lab::kit::Registry& R = k.reg;
    // The session's params: a dirty bit left for one of them is dropped by the
    // next applyBlock (mods != 0: "the mod plan writes it") instead of writing
    // through a binding into freed session memory.
    for (int i = snap_->count; i < R.count(); ++i) R.meta(lab::kit::ParamId(i)).mods = 1;
    std::memcpy(static_cast<void*>(&k.reg), static_cast<const void*>(snap_->reg), sizeof snap_->reg);
    std::memcpy(static_cast<void*>(&k.mod), static_cast<const void*>(snap_->mod), sizeof snap_->mod);
    std::memcpy(static_cast<void*>(&k.nav), static_cast<const void*>(snap_->nav), sizeof snap_->nav);
    if (R.baseArray() && snap_->count > 0) {
        std::memcpy(R.baseArray(), snap_->base, sizeof(float) * uint32_t(snap_->count));
        std::memcpy(R.effArray(), snap_->eff, sizeof(float) * uint32_t(snap_->count));
    }
    // route counts of the Shell's own params as the restored routes have them
    for (int i = 0; i < R.count(); ++i) {
        int n = 0;
        for (int r = 0; r < k.mod.routeCount(); ++r)
            if (k.mod.routeAt(r).param == i) ++n;
        if (R.meta(lab::kit::ParamId(i)).mods != n) R.setModCount(lab::kit::ParamId(i), n);
    }
    k.mod.markDirty();
    k.wsParams = snap_->wsParams;
}

uint32_t Platform::kitBaseParams() const { return snap_ && snap_->taken ? uint32_t(snap_->count) : 0u; }

void Platform::sessionNav() {
    if (!ctx_->shell) return;
    lab::kit::Kit& k = ctx_->shell->labkit();
    lab::kit::NavKeys keys;
    if (mode_ == kAppRun) native_->navKeys(keys);
    k.nav.setKeys(keys);
    sessTheme_ = mode_ == kAppRun ? native_->labTheme() : nullptr;
    ctx_->theme = sessTheme_ ? sessTheme_ : &theme_;
    if (k.active) k.nav.refreshPages();
}

void Platform::toError() {
    lab::copyStr(errTitle_, sizeof errTitle_, lua_->name());
    lab::copyStr(errText_, sizeof errText_, lua_->error());
    errApp_ = false;
    mode_ = kError;
    if (ctx_->shell && ctx_->shell->labkit().active) ctx_->shell->labkit().nav.close();
    ctx_->requestRedraw();
}

// ---- sessions ----------------------------------------------------------------------------
void Platform::closeSession(bool toLauncher) {
    nextOpen_ = -1;
    nextFile_[0] = 0;
    logOpen_ = false;
    if (mode_ == kScriptRun || mode_ == kAppRun || mode_ == kError) {
        kitOff();             // no mod / registry writes into the session from here on
        audioQuiet_ = true;   // render() stops calling the native app from the next block
        audioNative_ = false;
        if (lua_->running() || lua_->failed()) lua_->close();   // cleanup(), params saved
        engine_->requestReset();
        mode_ = kClosing;
        if (!audioRunning()) {
            lab::Transport t;
            float l[lab::kBlock], r[lab::kBlock];
            engine_->render(t, l, r);   // acknowledges the reset here
        }
        if (engine_->resetDone()) finishClose();
    } else if (mode_ != kClosing) {
        mode_ = kLauncher;
    }
    if (guardSet_) setGuard("", false);
    if (toLauncher && mode_ == kLauncher) rescan();
}

// The audio side has dropped the session (reset acknowledged): free it.
void Platform::finishClose() {
    native_->close();
    restoreKit();
    pool_.reset();
    mode_ = kLauncher;
    audioQuiet_ = false;
    sessTheme_ = nullptr;
    ctx_->theme = &theme_;
    kitOn();
    if (nextFile_[0]) {
        char f[48];
        lab::copyStr(f, sizeof f, nextFile_);
        nextFile_[0] = 0;
        rescan();
        for (int i = 0; i < nEntries_; ++i)
            if (!std::strcmp(entries_[i].file, f)) {
                openNow(i);
                return;
            }
    }
    rescan();
}

bool Platform::openEntry(int i) {
    if (i < 0 || i >= nEntries_) return false;
    bootOpen_ = -1;
    if (mode_ == kScriptRun || mode_ == kAppRun || mode_ == kError || mode_ == kClosing) {
        char f[48];
        lab::copyStr(f, sizeof f, entries_[i].file);
        if (mode_ != kClosing) closeSession(false);
        lab::copyStr(nextFile_, sizeof nextFile_, f);
        if (mode_ == kLauncher) {   // closed at once (no audio running)
            nextFile_[0] = 0;
            for (int k = 0; k < nEntries_; ++k)
                if (!std::strcmp(entries_[k].file, f)) return openNow(k);
            return false;
        }
        return true;   // update() opens it when the audio side let go
    }
    return openNow(i);
}

bool Platform::openNow(int i) {
    const Entry en = entries_[i];
    ensureSnapshot();
    kitOff();   // the session registers its params with the kit's audio block off
    restoreKit();
    pool_.reset();
    audioQuiet_ = false;
    lab::copyStr(lastFile_, sizeof lastFile_, en.file);
    setGuard(en.file, true);   // cleared after kCrashGuardMs of normal running
    openedMs_ = ctx_->millis();
    char path[128];
    char name[32];
    baseName(name, sizeof name, en.file);
    bool ok;
    errApp_ = en.kind != Entry::kScript;
    if (en.kind == Entry::kScript) {
        std::snprintf(path, sizeof path, "%s/%s/%s", ctx_->home, kScriptsDir, en.file);
        LuaConfig cfg;
        cfg.heapBytes = uint32_t(scriptHeapMB_) * 1024u * 1024u;
        mode_ = kScriptRun;
        ok = lua_->open(pool_, *ctx_, *engine_, path, name, cfg);
        if (!ok) {
            lab::copyStr(errTitle_, sizeof errTitle_, en.name);
            lab::copyStr(errText_, sizeof errText_, lua_->error());
            mode_ = kError;
        }
    } else {
        std::snprintf(path, sizeof path, "%s/%s/%s", ctx_->home, kAppsDir, en.file);
        char data[64];
        std::snprintf(data, sizeof data, "%s/Data/%s", ctx_->home, name);
        ok = native_->open(pool_, *ctx_, path, data);
        if (ok) {
            mode_ = kAppRun;
            audioNative_ = true;
        } else {
            lab::copyStr(errTitle_, sizeof errTitle_, en.name);
            lab::copyStr(errText_, sizeof errText_, native_->error());
            errApp_ = true;
            mode_ = kError;
        }
    }
    if (!ok) {
        setGuard("", false);
        native_->close();
        restoreKit();   // whatever the failed session registered
        sessTheme_ = nullptr;
        ctx_->theme = &theme_;
        kitOn();
    } else {
        kitOn();
        sessionNav();
    }
    ctx_->requestRedraw();
    return ok;
}

bool Platform::openByName(const char* n) {
    for (int i = 0; i < nEntries_; ++i)
        if (!std::strcmp(entries_[i].file, n) || !std::strcmp(entries_[i].name, n)) return openEntry(i);
    return false;
}

void Platform::openSelected() {
    if (sel_ >= 0 && sel_ < nView_) openEntry(view_[sel_]);
}

// ---- events ----------------------------------------------------------------------------
// Only keys of the original Tracker's matrix: Shift + F8 held = back to the
// launcher, Shift + F7 held = the LOG view (and the REPL). A tap of either
// still reaches the session, on the release (tests/lab/platform_test.cpp).
bool Platform::chordEvent(const Event& e) {
    const int k = e.id == btn::F7 ? 0 : (e.id == btn::F8 ? 1 : -1);
    if (k < 0 || !(e.type == Event::kPress || e.type == Event::kRelease || e.type == Event::kHold)) return false;
    if (e.type == Event::kPress && e.shift) {
        chordPending_[k] = true;
        chordPress_[k] = e;
        return true;
    }
    if (e.type == Event::kHold) {
        if (chordPending_[k]) {
            chordPending_[k] = false;
            chordSwallow_[k] = true;
            if (k == 1) {
                logOpen_ = false;
                if (mode_ != kLauncher) closeSession(true);
            } else {
                logOpen_ = !logOpen_;
                logScroll_ = 0;
            }
            ctx_->requestRedraw();
            return true;
        }
        return chordSwallow_[k];
    }
    // release
    if (chordSwallow_[k]) {
        chordSwallow_[k] = false;
        return true;
    }
    if (chordPending_[k]) {
        chordPending_[k] = false;
        const Event press = chordPress_[k];
        if (logOpen_) {
            logEvent(press);
            logEvent(e);
        } else {
            route(press);
            route(e);
        }
        return true;
    }
    return false;
}

void Platform::onEvent(const Event& e) {
    if (bootOpen_ >= 0) {   // any key or pad during the boot countdown: safe boot, stay here
        bootOpen_ = -1;
        safeBoot_ = true;
        ctx_->toast("Safe boot: staying in the launcher");
        return;
    }
    if (!bootChecked_) safeBoot_ = true;
    if (mode_ == kClosing) return;
    if (chordEvent(e)) return;
    if (logOpen_) {
        logEvent(e);
        return;
    }
    route(e);
}

void Platform::route(const Event& e) {
    switch (mode_) {
        case kLauncher: launcherEvent(e); break;
        case kScriptRun:
            lua_->event(e);
            if (lua_->failed()) toError();
            break;
        case kAppRun: native_->event(e); break;
        case kError:
            if (e.type != Event::kPress) break;
            if (e.id == btn::F8) closeSession(true);
            if ((e.id == btn::F7 || e.id == btn::Insert) && lastFile_[0]) {
                char f[48];
                lab::copyStr(f, sizeof f, lastFile_);
                closeSession(false);
                rescan();
                openByName(f);
            }
            if (e.id == btn::F1) {
                logOpen_ = true;
                logScroll_ = 0;
            }
            break;
        default: break;
    }
}

// LOG view: the script's print() lines (64 kept) or an app's log, the
// crash-safe runner's errors, and a REPL line (Insert) for a running script.
void Platform::logEvent(const Event& e) {
    const int total = mode_ == kAppRun ? native_->logCount() : lua_->consoleCount();
    int d = 0;
    if (e.type == Event::kEncoder) d = -e.delta;
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: d = 1; break;
            case btn::Down: d = -1; break;
            case btn::Left: d = 12; break;
            case btn::Right: d = -12; break;
            case btn::F8:
                logOpen_ = false;
                break;
            case btn::Delete:
            case btn::F2:
                if (mode_ != kAppRun) lua_->clearConsole();
                logScroll_ = 0;
                break;
            case btn::Insert:
            case btn::F1:
                if (mode_ == kScriptRun && lua_->running()) {
                    lab::kit::KeyboardRequest r;
                    r.title = "Lua  (id shows a param, id(v) sets it)";
                    r.maxLen = 31;
                    r.ctx = this;
                    r.onDone = [](void* c, const char* text) {
                        Platform* p = static_cast<Platform*>(c);
                        if (text && text[0] && p->mode_ == kScriptRun) {
                            p->lua_->eval(text);
                            p->logScroll_ = 0;
                            if (p->lua_->failed()) p->toError();
                        }
                    };
                    ctx_->keyboard().open(r);
                } else {
                    ctx_->toast(mode_ == kScriptRun ? "The script has stopped" : "The REPL needs a running script");
                }
                break;
            default: break;
        }
    }
    if (d) {
        logScroll_ += d;
        const int maxScroll = total > 14 ? total - 14 : 0;
        if (logScroll_ > maxScroll) logScroll_ = maxScroll;
        if (logScroll_ < 0) logScroll_ = 0;
    }
    ctx_->requestRedraw();
}

void Platform::launcherEvent(const Event& e) {
    if (e.type == Event::kPadDown && e.id < nView_) {
        sel_ = e.id;
        openSelected();
        return;
    }
    int d = 0;
    if (e.type == Event::kEncoder) d = e.delta;
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: d = -1; break;
            case btn::Down: d = 1; break;
            case btn::Left: d = -6; break;
            case btn::Right: d = 6; break;
            case btn::Insert:
            case btn::F1: openSelected(); return;
            case btn::F3:
                preview_ = !preview_;
                previewFor_ = -1;
                return;
            case btn::F2:
                filter_ = (filter_ + 1) % 3;
                rescan();
                return;
            case btn::F5:
                rescan();
                ctx_->toast("Card scanned");
                return;
            case btn::F6:
                installExamples(true);
                rescan();
                ctx_->toast("Example scripts written to the card");
                return;
            default: break;
        }
    }
    if (d && nView_) {
        sel_ += d;
        if (sel_ < 0) sel_ = 0;
        if (sel_ >= nView_) sel_ = nView_ - 1;
    }
}

void Platform::onMidi(const lab::MidiMsg& m) {
    if (mode_ == kScriptRun) lua_->midi(m);
    if (mode_ == kAppRun) native_->midi(m);
}

void Platform::onTransport(bool playing) {
    if (mode_ == kScriptRun) lua_->transport(playing);
    if (mode_ == kAppRun) native_->transport(playing);
}

void Platform::update(uint32_t ms) {
    nowMs_ = ms;
    ensureSnapshot();   // first pass: the Shell's kit is complete
    if (!bootChecked_) {
        bootChecked_ = true;
        bootStart_ = ms;
        // a session that did not survive its first seconds last time is not reopened
        if (guardSet_) {
            char m[96];
            std::snprintf(m, sizeof m, "%s did not start cleanly last time - not reopened", openFile_);
            setGuard("", false);
            ctx_->toast(m);
        } else if (autoOpen_ && lastFile_[0] && !safeBoot_) {
            for (int i = 0; i < nEntries_; ++i)
                if (!std::strcmp(entries_[i].file, lastFile_)) bootOpen_ = i;
        }
    }
    if (bootOpen_ >= 0 && ms - bootStart_ >= kBootCountdownMs) {
        const int i = bootOpen_;
        bootOpen_ = -1;
        openEntry(i);
    }
    if (mode_ == kClosing) {
        if (engine_->resetDone()) finishClose();
        return;
    }
    if (installed_ > 0 && mode_ == kLauncher) {
        char m[64];
        std::snprintf(m, sizeof m, "Installed %d app%s from /Firmware/Apps", installed_, installed_ == 1 ? "" : "s");
        ctx_->toast(m);
        installed_ = 0;
    }
    if (mode_ == kLauncher && preview_ && sel_ >= 0 && sel_ < nView_ && previewFor_ != view_[sel_]) loadPreview(view_[sel_]);
    if (guardSet_ && (mode_ == kScriptRun || mode_ == kAppRun) && ms - openedMs_ >= kCrashGuardMs) setGuard("", false);
    if (mode_ == kScriptRun) {
        lua_->update(ms);
        if (lua_->failed()) toError();
    } else if (mode_ == kAppRun) {
        native_->update(ms);
    }
}

void Platform::save() {
    if (mode_ == kScriptRun) lua_->save();
    if (mode_ == kAppRun) native_->save();
    writeState();
}

int Platform::settings(lab::SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names) {
        if (n >= cap) return;
        lab::SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.ctx = this;
        r.onChange = [](void* c, int) { static_cast<Platform*>(c)->writeState(); };
    };
    add("Reopen last script / app", &autoOpen_, 0, 1, kOffOn);
    add("Script memory (next open)", &scriptHeapMB_, 1, 8, kHeap);   // names[value - min]
    add("Session top bar", &sessionBar_, 0, 1, kOffOn);
    if (mode_ == kAppRun) n += native_->settings(rows + n, cap - n);
    return n;
}

// ---- audio ------------------------------------------------------------------------------
void Platform::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    // A native app owns the block: the voice engine (Lua's) is silent and was
    // reset before the app opened, so it is skipped -- the hosting overhead of
    // a station is then only the call bridge (hosted bench, make lab-gate).
    blocks_ = blocks_ + 1u;
    if (audioNative_ && !audioQuiet_) {
        native_->render(t, in, l, r);
        return;
    }
    engine_->render(t, l, r);
}

// ---- LEDs ---------------------------------------------------------------------------------
void Platform::leds(Rgb* pads) {
    for (int i = 0; i < lab::kPads; ++i) pads[i] = Rgb();
    if (mode_ == kScriptRun) {
        lua_->leds(pads);
        return;
    }
    if (mode_ == kAppRun) {
        native_->leds(pads);
        return;
    }
    if (mode_ == kError) {
        const float k = 0.5f + 0.5f * sinf(float(nowMs_) * 0.006f);
        for (int i = 0; i < lab::kPads; ++i) pads[i] = lab::scale(theme_.warn, 0.1f + 0.3f * k);
        return;
    }
    // launcher: one pad per entry, the selection bright
    for (int v = 0; v < nView_ && v < lab::kPads; ++v) {
        const Entry& en = entries_[view_[v]];
        const Rgb base = en.color ? Rgb(en.color) : (en.kind == Entry::kScript ? theme_.accent : theme_.accent2);
        pads[v] = v == sel_ ? base : lab::scale(base, 0.12f);
    }
}

// ---- drawing --------------------------------------------------------------------------------
void Platform::drawIcon(lab::Canvas& g, const char* icon, int cx, int cy, float r, Rgb c, uint32_t t) {
    g.color(c);
    const float ph = float(t % 4000) / 4000.0f * 6.2831853f;
    if (!std::strcmp(icon, "drone")) {
        g.ring(float(cx), float(cy), r, 20);
        g.ring(float(cx), float(cy), r * 0.6f + 2.0f * sinf(ph), 16);
    } else if (!std::strcmp(icon, "seq") || !std::strcmp(icon, "grid")) {
        g.beginRects();
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j)
                if ((i + j * 2) % 3 != 1) {
                    const float x = float(cx) - r + float(i) * r * 0.52f, y = float(cy) - r * 0.75f + float(j) * r * 0.55f;
                    g.vtx(x, y);
                    g.vtx(x + r * 0.38f, y + r * 0.38f);
                }
        g.endBatch();
    } else if (!std::strcmp(icon, "euclid")) {
        g.ring(float(cx), float(cy), r, 16);
        g.pointSize(3.0f);
        g.beginPoints();
        for (int i = 0; i < 16; i += 3) {
            const float a = float(i) / 16.0f * 6.2831853f;
            g.vtx(float(cx) + r * cosf(a), float(cy) + r * sinf(a));
        }
        g.endBatch();
    } else if (!std::strcmp(icon, "arp")) {
        g.beginLines();
        for (int i = 0; i < 4; ++i) {
            const float x = float(cx) - r + float(i) * r * 0.6f, y = float(cy) + r * 0.6f - float(i) * r * 0.4f;
            g.vtx(x, y);
            g.vtx(x + r * 0.45f, y);
        }
        g.endBatch();
    } else if (!std::strcmp(icon, "spark")) {
        g.beginLines();
        for (int i = 0; i < 4; ++i) {
            const float a = float(i) * 0.785398f + ph * 0.25f;
            g.vtx(float(cx) - r * cosf(a), float(cy) - r * sinf(a));
            g.vtx(float(cx) + r * cosf(a), float(cy) + r * sinf(a));
        }
        g.endBatch();
    } else if (!std::strcmp(icon, "wave") || !std::strcmp(icon, "script")) {
        g.beginStrip();
        for (int i = 0; i <= 10; ++i)
            g.vtx(float(cx) - r + float(i) * r * 0.2f, float(cy) + r * 0.6f * sinf(float(i) * 0.9f + ph));
        g.endBatch();
    } else if (!std::strcmp(icon, "station")) {
        g.frame(cx - int(r), cy - int(r * 0.7f), int(2 * r), int(1.4f * r));
        g.fill(cx - int(r * 0.6f), cy - int(r * 0.3f), int(r * 1.2f), 3);
        g.fill(cx - int(r * 0.6f), cy + int(r * 0.15f), int(r * 0.8f), 3);
    } else {   // app / dots
        g.pointSize(3.5f);
        g.beginPoints();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) g.vtx(float(cx) + (float(i) - 1.0f) * r * 0.6f, float(cy) + (float(j) - 1.0f) * r * 0.6f);
        g.endBatch();
    }
}

void Platform::drawIntro(lab::Canvas& g, uint32_t t) {
    // Boot intro: a field of points condenses onto a ring, the name types in.
    const float k = float(t) / float(introMs());
    const float e = k < 0.7f ? k / 0.7f : 1.0f;
    const float ease = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);
    const float cx = 400.0f, cy = 200.0f;
    g.pointSize(2.5f);
    g.color(lab::mix(theme_.bg, theme_.accent, 0.3f + 0.7f * ease));
    g.beginPoints();
    for (int i = 0; i < 96; ++i) {
        const uint32_t h = uint32_t(i) * 2654435761u;
        const float sx = float(h % 800u), sy = float((h >> 10) % 480u);
        const float a = float(i) / 96.0f * 6.2831853f + k * 1.4f;
        const float rr = 110.0f + 8.0f * sinf(float(i) * 0.7f + k * 9.0f);
        const float tx = cx + rr * cosf(a), ty = cy + rr * sinf(a) * 0.62f;
        g.vtx(sx + (tx - sx) * ease, sy + (ty - sy) * ease);
    }
    g.endBatch();
    if (k > 0.35f) {
        g.color(lab::mix(theme_.bg, theme_.accent2, (k - 0.35f) * 3.0f));
        g.lineWidth(1.5f);
        g.beginStrip();
        for (int i = 0; i <= 48; ++i) {
            const float a = float(i) / 48.0f * 6.2831853f;
            g.vtx(cx + 72.0f * cosf(a), cy + 72.0f * sinf(a) * 0.62f + 6.0f * sinf(a * 3.0f + k * 12.0f));
        }
        g.endBatch();
        g.lineWidth(1.0f);
    }
    static const char kName[] = "PLATFORM";
    const int shown = k < 0.45f ? 0 : (k > 0.8f ? 8 : int((k - 0.45f) / 0.35f * 8.0f) + 1);
    char buf[12] = {};
    for (int i = 0; i < shown && i < 8; ++i) buf[i] = kName[i];
    if (shown < 8 && shown > 0 && (t / 120) % 2) buf[shown] = '_';
    g.color(theme_.fg);
    g.text(400, 360, font::kHuge, buf, Canvas::kCenter);
    if (k > 0.8f) {
        g.color(lab::mix(theme_.bg, theme_.dim, (k - 0.8f) * 5.0f));
        g.text(400, 400, font::kSmall, "scripts  -  apps  -  stations", Canvas::kCenter);
    }
}

void Platform::drawSessionBar(lab::Canvas& g) {
    if (!sessionBar_) return;
    // stations draw the lab header themselves (drawHeader at y 0..30)
    if (mode_ == kAppRun && (native_->info().flags & PLAT_INFO_WORKSTATION)) return;
    // a thin strip on top: name, memory, hint
    g.color(theme_.panel);
    g.fill(0, 0, lab::kScreenW, 22);
    g.color(mode_ == kAppRun ? theme_.accent2 : theme_.accent);
    g.fill(0, 21, lab::kScreenW, 1);
    const char* nm = mode_ == kScriptRun ? lua_->name() : native_->info().name;
    g.text(8, 11, font::kTiny, "PLATFORM", Canvas::kCenterY);
    g.color(theme_.fg);
    g.text(86, 11, font::kTiny, nm, Canvas::kCenterY);
    g.color(theme_.dim);
    char buf[96];
    if (mode_ == kScriptRun)
        std::snprintf(buf, sizeof buf, "lua %luK/%luK%s   %.0f bpm   CPU %lu%%", static_cast<unsigned long>(lua_->heapUsed() / 1024),
                      static_cast<unsigned long>(lua_->heapCap() / 1024), lua_->screenDropped() ? "  SCREEN FULL" : "",
                      double(ctx_->bpm()), static_cast<unsigned long>(ctx_->shell->cpuPermille() / 10));
    else
        std::snprintf(buf, sizeof buf, "native %luK%s   %.0f bpm   CPU %lu%%",
                      static_cast<unsigned long>(native_->imageBytes() / 1024), native_->fastInSram() ? " +fast" : "",
                      double(ctx_->bpm()), static_cast<unsigned long>(ctx_->shell->cpuPermille() / 10));
    if (mode_ == kScriptRun && lua_->errorsTotal() > 0) {
        // the crash-safe runner kept the script going: a red count, details in the LOG
        g.color(theme_.warn);
        g.textf(190, 11, font::kTiny, Canvas::kCenterY, "%d ERR (Shift+F7)", lua_->errorsTotal());
        g.color(theme_.dim);
    }
    g.text(560, 11, font::kTiny, buf, Canvas::kCenterY | Canvas::kRight);
    g.text(792, 11, font::kTiny, "hold Shift+F8: launcher", Canvas::kCenterY | Canvas::kRight);
}

void Platform::drawLauncher(lab::Canvas& g) {
    const lab::Theme& t = theme_;
    // header
    g.color(t.panel);
    g.fill(0, 0, lab::kScreenW, 44);
    g.color(t.accent);
    g.text(20, 22, font::kTitle, "PLATFORM", Canvas::kCenterY);
    g.color(t.dim);
    static const char* const kFilters[] = {"all", "scripts", "apps"};
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "%d %s on the card", nView_, kFilters[filter_]);
    g.text(196, 24, font::kSmall, hdr, Canvas::kCenterY);
    g.color(t.fg);
    g.textf(780, 22, font::kBody, Canvas::kCenterY | Canvas::kRight, "%.1f BPM", double(ctx_->bpm()));
    if (ctx_->playing()) {
        g.color(t.ok);
        g.point(668, 22, 5);
    }
    g.color(t.accent);
    g.fill(0, 44, lab::kScreenW, 1);

    if (!ctx_->fs) {
        g.color(t.warn);
        g.text(400, 200, font::kLarge, "No SD card", Canvas::kCenter);
        g.color(t.dim);
        g.text(400, 240, font::kSmall, "Scripts and apps live in /Lab/PLATFORM on the card.", Canvas::kCenter);
    } else if (!nView_) {
        g.color(t.dim);
        g.text(400, 200, font::kBody, "Nothing here yet", Canvas::kCenter);
        g.text(400, 236, font::kSmall, "Put .lua files in /Lab/PLATFORM/Scripts and apps in /Lab/PLATFORM/Apps,",
               Canvas::kCenter);
        g.text(400, 258, font::kSmall, "or press F6 to write the example scripts.", Canvas::kCenter);
    }
    // grid of cards: 3 columns x 2 rows on screen (2 with the preview), scrolled by row
    const int cols = preview_ ? 2 : 3, rowsVis = 2;
    const int cw = 248, ch = 150, x0 = 16, y0 = 58, gap = 8;
    if (sel_ < top_ * cols) top_ = sel_ / cols;
    if (sel_ >= (top_ + rowsVis) * cols) top_ = sel_ / cols - rowsVis + 1;
    for (int v = top_ * cols; v < nView_ && v < (top_ + rowsVis) * cols; ++v) {
        const Entry& en = entries_[view_[v]];
        const int c = v % cols, r = v / cols - top_;
        const int x = x0 + c * (cw + gap), y = y0 + r * (ch + gap);
        const bool s = v == sel_;
        const Rgb kc = en.color ? Rgb(en.color) : (en.kind == Entry::kScript ? t.accent : t.accent2);
        g.color(s ? lab::mix(t.panel, kc, 0.14f) : t.panel);
        g.fill(x, y, cw, ch);
        if (s) {
            g.color(kc);
            g.frame(x, y, cw, ch);
            g.frame(x + 1, y + 1, cw - 2, ch - 2);
        }
        drawIcon(g, en.icon, x + 38, y + 40, 20.0f, s ? kc : lab::scale(kc, 0.7f), nowMs_);
        g.color(t.fg);
        g.text(x + 74, y + 32, font::kLarge, en.name, Canvas::kCenterY);
        g.color(kc);
        static const char* const kKinds[] = {"LUA SCRIPT", "NATIVE APP", "STATION"};
        g.text(x + 74, y + 56, font::kTiny, kKinds[en.kind], Canvas::kCenterY);
        // description: two lines, cut at word boundaries
        g.color(t.dim);
        char l1[40] = {}, l2[40] = {};
        const char* d = en.desc;
        int cut = 0, lastSpace = -1;
        const int kCols = 30;   // font 27 in a 248 px card
        for (; d[cut] && cut < kCols; ++cut)
            if (d[cut] == ' ') lastSpace = cut;
        if (d[cut] && lastSpace > 0) cut = lastSpace;
        std::memcpy(l1, d, size_t(cut));
        const char* rest = d + cut + (d[cut] == ' ' ? 1 : 0);
        lab::copyStr(l2, kCols + 1, rest);
        if (std::strlen(rest) > size_t(kCols)) {
            l2[kCols - 3] = l2[kCols - 2] = l2[kCols - 1] = '.';
            l2[kCols] = 0;
        }
        g.text(x + 14, y + 92, font::kSmall, l1, Canvas::kCenterY);
        g.text(x + 14, y + 112, font::kSmall, l2, Canvas::kCenterY);
        if (en.author[0]) {
            g.color(lab::scale(t.dim, 0.8f));
            g.text(x + cw - 10, y + ch - 14, font::kTiny, en.author, Canvas::kCenterY | Canvas::kRight);
        }
        if (!std::strcmp(en.file, lastFile_)) {
            g.color(kc);
            g.text(x + 14, y + ch - 14, font::kTiny, "LAST", Canvas::kCenterY);
        }
    }
    // scroll marks
    const int rowsTotal = (nView_ + cols - 1) / cols;
    if (rowsTotal > rowsVis) {
        g.color(t.dim);
        const int h = 300 * rowsVis / rowsTotal;
        g.fill(preview_ ? 518 : 790, 58 + 300 * top_ / rowsTotal, 4, h);
    }
    if (preview_ && nView_) drawPreview(g, 528, 58, 256, 308);
    // footer hint
    g.color(t.dim);
    g.text(400, 432, font::kTiny,
           "Arrows / encoder: choose   Insert, F1 or a pad: open   hold Shift+F8: back here   hold Shift+F7: LOG",
           Canvas::kCenter);
    const char* keys[8] = {"Open", filter_ == 0 ? "All" : (filter_ == 1 ? "Scripts" : "Apps"),
                           preview_ ? "Cards" : "Preview", "", "Rescan", "Examples", "", ""};
    lab::drawFKeyBar(g, t, keys);
}

void Platform::drawError(lab::Canvas& g) {
    const lab::Theme& t = theme_;
    g.color(lab::mix(t.bg, t.warn, 0.12f));
    g.fill(0, 0, lab::kScreenW, 60);
    g.color(t.warn);
    g.text(24, 30, font::kTitle, errApp_ ? "APP STOPPED" : "SCRIPT STOPPED", Canvas::kCenterY);
    g.color(t.fg);
    g.text(330, 32, font::kBody, errTitle_, Canvas::kCenterY);
    g.color(t.warn);
    g.fill(0, 60, lab::kScreenW, 2);
    // wrap the message at ~84 characters, honouring newlines
    g.color(t.fg);
    int y = 92;
    const char* p = errText_;
    for (int line = 0; *p && line < 11; ++line) {
        char buf[90];
        int n = 0;
        while (p[n] && p[n] != '\n' && n < 84) ++n;
        if (p[n] && p[n] != '\n' && n == 84) {
            int b = n;
            while (b > 40 && p[b] != ' ') --b;
            if (b > 40) n = b;
        }
        std::memcpy(buf, p, size_t(n));
        buf[n] = 0;
        for (int i = 0; i < n; ++i)
            if (static_cast<unsigned char>(buf[i]) < 32 || static_cast<unsigned char>(buf[i]) > 126) buf[i] = '?';
        g.text(28, y, font::kSmall, buf, Canvas::kCenterY);
        y += 24;
        p += n;
        while (*p == '\n' || *p == ' ') ++p;
        if (line == 0) g.color(t.dim);
    }
    // the last lines the script printed
    if (lua_->consoleCount()) {
        g.color(t.panel);
        g.fill(20, 352, 760, 66);
        g.color(t.dim);
        const int n = lua_->consoleCount();
        const int from = n > 3 ? n - 3 : 0;
        for (int i = from; i < n; ++i)
            g.text(30, 366 + (i - from) * 18, font::kTiny, lua_->consoleLine(i), Canvas::kCenterY);
    }
    g.color(t.dim);
    g.text(400, 434, font::kTiny, "Sound stopped. Fix the file on the card (or by USB storage), then Retry.",
           Canvas::kCenter);
    const char* keys[8] = {"Log", "", "", "", "", "", "Retry", "Launcher"};
    lab::drawFKeyBar(g, t, keys);
}

// The first lines of a script (or an app's record) for the launcher's preview.
void Platform::loadPreview(int i) {
    previewFor_ = i;
    previewText_[0] = 0;
    if (i < 0 || i >= nEntries_) return;
    const Entry& en = entries_[i];
    if (en.kind != Entry::kScript || !ctx_->fs) {
        static const char* const kKinds[] = {"Lua script", "native app", "station"};
        std::snprintf(previewText_, sizeof previewText_, "%s\n%s\n\nfile  Apps/%s\nby    %s\n\n%s\n\nInsert / F1 opens it.",
                      en.name, kKinds[en.kind], en.file, en.author[0] ? en.author : "-", en.desc);
        return;
    }
    char path[128];
    std::snprintf(path, sizeof path, "%s/%s/%s", ctx_->home, kScriptsDir, en.file);
    const ot::IStorage::Handle h = ctx_->fs->open(path, ot::IStorage::kRead);
    if (h < 0) return;
    const int32_t n = ctx_->fs->read(h, previewText_, sizeof previewText_ - 1);
    ctx_->fs->close(h);
    previewText_[n > 0 ? n : 0] = 0;
}

void Platform::drawPreview(lab::Canvas& g, int x, int y, int w, int h) {
    const lab::Theme& t = theme_;
    g.color(lab::mix(t.bg, t.panel, 0.7f));
    g.fill(x, y, w, h);
    g.color(t.dim);
    g.frame(x, y, w, h);
    const Entry& en = entries_[view_[sel_]];
    g.color(en.color ? Rgb(en.color) : (en.kind == Entry::kScript ? t.accent : t.accent2));
    g.text(x + 10, y + 14, font::kTiny, en.kind == Entry::kScript ? "SOURCE" : "INFO", Canvas::kCenterY);
    const int cols = (w - 20) / 7, rows = (h - 36) / 16;
    const char* p = previewText_;
    for (int r = 0; r < rows && *p; ++r) {
        char line[48];
        int n = 0;
        while (p[n] && p[n] != '\n' && n < cols && n < int(sizeof line) - 1) ++n;
        const bool wrap = en.kind != Entry::kScript && p[n] && p[n] != '\n';   // info: wrap words
        if (wrap) {
            int b = n;
            while (b > 8 && p[b] != ' ') --b;
            if (b > 8) n = b;
        }
        for (int k = 0; k < n; ++k) {
            const unsigned char c = static_cast<unsigned char>(p[k]);
            line[k] = (c == '\t') ? ' ' : (c < 32 || c > 126 ? '?' : char(c));
        }
        line[n] = 0;
        // comments dim, code bright
        const bool comment = n > 1 && line[0] == '-' && line[1] == '-';
        g.color(comment ? t.dim : t.fg);
        if (n) g.text(x + 10, y + 34 + r * 16, font::kTiny, line, Canvas::kCenterY);
        p += n;
        if (wrap) {
            while (*p == ' ') ++p;
            continue;
        }
        while (*p && *p != '\n') ++p;   // source: cut long lines
        if (*p == '\n') ++p;
    }
}

void Platform::drawLog(lab::Canvas& g) {
    const lab::Theme& t = theme_;
    const bool app = mode_ == kAppRun;
    g.color(t.bg);
    g.fill(0, 0, lab::kScreenW, lab::kScreenH);
    g.color(t.panel);
    g.fill(0, 0, lab::kScreenW, 36);
    g.color(t.accent);
    g.text(14, 18, font::kBody, "LOG", Canvas::kCenterY);
    g.color(t.fg);
    g.text(80, 18, font::kSmall, app ? native_->info().name : lua_->name(), Canvas::kCenterY);
    const int total = app ? native_->logCount() : lua_->consoleCount();
    g.color(t.dim);
    if (app)
        g.textf(786, 18, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d lines", total);
    else
        g.textf(786, 18, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d lines   %d errors   %s", lua_->consoleTotal(),
                lua_->errorsTotal(), lua_->running() ? "running" : "stopped");
    g.color(t.accent);
    g.fill(0, 36, lab::kScreenW, 1);
    const int rows = 15;
    const int last = total - 1 - logScroll_;
    const int first = last - rows + 1 < 0 ? 0 : last - rows + 1;
    for (int i = first, r = 0; i <= last; ++i, ++r) {
        const char* line = app ? native_->logLine(i) : lua_->consoleLine(i);
        const bool err = line[0] == '!' || !std::strncmp(line, "error", 5);
        const bool in = line[0] == '>' && line[1] == ' ';
        g.color(err ? t.warn : (in ? t.accent2 : t.fg));
        g.text(14, 52 + r * 22, font::kTiny, line, Canvas::kCenterY);
    }
    if (!total) {
        g.color(t.dim);
        g.text(400, 200, font::kSmall, app ? "The app logged nothing (PlatHost::log)." : "Nothing printed yet (print in a script).",
               Canvas::kCenter);
    }
    if (!app && lua_->errorsTotal() > 0) {
        g.color(lab::mix(t.bg, t.warn, 0.15f));
        g.fill(0, 384, lab::kScreenW, 40);
        g.color(t.warn);
        g.textf(14, 396, font::kTiny, Canvas::kCenterY, "last error (%lu s ago):",
                static_cast<unsigned long>((nowMs_ - lua_->lastErrorMs()) / 1000u));
        g.color(t.fg);
        g.text(14, 414, font::kTiny, lua_->lastError(), Canvas::kCenterY);
    }
    g.color(t.dim);
    g.text(400, 436, font::kTiny, "Up / Down / encoder: scroll   Insert: REPL line   Delete: clear   F8 or hold Shift+F7: close",
           Canvas::kCenter);
    const char* keys[8] = {app ? "" : "REPL", app ? "" : "Clear", "", "", "", "", "", "Close"};
    lab::drawFKeyBar(g, t, keys);
}

void Platform::draw(lab::Canvas& g) {
    if (logOpen_ && mode_ != kClosing) {
        drawLog(g);
        return;
    }
    switch (mode_) {
        case kClosing:
        case kLauncher:
            drawLauncher(g);
            if (bootOpen_ >= 0) {
                const uint32_t left = kBootCountdownMs - (nowMs_ - bootStart_ < kBootCountdownMs ? nowMs_ - bootStart_ : kBootCountdownMs);
                g.color(theme_.panel);
                g.fillRound(150, 364, 500, 52, 8);
                g.color(theme_.accent);
                g.frame(150, 364, 500, 52);
                g.textf(400, 380, font::kBody, Canvas::kCenter, "Opening %s", entries_[bootOpen_].name);
                g.color(theme_.dim);
                g.textf(400, 402, font::kTiny, Canvas::kCenter, "in %.1f s - press any key or pad to stay here (safe boot)",
                        double(left) / 1000.0);
                g.color(theme_.accent);
                g.fill(152, 412, int(496.0f * float(left) / float(kBootCountdownMs)), 2);
            }
            break;
        case kError: drawError(g); break;
        case kScriptRun: {
            BudgetCanvas c(g, kAppWords - (sessionBar_ ? 120u : 0u));
            lua_->draw(c);
            if (lua_->consoleCount()) {
                // most recent print line at the bottom
                g.color(theme_.dim);
                if (g.room(100)) g.text(10, 470, font::kTiny, lua_->consoleLine(lua_->consoleCount() - 1), Canvas::kCenterY);
            }
            drawSessionBar(g);
            break;
        }
        case kAppRun: {
            const bool station = (native_->info().flags & PLAT_INFO_WORKSTATION) != 0;
            // a station gets the whole frame budget of a firmware (the Shell draws the toast / REC after)
            BudgetCanvas c(g, station ? lab::Canvas::kBudgetWords - 120u : kAppWords - (sessionBar_ ? 120u : 0u));
            native_->draw(g, c);
            drawSessionBar(g);
            break;
        }
    }
}

// ---- LABKIT navigation ----------------------------------------------------------------------
int Platform::navPages(lab::kit::PageInfo* out, int cap) {
    if (cap <= 0) return 0;
    if (mode_ == kAppRun) {
        const int n = native_->navPages(out, cap);
        if (n > 0) return n;
        out[0] = lab::kit::PageInfo();
        out[0].name = native_->info().name;
        out[0].help = "The app's params (PlatHost::paramAdd)";
        return 1;
    }
    if (mode_ == kScriptRun && lua_->pageCount() > 0) {
        const int n = lua_->pageCount() < cap ? lua_->pageCount() : cap;
        for (int i = 0; i < n; ++i) {
            out[i] = lab::kit::PageInfo();
            out[i].name = lua_->pageName(i);
            out[i].help = "Script params: params:add{id, name, min, max, default, unit, page}";
        }
        return n;
    }
    static const char* const kLauncherKeys[8] = {"Open the selected script / app", "Filter: all / scripts / apps",
                                                 "Preview: source / info", nullptr, "Rescan the card",
                                                 "Write the example scripts", nullptr, nullptr};
    out[0] = lab::kit::PageInfo();
    if (mode_ == kScriptRun) {
        out[0].name = "SCRIPT";
        out[0].help = "This script has no params (params:add) - macros and LFOs still run";
    } else {
        out[0].name = "LAUNCHER";
        out[0].help = "Shift+F8 held: back here.  Shift+F7 held: LOG / REPL.";
        for (int i = 0; i < 8; ++i) out[0].fkeys[i] = kLauncherKeys[i];
    }
    return 1;
}

int Platform::navPage() const {
    if (mode_ == kAppRun) return native_->navPage();
    return -1;
}

void Platform::navGoto(int page) {
    if (mode_ == kAppRun) native_->navGoto(page);
}

void Platform::navFocus(lab::kit::ParamId id) {
    if (mode_ == kAppRun) native_->navFocus(id);
}

void Platform::navParamChanged(lab::kit::ParamId id) {
    if (mode_ == kAppRun) native_->navParamChanged(id);
}

// ---- bench ------------------------------------------------------------------------------------
const char* Platform::benchName(int i) const {
    static const char* const n[] = {"idle", "launcher", "voices16", "voices16_fx", "grains_samples", "lua_clock"};
    return (i >= 0 && i < 6) ? n[i] : "?";
}

void Platform::benchSetup(int i) {
    benchMode_ = i;
    bootOpen_ = -1;
    bootChecked_ = true;
    closeSession(false);   // no audio in the bench: closes at once
    pool_.reset();
    auto apply = [&](const Cmd& c) { engine_->applyNow(c); };
    auto set = [&](int v, int p, float x) {
        Cmd c;
        c.op = Cmd::kSet;
        c.a = uint8_t(v);
        c.b = uint8_t(p);
        c.v = x;
        apply(c);
    };
    auto on = [&](int v, float note) {
        Cmd c;
        c.op = Cmd::kOn;
        c.a = uint8_t(v);
        c.v = note;
        c.frames = 60000;
        apply(c);
    };
    auto fx = [&](int p, float x) {
        Cmd c;
        c.op = Cmd::kFx;
        c.b = uint8_t(p);
        c.v = x;
        apply(c);
    };
    if (i == 2 || i == 3) {
        // every voice sounding, the heaviest oscillators, filter + drive + filter env
        static const uint8_t waves[] = {kSaw, kSquare, kFm, kSaw, kFm, kSquare, kNoise, kSaw};
        for (int v = 0; v < kVoices; ++v) {
            set(v, pWave, float(waves[v % 8]));
            set(v, pCutoff, 600.0f + float(v) * 300.0f);
            set(v, pRes, 4.0f);
            set(v, pFenv, 2.0f);
            set(v, pDrive, 0.5f);
            set(v, pDetune, 7.0f);
            set(v, pSustain, 0.8f);
            set(v, pRelease, 5.0f);
            set(v, pAmp, 0.05f);
            if (i == 3) {
                set(v, pSendDelay, 0.5f);
                set(v, pSendReverb, 0.6f);
            }
            on(v, 36.0f + float(v) * 3.0f);
        }
        if (i == 3) {
            fx(fDelayFeedback, 0.7f);
            fx(fReverbSize, 0.95f);
        }
    } else if (i == 4) {
        // sample + grain voices on a synthetic stereo sample in the pool
        const uint32_t frames = 44100;
        int16_t* s = static_cast<int16_t*>(pool_.alloc(frames * 2 * 2, 16));
        if (s) {
            for (uint32_t k = 0; k < frames; ++k) {
                s[k] = int16_t(12000.0f * sinf(float(k) * 0.031f));
                s[frames + k] = int16_t(9000.0f * sinf(float(k) * 0.047f));
            }
            Cmd c;
            c.op = Cmd::kSlot;
            c.a = 0;
            c.b = 2;
            c.ptr = s;
            c.frames = frames;
            apply(c);
        }
        for (int v = 0; v < kVoices; ++v) {
            set(v, pWave, v < 8 ? float(kSample) : float(kGrain));
            set(v, pLoop, 1.0f);
            set(v, pRate, 0.7f + 0.1f * float(v % 5));
            set(v, pCutoff, 2000.0f);
            set(v, pRes, 2.0f);
            set(v, pGrainDensity, 120.0f);
            set(v, pGrainSize, 0.2f);
            set(v, pSendReverb, 0.4f);
            set(v, pSendDelay, 0.3f);
            set(v, pAmp, 0.05f);
            set(v, pSustain, 1.0f);
            on(v, 60.0f);
        }
    } else if (i == 5) {
        // a real Lua script driving all voices from clock coroutines; update() runs between blocks
        LuaConfig cfg;
        if (lua_->openSource(pool_, *ctx_, *engine_, "bench", kBenchScript, sizeof kBenchScript - 1, cfg))
            mode_ = kScriptRun;
    }
}

}  // namespace plat

lab::Workstation* lab::createWorkstation(lab::Arena& arena) { return arena.make<plat::Platform>(); }
