// Template workstation: a sine per pad. Each firmware replaces src/ws/.
#include <cmath>

#include "lab/dsp.h"
#include "lab/lab.h"

namespace lab {
namespace {

class Hello final : public Workstation {
public:
    const char* name() const override { return "LAB"; }
    const char* tagline() const override { return "Template workstation"; }
    const Theme& theme() const override { return theme_; }
    bool init(Context& c) override {
        ctx_ = &c;
        return true;
    }
    void drawIntro(Canvas& g, uint32_t t) override {
        const float k = float(t) / float(introMs());
        g.color(mix(theme_.bg, theme_.accent, k));
        g.text(400, 240, font::kHuge, "LAB", Canvas::kCenter);
    }
    void onEvent(const Event& e) override {
        if (e.type == Event::kPadDown) {
            gate_[e.id] = true;
            ctx_->midiNote(true, uint8_t(36 + e.id), e.velocity);
        }
        if (e.type == Event::kPadUp) {
            gate_[e.id] = false;
            ctx_->midiNote(false, uint8_t(36 + e.id), 0);
        }
    }
    void draw(Canvas& g) override {
        drawHeader(g, theme_, *ctx_->shell, "Template");
        for (int i = 0; i < kPads; ++i) {
            g.color(gate_[i] ? theme_.accent : theme_.panel);
            g.fill(40 + padCol(i) * 60, 100 + padRow(i) * 60, 54, 54);
        }
        const char* keys[8] = {"", "", "", "", "", "", "", ""};
        drawFKeyBar(g, theme_, keys);
    }
    void leds(Rgb* pads) override {
        for (int i = 0; i < kPads; ++i) pads[i] = gate_[i] ? theme_.accent : Rgb(0x080808);
    }
    void render(const Transport&, const AudioIn&, float* l, float* r) override {
        for (int p = 0; p < kPads; ++p) {
            float target = gate_[p] ? 0.15f : 0.0f;
            if (!gate_[p] && amp_[p] < 1e-4f) continue;
            const float inc = dsp::mtof(float(36 + p)) * dsp::kInvSr;
            for (uint32_t i = 0; i < kBlock; ++i) {
                amp_[p] += (target - amp_[p]) * 0.002f;
                ph_[p] += inc;
                if (ph_[p] >= 1.0f) ph_[p] -= 1.0f;
                const float s = sinf(dsp::kTwoPi * ph_[p]) * amp_[p];
                l[i] += s;
                r[i] += s;
            }
        }
    }

private:
    Context* ctx_ = nullptr;
    Theme theme_;
    volatile bool gate_[kPads] = {};
    float amp_[kPads] = {}, ph_[kPads] = {};
};

}  // namespace

Workstation* createWorkstation(Arena& arena) { return arena.make<Hello>(); }

}  // namespace lab
