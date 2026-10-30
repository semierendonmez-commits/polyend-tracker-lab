// lab::Shell -- intro, settings, transport, audio I/O, recorder, MIDI (lab.h).
#include <cmath>
#include <cstdio>

#include "formats/wav.h"
#include "lab/lab.h"

#ifndef OT_VERSION
#define OT_VERSION "2.2.1"
#endif
#ifndef LAB_VERSION
#define LAB_VERSION "0.1"
#endif

namespace lab {

// ---- Context --------------------------------------------------------------
uint32_t Context::millis() const { return shell->millis(); }
void Context::midiOut(const MidiMsg& m) { shell->midiOut(m); }
uint8_t Context::midiChannel() const { return shell->midiChannel(); }
void Context::midiNote(bool on, uint8_t note, uint8_t vel, uint8_t chOffset) {
    MidiMsg m;
    m.status = uint8_t((on ? 0x90 : 0x80) | ((shell->midiChannel() + chOffset) & 15));
    m.d1 = note & 127;
    m.d2 = on ? (vel & 127) : 0;
    shell->midiOut(m);
}
bool Context::playing() const { return shell->playing(); }
float Context::bpm() const { return shell->bpm(); }
void Context::setBpm(float b) { shell->setBpm(b); }
void Context::setPlaying(bool on) { shell->setPlaying(on); }
void Context::toast(const char* msg) { shell->toast(msg); }
void Context::requestRedraw() { shell->requestRedraw(); }
void Context::stem(int index, const float* l, const float* r) { shell->stem(index, l, r); }
kit::Registry& Context::params() { return shell->labkit().reg; }
kit::Mod& Context::mod() { return shell->labkit().mod; }
kit::Browser& Context::browser() { return *shell->labkit().browser; }
kit::Keyboard& Context::keyboard() { return *shell->labkit().keyboard; }

namespace {

const char* const kOffOn[] = {"Off", "On"};
const char* const kBright[] = {"Low", "Medium", "High"};
const char* const kClock[] = {"Internal", "Internal + send", "Follow MIDI"};
const char* const kInputs[] = {"Line in", "Radio", "Mic", "Off"};
const char* const kMidiIn[] = {"Omni", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int16_t toS16(float v) {
    const float s = clampf(v, -1.0f, 1.0f) * 32767.0f;
    return int16_t(s < 0.0f ? s - 0.5f : s + 0.5f);
}

}  // namespace

// ---- drawing helpers used by every firmware ---------------------------------
void drawFKeyBar(Canvas& g, const Theme& t, const char* const labels[8], int active) {
    g.color(t.panel);
    g.fill(0, kScreenH - 30, kScreenW, 30);
    for (int i = 0; i < 8; ++i) {
        const char* s = labels ? labels[i] : nullptr;
        const int x = i * 100;
        if (i == active) {
            g.color(t.accent);
            g.fill(x + 2, kScreenH - 28, 96, 26);
        }
        if (s && *s) {
            g.color(i == active ? t.bg : t.fg);
            g.text(x + 50, kScreenH - 15, font::kSmall, s, Canvas::kCenter);
        }
        if (i) {
            g.color(t.dim);
            g.vline(x, kScreenH - 26, 22);
        }
    }
}

void drawHeader(Canvas& g, const Theme& t, Shell& sh, const char* title) {
    g.color(t.panel);
    g.fill(0, 0, kScreenW, 30);
    g.color(t.accent);
    g.text(10, 15, font::kBody, sh.fwName(), Canvas::kCenterY);
    g.color(t.fg);
    if (title) g.text(150, 15, font::kBody, title, Canvas::kCenterY);
    // right: CPU, BPM, play, rec
    g.color(t.dim);
    g.textf(560, 15, font::kSmall, Canvas::kCenterY, "CPU %u%%", unsigned(sh.cpuPermille() / 10));
    g.color(t.fg);
    g.textf(650, 15, font::kBody, Canvas::kCenterY, "%.1f", double(sh.bpm()));
    if (sh.playing()) {
        g.color(t.ok);
        g.beginStrip();   // play triangle as a strip outline
        g.vtx(735, 8);
        g.vtx(749, 15);
        g.vtx(735, 22);
        g.vtx(735, 8);
        g.endBatch();
    } else {
        g.color(t.dim);
        g.fill(736, 9, 12, 12);
    }
    if (sh.recording()) {
        g.color(t.warn);
        g.point(778, 15, 7);
    } else {
        g.color(t.dim);
        g.ring(778, 15, 6, 12);
    }
}

// ---- init ------------------------------------------------------------------
bool Shell::init(const Hal& hal, uint8_t* mem, uint32_t memBytes, const char* fwName) {
    hal_ = hal;
    arena_.attach(mem, memBytes);
    canvas_.attach(hal_.display);
    copyStr(fwName_, sizeof fwName_, fwName);
    std::snprintf(home_, sizeof home_, "Lab/%s", fwName_);

    for (int s = 0; s < kRecStreams; ++s) {
        recRing_[s] = static_cast<int16_t*>(arena_.alloc(kRecRingFrames * 2 * sizeof(int16_t)));
        if (!recRing_[s]) return false;
    }

    if (hal_.fs) {
        hal_.fs->mkdir("Lab");
        hal_.fs->mkdir(home_);
        char p[48];
        std::snprintf(p, sizeof p, "%s/Rec", home_);
        hal_.fs->mkdir(p);
    }
    loadSettings();
    bpmX10_ = set_.bpmX10 < 200 ? 1200 : set_.bpmX10;
    bpmRow_ = bpmX10_ / 10;

    ctx_.shell = this;
    ctx_.arena = &arena_;
    ctx_.fs = hal_.fs;
    ctx_.radio = hal_.radio;
    ctx_.home = home_;
    ctx_.streamReader = hal_.streamReader;
    // LABKIT v2: the registry, mod engine, nav, browser and keyboard exist
    // before the workstation, so init() can register into them.
    kit_ = arena_.make<kit::Kit>();
    if (!kit_ || !kit_->init(*this, ctx_, arena_, hal_.audio != nullptr)) return false;
    ws_ = createWorkstation(arena_);
    if (!ws_) return false;
    ctx_.theme = &ws_->theme();
    if (!ws_->init(ctx_)) return false;
    kit_->afterWorkstationInit(ws_);

    applyInput();
    if (hal_.setBacklight) hal_.setBacklight(IDisplay::brightnessDuty(uint8_t(set_.brightness)));
    if (hal_.leds) {
        hal_.leds->clear();
        hal_.leds->show();
    }
    introOn_ = set_.intro != 0;
    introStart_ = millis();
    lastSaveMs_ = millis();
    if (hal_.audio) {
        hal_.audio->setCallback(&Shell::fillThunk, this);
        hal_.audio->start();
        hal_.audio->setOutputEnabled(true);
    }
    return true;
}

[[noreturn]] void Shell::run() {
    for (;;) loop();
}

// ---- main loop --------------------------------------------------------------
void Shell::loop() {
    if (hal_.service) hal_.service();
    const uint32_t now = millis();
    handleInput();
    pumpMidi();
    // Transport changes requested from the audio side or MIDI are reported here.
    static bool lastPlaying = false;
    if (playing_ != lastPlaying) {
        lastPlaying = playing_;
        if (ws_) ws_->onTransport(lastPlaying);
        dirty_ = true;
    }
    if (ws_ && !introOn_) ws_->update(now);
    if (kit_ && !introOn_) kit_->update(now);
    pumpRecorder();

    if (now - lastLedMs_ >= 33) {
        lastLedMs_ = now;
        updateLeds();
    }
    const bool frameDue = now - lastFrameMs_ >= 25;
    if (frameDue && (!hal_.display || hal_.display->frameReady())) {
        lastFrameMs_ = now;
        drawFrame();
    }
    if (now - lastSaveMs_ > 300000u && !recOn_) {   // every 5 minutes
        lastSaveMs_ = now;
        if (ws_) ws_->save();
        saveSettings();
    }
}

void Shell::powerOff() {
    if (recOn_) stopRecording();
    if (ws_) ws_->save();
    if (kit_) kit_->powerOff();
    saveSettings();
}

// ---- input -------------------------------------------------------------------
void Shell::handleInput() {
    if (!hal_.input) return;
    ot::InputEvent ie;
    while (hal_.input->poll(ie)) {
        Event e;
        e.ms = ie.timeMs;
        e.shift = held_[btn::Shift];
        switch (ie.type) {
            case ot::InputEvent::kButtonDown:
                if (ie.id < btn::kCount) {
                    held_[ie.id] = true;
                    heldSince_[ie.id] = ie.timeMs;
                    holdSent_[ie.id] = false;
                }
                e.type = Event::kPress;
                e.id = ie.id;
                break;
            case ot::InputEvent::kButtonUp:
                if (ie.id < btn::kCount) held_[ie.id] = false;
                e.type = Event::kRelease;
                e.id = ie.id;
                break;
            case ot::InputEvent::kPadDown:
                e.type = Event::kPadDown;
                e.id = ie.id;
                e.velocity = ie.velocity ? ie.velocity : 100;
                break;
            case ot::InputEvent::kPadUp:
                e.type = Event::kPadUp;
                e.id = ie.id;
                break;
            case ot::InputEvent::kEncoder:
                e.type = Event::kEncoder;
                e.delta = ie.delta;
                break;
            default:
                continue;
        }
        dispatch(e);
    }
    const uint32_t now = millis();
    for (uint8_t id = 0; id < btn::kCount; ++id) {
        if (held_[id] && !holdSent_[id] && now - heldSince_[id] >= kHoldMs) {
            holdSent_[id] = true;
            Event e;
            e.type = Event::kHold;
            e.id = id;
            e.ms = now;
            e.shift = held_[btn::Shift];
            dispatch(e);
        }
    }
}

void Shell::dispatch(const Event& e) {
    dirty_ = true;
    if (introOn_) {
        if (e.type == Event::kPress || e.type == Event::kPadDown) introOn_ = false;
        return;
    }
    if (e.type == Event::kPress && e.id == btn::Config && e.shift && !settingsOpen_ && kit_ && kit_->active &&
        kit_->nav.keys().shiftConfigHelp && !kit_->fullScreen()) {
        kit_->nav.toggleHelp();   // LABKIT: Shift + Config = HELP
        return;
    }
    if (e.type == Event::kPress && e.id == btn::Config && kit_ && kit_->active && kit_->nav.overlay() == kit::Nav::kHelp) {
        kit_->nav.close();
        return;
    }
    if (e.type == Event::kPress && e.id == btn::Config) {
        settingsOpen_ = !settingsOpen_;
        if (settingsOpen_) {
            settingsPage_ = 0;
            settingsRow_ = 0;
            settingsTop_ = 0;
            fwConfirm_ = false;
            rowCount_ = buildRows();
        } else {
            saveSettings();
        }
        return;
    }
    if (settingsOpen_) {
        settingsEvent(e);
        return;
    }
    if (e.type == Event::kPress && e.id == btn::Play) {
        togglePlay();
        return;
    }
    if (e.type == Event::kPress && e.id == btn::Rec && e.shift) {
        toggleRecording();
        return;
    }
    if (!ws_) return;
    if (kit_) {
        // A release (or hold) goes where its press went: notes and modifiers the
        // workstation saw always end there, even when a LABKIT view opened since.
        const bool isBtn = e.type == Event::kPress || e.type == Event::kRelease || e.type == Event::kHold;
        const uint64_t bit = 1ull << (e.id & 63);
        if ((e.type == Event::kRelease || e.type == Event::kHold) && (wsButtons_ & bit)) {
            if (e.type == Event::kRelease) wsButtons_ &= ~bit;
            ws_->onEvent(e);
            return;
        }
        if (e.type == Event::kPadUp && (wsPads_ & bit)) {
            wsPads_ &= ~bit;
            ws_->onEvent(e);
            return;
        }
        // Shift always reaches the workstation too (it tracks it itself).
        const bool shiftKey = isBtn && e.id == btn::Shift;
        if (!shiftKey && kit_->event(e)) return;
        if (e.type == Event::kPress) wsButtons_ |= bit;
        if (e.type == Event::kPadDown) wsPads_ |= bit;
    }
    ws_->onEvent(e);
}

// ---- transport -------------------------------------------------------------------
void Shell::setPlaying(bool on) {
    wantPlay_ = on;
    if (!hal_.audio) {   // tests without audio: apply directly
        playing_ = on;
        tr_.playing = on;
    }
}

void Shell::setBpm(float b) {
    if (b < 20.0f) b = 20.0f;
    if (b > 400.0f) b = 400.0f;
    bpmX10_ = int(b * 10.0f + 0.5f);
    set_.bpmX10 = bpmX10_;
    bpmRow_ = bpmX10_ / 10;
}

// ---- audio ------------------------------------------------------------------------
void Shell::fillThunk(int16_t* out, uint32_t frames, void* ctx) { static_cast<Shell*>(ctx)->fill(out, frames); }

void Shell::fill(int16_t* out, uint32_t frames) {
    if (frames != kBlock) {
        std::memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    const uint32_t t0 = hal_.clock ? hal_.clock->micros() : 0;
    for (uint32_t i = 0; i < kBlock; ++i) bufL_[i] = bufR_[i] = 0.0f;
    AudioIn in;
    if (hal_.lineIn && set_.input != 3) in.frames = hal_.lineIn(hal_.lineInCtx);

    // transport
    tr_.startedThisBlock = tr_.stoppedThisBlock = false;
    if (wantPlay_ != playing_) {
        playing_ = wantPlay_;
        if (playing_) {
            tr_.beat = 0.0;
            clockPhase_ = 0.0;
            tr_.startedThisBlock = true;
            if (set_.clockMode == 1) midiOutRing_.push(MidiMsg{0xFA, 0, 0});
        } else {
            tr_.stoppedThisBlock = true;
            if (set_.clockMode == 1) midiOutRing_.push(MidiMsg{0xFC, 0, 0});
        }
    }
    float bpm = float(bpmX10_) * 0.1f;
    if (set_.clockMode == 2 && clockInBpm_ > 20.0f) bpm = clockInBpm_;
    tr_.playing = playing_;
    tr_.bpm = bpm;
    tr_.beatsPerSample = double(bpm) / 60.0 / double(kSampleRate);

    stemWritten_ = 0;
    kitBlock(tr_);
    if (ws_) ws_->render(tr_, in, bufL_, bufR_);
    if (kit_) kit_->mix(bufL_, bufR_);   // the browser's preview voice

    if (set_.monitor && in.frames)
        for (uint32_t i = 0; i < kBlock; ++i) {
            bufL_[i] += in.l(i);
            bufR_[i] += in.r(i);
        }

    // master level + a simple peak limiter
    const float target = float(set_.master) * 0.01f;
    masterGain_ += (target - masterGain_) * 0.05f;
    float pk = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float l = bufL_[i] * masterGain_, r = bufR_[i] * masterGain_;
        if (set_.limiter) {
            const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            const float want = a > 0.95f ? 0.95f / a : 1.0f;
            limGain_ = want < limGain_ ? want : limGain_ + (1.0f - limGain_) * 0.0005f;
            l *= limGain_;
            r *= limGain_;
        }
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (a > pk) pk = a;
        out[2 * i] = toS16(l);
        out[2 * i + 1] = toS16(r);
    }
    peakL_ = pk;

    // recorder (master; stems were written by stem() during render)
    if (recOn_) {
        const uint32_t at = (recHead_ % kRecRingFrames) * 2;
        std::memcpy(recRing_[0] + at, out, kBlock * 2 * sizeof(int16_t));
        if (recStems_)
            for (int s = 0; s < Context::kStems; ++s)
                if (!(stemWritten_ & (1u << s))) std::memset(recRing_[1 + s] + at, 0, kBlock * 2 * sizeof(int16_t));
        recHead_ = recHead_ + kBlock;
    }

    // MIDI clock out, 24 ppqn
    if (playing_) {
        const double beats = tr_.beatsPerSample * double(kBlock);
        if (set_.clockMode == 1) {
            clockPhase_ += beats * 24.0;
            while (clockPhase_ >= 1.0) {
                clockPhase_ -= 1.0;
                midiOutRing_.push(MidiMsg{0xF8, 0, 0});
            }
        }
        tr_.beat += beats;
    }
    blocks_ = blocks_ + 1;
    if (hal_.clock && !hal_.cpuPermille) {
        const uint32_t us = hal_.clock->micros() - t0;
        cpuEst_ = (cpuEst_ * 15u + us * 1000u / 2902u) / 16u;   // 2902 us per block
    }
}

void Shell::kitBlock(const Transport& t) {
    if (kit_ && kit_->active) kit_->audioBlock(t);
}

void Shell::stem(int index, const float* l, const float* r) {
    if (!recOn_ || !recStems_ || index < 0 || index >= Context::kStems) return;
    int16_t* d = recRing_[1 + index] + (recHead_ % kRecRingFrames) * 2;
    for (uint32_t i = 0; i < kBlock; ++i) {
        d[2 * i] = toS16(l[i]);
        d[2 * i + 1] = toS16(r ? r[i] : l[i]);
    }
    stemWritten_ |= uint8_t(1u << index);
}

void Shell::applyInput() {
    if (!hal_.selectInput) return;
    const int src = set_.input;
    if (src == 3) return;
    const uint8_t hw = src == 0 ? 0 : (src == 1 ? 1 : 2);
    hal_.selectInput(hal_.inputCtx, hw, uint8_t(set_.inputGain));
    if (src == 1 && hal_.radio) {
        hal_.radio->setBand(87.5f, 108.0f, 0.1f);
        hal_.radio->tune(float(set_.radioX10) * 0.1f);
    }
}

// ---- recorder -----------------------------------------------------------------------
void Shell::toggleRecording() {
    if (recOn_) stopRecording();
    else startRecording();
}

void Shell::startRecording() {
    if (!hal_.fs) return;
    int n = 1;
    char path[96];
    for (; n < 10000; ++n) {
        std::snprintf(path, sizeof path, "%s/Rec/REC-%04d.wav", home_, n);
        if (!hal_.fs->exists(path)) break;
    }
    std::snprintf(recName_, sizeof recName_, "REC-%04d", n);
    recStems_ = set_.stems != 0;
    uint8_t hdr[ot::wav::kHeaderSize];
    ot::wavMakeHeader(hdr, 2, 0);
    for (int s = 0; s < kRecStreams; ++s) {
        recFile_[s] = -1;
        if (s > 0 && !recStems_) continue;
        if (s == 0) std::snprintf(path, sizeof path, "%s/Rec/%s.wav", home_, recName_);
        else std::snprintf(path, sizeof path, "%s/Rec/%s-stem%d.wav", home_, recName_, s);
        recFile_[s] = hal_.fs->open(path, IStorage::kWrite);
        if (recFile_[s] >= 0) hal_.fs->write(recFile_[s], hdr, sizeof hdr);
    }
    if (recFile_[0] < 0) {
        toast("Cannot write to the card");
        return;
    }
    recFrames_ = 0;
    recDropped_ = 0;
    recTail_ = recHead_;
    recOn_ = true;
    char msg[64];
    std::snprintf(msg, sizeof msg, "Recording %s", recName_);
    toast(msg);
}

void Shell::pumpRecorder() {
    if (!hal_.fs) return;
    const bool on = recOn_;
    if (!on && recFile_[0] < 0) return;
    uint32_t avail = recHead_ - recTail_;
    if (avail > kRecRingFrames - kBlock * 4) {   // the card fell behind: drop the oldest
        recDropped_ += avail - kBlock * 4;
        recTail_ = recHead_ - kBlock * 4;
        avail = kBlock * 4;
    }
    uint32_t chunk = avail > 8192 ? 8192 : avail;
    while (chunk) {
        const uint32_t at = recTail_ % kRecRingFrames;
        const uint32_t run = (kRecRingFrames - at) < chunk ? (kRecRingFrames - at) : chunk;
        for (int s = 0; s < kRecStreams; ++s)
            if (recFile_[s] >= 0) hal_.fs->write(recFile_[s], recRing_[s] + at * 2, run * 4);
        recTail_ += run;
        recFrames_ += run;
        chunk -= run;
    }
}

void Shell::stopRecording() {
    recOn_ = false;
    pumpRecorder();
    uint8_t hdr[ot::wav::kHeaderSize];
    ot::wavMakeHeader(hdr, 2, recFrames_ * 4);
    for (int s = 0; s < kRecStreams; ++s) {
        if (recFile_[s] < 0) continue;
        hal_.fs->seek(recFile_[s], 0);
        hal_.fs->write(recFile_[s], hdr, sizeof hdr);
        hal_.fs->close(recFile_[s]);
        recFile_[s] = -1;
    }
    char msg[64];
    std::snprintf(msg, sizeof msg, "Saved %s (%lu s)%s", recName_, static_cast<unsigned long>(recFrames_ / kSampleRate),
                  recDropped_ ? " - card too slow, gaps" : "");
    toast(msg);
}

// ---- MIDI ---------------------------------------------------------------------------
void Shell::pumpMidi() {
    if (!hal_.midi) return;
    MidiMsg m;
    while (midiOutRing_.pop(m)) {
        uint8_t b[3] = {m.status, m.d1, m.d2};
        uint32_t n = 3;
        if (m.status >= 0xF8) n = 1;
        else if ((m.status & 0xF0) == 0xC0 || (m.status & 0xF0) == 0xD0) n = 2;
        hal_.midi->send(ot::MidiPortId::kDin, b, n);
        hal_.midi->send(ot::MidiPortId::kUsb, b, n);
    }
    for (int port = 0; port < 2; ++port) {
        uint8_t buf[64];
        const uint32_t n = hal_.midi->receive(port ? ot::MidiPortId::kUsb : ot::MidiPortId::kDin, buf, sizeof buf);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t b = buf[i];
            if (b >= 0xF8) {   // realtime
                if (set_.clockMode == 2) {
                    if (b == 0xF8) {
                        const uint32_t us = hal_.clock ? hal_.clock->micros() : 0;
                        if (lastClockInUs_) {
                            const uint32_t d = us - lastClockInUs_;
                            if (d > 500 && d < 200000) {
                                const float inst = 60.0f * 1000000.0f / (float(d) * 24.0f);
                                clockInBpm_ = clockInBpm_ < 20.0f ? inst : clockInBpm_ + (inst - clockInBpm_) * 0.05f;
                            }
                        }
                        lastClockInUs_ = us;
                    } else if (b == 0xFA || b == 0xFB) {
                        setPlaying(true);
                    } else if (b == 0xFC) {
                        setPlaying(false);
                    }
                }
                continue;
            }
            if (b & 0x80) {
                if (b >= 0xF0) {   // system common: ignore, cancels running status
                    midiRun_[port] = 0;
                    continue;
                }
                midiRun_[port] = b;
                midiHave_[port] = 0;
                const uint8_t t = b & 0xF0;
                midiNeed_[port] = (t == 0xC0 || t == 0xD0) ? 1 : 2;
                continue;
            }
            if (!midiRun_[port]) continue;
            midiData_[port][midiHave_[port]++] = b;
            if (midiHave_[port] < midiNeed_[port]) continue;
            midiHave_[port] = 0;
            MidiMsg msg;
            msg.status = midiRun_[port];
            msg.d1 = midiData_[port][0];
            msg.d2 = midiNeed_[port] > 1 ? midiData_[port][1] : 0;
            if (set_.midiInCh != 0 && msg.channel() != set_.midiInCh - 1) continue;
            if (!ws_ || introOn_) continue;
            if (kit_ && kit_->midi(msg)) continue;   // LABKIT MIDI learn
            ws_->onMidi(msg);
        }
    }
}

// ---- LEDs -----------------------------------------------------------------------------
void Shell::updateLeds() {
    if (!hal_.leds || !ws_) return;
    for (auto& p : pads_) p = Rgb();
    if (settingsOpen_) {
        // a calm scan on the pads while the settings are open
        const int c = int((millis() / 120) % kPadCols);
        for (int r = 0; r < kPadRows; ++r) pads_[padAt(c, r)] = scale(ws_->theme().accent, 0.4f);
    } else if (!(kit_ && kit_->leds(pads_))) {
        ws_->leds(pads_);
    }
    for (int i = 0; i < kPads; ++i) hal_.leds->setPad(uint8_t(i), pads_[i].r, pads_[i].g, pads_[i].b);
    hal_.leds->show();
}

// ---- frame -----------------------------------------------------------------------------
void Shell::toast(const char* msg) {
    copyStr(toast_, sizeof toast_, msg);
    toastUntil_ = millis() + 2200;
    dirty_ = true;
}

void Shell::drawToast(Canvas& g) {
    if (!toast_[0] || int32_t(toastUntil_ - millis()) <= 0) return;
    const Theme& t = ws_->theme();
    int w = kit::textWidth(font::kBody, toast_) + 40;
    if (w < 400) w = 400;
    if (w > 780) w = 780;
    const int x = 400 - w / 2;
    g.color(t.panel);
    g.fillRound(x, 380, w, 40, 8);
    g.color(t.accent);
    g.frame(x, 380, w, 40);
    g.color(t.fg);
    g.text(400, 400, font::kBody, toast_, Canvas::kCenter);
}

void Shell::drawFrame() {
    if (!hal_.display || !ws_) return;
    const Theme& t = ws_->theme();
    Canvas& g = canvas_;
    g.begin(t.bg);
    if (introOn_) {
        const uint32_t el = millis() - introStart_;
        ws_->drawIntro(g, el);
        if (el >= ws_->introMs()) introOn_ = false;
    } else if (settingsOpen_) {
        drawSettings(g);
    } else {
        if (!kit_ || !kit_->fullScreen()) {
            const uint32_t keep = kit_ ? kit_->reserveWords() : 0;
            if (keep) g.setLimit(Canvas::kBudgetWords - keep);
            ws_->draw(g);
            g.setLimit(Canvas::kBudgetWords);
        }
        if (kit_) {
            // BULUT fix: a full-screen view (MAP of a big registry) fills the
            // budget; keep room for the toast and the REC badge drawn after it.
            const bool toast = toast_[0] && int32_t(toastUntil_ - millis()) > 0;
            if (toast || recOn_) g.setLimit(Canvas::kBudgetWords - 64u);
            kit_->draw(g);
            g.setLimit(Canvas::kBudgetWords);
        }
        if (recOn_) {   // REC badge on top of every page
            g.color(t.warn);
            g.fillRound(700, 36, 92, 22, 6);
            g.color(Rgb(0xFFFFFF));
            g.textf(746, 47, font::kTiny, Canvas::kCenter, "REC %lu:%02lu",
                    static_cast<unsigned long>(recFrames_ / kSampleRate / 60),
                    static_cast<unsigned long>((recFrames_ / kSampleRate) % 60));
        }
    }
    drawToast(g);
    g.end();
    if (g.words() > maxWords_) maxWords_ = g.words();
    if (g.dropped()) ++overBudget_;
    ++frameCount_;
    dirty_ = false;
}

// ---- settings ----------------------------------------------------------------------------
int Shell::buildRows() {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, const char* unit,
                   void (*fn)(void*, int)) {
        if (n >= kMaxRows) return;
        SettingRow& r = rows_[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.unit = unit;
        r.onChange = fn;
        r.ctx = this;
    };
    add("Display brightness", &set_.brightness, 0, 2, kBright, "", [](void* c, int v) {
        Shell* s = static_cast<Shell*>(c);
        if (s->hal_.setBacklight) s->hal_.setBacklight(IDisplay::brightnessDuty(uint8_t(v)));
    });
    add("Boot intro", &set_.intro, 0, 1, kOffOn, "", nullptr);
    add("Tempo", &bpmRow_, 20, 400, nullptr, " BPM", [](void* c, int v) { static_cast<Shell*>(c)->setBpm(float(v)); });
    add("Clock", &set_.clockMode, 0, 2, kClock, "", nullptr);
    add("MIDI in channel", &set_.midiInCh, 0, 16, kMidiIn, "", nullptr);
    add("MIDI out channel", &set_.midiOutCh, 1, 16, nullptr, "", nullptr);
    add("Input", &set_.input, 0, 3, kInputs, "", [](void* c, int) { static_cast<Shell*>(c)->applyInput(); });
    add("Input gain", &set_.inputGain, 0, 100, nullptr, "", [](void* c, int) { static_cast<Shell*>(c)->applyInput(); });
    add("Input monitor", &set_.monitor, 0, 1, kOffOn, "", nullptr);
    add("Radio (x0.1 MHz)", &set_.radioX10, 875, 1080, nullptr, "", [](void* c, int) { static_cast<Shell*>(c)->applyInput(); });
    add("Master level", &set_.master, 0, 100, nullptr, "", nullptr);
    add("Master limiter", &set_.limiter, 0, 1, kOffOn, "", nullptr);
    add("Record stems", &set_.stems, 0, 1, kOffOn, "", nullptr);
    wsRowStart_ = n;
    if (ws_) n += ws_->settings(rows_ + n, kMaxRows - n);
    return n;
}

void Shell::settingsEvent(const Event& e) {
    if (e.type == Event::kPress && e.id <= btn::F8) {
        if (e.id == btn::F1) { settingsPage_ = 0; fwConfirm_ = false; }
        if (e.id == btn::F2) { settingsPage_ = 1; fwConfirm_ = false; scanFirmware(); }
        if (e.id == btn::F3) { settingsPage_ = 2; fwConfirm_ = false; }
        if (e.id == btn::F6 && settingsPage_ == 0) {
            if (ws_) ws_->save();
            saveSettings();
            toast("Saved");
        }
        if (e.id == btn::F7 && settingsPage_ == 1 && fwCount_ > 0) {
            if (!fwConfirm_) {
                fwConfirm_ = true;
            } else if (hal_.requestFirmwareUpdate && hal_.fs) {
                powerOff();
                char path[48];
                copyStr(path, sizeof path, fwFiles_[fwSel_]);
                if (!hal_.requestFirmwareUpdate(*hal_.fs, path)) toast("The update did not start");
                fwConfirm_ = false;
            } else {
                toast("No updater in this build");
                fwConfirm_ = false;
            }
        }
        if (e.id == btn::F8) {
            settingsOpen_ = false;
            saveSettings();
        }
        return;
    }
    if (settingsPage_ == 1) {
        if (e.type == Event::kPress && (e.id == btn::Up || e.id == btn::Down)) {
            fwSel_ += e.id == btn::Up ? -1 : 1;
            fwConfirm_ = false;
        }
        if (e.type == Event::kEncoder) {
            fwSel_ += e.delta;
            fwConfirm_ = false;
        }
        if (fwSel_ < 0) fwSel_ = 0;
        if (fwSel_ >= fwCount_) fwSel_ = fwCount_ ? fwCount_ - 1 : 0;
        return;
    }
    if (settingsPage_ != 0 || rowCount_ == 0) return;
    int delta = 0;
    if (e.type == Event::kPress) {
        if (e.id == btn::Up) settingsRow_ = settingsRow_ > 0 ? settingsRow_ - 1 : 0;
        if (e.id == btn::Down) settingsRow_ = settingsRow_ + 1 < rowCount_ ? settingsRow_ + 1 : settingsRow_;
        if (e.id == btn::Left) delta = -1;
        if (e.id == btn::Right) delta = 1;
    }
    if (e.type == Event::kEncoder) delta = e.delta;
    if (delta) {
        SettingRow& r = rows_[settingsRow_];
        if (r.value) {
            const int step = (e.shift && !r.names) ? 10 : 1;
            int v = *r.value + delta * step;
            if (v < r.min) v = r.min;
            if (v > r.max) v = r.max;
            if (v != *r.value) {
                *r.value = v;
                if (r.onChange) r.onChange(r.ctx, v);
            }
        }
    }
    const int visible = 12;
    if (settingsRow_ < settingsTop_) settingsTop_ = settingsRow_;
    if (settingsRow_ >= settingsTop_ + visible) settingsTop_ = settingsRow_ - visible + 1;
}

void Shell::drawSettings(Canvas& g) {
    const Theme& t = ws_->theme();
    const char* titles[] = {"Settings", "Firmware update", "About"};
    drawHeader(g, t, *this, titles[settingsPage_]);
    const char* keys[8] = {"Settings", "Firmware", "About", "", "", settingsPage_ == 0 ? "Save" : "",
                           settingsPage_ == 1 ? (fwConfirm_ ? "Confirm" : "Install") : "", "Close"};
    if (settingsPage_ == 0) {
        const int visible = 12;
        for (int i = 0; i < visible && settingsTop_ + i < rowCount_; ++i) {
            const int idx = settingsTop_ + i;
            const SettingRow& r = rows_[idx];
            const int y = 44 + i * 30;
            if (idx == wsRowStart_ && wsRowStart_ < rowCount_) {
                g.color(t.accent2);
                g.hline(20, y - 2, 760);
            }
            if (idx == settingsRow_) {
                g.color(t.panel);
                g.fill(16, y, 768, 28);
                g.color(t.accent);
                g.fill(16, y, 4, 28);
            }
            g.color(idx >= wsRowStart_ ? t.accent2 : t.fg);
            g.text(34, y + 14, font::kBody, r.label, Canvas::kCenterY);
            char v[48];
            const int val = r.value ? *r.value : 0;
            if (r.names) copyStr(v, sizeof v, r.names[val - r.min]);
            else std::snprintf(v, sizeof v, "%d%s", val, r.unit ? r.unit : "");
            g.color(idx == settingsRow_ ? t.accent : t.fg);
            g.text(760, y + 14, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
        }
        g.color(t.dim);
        g.text(400, 422, font::kTiny, "Up / Down: row   Encoder or Left / Right: value   Shift: x10", Canvas::kCenter);
    } else if (settingsPage_ == 1) {
        g.color(t.dim);
        g.text(30, 50, font::kSmall, "Images in /Firmware on the card. Pick one, press Install, then Confirm.", 0);
        g.text(30, 70, font::kSmall, "Any OpenTracker, lab or stock image can be installed from here.", 0);
        if (!fwCount_) {
            g.color(t.warn);
            g.text(400, 220, font::kBody, "No .ptf files in /Firmware", Canvas::kCenter);
        }
        for (int i = 0; i < fwCount_; ++i) {
            const int y = 104 + i * 26;
            if (i == fwSel_) {
                g.color(t.panel);
                g.fill(26, y, 748, 24);
                g.color(t.accent);
                g.fill(26, y, 4, 24);
            }
            g.color(i == fwSel_ ? t.accent : t.fg);
            g.text(40, y + 12, font::kBody, fwFiles_[i], Canvas::kCenterY);
        }
        if (fwConfirm_) {
            g.color(t.warn);
            g.fillRound(150, 380, 500, 40, 8);
            g.color(Rgb(0xFFFFFF));
            g.text(400, 400, font::kBody, "Press Confirm (F7) again to install", Canvas::kCenter);
        }
    } else {
        g.color(t.accent);
        g.text(400, 120, font::kHuge, fwName_, Canvas::kCenter);
        g.color(t.fg);
        g.text(400, 170, font::kBody, ws_->tagline(), Canvas::kCenter);
        g.color(t.dim);
        g.textf(400, 220, font::kSmall, Canvas::kCenter, "polyend-tracker-lab %s  -  on OpenTracker %s", LAB_VERSION,
                OT_VERSION);
        g.textf(400, 245, font::kSmall, Canvas::kCenter, "Memory used %lu KB of %lu KB",
                static_cast<unsigned long>(arena_.used() / 1024), static_cast<unsigned long>(arena_.size() / 1024));
        g.text(400, 270, font::kSmall, "github.com/semierendonmez-commits/polyend-tracker-lab", Canvas::kCenter);
        g.text(400, 300, font::kSmall, "Clean-room OpenTracker base: no Polyend code. Not affiliated with Polyend.",
               Canvas::kCenter);
    }
    drawFKeyBar(g, t, keys, settingsPage_);
}

void Shell::scanFirmware() {
    fwCount_ = 0;
    fwSel_ = 0;
    if (!hal_.fs) return;
    struct Ctx {
        Shell* s;
    } c{this};
    hal_.fs->listDir(
        "Firmware",
        [](const IStorage::DirEntry& e, void* p) {
            Shell* s = static_cast<Ctx*>(p)->s;
            if (e.isDir || !endsWithCi(e.name, ".ptf")) return true;
            if (s->fwCount_ >= kMaxFw) return false;
            copyStr(s->fwFiles_[s->fwCount_], sizeof s->fwFiles_[0], e.name);
            if (s->hal_.isUpdateImageName && !s->hal_.isUpdateImageName(e.name)) return true;
            ++s->fwCount_;
            return true;
        },
        &c);
}

void Shell::loadSettings() {
    if (!hal_.fs) return;
    char path[48];
    std::snprintf(path, sizeof path, "%s/settings.bin", home_);
    const IStorage::Handle h = hal_.fs->open(path, IStorage::kRead);
    if (h < 0) return;
    Settings s;
    if (hal_.fs->read(h, &s, sizeof s) == int32_t(sizeof s) && s.version == Settings().version) set_ = s;
    hal_.fs->close(h);
}

void Shell::saveSettings() {
    if (!hal_.fs || recOn_) return;
    set_.bpmX10 = bpmX10_;
    char path[48];
    std::snprintf(path, sizeof path, "%s/settings.bin", home_);
    const IStorage::Handle h = hal_.fs->open(path, IStorage::kWrite);
    if (h < 0) return;
    hal_.fs->write(h, &set_, sizeof set_);
    hal_.fs->close(h);
}

}  // namespace lab

// LABKIT v2 implementation, in this translation unit on purpose: every build
// that links shell.cpp (firmware, simulator, host tests, QEMU bench, PLATFORM
// station apps) gets it without new entries in its makefiles.
// The UI side is built for size (-Os, ~20 KB less flash); the few functions
// that run in the audio interrupt are marked LABKIT_HOT and stay at -O2.
#if defined(__GNUC__) && !defined(__clang__)
#define LABKIT_HOT __attribute__((optimize("O2")))
#pragma GCC push_options
#pragma GCC optimize("Os")
#else
#define LABKIT_HOT
#endif
#include "lab/params_impl.h"
#include "lab/mod_impl.h"
#include "lab/nav_impl.h"
#include "lab/browser_impl.h"
#include "lab/keyboard_impl.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
