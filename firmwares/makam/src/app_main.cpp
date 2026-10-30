// polyend-tracker-lab device glue: hands the HAL drivers and the SDRAM to the
// lab Shell (src/lab/), which runs this firmware's workstation (src/ws/).
// The OpenTracker tracker App is not linked into a lab image.
#include <cstdio>
#include <cstring>

#include "hal/audio_sgtl5000.h"
#include "hal/board.h"
#include "hal/diag.h"
#include "hal/ft81x.h"
#include "hal/fw_update.h"
#include "hal/hal.h"
#include "hal/radio_si4703.h"
#include "hal/sd_storage.h"
#include "hal/sd_stream_reader.h"
#include "hal/stack_guard.h"
#include "hal/telemetry.h"
#include "lab/lab.h"

#ifndef LAB_FW_NAME
#define LAB_FW_NAME "LAB"
#endif

extern "C" uint8_t _ssdram_bss[], _esdram_bss[];   // src/hal/opentracker.ld

namespace {

constexpr float kHeadphoneLevel = 0.715f;   // OpenTracker's boot level (core/ui/ui.h)

lab::Shell gShell;
void (*gHalService)() = nullptr;
uint32_t gCpuPermille = 0, gLastCpuPoll = 0;

uint32_t cpuPermille() { return gCpuPermille; }

void service() {
    if (gHalService) gHalService();
    const uint32_t now = ot::hal::halSet().clock->millis();
    if (now - gLastCpuPoll >= 500) {
        gLastCpuPoll = now;
        uint32_t mn = 0, avg = 0, mx = 0;
        if (ot::hal::audio().renderCyclesWindow(mn, avg, mx))
            gCpuPermille = uint32_t(uint64_t(mx) * 1000u / ot::diag::kRenderBudgetCycles);
    }
}

void setBacklight(uint8_t duty) { ot::hal::halSet().display->setBacklight(duty); }

bool selectInputThunk(void* ctx, uint8_t source, uint8_t gain) { return ot::hal::selectInput(ctx, source, gain); }

void powerOffHook() { gShell.powerOff(); }

}  // namespace

void otAppMain(ot::hal::HalSet& hal) {
    using namespace ot;
    if (!hal::bootReport().sdramOk) return;
    hal::audio().setHeadphoneVolume(kHeadphoneLevel);
    // `.sdram_bss` (core/sdram_bss.h): NOLOAD, zeroed before any app code.
    std::memset(_ssdram_bss, 0, size_t(_esdram_bss - _ssdram_bss));
    hal::storage().enableSdramFiles();

    // The whole SDRAM below `.sdram_bss` is the lab arena.
    uint8_t* sdram = reinterpret_cast<uint8_t*>(board::kSdramBase);
    const uint32_t arenaBytes = board::kSdramBssBase - board::kSdramBase;

    lab::Hal h;
    h.display = hal.display;
    h.audio = hal.audio;
    h.fs = hal::bootReport().sdMounted ? hal.storage : nullptr;
    h.midi = hal.midi;
    h.input = hal.input;
    h.leds = hal.leds;
    h.clock = hal.clock;
    h.radio = &hal::radio();
    gHalService = hal.service;
    h.service = &service;
    h.isUpdateImageName = &hal::isUpdateImageName;
    h.requestFirmwareUpdate = &hal::requestFirmwareUpdate;
    h.lineIn = &hal::lineIn;
    h.selectInput = &selectInputThunk;
    h.setBacklight = &setBacklight;
    h.cpuPermille = &cpuPermille;
    // SD streaming: upstream's device reader (raw sectors through the file's
    // cluster map). A function-local static: its constructor clears the
    // bindings in .sdram_bss here, after the SDRAM is up (tools/check_ctors.py).
    if (h.fs) {
        static hal::SdStreamReader streamReader(hal::storage());
        h.streamReader = &streamReader;
    }
    if (!gShell.init(h, sdram, arenaBytes, LAB_FW_NAME)) {
        hal::diagLog("lab: shell init failed");
        return;   // falls back to the selftest
    }
    hal::setPowerOffHook(&powerOffHook);
    // Stack guard (upstream #101/#106): an overflow resets the Tracker; say so
    // on screen so it never looks like a random reboot.
    if (hal::lastResetWasLockup())
        gShell.toast(hal::stackGuardArmed() ? "Restarted after a crash (stack overflow)" : "Restarted after a crash");
    hal::diagLogf("lab: %s running, arena %lu KB used", LAB_FW_NAME,
                  static_cast<unsigned long>(gShell.context().arena->used() / 1024));
    gShell.run();
}
