// MAKAM -- break kits: three synthesised breaks (no card needed) and one
// WAV break from the card, each cut into 16 slices (eighth notes of a
// two-bar break) classified as kick / snare / ghost for the usul strokes.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "lab/lab.h"
#include "ws/song.h"

namespace makam {

enum SliceClass : uint8_t { kClsKick = 1, kClsSnare = 2, kClsGhost = 3 };

struct Kit {
    int16_t* data = nullptr;   // mono 44.1 kHz
    uint32_t cap = 0;          // frames allocated
    uint32_t frames = 0;       // frames used
    float bpm = 170.0f;        // the break's own tempo
    uint32_t start[kSlices + 1] = {};
    uint8_t cls[kSlices] = {};
    int8_t peak[200] = {};     // waveform overview (0..127 per column)
    char name[24] = {};
    // Slices of one class, in break order (for the strokes).
    uint8_t list[4][kSlices] = {};
    uint8_t count[4] = {};
    void index();              // fills list / count / peak from cls / data
};

constexpr uint32_t kKitFrames = 128000;          // built-in: 2 bars at 170 BPM = 124,517
constexpr uint32_t kWavFrames = 44100u * 12u;    // the card break: up to 12 s
extern const char* const kKitNames[kKits + 1];

// Synthesise built-in kit k (0 Kirik, 1 Sert, 2 Darbuka) into kit.data;
// `scratch` holds kKitFrames floats.
void synthKit(Kit& kit, int k, float* scratch);
// Load a WAV (mono or stereo, any rate / bit depth OpenTracker reads) and
// slice it as a 1/2/4-bar break (the bar count that puts it nearest to
// 170 BPM). Main loop only. False on any error (kit unchanged frames = 0).
// autoSlice: cut at the 16 strongest onsets (spectral flux, the semi fork's
// "Breaks" detector), else 16 equal parts. `scratch` = kKitFrames floats.
bool loadWavKit(Kit& kit, ot::IStorage& fs, const char* path, bool autoSlice = true, float* scratch = nullptr);
// The onset slicer on its own: fills kit.start[0..16] from kit.data/frames.
// Returns the number of onsets found (before filling to 16).
int onsetSlices(Kit& kit, float* scratch);
// Classify the 16 slices of kit.data by band energies at each onset.
void classifySlices(Kit& kit);

}  // namespace makam
