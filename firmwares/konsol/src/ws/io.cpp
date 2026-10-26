// KONSOL main-loop services: project files, samples, live generators, meters,
// MIDI in, hand play and recording, settings rows.
//
// Project file  <home>/Projects/<NAME>.kon  (little-endian):
//   "KONSOL" u16 version (1)
//   chunks: char tag[4], u32 length, payload -- unknown tags are skipped, so a
//   newer file still loads its known parts.
//     HEAD  name[16] f32 bpm, key, scale, swing, launchQ, recQ, metronome, songLoop
//     MFX   FxCfg fields (f32 / u8, fixed order)
//     TRK   u8 track, engine, octave; Mix fields; char samplePath[96]
//     ENG   u8 track, kind, u16 n, f32 value[n] (every parameter of that engine)
//     CLIP  u8 track, scene, lenSteps, gen, live, every; i16 gp[6]; u16 count;
//           count x {u16 start, u16 len, u8 pitch, vel, i8 tone, u8 chance, roll, flags}
//     SONG  u8 count, loop; count x {u8 scene, u8 bars}
//     MSTR  f32 master level
//     PREF  u8 pad brightness
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "ws/konsol.h"

namespace lab {
namespace konsol {

void makamSetKey(Engine* e, int key);

namespace {

constexpr uint16_t kVersion = 1;

struct Writer {
    Writer(IStorage* f, IStorage::Handle hh) : fs(f), h(hh) {}
    IStorage* fs;
    IStorage::Handle h;
    uint8_t buf[2048];
    uint32_t n = 0;
    bool ok = true;
    void flush() {
        if (n && fs->write(h, buf, n) != int32_t(n)) ok = false;
        n = 0;
    }
    void put(const void* p, uint32_t len) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        while (len) {
            uint32_t k = sizeof buf - n;
            if (k > len) k = len;
            std::memcpy(buf + n, b, k);
            n += k;
            b += k;
            len -= k;
            if (n == sizeof buf) flush();
        }
    }
    void u8(uint8_t v) { put(&v, 1); }
    void i8(int8_t v) { put(&v, 1); }
    void u16(uint16_t v) { put(&v, 2); }
    void i16(int16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    void chunk(const char* tag, uint32_t len) {
        put(tag, 4);
        u32(len);
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* e;
    bool ok = true;
    void get(void* d, uint32_t n) {
        if (uint32_t(e - p) < n) {
            ok = false;
            std::memset(d, 0, n);
            return;
        }
        std::memcpy(d, p, n);
        p += n;
    }
    uint8_t u8() { uint8_t v; get(&v, 1); return v; }
    int8_t i8() { int8_t v; get(&v, 1); return v; }
    uint16_t u16() { uint16_t v; get(&v, 2); return v; }
    int16_t i16() { int16_t v; get(&v, 2); return v; }
    uint32_t u32() { uint32_t v; get(&v, 4); return v; }
    float f32() { float v; get(&v, 4); return v; }
};

float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); }

}  // namespace

bool Konsol::saveProject(const char* name) {
    if (!ctx_ || !ctx_->fs || !name || !name[0]) return false;
    IStorage* fs = ctx_->fs;
    char path[96], tmp[100];
    std::snprintf(path, sizeof path, "%s/Projects/%s.kon", ctx_->home, name);
    std::snprintf(tmp, sizeof tmp, "%s/Projects/%s.tmp", ctx_->home, name);
    const IStorage::Handle h = fs->open(tmp, IStorage::kWrite);
    if (h < 0) return false;
    Writer w(fs, h);
    Project& p = *P_;
    p.bpm = ctx_->bpm();
    w.put("KONSOL", 6);
    w.u16(kVersion);
    // HEAD
    w.chunk("HEAD", 16 + 4 + 7);
    w.put(p.name, 16);
    w.f32(p.bpm);
    w.u8(p.key);
    w.u8(p.scale);
    w.u8(p.swing);
    w.u8(p.launchQ);
    w.u8(p.recQ);
    w.u8(p.metronome);
    w.u8(p.songLoop);
    // MFX
    const FxCfg& f = p.fx;
    w.chunk("MFX ", 2 + 4 * 9 + 1);
    w.u8(f.delaySync);
    w.u8(f.pingPong);
    w.f32(f.delayFb);
    w.f32(f.delayTone);
    w.f32(f.revSize);
    w.f32(f.revDamp);
    w.f32(f.retA);
    w.f32(f.retB);
    w.f32(f.compThr);
    w.f32(f.compRatio);
    w.f32(f.compMakeup);
    w.u8(f.limiter);
    // the master level last (added in the same version: optional on load)
    // TRK + ENG
    for (int t = 0; t < kTracks; ++t) {
        const TrackCfg& tc = p.track[t];
        w.chunk("TRK ", 3 + 4 * 4 + 4 + 96);
        w.u8(uint8_t(t));
        w.u8(tc.engine);
        w.u8(tc.octave);
        w.f32(tc.mix.level);
        w.f32(tc.mix.pan);
        w.f32(tc.mix.sendA);
        w.f32(tc.mix.sendB);
        w.u8(tc.mix.mute);
        w.u8(tc.mix.solo);
        w.u8(tc.mix.stem);
        w.u8(tc.mix.midiOut);
        w.put(smp_[t].path, 96);
        for (int k = 0; k < kEngCount; ++k) {
            Engine* e = eng_[t].e[k];
            const int n = e->paramCount();
            w.chunk("ENG ", 4 + 4 * uint32_t(n));
            w.u8(uint8_t(t));
            w.u8(uint8_t(k));
            w.u16(uint16_t(n));
            for (int i = 0; i < n; ++i) w.f32(e->get(i));
        }
    }
    // CLIP
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kScenes; ++s) {
            const Clip& c = p.clip[t][s];
            if (!c.exists()) continue;
            const NoteBuf& nb = c.notes();
            w.chunk("CLIP", 6 + 12 + 2 + uint32_t(nb.count) * 10);
            w.u8(uint8_t(t));
            w.u8(uint8_t(s));
            w.u8(c.lenSteps);
            w.u8(c.gen);
            w.u8(c.live);
            w.u8(c.every);
            for (int i = 0; i < kGenParams; ++i) w.i16(c.gp[i]);
            w.u16(nb.count);
            for (int i = 0; i < nb.count; ++i) {
                const Note& n = nb.n[i];
                w.u16(n.start);
                w.u16(n.len);
                w.u8(n.pitch);
                w.u8(n.vel);
                w.i8(n.tone);
                w.u8(n.chance);
                w.u8(n.roll);
                w.u8(uint8_t(n.flags & 7));
            }
        }
    // SONG
    w.chunk("SONG", 2 + 2 * uint32_t(p.songLen));
    w.u8(p.songLen);
    w.u8(p.songLoop);
    for (int i = 0; i < p.songLen; ++i) {
        w.u8(p.song[i].scene);
        w.u8(p.song[i].bars);
    }
    w.chunk("MSTR", 4);
    w.f32(f.master);
    w.chunk("PREF", 1);
    w.u8(uint8_t(setPadBright_));
    w.flush();
    fs->close(h);
    if (!w.ok) {
        fs->remove(tmp);
        return false;
    }
    fs->remove(path);
    if (!fs->rename(tmp, path)) return false;
    // remember it
    std::snprintf(path, sizeof path, "%s/last.txt", ctx_->home);
    const IStorage::Handle hl = fs->open(path, IStorage::kWrite);
    if (hl >= 0) {
        fs->write(hl, name, uint32_t(std::strlen(name)));
        fs->close(hl);
    }
    dirty_ = false;
    return true;
}

bool Konsol::loadProject(const char* name) {
    if (!ctx_ || !ctx_->fs || !name || !name[0]) return false;
    IStorage* fs = ctx_->fs;
    char path[96];
    std::snprintf(path, sizeof path, "%s/Projects/%s.kon", ctx_->home, name);
    const IStorage::Handle h = fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    const int32_t size = fs->size(h);
    // the file goes through the stutter ring as scratch (main loop, stopped)
    const uint32_t cap = uint32_t(kStutterMax) * 4u;
    if (size < 8 || uint32_t(size) > cap) {
        fs->close(h);
        return false;
    }
    // stop everything first: the audio must not read the project while it changes
    ctx_->setPlaying(false);
    hold_ = true;
    for (int t = 0; t < kTracks; ++t) {
        rt_[t].play = -1;
        rt_[t].queued = -2;
        rt_[t].nev = 0;
    }
    uint8_t* buf = reinterpret_cast<uint8_t*>(stutter_);
    stutHeld_ = false;
    int32_t got = 0;
    while (got < size) {
        const int32_t n = fs->read(h, buf + got, uint32_t(size - got));
        if (n <= 0) break;
        got += n;
    }
    fs->close(h);
    if (got != size || std::memcmp(buf, "KONSOL", 6) != 0) {
        std::memset(stutter_, 0, uint32_t(kStutterMax) * 4u);
        hold_ = false;
        return false;
    }
    Reader r{buf + 6, buf + size};
    const uint16_t ver = r.u16();
    if (ver < 1) {
        std::memset(stutter_, 0, uint32_t(kStutterMax) * 4u);
        hold_ = false;
        return false;
    }
    newProject(false);
    Project& p = *P_;
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kScenes; ++s) p.clip[t][s].lenSteps = 0;
    char samplePath[kTracks][96] = {};
    while (r.ok && r.p + 8 <= r.e) {
        char tag[4];
        r.get(tag, 4);
        const uint32_t len = r.u32();
        if (uint32_t(r.e - r.p) < len) break;
        Reader c{r.p, r.p + len};
        r.p += len;
        if (!std::memcmp(tag, "HEAD", 4)) {
            c.get(p.name, 16);
            p.name[15] = 0;
            p.bpm = clampf(c.f32(), 20.0f, 400.0f);
            p.key = uint8_t(c.u8() % 12);
            p.scale = uint8_t(c.u8() % kScaleCount);
            p.swing = c.u8();
            if (p.swing < 50 || p.swing > 75) p.swing = 50;
            p.launchQ = uint8_t(c.u8() % 5);
            p.recQ = uint8_t(c.u8() % 4);
            p.metronome = c.u8() ? 1 : 0;
            p.songLoop = c.u8() ? 1 : 0;
        } else if (!std::memcmp(tag, "MFX ", 4)) {
            FxCfg& f = p.fx;
            f.delaySync = uint8_t(c.u8() % 6);
            f.pingPong = c.u8() ? 1 : 0;
            f.delayFb = clampf(c.f32(), 0, 0.95f);
            f.delayTone = clampf(c.f32(), 0, 1);
            f.revSize = clampf(c.f32(), 0, 1);
            f.revDamp = clampf(c.f32(), 0, 1);
            f.retA = clampf(c.f32(), 0, 1.5f);
            f.retB = clampf(c.f32(), 0, 1.5f);
            f.compThr = clampf(c.f32(), 0, 1);
            f.compRatio = clampf(c.f32(), 0, 1);
            f.compMakeup = clampf(c.f32(), 0, 1);
            f.limiter = c.u8() ? 1 : 0;
        } else if (!std::memcmp(tag, "PREF", 4)) {
            setPadBright_ = c.u8() % 3;
        } else if (!std::memcmp(tag, "MSTR", 4)) {
            p.fx.master = clampf(c.f32(), 0, 1.25f);
        } else if (!std::memcmp(tag, "TRK ", 4)) {
            const int t = c.u8();
            if (t >= kTracks) continue;
            TrackCfg& tc = p.track[t];
            tc.engine = uint8_t(c.u8() % kEngCount);
            tc.octave = uint8_t(c.u8() % 9);
            tc.mix.level = clampf(c.f32(), 0, 1.25f);
            tc.mix.pan = clampf(c.f32(), -1, 1);
            tc.mix.sendA = clampf(c.f32(), 0, 1);
            tc.mix.sendB = clampf(c.f32(), 0, 1);
            tc.mix.mute = c.u8() ? 1 : 0;
            tc.mix.solo = c.u8() ? 1 : 0;
            tc.mix.stem = uint8_t(c.u8() % 5);
            tc.mix.midiOut = c.u8() ? 1 : 0;
            c.get(samplePath[t], 96);
            samplePath[t][95] = 0;
        } else if (!std::memcmp(tag, "ENG ", 4)) {
            const int t = c.u8(), k = c.u8();
            const int n = c.u16();
            if (t >= kTracks || k >= kEngCount) continue;
            Engine* e = eng_[t].e[k];
            for (int i = 0; i < n && c.ok; ++i) {
                const float v = c.f32();
                if (i < e->paramCount() && v == v) e->set(i, v);
            }
        } else if (!std::memcmp(tag, "CLIP", 4)) {
            const int t = c.u8(), s = c.u8();
            if (t >= kTracks || s >= kScenes) continue;
            Clip& cl = p.clip[t][s];
            cl.lenSteps = c.u8();
            if (cl.lenSteps > kMaxSteps) cl.lenSteps = kMaxSteps;
            cl.gen = uint8_t(c.u8() % kGenCount);
            cl.live = c.u8() ? 1 : 0;
            cl.every = c.u8();
            if (cl.every < 1 || cl.every > 8) cl.every = 1;
            for (int i = 0; i < kGenParams; ++i) cl.gp[i] = c.i16();
            int n = c.u16();
            cl.cur = 0;
            cl.pending = 0;
            NoteBuf& nb = cl.buf[0];
            nb.clear();
            if (n > kMaxNotes) n = kMaxNotes;
            const uint32_t maxTick = uint32_t(cl.lenSteps) * kTps;
            for (int i = 0; i < n && c.ok; ++i) {
                Note x;
                x.start = c.u16();
                x.len = c.u16();
                x.pitch = uint8_t(c.u8() & 127);
                x.vel = c.u8();
                x.tone = c.i8();
                x.chance = c.u8();
                x.roll = c.u8();
                x.flags = uint8_t(c.u8() & 7);
                if (x.chance > 100) x.chance = 100;
                if (x.roll > 16) x.roll = 16;
                if (x.vel < 1) x.vel = 1;
                if (x.vel > 127) x.vel = 127;
                if (x.len < 1) x.len = 1;
                if (x.start < maxTick) nb.add(x);
            }
        } else if (!std::memcmp(tag, "SONG", 4)) {
            int n = c.u8();
            p.songLoop = c.u8() ? 1 : 0;
            if (n > kSongMax) n = kSongMax;
            p.songLen = 0;
            for (int i = 0; i < n && c.ok; ++i) {
                p.song[i].scene = uint8_t(c.u8() % kScenes);
                p.song[i].bars = c.u8();
                if (p.song[i].bars < 1 || p.song[i].bars > 64) p.song[i].bars = 4;
                p.songLen = uint8_t(i + 1);
            }
        }
    }
    std::memset(stutter_, 0, uint32_t(kStutterMax) * 4u);
    for (int t = 0; t < kTracks; ++t) {
        if (samplePath[t][0]) loadSample(t, samplePath[t]);
        makamSetKey(eng_[t].e[kEngMakam], p.key);
    }
    if (!p.name[0]) std::snprintf(p.name, sizeof p.name, "%s", name);
    std::snprintf(p.name, sizeof p.name, "%s", name);
    setLaunchQ_ = p.launchQ;
    setRecQ_ = p.recQ;
    setSwing_ = p.swing;
    setKey_ = p.key;
    setScale_ = p.scale;
    setMetro_ = p.metronome;
    setLoop_ = p.songLoop;
    ctx_->setBpm(p.bpm);
    fxDirty_ = true;
    dirty_ = false;
    hold_ = false;
    std::snprintf(path, sizeof path, "%s/last.txt", ctx_->home);
    const IStorage::Handle hl = fs->open(path, IStorage::kWrite);
    if (hl >= 0) {
        fs->write(hl, name, uint32_t(std::strlen(name)));
        fs->close(hl);
    }
    return true;
}

void Konsol::save() {
    if (!ctx_ || !ctx_->fs || !P_) return;
    saveProject(P_->name);
}

// ---------------------------------------------------------------------------
// samples
// ---------------------------------------------------------------------------
void Konsol::loadSample(int t, const char* path) {
    SampleBuf& b = smp_[t];
    b.len = 0;   // the audio sees no sample while it loads
    if (!ctx_ || !ctx_->fs) {
        buildBreak(b);
        return;
    }
    ot::WavImportOptions opt;
    ot::WavImportResult res;
    // longer files: import the start that fits
    {
        const IStorage::Handle h = ctx_->fs->open(path, IStorage::kRead);
        if (h >= 0) {
            ot::WavInfo info;
            ot::StorageReader rd(*ctx_->fs, h);
            if (ot::wavParseHeader(rd, info) && ot::wavIsSupported(info)) {
                const uint32_t frames = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
                if (frames > b.cap) opt.endMarker = uint16_t(uint64_t(65535) * b.cap / (frames + 1));
            }
            ctx_->fs->close(h);
        }
    }
    const ot::FmtStatus st = ot::wavImportFile(*ctx_->fs, path, opt, b.data, b.cap, res);
    if (st != ot::FmtStatus::Ok || res.frames < 64) {
        buildBreak(b);
        if (ctx_) ctx_->toast("Cannot read that WAV - using the built-in break");
        return;
    }
    copyStr(b.path, sizeof b.path, path);
    b.len = res.frames;
    dirty_ = true;
}

namespace {
struct ListCtx {
    char (*list)[40];
    int n, cap;
    const char* ext;
    bool dirs;
};
bool listFn(const IStorage::DirEntry& e, void* p) {
    ListCtx& c = *static_cast<ListCtx*>(p);
    if (c.n >= c.cap) return false;
    if (e.name[0] == '.') return true;
    if (e.isDir) {
        if (!c.dirs) return true;
        std::snprintf(c.list[c.n++], 40, "/%s", e.name);   // a leading '/' marks a folder
        return true;
    }
    if (!endsWithCi(e.name, c.ext)) return true;
    std::snprintf(c.list[c.n++], 40, "%s", e.name);
    return true;
}
void sortList(char (*l)[40], int n) {
    for (int i = 1; i < n; ++i) {
        char x[40];
        std::memcpy(x, l[i], 40);
        int j = i;
        while (j > 0) {
            // folders first, then case-insensitive
            const bool xd = x[0] == '/', yd = l[j - 1][0] == '/';
            bool less;
            if (xd != yd) less = xd;
            else {
                int k = 0;
                for (;; ++k) {
                    char a = x[k], b = l[j - 1][k];
                    if (a >= 'a' && a <= 'z') a = char(a - 32);
                    if (b >= 'a' && b <= 'z') b = char(b - 32);
                    if (a != b || !a) {
                        less = a < b;
                        break;
                    }
                }
            }
            if (!less) break;
            std::memcpy(l[j], l[j - 1], 40);
            --j;
        }
        std::memcpy(l[j], x, 40);
    }
}
}  // namespace

void Konsol::scanSamples() {
    listN_ = 0;
    listSel_ = 0;
    listTop_ = 0;
    if (!ctx_ || !ctx_->fs) return;
    ListCtx c{list_, 0, kListMax, ".wav", true};
    if (listDir_[0] && std::strcmp(listDir_, "/") != 0) {
        std::snprintf(list_[c.n++], 40, "/..");
    }
    ctx_->fs->listDir(listDir_[0] ? listDir_ : "/", listFn, &c);
    listN_ = c.n;
    sortList(list_ + (listDir_[0] && std::strcmp(listDir_, "/") != 0 ? 1 : 0),
             listN_ - (listDir_[0] && std::strcmp(listDir_, "/") != 0 ? 1 : 0));
}

void Konsol::scanProjects() {
    listN_ = 0;
    listSel_ = 0;
    listTop_ = 0;
    if (!ctx_ || !ctx_->fs) return;
    char dir[64];
    std::snprintf(dir, sizeof dir, "%s/Projects", ctx_->home);
    ListCtx c{list_, 0, kListMax, ".kon", false};
    ctx_->fs->listDir(dir, listFn, &c);
    listN_ = c.n;
    for (int i = 0; i < listN_; ++i) {
        char* d = std::strrchr(list_[i], '.');
        if (d) *d = 0;
    }
    sortList(list_, listN_);
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------
void Konsol::update(uint32_t ms) {
    nowMs_ = ms;
    // live generators: after each loop of a live clip, prepare the next one
    for (int t = 0; t < kTracks; ++t) {
        TrackRt& r = rt_[t];
        const int play = r.play;
        if (play < 0) {
            livePlay_[t] = -1;
            continue;
        }
        Clip& c = P_->clip[t][play];
        const uint32_t loops = r.loops;
        if (livePlay_[t] != play) {
            livePlay_[t] = int8_t(play);
            liveSeen_[t] = loops;
            srcScene_[t] = -1;
            if (c.live && c.gen != kGenNotes) liveStep(t);
        } else if (loops != liveSeen_[t]) {
            liveSeen_[t] = loops;
            // recorded notes stop being "fresh" once their loop has passed
            if (c.live && c.gen != kGenNotes && (loops % (c.every ? c.every : 1)) == 0) liveStep(t);
        }
    }
    // meters: peak hold with a ~300 ms fall, updated every 30 ms
    static uint32_t lastMeter = 0;
    if (ms - lastMeter >= 30) {
        lastMeter = ms;
        for (int t = 0; t < kTracks; ++t) {
            const float pk = rt_[t].peak;
            rt_[t].peak = 0.0f;
            meter_[t] = pk > meter_[t] ? pk : meter_[t] * 0.8f;
        }
        const float pk = masterPk_;
        masterPk_ = 0.0f;
        masterMeter_ = pk > masterMeter_ ? pk : masterMeter_ * 0.8f;
    }
    // Device CPU feedback: the engine budget is an estimate calibrated under
    // QEMU; on the Tracker the real render load (DWT cycles, worst block of
    // the last 0.5 s) also pays for SDRAM / cache misses. Above 85 % the budget
    // shrinks (fewer voices, by stealing), below 60 % it recovers.
    static uint32_t lastCpu = 0;
    if (ctx_ && ctx_->shell && ms - lastCpu >= 250) {
        lastCpu = ms;
        const uint32_t pm = ctx_->shell->cpuPermille();
        float sc = budgetScale_;
        if (pm > 850) sc -= 0.05f;
        else if (pm < 600) sc += 0.02f;
        budgetScale_ = sc < 0.5f ? 0.5f : (sc > 1.0f ? 1.0f : sc);
    }
    // project tempo follows the shell
    if (ctx_) P_->bpm = ctx_->bpm();
}

void Konsol::onMidi(const MidiMsg& m) {
    const uint8_t ty = m.type();
    if (ty == 0x90 && m.d2) {
        playNote(m.d1, m.d2, true);
    } else if (ty == 0x80 || (ty == 0x90 && !m.d2)) {
        playNote(m.d1, 0, false);
    } else if (ty == 0xB0 && m.d1 == 7) {
        P_->track[selT_].mix.level = float(m.d2) / 127.0f * 1.25f;
    } else if (ty == 0xB0 && m.d1 == 10) {
        P_->track[selT_].mix.pan = (float(m.d2) - 64.0f) / 64.0f;
    }
}

void Konsol::playNote(int pitch, int vel, bool on) {
    if (pitch < 0 || pitch > 127) return;
    if (on) send(Cmd{Cmd::kNoteOn, uint8_t(selT_), uint8_t(pitch), uint8_t(vel)});
    else send(Cmd{Cmd::kNoteOff, uint8_t(selT_), uint8_t(pitch), 0});
    recordNote(pitch, vel, on);
}

// Live recording into the clip playing on the selected track (overdub).
void Konsol::recordNote(int pitch, int vel, bool on) {
    if (!recArm_ || !ctx_ || !ctx_->playing()) return;
    const int t = selT_;
    const TrackRt& r = rt_[t];
    const int s = r.play;
    if (s < 0) return;
    Clip& c = P_->clip[t][s];
    const uint32_t L = uint32_t(c.lenSteps) * kTps;
    if (!L) return;
    const uint32_t pos = posPub_;
    const uint32_t tick = pos > r.startPos ? ((pos - r.startPos) / 256u) % L : 0;
    if (on) {
        int k = 0;
        for (; k < 8; ++k)
            if (rec_[k].pitch < 0) break;
        if (k == 8) return;
        rec_[k].pitch = int8_t(pitch);
        rec_[k].start = uint16_t(tick);
        rec_[k].startPos = pos;
        rec_[k].vel = uint8_t(vel);
        rec_[k].scene = int8_t(s);
        return;
    }
    for (int k = 0; k < 8; ++k) {
        if (rec_[k].pitch != pitch) continue;
        RecNote& rn = rec_[k];
        rn.pitch = -1;
        if (rn.scene != s) return;
        uint32_t len = (pos - rn.startPos) / 256u;
        if (len < 2) len = 2;
        if (len > L) len = L;
        static const int kQ[4] = {0, kTps, 2 * kTps, kTps / 2};
        const int q = kQ[P_->recQ & 3];
        uint32_t st = rn.start;
        bool ahead = false;
        if (q) {
            uint32_t qs = ((st + uint32_t(q) / 2) / uint32_t(q)) * uint32_t(q);
            if (qs >= L) qs = 0;
            ahead = qs > st || (qs == 0 && st > L / 2);
            st = qs;
            if (len >= uint32_t(q)) len = ((len + uint32_t(q) / 2) / uint32_t(q)) * uint32_t(q);
        }
        (void)ahead;
        NoteBuf& b = c.beginEdit();
        const int dup = b.find(int(st), pitch);
        if (dup >= 0) b.remove(dup);
        Note n;
        n.start = uint16_t(st);
        n.len = uint16_t(len);
        n.pitch = uint8_t(pitch);
        n.vel = rn.vel;
        b.add(n);
        c.commit();
        dirty_ = true;
        return;
    }
}

// ---------------------------------------------------------------------------
// settings rows (Config page)
// ---------------------------------------------------------------------------
int Konsol::settings(SettingRow* rows, int cap) {
    static const char* const kKeys[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* const kScales[kScaleCount] = {"Chromatic", "Minor",      "Major",      "Dorian",
                                                     "Phrygian",  "Lydian",     "Mixolydian", "Harm. minor",
                                                     "Pent. minor", "Pent. major", "Blues",    "Makam (track)"};
    static const char* const kOff[2] = {"Off", "On"};
    static const char* const kBright[3] = {"Low", "Medium", "High"};
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, const char* unit,
                   void (*fn)(void*, int)) {
        if (n >= cap) return;
        SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.unit = unit;
        r.onChange = fn;
        r.ctx = this;
    };
    setLaunchQ_ = P_->launchQ;
    setRecQ_ = P_->recQ;
    setSwing_ = P_->swing;
    setKey_ = P_->key;
    setScale_ = P_->scale;
    setMetro_ = P_->metronome;
    setLoop_ = P_->songLoop;
    add("KONSOL launch quantise", &setLaunchQ_, 0, 4, launchQNames(), "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->launchQ = uint8_t(v); });
    add("KONSOL record quantise", &setRecQ_, 0, 3, recQNames(), "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->recQ = uint8_t(v); });
    add("KONSOL swing", &setSwing_, 50, 75, nullptr, " %",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->swing = uint8_t(v); });
    add("KONSOL key", &setKey_, 0, 11, kKeys, "", [](void* c, int v) {
        Konsol* k = static_cast<Konsol*>(c);
        k->P_->key = uint8_t(v);
        for (int t = 0; t < kTracks; ++t) makamSetKey(k->eng_[t].e[kEngMakam], v);
    });
    add("KONSOL scale", &setScale_, 0, kScaleCount - 1, kScales, "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->scale = uint8_t(v); });
    add("KONSOL metronome", &setMetro_, 0, 1, kOff, "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->metronome = uint8_t(v); });
    add("KONSOL song loops", &setLoop_, 0, 1, kOff, "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->songLoop = uint8_t(v); });
    add("KONSOL pad brightness", &setPadBright_, 0, 2, kBright, "", nullptr);
    return n;
}

}  // namespace konsol
}  // namespace lab
