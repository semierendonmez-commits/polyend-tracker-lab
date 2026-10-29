// PLATFORM -- the workstation: a launcher and host for Lua scripts and native
// apps from the SD card (MANUAL.md).
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "lab/nav.h"
#include "ws/engine.h"
#include "ws/lua_host.h"
#include "ws/native.h"

namespace plat {

struct Entry {
    enum Kind : uint8_t { kScript, kApp, kStation };
    Kind kind = kScript;
    char file[48] = {};      // file name in its folder
    char name[24] = {};
    char author[24] = {};
    char desc[96] = {};
    char icon[16] = {};
    uint32_t color = 0;      // accent from the app info (stations: their theme), 0 = default
};

class Platform final : public lab::Workstation {
public:
    static constexpr int kMaxEntries = 64;
    static constexpr uint32_t kArenaReserve = 64u * 1024u;          // left in the Shell arena
    static constexpr uint32_t kMinPool = 4u * 1024u * 1024u;        // smallest usable session pool
    static constexpr uint32_t kCrashGuardMs = 4000;              // "opened" flag cleared after this
    static constexpr uint32_t kAppWords = 1680;                  // display words an app may use

    const char* name() const override { return "PLATFORM"; }
    const char* tagline() const override { return "Lua scripts and native apps from the card"; }
    // a station's own theme while it runs (LABKIT overlays and the Shell use it)
    const lab::Theme& theme() const override { return sessTheme_ ? *sessTheme_ : theme_; }
    bool init(lab::Context& c) override;
    uint32_t introMs() const override { return 3000; }
    void drawIntro(lab::Canvas& g, uint32_t t) override;
    void onEvent(const lab::Event& e) override;
    void onMidi(const lab::MidiMsg& m) override;
    void update(uint32_t ms) override;
    void draw(lab::Canvas& g) override;
    void leds(lab::Rgb* pads) override;
    void render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) override;
    void onTransport(bool playing) override;
    int settings(lab::SettingRow* rows, int cap) override;
    void save() override;
    int benchScenarios() const override { return 6; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;
    // LABKIT navigation: the launcher's page, a script's params pages, an app's own pages
    int navPages(lab::kit::PageInfo* out, int cap) override;
    int navPage() const override;
    void navGoto(int page) override;
    void navFocus(lab::kit::ParamId id) override;
    void navParamChanged(lab::kit::ParamId id) override;

    // ---- used by tests / the simulator check ----
    enum Mode : uint8_t { kLauncher, kScriptRun, kAppRun, kError, kClosing };
    Mode mode() const { return mode_; }
    int entryCount() const { return nEntries_; }
    const Entry& entry(int i) const { return entries_[i]; }
    // Opens entry i: at once from the launcher; from a running session after the
    // audio side has dropped it (one audio block). Returns false on a load error
    // (the error page shows it) -- true while a switch is pending.
    bool openEntry(int i);
    bool openByName(const char* name);
    void closeSession(bool toLauncher = true);
    bool bootPending() const { return bootOpen_ >= 0; }
    Engine& engine() { return *engine_; }
    LuaSession& lua() { return *lua_; }
    NativeSession& native() { return *native_; }
    bool safeBoot() const { return safeBoot_; }
    bool logOpen() const { return logOpen_; }
    bool previewOpen() const { return preview_; }
    uint32_t kitBaseParams() const;   // registry params outside any session (the Shell's own)
    static constexpr uint32_t kHoldExitMs = lab::kHoldMs;   // Shift + F8 held: back to the launcher
    void rescan();
    uint32_t poolBytes() const { return pool_.size(); }

private:
    void installExamples(bool force);
    int installApps();   // /Firmware/Apps/*.papp -> /Lab/PLATFORM/Apps (missing or changed files)
    void scanScripts();
    void scanApps();
    void readState();
    void writeState();
    void setGuard(const char* file, bool on);
    void drawLauncher(lab::Canvas& g);
    void drawError(lab::Canvas& g);
    void drawSessionBar(lab::Canvas& g);
    void drawIcon(lab::Canvas& g, const char* icon, int cx, int cy, float r, lab::Rgb c, uint32_t t);
    void launcherEvent(const lab::Event& e);
    void openSelected();
    bool chordEvent(const lab::Event& e);   // Shift + F7 / F8 held
    void route(const lab::Event& e);        // to the launcher / session / error page
    void logEvent(const lab::Event& e);
    void drawLog(lab::Canvas& g);
    void drawPreview(lab::Canvas& g, int x, int y, int w, int h);
    void loadPreview(int entry);
    void toError();
    bool openNow(int i);
    // LABKIT state outside sessions: snapshot once after the Shell's init, the
    // audio block off while a session registers / is torn down, then restored.
    struct KitSnap;
    void ensureSnapshot();
    void kitOff();
    void kitOn();
    void restoreKit();
    void waitBlock();
    void sessionNav();   // after an open: keys, pages, theme
    void finishClose();
    bool audioRunning() const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine* engine_ = nullptr;
    LuaSession* lua_ = nullptr;
    NativeSession* native_ = nullptr;
    uint8_t* poolMem_ = nullptr;
    Pool pool_;
    Mode mode_ = kLauncher;
    volatile bool audioNative_ = false;   // render() calls the native app
    volatile bool audioQuiet_ = false;    // render() skips the session while it is replaced

    Entry entries_[kMaxEntries];
    int nEntries_ = 0, sel_ = 0, top_ = 0;
    int filter_ = 0;                      // 0 all, 1 scripts, 2 apps
    int view_[kMaxEntries];
    int nView_ = 0;
    char lastFile_[48] = {};
    char openFile_[48] = {};
    char errTitle_[48] = {};
    char errText_[640] = {};
    bool errApp_ = false;
    uint32_t openedMs_ = 0;
    bool guardSet_ = false;
    bool safeBoot_ = false;
    int autoOpen_ = 1;                    // Settings: reopen the last script / app at boot
    int scriptHeapMB_ = 3;                // Settings: Lua heap 1..8 MB
    int sessionBar_ = 1;                  // Settings: show the top bar in sessions
    bool bootChecked_ = false;
    int installed_ = 0;                   // apps copied from /Firmware/Apps at boot (toast)
    int bootOpen_ = -1;                   // entry reopened when the boot countdown ends
    uint32_t bootStart_ = 0;
    static constexpr uint32_t kBootCountdownMs = 1500;
    int nextOpen_ = -1;                   // entry opened once the closing session is gone
    char nextFile_[48] = {};
    uint32_t nowMs_ = 0;
    // Shift + F7 / Shift + F8: the press is held back until the hold (ours) or
    // the release (then press + release go to the session as usual)
    bool chordPending_[2] = {};
    bool chordSwallow_[2] = {};
    lab::Event chordPress_[2];
    bool logOpen_ = false;
    int logScroll_ = 0;                   // lines up from the newest
    bool preview_ = false;                // launcher: F3 source / info panel
    int previewFor_ = -1;
    char previewText_[1400] = {};
    KitSnap* snap_ = nullptr;
    volatile uint32_t blocks_ = 0;        // audio blocks rendered (waitBlock)
    const lab::Theme* sessTheme_ = nullptr;
    float meterPk_ = 0.0f;
    int benchMode_ = -1;
};

}  // namespace plat
