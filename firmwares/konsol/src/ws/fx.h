// KONSOL send effects (float, no allocation after init):
//   SendDelay  stereo tempo delay: Digital (clean, filtered feedback),
//              Tape (wow + flutter, saturation, darkening repeats, glides on
//              tempo changes) and Lo-fi (sample-rate / bit crush, band-limited)
//   FdnVerb    8-line feedback delay network after Jot / Stautner-Puckette:
//              Hall (long, diffuse), Shimmer (an octave-up pitch shifter in the
//              loop) and Gated (dense, cut by a gate that follows the input)
// The OpenTracker reverb stays the "Plate".
#pragma once

#include <cstdint>

#include "lab/lab.h"

namespace lab {
namespace konsol {

class SendDelay {
public:
    static constexpr uint32_t kMax = 44100u * 2u + 2048u;   // 2 s (1/2 note at 60 BPM)
    bool init(Arena& a);
    void setTime(uint32_t samples) { target_ = float(samples < 64 ? 64 : (samples > kMax - 4 ? kMax - 4 : samples)); }
    void set(int type, float fb, float tone, bool pingPong, float mod) {
        type_ = type;
        fb_ = fb;
        tone_ = tone;
        pp_ = pingPong;
        mod_ = mod;
    }
    // in: the mono send; adds nothing, writes ol / orr. false = silent (skipped).
    bool process(const float* in, float* ol, float* orr);
    void clear();

private:
    float* buf_[2] = {};
    uint32_t w_ = 0;
    float cur_ = 22050.0f, target_ = 22050.0f;
    int type_ = 0;
    float fb_ = 0.45f, tone_ = 0.6f, mod_ = 0.3f;
    bool pp_ = true;
    float lp_[2] = {}, hpX_[2] = {}, hpY_[2] = {};
    float wow_ = 0, flut_ = 0, hold_[2] = {}, crush_ = 0;
    uint32_t quiet_ = 1u << 20;
};

class FdnVerb {
public:
    static constexpr int kLines = 8;
    bool init(Arena& a);
    void set(int type, float size, float damp, float pre) {
        type_ = type;
        size_ = size;
        damp_ = damp;
        pre_ = pre;
        dirty_ = true;
    }
    bool process(const float* in, float* ol, float* orr);
    void clear();

private:
    void configure();
    static constexpr uint32_t kLineMax = 4096, kPreMax = 5400, kApMax = 1024, kShift = 4096;
    float* line_[kLines] = {};
    uint32_t len_[kLines] = {}, idx_[kLines] = {};
    float g_[kLines] = {}, lp_[kLines] = {};
    float* pre_buf_ = nullptr;
    float* ap_[4] = {};
    uint32_t apLen_[4] = {}, apPos_[4] = {};
    float* sh_ = nullptr;          // shimmer pitch shifter
    uint32_t shW_ = 0;
    float shR_ = 0;
    float shOut_ = 0, lastL_ = 0, lastR_ = 0;
    uint32_t pos_ = 0, preW_ = 0, preLen_ = 0;
    int type_ = 1;
    float size_ = 0.7f, damp_ = 0.5f, pre_ = 0.0f, dampK_ = 0.3f;
    float gateEnv_ = 0, gate_ = 0;
    uint32_t gateHold_ = 0, gateHoldLen_ = 8000;
    float modPh_ = 0;
    bool dirty_ = true;
    uint32_t quiet_ = 1u << 20;
};

}  // namespace konsol
}  // namespace lab
