// LABKIT v2 -- navigation: MAP, PARAM pads, MACRO and MOD pages, value HUD,
// HELP, MIDI learn (lab::kit::Nav), and the Kit that holds every LABKIT part.
//
// Keys (defaults, NAV.md; a workstation can move them with navKeys()):
//   I.Param          tap: PARAM pads on / off, hold: PARAM pads while held
//   Shift + I.Param  MACRO page
//   Master           hold: MAP while held, tap: MAP stays open
//   Shift + Master   MOD page (LFOs, random, routes)
//   Shift + Config   HELP (the page's help and this key card)
// All of them exist on the original Tracker's key matrix; none of Aux, Help
// or the encoder push is needed (they are not on that hardware).
//
// The navigation is on once the workstation registered a parameter or
// described its pages; otherwise the Shell behaves exactly as before.
#pragma once

#include "lab/lab.h"
#include "lab/mod.h"
#include "lab/params.h"

namespace lab {
namespace kit {

constexpr int kMaxPages = 16;      // workstation pages the MAP shows (+ MACRO, MOD)
constexpr uint8_t kNoKey = 0xFF;

struct PageInfo {
    const char* name = "";           // short, upper case: "OSC"
    const char* help = "";           // one line: what the page is for
    const char* fkeys[8] = {};       // one line per F-key, nullptr = unused
    Rgb color{0, 0, 0};              // 0 = the theme accent
};

struct NavKeys {
    uint8_t param = btn::InstrParams;   // PARAM pads (Shift: MACRO page)
    uint8_t map = btn::Master;          // MAP (Shift: MOD page)
    bool shiftConfigHelp = true;        // Shift + Config = HELP
};

class Browser;
class Keyboard;

// ---- drawing helpers for workstation pages built from the registry ----------
// A knob with name and value; ring = the effective (modulated) value.
void drawParamKnob(Canvas& g, const Theme& t, const Registry& r, ParamId id, int cx, int cy, int radius,
                   bool focused);
// A horizontal bar with name and value (w >= 120).
void drawParamBar(Canvas& g, const Theme& t, const Registry& r, ParamId id, int x, int y, int w, bool focused);
// Pen advance of a string in ROM font `fnt` (26..31), pixels.
int textWidth(uint8_t fnt, const char* s);
// Copy `s` into buf, cut with ".." to fit `px` pixels in font `fnt`.
void fitText(char* buf, int cap, uint8_t fnt, const char* s, int px);

class Nav {
public:
    enum Overlay : uint8_t { kNone, kParam, kMap, kMacro, kMod, kHelp, kBrowser, kKeyboard };

    void attach(Kit* kit) { kit_ = kit; }
    // Shell hooks (main loop).
    bool event(const Event& e);          // true = consumed
    bool midi(const MidiMsg& m);         // true = consumed (MIDI learn / learned CC)
    void update(uint32_t now);
    bool fullScreen() const;             // the Shell skips the workstation's draw
    uint32_t reserveWords() const;       // display words the overlay needs on top of the page
    void draw(Canvas& g);                // after the workstation's draw (or instead of it)
    bool leds(Rgb* pads);                // true = the overlay owns the pads
    void hud(ParamId id);                // show the value HUD for a param

    Overlay overlay() const { return ov_; }
    const char* overlayName() const;
    void open(Overlay o);
    void close();
    ParamId focus() const { return focus_; }
    void setFocus(ParamId id);
    int layerPage() const { return layerPage_; }
    int pageCount() const { return pageCount_; }     // workstation pages + MACRO + MOD
    int pageIndexOf(int page) const;                 // page id -> MAP column
    int pageAt(int col) const;                       // MAP column -> page id
    const char* pageName(int page) const;
    // "OSC > Cutoff" (withPage), "Cutoff"; MOD-page params read "LFO 2 Rate".
    void paramLabel(ParamId id, char* buf, int cap, bool withPage) const;
    void refreshPages();
    void toggleHelp();                               // Shift + Config
    int macroSelected() const { return macroSel_; }
    const NavKeys& keys() const { return keys_; }
    void setKeys(const NavKeys& k) { keys_ = k; }

    // MIDI learn table (saved with the registry chunk).
    struct Learn {
        uint8_t status = 0, cc = 0;   // status = 0xB0 | channel; 0 = unused
        ParamId param = kNoParam;
    };
    static constexpr int kMaxLearn = 32;
    Learn learn_[kMaxLearn];
    int learnCount() const;
    void clearLearn() {
        for (auto& l : learn_) l = Learn();
    }
    bool learnAssign(uint8_t status, uint8_t cc, ParamId p);

private:
    // events per overlay
    bool layerEvent(const Event& e, bool inMap);
    bool macroEvent(const Event& e);
    bool modEvent(const Event& e);
    bool helpEvent(const Event& e);
    void editFocused(int delta, bool fine);
    void layerPad(int pad, bool down, bool inMap);
    void sourceEncoder(int src, int delta, bool fine);
    ParamId padParam(int pad, bool inMap) const;
    int padSource(int pad) const;        // PARAM layer row 3 -> source id (-1)
    void loadLayout(int page);
    void jumpToPage(int page);
    // drawing
    void drawLayer(Canvas& g);
    void drawMap(Canvas& g);
    void drawMacro(Canvas& g);
    void drawMod(Canvas& g);
    void drawHelp(Canvas& g);
    void drawHud(Canvas& g);
    void drawFocusStrip(Canvas& g, int y);
    void layerLeds(Rgb* pads, bool inMap);
    const Theme& theme() const;

    Kit* kit_ = nullptr;
    NavKeys keys_;
    Overlay ov_ = kNone;
    Overlay underModal_ = kNone;      // what the browser / keyboard returns to
    bool latched_ = false;            // tap-latched (vs momentary while the key is held)
    uint32_t keyDownMs_ = 0;
    bool keyUsed_ = false;            // something happened while the key was held
    uint8_t keyDown_ = kNoKey;
    ParamId focus_ = kNoParam;
    ParamId lastTouched_ = kNoParam;  // the last param that changed (HUD)
    int layerPage_ = 0;
    int lastWsPage_ = -1;             // the workstation's page as last seen
    int layerBank_ = 0;               // row 3 bank: 0 LFO/RND/M1-6, 1 M7-8 + workstation sources
    ParamId layout_[kPadParams];
    int layoutPage_ = -999;
    int8_t heldParamPad_ = -1;        // pad index (0..47) of the held param pad
    int8_t heldSourcePad_ = -1;
    int lastRoute_ = -1;              // route edited while the source pad is held
    uint32_t padMs_ = 0;              // when the held source pad went down
    bool padMine_[kPads] = {};        // pad presses the overlay consumed
    bool learnArmed_ = false;
    // MAP / pages
    int pageCount_ = 0, wsPages_ = 0;
    int pageIds_[kMaxPages + 2] = {};
    PageInfo pages_[kMaxPages];
    int mapCol_ = 0;
    int returnPage_ = -1;
    // MACRO page
    int macroSel_ = 0, macroRow_ = 0, macroField_ = 0;
    // MOD page
    int modRow_ = 0, modField_ = 0;
    // HUD
    ParamId hudParam_ = kNoParam;
    uint32_t hudUntil_ = 0;
    uint32_t now_ = 0;
};

// Every LABKIT part, allocated from the arena by the Shell (Shell::labkit()).
struct Kit {
    Shell* shell = nullptr;
    Workstation* ws = nullptr;
    Context* ctx = nullptr;
    Registry reg;
    Mod mod;
    Nav nav;
    Browser* browser = nullptr;
    Keyboard* keyboard = nullptr;
    bool active = false;             // navigation on (the workstation registered params / pages)
    bool audioRunning = false;
    bool registeringShell = false;   // the Shell's own params are being added (not counted)
    bool inShellUi = false;          // edits come from a Shell view (the workstation is told)
    uint32_t wsParams = 0;           // params the workstation registered
    int hudOn = 1;

    // Shell hooks.
    bool init(Shell& sh, Context& c, Arena& arena, bool audio);   // before createWorkstation()
    void afterWorkstationInit(Workstation* w);                     // turns the navigation on if used
    bool event(const Event& e);        // modals, then the navigation; true = consumed
    bool midi(const MidiMsg& m);
    void update(uint32_t now);
    bool fullScreen() const;
    uint32_t reserveWords() const;
    void draw(Canvas& g);
    bool leds(Rgb* pads);
    void audioBlock(const Transport& t);
    void mix(float* l, float* r);
    void powerOff();
    const Theme& theme() const;
    // labkit.bin in /Lab/<FW>/: browser favourites / recent, recent names, HUD.
    void loadPrefs();
    void savePrefs();
    bool prefsDirty = false;
    uint32_t lastSyncMs = 0, prefsMs = 0;
};

}  // namespace kit
}  // namespace lab
