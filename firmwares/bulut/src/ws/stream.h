// BULUT TAPE FILE: a long WAV on the card (a field recording, a whole track)
// plays into a channel's buffer as if it were a live input (MANUAL.md,
// "TAPE FILE: a long WAV as a live input"). Adapted from KONSOL's SD
// streaming (a copy, not shared: the firmwares stay independent).
//
// One tape per channel (4 at once). The file itself is a 16-bit 44.1 kHz WAV
// (mono or interleaved stereo): the user's file when it already is one,
// otherwise a copy converted once into /Lab/BULUT/Stream/ (StreamConv).
//
// Resident head: kHeadFrames frames from FROM (the tape's start / loop point),
// mixed to mono, so Play from FROM starts at once. The rest comes from the
// card into a read-ahead ring per tape (raw file samples, interleaved when
// stereo) that the main loop refills in Bulut::update() (Tapes::service).
//
// Playback (audio side, Tapes::render, once per block for a channel whose
// source is TAPE FILE): the position runs in *virtual* frames from the last
// start / jump: file(v) = v up to the end, and with LOOP on frames past the
// end wrap to FROM, so the ring is filled straight across the loop point and
// a loop never waits for the card. Each block reads its window (tapeFetch:
// head or ring, mono), then resamples it at SPEED (varispeed 0.25 .. 2 x:
// the pitch follows). A frame that is not there yet is an underrun: it fades
// out from the last frame and back in afterwards (the audio never waits,
// never clicks). A jump or a stop lets the old signal decay over kStreamFade
// frames under the new one.
//
// Concurrency (one core, the audio interrupt preempts the main loop): the
// audio side owns playing / gen / need / geometry; the main loop owns the file
// (bound, ch, frames: changed only while `bound` is 0, which the audio side
// never touches past), the head (written while headOk is 0), fillGen / base /
// avail and the ring memory outside the published window. Main -> audio
// commands (play from a frame, stop) go through two request slots whose
// sequence number is published last. The main loop publishes ring data before
// `avail` (barriers); a ring whose fillGen is not the tape's gen counts as
// empty, and a fill that a jump overtook (gen changed while reading) is
// dropped.
//
// Card access goes through ot::audio::IStreamReader (lab::Context::
// streamReader): upstream's hal::SdStreamReader in the device image (raw
// sectors through the file's cluster map), StorageStreamReader in the
// simulator, FakeStreamReader in the CPU bench and the tests. Null (no card,
// a PLATFORM station): TAPE FILE is not available, BULUT works as before.
#pragma once

#include <cstdint>

#include "audio/stream_source.h"
#include "formats/wav.h"
#include "lab/lab.h"

namespace bulut {

constexpr int kTapes = 4;                            // one per channel
constexpr uint32_t kRingSamples = 1u << 17;          // int16 per ring (256 KB): 1.49 s stereo, 2.97 s mono at 1 x
constexpr uint32_t kChunkSamples = 1u << 15;         // one card read (64 KB)
constexpr uint32_t kHeadFrames = 32768;              // resident head from FROM, mono (0.74 s)
constexpr uint32_t kStreamMargin = 64;               // frames behind the position kept readable
constexpr uint8_t kStreamFade = 32;                  // underrun / jump fade, frames
constexpr float kTapeMinSpeed = 0.25f, kTapeMaxSpeed = 2.0f;
constexpr uint32_t kTapeMinFrames = 4u * 44100u;     // shorter files: load them as a SAMPLE
constexpr uint32_t kTapeFetchMax = 2u * lab::kBlock + 4u;   // frames one block reads at 2 x
// The virtual position is re-anchored (a short jump fade) before it gets near
// the uint32 range: after ~6.7 hours of looping at 2 x.
constexpr uint32_t kTapeRebase = 0x70000000u;

// The geometry of a run (file frames): FROM .. the end, looping or not.
struct StreamGeom {
    uint32_t a = 0, e = 0;   // loop start (FROM) / end of the file, a < e
    bool loop = false;
};

// Virtual frame -> file frame (LOOP wraps past e back to a).
inline uint32_t streamFileOf(const StreamGeom& g, uint32_t v) {
    if (v < g.e || !g.loop) return v;
    return g.a + (v - g.e) % (g.e - g.a);
}

struct TapeReq {
    uint8_t play = 0;        // 1 = play from v0 with g, 0 = stop
    uint32_t v0 = 0;
    StreamGeom g;
};

struct Tape {
    // ---- the file (main loop; changed only while bound == 0) ----
    volatile uint8_t bound = 0;
    uint8_t ch = 1, sid = 0;
    uint32_t frames = 0;               // file length
    // ---- the resident head (main loop; written while headOk == 0) ----
    volatile uint8_t headOk = 0;
    uint32_t headBase = 0, headLen = 0;   // file frames [headBase, headBase + headLen)
    int16_t* head = nullptr;              // kHeadFrames, mono
    // ---- main loop -> audio: two slots, the sequence published last ----
    TapeReq req[2];
    volatile uint32_t reqSeq = 0;
    // ---- audio side ----
    uint32_t seen = 0;
    volatile uint8_t playing = 0;
    volatile uint8_t ended = 0;        // a LOOP-off run reached the end (the main loop clears it)
    StreamGeom g;
    volatile uint32_t gen = 0;         // the current run's generation
    volatile uint32_t need = 0;        // the next virtual frame read (integer part of the position)
    uint32_t frac = 0;                 // fractional part of the position (16 bits)
    volatile uint32_t underruns = 0;
    int16_t last = 0;                  // last frame read (underrun decay)
    uint8_t fadeOut = 0, fadeIn = kStreamFade;
    int16_t xLast = 0;                 // jump / stop: the old signal decays from here
    uint8_t xFade = 0;
    int16_t outLast = 0;
    // ---- main loop: the ring ----
    volatile uint32_t fillGen = 0;
    volatile uint32_t base = 0, avail = 0;   // virtual frames [base, base + avail) are in buf
    int16_t* buf = nullptr;                  // kRingSamples, interleaved when stereo
    uint32_t ringFrames() const { return kRingSamples / ch; }
};

struct StreamStats {
    uint32_t chunks = 0, readErrors = 0, passes = 0, maxReadMs = 0;
    uint32_t minLead = 0xFFFFFFFFu;   // smallest lead seen (frames)
};

class Tapes {
public:
    // Rings, heads and a head-reading scratch from the arena (1.3 MB).
    bool init(lab::Arena& a);
    bool ready() const { return mem_ != nullptr; }
    void setReader(ot::audio::IStreamReader* r) { reader_ = r; }
    ot::audio::IStreamReader* reader() const { return reader_; }
    Tape& tape(int c) { return tape_[c]; }
    const Tape& tape(int c) const { return tape_[c]; }

    // ---- audio side ----
    // kBlock mono samples of tape c at `speed` into out. False: nothing to
    // write (stopped, no file): the channel's buffer holds.
    bool render(int c, float speed, int16_t* out);

    // ---- main loop ----
    // Play tape c from file frame `from` (start of the next block), or stop it.
    void play(int c, uint32_t from, uint32_t loopStart, bool loop);
    void stop(int c);
    bool requestPending(int c) const { return tape_[c].reqSeq != tape_[c].seen; }
    // The file frame the tape is at (or was stopped at).
    uint32_t position(int c) const;
    // The tape forgets its file: the audio side stops at the next block.
    void unbind(int c);
    // Bind tape c to a 16-bit 44.1 kHz WAV (data at dataOffset, ch channels,
    // frames long) through the reader. False: reason in *why.
    bool bind(int c, const char* file, uint32_t dataOffset, uint8_t ch, uint32_t frames, const char** why);
    // The resident head from file frame `from` (reads through the reader).
    bool loadHead(int c, uint32_t from);
    // Reads ahead until every ring is full or budgetMs passed (at least one
    // chunk when one is needed); maxChunks caps the reads. Returns the reads.
    int service(uint32_t (*millis)(void*), void* ctx, uint32_t budgetMs, int maxChunks);
    // Frames read ahead of tape c (0xFFFFFFFF: the rest of a LOOP-off run is in).
    uint32_t lead(int c) const;
    uint32_t underruns() const;
    StreamStats stats;

private:
    int fill(Tape& t);   // one chunk; 1 read, 0 nothing to do, -1 error
    void post(int c, const TapeReq& q);
    ot::audio::IStreamReader* reader_ = nullptr;
    int16_t* mem_ = nullptr;
    int16_t* scratch_ = nullptr;   // kChunkSamples: head reads
    Tape tape_[kTapes];
};

// Audio side: frames [v0, v0 + n) of tape t's current run, mixed to mono,
// into out. Head frames from the head, the rest from the ring. Missing frames
// fade (tape state); frames past a LOOP-off end are silence. False when any
// frame was missing (an underrun, counted on the tape).
bool tapeFetch(Tape& t, uint32_t gen, uint32_t v0, uint32_t n, int16_t* out);

// ---- conversion: any WAV the Tracker reads -> a 16-bit 44.1 kHz WAV ------
// A main-loop job in steps: one pass over the source, both channels through
// upstream's WavChunkConverter, written interleaved behind a canonical header
// into `dst`.tmp, renamed when complete.
class StreamConv {
public:
    bool init(lab::Arena& a);
    bool start(ot::IStorage& fs, const char* src, const char* dst);
    // Converts up to `bytes` source bytes; false when done (ok() says how).
    bool step(uint32_t bytes);
    void cancel();
    bool busy() const { return busy_; }
    bool ok() const { return ok_; }
    float progress() const { return total_ ? float(done_) / float(total_) : 0.0f; }
    const char* dst() const { return dst_; }

private:
    static void sinkL(void* ctx, const int16_t* s, uint32_t n);
    static void sinkR(void* ctx, const int16_t* s, uint32_t n);
    void finish(bool ok);
    ot::IStorage* fs_ = nullptr;
    ot::IStorage::Handle in_ = -1, out_ = -1;
    ot::WavChunkConverter cv_[2];
    int16_t* win_[2] = {};
    int16_t* stage_ = nullptr;     // left samples waiting for their right ones
    int16_t* io_ = nullptr;        // interleaved output
    uint32_t stageN_ = 0;
    uint8_t ch_ = 1;
    bool busy_ = false, ok_ = false, failed_ = false;
    uint32_t done_ = 0, total_ = 0, bpf_ = 0;
    char dst_[128] = {};
};

#if defined(OT_BENCH_M4) || !defined(__arm__)
// Bench / tests: a reader that synthesises any frame of any bound file (a
// 16-bit saw + square, interleaved when stereo); reads can fail.
class FakeStreamReader final : public ot::audio::IStreamReader {
public:
    bool read(const ot::audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) override;
    static int16_t sample(uint8_t sid, uint32_t sampleIndex);   // the value at interleaved index
    uint32_t reads = 0;
    bool fail = false;
};
#endif

}  // namespace bulut
