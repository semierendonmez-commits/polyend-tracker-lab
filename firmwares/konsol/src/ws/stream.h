// KONSOL SD streaming: long WAVs (whole tracks, stems, long loops) play from
// the card instead of the sample pool (MANUAL.md "Streaming from the card").
//
// A streamed pool slot keeps only its head (the first kHeadFrames frames,
// planar like every slot) resident; `frames` is that head, so every engine
// and editor that reads the pool sees a short, always-valid sample, and only
// the SAMPLER (One-shot / Loop, Warp Off) knows the whole length
// (SampleSlot::sframes). The file itself is a 16-bit 44.1 kHz WAV (mono or
// interleaved stereo): the user's file when it already is one, otherwise a
// copy converted once into /Lab/KONSOL/Stream/ (StreamConv).
//
// Playback: a SAMPLER voice on a streamed slot takes one of kStreamRings
// rings (audio side, at note-on). Its position runs in *virtual* frames from
// the note's start: file(v) = v up to the end, and in Loop mode frames past
// the loop end wrap to the loop start, so the ring is filled straight across
// the loop point and a loop never waits for the card. Each block the voice
// copies its window from the head or the ring (streamFetch); a frame that is
// not there yet is an underrun: it fades out from the last frame and back in
// afterwards (upstream's rule: the audio never waits, never clicks).
// The main loop (Konsol::update) reads ahead: one chunk at a time to the
// ring with the smallest lead, until a time budget (Streams::service).
//
// Concurrency (one core, the audio interrupt preempts the main loop): the
// audio side owns busy / gen / need / geometry; the main loop owns fillGen /
// base / avail and the ring memory outside the published window. The main
// loop publishes data before `avail` (barriers); a ring whose fillGen is not
// the voice's gen counts as empty. A fill that a new note overtook (gen
// changed while reading) is dropped.
//
// Card access goes through ot::audio::IStreamReader, handed over by the
// Shell (lab::Context::streamReader): upstream's hal::SdStreamReader in the
// device image (raw sectors through the file's cluster map, no FatFs on the
// stream path), StorageStreamReader in the simulator, a synthetic reader in
// the CPU bench. Null (a PLATFORM station, no card): nothing streams.
#pragma once

#include <cstdint>

#include "audio/stream_source.h"
#include "formats/wav.h"
#include "lab/lab.h"

namespace lab {
namespace konsol {

class SamplePool;

constexpr int kStreamRings = 8;                     // streamed voices at once (all tracks)
constexpr uint32_t kRingSamples = 1u << 17;         // int16 per ring (256 KB): 1.49 s stereo, 2.97 s mono
constexpr uint32_t kChunkSamples = 1u << 15;        // one card read (64 KB)
constexpr uint32_t kHeadFrames = 32768;             // resident head per channel (0.74 s)
constexpr uint32_t kStreamMargin = 64;              // frames behind the position kept readable
constexpr uint8_t kStreamFade = 32;                 // underrun fade, frames
constexpr int kStreamFiles = ot::audio::kMaxStreams;   // files bound at once (16)
constexpr float kStreamMaxRate = 2.0f;              // streamed playback speed limit (+12 semitones)
// Auto: a track sample streams from this length on (or when it does not fit).
constexpr uint32_t kStreamAutoFrames = 30u * 44100u;
// On: from this length on (shorter files are just loaded).
constexpr uint32_t kStreamMinFrames = 3u * kHeadFrames;

// Per-track switch (TrackSample::stream, Sample page "Stream" row).
enum StreamMode : uint8_t { kStreamAuto = 0, kStreamOn, kStreamOff, kStreamModes };
const char* streamModeName(int m);

// The note geometry a voice hands its ring (file frames).
struct StreamGeom {
    uint32_t a = 0, e = 0;   // start / end (Loop: the loop) of the note, a < e
    bool loop = false;
};

// Virtual frame -> file frame (Loop wraps past e back to a).
inline uint32_t streamFileOf(const StreamGeom& g, uint32_t v) {
    if (v < g.e || !g.loop) return v;
    return g.a + (v - g.e) % (g.e - g.a);
}

struct StreamRing {
    // ---- audio side (set at note-on, before gen) ----
    volatile uint8_t busy = 0;     // a voice owns it
    int8_t slot = -1;              // the pool slot (file) it plays
    uint8_t ch = 1, sid = 0xFF;
    StreamGeom g;
    uint32_t head = 0;             // resident head frames of that slot
    uint32_t age = 0;
    volatile uint32_t gen = 0;     // the owner's note generation
    volatile uint32_t need = 0;    // the next virtual frame the voice reads
    volatile uint32_t underruns = 0;
    int16_t lastL = 0, lastR = 0;
    uint8_t fadeOut = 0, fadeIn = kStreamFade;
    // ---- main loop ----
    volatile uint32_t fillGen = 0;
    volatile uint32_t base = 0, avail = 0;   // virtual frames [base, base + avail) are in buf
    int16_t* buf = nullptr;                  // kRingSamples, interleaved when stereo
    uint32_t frames() const { return kRingSamples / ch; }
};

struct StreamStats {
    uint32_t chunks = 0, readErrors = 0, passes = 0, maxReadMs = 0;
    uint32_t minLead = 0xFFFFFFFFu;   // smallest lead seen (frames)
};

class Streams {
public:
    // Rings from the arena (2 MB). False: no room (streaming stays off).
    bool init(Arena& a);
    bool ready() const { return mem_ != nullptr; }
    void setReader(ot::audio::IStreamReader* r) { reader_ = r; }
    ot::audio::IStreamReader* reader() const { return reader_; }
    StreamRing& ring(int i) { return ring_[i]; }

    // ---- audio side ----
    // A ring for a new note of pool slot `slot` (steals the oldest when all
    // are busy). Returns the ring index and the note's generation in *gen.
    int acquire(int slot, uint8_t ch, uint8_t sid, uint32_t head, const StreamGeom& g, uint32_t* gen);
    void release(int r, uint32_t gen);
    bool owns(int r, uint32_t gen) const { return r >= 0 && ring_[r].busy && ring_[r].gen == gen; }

    // ---- main loop ----
    // Reads ahead until every ring is full or budgetMs passed (at least one
    // chunk when one is needed); maxChunks caps the reads. Returns the reads.
    int service(const SamplePool& pool, uint32_t (*millis)(void*), void* ctx, uint32_t budgetMs, int maxChunks);
    // Every ring of slot s forgets it (the slot is released / rebound).
    void dropSlot(int s);
    uint32_t underruns() const;
    int active() const;
    StreamStats stats;

private:
    int fill(StreamRing& r, const SamplePool& pool);   // one chunk; 1 read, 0 nothing to do, -1 error
    ot::audio::IStreamReader* reader_ = nullptr;
    int16_t* mem_ = nullptr;
    StreamRing ring_[kStreamRings];
    uint32_t gen_ = 0, age_ = 0;
};

// Audio side: frames [v0, v0 + n) of a streamed note into l / r (r may equal
// l's channel for mono: both get the same samples). Head frames come from
// hl / hr, the rest from the ring (nullptr: head only). Missing frames fade
// (ring state); frames past a one-shot's end are silence. False when any
// frame was missing (an underrun, counted on the ring).
bool streamFetch(StreamRing* ring, uint32_t gen, const StreamGeom& g, uint8_t ch, const int16_t* hl,
                 const int16_t* hr, uint32_t head, uint32_t v0, uint32_t n, int16_t* l, int16_t* r);

// ---- conversion: any WAV the Tracker reads -> a 16-bit 44.1 kHz WAV ------
// A main-loop job in steps (like the time-stretch): one pass over the source,
// both channels through upstream's WavChunkConverter, written interleaved
// behind a canonical header into `dst`.tmp, renamed when complete.
class StreamConv {
public:
    bool init(Arena& a);
    bool start(IStorage& fs, const char* src, const char* dst);
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
    IStorage* fs_ = nullptr;
    IStorage::Handle in_ = -1, out_ = -1;
    ot::WavChunkConverter cv_[2];
    int16_t* win_[2] = {};
    int16_t* stage_ = nullptr;     // left samples waiting for their right ones
    int16_t* io_ = nullptr;        // interleaved output / source bytes
    uint32_t stageN_ = 0;
    uint8_t ch_ = 1;
    bool busy_ = false, ok_ = false, failed_ = false;
    uint32_t done_ = 0, total_ = 0, bpf_ = 0;
    char dst_[128] = {};
};

#if defined(OT_BENCH_M4) || !defined(__arm__)
// Bench / tests: a reader that synthesises any frame of any bound file (a
// 16-bit saw + sine, interleaved when stereo); reads can fail or be delayed.
class FakeStreamReader final : public ot::audio::IStreamReader {
public:
    bool read(const ot::audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) override;
    static int16_t sample(uint8_t sid, uint32_t sampleIndex);   // the value at interleaved index
    uint32_t reads = 0;
    bool fail = false;
};
#endif

}  // namespace konsol
}  // namespace lab
