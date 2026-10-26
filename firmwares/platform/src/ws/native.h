// PLATFORM -- native app sessions (MANUAL.md "Native apps").
//
// Device (and the QEMU test build): a .papp image (papp.h) read from the card
// into the session pool (SDRAM), its fast segment into a reserved SRAM_L
// buffer when it fits, relocated, caches synchronised, constructors run, then
// called through the PlatApp table. Simulator / host tests: the same app
// built as a host shared library, loaded with dlopen from a copy of the file
// read through IStorage (memfd), so it is still "loaded from the card".
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "platform_app.h"
#include "ws/budget_canvas.h"
#include "ws/pool.h"
#include "ws/papp.h"

namespace plat {

#if defined(__arm__)
constexpr const char* kAppExt = ".papp";
#else
constexpr const char* kAppExt = ".so";
#endif
constexpr uint32_t kFastRamBytes = PLAT_FAST_MAX;   // SRAM_L buffer for .fast segments (device)

// Header / metadata of an app file without running it.
bool readAppInfo(lab::Context& ctx, const char* path, PlatInfo& info);

class NativeSession {
public:
    bool open(Pool& pool, lab::Context& ctx, const char* path, const char* dataDir);
    void close();
    bool running() const { return app_ != nullptr && !failed_; }
    bool failed() const { return failed_; }
    const char* error() const { return error_; }
    const PlatApp* app() const { return app_; }
    const PlatInfo& info() const { return info_; }
    bool fastInSram() const { return fastInSram_; }
    uint32_t imageBytes() const { return imageBytes_; }
    uint32_t fastBytes() const { return fastBytes_; }

    // main loop
    void event(const lab::Event& e);
    void midi(const lab::MidiMsg& m);
    void update(uint32_t ms);
    void draw(lab::Canvas& g, BudgetCanvas& c);
    void drawIntro(lab::Canvas& g, BudgetCanvas& c, uint32_t ms);
    void leds(lab::Rgb* pads);
    void transport(bool playing);
    int settings(lab::SettingRow* rows, int cap);
    void save();
    // CPU bench scenarios of the app (hosted bench; 0 when the app has none)
    int benchScenarios() const;
    const char* benchName(int i) const;
    void benchSetup(int i);
    // audio interrupt
    void render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r);

    // host services (static thunks reach these through PlatHost::ctx)
    lab::Context* ctx() { return ctx_; }
    Pool* pool() { return pool_; }
    BudgetCanvas* canvas() { return canvas_; }
    void log(const char* s);
    const char* logLine(int i) const { return log_[(logN_ - logCount() + i) % kLog]; }
    int logCount() const { return logN_ < kLog ? logN_ : kLog; }

private:
    void fail(const char* msg);
    void buildHost();
    bool loadImage(const char* path);

    lab::Context* ctx_ = nullptr;
    Pool* pool_ = nullptr;
    PlatHost host_{};
    const PlatApp* app_ = nullptr;
    PlatInfo info_{};
    BudgetCanvas* canvas_ = nullptr;
    lab::Canvas* labCanvas_ = nullptr;
    char home_[64] = {};
    bool failed_ = false;
    char error_[256] = {};
    bool fastInSram_ = false;
    uint32_t imageBytes_ = 0, fastBytes_ = 0;
    static constexpr int kLog = 6;
    char log_[kLog][80] = {};
    int logN_ = 0;
#if !defined(__arm__)
    void* dl_ = nullptr;
    int fd_ = -1;
#endif
};

}  // namespace plat
