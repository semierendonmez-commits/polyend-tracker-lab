// LABKIT DEMO -- the template workstation, rewritten to use every LABKIT v2
// feature. Copy src/ws/ from here to start a new firmware.
//
//   - a small 8-voice synth (saw / square / tri / sine / sample, SVF, ADSR,
//     tempo delay) and an arpeggiator, played from the pads
//   - five pages, every value a registered param (float, int, enum, exp,
//     a setter binding), with groups, fixed pads and per-page help
//   - two exported mod sources (ENV, ARP) and a first-boot patch with routes
//     and named macros, so the MAP / PARAM / MACRO / MOD views have content
//   - the browser (F7: load a WAV as the oscillator), the keyboard (F6: name
//     the patch), save / load of the registry chunk inside its own file (F8)
//
// Keys: F1..F5 pages, F6 name, F7 sample, F8 save (Shift + F8: load),
// Left / Right: the page's cursor, encoder: its value, pads: notes.
// LABKIT adds I.Param (PARAM pads), Master (MAP), Shift + I.Param (MACRO),
// Shift + Master (MOD), Shift + Config (HELP) -- NAV.md.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "lab/dsp.h"
#include "lab/lab.h"

namespace lab {
namespace {

using kit::ParamId;

const char* const kWaves[] = {"Saw", "Square", "Tri", "Sine", "Sample"};
const char* const kDelayDiv[] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
const float kDelayBeats[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
const char* const kArpModes[] = {"Off", "Up", "Down", "Up-Dn", "Random"};
const char* const kArpRates[] = {"1/4", "1/8", "1/16", "1/32"};
const float kArpBeats[] = {1.0f, 0.5f, 0.25f, 0.125f};

enum Page { kPgOsc, kPgFilter, kPgAmp, kPgFx, kPgArp, kPageCount };
const char* const kPageNames[kPageCount] = {"OSC", "FILTER", "AMP", "FX", "ARP"};

constexpr int kVoices = 8;
constexpr uint32_t kSampleMax = 44100 * 3;   // 3 s mono sample for the "Sample" wave
constexpr uint32_t kDelayMax = 44100 * 2;    // 2 s delay line

struct Voice {
    bool on = false, gate = false;
    int note = 60;
    float vel = 1.0f, ph = 0.0f, ph2 = 0.0f, env = 0.0f, lp = 0.0f, bp = 0.0f, spos = 0.0f;
    int stage = 0;   // 0 attack, 1 decay, 2 sustain, 3 release
    uint32_t age = 0;
};

class Demo final : public Workstation {
public:
    const char* name() const override { return "LABKIT"; }
    const char* tagline() const override { return "LABKIT v2 demo: params, pads, MAP, macros, mod, browser"; }
    const Theme& theme() const override { return theme_; }

    bool init(Context& c) override {
        ctx_ = &c;
        theme_.bg = Rgb(0x07090C);
        theme_.panel = Rgb(0x141A22);
        theme_.fg = Rgb(0xE8EEF4);
        theme_.dim = Rgb(0x6A7888);
        theme_.accent = Rgb(0x35D0FF);
        theme_.accent2 = Rgb(0xFF5FA0);
        sample_ = static_cast<int16_t*>(c.arena->alloc(kSampleMax * 2));
        dl_ = static_cast<float*>(c.arena->alloc(kDelayMax * 4 * 2));
        if (!sample_ || !dl_) return false;
        copyStr(patch_, sizeof patch_, "INIT");
        registerParams();
        // Sources the routes can use (before loading routes that name them).
        c.mod().addSource("ENV", &envOut_, false);
        c.mod().addSource("ARP", &arpOut_, false);
        // Optional services: the browser (listing + 4 s preview) and the keyboard need nothing.
        c.browser().reserve(256, 4.0f);
        if (!load()) firstBootPatch();
        return true;
    }

    // ---- LABKIT navigation --------------------------------------------------
    int navPages(kit::PageInfo* out, int cap) override {
        static const char* const help[kPageCount] = {
            "The oscillator: wave, octave, detune, pulse width, noise and the sample start.",
            "A state-variable low-pass: cutoff, resonance, envelope amount, key tracking, drive.",
            "The amplitude envelope (ADSR), the level (a setter binding, dB) and the stereo spread.",
            "A tempo delay with feedback, tone and mix.",
            "The arpeggiator: hold pads, press Play. Mode, rate, gate, octaves, swing."};
        int n = 0;
        for (; n < kPageCount && n < cap; ++n) {
            out[n].name = kPageNames[n];
            out[n].help = help[n];
            out[n].color = pageColor(n);
            out[n].fkeys[0] = "OSC page";
            out[n].fkeys[1] = "FILTER page";
            out[n].fkeys[2] = "AMP page";
            out[n].fkeys[3] = "FX page";
            out[n].fkeys[4] = "ARP page";
            out[n].fkeys[5] = "Name the patch (keyboard)";
            out[n].fkeys[6] = "Load a WAV as the oscillator (browser)";
            out[n].fkeys[7] = "Save the patch (Shift: load it back)";
        }
        return n;
    }
    int navPage() const override { return page_; }
    void navGoto(int page) override {
        if (page >= 0 && page < kPageCount) page_ = page;
        cursor_ = 0;
    }
    void navFocus(ParamId id) override {
        // keep our own cursor on the param the MAP / PARAM pads focused
        for (int i = 0; i < pageCount_[page_]; ++i)
            if (pageParams_[page_][i] == id) cursor_ = i;
    }
    void navParamChanged(ParamId id) override {
        (void)id;
        dirty_ = true;   // a Shell view changed the patch
    }

    void drawIntro(Canvas& g, uint32_t t) override {
        const float k = float(t) / float(introMs());
        g.color(mix(theme_.bg, theme_.accent, k));
        g.text(400, 220, font::kHuge, "LABKIT", Canvas::kCenter);
        g.color(mix(theme_.bg, theme_.dim, k));
        g.text(400, 262, font::kSmall, "the shared navigation of every lab firmware", Canvas::kCenter);
    }

    void onEvent(const Event& e) override {
        kit::Registry& P = ctx_->params();
        if (e.type == Event::kPadDown) {
            padDown(e.id, e.velocity);
            return;
        }
        if (e.type == Event::kPadUp) {
            padUp(e.id);
            return;
        }
        if (e.type == Event::kEncoder) {
            const ParamId id = focused();
            P.nudge(id, e.delta, e.shift);   // through the registry: the HUD shows it
            dirty_ = true;
            return;
        }
        if (e.type != Event::kPress) return;
        if (e.id >= btn::F1 && e.id <= btn::F5) {
            navGoto(e.id - btn::F1);
            return;
        }
        switch (e.id) {
            case btn::Left: cursor_ = cursor_ > 0 ? cursor_ - 1 : 0; break;
            case btn::Right: cursor_ = cursor_ + 1 < pageCount_[page_] ? cursor_ + 1 : cursor_; break;
            case btn::Up: navGoto((page_ + kPageCount - 1) % kPageCount); break;
            case btn::Down: navGoto((page_ + 1) % kPageCount); break;
            case btn::Delete: P.reset(focused()); break;
            case btn::F6: nameDialog(); break;
            case btn::F7: sampleDialog(); break;
            case btn::F8:
                if (e.shift) ctx_->toast(load() ? "Patch loaded" : "No saved patch");
                else saveFile(true);
                break;
            default: break;
        }
    }

    void onMidi(const MidiMsg& m) override {
        if (m.type() == 0x90 && m.d2) post(true, m.d1, m.d2);
        else if (m.type() == 0x80 || (m.type() == 0x90 && !m.d2)) post(false, m.d1, 0);
    }

    void update(uint32_t ms) override {
        (void)ms;
        if (pendingSample_[0]) loadSample();
    }

    void draw(Canvas& g) override {
        kit::Registry& P = ctx_->params();
        drawHeader(g, theme_, *ctx_->shell, kPageNames[page_]);
        // patch and sample
        g.color(theme_.dim);
        g.text(20, 50, font::kTiny, "PATCH", Canvas::kCenterY);
        g.color(theme_.fg);
        g.textf(76, 50, font::kBody, Canvas::kCenterY, "%s%s", patch_, dirty_ ? " *" : "");
        g.color(theme_.dim);
        g.text(420, 50, font::kTiny, "SAMPLE", Canvas::kCenterY);
        g.color(sampleFrames_ ? theme_.fg : theme_.dim);
        char nm[48];
        kit::fitText(nm, sizeof nm, font::kSmall, sampleName_[0] ? sampleName_ : "none (F7)", 300);
        g.text(484, 50, font::kSmall, nm, Canvas::kCenterY);
        // page params as knobs, two rows of up to 6
        const Rgb pc = pageColor(page_);
        g.color(pc);
        g.fill(0, 66, kScreenW, 2);
        const int n = pageCount_[page_];
        for (int i = 0; i < n; ++i) {
            const int row = i / 6, col = i % 6;
            const int perRow = n - row * 6 < 6 ? n - row * 6 : 6;
            const int cx = 400 + (col * 2 - (perRow - 1)) * 62;
            kit::drawParamKnob(g, theme_, P, pageParams_[page_][i], cx, 128 + row * 104, 30, i == cursor_);
        }
        // voices + the pad keyboard
        g.color(theme_.dim);
        g.text(20, 352, font::kTiny, "VOICES", Canvas::kCenterY);
        for (int v = 0; v < kVoices; ++v) {
            g.color(voices_[v].on ? scale(pc, 0.3f + 0.7f * voices_[v].env) : theme_.panel);
            g.fill(80 + v * 22, 344, 18, 16);
        }
        g.color(theme_.dim);
        g.text(280, 352, font::kTiny, arpMode_ > 0.5f ? "ARP: hold pads, Play runs it" : "pads play notes (bottom row = C2)",
               Canvas::kCenterY);
        g.text(20, 380, font::kTiny,
               "I.Param: PARAM pads   Master: MAP   Shift+I.Param: MACROS   Shift+Master: MOD   Shift+Config: HELP",
               Canvas::kCenterY);
        g.text(20, 398, font::kTiny, "Left / Right: knob   encoder: value (Shift: fine)   Delete: default   Up / Down: page",
               Canvas::kCenterY);
        const char* keys[8] = {"OSC", "FILTER", "AMP", "FX", "ARP", "Name", "Sample", "Save"};
        drawFKeyBar(g, theme_, keys, page_);
    }

    void leds(Rgb* pads) override {
        const Rgb pc = pageColor(page_);
        for (int i = 0; i < kPads; ++i) {
            const int note = padNote(i);
            const bool black = ((1 << (note % 12)) & 0x54A) != 0;   // C# D# F# G# A#
            Rgb c = black ? scale(pc, 0.08f) : scale(pc, 0.22f);
            if (note % 12 == 0) c = scale(pc, 0.55f);
            if (held_[i]) c = Rgb(0xFFFFFF);
            pads[i] = c;
        }
    }

    void render(const Transport& t, const AudioIn&, float* l, float* r) override {
        NoteEv ev;   // notes from the main loop (pads, MIDI) arrive through a ring
        while (notes_.pop(ev)) {
            if (ev.on) noteOn(ev.note, float(ev.vel) / 127.0f);
            else noteOff(ev.note);
        }
        arp(t);
        const float sr = dsp::kSr;
        const float aK = 1.0f - expf(-1.0f / (attack_ * 0.001f * sr + 1.0f));
        const float dK = 1.0f - expf(-1.0f / (decay_ * 0.001f * sr + 1.0f));
        const float rK = 1.0f - expf(-1.0f / (release_ * 0.001f * sr + 1.0f));
        const int wave = waveSel_;
        float outL[kBlock] = {}, outR[kBlock] = {};
        float envMax = 0.0f;
        for (int v = 0; v < kVoices; ++v) {
            Voice& vo = voices_[v];
            if (!vo.on) continue;
            const float note = float(vo.note + octave_ * 12);
            const float hz = dsp::mtof(note);
            const float inc = hz * dsp::kInvSr, inc2 = dsp::mtof(note + detune_ * 0.01f) * dsp::kInvSr;
            const float track = dsp::mtof(60.0f + (float(vo.note) - 60.0f) * keyTrack_) / dsp::mtof(60.0f);
            const float pan = (float(v) / float(kVoices - 1) - 0.5f) * spread_;
            const float gl = 0.5f - pan * 0.5f, gr = 0.5f + pan * 0.5f;
            const float sinc = float(sampleRate_) / sr * dsp::mtof(float(vo.note)) / dsp::mtof(60.0f);
            const float cutoffT = cutoff_ * track, envK = envAmt_ * 4.0f, wK = dsp::kPi * dsp::kInvSr * 0.5f;
            for (uint32_t i = 0; i < kBlock; ++i) {
                // envelope
                if (vo.stage == 0) {
                    vo.env += (1.05f - vo.env) * aK;
                    if (vo.env >= 1.0f) { vo.env = 1.0f; vo.stage = 1; }
                } else if (vo.stage == 1) {
                    vo.env += (sustain_ - vo.env) * dK;
                    if (fabsf(vo.env - sustain_) < 1e-3f) vo.stage = 2;
                } else if (vo.stage == 3) {
                    vo.env -= vo.env * rK;
                    if (vo.env < 1e-4f) { vo.on = false; break; }
                }
                // oscillator
                vo.ph += inc;
                if (vo.ph >= 1.0f) vo.ph -= 1.0f;
                vo.ph2 += inc2;
                if (vo.ph2 >= 1.0f) vo.ph2 -= 1.0f;
                float s;
                switch (wave) {
                    case 0: s = (vo.ph + vo.ph2) - 1.0f; break;
                    case 1: s = ((vo.ph < pw_) ? 0.5f : -0.5f) + ((vo.ph2 < pw_) ? 0.5f : -0.5f); break;
                    case 2: s = (1.0f - 4.0f * fabsf(vo.ph - 0.5f)) * 0.5f + (1.0f - 4.0f * fabsf(vo.ph2 - 0.5f)) * 0.5f; break;
                    case 3: s = sinf(dsp::kTwoPi * vo.ph); break;
                    default: {
                        const uint32_t k = uint32_t(vo.spos);
                        s = (sampleFrames_ && k + 1 < sampleFrames_) ? float(sample_[k]) * (1.0f / 32768.0f) : 0.0f;
                        vo.spos += sinc;
                    }
                }
                if (noise_ > 0.0f) s += noise_ * rng_.bi();
                // SVF low-pass
                float fc = cutoffT * (1.0f + envK * vo.env);
                fc = fc < 20.0f ? 20.0f : (fc > 16000.0f ? 16000.0f : fc);
                const float xw = fc * wK;                    // 2 sin(x), x <= 0.57: a cubic is plenty
                const float f = 2.0f * xw * (1.0f - xw * xw * (1.0f / 6.0f));
                const float q = 1.0f - reso_ * 0.96f;
                const float x = dsp::softclip(s * drive_);
                vo.lp += f * vo.bp;
                const float hp = x - vo.lp - q * vo.bp;
                vo.bp += f * hp;
                const float y = vo.lp * vo.env * vo.vel * 0.3f;
                outL[i] += y * gl * 2.0f;
                outR[i] += y * gr * 2.0f;
            }
            if (vo.env > envMax) envMax = vo.env;
        }
        envOut_ = envMax;
        // tempo delay
        const float beats = kDelayBeats[delayDiv_ < 0 ? 0 : (delayDiv_ > 5 ? 5 : delayDiv_)];
        const float bpm = t.bpm > 20.0f ? t.bpm : 120.0f;
        uint32_t dlen = uint32_t(beats * 60.0f / bpm * dsp::kSr);
        if (dlen >= kDelayMax) dlen = kDelayMax - 1;
        const float toneK = 1.0f - expf(-dsp::kTwoPi * tone_ * dsp::kInvSr);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint32_t rd = (dw_ + kDelayMax - dlen) % kDelayMax;
            const float dlL = dl_[rd * 2], dlR = dl_[rd * 2 + 1];
            dtl_ += (dlR - dtl_) * toneK;   // ping-pong
            dtr_ += (dlL - dtr_) * toneK;
            dl_[dw_ * 2] = outL[i] + dtl_ * feedback_;
            dl_[dw_ * 2 + 1] = outR[i] + dtr_ * feedback_;
            dw_ = (dw_ + 1) % kDelayMax;
            l[i] += (outL[i] + dlL * dmix_) * level_;
            r[i] += (outR[i] + dlR * dmix_) * level_;
        }
    }

    void onTransport(bool playing) override {
        if (!playing)
            for (Voice& v : voices_)
                if (v.gate) { v.gate = false; v.stage = 3; }
    }

    void save() override { saveFile(false); }

    int benchScenarios() const override { return 2; }
    const char* benchName(int i) const override { return i ? "8voices+arp" : "8voices"; }
    void benchSetup(int i) override {
        for (int v = 0; v < kVoices; ++v) noteOn(48 + v * 3, 1.0f);
        arpMode_ = i ? 1.0f : 0.0f;
        for (int p = 0; p < 4; ++p) held_[24 + p * 2] = true;
    }

private:
    // ---- params --------------------------------------------------------------
    void registerParams() {
        kit::Registry& P = ctx_->params();
        auto track = [this](int page, ParamId id) {
            if (pageCount_[page] < 12) pageParams_[page][pageCount_[page]++] = id;
        };
        P.setPage(kPgOsc);
        P.setGroup("OSC", pageColor(kPgOsc));
        track(kPgOsc, P.add("osc.wave", "Wave", &waveSel_).names(kWaves, 5));
        track(kPgOsc, P.add("osc.oct", "Octave", &octave_).range(-2, 2).bipolar());
        track(kPgOsc, P.add("osc.det", "Detune", &detune_).range(0.0f, 50.0f).step(0.5f).unit("ct"));
        track(kPgOsc, P.add("osc.pw", "Width", &pw_).range(0.05f, 0.95f).percent());
        track(kPgOsc, P.add("osc.noise", "Noise", &noise_).range(0.0f, 0.5f).percent());
        P.setPage(kPgFilter);
        P.setGroup("FILTER", pageColor(kPgFilter));
        cutoffId_ = P.add("flt.cut", "Cutoff", &cutoff_).range(40.0f, 16000.0f).exp().unit("Hz");
        track(kPgFilter, cutoffId_);
        resoId_ = P.add("flt.res", "Reso", &reso_).percent();
        track(kPgFilter, resoId_);
        track(kPgFilter, P.add("flt.env", "Env amt", &envAmt_).range(-1.0f, 1.0f).percent().bipolar());
        track(kPgFilter, P.add("flt.key", "Key trk", &keyTrack_).percent());
        P.setGroup("DRIVE", Rgb(0xFF5A6A));
        track(kPgFilter, P.add("flt.drive", "Drive", &drive_).range(1.0f, 8.0f).exp().unit("x"));
        P.setPage(kPgAmp);
        P.setGroup("ENV", pageColor(kPgAmp));
        track(kPgAmp, P.add("amp.a", "Attack", &attack_).range(1.0f, 2000.0f).exp().unit("ms"));
        track(kPgAmp, P.add("amp.d", "Decay", &decay_).range(5.0f, 4000.0f).exp().unit("ms"));
        track(kPgAmp, P.add("amp.s", "Sustain", &sustain_).percent());
        track(kPgAmp, P.add("amp.r", "Release", &release_).range(5.0f, 5000.0f).exp().unit("ms"));
        P.setGroup("OUT", Rgb(0xE8EEF4));
        // A setter binding: dB in the UI, a linear gain in the DSP (runs in the audio block).
        track(kPgAmp, P.add("amp.level", "Level", [](void* c, float db) { static_cast<Demo*>(c)->level_ = dsp::dbToGain(db); },
                            this, -6.0f)
                          .range(-40.0f, 6.0f)
                          .step(0.5f)
                          .unit("dB")
                          .def(-6.0f));
        track(kPgAmp, P.add("amp.spread", "Spread", &spread_).percent());
        P.setPage(kPgFx);
        P.setGroup("DELAY", pageColor(kPgFx));
        track(kPgFx, P.add("fx.time", "Time", &delayDiv_).names(kDelayDiv, 6));
        track(kPgFx, P.add("fx.fb", "Feedback", &feedback_).range(0.0f, 0.95f).percent());
        track(kPgFx, P.add("fx.tone", "Tone", &tone_).range(300.0f, 16000.0f).exp().unit("Hz"));
        dmixId_ = P.add("fx.mix", "Mix", &dmix_).percent();
        track(kPgFx, dmixId_);
        P.setPage(kPgArp);
        P.setGroup("ARP", pageColor(kPgArp));
        track(kPgArp, P.add("arp.mode", "Mode", &arpMode_).names(kArpModes, 5).pad(0));
        track(kPgArp, P.add("arp.rate", "Rate", &arpRate_).names(kArpRates, 4).pad(1));
        track(kPgArp, P.add("arp.gate", "Gate", &arpGate_).range(0.05f, 1.0f).percent().pad(2));
        track(kPgArp, P.add("arp.oct", "Octaves", &arpOct_).range(1, 4).pad(3));
        track(kPgArp, P.add("arp.swing", "Swing", &swing_).range(0.0f, 0.5f).percent().pad(12));   // second pad row
        P.setPage(0);
    }

    void firstBootPatch() {
        kit::Mod& M = ctx_->mod();
        kit::Registry& P = ctx_->params();
        M.setMacroName(0, "BRIGHT");
        M.setMacroName(1, "SPACE");
        M.setMacroName(2, "BITE");
        M.route(kit::kSrcMacro + 0, cutoffId_, 0.45f);
        M.route(kit::kSrcMacro + 0, resoId_, 0.2f);
        const int w = M.route(kit::kSrcMacro + 1, dmixId_, 0.0f);
        M.setWindow(w, true, 0.05f, 0.6f);
        M.route(kit::kSrcMacro + 1, P.find("fx.fb"), 0.35f);
        M.route(kit::kSrcMacro + 2, P.find("flt.drive"), 0.6f);
        M.route(kit::kSrcLfo + 0, cutoffId_, 0.12f);
        M.route(M.findSource("ENV"), P.find("osc.pw"), -0.25f);
    }

    ParamId focused() const { return pageCount_[page_] ? pageParams_[page_][cursor_] : kit::kNoParam; }

    static Rgb pageColor(int p) {
        static const Rgb c[kPageCount] = {Rgb(0x35D0FF), Rgb(0xFF8A3D), Rgb(0x6BE36B), Rgb(0xB07CFF), Rgb(0xF2D230)};
        return c[p < 0 ? 0 : p % kPageCount];
    }

    // ---- notes ----------------------------------------------------------------
    static int padNote(int pad) { return 36 + (3 - padRow(pad)) * 12 + padCol(pad); }
    struct NoteEv {
        bool on;
        uint8_t note, vel;
    };
    void post(bool on, int note, int vel) { notes_.push(NoteEv{on, uint8_t(note), uint8_t(vel)}); }
    void padDown(int pad, uint8_t vel) {
        held_[pad] = true;
        if (arpMode_ < 0.5f || !ctx_->playing()) post(true, padNote(pad), vel);
    }
    void padUp(int pad) {
        held_[pad] = false;
        post(false, padNote(pad), 0);
    }
    void noteOn(int note, float vel) {
        int best = 0;
        uint32_t oldest = 0;
        for (int v = 0; v < kVoices; ++v) {
            if (!voices_[v].on) { best = v; break; }
            if (voices_[v].age >= oldest) { oldest = voices_[v].age; best = v; }
        }
        for (Voice& v : voices_) ++v.age;
        Voice& vo = voices_[best];
        vo.on = vo.gate = true;
        vo.note = note;
        vo.vel = vel;
        vo.stage = 0;
        vo.age = 0;
        vo.spos = 0.0f;
    }
    void noteOff(int note) {
        for (Voice& v : voices_)
            if (v.on && v.gate && v.note == note) {
                v.gate = false;
                v.stage = 3;
            }
    }
    // The arpeggiator runs in render(): held pads, in order, at the transport.
    void arp(const Transport& t) {
        if (arpMode_ < 0.5f || !t.playing) {
            arpOut_ = 0.0f;
            return;
        }
        int notes[kPads], n = 0;
        for (int p = kPads - 1; p >= 0; --p)   // bottom row first = low notes first
            if (held_[p]) notes[n++] = padNote(p);
        if (!n) return;
        for (int i = 1; i < n; ++i)   // sort up
            for (int j = i; j > 0 && notes[j] < notes[j - 1]; --j) {
                const int x = notes[j];
                notes[j] = notes[j - 1];
                notes[j - 1] = x;
            }
        const int oct = arpOct_ < 1 ? 1 : arpOct_;
        const int total = n * oct;
        const float stepBeats = kArpBeats[int(arpRate_ + 0.5f) & 3];
        const double step = t.beat / double(stepBeats);
        const int64_t idx = int64_t(step);
        const float into = float(step - double(idx));
        arpOut_ = float(idx % total) / float(total > 1 ? total - 1 : 1);
        if (idx != lastStep_) {
            lastStep_ = idx;
            if (lastArpNote_ >= 0) noteOff(lastArpNote_);
            const int mode = int(arpMode_ + 0.5f);
            int k = int(idx % total);
            if (mode == 2) k = total - 1 - k;
            if (mode == 3) {
                const int cyc = total > 1 ? 2 * total - 2 : 1;
                k = int(idx % cyc);
                if (k >= total) k = cyc - k;
            }
            if (mode == 4) k = rng_.range(total);
            lastArpNote_ = notes[k % n] + 12 * (k / n);
            gateOff_ = arpGate_ + ((idx & 1) ? swing_ : 0.0f);
            noteOn(lastArpNote_, 0.9f);
        } else if (into > gateOff_ && lastArpNote_ >= 0) {
            noteOff(lastArpNote_);
            lastArpNote_ = -1;
        }
    }

    // ---- browser + keyboard ------------------------------------------------------
    void nameDialog() {
        kit::KeyboardRequest q;
        q.title = "Patch name";
        q.initial = patch_;
        q.maxLen = 16;
        q.ctx = this;
        q.onDone = [](void* c, const char* text) {
            Demo* d = static_cast<Demo*>(c);
            copyStr(d->patch_, sizeof d->patch_, text[0] ? text : "INIT");
            d->dirty_ = true;
        };
        ctx_->keyboard().open(q);
    }
    void sampleDialog() {
        kit::BrowserRequest q;
        q.title = "Load a sample";
        q.folder = "Samples";
        q.exts = ".wav";
        q.ctx = this;
        q.onPick = [](void* c, const char* path) {
            Demo* d = static_cast<Demo*>(c);
            copyStr(d->pendingSample_, sizeof d->pendingSample_, path);   // loaded in update()
        };
        ctx_->browser().open(q);
    }
    void loadSample() {
        IStorage* fs = ctx_->fs;
        char path[160];
        copyStr(path, sizeof path, pendingSample_);
        pendingSample_[0] = 0;
        if (!fs) return;
        const IStorage::Handle h = fs->open(path, IStorage::kRead);
        if (h < 0) return;
        ot::StorageReader rd(*fs, h);
        ot::WavInfo info;
        uint32_t frames = 0;
        if (ot::wavParseHeader(rd, info) && ot::wavIsSupported(info)) {
            const uint32_t bpf = ot::wavBytesPerFrame(info), bps = info.bitsPerSample / 8u;
            fs->seek(h, info.dataOffset);
            uint8_t raw[2048];
            const uint32_t want = ot::wavSourceFrames(info) < kSampleMax ? ot::wavSourceFrames(info) : kSampleMax;
            sampleFrames_ = 0;   // the audio side stops reading while we fill
            while (frames < want) {
                const uint32_t n = (want - frames) < sizeof raw / bpf ? want - frames : uint32_t(sizeof raw / bpf);
                const int32_t got = fs->read(h, raw, n * bpf);
                if (got <= 0) break;
                for (uint32_t i = 0; i < uint32_t(got) / bpf; ++i) {
                    const uint8_t* p = raw + i * bpf;
                    int a = ot::wavDecodeSample(p, info.format, info.bitsPerSample);
                    if (info.channels > 1) a = (a + ot::wavDecodeSample(p + bps, info.format, info.bitsPerSample)) / 2;
                    sample_[frames + i] = int16_t(a);
                }
                frames += uint32_t(got) / bpf;
            }
            sampleRate_ = info.sampleRate;
        }
        fs->close(h);
        sampleFrames_ = frames;
        const char* b = std::strrchr(path, '/');
        copyStr(sampleName_, sizeof sampleName_, b ? b + 1 : path);
        copyStr(samplePath_, sizeof samplePath_, path);
        if (frames) ctx_->params().set(ctx_->params().find("osc.wave"), 4.0f);   // switch to the sample
        dirty_ = true;
    }

    // ---- save / load: our own file with the LABKIT chunk inside ---------------------
    // demo.lkp = "LKDM" | name[24] | sample path[128] | u32 chunk bytes | chunk
    struct FileHead {
        uint32_t magic = 0x4D444B4Cu;
        char name[24] = {};
        char sample[128] = {};
        uint32_t chunk = 0;
    };
    void saveFile(bool toastIt) {
        IStorage* fs = ctx_->fs;
        if (!fs) return;
        static uint8_t chunk[8192];
        FileHead h;
        copyStr(h.name, sizeof h.name, patch_);
        copyStr(h.sample, sizeof h.sample, samplePath_);
        h.chunk = ctx_->params().saveChunk(chunk, sizeof chunk);
        char path[64];
        std::snprintf(path, sizeof path, "%s/demo.lkp", ctx_->home);
        const IStorage::Handle f = fs->open(path, IStorage::kWrite);
        if (f < 0) return;
        fs->write(f, &h, sizeof h);
        fs->write(f, chunk, h.chunk);
        fs->close(f);
        dirty_ = false;
        if (toastIt) ctx_->toast("Patch saved");
    }
    bool load() {
        IStorage* fs = ctx_->fs;
        if (!fs) return false;
        char path[64];
        std::snprintf(path, sizeof path, "%s/demo.lkp", ctx_->home);
        const IStorage::Handle f = fs->open(path, IStorage::kRead);
        if (f < 0) return false;
        static uint8_t chunk[8192];
        FileHead h;
        bool ok = fs->read(f, &h, sizeof h) == int32_t(sizeof h) && h.magic == FileHead().magic && h.chunk <= sizeof chunk &&
                  fs->read(f, chunk, h.chunk) == int32_t(h.chunk);
        fs->close(f);
        if (ok) ok = ctx_->params().loadChunk(chunk, h.chunk);
        if (ok) {
            h.name[sizeof h.name - 1] = 0;
            h.sample[sizeof h.sample - 1] = 0;
            copyStr(patch_, sizeof patch_, h.name);
            if (h.sample[0]) copyStr(pendingSample_, sizeof pendingSample_, h.sample);
            dirty_ = false;
        }
        return ok;
    }
    Context* ctx_ = nullptr;
    Theme theme_;
    int page_ = 0, cursor_ = 0;
    ParamId pageParams_[kPageCount][12] = {};
    int pageCount_[kPageCount] = {};
    ParamId cutoffId_ = kit::kNoParam, resoId_ = kit::kNoParam, dmixId_ = kit::kNoParam;
    char patch_[24] = {};
    bool dirty_ = false;
    // sound state (the registry writes these at block rate)
    int waveSel_ = 0, octave_ = 0, delayDiv_ = 3, arpOct_ = 1;
    float detune_ = 7.0f, pw_ = 0.5f, noise_ = 0.0f;
    float cutoff_ = 1200.0f, reso_ = 0.35f, envAmt_ = 0.3f, keyTrack_ = 0.5f, drive_ = 1.5f;
    float attack_ = 5.0f, decay_ = 400.0f, sustain_ = 0.6f, release_ = 300.0f, level_ = 0.5f, spread_ = 0.5f;
    float feedback_ = 0.35f, tone_ = 4000.0f, dmix_ = 0.25f;
    float arpMode_ = 0.0f, arpRate_ = 2.0f, arpGate_ = 0.5f, swing_ = 0.0f;
    // exported mod sources
    float envOut_ = 0.0f, arpOut_ = 0.0f;
    // engine (voices belong to the audio side; the main loop posts notes)
    SpscRing<NoteEv, 64> notes_;
    Voice voices_[kVoices];
    volatile bool held_[kPads] = {};
    int64_t lastStep_ = -1;
    int lastArpNote_ = -1;
    float gateOff_ = 0.5f;
    dsp::Rng rng_{0xC0FFEEu};
    int16_t* sample_ = nullptr;
    volatile uint32_t sampleFrames_ = 0;
    uint32_t sampleRate_ = 44100;
    char sampleName_[64] = {}, samplePath_[128] = {}, pendingSample_[160] = {};
    float* dl_ = nullptr;
    uint32_t dw_ = 0;
    float dtl_ = 0.0f, dtr_ = 0.0f;
};

}  // namespace

Workstation* createWorkstation(Arena& arena) { return arena.make<Demo>(); }

}  // namespace lab
