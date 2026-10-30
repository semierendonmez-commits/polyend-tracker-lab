// BULUT -- a granular "cloud" production workstation for the original Polyend
// Tracker (polyend-tracker-lab). "Bulut" is Turkish for cloud.
//
// Round 2: four deep channels ("clouds"). Each channel has a source (line in /
// radio, BULUT's own output, a sample, or a generator), one of four engines
// (GRANULAR, QUANTA, BEADS, TAPE) on twelve engine knobs, an insert chain
// (drive, filter, one FX mode: Clouds texture / pitch shifter or an artifex
// mode), its own LFO, a freeze bed, a gesture loop and its mixer strip.
//
// Every user-facing value is a LABKIT registry parameter (params.cpp); the
// structs here hold the bound fields the DSP reads, plus the state that is not
// a parameter (switches, paths, gestures, scenes, the freeze library).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace bulut {

constexpr int kCh = 4;                                  // channels (clouds)
constexpr uint32_t kBufBits = 19;                       // 524,288 samples = 11.9 s per channel
constexpr uint32_t kBufLen = 1u << kBufBits, kBufMask = kBufLen - 1;
constexpr uint32_t kFrzBits = 16;                       // spectral freeze loop: 65,536 samples (1.49 s)
constexpr uint32_t kFrzLen = 1u << kFrzBits, kFrzMask = kFrzLen - 1;
constexpr int kEmitters = 8;                            // per channel: 0..3 pads / MIDI, 4..7 gesture ghosts
constexpr int kLiveEmitters = 4;
constexpr int kGestEvents = 1024;
constexpr int kScenes = 8, kTimeline = 32, kFreezeSlots = 16, kProjects = 16;
constexpr int kKnobs = 12;                              // engine knobs per channel
constexpr int kChParams = 36;                           // registered params per channel (one PARAM-pads page)
constexpr int kSpecBins = 2048;                         // stored freeze spectrum (4096-point analysis)
constexpr int kStemCount = 4;
constexpr float kSr = 44100.0f;

enum EngineKind : uint8_t { kGranular = 0, kQuanta, kBeads, kTape, kEngines };
// kSrcTape (TAPE FILE, stream.h) came last, so saved indices keep their meaning.
enum SrcType : uint8_t { kSrcInput = 0, kSrcOutput, kSrcSample, kSrcGen, kSrcTape, kSrcCount };
constexpr int kSrcCountV1 = 4;   // sources before TAPE FILE (scene values of older projects)
enum GenType : uint8_t { kGenRete = 0, kGenTone, kGenNoise, kGenCount };
enum QType : uint8_t { kQSample = 0, kQGlisson, kQGabor, kQGrainlet, kQPulsar, kQTrainlet, kQTypes };
enum QLaw : uint8_t { kLawSync = 0, kLawPoisson, kLawGauss, kLawCauchy, kLawLogistic, kLaws };
enum Scale : uint8_t { kScaleFree = 0, kScaleOct, kScaleFifth, kScaleMajor, kScaleMinor, kScalePent, kScales };
enum Media : uint8_t { kMediaHifi = 0, kMediaCleanTape, kMediaLofi, kMediaDirtyTape, kMedias };
enum FltMode : uint8_t { kFltOff = 0, kFltLp, kFltBp, kFltHp, kFltModes };
enum FxMode : uint8_t {
    kFxOff = 0,
    kFxTexture,    // Clouds diffuser
    kFxShifter,    // Clouds pitch shifter
    kFxFlanger,    // artifex modes (float ports)
    kFxPanner,
    kFxCrusher,
    kFxSlicer,
    kFxFreezer,
    kFxModes
};
enum LfoShape : uint8_t { kLfoSine = 0, kLfoTri, kLfoSaw, kLfoSquare, kLfoSh, kLfoDrift, kLfoShapes };
enum FreezeFrom : uint8_t { kFromSource = 0, kFromMix, kFroms };

// Engine knob slots, by engine (k index 0..11). The slots line up where the
// engines share an idea (position, size, density, pitch, spread, shape,
// scatter, feedback), so a route or a gesture on a knob keeps its sense when
// the engine changes.
namespace K {
enum : uint8_t {
    Pos = 0, Size, Dens, Pitch, Spread, Shape, Scatter, Feedback, Mode, Rand, Scale, Extra,
};
}

// ---- fast float math (the audio interrupt avoids the libm calls) ------------
// 2^x: the nearest integer goes into the exponent, the fraction (-0.5..0.5)
// through a degree-5 polynomial; relative error < 3e-6 (0.00005 semitone).
inline float fexp2(float x) {
    if (x < -125.0f) return 0.0f;
    if (x > 125.0f) x = 125.0f;
    const int i = int(x + (x >= 0.0f ? 0.5f : -0.5f));
    const float f = x - float(i);
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union {
        uint32_t u;
        float v;
    } b;
    b.u = uint32_t(i + 127) << 23;
    return p * b.v;
}
// log2(x) for x > 0, absolute error < 1e-5.
inline float flog2(float x) {
    union {
        float v;
        uint32_t u;
    } b;
    b.v = x;
    const int e = int((b.u >> 23) & 255u) - 127;
    b.u = (b.u & 0x7FFFFFu) | 0x3F800000u;   // mantissa 1..2
    const float t = (b.v - 1.0f) / (b.v + 1.0f);
    const float t2 = t * t;
    return float(e) + 2.8853901f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * 0.14285714f)));
}
inline float fexp(float x) { return fexp2(x * 1.44269504f); }

// ---- parameter scaling (engine and UI use the same curves) -----------------
inline float sizeSec(float v) { return 0.01f * fexp(4.6051702f * v); }     // 10 ms .. 1 s
inline float densHz(float v) { return 0.5f * fexp(5.4806389f * v); }       // 0.5 .. 120 grains/s
inline float pitchSemi(float v) { return (v - 0.5f) * 48.0f; }              // -24 .. +24
inline float rateHz(float v) { return fexp(7.3132204f * v); }               // 1 .. 1500 particles/s
inline float durSec(float v) { return 0.0005f * fexp(4.7874917f * v); }    // 0.5 .. 60 ms
inline float freqHz(float v) { return 30.0f * fexp(5.5857486f * v); }      // 30 Hz .. 8 kHz
inline float bandOct(float v) { return 3.0f * v; }                          // 0 .. 3 octaves
inline float tapeGrainSec(float v) { return 0.02f * fexp(3.2188758f * v); } // 20 ms .. 500 ms
// TAPE speed: -2 .. +2 x, centre = stopped (scrub), detents at -1 x and +1 x.
inline float tapeSpeed(float v) {
    float s = (v - 0.5f) * 4.0f;
    if (s > 0.92f && s < 1.08f) s = 1.0f;
    if (s < -0.92f && s > -1.08f) s = -1.0f;
    if (s > -0.03f && s < 0.03f) s = 0.0f;
    return s;
}
inline int enumIndex(float v, int n) {
    const int i = int(v * float(n - 1) + 0.5f);
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}
inline float enumValue(int i, int n) { return n > 1 ? float(i) / float(n - 1) : 0.0f; }
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---- per-channel registry-bound fields (the DSP reads them) -----------------
// The registry writes the effective value (base + modulation) once per audio
// block. Engine knobs are 0..1 and the engine maps them itself.
struct ChParams {
    float k[kKnobs];        // engine knobs (meaning per engine, ui_text.h)
    // source + region + generator + bed
    float gain = 1.0f;      // live input level into the buffer (0..2)
    float regStart = 0.0f;  // grain region in the source, 0..1
    float regEnd = 1.0f;
    float genA = 0.5f;      // generator: pitch (TONE), colour (NOISE), leak (RETE)
    float genB = 0.5f;      // generator: shape (TONE), density (NOISE), scale (RETE)
    float bed = 0.5f;       // freeze bed level
    // channel LFO
    float lfoRate = 0.5f;   // Hz (exp 0.02..20)
    int lfoShape = kLfoSine;
    // mixer
    float level = 0.7f, pan = 0.0f;   // pan -1..1
    float dly = 0.1f, rev = 0.3f;     // sends 0..1
    // insert chain
    float drive = 0.0f;     // 0..1
    int fltMode = kFltOff;
    float cutoff = 2000.0f; // Hz
    float reso = 0.2f;      // 0..1
    int fxMode = kFxOff;
    float fxTime = 0.5f, fxAmt = 0.5f, fxFb = 0.0f;
    // switches that are also params (noMod)
    int engine = kGranular;
    int source = kSrcInput;
    int genType = kGenRete;
    float gestSpeed = 2.0f; // index into speedMul (int-like float)
    // TAPE FILE (registry page TAPE, not part of the 36 channel params / scenes)
    float tapeSpeed = 1.0f; // varispeed 0.25 .. 2 x (pitch follows)
    float tapeFrom = 0.0f;  // start / loop point, 0..1 of the file
    int tapeLoop = 1;       // at the end: back to FROM (1) or stop (0)
};

// ---- per-channel state that is not a parameter ------------------------------
struct ChState {
    uint8_t drone;         // the cloud breathes on its own
    uint8_t freeze;        // grains read the spectral freeze loop
    uint8_t freezeSlot;    // library slot shown in the loop (0xFF = none)
    uint8_t mute;
    uint8_t solo;
    uint8_t stem;          // 0..3
    uint8_t gestOn;        // gesture loop plays
    uint8_t hold;          // live buffer stopped (held)
    float engMem[kEngines][kKnobs];   // every engine's knobs (the active one mirrors the registry)
};
void defaultEngineKnobs(int engine, float* k, int channel);

// What feeds a channel besides the source param (paths are not params).
struct ChSource {
    char path[128];        // SAMPLE: the WAV on the card ("" = none / not saved)
    char label[40];        // what the channel shows ("kick.wav", "RESAMPLE MIX 003")
};

// ---- gestures: recorded movements that loop ---------------------------------
enum GestType : uint8_t { kGPadDown = 1, kGPadUp = 2, kGParam = 3 };
struct GestEvent {
    uint32_t t;            // samples from the loop start
    uint8_t type;
    uint8_t a;             // pad 0..47, or channel param index 0..35
    uint16_t v;            // velocity 0..127, or param value 0..65535 (normalized)
};
enum GestQuant : uint8_t { kQuantOff = 0, kQuantBeat, kQuantBar, kQuants };
constexpr int kSpeeds = 5;   // 1/4 1/2 1 2 4
inline float speedMul(int i) {
    static const float k[kSpeeds] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    return k[i < 0 ? 0 : (i >= kSpeeds ? kSpeeds - 1 : i)];
}
struct Gesture {
    uint32_t len;          // samples (0 = empty)
    uint16_t count;
    uint8_t quant;
    uint8_t speed;         // unused since round 2 (the GEST SPEED param); kept for the file format
    GestEvent ev[kGestEvents];
};

// ---- arrangement -----------------------------------------------------------
constexpr int kMasterParams = 8;   // send FX + morph time (registry page CLOUDS / MOTION)
struct Scene {
    uint8_t used;
    uint8_t reserved[3];
    float norm[kCh][kChParams];     // registry normalized values of every channel param
    float master[kMasterParams];
    ChState st[kCh];
    Gesture gest[kCh];
};
struct TimeEntry {
    uint8_t scene;
    uint8_t bars;
};

// ---- the library of frozen moments ------------------------------------------
struct FreezeSlot {
    uint8_t used;
    uint8_t cloud;         // channel it was taken from (0..3)
    uint8_t from;          // FreezeFrom
    uint8_t reserved;
    float scale;           // magnitude of 65535
    uint16_t mag[kSpecBins];
};

// ---- TAPE FILE: what a project remembers of each channel's tape -------------
// (speed, FROM and LOOP are registry params: the REGC chunk holds them)
struct TapeSave {
    char path[128];        // the user's WAV ("" = none)
    char label[40];
    uint8_t playing;       // it was playing: it plays again once the file is open
    uint8_t reserved[3];
    uint32_t cue;          // the position (file frames at 44.1 kHz)
};

// ---- master send effects (registry-bound) ------------------------------------
struct FxState {
    int dlyDiv = 5;
    float dlyFb = 0.45f, dlyTone = 0.55f, dlyRet = 0.7f;
    float revSize = 0.75f, revDamp = 0.45f, revRet = 0.8f;
    float morphSec = 1.0f;   // scene morph time
};
constexpr int kDlyDivs = 9;
const char* dlyDivName(int i);
float dlyDivBeats(int i);

// ---- spectral tools (registry-bound, applied when a freeze is recalled) ------
struct SpecTools {
    float blur = 0.0f;     // smooth the magnitudes (0..1)
    float shift = 0.0f;    // bin shift in semitones (-24..24)
    float tilt = 0.0f;     // -1 dark .. +1 bright
    float sparse = 0.0f;   // keep only the peaks (0..1)
    float morph = 0.0f;    // towards the morph slot (0..1)
    int morphSlot = 0;     // 0..15
};

}  // namespace bulut
