// KONSOL -- a hybrid clip / scene production workstation for the original
// Polyend Tracker: 8 tracks (any engine), 16 scenes of clips, players and
// generators per clip (live or written into the piano roll), arrangement,
// mixer with 2 send buses and a master compressor / limiter.
#pragma once

#include <cstdint>

#include "audio/effects.h"
#include "lab/dsp.h"
#include "lab/lab.h"
#include "ws/engines.h"
#include "ws/model.h"
#include "ws/samples.h"

namespace ot {
struct Pattern;
}

namespace lab {
namespace konsol {

class Stretcher;

enum Page : uint8_t {
    kPgSession = 0,
    kPgSteps,
    kPgRoll,
    kPgGen,
    kPgSound,
    kPgMixer,
    kPgSong,
    kPgProject,
    kPgSamples,
    kPgCount
};

enum GridMode : uint8_t { kGmLaunch = 0, kGmStop, kGmMute, kGmSolo, kGmSelect, kGmDup };

// main -> audio commands
struct Cmd {
    enum Type : uint8_t { kNoteOn, kNoteOff, kLaunch, kStop, kScene, kStopAll, kAllOff, kPanic };
    uint8_t type = 0, track = 0, a = 0, b = 0;
};

// One pending event of a track (note-off, roll / ratchet hit).
struct Ev {
    uint32_t at;     // global subtick
    uint8_t pitch, vel;
    int8_t tone;
    uint8_t on;      // 1 = note-on (roll hit), 0 = note-off
};

struct TrackRt {
    volatile int8_t play = -1;       // playing scene, -1 none
    volatile int8_t queued = -2;     // -2 nothing, -1 stop, else scene
    uint32_t launchAt = 0;           // global subtick of the queued change
    bool launchNow = false;
    uint32_t startPos = 0;           // global subtick of the playing clip's tick 0
    volatile uint32_t loops = 0;     // completed loops of the playing clip
    volatile uint16_t tick = 0;      // clip tick at the last block (UI, recording)
    Ev ev[32];
    int nev = 0;
    float peak = 0.0f;               // audio max, the UI resets it
    uint8_t heldPitch[8] = {};       // hand-played notes (for MIDI out / panic)
    int nHeld = 0;
    uint8_t flash = 0;               // a note fired (LED)
};

constexpr int kStutterMax = 44100 * 3 / 2;   // 1.5 s stereo frames

class Konsol final : public Workstation {
public:
    const char* name() const override { return "KONSOL"; }
    const char* tagline() const override { return "Clip / scene production console - 8 channels"; }
    const Theme& theme() const override { return theme_; }
    bool init(Context& c) override;
    uint32_t introMs() const override { return 2700; }
    void drawIntro(Canvas& g, uint32_t t) override;
    void onEvent(const Event& e) override;
    void onMidi(const MidiMsg& m) override;
    void update(uint32_t ms) override;
    void draw(Canvas& g) override;
    void leds(Rgb* pads) override;
    void render(const Transport& t, const AudioIn& in, float* l, float* r) override;
    void onTransport(bool playing) override;
    int settings(SettingRow* rows, int cap) override;
    void save() override;
    int benchScenarios() const override { return 25; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;
    void benchSetupNew(int i);

    // ---- used by the tests ---------------------------------------------------
    Project& project() { return *P_; }
    TrackRt& rt(int t) { return rt_[t]; }
    Engine* engine(int t) const { return eng_[t].e[P_->track[t].engine]; }
    Engine* engineOf(int t, int kind) { return eng_[t].e[kind]; }
    void launch(int t, int scene) { send(Cmd{Cmd::kLaunch, uint8_t(t), uint8_t(scene), 0}); }
    void stopTrack(int t) { send(Cmd{Cmd::kStop, uint8_t(t), 0, 0}); }
    void launchScene(int s) { send(Cmd{Cmd::kScene, 0, uint8_t(s), 0}); }
    void setEngine(int t, int kind);
    // Generators: write clip (t, s) from its generator settings (main loop).
    bool runGenerator(int t, int s, uint32_t seed, char* msg = nullptr, int cap = 0);
    void liveStep(int t);              // one live regeneration of track t's playing clip
    bool saveProject(const char* name);
    bool loadProject(const char* name);
    void newProject(bool demo);
    void defaultGen(Clip& c, int kind);
    int lastBlockCost() const { return lastCost_; }
    float budgetScale() const { return budgetScale_; }
    int selTrack() const { return selT_; }
    int selScene() const { return selS_; }
    void select(int t, int s) {
        selT_ = t;
        selS_ = s;
    }
    void setPage(int p) { page_ = uint8_t(p); }
    void focusRoll();
    int page() const { return page_; }
    bool songMode() const { return songMode_; }
    void setSongMode(bool on) { songMode_ = on; songReq_ = true; }
    int songIndex() const { return songIdx_; }
    float masterPeak() const { return masterPk_; }
    // Notes of a clip from an ot::Pattern track (generator output), and back.
    static void patternToNotes(const ot::Pattern& p, int track, int steps, NoteBuf& out, bool sliceNotes);
    static void notesToPattern(const NoteBuf& in, int steps, ot::Pattern& p, int track, uint8_t instr);

private:
    friend struct KonsolUi;
    // ---- audio ------------------------------------------------------------
    void send(const Cmd& c) { cmds_.push(c); }
    void applyCmd(const Cmd& c, uint32_t pos);
    void startClip(int t, int scene, uint32_t at);
    void stopClip(int t);
    void fire(int t, int pitch, int vel, int tone, int flags, int offset, uint32_t at, uint32_t lenSub);
    void schedule(int t, const Ev& e);
    bool admit(int t, int32_t extra);
    int32_t engineBudget(int extraTrack) const;
    void trackNotes(int t, uint32_t pos, uint32_t end, float subPerSample);
    void songStep(uint32_t pos, uint32_t end, float subPerSample);
    void fxBlock(float* l, float* r);
    void midiFromAudio(int t, bool on, int pitch, int vel);

    // ---- main loop ----------------------------------------------------------
    void padEvent(const Event& e);
    void sessionPad(int col, int row, bool down, int vel);
    void controlPad(int col, int row, bool down, int vel);
    void keysPad(int pad, bool down, int vel, int rows);
    void stepsPad(int col, int row, bool down, int vel);
    void rollPad(int pad, bool down, int vel);
    void mixerPad(int col, int row, bool down);
    void button(const Event& e);
    void encoder(int d, bool shift);
    void arrow(int id, bool shift);
    void fkey(int k, bool shift);
    void playNote(int pitch, int vel, bool on);
    void recordNote(int pitch, int vel, bool on);
    void scrollScenes(int d);
    void toggleStep(int step, int vel);
    void ensureClip(int t, int s, int steps);
    void undoSave(int t, int s);
    void undoRestore();
    int rootNote(int t) const;
    uint16_t scaleMask(int t) const;
    // ---- samples (sample_mgr.cpp) ----
public:
    bool loadSample(int t, const char* path);
    bool stretchTrack(int t);                  // to the project tempo (job runs in update())
    void unstretch(int t);
    float stretchProgress() const;             // -1 idle
    int kitFromSlices(int t, bool byClass);
    int kitFromFolder(int t, const char* dir);
    bool saveKit(int t, const char* name);
    bool loadKit(int t, const char* path);
    SamplePool& pool() { return pool_; }
    TrackSample& trackSample(int t) { return ts_[t]; }
    void stretchService(uint32_t budgetFrames);
    void gcSamples();

private:
    void ensureBreak();
    void setTrackBreak(int t);
    int poolFind(const char* path) const;
    int poolLoad(const char* path);
    bool slotUsed(int s) const;
    void compactPool();
    const char* stretchState(int t) const;
    void kitWrite(int t, uint8_t* out) const;
    void kitRead(int t, const uint8_t* in);
    static uint32_t kitBytes();
    void scanSamples();
    void scanProjects();
    void applyFxParams();
    int keyPitch(int pad, int rows) const;

    // ---- drawing (ui.cpp) -------------------------------------------------------
    void drawHeaderBar(Canvas& g, const char* page);
    void drawFKeys(Canvas& g, const char* const k[8], int active = -1);
    void drawSession(Canvas& g);
    void drawSteps(Canvas& g);
    void drawRoll(Canvas& g);
    void drawGen(Canvas& g);
    void drawSound(Canvas& g);
    void drawMixer(Canvas& g);
    void drawSong(Canvas& g);
    void drawProject(Canvas& g);
    void drawSamples(Canvas& g);
    void drawHelp(Canvas& g);

    Context* ctx_ = nullptr;
    Theme theme_;
    Project* P_ = nullptr;
    TrackEngines eng_[kTracks];
    SamplePool pool_;
    TrackSample ts_[kTracks];
    Stretcher* str_ = nullptr;
    int16_t breakSlot_ = -1;
    int strTrack_ = -1;
    float strTarget_ = 120.0f;
    uint8_t strPend_ = 0;               // tracks to re-stretch (after a project load)
    ot::Pattern* scratch_ = nullptr;   // generator target
    NoteBuf* src_ = nullptr;            // per-track live source (Genotype / Turnaround)
    void* acidPh_ = nullptr;            // per-track Acid live phrase (gen::MonoStep[128])
    uint32_t liveSeen_[kTracks] = {};
    int8_t livePlay_[kTracks] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int8_t srcScene_[kTracks] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int8_t acidScene_[kTracks] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t liveCount_[kTracks] = {};
    int16_t* stutter_ = nullptr;
    NoteBuf* undo_ = nullptr;
    int undoT_ = -1, undoS_ = -1, undoLen_ = 0;

    // audio-side state
    SpscRing<Cmd, 128> cmds_;
    TrackRt rt_[kTracks];
    ot::audio::Delay delay_;
    ot::audio::Reverb* reverb_ = nullptr;
    uint32_t pos_ = 0;                 // global subtick at the block start
    volatile uint32_t posPub_ = 0;     // published for the UI
    float subPerBeat_ = float(kPpq * 256);
    bool wasPlaying_ = false;
    volatile bool songMode_ = false, songReq_ = false;
    volatile int songIdx_ = -1;
    uint32_t songStartPos_ = 0;
    float compEnv_ = 0.0f, compGain_ = 1.0f, limGain_ = 1.0f;
    volatile float compGr_ = 0.0f;     // dB of gain reduction (UI)
    volatile float masterPk_ = 0.0f;
    float hpZ_[4] = {};
    float hpCut_ = 0.0f;               // performance HP sweep 0..1
    volatile bool hpHeld_ = false, stutHeld_ = false, dlyThrow_ = false, revThrow_ = false;
    int32_t stutW_ = 0, stutLen_ = 0, stutR_ = 0, stutStart_ = 0;
    bool stutActive_ = false;
    int lastCost_ = 0;
    float metroPh_ = 0, metroEnv_ = 0;
    uint32_t lastBeatSub_ = 0;
    float bpm_ = 120.0f;
    uint32_t blockCount_ = 0;
    volatile bool fxDirty_ = true;
    volatile bool hold_ = false;
    volatile float budgetScale_ = 1.0f;   // device CPU feedback (update())       // main loop rewrites the project: render() outputs silence
    // smoothed mixer gains
    float gl_[kTracks] = {}, gr_[kTracks] = {}, ga_[kTracks] = {}, gb_[kTracks] = {};

    // ---- UI state -------------------------------------------------------------
    uint8_t page_ = kPgSession, prevPage_ = kPgSession;
    uint8_t gridMode_ = kGmLaunch;
    bool gridModeLatched_ = false;
    uint32_t gridModeDownMs_ = 0;
    bool keysOverlay_ = false;         // Aux: pads play the selected track on any page
    bool help_ = false;
    int selT_ = 0, selS_ = 0;
    int sceneTop_ = 0;
    bool recArm_ = false;
    uint32_t nowMs_ = 0;
    // steps
    int stepPage_ = 0, stepLock_ = 0, stepCursor_ = 0;
    int stepNote_ = -1;               // pitch for new steps (-1 = root)
    int heldStep_ = -1;
    bool stepEdited_ = false;
    int keyOct_ = 0;
    // roll
    int rollTick_ = 0, rollPitch_ = 60, rollGrid_ = 1, rollFn_ = 0, rollTop_ = 72, rollLeft_ = 0;
    // gen
    int genRow_ = 0;
    // sound
    int sndSec_ = 0, sndRow_ = 0;
    // mixer
    int mixRow_ = 0, mixCol_ = 0;
    bool mixFx_ = false;
    int fxRow_ = 0;
    // song
    int songCur_ = 0;
    // project
    int projRow_ = 0, projTab_ = 0, trkRow_ = 0, trkCol_ = 0;
    int nameCur_ = 0;
    bool confirm_ = false;
    int confirmKey_ = -1;
    // file lists
    static constexpr int kListMax = 64;
    char (*list_)[40] = nullptr;
    int listN_ = 0, listSel_ = 0, listTop_ = 0;
    char listDir_[48] = {};
    // hand play / recording
    int8_t padPitch_[kPads];
    struct RecNote {
        int8_t pitch = -1;
        uint16_t start = 0;
        uint32_t startPos = 0;
        uint8_t vel = 0;
        int8_t scene = -1;
    } rec_[8];
    // meters (UI)
    float meter_[kTracks] = {};
    float masterMeter_ = 0.0f;
    char msg_[48] = {};
    uint32_t lastAutosave_ = 0;
    bool dirty_ = false;
    // settings rows
    int setLaunchQ_ = 2, setRecQ_ = 1, setSwing_ = 50, setKey_ = 9, setScale_ = 1, setMetro_ = 0, setLoop_ = 1;
    int setPadBright_ = 2;
};

// Track colours (console strip colours).
Rgb trackColor(int t);

}  // namespace konsol
}  // namespace lab
