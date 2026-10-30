// PLATFORM host tests: the Lua heap, the Lua API, the voice engine's timing,
// the .papp relocation (host side), the launcher, the crash guard and safe boot.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mem_storage.h"
#include "lab/lab.h"
#include "lab/nav.h"
#include "lab_test.h"
#include "ws/engine.h"
#include "ws/examples_data.h"
#include "ws/heap.h"
#include "ws/lua_host.h"
#include "ws/papp.h"
#include "ws/platform.h"

namespace {

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A booted Shell + PLATFORM on an in-memory card (no audio: engine reset and
// session switches complete at once).
struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(32u * 1024u * 1024u);
    MemStorage fs;
    lab::Shell shell;
    plat::Platform* ws = nullptr;
    bool ok = false;
    void boot() {
        lab::Hal h;
        h.fs = &fs;
        ok = shell.init(h, mem.data(), uint32_t(mem.size()), "PLATFORM");
        ws = ok ? static_cast<plat::Platform*>(shell.workstation()) : nullptr;
    }
    void addScript(const char* file, const std::string& text) {
        auto& v = fs.files[std::string("Lab/PLATFORM/Scripts/") + file];
        v.assign(text.begin(), text.end());
    }
    // run update + render for `blocks` audio blocks (3 ms each)
    float run(int blocks, lab::Transport* trp = nullptr) {
        lab::Transport local;
        lab::Transport& t = trp ? *trp : local;
        if (t.beatsPerSample <= 0.0) t.beatsPerSample = 120.0 / 60.0 / 44100.0;
        float peak = 0.0f;
        static uint32_t ms = 1000;
        for (int b = 0; b < blocks; ++b) {
            float l[lab::kBlock] = {}, r[lab::kBlock] = {};
            shell.kitBlock(t);   // LABKIT: mod sources, routes, registry writes (as Shell::fill)
            ws->render(t, lab::AudioIn(), l, r);
            for (uint32_t i = 0; i < lab::kBlock; ++i) peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            if (t.playing) t.beat += t.beatsPerSample * lab::kBlock;
            t.startedThisBlock = false;
            ms += 3;
            ws->update(ms);
            shell.labkit().update(ms);
        }
        return peak;
    }
};

lab::Event press(uint8_t id, bool shift = false) {
    lab::Event e;
    e.type = lab::Event::kPress;
    e.id = id;
    e.shift = shift;
    return e;
}
lab::Event release(uint8_t id, bool shift = false) {
    lab::Event e = press(id, shift);
    e.type = lab::Event::kRelease;
    return e;
}
lab::Event hold(uint8_t id, bool shift = true) {
    lab::Event e = press(id, shift);
    e.type = lab::Event::kHold;
    return e;
}
bool consoleHas(plat::LuaSession& l, const char* s) {
    for (int i = 0; i < l.consoleCount(); ++i)
        if (std::strstr(l.consoleLine(i), s)) return true;
    return false;
}

}  // namespace

// ---- heap -----------------------------------------------------------------------------
LAB_TEST(platform_heap_alloc_free_coalesce) {
    std::vector<uint8_t> mem(1u << 20);
    plat::Heap h;
    CHECK(h.init(mem.data(), uint32_t(mem.size())));
    CHECK(h.check());
    std::vector<void*> ptrs;
    uint32_t seed = 1;
    for (int round = 0; round < 20000; ++round) {
        seed = seed * 1664525u + 1013904223u;
        if (ptrs.empty() || (seed >> 28) < 9) {
            const uint32_t n = 1 + (seed >> 8) % 3000;
            void* p = h.alloc(n);
            if (p) {
                CHECK((reinterpret_cast<uintptr_t>(p) & 7u) == 0);
                std::memset(p, 0xA5, n);
                ptrs.push_back(p);
            }
        } else if ((seed >> 27) & 1) {
            const size_t k = (seed >> 4) % ptrs.size();
            ptrs[k] = h.realloc(ptrs[k], 1 + (seed >> 12) % 5000) ?: ptrs[k];
        } else {
            const size_t k = (seed >> 4) % ptrs.size();
            h.free(ptrs[k]);
            ptrs.erase(ptrs.begin() + long(k));
        }
    }
    CHECK(h.check());
    for (void* p : ptrs) h.free(p);
    CHECK(h.check());
    CHECK(h.used() == 0);
    CHECK(h.largestFree() + 8 == h.capacity());   // everything merged back into one block
    // the hard cap
    CHECK(h.alloc(2u << 20) == nullptr);
}

// ---- Lua ------------------------------------------------------------------------------
LAB_TEST(platform_lua_runs_and_reports_errors) {
    Rig r;
    r.addScript("t1.lua", "-- name: T1\nx = 0\nfunction init() x = 41 + 1 print('hi', x) end\n");
    r.addScript("bad.lua", "function init() local t = nil; return t.x end\n");
    r.addScript("syntax.lua", "function init( end\n");
    r.addScript("loop.lua", "function init() while true do end end\n");
    r.addScript("mem.lua", "function init() local t = {} for i = 1, 1e7 do t[i] = string.rep('x', 100) .. i end end\n");
    r.boot();
    CHECK(r.ok);
    if (!r.ok) return;
    CHECK(r.ws->openByName("t1.lua"));
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    CHECK(std::strcmp(r.ws->lua().consoleLine(r.ws->lua().consoleCount() - 1), "hi\t42") == 0 ||
          std::strstr(r.ws->lua().consoleLine(0), "hi") != nullptr);
    // runtime error -> error page with file:line
    CHECK(!r.ws->openByName("bad.lua"));
    CHECK(r.ws->mode() == plat::Platform::kError);
    CHECK(std::strstr(r.ws->lua().error(), "bad.lua:1") != nullptr);
    // syntax error
    CHECK(!r.ws->openByName("syntax.lua"));
    CHECK(std::strstr(r.ws->lua().error(), "syntax.lua") != nullptr);
    // endless loop: stopped by the instruction hook
    CHECK(!r.ws->openByName("loop.lua"));
    CHECK(std::strstr(r.ws->lua().error(), "too long") != nullptr);
    // out of memory: stopped by the heap cap, not by the host
    CHECK(!r.ws->openByName("mem.lua"));
    CHECK(std::strstr(r.ws->lua().error(), "memory") != nullptr);
    // the launcher still works afterwards
    r.ws->onEvent(press(lab::btn::F8));
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
}

LAB_TEST(platform_lua_api_engine_screen_params_files) {
    Rig r;
    r.addScript("api.lua", R"(
function init()
  engine.voice(1, {wave = "saw", cutoff = 1200, attack = 0.001, amp = 0.8})
  engine.set(2, "wave", "fm")
  engine.fx("delay_level", 0.5)
  params:add_number("n", "number", 0, 10, 3)
  params:add_option("o", "option", {"a", "b", "c"}, 2)
  params:add_control("c", "control", 0, 1, 0.25, 0.05)
  params:set_action("n", function(v) gotN = v end)
  assert(params:get("o") == 2 and params:string("o") == "b")
  assert(file.write("x.txt", "hello"))
  assert(file.read("x.txt") == "hello")
  assert(file.exists("x.txt"))
  assert(not pcall(file.read, "../escape.txt"))
  assert(music.note_to_hz(69) == 440)
  local e = music.euclid(8, 3)
  assert(e[1] and not e[2])
  assert(math.type(1) == "integer" and math.type(1.5) == "float")
  assert(2^31 > 0)   -- float math
end
function redraw()
  screen.color(255, 0, 0)
  screen.rect(10, 10, 100, 50)
  screen.font("large")
  screen.text(10, 100, "abc")
  screen.points({1, 2, 3, 4, 5, 6}, 3)
end
function key(n, z) lastKey = n .. z end
function pad(n, v, z) if z == 1 then engine.note(1, 48 + n, v / 127) else engine.off(1) end end
)");
    r.boot();
    if (!r.ok) return;
    CHECK(r.ws->openByName("api.lua"));
    if (r.ws->mode() != plat::Platform::kScriptRun) {
        std::fprintf(stderr, "  error: %s\n", r.ws->lua().error());
        return;
    }
    r.run(4);
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    if (r.ws->mode() != plat::Platform::kScriptRun) std::fprintf(stderr, "  error: %s\n", r.ws->lua().error());
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/api/x.txt") == 1);
    // a pad plays the saw voice
    lab::Event e;
    e.type = lab::Event::kPadDown;
    e.id = 5;
    e.velocity = 127;
    r.ws->onEvent(e);
    const float peak = r.run(20);
    CHECK(peak > 0.05f);
    CHECK(std::fabs(r.ws->engine().paramValue(0, plat::pCutoff) - 1200.0f) < 0.01f);
    CHECK(std::fabs(r.ws->engine().paramValue(1, plat::pWave) - float(plat::kFm)) < 0.01f);
    // key callback
    r.ws->onEvent(press(lab::btn::F3));
    CHECK(r.ws->lua().running());
    // params live in the LABKIT registry ("lua.<id>"), saved per script on close
    lab::kit::Registry& R = r.shell.labkit().reg;
    const lab::kit::ParamId n = R.find("lua.n");
    CHECK(R.valid(n));
    if (R.valid(n)) R.set(n, 7.0f);
    r.run(2);
    r.ws->closeSession(true);
    r.run(1);
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/api/labkit.lkc") == 1);
    CHECK(!R.valid(R.find("lua.n")));   // gone with the session
    CHECK(r.ws->openByName("api.lua"));
    r.run(2);
    CHECK(R.valid(R.find("lua.n")) && std::fabs(R.get(R.find("lua.n")) - 7.0f) < 0.01f);
}

LAB_TEST(platform_lua_clock_is_sample_accurate) {
    // A clock.sync(1/4) coroutine schedules a click every 16th note; the engine
    // places each one on its exact sample, whatever the main loop's timing.
    Rig r;
    r.addScript("clk.lua", R"(
function init()
  engine.voice(1, {wave = "click", env_mode = "perc", attack = 0, decay = 0.002, amp = 1, ftype = "off"})
  clock.run(function() while true do clock.sync(1/4) engine.note(1, 60, 1) end end)
end
)");
    r.boot();
    if (!r.ok) return;
    CHECK(r.ws->openByName("clk.lua"));
    lab::Transport t;
    t.bpm = 120.0f;
    t.beatsPerSample = 120.0 / 60.0 / 44100.0;
    t.playing = true;
    t.startedThisBlock = true;
    std::vector<float> out;
    uint32_t ms = 5000;
    for (int b = 0; b < 800; ++b) {   // ~2.3 s = 18 sixteenths
        float l[lab::kBlock] = {}, rr[lab::kBlock] = {};
        r.ws->render(t, lab::AudioIn(), l, rr);
        out.insert(out.end(), l, l + lab::kBlock);
        t.beat += t.beatsPerSample * lab::kBlock;
        t.startedThisBlock = false;
        ms += 3;
        r.ws->update(ms);   // main loop once per block: commands go ~60 ms ahead
    }
    // find click onsets (rising edges above 0.3)
    std::vector<size_t> on;
    for (size_t i = 1; i < out.size(); ++i)
        if (std::fabs(out[i]) > 0.3f && std::fabs(out[i - 1]) < 0.1f && (on.empty() || i - on.back() > 1000)) on.push_back(i);
    CHECK(on.size() >= 10);
    if (on.size() < 10) {
        float pk = 0.0f;
        for (float x : out) pk = std::fmax(pk, std::fabs(x));
        std::fprintf(stderr, "  %zu onsets, peak %.3f, mode %d, err %s\n", on.size(), double(pk), int(r.ws->mode()),
                     r.ws->lua().error());
    }
    // 16th note at 120 BPM = 5512.5 samples; every onset within 1 sample of the grid
    const double step = 44100.0 * 60.0 / 120.0 / 4.0;
    int bad = 0;
    for (size_t k = 1; k < on.size(); ++k) {
        const double d = double(on[k]) - double(on[0]) - step * double(k);
        if (std::fabs(d) > 1.01) ++bad;   // the grid is 5512.5 samples: +-1 sample of rounding
    }
    CHECK(bad == 0);
    if (bad) std::fprintf(stderr, "  %d onsets off the grid (first at %zu)\n", bad, on.empty() ? 0 : on[0]);
}

LAB_TEST(platform_examples_load_and_match_the_embedded_copies) {
    Rig r;
    r.boot();
    if (!r.ok) return;
    // the first boot wrote the examples to the card
    for (int i = 0; i < plat::kExampleScriptCount; ++i) {
        const std::string p = std::string("Lab/PLATFORM/Scripts/") + plat::kExampleScripts[i].file;
        CHECK(r.fs.files.count(p) == 1);
        // examples/scripts and the embedded copy are the same text (tools/embed_scripts.py)
        const std::string disk = slurp(std::string("../examples/scripts/") + plat::kExampleScripts[i].file);
        CHECK(!disk.empty() && disk == plat::kExampleScripts[i].text);
    }
    CHECK(r.ws->entryCount() == plat::kExampleScriptCount);
    // every example opens, runs 2 s with the transport playing, and draws within budget
    for (int i = 0; i < r.ws->entryCount(); ++i) {
        const std::string name = r.ws->entry(i).file;
        CHECK(r.ws->openEntry(i));
        if (r.ws->mode() != plat::Platform::kScriptRun) {
            std::fprintf(stderr, "  %s: %s\n", name.c_str(), r.ws->lua().error());
            continue;
        }
        lab::Transport t;
        t.playing = true;
        t.startedThisBlock = true;
        t.beatsPerSample = 120.0 / 60.0 / 44100.0;
        r.run(700, &t);
        CHECK(r.ws->mode() == plat::Platform::kScriptRun);
        if (r.ws->mode() == plat::Platform::kError) std::fprintf(stderr, "  %s: %s\n", name.c_str(), r.ws->lua().error());
        CHECK(r.ws->lua().heapPeak() < 1024u * 1024u);
        r.ws->closeSession(true);
        r.run(1);
    }
}

// ---- engine ---------------------------------------------------------------------------
LAB_TEST(platform_engine_event_lands_on_its_sample) {
    std::vector<uint8_t> mem(8u << 20);
    lab::Arena a;
    a.attach(mem.data(), uint32_t(mem.size()));
    plat::Engine e;
    CHECK(e.init(a));
    plat::Cmd c;
    c.op = plat::Cmd::kSet;
    c.a = 0;
    c.b = plat::pWave;
    c.v = float(plat::kClick);
    e.applyNow(c);
    c.b = plat::pFtype;
    c.v = 3.0f;
    e.applyNow(c);
    lab::Transport t;
    t.playing = true;
    t.beatsPerSample = 1.0 / 44100.0;   // 1 beat per second: beat b = sample b * 44100
    // render one block first so the engine has published its clock
    float l[lab::kBlock], r[lab::kBlock];
    e.render(t, l, r);
    t.beat += t.beatsPerSample * lab::kBlock;
    // note at sample 300 from now (next block start = beat t.beat)
    const double when = t.beat + 300.0 / 44100.0;
    e.setStampEpoch(e.epoch());
    e.noteOn(0, 60.0f, 1.0f, when, false);
    std::vector<float> out;
    for (int b = 0; b < 4; ++b) {
        for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
        e.render(t, l, r);
        out.insert(out.end(), l, l + lab::kBlock);
        t.beat += t.beatsPerSample * lab::kBlock;
    }
    size_t first = 0;
    while (first < out.size() && std::fabs(out[first]) < 1e-3f) ++first;
    CHECK(first == 300 || first == 301);
    std::fprintf(stderr, "  click at sample %zu (expected 300)\n", first);
}

LAB_TEST(platform_engine_governor_bounds_the_cost) {
    std::vector<uint8_t> mem(8u << 20);
    lab::Arena a;
    a.attach(mem.data(), uint32_t(mem.size()));
    plat::Engine e;
    CHECK(e.init(a));
    // 16 heavy stereo-sample voices with drive + sends: far beyond the budget
    std::vector<int16_t> smp(44100 * 2, 1000);
    plat::Cmd c;
    c.op = plat::Cmd::kSlot;
    c.a = 0;
    c.b = 2;
    c.ptr = smp.data();
    c.frames = 44100;
    e.applyNow(c);
    for (int v = 0; v < plat::kVoices; ++v) {
        auto set = [&](int p, float x) {
            plat::Cmd s;
            s.op = plat::Cmd::kSet;
            s.a = uint8_t(v);
            s.b = uint8_t(p);
            s.v = x;
            e.applyNow(s);
        };
        set(plat::pWave, float(plat::kSample));
        set(plat::pDrive, 1.0f);
        set(plat::pSendReverb, 1.0f);
        set(plat::pLoop, 1.0f);
        set(plat::pSustain, 1.0f);
        plat::Cmd on;
        on.op = plat::Cmd::kOn;
        on.a = uint8_t(v);
        on.v = 60.0f;
        on.frames = 65535;
        e.applyNow(on);
    }
    lab::Transport t;
    float l[lab::kBlock], r[lab::kBlock];
    for (int b = 0; b < 10; ++b) e.render(t, l, r);
    CHECK(e.lastCost() <= plat::Engine::kBudgetInsn);
    CHECK(e.overloadBlocks() > 0);
}

// ---- .papp relocation (host side) -------------------------------------------------------
LAB_TEST(platform_papp_relocates_for_any_address) {
    const std::string img = slurp("../build/sdk-test/app.papp");
    if (img.empty()) {
        std::fprintf(stderr, "  (no build/sdk-test/papp.papp: run make -C tests papp-qemu first) skipped\n");
        return;
    }
    const uint8_t* f = reinterpret_cast<const uint8_t*>(img.data());
    plat::PappHeader h;
    CHECK(plat::pappParseHeader(f, uint32_t(img.size()), h) == plat::PappStatus::Ok);
    CHECK(std::strcmp(h.info.name, "LoaderTest") == 0);
    std::vector<uint8_t> a(h.imageSize + h.bssSize), b(h.imageSize + h.bssSize), fa(h.fastSize + 4), fb(h.fastSize + 4);
    CHECK(plat::pappPlace(f, uint32_t(img.size()), h, a.data(), fa.data(), 0x70100000u, 0x1FFF4000u) == plat::PappStatus::Ok);
    CHECK(plat::pappPlace(f, uint32_t(img.size()), h, b.data(), fb.data(), 0x70200010u, 0x1FFF4000u) == plat::PappStatus::Ok);
    // every relocated word differs by exactly the load offset; nothing else differs
    uint32_t relocated = 0;
    for (uint32_t i = 0; i + 4 <= h.imageSize; i += 4) {
        uint32_t wa, wb;
        std::memcpy(&wa, &a[i], 4);
        std::memcpy(&wb, &b[i], 4);
        if (wa != wb) {
            CHECK(wb - wa == 0x00100010u);
            ++relocated;
        }
    }
    CHECK(relocated > 10);
    // damaged images are refused
    std::string bad = img;
    bad[bad.size() - 5] ^= 1;
    CHECK(plat::pappPlace(reinterpret_cast<const uint8_t*>(bad.data()), uint32_t(bad.size()), h, a.data(), fa.data(),
                          0x70100000u, 0x1FFF4000u) == plat::PappStatus::BadCrc);
    std::string shortImg = img.substr(0, 100);
    CHECK(plat::pappParseHeader(reinterpret_cast<const uint8_t*>(shortImg.data()), 100, h) == plat::PappStatus::TooShort);
}

// ---- native app in the host build (dlopen) ------------------------------------------------
LAB_TEST(platform_native_app_runs_from_the_card) {
    const std::string so = slurp("../build/sdk/drone.so");
    if (so.empty()) {
        std::fprintf(stderr, "  (no build/sdk/drone.so: run make -C sdk host first) skipped\n");
        return;
    }
    Rig r;
    r.fs.files["Lab/PLATFORM/Apps/drone.so"].assign(so.begin(), so.end());
    r.boot();
    if (!r.ok) return;
    bool found = false;
    for (int i = 0; i < r.ws->entryCount(); ++i)
        if (!std::strcmp(r.ws->entry(i).name, "Drone")) found = r.ws->entry(i).kind == plat::Entry::kApp;
    CHECK(found);
    CHECK(r.ws->openByName("drone.so"));
    CHECK(r.ws->mode() == plat::Platform::kAppRun);
    lab::Event e;
    e.type = lab::Event::kPadDown;
    e.id = 2;
    r.ws->onEvent(e);
    CHECK(r.run(200) > 0.01f);
    // LABKIT: the app's params (PlatHost::paramAdd), its page and mod source
    lab::kit::Kit& K = r.shell.labkit();
    const int base = int(r.ws->kitBaseParams());
    CHECK(K.reg.count() == base + 4);
    const lab::kit::ParamId cut = K.reg.find("drone.cutoff");
    CHECK(K.reg.valid(cut) && K.reg.curve(cut) == lab::kit::Curve::kExp);
    CHECK(K.nav.pageCount() == 3 && !std::strcmp(K.nav.pageName(0), "DRONE"));
    CHECK(K.mod.findSource("DRONE") >= 0);
    CHECK(K.mod.route(K.mod.findSource("LFO1"), cut, 0.4f) >= 0);
    K.reg.set(cut, 3000.0f);
    CHECK(r.run(20) > 0.0f);
    r.ws->closeSession(true);
    r.run(1);
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/drone/state.bin") == 1);   // save() on close
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/drone/labkit.lkc") == 1);  // its params, routes, macros
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
    CHECK(K.reg.count() == base && K.mod.routeCount() == 0 && K.mod.findSource("DRONE") < 0);
    r.run(4);   // the registry no longer writes into the unloaded app (ASan)
    CHECK(r.ws->openByName("drone.so"));
    r.run(2);
    CHECK(K.reg.valid(K.reg.find("drone.cutoff")) && std::fabs(K.reg.get(K.reg.find("drone.cutoff")) - 3000.0f) < 2.0f);
    CHECK(K.mod.routeCount() == 1);
    r.ws->closeSession(true);
    r.run(1);
}

// ---- crash guard, safe boot -----------------------------------------------------------------
LAB_TEST(platform_crash_guard_and_safe_boot) {
    // 1. last session reopened after the boot countdown
    {
        Rig r;
        r.addScript("a.lua", "function init() end\n");
        r.fs.files["Lab/PLATFORM/state.txt"] = std::vector<uint8_t>();
        const std::string st = "last=a.lua\nopen=\nauto=1\nheap=3\nbar=1\n";
        r.fs.files["Lab/PLATFORM/state.txt"].assign(st.begin(), st.end());
        r.boot();
        if (!r.ok) return;
        r.run(2);
        CHECK(r.ws->bootPending());
        r.run(600);   // > 1.5 s
        CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    }
    // 2. a key during the countdown = safe boot: stays in the launcher
    {
        Rig r;
        r.addScript("a.lua", "function init() end\n");
        const std::string st = "last=a.lua\nopen=\nauto=1\nheap=3\nbar=1\n";
        r.fs.files["Lab/PLATFORM/state.txt"].assign(st.begin(), st.end());
        r.boot();
        if (!r.ok) return;
        r.run(2);
        r.ws->onEvent(press(lab::btn::Down));
        r.run(600);
        CHECK(r.ws->mode() == plat::Platform::kLauncher);
        CHECK(r.ws->safeBoot());
    }
    // 3. the open= flag of a session that never got past its first seconds: not reopened
    {
        Rig r;
        r.addScript("a.lua", "function init() end\n");
        const std::string st = "last=a.lua\nopen=a.lua\nauto=1\nheap=3\nbar=1\n";
        r.fs.files["Lab/PLATFORM/state.txt"].assign(st.begin(), st.end());
        r.boot();
        if (!r.ok) return;
        r.run(600);
        CHECK(r.ws->mode() == plat::Platform::kLauncher);
        const auto& f = r.fs.files["Lab/PLATFORM/state.txt"];
        const std::string now(f.begin(), f.end());
        CHECK(now.find("open=\n") != std::string::npos);   // cleared
    }
    // 4. the flag is written while a session starts and cleared after a few seconds
    {
        Rig r;
        r.addScript("a.lua", "function init() end\n");
        r.boot();
        if (!r.ok) return;
        r.run(2);
        CHECK(r.ws->openByName("a.lua"));
        auto state = [&]() {
            const auto& f = r.fs.files["Lab/PLATFORM/state.txt"];
            return std::string(f.begin(), f.end());
        };
        CHECK(state().find("open=a.lua") != std::string::npos);
        r.run(1500);   // 4.5 s
        CHECK(state().find("open=\n") != std::string::npos);
    }
}

LAB_TEST(platform_exit_combos_return_to_the_launcher) {
    // Real Tracker keys only: Shift + F8 held = launcher, Shift + F7 held = LOG.
    // A tap of either still reaches the script (on the release).
    Rig r;
    r.addScript("a.lua", "function init() end\nfunction key(n, z, s) print('key', n, z) end\n");
    r.boot();
    if (!r.ok) return;
    r.run(2);
    CHECK(r.ws->openByName("a.lua"));
    plat::LuaSession& L = r.ws->lua();
    r.ws->onEvent(press(lab::btn::F8, true));
    CHECK(!consoleHas(L, "key F8 1"));   // held back until the release / hold
    r.ws->onEvent(release(lab::btn::F8, true));
    CHECK(consoleHas(L, "key F8 1") && consoleHas(L, "key F8 0"));
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    // held: back to the launcher, the release is swallowed
    r.ws->onEvent(press(lab::btn::F8, true));
    r.ws->onEvent(hold(lab::btn::F8));
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
    r.ws->onEvent(release(lab::btn::F8, true));
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
    // Shift + F7 held: the LOG view over the running script; F8 closes it
    CHECK(r.ws->openByName("a.lua"));
    r.ws->onEvent(press(lab::btn::F7, true));
    r.ws->onEvent(hold(lab::btn::F7));
    r.ws->onEvent(release(lab::btn::F7, true));
    CHECK(r.ws->logOpen());
    r.ws->onEvent(press(lab::btn::Up));   // scrolls, does not reach the script
    CHECK(!consoleHas(L, "key UP"));
    r.ws->onEvent(press(lab::btn::F8));
    CHECK(!r.ws->logOpen());
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    // keys that are not on the Tracker do nothing (no callback, no exit)
    r.ws->onEvent(press(lab::btn::Aux));
    r.ws->onEvent(press(lab::btn::Help, true));
    r.ws->onEvent(press(lab::btn::EncPush));
    r.run(400);
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    CHECK(!consoleHas(L, "key ?"));
}

// No source uses a key the original Tracker does not have (Aux 0x22, Help
// 0x2b, the encoder push 0x21): PLATFORM, the SDK examples, the station glue,
// the example scripts and the manual.
LAB_TEST(platform_uses_only_real_tracker_keys) {
    namespace fs = std::filesystem;
    // a token counts when the next character cannot continue an identifier
    auto uses = [](const std::string& text, const char* tok) {
        for (size_t at = text.find(tok); at != std::string::npos; at = text.find(tok, at + 1)) {
            const char c = at + std::strlen(tok) < text.size() ? text[at + std::strlen(tok)] : ' ';
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return true;
        }
        return false;
    };
    static const char* const kCpp[] = {"btn::Aux", "btn::Help", "btn::EncPush", "PLAT_BTN_AUX", "PLAT_BTN_HELP",
                                       "PLAT_BTN_ENCPUSH"};
    static const char* const kLua[] = {"\"ENC\"", "\"AUX\"", "\"HELP\"", "'ENC'", "'AUX'", "'HELP'"};
    int files = 0;
    for (const char* dir : {"../src/ws", "../sdk/examples", "../sdk/station", "../sdk/include", "../examples"}) {
        if (!fs::exists(dir)) continue;
        for (const auto& f : fs::recursive_directory_iterator(dir)) {
            if (!f.is_regular_file()) continue;
            const std::string p = f.path().string(), ext = f.path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".lua") continue;
            const std::string text = slurp(p);
            ++files;
            bool hit = false;
            for (const char* t : kCpp) hit = hit || uses(text, t);
            if (ext == ".lua")
                for (const char* t : kLua) hit = hit || text.find(t) != std::string::npos;
            if (hit) std::fprintf(stderr, "  %s uses a key the Tracker does not have\n", p.c_str());
            CHECK(!hit);
        }
    }
    CHECK(files > 20);
    // the Lua key table has no such names either
    CHECK(!std::strcmp(plat::keyName(lab::btn::Aux), "?"));
    CHECK(!std::strcmp(plat::keyName(lab::btn::Help), "?"));
    CHECK(!std::strcmp(plat::keyName(lab::btn::EncPush), "?"));
    CHECK(!std::strcmp(plat::keyName(lab::btn::Insert), "INSERT"));
}

// LABKIT v2 in scripts: params:add{...} in the shared registry with pages,
// mod.source / mod.route / macros, saved per script, and the kit put back as
// it was when the session closes.
LAB_TEST(platform_lua_params_and_mod_live_in_labkit) {
    Rig r;
    r.addScript("k.lua", R"(
function init()
  params:add{id = "cut", name = "Cutoff", min = 100, max = 8000, default = 1000, unit = "Hz", page = "FILTER",
             action = function(v) lastCut = v end}
  params:add{id = "res", name = "Reso", min = 0, max = 1, default = 0.2, page = "FILTER"}
  params:add{id = "mode", name = "Mode", kind = "option", options = {"a", "b", "c"}, default = 2, page = "SEQ"}
  env = mod.source("ENV")
  mod.route("LFO1", "cut", 0.5)
  mod.macro_target(1, "res", 1)
  mod.macro_name(1, "OPEN")
end
function redraw() end
)");
    r.boot();
    if (!r.ok) return;
    r.run(2);
    lab::kit::Kit& K = r.shell.labkit();
    CHECK(K.active);   // the launcher has a page: the navigation is on from boot
    const int base = K.reg.count();
    CHECK(int(r.ws->kitBaseParams()) == base);
    CHECK(r.ws->openByName("k.lua"));
    if (r.ws->mode() != plat::Platform::kScriptRun) {
        std::fprintf(stderr, "  error: %s\n", r.ws->lua().error());
        return;
    }
    r.run(4);
    CHECK(K.active);
    CHECK(K.reg.count() == base + 3);
    const lab::kit::ParamId cut = K.reg.find("lua.cut"), res = K.reg.find("lua.res"), mode = K.reg.find("lua.mode");
    CHECK(K.reg.valid(cut) && K.reg.valid(res) && K.reg.valid(mode));
    CHECK(K.reg.page(cut) == 0 && K.reg.page(mode) == 1);
    CHECK(K.nav.pageCount() == 4);   // FILTER, SEQ, MACRO, MOD
    CHECK(!std::strcmp(K.nav.pageName(0), "FILTER") && !std::strcmp(K.nav.pageName(1), "SEQ"));
    CHECK(K.mod.routeCount() == 2);
    CHECK(K.mod.findSource("ENV") >= 0);
    CHECK(!std::strcmp(K.mod.macroName(0), "OPEN"));
    CHECK(K.reg.enumCount(mode) == 3 && std::fabs(K.reg.get(mode) - 1.0f) < 0.01f);   // Lua 2 = registry 1
    // a change from the Shell side (pads / MAP) reaches the script's action
    K.reg.set(cut, 2000.0f);
    r.run(8);
    // (actions get the effective value: base + the LFO route)
    CHECK(std::fabs(K.reg.get(cut) - 2000.0f) < 0.5f);
    r.ws->lua().eval("math.abs(lastCut - 1000) > 50");
    CHECK(consoleHas(r.ws->lua(), "true"));
    // the REPL: a param id alone shows it, id(v) sets it
    CHECK(r.ws->lua().eval("res(0.5)"));
    r.run(2);
    CHECK(std::fabs(K.reg.get(res) - 0.5f) < 0.01f);
    CHECK(consoleHas(r.ws->lua(), "res = "));
    // close: saved per script, the kit back to the launcher's state
    r.ws->closeSession(true);
    r.run(2);
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/k/labkit.lkc") == 1);
    CHECK(K.reg.count() == base);
    CHECK(K.mod.routeCount() == 0);
    CHECK(K.mod.findSource("ENV") < 0);
    CHECK(std::strcmp(K.mod.macroName(0), "OPEN") != 0);
    CHECK(K.nav.pageCount() == 3 && !std::strcmp(K.nav.pageName(0), "LAUNCHER"));
    for (int b = 0; b < 4; ++b) r.run(1);   // no write into the freed session memory (ASan)
    // reopen: values, routes and macro names come back from labkit.lkc
    CHECK(r.ws->openByName("k.lua"));
    r.run(2);
    const lab::kit::ParamId cut2 = K.reg.find("lua.cut");
    CHECK(K.reg.valid(cut2) && std::fabs(K.reg.get(cut2) - 2000.0f) < 1.0f);
    CHECK(K.mod.routeCount() == 2);
    CHECK(!std::strcmp(K.mod.macroName(0), "OPEN"));
    r.ws->closeSession(true);
    r.run(1);
}

// The crash-safe runner: an error in a callback is logged and the script keeps
// running; more than kMaxErrors within kErrorWindowMs stop it (error page).
LAB_TEST(platform_crash_safe_runner) {
    Rig r;
    r.addScript("c.lua", "function init() end\nfunction key(n, z) if z == 1 then error('boom ' .. n) end end\n");
    r.boot();
    if (!r.ok) return;
    r.run(2);
    CHECK(r.ws->openByName("c.lua"));
    for (int i = 0; i < 3; ++i) r.ws->onEvent(press(lab::btn::Up));
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kScriptRun);
    CHECK(r.ws->lua().errorsTotal() == 3);
    CHECK(std::strstr(r.ws->lua().lastError(), "boom UP") != nullptr);
    CHECK(consoleHas(r.ws->lua(), "boom UP"));
    for (int i = 0; i < 3; ++i) r.ws->onEvent(press(lab::btn::Down));
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kError);
    CHECK(std::strstr(r.ws->lua().error(), "boom DOWN") != nullptr);
    // the error page: F1 shows the LOG, F8 the launcher
    r.ws->onEvent(press(lab::btn::F1));
    CHECK(r.ws->logOpen());
    r.ws->onEvent(press(lab::btn::F8));
    CHECK(!r.ws->logOpen());
    r.ws->onEvent(press(lab::btn::F8));
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
    // an endless loop in a callback is never "keep going": stopped at once
    r.addScript("hang.lua", "function pad(n, v, z) if z == 1 then while true do end end end\n");
    r.ws->rescan();
    CHECK(r.ws->openByName("hang.lua"));
    lab::Event pd;
    pd.type = lab::Event::kPadDown;
    pd.id = 0;
    pd.velocity = 100;
    r.ws->onEvent(pd);
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kError);
    CHECK(std::strstr(r.ws->lua().error(), "too long") != nullptr);
}

// ---- apps shipped next to the image: /Firmware/Apps -> /Lab/PLATFORM/Apps at boot ----
LAB_TEST(platform_installs_apps_from_firmware_folder) {
    const std::string so = slurp("../build/sdk/drone.so");
    if (so.empty()) return;
    Rig r;
    r.fs.files["Firmware/Apps/drone.so"].assign(so.begin(), so.end());
    r.fs.files["Firmware/Apps/notes.txt"] = {1, 2, 3};   // not an app: ignored
    r.boot();
    if (!r.ok) return;
    CHECK(r.fs.files.count("Lab/PLATFORM/Apps/drone.so") == 1);
    CHECK(r.fs.files["Lab/PLATFORM/Apps/drone.so"].size() == so.size());
    CHECK(r.fs.files.count("Lab/PLATFORM/Apps/notes.txt") == 0);
    CHECK(r.fs.files.count("Lab/PLATFORM/Apps/drone.so.part") == 0);
    bool listed = false;
    for (int i = 0; i < r.ws->entryCount(); ++i) listed |= !std::strcmp(r.ws->entry(i).name, "Drone");
    CHECK(listed);
    // a second boot with the same file copies nothing (same size)
    r.fs.files["Lab/PLATFORM/Apps/drone.so"][0] ^= 0;   // untouched
    Rig r2;
    r2.fs = r.fs;
    r2.boot();
    CHECK(r2.fs.files["Lab/PLATFORM/Apps/drone.so"] == r.fs.files["Lab/PLATFORM/Apps/drone.so"]);
}
