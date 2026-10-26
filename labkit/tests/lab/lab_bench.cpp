// polyend-tracker-lab CPU gate: the workstation's render() per audio block, in
// Cortex-M4 instructions under QEMU mps2-an386 (-icount), for every bench
// scenario. tools/lab_gate.py fails the build when a worst block goes over
// the limit (LAB_GATE.md).
//
// LABKIT v2: each block also runs the Shell's LABKIT block (mod sources,
// routes, registry writes) as fill() does, and the "labkit-mod64" line
// measures that block alone with 64 routes on 32 params, 4 LFOs, 2 random
// sources and 8 macros, against its own limit (3,000 instructions).
#include <cmath>
#include <cstdio>

#include "lab/lab.h"

#ifdef OT_BENCH_M4
#define LAB_BIG __attribute__((section(".psram")))
volatile uint32_t* const kTimer = reinterpret_cast<volatile uint32_t*>(0x40000000u);
static void clockStart() {
    kTimer[0] = 0;
    kTimer[2] = 0xFFFFFFFFu;
    kTimer[1] = 0xFFFFFFFFu;
    kTimer[0] = 1;
}
static double nowInsn() { return double(0xFFFFFFFFu - kTimer[1]) * 40.0; }
static const char* kUnit = "M4 insn";
static constexpr int kBlocks = 600;
#else
#include <chrono>
#define LAB_BIG
static void clockStart() {}
static double nowInsn() {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
static const char* kUnit = "host ns";
static constexpr int kBlocks = 3000;
#endif

// Worst block budget for render(): 180,000 M4 instructions = ~67 % of the
// 522,240-cycle block at the bench's 1.94 cycles/instruction, leaving room
// for the shell, line in, DMA and the main loop's interrupts.
static constexpr int kLimit = 180000;
static constexpr int kKitLimit = 3000;

namespace {
// A workstation with params only, for the LABKIT block on its own.
class KitBenchWs final : public lab::Workstation {
public:
    lab::Theme th;
    const char* name() const override { return "KITBENCH"; }
    const char* tagline() const override { return ""; }
    const lab::Theme& theme() const override { return th; }
    bool init(lab::Context&) override { return true; }
    void drawIntro(lab::Canvas&, uint32_t) override {}
    void onEvent(const lab::Event&) override {}
    void draw(lab::Canvas&) override {}
    void leds(lab::Rgb*) override {}
    void render(const lab::Transport&, const lab::AudioIn&, float*, float*) override {}
};
LAB_BIG static uint8_t gKitMem[512u * 1024u];
float gKitVals[32];
int gKitInts[8];

void benchKit(lab::Shell& shell) {
    using namespace lab::kit;
    lab::Arena arena;
    arena.attach(gKitMem, sizeof gKitMem);
    lab::Context ctx;
    ctx.shell = &shell;
    ctx.arena = &arena;
    Kit* k = arena.make<Kit>();
    if (!k || !k->init(shell, ctx, arena, false)) {
        std::printf("lab-bench: labkit init failed\n");
        return;
    }
    Registry& P = k->reg;
    char key[16];
    for (int i = 0; i < 24; ++i) {   // 24 floats: linear and exponential
        std::snprintf(key, sizeof key, "p%d", i);
        gKitVals[i] = 0.5f;
        if (i % 3 == 0) {
            gKitVals[i] = 1000.0f;
            P.add(key, "Exp", &gKitVals[i]).range(20.0f, 20000.0f).exp();
        } else {
            P.add(key, "Lin", &gKitVals[i]).range(0.0f, 1.0f);
        }
    }
    for (int i = 0; i < 8; ++i) {    // 8 ints
        std::snprintf(key, sizeof key, "i%d", i);
        gKitInts[i] = 4;
        P.add(key, "Int", &gKitInts[i]).range(0, 16);
    }
    static KitBenchWs ws;
    k->afterWorkstationInit(&ws);
    // 64 routes: every LFO / RND / macro source onto the 32 params
    for (int s = 0; s < kSrcWs && k->mod.routeCount() < kMaxRoutes; ++s)
        for (int p = 0; p < 32 && k->mod.routeCount() < kMaxRoutes; p += 1 + (s & 1))
            k->mod.route(s, ParamId((p + s * 5) % 32), 0.1f + 0.01f * float(p));
    const int made = k->mod.routeCount();
    for (int i = 0; i < kLfos; ++i) {
        P.set(k->mod.lfoParam(i, 0), float(i));        // sine, tri, saw, square
        P.set(k->mod.lfoParam(i, 2), float(i & 1 ? 4 : 0));   // half of them synced
    }
    P.set(k->mod.rndParam(1, 0), 1.0f);   // one drunk walk
    k->mod.update(false);
    lab::Transport tr;
    tr.playing = true;
    tr.bpm = 174.0f;
    tr.beatsPerSample = 174.0 / 60.0 / 44100.0;
    // the params registered just now are all marked: a few blocks write them
    // (32 a block), like after a preset load -- measured as the burst
    double burst = 0.0;
    for (int b = 0; b < 4; ++b) {
        const double t0 = nowInsn();
        k->audioBlock(tr);
        const double d = nowInsn() - t0;
        if (d > burst) burst = d;
        tr.beat += tr.beatsPerSample * lab::kBlock;
    }
    double sum = 0.0, worst = 0.0;
    for (int b = 0; b < kBlocks; ++b) {
        if ((b & 3) == 0) {   // the user turns things: a few registry writes per block
            P.setNorm(ParamId(b % 32), float(b % 100) * 0.01f, false);
            P.setNorm(k->mod.macroParam(b & 7), float((b * 7) % 100) * 0.01f, false);
        }
        const double t0 = nowInsn();
        k->audioBlock(tr);
        const double d = nowInsn() - t0;
        sum += d;
        if (d > worst) worst = d;
        tr.beat += tr.beatsPerSample * lab::kBlock;
    }
    std::printf("lab.%-14s avg %9.0f  worst %9.0f %s per block, limit %d\n", "labkit-mod64", sum / kBlocks, worst, kUnit,
                kKitLimit);
    std::printf("lab-bench: labkit %d routes, %d params; load burst (32 param writes + routes) worst %.0f %s\n", made,
                P.count(), burst, kUnit);
}
}  // namespace

LAB_BIG static uint8_t gMem[15u * 1024u * 1024u];

int main() {
    clockStart();
    lab::Shell shell;
    lab::Hal h;
    if (!shell.init(h, gMem, sizeof gMem, "BENCH")) {
        std::printf("lab-bench: shell init failed (arena too small?)\n");
        return 1;
    }
    lab::Workstation* ws = shell.workstation();
    std::printf("lab-bench: %s, %d scenario(s), arena %lu KB\n", ws->name(), ws->benchScenarios(),
                static_cast<unsigned long>(shell.context().arena->used() / 1024));
    static int16_t input[lab::kBlock * 2];
    for (uint32_t i = 0; i < lab::kBlock; ++i) {
        input[2 * i] = int16_t(8000.0f * sinf(float(i) * 0.05f));
        input[2 * i + 1] = int16_t(-6000.0f * sinf(float(i) * 0.031f));
    }
    lab::AudioIn in;
    in.frames = input;
    for (int s = 0; s < ws->benchScenarios(); ++s) {
        ws->benchSetup(s);
        lab::Transport tr;
        tr.playing = true;
        tr.bpm = 174.0f;
        tr.beatsPerSample = 174.0 / 60.0 / 44100.0;
        static float l[lab::kBlock], r[lab::kBlock];
        double sum = 0.0, worst = 0.0;
        for (int b = 0; b < kBlocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            const double t0 = nowInsn();
            shell.kitBlock(tr);   // LABKIT: sources, routes, registry writes (as fill() does)
            ws->render(tr, in, l, r);
            const double d = nowInsn() - t0;
            sum += d;
            if (d > worst) worst = d;
            tr.beat += tr.beatsPerSample * lab::kBlock;
            if ((b & 7) == 0) ws->update(uint32_t(b) * 3u);
        }
        std::printf("lab.%-14s avg %9.0f  worst %9.0f %s per block, limit %d\n", ws->benchName(s), sum / kBlocks,
                    worst, kUnit, kLimit);
    }
    benchKit(shell);
    return 0;
}
