// MAKAM -- the project (song) data model and its versioned file format.
//
// Every editable value is an `int` so the UI rows can point at it; the file
// stores each one as int16 (little endian) in a fixed visit order. The
// main loop writes these fields, the audio interrupt reads them (single
// words: a value changes between two blocks, never half-way).
#pragma once

#include <cstddef>
#include <cstdint>

namespace makam {

constexpr int kSteps = 12;         // max usul length (one pad row)
constexpr int kVars = 4;           // break / bass variations A..D
constexpr int kPhrases = 4;        // lead phrases A..D
constexpr int kPhraseMax = 48;     // 4 cycles x 12 units (the 48 pads)
constexpr int kSections = 16;
constexpr int kChannels = 4;       // Break, Bass, Lead, FX return
constexpr int kKits = 3;           // built-in synthesised breaks
constexpr int kSlices = 16;

// Note step flags (bass patterns and lead phrases).
enum : int { kNoteOn = 1, kNoteTie = 2, kNoteGlide = 4, kNoteGrace = 8, kNoteAccent = 16 };

struct BreakStep {
    int stroke = 0;    // Stroke (usul.h)
    int slice = -1;    // -1 = auto from the stroke, else 0..15
    int roll = 1;      // 1..4 hits in the step
    int pitch = 0;     // -12..12 semitones
    int rev = 0;       // reverse
    int stutter = 0;   // loop the first 1/4 of the slice
    int choke = 0;     // on a rest: stop the playing slice
};
struct BreakVar {
    BreakStep s[kSteps];
};
struct NoteStep {
    int deg = 0;       // makam degree (0 = karar, 7 = octave/tiz durak, -1 = yeden)
    int flags = 0;     // 0 = rest, kNoteOn | ..., kNoteTie = hold
};
struct BassPat {
    NoteStep s[kSteps];
};
struct Phrase {
    int cycles = 4;    // 1..4 usul cycles
    NoteStep s[kPhraseMax];
};
enum SectionType : int { kSecIntro, kSecBuild, kSecDrop, kSecBreak, kSecTaksim, kSecDrop2, kSecOutro, kSecTypes };
struct Section {
    int type = kSecDrop;
    int cycles = 8;    // 1..64 usul cycles
    int brk = 1;       // 0 off, 1..4 = variation A..D
    int bass = 1;      // 0 off, 1..4
    int lead = 1;      // 0 off, 1..4 = phrase A..D
    int chaos = 0;     // 0..100 live jungle edits
    int filter = 0;    // -100..100 break DJ filter (added to the mixer's)
};
struct Channel {
    int level = 80, pan = 0, dly = 0, rvb = 0, mute = 0;
};
struct GenParams {
    int density = 60;  // 0..100
    int range = 9;     // 4..14 degrees above the karar
    int ornament = 35; // 0..100 glissando / grace notes
    int start = 0;     // 0 auto (from the seyir), 1 karar, 2 guclu, 3 tiz durak
    int live = 0;      // regenerate every time the phrase wraps
    int seed = 1;      // 1..999
};
struct BassParams {
    int model = 0;     // 0 Reese, 1 Sub, 2 Growl
    int octave = -3;   // -4..-1 octaves from the makam's middle register
    int detune = 45, cutoff = 55, reso = 25, env = 40, decay = 50, drive = 35, glide = 30;
};
struct LeadParams {
    int model = 0;     // 0 Ney, 1 Kanun, 2 Saz, 3 Zurna
    int octave = 0;    // -2..1
    int tone = 55, decay = 50, vibrato = 40, glide = 40;
};

struct Song {
    int bpm = 172;
    int usul = 0;          // kUsuls index
    int unit = 0;          // 0: one unit = 16th, 1: 8th
    int swing = 0;         // 0..50 %
    int makam = 0;         // kMakams index
    int tuning = 0;        // Tuning
    int transpose = 0;     // -12..12 semitones (ahenk)
    int kit = 0;           // 0..2 built-in, 3 = WAV from the card
    char wav[40] = {};     // WAV name in /Lab/MAKAM/Breaks
    int breakResample = 1; // 1: the break follows the tempo (pitch), 0: cut only
    int songMode = 1;      // 0 loop the selected section, 1 play the arrangement
    int sectionCount = 6;
    int curSection = 0;
    int editVar = 0;       // break / bass variation being edited
    int editPhrase = 0;    // lead phrase being edited
    BreakVar brk[kVars];
    BassPat bass[kVars];
    Phrase phr[kPhrases];
    Section sec[kSections];
    Channel ch[kChannels];
    int dlyTime = 2;       // index into kDelaySync
    int dlyFb = 45;
    int rvbSize = 60, rvbDamp = 50;
    int duck = 40;         // bass/lead ducked by each dum
    int djFilter = 0;      // -100..100
    int drive = 25;        // break bus drive
    int editDensity = 50;  // jungle edit generator density
    GenParams gen;
    BassParams bp;
    LeadParams lp;
    int midiBend = 2;      // pitch-bend range of the receiver, semitones (1..24)
    int midiOut = 1;       // 0 off, 1 bass on ch+0 / lead on ch+1
};

constexpr int kDelaySyncCount = 6;
extern const char* const kDelaySyncNames[kDelaySyncCount];
extern const float kDelaySyncBeats[kDelaySyncCount];
extern const char* const kSectionNames[kSecTypes];

// Defaults: a playable song out of the box (sections, variations, phrases).
void songDefaults(Song& s);
// Re-derive everything that depends on the usul / makam (strokes, bass, phrases).
void songApplyUsul(Song& s);
void songApplyMakam(Song& s);
// Cycle the section at `cycle` falls in (song mode), with its start cycle.
int songSectionAt(const Song& s, uint32_t cycle, uint32_t* startCycle);
uint32_t songCycles(const Song& s);

// File format: "MKMJ" magic, uint16 version, uint16 reserved, uint32 payload
// bytes, payload (int16 per field, char arrays raw), uint32 CRC-32 of the
// payload. Older payloads (shorter) load with defaults for the new fields.
constexpr uint16_t kFileVersion = 1;
constexpr uint32_t kFileMax = 16384;
// Returns bytes written, 0 if cap is too small.
size_t songSave(const Song& s, uint8_t* out, size_t cap);
// false: bad magic / version / CRC (s unchanged).
bool songLoad(Song& s, const uint8_t* data, size_t len);

}  // namespace makam
