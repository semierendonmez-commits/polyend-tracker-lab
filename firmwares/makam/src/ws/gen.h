// MAKAM -- generators: break variations on the usul strokes, bass patterns,
// and the SEYIR melody generator. Main loop only (they write the song).
#pragma once

#include <cstdint>

#include "ws/song.h"
#include "ws/theory.h"

namespace makam {

// xorshift32, deterministic per seed (tests rely on it).
struct GenRng {
    uint32_t s;
    explicit GenRng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n) { return n > 0 ? int(next() % uint32_t(n)) : 0; }
    bool chance(int percent) { return range(100) < percent; }
};

// Variation v (0..3) of the break on the current usul: A plain strokes,
// B + ghost notes, C + rolls / stutters at the group ends, D + reverse and
// pitched edits (amount from s.editDensity).
void genBreakVar(Song& s, int v, uint32_t seed);
// Bass pattern v (0..3): A dum roots held, B dum + tek answers on the guclu,
// C rolling (every group), D half-time with octave jumps and glides.
void genBass(Song& s, int v);

// SEYIR melody generator. Fills `out` with `out.cycles` usul cycles (the
// cycle count is kept, 1..4; units beyond kPhraseMax are dropped):
//   - start region from the makam's seyir (ascending: karar region,
//     descending: tiz durak, desc-asc: guclu) or params.start,
//   - waypoints: guclu emphasis (yarim karar on the guclu, accented),
//     an asma karar, then the final cadence (tam karar) on the karar,
//     approached from above or from the yeden (makams that do),
//   - stepwise motion mostly (makam melodies move by seconds), thirds
//     sometimes, notes on the usul's dum / tek strokes first,
//   - ornaments: grace notes (from the degree above) and glissando.
// Degrees stay within -1 (yeden) .. params.range.
void genPhrase(const Makam& m, const GenParams& p, int usul, Phrase& out, uint32_t seed);

// Form = call + response (GenParams::form 1, 2+ cycles): the call (first
// half) ends on the guclu, the response keeps its rhythm and ends on the karar.
// The last degree played in the call half.
int phraseCallEnd(const Phrase& p, int usul);

// Last sounding degree of a phrase (the test checks the cadence).
int phraseLastDegree(const Phrase& p, int usul);
int phraseSteps(const Phrase& p, int usul);

}  // namespace makam
