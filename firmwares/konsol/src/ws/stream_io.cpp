// KONSOL SD streaming, main loop: the WAV conversion job and the stream
// readers of each build (stream.h). Built for size.
#pragma GCC optimize("Os")
#include <cstdio>
#include <cstring>
#include <new>

#include "ws/stream.h"

#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
#include "hal/hal.h"
#include "hal/sd_storage.h"
#include "hal/sd_stream_reader.h"
#elif !defined(OT_BENCH_M4)
#include "app/storage_stream_reader.h"
#endif

namespace lab {
namespace konsol {

namespace {
constexpr uint32_t kWin = 4096;        // converter window, frames per channel
constexpr uint32_t kFeedFrames = 512;  // source frames per feed (outputs stay under one window)
}  // namespace

ot::audio::IStreamReader* makeStreamReader(Context& c) {
    if (!c.fs || !c.arena) return nullptr;
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
    // Upstream's device reader: raw sectors through the file's cluster map.
    // Its bindings live in .sdram_bss and are cleared by this constructor
    // (run here, long after boardInit() started the SDRAM: tools/check_ctors.py).
    void* m = c.arena->alloc(sizeof(ot::hal::SdStreamReader), 8);
    return m ? new (m) ot::hal::SdStreamReader(ot::hal::storage()) : nullptr;
#elif defined(OT_BENCH_M4)
    return nullptr;   // the bench installs a FakeStreamReader
#else
    void* m = c.arena->alloc(sizeof(ot::StorageStreamReader), 8);
    return m ? new (m) ot::StorageStreamReader(*c.fs) : nullptr;
#endif
}

// ---- conversion ------------------------------------------------------------------

bool StreamConv::init(Arena& a) {
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

bool StreamConv::start(IStorage& fs, const char* src, const char* dst) {
    cancel();
    if (!win_[0]) return false;
    fs_ = &fs;
    in_ = fs.open(src, IStorage::kRead);
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
    out_ = fs.open(tmp, IStorage::kWrite);
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

}  // namespace konsol
}  // namespace lab
