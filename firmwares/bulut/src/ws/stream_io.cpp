// BULUT TAPE FILE, main loop: binding a file, the resident head, the WAV
// conversion job and the bench / test reader (stream.h). Built for size.
#pragma GCC optimize("Os")
#include <cstdio>
#include <cstring>

#include "ws/stream.h"

namespace bulut {

namespace {
constexpr uint32_t kWin = 4096;        // converter window, frames per channel
constexpr uint32_t kFeedFrames = 512;  // source frames per feed (outputs stay under one window)
inline void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
}  // namespace

// ---- binding and the head ------------------------------------------------------------

bool Tapes::bind(int c, const char* file, uint32_t dataOffset, uint8_t ch, uint32_t frames, const char** why) {
    static const char* sink;
    if (!why) why = &sink;
    *why = "";
    if (!mem_ || !reader_ || c < 0 || c >= kTapes) {
        *why = "no card";
        return false;
    }
    unbind(c);
    Tape& t = tape_[c];
    ot::audio::StreamSource src;
    src.instrument = t.sid;
    src.frames = frames * ch;   // read as one plain run of int16 (interleaved when stereo)
    ot::audio::StreamBindInfo bi;
    if (!reader_->bind(src, file, dataOffset, false, &bi)) {
        *why = bi.reason && bi.reason[0] ? bi.reason : "read";
        return false;
    }
    t.ch = ch == 2 ? 2 : 1;
    t.frames = frames;
    t.avail = 0;
    t.fillGen = t.gen + 0x80000000u;
    barrier();
    t.bound = 1;
    return true;
}

bool Tapes::loadHead(int c, uint32_t from) {
    Tape& t = tape_[c];
    if (!mem_ || !reader_ || !t.bound) return false;
    t.headOk = 0;   // the audio side reads the ring (or misses) until the new head is complete
    barrier();
    if (from >= t.frames) from = 0;
    const uint32_t len = t.frames - from < kHeadFrames ? t.frames - from : kHeadFrames;
    const uint8_t ch = t.ch;
    const uint32_t per = kChunkSamples / ch;
    ot::audio::StreamSource src;
    src.instrument = t.sid;
    for (uint32_t f = 0; f < len;) {
        const uint32_t n = len - f < per ? len - f : per;
        if (!reader_->read(src, (from + f) * ch, n * ch, scratch_, nullptr)) {
            ++stats.readErrors;
            return false;
        }
        int16_t* h = t.head + f;
        if (ch == 2)
            for (uint32_t i = 0; i < n; ++i) h[i] = int16_t((int32_t(scratch_[2 * i]) + int32_t(scratch_[2 * i + 1])) >> 1);
        else
            std::memcpy(h, scratch_, n * sizeof(int16_t));
        f += n;
    }
    t.headBase = from;
    t.headLen = len;
    barrier();
    t.headOk = 1;
    return true;
}

// ---- conversion ------------------------------------------------------------------

bool StreamConv::init(lab::Arena& a) {
    win_[0] = static_cast<int16_t*>(a.alloc(kWin * 2));
    win_[1] = static_cast<int16_t*>(a.alloc(kWin * 2));
    stage_ = static_cast<int16_t*>(a.alloc(kWin * 2 * 2));
    io_ = static_cast<int16_t*>(a.alloc(kWin * 2 * 2));
    return win_[0] && win_[1] && stage_ && io_;
}

void StreamConv::sinkL(void* ctx, const int16_t* s, uint32_t n) {
    StreamConv& c = *static_cast<StreamConv*>(ctx);
    if (c.ch_ == 1) {   // mono: straight out
        if (c.fs_->write(c.out_, s, n * 2) != int32_t(n * 2)) c.failed_ = true;
        return;
    }
    if (c.stageN_ + n > kWin * 2) {   // cannot happen (kFeedFrames): never overflow
        c.failed_ = true;
        return;
    }
    std::memcpy(c.stage_ + c.stageN_, s, n * 2);
    c.stageN_ += n;
}

void StreamConv::sinkR(void* ctx, const int16_t* s, uint32_t n) {
    StreamConv& c = *static_cast<StreamConv*>(ctx);
    if (n > c.stageN_ || n > kWin) {   // the channels always produce the same counts
        c.failed_ = true;
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        c.io_[2 * i] = c.stage_[i];
        c.io_[2 * i + 1] = s[i];
    }
    c.stageN_ -= n;
    std::memmove(c.stage_, c.stage_ + n, c.stageN_ * 2);
    if (c.fs_->write(c.out_, c.io_, n * 4) != int32_t(n * 4)) c.failed_ = true;
}

bool StreamConv::start(ot::IStorage& fs, const char* src, const char* dst) {
    cancel();
    if (!win_[0]) return false;
    fs_ = &fs;
    in_ = fs.open(src, ot::IStorage::kRead);
    if (in_ < 0) return false;
    ot::WavInfo info;
    ot::StorageReader rd(fs, in_);
    if (!ot::wavParseHeader(rd, info) || !ot::wavIsSupported(info) || !fs.seek(in_, info.dataOffset)) {
        fs.close(in_);
        in_ = -1;
        return false;
    }
    ch_ = info.channels >= 2 ? 2 : 1;
    ot::WavImportOptions opt;
    opt.stereo = ch_ == 2;
    for (int c = 0; c < ch_; ++c)
        if (cv_[c].begin(info, opt, c, win_[c], kWin) != ot::FmtStatus::Ok) {
            fs.close(in_);
            in_ = -1;
            return false;
        }
    std::snprintf(dst_, sizeof dst_, "%s", dst);
    char tmp[136];
    std::snprintf(tmp, sizeof tmp, "%s.tmp", dst_);
    out_ = fs.open(tmp, ot::IStorage::kWrite);
    if (out_ < 0) {
        fs.close(in_);
        in_ = -1;
        return false;
    }
    const uint32_t data = cv_[0].outputFrames() * 2u * ch_;
    fs.reserve(out_, ot::wav::kHeaderSize + data);   // one contiguous run when the card has it (fewer extents)
    uint8_t hdr[ot::wav::kHeaderSize];
    ot::wavMakeHeader(hdr, ch_, data);
    if (fs.write(out_, hdr, sizeof hdr) != int32_t(sizeof hdr)) {
        finish(false);
        return false;
    }
    bpf_ = ot::wavBytesPerFrame(info);
    total_ = info.dataSize;
    done_ = 0;
    stageN_ = 0;
    failed_ = false;
    ok_ = false;
    busy_ = true;
    return true;
}

bool StreamConv::step(uint32_t bytes) {
    if (!busy_) return false;
    const uint32_t feed = kFeedFrames * bpf_;
    while (bytes && !failed_) {
        uint8_t src[2048];
        uint32_t want = feed < sizeof src ? feed : uint32_t(sizeof src);
        if (want > total_ - done_) want = total_ - done_;
        const int32_t got = want ? fs_->read(in_, src, want) : 0;
        if (got <= 0) {   // the end of the data (or of a short file): pad and close
            for (int c = 0; c < ch_; ++c) cv_[c].finish(c ? sinkR : sinkL, this);
            finish(!failed_);
            return false;
        }
        for (int c = 0; c < ch_; ++c) cv_[c].feed(src, size_t(got), c ? sinkR : sinkL, this);
        done_ += uint32_t(got);
        bytes = bytes > uint32_t(got) ? bytes - uint32_t(got) : 0;
    }
    if (failed_) {
        finish(false);
        return false;
    }
    return true;
}

void StreamConv::finish(bool ok) {
    char tmp[136];
    std::snprintf(tmp, sizeof tmp, "%s.tmp", dst_);
    if (out_ >= 0) {
        if (ok) fs_->truncate(out_);
        fs_->close(out_);
    }
    if (in_ >= 0) fs_->close(in_);
    in_ = out_ = -1;
    if (ok) {
        fs_->remove(dst_);
        ok = fs_->rename(tmp, dst_);
    }
    if (!ok && fs_) fs_->remove(tmp);
    ok_ = ok;
    busy_ = false;
}

void StreamConv::cancel() {
    if (busy_) finish(false);
}

// ---- bench / tests ------------------------------------------------------------------
#if defined(OT_BENCH_M4) || !defined(__arm__)
int16_t FakeStreamReader::sample(uint8_t sid, uint32_t i) {
    // a saw (period 401 samples) + a slow square, different per file
    const int32_t saw = int32_t((i * 163u + sid * 997u) % 401u) * 120 - 24000;
    const int32_t sq = ((i / 2205u) & 1u) ? 3000 : -3000;
    return int16_t(saw / 2 + sq);
}

bool FakeStreamReader::read(const ot::audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) {
    (void)r;
    if (fail) return false;
    ++reads;
    for (uint32_t i = 0; i < n; ++i) l[i] = sample(s.instrument, frame + i);
    return true;
}
#endif

}  // namespace bulut
