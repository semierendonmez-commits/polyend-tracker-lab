// Builds the third-party sources BULUT uses (tp.h): stmlib's random state,
// Beads' mu-law table and the one Beads lookup table WowAndFlutter needs.

// stmlib picks ARM inline assembly (ssat / usat) unless TEST is defined:
// on the host (tests, simulator) it takes its portable C path.
#if !defined(__arm__) && !defined(TEST)
#define TEST 1
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "stmlib/utils/random.cc"
#include "beads/dsp/mu_law.cc"
#pragma GCC diagnostic pop

#include "ws/tp_tables.inc"
