// PLATFORM hosted bench (make lab-gate): a station .papp running inside
// PLATFORM under QEMU mps2-an386 (-icount). The real Shell + PLATFORM boot on
// a fake card that holds the station file (read from the host through
// semihosting: build/perf/hosted_station.papp), the launcher code opens it
// (loader, relocation, init), and each of the station's own bench scenarios
// (Workstation::benchScenarios / benchSetup, through PlatApp) runs 600 audio
// blocks through Platform::render -- the voice engine (idle) + the station.
// Output lines "lab.<STATION>.<scenario> avg N worst N M4 insn per block,
// limit 180000" are checked by tools/lab_gate.py like the firmware's own bench.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/lab.h"
#include "ws/platform.h"

namespace {

volatile uint32_t* const kTimer = reinterpret_cast<volatile uint32_t*>(0x40000000u);
double nowInsn() { return double(0xFFFFFFFFu - kTimer[1]) * 40.0; }
constexpr int kBlocks = 600;
constexpr int kLimit = 180000;

uint8_t gFile[1u << 20];   // the station image (RAM region)
uint32_t gFileSize = 0;
char gFileName[48] = "station.papp";
__attribute__((section(".psram"))) uint8_t gMem[0x00FD9000u];   // the device's SDRAM arena size

// A card with one file in Lab/PLATFORM/Apps; everything else is empty, writes are dropped.
class Card final : public ot::IStorage {
public:
    uint32_t pos = 0;
    bool isApp(const char* p) const {
        const char* s = std::strrchr(p, '/');
        return s && !std::strcmp(s + 1, gFileName) && std::strstr(p, "Lab/PLATFORM/Apps/");
    }
    Handle open(const char* path, OpenMode m) override {
        if (m == kRead) {
            if (!isApp(path)) return -1;
            pos = 0;
            return 1;
        }
        return 2;   // write sink
    }
    int32_t read(Handle h, void* d, uint32_t n) override {
        if (h != 1) return -1;
        const uint32_t k = n < gFileSize - pos ? n : gFileSize - pos;
        std::memcpy(d, gFile + pos, k);
        pos += k;
        return int32_t(k);
    }
    int32_t write(Handle, const void*, uint32_t n) override { return int32_t(n); }
    bool seek(Handle h, uint32_t p) override {
        if (h == 1) pos = p < gFileSize ? p : gFileSize;
        return true;
    }
    int32_t size(Handle h) override { return h == 1 ? int32_t(gFileSize) : 0; }
    void close(Handle) override {}
    bool exists(const char* p) override { return isApp(p) || !std::strcmp(p, "Lab/PLATFORM/Scripts"); }
    bool mkdir(const char*) override { return true; }
    bool remove(const char*) override { return true; }
    bool rename(const char*, const char*) override { return true; }
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        if (std::strcmp(path, "Lab/PLATFORM/Apps") != 0) return 0;
        DirEntry e{gFileName, false, gFileSize};
        fn(e, ctx);
        return 1;
    }
};
Card gCard;

}  // namespace

int main() {
    kTimer[0] = 0;
    kTimer[2] = 0xFFFFFFFFu;
    kTimer[1] = 0xFFFFFFFFu;
    kTimer[0] = 1;
    FILE* f = std::fopen("build/perf/hosted_station.papp", "rb");
    if (!f) {
        std::printf("hosted-bench: FAIL cannot read build/perf/hosted_station.papp\n");
        return 1;
    }
    gFileSize = uint32_t(std::fread(gFile, 1, sizeof gFile, f));
    std::fclose(f);

    lab::Shell shell;
    lab::Hal h;
    h.fs = &gCard;
    if (!shell.init(h, gMem, sizeof gMem, "PLATFORM")) {
        std::printf("hosted-bench: FAIL shell init\n");
        return 1;
    }
    plat::Platform* p = static_cast<plat::Platform*>(shell.workstation());
    p->update(1);
    const bool ok = p->openByName(gFileName) && p->mode() == plat::Platform::kAppRun;
    const char* name = p->native().info().name;
    if (!ok) {
        std::printf("hosted-bench: FAIL %s did not open: %s\n", name, p->native().error());
        return 1;
    }
    const int n = p->native().benchScenarios();
    std::printf("hosted-bench: %s in PLATFORM: image %lu KB (fast %lu B in %s), session pool %lu KB, %d scenario(s)\n", name,
                static_cast<unsigned long>(p->native().imageBytes() / 1024), static_cast<unsigned long>(p->native().fastBytes()),
                p->native().fastInSram() ? "SRAM" : "SDRAM", static_cast<unsigned long>(p->poolBytes() / 1024), n);
    static int16_t input[lab::kBlock * 2];
    for (uint32_t i = 0; i < lab::kBlock; ++i) {
        input[2 * i] = int16_t(8000.0f * sinf(float(i) * 0.05f));
        input[2 * i + 1] = int16_t(-6000.0f * sinf(float(i) * 0.031f));
    }
    lab::AudioIn in;
    in.frames = input;
    for (int s = 0; s < n; ++s) {
        p->native().benchSetup(s);
        lab::Transport tr;
        tr.playing = true;
        tr.bpm = 174.0f;
        tr.beatsPerSample = 174.0 / 60.0 / 44100.0;
        static float l[lab::kBlock], r[lab::kBlock];
        double sum = 0.0, worst = 0.0;
        bool finite = true;
        for (int b = 0; b < kBlocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            const double t0 = nowInsn();
            p->render(tr, in, l, r);
            const double d = nowInsn() - t0;
            sum += d;
            if (d > worst) worst = d;
            for (uint32_t i = 0; i < lab::kBlock; ++i)
                if (!(std::fabs(l[i]) < 1e6f)) finite = false;
            tr.beat += tr.beatsPerSample * lab::kBlock;
            if ((b & 7) == 0) p->update(uint32_t(b) * 3u + 2u);
        }
        std::printf("lab.%s.%-14s avg %9.0f  worst %9.0f M4 insn per block, limit %d%s\n", name, p->native().benchName(s),
                    sum / kBlocks, worst, kLimit, finite ? "" : " (NON-FINITE OUTPUT)");
    }
    return 0;
}
