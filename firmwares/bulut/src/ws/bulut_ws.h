// BULUT workstation: pages, controls, LEDs, files (main loop side).
#pragma once

#include <cstdint>

#include "formats/wav.h"
#include "lab/lab.h"
#include "ws/bulut.h"
#include "ws/engine.h"
#include "ws/project.h"
#include "ws/spectral.h"

namespace bulut {

enum Page : uint8_t { kPgCloud = 0, kPgGrain, kPgSource, kPgGesture, kPgFreeze, kPgMix, kPgScene, kPgFile, kPages };

// One drawn particle (main loop): born from an audio Spark.
struct Particle {
    float x, y;            // 0..1 inside the field
    float vx, vy;          // per second
    float age, life;       // seconds
    uint8_t cloud, size, kind, live;
};
constexpr int kParticles = 640;

struct BrowseEntry {
    char name[40];
    uint8_t dir;
};
constexpr int kBrowseMax = 64;

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
    int benchScenarios() const override { return 6; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // ---- used by the host tests --------------------------------------------
    Engine& engine() { return *eng_; }
    CloudState& cloud(int i) { return clouds_[i]; }
    CloudSource& source(int i) { return sources_[i]; }
    int selected() const { return sel_; }
    int page() const { return page_; }
    bool loadSample(int cloud, const char* path);
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
    void setParam(int cloud, int slot, float v, bool record);
    void tick(uint32_t ms) { update(ms); }

private:
    // pages
    void setPage(int p);
    void action(int k);          // Shift + F1..F8 on the current page
    void encoder(int delta, bool fine);
    void arrows(int id);
    void push();
    void selectCloud(int c);
    void gestureRecButton();
    void finishGesture();
    void toggleFreeze();
    void waitGesture(int c);
    void shiftPad(int pad);
    // params
    struct ParamRef { int slot; };
    int pageParam(int idx) const;            // Grain page: grid index -> slot
    int stripParam(int idx) const;           // Cloud page strip
    void nudgeParam(int c, int slot, int delta, bool fine);
    // files
    void scanBrowse();
    void browseEnter();
    void reloadSources();
    void pumpResample();
    void pumpMorph(uint32_t ms);
    void pumpTimeline();
    void pumpParticles(float dt);
    void updatePeaks();
    void loadAutosave();
    void projectPath(int slot, char* out, uint32_t cap) const;
    void scanProjects();
    void newProject();
    // draw
    void drawField(lab::Canvas& g, int x, int y, int w, int h, int cloud, bool big);
    void drawParticles(lab::Canvas& g, int x, int y, int w, int h, int cloud, bool big, int maxWords);
    void drawCloudCards(lab::Canvas& g, int x, int y, int w, int h);
    void drawParamCell(lab::Canvas& g, int x, int y, int w, int slot, bool selected);
    void drawPageCloud(lab::Canvas& g);
    void drawPageGrain(lab::Canvas& g);
    void drawPageSource(lab::Canvas& g);
    void drawPageGesture(lab::Canvas& g);
    void drawPageFreeze(lab::Canvas& g);
    void drawPageMix(lab::Canvas& g);
    void drawPageScene(lab::Canvas& g);
    void drawPageFile(lab::Canvas& g);
    void drawFooter(lab::Canvas& g);
    lab::Rgb cloudColor(int c) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine* eng_ = nullptr;
    Spectral spec_;
    GestureRecorder* rec_ = nullptr;
    CloudState clouds_[kClouds];
    CloudSource sources_[kClouds];
    Gesture* gest_ = nullptr;          // kGestBufs * kClouds (engine.h)
    FxState fx_;
    Scene* scenes_ = nullptr;          // kScenes
    TimeEntry timeline_[kTimeline];
    int tlLen_ = 0;
    FreezeSlot* frz_ = nullptr;        // kFreezeSlots
    float bpmSave_ = 120.0f;
    int songMode_ = 0;

    // ui
    int page_ = kPgCloud, sel_ = 0;
    bool shift_ = false;
    uint64_t held_ = 0;
    int stripCur_ = 0;                 // Cloud page strip
    int grainCur_ = 0;                 // Grain page 0..15
    int srcCur_ = 0;                   // Source page row
    int gestCur_ = 0;                  // Gesture page row
    int gestMode_ = 0;                 // 0 new, 1 overdub
    int frzCur_ = 0;                   // Freeze page: selected slot
    int frzFrom_ = kFromSource;
    int mixCol_ = 0, mixRow_ = 0;
    int scnCur_ = 0, tlCur_ = 0, scnFocus_ = 0;   // focus 0 scenes, 1 timeline
    int fileCur_ = 0, fileFocus_ = 0;  // 0 projects, 1 resample row
    int rsSrc_ = -1, rsDst_ = 1;
    int curScene_ = -1, tlPlaying_ = -1;
    // settings
    int setGrains_ = kDefaultGrains, setCloudGrains_ = kDefaultCloudGrains, setVelocity_ = 1, setMidiCloud_ = 0, setMorph_ = 2, setLeds_ = 2,
        setAutosave_ = 1;
    float radioMHz_ = 100.0f, shellRadio_ = 0.0f;

    // browser
    BrowseEntry browse_[kBrowseMax];
    int browseN_ = 0, browseCur_ = 0, browseTop_ = 0;
    char browseDir_[96] = "Samples";
    // projects
    uint8_t projUsed_[kProjects] = {};

    // particles + display caches
    Particle part_[kParticles];
    int partHead_ = 0;
    float colGlow_[kClouds][12] = {};
    uint8_t wave_[kClouds][64] = {};   // buffer overview
    uint8_t colPeak_[kClouds][12] = {};
    uint32_t lastMs_ = 0, lastPeakMs_ = 0, lastBlinkMs_ = 0;
    float gestFlash_[kClouds] = {};

    // morph
    bool morphOn_ = false;
    float morphT_ = 0.0f, morphDur_ = 1.0f;
    CloudState* morphFrom_ = nullptr;  // kClouds
    CloudState* morphTo_ = nullptr;    // kClouds

    // resample
    int rsState_ = 0;                  // 0 idle, 1 capturing, 2 writing
    ot::WavWriter rsWriter_;
    uint32_t rsWritten_ = 0, rsLen_ = 0;
    char rsPath_[64] = {};
    int rsTarget_ = 1;
};

}  // namespace bulut
