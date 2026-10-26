// TILT -- the workstation: init, the audio glue (move grid, hits -> voices and
// MIDI), commands, MIDI in, settings, the CPU bench scenarios (tilt_ws.h).
#include "ws/tilt_ws.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace tilt {

using lab::Rgb;

const PageInfo kPages_[kPgCount] = {
    {"GRAPH", "Graph", {"RELAUNCH", "HALT", "+BALL", "NUDGE", "CAPTURE", "REPLAY", "RANDOM", "UNDO"},
     "Steps on a circle, arrows as thick as the weights. Pads: perform."},
    {"MATRIX", "Matrix 17x17", {"WEIGHT", "TRANSP", "DELAY", "VELO", "SHAPE", "ROW FN", "PAGE+", "VIEW"},
     "Row = where the ball is, column = where it goes. D = drain."},
    {"STEPS", "Steps", {"RND TRIG", "CLR STEPS", "ROW=ALL", "LOCK", "ROT <", "ROT >", "COPY", "PASTE"},
     "Each step: the channel it fires and how. Last row: melody."},
    {"TABLE", "Table", {"RELAUNCH", "HALT", "+BALL", "TILT!", "", "", "DEFAULT", ""},
     "Clock, flippers, tilt, ball save, gravity, multiball."},
    {"EVOLVE", "Evolve", {"RANDOM", "UNDO", "R MATRIX", "R STEPS", "R LANES", "R RULES", "R SOUND", "RESET MEM"},
     "Memory, mutation, meta pages, rules, the randomizer."},
    {"COMBOS", "Combos", {"FIRE 1", "FIRE 2", "FIRE 3", "FIRE 4", "CLR PATH", "", "", ""},
     "Paths of 2-4 steps that trigger an action."},
    {"CHANNELS", "Channels", {"KICK", "SNARE", "HAT", "ACID", "FM", "SAMPLE", "COPY", "PASTE"},
     "Eight channels: a sample or a synth each. Pads 0-35 play it."},
    {"MIX", "Mix", {"MUTE", "SOLO", "", "", "", "", "FX", "PANIC"}, "Levels, pan, sends, choke, stems; delay + reverb."},
};

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const Rgb kChColors[kCh] = {Rgb(0xFF4D4D), Rgb(0xFFA62B), Rgb(0x3DF2A0), Rgb(0x2EC4FF),
                            Rgb(0xB07CFF), Rgb(0xC6FF3D), Rgb(0xFF5FD2), Rgb(0x6F8BFF)};
}  // namespace

const char* Tilt::modSourceName(int i) {
    static const char* const k[kModSources] = {"Ball step", "Entropy", "Heat", "Tilt", "Balls", "Page", "Velocity"};
    return (i >= 0 && i < kModSources) ? k[i] : "";
}

Rgb Tilt::chColor(int ch) const { return kChColors[clampi(ch, 0, kCh - 1)]; }
Rgb Tilt::trigColor(int trig) const {
    if (trig >= 1 && trig <= 8) return kChColors[trig - 1];
    if (trig == kTrigMute) return Rgb(0x9A9AB0);
    return Rgb(0x2A2B4A);
}

bool Tilt::init(lab::Context& c) {
    ctx_ = &c;
    theme_.bg = Rgb(0x07081A);
    theme_.panel = Rgb(0x15173A);
    theme_.fg = Rgb(0xF4EEDC);
    theme_.dim = Rgb(0x6E6A9A);
    theme_.accent = Rgb(0xFF2E88);
    theme_.accent2 = Rgb(0x2EE6FF);
    theme_.warn = Rgb(0xFF4A3D);
    theme_.ok = Rgb(0x4DFFA6);
    pat_ = c.arena->make<Pattern>();
    staged_ = c.arena->make<Pattern>();
    undo_ = c.arena->make<Pattern>();
    eng_ = c.arena->make<Engine>();
    io_ = static_cast<uint8_t*>(c.arena->alloc(kIoBytes));
    if (!pat_ || !staged_ || !undo_ || !eng_ || !io_) return false;
    if (!audio_.init(*c.arena)) return false;
    rng_.seed(0x7117u ^ c.millis());
    factoryPattern(0, *pat_);
    eng_->attach(pat_);
    eng_->reset();
    eng_->seed(0x5EEDu);
    audio_.attach(pat_);
    audio_.renderBuiltins();
    if (c.fs) {
        char p[96];
        const char* sub[] = {"Patterns", "Kits", "Midi", "Samples"};
        for (const char* s : sub) {
            std::snprintf(p, sizeof p, "%s/%s", c.home, s);
            c.fs->mkdir(p);
        }
        scanSlots();
        scanWavs();
        std::snprintf(p, sizeof p, "%s/tilt.cfg", c.home);
        uint8_t cfg[4] = {1, 1, 1, 1};
        if (readFile(*c.fs, p, cfg, 4) == 4) {
            setAutoload_ = cfg[0] ? 1 : 0;
            setMidiOut_ = cfg[1] ? 1 : 0;
            setStepNotes_ = cfg[2] ? 1 : 0;
            setTrail_ = cfg[3] ? 1 : 0;
        }
        // the last session
        std::snprintf(p, sizeof p, "%s/state.tlt", c.home);
        const int32_t n = readFile(*c.fs, p, io_, kIoBytes);
        if (setAutoload_ && n > 0 && patternFromBytes(*staged_, io_, uint32_t(n))) {
            std::memcpy(pat_, staged_, sizeof(Pattern));
            eng_->reset();
        }
        loadSamplesFor();
    }
    c.setBpm(float(pat_->bpmX10) * 0.1f);
    return true;
}

void Tilt::post(Cmd::Type t, int a, int b, int c) {
    Cmd m;
    m.type = uint8_t(t);
    m.a = int16_t(a);
    m.b = int16_t(b);
    m.c = int16_t(c);
    cmds_.push(m);
}

// ---- audio side ---------------------------------------------------------------------------------
void Tilt::applyCommands(double beat) {
    Cmd m;
    while (cmds_.pop(m)) {
        switch (m.type) {
            case Cmd::kLaunch:
                eng_->launch(beat);
                launched_ = true;
                break;
            case Cmd::kHalt: eng_->halt(); break;
            case Cmd::kFlip: eng_->flip(m.a, m.b); break;
            case Cmd::kFlipper: eng_->flipper(m.a, m.b); break;
            case Cmd::kNudge: eng_->nudge(m.a); break;
            case Cmd::kBall: eng_->addBall(); break;
            case Cmd::kCombo: eng_->fireCombo(m.a); break;
            case Cmd::kPage:
                eng_->setPage(m.a);
                events_.push(kEvScene);
                break;
            case Cmd::kCapture:
                if (eng_->capture(m.a, beat) > 0) {
                    eng_->setReplay(m.b != 0);
                    events_.push(kEvCapture);
                }
                break;
            case Cmd::kReplay: eng_->setReplay(m.a != 0); break;
            case Cmd::kTilt: eng_->tilt(); break;
            case Cmd::kAudition: {
                Pending e;
                e.at = sample_;
                e.kind = Hit::kNote;
                e.ch = uint8_t(clampi(m.a, 0, kCh - 1));
                e.note = uint8_t(clampi(m.b, 0, 127));
                e.vel = uint8_t(clampi(m.c, 1, 127));
                e.len = 44100 / 4;
                audio_.post(e);
                break;
            }
            case Cmd::kPanic:
                audio_.panic();
                for (int i = 0; i < nOffs_; ++i) ctx_->midiOut(lab::MidiMsg{uint8_t(0x80 | (offs_[i].status & 15)), offs_[i].note, 0});
                nOffs_ = 0;
                break;
            case Cmd::kSwap:
                std::memcpy(pat_, staged_, sizeof(Pattern));
                eng_->reset();
                eng_->setReplay(false);
                if (wasPlaying_) {
                    eng_->launch(beat);
                    launched_ = true;
                }
                audio_.panic();
                break;
            case Cmd::kResetEvolve: {
                for (int r = 0; r < kStates; ++r)
                    for (int c = 0; c < kStates; ++c) eng_->mem[r][c] = 1.0f;
                std::memset(eng_->mut, 0, sizeof eng_->mut);
                break;
            }
            default: break;
        }
        for (int i = 0; i < eng_->nEvents; ++i) events_.push(eng_->events[i]);
        eng_->nEvents = 0;
        // hits made by a command (combo NOTE, CC) play now
        for (int i = 0; i < eng_->nHits; ++i) {
            const Hit& h = eng_->hits[i];
            Pending e;
            e.at = sample_;
            e.kind = h.kind;
            e.ch = h.ch;
            e.note = h.note;
            e.vel = h.vel;
            e.len = uint32_t(h.len * 60.0f / (bpmNow_ > 20.0f ? bpmNow_ : 120.0f) * 44100.0f);
            audio_.post(e);
        }
        eng_->nHits = 0;
    }
}

void Tilt::schedule(const lab::Transport& t, double moveBeat, uint32_t blockStart) {
    const double bps = t.beatsPerSample > 0.0 ? t.beatsPerSample : 120.0 / 60.0 / 44100.0;
    const float spb = float(1.0 / bps);   // samples per beat
    for (int i = 0; i < eng_->nHits; ++i) {
        const Hit& h = eng_->hits[i];
        Pending e;
        const double at = (moveBeat + double(h.at) - t.beat) / bps;
        e.at = blockStart + uint32_t(at > 0.0 ? at : 0.0);
        e.kind = h.kind;
        e.ch = h.ch;
        e.note = h.note;
        e.vel = h.vel;
        e.step = h.step;
        e.ball = h.ball;
        e.chokeMask = h.chokeMask;
        const float len = h.len * spb;
        e.len = uint32_t(len < 32.0f ? 32.0f : len);
        audio_.post(e);
    }
    for (int i = 0; i < eng_->nEvents; ++i) events_.push(eng_->events[i]);
}

void Tilt::midiOut(const Pending& e) {
    if (!setMidiOut_) return;
    if (e.kind == Hit::kCc) {
        const int ch = clampi(pat_->ch[0].midiCh ? pat_->ch[0].midiCh : 1, 1, 16) - 1;
        ctx_->midiOut(lab::MidiMsg{uint8_t(0xB0 | ch), uint8_t(e.note & 127), uint8_t(e.vel & 127)});
        return;
    }
    const Channel& c = pat_->ch[e.ch];
    if (!c.midiCh) return;
    const uint8_t st = uint8_t(c.midiCh - 1);
    // retrigger: end a sounding copy of the same note first
    for (int i = 0; i < nOffs_; ++i)
        if (offs_[i].note == e.note && (offs_[i].status & 15) == st) {
            ctx_->midiOut(lab::MidiMsg{uint8_t(0x80 | st), e.note, 0});
            offs_[i] = offs_[--nOffs_];
            break;
        }
    ctx_->midiOut(lab::MidiMsg{uint8_t(0x90 | st), e.note, e.vel});
    if (nOffs_ >= kOffCap) {   // full: end the oldest now
        ctx_->midiOut(lab::MidiMsg{uint8_t(0x80 | (offs_[0].status & 15)), offs_[0].note, 0});
        offs_[0] = offs_[--nOffs_];
    }
    Off& o = offs_[nOffs_++];
    o.note = e.note;
    o.status = st;
    o.left = int32_t(e.len);
}

void Tilt::runOffs() {
    for (int i = 0; i < nOffs_;) {
        offs_[i].left -= int32_t(lab::kBlock);
        if (offs_[i].left <= 0) {
            ctx_->midiOut(lab::MidiMsg{uint8_t(0x80 | (offs_[i].status & 15)), offs_[i].note, 0});
            offs_[i] = offs_[--nOffs_];
        } else {
            ++i;
        }
    }
}

void Tilt::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    (void)in;
    bpmNow_ = t.bpm;
    applyCommands(t.beat);
    if (t.startedThisBlock || (t.playing && !launched_)) {
        eng_->reset();
        eng_->launch(t.beat);
        launched_ = true;
        nextMove_ = -1.0;
        for (int i = 0; i < eng_->nEvents; ++i) events_.push(eng_->events[i]);
        eng_->nEvents = 0;
    }
    if (t.stoppedThisBlock) {
        launched_ = false;
        for (int i = 0; i < nOffs_; ++i)
            ctx_->midiOut(lab::MidiMsg{uint8_t(0x80 | (offs_[i].status & 15)), offs_[i].note, 0});
        nOffs_ = 0;
    }
    wasPlaying_ = t.playing;
    if (t.playing) {
        const double mb = eng_->moveBeats();
        const double end = t.beat + t.beatsPerSample * double(lab::kBlock);
        if (nextMove_ < t.beat - 1e-9 || nextMove_ > t.beat + mb + 1e-9)
            nextMove_ = ceilD(t.beat / mb - 1e-9) * mb;
        int guard = 0;
        while (nextMove_ < end && guard++ < 16) {
            eng_->tick(nextMove_);
            schedule(t, nextMove_, sample_);
            nextMove_ += eng_->moveBeats();
        }
        beatNow_ = t.beat;
    }
    // modulation sources
    {
        const Ball& b = eng_->balls[0];
        const int st = b.active ? b.state : 0;
        mod_[0] = float(st) / 16.0f;
        mod_[1] = eng_->entropy;
        mod_[2] = st >= 1 ? (eng_->heat[st - 1] > 3.0f ? 1.0f : eng_->heat[st - 1] / 3.0f) : 0.0f;
        mod_[3] = eng_->tilted() ? 1.0f : 0.0f;
        mod_[4] = float(eng_->ballsActive()) / float(kBalls);
        mod_[5] = float(pat_->ev.page) / float(kPages - 1);
        for (int i = 0; i < audio_.nFired; ++i)
            if (audio_.fired[i].kind == Hit::kNote) mod_[6] = float(audio_.fired[i].vel) / 127.0f;
    }
    audio_.stemsOn = ctx_->shell && ctx_->shell->recording();
    audio_.render(sample_, t.bpm, l, r);
    for (int i = 0; i < audio_.nFired; ++i) midiOut(audio_.fired[i]);
    runOffs();
    for (int s = 0; s < 4; ++s)
        if (audio_.stemUsed[s]) ctx_->stem(s, audio_.stemL[s], audio_.stemR[s]);
    sample_ += lab::kBlock;
}

void Tilt::onTransport(bool playing) {
    if (!playing) banner_ = 0;
}

// ---- main loop ------------------------------------------------------------------------------------
void Tilt::onBanner(uint8_t ev) {
    switch (ev) {
        case kEvTilt:
        case kEvSave:
        case kEvMultiball:
        case kEvCombo1:
        case kEvCombo2:
        case kEvCombo3:
        case kEvCombo4:
        case kEvScene:
        case kEvHalt:
        case kEvDrain:
        case kEvCapture:
        case kEvLaunch:
            banner_ = ev;
            bannerAt_ = now_;
            break;
        default: break;
    }
}

void Tilt::update(uint32_t ms) {
    now_ = ms;
    uint8_t ev;
    while (events_.pop(ev)) onBanner(ev);
    for (int i = 0; i < kCh; ++i) {
        const uint32_t h = audio_.chHits[i];
        if (h != lastHits_[i]) {
            lastHits_[i] = h;
            chFlash_[i] = 255;
        } else if (chFlash_[i]) {
            chFlash_[i] = uint8_t(chFlash_[i] > 24 ? chFlash_[i] - 24 : 0);
        }
    }
    bool any = false;
    for (int i = 0; i < kCh; ++i) any |= sampleDirty_[i];
    if (any) loadSamplesFor();
    // the tempo is part of the pattern
    pat_->bpmX10 = uint16_t(ctx_->bpm() * 10.0f + 0.5f);
}

void Tilt::auditionCh(int ch, int note, int vel) { post(Cmd::kAudition, ch, note, vel); }

void Tilt::onMidi(const lab::MidiMsg& m) {
    const uint8_t t = m.type();
    if (t != 0x90 || m.d2 == 0) return;
    for (int i = 0; i < kFlips; ++i)
        if (pat_->flips[i].note == m.d1) {
            post(Cmd::kFlipper, i, m.d2);
            return;
        }
    const int base = pat_->table.stepNoteBase;
    if (setStepNotes_ && m.d1 >= base && m.d1 < base + kSteps) {
        post(Cmd::kFlip, m.d1 - base + 1, m.d2);
        return;
    }
    auditionCh(chSel_, m.d1, m.d2);
}

int Tilt::settings(lab::SettingRow* rows, int cap) {
    static const char* const kOff[] = {"Off", "On"};
    int n = 0;
    auto add = [&](const char* label, int* v, const char* const* names) {
        if (n >= cap) return;
        rows[n].label = label;
        rows[n].value = v;
        rows[n].min = 0;
        rows[n].max = 1;
        rows[n].names = names;
        ++n;
    };
    add("TILT: load last session", &setAutoload_, kOff);
    add("TILT: MIDI notes out", &setMidiOut_, kOff);
    add("TILT: MIDI step notes flip", &setStepNotes_, kOff);
    add("TILT: ball trails", &setTrail_, kOff);
    return n;
}

// ---- bench -----------------------------------------------------------------------------------------
const char* Tilt::benchName(int i) const {
    static const char* const k[] = {"groove", "busy8", "multiball", "allfx", "p.sample", "p.kick", "p.snare",
                                    "p.hat", "p.acid", "p.fm", "p.fxonly", "p.engine"};
    return k[clampi(i, 0, 11)];
}

void Tilt::benchSetup(int i) {
#ifdef TILT_PROFILE
    if (i >= 4) {   // profiling: 8 channels of one engine, or the FX alone, or the engine alone
        factoryPattern(0, *pat_);
        Pattern& p = *pat_;
        p.table.rate = 1;
        for (int s = 0; s < kSteps; ++s) {
            p.steps[s].trig = uint8_t(i == 11 ? 0 : 1 + s % 8);
            p.steps[s].len = 16;
            p.steps[s].bumper = 0;
        }
        for (int c = 0; c < kCh; ++c) {
            if (i < 4 + kEngines) engineDefaults(p.ch[c], i - 4);
            p.ch[c].choke = 0;
            p.ch[c].sample[0] = 0;
            p.ch[c].sendA = p.ch[c].sendB = uint8_t(i == 10 ? 127 : 0);
            p.ch[c].p[2] = 127;
        }
        if (i == 10) {
            for (int s = 0; s < kSteps; ++s) p.steps[s].trig = s == 0 ? 1 : 0;
        }
        eng_->reset();
        launched_ = false;
        nextMove_ = -1.0;
        return;
    }
#endif
    factoryPattern(i == 0 ? 0 : 4, *pat_);
    Pattern& p = *pat_;
    if (i >= 1) {
        // every channel sounding: long decays, all engines, a fast walk over all lanes
        p.table.rate = 1;   // 1/32
        static const uint8_t t[16] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};
        for (int s = 0; s < kSteps; ++s) {
            p.steps[s].trig = t[s];
            p.steps[s].trigP = 100;
            p.steps[s].len = 16;
            p.steps[s].bumper = uint8_t(i >= 2 ? 3 : 1);
            p.steps[s].degree = int8_t(s % 7);
        }
        p.ch[0].p[2] = 127;
        p.ch[1].p[2] = 127;
        p.ch[2].p[1] = 127;
        p.ch[3].p[1] = 127;
        p.ch[4].p[0] = 4;   // BELL: long
        p.ch[2].choke = p.ch[3].choke = 0;
        p.ch[7].p[2] = 0;   // the whole break
        p.ch[7].p[1] = 1;
        p.ch[6].p[3] = 127;
        p.ch[5].p[2] = 127;
        p.mel.on = 1;
        p.mel.octUp = 30;
    }
    if (i >= 2) {
        p.table.mbMax = 4;
        p.table.mbLife = 64;
        p.combos[0].len = 0;
    }
    if (i >= 3) {
        p.table.rate = 0;   // 1/64
        for (auto& c : p.ch) {
            c.sendA = 127;
            c.sendB = 127;
        }
        p.fx.drive = 60;
        p.fx.dlyFb = 90;
        p.ev.mutate = 80;
        p.ev.mutSpread = 80;
        p.ev.fatigue = 50;
        p.ev.reinforce = 30;
        p.ev.metaOn = 1;
        p.ev.metaBars = 1;
        p.ev.noBounce = 1;
        p.ev.maxRepeat = 2;
        p.table.gravity = 40;
    }
    eng_->reset();
    launched_ = false;
    nextMove_ = -1.0;
    if (i >= 2) {
        eng_->launch(0.0);
        launched_ = true;
        for (int k = 0; k < 3; ++k) eng_->addBall();
    }
}

}  // namespace tilt

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<tilt::Tilt>(); }
}  // namespace lab
