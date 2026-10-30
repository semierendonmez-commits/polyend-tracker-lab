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
//     TSMP  u8 track, bars, stretch mode, i8 pitch, u8 stretched (v2; the stretch re-runs on load)
//           + u8 stream (SD streaming switch: Auto, On, Off; optional on load)
//     KIT   u8 track, 16 pad records (sample_mgr.cpp) (v2)
//     LKIT  the LABKIT chunk: macros, modulation routes, MIDI learn, param values (v2)
//     XCLP  u8 track, scene, launchQ (v2: the clip's own launch quantise)
//   v2 also: CLIP notes carry a u8 trig condition (11 bytes a note), TRK ends
//   with u8 midiCh, MFX with u8 revType, dlyType + f32 revPre, dlyMod, PREF
//   with u8 keyLayout, recHits.
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

void makamSetKey(Engine* e, int key);

namespace {

constexpr uint16_t kVersion = 2;   // 2: stereo pool, SAMPLER params reordered, TSMP / KIT / LKIT chunks

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
int dx_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
    w.chunk("MFX ", 2 + 4 * 9 + 1 + 2 + 8);
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
    w.u8(f.revType);
    w.u8(f.dlyType);
    w.f32(f.revPre);
    w.f32(f.dlyMod);
    // the master level last (added in the same version: optional on load)
    // TRK + ENG
    for (int t = 0; t < kTracks; ++t) {
        const TrackCfg& tc = p.track[t];
        w.chunk("TRK ", 3 + 4 * 4 + 4 + 96 + 1);
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
        {
            char sp[96] = {};
            const int src = ts_[t].src;
            if (pool_.valid(src) && src != breakSlot_) std::snprintf(sp, sizeof sp, "%s", pool_.slot(src).path);
            w.put(sp, 96);
        }
        w.u8(tc.midiCh);
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
    // TSMP + KIT
    for (int t = 0; t < kTracks; ++t) {
        const TrackSample& ts = ts_[t];
        w.chunk("TSMP", 6);
        w.u8(uint8_t(t));
        w.u8(ts.bars);
        w.u8(ts.mode);
        w.i8(ts.pitch);
        w.u8(ts.stretched);
        w.u8(ts.stream);
        w.chunk("KIT ", 1 + kitBytes());
        w.u8(uint8_t(t));
        static uint8_t kb[4096];
        kitWrite(t, kb);
        w.put(kb, kitBytes());
    }
    // CLIP
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kScenes; ++s) {
            const Clip& c = p.clip[t][s];
            if (!c.exists()) continue;
            const NoteBuf& nb = c.notes();
            w.chunk("CLIP", 6 + 12 + 2 + uint32_t(nb.count) * 11);
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
                w.u8(n.cond);
            }
            if (c.launchQ) {
                w.chunk("XCLP", 3);
                w.u8(uint8_t(t));
                w.u8(uint8_t(s));
                w.u8(c.launchQ);
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
    w.chunk("PREF", 3);
    w.u8(uint8_t(setPadBright_));
    w.u8(p.keyLayout);
    w.u8(p.recHits);
    if (lkitBuf_) {
        ctx_->params().sync();   // fields written directly (presets, this page) first
        const uint32_t n = ctx_->params().saveChunk(lkitBuf_, kLkitCap);
        if (n) {
            w.chunk("LKIT", n);
            w.put(lkitBuf_, n);
        }
    }
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
    TrackSample tsLoad[kTracks];
    const uint8_t* kitChunk[kTracks] = {};
    const uint8_t* lkitAt = nullptr;
    uint32_t lkitLen = 0;
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
            if (uint32_t(c.e - c.p) >= 10) {
                f.revType = uint8_t(c.u8() % kRevTypes);
                f.dlyType = uint8_t(c.u8() % kDlyTypes);
                f.revPre = clampf(c.f32(), 0, 1);
                f.dlyMod = clampf(c.f32(), 0, 1);
            }
        } else if (!std::memcmp(tag, "PREF", 4)) {
            setPadBright_ = c.u8() % 3;
            if (c.p < c.e) p.keyLayout = c.u8() ? 1 : 0;
            if (c.p < c.e) p.recHits = c.u8() ? 1 : 0;
        } else if (!std::memcmp(tag, "LKIT", 4)) {
            lkitAt = c.p;
            lkitLen = uint32_t(c.e - c.p);
        } else if (!std::memcmp(tag, "XCLP", 4)) {
            const int t = c.u8(), sc = c.u8();
            if (t < kTracks && sc < kScenes) p.clip[t][sc].launchQ = uint8_t(c.u8() % 6);
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
            if (c.p < c.e) tc.midiCh = uint8_t(c.u8() % 17);
        } else if (!std::memcmp(tag, "ENG ", 4)) {
            const int t = c.u8(), k = c.u8();
            const int n = c.u16();
            if (t >= kTracks || k >= kEngCount) continue;
            if (ver < 2 && k == kEngSampler) continue;   // v1 SAMPLER params had another order
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
                x.cond = ver >= 2 ? uint8_t(c.u8() % kCondCount) : 0;
                if (x.chance > 100) x.chance = 100;
                if (x.roll > 16) x.roll = 16;
                if (x.vel < 1) x.vel = 1;
                if (x.vel > 127) x.vel = 127;
                if (x.len < 1) x.len = 1;
                if (x.start < maxTick) nb.add(x);
            }
        } else if (!std::memcmp(tag, "TSMP", 4)) {
            const int t = c.u8();
            if (t >= kTracks) continue;
            tsLoad[t].bars = uint8_t(c.u8() % 33);
            tsLoad[t].mode = uint8_t(c.u8() % kStrModes);
            tsLoad[t].pitch = int8_t(dx_clampi(c.i8(), -24, 24));
            tsLoad[t].stretched = c.u8() ? 1 : 0;
            if (c.p < c.e) tsLoad[t].stream = uint8_t(c.u8() % kStreamModes);   // SD streaming switch
        } else if (!std::memcmp(tag, "KIT ", 4)) {
            const int t = c.u8();
            if (t >= kTracks || uint32_t(c.e - c.p) < kitBytes()) continue;
            kitChunk[t] = c.p;
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
    // samples and kits (the file buffer is still intact: kits read from it)
    for (int t = 0; t < kTracks; ++t) {
        ts_[t].stream = tsLoad[t].stream;
        if (samplePath[t][0] && !loadSample(t, samplePath[t])) setTrackBreak(t);
        TrackSample& ts = ts_[t];
        ts.mode = tsLoad[t].mode;
        ts.pitch = tsLoad[t].pitch;
        if (tsLoad[t].bars) {
            ts.bars = tsLoad[t].bars;
            int bars = ts.bars;
            if (pool_.valid(ts.src))
                ts.srcBpm = ts.outBpm = loopBpm(pool_.length(ts.src), bars, p.bpm, pool_.streamed(ts.src) ? nullptr : &pool_.slot(ts.src));
        }
        if (tsLoad[t].stretched) strPend_ = uint8_t(strPend_ | (1u << t));
        if (kitChunk[t]) kitRead(t, kitChunk[t]);
        makamSetKey(eng_[t].e[kEngMakam], p.key);
    }
    gcSamples();
    // LABKIT: macros, routes, MIDI learn (engine slots follow the engines loaded above)
    if (lkitAt && lkitLen) {
        // the chunk writes the registered fields too: the project's own chunks win
        Mix mixes[kTracks];
        for (int t = 0; t < kTracks; ++t) mixes[t] = p.track[t].mix;
        const FxCfg fx = p.fx;
        ctx_->params().loadChunk(lkitAt, lkitLen);
        for (int t = 0; t < kTracks; ++t) p.track[t].mix = mixes[t];
        p.fx = fx;
    }
    refreshRegistry();
    std::memset(stutter_, 0, uint32_t(kStutterMax) * 4u);
    if (!p.name[0]) std::snprintf(p.name, sizeof p.name, "%s", name);
    std::snprintf(p.name, sizeof p.name, "%s", name);
    setLaunchQ_ = p.launchQ;
    setRecQ_ = p.recQ;
    setSwing_ = p.swing;
    setKey_ = p.key;
    setScale_ = p.scale;
    setMetro_ = p.metronome;
    setLoop_ = p.songLoop;
    setKeyLayout_ = p.keyLayout;
    setRecHits_ = p.recHits;
    undoN_ = redoN_ = 0;
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
// main loop
// ---------------------------------------------------------------------------
void Konsol::update(uint32_t ms) {
    nowMs_ = ms;
    applyPick();
    placeSlots();
    stretchService(4096);
    streamService();
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
            meterHold_[t] = meter_[t] > meterHold_[t] ? meter_[t] : meterHold_[t] - 0.01f;
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

// MIDI in: a track whose MIDI-in channel matches plays; notes on a channel no
// track claims play the selected track (as before).
void Konsol::onMidi(const MidiMsg& m) {
    const uint8_t ty = m.type();
    int t = -1;
    for (int k = 0; k < kTracks; ++k)
        if (P_->track[k].midiCh == m.channel() + 1) {
            t = k;
            break;
        }
    if (t < 0) t = selT_;
    if (ty == 0x90 && m.d2) {
        playNoteOn(t, m.d1, m.d2, true);
    } else if (ty == 0x80 || (ty == 0x90 && !m.d2)) {
        playNoteOn(t, m.d1, 0, false);
    } else if (ty == 0xB0 && m.d1 == 7) {
        P_->track[t].mix.level = float(m.d2) / 127.0f * 1.25f;
    } else if (ty == 0xB0 && m.d1 == 10) {
        P_->track[t].mix.pan = (float(m.d2) - 64.0f) / 64.0f;
    }
}

void Konsol::playNoteOn(int t, int pitch, int vel, bool on) {
    if (pitch < 0 || pitch > 127 || t < 0 || t >= kTracks) return;
    if (on) send(Cmd{Cmd::kNoteOn, uint8_t(t), uint8_t(pitch), uint8_t(vel)});
    else send(Cmd{Cmd::kNoteOff, uint8_t(t), uint8_t(pitch), 0});
    recordNote(t, pitch, vel, on);
}

// Live recording into the clip playing on the selected track (overdub).
void Konsol::recordNote(int t, int pitch, int vel, bool on) {
    if (!recArm_ || !ctx_ || !ctx_->playing()) return;
    const TrackRt& r = rt_[t];
    const int s = r.play;
    if (s < 0) return;
    Clip& c = P_->clip[t][s];
    const uint32_t L = uint32_t(c.lenSteps) * kTps;
    if (!L) return;
    const uint32_t pos = posPub_;
    const uint32_t tick = pos > r.startPos ? ((pos - r.startPos) / 256u) % L : 0;
    // drum-like engines: hits are written at once with a fixed length (no note-off needed)
    if (on && P_->recHits && drumLike(t)) {
        static const int kQ[4] = {0, kTps, 2 * kTps, kTps / 2};
        const int q = kQ[P_->recQ & 3];
        uint32_t st = tick;
        if (q) {
            st = ((st + uint32_t(q) / 2) / uint32_t(q)) * uint32_t(q);
            if (st >= L) st = 0;
        }
        NoteBuf& b = c.beginEdit();
        const int dup = b.find(int(st), pitch);
        if (dup >= 0) b.remove(dup);
        Note n;
        n.start = uint16_t(st);
        n.len = uint16_t(kTps - 1);
        n.pitch = uint8_t(pitch);
        n.vel = uint8_t(vel);
        b.add(n);
        c.commit();
        dirty_ = true;
        return;
    }
    if (on) {
        int k = 0;
        for (; k < 8; ++k)
            if (rec_[k].pitch < 0) break;
        if (k == 8) return;
        rec_[k].track = int8_t(t);
        rec_[k].pitch = int8_t(pitch);
        rec_[k].start = uint16_t(tick);
        rec_[k].startPos = pos;
        rec_[k].vel = uint8_t(vel);
        rec_[k].scene = int8_t(s);
        return;
    }
    for (int k = 0; k < 8; ++k) {
        if (rec_[k].pitch != pitch || rec_[k].track != t) continue;
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
    static const char* const kLayout[2] = {"Chromatic", "In key"};
    setKeyLayout_ = P_->keyLayout;
    setRecHits_ = P_->recHits;
    add("KONSOL pad keyboard", &setKeyLayout_, 0, 1, kLayout, "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->keyLayout = uint8_t(v); });
    add("KONSOL drums record hits", &setRecHits_, 0, 1, kOff, "",
        [](void* c, int v) { static_cast<Konsol*>(c)->P_->recHits = uint8_t(v); });
    return n;
}

}  // namespace konsol
}  // namespace lab
