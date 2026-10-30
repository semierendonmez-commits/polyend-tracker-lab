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
#include "ws/kit.h"
#include "ws/stretch.h"

namespace lab {
namespace konsol {

void makamSetKey(Engine* e, int key);

namespace {
constexpr uint32_t kSubPerTick = 256;                     // global position unit
constexpr uint32_t kSubPerBeat = kPpq * kSubPerTick;      // 12288
constexpr uint32_t kSubPerStep = kTps * kSubPerTick;      // 3072
constexpr uint32_t kPoolSamples = 5200u * 1024u;          // the shared sample pool (int16, ~10.6 MB)
// Engines may use this many M4 instructions per block in total; the rest of
// the 180k gate is mixer, sends, master and the shell.
// Engine costs below are estimates calibrated against QEMU (tests/lab bench);
// the fixed part (mixer, delay, reverb, compressor, limiter, shell) is ~55k.
constexpr int32_t kEngineBudget = 100000;
// Every other sounding track costs the mixer (gains, sends, meters, stems) about this much.
constexpr int32_t kTrackOverhead = 3800;

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
    undo_ = static_cast<UndoRec*>(a.alloc(sizeof(UndoRec) * kUndo));
    redo_ = static_cast<UndoRec*>(a.alloc(sizeof(UndoRec) * kUndo));
    lkitBuf_ = static_cast<uint8_t*>(a.alloc(kLkitCap));
    acidPh_ = a.alloc(sizeof(ot::gen::MonoStep) * kMaxSteps * kTracks);
    list_ = static_cast<char(*)[40]>(a.alloc(kListMax * 40));
    stutter_ = static_cast<int16_t*>(a.alloc(kStutterMax * 2 * sizeof(int16_t)));
    int16_t* rmem = static_cast<int16_t*>(a.alloc(ot::audio::Reverb::kMemorySamples * sizeof(int16_t)));
    reverb_ = a.make<ot::audio::Reverb>();
    if (!P_ || !scratch_ || !src_ || !undo_ || !redo_ || !lkitBuf_ || !acidPh_ || !list_ || !stutter_ || !rmem || !reverb_)
        return false;
    if (!dly_.init(a) || !verb_.init(a)) return false;
    reverb_->init(rmem);
    str_ = a.make<Stretcher>();
    if (!str_ || !str_->init(a)) return false;
    // SD streaming: 8 rings (2 MB) and the conversion job, before the pool
    // takes the rest -- only when the Shell hands over a stream reader (a
    // firmware image with a card, the simulator); a PLATFORM station (no
    // reader, or an older lab.h) keeps every sample in memory and the 2 MB.
    ot::audio::IStreamReader* reader = nullptr;
#if defined(LAB_HAS_STREAM_READER)
    if (c.fs) reader = c.streamReader;
#endif
    bool wantStreams = reader != nullptr;
#if defined(OT_BENCH_M4) || !defined(__arm__)
    wantStreams = wantStreams || !c.fs;   // the CPU bench (no card): stream8 brings a synthetic reader
#endif
    if (wantStreams) {
        streams_ = a.make<Streams>();
        if (streams_ && !streams_->init(a)) streams_ = nullptr;
        conv_ = a.make<StreamConv>();
        if (conv_ && !conv_->init(a)) conv_ = nullptr;
        if (streams_) streams_->setReader(reader);
    }
    pool_.setStreams(streams_);
    for (int t = 0; t < kTracks; ++t)
        if (!eng_[t].init(a, &pool_, &ts_[t], t)) return false;
    // LABKIT: the browser (listing + a 4 s preview) and the registry before the pool takes the rest
    c.browser().reserve(256, 4.0f);
    // the pool takes what is left, minus a reserve for the Shell (LABKIT) and later needs
    {
        const uint32_t freeB = a.free();
        uint32_t want = kPoolSamples * 2u;
        if (freeB < want + 512u * 1024u) want = freeB > 1024u * 1024u ? freeB - 512u * 1024u : freeB / 2u;
        if (!pool_.init(a, want / 2u)) return false;
    }
    ensureBreak();
    for (auto& p : padPitch_) p = -1;
    for (auto& p : padTrack_) p = -1;
    newProject(true);
    registerParams();
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

void Konsol::setEngine(int t, int kind) {
    if (kind < 0 || kind >= kEngCount) return;
    Engine* old = engine(t);
    if (old) old->allOff(true);
    P_->track[t].engine = uint8_t(kind);
    makamSetKey(eng_[t].e[kEngMakam], P_->key);
    refreshSlots(t);   // LABKIT: the track's slots follow the new engine
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
    convPend_ = 0;   // SD streaming: conversions of the old project's files stop
    convTrack_ = -1;
    if (conv_) conv_->cancel();
    Project& p = *P_;
    // in place: a Project is ~330 KB, never a temporary on the (M4) stack
    static_assert(__has_trivial_destructor(Project), "Project must stay trivially destructible");
    new (&p) Project();
    for (int t = 0; t < kTracks; ++t) {
        TrackCfg& tc = p.track[t];
        tc.mix = Mix();
        tc.mix.stem = uint8_t(t / 2);
        for (int k = 0; k < kEngCount; ++k) eng_[t].e[k]->preset(0);
        setTrackBreak(t);
        ts_[t].stream = kStreamAuto;
        for (auto& p : *reinterpret_cast<KitPad(*)[kKitPads]>(kitPads(eng_[t].e[kEngKit]))) p = KitPad();
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

int32_t Konsol::engineBudget(int extraTrack) const {
    int act = 0;
    for (int k = 0; k < kTracks; ++k) act += (engine(k)->active() || k == extraTrack) ? 1 : 0;
    // the send effects beyond the OpenTracker plate + delay the budget was calibrated with
    // (QEMU: plate 19k, OT delay 10k; hall 27k, shimmer 36.5k, gated 29k; delay 11.6k / tape 16.9k / lo-fi 13.7k)
    static const int32_t kRevExtra[kRevTypes] = {0, 8000, 17500, 10000};
    static const int32_t kDlyExtra[kDlyTypes] = {1500, 6700, 3500};
    const int32_t fx = kRevExtra[P_->fx.revType % kRevTypes] + kDlyExtra[P_->fx.dlyType % kDlyTypes];
    const int32_t b = kEngineBudget - kTrackOverhead * (act > 1 ? act - 1 : 0) - fx;
    return int32_t(float(b) * budgetScale_);
}

bool Konsol::admit(int t, int32_t extra) {
    Engine* e = engine(t);
    int32_t total = 0;
    for (int k = 0; k < kTracks; ++k) total += engine(k)->cost();
    int guard = 0;
    const int32_t budget = engineBudget(t);
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
    noteEnv_[t] = v;
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
            if (P_->clip[t][c.a & 15].launchQ && !c.b && wasPlaying_) {   // the clip's own quantise
                const int cq = launchQBeats(P_->clip[t][c.a & 15].launchQ - 1);
                at = pos;
                if (cq > 0) {
                    const uint32_t qs = uint32_t(cq) * kSubPerBeat;
                    at = ((pos + qs - 1) / qs) * qs;
                }
            }
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
            if (n.cond && !condPasses(n.cond, k, fillHeld_)) continue;
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
    dly_.setTime(t);
    dly_.set(f.dlyType, clampf(f.delayFb, 0.0f, 0.95f), f.delayTone, f.pingPong != 0, f.dlyMod);
    verb_.set(f.revType, f.revSize, f.revDamp, f.revPre);
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

    // ---- governor: engines that grow by themselves (clouds) never pass the budget
    {
        int32_t total = 0;
        for (int t = 0; t < kTracks; ++t) total += engine(t)->cost();
        const int32_t budget = engineBudget(-1);
        for (int guard = 0; total > budget && guard < 8; ++guard) {
            Engine* victim = nullptr;
            int32_t best = 0;
            for (int t = 0; t < kTracks; ++t) {
                const int32_t c = engine(t)->cost();
                if (c > best) {
                    best = c;
                    victim = engine(t);
                }
            }
            if (!victim || !victim->steal()) break;
            total = 0;
            for (int t = 0; t < kTracks; ++t) total += engine(t)->cost();
        }
    }
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
    // modulation sources (LABKIT): note envelopes, bar / beat ramps
    for (int t = 0; t < kTracks; ++t) noteEnv_[t] *= 0.985f;
    {
        const uint32_t p = posPub_;
        barPh_ = float(p % (4 * kSubPerBeat)) / float(4 * kSubPerBeat);
        beatPh_ = float(p % kSubPerBeat) / float(kSubPerBeat);
    }
    fxBlock(l, r);
    // sends -> returns
    {
        alignas(16) float ol[kBlock], orr[kBlock];
        const float ra = P_->fx.retA, rb = P_->fx.retB;
        if (dly_.process(sa, ol, orr))
            for (uint32_t i = 0; i < kBlock; ++i) {
                l[i] += ol[i] * ra;
                r[i] += orr[i] * ra;
            }
        const bool rv = P_->fx.revType == 0 ? reverb_->process(sb, ol, orr) : verb_.process(sb, ol, orr);
        if (rv)
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
// bench scenarios (make lab-gate): the QEMU bench and the host tests only --
// the device image leaves them out (flash).
// ============================================================================
#if defined(OT_BENCH_M4) || !defined(__arm__)
const char* Konsol::benchName(int i) const {
    static const char* const kN[] = {"idle",     "demo_scene4", "synth8_busy", "samples8", "gen_live", "worst_fx",
                                     "plaits8",  "semi8",       "kit_warp", "fx_shimmer_tape", "mod_slots8",
                                     // solo engines (calibration of the governor's cost estimates)
                                     "s_sampler", "s_grain", "s_drums", "s_ikeda", "s_makam", "s_acd", "s_vap", "s_fat",
                                     "s_wtfm", "s_perc", "s_plaits", "s_floater", "s_tb303", "s_neuro", "s_quanta", "s_kit",
                                     // SD streaming: 8 SAMPLER voices from the card (a synthetic reader)
                                     "stream8"};
    const int n = int(sizeof kN / sizeof kN[0]);
    return kN[i < 0 ? 0 : (i >= n ? n - 1 : i)];
}

void Konsol::benchSetup(int i) {
    ctx_->mod().clearRoutes();
    P_->fx.revType = 0;
    P_->fx.dlyType = 0;
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
    if (i >= 6) {
        benchSetupNew(i);
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

void Konsol::benchSetupNew(int i) {
    // i = 6 plaits8, 7 semi8, 8 kit_warp, 9 fx_shimmer_tape, 10 mod_slots8,
    // 11.. one engine alone on track 1 (all voices busy), 27 stream8
    if (i == 27) {
        benchStream8();
        return;
    }
    if (i == 9 || i == 10) {   // the busy synth scenario + the costliest send FX / 8 LFO routes on engine slots
        benchSetup(2);
        if (i == 9) {
            P_->fx.revType = 2;   // shimmer
            P_->fx.dlyType = 1;   // tape
            P_->fx.revSize = 1.0f;
            P_->fx.delayFb = 0.9f;
        } else {
            ctx_->mod().clearRoutes();
            for (int t = 0; t < kTracks; ++t) {
                refreshSlots(t);
                ctx_->mod().route(kit::kSrcLfo + (t & 3), slotParam_[t][t % 4], 0.4f);
            }
        }
        fxDirty_ = true;
        return;
    }
    if (i >= 11) i -= 2;
    static const uint8_t kPl[kTracks] = {kEngPlaits, kEngPlaits, kEngPlaits, kEngPlaits,
                                         kEngPlaits, kEngPlaits, kEngPlaits, kEngPlaits};
    static const uint8_t kSemi[kTracks] = {kEngFloater, kEngTb303, kEngNeuro, kEngQuanta,
                                           kEngFloater, kEngTb303, kEngNeuro, kEngQuanta};
    static const uint8_t kKw[kTracks] = {kEngKit, kEngKit, kEngKit, kEngSampler,
                                         kEngSampler, kEngSampler, kEngGrain, kEngGrain};
    static const uint8_t kModels[kTracks] = {20, 12, 18, 2, 3, 4, 15, 19};   // the costliest Plaits models (modal, harmonic, string, the 6-op FM banks, speech)
    const bool solo = i >= 9;
    for (int t = 0; t < kTracks; ++t) {
        Clip& c = P_->clip[t][0];
        c.lenSteps = 16;
        c.cur = 0;
        c.pending = 0;
        c.gen = kGenNotes;
        c.live = 0;
        c.buf[0].clear();
        rt_[t].queued = -2;
        if (solo && t > 0) continue;
        const int kind = solo ? i - 9 : (i == 6 ? kPl[t] : (i == 7 ? kSemi[t] : kKw[t]));
        setEngine(t, kind);
        Engine* e = engine(t);
        e->preset(0);
        if (kind == kEngPlaits) e->set(0, float(solo ? 6 : kModels[t]));
        if (kind == kEngWtfm) e->set(8, 1.0f);
        if (kind >= kEngAcd && kind <= kEngWtfm) e->set(e->paramCount() - 1, 4.0f);
        if (kind == kEngNeuro) e->set(0, float((t * 5 + 4) % 12));   // Hoover, Snarl, ...
        if (kind == kEngQuanta) {
            e->set(0, float(t % 5));
            e->set(1, 120.0f);   // dense
            e->set(3, 60.0f);
        }
        if (kind == kEngSampler) {
            e->set(0, 1.0f);                               // loop
            e->set(5, float(1 + (t % 2)));                 // warp Beats / Texture
            e->set(10, 4.0f);
        }
        if (kind == kEngKit) kitFromSlices(t, t == 1);
        NoteBuf& b = c.buf[0];
        for (int s = 0; s < 16; ++s) {
            Note n;
            n.start = uint16_t(s * kTps);
            n.len = uint16_t(kind == kEngSampler || kind == kEngGrain ? kTps * 16 : kTps * 4);
            n.vel = 110;
            switch (kind) {
                case kEngFloater: n.pitch = uint8_t(s % 4 == 0 ? 36 : (s % 4 == 2 ? 38 : 60 + s)); break;
                case kEngKit: n.pitch = uint8_t(36 + (s * 5) % 16); break;
                case kEngPerc: n.pitch = uint8_t(60 + (s % 12)); break;
                case kEngDrums: n.pitch = uint8_t(36 + s % 8); break;
                default: n.pitch = uint8_t(36 + (s * 7) % 24); break;
            }
            if ((kind == kEngSampler || kind == kEngGrain) && s % 8) continue;   // long loops / clouds
            if (kind == kEngTb303 && (s % 3) == 1) n.flags = kFlagSlide | kFlagAccent;
            b.add(n);
            if (kind == kEngKit || kind == kEngFloater) {   // + a second hit
                n.pitch = uint8_t(kind == kEngKit ? 36 + (s * 3 + 7) % 16 : 42);
                b.add(n);
            }
            if (solo && kind != kEngKit && kind != kEngFloater && kind != kEngTb303 && kind != kEngNeuro &&
                kind != kEngPlaits && kind != kEngSampler && kind != kEngGrain) {
                n.pitch = uint8_t(n.pitch + 7);
                b.add(n);
            }
        }
        Mix& m = P_->track[t].mix;
        m.sendA = 0.5f;
        m.sendB = 0.5f;
        m.mute = m.solo = 0;
        rt_[t].queued = 0;
    }
    P_->fx.compRatio = 0.6f;
    P_->metronome = 0;
    hpHeld_ = dlyThrow_ = revThrow_ = false;
    fxDirty_ = true;
}

// stream8: every track a SAMPLER looping a 10-minute stereo file from the
// card (FakeStreamReader: no card in the bench), at +0..+12 semitones (the
// streamed limit, 2x speed), from 8 different start points: 8 rings busy,
// the main loop (update(), every 8 blocks in the bench) reading ahead.
void Konsol::benchStream8() {
    static FakeStreamReader fake;
    if (!streams_) return;
    streams_->setReader(&fake);
    int s = -1;
    for (int k = 0; k < kSlots; ++k)
        if (pool_.streamed(k)) s = k;
    if (s < 0) {
        gcSamples();
        s = pool_.alloc(kHeadFrames, 2);
        if (s < 0) return;
        for (uint32_t f = 0; f < kHeadFrames; ++f) {
            pool_.leftW(s)[f] = FakeStreamReader::sample(0, 2 * f);
            pool_.rightW(s)[f] = FakeStreamReader::sample(0, 2 * f + 1);
        }
        SampleSlot& sl = pool_.slot(s);
        copyStr(sl.path, sizeof sl.path, "bench/stream.wav");
        sl.sid = 0;
        sl.sframes = 44100u * 600u;
        sl.stream = 1;
        sl.frames = kHeadFrames;
    }
    for (int t = 0; t < kTracks; ++t) {
        ts_[t].src = ts_[t].slot = int16_t(s);
        ts_[t].stretched = 0;
        setEngine(t, kEngSampler);
        Engine* e = engine(t);
        e->preset(0);
        e->set(0, 1.0f);                         // loop
        e->set(1, 0.05f + 0.1f * float(t));      // start: past the head, a different place per track
        e->set(6, float(t * 12 / 7));            // pitch 0 .. +12
        e->set(10, 1.0f);                        // one voice: 8 streamed voices in all
        e->set(11, 6000.0f);                     // the filter on (the costlier path)
        Clip& c = P_->clip[t][0];
        c.lenSteps = 16;
        c.cur = 0;
        c.pending = 0;
        c.gen = kGenNotes;
        c.live = 0;
        NoteBuf& b = c.buf[0];
        b.clear();
        Note n;
        n.start = 0;
        n.len = uint16_t(kTps * 16);
        n.pitch = 60;
        n.vel = 110;
        b.add(n);
        Mix& m = P_->track[t].mix;
        m.sendA = 0.3f;
        m.sendB = 0.3f;
        m.mute = m.solo = 0;
        rt_[t].queued = 0;
    }
    P_->fx.compRatio = 0.6f;
    P_->metronome = 0;
    hpHeld_ = dlyThrow_ = revThrow_ = false;
    fxDirty_ = true;
}

#else
const char* Konsol::benchName(int) const { return "idle"; }
void Konsol::benchSetup(int) {}
void Konsol::benchSetupNew(int) {}
void Konsol::benchStream8() {}
#endif

}  // namespace konsol

Workstation* createWorkstation(Arena& arena) { return arena.make<konsol::Konsol>(); }

}  // namespace lab
