// MAKAM -- Turkish makam theory: perdes, the two tuning systems, the makam
// table. Pure data + small functions, shared by the audio engine, the
// generators, the UI and the host tests (tests/lab/makam_test.cpp).
//
// Pitch positions are absolute commas (Holdrian comma = 1/53 octave) above
// Kaba Cargah (C4 in the AEU notation); Rast = 31 (G), Dugah = 40 (A),
// Cargah = 53 (C5), Gerdaniye = 84 (G5), Muhayyer = 93 (A5).
//
// Tuning systems:
//   AEU  -- Arel-Ezgi-Uzdilek: 24 pitches per octave from a chain of pure
//           fifths (3/2), from -12 to +11 fifths of Kaba Cargah. The comma
//           counts name the steps (koma 1, bakiye 4, kucuk mucenneb 5,
//           buyuk mucenneb 8, tanini 9, artik ikili 12/13); the real
//           sizes are Pythagorean (bakiye = limma 256/243 = 90.2 c ...).
//   53-TET -- every comma exactly 1200/53 = 22.64 cents.
#pragma once

#include <cstdint>

namespace makam {

constexpr int kCommasPerOctave = 53;
constexpr int kDugah = 40;        // reference perde (A4 = 440 Hz, ahenk 0)

enum Tuning : uint8_t { kTuneAEU = 0, kTune53 = 1 };
enum Seyir : uint8_t { kSeyirAsc = 0, kSeyirDescAsc = 1, kSeyirDesc = 2 };

// The 24 AEU pitch classes (commas 0..52 above Cargah).
constexpr int kAeuCount = 24;
extern const uint8_t kAeuComma[kAeuCount];
extern const int8_t kAeuFifths[kAeuCount];      // position on the chain of fifths
extern const char* const kAeuShort[kAeuCount];  // 3-4 letter ring labels

bool isAeuPitch(int comma);                      // pitch class is one of the 24
int aeuIndex(int comma);                         // 0..23 for the pitch class, -1 if none
float aeuCents(int comma);                       // Pythagorean cents above Kaba Cargah (absolute)
float tetCents(int comma);                       // 53-TET cents above Kaba Cargah
float cents(int comma, Tuning t);
// Frequency in Hz; Dugah (40) = refHz * 2^(ahenk/12).
float freqHz(int comma, Tuning t, float refHz = 440.0f, int ahenk = 0);
// Perde name (ASCII transliteration), e.g. "Dugah", "Tiz Segah", "Kaba Rast".
// Writes into buf (>= 24 chars); returns buf.
const char* perdeName(int comma, char* buf, int cap);

// ---------------------------------------------------------------------------
struct Makam {
    const char* name;        // ASCII (screen)
    const char* parts;       // short composition, e.g. "Ussak4 + Buselik5@Neva"
    int8_t asc[8];           // degrees 0..7 ascending (absolute commas); asc[0] = karar
    int8_t desc[8];          // degrees 0..7 when the melody descends
    uint8_t guclu;           // degree index of the guclu (dominant)
    int8_t yeden;            // leading tone below the karar (absolute commas)
    uint8_t seyir;           // Seyir
    int8_t asma[2];          // asma karar degrees (suspended cadences; may be below the karar)
    bool yedenCadence;       // the final cadence typically comes up from the yeden
};

constexpr int kMakamCount = 25;
extern const Makam kMakams[kMakamCount];
const char* seyirName(uint8_t s);   // "Ascending" / "Desc-Asc" / "Descending"

// Degree -> absolute comma, extended over octaves: note(d) for d >= 8 is
// note(d - 7) + 53, for d < 0 note(d + 7) - 53 (so Saba keeps its
// non-octave Sehnaz on degree 7 and Tiz Segah on degree 8).
int degreeComma(const Makam& m, int degree, bool descending = false);
// Interval in commas between degree i and i+1 (0..6) of the ascending table.
int stepCommas(const Makam& m, int i, bool descending = false);
// Is `comma` one of the makam's pitches (any octave, either table)?
bool inMakam(const Makam& m, int comma);
// Nearest degree to an absolute comma (for MIDI-in quantising).
int nearestDegree(const Makam& m, int comma);

}  // namespace makam
