// BULUT -- a granular "cloud" production workstation for the original Polyend
// Tracker (polyend-tracker-lab). "Bulut" is Turkish for cloud.
//
// Shared types and constants of the workstation: clouds, parameters, gestures,
// scenes, the freeze library. Everything here is plain data (POD) so scenes
// and projects can copy and save it byte for byte.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace bulut {

constexpr int kClouds = 6;
constexpr uint32_t kBufBits = 19;                       // 524,288 samples = 11.9 s per cloud
constexpr uint32_t kBufLen = 1u << kBufBits, kBufMask = kBufLen - 1;
constexpr uint32_t kFrzBits = 16;                       // spectral freeze loop: 65,536 samples (1.49 s)
constexpr uint32_t kFrzLen = 1u << kFrzBits, kFrzMask = kFrzLen - 1;
constexpr int kEmitters = 8;                            // per cloud: 0..3 pads / MIDI (main loop), 4..7 gesture ghosts (audio)
constexpr int kLiveEmitters = 4;
constexpr int kGestEvents = 1024;
constexpr int kScenes = 8, kTimeline = 32, kFreezeSlots = 16, kProjects = 16;
constexpr int kParams = 16;
constexpr int kSpecBins = 2048;                         // stored freeze spectrum (4096-point analysis)
constexpr int kPeakChunks = 128;                        // waveform overview of a cloud buffer (4096 samples each)
constexpr int kStemCount = 4;
constexpr float kSr = 44100.0f;

// Grain cost units (quarter grains) for the global CPU budget.
constexpr int kCostUnit = 4;

enum EngineKind : uint8_t { kGranular = 0, kQuanta = 1 };
enum SrcType : uint8_t { kSrcLiveIn = 0, kSrcLiveOut = 1, kSrcSample = 2, kSrcCount = 3 };
enum QType : uint8_t { kQSample = 0, kQGlisson, kQGabor, kQPulsar, kQTrainlet, kQTypes };
enum QLaw : uint8_t { kLawSync = 0, kLawPoisson, kLawGauss, kLawCauchy, kLawLogistic, kLaws };
enum Scale : uint8_t { kScaleFree = 0, kScaleOct, kScaleFifth, kScaleMajor, kScaleMinor, kScalePent, kScales };
enum FreezeFrom : uint8_t { kFromSource = 0, kFromMix, kFroms };

// Parameter slots. Each cloud keeps one set per engine; the shared slots
// (reverb, engine, level, delay, scale, tone) are mirrored in both sets.
namespace P {
enum : uint8_t {
    // granular (Beads / Clouds style)
    Pos = 0, Size = 1, Dens = 2, Pitch = 3, Spread = 4, Texture = 5, Feedback = 6,
    Reverb = 7, Engine = 8, Jitter = 9, PRand = 10, Reverse = 11, Level = 12, Delay = 13, Scale = 14, Tone = 15,
    // quanta (microsound) -- same slot numbers
    QType = 0, QRate = 1, QDur = 2, QFreq = 3, QBand = 4, QLaw = 5, QChar = 6,
    QPos = 9, QDrift = 10, QSpread = 11,
};
}
inline bool sharedParam(int i) { return i == P::Reverb || i == P::Engine || i == P::Level || i == P::Delay || i == P::Scale || i == P::Tone; }

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
inline int enumIndex(float v, int n) {
    const int i = int(v * float(n - 1) + 0.5f);
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}
inline float enumValue(int i, int n) { return n > 1 ? float(i) / float(n - 1) : 0.0f; }
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---- per-cloud state that scenes snapshot and projects save ----------------
struct CloudState {
    float p[2][kParams];   // [engine][slot], 0..1 normalised
    float pan;             // 0..1 (mixer balance)
    float bed;             // freeze bed level 0..1
    uint8_t drone;         // the cloud breathes on its own (param emitter)
    uint8_t freeze;        // grains read the spectral freeze loop
    uint8_t freezeSlot;    // library slot shown in the loop (0xFF = none)
    uint8_t mute;
    uint8_t stem;          // 0..3
    uint8_t gestOn;        // gesture loop plays
    uint8_t reserved[2];
    int engine() const { return p[0][P::Engine] > 0.5f ? kQuanta : kGranular; }
    float& at(int slot) { return p[engine()][slot]; }
    float get(int slot) const { return p[engine()][slot]; }
    void set(int eng, int slot, float v) {
        v = clamp01(v);
        if (sharedParam(slot)) {
            p[0][slot] = v;
            p[1][slot] = v;
        } else {
            p[eng & 1][slot] = v;
        }
    }
};
void defaultCloud(CloudState& c, int index);

// What feeds a cloud (not part of scenes).
struct CloudSource {
    uint8_t type;          // SrcType
    uint8_t hold;          // stop writing the live buffer (buffer freeze)
    uint8_t reserved[2];
    float inGain;          // live input level into the buffer
    char path[96];         // SAMPLE: the WAV on the card
};

// ---- gestures: recorded movements that loop ---------------------------------
enum GestType : uint8_t { kGPadDown = 1, kGPadUp = 2, kGParam = 3 };
struct GestEvent {
    uint32_t t;            // samples from the loop start
    uint8_t type;
    uint8_t a;             // pad 0..47, or param slot | engine << 4
    uint16_t v;            // velocity 0..127, or param value 0..65535
};
enum GestQuant : uint8_t { kQuantOff = 0, kQuantBeat, kQuantBar, kQuants };
constexpr int kSpeeds = 5;   // 1/4 1/2 1 2 4
inline float speedMul(int i) { static const float k[kSpeeds] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f}; return k[i < 0 ? 0 : (i >= kSpeeds ? kSpeeds - 1 : i)]; }
struct Gesture {
    uint32_t len;          // samples (0 = empty)
    uint16_t count;
    uint8_t quant;
    uint8_t speed;         // index into speedMul, 2 = x1
    GestEvent ev[kGestEvents];
};

// ---- arrangement -----------------------------------------------------------
struct Scene {
    uint8_t used;
    uint8_t reserved[3];
    CloudState cloud[kClouds];
    Gesture gest[kClouds];
};
struct TimeEntry {
    uint8_t scene;
    uint8_t bars;
};

// ---- the library of frozen moments ------------------------------------------
struct FreezeSlot {
    uint8_t used;
    uint8_t cloud;         // cloud it was taken from (0..5)
    uint8_t from;          // FreezeFrom
    uint8_t reserved;
    float scale;           // magnitude of 65535
    uint16_t mag[kSpecBins];
};

// ---- master effects ---------------------------------------------------------
struct FxState {
    float dlyDiv;          // 0..1 -> division index
    float dlyFb, dlyTone, dlyRet;
    float revSize, revDamp, revRet;
    float reserved;
};
constexpr int kDlyDivs = 9;
const char* dlyDivName(int i);
float dlyDivBeats(int i);
void defaultFx(FxState& f);

}  // namespace bulut
