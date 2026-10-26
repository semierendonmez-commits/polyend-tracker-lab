// DATAMATICS -- the workstation: pages, pads, data loader, pieces, visuals,
// performance (throws, scenes, recorder), LABKIT v2 registry.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/dm_data.h"
#include "ws/dm_engine.h"

namespace dm {

// Page ids = LABKIT page ids (the MAP lists them in this order; its pads reach
// the first 12). ST0..ST7 are the STREAM page of each stream.
enum Page : uint8_t {
    PG_PERFORM, PG_MATRIX, PG_MIXER, PG_SONG, PG_ST0, PG_ST7 = PG_ST0 + 7, PG_VIEW, PG_DATA, PG_PIECE, PG_COUNT
};
inline bool isStreamPage(int p) { return p >= PG_ST0 && p <= PG_ST7; }

enum VisMode : uint8_t { VM_STREAM, VM_BARCODE, VM_TRANSIT, VM_GRID, VM_SPATIAL, VM_COUNT };
const char* visName(int m);

// Slot colours (the one place colour is used for meaning).
lab::Rgb slotColor(int slot);
uint8_t slotColor332(int slot, bool dim = false);

int streamParamAt(int row);   // STREAM page row -> P_*
int streamParamRows();

// Composition file (text, versioned): serialise / parse. Pure (tests).
constexpr int kPieceVersion = 2;
uint32_t writePiece(const Engine& e, float bpm, char* out, uint32_t cap);
// Fills e (params, sections, options, slot paths + modes; does not load data).
bool readPiece(Engine& e, const char* text, uint32_t len, float* bpmOut);

// What a loaded file is: shown on DATA and in the VIEW overlay.
struct SlotMeta {
    uint32_t fileSize = 0, hash = 0;   // FNV-1a 32 of the decoded values
    float entropy = 0.0f;              // bits per value, 0..8
    float ones = 0.0f;                 // share of 1 bits, 0..1
};

// Performance recorder events (BLACKBOX-style), engine sample clock.
enum PerfType : uint8_t { PE_PARAM, PE_THROWS, PE_FILL, PE_SECTION, PE_SCENE, PE_SLOTSOLO };
struct PerfEv {
    uint32_t t;
    uint32_t key;     // PE_PARAM: registry key hash; masks for the others
    uint16_t val;     // PE_PARAM: normalized value x 65535
    uint8_t type, a;  // a: section / scene index
};

class Datamatics final : public lab::Workstation {
public:
    const char* name() const override { return "DATAMATICS"; }
    const char* tagline() const override { return "Data music: any file, sine / click / noise / impulse"; }
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

    // LABKIT v2
    int navPages(lab::kit::PageInfo* out, int cap) override;
    int navPage() const override { return page_; }
    void navGoto(int page) override;
    void navFocus(lab::kit::ParamId id) override;
    void navParamChanged(lab::kit::ParamId id) override;

    // tests / sim
    Engine& engine() { return eng_; }
    int page() const { return page_; }
    bool loadBusy() const { return jobCount_ > 0 || job_.active; }
    void requestLoad(int slot, const char* path, int mode);
    void finishLoadsNow();   // run queued loads to completion (tests)
    bool savePiece(const char* path);
    bool loadPiece(const char* path);
    const SlotMeta& slotMeta(int i) const { return meta_[i & 3]; }
    void assign(int stream, int slot);          // patch a stream to a slot (through the registry)
    void storeScene(int i);
    void recallScene(int i, bool quantised);
    bool sceneUsed(int i) const { return i >= 0 && i < kScenes && scene_[i].used; }
    int perfCount() const { return perfN_; }
    bool perfRecording() const { return perfRec_; }
    void setPerfArm(bool on) { perfArm_ = on; }
    void setPerfReplay(bool on) { perfReplay_ = on; }
    lab::kit::ParamId pid(int stream, int p) const { return pid_[stream & 7][p]; }

    static constexpr int kScenes = 8;

private:
    struct LoadJob {
        char path[128];
        int slot = 0, mode = 0;
    };
    struct ActiveJob {
        bool active = false;
        char path[128] = {};
        int slot = 0, mode = 0;
        int32_t handle = -1;
        uint32_t size = 0, pos = 0, limit = 0;
    };
    static constexpr int kSceneMax = 256;
    struct Scene {
        bool used = false;
        int section = -1;
        float v[kSceneMax];
    };

    // pages
    void setPage(int p);
    int screenPage() const { return isStreamPage(page_) ? PG_ST0 : page_; }
    void drawTop(lab::Canvas& g, const char* title);
    void drawKeys(lab::Canvas& g, const char* f8);
    void drawPerform(lab::Canvas& g);
    void drawMatrix(lab::Canvas& g);
    void drawStream(lab::Canvas& g);
    void drawData(lab::Canvas& g);
    void drawSong(lab::Canvas& g);
    void drawMixer(lab::Canvas& g);
    void drawPiece(lab::Canvas& g);
    void drawView(lab::Canvas& g);
    void drawSlotTag(lab::Canvas& g, int x, int y, int stream, int maxChars);
    void drawWaterfall(lab::Canvas& g, int x, int y, int scale);
    // VIEW modes (dm_vis.cpp)
    void visBarcode(lab::Canvas& g);
    void visTransit(lab::Canvas& g);
    void visGrid(lab::Canvas& g);
    void visSpatial(lab::Canvas& g);
    void visOverlay(lab::Canvas& g);
    void fillWindow();

    void performEvent(const lab::Event& e);
    void matrixEvent(const lab::Event& e);
    void streamEvent(const lab::Event& e);
    void dataEvent(const lab::Event& e);
    void songEvent(const lab::Event& e);
    void mixerEvent(const lab::Event& e);
    void pieceEvent(const lab::Event& e);
    void viewEvent(const lab::Event& e);
    void padEvent(const lab::Event& e);
    void performPad(int row, int col, bool down, uint8_t vel, bool shift);
    void matrixPad(int row, int col, bool down, uint8_t vel, bool shift);
    void dataPad(int row, int col, bool down, bool shift);
    void nudge(lab::kit::ParamId id, int delta, bool fine);
    void setParam(lab::kit::ParamId id, float v);
    bool wouldHit(int s, uint8_t val) const;
    void launchSection(int i);
    void setThrows(uint32_t held, uint32_t latch);
    void setFill(uint32_t mask);
    void setSlotSolo(uint32_t mask);
    void panic();

    // data
    void pumpLoader();
    void finishJob();
    void computeMeta(int slot, uint32_t fileSize);
    void openBrowser(int slot);
    void scanPieces();
    void newPiece();
    void loadBuiltinNow(int slot, int b, int mode);
    void reloadSlot(int slot, int mode);

    // registry
    void registerParams();
    void pushRegistry();        // fields -> registry base (after direct field writes)
    void firstBootScenes();

    // performance: scenes, morph, recorder
    void captureScene(Scene& s);
    void applySceneNow(const Scene& s);
    void startMorph(int i);
    void stepMorph();
    void perfLog(uint8_t type, uint32_t key, uint16_t val, uint8_t a = 0);
    void perfStart();
    void perfStop();
    void perfReplayStep();

    // visuals
    void composeRow();
    void composeMatrix();
    void composeBarcodes();
    void uploadVisuals();
    void drainEvents(uint32_t ms);
    void drawIntroBars(lab::Canvas& g, float reveal, float height, int cy);

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine eng_;
    int page_ = PG_PERFORM, lastPage_ = PG_PERFORM;
    int sel_ = 0;            // selected stream
    int bank_ = 0;           // MATRIX pads: streams 0-3 or 4-7
    int streamRow_ = 0;
    int secSel_ = 0, secField_ = 0, maskBit_ = 0;
    int mixField_ = 0;
    int perfField_ = 0;      // PERFORM field strip
    int dataSlot_ = 0, dataCol_ = 0;
    int clip_[P_COUNT] = {};
    bool clipValid_ = false;

    // registry ids
    lab::kit::ParamId pid_[kStreams][P_COUNT];
    lab::kit::ParamId quantId_ = -1, morphId_ = -1, stutId_ = -1, freezeId_ = -1, depthId_ = -1;
    lab::kit::ParamId scrubId_[kSlots] = {-1, -1, -1, -1};
    lab::kit::ParamId visId_ = -1, followId_ = -1, autoId_ = -1, speedId_ = -1;
    lab::kit::ParamId songId_ = -1;

    // performance state
    uint32_t thHeld_ = 0, thLatch_ = 0, fill_ = 0, slotSoloMask_ = 0;
    int slotHeld_ = -1;          // PERFORM: slot pad held (scrub / patch)
    bool slotUsed_ = false;      // the held slot pad did something (no select on release)
    int scrubSlot_ = 0;
    int secBank_ = 0;
    int morphBars_ = 0;          // registry: 0..16 bars (0 = instant)
    Scene* scene_ = nullptr;     // kScenes + 1 (the take's start snapshot)
    lab::kit::ParamId sceneIds_[kSceneMax];
    int sceneN_ = 0;
    int sceneSel_ = 0;
    int morphTo_ = -1, morphPending_ = -1;
    uint32_t morphAt_ = 0, morphLen_ = 0, morphStart_ = 0;
    float morphFrom_[kSceneMax];
    float morphProg_ = 0.0f;     // mod source
    bool morphing_ = false;
    // recorder
    static constexpr int kPerfMax = 8192;
    PerfEv* perf_ = nullptr;
    int perfN_ = 0, perfPos_ = 0;
    bool perfArm_ = false, perfRec_ = false, perfReplay_ = false, perfReplaying_ = false;
    uint32_t perfLen_ = 0;
    int perfStartSec_ = 0;
    bool inReplay_ = false;
    uint32_t playSeen_ = 0;
    void onPlayStarted();
    void perfDispatch(const PerfEv& e);

    // pieces
    static constexpr int kMaxPieces = 48;
    char (*pieces_)[24] = nullptr;
    int pieceCount_ = 0, pieceSel_ = 0;
    char pieceName_[24] = {};
    bool delArm_ = false;
    char* pieceBuf_ = nullptr;
    static constexpr uint32_t kPieceBuf = 256u * 1024u;

    // loader
    static constexpr int kJobQ = 8;
    LoadJob jobs_[kJobQ];
    int jobCount_ = 0;
    ActiveJob job_;
    uint8_t* stage_ = nullptr;
    static constexpr uint32_t kStageBytes = 4u << 20;
    uint8_t* builtinBuf_ = nullptr;
    MidiEv* midiScratch_ = nullptr;
    uint32_t slotGen_[kSlots] = {};
    SlotMeta meta_[kSlots];
    uint32_t fileSize_[kSlots] = {};
    char status_[64] = {};
    uint32_t statusUntil_ = 0;
    char pendingPick_[128] = {};
    int pickSlot_ = 0;

    // visuals: waterfall ring (RGB332) in RAM + RAM_G
    static constexpr int kWfW = 400, kWfH = 240;
    static constexpr uint32_t kRamWf = 0x000000;
    static constexpr int kBcW = 200, kBcH = 12;           // slot barcodes (DATA), 4 stacked
    static constexpr uint32_t kRamBc = 0x020000;
    static constexpr int kMxW = 160, kMxH = 64, kMxPast = 16;
    static constexpr uint32_t kRamMx = 0x028000;
    uint8_t* wf_ = nullptr;
    int wfHead_ = 0;
    int wfDirtyCount_ = 0;
    bool wfAllDirty_ = true;
    uint32_t wfLastMs_ = 0, wfRows_ = 0;
    uint8_t* bc_ = nullptr;
    uint32_t bcGen_[kSlots] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    bool bcUploaded_ = false;
    uint8_t* mx_ = nullptr;
    uint32_t mxLastMs_ = 0;
    bool mxUploaded_ = false;
    lab::IDisplay* disp_ = nullptr;
    struct LogLine {
        uint32_t idx;
        float hz;
        uint8_t s, val, sound;
    };
    LogLine log_[10] = {};
    int logHead_ = 0;
    uint32_t wfPend_[kStreams] = {};
    bool wfSecMark_ = false;
    uint32_t flashMs_[kStreams] = {};
    uint8_t lastVal_[kStreams] = {};
    uint32_t lastIdx_[kStreams] = {};
    float lastHz_[kStreams] = {};
    uint32_t evCount_ = 0;
    int lastSec_ = -1;
    uint32_t nowMs_ = 0;
    // VIEW
    int visMode_ = VM_STREAM, follow_ = 1, visAuto_ = 0, wfSpeed_ = 1;
    uint32_t visAutoMs_ = 0;
    uint8_t win_[1024];          // the bytes around the followed read head
    float yaw_ = 0.6f;
    float level_ = 0.0f;         // smoothed event activity (visual reaction)

    // settings rows
    int setAutoload_ = 1;

    // mod sources
    float srcMorph_ = 0.0f;

    // render scratch
    float stemL_[4][lab::kBlock];
    float stemR_[4][lab::kBlock];
};

}  // namespace dm
