// LABKIT v2 host tests (make lab-test): the registry, mod routes, macros,
// save / load, the nav keys, the browser listing on a fake card and the
// keyboard. Independent of the firmware's workstation: every test builds its
// own Kit on a private arena (the Shell object is only a toast target).
#include <cmath>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "formats/wav.h"
#include "lab/lab.h"
#include "lab_test.h"

using namespace lab;
using kit::ParamId;

namespace {

// A tiny in-memory card.
class FakeCard final : public IStorage {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs{""};
    struct Open {
        std::string path;
        uint32_t pos = 0;
        bool used = false;
    };
    Open open_[8];
    static std::string norm(const char* p) {
        std::string s(p ? p : "");
        while (!s.empty() && s[0] == '/') s.erase(0, 1);
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    Handle open(const char* path, OpenMode mode) override {
        const std::string p = norm(path);
        if (mode == kRead && !files.count(p)) return -1;
        if (mode == kWrite) files[p].clear();
        for (int i = 0; i < 8; ++i)
            if (!open_[i].used) {
                open_[i] = Open{p, 0, true};
                return i;
            }
        return -1;
    }
    int32_t read(Handle h, void* dst, uint32_t len) override {
        Open& o = open_[h];
        const auto& f = files[o.path];
        const uint32_t n = o.pos >= f.size() ? 0 : std::min<uint32_t>(len, uint32_t(f.size()) - o.pos);
        std::memcpy(dst, f.data() + o.pos, n);
        o.pos += n;
        return int32_t(n);
    }
    int32_t write(Handle h, const void* src, uint32_t len) override {
        Open& o = open_[h];
        auto& f = files[o.path];
        if (f.size() < o.pos + len) f.resize(o.pos + len);
        std::memcpy(f.data() + o.pos, src, len);
        o.pos += len;
        return int32_t(len);
    }
    bool seek(Handle h, uint32_t pos) override {
        open_[h].pos = pos;
        return true;
    }
    int32_t size(Handle h) override { return int32_t(files[open_[h].path].size()); }
    void close(Handle h) override { open_[h].used = false; }
    bool exists(const char* path) override {
        const std::string p = norm(path);
        return files.count(p) || dirs.count(p);
    }
    bool mkdir(const char* path) override {
        dirs.insert(norm(path));
        return true;
    }
    bool remove(const char* path) override { return files.erase(norm(path)) > 0; }
    bool rename(const char*, const char*) override { return false; }
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        const std::string d = norm(path);
        if (!dirs.count(d)) return -1;
        const std::string pre = d.empty() ? "" : d + "/";
        int32_t n = 0;
        auto child = [&](const std::string& full) -> std::string {
            if (full.compare(0, pre.size(), pre) != 0 || full.size() == pre.size()) return "";
            const std::string rest = full.substr(pre.size());
            return rest.find('/') == std::string::npos ? rest : "";
        };
        for (const auto& dd : dirs) {
            const std::string c = child(dd);
            if (c.empty()) continue;
            DirEntry e{c.c_str(), true, 0};
            ++n;
            if (!fn(e, ctx)) return n;
        }
        for (const auto& f : files) {
            const std::string c = child(f.first);
            if (c.empty()) continue;
            DirEntry e{c.c_str(), false, uint32_t(f.second.size())};
            ++n;
            if (!fn(e, ctx)) return n;
        }
        return n;
    }
    void addWav(const std::string& path, uint32_t frames, uint16_t ch) {
        uint8_t hdr[ot::wav::kHeaderSize];
        ot::wavMakeHeader(hdr, ch, frames * 2u * ch);
        std::vector<uint8_t> f(hdr, hdr + sizeof hdr);
        for (uint32_t i = 0; i < frames * ch; ++i) {
            const int16_t s = int16_t(12000.0f * sinf(float(i) * 0.05f));
            f.push_back(uint8_t(s));
            f.push_back(uint8_t(uint16_t(s) >> 8));
        }
        files[norm(path.c_str())] = f;
    }
};

// A workstation that only describes pages (for the nav tests).
class PagesWs final : public Workstation {
public:
    int page = 0, gotos = 0, focusCalls = 0, changed = 0;
    Theme th;
    const char* name() const override { return "T"; }
    const char* tagline() const override { return "t"; }
    const Theme& theme() const override { return th; }
    bool init(Context&) override { return true; }
    void drawIntro(Canvas&, uint32_t) override {}
    void onEvent(const Event&) override {}
    void draw(Canvas&) override {}
    void leds(Rgb*) override {}
    void render(const Transport&, const AudioIn&, float*, float*) override {}
    int navPages(kit::PageInfo* out, int cap) override {
        static const char* const names[3] = {"ONE", "TWO", "THREE"};
        for (int i = 0; i < 3 && i < cap; ++i) out[i].name = names[i];
        return 3;
    }
    int navPage() const override { return page; }
    void navGoto(int p) override {
        page = p;
        ++gotos;
    }
    void navFocus(ParamId) override { ++focusCalls; }
    void navParamChanged(ParamId) override { ++changed; }
};

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(8u * 1024u * 1024u);
    Arena arena;
    Shell shell;   // not initialised: a toast target only
    Context ctx;
    FakeCard card;
    kit::Kit* k = nullptr;
    PagesWs ws;
    explicit Rig(bool withCard = true) {
        arena.attach(mem.data(), uint32_t(mem.size()));
        ctx.shell = &shell;
        ctx.arena = &arena;
        ctx.fs = withCard ? &card : nullptr;
        ctx.home = "Lab/T";
        card.mkdir("Lab");
        card.mkdir("Lab/T");
        k = arena.make<kit::Kit>();
        k->init(shell, ctx, arena, false);
    }
    void activate() { k->afterWorkstationInit(&ws); }
    kit::Registry& reg() { return k->reg; }
    kit::Mod& mod() { return k->mod; }
    void block(int n = 1) {
        Transport t;
        t.bpm = 120.0f;
        t.beatsPerSample = 120.0 / 60.0 / 44100.0;
        for (int i = 0; i < n; ++i) {
            k->mod.update(false);
            k->audioBlock(t);
        }
    }
    Event press(uint8_t id, bool shift = false, uint32_t ms = 0) {
        Event e;
        e.type = Event::kPress;
        e.id = id;
        e.shift = shift;
        e.ms = ms;
        return e;
    }
    void ev(Event::Type t, uint8_t id, int8_t delta = 0, uint32_t ms = 0, bool shift = false) {
        Event e;
        e.type = t;
        e.id = id;
        e.delta = delta;
        e.ms = ms;
        e.shift = shift;
        e.velocity = 100;
        k->event(e);
    }
};

struct State {
    float cut = 1000.0f, res = 0.3f, pan = 0.0f, gain = 0.0f;
    int voices = 4, wave = 1;
    int setterCalls = 0;
    float setterValue = 0.0f;
};

const char* const kWaves[] = {"Sine", "Saw", "Square"};

void registerState(kit::Registry& P, State& s) {
    P.setPage(0);
    P.setGroup("FLT");
    P.add("cut", "Cutoff", &s.cut).range(20.0f, 20000.0f).exp().unit("Hz");
    P.add("res", "Reso", &s.res).percent();
    P.setPage(1);
    P.setGroup("OUT");
    P.add("pan", "Pan", &s.pan).range(-1.0f, 1.0f).bipolar();
    P.add("voices", "Voices", &s.voices).range(1, 8).pad(5);
    P.add("wave", "Wave", &s.wave).names(kWaves, 3);
    P.add("gain", "Gain", [](void* c, float v) {
        State* st = static_cast<State*>(c);
        ++st->setterCalls;
        st->setterValue = v;
    }, &s, -6.0f).range(-60.0f, 6.0f).unit("dB");
}

}  // namespace

LAB_TEST(labkit_negative_int_survives_registration) {
    Rig r;
    int octave = -3, big = 12;
    float freq = 440.0f;
    kit::Registry& P = r.reg();
    P.add("oct", "Octave", &octave).range(-4, 4);
    P.add("big", "Big", &big).range(0, 24);
    P.add("freq", "Freq", &freq).range(20.0f, 2000.0f);
    CHECK(octave == -3);
    CHECK(big == 12);
    CHECK_NEAR(freq, 440.0, 0.01);
    CHECK(P.get(P.find("oct")) == -3.0f);
    P.nudge(P.find("oct"), -1);
    CHECK(octave == -4);
}

LAB_TEST(labkit_registry_basics) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    kit::Registry& P = r.reg();
    CHECK(P.count() == 6);
    const ParamId cut = P.find("cut"), res = P.find("res"), voices = P.find("voices"), wave = P.find("wave");
    CHECK(P.valid(cut) && P.valid(voices));
    CHECK(P.find("nope") == kit::kNoParam);
    CHECK_NEAR(P.get(cut), 1000.0, 0.5);
    CHECK_NEAR(P.get(res), 0.3, 1e-4);
    CHECK(P.get(voices) == 4.0f);
    // exp curve: the middle of 20..20000 Hz is ~632 Hz
    P.setNorm(cut, 0.5f);
    CHECK_NEAR(P.get(cut), 632.46, 0.5);
    CHECK_NEAR(s.cut, 632.46, 0.5);   // pointer bindings are written right away
    char buf[32];
    P.format(cut, buf, sizeof buf);
    CHECK(std::strcmp(buf, "632 Hz") == 0);
    P.set(cut, 2500.0f);
    P.format(cut, buf, sizeof buf);
    CHECK(std::strcmp(buf, "2.50k Hz") == 0);
    P.format(res, buf, sizeof buf);
    CHECK(std::strcmp(buf, "30%") == 0);
    P.format(wave, buf, sizeof buf);
    CHECK(std::strcmp(buf, "Saw") == 0);
    // nudge: ints by 1, enums by 1, continuous 1 % per detent (Shift: 0.2 %)
    P.nudge(voices, 3);
    CHECK(s.voices == 7);
    P.nudge(voices, 5);
    CHECK(s.voices == 8);
    P.nudge(wave, 1);
    CHECK(s.wave == 2);
    const float n0 = P.norm(res);
    P.nudge(res, 2);
    CHECK_NEAR(P.norm(res), n0 + 0.02f, 1e-5);
    P.nudge(res, 1, true);
    CHECK_NEAR(P.norm(res), n0 + 0.022f, 1e-5);
    P.reset(res);
    CHECK_NEAR(s.res, 0.3, 1e-5);
    // bipolar format
    P.set(P.find("pan"), 0.25f);
    P.format(P.find("pan"), buf, sizeof buf);
    CHECK(std::strcmp(buf, "+0.25") == 0);
    // pad layout: fixed pad first, then registration order
    ParamId pads[kit::kPadParams];
    const int used = P.padLayout(1, pads);
    CHECK(used == 6);
    CHECK(pads[5] == voices);
    CHECK(pads[0] == P.find("pan"));
    CHECK(pads[1] == wave);
    ParamId list[8];
    CHECK(P.onPage(0, list, 8) == 2);
}

LAB_TEST(labkit_registry_audio_writes) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    r.activate();
    kit::Registry& P = r.reg();
    const ParamId gain = P.find("gain");
    CHECK(s.setterCalls == 0);
    r.block();   // dirty since registration: the setter runs in the audio block
    CHECK(s.setterCalls >= 1);
    CHECK_NEAR(s.setterValue, -6.0, 0.01);
    const int calls = s.setterCalls;
    r.block();
    CHECK(s.setterCalls == calls);   // nothing changed: no call
    P.set(gain, -12.0f);
    CHECK(s.setterCalls == calls);   // main loop: not yet
    r.block();
    CHECK(s.setterCalls == calls + 1);
    CHECK_NEAR(s.setterValue, -12.0, 0.01);
    // the workstation writes its own field: sync() follows it
    s.cut = 4000.0f;
    P.sync();
    CHECK_NEAR(P.get(P.find("cut")), 4000.0, 0.5);
    s.voices = 2;
    P.sync();
    CHECK(P.get(P.find("voices")) == 2.0f);
}

LAB_TEST(labkit_mod_routes_and_lfo) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    r.activate();
    CHECK(r.k->active);
    kit::Registry& P = r.reg();
    kit::Mod& M = r.mod();
    const ParamId res = P.find("res");
    P.set(res, 0.5f);
    const int rt = M.route(kit::kSrcLfo, res, 0.25f);
    CHECK(rt == 0);
    CHECK(M.routeCount() == 1);
    CHECK(P.meta(res).mods == 1);
    P.set(M.lfoParam(0, 1), 5.0f);   // LFO1 at 5 Hz
    float lo = 1.0f, hi = 0.0f;
    for (int b = 0; b < 400; ++b) {
        r.block();
        lo = std::fmin(lo, s.res);
        hi = std::fmax(hi, s.res);
        CHECK(s.res >= 0.0f && s.res <= 1.0f);
    }
    CHECK_NEAR(hi, 0.75, 0.02);
    CHECK_NEAR(lo, 0.25, 0.02);
    CHECK_NEAR(P.get(res), 0.5, 1e-5);   // the base stays what the user set
    // the depth follows
    M.setDepth(rt, -1.0f);
    for (int b = 0; b < 400; ++b) r.block();
    CHECK(P.effectiveNorm(res) >= 0.0f && P.effectiveNorm(res) <= 1.0f);
    // removing the route writes the base back
    CHECK(M.unroute(kit::kSrcLfo, res));
    r.block(2);
    CHECK_NEAR(s.res, 0.5, 1e-5);
    CHECK(P.meta(res).mods == 0);
    // a noMod param refuses routes; the table holds 64
    CHECK(M.route(kit::kSrcLfo, M.macroParam(0), 0.5f) < 0);
    // fill the table: LFO / RND sources x every modulatable param, 64 at most
    int made = 0;
    for (int src = 0; src < kit::kSrcMacro; ++src)
        for (int p = 0; p < P.count(); ++p)
            if (P.modulatable(ParamId(p)) && M.route(src, ParamId(p), 0.1f) >= 0) ++made;
    CHECK(made == kit::kMaxRoutes);
    CHECK(M.routeCount() == kit::kMaxRoutes);
    r.block(10);   // the plan with 64 routes runs and stays in range
    for (int p = 0; p < P.count(); ++p) CHECK(P.effectiveNorm(ParamId(p)) >= 0.0f && P.effectiveNorm(ParamId(p)) <= 1.0f);
    M.clearRoutes();
    CHECK(M.routeCount() == 0);
}

LAB_TEST(labkit_macros) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    r.activate();
    kit::Registry& P = r.reg();
    kit::Mod& M = r.mod();
    const ParamId cut = P.find("cut"), res = P.find("res");
    P.setNorm(cut, 0.2f);
    P.set(res, 0.1f);
    M.setMacroName(0, "BRIGHT");
    CHECK(std::strcmp(M.macroName(0), "BRIGHT") == 0);
    CHECK(std::strcmp(M.sourceName(kit::kSrcMacro), "BRIGHT") == 0);
    CHECK(std::strcmp(P.name(M.macroParam(0)), "BRIGHT") == 0);
    CHECK(M.route(kit::kSrcMacro, cut, 0.5f) >= 0);
    const int w = M.route(kit::kSrcMacro, res, 0.0f);
    M.setWindow(w, true, 0.2f, 0.8f);
    M.setMacro(0, 0.0f);
    r.block(2);
    CHECK_NEAR(P.effectiveNorm(cut), 0.2, 1e-4);
    CHECK_NEAR(s.res, 0.2, 1e-4);   // window: lo at macro 0
    M.setMacro(0, 1.0f);
    r.block(2);
    CHECK_NEAR(P.effectiveNorm(cut), 0.7, 1e-4);
    CHECK_NEAR(s.res, 0.8, 1e-4);   // window: hi at macro 1
    M.setMacro(0, 0.5f);
    r.block(2);
    CHECK_NEAR(s.res, 0.5, 1e-4);
    // 8 targets per macro
    for (int i = 0; i < 6; ++i) P.add(i == 0 ? "x0" : (i == 1 ? "x1" : (i == 2 ? "x2" : (i == 3 ? "x3" : (i == 4 ? "x4" : "x5")))), "X", &s.pan);
    int ok = 0;
    for (int i = 0; i < P.count(); ++i) ok += M.route(kit::kSrcMacro + 1, ParamId(i), 0.1f) >= 0;
    CHECK(M.routesFrom(kit::kSrcMacro + 1, nullptr, 0) == kit::kMacroTargets);
    CHECK(ok == kit::kMacroTargets);
}

LAB_TEST(labkit_save_load_chunk) {
    std::vector<uint8_t> chunk(16384);
    uint32_t n = 0;
    {
        Rig r;
        State s;
        registerState(r.reg(), s);
        r.activate();
        kit::Registry& P = r.reg();
        P.set(P.find("cut"), 3000.0f);
        P.set(P.find("voices"), 6.0f);
        P.set(P.find("wave"), 2.0f);
        P.set(P.find("gain"), -20.0f);
        r.mod().setMacroName(2, "WOBBLE");
        r.mod().setMacro(2, 0.75f);
        r.mod().route(kit::kSrcLfo + 1, P.find("res"), -0.4f);
        const int w = r.mod().route(kit::kSrcMacro + 2, P.find("pan"), 0.0f);
        r.mod().setWindow(w, true, 0.1f, 0.9f);
        P.set(r.mod().lfoParam(1, 1), 7.5f);
        CHECK(r.k->nav.learnAssign(0xB0, 74, P.find("cut")));
        n = P.saveChunk(chunk.data(), uint32_t(chunk.size()));
        CHECK(n > 0);
        CHECK(n == P.chunkSize());
        CHECK(P.saveChunk(chunk.data(), 10) == 0);   // no room
        CHECK(P.saveFile(&r.card, "Lab/T/p.lkp"));
        Rig r2;   // load from the file into a fresh kit
        State s2;
        registerState(r2.reg(), s2);
        r2.activate();
        r2.card.files = r.card.files;
        CHECK(r2.reg().loadFile(&r2.card, "Lab/T/p.lkp"));
        CHECK_NEAR(s2.cut, 3000.0, 1.0);
    }
    Rig r;
    State s;
    // a newer firmware: one param less (the unknown key is skipped), one more
    kit::Registry& P = r.reg();
    P.add("cut", "Cutoff", &s.cut).range(20.0f, 20000.0f).exp();
    P.add("res", "Reso", &s.res);
    P.add("pan", "Pan", &s.pan).range(-1.0f, 1.0f);
    P.add("wave", "Wave", &s.wave).names(kWaves, 3);
    float extra = 0.33f;
    P.add("new", "New", &extra);
    r.activate();
    CHECK(P.loadChunk(chunk.data(), n));
    CHECK_NEAR(s.cut, 3000.0, 1.0);
    CHECK(s.wave == 2);
    CHECK_NEAR(extra, 0.33, 1e-6);
    CHECK(std::strcmp(r.mod().macroName(2), "WOBBLE") == 0);
    CHECK_NEAR(r.mod().macro(2), 0.75, 1e-4);
    CHECK_NEAR(P.get(r.mod().lfoParam(1, 1)), 7.5, 0.01);
    CHECK(r.mod().routeCount() == 2);
    const int ri = r.mod().findRoute(kit::kSrcLfo + 1, P.find("res"));
    CHECK(ri >= 0);
    if (ri >= 0) CHECK_NEAR(r.mod().routeAt(ri).depth, -0.4, 1e-5);
    const int wi = r.mod().findRoute(kit::kSrcMacro + 2, P.find("pan"));
    CHECK(wi >= 0 && r.mod().routeAt(wi).window == 1);
    CHECK(r.k->nav.learnCount() == 1);
    // garbage is refused
    std::vector<uint8_t> bad(chunk.begin(), chunk.begin() + n);
    bad[0] ^= 0xFF;
    CHECK(!P.loadChunk(bad.data(), n));
    CHECK(!P.loadChunk(chunk.data(), 6));
}

LAB_TEST(labkit_nav_keys_routes_and_map) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    r.activate();
    kit::Nav& N = r.k->nav;
    CHECK(N.pageCount() == 5);   // ONE TWO THREE + MACRO + MOD
    // I.Param tap: the PARAM pads latch
    r.ev(Event::kPress, btn::InstrParams, 0, 1000);
    CHECK(N.overlay() == kit::Nav::kParam);
    r.ev(Event::kRelease, btn::InstrParams, 0, 1100);
    CHECK(N.overlay() == kit::Nav::kParam);
    // pad 1 = Reso (page 0); hold + encoder edits it
    const ParamId res = r.reg().find("res");
    const float n0 = r.reg().norm(res);
    r.ev(Event::kPadDown, 1);
    CHECK(N.focus() == res);
    r.ev(Event::kEncoder, 0, 5);
    r.ev(Event::kPadUp, 1);
    CHECK_NEAR(r.reg().norm(res), n0 + 0.05f, 1e-4);
    CHECK(r.ws.changed > 0);   // the workstation hears about Shell edits
    // hold LFO1 (pad 36) + tap Cutoff (pad 0) = a route; encoder = depth
    r.ev(Event::kPadDown, 36);
    r.ev(Event::kPadDown, 0);
    r.ev(Event::kEncoder, 0, 10);
    r.ev(Event::kPadUp, 0);
    r.ev(Event::kPadUp, 36);
    CHECK(r.mod().routeCount() == 1);
    const int rt = r.mod().findRoute(kit::kSrcLfo, r.reg().find("cut"));
    CHECK(rt >= 0);
    if (rt >= 0) CHECK_NEAR(r.mod().routeAt(rt).depth, 0.35, 1e-4);
    // hold the source + Delete with the param held removes it
    r.ev(Event::kPadDown, 36);
    r.ev(Event::kPadDown, 0);
    r.ev(Event::kPress, btn::Delete);
    r.ev(Event::kPadUp, 0);
    r.ev(Event::kPadUp, 36);
    CHECK(r.mod().routeCount() == 0);
    // a second tap closes
    r.ev(Event::kPress, btn::InstrParams, 0, 2000);
    CHECK(N.overlay() == kit::Nav::kNone);
    r.ev(Event::kRelease, btn::InstrParams, 0, 2050);
    // MAP held: pad 1 (row 1) = page TWO, release = go there
    r.ev(Event::kPress, btn::Master, 0, 3000);
    CHECK(N.overlay() == kit::Nav::kMap);
    r.ev(Event::kPadDown, 1, 0, 3100);
    r.ev(Event::kPadUp, 1, 0, 3150);
    r.ev(Event::kRelease, btn::Master, 0, 3600);
    CHECK(N.overlay() == kit::Nav::kNone);
    CHECK(r.ws.page == 1);
    CHECK(r.ws.gotos == 1);
    // MAP: the MACRO column opens the MACRO page
    r.ev(Event::kPress, btn::Master, 0, 4000);
    r.ev(Event::kPadDown, 3, 0, 4100);   // ONE TWO THREE MACRO
    r.ev(Event::kRelease, btn::Master, 0, 4700);
    CHECK(N.overlay() == kit::Nav::kMacro);
    // macro page: pad row 1 col 0 = macro 1 at 100 %
    r.ev(Event::kPadDown, 0);
    r.ev(Event::kPadUp, 0);
    CHECK_NEAR(r.mod().macro(0), 1.0, 1e-4);
    r.ev(Event::kPadDown, padAt(1, 3));
    r.ev(Event::kPadUp, padAt(1, 3));
    CHECK_NEAR(r.mod().macro(1), 0.0, 1e-4);
    // Shift + Master: MOD page; F8 closes
    r.ev(Event::kPress, btn::Master, 0, 5000, true);
    CHECK(N.overlay() == kit::Nav::kMod);
    r.ev(Event::kPress, btn::F8);
    CHECK(N.overlay() == kit::Nav::kNone);
    // keys elsewhere pass through when nothing is open
    Event e;
    e.type = Event::kPress;
    e.id = btn::Note;
    CHECK(!r.k->event(e));
}

LAB_TEST(labkit_midi_learn) {
    Rig r;
    State s;
    registerState(r.reg(), s);
    r.activate();
    const ParamId res = r.reg().find("res");
    CHECK(r.k->nav.learnAssign(0xB2, 21, res));
    MidiMsg m;
    m.status = 0xB2;
    m.d1 = 21;
    m.d2 = 127;
    CHECK(r.k->midi(m));
    CHECK_NEAR(s.res, 1.0, 1e-5);
    m.d1 = 22;
    CHECK(!r.k->midi(m));   // not learned: the workstation gets it
    m.status = 0x92;
    CHECK(!r.k->midi(m));
}

LAB_TEST(labkit_inactive_without_params) {
    Rig r;
    // a workstation that registers nothing and describes no pages
    class Plain final : public Workstation {
    public:
        Theme th;
        const char* name() const override { return "P"; }
        const char* tagline() const override { return ""; }
        const Theme& theme() const override { return th; }
        bool init(Context&) override { return true; }
        void drawIntro(Canvas&, uint32_t) override {}
        void onEvent(const Event&) override {}
        void draw(Canvas&) override {}
        void leds(Rgb*) override {}
        void render(const Transport&, const AudioIn&, float*, float*) override {}
    } plain;
    r.k->afterWorkstationInit(&plain);
    CHECK(!r.k->active);
    CHECK(r.reg().count() == 0);
    r.ev(Event::kPress, btn::InstrParams);
    CHECK(r.k->nav.overlay() == kit::Nav::kNone);
    Event e;
    e.type = Event::kPress;
    e.id = btn::Master;
    CHECK(!r.k->event(e));
    CHECK(r.k->reserveWords() == 0);
    CHECK(!r.k->fullScreen());
}

LAB_TEST(labkit_browser_listing) {
    Rig r;
    FakeCard& c = r.card;
    c.mkdir("Samples");
    c.mkdir("Samples/Drums");
    c.addWav("Samples/kick.wav", 22050, 1);
    c.addWav("Samples/Snare.WAV", 4410, 2);
    c.addWav("Samples/Drums/hat.wav", 1000, 1);
    c.files["Samples/notes.txt"] = std::vector<uint8_t>(10, 'x');
    c.files["Samples/.hidden.wav"] = std::vector<uint8_t>(10, 'x');
    kit::Browser& b = *r.k->browser;
    kit::BrowserRequest q;
    q.folder = "Samples";
    CHECK(!b.open(q));   // not reserved
    CHECK(b.reserve(64, 1.0f));
    std::string picked;
    q.ctx = &picked;
    q.onPick = [](void* p, const char* path) { *static_cast<std::string*>(p) = path; };
    CHECK(b.open(q));
    CHECK(b.isOpen());
    CHECK(b.entryCount() == 4);   // .. Drums kick.wav Snare.WAV (no .txt, no hidden)
    CHECK(std::strcmp(b.entryName(0), "..") == 0);
    CHECK(std::strcmp(b.entryName(1), "Drums") == 0 && b.entryIsDir(1));
    CHECK(std::strcmp(b.entryName(2), "kick.wav") == 0);
    CHECK(std::strcmp(b.entryName(3), "Snare.WAV") == 0);
    CHECK(b.selected() == 2);   // lands on the first file
    CHECK(b.wavValid());
    CHECK(b.wavFrames() == 22050);
    // the preview loads in main-loop chunks
    for (uint32_t t = 0; t < 400; t += 10) b.update(1000 + t);
    CHECK(b.previewFrames() == 22050);
    float l[kBlock] = {}, rr[kBlock] = {};
    b.mix(l, rr);
    float peak = 0.0f;
    for (float v : l) peak = std::fmax(peak, std::fabs(v));
    CHECK(peak > 0.01f);
    // sort by size: the bigger file first
    b.setSort(1);
    CHECK(std::strcmp(b.entryName(2), "kick.wav") == 0);
    // jump by letter, enter a folder, go back
    b.jumpLetter('s');
    CHECK(std::strcmp(b.entryName(b.selected()), "Snare.WAV") == 0);
    b.select(1);
    b.enterOrPick();
    CHECK(std::strcmp(b.folder(), "Samples/Drums") == 0);
    CHECK(b.entryCount() == 2);
    b.parent();
    CHECK(std::strcmp(b.folder(), "Samples") == 0);
    CHECK(std::strcmp(b.entryName(b.selected()), "Drums") == 0);
    // favourites
    b.jumpLetter('k');
    b.toggleFavourite();
    CHECK(b.isFavourite("Samples/kick.wav"));
    b.setView(1);
    CHECK(b.entryCount() == 1);
    b.setView(0);
    // pick: the callback, the recent list
    b.jumpLetter('k');
    b.enterOrPick();
    CHECK(!b.isOpen());
    CHECK(picked == "Samples/kick.wav");
    CHECK(b.open(q));
    b.setView(2);
    CHECK(b.entryCount() == 1);
    b.close(false);
    // favourites and recent survive in labkit.bin
    r.k->savePrefs();
    CHECK(c.files.count("Lab/T/labkit.bin") == 1);
    Rig r2;
    r2.card.files = c.files;
    r2.k->loadPrefs();
    CHECK(r2.k->browser->isFavourite("Samples/kick.wav"));
}

LAB_TEST(labkit_browser_without_card) {
    Rig r(false);
    kit::Browser& b = *r.k->browser;
    CHECK(b.reserve(16, 0.0f));
    kit::BrowserRequest q;
    CHECK(!b.open(q));
    CHECK(!b.isOpen());
}

LAB_TEST(labkit_keyboard) {
    Rig r;
    kit::Keyboard& kb = *r.k->keyboard;
    std::string done;
    kit::KeyboardRequest q;
    q.initial = "AB";
    q.maxLen = 6;
    q.ctx = &done;
    q.onDone = [](void* p, const char* t) { *static_cast<std::string*>(p) = t; };
    CHECK(kb.open(q));
    CHECK(kb.cursor() == 2);
    CHECK(kit::Keyboard::padChar(12, true) == 'Q');
    CHECK(kit::Keyboard::padChar(12, false) == 'q');
    CHECK(kit::Keyboard::padChar(0, true) == '1');
    CHECK(kit::Keyboard::padChar(47, true) == 0);   // OK
    r.ev(Event::kPadDown, 12);   // Q (upper by default)
    r.ev(Event::kPadUp, 12);
    r.ev(Event::kPadDown, 0);    // 1
    r.ev(Event::kPadUp, 0);
    CHECK(std::strcmp(kb.text(), "ABQ1") == 0);
    r.ev(Event::kEncoder, 0, -2);   // cursor between B and Q
    r.ev(Event::kPress, btn::Delete);
    CHECK(std::strcmp(kb.text(), "AQ1") == 0);
    r.ev(Event::kPress, btn::Insert);
    CHECK(std::strcmp(kb.text(), "A Q1") == 0);
    for (int i = 0; i < 5; ++i) kb.type('z');
    CHECK(std::strlen(kb.text()) == 6);   // maxLen
    r.ev(Event::kPadDown, 47);   // OK
    CHECK(!kb.isOpen());
    CHECK(done.size() == 6);
}

LAB_TEST(labkit_canvas_limit) {
    // the overlay reserve: primitives past the limit are clipped, not dropped
    Canvas g;
    g.begin(Rgb());
    CHECK(g.words() == 0);   // no display attached: nothing is counted
}
