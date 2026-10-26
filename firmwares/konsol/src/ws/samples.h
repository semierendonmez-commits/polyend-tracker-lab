// KONSOL sample pool: every sample of the project (track samples, kit pads,
// stretched versions) lives in one SDRAM pool, mono or stereo (planar: the
// left channel, then the right one). Slots are allocated by the main loop;
// the audio reads a slot through its index. A slot's frames field is 0 while
// it is being written, so the audio never plays half a sample.
//
// Onsets: each slot can carry up to 64 transient positions (spectral-flux
// detector, after the semi fork's "Breaks" auto slice) with a class per
// onset (kick / snare / hat / other) for building drum kits.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "lab/lab.h"

namespace lab {
namespace konsol {

constexpr int kSlots = 96;
constexpr int kMaxOnsets = 64;
enum HitClass : uint8_t { kClsOther = 0, kClsKick, kClsSnare, kClsHat };
const char* hitClassName(int c);

struct SampleSlot {
    volatile uint32_t frames = 0;   // playable frames (0 = empty / being written)
    uint32_t off = 0;               // int16 offset of the left channel in the pool
    uint32_t cap = 0;               // allocated frames
    uint8_t ch = 1;                 // 1 mono, 2 stereo (planar)
    uint8_t used = 0;
    uint8_t nOn = 0;                // onsets found
    uint8_t derived = 0;            // made by KONSOL (stretch, kit render): not a file
    char path[96] = {};             // card path ("" = built in / derived)
    uint32_t onset[kMaxOnsets] = {};
    uint8_t cls[kMaxOnsets] = {};
    float strength[kMaxOnsets] = {};
};

class SamplePool {
public:
    bool init(Arena& a, uint32_t samples);
    // Allocate `frames` frames of `ch` channels at the top. -1: no room (the
    // caller may compact() and retry).
    int alloc(uint32_t frames, uint8_t ch);
    void release(int i);
    // Moves every used slot down so the free space is one block at the top.
    // The audio must not read the pool meanwhile (KONSOL holds render()).
    void compact();
    uint32_t freeFrames(uint8_t ch) const { return (cap_ - top_) / ch; }
    uint32_t capSamples() const { return cap_; }
    uint32_t usedSamples() const { return top_; }
    SampleSlot& slot(int i) { return slot_[i]; }
    const SampleSlot& slot(int i) const { return slot_[i]; }
    bool valid(int i) const { return i >= 0 && i < kSlots && slot_[i].used && slot_[i].frames; }
    const int16_t* left(int i) const { return data_ + slot_[i].off; }
    const int16_t* right(int i) const {
        return data_ + slot_[i].off + (slot_[i].ch == 2 ? slot_[i].cap : 0);
    }
    int16_t* leftW(int i) { return data_ + slot_[i].off; }
    int16_t* rightW(int i) { return data_ + slot_[i].off + (slot_[i].ch == 2 ? slot_[i].cap : 0); }
    // Loads a WAV (any format the Tracker reads; stereo kept). Returns the
    // slot or -1 (err gets a reason). maxFrames limits the length.
    int loadWav(IStorage& fs, const char* path, uint32_t maxFrames, char* err, int errCap);
    // The built-in 1-bar break (120 BPM, 16 hits).
    int builtinBreak();
    // Mono view helper: (l + r) / 2 at frame i.
    inline float mono(int s, uint32_t i) const {
        const SampleSlot& sl = slot_[s];
        if (sl.ch == 2) return (float(left(s)[i]) + float(right(s)[i])) * (0.5f / 32768.0f);
        return float(left(s)[i]) * (1.0f / 32768.0f);
    }
    // Onsets of slot s (spectral flux), and a class for every onset. Main
    // loop; ~0.2 s per 10 s of audio on the device.
    void detectOnsets(int s, float sensitivity = 0.5f);
    void classify(int s);
    // Peak level of a slot (0..1).
    float peak(int s) const;
    // Slice editor: n equal slices; insert / remove one point (kept sorted).
    void equalOnsets(int s, int n);
    int addOnset(int s, uint32_t at);       // index of the new point, -1 full
    void removeOnset(int s, int k);         // point 0 stays
    void moveOnset(int s, int k, int32_t d);
    // Slice k of slot s: [a, e). false if k is out of range.
    bool sliceRange(int s, int k, uint32_t& a, uint32_t& e) const;

private:
    int16_t* data_ = nullptr;
    uint32_t cap_ = 0, top_ = 0;
    SampleSlot slot_[kSlots];
};

// One track's sample: the loaded file (src) and what the engines play (slot:
// src itself, or a time-stretched copy). Engines read `slot` once per voice.
struct TrackSample {
    volatile int16_t slot = -1;
    int16_t src = -1;
    uint8_t bars = 0;               // loop length in bars (0 = detect)
    uint8_t mode = 0;               // stretch mode (StretchMode)
    int8_t pitch = 0;               // stretch pitch, semitones
    uint8_t stretched = 0;          // slot is a stretched copy of src
    float srcBpm = 0;               // tempo of the file (from its length and bars)
    float outBpm = 0;               // tempo of `slot`
};

// Tempo of a loop from its length: the bar count in 1..16 whose tempo is the
// most likely (70..180 BPM, nearest to `hint`). bars = 0 picks; returns BPM.
float loopBpm(uint32_t frames, int& bars, float hint);

}  // namespace konsol
}  // namespace lab
