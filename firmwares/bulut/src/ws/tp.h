// The third-party DSP BULUT compiles unchanged (THIRD_PARTY.md):
//   Mutable Instruments Clouds: Diffuser, PitchShifter (MIT, pichenettes/eurorack)
//   Mutable Instruments Beads:  WowAndFlutter, mu-law (MIT)
//   stmlib (MIT), the support code of both.
// Their headers are included here only, with the warnings they raise under
// BULUT's flags (-Wall -Wextra) silenced.
#pragma once

// stmlib picks ARM inline assembly (ssat / usat) unless TEST is defined:
// on the host (tests, simulator) it takes its portable C path.
#if !defined(__arm__) && !defined(TEST)
#define TEST 1
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "beads/dsp/fx/wow_and_flutter.h"
#include "beads/dsp/mu_law.h"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/diffuser.h"
#include "clouds/dsp/fx/pitch_shifter.h"
#pragma GCC diagnostic pop
