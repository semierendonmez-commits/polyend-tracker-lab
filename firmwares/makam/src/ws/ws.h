// MAKAM-JUNGLE -- the workstation: pages, input, LEDs, project files.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "lab/nav.h"
#include "lab/params.h"
#include "ws/engine.h"
#include "ws/kit.h"
#include "ws/song.h"
#include "ws/theory.h"
#include "ws/usul.h"

namespace makam {

enum Page : int { kPgUsul, kPgMakam, kPgBreak, kPgMelody, kPgSynth, kPgArrange, kPgMixer, kPgFile, kPages };

// One editable value on a page.
struct PRow {
    const char* label = "";
    int* v = nullptr;
    int min = 0, max = 1;
    const char* const* names = nullptr;   // max - min + 1 entries
    uint8_t id = 0;                       // RowId: what changing it implies
};
enum RowId : uint8_t { kRidNone, kRidBpm, kRidUsul, kRidMakam, kRidTuning, kRidKit, kRidSongMode, kRidGen,
                       kRidBreakEdit, kRidVar, kRidPhrase, kRidSectionSel, kRidPhraseCycles, kRidAutoSlice };

// The palette: Istanbul at night, Iznik tiles.
namespace col {
constexpr lab::Rgb kNight{0x060B1A}, kPanel{0x101A33}, kPanel2{0x17244A}, kCream{0xF3EBDD}, kDim{0x5D6C8F},
    kTurq{0x2FD0C0}, kCobalt{0x2F5FD8}, kRed{0xE5484D}, kGold{0xE9B949}, kEmerald{0x2A9D6F}, kViolet{0x9B6BE0},
    kSaffron{0xF4A261}, kMuted{0x26304D};
}

class MakamWs final : public lab::Workstation {
public:
    const char* name() const override { return "MAKAM"; }
    const char* tagline() const override { return "MAKAM-JUNGLE: Turkish makam meets jungle / drum & bass"; }
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
    int benchScenarios() const override { return 7; }
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // LABKIT v2
    int navPages(lab::kit::PageInfo* out, int cap) override;
    int navPage() const override { return page_; }
    void navGoto(int page) override { setPage(page); }
    void navFocus(lab::kit::ParamId id) override;
    void navParamChanged(lab::kit::ParamId id) override;

    // Scenes (Perform + pad): snapshots of every continuous parameter, morphed.
    static constexpr int kScenes = 8;
    void storeScene(int i);
    void recallScene(int i, bool morph = true);
    bool sceneUsed(int i) const { return i >= 0 && i < kScenes && sceneUsed_[i]; }
    bool morphing() const { return morphOn_; }

    // Host tests.
    Song& song() { return *song_; }
    lab::kit::ParamId paramOf(const void* field) const;
    Engine& engine() { return eng_; }
    Kit& kit(int i) { return kits_[i]; }
    int page() const { return page_; }
    bool saveProject(const char* path);
    bool loadProject(const char* path);
    bool loadWav(const char* name);

private:
    // LABKIT glue (makam_nav.cpp)
    void registerParams();
    void pullFields();
    void defaultMacros();
    void applyRid(uint8_t rid);
    uint8_t ridOf(const void* field) const;
    void updateMorph(uint32_t ms);
    size_t saveTrailers(uint8_t* out, size_t cap) const;
    void loadTrailers(const uint8_t* data, size_t len);
    void openBreakBrowser();

    // pages
    int buildRows(int page, PRow* rows, int cap);
    void rowChanged(const PRow& r);
    void setPage(int p);
    void encoder(int delta, bool shift);
    void action(bool shift);
    void padDown(int pad, int vel, bool shift);
    void padUp(int pad);
    int padDegree(int pad) const;          // lead / bass degree of a play pad, or -100
    bool padIsLead(int pad) const;
    void breakStepPad(int step, bool shift);
    void melodyWrite(int deg);
    void sectionInsert();
    void sectionDelete();
    void sectionCopy();
    void scanFiles();
    void slotPath(int slot, char* buf, int cap) const;
    void selectKit(int k);

    // drawing (ui.cpp)
    void drawUsul(lab::Canvas& g);
    void drawMakam(lab::Canvas& g);
    void drawBreak(lab::Canvas& g);
    void drawMelody(lab::Canvas& g);
    void drawSynth(lab::Canvas& g);
    void drawArrange(lab::Canvas& g);
    void drawMixer(lab::Canvas& g);
    void drawFile(lab::Canvas& g);
    void drawRows(lab::Canvas& g, int x, int y, int w, int rowH, int first, int count);
    void drawDial(lab::Canvas& g, float cx, float cy, float r, bool showVar, int selStep);
    void drawTop(lab::Canvas& g, const char* title);
    float playFrac() const;               // 0..1 position in the usul cycle

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Song* song_ = nullptr;
    Kit kits_[kKits + 1];
    Engine eng_;
    float* scratch_ = nullptr;
    uint8_t* fileBuf_ = nullptr;

    int page_ = kPgUsul;
    int row_[kPages] = {};
    static constexpr int kMaxRows = 24;
    PRow rows_[kMaxRows];
    int rowCount_ = 0;
    int stepSel_ = 0;          // break step / bass step
    int phraseCur_ = 0;        // melody cursor
    int secSel_ = 0;           // arrangement selection
    int mixCol_ = 0;           // mixer column (0..3 channels, 4 fx)
    int fileCol_ = 0, fileSel_[2] = {};
    bool slotUsed_[8] = {};
    static constexpr int kMaxWavs = 24;
    char wavs_[kMaxWavs][40] = {};
    int wavCount_ = 0;
    bool padHeld_[lab::kPads] = {};
    int8_t padNote_[lab::kPads] = {};
    uint32_t lastMs_ = 0;
    uint32_t lastPhraseWraps_ = 0;
    int bendRange_ = 2, midiNotes_ = 1;
    int lastBpm_ = 0;
    bool dirtyState_ = false;
    bool benchMode_ = false;
    // LABKIT
    static constexpr int kMaxReg = 96;
    const void* regPtr_[kMaxReg] = {};
    lab::kit::ParamId regId_[kMaxReg] = {};
    int regCount_ = 0;
    // scenes
    static constexpr int kSceneParams = 80;
    lab::kit::ParamId sceneIds_[kSceneParams] = {};
    int sceneCount_ = 0;
    float* scene_ = nullptr;          // kScenes x kSceneParams normalized values
    bool sceneUsed_[kScenes] = {};
    float morphFrom_[kSceneParams] = {};
    int morphTo_ = -1;
    bool morphOn_ = false;
    uint32_t morphStart_ = 0, morphMs_ = 0, lastMorphMs_ = 0;
    int lastSceneSection_ = -1;
    bool performHeld_ = false;
    char pendingWav_[128] = {};
    bool pendingWavReady_ = false;
};

}  // namespace makam
