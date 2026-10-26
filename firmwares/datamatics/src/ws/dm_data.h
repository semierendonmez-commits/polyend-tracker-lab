// DATAMATICS -- data sources: any file on the card decoded into a sequence of
// 8-bit values by one of the reading modes. Pure functions (no I/O, no heap)
// so the host tests can feed synthetic files.
#pragma once

#include <cstdint>

namespace dm {

// Reading modes. kAuto picks one from the file name / content.
enum Mode : uint8_t { kAuto = 0, kRaw, kBits, kImage, kWav, kMidi, kText, kModeCount };
const char* modeName(int m);   // "AUTO", "RAW", ...

constexpr uint32_t kMaxValues = 262144;      // values per data slot (256 Ki)
constexpr uint32_t kMaxFileBytes = 2u << 20; // bytes staged from a file (2 MiB)
constexpr uint32_t kMaxMidiEvents = 16384;

// A decoded data set. `v` holds the values 0..255; `aux` a second value per
// entry (MIDI: velocity; otherwise equal to v). `width` is the raster width
// (images: pixels per row; others 256) used by column-order reading.
struct DataView {
    const uint8_t* v = nullptr;
    const uint8_t* aux = nullptr;
    uint32_t n = 0;
    uint16_t width = 256;
    uint8_t mode = kRaw;      // the mode actually used (never kAuto)
    uint16_t srcW = 0, srcH = 0;   // image: source size before any downsampling
};

// Output buffers for a decode.
struct DecodeOut {
    uint8_t* v = nullptr;
    uint8_t* aux = nullptr;
    uint32_t cap = kMaxValues;
    uint32_t n = 0;
    uint16_t width = 256;
    uint8_t mode = kRaw;
    uint16_t srcW = 0, srcH = 0;
};

// Scratch for MIDI decoding (kMaxMidiEvents entries).
struct MidiEv {
    uint32_t tick;
    uint8_t note, vel, track, pad;
};

// Mode for a file: from its extension first, then from its first bytes.
Mode detectMode(const char* name, const uint8_t* head, uint32_t len);

// Decode `file` (len bytes) with `mode` (kAuto = detect from name/content).
// Returns false only when nothing could be decoded (out.n == 0). A mode that
// does not fit the content (e.g. kImage on a text file) falls back to raw.
bool decode(const char* name, const uint8_t* file, uint32_t len, Mode mode, DecodeOut& out, MidiEv* midiScratch);

// Individual decoders (tests).
uint32_t decodeRaw(const uint8_t* f, uint32_t len, DecodeOut& o);
uint32_t decodeBits(const uint8_t* f, uint32_t len, DecodeOut& o);
uint32_t decodeText(const uint8_t* f, uint32_t len, DecodeOut& o);
uint32_t decodeImage(const uint8_t* f, uint32_t len, DecodeOut& o);   // BMP, PGM/PPM/PBM
uint32_t decodeWav(const uint8_t* f, uint32_t len, DecodeOut& o);
uint32_t decodeMidi(const uint8_t* f, uint32_t len, DecodeOut& o, MidiEv* scratch);

// Built-in data sets (no card needed): "@PRIMES", "@TESTCARD", "@LFSR", "@SINE", "@FIBONACCI".
constexpr int kBuiltinCount = 5;
const char* builtinName(int i);
int builtinIndex(const char* path);   // -1 if not a built-in
uint32_t makeBuiltin(int i, DecodeOut& o);

}  // namespace dm
