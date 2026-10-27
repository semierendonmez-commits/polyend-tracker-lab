// KONSOL offline time-stretch / pitch-shift (main loop, incremental).
//
//   BEATS    transient-locked WSOLA: the range is cut at its onsets; every
//            segment starts exactly on its (time-scaled) onset with its
//            attack untouched, and its body is stretched by WSOLA (grains of
//            23 ms, 50 % overlap, waveform-similarity search +-6 ms) inside
//            the segment only, so no hit smears into the next one.
//   TONAL    phase vocoder (2048-point FFT, hop 512, identity phase locking
//            after Laroche & Dolson, phase reset at strong onsets, stereo
//            phase-coherent: one phase advance for both channels).
//   REPITCH  plain resampling: speed and pitch change together (like tape).
//
// Pitch (semitones) keeps the length: the signal is stretched by an extra
// 2^(p/12) and then resampled by the same factor.
#pragma once

#include <cstdint>

#include "lab/lab.h"
#include "ws/samples.h"

namespace lab {
namespace konsol {

enum StretchMode : uint8_t { kStrBeats = 0, kStrTonal, kStrRepitch, kStrModes };
const char* stretchModeName(int m);

class Stretcher {
public:
    bool init(Arena& a);
    // Starts a job: source slot `src`, frames [a, e), output length ratio
    // `ratio` (2 = twice as long), pitch in semitones. Allocates the output
    // slot (-1 / false when the pool is full).
    bool start(SamplePool& pool, int src, uint32_t a, uint32_t e, float ratio, float pitch, int mode);
    // Works for about `frames` output frames; false when done (or idle).
    bool step(uint32_t frames = 4096);
    void run() {
        while (step(1 << 20)) {
        }
    }
    bool busy() const { return busy_; }
    float progress() const { return outLen_ ? float(written_) / float(outLen_) : 0.0f; }
    int out() const { return out_; }
    int src() const { return src_; }
    void cancel();

private:
    // producer: the time-stretched (by ratio * pitch) signal, frame by frame
    bool produce();               // one grain / frame into the intermediate ring
    bool produceWsola();
    bool producePv();
    float readSrc(int c, float pos) const;   // linear interpolation, 0 outside [a, e)
    void emit(int c, uint32_t at, float v);
    void flushTo(uint32_t upTo);  // intermediate frames < upTo are final
    void resampleOut();           // intermediate -> output slot at speed `pitchRatio`

    SamplePool* pool_ = nullptr;
    int src_ = -1, out_ = -1, mode_ = 0;
    uint8_t ch_ = 1;
    uint32_t a_ = 0, e_ = 0, outLen_ = 0, written_ = 0;
    float ratio_ = 1, pr_ = 1;    // time ratio, pitch ratio
    float tr_ = 1;                // ratio of the producer (ratio * pitch)
    uint32_t interLen_ = 0;       // producer output length
    bool busy_ = false;

    // intermediate ring (producer output), float, per channel
    static constexpr uint32_t kRing = 16384;
    float* ring_[2] = {};
    uint32_t ringFinal_ = 0;      // producer frames that are final
    uint32_t ringBase_ = 0;       // producer frame of ring index 0 (mod kRing)
    double readPos_ = 0;          // resampler read position in producer frames

    // WSOLA
    static constexpr int kGrain = 1024, kHop = 512, kTol = 256;
    float win_[kGrain];
    int seg_ = 0, nSeg_ = 0;
    uint32_t segS_[kMaxOnsets + 1], segT_[kMaxOnsets + 1];
    int grain_ = 0;
    float prevX_ = 0;

    // phase vocoder
    static constexpr int kN = 2048, kH = 512, kBins = kN / 2 + 1;
    float* re_[2] = {};
    float* im_[2] = {};
    float* lastPh_[2] = {};       // analysis phase of the previous frame
    float* synPh_ = nullptr;      // synthesis phase advance (shared, mid channel)
    float* mag_ = nullptr;
    float* pvWin_ = nullptr;
    int frame_ = 0;
    bool firstFrame_ = true;
    uint32_t nextOnset_ = 0;
};

}  // namespace konsol
}  // namespace lab
