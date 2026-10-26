// KONSOL sample management (main loop): the shared pool, track samples,
// time-stretch jobs, drum kits (from slices or a folder of one-shots), kit
// files, garbage collection and compaction of the pool.
#include <cstdio>
#include <cstring>

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
    ts.srcBpm = loopBpm(pool_.valid(breakSlot_) ? pool_.slot(breakSlot_).frames : 1, bars, 172.0f);
    ts.outBpm = ts.srcBpm;
}

int Konsol::poolFind(const char* path) const {
    if (!path || !path[0]) return pool_.valid(breakSlot_) ? breakSlot_ : -1;
    for (int i = 0; i < kSlots; ++i) {
        const SampleSlot& s = pool_.slot(i);
        if (s.used && s.frames && !s.derived && std::strcmp(s.path, path) == 0) return i;
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
    const int s = poolLoad(path);
    if (s < 0) return false;
    TrackSample& ts = ts_[t];
    const int oldSlot = ts.slot;
    ts.src = int16_t(s);
    ts.slot = int16_t(s);
    ts.stretched = 0;
    int bars = ts.bars;
    ts.srcBpm = loopBpm(pool_.slot(s).frames, bars, bpm_);
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
    if (str_->busy()) str_->cancel();
    int bars = ts.bars;
    ts.srcBpm = loopBpm(pool_.slot(ts.src).frames, bars, bpm_);
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
    const int32_t n = ctx_->fs->read(h, buf, sizeof buf);
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

}  // namespace konsol
}  // namespace lab
