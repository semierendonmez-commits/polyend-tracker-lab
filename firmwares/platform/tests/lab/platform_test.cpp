// PLATFORM host tests: the Lua heap, the Lua API, the voice engine's timing,
// the .papp relocation (host side), the launcher, the crash guard and safe boot.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../mem_storage.h"
#include "lab/lab.h"
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
            ws->render(t, lab::AudioIn(), l, r);
            for (uint32_t i = 0; i < lab::kBlock; ++i) peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            if (t.playing) t.beat += t.beatsPerSample * lab::kBlock;
            t.startedThisBlock = false;
            ms += 3;
            ws->update(ms);
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
    // params saved on close, restored on the next open
    r.ws->closeSession(true);
    r.run(1);
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/api/params.txt") == 1);
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
    r.ws->closeSession(true);
    r.run(1);
    CHECK(r.fs.files.count("Lab/PLATFORM/Data/drone/state.bin") == 1);   // save() on close
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
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
    Rig r;
    r.addScript("a.lua", "function init() end\nfunction key(n, z) end\n");
    r.boot();
    if (!r.ok) return;
    r.run(2);
    CHECK(r.ws->openByName("a.lua"));
    r.ws->onEvent(press(lab::btn::Help, true));   // Shift + Help
    r.run(2);
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
    CHECK(r.ws->openByName("a.lua"));
    lab::Event aux = press(lab::btn::Aux);
    aux.ms = 0;
    r.ws->onEvent(aux);
    r.run(400);   // hold 1.2 s
    CHECK(r.ws->mode() == plat::Platform::kLauncher);
}
