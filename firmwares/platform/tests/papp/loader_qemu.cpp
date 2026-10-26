// PLATFORM loader test under QEMU mps2-an386 (Cortex-M4F): the real
// NativeSession / pappPlace code loads tests/papp/app (built by the SDK into a
// .papp, embedded here as a byte array behind an in-memory IStorage) into the
// 16 MB PSRAM at 0x21000000 (the stand-in for the device's SDRAM at
// 0x70000000), relocates it, synchronises (DSB/ISB), runs its constructors,
// calls it through the PlatApp table (init, event, render, draw, leds) and
// checks the answers. The image is loaded twice, at two different addresses,
// and executed from both. The fast segment goes into an SRAM buffer.
// Output: "papp-qemu: PASS" / "FAIL ..." on semihosting; exit code 0 / 1.
//   make -C tests papp-qemu
#include <cstdio>
#include <cstring>

#include "lab/lab.h"
#include "ws/budget_canvas.h"
#include "ws/native.h"
#include "ws/papp.h"

extern "C" const unsigned char gTestPapp[];
extern "C" const unsigned int gTestPappSize;
// Optional (make papp-qemu STATION=build/stations/flipper.papp): a real station
// image -- loaded, relocated and run up to its init() on a fake card.
extern "C" const unsigned char gStationPapp[] __attribute__((weak));
extern "C" const unsigned int gStationPappSize __attribute__((weak));

namespace {

int gFails = 0;
#define EXPECT(c)                                                       \
    do {                                                                \
        if (!(c)) {                                                     \
            std::printf("papp-qemu: FAIL line %d: %s\n", __LINE__, #c); \
            ++gFails;                                                   \
        }                                                               \
    } while (0)

// IStorage with one file in memory.
class OneFile final : public ot::IStorage {
public:
    const uint8_t* data = nullptr;
    uint32_t size_ = 0, pos = 0;
    Handle open(const char* path, OpenMode m) override {
        if (m != kRead) return std::strstr(path, "Data") ? 7 : -1;
        if (!std::strstr(path, ".papp")) return -1;
        pos = 0;
        return 1;
    }
    int32_t read(Handle h, void* d, uint32_t n) override {
        if (h != 1) return -1;
        const uint32_t k = n < size_ - pos ? n : size_ - pos;
        std::memcpy(d, data + pos, k);
        pos += k;
        return int32_t(k);
    }
    int32_t write(Handle, const void*, uint32_t n) override { return int32_t(n); }
    bool seek(Handle, uint32_t p) override { pos = p; return true; }
    int32_t size(Handle h) override { return h == 1 ? int32_t(size_) : 0; }
    void close(Handle) override {}
    bool exists(const char*) override { return true; }
    bool mkdir(const char*) override { return true; }
    bool remove(const char*) override { return true; }
    bool rename(const char*, const char*) override { return true; }
    int32_t listDir(const char*, DirFn, void*) override { return 0; }
};

// IDisplay that counts what reaches it.
class CountDisplay final : public ot::IDisplay {
public:
    int rects = 0, texts = 0;
    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override {}
    void clearColorRGB(uint8_t, uint8_t, uint8_t) override {}
    void clear(bool, bool, bool) override {}
    void colorRGB(uint8_t, uint8_t, uint8_t) override {}
    void colorA(uint8_t) override {}
    void lineWidth(uint16_t) override {}
    void pointSize(uint16_t) override {}
    void scissorXY(uint16_t, uint16_t) override {}
    void scissorSize(uint16_t, uint16_t) override {}
    void begin(Prim p) override { rects += p == kRects; }
    void end() override {}
    void vertex2f(int32_t, int32_t) override {}
    void vertex2ii(uint16_t, uint16_t, uint8_t, uint8_t) override {}
    void text(int16_t, int16_t, uint8_t, uint16_t, const char*) override { ++texts; }
    void number(int16_t, int16_t, uint8_t, uint16_t, int32_t) override {}
    void swap() override {}
};

__attribute__((section(".psram"))) uint8_t gPsram[14u << 20];   // the "SDRAM" (16 MB PSRAM region)

}  // namespace

// The parts of lab::Shell the session touches (Context services): a Shell
// object is never initialised here, so Context gets a static stand-in.
int main() {
    std::printf("papp-qemu: image %u bytes\n", gTestPappSize);
    plat::PappHeader h;
    EXPECT(plat::pappParseHeader(gTestPapp, gTestPappSize, h) == plat::PappStatus::Ok);
    EXPECT(h.fastSize > 0);
    EXPECT(h.relocCount > 10);
    // a corrupt copy is refused by the CRC (the streaming loader checks it too, below)
    {
        static uint8_t bad[32768];
        if (gTestPappSize <= sizeof bad) {
            std::memcpy(bad, gTestPapp, gTestPappSize);
            bad[gTestPappSize - 9] ^= 0x40;
            plat::PappHeader hb;
            EXPECT(plat::pappParseHeader(bad, gTestPappSize, hb) == plat::PappStatus::Ok);
            EXPECT(plat::pappPlace(bad, gTestPappSize, hb, gPsram, gPsram + 65536, uint32_t(uintptr_t(gPsram)),
                                   uint32_t(uintptr_t(gPsram + 65536))) == plat::PappStatus::BadCrc);
        }
    }

    OneFile fs;
    fs.data = gTestPapp;
    fs.size_ = gTestPappSize;
    lab::Shell shell;   // not initialised: only fwName()/millis() style calls are made
    lab::Context ctx;
    ctx.shell = &shell;
    ctx.fs = &fs;
    ctx.home = "Lab/PLATFORM";
    CountDisplay disp;
    lab::Canvas canvas;
    canvas.attach(&disp);

    for (int round = 0; round < 2; ++round) {
        // load at two different PSRAM addresses (the second shifted by 1 MB + 136 B)
        plat::Pool pool;
        const uint32_t off = round ? (1u << 20) + 136u : 0u;
        pool.attach(gPsram + off, sizeof gPsram - off);
        plat::NativeSession s;
        const bool ok = s.open(pool, ctx, "Lab/PLATFORM/Apps/papp.papp", "Lab/PLATFORM/Data/papp");
        if (!ok) std::printf("papp-qemu: open failed: %s\n", s.error());
        EXPECT(ok);
        if (!ok) continue;
        EXPECT(std::strcmp(s.info().name, "LoaderTest") == 0);
        // the loader's staging buffers were released: only the image (+ bss) stays in the pool
        EXPECT(pool.used() <= h.imageSize + h.bssSize + h.imageAlign + 64u);
        EXPECT(s.fastBytes() == h.fastSize);
        // init computed: 1234 + 4 + 12 + 0 + 1000 = 2250
        uint8_t rgb[48 * 3] = {};
        s.leds(reinterpret_cast<lab::Rgb*>(rgb));
        lab::Rgb pads[48];
        s.leds(pads);
        EXPECT(pads[0].r == uint8_t(2250 & 255));
        // events
        lab::Event e;
        e.type = lab::Event::kPadDown;
        e.id = 3;
        s.event(e);
        // render: fast code adds a ramp
        float l[lab::kBlock] = {}, r[lab::kBlock] = {};
        lab::Transport t;
        lab::AudioIn in;
        s.render(t, in, l, r);
        EXPECT(l[0] < -0.079f && l[0] > -0.081f);
        EXPECT(r[15] < -0.069f && r[15] > -0.071f);
        // draw through the budgeted canvas
        canvas.begin(lab::Rgb());
        plat::BudgetCanvas bc(canvas, 1600);
        s.draw(canvas, bc);
        canvas.end();
        EXPECT(disp.texts >= 1 + round);
        EXPECT(disp.rects >= 1 + round);
        if (round == 1) {   // a damaged file on the card: refused, nothing runs
            static uint8_t bad[32768];
            std::memcpy(bad, gTestPapp, gTestPappSize);
            bad[gTestPappSize - 9] ^= 0x40;
            fs.data = bad;
            plat::Pool p2;
            p2.attach(gPsram + (2u << 20), 1u << 20);
            plat::NativeSession s2;
            EXPECT(!s2.open(p2, ctx, "Lab/PLATFORM/Apps/papp.papp", "Lab/PLATFORM/Data/papp"));
            EXPECT(std::strstr(s2.error(), "checksum") != nullptr);
            fs.data = gTestPapp;
        }
        std::printf("papp-qemu: round %d loaded at %p, fast in %s, leds[0]=%u\n", round, static_cast<void*>(gPsram + off),
                    s.fastInSram() ? "SRAM" : "PSRAM", unsigned(pads[0].r));
        s.close();
    }
    if (&gStationPappSize && gStationPappSize) {
        plat::PappHeader hs;
        EXPECT(plat::pappParseHeader(gStationPapp, gStationPappSize, hs) == plat::PappStatus::Ok);
        fs.data = gStationPapp;
        fs.size_ = gStationPappSize;
        plat::Pool pool;
        pool.attach(gPsram, sizeof gPsram);
        plat::NativeSession s;
        // The whole workstation: load, relocate, constructors, init() on the
        // fake card, then render audio blocks with the transport playing and
        // draw frames through the budgeted canvas -- all on the M4 from PSRAM.
        const bool ok = s.open(pool, ctx, "Lab/PLATFORM/Apps/station.papp", "Lab/PLATFORM/Data/station");
        std::printf("papp-qemu: station %s: %u B image, %u relocations, init %s %s\n", hs.info.name,
                    unsigned(hs.imageSize + hs.bssSize), unsigned(hs.relocCount), ok ? "ok" : "FAILED", ok ? "" : s.error());
        EXPECT(ok);
        if (ok) {
            s.transport(true);
            lab::Transport t;
            t.playing = true;
            t.startedThisBlock = true;
            t.bpm = 120.0f;
            t.beatsPerSample = 120.0 / 60.0 / 44100.0;
            float peak = 0.0f;
            bool finite = true;
            volatile uint32_t* const timer = reinterpret_cast<volatile uint32_t*>(0x40000000u);
            timer[0] = 0;
            timer[2] = 0xFFFFFFFFu;
            timer[1] = 0xFFFFFFFFu;
            timer[0] = 1;
            double worst = 0.0, sum = 0.0;
            for (int b = 0; b < 400; ++b) {
                float l[lab::kBlock] = {}, r[lab::kBlock] = {};
                const double t0 = double(0xFFFFFFFFu - timer[1]) * 40.0;
                s.render(t, lab::AudioIn(), l, r);
                const double d = double(0xFFFFFFFFu - timer[1]) * 40.0 - t0;
                sum += d;
                if (d > worst) worst = d;
                for (uint32_t i = 0; i < lab::kBlock; ++i) {
                    const float a = l[i] < 0 ? -l[i] : l[i];
                    if (!(a < 1e6f)) finite = false;
                    if (a > peak) peak = a;
                }
                t.beat += t.beatsPerSample * lab::kBlock;
                t.startedThisBlock = false;
                if ((b & 7) == 0) s.update(uint32_t(b) * 3u);
            }
            canvas.begin(lab::Rgb());
            plat::BudgetCanvas bc(canvas, 1680);
            const int texts0 = disp.texts;
            s.draw(canvas, bc);
            canvas.end();
            std::printf("papp-qemu: station %s rendered 400 blocks (avg %.0f, worst %.0f M4 insn), peak %.3f, drew %d texts in %u words\n",
                        hs.info.name, sum / 400.0, worst, double(peak), disp.texts - texts0, unsigned(canvas.words()));
            EXPECT(finite);
            EXPECT(disp.texts > texts0);
        }
        s.close();
    }
    std::printf("papp-qemu: %s (%d failures)\n", gFails ? "FAIL" : "PASS", gFails);
    return gFails ? 1 : 0;
}
