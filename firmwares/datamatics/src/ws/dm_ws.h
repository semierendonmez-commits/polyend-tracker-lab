// DATAMATICS -- the workstation: pages, pads, data loader, pieces, visuals.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/dm_data.h"
#include "ws/dm_engine.h"

namespace dm {

enum Page : uint8_t { PG_MATRIX, PG_STREAM, PG_DATA, PG_SECTIONS, PG_MIXER, PG_PIECE, PG_VIEW, PG_COUNT };

int streamParamAt(int row);
int streamParamRows();

// Composition file (text, versioned): serialise / parse. Pure (tests).
constexpr int kPieceVersion = 1;
uint32_t writePiece(const Engine& e, float bpm, char* out, uint32_t cap);
// Fills e (params, sections, options, slot paths + modes; does not load data).
bool readPiece(Engine& e, const char* text, uint32_t len, float* bpmOut);

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
    int benchScenarios() const override { return 5; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // tests / sim
    Engine& engine() { return eng_; }
    int page() const { return page_; }
    bool loadBusy() const { return jobCount_ > 0 || job_.active; }
    void requestLoad(int slot, const char* path, int mode);
    void finishLoadsNow();   // run queued loads to completion (tests)
    bool savePiece(const char* path);
    bool loadPiece(const char* path);

private:
    struct Entry {
        char name[48];
        uint32_t size;
        bool dir;
    };
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

    // pages
    void setPage(int p);
    void drawTop(lab::Canvas& g, const char* title);
    void drawKeys(lab::Canvas& g, const char* f7, const char* f8);
    void drawMatrix(lab::Canvas& g, bool full);
    void drawStream(lab::Canvas& g);
    void drawData(lab::Canvas& g);
    void drawSections(lab::Canvas& g);
    void drawMixer(lab::Canvas& g);
    void drawPiece(lab::Canvas& g);
    void drawWaterfall(lab::Canvas& g, int x, int y, int rows);

    void matrixEvent(const lab::Event& e);
    void streamEvent(const lab::Event& e);
    void dataEvent(const lab::Event& e);
    void sectionsEvent(const lab::Event& e);
    void mixerEvent(const lab::Event& e);
    void pieceEvent(const lab::Event& e);
    void padEvent(const lab::Event& e);
    void editParam(int s, int p, int delta, bool coarse);
    bool wouldHit(int s, uint8_t val) const;

    // data
    void pumpLoader();
    void finishJob();
    void scanDir();
    void enterEntry();
    void scanPieces();
    void newPiece();
    void loadBuiltinNow(int slot, int b, int mode);

    // visuals
    void composeRow();
    void composeMatrix();
    void uploadVisuals();
    void drawIntroBars(lab::Canvas& g, float reveal, float height, int cy);
    void rebuildPreview(int slot);
    void drainEvents(uint32_t ms);
    bool previewXY(int slot, uint32_t idx, int& x, int& y) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine eng_;
    int page_ = PG_MATRIX, lastPage_ = PG_MATRIX;
    int sel_ = 0;            // selected stream
    int bank_ = 0;           // pads: streams 0-3 or 4-7
    int streamParam_ = 0;
    int secSel_ = 0, secField_ = 0, maskBit_ = 0;
    int mixField_ = 0;
    int clip_[P_COUNT] = {};
    bool clipValid_ = false;

    // browser
    static constexpr int kMaxEntries = 160;
    Entry* ent_ = nullptr;
    int entCount_ = 0, entSel_ = 0, entTop_ = 0;
    char dir_[128] = {};
    int target_ = 0;         // slot the browser loads into
    int loadMode_ = kAuto;
    char status_[64] = {};
    uint32_t statusUntil_ = 0;

    // pieces
    static constexpr int kMaxPieces = 48;
    char (*pieces_)[24] = nullptr;
    int pieceCount_ = 0, pieceSel_ = 0;
    char pieceName_[24] = {};
    bool delArm_ = false;

    // loader
    static constexpr int kJobQ = 8;
    LoadJob jobs_[kJobQ];
    int jobCount_ = 0;
    ActiveJob job_;
    uint8_t* stage_ = nullptr;
    uint8_t* builtinBuf_ = nullptr;
    static constexpr uint32_t kStageBytes = 4u << 20;
    MidiEv* midiScratch_ = nullptr;
    uint32_t slotGen_[kSlots] = {};

    // visuals: waterfall ring (RGB332) in RAM + RAM_G
    static constexpr int kWfW = 400, kWfH = 240;
    static constexpr uint32_t kRamWf = 0x000000;
    static constexpr int kPvW = 150, kPvH = 50;
    static constexpr int kMxW = 160, kMxH = 64, kMxPast = 16;
    static constexpr uint32_t kRamMx = 0x028000;
    uint8_t* mx_ = nullptr;
    uint32_t mxLastMs_ = 0;
    bool mxUploaded_ = false;
    struct LogLine { uint32_t idx; float hz; uint8_t s, val, sound; };
    LogLine log_[10] = {};
    int logHead_ = 0;
    uint32_t wfPend_[kStreams] = {};   // value bits OR-ed since the last row
    bool wfSecMark_ = false;
    static constexpr uint32_t kRamPv = 0x020000;
    uint8_t* wf_ = nullptr;
    int wfHead_ = 0;          // next row to write
    int wfDirtyFrom_ = -1, wfDirtyCount_ = 0;
    bool wfAllDirty_ = true;
    uint32_t wfLastMs_ = 0, wfRows_ = 0;
    uint32_t liveUntil_ = 0;
    uint8_t* pv_ = nullptr;
    int pvSlot_ = -1;
    uint32_t pvGen_ = 0xFFFFFFFFu;
    bool pvUploaded_ = false;
    lab::IDisplay* disp_ = nullptr;
    uint8_t fired_[kStreams] = {};       // events since the last waterfall row
    uint32_t flashMs_[kStreams] = {};
    uint8_t lastVal_[kStreams] = {};
    uint32_t lastIdx_[kStreams] = {};
    float lastHz_[kStreams] = {};
    uint32_t evCount_ = 0;
    int lastSec_ = -1;
    uint32_t secFlashMs_ = 0;
    uint32_t nowMs_ = 0;

    // settings rows
    int setArrange_ = 1, setEnd_ = 0, setMidi_ = 0, setJump_ = 0, setSpeed_ = 1, setAutoload_ = 1;

    // render scratch
    float stemL_[4][lab::kBlock];
    float stemR_[4][lab::kBlock];
};

}  // namespace dm
