// BULUT workstation: pages, controls, LEDs, files, LABKIT registration (main loop side).
#pragma once

#include <cstdint>

#include "formats/wav.h"
#include "lab/lab.h"
#include "ws/bulut.h"
#include "ws/engine.h"
#include "ws/project.h"
#include "ws/spectral.h"

namespace bulut {

// The pages on the F-keys (F8 = the page's action).
enum Page : uint8_t { kPgClouds = 0, kPgSound, kPgSource, kPgFx, kPgMotion, kPgFreeze, kPgFile, kPages };
// LABKIT pages (the registry's pages; PARAM pads / MAP / HELP): CLOUDS, one
// page per channel (its 36 params), MOTION, FREEZE, FILE.
enum NavPage : uint8_t { kNavClouds = 0, kNavCh1 = 1, kNavMotion = 5, kNavFreeze = 6, kNavFile = 7, kNavPages = 8 };

// Channel param indices (pad order on the channel's PARAM-pads page).
namespace CP {
enum : uint8_t {
    K0 = 0,   // 0..11 engine knobs
    Gain = 12, Start, End, GenA, GenB, Bed, LfoRate, LfoShape, Level, Pan, Dly, Rev,
    Drive = 24, Flt, Cutoff, Reso, FxMode, FxTime, FxAmt, FxFb, GestSpeed, Engine, Source, GenType,
};
}
enum MasterIdx : uint8_t { kMDlyDiv = 0, kMDlyFb, kMDlyTone, kMDlyRet, kMRevSize, kMRevDamp, kMRevRet, kMMorph };

// One drawn particle (main loop): born from an audio Spark.
struct Particle {
    float x, y;            // 0..1 inside the field
    float vx, vy;          // per second
    float age, life;       // seconds
    uint8_t cloud, size, kind, live;
};
constexpr int kParticles = 640;

class Bulut final : public lab::Workstation {
public:
    const char* name() const override { return "BULUT"; }
    const char* tagline() const override { return "Granular cloud workstation"; }
    const lab::Theme& theme() const override { return theme_; }
    bool init(lab::Context& c) override;
    uint32_t introMs() const override { return 2800; }
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
    int benchScenarios() const override { return 11; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;
    // LABKIT
    int navPages(lab::kit::PageInfo* out, int cap) override;
    int navPage() const override;
    void navGoto(int page) override;
    void navFocus(lab::kit::ParamId id) override;
    void navParamChanged(lab::kit::ParamId id) override;

    // ---- used by the host tests --------------------------------------------
    Engine& engine() { return *eng_; }
    ChParams& params(int c) { return par_[c]; }
    ChState& state(int c) { return st_[c]; }
    ChSource& source(int c) { return src_[c]; }
    lab::kit::ParamId pid(int c, int idx) const { return pid_[c][idx]; }
    int selected() const { return sel_; }
    int page() const { return page_; }
    void setPage(int p);
    void selectChannel(int c);
    bool loadSample(int c, const char* path);
    bool swapSample(int c, int dir);        // next / previous WAV in the sample's folder
    bool saveProjectTo(const char* path);
    bool loadProjectFrom(const char* path);
    bool captureFreeze(int slot, int from);
    void recallFreeze(int slot);
    void storeScene(int s);
    void recallScene(int s, bool morph);
    bool sceneUsed(int s) const { return scenes_[s].used != 0; }
    FreezeSlot& freezeSlot(int i) { return frz_[i]; }
    void startResample();
    void stopResample();
    bool resampleBusy() const { return rsState_ != 0; }
    const char* lastResample() const { return rsPath_; }
    GestureRecorder& recorder() { return *rec_; }
    void setParam(int c, int idx, float value, bool record);   // in units
    void setNorm(int c, int idx, float norm, bool record);
    float norm(int c, int idx) const;
    void setEngine(int c, int engine);
    void setSource(int c, int src);
    void regionPad(int pad);
    bool regionMode() const { return regionMode_; }
    void tick(uint32_t ms) { update(ms); }

private:
    // registration (params.cpp)
    void registerParams();
    void renameKnobs(int c);
    void syncEngines();                 // engine params changed (any path): swap the knob sets
    int chOf(lab::kit::ParamId id, int* idx) const;
    // pages and keys
    void action(int k, bool shift);     // F8 / Shift + F1..F8
    void encoder(int delta, bool fine);
    void arrows(int id);
    void insertKey();
    void deleteKey(bool shift);
    void shiftPad(int pad);
    void gestureRecButton(int c);
    void finishGesture();
    void waitGesture(int c);
    void toggleFreeze(int c);
    void openBrowser(int c);
    int cursorParam() const;            // the channel param the page cursor is on (-1 = none)
    // files
    void reloadSources();
    void pumpResample();
    void pumpMorph(float dt);
    void pumpTimeline();
    void pumpParticles(float dt);
    void pumpParamEvents();
    void updatePeaks();
    void loadAutosave();
    void projectPath(int slot, char* out, uint32_t cap) const;
    void scanProjects();
    void newProject();
    void firstBoot();
    void addRoute(int c);
    // draw (draw.cpp)
    void drawLane(lab::Canvas& g, int c, int x, int y, int w, int h, bool big);
    void drawField(lab::Canvas& g, int c, int x, int y, int w, int h, bool big);
    void drawParticles(lab::Canvas& g, int c, int x, int y, int w, int h, bool big, uint32_t limit);
    void drawKnob(lab::Canvas& g, int c, int idx, int x, int y, int w, bool focused);
    void drawBadges(lab::Canvas& g, int c, int x, int y);
    void drawPageClouds(lab::Canvas& g);
    void drawPageSound(lab::Canvas& g);
    void drawPageSource(lab::Canvas& g);
    void drawPageFx(lab::Canvas& g);
    void drawPageMotion(lab::Canvas& g);
    void drawPageFreeze(lab::Canvas& g);
    void drawPageFile(lab::Canvas& g);
    void drawFooter(lab::Canvas& g);
    void paramText(int c, int idx, char* out, int cap) const;
    const char* paramName(int c, int idx) const;
    lab::Rgb chColor(int c) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine* eng_ = nullptr;
    Spectral spec_;
    GestureRecorder* rec_ = nullptr;
    ChParams par_[kCh];
    ChState st_[kCh];
    ChSource src_[kCh];
    Gesture* gest_ = nullptr;          // kGestBufs * kCh (engine.h)
    FxState fx_;
    SpecTools tools_;
    Scene* scenes_ = nullptr;          // kScenes
    TimeEntry timeline_[kTimeline];
    int tlLen_ = 0;
    FreezeSlot* frz_ = nullptr;        // kFreezeSlots
    float bpmSave_ = 120.0f;
    int songMode_ = 0;
    uint8_t* chunkBuf_ = nullptr;      // registry chunk scratch
    static constexpr uint32_t kChunkCap = 24576;

    // registry ids
    lab::kit::ParamId pid_[kCh][kChParams];
    lab::kit::ParamId mid_[kMasterParams];
    lab::kit::ParamId tid_[6];          // spectral tools
    int engNow_[kCh] = {-1, -1, -1, -1};   // the engine whose knobs the registry holds
    int srcNow_[kCh] = {-1, -1, -1, -1};
    char knobNames_[kCh][kKnobs][12];

    // ui
    int page_ = kPgClouds, sel_ = 0;
    bool shift_ = false;
    uint64_t held_ = 0;
    int cur_[kPages] = {};             // per page cursor
    int frzCur_ = 0, frzFrom_ = kFromSource;
    int frzFocus_ = 0, frzTool_ = 0;   // FREEZE: 0 library, 1 tools row (0 = bed, 1..6 tools)
    int cloudsRow_ = 0;                // CLOUDS: 0..3 channel rows, 4 = send FX row
    int scnCur_ = 0, tlCur_ = 0, motionFocus_ = 0;   // 0 gestures, 1 scenes, 2 timeline
    int gestMode_ = 0;                 // 0 new, 1 overdub
    int fileCur_ = 0, fileFocus_ = 0;  // 0 projects, 1 resample row
    int rsSrc_ = -1, rsDst_ = 1;
    int curScene_ = -1, tlPlaying_ = -1;
    bool regionMode_ = false;
    int regionFirst_ = -1;
    int routeSel_ = 0;
    // settings
    int setGrains_ = kDefaultGrains, setCloudGrains_ = kDefaultCloudGrains, setVelocity_ = 1, setMidiCh_ = 0,
        setLeds_ = 2, setAutosave_ = 1;
    float radioMHz_ = 100.0f, shellRadio_ = 0.0f;
    // projects
    uint8_t projUsed_[kProjects] = {};
    // browser hand-off
    char pendingPath_[160] = {};
    int pendingCh_ = -1;

    // particles + display caches
    Particle part_[kParticles];
    int partHead_ = 0;
    float colGlow_[kCh][12] = {};
    uint8_t wave_[kCh][96] = {};       // source overview
    uint8_t colPeak_[kCh][12] = {};
    uint8_t regWave_[kCh][64] = {};    // the grain region (lanes, SOUND field)
    uint32_t lastMs_ = 0, lastPeakMs_ = 0;
    float gestFlash_[kCh] = {};
    // particles are drawn after everything else on the page (draw.cpp)
    struct PartReq {
        int16_t c, x, y, w, h;
        bool big;
        uint32_t limit;
    };
    PartReq deferReq_[4] = {};
    int nDefer_ = 0;
    bool deferDone_ = true;
    int knobH_ = 62;                   // drawKnob cell height
    float specView_[kSpecBins];        // the selected slot after the tools (drawn)
    int specViewSlot_ = -1;
    float specViewKey_ = -1.0f;

    // morph
    bool morphOn_ = false;
    float morphT_ = 0.0f, morphDur_ = 1.0f;
    float* morphFrom_ = nullptr;       // kCh * kChParams + kMasterParams
    float* morphTo_ = nullptr;

    // resample
    int rsState_ = 0;                  // 0 idle, 1 capturing, 2 writing
    ot::WavWriter rsWriter_;
    uint32_t rsWritten_ = 0;
    char rsPath_[64] = {};
    char rsLabel_[40] = {};
    int rsTarget_ = 1;
};

}  // namespace bulut
