// FLIPPER -- the workstation: pages, input, LEDs, the audio side (step
// scheduling, notes, MIDI, stems) and files. Drawing lives in flip_draw.cpp,
// input in flip_ui.cpp.
#pragma once

#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/flip_audio.h"
#include "ws/flip_game.h"
#include "ws/flip_model.h"

namespace flip {

enum Page : uint8_t { kPgPlay, kPgEdit, kPgSounds, kPgTables, kPgRun, kPgCount };

// Main loop -> audio commands.
struct Cmd {
    enum Type : uint8_t {
        kNewGame, kGoto, kGotoNow, kReplay, kSwap, kRebuild, kPanic, kTilt, kDrop, kStop, kAudition, kFlip, kLaunch,
        kMagnet, kPerf, kMode
    };
    uint8_t type = 0;
    int16_t a = 0, b = 0, c = 0;
};

constexpr uint32_t kRunCap = 8192;
constexpr int kMaxWavs = 48;
constexpr int kSlots = 8;
constexpr int kCaptureCount = 6;   // whole game, last 4 / 8 / 16 / 32 / 64 bars

class Flipper final : public lab::Workstation {
public:
    const char* name() const override { return "FLIPPER"; }
    const char* tagline() const override { return "The song is a pinball machine"; }
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
    int benchScenarios() const override { return 5; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // tests
    Game& game() { return game_; }
    Machine& machine() { return *m_; }
    Audio& audio() { return audio_; }
    int page() const { return page_; }
    bool loadSlot(int slot);
    bool saveSlot(int slot);
    bool capture(int what);
    void pumpCommands() { applyCommands(); }

private:
    // audio side
    void applyCommands();
    void emit(int offset);
    void scheduleOffs(int offset);
    void midiNoteOut(const NoteOut& n, int offset);
    void runOffs();
    // main loop helpers
    void post(Cmd::Type t, int a = 0, int b = 0, int c = 0);
    void loadSamples(bool all);
    void loadSample(int inst);
    void scanWavs();
    void rescanSlots();
    void slotPath(int slot, char* buf, uint32_t cap) const;
    void cellChanged();
    void audition(int inst, int note, int vel);
    bool padHeld(int pad) const { return padDown_[pad]; }

    // input per page
    void eventPlay(const lab::Event& e);
    void eventEdit(const lab::Event& e);
    void eventSounds(const lab::Event& e);
    void eventTables(const lab::Event& e);
    void eventRun(const lab::Event& e);
    void editValue(int row, int delta);
    void soundValue(int row, int delta);
    void tableValue(int row, int delta);
    void runValue(int row, int delta);
    int editRows() const;
    int editRowId(int i) const;

    // drawing
    void drawPlay(lab::Canvas& g);
    void drawField(lab::Canvas& g, int ox, int oy, float sc, bool markov, bool editor);
    void drawHud(lab::Canvas& g);
    void drawBanner(lab::Canvas& g);
    void drawEdit(lab::Canvas& g);
    void drawSounds(lab::Canvas& g);
    void drawTables(lab::Canvas& g);
    void drawGraph(lab::Canvas& g, int cx, int cy, int rad);
    void drawRun(lab::Canvas& g);
    void drawHelp(lab::Canvas& g);
    void drawTop(lab::Canvas& g, const char* title);
    void drawKeys(lab::Canvas& g, const char* const k6[3]);
    void drawList(lab::Canvas& g, int x, int y, int w, int rows, int sel, int top, int count,
                  void (Flipper::*row)(int, char*, char*) const);
    void editRowText(int i, char* label, char* value) const;
    void soundRowText(int i, char* label, char* value) const;
    void tableRowText(int i, char* label, char* value) const;
    void runRowText(int i, char* label, char* value) const;
    lab::Rgb instColor(int inst) const;
    lab::Rgb tableColor(int t) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Machine* m_ = nullptr;        // the machine being played and edited
    Machine* spare_ = nullptr;    // load target (swapped in by the audio side)
    Game game_;
    Audio audio_;
    ClipStep* run_ = nullptr;
    lab::SpscRing<Cmd, 64> cmds_;
    lab::SpscRing<uint8_t, 32> bannersOut_;

    // audio-side state
    double nextStep_ = 0.0;
    double stepBase_ = 0.0;
    uint32_t stepK_ = 0;
    int lastRate_ = -1, lastTable_ = -1;
    bool wasPlaying_ = false;
    volatile float stepPhase_ = 0.0f;
    volatile uint32_t audioSteps_ = 0;
    float perfAmt_[kFxCount] = {};
    struct Off {
        uint8_t note = 0, ch = 0;
        int32_t samples = 0;
    };
    static constexpr int kOffCap = 48;
    Off offs_[kOffCap];
    int offCount_ = 0;
    volatile bool swapPending_ = false;
    float cpuGuard_ = 0.0f;
    uint32_t stressN_ = 0, stressBlk_ = 0;
    double gridBeat_ = 0.0;
    float bps_ = 0.25f;
    uint32_t parity_ = 0;

    // ui
    int page_ = kPgPlay;
    bool help_ = false;
    bool padDown_[lab::kPads] = {};
    int editCell_ = cellAt(2, 1), editRow_ = 0, editTop_ = 0;
    int sndInst_ = 0, sndRow_ = 0, sndTop_ = 0;
    int tabSel_ = 0, tabRow_ = 0, tabTop_ = 0;
    int activeSlot_ = 0;   // the machine being played: autosave writes here (-1: not yet saved)
    uint32_t sampleTouchMs_ = 0;
    int runRow_ = 0, slot_ = 0, captureWhat_ = 0, replaySrc_ = 0, confirm_ = 0;
    int tabFile_ = 0, tabFileCount_ = 0, preset_ = 0;
    char tabFiles_[16][kPathLen] = {};
    void scanTableFiles();
    bool saveTableFile(int t);
    bool loadTableFile(int t, const char* path);
    int clipA_ = 0, clipB_ = 0;
    bool shiftHeld_ = false;
    Cell clipCell_;
    bool haveClipCell_ = false;
    Table* clipTable_ = nullptr;
    bool haveClipTable_ = false;
    uint8_t banner_ = kBnNone;
    uint32_t bannerAt_ = 0;
    uint32_t now_ = 0;
    int fpsTick_ = 0;
    int resetMachine_ = 0;
    bool slotUsed_[kSlots] = {};
    char wavs_[kMaxWavs][kPathLen] = {};
    int wavCount_ = 0;
    int sampleSel_[kInsts] = {};   // index into wavs_ (+1), 0 = none
    bool sampleDirty_[kInsts] = {};
    bool needSamples_ = true;
    uint32_t lastScore_ = 0;
    // trails (screen)
    static constexpr int kTrail = 8;
    float trailX_[kMaxBalls][kTrail] = {}, trailY_[kMaxBalls][kTrail] = {};
    int trailHead_ = 0;
    uint32_t lastTrailMs_ = 0;
};

}  // namespace flip
