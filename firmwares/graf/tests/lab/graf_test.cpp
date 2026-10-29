// GRAF host tests: patch graph logic, module DSP sanity, snapshot morph and
// crossfader, song timeline, save / load roundtrip, MIDI voices, the CPU budget
// guard, undo / redo, tidy, templates, the Plaits pool, LABKIT params / routes / taps.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lab/lab.h"
#include "lab_test.h"
#include "ws/graf_engine.h"
#include "ws/graf_modules.h"
#include "ws/graf_ws.h"

using namespace graf;

namespace {
struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(40u * 1024u * 1024u);
    lab::Arena arena;
    Engine e;
    bool ok = false;
    Rig() {
        arena.attach(mem.data(), uint32_t(mem.size()));
        ok = e.init(arena);
        e.setConcurrent(false);
        e.clear();
        e.quiesce();
    }
    // Renders n blocks; returns the peak, false in *finite on NaN / inf.
    float run(int blocks, bool* finite = nullptr, bool playing = true, float* rms = nullptr) {
        lab::Transport t;
        t.playing = playing;
        t.bpm = 120.0f;
        t.beatsPerSample = 120.0 / 60.0 / 44100.0;
        lab::AudioIn in;
        float l[lab::kBlock], r[lab::kBlock], pk = 0.0f;
        double sum = 0.0;
        bool fin = true;
        for (int b = 0; b < blocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            t.startedThisBlock = b == 0;
            e.render(t, in, l, r, nullptr);
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i])) fin = false;
                pk = std::fmax(pk, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
                sum += double(l[i]) * double(l[i]);
            }
            t.beat += t.beatsPerSample * lab::kBlock;
            e.service(uint32_t(b * 3));
        }
        if (finite) *finite = fin;
        if (rms) *rms = float(std::sqrt(sum / double(blocks * int(lab::kBlock))));
        return pk;
    }
    void setP(int s, int p, float v) { e.setParamK(s, p, Engine::unmapParam(def(e.mod(s).type).p[p], v)); }
};

// Peak of one output of a module after n blocks.
float outPeak(Rig& r, int slot, int out, int blocks, bool* finite) {
    lab::Transport t;
    t.playing = true;
    t.bpm = 120.0f;
    t.beatsPerSample = 120.0 / 60.0 / 44100.0;
    lab::AudioIn in;
    float l[lab::kBlock], rr[lab::kBlock], pk = 0.0f;
    *finite = true;
    r.e.allOutputs = true;
    for (int b = 0; b < blocks; ++b) {
        t.startedThisBlock = b == 0;
        r.e.render(t, in, l, rr, nullptr);
        const float* o = r.e.mod(slot).out[out];
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            if (!std::isfinite(o[i])) *finite = false;
            pk = std::fmax(pk, std::fabs(o[i]));
        }
        t.beat += t.beatsPerSample * lab::kBlock;
    }
    r.e.allOutputs = false;
    return pk;
}
}  // namespace

LAB_TEST(graf_math_basics) {
    initMath();
    CHECK_NEAR(fexp2(0.0f), 1.0f, 1e-4);
    CHECK_NEAR(fexp2(1.0f), 2.0f, 1e-3);
    CHECK_NEAR(fexp2(-1.5f), 0.35355f, 1e-3);
    CHECK_NEAR(pitchHz(0.0f), 261.63f, 0.1);
    CHECK_NEAR(pitchHz(0.75f), 440.0f, 0.3);
    CHECK_NEAR(sin01(0.25f), 1.0f, 1e-3);
    CHECK_NEAR(sin01(-0.25f), -1.0f, 1e-3);
    CHECK_NEAR(tanPi(1000.0f), std::tan(3.14159265 * 1000.0 / 44100.0), 1e-4);
    CHECK_NEAR(quantizeSemi(1.2f, 1), 2.0f, 1e-6);    // major: C# -> D
    CHECK_NEAR(quantizeSemi(-0.6f, 1), -1.0f, 1e-6);  // B below
    CHECK(scaleDegree(1, 7) == 12);
    CHECK(scaleDegree(1, -1) == -1);
    CHECK(scaleDegree(5, 5) == 12);
}

LAB_TEST(graf_param_mapping_roundtrip) {
    for (int t = 0; t < kTypeCount; ++t) {
        const ModDef& d = def(t);
        CHECK(d.nIn <= kMaxIn && d.nOut <= kMaxOut && d.nParam <= kMaxParam);
        CHECK(d.process != nullptr);
        CHECK(d.cost > 0);
        for (int p = 0; p < d.nParam; ++p) {
            const ParamDef& pd = d.p[p];
            CHECK(pd.max > pd.min);
            CHECK(pd.def >= pd.min - 1e-6f && pd.def <= pd.max + 1e-6f);
            const float k = Engine::unmapParam(pd, pd.def);
            CHECK_NEAR(Engine::mapParam(pd, k), pd.def, std::fabs(pd.def) * 1e-3 + 1e-3);
        }
    }
}

LAB_TEST(graf_graph_add_connect_remove) {
    Rig r;
    CHECK(r.ok);
    const int a = r.e.add(kVco, 0, 5);
    const int b = r.e.add(kVca, 0, 5);   // cell taken: next free
    CHECK(a >= 0 && b >= 0);
    CHECK(r.e.mod(a).cell == 5);
    CHECK(r.e.mod(b).cell == 0);
    CHECK(r.e.mod(a).number == 1);
    const int a2 = r.e.add(kVco, 0);
    CHECK(r.e.mod(a2).number == 2);
    const int c = r.e.connect(a, 0, b, 0, 0.5f);
    CHECK(c >= 0);
    CHECK(r.e.connect(a, 0, b, 0, 0.7f) == c);   // same jack pair: updates
    CHECK_NEAR(r.e.cable(c).amount, 0.7f, 1e-6);
    CHECK(r.e.connect(a, 9, b, 0, 1.0f) < 0);    // no such output
    CHECK(r.e.cableCount() == 1);
    r.e.remove(a);
    CHECK(r.e.cableCount() == 0);
    CHECK(r.e.moduleCount() == 2);
    r.e.quiesce();
    // removed slot reusable
    const int d = r.e.add(kLfo, 0);
    CHECK(d >= 0);
    // 8-patch model: no cables across patches
    r.e.rack = false;
    const int p1 = r.e.add(kVco, 1);
    CHECK(r.e.connect(p1, 0, b, 0, 1.0f) < 0);
    r.e.rack = true;
    CHECK(r.e.connect(p1, 0, b, 0, 1.0f) >= 0);
}

LAB_TEST(graf_feedback_loop_is_stable) {
    Rig r;
    const int vco = r.e.add(kVco, 0);
    const int f = r.e.add(kSvf, 0);
    const int d = r.e.add(kDelay, 0);
    const int o = r.e.add(kOut, 0);
    r.e.connect(vco, 0, f, 0, 1.0f);
    r.e.connect(f, 0, d, 0, 1.0f);
    r.e.connect(d, 0, f, 0, 1.2f);   // loop: delay back into the filter
    r.e.connect(d, 0, vco, 1, 1.0f); // and into the VCO's FM
    r.e.connect(d, 0, o, 0, 1.0f);
    r.e.rebuild();
    bool fin = false;
    const float pk = r.run(2000, &fin);
    CHECK(fin);
    CHECK(pk > 0.01f);
    CHECK(pk < 4.0f);
}

LAB_TEST(graf_every_module_sane) {
    // Each module fed by a VCO saw, an LFO and a clock on every input type;
    // outputs must stay finite and within a sensible range.
    for (int t = 0; t < kTypeCount; ++t) {
        Rig r;
        const int vco = r.e.add(kVco, 0, 0);
        const int lfo = r.e.add(kLfo, 0, 1);
        const int clk = r.e.add(kClk, 0, 2);
        const int mi = r.e.add(kMidi, 0, 3);
        const int m = r.e.add(t, 0, 10);
        CHECK(m >= 0);
        if (m < 0) continue;
        const ModDef& d = def(t);
        for (int i = 0; i < d.nIn; ++i) {
            const int sig = d.in[i].sig;
            if (sig == kAudio) r.e.connect(vco, 0, m, i, 0.8f);
            else if (sig == kGate) r.e.connect(clk, 0, m, i, 1.0f);
            else if (sig == kPitch) r.e.connect(mi, 0, m, i, 1.0f);
            else r.e.connect(lfo, 0, m, i, 0.5f);
        }
        r.e.rebuild();
        r.e.noteOn(0, 57, 100);
        // a sampler needs its bank: the built-in sound
        if (t == kSmpl) CHECK(r.e.loadSample(m, nullptr, ""));
        for (int o = 0; o < d.nOut; ++o) {
            bool fin = true;
            const float pk = outPeak(r, m, o, 400, &fin);
            CHECK(fin);
            const float lim = d.out[o].sig == kPitch ? 8.0f : 4.0f;
            if (!(pk <= lim)) std::fprintf(stderr, "  %s out %s peak %f\n", d.tag, d.out[o].name, double(pk));
            CHECK(pk <= lim);
        }
        // sources make a signal
        if (t == kVco || t == kCplx || t == kWtbl || t == kNoise || t == kRete || t == kSmpl || t == kLfo || t == kPlts ||
            t == kChaos || t == kNet) {
            bool fin = true;
            CHECK(outPeak(r, m, 0, 400, &fin) > 0.05f);
        }
    }
}

LAB_TEST(graf_vco_pitch) {
    Rig r;
    const int v = r.e.add(kVco, 0);
    r.setP(v, 0, 0.0f);
    r.setP(v, 1, 9.0f);   // A4 = 440 Hz
    r.e.rebuild();
    // count zero crossings of the sine over 1 s
    lab::Transport t;
    lab::AudioIn in;
    float l[lab::kBlock], rr[lab::kBlock];
    int cross = 0;
    float prev = 0.0f;
    r.e.allOutputs = true;
    for (int b = 0; b < 345; ++b) {
        r.e.render(t, in, l, rr, nullptr);
        const float* o = r.e.mod(v).out[3];
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            if (prev < 0.0f && o[i] >= 0.0f) ++cross;
            prev = o[i];
        }
    }
    CHECK(cross >= 438 && cross <= 442);
}

LAB_TEST(graf_ladder_resonance_and_filtering) {
    Rig r;
    const int n = r.e.add(kNoise, 0);
    const int f = r.e.add(kLadder, 0);
    const int o = r.e.add(kOut, 0);
    r.e.connect(n, 0, f, 0, 1.0f);
    r.e.connect(f, 0, o, 0, 1.0f);
    r.setP(f, 0, 200.0f);
    r.setP(f, 1, 0.0f);
    r.e.rebuild();
    float rmsLow = 0, rmsHigh = 0;
    bool fin = true;
    r.run(400, &fin, true, &rmsLow);
    r.setP(f, 0, 12000.0f);
    r.run(400, &fin, true, &rmsHigh);
    CHECK(fin);
    CHECK(rmsHigh > rmsLow * 2.0f);   // a low cutoff removes most of white noise
    // self-oscillation at full resonance, no input
    r.e.disconnect(r.e.cableFind(n, 0, f, 0));
    r.setP(f, 0, 800.0f);
    r.setP(f, 1, 1.05f);
    r.e.rebuild();
    const float pk = r.run(800, &fin);
    CHECK(fin);
    CHECK(pk < 3.0f);
}

LAB_TEST(graf_sequencers_make_gates) {
    const int types[] = {kSeq, kEucl, kTuring, kMarkov, kPinb, kClk};
    for (int t : types) {
        Rig r;
        const int m = r.e.add(t, 0);
        r.e.rebuild();
        const int gateOut = t == kSeq ? 1 : (t == kEucl ? 0 : (t == kTuring ? 3 : (t == kMarkov ? 1 : (t == kPinb ? 1 : 0))));
        lab::Transport tr;
        tr.playing = true;
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
        lab::AudioIn in;
        float l[lab::kBlock], rr[lab::kBlock];
        int rises = 0;
        float prev = 0.0f;
        r.e.allOutputs = true;
        for (int b = 0; b < 1378; ++b) {   // 4 s = 8 beats = 32 sixteenths
            tr.startedThisBlock = b == 0;
            r.e.render(tr, in, l, rr, nullptr);
            const float* o = r.e.mod(m).out[gateOut];
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                if (prev < 0.5f && o[i] >= 0.5f) ++rises;
                prev = o[i];
            }
            tr.beat += tr.beatsPerSample * lab::kBlock;
        }
        std::fprintf(stderr, "  %s: %d gates in 8 beats\n", def(t).tag, rises);
        CHECK(rises >= 3);
        CHECK(rises <= 40);
    }
}

LAB_TEST(graf_euclid_pattern) {
    Rig r;
    const int m = r.e.add(kEucl, 0);
    r.setP(m, 0, 8.0f);   // steps
    r.setP(m, 1, 3.0f);   // hits
    r.setP(m, 3, 4.0f);   // 1/16
    r.e.rebuild();
    lab::Transport tr;
    tr.playing = true;
    tr.bpm = 120.0f;
    tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
    lab::AudioIn in;
    float l[lab::kBlock], rr[lab::kBlock];
    int rises = 0;
    float prev = 0.0f;
    r.e.allOutputs = true;
    for (int b = 0; b < 689; ++b) {   // 2 s = 4 beats = 16 sixteenths = 2 cycles
        tr.startedThisBlock = b == 0;
        r.e.render(tr, in, l, rr, nullptr);
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            const float v = r.e.mod(m).out[0][i];
            if (prev < 0.5f && v >= 0.5f) ++rises;
            prev = v;
        }
        tr.beat += tr.beatsPerSample * lab::kBlock;
    }
    CHECK(rises == 6);
}

LAB_TEST(graf_midi_voices) {
    Rig r;
    const int m1 = r.e.add(kMidi, 0);
    const int m2 = r.e.add(kMidi, 0);
    r.setP(m2, 0, 2.0f);
    r.e.rebuild();
    CHECK(r.e.voices(0).count == 2);
    r.e.noteOn(0, 60, 100);
    r.e.noteOn(0, 67, 80);
    r.run(2);
    CHECK_NEAR(r.e.mod(m1).out[0][lab::kBlock - 1], 0.0f, 1e-4);
    CHECK_NEAR(r.e.mod(m2).out[0][lab::kBlock - 1], 7.0f / 12.0f, 1e-4);
    CHECK(r.e.mod(m1).out[1][lab::kBlock - 1] > 0.5f);
    r.e.noteOff(0, 60);
    r.run(2);
    CHECK(r.e.mod(m1).out[1][lab::kBlock - 1] < 0.5f);
    CHECK(r.e.mod(m2).out[1][lab::kBlock - 1] > 0.5f);
    // mono stack: last note priority, falls back to the held note
    Rig s;
    const int m = s.e.add(kMidi, 0);
    s.e.rebuild();
    s.e.noteOn(0, 48, 100);
    s.e.noteOn(0, 55, 100);
    s.e.noteOff(0, 55);
    s.run(2);
    CHECK_NEAR(s.e.mod(m).out[0][lab::kBlock - 1], -1.0f, 1e-4);
    CHECK(s.e.mod(m).out[1][lab::kBlock - 1] > 0.5f);
    lab::MidiMsg cc;
    cc.status = 0xB0;
    cc.d1 = 1;
    cc.d2 = 127;
    s.e.midiIn(cc);
    s.run(1);
    CHECK_NEAR(s.e.mod(m).out[3][0], 1.0f, 1e-4);
}

LAB_TEST(graf_cpu_budget_guard) {
    Rig r;
    int added = 0;
    for (int i = 0; i < 64; ++i)
        if (r.e.add(kRete, i / kCells, i % kCells) >= 0) ++added;
    CHECK(added > 0);
    CHECK(added < 10);
    CHECK(r.e.cpuCost() <= kCpuBudget);
    CHECK(std::strstr(r.e.lastError(), "CPU") != nullptr);
}

LAB_TEST(graf_snapshot_morph) {
    Rig r;
    const int v = r.e.add(kVco, 0, 0);
    const int f = r.e.add(kSvf, 0, 1);
    const int o = r.e.add(kOut, 0, 2);
    r.e.connect(v, 0, f, 0, 1.0f);
    r.e.connect(f, 0, o, 0, 1.0f);
    r.setP(f, 0, 200.0f);
    r.e.rebuild();
    r.e.storeSnap(0);
    // B: cutoff up, a new LFO cable, the OUT removed and a DLY in another cell
    r.setP(f, 0, 5000.0f);
    const int lfo = r.e.add(kLfo, 0, 3);
    r.e.connect(lfo, 0, f, 2, 0.5f);
    r.e.storeSnap(1);
    const float kA = r.e.snap(0).mod[r.e.snap(0).find(1)].k[0];
    const float kB = r.e.snap(1).mod[r.e.snap(1).find(1)].k[0];
    CHECK(kB > kA);
    // back to A, then morph to B over 1000 ms
    r.e.recallSnap(0, 0, 0);
    CHECK(r.e.slotAtCell(0, 3) < 0);   // the LFO is gone at A
    CHECK_NEAR(r.e.mod(f).k[0], kA, 1e-5);
    r.e.recallSnap(1, 1000, 0);
    r.e.morphStep(500);
    CHECK_NEAR(r.e.mod(f).k[0], (kA + kB) * 0.5f, 1e-4);
    const int l2 = r.e.slotAtCell(0, 3);
    CHECK(l2 >= 0);   // modules of B appear at the start
    const int c = r.e.cableFind(l2, 0, f, 2);
    CHECK(c >= 0);
    CHECK_NEAR(r.e.cable(c).amount, 0.25f, 1e-4);   // fading in
    r.e.morphStep(1000);
    CHECK(!r.e.morphing());
    CHECK_NEAR(r.e.mod(f).k[0], kB, 1e-5);
    CHECK_NEAR(r.e.cable(c).amount, 0.5f, 1e-5);
    // and back to A: the LFO and its cable leave at the end
    r.e.recallSnap(0, 1000, 2000);
    r.e.morphStep(2999);
    CHECK(r.e.slotAtCell(0, 3) >= 0);
    r.e.morphStep(3000);
    CHECK(r.e.slotAtCell(0, 3) < 0);
    bool fin = false;
    r.run(100, &fin);
    CHECK(fin);
}

LAB_TEST(graf_song_timeline) {
    Rig r;
    const int f = r.e.add(kSvf, 0, 0);
    r.setP(f, 1, 0.0f);
    r.e.storeSnap(0);
    r.setP(f, 1, 1.0f);
    r.e.storeSnap(1);
    r.e.songLen = 2;
    r.e.song[0] = SongStep{0, 0, 2};   // A: cut, hold 2 bars
    r.e.song[1] = SongStep{1, 2, 2};   // B: morph 2 bars, hold 2
    CHECK(r.e.songBars() == 6);
    float t = 0;
    CHECK(r.e.songStepAt(0.0, &t, nullptr) == 0);
    CHECK(r.e.songStepAt(8.0, &t, nullptr) == 1);   // bar 2
    CHECK_NEAR(t, 0.0f, 1e-5);
    CHECK(r.e.songStepAt(12.0, &t, nullptr) == 1);   // bar 3: half way
    CHECK_NEAR(t, 0.5f, 1e-5);
    r.e.songApply(12.0);
    CHECK_NEAR(r.e.mod(f).k[1], 0.5f, 1e-4);
    r.e.songApply(18.0);
    CHECK_NEAR(r.e.mod(f).k[1], 1.0f, 1e-4);
    CHECK(r.e.songStepAt(24.0, &t, nullptr) == 0);   // loops after 6 bars
}

LAB_TEST(graf_save_load_roundtrip) {
    Rig r;
    const int s = r.e.add(kSeq, 2, 7);
    const int v = r.e.add(kCplx, 2, 8);
    const int o = r.e.add(kOut, 2, 9);
    r.e.connect(s, 0, v, 0, 1.0f);
    r.e.connect(v, 0, o, 0, -0.6f);
    r.setP(v, 2, 3.25f);
    seqNote(r.e.mod(s), 3) = 9;
    r.e.mix[2].level = 0.33f;
    r.e.rack = false;
    r.e.storeSnap(4);
    r.e.songLen = 3;
    r.e.song[2] = SongStep{4, 3, 5};
    r.e.songOn = true;
    std::vector<uint8_t> buf(200 * 1024);
    const uint32_t n = r.e.serialize(buf.data(), uint32_t(buf.size()));
    CHECK(n > 100);
    Rig q;
    CHECK(q.e.deserialize(buf.data(), n));
    CHECK(q.e.moduleCount() == 3);
    CHECK(q.e.cableCount() == 2);
    CHECK(!q.e.rack);
    CHECK(q.e.songOn);
    CHECK(q.e.songLen == 3);
    CHECK(q.e.song[2].snap == 4 && q.e.song[2].morphBars == 3 && q.e.song[2].holdBars == 5);
    CHECK_NEAR(q.e.mix[2].level, 0.33f, 1e-6);
    CHECK(q.e.snap(4).valid && q.e.snap(4).nMod == 3 && q.e.snap(4).nCab == 2);
    CHECK(!q.e.snap(0).valid);
    const int v2 = q.e.slotAtCell(2, 8);
    CHECK(v2 >= 0 && q.e.mod(v2).type == kCplx);
    if (v2 >= 0) CHECK_NEAR(q.e.mod(v2).v[2], 3.25f, 1e-3);
    const int s2 = q.e.slotAtCell(2, 7);
    if (s2 >= 0) CHECK(seqNote(q.e.mod(s2), 3) == 9);
    const int o2 = q.e.slotAtCell(2, 9);
    const int c = q.e.cableFind(v2, 0, o2, 0);
    CHECK(c >= 0);
    if (c >= 0) CHECK_NEAR(q.e.cable(c).amount, -0.6f, 1e-6);
    // the same bytes again
    std::vector<uint8_t> buf2(200 * 1024);
    const uint32_t n2 = q.e.serialize(buf2.data(), uint32_t(buf2.size()));
    CHECK(n2 == n);
    CHECK(std::memcmp(buf.data(), buf2.data(), n) == 0);
    // damaged files are refused, the patch stays
    buf[n / 2] ^= 0x55;
    CHECK(!q.e.deserialize(buf.data(), n));
    CHECK(q.e.moduleCount() == 3);
    CHECK(!q.e.deserialize(buf.data(), 10));
}

LAB_TEST(graf_workstation_pages_and_demos) {
    std::vector<uint8_t> mem(64u * 1024u * 1024u);
    lab::Shell shell;
    lab::Hal h;
    CHECK(shell.init(h, mem.data(), uint32_t(mem.size()), "GRAF"));
    auto* g = static_cast<Graf*>(shell.workstation());
    CHECK(g != nullptr);
    if (!g) return;
    static const int kDemoModules[] = {9, 9, 6, 14, 22, 16, 10, 10};
    CHECK(Graf::demoCount() == 8);
    for (int d = 0; d < Graf::demoCount(); ++d) {
        g->loadDemo(d);
        if (g->engine().moduleCount() != kDemoModules[d])
            std::fprintf(stderr, "  demo %d: %d modules\n", d, g->engine().moduleCount());
        CHECK(g->engine().moduleCount() == kDemoModules[d]);
        CHECK(g->engine().cpuCost() <= kCpuBudget);
        CHECK(g->engine().snap(0).valid);
    }
    // matrix pin toggles a cable
    g->loadDemo(0);
    lab::Event e;
    e.type = lab::Event::kPress;
    e.id = lab::btn::F2;
    g->onEvent(e);
    CHECK(g->page() == kPgMatrix);
    const int before = g->engine().cableCount();
    // find an empty pin on the first pad page
    int pad = -1;
    for (int p = 0; p < 48 && pad < 0; ++p) {
        const int r = p / 12, c = p % 12;
        if (r < g->nSrc() && c < g->nDst() &&
            g->engine().cableFind(g->src(r).slot, g->src(r).jack, g->dst(c).slot, g->dst(c).jack) < 0)
            pad = p;
    }
    CHECK(pad >= 0);
    lab::Event pd;
    pd.type = lab::Event::kPadDown;
    pd.id = uint8_t(pad);
    pd.velocity = 100;
    g->onEvent(pd);
    pd.type = lab::Event::kPadUp;
    g->onEvent(pd);
    CHECK(g->engine().cableCount() == before + 1);
    pd.type = lab::Event::kPadDown;
    g->onEvent(pd);
    pd.type = lab::Event::kPadUp;
    g->onEvent(pd);
    CHECK(g->engine().cableCount() == before);
    // model switch to 8 patches and back
    g->setModel(false);
    CHECK(!g->engine().rack);
    g->setModel(true);
    CHECK(g->engine().rack);
}

LAB_TEST(graf_bench_scenarios_within_budget_estimate) {
    std::vector<uint8_t> mem(64u * 1024u * 1024u);
    lab::Shell shell;
    lab::Hal h;
    CHECK(shell.init(h, mem.data(), uint32_t(mem.size()), "GRAF"));
    lab::Workstation* ws = shell.workstation();
    for (int s = 0; s < ws->benchScenarios(); ++s) {
        ws->benchSetup(s);
        auto* g = static_cast<Graf*>(ws);
        std::fprintf(stderr, "  %-14s estimate %6d insn, %2d modules, %3d cables\n", ws->benchName(s),
                     int(g->engine().cpuCost()), g->engine().moduleCount(), g->engine().cableCount());
        CHECK(g->engine().cpuCost() <= kCpuBudget);
    }
}

// The densest canvas the CPU budget admits (48 cheap modules, as many cables
// as fit). With GRAF_STRESS_OUT=<file> it is written out for the simulator's
// display-budget check (sim: File page, load P01).
LAB_TEST(graf_densest_patch_file) {
    Rig r;
    for (int c = 0; c < kCells; ++c) CHECK(r.e.add(c % 2 ? kRing : kMix, 0, c) >= 0);
    int cables = 0;
    for (int k = 0; k < 400; ++k) {
        const int a = r.e.slotAtCell(0, k % kCells), b = r.e.slotAtCell(0, (k * 7 + 5) % kCells);
        if (r.e.connect(a, k % 2, b, (k / 3) % 2, 0.5f) >= 0) ++cables;
    }
    CHECK(cables > 40);
    CHECK(r.e.cpuCost() <= kCpuBudget);
    bool fin = false;
    r.run(50, &fin);
    CHECK(fin);
    if (const char* out = std::getenv("GRAF_STRESS_OUT")) {
        std::vector<uint8_t> buf(200 * 1024);
        const uint32_t n = r.e.serialize(buf.data(), uint32_t(buf.size()));
        if (FILE* f = std::fopen(out, "wb")) {
            std::fwrite(buf.data(), 1, n, f);
            std::fclose(f);
        }
        std::fprintf(stderr, "  wrote %s: %d modules, %d cables\n", out, r.e.moduleCount(), r.e.cableCount());
    }
}

// Card paths: a WAV in Samples loads into a sampler (resampled, stereo), the
// patch autosaves at power off and comes back at the next boot.
#include "../mem_storage.h"
#include "formats/wav.h"

LAB_TEST(graf_card_samples_and_autosave) {
    MemStorage card;
    // a 22.05 kHz stereo WAV, 0.5 s
    const uint32_t frames = 11025;
    std::vector<int16_t> pcm(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        pcm[2 * i] = int16_t(12000.0 * std::sin(i * 0.05));
        pcm[2 * i + 1] = int16_t(-8000.0 * std::sin(i * 0.03));
    }
    std::vector<uint8_t> wav(frames * 4 + 64);
    const size_t n = ot::wavEncode(pcm.data(), frames, 2, wav.data(), wav.size());
    CHECK(n > 44);
    wav[24] = 0x22;   // sample rate 22050 (0x5622)
    wav[25] = 0x56;
    const uint32_t br = 22050 * 4;   // byte rate
    wav[28] = uint8_t(br);
    wav[29] = uint8_t(br >> 8);
    wav[30] = uint8_t(br >> 16);
    wav[31] = uint8_t(br >> 24);
    wav.resize(n);
    card.dirs.insert("Lab");
    card.dirs.insert("Lab/GRAF");
    card.dirs.insert("Lab/GRAF/Samples");
    card.files["Lab/GRAF/Samples/zz-tone.wav"] = wav;
    card.files["Lab/GRAF/Samples/aa-tone.wav"] = wav;
    std::vector<uint8_t> mem(64u * 1024u * 1024u);
    int smplSlot = -1;
    {
        lab::Shell shell;
        lab::Hal h;
        h.fs = &card;
        CHECK(shell.init(h, mem.data(), uint32_t(mem.size()), "GRAF"));
        auto* g = static_cast<Graf*>(shell.workstation());
        Engine& e = g->engine();
        CHECK(e.sampleFileCount() == 2);
        CHECK(std::strcmp(e.sampleFileName(1), "aa-tone.wav") == 0);   // sorted
        smplSlot = e.add(kSmpl, 0, 30);
        CHECK(smplSlot >= 0);
        e.setParamK(smplSlot, 0, Engine::unmapParam(def(kSmpl).p[0], 2.0f));   // zz-tone
        for (uint32_t t = 0; t < 2000; t += 50) g->update(t);
        CHECK(e.sampleLoaded[smplSlot] == 2);
        CHECK(e.sampleStereo[smplSlot]);
        CHECK(e.sampleFrames[smplSlot] >= 22040 && e.sampleFrames[smplSlot] <= 22060);   // resampled to 44.1 k
        CHECK(std::strcmp(e.sampleName[smplSlot], "zz-tone.wav") == 0);
        shell.powerOff();
    }
    CHECK(card.files.count("Lab/GRAF/autosave.grf") == 1);
    CHECK(card.files.count("Lab/GRAF/graf.cfg") == 1);
    // next boot: the autosave comes back, the sampler finds its file again
    // even though a new file sorted before it shifts the numbering
    card.files["Lab/GRAF/Samples/ab-new.wav"] = wav;
    std::fill(mem.begin(), mem.end(), 0);
    lab::Shell shell2;
    lab::Hal h2;
    h2.fs = &card;
    CHECK(shell2.init(h2, mem.data(), uint32_t(mem.size()), "GRAF"));
    auto* g2 = static_cast<Graf*>(shell2.workstation());
    Engine& e2 = g2->engine();
    CHECK(e2.mod(smplSlot).used && e2.mod(smplSlot).type == kSmpl);
    CHECK(int(e2.mod(smplSlot).v[0]) == 3);
    for (uint32_t t = 0; t < 2000; t += 50) g2->update(t);
    CHECK(std::strcmp(e2.sampleName[smplSlot], "zz-tone.wav") == 0);
    CHECK(e2.sampleFrames[smplSlot] > 22000);
    // no card at all: boots with the demo, saving is a no-op
    lab::Shell shell3;
    lab::Hal h3;
    std::fill(mem.begin(), mem.end(), 0);
    CHECK(shell3.init(h3, mem.data(), uint32_t(mem.size()), "GRAF"));
    auto* g3 = static_cast<Graf*>(shell3.workstation());
    CHECK(g3->engine().moduleCount() > 0);
    CHECK(!g3->savePatch("Lab/GRAF/x.grf"));
    shell3.powerOff();
}

// ---------------------------------------------------------------- round 2
namespace {
struct WsRig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    lab::Shell shell;
    lab::Hal h;
    Graf* g = nullptr;
    WsRig() {
        if (shell.init(h, mem.data(), uint32_t(mem.size()), "GRAF")) g = static_cast<Graf*>(shell.workstation());
    }
    void blocks(int n) {
        lab::Transport t;
        t.playing = true;
        t.bpm = 120.0f;
        t.beatsPerSample = 120.0 / 60.0 / 44100.0;
        lab::AudioIn in;
        float l[lab::kBlock], r[lab::kBlock];
        for (int b = 0; b < n; ++b) {
            t.startedThisBlock = b == 0;
            shell.kitBlock(t);
            g->render(t, in, l, r);
            t.beat += t.beatsPerSample * lab::kBlock;
        }
    }
    void press(int id, bool shift = false) {
        lab::Event e;
        e.type = lab::Event::kPress;
        e.id = uint8_t(id);
        e.shift = shift;
        g->onEvent(e);
    }
};
}  // namespace

LAB_TEST(graf_undo_redo_keeps_structure) {
    WsRig w;
    CHECK(w.g != nullptr);
    if (!w.g) return;
    Graf& g = *w.g;
    Engine& e = g.engine();
    g.loadDemo(0);
    const int n0 = e.moduleCount(), c0 = e.cableCount();
    g.checkpoint(1);
    const int s = e.add(kLfo, 0);
    CHECK(s >= 0);
    CHECK(e.moduleCount() == n0 + 1);
    CHECK(g.undo());
    CHECK(e.moduleCount() == n0);
    CHECK(e.cableCount() == c0);
    CHECK(g.redo());
    CHECK(e.moduleCount() == n0 + 1);
    // a knob: one gesture is one step, restored exactly
    int v = -1;
    for (int k = 0; k < kMaxSlots && v < 0; ++k)
        if (e.mod(k).used && e.mod(k).type == kVco) v = k;
    CHECK(v >= 0);
    if (v < 0) return;
    const float k0 = e.mod(v).k[0];
    g.checkpoint(2, v * 16);
    e.setParamK(v, 0, 0.9f);
    g.checkpoint(2, v * 16);   // same knob, same gesture: no new step
    e.setParamK(v, 0, 0.95f);
    const int depth = g.undoDepth();
    CHECK(g.undo());
    CHECK(g.undoDepth() == depth - 1);
    CHECK_NEAR(e.mod(v).k[0], k0, 1e-6);
    // Pattern / Shift + Pattern are the keys
    w.press(lab::btn::Pattern, true);
    CHECK_NEAR(e.mod(v).k[0], 0.95f, 1e-6);
    w.press(lab::btn::Pattern);
    CHECK_NEAR(e.mod(v).k[0], k0, 1e-6);
    bool fin = true;
    w.blocks(50);
    for (uint32_t i = 0; i < lab::kBlock; ++i) fin = fin && std::isfinite(e.mod(v).out[0][i]);
    CHECK(fin);
}

LAB_TEST(graf_tidy_layout) {
    WsRig w;
    if (!w.g) return;
    Graf& g = *w.g;
    Engine& e = g.engine();
    for (int d = 0; d < Graf::demoCount(); ++d) {
        g.loadDemo(d);
        const int n = e.moduleCount(), c = e.cableCount();
        for (int p = 0; p < kPatches; ++p) g.tidy(p);
        CHECK(e.moduleCount() == n);
        CHECK(e.cableCount() == c);
        bool taken[kPatches][kCells] = {};
        bool clash = false;
        for (int s = 0; s < kMaxSlots; ++s) {
            const Module& m = e.mod(s);
            if (!m.used) continue;
            if (taken[m.patch][m.cell]) clash = true;
            taken[m.patch][m.cell] = true;
            CHECK(e.slotAtCell(m.patch, m.cell) == s);
        }
        CHECK(!clash);
        // an OUT sits right of every module that feeds it
        for (int k = 0; k < kMaxCables; ++k) {
            const Cable& cb = e.cable(k);
            if (!cb.used || e.mod(cb.dst).type != kOut || e.mod(cb.src).patch != e.mod(cb.dst).patch) continue;
            CHECK(e.mod(cb.dst).cell % 12 >= e.mod(cb.src).cell % 12);
        }
    }
    // undo puts the layout back
    g.loadDemo(0);
    int cells[kMaxSlots];
    for (int s = 0; s < kMaxSlots; ++s) cells[s] = e.mod(s).used ? e.mod(s).cell : -1;
    g.tidy(0);
    CHECK(g.undo());
    bool same = true;
    for (int s = 0; s < kMaxSlots; ++s)
        if (e.mod(s).used && e.mod(s).cell != cells[s]) same = false;
    CHECK(same);
}

LAB_TEST(graf_templates_fit_an_empty_canvas) {
    WsRig w;
    if (!w.g) return;
    Graf& g = *w.g;
    Engine& e = g.engine();
    for (int t = 0; t < kTemplates; ++t) {
        e.clear();
        e.quiesce();
        const int n = g.addTemplate(t, 0, 0);
        std::fprintf(stderr, "  template %-16s %d modules, %d cables\n", Graf::templateName(t), n, e.cableCount());
        CHECK(n > 0);
        CHECK(e.cableCount() > 0);
        CHECK(e.cpuCost() <= kCpuBudget);
        // twice on one canvas: the second block goes elsewhere and reuses the OUT
        int outs = 0;
        const int n2 = g.addTemplate(t, 0, 0);
        for (int s = 0; s < kMaxSlots; ++s) outs += e.mod(s).used && e.mod(s).type == kOut;
        if (n2 > 0) CHECK(outs <= 1);
        w.blocks(40);
    }
}

LAB_TEST(graf_crossfader_between_snapshots) {
    Rig r;
    const int v = r.e.add(kVco, 0, 0);
    const int f = r.e.add(kSvf, 0, 1);
    const int o = r.e.add(kOut, 0, 2);
    r.e.connect(v, 0, f, 0, 1.0f);
    r.e.connect(f, 0, o, 0, 1.0f);
    r.setP(f, 0, 200.0f);
    r.e.storeSnap(0);
    r.setP(f, 0, 6000.0f);
    const int l = r.e.add(kLfo, 0, 3);
    r.e.connect(l, 0, f, 2, 0.8f);
    r.e.storeSnap(1);
    const float kA = r.e.snap(0).mod[r.e.snap(0).find(1)].k[0];
    const float kB = r.e.snap(1).mod[r.e.snap(1).find(1)].k[0];
    r.e.crossfade(0, 1, 0.0f);
    CHECK_NEAR(r.e.mod(f).k[0], kA, 1e-5);
    r.e.crossfade(0, 1, 0.25f);
    CHECK_NEAR(r.e.mod(f).k[0], kA + (kB - kA) * 0.25f, 1e-4);
    CHECK(r.e.slotAtCell(0, 3) >= 0);   // B's LFO is there in between
    r.e.crossfade(0, 1, 1.0f);
    CHECK_NEAR(r.e.mod(f).k[0], kB, 1e-5);
    r.e.crossfade(0, 1, 0.0f);
    CHECK(r.e.slotAtCell(0, 3) < 0);    // and gone at A
    bool fin = false;
    r.run(50, &fin);
    CHECK(fin);
}

LAB_TEST(graf_plaits_pool_and_model_cost) {
    Rig r;
    int made = 0;
    for (int i = 0; i < 6; ++i)
        if (r.e.add(kPlts, 0, i) >= 0) ++made;
    CHECK(made <= kPlaitsPool);
    CHECK(made >= 1);
    CHECK(r.e.cpuCost() <= kCpuBudget);
    // the most expensive model on every voice: refused once over budget
    for (int s = 0; s < kMaxSlots; ++s)
        if (r.e.mod(s).used && r.e.mod(s).type == kPlts) {
            int heaviest = 0;
            for (int m = 0; m < 24; ++m)
                if (plaitsCost(m) > plaitsCost(heaviest)) heaviest = m;
            r.setP(s, 0, float(heaviest));
        }
    CHECK(r.e.cpuCost() <= kCpuBudget);
    // removing one frees its voice for a new one
    const int first = r.e.slotAtCell(0, 0);
    r.e.remove(first);
    r.e.quiesce();
    CHECK(r.e.add(kPlts, 0, 20) >= 0 || r.e.cpuCost() + plaitsCost(8) > kCpuBudget);
    bool fin = false;
    r.run(100, &fin);
    CHECK(fin);
}

LAB_TEST(graf_labkit_knobs_routes_and_taps) {
    WsRig w;
    if (!w.g) return;
    Graf& g = *w.g;
    Engine& e = g.engine();
    kit::Registry& P = w.shell.labkit().reg;
    kit::Mod& M = w.shell.labkit().mod;
    g.loadDemo(0);
    int v = -1;
    for (int k = 0; k < kMaxSlots && v < 0; ++k)
        if (e.mod(k).used && e.mod(k).type == kVco) v = k;
    CHECK(v >= 0);
    if (v < 0) return;
    // every knob of every slot is a stable parameter
    const kit::ParamId id = g.knobId(v, 2);
    CHECK(id != kit::kNoParam);
    char key[12];
    std::snprintf(key, sizeof key, "s%02d.k2", v);
    CHECK(P.find(key) == id);
    // the registry edits the GRAF knob (base) and back
    P.setNormFromShell(id, 0.8f, false);   // a LABKIT view (PARAM pads, MAP, MIDI learn)
    CHECK_NEAR(e.mod(v).k[2], 0.8f, 1e-5);
    e.setParamK(v, 2, 0.3f);
    CHECK_NEAR(P.norm(id), 0.3f, 1e-5);
    // a macro reaches any knob: the effective value moves, the base stays
    M.clearRoutes();
    const int ri = M.route(kit::kSrcMacro + 1, id, 0.5f);
    CHECK(ri >= 0);
    M.setMacro(1, 1.0f);
    M.update(false);   // the main loop builds the plan
    w.blocks(4);
    CHECK(e.mod(v).ke[2] > e.mod(v).k[2] + 0.2f);
    CHECK_NEAR(e.mod(v).k[2], 0.3f, 1e-5);
    // removing the module drops its routes
    g.checkpoint(1);
    e.remove(v);
    M.update(false);
    w.blocks(1);
    int tmp[4];
    CHECK(M.routesTo(id, tmp, 4) == 0);
    CHECK(g.undo());
    // a tap: a module output as a LABKIT source
    int lfo = -1;
    for (int k = 0; k < kMaxSlots && lfo < 0; ++k)
        if (e.mod(k).used && e.mod(k).type == kLfo) lfo = k;
    CHECK(lfo >= 0);
    if (lfo < 0) return;
    const int t = e.toggleTap(lfo, 0);
    CHECK(t >= 0 && t < kTaps);
    CHECK(e.tapOf(lfo, 0) == t);
    float lo = 1e9f, hi = -1e9f;
    for (int b = 0; b < 200; ++b) {
        w.blocks(1);
        lo = std::fmin(lo, e.tapVal[t]);
        hi = std::fmax(hi, e.tapVal[t]);
    }
    CHECK(hi - lo > 0.1f);
    CHECK(e.toggleTap(lfo, 0) == -1);
    CHECK(e.tapOf(lfo, 0) < 0);
    // the main knobs sit on the GRAPH page, the selected module's on MODULE
    g.select(lfo);
    g.reassignPages();
    CHECK(P.meta(g.knobId(lfo, 0)).page == kPgModule);
    // the knob params survive save / load with their routes
    M.route(kit::kSrcMacro + 2, g.knobId(lfo, 1), 0.25f);
    std::vector<uint8_t> chunk(8192);
    const uint32_t n = P.saveChunk(chunk.data(), uint32_t(chunk.size()));
    CHECK(n > 0);
}

LAB_TEST(graf_keys_exist_on_the_hardware) {
    WsRig w;
    if (!w.g) return;
    Graf& g = *w.g;
    g.loadDemo(0);
    // F4 then Insert adds the selected type; Pattern undoes it
    const int n = g.engine().moduleCount();
    w.press(lab::btn::F4);
    CHECK(g.page() == kPgAdd);
    w.press(lab::btn::Insert);
    CHECK(g.engine().moduleCount() == n + 1);
    w.press(lab::btn::Pattern);
    CHECK(g.engine().moduleCount() == n);
    // Note / Instr follow the signal
    w.press(lab::btn::F1);
    int seq = -1;
    for (int k = 0; k < kMaxSlots && seq < 0; ++k)
        if (g.engine().mod(k).used && g.engine().mod(k).type == kSeq) seq = k;
    if (seq >= 0) {
        g.select(seq);
        w.press(lab::btn::Instr);
        CHECK(g.selected() != seq);
        w.press(lab::btn::Note);
        CHECK(g.selected() == seq);
    }
    // Shift + Insert on the graph tidies (an undo step)
    const int d = g.undoDepth();
    w.press(lab::btn::Insert, true);
    CHECK(g.undoDepth() == d + 1 || g.undoDepth() == 24);
}
