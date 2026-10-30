// KONSOL sample management (main loop): the shared pool, track samples,
// time-stretch jobs, drum kits (from slices or a folder of one-shots), kit
// files, garbage collection and compaction of the pool.
// Main loop only (no audio-interrupt code here): built for size.
#pragma GCC optimize("Os")
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "ws/kit.h"
#include "ws/konsol.h"
#include "ws/stretch.h"

namespace lab {
namespace konsol {

namespace {
bool hasWord(const char* name, const char* const* words) {
    char low[64];
    int n = 0;
    for (const char* p = name; *p && n < 63; ++p) low[n++] = char(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    low[n] = 0;
    for (int i = 0; words[i]; ++i)
        if (std::strstr(low, words[i])) return true;
    return false;
}
const char* baseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') b = p + 1;
    return b;
}
}  // namespace

// The class of a one-shot from its file name (-1: unknown).
int nameClass(const char* path) {
    static const char* const kK[] = {"kick", "kik", "bd", "bassdrum", "808", nullptr};
    static const char* const kS[] = {"snare", "snr", "sd", "clap", "cp", "rim", nullptr};
    static const char* const kH[] = {"hat", "hh", "oh", "ch", "ride", "cym", "crash", "shak", nullptr};
    const char* b = baseName(path);
    if (hasWord(b, kK)) return kClsKick;
    if (hasWord(b, kS)) return kClsSnare;
    if (hasWord(b, kH)) return kClsHat;
    return -1;
}

void Konsol::ensureBreak() {
    if (pool_.valid(breakSlot_)) return;
    breakSlot_ = int16_t(pool_.builtinBreak());
    if (breakSlot_ >= 0) {
        pool_.detectOnsets(breakSlot_, 0.5f);
    }
}

void Konsol::setTrackBreak(int t) {
    ensureBreak();
    TrackSample& ts = ts_[t];
    if (ts.stretched && ts.slot != ts.src) {
        const int old = ts.slot;
        ts.slot = ts.src;
        pool_.release(old);
    }
    ts.src = breakSlot_;
    ts.slot = breakSlot_;
    ts.stretched = 0;
    ts.bars = 2;
    int bars = 2;
    ts.srcBpm = loopBpm(pool_.valid(breakSlot_) ? pool_.slot(breakSlot_).frames : 1, bars, 172.0f,
                        pool_.valid(breakSlot_) ? &pool_.slot(breakSlot_) : nullptr);
    ts.outBpm = ts.srcBpm;
}

int Konsol::poolFind(const char* path, bool streamed) const {
    if (!path || !path[0]) return pool_.valid(breakSlot_) ? breakSlot_ : -1;
    for (int i = 0; i < kSlots; ++i) {
        const SampleSlot& s = pool_.slot(i);
        if (s.used && s.frames && !s.derived && (s.stream != 0) == streamed && std::strcmp(s.path, path) == 0) return i;
    }
    return -1;
}

int Konsol::poolLoad(const char* path) {
    const int have = poolFind(path);
    if (have >= 0) return have;
    if (!ctx_ || !ctx_->fs || !path || !path[0]) return -1;
    char err[48] = {};
    int s = pool_.loadWav(*ctx_->fs, path, 44100u * 60u, err, sizeof err);
    if (s < 0) {
        gcSamples();
        compactPool();
        s = pool_.loadWav(*ctx_->fs, path, 44100u * 60u, err, sizeof err);
    }
    if (s < 0) {
        std::snprintf(msg_, sizeof msg_, "%s", err);
        if (ctx_) ctx_->toast(err);
        return -1;
    }
    pool_.detectOnsets(s, 0.5f);
    return s;
}

bool Konsol::slotUsed(int s) const {
    if (s < 0) return false;
    if (s == breakSlot_) return true;
    if (str_ && str_->busy() && (s == str_->src() || s == str_->out())) return true;
    for (int t = 0; t < kTracks; ++t) {
        if (ts_[t].slot == s || ts_[t].src == s) return true;
        const KitPad* pads = kitPads(eng_[t].e[kEngKit]);
        if (pads)
            for (int k = 0; k < kKitPads; ++k)
                if (pads[k].slot == s) return true;
    }
    return false;
}

void Konsol::gcSamples() {
    for (int i = 0; i < kSlots; ++i)
        if (pool_.slot(i).used && !slotUsed(i)) pool_.release(i);
}

void Konsol::compactPool() {
    if (str_ && str_->busy()) return;   // the job writes by index: fine, but keep it simple
    const bool was = hold_;
    hold_ = true;   // the next interrupt outputs silence; single core: safe from here
    pool_.compact();
    hold_ = was;
}

bool Konsol::loadSample(int t, const char* path) {
    // SD streaming: a long file (or the track's Stream switch) plays from the card
    bool direct = false;
    uint32_t full = 0;
    if (wantStream(t, path, &direct, &full) && streamTrack(t, path, direct, full)) return true;
    if (str_ && str_->busy() && strTrack_ == t) str_->cancel();
    const int s = poolLoad(path);
    if (s < 0) return false;
    if (convPend_ & (1u << t)) convPend_ = uint8_t(convPend_ & ~(1u << t));
    if (convTrack_ == t && conv_) {
        conv_->cancel();
        convTrack_ = -1;
    }
    TrackSample& ts = ts_[t];
    const int oldSlot = ts.slot;
    ts.src = int16_t(s);
    ts.slot = int16_t(s);
    ts.stretched = 0;
    int bars = ts.bars;
    ts.srcBpm = loopBpm(pool_.length(s), bars, bpm_, &pool_.slot(s));
    ts.bars = uint8_t(bars);
    ts.outBpm = ts.srcBpm;
    (void)oldSlot;
    gcSamples();
    dirty_ = true;
    return true;
}

// ---- time stretch ------------------------------------------------------------------
const char* Konsol::stretchState(int t) const {
    if (str_ && str_->busy() && strTrack_ == t) return "working";
    return ts_[t].stretched ? "stretched" : "original";
}

bool Konsol::stretchTrack(int t) {
    if (!str_) return false;
    TrackSample& ts = ts_[t];
    if (!pool_.valid(ts.src)) return false;
    if (pool_.streamed(ts.src)) {
        if (ctx_) ctx_->toast("A streamed sample does not stretch: use the SAMPLER's Pitch");
        return false;
    }
    if (str_->busy()) str_->cancel();
    int bars = ts.bars;
    ts.srcBpm = loopBpm(pool_.slot(ts.src).frames, bars, bpm_, &pool_.slot(ts.src));
    ts.bars = uint8_t(bars);
    const float target = ctx_ ? ctx_->bpm() : bpm_;
    const float ratio = ts.srcBpm / (target > 20.0f ? target : 120.0f);
    if (ratio > 0.999f && ratio < 1.001f && ts.pitch == 0) {   // nothing to do
        unstretch(t);
        ts.outBpm = ts.srcBpm;
        return true;
    }
    bool ok = str_->start(pool_, ts.src, 0, pool_.slot(ts.src).frames, ratio, float(ts.pitch), ts.mode);
    if (!ok) {
        gcSamples();
        compactPool();
        ok = str_->start(pool_, ts.src, 0, pool_.slot(ts.src).frames, ratio, float(ts.pitch), ts.mode);
    }
    if (!ok) {
        if (ctx_) ctx_->toast("Not enough sample memory to stretch");
        return false;
    }
    strTrack_ = t;
    strTarget_ = target;
    return true;
}

void Konsol::unstretch(int t) {
    TrackSample& ts = ts_[t];
    if (ts.stretched && ts.slot != ts.src) {
        const int old = ts.slot;
        ts.slot = ts.src;
        pool_.release(old);
    }
    ts.stretched = 0;
    ts.outBpm = ts.srcBpm;
}

void Konsol::stretchService(uint32_t budgetFrames) {
    if (str_ && str_->busy()) {
        if (str_->step(budgetFrames)) return;   // still working
        // finished
        const int t = strTrack_;
        const int out = str_->out();
        strTrack_ = -1;
        if (t < 0 || t >= kTracks || !pool_.valid(out)) return;
        TrackSample& ts = ts_[t];
        const int old = ts.stretched ? ts.slot : -1;
        pool_.slot(out).derived = 1;
        ts.slot = int16_t(out);
        ts.stretched = 1;
        ts.outBpm = strTarget_;
        if (old >= 0 && old != out) pool_.release(old);
        dirty_ = true;
        std::snprintf(msg_, sizeof msg_, "T%d stretched to %d BPM", t + 1, int(strTarget_ + 0.5f));
        return;
    }
    // tracks waiting for a re-stretch (project load)
    for (int t = 0; t < kTracks; ++t)
        if (strPend_ & (1u << t)) {
            strPend_ = uint8_t(strPend_ & ~(1u << t));
            stretchTrack(t);
            return;
        }
}

float Konsol::stretchProgress() const { return str_ && str_->busy() ? str_->progress() : -1.0f; }

// ---- kits ----------------------------------------------------------------------------
namespace {
void sortByClass(KitPad* p, int n) {
    static const int kOrder[4] = {3, 0, 1, 2};   // kick, snare, hat, other
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && kOrder[p[j].cls & 3] < kOrder[p[j - 1].cls & 3]; --j) {
            const KitPad x = p[j];
            p[j] = p[j - 1];
            p[j - 1] = x;
        }
}
}  // namespace

int Konsol::kitFromSlices(int t, bool byClass) {
    TrackSample& ts = ts_[t];
    const int s = ts.src;
    if (!pool_.valid(s)) return 0;
    if (pool_.streamed(s)) {
        if (ctx_) ctx_->toast("A kit needs the sample in memory (Stream: Off)");
        return 0;
    }
    if (pool_.slot(s).nOn <= 1) pool_.detectOnsets(s, 0.5f);
    KitPad* pads = kitPads(eng_[t].e[kEngKit]);
    const SampleSlot& sl = pool_.slot(s);
    KitPad tmp[kKitPads];
    int n = 0;
    int count[4] = {};
    for (int k = 0; k < sl.nOn && n < kKitPads; ++k) {
        uint32_t a, e;
        if (!pool_.sliceRange(s, k, a, e)) continue;
        KitPad& p = tmp[n++];
        p = KitPad();
        p.slot = int16_t(s);
        p.a = a;
        p.e = e;
        p.cls = sl.cls[k];
        if (p.cls == kClsHat) p.choke = 1;
        std::snprintf(p.name, sizeof p.name, "%s %d", hitClassName(p.cls), ++count[p.cls & 3]);
    }
    if (byClass) sortByClass(tmp, n);
    for (int k = 0; k < kKitPads; ++k) pads[k] = k < n ? tmp[k] : KitPad();
    for (int k = n; k < kKitPads; ++k) std::snprintf(pads[k].name, sizeof pads[k].name, "PAD %d", k + 1);
    kitRefresh(eng_[t].e[kEngKit]);
    gcSamples();
    dirty_ = true;
    return n;
}

namespace {
struct WavList {
    char (*names)[40];
    int n, cap;
};
bool wavFn(const IStorage::DirEntry& e, void* p) {
    WavList& w = *static_cast<WavList*>(p);
    if (e.isDir || w.n >= w.cap) return w.n < w.cap;
    const size_t l = std::strlen(e.name);
    if (l < 5 || l >= 40) return true;
    const char* x = e.name + l - 4;
    if ((x[1] | 32) == 'w' && (x[2] | 32) == 'a' && (x[3] | 32) == 'v' && x[0] == '.')
        std::snprintf(w.names[w.n++], 40, "%s", e.name);
    return true;
}
}  // namespace

int Konsol::kitFromFolder(int t, const char* dir) {
    if (!ctx_ || !ctx_->fs || !dir) return 0;
    WavList wl{list_, 0, kKitPads};
    ctx_->fs->listDir(dir, wavFn, &wl);
    if (!wl.n) {
        if (ctx_) ctx_->toast("No WAV files in that folder");
        return 0;
    }
    // name order first (01 kick, 02 snare ...), then by class
    for (int i = 1; i < wl.n; ++i)
        for (int j = i; j > 0 && std::strcmp(wl.names[j], wl.names[j - 1]) < 0; --j) {
            char x[40];
            std::memcpy(x, wl.names[j], 40);
            std::memcpy(wl.names[j], wl.names[j - 1], 40);
            std::memcpy(wl.names[j - 1], x, 40);
        }
    KitPad tmp[kKitPads];
    int n = 0;
    for (int i = 0; i < wl.n; ++i) {
        char path[160];
        std::snprintf(path, sizeof path, "%s/%s", dir, wl.names[i]);
        const int s = poolLoad(path);
        if (s < 0) continue;
        KitPad& p = tmp[n++];
        p = KitPad();
        p.slot = int16_t(s);
        const int nc = nameClass(path);
        p.cls = uint8_t(nc >= 0 ? nc : (pool_.slot(s).nOn ? pool_.slot(s).cls[0] : kClsOther));
        if (p.cls == kClsHat) p.choke = 1;
        // the stem of the file name
        char stem[40];
        std::snprintf(stem, sizeof stem, "%s", wl.names[i]);
        char* dot = std::strrchr(stem, '.');
        if (dot) *dot = 0;
        for (char* q = stem; *q; ++q)
            if (*q >= 'a' && *q <= 'z') *q = char(*q - 32);
        std::snprintf(p.name, sizeof p.name, "%s", stem);
    }
    sortByClass(tmp, n);
    KitPad* pads = kitPads(eng_[t].e[kEngKit]);
    for (int k = 0; k < kKitPads; ++k) pads[k] = k < n ? tmp[k] : KitPad();
    for (int k = n; k < kKitPads; ++k) std::snprintf(pads[k].name, sizeof pads[k].name, "PAD %d", k + 1);
    kitRefresh(eng_[t].e[kEngKit]);
    gcSamples();
    dirty_ = true;
    return n;
}

bool Konsol::padSample(int t, int pad, const char* path) {
    KitPad* pads = kitPads(eng_[t].e[kEngKit]);
    if (!pads || pad < 0 || pad >= kKitPads) return false;
    const int s = poolLoad(path);
    if (s < 0) return false;
    KitPad& p = pads[pad];
    const KitPad old = p;
    p = KitPad();
    p.slot = int16_t(s);
    p.choke = old.choke;
    const int nc = nameClass(path);
    p.cls = uint8_t(nc >= 0 ? nc : (pool_.slot(s).nOn ? pool_.slot(s).cls[0] : kClsOther));
    char stem[40];
    std::snprintf(stem, sizeof stem, "%s", baseName(path));
    char* dot = std::strrchr(stem, '.');
    if (dot) *dot = 0;
    for (char* q = stem; *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = char(*q - 32);
    std::snprintf(p.name, sizeof p.name, "%s", stem);
    kitRefresh(eng_[t].e[kEngKit]);
    gcSamples();
    dirty_ = true;
    return true;
}

// Kit record (shared by .kit files and the project's KIT chunk):
//   char path[96]; u32 a, e; f32 tune, fine, gain, pan, decay, filter, res;
//   u8 choke, rev, cls; char name[14]
constexpr uint32_t kKitRec = 96 + 8 + 28 + 3 + 14;

void Konsol::kitWrite(int t, uint8_t* out) const {
    const KitPad* pads = kitPads(eng_[t].e[kEngKit]);
    for (int k = 0; k < kKitPads; ++k) {
        const KitPad& p = pads[k];
        uint8_t* o = out + uint32_t(k) * kKitRec;
        std::memset(o, 0, kKitRec);
        if (pool_.valid(p.slot) && p.slot != breakSlot_) std::snprintf(reinterpret_cast<char*>(o), 96, "%s", pool_.slot(p.slot).path);
        if (p.slot < 0) o[0] = '-';   // empty pad ("" = the built-in break)
        std::memcpy(o + 96, &p.a, 4);
        std::memcpy(o + 100, &p.e, 4);
        const float f[7] = {p.tune, p.fine, p.gain, p.pan, p.decay, p.filter, p.res};
        std::memcpy(o + 104, f, 28);
        o[132] = p.choke;
        o[133] = p.rev;
        o[134] = p.cls;
        std::memcpy(o + 135, p.name, 14);
    }
}

void Konsol::kitRead(int t, const uint8_t* in) {
    KitPad* pads = kitPads(eng_[t].e[kEngKit]);
    for (int k = 0; k < kKitPads; ++k) {
        const uint8_t* o = in + uint32_t(k) * kKitRec;
        KitPad& p = pads[k];
        p = KitPad();
        char path[96];
        std::memcpy(path, o, 96);
        path[95] = 0;
        if (path[0] == '-' && !path[1]) p.slot = -1;
        else if (!path[0]) {
            ensureBreak();
            p.slot = breakSlot_;
        } else {
            p.slot = int16_t(poolLoad(path));
        }
        std::memcpy(&p.a, o + 96, 4);
        std::memcpy(&p.e, o + 100, 4);
        float f[7];
        std::memcpy(f, o + 104, 28);
        auto ok = [](float v, float lo, float hi, float d) { return (v >= lo && v <= hi) ? v : d; };
        p.tune = ok(f[0], -24, 24, 0);
        p.fine = ok(f[1], -1, 1, 0);
        p.gain = ok(f[2], 0, 2, 1);
        p.pan = ok(f[3], -1, 1, 0);
        p.decay = ok(f[4], 0.02f, 1, 1);
        p.filter = ok(f[5], -1, 1, 0);
        p.res = ok(f[6], 0, 1, 0.15f);
        p.choke = uint8_t(o[132] % 5);
        p.rev = o[133] ? 1 : 0;
        p.cls = uint8_t(o[134] & 3);
        std::memcpy(p.name, o + 135, 14);
        p.name[13] = 0;
        if (pool_.valid(p.slot)) {
            const uint32_t fr = pool_.slot(p.slot).frames;
            if (p.e > fr) p.e = fr;
            if (p.a >= fr) p.a = 0;
        }
    }
    kitRefresh(eng_[t].e[kEngKit]);
}

uint32_t Konsol::kitBytes() { return kKitRec * kKitPads; }

bool Konsol::saveKit(int t, const char* name) {
    if (!ctx_ || !ctx_->fs || !name || !name[0]) return false;
    char path[96];
    std::snprintf(path, sizeof path, "%s/Kits", ctx_->home);
    ctx_->fs->mkdir(path);
    std::snprintf(path, sizeof path, "%s/Kits/%s.kit", ctx_->home, name);
    const IStorage::Handle h = ctx_->fs->open(path, IStorage::kWrite);
    if (h < 0) return false;
    static uint8_t buf[kKitRec * kKitPads + 8];
    std::memcpy(buf, "KKIT", 4);
    buf[4] = 1;
    buf[5] = 0;
    buf[6] = kKitPads;
    buf[7] = 0;
    kitWrite(t, buf + 8);
    const bool ok = ctx_->fs->write(h, buf, sizeof buf) == int32_t(sizeof buf);
    ctx_->fs->close(h);
    return ok;
}

bool Konsol::loadKit(int t, const char* path) {
    if (!ctx_ || !ctx_->fs || !path) return false;
    const IStorage::Handle h = ctx_->fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    static uint8_t buf[kKitRec * kKitPads + 8];
    int32_t n = 0;
    while (n < int32_t(sizeof buf)) {   // cards (and the host fake) may read short
        const int32_t k = ctx_->fs->read(h, buf + n, uint32_t(sizeof buf) - uint32_t(n));
        if (k <= 0) break;
        n += k;
    }
    ctx_->fs->close(h);
    if (n != int32_t(sizeof buf) || std::memcmp(buf, "KKIT", 4) != 0 || buf[6] != kKitPads) {
        if (ctx_) ctx_->toast("Not a KONSOL kit");
        return false;
    }
    kitRead(t, buf + 8);
    gcSamples();
    dirty_ = true;
    return true;
}

// ---- SD streaming (stream.h) ------------------------------------------------------

namespace {
bool streamable16(const ot::WavInfo& i) {
    return i.format == ot::wav::kFormatPcm && i.bitsPerSample == 16 && i.sampleRate == 44100 &&
           (i.channels == 1 || i.channels == 2);
}
bool probeWav(IStorage& fs, const char* path, ot::WavInfo& info) {
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return false;
    ot::StorageReader rd(fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    fs.close(h);
    return ok;
}
uint32_t ctxMillis(void* c) { return static_cast<Context*>(c)->millis(); }
}  // namespace

// Should track t stream `path`? (direct: the file streams as it is; frames:
// its length at 44.1 kHz.)
int Konsol::wantStream(int t, const char* path, bool* direct, uint32_t* frames) {
    *direct = false;
    *frames = 0;
    const int mode = ts_[t].stream;
    if (mode == kStreamOff || !ctx_ || !ctx_->fs || !path || !path[0] || !streams_ || !streams_->ready() ||
        !streams_->reader())
        return 0;
    ot::WavInfo info;
    if (!probeWav(*ctx_->fs, path, info)) return 0;
    const uint32_t full = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
    const uint8_t ch = info.channels >= 2 ? 2 : 1;
    *direct = streamable16(info);
    *frames = full;
    if (full < kStreamMinFrames) return 0;
    if (mode == kStreamOn) return 1;
    // Auto: long files, files over the in-memory limit and files that do not fit
    return full >= kStreamAutoFrames || full > 44100u * 60u || full + 64u > pool_.freeFrames(ch) ? 1 : 0;
}

void Konsol::convPath(const char* src, uint32_t frames, char* out, int cap) const {
    // <home>/Stream/<file stem>_<FNV-1a of the path and the length>.wav
    uint32_t h = 2166136261u ^ frames;
    for (const char* p = src; *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
    const char* b = src;
    for (const char* p = src; *p; ++p)
        if (*p == '/') b = p + 1;
    char stem[25];
    int n = 0;
    for (const char* p = b; *p && *p != '.' && n < 24; ++p) {
        const char c = *p;
        const bool okc = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem[n++] = okc ? c : '_';
    }
    stem[n] = 0;
    std::snprintf(out, size_t(cap), "%s/Stream/%s_%08lX.wav", ctx_ ? ctx_->home : "", stem, static_cast<unsigned long>(h));
}

int Konsol::streamOpen(const char* orig, const char* file, bool quiet) {
    if (!ctx_ || !ctx_->fs || !streams_ || !streams_->reader()) return -1;
    const int have = poolFind(orig, true);
    if (have >= 0) return have;
    IStorage& fs = *ctx_->fs;
    ot::WavInfo info;
    if (!probeWav(fs, file, info) || !streamable16(info)) return -1;
    const uint8_t ch = uint8_t(info.channels);
    const uint32_t frames = ot::wavSourceFrames(info);
    // a free binding (16 files at once)
    uint32_t usedSid = 0;
    for (int i = 0; i < kSlots; ++i)
        if (pool_.slot(i).used && pool_.slot(i).stream) usedSid |= 1u << (pool_.slot(i).sid & 31);
    int sid = 0;
    while (sid < kStreamFiles && (usedSid >> sid) & 1u) ++sid;
    if (sid >= kStreamFiles) {
        if (!quiet) ctx_->toast("16 files already stream from the card");
        return -1;
    }
    const uint32_t head = frames < kHeadFrames ? frames : kHeadFrames;
    int s = pool_.alloc(head, ch);
    if (s < 0) {
        gcSamples();
        compactPool();
        s = pool_.alloc(head, ch);
    }
    if (s < 0) {
        if (!quiet) ctx_->toast("Sample memory is full (the stream head)");
        return -1;
    }
    // the head: interleaved in, planar into the slot
    bool ok = false;
    const IStorage::Handle h = fs.open(file, IStorage::kRead);
    if (h >= 0 && fs.seek(h, info.dataOffset)) {
        int16_t* L = pool_.leftW(s);
        int16_t* R = pool_.rightW(s);
        int16_t buf[1024];
        uint32_t f = 0;
        ok = true;
        while (f < head && ok) {
            uint32_t n = uint32_t(sizeof buf / 2) / ch;
            if (n > head - f) n = head - f;
            uint32_t want = n * ch * 2u, got = 0;
            while (got < want) {   // cards (and the host fake) may read short
                const int32_t k = fs.read(h, reinterpret_cast<uint8_t*>(buf) + got, want - got);
                if (k <= 0) break;
                got += uint32_t(k);
            }
            if (got != want) ok = false;
            for (uint32_t i = 0; i < n && ok; ++i) {
                L[f + i] = buf[i * ch];
                if (ch == 2) R[f + i] = buf[i * 2 + 1];
            }
            f += n;
        }
    }
    if (h >= 0) fs.close(h);
    ot::audio::StreamSource src;
    src.instrument = uint8_t(sid);
    src.frames = frames * ch;
    ot::audio::StreamBindInfo bi;
    if (ok) ok = streams_->reader()->bind(src, file, info.dataOffset, false, &bi);
    if (!ok) {
        pool_.release(s);
        if (!quiet) {
            char m[48];
            std::snprintf(m, sizeof m, "Cannot stream the file (%s)", bi.reason[0] ? bi.reason : "read");
            if (!std::strcmp(bi.reason, "fragmented")) std::snprintf(m, sizeof m, "Card too fragmented: copy the file again");
            ctx_->toast(m);
        }
        return -1;
    }
    SampleSlot& sl = pool_.slot(s);
    copyStr(sl.path, sizeof sl.path, orig);
    sl.sid = uint8_t(sid);
    sl.sframes = frames;
    sl.stream = 1;
    sl.frames = head;   // last: the slot becomes valid
    pool_.detectOnsets(s, 0.5f);
    return s;
}

// Track t streams `path`: at once when the file is a 16-bit 44.1 kHz WAV (or
// its converted copy exists), else after the conversion job. False: it
// cannot stream (the caller loads it into memory).
bool Konsol::streamTrack(int t, const char* path, bool direct, uint32_t frames) {
    char file[128];
    if (direct) std::snprintf(file, sizeof file, "%s", path);
    else convPath(path, frames, file, sizeof file);
    int s = -1;
    if (direct || ctx_->fs->exists(file)) s = streamOpen(path, file, !direct);
    if (s < 0 && !direct) {   // convert first (one job at a time; the track keeps its sample meanwhile)
        if (!conv_) return false;
        std::snprintf(convSrc_[t], sizeof convSrc_[t], "%s", path);
        convFrames_[t] = frames;
        if (convTrack_ == t) {
            conv_->cancel();
            convTrack_ = -1;
        }
        convPend_ = uint8_t(convPend_ | (1u << t));
        startConv();
        if (ctx_) ctx_->toast("Preparing the file for streaming (16-bit copy)...");
        return true;
    }
    if (s < 0) return false;
    if (convPend_ & (1u << t)) convPend_ = uint8_t(convPend_ & ~(1u << t));
    assignStream(t, s);
    return true;
}

void Konsol::assignStream(int t, int s) {
    TrackSample& ts = ts_[t];
    if (ts.stretched && ts.slot != ts.src) {
        const int old = ts.slot;
        ts.slot = ts.src;
        pool_.release(old);
    }
    ts.src = int16_t(s);
    ts.slot = int16_t(s);
    ts.stretched = 0;
    int bars = ts.bars;
    ts.srcBpm = loopBpm(pool_.length(s), bars, bpm_, nullptr);
    ts.bars = uint8_t(bars);
    ts.outBpm = ts.srcBpm;
    gcSamples();
    dirty_ = true;
}

void Konsol::startConv() {
    if (!conv_ || conv_->busy() || !ctx_ || !ctx_->fs) return;
    for (int t = 0; t < kTracks; ++t) {
        if (!(convPend_ & (1u << t))) continue;
        char dst[128];
        convPath(convSrc_[t], convFrames_[t], dst, sizeof dst);
        char dir[96];
        std::snprintf(dir, sizeof dir, "%s/Stream", ctx_->home);
        ctx_->fs->mkdir(dir);
        if (conv_->start(*ctx_->fs, convSrc_[t], dst)) {
            convTrack_ = t;
            return;
        }
        convPend_ = uint8_t(convPend_ & ~(1u << t));
        ctx_->toast("Cannot convert the file for streaming");
    }
}

float Konsol::convProgress(int t) const {
    if (convTrack_ == t && conv_ && conv_->busy()) return conv_->progress();
    return (convPend_ >> t) & 1u ? 0.0f : -1.0f;
}

void Konsol::streamService() {
    if (!streams_) return;
    // read-ahead first: up to 12 ms of card reads a pass (a chunk is ~2-6 ms)
    streams_->service(pool_, ctxMillis, ctx_, 12, 24);
    if (!conv_ || !ctx_) return;
    if (conv_->busy()) {
        const uint32_t t0 = ctx_->millis();
        bool more = true;
        for (int i = 0; i < 64 && more; ++i) {   // 64 x 4 KB, or ~10 ms
            more = conv_->step(4096);
            if (ctx_->millis() - t0 >= 10) break;
        }
        if (more) return;
        const int t = convTrack_;
        convTrack_ = -1;
        if (t >= 0 && (convPend_ & (1u << t))) {
            convPend_ = uint8_t(convPend_ & ~(1u << t));
            const int s = conv_->ok() ? streamOpen(convSrc_[t], conv_->dst()) : -1;
            if (s >= 0) assignStream(t, s);
            if (s < 0) {   // fall back: as much as fits in memory
                ctx_->toast("Cannot stream the file: loaded into memory");
                const int s = poolLoad(convSrc_[t]);
                if (s >= 0) {
                    ts_[t].src = ts_[t].slot = int16_t(s);
                    ts_[t].stretched = 0;
                    int bars = ts_[t].bars;
                    ts_[t].srcBpm = ts_[t].outBpm = loopBpm(pool_.slot(s).frames, bars, bpm_, &pool_.slot(s));
                    ts_[t].bars = uint8_t(bars);
                }
            } else {
                std::snprintf(msg_, sizeof msg_, "T%d streams from the card", t + 1);
            }
        }
    }
    if (convPend_ && !conv_->busy()) startConv();
}

}  // namespace konsol
}  // namespace lab
