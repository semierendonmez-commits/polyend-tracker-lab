// TILT -- patterns as JSON ("tilt-pattern", MANUAL.md "Pattern JSON"): the
// readme's model (steps, four 17 x 17 pages with edge properties, flippers,
// combos, tilt, ball, memory, mutation, meta, rules, melody, randomizer, the
// eight trig lanes) plus a "tracker" object for this firmware's channels and FX.
// Reading is lenient: missing keys keep their defaults, unknown keys are
// skipped, a single top-level "matrix" (one page) is accepted too.
// Main loop only; no allocation: the caller passes the text and token buffers.
#pragma once

#include <cstdint>

#include "ws/tilt_model.h"

namespace tilt {

struct JsonTok {
    uint8_t type = 0;   // 1 object, 2 array, 3 string, 4 primitive (number, true, false, null)
    int32_t start = 0, end = 0;
    int32_t size = 0;   // children: keys of an object, items of an array
};

// The pattern as JSON text (not terminated by the size); 0 = no room.
uint32_t patternToJson(const Pattern& p, char* out, uint32_t cap);
// Parses `text` (len bytes) into p (starting from the defaults). err gets a
// short reason on failure.
bool patternFromJson(Pattern& p, const char* text, uint32_t len, JsonTok* toks, int maxToks, char* err, int errCap);
constexpr int kJsonTokens = 4096;
constexpr uint32_t kJsonMaxBytes = 49152;

}  // namespace tilt
