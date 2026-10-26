// polyend-tracker-lab -- the shell every lab firmware is built on.
//
// The stock-style tracker App (src/core/app, src/core/ui) is not used by a lab
// firmware: src/app_main.cpp (device) and sim/src/lab_main.cpp (simulator) build a
// lab::Shell instead, and the Shell runs one Workstation (src/ws/). The rest of
// OpenTracker -- HAL, FT81x display, codec, SD card, MIDI, pads, radio, the
// audio helpers in src/core/audio and the WAV code in src/core/formats -- is
// used as it is.
//
// The Shell owns:
//   - the boot intro (each workstation draws its own, Settings > Intro turns it off),
//   - the Settings page (Config button): general rows, the workstation's own rows,
//     Firmware update (any .ptf in /Firmware, so you can always flash back), About,
//   - transport (tempo, play/stop, MIDI clock in/out),
//   - audio I/O: line-in / radio / mic selection, the master limiter, the output,
//   - the master + stem recorder (Rec button, WAV files in /Lab/<FW>/Rec),
//   - pad LEDs, MIDI parsing, the settings file (/Lab/<FW>/settings.bin).
//
// Rules for workstation code (same as OpenTracker's core rules):
//   - no exceptions, no RTTI, no heap after init: allocate from Arena in init(),
//   - render() runs in the audio interrupt: no file access, no display, no locks,
//     never block; main-loop <-> audio data goes through plain fields written by
//     one side only, or the small SPSC rings below,
//   - keep a frame under Canvas::kBudgetWords display-list words.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "hal_api.h"
#include "ui/buttons.h"

namespace lab {

using ot::IDisplay;
using ot::IStorage;

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kBlock = 128;          // frames per audio block
constexpr int kPads = 48, kPadCols = 12, kPadRows = 4;
constexpr int kScreenW = 800, kScreenH = 480;

// ---------------------------------------------------------------------------
// Memory: one bump arena over the SDRAM (device) or a host buffer (sim/tests).
class Arena {
public:
    void attach(uint8_t* base, uint32_t size) { base_ = base; size_ = size; used_ = 0; }
    void* alloc(uint32_t bytes, uint32_t align = 16) {
        uint32_t at = (used_ + (align - 1)) & ~(align - 1);
        if (!base_ || at + bytes > size_) return nullptr;
        used_ = at + bytes;
        std::memset(base_ + at, 0, bytes);
        return base_ + at;
    }
    template <class T> T* make(uint32_t n = 1) {
        void* p = alloc(uint32_t(sizeof(T)) * n, alignof(T) < 16 ? 16 : uint32_t(alignof(T)));
        if (!p) return nullptr;
        T* t = static_cast<T*>(p);
        for (uint32_t i = 0; i < n; ++i) new (&t[i]) T();
        return t;
    }
    uint32_t used() const { return used_; }
    uint32_t free() const { return size_ - used_; }
    uint32_t size() const { return size_; }

private:
    uint8_t* base_ = nullptr;
    uint32_t size_ = 0, used_ = 0;
};

// ---------------------------------------------------------------------------
// Colours and the per-firmware theme.
struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    constexpr Rgb() = default;
    constexpr Rgb(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
    constexpr explicit Rgb(uint32_t hex) : r(uint8_t(hex >> 16)), g(uint8_t(hex >> 8)), b(uint8_t(hex)) {}
};
Rgb mix(Rgb a, Rgb b, float t);
Rgb scale(Rgb a, float k);

struct Theme {
    Rgb bg{0x000000}, panel{0x101010}, fg{0xFFFFFF}, dim{0x707070}, accent{0x00C0FF}, accent2{0xFF4080},
        warn{0xFF3030}, ok{0x30FF70};
};

// FT81x ROM font numbers (16..31). 26..31 are anti-aliased: 26 ~12 px tall,
// 27 ~14, 28 ~17, 29 ~20, 30 ~24, 31 ~30. 16..25 are the legacy fonts.
namespace font {
constexpr uint8_t kTiny = 26, kSmall = 27, kBody = 28, kLarge = 29, kTitle = 30, kHuge = 31;
}

// ---------------------------------------------------------------------------
// Canvas: drawing on the FT81x display list with a word budget. The FT81x RAM_DL
// holds 2048 words; a frame that overflows it faults the coprocessor, so every
// primitive is counted and a frame stops drawing once it would go over.
class Canvas {
public:
    static constexpr uint32_t kBudgetWords = 1800;

    void attach(IDisplay* d) { d_ = d; }
    IDisplay* display() { return d_; }
    void begin(Rgb clearColor);
    void end();   // DISPLAY + swap
    uint32_t words() const { return words_; }
    uint32_t dropped() const { return dropped_; }
    bool room(uint32_t w) const { return words_ + w <= limit_; }
    // The Shell keeps words for its own overlays (LABKIT HUD, PARAM pads):
    // primitives past the limit are skipped like over-budget ones, but they are
    // counted in clipped(), not dropped(). begin() resets the limit.
    void setLimit(uint32_t w) { limit_ = w > kBudgetWords ? kBudgetWords : w; }
    uint32_t clipped() const { return clipped_; }

    void color(Rgb c);
    void alpha(uint8_t a);
    void lineWidth(float px);
    void pointSize(float px);
    void clip(int x, int y, int w, int h);
    void noClip();

    void fill(int x, int y, int w, int h);            // filled rectangle
    void fillRound(int x, int y, int w, int h, float radius);
    void frame(int x, int y, int w, int h);           // 1 px outline
    void line(float x0, float y0, float x1, float y1);
    void hline(int x, int y, int w) { fill(x, y, w, 1); }
    void vline(int x, int y, int h) { fill(x, y, 1, h); }
    void point(float x, float y, float radius);       // filled circle
    void ring(float x, float y, float radius, int segments = 24);
    void arc(float x, float y, float radius, float a0, float a1, int segments = 16);
    // Batch API: many lines / points / rects in one BEGIN..END (1 word each vertex).
    void beginLines();
    void beginPoints();
    void beginRects();
    void beginStrip();
    void vtx(float x, float y);
    void endBatch();

    void text(int x, int y, uint8_t fnt, const char* s, uint16_t opt = 0);
    void textf(int x, int y, uint8_t fnt, uint16_t opt, const char* fmt, ...) __attribute__((format(printf, 6, 7)));
    void number(int x, int y, uint8_t fnt, int32_t n, uint16_t opt = 0);
    // RGB332 bitmap already in RAM_G (IDisplay::writeRamG), scaled up by `scale`.
    void bitmap332(int x, int y, uint32_t ramg, int w, int h, int scale);

    static constexpr uint16_t kCenterX = IDisplay::kOptCenterX, kCenter = IDisplay::kOptCenter,
                              kRight = IDisplay::kOptRightX, kCenterY = IDisplay::kOptCenterY;

private:
    bool spend(uint32_t w);
    IDisplay* d_ = nullptr;
    uint32_t words_ = 0, dropped_ = 0;
    bool inBatch_ = false;
    uint32_t limit_ = kBudgetWords, clipped_ = 0;
};

// ---------------------------------------------------------------------------
// Input events after the Shell: buttons with press / release / hold, pads with
// velocity, the encoder. Hold fires once per button after kHoldMs.
struct Event {
    enum Type : uint8_t { kNone, kPress, kRelease, kHold, kPadDown, kPadUp, kEncoder };
    Type type = kNone;
    uint8_t id = 0;         // button id (ot::btn) or pad 0..47 (row * 12 + col)
    uint8_t velocity = 0;   // pads
    int8_t delta = 0;       // encoder
    bool shift = false;     // Shift held
    uint32_t ms = 0;
};
constexpr uint32_t kHoldMs = 500;
namespace btn = ot::btn;
inline int padRow(int pad) { return pad / kPadCols; }
inline int padCol(int pad) { return pad % kPadCols; }
inline int padAt(int col, int row) { return row * kPadCols + col; }

// ---------------------------------------------------------------------------
// Transport, as seen by render(): the beat position at the block start and the
// beats per sample (tempo / 60 / 44100). Workstations schedule their own events
// inside the block from these.
struct Transport {
    bool playing = false;
    double beat = 0.0;            // quarter notes since Play
    double beatsPerSample = 0.0;
    float bpm = 120.0f;
    bool startedThisBlock = false;
    bool stoppedThisBlock = false;
    // First sample of this block (0..kBlock-1) that reaches beat `b`, or -1.
    int sampleAt(double b) const {
        if (!playing || beatsPerSample <= 0.0 || b < beat) return -1;
        const double s = (b - beat) / beatsPerSample;
        return s < double(kBlock) ? int(s) : -1;
    }
};

// ---------------------------------------------------------------------------
// MIDI messages (parsed by the Shell from DIN and USB; running status handled).
struct MidiMsg {
    uint8_t status = 0, d1 = 0, d2 = 0;
    uint8_t type() const { return status & 0xF0; }
    uint8_t channel() const { return status & 0x0F; }
};

// Lock-free single-producer / single-consumer ring (audio -> main or main -> audio).
template <class T, uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "power of two");

public:
    bool push(const T& v) {
        const uint32_t h = head_, t = tail_;
        if (h - t >= N) return false;
        buf_[h & (N - 1)] = v;
        __atomic_store_n(&head_, h + 1, __ATOMIC_RELEASE);
        return true;
    }
    bool pop(T& v) {
        const uint32_t t = tail_, h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (t == h) return false;
        v = buf_[t & (N - 1)];
        __atomic_store_n(&tail_, t + 1, __ATOMIC_RELEASE);
        return true;
    }
    uint32_t size() const { return head_ - tail_; }

private:
    T buf_[N];
    volatile uint32_t head_ = 0, tail_ = 0;
};

// ---------------------------------------------------------------------------
// Settings rows. Values are ints; `names` (optional) labels each value.
struct SettingRow {
    const char* label = "";
    int* value = nullptr;
    int min = 0, max = 1;
    const char* const* names = nullptr;   // max - min + 1 entries, or null = number
    void (*onChange)(void* ctx, int v) = nullptr;
    void* ctx = nullptr;
    const char* unit = "";
};

// Audio in for render(): one interleaved stereo int16 block from the selected
// input (line in, radio or mic), or silence.
struct AudioIn {
    const int16_t* frames = nullptr;   // kBlock * 2, or null
    float l(uint32_t i) const { return frames ? frames[2 * i] * (1.0f / 32768.0f) : 0.0f; }
    float r(uint32_t i) const { return frames ? frames[2 * i + 1] * (1.0f / 32768.0f) : 0.0f; }
};

class Shell;

// LABKIT v2 (params.h, mod.h, nav.h, browser.h, keyboard.h): the shared
// parameter registry, modulation, macros, navigation (MAP, PARAM pads, HUD,
// HELP), the sample / file browser and the text entry. Everything lives in
// lab::kit; a workstation that registers nothing keeps working exactly as before.
namespace kit {
class Registry;
class Mod;
class Browser;
class Keyboard;
struct Kit;
struct PageInfo;
struct NavKeys;
using ParamId = int16_t;
}  // namespace kit

// Context handed to the workstation: everything it may use.
struct Context {
    Shell* shell = nullptr;
    Arena* arena = nullptr;
    IStorage* fs = nullptr;
    ot::IRadio* radio = nullptr;
    const Theme* theme = nullptr;
    // "/Lab/<FW>" -- the workstation's folder on the card (created at boot).
    const char* home = "";
    // Main loop only.
    uint32_t millis() const;
    void midiOut(const MidiMsg& m);             // queued; any context
    uint8_t midiChannel() const;                 // Settings > MIDI out channel, 0..15
    void midiNote(bool on, uint8_t note, uint8_t vel, uint8_t chOffset = 0);   // on midiChannel() + chOffset
    bool playing() const;
    float bpm() const;
    void setBpm(float bpm);
    void setPlaying(bool on);
    void toast(const char* msg);                 // a short message on screen
    void requestRedraw();
    // Stems for the recorder: render() writes up to kStems stereo stems per block
    // (call once per block per stem, before render returns). Ignored when not recording.
    static constexpr int kStems = 4;
    void stem(int index, const float* l, const float* r);
    // LABKIT v2 services (main loop; see WORKSTATION_GUIDE.md "LABKIT v2").
    kit::Registry& params();     // register parameters in init()
    kit::Mod& mod();             // LFOs, random, macros, routes, exported sources
    kit::Browser& browser();     // sample / file browser modal
    kit::Keyboard& keyboard();   // text entry modal
};

// ---------------------------------------------------------------------------
// A workstation: one firmware's instrument.
class Workstation {
public:
    virtual ~Workstation() = default;
    virtual const char* name() const = 0;        // short, upper case: "GRAF"
    virtual const char* tagline() const = 0;     // one line for About / the intro
    virtual const Theme& theme() const = 0;
    virtual bool init(Context& c) = 0;           // allocate, load state; false = fatal

    // Boot intro: drawn every frame for introMs() (any key skips it).
    virtual uint32_t introMs() const { return 2600; }
    virtual void drawIntro(Canvas& g, uint32_t t) = 0;

    virtual void onEvent(const Event& e) = 0;
    virtual void onMidi(const MidiMsg& m) { (void)m; }
    virtual void update(uint32_t ms) { (void)ms; }   // main loop, every pass
    virtual void draw(Canvas& g) = 0;                // ~40 frames a second
    virtual void leds(Rgb* pads) = 0;                // fill 48 pad colours (~30 Hz)

    // Audio interrupt: add (+=) into l/r (already zeroed), kBlock frames.
    virtual void render(const Transport& t, const AudioIn& in, float* l, float* r) = 0;
    virtual void onTransport(bool playing) { (void)playing; }   // main loop

    // Workstation rows on the Settings page.
    virtual int settings(SettingRow* rows, int cap) { (void)rows; (void)cap; return 0; }
    // Save state to the card (power off, Settings > Save, every few minutes).
    virtual void save() {}

    // CPU gate (make lab-gate, tests/lab/lab_bench.cpp under QEMU): named load
    // scenarios; benchSetup(i) puts the workstation in that state (voices on,
    // sequences running, effects on) right after init(). The worst block of each
    // must stay under the limit in LAB_GATE.md.
    virtual int benchScenarios() const { return 1; }
    virtual const char* benchName(int i) const { (void)i; return "idle"; }
    virtual void benchSetup(int i) { (void)i; }

    // ---- LABKIT v2 navigation (nav.h). All optional; the defaults keep a
    // workstation that registers no parameters exactly as it was.
    // Describe the pages (name, one-line help, a line per F-key); return the count.
    virtual int navPages(kit::PageInfo* out, int cap) { (void)out; (void)cap; return 0; }
    // The page on screen now (-1 = none / unknown) and a jump request from the MAP.
    virtual int navPage() const { return -1; }
    virtual void navGoto(int page) { (void)page; }
    // The MAP / PARAM pads focused a parameter (highlight it on your page if you like).
    virtual void navFocus(kit::ParamId id) { (void)id; }
    // A parameter's base value was changed by the Shell (pads, MAP, macro page, MIDI learn, load).
    virtual void navParamChanged(kit::ParamId id) { (void)id; }
    // Move the LABKIT keys away from keys this workstation needs (NAV.md).
    virtual void navKeys(kit::NavKeys& keys) { (void)keys; }
};

// Each firmware defines this (src/ws/).
Workstation* createWorkstation(Arena& arena);

// ---------------------------------------------------------------------------
// Board hooks the Shell needs (device glue / simulator fill these in).
struct Hal {
    IDisplay* display = nullptr;
    ot::IAudioOut* audio = nullptr;
    IStorage* fs = nullptr;
    ot::IMidiPort* midi = nullptr;
    ot::IInput* input = nullptr;
    ot::ILeds* leds = nullptr;
    ot::IClock* clock = nullptr;
    ot::IRadio* radio = nullptr;
    void (*service)() = nullptr;
    bool (*isUpdateImageName)(const char* name) = nullptr;
    bool (*requestFirmwareUpdate)(IStorage& fs, const char* name) = nullptr;
    const int16_t* (*lineIn)(void* ctx) = nullptr;
    void* lineInCtx = nullptr;
    bool (*selectInput)(void* ctx, uint8_t source, uint8_t gain) = nullptr;
    void* inputCtx = nullptr;
    void (*setBacklight)(uint8_t duty) = nullptr;
    // Audio render load of the last second, permille of the block budget (device: DWT).
    uint32_t (*cpuPermille)() = nullptr;
};

// ---------------------------------------------------------------------------
// The shell.
class Shell {
public:
    bool init(const Hal& hal, uint8_t* mem, uint32_t memBytes, const char* fwName);
    void loop();     // one main-loop pass
    [[noreturn]] void run();
    void powerOff(); // save everything (power switch hook)

    // Audio callback (IAudioOut::FillFn).
    static void fillThunk(int16_t* out, uint32_t frames, void* ctx);
    void fill(int16_t* out, uint32_t frames);

    Context& context() { return ctx_; }
    Workstation* workstation() { return ws_; }
    const Transport& transport() const { return tr_; }
    void setPlaying(bool on);
    void togglePlay() { setPlaying(!playing_); }
    bool playing() const { return playing_; }
    float bpm() const { return bpmX10_ / 10.0f; }
    void setBpm(float bpm);
    bool recording() const { return recOn_; }
    void toggleRecording();
    bool inSettings() const { return settingsOpen_; }
    uint32_t frames() const { return frameCount_; }
    uint32_t millis() const { return hal_.clock ? hal_.clock->millis() : 0; }
    uint8_t midiChannel() const { return uint8_t(set_.midiOutCh - 1); }
    uint32_t cpuPermille() const { return hal_.cpuPermille ? hal_.cpuPermille() : cpuEst_; }
    const char* fwName() const { return fwName_; }

    // Context services.
    void midiOut(const MidiMsg& m) { midiOutRing_.push(m); }
    void toast(const char* msg);
    void requestRedraw() { dirty_ = true; }
    void stem(int index, const float* l, const float* r);

    // Input source (Settings > Input): 0 line, 1 radio, 2 mic, 3 off.
    int inputSource() const { return set_.input; }
    float radioMHz() const { return set_.radioX10 / 10.0f; }

    // Headless tests / sim scripts.
    bool introRunning() const { return introOn_; }
    uint32_t recordedFrames() const { return recFrames_; }
    // Display-list words of the largest frame since the last reset, and frames
    // that hit the budget (sim "budget" check).
    uint32_t maxFrameWords() const { return maxWords_; }
    uint32_t overBudgetFrames() const { return overBudget_; }
    void resetFrameStats() { maxWords_ = 0; overBudget_ = 0; }

    // LABKIT v2 (nav.h): the kit (registry, mod engine, nav, browser, keyboard).
    kit::Kit& labkit() { return *kit_; }
    bool shiftHeld() const { return held_[btn::Shift]; }
    bool buttonHeld(uint8_t id) const { return id < btn::kCount && held_[id]; }
    // One LABKIT audio block (mod sources, routes, registry writes) -- fill() runs
    // it before render(); public for the CPU bench.
    void kitBlock(const Transport& t);

private:
    struct Settings {
        int version = 3;
        int brightness = 2;     // 0 low, 1 medium, 2 high
        int intro = 1;          // boot intro on / off
        int midiInCh = 0;       // 0 = omni, 1..16
        int midiOutCh = 1;      // 1..16
        int clockMode = 0;      // 0 internal, 1 internal + send clock, 2 follow MIDI clock
        int input = 0;          // 0 line, 1 radio, 2 mic, 3 off
        int inputGain = 60;     // 0..100
        int monitor = 0;        // input monitor through the output
        int radioX10 = 1000;    // 100.0 MHz
        int master = 80;        // 0..100
        int limiter = 1;
        int stems = 0;          // record stems as well
        int bpmX10 = 1200;
    };
    void handleInput();
    void dispatch(const Event& e);
    void settingsEvent(const Event& e);
    void drawFrame();
    void drawSettings(Canvas& g);
    void drawToast(Canvas& g);
    void drawStatus(Canvas& g);
    void updateLeds();
    void pumpMidi();
    void pumpRecorder();
    void startRecording();
    void stopRecording();
    void applyInput();
    void loadSettings();
    void saveSettings();
    void scanFirmware();
    int buildRows();

    Hal hal_;
    Arena arena_;
    Context ctx_;
    Canvas canvas_;
    Workstation* ws_ = nullptr;
    kit::Kit* kit_ = nullptr;
    uint64_t wsButtons_ = 0, wsPads_ = 0;   // presses the workstation received (their releases go there too)
    Settings set_;
    char fwName_[16] = {};
    char home_[32] = {};

    // transport
    Transport tr_;
    volatile bool playing_ = false, wantPlay_ = false;
    int bpmX10_ = 1200;
    uint32_t midiClockTicks_ = 0;
    double clockPhase_ = 0.0;           // 24 ppqn out
    uint32_t lastClockInUs_ = 0;
    float clockInBpm_ = 0.0f;
    bool clockFollowPlaying_ = false;

    // input
    bool held_[ot::btn::kCount] = {};
    uint32_t heldSince_[ot::btn::kCount] = {};
    bool holdSent_[ot::btn::kCount] = {};

    // ui
    bool introOn_ = true;
    uint32_t introStart_ = 0;
    bool settingsOpen_ = false;
    int settingsRow_ = 0, settingsTop_ = 0;
    int settingsPage_ = 0;   // 0 settings, 1 firmware update, 2 about
    bool dirty_ = true;
    uint32_t lastFrameMs_ = 0, lastLedMs_ = 0, lastSaveMs_ = 0, frameCount_ = 0;
    char toast_[64] = {};
    uint32_t toastUntil_ = 0;
    static constexpr int kMaxRows = 48;
    SettingRow rows_[kMaxRows];
    int rowCount_ = 0, wsRowStart_ = 0;
    static constexpr int kMaxFw = 12;
    char fwFiles_[kMaxFw][40] = {};
    int fwCount_ = 0, fwSel_ = 0;
    bool fwConfirm_ = false;
    Rgb pads_[kPads];

    // midi
    SpscRing<MidiMsg, 256> midiOutRing_;
    uint8_t midiRun_[2] = {}, midiData_[2][2] = {}, midiNeed_[2] = {}, midiHave_[2] = {};

    // audio
    float bufL_[kBlock], bufR_[kBlock];
    float limGain_ = 1.0f;
    float masterGain_ = 0.8f;
    volatile uint32_t blocks_ = 0;
    float peakL_ = 0, peakR_ = 0;
    uint32_t cpuEst_ = 0;       // host: render time of the last block vs its real time
    uint32_t maxWords_ = 0, overBudget_ = 0;
    int bpmRow_ = 120;

    // recorder: interleaved int16 rings in the arena, main loop writes WAV files.
    static constexpr uint32_t kRecRingFrames = 44100 * 2;   // 2 s per stream
    static constexpr int kRecStreams = 1 + Context::kStems;
    int16_t* recRing_[kRecStreams] = {};
    volatile uint32_t recHead_ = 0;      // frames written by audio (all streams together)
    uint32_t recTail_ = 0;               // frames written to the card
    volatile bool recOn_ = false;
    bool recStems_ = false;
    IStorage::Handle recFile_[kRecStreams] = {-1, -1, -1, -1, -1};
    uint32_t recFrames_ = 0, recDropped_ = 0;
    uint8_t stemWritten_ = 0;
    char recName_[64] = {};
};

// The F1..F8 label strip along the bottom edge (the eight buttons under the
// screen, 100 px apart). `labels` may hold nullptr / "" for unused keys;
// `active` highlights one key (-1 = none).
void drawFKeyBar(Canvas& g, const Theme& t, const char* const labels[8], int active = -1);
// A header bar: firmware name on the left, a page title, BPM / play / REC / CPU on the right.
void drawHeader(Canvas& g, const Theme& t, Shell& sh, const char* title);

// Small text helpers (no printf in render()).
void copyStr(char* dst, uint32_t cap, const char* src);
bool endsWithCi(const char* s, const char* suffix);

}  // namespace lab

// LABKIT v2 API (after the shell types it builds on).
#include "lab/params.h"
#include "lab/mod.h"
#include "lab/nav.h"
#include "lab/browser.h"
#include "lab/keyboard.h"
