// MAKAM -- usul (rhythmic cycles) with their dum / tek strokes.
//
// One character per time unit (the usul's own denominator; at jungle tempo
// one unit = one 16th note by default, Settings > Usul unit):
//   'D' dum  -- strong, low (right hand)      -> kick slices of the break
//   'T' tek  -- light, high (left hand)       -> snare slices
//   'k' ke / ka (te-ke, tekka fills)          -> ghost-snare slices
//   '.' rest: the slice that is playing keeps playing (jungle chop)
#pragma once

#include <cstdint>

namespace makam {

constexpr int kUsulMaxSteps = 12;   // one pad row

enum Stroke : uint8_t { kRest = 0, kDum = 1, kTek = 2, kKe = 3 };

struct Usul {
    const char* name;        // ASCII
    const char* meter;       // "9/8"
    const char* groups;      // "2+2+2+3"
    const char* strokes;     // one char per unit (D T k .)
};

constexpr int kUsulCount = 13;
extern const Usul kUsuls[kUsulCount];

int usulLength(int index);                       // units per cycle
Stroke strokeOf(char c);
char strokeChar(Stroke s);
// Group boundaries: fills starts[] with the first unit of each group, returns the group count.
int usulGroups(int index, int* starts, int cap);

}  // namespace makam
