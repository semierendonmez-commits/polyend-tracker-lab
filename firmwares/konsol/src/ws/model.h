// KONSOL data model: tracks, clips (notes + generator settings), scenes, the
// arrangement, the mixer, and the project file format.
//
// Clip notes are double-buffered: the audio context reads buf[cur]; the main
// loop edits a copy in buf[cur ^ 1] and publishes it by flipping `cur` (one
// byte store). On the device the audio block is an interrupt that never runs
// concurrently with itself, so it always sees a complete buffer. A live
// generator fills the back buffer and sets `pending`; the audio flips it at
// the clip's loop point.
#pragma once

#include <cstdint>

#include "lab/lab.h"

namespace lab {
namespace konsol {

constexpr int kTracks = 8;
constexpr int kScenes = 16;
constexpr int kMaxNotes = 128;
constexpr int kPpq = 48;                 // ticks per quarter note
constexpr int kTps = 12;                 // ticks per step (1/16)
constexpr int kMaxSteps = 128;           // 8 bars of 16ths
constexpr int kSongMax = 64;             // arrangement entries
constexpr int kStems = 4;

enum NoteFlag : uint8_t { kFlagSlide = 1, kFlagReverse = 2, kFlagAccent = 4 };

struct Note {
    uint16_t start = 0;   // ticks from the clip start
    uint16_t len = kTps;  // ticks
    uint8_t pitch = 60;
    uint8_t vel = 100;    // 1..127
    int8_t tone = 0;      // brightness lock -64..63 (0 = none)
    uint8_t chance = 100; // 0..100 %
    uint8_t roll = 0;     // hits per step (0/1 = none, 2..16)
    uint8_t flags = 0;
    uint8_t cond = 0;     // trig condition (kCond*: A:B loop counts, Fill, First)
};

// Trig conditions (after the Elektron A:B idea): a note plays only on some
// loops of its clip, only while Fill is held, or only on the first loop.
enum TrigCond : uint8_t {
    kCondAlways = 0, kCond12, kCond22, kCond13, kCond23, kCond33, kCond14, kCond24, kCond34, kCond44,
    kCondFill, kCondNotFill, kCondFirst, kCondNotFirst, kCondCount
};
const char* condName(int c);
bool condPasses(int cond, uint32_t loop, bool fill);

struct NoteBuf {
    Note n[kMaxNotes];
    uint16_t count = 0;
    void clear() { count = 0; }
    bool add(const Note& x);
    void sort();
    void remove(int i);
    int find(int startTick, int pitch) const;              // note starting at tick with pitch, -1
    int at(int tick, int pitch) const;                      // note covering tick at pitch, -1
};

// Player / generator kinds of a clip.
enum GenKind : uint8_t {
    kGenNotes = 0,   // the notes as written (steps, piano roll, recording)
    kGenAcid,
    kGenCallResp,
    kGenBBCut,
    kGenTurnaround,
    kGenGenotype,
    kGenStation,
    kGenEuclid,
    kGenUsul,
    kGenPinball,
    kGenMarkov,
    kGenCount
};
constexpr int kGenParams = 6;

struct Clip {
    NoteBuf buf[2];
    volatile uint8_t cur = 0;
    volatile uint8_t pending = 0;   // buf[cur ^ 1] holds the next loop (live generator)
    uint8_t lenSteps = 0;           // 0 = empty slot
    uint8_t gen = kGenNotes;
    int16_t gp[kGenParams] = {};
    uint8_t live = 0;               // generator runs every `every` loops
    uint8_t every = 1;
    uint8_t launchQ = 0;            // 0 = the project's launch quantise, else launchQ + 1
    bool exists() const { return lenSteps > 0; }
    const NoteBuf& notes() const { return buf[cur]; }
    // Main loop: start an edit (copy of the live notes, cancels a pending loop)
    // and publish it.
    NoteBuf& beginEdit();
    void commit();
};

struct Mix {
    float level = 0.8f;   // 0..1.25 (fader)
    float pan = 0.0f;     // -1..1
    float sendA = 0.0f;   // delay 0..1
    float sendB = 0.0f;   // reverb 0..1
    uint8_t mute = 0, solo = 0;
    uint8_t stem = 0;     // 0..3 stem group, 4 = none
    uint8_t midiOut = 0;  // send this track's notes on MIDI (channel + track)
};

struct TrackCfg {
    uint8_t engine = 0;   // EngineKind
    uint8_t octave = 4;   // generator base octave (root = 12 * octave + key)
    Mix mix;
    uint8_t midiCh = 0;   // MIDI in: 0 = plays when selected (any channel), 1..16 = that channel
};

struct SongEntry {
    uint8_t scene = 0;
    uint8_t bars = 4;
};

struct FxCfg {
    uint8_t delaySync = 3;     // index into the sync table (1/16 .. 1/2.)
    float delayFb = 0.45f;
    float delayTone = 0.6f;    // 0..1 -> filter
    uint8_t pingPong = 1;
    float revSize = 0.7f, revDamp = 0.5f;
    float retA = 0.8f, retB = 0.8f;
    float compThr = 0.6f;      // 0..1 -> -30..0 dB
    float compRatio = 0.2f;    // 0..1 -> 1:1 .. 10:1
    float compMakeup = 0.15f;  // 0..1 -> 0..+12 dB
    uint8_t limiter = 1;
    float master = 0.9f;
    uint8_t revType = 0;       // 0 Plate (OpenTracker), 1 Hall, 2 Shimmer, 3 Gated
    uint8_t dlyType = 0;       // 0 Digital, 1 Tape, 2 Lo-fi
    float revPre = 0.0f;       // pre-delay 0..1 -> 0..120 ms (Hall / Shimmer / Gated)
    float dlyMod = 0.3f;       // tape wow / lo-fi crush amount
};
constexpr int kRevTypes = 4, kDlyTypes = 3;
const char* revTypeName(int t);
const char* dlyTypeName(int t);

struct Project {
    char name[16] = "SONG01";
    TrackCfg track[kTracks];
    Clip clip[kTracks][kScenes];
    SongEntry song[kSongMax];
    uint8_t songLen = 0;
    uint8_t songLoop = 1;
    uint8_t key = 9;           // A
    uint8_t scale = 1;         // minor
    uint8_t swing = 50;        // 50..75 %
    uint8_t launchQ = 2;       // 0 none, 1 beat, 2 bar, 3 2 bars, 4 4 bars
    uint8_t recQ = 1;          // 0 off, 1 1/16, 2 1/8, 3 1/32
    uint8_t metronome = 0;
    uint8_t keyLayout = 0;     // pads as a keyboard: 0 chromatic rows, 1 in key (scale lock)
    uint8_t recHits = 1;       // drum-like engines record hits (no note-off needed)
    FxCfg fx;
    float bpm = 120.0f;
};

// Scales (generator key + piano roll highlight).
constexpr int kScaleCount = 12;
const char* scaleName(int s);
uint16_t scaleMaskOf(int s, int trackEngineMakam);   // makam scale uses the makam engine's mask
const char* noteText(int note, char out[6]);           // "C#3"
const char* genName(int k);
const char* genInfo(int k);
const char* const* launchQNames();
const char* const* recQNames();
int launchQBeats(int q);   // 0 = immediate

}  // namespace konsol
}  // namespace lab
