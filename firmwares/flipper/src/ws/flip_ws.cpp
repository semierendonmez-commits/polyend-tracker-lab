// FLIPPER -- workstation core: init, the audio block (steps, notes, MIDI,
// stems), commands, files, capture, settings, LEDs, bench scenarios.
#include "ws/flip_ws.h"

#include <cmath>
#include <cstdio>

#include "formats/wav.h"

namespace flip {

using lab::kBlock;

namespace {
const char* const kOffOn[] = {"Off", "On"};
const char* const kModeNames[] = {"Hybrid", "Markov only"};
const char* const kQuantNames[] = {"Next bar", "Every 2 bars", "Every 4 bars"};
const char* const kLedNames[] = {"Low", "Medium", "High"};
const char* const kTableNumNames[] = {"1", "2", "3", "4", "5", "6", "7", "8"};

}  // namespace

// ---- init -------------------------------------------------------------------------------
bool Flipper::init(lab::Context& c) {
    ctx_ = &c;
    theme_.bg = lab::Rgb(0x07030F);
    theme_.panel = lab::Rgb(0x1A0B2E);
    theme_.fg = lab::Rgb(0xF4EEFF);
    theme_.dim = lab::Rgb(0x8A78A8);
    theme_.accent = lab::Rgb(0x00F0FF);
    theme_.accent2 = lab::Rgb(0xFF2E97);
    theme_.warn = lab::Rgb(0xFF3B30);
    theme_.ok = lab::Rgb(0x7CFF4F);
    m_ = c.arena->make<Machine>();
    spare_ = c.arena->make<Machine>();
    clipTable_ = c.arena->make<Table>();
    run_ = c.arena->make<ClipStep>(kRunCap);
    if (!m_ || !spare_ || !clipTable_ || !run_) return false;
    if (!audio_.init(*c.arena)) return false;
    factoryMachine(*m_);
    if (c.fs) {
        char p[96];
        std::snprintf(p, sizeof p, "%s/Samples", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/Tables", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/state.bin", c.home);
        const ot::IStorage::Handle h = c.fs->open(p, ot::IStorage::kRead);
        if (h >= 0) {
            uint8_t s = 0;
            if (c.fs->read(h, &s, 1) == 1 && s < kSlots) slot_ = activeSlot_ = s;
            c.fs->close(h);
        }
        rescanSlots();
        if (slotUsed_[slot_]) {
            factoryMachine(*spare_);
            if (loadMachine(*c.fs, [&] { slotPath(slot_, p, sizeof p); return p; }(), *spare_)) {
                Machine* t = m_;
                m_ = spare_;
                spare_ = t;
            }
        }
        scanWavs();
    }
    audio_.setMachine(m_);
    game_.init(m_, 0xF1199E5u ^ c.millis());
    game_.attachRun(run_, kRunCap);
    game_.setMode(m_->g.mode);
    // match the sample choices to the scanned files
    for (int i = 0; i < kInsts; ++i) {
        sampleSel_[i] = 0;
        for (int w = 0; w < wavCount_; ++w)
            if (m_->sample[i][0] && std::strcmp(m_->sample[i], wavs_[w]) == 0) sampleSel_[i] = w + 1;
    }
    needSamples_ = true;
    clipB_ = int(m_->clipLen);
    return true;
}

void Flipper::slotPath(int slot, char* buf, uint32_t cap) const {
    std::snprintf(buf, cap, "%s/MACHINE%d.FLP", ctx_->home, slot + 1);
}

void Flipper::rescanSlots() {
    for (int i = 0; i < kSlots; ++i) {
        char p[96];
        slotPath(i, p, sizeof p);
        slotUsed_[i] = ctx_->fs && ctx_->fs->exists(p);
    }
}

void Flipper::scanWavs() {
    wavCount_ = 0;
    if (!ctx_->fs) return;
    char dir[96];
    std::snprintf(dir, sizeof dir, "%s/Samples", ctx_->home);
    struct Ctx {
        Flipper* f;
        const char* dir;
    } cx{this, dir};
    ctx_->fs->listDir(
        dir,
        [](const ot::IStorage::DirEntry& e, void* p) {
            Ctx* c = static_cast<Ctx*>(p);
            if (e.isDir || !lab::endsWithCi(e.name, ".wav")) return true;
            if (c->f->wavCount_ >= kMaxWavs) return false;
            char path[kPathLen];
            const int n = std::snprintf(path, sizeof path, "%s/%s", c->dir, e.name);
            if (n <= 0 || n >= int(sizeof path)) return true;
            lab::copyStr(c->f->wavs_[c->f->wavCount_++], kPathLen, path);
            return true;
        },
        &cx);
    // sort by name (small insertion sort)
    for (int i = 1; i < wavCount_; ++i)
        for (int j = i; j > 0 && std::strcmp(wavs_[j - 1], wavs_[j]) > 0; --j) {
            char t[kPathLen];
            std::memcpy(t, wavs_[j], kPathLen);
            std::memcpy(wavs_[j], wavs_[j - 1], kPathLen);
            std::memcpy(wavs_[j - 1], t, kPathLen);
        }
}

void Flipper::loadSample(int i) {
    SampleSlot& s = audio_.samples[i];
    s.frames = 0;
    if (!ctx_->fs || !m_->sample[i][0]) return;
    const ot::IStorage::Handle h = ctx_->fs->open(m_->sample[i], ot::IStorage::kRead);
    if (h < 0) return;
    ot::WavInfo info;
    ot::StorageReader rd(*ctx_->fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    ctx_->fs->close(h);
    if (!ok) return;
    ot::WavImportOptions opt;
    const uint32_t src = ot::wavSourceFrames(info);
    const uint32_t out = ot::wavResampledFrames(src, info.sampleRate);
    if (out > kSampleMaxFrames - 4 && out) opt.endMarker = uint16_t(uint64_t(65535) * (kSampleMaxFrames - 4) / out);
    ot::WavImportResult res;
    if (ot::wavImportFile(*ctx_->fs, m_->sample[i], opt, s.data, kSampleMaxFrames, res) == ot::FmtStatus::Ok)
        s.frames = res.frames;
}

void Flipper::loadSamples(bool all) {
    for (int i = 0; i < kInsts; ++i)
        if (all || sampleDirty_[i]) {
            sampleDirty_[i] = false;
            loadSample(i);
        }
}

bool Flipper::saveSlot(int slot) {
    if (!ctx_->fs) return false;
    char p[96];
    slotPath(slot, p, sizeof p);
    const bool ok = saveMachine(*ctx_->fs, p, *m_);
    if (ok) activeSlot_ = slot;
    std::snprintf(p, sizeof p, "%s/state.bin", ctx_->home);
    const ot::IStorage::Handle h = ctx_->fs->open(p, ot::IStorage::kWrite);
    if (h >= 0) {
        const uint8_t s = uint8_t(slot);
        ctx_->fs->write(h, &s, 1);
        ctx_->fs->close(h);
    }
    rescanSlots();
    return ok;
}

bool Flipper::loadSlot(int slot) {
    if (!ctx_->fs || swapPending_) return false;
    char p[96];
    slotPath(slot, p, sizeof p);
    factoryMachine(*spare_);
    if (!loadMachine(*ctx_->fs, p, *spare_)) return false;
    activeSlot_ = slot;
    swapPending_ = true;
    post(Cmd::kSwap);
    return true;
}

void Flipper::scanTableFiles() {
    tabFileCount_ = 0;
    if (!ctx_->fs) return;
    char dir[96];
    std::snprintf(dir, sizeof dir, "%s/Tables", ctx_->home);
    struct Ctx {
        Flipper* f;
        const char* dir;
    } cx{this, dir};
    ctx_->fs->listDir(
        dir,
        [](const ot::IStorage::DirEntry& e, void* p) {
            Ctx* c = static_cast<Ctx*>(p);
            if (e.isDir || !lab::endsWithCi(e.name, ".fpt")) return true;
            if (c->f->tabFileCount_ >= 16) return false;
            const int n = std::snprintf(c->f->tabFiles_[c->f->tabFileCount_], kPathLen, "%s/%s", c->dir, e.name);
            if (n > 0 && n < kPathLen) ++c->f->tabFileCount_;
            return true;
        },
        &cx);
    if (tabFile_ >= tabFileCount_) tabFile_ = 0;
}

// A single table file: the same chunk format as a machine, with one table chunk.
bool Flipper::saveTableFile(int t) {
    if (!ctx_->fs) return false;
    Machine& tmp = *spare_;
    if (swapPending_) return false;
    factoryMachine(tmp);
    tmp.tab[0] = m_->tab[t];
    for (int i = 0; i < kInsts; ++i) tmp.inst[i] = m_->inst[i];
    char name[kNameLen];
    lab::copyStr(name, sizeof name, m_->tab[t].name);
    for (char& ch : name)
        if (ch == ' ' || ch == '/' || ch == '.') ch = '_';
    char p[96];
    std::snprintf(p, sizeof p, "%s/Tables/%s.FPT", ctx_->home, name[0] ? name : "TABLE");
    const bool ok = saveMachine(*ctx_->fs, p, tmp);
    scanTableFiles();
    return ok;
}

bool Flipper::loadTableFile(int t, const char* path) {
    if (!ctx_->fs || swapPending_) return false;
    Machine& tmp = *spare_;
    factoryMachine(tmp);
    if (!loadMachine(*ctx_->fs, path, tmp)) return false;
    m_->tab[t] = tmp.tab[0];
    post(Cmd::kRebuild);
    return true;
}

bool Flipper::capture(int what) {
    const uint32_t count = game_.runCount();
    if (game_.replaying() || count == 0) return false;
    uint32_t n;
    if (what == 0) n = count - game_.runGameStart();
    else {
        static const uint32_t kBars[kCaptureCount] = {0, 4, 8, 16, 32, 64};
        n = kBars[what < kCaptureCount ? what : 1] * uint32_t(game_.stepsPerBar());
    }
    if (n > count) n = count;
    if (n > kRunCap) n = kRunCap;
    if (n > uint32_t(kClipMax)) n = kClipMax;
    if (n == 0) return false;
    const uint32_t start = count - n;
    m_->clipLen = 0;
    for (uint32_t i = 0; i < n; ++i) m_->clip[i] = run_[(start + i) % kRunCap];
    m_->clipLen = n;
    clipA_ = 0;
    clipB_ = int(n);
    return true;
}

void Flipper::save() {
    if (ctx_->fs && !swapPending_ && activeSlot_ >= 0) saveSlot(activeSlot_);
}

void Flipper::post(Cmd::Type t, int a, int b, int c) {
    Cmd cmd;
    cmd.type = t;
    cmd.a = int16_t(a);
    cmd.b = int16_t(b);
    cmd.c = int16_t(c);
    if (!cmds_.push(cmd) && t == Cmd::kSwap) swapPending_ = false;
}

// ---- audio side -------------------------------------------------------------------------
void Flipper::applyCommands() {
    Cmd c;
    while (cmds_.pop(c)) {
        switch (c.type) {
            case Cmd::kNewGame: game_.newGame(c.a); break;
            case Cmd::kGoto: game_.gotoTable(c.a, false); break;
            case Cmd::kGotoNow: game_.gotoTable(c.a, true); break;
            case Cmd::kReplay:
                game_.setClipLoop(uint32_t(c.b), uint32_t(c.c));
                game_.setReplay(c.a != 0);
                break;
            case Cmd::kSwap: {
                Machine* t = m_;
                m_ = spare_;
                spare_ = t;
                game_.setMachine(m_);
                audio_.setMachine(m_);
                audio_.panic();
                game_.setReplay(false);
                game_.newGame(m_->g.startTable);
                if (!wasPlaying_) game_.stopGame();
                swapPending_ = false;
                break;
            }
            case Cmd::kRebuild: game_.rebuild(); break;
            case Cmd::kPanic:
                audio_.panic();
                runOffs();
                break;
            case Cmd::kTilt: game_.tilt(); break;
            case Cmd::kDrop: game_.dropBall(c.a); break;
            case Cmd::kStop: game_.stopGame(); break;
            case Cmd::kAudition: {
                NoteOut n;
                n.inst = uint8_t(c.a);
                n.note = uint8_t(c.b);
                n.vel = uint8_t(c.c);
                audio_.trigger(c.a, c.b, c.c, 0);
                midiNoteOut(n, 0);
                break;
            }
            case Cmd::kFlip: game_.flip(c.a, c.b != 0, uint8_t(c.c)); break;
            case Cmd::kLaunch: game_.launch(); break;
            case Cmd::kMagnet: game_.setMagnet(c.a); break;
            case Cmd::kPerf:
                if (c.a >= 0 && c.a < kFxCount) perfAmt_[c.a] = float(c.b) * 0.01f;
                break;
            default: break;
        }
    }
}

void Flipper::runOffs() {
    for (int i = 0; i < offCount_; ++i) ctx_->midiNote(false, offs_[i].note, 0, offs_[i].ch);
    offCount_ = 0;
}

void Flipper::midiNoteOut(const NoteOut& n, int offset) {
    if (!m_->g.midiNotes || n.ghost) return;
    const Instrument& in = m_->inst[n.inst];
    if (in.midi == 0) return;
    const uint8_t base = ctx_->midiChannel();
    const uint8_t ch = in.midi == 1 ? 0 : uint8_t((int(in.midi) - 2 - int(base)) & 15);
    for (int i = 0; i < offCount_; ++i)
        if (offs_[i].note == n.note && offs_[i].ch == ch) {   // retrigger: close the old one first
            ctx_->midiNote(false, n.note, 0, ch);
            offs_[i] = offs_[--offCount_];
            break;
        }
    ctx_->midiNote(true, n.note, n.vel, ch);
    if (offCount_ < kOffCap) {
        Off& o = offs_[offCount_++];
        o.note = n.note;
        o.ch = ch;
        o.samples = int32_t(bps_ * 0.9f * 60.0f / (ctx_->bpm() > 20.0f ? ctx_->bpm() : 120.0f) * 44100.0f) + offset;
    } else {
        ctx_->midiNote(false, n.note, 0, ch);
    }
}

void Flipper::emit(int offset) {
    for (int i = 0; i < game_.noteCount; ++i) {
        const NoteOut& n = game_.notes[i];
        audio_.trigger(n.inst, n.note, n.ghost ? n.vel : n.vel, offset);
        midiNoteOut(n, offset);
    }
    for (int i = 0; i < game_.fxCount; ++i) audio_.fx(game_.fx[i], offset);
    for (int i = 0; i < game_.bannerCount; ++i) bannersOut_.push(game_.banners[i]);
    game_.clearOut();
}

void Flipper::render(const lab::Transport& t, const lab::AudioIn&, float* l, float* r) {
    applyCommands();
    for (int f = 0; f < kFxCount; ++f) audio_.perf(f, perfAmt_[f]);
    if (game_.mode() != m_->g.mode) game_.setMode(m_->g.mode);
    const float bpm = t.bpm > 20.0f ? t.bpm : 120.0f;
    const Table& tb = game_.table();
    bps_ = beatsPerStep(tb.stepRate);
    const float stepSec = bps_ * 60.0f / bpm;
    audio_.setTempo(bpm, stepSec * 44100.0f);
    game_.setStepSeconds(stepSec);
    if (t.startedThisBlock) {
        gridBeat_ = 0.0;
        parity_ = 0;
    }
    if (t.stoppedThisBlock) runOffs();
    wasPlaying_ = t.playing;
    if (t.playing && t.beatsPerSample > 0.0) {
        const double blockBeats = t.beatsPerSample * double(kBlock);
        // fell far behind (tempo jump) or the transport position jumped back: resync
        if (gridBeat_ < t.beat - 4.0 || gridBeat_ > t.beat + 4.0) gridBeat_ = t.beat;
        for (int guard = 0; guard < 8; ++guard) {
            const float swing = float(game_.table().swing) * 0.01f;
            const double at = gridBeat_ + ((parity_ & 1) ? double(swing * 0.5f * bps_) : 0.0);
            if (at >= t.beat + blockBeats) break;
            int off = at <= t.beat ? 0 : int((at - t.beat) / t.beatsPerSample);
            if (off >= int(kBlock)) off = int(kBlock) - 1;
            game_.clearOut();
            game_.step();
            audioSteps_ = audioSteps_ + 1;
            emit(off);
            gridBeat_ += double(bps_);
            bps_ = beatsPerStep(game_.table().stepRate);
            ++parity_;
            if (lastTable_ != game_.tableIndex()) {
                lastTable_ = game_.tableIndex();
                parity_ = 0;
            }
        }
        game_.clearOut();
        game_.tick(float(kBlock) / 44100.0f);
        emit(0);
        const double prev = gridBeat_ - double(bps_);
        float ph = float((t.beat + blockBeats - prev) / double(bps_));
        stepPhase_ = ph < 0.0f ? 0.0f : (ph > 1.0f ? 1.0f : ph);
    }
    // pending MIDI note-offs
    for (int i = 0; i < offCount_;) {
        offs_[i].samples -= int32_t(kBlock);
        if (offs_[i].samples <= 0) {
            ctx_->midiNote(false, offs_[i].note, 0, offs_[i].ch);
            offs_[i] = offs_[--offCount_];
        } else {
            ++i;
        }
    }
    if (cpuGuard_ > 0.0f) {   // bench stress: refill free voices (two new notes per block at most)
        static const uint8_t kStress[6] = {7, 5, 4, 7, 6, 5};
        for (int k = 0; k < 2 && audio_.voicesOn() < kVoices; ++k) {
            stressN_ = (stressN_ + 1) % 6;
            audio_.trigger(kStress[stressN_], 48 + int(stressN_) * 5, 120, k * 40);
        }
        // the busiest step: 4 balls + 3 rails start notes in one block (every 16 blocks)
        if ((++stressBlk_ & 15) == 0)
            for (int k = 0; k < 7; ++k) audio_.trigger(k, 40 + k * 4, 127, k * 8);
    }
    audio_.render(l, r);
    for (int s = 0; s < lab::Context::kStems; ++s) ctx_->stem(s, audio_.stemL(s), audio_.stemR(s));
}

void Flipper::onTransport(bool playing) {
    if (playing && !game_.running() && !game_.replaying()) post(Cmd::kNewGame, m_->g.startTable);
}

// ---- main loop ------------------------------------------------------------------------------
void Flipper::update(uint32_t ms) {
    now_ = ms;
    uint8_t b;
    while (bannersOut_.pop(b)) {
        if (b == kBnNone) continue;
        // keep a big banner (multiball, jackpot, tilt, table) over a small one for a moment
        const bool big = b == kBnMultiball || b == kBnJackpot || b == kBnTilt || b == kBnTable || b == kBnGameOver;
        const bool curBig = banner_ == kBnMultiball || banner_ == kBnJackpot || banner_ == kBnTilt ||
                            banner_ == kBnTable || banner_ == kBnGameOver;
        if (big || !curBig || ms - bannerAt_ > 900) {
            banner_ = b;
            bannerAt_ = ms;
        }
    }
    if (swapPending_) return;
    if (needSamples_) {
        needSamples_ = false;
        loadSamples(true);
    } else if (ms - sampleTouchMs_ > 400) {   // wait until the encoder stops on a file
        for (int i = 0; i < kInsts; ++i)
            if (sampleDirty_[i]) {
                loadSamples(false);
                break;
            }
    }
    if (ms - lastTrailMs_ >= 28) {
        lastTrailMs_ = ms;
        trailHead_ = (trailHead_ + 1) % kTrail;
        for (int i = 0; i < kMaxBalls; ++i) {
            const Ball& bl = game_.balls[i];
            float x = bl.x, y = bl.y;
            if (!bl.phys) {
                x = cellX(bl.cell);
                y = cellY(bl.cell);
            }
            trailX_[i][trailHead_] = bl.active ? x : -1000.0f;
            trailY_[i][trailHead_] = y;
        }
    }
}

void Flipper::audition(int inst, int note, int vel) { post(Cmd::kAudition, inst, note, vel); }

void Flipper::onMidi(const lab::MidiMsg& m) {
    if (!m_->g.midiIn) return;
    const uint8_t t = m.type();
    if (t != 0x90 && t != 0x80) return;
    const int pad = int(m.d1) - 36;
    if (pad < 0 || pad >= lab::kPads) return;
    lab::Event e;
    e.type = (t == 0x90 && m.d2) ? lab::Event::kPadDown : lab::Event::kPadUp;
    e.id = uint8_t(pad);
    e.velocity = m.d2 ? m.d2 : 100;
    e.ms = now_;
    onEvent(e);
}

// ---- settings ------------------------------------------------------------------------------
int Flipper::settings(lab::SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int32_t* v, int mn, int mx, const char* const* names, const char* unit) {
        if (n >= cap) return;
        lab::SettingRow& r = rows[n++];
        r.label = label;
        r.value = reinterpret_cast<int*>(v);
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.unit = unit;
    };
    static_assert(sizeof(int32_t) == sizeof(int), "int rows");
    Global& g = m_->g;
    add("Ball motion", &g.mode, 0, 1, kModeNames, "");
    add("Auto-player", &g.autoOn, 0, 1, kOffOn, "");
    add("Auto-player takes over after", &g.autoAfter, 1, 10, nullptr, " s");
    add("Auto-player skill", &g.autoSkill, 0, 100, nullptr, " %");
    add("Hybrid: grid follows the ball", &g.follow, 0, 100, nullptr, " %");
    add("Hybrid: grid ball plays", &g.ghost, 0, 1, kOffOn, "");
    add("Table change", &g.quant, 0, 2, kQuantNames, "");
    add("Bumper notes to MIDI out", &g.midiNotes, 0, 1, kOffOn, "");
    add("MIDI in notes play pads", &g.midiIn, 0, 1, kOffOn, "");
    add("Ball trails", &g.trails, 0, 1, kOffOn, "");
    add("Start table", &g.startTable, 0, kTables - 1, kTableNumNames, "");
    add("Pad brightness", &g.ledLevel, 0, 2, kLedNames, "");
    return n;
}

// ---- LEDs --------------------------------------------------------------------------------------
lab::Rgb Flipper::instColor(int inst) const {
    static const uint32_t k[8] = {0xFF2E2E, 0xFF8A00, 0xFFE600, 0x2EFF6A, 0x00E5FF, 0x3D6BFF, 0xB84DFF, 0xFF3DB8};
    if (inst < 0 || inst >= kInsts) return lab::Rgb(0x404040);
    return lab::Rgb(k[m_->inst[inst].color & 7]);
}

lab::Rgb Flipper::tableColor(int t) const {
    static const uint32_t k[6] = {0x00F0FF, 0xFF2E97, 0xFFD400, 0x7CFF4F, 0xB84DFF, 0xFF7A1A};
    return lab::Rgb(k[m_->tab[t & 7].color % 6]);
}

void Flipper::leds(lab::Rgb* pads) {
    const float lvl = m_->g.ledLevel == 0 ? 0.35f : (m_->g.ledLevel == 1 ? 0.65f : 1.0f);
    const Table& t = game_.table();
    const uint32_t step = game_.steps();
    auto put = [&](int i, lab::Rgb c) { pads[i] = lab::scale(c, lvl); };
    if (page_ == kPgSounds) {
        for (int i = 0; i < lab::kPads; ++i) {
            if (i < kInsts) put(i, i == sndInst_ ? lab::Rgb(0xFFFFFF) : lab::scale(instColor(i), 0.5f));
            else if (i < 12) put(i, lab::Rgb());
            else {
                const int n = i - 12;
                const bool c = (n % 12) == 0;
                put(i, padDown_[i] ? lab::Rgb(0xFFFFFF) : lab::scale(instColor(sndInst_), c ? 0.6f : 0.18f));
            }
        }
        return;
    }
    if (page_ == kPgTables) {
        for (int i = 0; i < lab::kPads; ++i) {
            const int col = lab::padCol(i), row = lab::padRow(i);
            lab::Rgb c;
            if (col < kTables && row == 0) {
                const bool on = m_->tab[col].enabled;
                c = col == game_.tableIndex() ? lab::Rgb(0xFFFFFF)
                                               : lab::scale(tableColor(col), col == tabSel_ ? 1.0f : (on ? 0.3f : 0.05f));
            } else if (col < kTables && row == 1) {
                c = lab::scale(tableColor(col), float(m_->tab[tabSel_].graph[col]) / 9.0f);
            }
            put(i, c);
        }
        return;
    }
    for (int i = 0; i < kCells; ++i) {
        const Cell& c = t.cells[i];
        lab::Rgb col;
        switch (c.type) {
            case kBumper: col = lab::scale(instColor(c.inst), 0.35f); break;
            case kRamp: col = lab::Rgb(0x10304A); break;
            case kHole: col = lab::Rgb(0x300040); break;
            case kTarget: col = game_.lit(i) ? lab::Rgb(0xFFD400) : lab::Rgb(0x3A2A00); break;
            case kFlipper: col = game_.flipperUp(c.dir ? 1 : 0) ? lab::Rgb(0xFFFFFF) : lab::Rgb(0x6A1040); break;
            case kWall: col = lab::Rgb(0x0A0A14); break;
            default: col = lab::Rgb(0x000000); break;
        }
        const uint32_t since = step - game_.hitStep(i);
        if (game_.hitStep(i) && since < 3 && c.type != kWall) col = lab::mix(lab::Rgb(0xFFFFFF), col, float(since) / 3.0f);
        if (page_ == kPgEdit && i == editCell_) col = (now_ / 250) & 1 ? lab::Rgb(0x00F0FF) : lab::mix(col, lab::Rgb(0x00F0FF), 0.4f);
        if (game_.magnet() == i) col = lab::Rgb(0x00F0FF);
        put(i, col);
    }
    for (int b = 0; b < kMaxBalls; ++b) {
        const Ball& bl = game_.balls[b];
        if (!bl.active) continue;
        put(bl.cell, b == 0 ? lab::Rgb(0xFFFFFF) : lab::Rgb(0xFFF0A0));
    }
    if (game_.tiltLocked())
        for (int i = 0; i < lab::kPads; ++i) pads[i] = lab::mix(pads[i], lab::Rgb(0xFF0000), 0.6f);
}

// ---- bench ------------------------------------------------------------------------------------
const char* Flipper::benchName(int i) const {
    static const char* const k[5] = {"markov", "hybrid", "multiball", "allvoices", "markovmulti"};
    return k[i < 0 || i > 4 ? 0 : i];
}

void Flipper::benchSetup(int i) {
    audio_.panic();
    gridBeat_ = 0.0;
    parity_ = 0;
    cpuGuard_ = 0.0f;
    for (auto& p : perfAmt_) p = 0.0f;
    m_->g.autoOn = 1;
    m_->g.mode = (i == 0 || i == 4) ? kMarkov : kHybrid;
    game_.setMode(m_->g.mode);
    game_.newGame(i == 3 ? 3 : 1);
    game_.launch();
    if (i == 2 || i == 3 || i == 4) {
        static const int kDrop[3] = {cellAt(2, 0), cellAt(5, 1), cellAt(8, 0)};
        for (int c : kDrop) game_.dropBall(c);
    }
    if (i == 2 || i == 3) {   // every effect on every group (stutter only in the all-voices case)
        for (int f = 0; f < kFxCount; ++f) perfAmt_[f] = (f == kFxStutter && i == 2) ? 0.0f : 0.8f;
    }
    if (i == 3) {
        // every voice sounding with long tails: 12 voices of the costliest engines + the game on top
        cpuGuard_ = 1.0f;
        for (int k = 4; k < kInsts; ++k) m_->inst[k].decay = 100;
        for (int k = 0; k < kVoices; ++k) audio_.trigger(k % 2 ? 7 : 5, 48 + k * 2, 120, 0);
    }
}

}  // namespace flip

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<flip::Flipper>(); }
}  // namespace lab
