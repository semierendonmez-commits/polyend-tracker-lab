// DATAMATICS -- the audio engine: 8 data streams, 4 data slots, sections.
//
// Threading: the main loop writes parameters (plain ints), data slots (n is set
// to 0 while a slot is rewritten) and request fields; render() (audio interrupt)
// reads them and writes the feedback fields (k, lastVal, events...). No locks.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/dm_data.h"

namespace dm {

constexpr int kStreams = 8;
constexpr int kSlots = 4;
constexpr int kMaxSections = 24;
constexpr int kVoices = 4;   // per stream

// Stream parameters (ints, edited in the main loop).
enum Param : uint8_t {
    P_SLOT, P_UNIT, P_ORDER, P_START, P_LEN, P_STRIDE, P_SYNC, P_RATE_MS, P_RATE_DIV, P_SOUND, P_THR,
    P_LO, P_HI, P_SCALE, P_DUR, P_DURMOD, P_SHAPE, P_LEVEL, P_VEL, P_PANMODE, P_PAN, P_STEM, P_MIDI,
    P_MUTE, P_SOLO, P_COUNT
};
enum Unit : uint8_t { U_BYTE, U_BIT };
enum Order : uint8_t { O_FWD, O_REV, O_PING, O_COL, O_HOP, O_COUNT };
enum Sound : uint8_t { S_SINE, S_CLICK, S_NOISE, S_IMPULSE, S_COUNT };
enum Scale : uint8_t { SC_FREE, SC_HARM, SC_OCT, SC_12TET, SC_COUNT };
enum Shape : uint8_t { SH_RECT, SH_FADE, SH_DECAY, SH_COUNT };
enum PanMode : uint8_t { PM_FIX, PM_DATA, PM_BIT, PM_ALT, PM_SWEEP, PM_COUNT };

struct ParamInfo {
    const char* name;   // short label
    int min, max, def;
    const char* const* names;   // value names or null
};
const ParamInfo& paramInfo(int p);
constexpr int kRateDivs = 14;
float rateDivBeats(int idx);          // beats per step
uint64_t beatsQ(uint32_t num, uint32_t den, float bpm);   // num/den beats in Q16 samples
const char* rateDivName(int idx);     // "1/16"

// Sections.
enum SecType : uint8_t { T_PLAY, T_SCAN, T_DENSIFY, T_THIN, T_SILENCE, T_CUT, T_FREEZE, T_COUNT };
const char* secTypeName(int t);       // "SCAN"
const char* secTypeShort(int t);      // "SCN"

struct Section {
    int type = T_PLAY;
    int bars = 4;       // 1..64
    int mask = 0xFF;    // active streams
    int depth = 50;     // 0..100
};

struct Slot {
    uint8_t* v = nullptr;
    uint8_t* aux = nullptr;
    volatile uint32_t n = 0;
    uint16_t width = 256;
    uint8_t mode = kRaw;
    uint16_t srcW = 0, srcH = 0;
    char path[128] = {};
    uint8_t wantMode = kAuto;   // mode requested (kAuto or forced)
};

struct Stream {
    int p[P_COUNT] = {};
    // feedback (audio writes)
    volatile uint32_t k = 0;          // step counter (events read so far)
    volatile uint32_t lastIdx = 0;    // unit index of the last read
    volatile uint8_t lastVal = 0, lastAux = 0;
    volatile uint32_t events = 0;     // sounding events
    volatile float lastHz = 0.0f, lastAmp = 0.0f;
    // requests (main writes, audio clears)
    volatile int32_t cue = -1;        // set k to this value
    volatile uint8_t audition = 0;    // fire one event now (velocity 1..127)
    volatile uint16_t audOff = 0;     // ... reading the unit k + audOff
};

// One sounding event, audio -> main (visuals, numbers).
struct EventRec {
    uint8_t stream, val, aux, sound;
    uint32_t idx;
    float hz;
};

// Pure helpers (main loop and audio).
uint32_t unitCount(const Slot& s, int unit);
uint32_t readIndex(const Stream& st, const Slot& slot, uint32_t k, uint32_t scanOffset);
void readValue(const Slot& slot, int unit, uint32_t idx, uint8_t& val, uint8_t& aux);
float mapFreq(const int* p, uint8_t val);

class Engine {
public:
    bool init(lab::Arena& a);
    void defaults();   // the built-in demo piece
    void reset() { wasPlaying_ = false; wantStop = false; }   // next playing block restarts the piece

    // Audio interrupt. stemL/stemR: 4 x kBlock each (written, not added).
    void render(const lab::Transport& t, float* l, float* r, float (*stemL)[lab::kBlock], float (*stemR)[lab::kBlock]);
    void bindMidi(lab::Context* c) { ctx_ = c; }

    Slot slot[kSlots];
    Stream st[kStreams];
    Section sec[kMaxSections];
    volatile int secCount = 7;
    int arrange = 1;        // follow the sections
    int endMode = 0;        // 0 loop, 1 stop at the end
    int midiMode = 0;       // 0 off, 1 one channel, 2 channel per stream
    int jumpQuant = 0;      // 0 next bar, 1 immediately

    // feedback
    volatile int curSec = 0;
    volatile float secProgress = 0.0f;
    volatile uint32_t clock = 0;       // samples since Play
    volatile uint32_t secBarsDone = 0;
    volatile uint8_t gateOpen = 1;     // CUT gate
    volatile bool wantStop = false;    // audio asks the main loop to stop (end of piece)
    volatile int jumpReq = -1;         // main: jump to section
    volatile int startSec = 0;         // section Play starts from
    lab::SpscRing<EventRec, 256> events;
    volatile uint32_t eventsLost = 0;

    // Effective step length in samples (Q16) for a stream at a tempo.
    uint64_t stepQ(int s, float bpm) const;
    bool streamAudible(int s) const;   // mute / solo
    bool anySolo() const;

    // What the UI previews: the effective threshold and the scan offset now.
    int thrFor(int s) const;
    uint32_t scanOffset(int s) const;
    bool sectionAllows(int s) const;
    // Total bars of the piece.
    int totalBars() const;

    // test hooks
    uint32_t voicesActive() const;
    uint64_t nextQ(int s) const { return next_[s]; }

private:
    struct Voice {
        uint8_t type = 0, shape = 0;
        int32_t left = 0;        // samples remaining (<0 = off), kHold = until retrigger
        int32_t total = 0, age = 0;
        uint32_t ph = 0, inc = 0;
        float gl = 0, gr = 0, env = 1.0f, k = 1.0f;
        float lp = 0, lpa = 1.0f, sign = 1.0f;
        int stem = 0;
    };
    static constexpr int32_t kHold = 0x7FFFFFFF;

    void startPlay();
    void enterSection(int i, uint32_t atSample);
    void fire(int s, int at, float velScale);
    void renderVoices(int s, int a, int b);
    void killStream(int s);
    void killAll();
    void midiOff(int s);
    float panFor(int s, uint8_t val, uint8_t aux, uint32_t k);

    lab::Context* ctx_ = nullptr;
    Voice v_[kStreams][kVoices];
    uint8_t rr_[kStreams] = {};
    uint64_t next_[kStreams] = {};
    uint64_t now_ = 0;          // Q16 sample clock at the block start
    uint64_t secStartQ_ = 0, secEndQ_ = 0;
    uint64_t cutNextQ_ = 0;
    uint32_t cutK_ = 0;
    uint32_t freezeK_[kStreams] = {};
    uint8_t lastNote_[kStreams] = {};
    uint8_t altPan_[kStreams] = {};
    bool wasPlaying_ = false;
    float bpm_ = 120.0f;
    float* bufL_ = nullptr;   // per-stem scratch pointers (set per block)
    float (*stL_)[lab::kBlock] = nullptr;
    float (*stR_)[lab::kBlock] = nullptr;
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace dm
