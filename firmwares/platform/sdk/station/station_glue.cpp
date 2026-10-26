// PLATFORM station glue: runs an unchanged lab::Workstation (a firmware's
// src/ws/, with createWorkstation()) as a PLATFORM native app.
//
// The app is linked with that workstation's sources, src/lab/canvas.cpp and
// src/lab/shell.cpp (only the Context services, drawHeader / drawFKeyBar and
// the Canvas survive --gc-sections) and the core sources it uses. It reaches
// the host's own Shell, Canvas, IStorage and IRadio objects through
// PlatHost::labShell / labCanvas / labStorage / labRadio: inline members and
// virtual calls only use the object layouts, which PlatHost::labLayout checks
// against this build (src/ws/lab_layout.h). Settings rows pass straight through
// (PlatSetting mirrors lab::SettingRow).
//
// Define in the app (station.mk):  -DPLAT_STATION_NAME="\"GRAF\""
//                                  -DPLAT_STATION_MEMKB=<arena KB the station needs at least>
//                                  -DPLAT_STATION_HOME="\"Lab/GRAF\""   (its card folder)
#include <cstring>

#include "lab/lab.h"
#include "platform_app.h"
#include "ws/lab_layout.h"

#ifndef PLAT_STATION_NAME
#define PLAT_STATION_NAME "STATION"
#endif
#ifndef PLAT_STATION_DESC
#define PLAT_STATION_DESC "A polyend-tracker-lab workstation as a PLATFORM app"
#endif
#ifndef PLAT_STATION_MEMKB
#define PLAT_STATION_MEMKB 4096
#endif
#ifndef PLAT_STATION_HOME
#define PLAT_STATION_HOME "Lab/" PLAT_STATION_NAME
#endif

PLAT_APP_INFO(PLAT_STATION_NAME, "polyend-tracker-lab", PLAT_STATION_DESC, "station", PLAT_STATION_MEMKB,
              PLAT_INFO_WORKSTATION);

namespace {

struct Station {
    const PlatHost* host = nullptr;
    lab::Arena arena;
    lab::Context ctx;
    lab::Workstation* ws = nullptr;
    char home[48] = PLAT_STATION_HOME;
};
Station gS;

lab::Canvas& canvas() { return *static_cast<lab::Canvas*>(gS.host->labCanvas); }

bool init(void*, const PlatHost* h) {
    if (!h || h->abi != PLAT_ABI_VERSION || h->size < sizeof(PlatHost)) return false;
    gS.host = h;
    if (h->labLayout != plat::kLabLayout || !h->labShell) {
        h->log(h->ctx, "station: built against other lab.h sources (layout mismatch)");
        return false;
    }
    // The station's arena: all the memory left in PLATFORM's session pool (a
    // firmware's arena is the whole SDRAM; workstations take what they need).
    const uint32_t freeBytes = h->memFree(h->ctx);
    const uint32_t bytes = freeBytes > 4096u ? (freeBytes - 4096u) & ~63u : 0u;
    void* mem = bytes >= uint32_t(PLAT_STATION_MEMKB) * 1024u ? h->alloc(h->ctx, bytes, 64) : nullptr;
    if (!mem) {
        h->log(h->ctx, "station: not enough memory");
        return false;
    }
    gS.arena.attach(static_cast<uint8_t*>(mem), bytes);
    gS.ctx.shell = static_cast<lab::Shell*>(h->labShell);
    gS.ctx.arena = &gS.arena;
    gS.ctx.fs = static_cast<ot::IStorage*>(h->labStorage);
    gS.ctx.radio = static_cast<ot::IRadio*>(h->labRadio);
    gS.ctx.home = gS.home;
    if (gS.ctx.fs) {
        gS.ctx.fs->mkdir("Lab");
        gS.ctx.fs->mkdir(gS.home);
    }
    gS.ws = lab::createWorkstation(gS.arena);
    if (!gS.ws) return false;
    gS.ctx.theme = &gS.ws->theme();
    return gS.ws->init(gS.ctx);
}

void drawIntro(void*, uint32_t ms) { gS.ws->drawIntro(canvas(), ms); }
void event(void*, const PlatEvent* pe) {
    lab::Event e;
    e.type = lab::Event::Type(pe->type);
    e.id = pe->id;
    e.velocity = pe->velocity;
    e.delta = pe->delta;
    e.shift = pe->shift != 0;
    e.ms = pe->ms;
    gS.ws->onEvent(e);
}
void midi(void*, uint8_t s, uint8_t a, uint8_t b) {
    lab::MidiMsg m;
    m.status = s;
    m.d1 = a;
    m.d2 = b;
    gS.ws->onMidi(m);
}
void update(void*, uint32_t ms) { gS.ws->update(ms); }
void draw(void*) { gS.ws->draw(canvas()); }
void leds(void*, uint8_t* rgb) {
    lab::Rgb pads[lab::kPads];
    gS.ws->leds(pads);
    for (int i = 0; i < lab::kPads; ++i) {
        rgb[3 * i] = pads[i].r;
        rgb[3 * i + 1] = pads[i].g;
        rgb[3 * i + 2] = pads[i].b;
    }
}
void render(void*, const PlatTransport* pt, const int16_t* in, float* l, float* r) {
    lab::Transport t;
    t.playing = pt->playing != 0;
    t.bpm = pt->bpm;
    t.beat = pt->beat;
    t.beatsPerSample = pt->beatsPerSample;
    t.startedThisBlock = pt->startedThisBlock != 0;
    t.stoppedThisBlock = pt->stoppedThisBlock != 0;
    lab::AudioIn ai;
    ai.frames = in;
    gS.ws->render(t, ai, l, r);
}
void transport(void*, bool p) { gS.ws->onTransport(p); }
int settings(void*, PlatSetting* rows, int cap) {
    return gS.ws->settings(reinterpret_cast<lab::SettingRow*>(rows), cap);
}
void save(void*) { gS.ws->save(); }
void close(void*) {}

}  // namespace

extern "C" __attribute__((used, visibility("default"), section(".text.plat_app_entry"))) const PlatApp*
plat_app_entry(void) {
    static PlatApp a;
    a.abi = PLAT_ABI_VERSION;
    a.size = sizeof(PlatApp);
    a.name = plat_app_info.name;
    a.tagline = plat_app_info.desc;
    a.introMs = 0;
    a.self = &gS;
    a.init = init;
    a.drawIntro = drawIntro;
    a.event = event;
    a.midi = midi;
    a.update = update;
    a.draw = draw;
    a.leds = leds;
    a.render = render;
    a.transport = transport;
    a.settings = settings;
    a.save = save;
    a.close = close;
    return &a;
}
