// PLATFORM native app example: DRONE -- six detuned oscillator pairs through a
// resonant low-pass, a slow LFO per pair, pads pick the chord.
// Built for the device (.papp) and the simulator (.so) from this one file.
#include <math.h>

#include "platform_app.hpp"

PLAT_APP_INFO("Drone", "PLATFORM SDK", "Native C++ drone: 12 oscillators, filter, LFOs. Pads pick the chord.", "drone",
              64, 0);

namespace {

constexpr int kPairs = 6;
constexpr float kTwoPi = 6.28318531f;

// A small sine table built by a static constructor: exercises .init_array.
struct SineTable {
    float t[257];
    SineTable() {
        for (int i = 0; i <= 256; ++i) t[i] = sinf(kTwoPi * float(i) / 256.0f);
    }
    float at(float ph) const {   // ph in [0, 1)
        const float x = ph * 256.0f;
        const int i = int(x);
        return t[i] + (t[i + 1] - t[i]) * (x - float(i));
    }
};
SineTable gSine;

const int kChords[8][kPairs] = {
    {0, 7, 12, 15, 19, 24}, {0, 5, 12, 17, 19, 24}, {0, 7, 10, 14, 17, 22}, {0, 3, 7, 10, 14, 19},
    {0, 7, 11, 14, 18, 23}, {0, 2, 7, 12, 14, 19}, {0, 5, 10, 15, 20, 25}, {0, 6, 12, 13, 19, 24},
};

}  // namespace

class Drone : public plat::App {
public:
    // state shared main -> audio (plain floats written by the main loop)
    volatile float target_[kPairs] = {};
    // LABKIT params: the registry writes base + modulation here every block
    float cutoff_ = 900.0f, res_ = 2.5f, level_ = 0.0f, detune_ = 0.004f;
    int pid_[3] = {-1, -1, -1};   // cutoff, resonance, detune
    float env_ = 0.0f;            // a mod source: the output level, 0..1
    int chord_ = 0, root_ = 36;
    float ph_[kPairs][2] = {}, lfo_[kPairs] = {}, hz_[kPairs] = {};
    float ic1_ = 0, ic2_ = 0, ic1r_ = 0, ic2r_ = 0;
    float amp_ = 0.0f;
    float scope_[64] = {};
    volatile int scopeW_ = 0;
    int sel_ = 0;   // 0 cutoff 1 res 2 detune 3 root

    bool init() override {
        setChord(0);
        level_ = 0.5f;
        // restore the last settings from the app's data folder
        char path[96];
        snprintf_path(path, "state.bin");
        const int32_t h = host->fileOpen(host->ctx, path, PLAT_FILE_READ);
        if (h >= 0) {
            float v[4];
            if (host->fileRead(host->ctx, h, v, sizeof v) == int32_t(sizeof v)) {
                cutoff_ = v[0];
                res_ = v[1];
                detune_ = v[2];
                root_ = int(v[3]);
            }
            host->fileClose(host->ctx, h);
        }
        // LABKIT: the three sound params on the PARAM pads / MAP (macros, LFO
        // routes, MIDI learn come with them); the output level as a mod source
        pid_[0] = param("drone.cutoff", "Cutoff", &cutoff_, 60.0f, 9000.0f, cutoff_, "Hz", 0, PLAT_PARAM_EXP);
        pid_[1] = param("drone.res", "Reso", &res_, 0.5f, 12.0f, res_, nullptr, 0, 0);
        pid_[2] = param("drone.detune", "Detune", &detune_, 0.0f, 0.03f, detune_, nullptr, 0, 0);
        param("drone.level", "Level", &level_, 0.0f, 1.0f, level_, nullptr, 0, PLAT_PARAM_PERCENT);
        modSource("DRONE", &env_);
        log("drone ready");
        return gSine.t[64] > 0.99f;   // the static constructor ran
    }
    void snprintf_path(char* out, const char* name) {
        // home + "/" + name, without snprintf
        int n = 0;
        for (const char* p = host->home; *p && n < 80; ++p) out[n++] = *p;
        out[n++] = '/';
        for (const char* p = name; *p && n < 94; ++p) out[n++] = *p;
        out[n] = 0;
    }
    void setChord(int c) {
        chord_ = c & 7;
        for (int i = 0; i < kPairs; ++i)
            target_[i] = 440.0f * exp2f((float(root_ + kChords[chord_][i]) - 69.0f) / 12.0f);
    }
    void save() override {
        char path[96];
        snprintf_path(path, "state.bin");
        const int32_t h = host->fileOpen(host->ctx, path, PLAT_FILE_WRITE);
        if (h < 0) return;
        const float v[4] = {cutoff_, res_, detune_, float(root_)};
        host->fileWrite(host->ctx, h, v, sizeof v);
        host->fileClose(host->ctx, h);
    }
    void event(const PlatEvent& e) override {
        if (e.type == PLAT_EV_PAD_DOWN) {
            setChord(e.id % 8);
            root_ = 24 + 12 * (3 - e.id / 12) + (e.id % 12 >= 8 ? (e.id % 12 - 8) * 2 : 0);
            setChord(e.id % 8);
        }
        if (e.type == PLAT_EV_PRESS) {
            if (e.id == PLAT_BTN_UP) sel_ = (sel_ + 3) % 4;
            if (e.id == PLAT_BTN_DOWN) sel_ = (sel_ + 1) % 4;
            if (e.id >= PLAT_BTN_F1 && e.id <= PLAT_BTN_F4) sel_ = e.id - PLAT_BTN_F1;
        }
        if (e.type == PLAT_EV_ENCODER) {
            const float d = float(e.delta) * (e.shift ? 10.0f : 1.0f);
            // through the registry when it has the param (base value; LFOs add on top)
            const float c = pid_[0] >= 0 ? paramGet(pid_[0]) : cutoff_;
            const float q = pid_[1] >= 0 ? paramGet(pid_[1]) : res_;
            const float dt = pid_[2] >= 0 ? paramGet(pid_[2]) : detune_;
            switch (sel_) {
                case 0: setp(0, cutoff_, clamp(c * exp2f(d / 24.0f), 60.0f, 9000.0f)); break;
                case 1: setp(1, res_, clamp(q + d * 0.1f, 0.5f, 12.0f)); break;
                case 2: setp(2, detune_, clamp(dt + d * 0.0005f, 0.0f, 0.03f)); break;
                default:
                    root_ = int(clamp(float(root_) + d, 24.0f, 60.0f));
                    setChord(chord_);
                    break;
            }
        }
    }
    static float clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
    void setp(int i, float& field, float v) {
        if (pid_[i] >= 0) paramSet(pid_[i], v);
        else field = v;
    }
    int navPages(PlatPage* out, int cap) override {
        if (cap < 1) return 0;
        out[0] = PlatPage();
        out[0].name = "DRONE";
        out[0].help = "Pads pick the chord. Cutoff / Reso / Detune / Level take macros and LFOs.";
        out[0].fkeys[0] = "Cutoff";
        out[0].fkeys[1] = "Resonance";
        out[0].fkeys[2] = "Detune";
        out[0].fkeys[3] = "Root";
        return 1;
    }
    int navPage() override { return 0; }

    void render(const PlatTransport&, const int16_t*, float* l, float* r) override {
        const float g = tanf(3.14159265f * clamp(cutoff_, 20.0f, 18000.0f) / 44100.0f);
        const float k = 1.0f / res_;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float det = detune_;
        float sl[PLAT_BLOCK], sr[PLAT_BLOCK];
        for (uint32_t i = 0; i < PLAT_BLOCK; ++i) sl[i] = sr[i] = 0.0f;
        for (int p = 0; p < kPairs; ++p) {
            hz_[p] += (target_[p] - hz_[p]) * 0.02f;
            lfo_[p] += (0.05f + 0.031f * float(p)) * float(PLAT_BLOCK) / 44100.0f;
            if (lfo_[p] >= 1.0f) lfo_[p] -= 1.0f;
            const float wob = 1.0f + 0.002f * gSine.at(lfo_[p]);
            const float inc0 = hz_[p] * wob * (1.0f - det) / 44100.0f;
            const float inc1 = hz_[p] * wob * (1.0f + det) / 44100.0f;
            float p0 = ph_[p][0], p1 = ph_[p][1];
            for (uint32_t i = 0; i < PLAT_BLOCK; ++i) {
                // band-limited enough for a drone: saw from the table's harmonics (sine + soft saw)
                const float s0 = 2.0f * p0 - 1.0f, s1 = 2.0f * p1 - 1.0f;
                sl[i] += s0 * 0.6f + s1 * 0.4f;
                sr[i] += s0 * 0.4f + s1 * 0.6f;
                p0 += inc0;
                if (p0 >= 1.0f) p0 -= 1.0f;
                p1 += inc1;
                if (p1 >= 1.0f) p1 -= 1.0f;
            }
            ph_[p][0] = p0;
            ph_[p][1] = p1;
        }
        const float lv = level_ * 0.06f;
        for (uint32_t i = 0; i < PLAT_BLOCK; ++i) {
            amp_ += (lv - amp_) * 0.0005f;
            float v1 = a1 * ic1_ + g * a1 * (sl[i] - ic2_);
            float v2 = ic2_ + g * v1;
            ic1_ = 2.0f * v1 - ic1_;
            ic2_ = 2.0f * v2 - ic2_;
            const float yl = v2;
            v1 = a1 * ic1r_ + g * a1 * (sr[i] - ic2r_);
            v2 = ic2r_ + g * v1;
            ic1r_ = 2.0f * v1 - ic1r_;
            ic2r_ = 2.0f * v2 - ic2r_;
            l[i] += yl * amp_;
            r[i] += v2 * amp_;
        }
        env_ = clamp(amp_ * 20.0f * (0.5f + res_ * 0.05f), 0.0f, 1.0f);
        // a few points for the scope (the main loop reads them)
        int w = scopeW_;
        for (uint32_t i = 0; i < PLAT_BLOCK; i += 32) {
            scope_[w] = l[i];
            w = (w + 1) & 63;
        }
        scopeW_ = w;
    }

    void draw() override {
        color(0xFFB347);
        text(30, 34, PLAT_FONT_HUGE, "DRONE");
        color(0x808890);
        text(32, 92, PLAT_FONT_SMALL, "native C++ app - built with the PLATFORM SDK");
        static const char* const names[4] = {"cutoff", "resonance", "detune", "root"};
        char buf[32];
        for (int i = 0; i < 4; ++i) {
            const int y = 140 + i * 44;
            color(i == sel_ ? 0x39E6B4 : 0x66707C);
            text(40, y, PLAT_FONT_LARGE, names[i], PLAT_TEXT_CENTERY);
            float v = i == 0 ? cutoff_ : (i == 1 ? res_ : (i == 2 ? detune_ * 1000.0f : float(root_)));
            fmt(buf, v);
            text(330, y, PLAT_FONT_LARGE, buf, PLAT_TEXT_CENTERY | PLAT_TEXT_RIGHT);
            float k = i == 0 ? (log2f(cutoff_ / 60.0f) / log2f(150.0f)) : (i == 1 ? res_ / 12.0f : (i == 2 ? detune_ / 0.03f : (float(root_) - 24.0f) / 36.0f));
            color(0x1A222C);
            fill(350, y - 6, 200, 12);
            color(i == sel_ ? 0x39E6B4 : 0x3A4450);
            fill(350, y - 6, int(200.0f * clamp(k, 0.0f, 1.0f)), 12);
        }
        // scope
        color(0xFFB347);
        host->lineWidth(host->ctx, 2.0f);
        host->beginBatch(host->ctx, PLAT_BATCH_STRIP);
        const int w0 = scopeW_;
        for (int i = 0; i < 64; ++i) host->vtx(host->ctx, 590.0f + float(i) * 3.0f, 220.0f - scope_[(w0 + i) & 63] * 300.0f);
        host->endBatch(host->ctx);
        host->lineWidth(host->ctx, 1.0f);
        // chord rings
        for (int p = 0; p < kPairs; ++p) {
            color(0x39E6B4);
            ring(400.0f + float(p - 3) * 60.0f + 30.0f, 380.0f, 8.0f + hz_[p] * 0.04f, 20);
        }
        color(0x66707C);
        text(400, 440, PLAT_FONT_TINY, "pads: chord + root   Up/Down or F1-F4: parameter   encoder: change (Shift x10)",
             PLAT_TEXT_CENTER);
    }
    static void fmt(char* out, float v) {
        // tiny float formatter (no printf in apps): one decimal
        int n = 0;
        if (v < 0) {
            out[n++] = '-';
            v = -v;
        }
        const int iv = int(v * 10.0f + 0.5f);
        char tmp[12];
        int t = 0;
        int ip = iv / 10;
        do {
            tmp[t++] = char('0' + ip % 10);
            ip /= 10;
        } while (ip && t < 10);
        while (t) out[n++] = tmp[--t];
        out[n++] = '.';
        out[n++] = char('0' + iv % 10);
        out[n] = 0;
    }
    void leds(uint8_t* rgb) override {
        for (int i = 0; i < PLAT_PADS; ++i) {
            const bool on = (i % 8) == chord_;
            rgb[3 * i + 0] = on ? 255 : ((i % 12) < 8 ? 30 : 6);
            rgb[3 * i + 1] = on ? 179 : ((i % 12) < 8 ? 20 : 6);
            rgb[3 * i + 2] = on ? 71 : 6;
        }
    }
};

PLAT_APP(Drone)
