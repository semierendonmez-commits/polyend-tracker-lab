// KONSOL sound engines: one Engine per track (any of ten kinds). Every engine
// is driven only from the audio context (noteOn / noteOff / render); the main
// loop reads and writes parameters (plain floats, picked up at the next block).
#pragma once

#include <cstdint>

#include "audio/synth_engine.h"
#include "lab/lab.h"
#include "model/synth_patch.h"
#include "ws/samples.h"

namespace lab {
namespace konsol {

enum EngineKind : uint8_t {
    kEngSampler = 0,
    kEngGrain,
    kEngDrums,
    kEngIkeda,
    kEngMakam,
    kEngAcd,
    kEngVap,
    kEngFat,
    kEngWtfm,
    kEngPerc,
    kEngPlaits,     // Mutable Instruments Plaits, 24 models
    kEngFloater,    // FLOATER kick / snare / perc
    kEngTb303,      // Open303
    kEngNeuro,      // NEUROSTACK neuro bass
    kEngQuanta,     // QUANTA microsound particles
    kEngKit,        // 16-pad sample kit
    kEngCount
};
const char* engineName(int k);      // "SAMPLER", ...
const char* engineInfo(int k);      // one line

// Parameter display formats of the KONSOL-native engines.
enum PFmt : uint8_t { kFPct, kFNum, kFHz, kFMs, kFSemi, kFList, kFdB, kFInt, kFBi };
struct SecDesc {
    const char* name;
    uint8_t first, count;
};
struct PDesc {
    const char* name;
    float min, max, def, step;
    uint8_t fmt;
    const char* const* labels;
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual int kind() const = 0;
    // --- parameters (main loop) ---------------------------------------------
    virtual int sections() const { return 1; }
    virtual const char* sectionName(int s) const { (void)s; return "Main"; }
    virtual int sectionParams(int s, uint8_t* out, int cap) const;   // param indices of section s
    virtual int paramCount() const = 0;
    virtual void paramName(int i, char* out, int cap) const = 0;
    virtual float get(int i) const = 0;
    virtual void set(int i, float v) = 0;
    virtual void stepParam(int i, int d, bool coarse) = 0;
    virtual void format(int i, char* out, int cap) const = 0;
    virtual float frac(int i) const = 0;   // 0..1 for the value bar
    virtual void reset() = 0;              // every parameter to its default
    // Musical default for a fresh project (may differ from reset()).
    virtual void preset(int variant) { (void)variant; reset(); }
    // --- audio context -------------------------------------------------------
    // offset: frames into the current block (engines without sample-accurate
    // starts may ignore it). tone: per-note brightness lock -64..63.
    // flags: kNoteSlide / kNoteReverse (model.h NoteFlag values).
    virtual void noteOn(int pitch, float vel, int offset, int tone, int flags) = 0;
    virtual void noteOff(int pitch) = 0;
    virtual void allOff(bool hard) = 0;
    virtual void render(float* l, float* r) = 0;   // adds kBlock frames
    virtual int active() const = 0;                  // sounding voices
    virtual int32_t cost() const = 0;                // est. M4 insn per block now
    virtual int32_t noteCost() const = 0;            // added by one more note
    virtual bool steal() = 0;                        // drop the cheapest-to-lose voice
    virtual void setTempo(float bpm) { (void)bpm; }
};

// Builds every engine of one track from the arena (all kinds exist side by
// side; the track switches a pointer). False when the arena is exhausted.
struct TrackEngines {
    Engine* e[kEngCount] = {};
    bool init(Arena& a, SamplePool* pool, TrackSample* sample, int track);
};

// Makam helpers (the generators get the makam's 12-TET mask).
constexpr int kMakamCount = 8;
const char* makamName(int m);
uint16_t makamMask(int m);          // root-relative pitch classes

}  // namespace konsol
}  // namespace lab
