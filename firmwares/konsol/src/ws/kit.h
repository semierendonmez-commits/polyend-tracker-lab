// KONSOL KIT engine: 16 pads (notes C2..D#3), each a range of a pool sample
// (a slice of a break or a whole one-shot), with tune / gain / pan / decay /
// filter / choke / reverse per pad. The engine parameters show the selected
// pad; the pad data itself lives in `pad[]` (main loop writes, audio reads).
#pragma once

#include "ws/engine_table.h"
#include "ws/samples.h"

namespace lab {
namespace konsol {

constexpr int kKitPads = 16;
constexpr int kKitBase = 36;   // C2 = pad 1

struct KitPad {
    int16_t slot = -1;          // pool slot (-1 = empty)
    uint32_t a = 0, e = 0;      // range in the slot (e = 0: to the end)
    float tune = 0, fine = 0, gain = 1, pan = 0, decay = 1, filter = 0, res = 0.15f;
    uint8_t choke = 0, rev = 0, cls = 0;
    char name[14] = {};
};

// Engine parameter indices (the selected pad's values + kit-wide ones).
enum KitP { kpPad, kpTune, kpFine, kpGain, kpPan, kpDecay, kpFilter, kpRes, kpChoke, kpRev, kpVoices, kpVelSens,
            kpKitGain, kpN };

// Builds the KIT engine of one track.
Engine* makeKit(Arena& a, SamplePool* pool);
KitPad* kitPads(Engine* kit);          // the 16 pads
int kitLastHit(const Engine* kit);     // pad of the last note (-1 none)
void kitSelect(Engine* kit, int pad);  // the pad the parameters show
void kitRefresh(Engine* kit);          // pad data changed: reload the shown values

}  // namespace konsol
}  // namespace lab
