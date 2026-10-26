// KONSOL data model helpers (model.h).
#include "ws/model.h"

#include <cstdio>
#include <cstring>

namespace lab {
namespace konsol {

bool NoteBuf::add(const Note& x) {
    if (count >= kMaxNotes) return false;
    // keep sorted by start, then pitch
    int i = count;
    while (i > 0 && (n[i - 1].start > x.start || (n[i - 1].start == x.start && n[i - 1].pitch > x.pitch))) {
        n[i] = n[i - 1];
        --i;
    }
    n[i] = x;
    ++count;
    return true;
}

void NoteBuf::sort() {
    for (int i = 1; i < count; ++i) {
        const Note x = n[i];
        int j = i;
        while (j > 0 && (n[j - 1].start > x.start || (n[j - 1].start == x.start && n[j - 1].pitch > x.pitch))) {
            n[j] = n[j - 1];
            --j;
        }
        n[j] = x;
    }
}

void NoteBuf::remove(int i) {
    if (i < 0 || i >= count) return;
    for (int k = i; k + 1 < count; ++k) n[k] = n[k + 1];
    --count;
}

int NoteBuf::find(int startTick, int pitch) const {
    for (int i = 0; i < count; ++i)
        if (n[i].start == startTick && n[i].pitch == pitch) return i;
    return -1;
}

int NoteBuf::at(int tick, int pitch) const {
    for (int i = count - 1; i >= 0; --i)
        if (n[i].pitch == pitch && tick >= n[i].start && tick < n[i].start + (n[i].len ? n[i].len : 1)) return i;
    return -1;
}

NoteBuf& Clip::beginEdit() {
    pending = 0;   // a queued live loop is dropped: the edit wins
    const uint8_t c = cur;
    NoteBuf& b = buf[c ^ 1];
    std::memcpy(&b, &buf[c], sizeof(NoteBuf));
    return b;
}

void Clip::commit() { cur = uint8_t(cur ^ 1); }

namespace {
const char* const kScaleNames[kScaleCount] = {"Chromatic", "Minor",      "Major",     "Dorian",
                                               "Phrygian",  "Lydian",     "Mixolyd.",  "Harm.Minor",
                                               "Pent.Minor", "Pent.Major", "Blues",     "Makam"};
const uint16_t kScaleMasks[kScaleCount] = {
    0x0FFF,   // chromatic
    0x05AD,   // minor 0 2 3 5 7 8 10
    0x0AB5,   // major 0 2 4 5 7 9 11
    0x06AD,   // dorian 0 2 3 5 7 9 10
    0x05AB,   // phrygian 0 1 3 5 7 8 10
    0x0AD5,   // lydian 0 2 4 6 7 9 11
    0x06B5,   // mixolydian 0 2 4 5 7 9 10
    0x09AD,   // harmonic minor 0 2 3 5 7 8 11
    0x04A9,   // minor pentatonic 0 3 5 7 10
    0x0295,   // major pentatonic 0 2 4 7 9
    0x04E9,   // blues 0 3 5 6 7 10
    0x05B3,   // makam placeholder (Hicaz 0 1 4 5 7 8 10)
};
const char* const kGenNames[kGenCount] = {"Notes",    "Acid",     "Call+Resp", "BBCut",  "Turnaround", "Genotype",
                                          "Station",  "Euclid",   "Usul",      "Pinball", "Markov"};
const char* const kGenInfo[kGenCount] = {
    "The notes as written: steps, piano roll, recording",
    "303 line, live Mutate (TB-3PO style)",
    "Call phrase + answer (Nordl3 C/R Generator, MIT)",
    "Cut a sliced break (BBCut procs, Nick Collins)",
    "Phrase-end rolls, stutters, drops on a budget",
    "Learns this clip, plays variations (Markov/ball)",
    "Numbers-station digits from the radio",
    "Euclidean hits spread over the clip",
    "Usul: aksak rhythm cycles (dum / tek)",
    "A ball bouncing over a scale grid",
    "Markov chain melody over the scale",
};
const char* const kLaunchQ[5] = {"None", "1 beat", "1 bar", "2 bars", "4 bars"};
const char* const kRecQ[4] = {"Off", "1/16", "1/8", "1/32"};
}  // namespace

const char* scaleName(int s) { return kScaleNames[s < 0 ? 0 : (s >= kScaleCount ? kScaleCount - 1 : s)]; }
uint16_t scaleMaskOf(int s, int makamMask) {
    if (s == kScaleCount - 1 && makamMask) return uint16_t(makamMask);
    return kScaleMasks[s < 0 ? 0 : (s >= kScaleCount ? kScaleCount - 1 : s)];
}
const char* noteText(int note, char out[6]) {
    static const char* const kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0 || note > 127) {
        std::snprintf(out, 6, "--");
        return out;
    }
    std::snprintf(out, 6, "%s%d", kN[note % 12], note / 12 - 1);
    return out;
}
const char* genName(int k) { return kGenNames[k < 0 ? 0 : (k >= kGenCount ? kGenCount - 1 : k)]; }
const char* genInfo(int k) { return kGenInfo[k < 0 ? 0 : (k >= kGenCount ? kGenCount - 1 : k)]; }
const char* const* launchQNames() { return kLaunchQ; }
const char* const* recQNames() { return kRecQ; }
int launchQBeats(int q) {
    static const int kB[5] = {0, 1, 4, 8, 16};
    return kB[q < 0 ? 0 : (q > 4 ? 4 : q)];
}

}  // namespace konsol
}  // namespace lab
