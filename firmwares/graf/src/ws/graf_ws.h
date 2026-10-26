// GRAF -- the workstation: pages, pads, encoder, files (lab::Workstation).
#pragma once

#include "lab/lab.h"
#include "ws/graf_engine.h"
#include "ws/graf_modules.h"

namespace graf {

using lab::Canvas;
using lab::Event;
using lab::Rgb;

enum Page : uint8_t { kPgGraph, kPgMatrix, kPgModule, kPgAdd, kPgSnap, kPgPlay, kPgFile, kPgCount };

// One row (source = module output) or column (destination = module input)
// of the pin matrix.
struct Jack {
    uint8_t slot, jack;
};

// Palette.
namespace col {
constexpr Rgb kBg{0x0E1013}, kPanel{0x1A1D22}, kPanel2{0x252A31}, kRail{0x31373F}, kFg{0xE4E7EA},
    kDim{0x7C848E}, kFaint{0x464C55}, kAccent{0xFFB000}, kWarn{0xFF4040}, kOk{0x3CE07A};
constexpr Rgb kSig[4] = {Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0xFFD23A), Rgb(0xFF4FA0)};   // audio cv gate pitch
constexpr Rgb kCat[kCatCount] = {Rgb(0xF07A3A), Rgb(0xE0484E), Rgb(0x3A8EF0), Rgb(0xE8C030),
                                 Rgb(0xA060F0), Rgb(0x40C878), Rgb(0x20D0C0)};
}  // namespace col

class Graf final : public lab::Workstation {
public:
    const char* name() const override { return "GRAF"; }
    const char* tagline() const override { return "Modular synth workstation: pin matrix + patch graph"; }
    const lab::Theme& theme() const override { return theme_; }
    bool init(lab::Context& c) override;
    uint32_t introMs() const override { return 2800; }
    void drawIntro(Canvas& g, uint32_t t) override;
    void onEvent(const Event& e) override;
    void onMidi(const lab::MidiMsg& m) override;
    void update(uint32_t ms) override;
    void draw(Canvas& g) override;
    void leds(Rgb* pads) override;
    void render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) override;
    void onTransport(bool playing) override;
    int settings(lab::SettingRow* rows, int cap) override;
    void save() override;
    int benchScenarios() const override;
    const char* benchName(int i) const override;
    void benchSetup(int i) override;

    // used by tests / demos
    Engine& engine() { return eng_; }
    int page() const { return page_; }
    int selected() const { return sel_; }
    int viewPatch() const { return eng_.rack ? 0 : patch_; }
    bool savePatch(const char* path);
    bool loadPatch(const char* path);
    void loadDemo(int i);
    static int demoCount();
    static const char* demoName(int i);
    void setModel(bool rack);
    void rebuildJacks();
    int nSrc() const { return nSrc_; }
    int nDst() const { return nDst_; }
    const Jack& src(int i) const { return srcs_[i]; }
    const Jack& dst(int i) const { return dsts_[i]; }

private:
    // events per page
    void evGraph(const Event& e);
    void evMatrix(const Event& e);
    void evModule(const Event& e);
    void evAdd(const Event& e);
    void evSnap(const Event& e);
    void evPlay(const Event& e);
    void evFile(const Event& e);
    void fkey(int k, bool shift);
    void action(bool shift);   // F8
    void go(int page);
    void select(int slot);
    void keyPad(int pad, bool down, uint8_t vel);   // keyboard pads
    int keyNote(int pad) const;                     // -1 = not a key
    int keyBase() const;
    void toastErr();
    void deleteSelected();
    void scanSamples();
    void loadPrefs();
    void savePrefs();
    void listFiles();
    uint32_t morphMs() const;
    void moveCursor(int dx, int dy);
    void matrixPad(int pad, bool down);
    int matrixCable(int r, int c) const;   // cable index at matrix row / col, -1
    void focusMatrix(int srcSlot, int dstSlot);

    // drawing
    void drawGraph(Canvas& g);
    void drawMatrix(Canvas& g);
    void drawModule(Canvas& g);
    void drawAdd(Canvas& g);
    void drawSnap(Canvas& g);
    void drawPlay(Canvas& g);
    void drawFile(Canvas& g);
    void drawTop(Canvas& g, const char* title);
    void drawKeys(Canvas& g);
    void drawScope(Canvas& g, int x, int y, int w, int h, Rgb c);
    void drawKnob(Canvas& g, int x, int y, int slot, int p, bool sel);
    void cellXY(int cell, float* x, float* y) const;
    void jackXY(int slot, bool out, int jack, float* x, float* y) const;
    void fmtParam(char* buf, int cap, int slot, int p) const;
    void modLabel(char* buf, int cap, int slot) const;
    void jackLabel(char* buf, int cap, const Jack& j, bool out) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine eng_;
    int page_ = kPgGraph, prevPage_ = kPgGraph;
    int patch_ = 0;
    int sel_ = -1;            // selected module slot
    int cursor_ = 0;          // graph cursor cell
    int param_ = 0;           // selected parameter on the module page
    // matrix
    Jack srcs_[kMaxSlots * kMaxOut];
    Jack dsts_[kMaxSlots * kMaxIn];
    int nSrc_ = 0, nDst_ = 0;
    int mRow_ = 0, mCol_ = 0;       // pad window origin
    int selCable_ = -1;
    int mCurR_ = 0, mCurC_ = 0;     // matrix cursor (absolute)
    // add page
    int addType_ = 0;
    int addCell_ = -1;
    int addTapPad_ = -1;
    uint32_t addTapMs_ = 0;
    int smplWant_[kMaxSlots] = {};
    uint32_t smplSince_[kMaxSlots] = {};
    uint8_t addOrder_[kTypeCount];
    // snapshots / song
    int songSel_ = 0, songField_ = 0;
    int snapSel_ = 0;
    // play page
    int stripSel_ = 0, stripField_ = 0;
    // file page
    int fileSel_ = 0;
    bool fileExists_[24] = {};
    bool confirm_ = false;
    int confirmWhat_ = -1;
    int demoSel_ = 0;
    // pads held
    bool held_[lab::kPads] = {};
    uint32_t heldMs_[lab::kPads] = {};
    int heldNote_[lab::kPads];
    int holdCell_ = -1;
    int emptyTap_ = -1;       // graph: last tapped empty cell       // graph: first pad held (for connecting)
    int heldStep_ = -1;       // module page: SEQ step held
    bool usedHold_ = false;
    // prefs
    int prefMorph_ = 3;       // live recall morph: 0 cut .. 6 = 8 bars
    int prefSongPlay_ = 0;
    int prefOctave_ = 0;
    int prefVel_ = 1;         // pad velocity on
    int prefModel_ = 0;       // 0 rack, 1 eight patches (mirrors eng_.rack)
    int prefSag_ = 1;
    // misc
    uint32_t now_ = 0;
    uint32_t lastSave_ = 0;
    uint32_t lastJacks_ = 0, lastMorph_ = 0;
    bool samplesScanned_ = false;
    char toastBuf_[64] = {};
    float meterShow_[kPatches] = {};
    float masterShow_ = 0.0f;
    uint32_t blinkMs_ = 0;
    int benchScenario_ = 0;
    uint8_t* fileBuf_ = nullptr;
    static constexpr uint32_t kFileBuf = 160 * 1024;
};

}  // namespace graf
