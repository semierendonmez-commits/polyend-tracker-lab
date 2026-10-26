// TILT -- the workstation: the renewed Pinball 2 as a Tracker firmware.
// tilt_ws.cpp (init, audio glue, commands, MIDI, settings, bench),
// tilt_ui.cpp (input), tilt_draw.cpp (pages, intro, LEDs), tilt_io.cpp (files).
#pragma once

#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/tilt_audio.h"
#include "ws/tilt_engine.h"
#include "ws/tilt_model.h"

namespace tilt {

enum Page : uint8_t { kPgGraph, kPgMatrix, kPgSteps, kPgTable, kPgEvolve, kPgCombos, kPgChannels, kPgMix, kPgCount };
struct PageInfo {
    const char* name;      // F-key label
    const char* title;     // header
    const char* shift[8];  // Shift + F1..F8 labels
    const char* help;      // one line
};
extern const PageInfo kPages_[kPgCount];
// A whole file into data (loops over short reads); -1 when it does not exist.
int32_t readFile(ot::IStorage& fs, const char* path, void* data, uint32_t cap);

// Main loop -> audio.
struct Cmd {
    enum Type : uint8_t {
        kLaunch, kHalt, kFlip, kFlipper, kNudge, kBall, kCombo, kPage, kCapture, kReplay, kAudition, kPanic,
        kTilt, kSwap, kResetEvolve
    };
    uint8_t type = 0;
    int16_t a = 0, b = 0, c = 0;
};

class Tilt final : public lab::Workstation {
public:
    const char* name() const override { return "TILT"; }
    const char* tagline() const override { return "17x17 pinball: the renewed Pinball 2"; }
    const lab::Theme& theme() const override { return theme_; }
    bool init(lab::Context& c) override;
    uint32_t introMs() const override { return 2700; }
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
#ifdef TILT_PROFILE
    int benchScenarios() const override { return 4 + kEngines + 2; }
#else
    int benchScenarios() const override { return 4; }
#endif
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // ---- tests / sim ---------------------------------------------------------------------
    Engine& engine() { return *eng_; }
    Pattern& pattern() { return *pat_; }
    Audio& audio() { return audio_; }
    int page() const { return page_; }
    bool savePattern(int slot);
    bool loadPattern(int slot);
    bool saveKit(int slot);
    bool loadKit(int slot);
    void loadFactory(int i);
    bool exportMidi(char* pathOut, int cap);
    void randomizeNow(uint8_t targets);
    void undoRandom();
    int capBars() const { return capBars_; }

    // ---- modulation sources (0..1 floats, updated every block; for the LABKIT mod layer) ----
    static constexpr int kModSources = 7;
    static const char* modSourceName(int i);
    const float* modSourceValue(int i) const { return (i >= 0 && i < kModSources) ? &mod_[i] : nullptr; }

    // ---- shared with the ui / draw files ----------------------------------------------------
    void post(Cmd::Type t, int a = 0, int b = 0, int c = 0);
    lab::Rgb chColor(int ch) const;
    lab::Rgb trigColor(int trig) const;

private:
    // audio side
    void applyCommands(double beat);
    void schedule(const lab::Transport& t, double moveBeat, uint32_t blockStart);
    void midiOut(const Pending& e);
    void runOffs();
    // main loop
    void onBanner(uint8_t ev);
    void swapIn(const Pattern& p);
    void loadSamplesFor();
    void loadSample(int ch);
    void scanWavs();
    void scanSlots();
    void slotPath(bool kit, int slot, char* buf, int cap) const;
    void auditionCh(int ch, int note, int vel);
    // input
    void eventGlobal(const lab::Event& e, bool& used);
    void eventPerform(const lab::Event& e);
    void eventGraph(const lab::Event& e);
    void eventMatrix(const lab::Event& e);
    void eventSteps(const lab::Event& e);
    void eventParams(const lab::Event& e);
    void eventCombos(const lab::Event& e);
    void eventChannels(const lab::Event& e);
    void eventMix(const lab::Event& e);
    void eventFile(const lab::Event& e);
    void shiftAction(int page, int key);
    void editStep(int step, int row, int delta);
    void editMatrix(int delta);
    void editChannel(int field, int delta);
    void editCombo(int delta);
    void editMix(int delta);
    int paramListOf(int page, int* out, int cap) const;
    // drawing
    void drawGraph(lab::Canvas& g);
    void drawGraphView(lab::Canvas& g, int cx, int cy, float rad, bool big);
    void drawSidePanel(lab::Canvas& g, int x);
    void drawMatrix(lab::Canvas& g);
    void drawSteps(lab::Canvas& g);
    void drawParams(lab::Canvas& g);
    void drawCombos(lab::Canvas& g);
    void drawChannels(lab::Canvas& g);
    void drawMix(lab::Canvas& g);
    void drawFile(lab::Canvas& g);
    void drawHelp(lab::Canvas& g);
    void drawBanner(lab::Canvas& g);
    void drawKeys(lab::Canvas& g);
    void drawParamList(lab::Canvas& g, const int* ids, int n, int x0, int y0, int colW, int focus, int index);
    void ledsPerform(lab::Rgb* pads);
    float nodeX(int s, int cx, float rad) const;
    float nodeY(int s, int cy, float rad) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Pattern* pat_ = nullptr;      // live (audio reads, ui edits)
    Pattern* staged_ = nullptr;   // a pattern being swapped in
    Pattern* undo_ = nullptr;     // before the last randomize
    bool haveUndo_ = false;
    Engine* eng_ = nullptr;
    Audio audio_;
    Rng32 rng_;
    lab::SpscRing<Cmd, 64> cmds_;
    lab::SpscRing<uint8_t, 64> events_;

    // audio-side state
    double nextMove_ = -1.0;
    uint32_t sample_ = 0;
    bool launched_ = false;
    volatile double beatNow_ = 0.0;
    volatile float bpmNow_ = 120.0f;
    struct Off {
        uint8_t note = 0, status = 0;
        int32_t left = 0;
    };
    static constexpr int kOffCap = 64;
    Off offs_[kOffCap];
    int nOffs_ = 0;
    bool wasPlaying_ = false;

    // ui
    int page_ = kPgGraph;
    bool help_ = false, file_ = false, shift_ = false, perform_ = false;
    bool padDown_[lab::kPads] = {};
    uint8_t padVel_[lab::kPads] = {};
    int mxRow_ = 1, mxCol_ = 2, mxField_ = 0, mxShape_ = 0, mxRowFn_ = 1;
    bool mxLive_ = false;
    int stStep_ = 0, stRow_ = 0, melField_ = 0;
    Step clipStep_;
    bool haveClipStep_ = false;
    int parFocus_[3] = {};        // TABLE, EVOLVE (incl. randomizer), MIX fx list
    int metaCell_ = -1;           // EVOLVE: -1 = params, 0..15 the meta grid cell
    int evLock_ = -1;             // EVOLVE: row-lock cursor
    int cbSel_ = 0, cbField_ = 0;
    int chSel_ = 0, chField_ = 0;
    int mixCh_ = 0, mixRow_ = 0;
    bool mixFx_ = false;
    int fileCol_ = 0, fileSel_[3] = {};
    int capBars_ = 4;
    uint8_t banner_ = 0;
    uint32_t bannerAt_ = 0;
    uint32_t now_ = 0;
    uint8_t chFlash_[kCh] = {};
    uint32_t lastHits_[kCh] = {};
    // files
    static constexpr int kUserSlots = 16, kKitSlots = 8, kMaxWavs = 64;
    bool slotUsed_[kUserSlots] = {};
    char slotName_[kUserSlots][kNameLen] = {};
    bool kitUsed_[kKitSlots] = {};
    char wavs_[kMaxWavs][kPathLen] = {};
    int wavCount_ = 0, wavSel_ = 0;
    bool sampleDirty_[kCh] = {};
    int midiFiles_ = 0;
    // settings rows
    int setAutoload_ = 1, setMidiOut_ = 1, setStepNotes_ = 1, setTrail_ = 1;
    int activeSlot_ = -1;
    float mod_[kModSources] = {};   // ball step, entropy, heat under the ball, tilt, balls, page, last velocity
    char loaded_[kCh][kPathLen] = {};
    static constexpr uint32_t kIoBytes = 32768;
    uint8_t* io_ = nullptr;          // file buffer (patterns, kits, MIDI files)
    char lastMidi_[64] = {};
};

}  // namespace tilt
