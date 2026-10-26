// KONSOL core: init, the demo project, the audio side (clip sequencer, launch
// quantisation, song mode, voice governor, mixer, sends, master bus).
#include "ws/konsol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "audio/audio_tables.h"
#include "gen/gen.h"
#include "model/pattern.h"

namespace lab {
namespace konsol {

Engine* makeSampler(Arena& a, SampleBuf* b);
void makamSetKey(Engine* e, int key);

namespace {
constexpr uint32_t kSubPerTick = 256;                     // global position unit
constexpr uint32_t kSubPerBeat = kPpq * kSubPerTick;      // 12288
constexpr uint32_t kSubPerStep = kTps * kSubPerTick;      // 3072
constexpr uint32_t kSampleCap = 600u * 1024u;             // samples per track (13.9 s)
// Engines may use this many M4 instructions per block in total; the rest of
// the 180k gate is mixer, sends, master and the shell.
// Engine costs below are estimates calibrated against QEMU (tests/lab bench);
// the fixed part (mixer, delay, reverb, compressor, limiter, shell) is ~55k.
constexpr int32_t kEngineBudget = 92000;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Mixer law: fader 0..1.25 -> gain (cubic, 0.8 = unity).
inline float faderGain(float f) { return f * f * (1.0f / 0.64f); }
}  // namespace

Rgb trackColor(int t) {
    static const Rgb kC[kTracks] = {Rgb(0xFF5A4E), Rgb(0xFF9F1C), Rgb(0xFFD23F), Rgb(0x8BE36B),
                                    Rgb(0x2EC4B6), Rgb(0x4D9DE0), Rgb(0xA77BFF), Rgb(0xF06EAA)};
    return kC[t & 7];
}

// ============================================================================
// init
// ============================================================================
bool Konsol::init(Context& c) {
    ctx_ = &c;
    theme_.bg = Rgb(0x0A0D13);
    theme_.panel = Rgb(0x161C28);
    theme_.fg = Rgb(0xECE6D6);
    theme_.dim = Rgb(0x66708A);
    theme_.accent = Rgb(0xFFB000);
    theme_.accent2 = Rgb(0x2EC4B6);
    theme_.warn = Rgb(0xFF4D4D);
    theme_.ok = Rgb(0x7CDB5A);
    ot::audio::initTables();
    Arena& a = *c.arena;
    P_ = a.make<Project>();
    scratch_ = static_cast<ot::Pattern*>(a.alloc(sizeof(ot::Pattern)));
    src_ = a.make<NoteBuf>(kTracks);
    undo_ = a.make<NoteBuf>();
    acidPh_ = a.alloc(sizeof(ot::gen::MonoStep) * kMaxSteps * kTracks);
    list_ = static_cast<char(*)[40]>(a.alloc(kListMax * 40));
    stutter_ = static_cast<int16_t*>(a.alloc(kStutterMax * 2 * sizeof(int16_t)));
    int16_t* dmem = static_cast<int16_t*>(a.alloc(ot::audio::Delay::kMemorySamples * sizeof(int16_t)));
    int16_t* rmem = static_cast<int16_t*>(a.alloc(ot::audio::Reverb::kMemorySamples * sizeof(int16_t)));
    reverb_ = a.make<ot::audio::Reverb>();
    if (!P_ || !scratch_ || !src_ || !undo_ || !acidPh_ || !list_ || !stutter_ || !dmem || !rmem || !reverb_)
        return false;
    delay_.init(dmem);
    reverb_->init(rmem);
    for (int t = 0; t < kTracks; ++t) {
        smp_[t].data = static_cast<int16_t*>(a.alloc(kSampleCap * sizeof(int16_t)));
        smp_[t].cap = kSampleCap;
        if (!smp_[t].data) return false;
        if (!eng_[t].init(a, &smp_[t], t)) return false;
    }
    for (auto& p : padPitch_) p = -1;
    newProject(true);
    // last project on the card
    if (c.fs) {
        char path[96];
        std::snprintf(path, sizeof path, "%s/Projects", c.home);
        c.fs->mkdir(path);
        std::snprintf(path, sizeof path, "%s/Samples", c.home);
        c.fs->mkdir(path);
        std::snprintf(path, sizeof path, "%s/last.txt", c.home);
        const IStorage::Handle h = c.fs->open(path, IStorage::kRead);
        if (h >= 0) {
            char name[20] = {};
            const int32_t n = c.fs->read(h, name, sizeof name - 1);
            c.fs->close(h);
            if (n > 0) {
                name[n] = 0;
                for (char* q = name; *q; ++q)
                    if (*q == '\n' || *q == '\r') *q = 0;
                if (name[0]) loadProject(name);
            }
        }
    }
    c.setBpm(P_->bpm);
    applyFxParams();
    return true;
}

// A synthetic 1-bar break (120 BPM, 2 s): the default sample of every track,
// 16 equal slices = 16 steps.
void Konsol::buildBreak(SampleBuf& b) {
    const uint32_t n = 88200;
    b.len = 0;
    uint32_t seed = 0x1234567u;
    auto noise = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return float(int32_t(seed)) * (1.0f / 2147483648.0f);
    };
    // kick K, snare S, ghost g, hats on every 8th
    static const char kPat[16] = {'K', 0, 'h', 0, 'S', 0, 'h', 'g', 0, 'K', 'K', 0, 'S', 0, 'h', 'g'};
    for (uint32_t i = 0; i < n; ++i) b.data[i] = 0;
    const uint32_t step = n / 16;
    float hpz = 0, hpx = 0;
    for (int s = 0; s < 16; ++s) {
        const char k = kPat[s];
        const uint32_t at = uint32_t(s) * step;
        const bool hat = (s % 2) == 0 || k == 'h';
        for (uint32_t i = 0; i < step * 3 && at + i < n; ++i) {
            const float tt = float(i) / 44100.0f;
            float v = 0;
            if (k == 'K') {
                const float f = 50.0f + 120.0f * expf(-tt * 40.0f);
                v += sinf(6.2831853f * f * tt + 3.0f * (1.0f - expf(-tt * 40.0f)) / 40.0f) * expf(-tt * 7.0f) * 0.95f;
            }
            if (k == 'S' || k == 'g') {
                const float amp = k == 'S' ? 0.8f : 0.25f;
                v += (sinf(6.2831853f * 190.0f * tt) * expf(-tt * 25.0f) * 0.5f + noise() * expf(-tt * 14.0f)) * amp;
            }
            if (hat && i < step) {
                const float x = noise();
                hpz = 0.6f * (hpz + x - hpx);
                hpx = x;
                v += hpz * expf(-tt * 90.0f) * 0.35f;
            }
            int32_t o = int32_t(b.data[at + i]) + int32_t(v * 20000.0f);
            b.data[at + i] = int16_t(o > 32767 ? 32767 : (o < -32768 ? -32768 : o));
        }
    }
    b.path[0] = 0;
    b.len = n;
}

void Konsol::setEngine(int t, int kind) {
    if (kind < 0 || kind >= kEngCount) return;
    Engine* old = engine(t);
    if (old) old->allOff(true);
    P_->track[t].engine = uint8_t(kind);
    makamSetKey(eng_[t].e[kEngMakam], P_->key);
    dirty_ = true;
}

int Konsol::rootNote(int t) const {
    int r = 12 * (P_->track[t].octave + 1) + P_->key;   // octave 2 -> C2 = 36
    return r < 12 ? 12 : (r > 108 ? 108 : r);
}

uint16_t Konsol::scaleMask(int t) const {
    int m = 0;
    if (P_->scale == kScaleCount - 1) {
        // Makam scale: from the makam engine of the selected track (or track t).
        const Engine* e = eng_[t].e[kEngMakam];
        m = makamMask(int(e->get(0) + 0.5f));
    }
    return scaleMaskOf(P_->scale, m);
}

// ============================================================================
// demo / new project
// ============================================================================
namespace {
void put(NoteBuf& b, int step, int pitch, int vel = 100, int lenSteps = 1, int flags = 0) {
    Note n;
    n.start = uint16_t(step * kTps);
    n.len = uint16_t(lenSteps * kTps - 2);
    n.pitch = uint8_t(pitch);
    n.vel = uint8_t(vel);
    n.flags = uint8_t(flags);
    b.add(n);
}
}  // namespace

void Konsol::defaultGen(Clip& c, int kind) {
    c.gen = uint8_t(kind);
    for (int i = 0; i < kGenParams; ++i) c.gp[i] = 0;
    if (kind >= kGenAcid && kind <= kGenStation) {
        const int k = kind - kGenAcid;
        for (int i = 0; i < kGenParams; ++i) c.gp[i] = ot::gen::param(k, i, c.gp).def;
        for (int i = 0; i < kGenParams; ++i) c.gp[i] = ot::gen::param(k, i, c.gp).def;
        if (kind == kGenAcid) c.gp[4] = 25;   // live Mutate
    } else if (kind == kGenEuclid) {
        const int16_t d[6] = {5, 0, 0, 2, 40, 0};   // hits, rotate, note offset, spread, accent, seed
        for (int i = 0; i < 6; ++i) c.gp[i] = d[i];
    } else if (kind == kGenUsul) {
        const int16_t d[6] = {0, 2, 0, 7, 30, 0};   // usul, repeats, dum offset, tek offset, fills, seed
        for (int i = 0; i < 6; ++i) c.gp[i] = d[i];
    } else if (kind == kGenPinball) {
        const int16_t d[6] = {60, 30, 45, 2, 60, 0};   // speed, gravity, bumpers, range, gate, seed
        for (int i = 0; i < 6; ++i) c.gp[i] = d[i];
    } else if (kind == kGenMarkov) {
        const int16_t d[6] = {65, 60, 15, 2, 70, 0};   // density, stepwise, leaps, range, gate, seed
        for (int i = 0; i < 6; ++i) c.gp[i] = d[i];
    }
    c.every = 1;
}

void Konsol::newProject(bool demo) {
    const bool wasHeld = hold_;
    hold_ = true;
    for (int t = 0; t < kTracks; ++t) {
        rt_[t].play = -1;
        rt_[t].queued = -2;
        rt_[t].nev = 0;
        for (int e = 0; e < kEngCount; ++e)
            if (eng_[t].e[e]) eng_[t].e[e]->allOff(true);
    }
    songMode_ = false;
    songIdx_ = -1;
    Project& p = *P_;
    // in place: a Project is ~330 KB, never a temporary on the (M4) stack
    static_assert(__has_trivial_destructor(Project), "Project must stay trivially destructible");
    new (&p) Project();
    for (int t = 0; t < kTracks; ++t) {
        TrackCfg& tc = p.track[t];
        tc.mix = Mix();
        tc.mix.stem = uint8_t(t / 2);
        for (int k = 0; k < kEngCount; ++k) eng_[t].e[k]->preset(0);
        if (!smp_[t].len || smp_[t].path[0]) buildBreak(smp_[t]);
    }
    std::snprintf(p.name, sizeof p.name, demo ? "DEMO" : "SONG01");
    p.bpm = 122.0f;
    p.key = 9;    // A
    p.scale = 1;  // minor
    static const uint8_t kEng[kTracks] = {kEngDrums, kEngSampler, kEngAcd, kEngVap, kEngMakam, kEngIkeda, kEngGrain, kEngPerc};
    static const uint8_t kOct[kTracks] = {2, 2, 1, 3, 3, 4, 3, 4};
    for (int t = 0; t < kTracks; ++t) {
        p.track[t].engine = demo ? kEng[t] : uint8_t(t == 0 ? kEngDrums : (t == 1 ? kEngSampler : kEngAcd));
        p.track[t].octave = kOct[t];
    }
    makamSetKey(eng_[4].e[kEngMakam], p.key);
    if (demo) {
        auto& tr = p.track;
        tr[0].mix.level = 0.85f;
        tr[1].mix.level = 0.62f;
        tr[1].mix.sendB = 0.15f;
        tr[2].mix.level = 0.62f;
        tr[2].mix.sendA = 0.3f;
        tr[3].mix.level = 0.5f;
        tr[3].mix.sendB = 0.45f;
        tr[3].mix.pan = -0.15f;
        tr[4].mix.level = 0.6f;
        tr[4].mix.sendA = 0.25f;
        tr[4].mix.sendB = 0.3f;
        tr[4].mix.pan = 0.2f;
        tr[5].mix.level = 0.45f;
        tr[5].mix.sendA = 0.35f;
        tr[6].mix.level = 0.5f;
        tr[6].mix.sendB = 0.6f;
        tr[7].mix.level = 0.55f;
        tr[7].mix.pan = 0.25f;
        // Sampler track: Slices mode is the preset.
        // ---- clips ----
        // T1 drums: scene 0 four-to-floor, 1 broken, 2 sparse, 3 full
        for (int s = 0; s < 4; ++s) {
            Clip& c = p.clip[0][s];
            c.lenSteps = 16;
            NoteBuf& b = c.buf[0];
            if (s != 2) {
                if (s == 1) {
                    put(b, 0, 36); put(b, 6, 36, 90); put(b, 10, 36); put(b, 4, 37); put(b, 12, 37);
                } else {
                    for (int k = 0; k < 16; k += 4) put(b, k, 36);
                    put(b, 4, 38, 90); put(b, 12, 38, 90);
                }
            } else {
                put(b, 0, 36); put(b, 8, 36, 80);
            }
            for (int k = 2; k < 16; k += 4) put(b, k, 39, s == 2 ? 50 : 70);
            if (s >= 1) { put(b, 14, 40, 60); }
            if (s == 3) { put(b, 7, 38, 45); put(b, 15, 38, 50); put(b, 11, 42, 60); }
        }
        // T2 sampler break (slices): scene 1 straight, scene 1 BBCut live, scene 3 BBCut live
        {
            Clip& c = p.clip[1][1];
            c.lenSteps = 16;
            for (int k = 0; k < 16; ++k) put(c.buf[0], k, 36 + k, k % 4 == 0 ? 110 : 85);
            for (int s : {2, 3}) {
                Clip& d = p.clip[1][s];
                d.lenSteps = 16;
                defaultGen(d, kGenBBCut);
                d.live = 1;
                d.gp[0] = s == 2 ? 2 : 5;
            }
        }
        // T3 acid: generated (seeded), live mutate in scenes 1..3
        for (int s = 0; s < 4; ++s) {
            Clip& c = p.clip[2][s];
            if (s == 2) continue;
            c.lenSteps = 16;
            defaultGen(c, kGenAcid);
            c.gp[5] = int16_t(17 + s);
            c.live = s >= 1;
        }
        // T4 VAP chords (i - VI - III - VII in A minor), 2 bars
        {
            static const int kCh[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
            for (int s : {0, 2, 3}) {
                Clip& c = p.clip[3][s];
                c.lenSteps = 32;
                for (int k = 0; k < 4; ++k)
                    for (int j = 0; j < 3; ++j) put(c.buf[0], k * 8, kCh[k][j], 80, s == 2 ? 8 : 3);
            }
        }
        // T5 makam (Hicaz): Markov melody, live in scene 3
        for (int s : {1, 2, 3}) {
            Clip& c = p.clip[4][s];
            c.lenSteps = 32;
            defaultGen(c, kGenMarkov);
            c.gp[5] = int16_t(5 + s);
            c.live = s == 3;
            c.every = 2;
        }
        // T6 ikeda: euclid clicks / sines
        for (int s : {0, 1, 3}) {
            Clip& c = p.clip[5][s];
            c.lenSteps = 16;
            defaultGen(c, kGenEuclid);
            c.gp[0] = int16_t(s == 1 ? 7 : 5);
            c.gp[3] = 5;
        }
        // T7 grain: long drones
        for (int s : {2, 3}) {
            Clip& c = p.clip[6][s];
            c.lenSteps = 32;
            put(c.buf[0], 0, 57, 90, 16);
            put(c.buf[0], 16, s == 2 ? 60 : 55, 90, 16);
        }
        // T8 PERC: usul aksak 9/8 (polymeter against 4/4)
        for (int s : {1, 3}) {
            Clip& c = p.clip[7][s];
            defaultGen(c, kGenUsul);
            c.gp[0] = s == 1 ? 0 : 2;
            c.lenSteps = 18;
        }
        // generated clips get their notes now
        for (int t = 0; t < kTracks; ++t)
            for (int s = 0; s < kScenes; ++s) {
                Clip& c = p.clip[t][s];
                if (c.exists() && c.gen != kGenNotes) runGenerator(t, s, 0);
            }
        // arrangement
        static const uint8_t kSongS[6] = {0, 1, 2, 3, 1, 3};
        static const uint8_t kSongB[6] = {4, 8, 4, 8, 4, 8};
        for (int i = 0; i < 6; ++i) {
            p.song[i].scene = kSongS[i];
            p.song[i].bars = kSongB[i];
        }
        p.songLen = 6;
        eng_[2].e[kEngAcd]->preset(0);
        eng_[1].e[kEngSampler]->preset(0);
        eng_[4].e[kEngMakam]->preset(0);
        eng_[3].e[kEngVap]->preset(0);
    }
    setLaunchQ_ = p.launchQ;
    setRecQ_ = p.recQ;
    setSwing_ = p.swing;
    setKey_ = p.key;
    setScale_ = p.scale;
    setMetro_ = p.metronome;
    setLoop_ = p.songLoop;
    selT_ = 0;
    selS_ = 0;
    sceneTop_ = 0;
    songCur_ = 0;
    fxDirty_ = true;
    if (ctx_) ctx_->setBpm(p.bpm);
    dirty_ = false;
    hold_ = wasHeld;
}

// ============================================================================
// audio
// ============================================================================
void Konsol::schedule(int t, const Ev& e) {
    TrackRt& r = rt_[t];
    if (r.nev < int(sizeof(r.ev) / sizeof(r.ev[0]))) {
        r.ev[r.nev++] = e;
        return;
    }
    // full: keep note-offs, drop the oldest roll hit
    if (e.on) return;
    for (int i = 0; i < r.nev; ++i)
        if (r.ev[i].on) {
            r.ev[i] = e;
            return;
        }
}

void Konsol::midiFromAudio(int t, bool on, int pitch, int vel) {
    if (!P_->track[t].mix.midiOut) return;
    MidiMsg m;
    m.status = uint8_t((on ? 0x90 : 0x80) | ((ctx_->midiChannel() + t) & 15));
    m.d1 = uint8_t(pitch & 127);
    m.d2 = uint8_t(on ? (vel & 127) : 0);
    ctx_->midiOut(m);
}

bool Konsol::admit(int t, int32_t extra) {
    Engine* e = engine(t);
    int32_t total = 0;
    for (int k = 0; k < kTracks; ++k) total += engine(k)->cost();
    int guard = 0;
    const int32_t budget = int32_t(float(kEngineBudget) * budgetScale_);
    while (total + extra > budget && guard++ < 8) {
        // steal on this track first, then on the costliest other track
        Engine* victim = nullptr;
        if (e->active()) victim = e;
        else {
            int32_t best = 0;
            for (int k = 0; k < kTracks; ++k) {
                const int32_t c = engine(k)->cost();
                if (k != t && c > best) {
                    best = c;
                    victim = engine(k);
                }
            }
        }
        if (!victim || !victim->steal()) return false;
        total = 0;
        for (int k = 0; k < kTracks; ++k) total += engine(k)->cost();
    }
    return total + extra <= budget;
}

void Konsol::fire(int t, int pitch, int vel, int tone, int flags, int offset, uint32_t at, uint32_t lenSub) {
    Engine* e = engine(t);
    if (!admit(t, e->noteCost())) return;
    const float v = float(vel < 1 ? 1 : (vel > 127 ? 127 : vel)) * (1.0f / 127.0f);
    TrackRt& r = rt_[t];
    for (int i = 0; i < r.nev;) {   // a pending note-off of this pitch would cut the new note
        if (!r.ev[i].on && r.ev[i].pitch == pitch) {
            r.ev[i] = r.ev[--r.nev];
            continue;
        }
        ++i;
    }
    e->noteOn(pitch, v, offset, tone, flags);
    rt_[t].flash = 6;
    midiFromAudio(t, true, pitch, vel);
    if (lenSub) {
        Ev off{at + lenSub, uint8_t(pitch), 0, 0, 0};
        schedule(t, off);
    }
}

void Konsol::startClip(int t, int scene, uint32_t at) {
    TrackRt& r = rt_[t];
    // release what the old clip holds
    for (int i = 0; i < r.nev; ++i)
        if (!r.ev[i].on) engine(t)->noteOff(r.ev[i].pitch), midiFromAudio(t, false, r.ev[i].pitch, 0);
    r.nev = 0;
    r.play = int8_t(scene);
    r.startPos = at;
    r.loops = 0;
    r.queued = -2;
}

void Konsol::stopClip(int t) {
    TrackRt& r = rt_[t];
    for (int i = 0; i < r.nev; ++i)
        if (!r.ev[i].on) engine(t)->noteOff(r.ev[i].pitch), midiFromAudio(t, false, r.ev[i].pitch, 0);
    r.nev = 0;
    r.play = -1;
    r.queued = -2;
}

void Konsol::applyCmd(const Cmd& c, uint32_t pos) {
    const int t = c.track & 7;
    TrackRt& r = rt_[t];
    const int q = c.b && (c.type == Cmd::kStop || c.type == Cmd::kLaunch) ? 0 : launchQBeats(P_->launchQ);
    uint32_t at = pos;
    if (q > 0 && wasPlaying_) {
        const uint32_t qs = uint32_t(q) * kSubPerBeat;
        at = ((pos + qs - 1) / qs) * qs;
    }
    switch (c.type) {
        case Cmd::kNoteOn:
            fire(t, c.a, c.b, 0, 0, 0, pos, 0);
            if (r.nHeld < 8) r.heldPitch[r.nHeld++] = c.a;
            break;
        case Cmd::kNoteOff:
            engine(t)->noteOff(c.a);
            midiFromAudio(t, false, c.a, 0);
            for (int i = 0; i < r.nHeld; ++i)
                if (r.heldPitch[i] == c.a) {
                    r.heldPitch[i] = r.heldPitch[--r.nHeld];
                    break;
                }
            break;
        case Cmd::kLaunch:
            if (!P_->clip[t][c.a & 15].exists()) {
                r.queued = -1;
            } else {
                r.queued = int8_t(c.a & 15);
            }
            r.launchAt = at;
            break;
        case Cmd::kStop:
            r.queued = -1;
            r.launchAt = at;
            if (!wasPlaying_) stopClip(t);
            break;
        case Cmd::kScene:
            for (int k = 0; k < kTracks; ++k) {
                rt_[k].queued = P_->clip[k][c.a & 15].exists() ? int8_t(c.a & 15) : int8_t(-1);
                rt_[k].launchAt = at;
            }
            break;
        case Cmd::kStopAll:
            for (int k = 0; k < kTracks; ++k) {
                rt_[k].queued = -1;
                rt_[k].launchAt = at;
                if (!wasPlaying_) stopClip(k);
            }
            break;
        case Cmd::kAllOff:
            for (int k = 0; k < kTracks; ++k) {
                engine(k)->allOff(false);
                rt_[k].nev = 0;
            }
            break;
        case Cmd::kPanic:
            for (int k = 0; k < kTracks; ++k) {
                for (int e = 0; e < kEngCount; ++e) eng_[k].e[e]->allOff(true);
                rt_[k].nev = 0;
                rt_[k].nHeld = 0;
            }
            break;
        default: break;
    }
}

// Notes of track t's playing clip that start in [pos, end).
void Konsol::trackNotes(int t, uint32_t pos, uint32_t end, float subPerSample) {
    TrackRt& r = rt_[t];
    if (r.play < 0 || end <= pos) return;
    Clip& c = P_->clip[t][r.play];
    if (!c.exists()) {
        r.play = -1;
        return;
    }
    const uint32_t L = uint32_t(c.lenSteps) * kSubPerStep;
    if (end <= r.startPos) return;
    const uint32_t from = pos > r.startPos ? pos : r.startPos;
    uint32_t k = (from - r.startPos) / L;
    const int swing = P_->swing;
    const uint32_t swingSub = swing > 50 ? uint32_t(float(swing - 50) * (1.0f / 50.0f) * float(kSubPerStep)) : 0;
    for (int guard = 0; guard < 3; ++guard, ++k) {
        const uint32_t loopStart = r.startPos + k * L;
        if (loopStart >= end) break;
        if (k > 0 && loopStart >= from) {   // a loop boundary inside this block
            r.loops = k;
            if (c.pending) {
                c.cur = uint8_t(c.cur ^ 1);
                c.pending = 0;
            }
        }
        const NoteBuf& nb = c.buf[c.cur];
        for (int i = 0; i < nb.count; ++i) {
            const Note& n = nb.n[i];
            uint32_t at = loopStart + uint32_t(n.start) * kSubPerTick;
            if (swingSub && (n.start % (2 * kTps)) == kTps) at += swingSub;
            if (at < from || at >= end) continue;
            if (n.chance < 100) {
                static uint32_t s = 0x9E3779B9u;
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                if (int(s % 100u) >= n.chance) continue;
            }
            const int offset = int(float(at - pos) / subPerSample);
            uint32_t len = uint32_t(n.len ? n.len : 1) * kSubPerTick;
            fire(t, n.pitch, n.vel, n.tone, n.flags, offset < 0 ? 0 : (offset > int(kBlock) - 1 ? int(kBlock) - 1 : offset),
                 at, len);
            if (n.roll > 1) {   // ratchets: n.roll hits per step across the note
                const uint32_t gap = kSubPerStep / n.roll;
                const uint32_t hits = (len + gap - 1) / gap;
                for (uint32_t h = 1; h < hits && h < 16; ++h) {
                    Ev e{at + h * gap, n.pitch, uint8_t(n.vel * (16 - h / 2) / 16), n.tone, 1};
                    schedule(t, e);
                }
            }
        }
    }
    r.tick = uint16_t(((end - r.startPos) % L) / kSubPerTick);
}

void Konsol::songStep(uint32_t pos, uint32_t end, float) {
    if (!songMode_ || !P_->songLen) return;
    // song position: entry songIdx_ started at songStartPos_
    if (songIdx_ < 0) return;
    const SongEntry& e = P_->song[songIdx_];
    const uint32_t entryEnd = songStartPos_ + uint32_t(e.bars ? e.bars : 1) * 4u * kSubPerBeat;
    if (entryEnd >= pos && entryEnd < end) {
        int next = songIdx_ + 1;
        if (next >= P_->songLen) next = P_->songLoop ? 0 : -1;
        if (next < 0) {
            for (int k = 0; k < kTracks; ++k) {
                rt_[k].queued = -1;
                rt_[k].launchAt = entryEnd;
            }
            songIdx_ = -1;
            return;
        }
        songIdx_ = next;
        songStartPos_ = entryEnd;
        const int sc = P_->song[next].scene;
        for (int k = 0; k < kTracks; ++k) {
            const bool has = P_->clip[k][sc].exists();
            // a track already playing this scene keeps going (no restart)
            if (has && rt_[k].play == sc) continue;
            rt_[k].queued = has ? int8_t(sc) : int8_t(-1);
            rt_[k].launchAt = entryEnd;
        }
    }
}

void Konsol::applyFxParams() {
    const FxCfg& f = P_->fx;
    static const float kSync[6] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};   // beats
    const float beats = kSync[f.delaySync > 5 ? 5 : f.delaySync];
    uint32_t t = uint32_t(beats * 60.0f / (bpm_ > 20.0f ? bpm_ : 120.0f) * 44100.0f);
    delay_.setTimeSamples(t);
    delay_.setFeedbackK(uint8_t(clampf(f.delayFb, 0.0f, 0.95f) * 255.0f));
    delay_.setFilter(800.0f * exp2f(f.delayTone * 4.6f));
    delay_.setPingPong(f.pingPong != 0);
    reverb_->setSize(0.35f + 0.6f * f.revSize);
    reverb_->setDamping(f.revDamp);
    reverb_->setDiffusion(0.7f);
    reverb_->setAmount(1.0f);
    reverb_->setInputGain(0.25f);
}

void Konsol::render(const Transport& tr, const AudioIn& in, float* l, float* r) {
    (void)in;
    if (hold_) return;   // a project load / reset is rewriting the clips
    // tempo / delay sync
    if (tr.bpm != bpm_) {
        bpm_ = tr.bpm;
        fxDirty_ = true;
    }
    if (fxDirty_) {
        fxDirty_ = false;
        applyFxParams();
    }
    const float subPerSample = float(tr.beatsPerSample) * float(kSubPerBeat);
    const bool playing = tr.playing;
    uint32_t pos = playing ? uint32_t(tr.beat * double(kSubPerBeat)) : pos_;
    const uint32_t end = playing ? pos + uint32_t(subPerSample * float(kBlock) + 0.5f) : pos;
    if (tr.startedThisBlock || (playing && !wasPlaying_)) {
        // every armed clip starts at bar 0; song mode starts at its cursor
        pos = 0;
        bool armed = false;
        for (int t = 0; t < kTracks; ++t) armed = armed || rt_[t].play >= 0 || rt_[t].queued >= 0;
        if (!armed && !songMode_) {   // nothing launched: Play starts the selected scene
            const int sc = selS_;
            for (int t = 0; t < kTracks; ++t)
                if (P_->clip[t][sc].exists()) rt_[t].queued = int8_t(sc);
        }
        for (int t = 0; t < kTracks; ++t) {
            rt_[t].nev = 0;
            if (rt_[t].queued >= 0) {
                rt_[t].play = rt_[t].queued;
                rt_[t].queued = -2;
            }
            if (rt_[t].queued == -1) {
                rt_[t].play = -1;
                rt_[t].queued = -2;
            }
            rt_[t].startPos = 0;
            rt_[t].loops = 0;
        }
        if (songMode_ && P_->songLen) {
            songIdx_ = songCur_ < P_->songLen ? songCur_ : 0;
            songStartPos_ = 0;
            const int sc = P_->song[songIdx_].scene;
            for (int t = 0; t < kTracks; ++t) rt_[t].play = P_->clip[t][sc].exists() ? int8_t(sc) : int8_t(-1);
        }
        wasPlaying_ = true;
    }
    if (!playing && wasPlaying_) {
        wasPlaying_ = false;
        for (int t = 0; t < kTracks; ++t) {
            TrackRt& rr = rt_[t];
            for (int i = 0; i < rr.nev; ++i)
                if (!rr.ev[i].on) engine(t)->noteOff(rr.ev[i].pitch), midiFromAudio(t, false, rr.ev[i].pitch, 0);
            rr.nev = 0;
            if (rr.queued == -1) rr.play = -1;
            rr.queued = -2;
            rr.tick = 0;
            engine(t)->allOff(false);
        }
        songIdx_ = -1;
    }
    if (songReq_) {
        songReq_ = false;
        if (!songMode_) songIdx_ = -1;
        else if (playing && P_->songLen) {
            // jump in at the next bar
            const uint32_t bar = 4 * kSubPerBeat;
            const uint32_t at = ((pos + bar - 1) / bar) * bar;
            songIdx_ = songCur_ < P_->songLen ? songCur_ : 0;
            songStartPos_ = at;
            const int sc = P_->song[songIdx_].scene;
            for (int t = 0; t < kTracks; ++t) {
                rt_[t].queued = P_->clip[t][sc].exists() ? int8_t(sc) : int8_t(-1);
                rt_[t].launchAt = at;
            }
        }
    }
    // commands
    Cmd c;
    while (cmds_.pop(c)) applyCmd(c, pos);
    for (int t = 0; t < kTracks; ++t) engine(t)->setTempo(tr.bpm);

    if (playing) {
        songStep(pos, end, subPerSample);
        for (int t = 0; t < kTracks; ++t) {
            TrackRt& rr = rt_[t];
            // queued launch / stop inside this block
            if (rr.queued != -2 && rr.launchAt < end) {
                const uint32_t at = rr.launchAt > pos ? rr.launchAt : pos;
                trackNotes(t, pos, at, subPerSample);
                if (rr.queued >= 0) startClip(t, rr.queued, at);
                else stopClip(t);
                trackNotes(t, at, end, subPerSample);
            } else {
                trackNotes(t, pos, end, subPerSample);
            }
            // pending events (note-offs, roll hits)
            for (int i = 0; i < rr.nev;) {
                Ev& e = rr.ev[i];
                if (e.at < end) {
                    if (e.on) {
                        const int off = e.at > pos ? int(float(e.at - pos) / subPerSample) : 0;
                        Ev copy = e;
                        rr.ev[i] = rr.ev[--rr.nev];
                        fire(t, copy.pitch, copy.vel, copy.tone, 0, off, copy.at, kSubPerStep / 3);
                        continue;
                    }
                    engine(t)->noteOff(e.pitch);
                    midiFromAudio(t, false, e.pitch, 0);
                    rr.ev[i] = rr.ev[--rr.nev];
                    continue;
                }
                ++i;
            }
        }
        // metronome
        if (P_->metronome) {
            const uint32_t beat = end / kSubPerBeat;
            if (beat != lastBeatSub_ && (beat * kSubPerBeat) >= pos) {
                metroEnv_ = (beat % 4) == 0 ? 0.5f : 0.3f;
                metroPh_ = 0;
            }
            lastBeatSub_ = beat;
        }
        pos_ = end;
    }
    posPub_ = playing ? end : 0;

    // ---- engines -> mixer ----------------------------------------------------
    alignas(16) float tl[kBlock], trr[kBlock], sa[kBlock], sb[kBlock];
    alignas(16) float st[kStems][2][kBlock];
    const bool stems = ctx_->shell && ctx_->shell->recording();
    for (uint32_t i = 0; i < kBlock; ++i) sa[i] = sb[i] = 0.0f;
    if (stems)
        for (int s = 0; s < kStems; ++s)
            for (uint32_t i = 0; i < kBlock; ++i) st[s][0][i] = st[s][1][i] = 0.0f;
    bool anySolo = false;
    for (int t = 0; t < kTracks; ++t) anySolo = anySolo || P_->track[t].mix.solo;
    int32_t cost = 0;
    const bool dThrow = dlyThrow_, rThrow = revThrow_;
    for (int t = 0; t < kTracks; ++t) {
        Engine* e = engine(t);
        const Mix& m = P_->track[t].mix;
        const bool audible = !m.mute && (!anySolo || m.solo);
        const float g = audible ? faderGain(m.level) : 0.0f;
        const float pl = g * (m.pan <= 0.0f ? 1.0f : 1.0f - m.pan);
        const float pr = g * (m.pan >= 0.0f ? 1.0f : 1.0f + m.pan);
        const float a = audible ? (dThrow ? 1.0f : m.sendA) : 0.0f;
        const float b = audible ? (rThrow ? 1.0f : m.sendB) : 0.0f;
        if (!e->active()) {
            gl_[t] = pl;
            gr_[t] = pr;
            ga_[t] = a;
            gb_[t] = b;
            continue;
        }
        cost += e->cost();
        for (uint32_t i = 0; i < kBlock; ++i) tl[i] = trr[i] = 0.0f;
        e->render(tl, trr);
        const bool ramp = pl != gl_[t] || pr != gr_[t] || a != ga_[t] || b != gb_[t];
        const bool sends = a > 0.0f || b > 0.0f || ga_[t] > 0.0f || gb_[t] > 0.0f;
        float pk = 0.0f;
        if (ramp) {
            const float dl = (pl - gl_[t]) * (1.0f / kBlock), dr = (pr - gr_[t]) * (1.0f / kBlock);
            const float da = (a - ga_[t]) * (0.5f / kBlock), db = (b - gb_[t]) * (0.5f / kBlock);
            float cl = gl_[t], cr = gr_[t], ca = ga_[t] * 0.5f, cb = gb_[t] * 0.5f;
            for (uint32_t i = 0; i < kBlock; ++i) {
                cl += dl;
                cr += dr;
                ca += da;
                cb += db;
                const float xl = tl[i] * cl, xr = trr[i] * cr;
                l[i] += xl;
                r[i] += xr;
                const float mono = xl + xr;
                sa[i] += mono * ca;
                sb[i] += mono * cb;
                tl[i] = xl;
                trr[i] = xr;
            }
        } else if (sends) {
            const float ca = a * 0.5f, cb = b * 0.5f;
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float xl = tl[i] * pl, xr = trr[i] * pr;
                l[i] += xl;
                r[i] += xr;
                const float mono = xl + xr;
                sa[i] += mono * ca;
                sb[i] += mono * cb;
                tl[i] = xl;
                trr[i] = xr;
            }
        } else {
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float xl = tl[i] * pl, xr = trr[i] * pr;
                l[i] += xl;
                r[i] += xr;
                tl[i] = xl;
                trr[i] = xr;
            }
        }
        for (uint32_t i = 0; i < kBlock; i += 4) {   // meter: every 4th frame is enough
            const float ax = fabsf(tl[i]) + fabsf(trr[i]);
            if (ax > pk) pk = ax;
        }
        pk *= 0.5f;
        gl_[t] = pl;
        gr_[t] = pr;
        ga_[t] = a;
        gb_[t] = b;
        if (pk > rt_[t].peak) rt_[t].peak = pk;
        if (stems && m.stem < kStems)
            for (uint32_t i = 0; i < kBlock; ++i) {
                st[m.stem][0][i] += tl[i];
                st[m.stem][1][i] += trr[i];
            }
    }
    lastCost_ = cost;
    fxBlock(l, r);
    // sends -> returns
    {
        alignas(16) float ol[kBlock], orr[kBlock];
        const float ra = P_->fx.retA, rb = P_->fx.retB;
        if (delay_.process(sa, ol, orr))
            for (uint32_t i = 0; i < kBlock; ++i) {
                l[i] += ol[i] * ra;
                r[i] += orr[i] * ra;
            }
        if (reverb_->process(sb, ol, orr))
            for (uint32_t i = 0; i < kBlock; ++i) {
                l[i] += ol[i] * rb;
                r[i] += orr[i] * rb;
            }
    }
    // master: performance HP, stutter, compressor, limiter, level
    const FxCfg& f = P_->fx;
    {
        // HP sweep (held pad): one-pole pair, cutoff rises while held
        hpCut_ += hpHeld_ ? 0.02f : -0.05f;
        hpCut_ = clampf(hpCut_, 0.0f, 1.0f);
        if (hpCut_ > 0.001f) {
            const float hz = 30.0f * exp2f(hpCut_ * 8.5f);
            const float aa = expf(-6.2831853f * hz / 44100.0f);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float yl = aa * (hpZ_[1] + l[i] - hpZ_[0]);
                hpZ_[0] = l[i];
                hpZ_[1] = yl;
                const float yr = aa * (hpZ_[3] + r[i] - hpZ_[2]);
                hpZ_[2] = r[i];
                hpZ_[3] = yr;
                l[i] = yl;
                r[i] = yr;
            }
        }
        // stutter: the master goes into a ring; held = repeat a 1/16 slice
        const bool held = stutHeld_;
        if (held && !stutActive_) {
            stutActive_ = true;
            int32_t len = int32_t(60.0f / (bpm_ > 20.0f ? bpm_ : 120.0f) * 44100.0f * 0.25f);
            if (len > kStutterMax - 256) len = kStutterMax - 256;
            if (len < 256) len = 256;
            stutLen_ = len;
            stutStart_ = (stutW_ - len + kStutterMax) % kStutterMax;
            stutR_ = 0;
        } else if (!held) {
            stutActive_ = false;
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (!stutActive_) {
                stutter_[2 * stutW_] = int16_t(clampf(l[i], -1.0f, 1.0f) * 32767.0f);
                stutter_[2 * stutW_ + 1] = int16_t(clampf(r[i], -1.0f, 1.0f) * 32767.0f);
                if (++stutW_ >= kStutterMax) stutW_ = 0;
            } else {
                int32_t p = stutStart_ + stutR_;
                if (p >= kStutterMax) p -= kStutterMax;
                // short fades at the slice edges
                const int32_t e1 = stutR_, e2 = stutLen_ - stutR_;
                const float fade = (e1 < 64 ? float(e1) / 64.0f : 1.0f) * (e2 < 64 ? float(e2) / 64.0f : 1.0f);
                l[i] = float(stutter_[2 * p]) * (1.0f / 32768.0f) * fade;
                r[i] = float(stutter_[2 * p + 1]) * (1.0f / 32768.0f) * fade;
                if (++stutR_ >= stutLen_) stutR_ = 0;
            }
        }
        // metronome click
        if (metroEnv_ > 0.0001f) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                metroPh_ += 1500.0f / 44100.0f;
                if (metroPh_ >= 1.0f) metroPh_ -= 1.0f;
                const float s = (metroPh_ < 0.5f ? 1.0f : -1.0f) * metroEnv_;
                l[i] += s;
                r[i] += s;
                metroEnv_ *= 0.9985f;
            }
        }
        // compressor (feed-forward, stereo-linked, gain at 16-sample rate)
        const float thrDb = -30.0f + 30.0f * f.compThr;
        const float ratio = 1.0f + 9.0f * f.compRatio;
        const float makeup = powf(10.0f, (12.0f * f.compMakeup) * 0.05f);
        const bool compOn = f.compRatio > 0.01f;
        const float thrLin = powf(10.0f, thrDb * 0.05f);
        const float att = 0.3f, rel = 0.0025f;
        float gr = 0.0f;
        for (uint32_t i0 = 0; i0 < kBlock; i0 += 16) {
            float g = 1.0f;
            if (compOn) {
                float pk = 0;
                for (uint32_t i = i0; i < i0 + 16; ++i) {
                    const float a = fabsf(l[i]) > fabsf(r[i]) ? fabsf(l[i]) : fabsf(r[i]);
                    if (a > pk) pk = a;
                }
                compEnv_ += (pk - compEnv_) * (pk > compEnv_ ? att : rel * 16.0f);
                g = makeup;
                if (compEnv_ > thrLin) {   // above the threshold only: the dB math
                    const float lv = 20.0f * log10f(compEnv_);
                    const float redDb = (lv - thrDb) * (1.0f - 1.0f / ratio);
                    if (redDb > gr) gr = redDb;
                    g = powf(10.0f, -redDb * 0.05f) * makeup;
                }
            }
            const float dg = (g - compGain_) * (1.0f / 16.0f);
            float cg = compGain_;
            for (uint32_t i = i0; i < i0 + 16; ++i) {
                cg += dg;
                l[i] *= cg;
                r[i] *= cg;
            }
            compGain_ = g;
        }
        compGr_ = gr;
        // limiter + master level
        const float mv = f.master;
        float pk = 0;
        for (uint32_t i = 0; i < kBlock; ++i) {
            float xl = l[i] * mv, xr = r[i] * mv;
            if (f.limiter) {
                const float a = fabsf(xl) > fabsf(xr) ? fabsf(xl) : fabsf(xr);
                const float want = a > 0.92f ? 0.92f / a : 1.0f;
                limGain_ = want < limGain_ ? want : limGain_ + (1.0f - limGain_) * 0.0003f;
                xl *= limGain_;
                xr *= limGain_;
            }
            l[i] = xl;
            r[i] = xr;
            const float a = fabsf(xl) > fabsf(xr) ? fabsf(xl) : fabsf(xr);
            if (a > pk) pk = a;
        }
        if (pk > masterPk_) masterPk_ = pk;
    }
    if (stems)
        for (int s = 0; s < kStems; ++s) ctx_->stem(s, st[s][0], st[s][1]);
    ++blockCount_;
    for (int t = 0; t < kTracks; ++t)
        if (rt_[t].flash) --rt_[t].flash;
}

void Konsol::fxBlock(float* l, float* r) {
    (void)l;
    (void)r;
}

void Konsol::onTransport(bool playing) {
    if (!playing) {
        for (int k = 0; k < 8; ++k) rec_[k].pitch = -1;
    }
}

// ============================================================================
// bench scenarios (make lab-gate)
// ============================================================================
const char* Konsol::benchName(int i) const {
    static const char* const kN[6] = {"idle", "demo_scene4", "synth8_busy", "samples8", "gen_live", "worst_fx"};
    return kN[i < 0 ? 0 : (i > 5 ? 5 : i)];
}

void Konsol::benchSetup(int i) {
    // reset playback
    for (int t = 0; t < kTracks; ++t) {
        for (int e = 0; e < kEngCount; ++e) eng_[t].e[e]->allOff(true);
        rt_[t] = TrackRt();
    }
    wasPlaying_ = false;
    P_->launchQ = 0;
    Cmd stale;
    while (cmds_.pop(stale)) {
    }
    if (i == 0) {
        selS_ = kScenes - 1;   // an empty scene: Play starts nothing
        return;
    }
    if (i == 1) {   // the demo's busiest scene, every clip running
        newProject(true);
        while (cmds_.pop(stale)) {
        }
        P_->launchQ = 0;
        for (int t = 0; t < kTracks; ++t) rt_[t].queued = P_->clip[t][3].exists() ? 3 : -2;
        return;
    }
    // 8 tracks, every step busy, on the costliest engines
    static const uint8_t kHeavy[kTracks] = {kEngWtfm, kEngFat, kEngVap, kEngAcd, kEngWtfm, kEngFat, kEngVap, kEngPerc};
    static const uint8_t kSamp[kTracks] = {kEngSampler, kEngSampler, kEngGrain, kEngGrain,
                                           kEngSampler, kEngDrums,   kEngMakam, kEngIkeda};
    for (int t = 0; t < kTracks; ++t) {
        int kind = i == 3 ? kSamp[t] : kHeavy[t];
        if (i == 4) kind = t < 2 ? kEngAcd : (t < 4 ? kEngSampler : (t < 6 ? kEngMakam : kEngDrums));
        setEngine(t, kind);
        Engine* e = engine(t);
        e->preset(0);
        if (kind == kEngWtfm) e->set(8, 1.0f);   // oversampling on: the costliest WTFM
        if (kind == kEngSampler) {
            e->set(0, 1.0f);   // loop mode: voices never end
            e->set(8, 4.0f);
        }
        if (e->kind() >= kEngAcd && e->kind() <= kEngWtfm) e->set(e->paramCount() - 1, 4.0f);   // 4 voices
        Clip& c = P_->clip[t][0];
        c.lenSteps = 16;
        c.cur = 0;
        c.pending = 0;
        NoteBuf& b = c.buf[0];
        b.clear();
        for (int s = 0; s < 16; ++s) {
            Note n;
            n.start = uint16_t(s * kTps);
            n.len = uint16_t(kTps * 4);   // overlapping notes: chords build up
            n.pitch = uint8_t(kind == kEngPerc ? 60 + (s % 12) : (kind == kEngDrums ? 36 + s % 8 : 48 + (s * 7) % 24));
            n.vel = 110;
            if (i == 5 && (s % 4) == 3) n.roll = 4;
            b.add(n);
            if (kind != kEngPerc && kind != kEngDrums && i != 3) {   // + a second voice
                n.pitch = uint8_t(n.pitch + 7);
                b.add(n);
            }
        }
        if (i == 4) {
            defaultGen(c, t < 2 ? kGenAcid : (t < 4 ? kGenBBCut : (t < 6 ? kGenMarkov : kGenEuclid)));
            c.live = 1;
            runGenerator(t, 0, 7);
        }
        Mix& m = P_->track[t].mix;
        m.sendA = 0.5f;
        m.sendB = 0.5f;
        m.mute = m.solo = 0;
        rt_[t].queued = 0;
    }
    P_->fx.compRatio = 0.6f;
    P_->metronome = i == 5;
    if (i == 5) {
        hpHeld_ = true;
        dlyThrow_ = true;
        revThrow_ = true;
    } else {
        hpHeld_ = dlyThrow_ = revThrow_ = false;
    }
    fxDirty_ = true;
}

}  // namespace konsol

Workstation* createWorkstation(Arena& arena) { return arena.make<konsol::Konsol>(); }

}  // namespace lab
