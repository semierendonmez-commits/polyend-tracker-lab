// Loader test app (tests/papp): exercises every relocation path of a .papp --
// a static constructor (.init_array), data with absolute pointers (vtables,
// function-pointer tables, string tables), a .fast segment (PLAT_FAST) that
// calls back into the main segment and into libm (veneers both ways), and
// the host table. The QEMU test and the host test check its answers.
#include <math.h>

#include "platform_app.hpp"

PLAT_APP_INFO("LoaderTest", "PLATFORM tests", "relocation test app", "app", 16, 0);

namespace {

int gCtorRuns = 0;
struct Ctor {
    int v;
    Ctor() : v(1234) { ++gCtorRuns; }
};
Ctor gCtor;

const char* const kWords[] = {"alpha", "beta", "gamma"};   // absolute pointers in data

int mainSide(int x) { return x * 3 + 1; }

PLAT_FAST int fastAdd(int a, int b) { return mainSide(a) + b; }        // fast -> main veneer
PLAT_FAST float fastSine(float x) { return sinf(x) * 2.0f; }          // fast -> libm veneer
PLAT_FAST void fastRender(float* l, float* r, float g) {
    for (uint32_t i = 0; i < PLAT_BLOCK; ++i) {
        l[i] += g * float(int(i % 16) - 8) * 0.01f;
        r[i] -= g * float(int(i % 16) - 8) * 0.01f;
    }
}
int (*const kOps[])(int, int) = {fastAdd};                            // data -> fast pointer

}  // namespace

class LoaderTest : public plat::App {
public:
    int answer = 0;
    uint32_t events = 0;
    bool init() override {
        // 1234 (ctor) + 4 (len "beta") + fastAdd(2, 5) = 3*2+1+5 = 12 + fastSine ~0
        int len = 0;
        for (const char* p = kWords[1]; *p; ++p) ++len;
        answer = gCtor.v + len + kOps[0](2, 5) + int(fastSine(0.0f)) + gCtorRuns * 1000;
        log("loader test: init");
        return gCtorRuns == 1;
    }
    void event(const PlatEvent& e) override { events += e.type == PLAT_EV_PAD_DOWN ? 1u : 0u; }
    void render(const PlatTransport&, const int16_t*, float* l, float* r) override { fastRender(l, r, 1.0f); }
    void draw() override {
        color(0xFFFFFF);
        text(10, 10, PLAT_FONT_BODY, kWords[answer == 2250 ? 0 : 2]);
        fill(1, 2, 3, 4);
    }
    void leds(uint8_t* rgb) override { rgb[0] = uint8_t(answer & 255); }
};

PLAT_APP(LoaderTest)

// Read by the tests through the app table: the computed answer.
extern "C" __attribute__((used)) int plat_test_answer(void) { return plat::AppGlue<LoaderTest>::obj().answer; }
