// BULUT projects and the gesture recorder (project.h).
#include "ws/project.h"

#include <cstring>

namespace bulut {

namespace {

constexpr uint32_t tag(const char* s) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// Buffered writer: chunks are assembled in a small buffer and flushed.
struct Writer {
    ot::IStorage* fs;
    ot::IStorage::Handle h;
    bool ok = true;
    uint8_t buf[2048];
    uint32_t n = 0, pos = 0;
    void flush() {
        if (n && ok) ok = fs->write(h, buf, n) == int32_t(n);
        pos += n;
        n = 0;
    }
    void put(const void* p, uint32_t len) {
        const uint8_t* s = static_cast<const uint8_t*>(p);
        while (len) {
            uint32_t k = sizeof buf - n;
            if (k > len) k = len;
            std::memcpy(buf + n, s, k);
            n += k;
            s += k;
            len -= k;
            if (n == sizeof buf) flush();
        }
    }
    void u32(uint32_t v) { put(&v, 4); }
    void u16(uint16_t v) { put(&v, 2); }
    uint32_t at() const { return pos + n; }
};

struct Reader {
    ot::IStorage* fs;
    ot::IStorage::Handle h;
    uint32_t pos = 0, size = 0;
    bool get(void* p, uint32_t len) {
        uint8_t* d = static_cast<uint8_t*>(p);
        while (len) {
            const int32_t got = fs->read(h, d, len);
            if (got <= 0) return false;
            d += got;
            len -= uint32_t(got);
            pos += uint32_t(got);
        }
        return true;
    }
    bool skip(uint32_t len) {
        pos += len;
        return fs->seek(h, pos);
    }
};

// A gesture as len, count, quant, speed, then `count` events.
uint32_t gestBytes(const Gesture& g) { return 8u + uint32_t(g.count) * sizeof(GestEvent); }
void putGest(Writer& w, const Gesture& g) {
    w.u32(g.len);
    w.u16(g.count);
    w.put(&g.quant, 1);
    w.put(&g.speed, 1);
    w.put(g.ev, uint32_t(g.count) * sizeof(GestEvent));
}
bool getGest(Reader& r, Gesture& g) {
    uint32_t len = 0;
    uint16_t count = 0;
    uint8_t q = 0, sp = 2;
    if (!r.get(&len, 4) || !r.get(&count, 2) || !r.get(&q, 1) || !r.get(&sp, 1)) return false;
    if (count > kGestEvents) return false;
    g.len = len;
    g.count = count;
    g.quant = q < kQuants ? q : 0;
    g.speed = sp < kSpeeds ? sp : 2;
    if (!r.get(g.ev, uint32_t(count) * sizeof(GestEvent))) return false;
    if (g.len == 0) g.count = 0;
    return true;
}

}  // namespace

const char* projResultText(ProjResult r) {
    switch (r) {
        case ProjResult::kOk: return "ok";
        case ProjResult::kNoCard: return "no SD card";
        case ProjResult::kIoError: return "card error";
        case ProjResult::kBadFile: return "not a BULUT project";
        case ProjResult::kPartial: return "file cut short, partly loaded";
        case ProjResult::kTooOld: return "a BULUT 1 project (6 clouds)";
        default: return "made by a newer BULUT";
    }
}

ProjResult saveProject(ot::IStorage* fs, const char* path, const ProjectRefs& p) {
    if (!fs) return ProjResult::kNoCard;
    char tmp[128];
    uint32_t i = 0;
    for (; path[i] && i + 5 < sizeof tmp; ++i) tmp[i] = path[i];
    std::memcpy(tmp + i, ".tmp", 5);
    const ot::IStorage::Handle h = fs->open(tmp, ot::IStorage::kWrite);
    if (h < 0) return ProjResult::kIoError;
    static Writer w;   // 2 KB buffer: not on the stack
    w.fs = fs;
    w.h = h;
    w.ok = true;
    w.n = 0;
    w.pos = 0;
    w.put("BLUT", 4);
    w.u16(kProjectVersion);
    w.u16(kProjectMinor);
    auto chunk = [&](const char* t, uint32_t len) {
        w.u32(tag(t));
        w.u32(len);
    };
    chunk("CHST", sizeof(ChState) * kCh);
    w.put(p.states, sizeof(ChState) * kCh);
    chunk("SRCS", sizeof(ChSource) * kCh);
    w.put(p.sources, sizeof(ChSource) * kCh);
    uint32_t gl = 0;
    for (int c = 0; c < kCh; ++c) gl += gestBytes(*p.gestures[c]);
    chunk("GEST", gl);
    for (int c = 0; c < kCh; ++c) putGest(w, *p.gestures[c]);
    if (p.reg && p.scratch) {
        const uint32_t n = p.reg->saveChunk(p.scratch, p.scratchCap);
        if (n) {
            chunk("REGC", n);
            w.put(p.scratch, n);
        }
    }
    chunk("TEMP", 8);
    w.put(p.bpm, 4);
    w.u32(uint32_t(*p.songMode));
    for (int s = 0; s < kScenes; ++s) {
        const Scene& sc = p.scenes[s];
        if (!sc.used) continue;
        const uint32_t fixed = sizeof sc.norm + sizeof sc.master + sizeof sc.st;
        uint32_t len = 4 + fixed;
        for (int c = 0; c < kCh; ++c) len += gestBytes(sc.gest[c]);
        chunk("SCN2", len);
        w.u32(uint32_t(s));
        w.put(sc.norm, sizeof sc.norm);
        w.put(sc.master, sizeof sc.master);
        w.put(sc.st, sizeof sc.st);
        for (int c = 0; c < kCh; ++c) putGest(w, sc.gest[c]);
    }
    if (p.tapes) {
        chunk("TAPE", sizeof(TapeSave) * kCh);
        w.put(p.tapes, sizeof(TapeSave) * kCh);
    }
    chunk("TIME", 4 + sizeof(TimeEntry) * kTimeline);
    w.u32(uint32_t(*p.timelineLen));
    w.put(p.timeline, sizeof(TimeEntry) * kTimeline);
    for (int f = 0; f < kFreezeSlots; ++f) {
        if (!p.freezes[f].used) continue;
        chunk("FRZE", 4 + sizeof(FreezeSlot));
        w.u32(uint32_t(f));
        w.put(&p.freezes[f], sizeof(FreezeSlot));
    }
    chunk("END ", 0);
    w.flush();
    fs->close(h);
    if (!w.ok) {
        fs->remove(tmp);
        return ProjResult::kIoError;
    }
    // replace the old file only once the new one is complete
    fs->remove(path);
    if (!fs->rename(tmp, path)) return ProjResult::kIoError;
    return ProjResult::kOk;
}

ProjResult loadProject(ot::IStorage* fs, const char* path, const ProjectRefs& p) {
    if (!fs) return ProjResult::kNoCard;
    const ot::IStorage::Handle h = fs->open(path, ot::IStorage::kRead);
    if (h < 0) return ProjResult::kIoError;
    Reader r{fs, h};
    r.size = uint32_t(fs->size(h));
    char magic[4];
    uint16_t ver = 0, minor = 0;
    if (!r.get(magic, 4) || std::memcmp(magic, "BLUT", 4) != 0 || !r.get(&ver, 2) || !r.get(&minor, 2)) {
        fs->close(h);
        return ProjResult::kBadFile;
    }
    if (ver > kProjectVersion) {
        fs->close(h);
        return ProjResult::kTooNew;
    }
    if (ver < 2) {
        fs->close(h);
        return ProjResult::kTooOld;
    }
    // A project replaces everything: clear what the file may not hold.
    for (int s = 0; s < kScenes; ++s) p.scenes[s].used = 0;
    for (int f = 0; f < kFreezeSlots; ++f) p.freezes[f].used = 0;
    *p.timelineLen = 0;
    if (p.tapes) std::memset(p.tapes, 0, sizeof(TapeSave) * kCh);
    bool ok = true;
    while (ok && r.pos + 8 <= r.size) {
        uint32_t t = 0, len = 0;
        if (!r.get(&t, 4) || !r.get(&len, 4)) { ok = false; break; }
        if (t == tag("END ")) break;
        const uint32_t end = r.pos + len;
        if (end > r.size) { ok = false; break; }
        if (t == tag("CHST") && len == sizeof(ChState) * kCh) {
            ok = r.get(p.states, len);
        } else if (t == tag("SRCS") && len == sizeof(ChSource) * kCh) {
            ok = r.get(p.sources, len);
            for (int c = 0; c < kCh && ok; ++c) {
                p.sources[c].path[sizeof p.sources[c].path - 1] = 0;
                p.sources[c].label[sizeof p.sources[c].label - 1] = 0;
            }
        } else if (t == tag("GEST")) {
            for (int c = 0; c < kCh && ok; ++c) ok = getGest(r, *p.gestures[c]);
        } else if (t == tag("REGC") && p.reg && p.scratch && len <= p.scratchCap) {
            ok = r.get(p.scratch, len);
            if (ok) p.reg->loadChunk(p.scratch, len);
        } else if (t == tag("TEMP") && len == 8) {
            uint32_t sm = 0;
            ok = r.get(p.bpm, 4) && r.get(&sm, 4);
            *p.songMode = sm ? 1 : 0;
        } else if (t == tag("SCN2") && len >= 4 + sizeof(Scene::norm) + sizeof(Scene::master) + sizeof(Scene::st)) {
            uint32_t sidx = 0;
            ok = r.get(&sidx, 4);
            if (ok && sidx < uint32_t(kScenes)) {
                Scene& sc = p.scenes[sidx];
                ok = r.get(sc.norm, sizeof sc.norm) && r.get(sc.master, sizeof sc.master) && r.get(sc.st, sizeof sc.st);
                for (int c = 0; c < kCh && ok; ++c) ok = getGest(r, sc.gest[c]);
                sc.used = ok ? 1 : 0;
            }
        } else if (t == tag("TAPE") && p.tapes && len == sizeof(TapeSave) * kCh) {
            ok = r.get(p.tapes, len);
            for (int c = 0; c < kCh && ok; ++c) {
                p.tapes[c].path[sizeof p.tapes[c].path - 1] = 0;
                p.tapes[c].label[sizeof p.tapes[c].label - 1] = 0;
                p.tapes[c].playing = p.tapes[c].playing ? 1 : 0;
            }
        } else if (t == tag("TIME") && len == 4 + sizeof(TimeEntry) * kTimeline) {
            uint32_t n = 0;
            ok = r.get(&n, 4) && r.get(p.timeline, sizeof(TimeEntry) * kTimeline);
            *p.timelineLen = n > uint32_t(kTimeline) ? kTimeline : int(n);
        } else if (t == tag("FRZE") && len == 4 + sizeof(FreezeSlot)) {
            uint32_t f = 0;
            ok = r.get(&f, 4);
            if (ok && f < uint32_t(kFreezeSlots)) ok = r.get(&p.freezes[f], sizeof(FreezeSlot));
        }
        if (ok && r.pos != end) ok = r.skip(end - r.pos);   // unknown chunk / rest
    }
    fs->close(h);
    // keep everything in range whatever the file held
    for (int c = 0; c < kCh; ++c) {
        ChState& st = p.states[c];
        for (int e = 0; e < kEngines; ++e)
            for (int i = 0; i < kKnobs; ++i) {
                float& v = st.engMem[e][i];
                if (!(v >= 0.0f && v <= 1.0f)) v = 0.5f;
            }
        st.stem &= 3;
        if (st.freezeSlot != 0xFF && st.freezeSlot >= kFreezeSlots) st.freezeSlot = 0xFF;
    }
    for (int s2 = 0; s2 < kScenes; ++s2) {
        Scene& sc = p.scenes[s2];
        if (!sc.used) continue;
        for (int c = 0; c < kCh; ++c)
            for (int i = 0; i < kChParams; ++i)
                if (!(sc.norm[c][i] >= 0.0f && sc.norm[c][i] <= 1.0f)) sc.norm[c][i] = 0.5f;
        for (int i = 0; i < kMasterParams; ++i)
            if (!(sc.master[i] >= 0.0f && sc.master[i] <= 1.0f)) sc.master[i] = 0.5f;
        // before TAPE FILE a scene's SOURCE was one of 4 (its normalized value counts them)
        if (minor < 1)
            for (int c = 0; c < kCh; ++c)
                sc.norm[c][kChParams - 2] = enumValue(enumIndex(sc.norm[c][kChParams - 2], kSrcCountV1), kSrcCount);
    }
    for (int i = 0; i < *p.timelineLen; ++i) {
        if (p.timeline[i].scene >= kScenes) p.timeline[i].scene = 0;
        if (p.timeline[i].bars < 1) p.timeline[i].bars = 1;
    }
    if (!(*p.bpm >= 20.0f && *p.bpm <= 400.0f)) *p.bpm = 120.0f;
    return ok ? ProjResult::kOk : ProjResult::kPartial;
}

// ---- gestures ----------------------------------------------------------------

void sortEvents(GestEvent* ev, int n) {
    for (int i = 1; i < n; ++i) {
        const GestEvent x = ev[i];
        int j = i - 1;
        while (j >= 0 && ev[j].t > x.t) {
            ev[j + 1] = ev[j];
            --j;
        }
        ev[j + 1] = x;
    }
}

void GestureRecorder::arm(int cloud, bool overdub) {
    cloud_ = cloud;
    overdub_ = overdub;
    state_ = kArmed;
    n_ = 0;
    held_ = 0;
}

void GestureRecorder::start(uint32_t now, const Gesture* live, uint32_t livePhase, float liveSpeed) {
    t0_ = now;
    n_ = 0;
    held_ = 0;
    if (overdub_ && live && live->len) {
        loopLen_ = live->len;
        base_ = livePhase % live->len;
        speed_ = liveSpeed;
    } else {
        overdub_ = false;
        loopLen_ = 0;
        base_ = 0;
        speed_ = 1.0f;
    }
    state_ = kRecording;
}

uint32_t GestureRecorder::stamp(uint32_t now) const {
    const uint32_t e = now - t0_;
    if (!overdub_) return e;
    // loop time advances at the playback speed
    const uint32_t lt = base_ + uint32_t(float(e) * speed_);
    return lt % loopLen_;
}

void GestureRecorder::add(uint8_t type, uint8_t a, uint16_t v, uint32_t now) {
    if (n_ >= kGestEvents - 8) return;   // room kept for the closing pad-ups
    GestEvent& e = ev_[n_++];
    e.t = stamp(now);
    e.type = type;
    e.a = a;
    e.v = v;
}

void GestureRecorder::pad(bool down, int pad, int vel, uint32_t now) {
    if (state_ != kRecording || pad < 0 || pad >= 48) return;
    if (down) held_ |= 1ull << pad;
    else if (!((held_ >> pad) & 1)) return;   // a release of a press from before the recording
    else held_ &= ~(1ull << pad);
    add(down ? kGPadDown : kGPadUp, uint8_t(pad), uint16_t(down ? vel : 0), now);
}

void GestureRecorder::param(int index, float norm, uint32_t now) {
    if (state_ != kRecording || index < 0 || index >= kChParams) return;
    const uint8_t a = uint8_t(index);
    const uint16_t v = uint16_t(clamp01(norm) * 65535.0f + 0.5f);
    const uint32_t t = stamp(now);
    // thin: a move of the same parameter within 10 ms replaces the last one
    if (n_ > 0) {
        GestEvent& last = ev_[n_ - 1];
        if (last.type == kGParam && last.a == a && t >= last.t && t - last.t < 441) {
            last.v = v;
            return;
        }
    }
    add(kGParam, a, v, now);
}

bool GestureRecorder::finish(uint32_t now, uint32_t quant, Gesture& out, const Gesture* live) {
    if (state_ != kRecording) {
        state_ = kIdle;
        return false;
    }
    state_ = kIdle;
    if (overdub_) {
        // close held pads at the current loop time, then merge into the loop
        for (int p = 0; p < 48; ++p)
            if ((held_ >> p) & 1) {
                GestEvent& e = ev_[n_++];
                e.t = stamp(now);
                e.type = kGPadUp;
                e.a = uint8_t(p);
                e.v = 0;
            }
        if (n_ == 0 || !live) return false;
        out.len = live->len;
        out.quant = live->quant;
        out.speed = live->speed;
        int n = live->count;
        std::memcpy(out.ev, live->ev, sizeof(GestEvent) * size_t(n));
        for (int i = 0; i < n_ && n < kGestEvents; ++i) out.ev[n++] = ev_[i];
        sortEvents(out.ev, n);
        out.count = uint16_t(n);
        return true;
    }
    uint32_t len = now - t0_;
    if (quant) {
        uint32_t k = (len + quant / 2) / quant;
        if (k < 1) k = 1;
        len = k * quant;
    }
    if (len < 4410) len = 4410;   // 100 ms minimum
    if (n_ == 0) return false;
    // events past the rounded length wrap to the loop start (sorted below)
    for (int i = 0; i < n_; ++i) ev_[i].t = ev_[i].t < len ? ev_[i].t : ev_[i].t % len;
    for (int p = 0; p < 48; ++p)
        if ((held_ >> p) & 1) {
            GestEvent& e = ev_[n_++];
            e.t = len - 1;
            e.type = kGPadUp;
            e.a = uint8_t(p);
            e.v = 0;
        }
    sortEvents(ev_, n_);
    out.len = len;
    out.count = uint16_t(n_);
    std::memcpy(out.ev, ev_, sizeof(GestEvent) * size_t(n_));
    if (out.speed >= kSpeeds) out.speed = 2;
    if (out.quant >= kQuants) out.quant = 0;
    return true;
}

}  // namespace bulut
