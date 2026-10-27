// GRAF -- the workstation: pages, pads, encoder, files, LABKIT (lab::Workstation).
//
// Files of the workstation:
//   graf_ws.cpp       init, settings, files, update, MIDI, render
//   graf_input.cpp    keys, pads and encoder of every page
//   graf_nav.cpp      LABKIT v2: knobs in the registry, pages, sources, focus
//   graf_undo.cpp     undo / redo, auto-layout, follow-the-signal
//   graf_presets.cpp  demos, templates, bench scenarios
//   graf_draw.cpp     the intro, the seven pages, pad LEDs
#pragma once

#include "lab/lab.h"
#include "lab/mod.h"
#include "lab/nav.h"
#include "lab/params.h"
#include "ws/graf_engine.h"
#include "ws/graf_modules.h"

namespace graf {

using lab::Canvas;
using lab::Event;
using lab::Rgb;
namespace kit = lab::kit;

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
extern const char* const kCatNames[kCatCount];
constexpr int kTemplates = 9;

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
    // LABKIT v2
    int navPages(kit::PageInfo* out, int cap) override;
    int navPage() const override { return page_; }
    void navGoto(int page) override;
    void navFocus(kit::ParamId id) override;
    void navParamChanged(kit::ParamId id) override;

    // used by tests / demos
    Engine& engine() { return eng_; }
    int page() const { return page_; }
    int selected() const { return sel_; }
    int canvas() const { return patch_; }
    bool savePatch(const char* path);
    bool loadPatch(const char* path);
    void loadDemo(int i);
    static int demoCount();
    static const char* demoName(int i);
    static const char* templateName(int i);
    int addTemplate(int i, int patch, int nearCell);   // modules added (0 = none fitted)
    void setModel(bool rack);
    void rebuildJacks();
    int nSrc() const { return nSrc_; }
    int nDst() const { return nDst_; }
    const Jack& src(int i) const { return srcs_[i]; }
    const Jack& dst(int i) const { return dsts_[i]; }
    kit::ParamId knobId(int slot, int p) const { return knobBase_ < 0 ? kit::kNoParam : knobBase_ + slot * kMaxParam + p; }
    // undo / redo (true = something changed)
    void checkpoint(int kind = 0, int target = -1);
    bool undo();
    bool redo();
    int undoDepth() const { return nUndo_; }
    int redoDepth() const { return nRedo_; }
    void tidy(int patch);           // auto-layout of a canvas
    void select(int slot);
    void reassignPages();           // LABKIT: which knobs sit on which page / pad

private:
    // events per page
    void evGraph(const Event& e);
    void evMatrix(const Event& e);
    void evModule(const Event& e);
    void evAdd(const Event& e);
    void evSnap(const Event& e);
    void evPlay(const Event& e);
    void evFile(const Event& e);
    bool globalKey(const Event& e);
    void fkey(int k, bool shift);
    void action(bool shift);   // F8
    void go(int page);
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
    void patchGesture(int srcSlot, int dstSlot);   // graph: hold A + tap B
    void cycleGestureJack(int dOut, int dIn);
    void follow(bool down);                        // follow the signal
    void addSelected(bool patchFromSel);           // Add page: F8 / Insert / Shift+F8
    void openSampleBrowser();
    void editKnob(int slot, int p, int delta, bool fine);
    int addOrderCount(int cat) const;
    int addTypeAt(int cat, int i) const;
    // LABKIT
    void registerParams();
    void modulesChanged();
    void knobChanged(int slot, int p);
    void dropRoutes(int slot);
    void clearAllRoutes();
    // drawing
    void drawGraph(Canvas& g);
    void drawMatrix(Canvas& g);
    void drawModule(Canvas& g);
    void drawAdd(Canvas& g);
    void drawSnap(Canvas& g);
    void drawPlay(Canvas& g);
    void drawFile(Canvas& g);
    void drawTop(Canvas& g, const char* title);
    void drawScope(Canvas& g, int x, int y, int w, int h, Rgb c, float gain = 1.0f);
    void drawKnob(Canvas& g, int x, int y, int r, int slot, int p, bool sel);
    void drawPatchStrip(Canvas& g, int y);
    void cellXY(int cell, float* x, float* y) const;
    void jackXY(int slot, bool out, int jack, float* x, float* y) const;
    void fmtParam(char* buf, int cap, int slot, int p) const;
    void modLabel(char* buf, int cap, int slot) const;
    void jackLabel(char* buf, int cap, const Jack& j, bool out) const;

    lab::Context* ctx_ = nullptr;
    lab::Theme theme_;
    Engine eng_;
    int page_ = kPgGraph, prevPage_ = kPgGraph;
    int patch_ = 0;           // the canvas on screen
    int sel_ = -1;            // selected module slot
    int cursor_ = 0;          // graph cursor cell
    int param_ = 0;           // selected parameter on the module page
    // matrix
    Jack srcs_[kMaxSlots * kMaxOut];
    Jack dsts_[kMaxSlots * kMaxIn];
    int nSrc_ = 0, nDst_ = 0;
    int mRow_ = 0, mCol_ = 0;       // pad window origin
    int selCable_ = -1;
    int heldPin_ = -1;              // matrix pad held
    // graph patch gesture
    int holdCell_ = -1;             // first pad held (source)
    int gestCable_ = -1;            // the cable the gesture made / picked
    bool gestUsed_ = false;
    int emptyTap_ = -1;             // graph: last tapped empty cell
    int followIdx_ = 0;
    // add page
    int addCat_ = 0;                // 0..6 categories, 7 templates
    int addType_ = 0;
    int addTpl_ = 0;
    int addCell_ = -1;
    int addTapPad_ = -1;
    uint32_t addTapMs_ = 0;
    uint8_t recent_[6] = {0, 24, 18, 35, 11, 33};
    // snapshots / song
    int songSel_ = 0, songField_ = 0;
    int snapSel_ = 0;
    // play page
    int stripSel_ = 0, stripField_ = 0;
    // file page
    int fileSel_ = 0;
    bool fileExists_[24] = {};
    bool demoPicked_ = false;
    bool confirm_ = false;
    int confirmWhat_ = -1;
    int demoSel_ = 0;
    // module page
    int heldKnob_ = -1;             // knob pad held (row 1)
    bool knobUsed_ = false;
    int heldStep_ = -1;             // SEQ step held
    bool stepUsed_ = false;
    // pads held
    bool held_[lab::kPads] = {};
    uint32_t heldMs_[lab::kPads] = {};
    int heldNote_[lab::kPads];
    // prefs
    int prefMorph_ = 3;       // live recall morph: 0 cut .. 6 = 8 bars
    int prefSongPlay_ = 0;
    int prefOctave_ = 0;
    int prefVel_ = 1;         // pad velocity on
    int prefModel_ = 0;       // 0 rack, 1 eight patches (mirrors eng_.rack)
    int prefSag_ = 1;
    // crossfader (SNAPS page, LABKIT params)
    int xfA_ = 0, xfB_ = 1, xfOn_ = 0;
    float xf_ = 0.0f, xfLast_ = -1.0f;
    // LABKIT
    kit::ParamId knobBase_ = kit::kNoParam;
    kit::ParamId stripLvl_[kPatches] = {}, stripPan_[kPatches] = {}, stripMute_[kPatches] = {};
    kit::ParamId xfAId_ = kit::kNoParam, xfBId_ = kit::kNoParam, xfId_ = kit::kNoParam, xfOnId_ = kit::kNoParam;
    int catGroup_[kCatCount] = {};
    char (*knobName_)[kMaxParam][14] = nullptr;   // [kMaxSlots]
    bool pagesDirty_ = true;
    bool inNav_ = false;      // a LABKIT edit is being applied (no feedback loop)
    // undo: two stacks of structure states
    static constexpr int kUndo = 24;
    static constexpr uint32_t kUndoBytes = 12288;
    uint8_t* undoMem_ = nullptr;   // kUndo states
    uint8_t* redoMem_ = nullptr;
    uint32_t undoLen_[kUndo] = {}, redoLen_[kUndo] = {};
    int undoTop_ = 0, nUndo_ = 0, redoTop_ = 0, nRedo_ = 0;
    int lastCpKind_ = -1, lastCpTarget_ = -1;
    uint32_t lastCpMs_ = 0;
    // misc
    uint32_t now_ = 0;
    bool samplesScanned_ = false;
    float meterShow_[kPatches] = {};
    float masterShow_ = 0.0f;
    uint32_t blinkMs_ = 0;
    uint32_t lastJacks_ = 0, lastMorph_ = 0;
    int benchScenario_ = 0;
    uint8_t* fileBuf_ = nullptr;
    static constexpr uint32_t kFileBuf = 160 * 1024;
    uint8_t* stateBuf_ = nullptr;   // kUndoBytes scratch
};

}  // namespace graf
