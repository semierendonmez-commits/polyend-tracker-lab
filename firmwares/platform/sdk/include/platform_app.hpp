// PLATFORM native app SDK -- C++ convenience layer over platform_app.h.
//
//   class MyApp : public plat::App { ... override what you need ... };
//   PLAT_APP_INFO("MyApp", "me", "what it does", "drone", 256, 0);
//   PLAT_APP(MyApp)
//
// The object lives in static storage (no heap); plat::App keeps the host
// table in `host` and offers short drawing / file helpers.
#pragma once

#include <stdint.h>
#include <string.h>

#include "platform_app.h"

namespace plat {

class App {
public:
    const PlatHost* host = nullptr;
    virtual ~App() {}

    virtual bool init() { return true; }
    virtual void drawIntro(uint32_t ms) { (void)ms; }
    virtual void event(const PlatEvent& e) { (void)e; }
    virtual void midi(uint8_t status, uint8_t d1, uint8_t d2) { (void)status; (void)d1; (void)d2; }
    virtual void update(uint32_t ms) { (void)ms; }
    virtual void draw() {}
    virtual void leds(uint8_t* rgb) { (void)rgb; }
    virtual void render(const PlatTransport& t, const int16_t* in, float* l, float* r) {
        (void)t; (void)in; (void)l; (void)r;
    }
    virtual void transport(bool playing) { (void)playing; }
    virtual int settings(PlatSetting* rows, int cap) { (void)rows; (void)cap; return 0; }
    virtual void save() {}
    virtual void close() {}
    // LABKIT navigation (optional): pages for the MAP / PARAM pads / HELP
    virtual int navPages(PlatPage* out, int cap) { (void)out; (void)cap; return 0; }
    virtual int navPage() { return -1; }
    virtual void navGoto(int page) { (void)page; }
    virtual void navFocus(int param) { (void)param; }
    virtual void navParamChanged(int param) { (void)param; }

    // ---- helpers ----
    template <class T> T* alloc(uint32_t n = 1) {
        return static_cast<T*>(host->alloc(host->ctx, uint32_t(sizeof(T)) * n, alignof(T) < 8 ? 8 : alignof(T)));
    }
    void color(uint32_t rgb) const { host->color(host->ctx, rgb); }
    void fill(int x, int y, int w, int h) const { host->fill(host->ctx, x, y, w, h); }
    void frame(int x, int y, int w, int h) const { host->frame(host->ctx, x, y, w, h); }
    void line(float x0, float y0, float x1, float y1) const { host->line(host->ctx, x0, y0, x1, y1); }
    void text(int x, int y, uint8_t font, const char* s, uint16_t opt = 0) const { host->text(host->ctx, x, y, font, s, opt); }
    void point(float x, float y, float r) const { host->point(host->ctx, x, y, r); }
    void ring(float x, float y, float r, int seg = 24) const { host->ring(host->ctx, x, y, r, seg); }
    void toast(const char* m) const { host->toast(host->ctx, m); }
    void log(const char* m) const { host->log(host->ctx, m); }
    uint32_t millis() const { return host->millis(host->ctx); }
    // LABKIT: a parameter in the shared registry (PARAM pads, MAP, macros, mod
    // routes, MIDI learn, saved per app). *value gets base + modulation once
    // per audio block before render(). Returns the id (< 0: none).
    int param(const char* key, const char* name, float* value, float lo, float hi, float def, const char* unit = nullptr,
              int page = 0, uint32_t flags = 0) const {
        return host->paramAdd ? host->paramAdd(host->ctx, key, name, value, lo, hi, def, unit, page, flags) : -1;
    }
    float paramGet(int id, bool effective = false) const { return host->paramGet ? host->paramGet(host->ctx, id, effective) : 0.0f; }
    void paramSet(int id, float v) const {
        if (host->paramSet && id >= 0) host->paramSet(host->ctx, id, v);
    }
    int modSource(const char* name, const float* value, bool bipolar = false) const {
        return host->modSource ? host->modSource(host->ctx, name, value, bipolar) : -1;
    }
};

// Adapter: a static App object -> the PlatApp table.
template <class T>
struct AppGlue {
    static T& obj() {
        static T t;
        return t;
    }
    static bool init(void*, const PlatHost* h) {
        if (!h || h->abi != PLAT_ABI_VERSION || h->size < sizeof(PlatHost)) return false;
        obj().host = h;
        return obj().init();
    }
    static void drawIntro(void*, uint32_t ms) { obj().drawIntro(ms); }
    static void event(void*, const PlatEvent* e) { obj().event(*e); }
    static void midi(void*, uint8_t s, uint8_t a, uint8_t b) { obj().midi(s, a, b); }
    static void update(void*, uint32_t ms) { obj().update(ms); }
    static void draw(void*) { obj().draw(); }
    static void leds(void*, uint8_t* rgb) { obj().leds(rgb); }
    static void render(void*, const PlatTransport* t, const int16_t* in, float* l, float* r) { obj().render(*t, in, l, r); }
    static void transport(void*, bool p) { obj().transport(p); }
    static int settings(void*, PlatSetting* rows, int cap) { return obj().settings(rows, cap); }
    static void save(void*) { obj().save(); }
    static void close(void*) { obj().close(); }
    static int navPages(void*, PlatPage* out, int cap) { return obj().navPages(out, cap); }
    static int navPage(void*) { return obj().navPage(); }
    static void navGoto(void*, int p) { obj().navGoto(p); }
    static void navFocus(void*, int id) { obj().navFocus(id); }
    static void navParamChanged(void*, int id) { obj().navParamChanged(id); }
    static const PlatApp* table(const char* name, const char* tagline, const uint32_t theme[8], uint32_t introMs) {
        static PlatApp a;
        a.abi = PLAT_ABI_VERSION;
        a.size = sizeof(PlatApp);
        a.name = name;
        a.tagline = tagline;
        for (int i = 0; i < 8; ++i) a.theme[i] = theme ? theme[i] : 0;
        a.introMs = introMs;
        a.self = &obj();
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
        a.navPages = navPages;
        a.navPage = navPage;
        a.navGoto = navGoto;
        a.navFocus = navFocus;
        a.navParamChanged = navParamChanged;
        return &a;
    }
};

}  // namespace plat

#define PLAT_APP(CLASS)                                                             \
    extern "C" __attribute__((used, visibility("default"), section(".text.plat_app_entry"))) const PlatApp* \
    plat_app_entry(void) {                                                          \
        return ::plat::AppGlue<CLASS>::table(plat_app_info.name, plat_app_info.desc, nullptr, 0); \
    }
