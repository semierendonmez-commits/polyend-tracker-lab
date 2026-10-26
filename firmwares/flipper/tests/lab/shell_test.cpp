// The lab Shell with no hardware: the workstation boots, renders finite audio
// in every bench scenario, and draws without a display.
#include <cmath>
#include <vector>

#include "lab/lab.h"
#include "lab_test.h"

namespace {
struct Booted {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    lab::Shell shell;
    bool ok = false;
    Booted() {
        lab::Hal h;   // no display, audio, card, MIDI
        ok = shell.init(h, mem.data(), uint32_t(mem.size()), "TEST");
    }
};
}  // namespace

LAB_TEST(shell_boots_and_renders_every_scenario) {
    Booted b;
    CHECK(b.ok);
    if (!b.ok) return;
    lab::Workstation* ws = b.shell.workstation();
    CHECK(ws != nullptr);
    CHECK(ws->name() && ws->name()[0]);
    for (int s = 0; s < ws->benchScenarios(); ++s) {
        ws->benchSetup(s);
        lab::Transport tr;
        tr.playing = true;
        tr.bpm = 174.0f;
        tr.beatsPerSample = 174.0 / 60.0 / 44100.0;
        lab::AudioIn in;
        float l[lab::kBlock], r[lab::kBlock];
        bool finite = true;
        float peak = 0.0f;
        for (int blk = 0; blk < 3000; ++blk) {   // ~8.7 s
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            ws->render(tr, in, l, r);
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i])) finite = false;
                peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            }
            tr.beat += tr.beatsPerSample * lab::kBlock;
            ws->update(uint32_t(blk * 3));
        }
        CHECK(finite);
        CHECK(peak < 8.0f);   // no runaway feedback
        std::fprintf(stderr, "  scenario %s: peak %.3f\n", ws->benchName(s), double(peak));
    }
}
