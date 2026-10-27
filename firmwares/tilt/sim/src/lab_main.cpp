// polyend-tracker-lab desktop simulator: the same lab::Shell and workstation
// as the device image, on the simulator's host HAL (sim/README.md, LAB_SIM.md).
//
//   build/sim/lab-sim                       window (front panel, pads, keys)
//   build/sim/lab-sim --script S --panel    headless: replay a script, screenshots
//
// Script commands (one per line, # comments):
//   wait MS | press NAME | release NAME | tap NAME [MS] | hold NAME [MS]
//   pad N down|up|tap [VEL] | enc DELTA
//   screenshot FILE.png | panelshot FILE.png
//   peak-reset | expect-sound [MIN]     audio peak since the reset >= MIN (default 300)
//   budget [MAX]                         largest frame <= MAX display-list words, none over budget
//   midi B1 B2 [B3]                      inject a MIDI message on the DIN input
//   expect-midi-notes N                  at least N note-ons sent on DIN so far
//   expect-fps N
// LABKIT v2 (labkit/NAV.md):
//   expect-overlay NAME                  none|param|map|macro|mod|help|browser|keyboard
//   expect-param KEY VALUE [EPS]         a registered param's base value (units)
//   expect-effective KEY VALUE [EPS]     ... after modulation
//   expect-routes N                      mod routes in use
//   expect-learn N                       MIDI learn entries
//   cc CH CC VALUE                       a MIDI CC on the DIN input (CH 1..16)
//   mkdir PATH | mkwav PATH SECONDS [HZ] [CHANNELS]   make test files on the card
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "canvas.h"
#include "formats/wav.h"
#include "host_hal.h"
#include "host_midi.h"
#include "host_storage.h"
#include "lab/lab.h"
#include "panel.h"
#include "png.h"
#include "sdl_audio.h"
#include "sw_display.h"
#include "ui/buttons.h"

#ifndef LAB_FW_NAME
#define LAB_FW_NAME "LAB"
#endif

using namespace sim;
namespace b = ot::btn;

namespace {

struct Options {
    std::string sd, script, screenshot, wav;
    int scale = 1;
    bool lineIn = false, noAudio = false, noMidi = false, panelShot = false, busyDisplay = true;
    float lineInTone = 0.0f;
    long quitAfterMs = -1;
};

void usage() {
    std::fprintf(stderr,
                 "usage: lab-sim [options]\n"
                 "  --sd DIR          SD card root folder (default ~/.lab-sd-<fw>)\n"
                 "  --scale 1|2       window scale\n"
                 "  --line-in         use the default capture device as line in\n"
                 "  --line-in-tone HZ synthetic line input (tone bursts)\n"
                 "  --no-audio | --no-midi\n"
                 "  --quit-after MS\n"
                 "headless: --script FILE  --screenshot PNG  --panel  --wav OUT.wav\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], v;
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--sd" && next(o.sd)) continue;
        if (a == "--scale" && next(v)) { o.scale = std::atoi(v.c_str()) >= 2 ? 2 : 1; continue; }
        if (a == "--line-in") { o.lineIn = true; continue; }
        if (a == "--no-audio") { o.noAudio = true; continue; }
        if (a == "--no-midi") { o.noMidi = true; continue; }
        if (a == "--panel") { o.panelShot = true; continue; }
        if (a == "--instant-display") { o.busyDisplay = false; continue; }
        if (a == "--script" && next(o.script)) continue;
        if (a == "--screenshot" && next(o.screenshot)) continue;
        if (a == "--wav" && next(o.wav)) continue;
        if (a == "--line-in-tone" && next(v)) { o.lineInTone = float(std::atof(v.c_str())); continue; }
        if (a == "--quit-after" && next(v)) { o.quitAfterMs = std::atol(v.c_str()); continue; }
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        std::fprintf(stderr, "unknown or incomplete option: %s\n", a.c_str());
        usage();
        return false;
    }
    if (o.sd.empty()) {
        const char* home = std::getenv("HOME");
        std::string fw = LAB_FW_NAME;
        for (auto& c : fw) c = char(std::tolower(uint8_t(c)));
        o.sd = std::string(home ? home : ".") + "/.lab-sd-" + fw;
    }
    return true;
}

// A stand-in FM tuner: remembers the frequency; its "audio" is synthesised in
// Machine::lineBlock() (stations every 0.4 MHz, noise between them).
class SimRadio final : public ot::IRadio {
public:
    void setBand(float, float, float) override {}
    bool tune(float mhz) override { f_ = mhz; return true; }
    bool seek(bool up) override { f_ += up ? 0.4f : -0.4f; return true; }
    bool busy() const override { return false; }
    float frequency() const override { return f_; }

private:
    float f_ = 100.0f;
};

// HostMidi plus a script-injected input queue and a count of sent bytes.
class LabMidi final : public ot::IMidiPort {
public:
    HostMidi host;
    std::vector<uint8_t> injected;
    uint32_t sentBytes = 0, sentNotes = 0;
    bool send(ot::MidiPortId port, const uint8_t* data, uint32_t len) override {
        if (port == ot::MidiPortId::kDin) {
            sentBytes += len;
            if (len == 3 && (data[0] & 0xF0) == 0x90 && data[2]) ++sentNotes;
        }
        return host.send(port, data, len);
    }
    uint32_t receive(ot::MidiPortId port, uint8_t* dst, uint32_t cap) override {
        uint32_t n = 0;
        if (port == ot::MidiPortId::kDin && !injected.empty()) {
            n = uint32_t(injected.size() < cap ? injected.size() : cap);
            std::memcpy(dst, injected.data(), n);
            injected.erase(injected.begin(), injected.begin() + n);
            return n;
        }
        return host.receive(port, dst, cap);
    }
};

struct Machine {
    SimClock clock;
    SwDisplay display;
    SimInput input{clock};
    SimLeds leds;
    HostStorage storage;
    LabMidi midi;
    SdlAudio sdlAudio;
    ManualAudio manualAudio;
    SimRadio radio;
    ot::IAudioOut* audio = nullptr;
    std::vector<uint8_t> sdram;
    std::unique_ptr<lab::Shell> shell;
    // synthetic inputs
    float toneHz = 0.0f;
    uint8_t source = 0;
    uint64_t n = 0;
    uint32_t rng = 0x9E3779B9u;
    int16_t buf[128 * 2] = {};
    bool captureLineIn = false;

    const int16_t* lineBlock() {
        if (source == 0 && captureLineIn) return sdlAudio.lineIn();
        if (source == 0 && toneHz <= 0.0f) return nullptr;
        for (int i = 0; i < 128; ++i, ++n) {
            const double t = double(n) / 44100.0;
            double l = 0.0, r = 0.0;
            if (source == 1) {   // radio: a station near every 0.4 MHz, noise between
                const float f = radio.frequency();
                const float off = std::fabs(std::fmod(f - 87.5f, 0.4f) - 0.2f);   // 0.2 = on a station
                const double sig = off / 0.2;
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const double noise = (double(rng >> 8) / 8388608.0 - 1.0);
                const double prog = 0.5 * std::sin(2 * M_PI * (220.0 + 40.0 * std::sin(t * 0.7)) * t) *
                                    (0.6 + 0.4 * std::sin(2 * M_PI * 2.0 * t));
                l = r = sig * prog + (1.0 - sig) * 0.4 * noise;
            } else {
                const double ph = std::fmod(t, 0.6);
                const double env = ph > 0.4 ? 0.0 : std::sin(M_PI * ph / 0.4);
                l = 0.4 * env * std::sin(2 * M_PI * toneHz * t);
                r = 0.3 * env * std::sin(2 * M_PI * toneHz * 1.5 * t);
            }
            buf[2 * i] = int16_t(l * 32767.0);
            buf[2 * i + 1] = int16_t(r * 32767.0);
        }
        return buf;
    }

    bool start(const Options& o, bool headless) {
        clock.setVirtual(headless);
        if (!storage.init(o.sd)) {
            std::fprintf(stderr, "cannot use SD folder %s\n", o.sd.c_str());
            return false;
        }
        if (!o.noMidi && !headless) midi.host.open();
        if (headless) {
            audio = &manualAudio;
            if (!o.wav.empty() && !manualAudio.recordWav(o.wav.c_str()))
                std::fprintf(stderr, "cannot write %s\n", o.wav.c_str());
        } else if (!o.noAudio) {
            sdlAudio.enableLineIn(o.lineIn);
            captureLineIn = o.lineIn;
            audio = &sdlAudio;
        }
        toneHz = o.lineInTone;
        // Same arena size as the device: the SDRAM below `.sdram_bss`.
        sdram.assign(0x00FD9000u, 0);
        lab::Hal h;
        h.display = &display;
        h.audio = audio;
        h.fs = &storage;
        h.midi = &midi;
        h.input = &input;
        h.leds = &leds;
        h.clock = &clock;
        h.radio = &radio;
        h.lineIn = [](void* c) { return static_cast<Machine*>(c)->lineBlock(); };
        h.lineInCtx = this;
        h.selectInput = [](void* c, uint8_t src, uint8_t) {
            static_cast<Machine*>(c)->source = src;
            return true;
        };
        h.inputCtx = this;
        h.setBacklight = nullptr;
        h.isUpdateImageName = [](const char* name) {
            return std::strncmp(name, "PolyendTrac", 11) == 0 && lab::endsWithCi(name, ".ptf");
        };
        h.requestFirmwareUpdate = [](ot::IStorage&, const char* name) {
            std::fprintf(stderr, "sim: firmware update requested: %s (not flashed in the simulator)\n", name);
            return true;
        };
        shell.reset(new lab::Shell());
        if (!shell->init(h, sdram.data(), uint32_t(sdram.size()), LAB_FW_NAME)) {
            std::fprintf(stderr, "lab shell init failed\n");
            return false;
        }
        return true;
    }
    void step() { shell->loop(); }
    void shutdown() {
        if (audio) audio->stop();
        if (shell) shell->powerOff();
    }
};

void saveShot(Machine& m, const std::string& path, bool panel) {
    std::vector<uint32_t> fb(800 * 480);
    m.display.frontCopy(fb.data());
    bool ok;
    if (panel) {
        ::sim::Canvas c(Panel::kW, Panel::kH);
        Panel().draw(c, fb.data(), m.input, m.leds, 0, LAB_FW_NAME " - polyend-tracker-lab");
        ok = writePng(path.c_str(), c.pixels(), c.width(), c.height());
    } else {
        ok = writePng(path.c_str(), fb.data(), 800, 480);
    }
    std::fprintf(stderr, "%s %s\n", ok ? "saved" : "FAILED to save", path.c_str());
}

// A test WAV on the card: a decaying saw + sine (16-bit, 44.1 kHz).
bool makeWav(Machine& m, const std::string& path, double seconds, double hz, std::istringstream& rest) {
    int ch = 1;
    rest >> ch;
    ch = ch == 2 ? 2 : 1;
    const uint32_t frames = uint32_t(seconds * 44100.0);
    std::vector<int16_t> pcm(size_t(frames) * size_t(ch));
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = double(i) / 44100.0;
        const double env = std::exp(-t * 3.0);
        const double saw = 2.0 * std::fmod(t * hz, 1.0) - 1.0;
        const double v = env * (0.5 * saw + 0.4 * std::sin(2.0 * M_PI * hz * 0.5 * t));
        for (int c = 0; c < ch; ++c) pcm[size_t(i) * size_t(ch) + size_t(c)] = int16_t(v * (c ? 20000.0 : 26000.0));
    }
    uint8_t hdr[ot::wav::kHeaderSize];
    ot::wavMakeHeader(hdr, uint16_t(ch), frames * 2u * uint32_t(ch));
    const ot::IStorage::Handle h = m.storage.open(path.c_str(), ot::IStorage::kWrite);
    if (h < 0) return false;
    m.storage.write(h, hdr, sizeof hdr);
    m.storage.write(h, pcm.data(), uint32_t(pcm.size() * 2));
    m.storage.close(h);
    return true;
}

struct Headless {
    Machine& m;
    bool panel;
    uint64_t frameAcc = 0;
    int peak = 0;

    void advance(long ms) {
        for (long i = 0; i < ms; ++i) {
            m.clock.advanceUs(1000);
            frameAcc += 44100u;
            while (frameAcc >= 128u * 1000u) {
                frameAcc -= 128u * 1000u;
                m.manualAudio.pull();
            }
            m.step();
        }
    }
    bool button(const std::string& name, bool down, int line) {
        const int id = buttonByName(name.c_str());
        if (id < 0) {
            std::fprintf(stderr, "script:%d: unknown button '%s'\n", line, name.c_str());
            return false;
        }
        m.input.button(uint8_t(id), down);
        return true;
    }
    bool run(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "cannot read script %s\n", path.c_str());
            return false;
        }
        std::string s;
        int line = 0;
        uint32_t frames0 = 0, ms0 = 0;
        while (std::getline(f, s)) {
            ++line;
            const size_t hash = s.find('#');
            if (hash != std::string::npos) s.resize(hash);
            std::istringstream ss(s);
            std::string cmd, a1, a2, a3;
            ss >> cmd >> a1 >> a2 >> a3;
            if (cmd.empty()) continue;
            const long n1 = std::atol(a1.c_str());
            if (cmd == "wait") advance(n1);
            else if (cmd == "press" || cmd == "down") { if (!button(a1, true, line)) return false; advance(1); }
            else if (cmd == "release" || cmd == "up") { if (!button(a1, false, line)) return false; advance(1); }
            else if (cmd == "tap" || cmd == "hold") {
                const long t = a2.empty() ? (cmd == "tap" ? 60 : 700) : std::atol(a2.c_str());
                if (!button(a1, true, line)) return false;
                advance(t);
                button(a1, false, line);
                advance(60);
            } else if (cmd == "pad") {
                const int vel = a3.empty() ? 100 : std::atoi(a3.c_str());
                if (a2 == "down") m.input.pad(uint8_t(n1), true, uint8_t(vel));
                else if (a2 == "up") m.input.pad(uint8_t(n1), false);
                else {
                    m.input.pad(uint8_t(n1), true, uint8_t(vel));
                    advance(60);
                    m.input.pad(uint8_t(n1), false);
                }
                advance(1);
            } else if (cmd == "enc") {
                m.input.encoder(int(n1));
                advance(1);
            } else if (cmd == "screenshot") {
                advance(30);
                saveShot(m, a1, panel);
            } else if (cmd == "panelshot") {
                advance(30);
                saveShot(m, a1, true);
            } else if (cmd == "peak-reset") {
                m.manualAudio.peak = 0;
            } else if (cmd == "expect-sound") {
                const int min = a1.empty() ? 300 : int(n1);
                if (m.manualAudio.peak < min) {
                    std::fprintf(stderr, "script:%d: audio peak %d < %d\n", line, m.manualAudio.peak, min);
                    return false;
                }
                std::fprintf(stderr, "sound ok: peak %d\n", m.manualAudio.peak);
            } else if (cmd == "expect-silence") {
                const int max = a1.empty() ? 50 : int(n1);
                if (m.manualAudio.peak > max) {
                    std::fprintf(stderr, "script:%d: audio peak %d > %d\n", line, m.manualAudio.peak, max);
                    return false;
                }
            } else if (cmd == "budget") {
                const uint32_t max = a1.empty() ? lab::Canvas::kBudgetWords : uint32_t(n1);
                const uint32_t w = m.shell->maxFrameWords(), over = m.shell->overBudgetFrames();
                std::fprintf(stderr, "budget: max frame %u words, %u frames cut\n", unsigned(w), unsigned(over));
                if (w > max || over) {
                    std::fprintf(stderr, "script:%d: OVER BUDGET\n", line);
                    return false;
                }
                m.shell->resetFrameStats();
            } else if (cmd == "midi") {
                uint8_t bytes[3] = {uint8_t(std::strtol(a1.c_str(), nullptr, 0)),
                                    uint8_t(std::strtol(a2.c_str(), nullptr, 0)),
                                    uint8_t(std::strtol(a3.c_str(), nullptr, 0))};
                m.midi.injected.insert(m.midi.injected.end(), bytes, bytes + (a3.empty() ? (a2.empty() ? 1 : 2) : 3));
                advance(1);
            } else if (cmd == "expect-midi-notes") {
                if (m.midi.sentNotes < uint32_t(n1)) {
                    std::fprintf(stderr, "script:%d: %u MIDI notes sent < %ld\n", line, unsigned(m.midi.sentNotes), n1);
                    return false;
                }
                std::fprintf(stderr, "midi ok: %u notes sent\n", unsigned(m.midi.sentNotes));
            } else if (cmd == "expect-overlay") {
                const char* ov = m.shell->labkit().nav.overlayName();
                if (a1 != ov) {
                    std::fprintf(stderr, "script:%d: overlay '%s', expected '%s'\n", line, ov, a1.c_str());
                    return false;
                }
            } else if (cmd == "expect-param" || cmd == "expect-effective") {
                lab::kit::Registry& reg = m.shell->labkit().reg;
                const lab::kit::ParamId id = reg.find(a1.c_str());
                const double want = std::atof(a2.c_str()), eps = a3.empty() ? 1e-3 : std::atof(a3.c_str());
                const double got = cmd == "expect-param" ? reg.get(id) : reg.effective(id);
                if (!reg.valid(id) || std::fabs(got - want) > eps) {
                    std::fprintf(stderr, "script:%d: %s %s = %g, expected %g\n", line, cmd.c_str() + 7, a1.c_str(), got, want);
                    return false;
                }
            } else if (cmd == "expect-routes") {
                const int n = m.shell->labkit().mod.routeCount();
                if (n != int(n1)) {
                    std::fprintf(stderr, "script:%d: %d routes, expected %ld\n", line, n, n1);
                    return false;
                }
            } else if (cmd == "expect-learn") {
                const int n = m.shell->labkit().nav.learnCount();
                if (n != int(n1)) {
                    std::fprintf(stderr, "script:%d: %d MIDI learn entries, expected %ld\n", line, n, n1);
                    return false;
                }
            } else if (cmd == "cc") {
                const uint8_t bytes[3] = {uint8_t(0xB0 | ((n1 - 1) & 15)), uint8_t(std::atoi(a2.c_str()) & 127),
                                          uint8_t(std::atoi(a3.c_str()) & 127)};
                m.midi.injected.insert(m.midi.injected.end(), bytes, bytes + 3);
                advance(2);
            } else if (cmd == "mkdir") {
                m.storage.mkdir(a1.c_str());
            } else if (cmd == "mkwav") {
                if (!makeWav(m, a1, std::atof(a2.c_str()), a3.empty() ? 110.0 : std::atof(a3.c_str()), ss)) {
                    std::fprintf(stderr, "script:%d: cannot write %s\n", line, a1.c_str());
                    return false;
                }
            } else if (cmd == "fps-reset") {
                frames0 = m.shell->frames();
                ms0 = m.clock.millis();
            } else if (cmd == "expect-fps") {
                const uint32_t ms = m.clock.millis();
                const double fps = ms != ms0 ? double(m.shell->frames() - frames0) * 1000.0 / double(ms - ms0) : 0.0;
                if (fps < double(n1)) {
                    std::fprintf(stderr, "script:%d: %.1f frames/s < %ld\n", line, fps, n1);
                    return false;
                }
            } else {
                std::fprintf(stderr, "script:%d: unknown command '%s'\n", line, cmd.c_str());
                return false;
            }
        }
        return true;
    }
};

int keyToButton(SDL_Scancode sc, bool padMode) {
    switch (sc) {
        case SDL_SCANCODE_F1: return b::F1;
        case SDL_SCANCODE_F2: return b::F2;
        case SDL_SCANCODE_F3: return b::F3;
        case SDL_SCANCODE_F4: return b::F4;
        case SDL_SCANCODE_F5: return b::F5;
        case SDL_SCANCODE_F6: return b::F6;
        case SDL_SCANCODE_F7: return b::F7;
        case SDL_SCANCODE_F8: return b::F8;
        case SDL_SCANCODE_F9: return b::Pattern;
        case SDL_SCANCODE_F10: return b::Song;
        case SDL_SCANCODE_F11: return b::Aux;
        case SDL_SCANCODE_F12: return b::Help;
        case SDL_SCANCODE_UP: return b::Up;
        case SDL_SCANCODE_DOWN: return b::Down;
        case SDL_SCANCODE_LEFT: return b::Left;
        case SDL_SCANCODE_RIGHT: return b::Right;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return b::Shift;
        case SDL_SCANCODE_SPACE: return b::Play;
        case SDL_SCANCODE_INSERT: return b::Insert;
        case SDL_SCANCODE_DELETE:
        case SDL_SCANCODE_BACKSPACE: return b::Delete;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return b::EncPush;
        default: break;
    }
    if (padMode) return -1;
    switch (sc) {
        case SDL_SCANCODE_R: return b::Rec;
        case SDL_SCANCODE_C: return b::Copy;
        case SDL_SCANCODE_N: return b::Note;
        case SDL_SCANCODE_I: return b::Instr;
        case SDL_SCANCODE_X: return b::Fx1;
        case SDL_SCANCODE_V: return b::Fx2;
        case SDL_SCANCODE_P: return b::Pattern;
        case SDL_SCANCODE_A: return b::Aux;
        case SDL_SCANCODE_H: return b::Help;
        default: break;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return 0x08 + (sc - SDL_SCANCODE_1);
    return -1;
}

int keyToPad(SDL_Scancode sc) {
    static const SDL_Scancode rows[4][12] = {
        {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
         SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS},
        {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y,
         SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
         SDL_SCANCODE_RIGHTBRACKET},
        {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
         SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
         SDL_SCANCODE_BACKSLASH},
        {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N,
         SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_UNKNOWN,
         SDL_SCANCODE_UNKNOWN},
    };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 12; ++c)
            if (rows[r][c] == sc && sc != SDL_SCANCODE_UNKNOWN) return r * 12 + c;
    return -1;
}

int runInteractive(Machine& m, const Options& o) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow(LAB_FW_NAME " - polyend-tracker-lab", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, Panel::kW * o.scale, Panel::kH * o.scale,
                                       SDL_WINDOW_RESIZABLE);
    if (!win) return 1;
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) return 1;
    SDL_RenderSetLogicalSize(ren, Panel::kW, Panel::kH);
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Panel::kW, Panel::kH);
    Panel panel;
    ::sim::Canvas canvas(Panel::kW, Panel::kH);
    std::vector<uint32_t> fb(800 * 480);
    bool padMode = true, quit = false;
    uint64_t latched = 0;
    Control mouseCtl;
    uint32_t lastPresent = 0;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: quit = true; break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    if (ev.key.repeat) break;
                    const bool down = ev.type == SDL_KEYDOWN;
                    const SDL_Scancode sc = ev.key.keysym.scancode;
                    if (down && sc == SDL_SCANCODE_Q && (ev.key.keysym.mod & KMOD_CTRL)) { quit = true; break; }
                    if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_CAPSLOCK) {
                        if (down) padMode = !padMode;
                        break;
                    }
                    if (sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN) {
                        if (down) m.input.encoder(sc == SDL_SCANCODE_PAGEUP ? 1 : -1);
                        break;
                    }
                    const int id = keyToButton(sc, padMode);
                    if (id >= 0) {
                        if (!((latched >> id) & 1)) m.input.button(uint8_t(id), down);
                        break;
                    }
                    if (padMode) {
                        const int p = keyToPad(sc);
                        if (p >= 0) m.input.pad(uint8_t(p), down, 100);
                    }
                    break;
                }
                case SDL_MOUSEWHEEL: {
                    int d = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
                    if (d) m.input.encoder(d);
                    break;
                }
                case SDL_MOUSEBUTTONDOWN: {
                    const Control c = panel.hit(ev.button.x, ev.button.y);
                    if (ev.button.button == SDL_BUTTON_RIGHT) {
                        if (c.kind == Control::kButton) {
                            latched ^= 1ull << c.id;
                            m.input.button(c.id, (latched >> c.id) & 1);
                        }
                        break;
                    }
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    mouseCtl = c;
                    switch (c.kind) {
                        case Control::kButton:
                            if (!((latched >> c.id) & 1)) m.input.button(c.id, true);
                            break;
                        case Control::kPad: m.input.pad(c.id, true, 100); break;
                        case Control::kEncoderCcw: m.input.encoder(-1); break;
                        case Control::kEncoderCw: m.input.encoder(1); break;
                        case Control::kEncoderPush: m.input.button(b::EncPush, true); break;
                        default: break;
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    if (ev.button.button != SDL_BUTTON_LEFT) break;
                    const Control c = mouseCtl;
                    mouseCtl = Control();
                    if (c.kind == Control::kButton && !((latched >> c.id) & 1)) m.input.button(c.id, false);
                    if (c.kind == Control::kPad) m.input.pad(c.id, false);
                    if (c.kind == Control::kEncoderPush) m.input.button(b::EncPush, false);
                    break;
                }
                default: break;
            }
        }
        m.step();
        const uint32_t now = SDL_GetTicks();
        if (o.quitAfterMs >= 0 && long(now) >= o.quitAfterMs) quit = true;
        if (now - lastPresent >= 16) {
            lastPresent = now;
            m.display.frontCopy(fb.data());
            const std::string status = std::string(LAB_FW_NAME "  -  keys: ") +
                                       (padMode ? "PAD mode (letters/digits play pads)" : "COMMAND mode") +
                                       "\nTab toggles; Space = Play, 4 = Config (settings)\nSD card: " +
                                       m.storage.root() + "\nCtrl+Q quits";
            panel.draw(canvas, fb.data(), m.input, m.leds, latched, status);
            SDL_UpdateTexture(tex, nullptr, canvas.pixels(), Panel::kW * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        } else {
            SDL_Delay(1);
        }
    }
    m.shutdown();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;
    const bool headless = !o.script.empty() || !o.screenshot.empty();
    Machine m;
    if (o.busyDisplay) m.display.setBusyClock(&m.clock);
    if (!m.start(o, headless)) return 1;
    if (!headless) {
        if (m.audio) m.audio->start();
        return runInteractive(m, o);
    }
    Headless h{m, o.panelShot};
    if (m.audio) m.audio->start();
    h.advance(300);
    const bool ok = o.script.empty() || h.run(o.script);
    h.advance(50);
    if (!o.screenshot.empty()) saveShot(m, o.screenshot, o.panelShot);
    m.shutdown();
    return ok ? 0 : 1;
}
