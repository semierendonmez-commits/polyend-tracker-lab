// polyend-tracker-lab CPU gate: the workstation's render() per audio block, in
// Cortex-M4 instructions under QEMU mps2-an386 (-icount), for every bench
// scenario. tools/lab_gate.py fails the build when a worst block goes over
// the limit (LAB_GATE.md).
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
    return 0;
}
