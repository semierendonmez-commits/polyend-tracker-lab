// BULUT host tests (round 2: four deep channels on LABKIT v2): scheduler
// counts and budgets, window / pitch math, every engine x FX mode stays
// finite, spectral freeze + tools, gestures (record, loop, param playback),
// scenes + morph, engine knob memory, sample swap + region pads, the resample
// round trip into a channel, the project v2 round trip, no card, ASCII text.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "formats/wav.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "mem_storage.h"
#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

using namespace bulut;
namespace kit = lab::kit;

namespace {

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    MemStorage fs;
    lab::Shell shell;
    bool ok = false;
    lab::Transport tr;
    int16_t input[lab::kBlock * 2] = {};
    uint32_t n = 0;
    float toneHz = 330.0f;
    explicit Rig(bool card = true) {
        lab::Hal h;
        if (card) h.fs = &fs;
        ok = shell.init(h, mem.data(), uint32_t(mem.size()), "BULUT");
        tr.playing = true;
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
    }
    Bulut& ws() { return *static_cast<Bulut*>(shell.workstation()); }
    kit::Registry& P() { return shell.context().params(); }
    // One block with a test tone on the input; returns the block peak.
    float block(float* lOut = nullptr, float* rOut = nullptr) {
        for (uint32_t i = 0; i < lab::kBlock; ++i, ++n) {
            const int16_t v = int16_t(12000.0f * sinf(6.2831853f * toneHz * float(n) / 44100.0f));
            input[2 * i] = input[2 * i + 1] = v;
        }
        lab::AudioIn in;
        in.frames = input;
        float l[lab::kBlock] = {}, r[lab::kBlock] = {};
        shell.kitBlock(tr);
        ws().render(tr, in, l, r);
        tr.beat += tr.beatsPerSample * lab::kBlock;
        float pk = 0.0f;
        for (uint32_t i = 0; i < lab::kBlock; ++i) {
            pk = std::fmax(pk, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            if (lOut) lOut[i] = l[i];
            if (rOut) rOut[i] = r[i];
        }
        return pk;
    }
    void blocks(int k) {
        for (int i = 0; i < k; ++i) block();
    }
    // A mono 16-bit WAV on the card: `secs` of a sine at `hz`.
    void wav(const char* path, float secs, float hz) {
        ot::WavWriter w;
        if (!w.open(fs, path, 1)) return;
        std::vector<int16_t> s(uint32_t(secs * 44100.0f));
        for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(9000.0f * sinf(6.2831853f * hz * float(i) / 44100.0f));
        w.write(s.data(), uint32_t(s.size()));
        w.close();
    }
};

// Everything quiet except channel `keep`.
void solo(Bulut& b, int keep) {
    for (int c = 0; c < kCh; ++c) {
        b.setParam(c, CP::Level, c == keep ? 0.7f : 0.0f, false);
        b.state(c).drone = 0;
    }
}

}  // namespace

// ---- scheduler -----------------------------------------------------------------
LAB_TEST(bulut_grain_rate_follows_density) {
    Rig rig;
    CHECK(rig.ok);
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    solo(b, 0);
    b.setEngine(0, kGranular);
    b.setNorm(0, K::Size, 0.0f, false);    // 10 ms grains: no budget pressure
    b.setNorm(0, K::Dens, 0.5f, false);    // + row offset of pad 12 (row 1)
    e.padDown(0, 12, 100);
    rig.blocks(20);
    const uint32_t s0 = e.rt[0].spawned;
    const int kBlocks = 3445;               // ~10 s
    rig.blocks(kBlocks);
    const float secs = float(kBlocks) * 128.0f / 44100.0f;
    const float rate = float(e.rt[0].spawned - s0) / secs;
    const float want = densHz(0.5f + padDensOff(12));
    std::fprintf(stderr, "  granular rate %.2f /s, expected %.2f\n", double(rate), double(want));
    CHECK(std::fabs(rate - want) < want * 0.08f);
}

LAB_TEST(bulut_quanta_sync_law_is_periodic) {
    Rig rig;
    if (!rig.ok) return;
    Engine& e = rig.ws().engine();
    float k[kKnobs] = {};
    k[K::Rand] = enumValue(kLawSync, kLaws);
    const float hz = rateHz(0.4f);
    for (int i = 0; i < 8; ++i) CHECK_NEAR(e.nextInterval(1, kQuanta, k, hz), 44100.0f / hz, 0.01f);
    k[K::Rand] = enumValue(kLawPoisson, kLaws);
    float sum = 0.0f;
    for (int i = 0; i < 4000; ++i) sum += e.nextInterval(1, kQuanta, k, hz);
    CHECK_NEAR(sum / 4000.0f, 44100.0f / hz, 44100.0f / hz * 0.06f);
}

LAB_TEST(bulut_grain_budgets_hold) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    e.grainBudget = kMaxGrains;
    e.cloudBudget = kMaxCloudGrains;
    for (int c = 0; c < kCh; ++c) {
        b.setEngine(c, c);   // one of each engine
        b.setNorm(c, K::Dens, 1.0f, false);
        b.setNorm(c, K::Size, 1.0f, false);
        b.state(c).drone = 1;
        for (int p = 0; p < 4; ++p) e.padDown(c, p * 11, 127);
    }
    int maxTotal = 0, maxCh = 0;
    for (int i = 0; i < 2000; ++i) {
        rig.block();
        maxTotal = e.totalActive > maxTotal ? e.totalActive : maxTotal;
        for (int c = 0; c < kCh; ++c) maxCh = e.rt[c].active > maxCh ? e.rt[c].active : maxCh;
        CHECK(e.totalActive <= e.grainLimit || e.totalActive <= kMaxGrains);
    }
    std::fprintf(stderr, "  max grains %d (limit %d), per channel %d (limit %d), refused %u, units %.1f\n", maxTotal,
                 kMaxGrains, maxCh, kMaxCloudGrains, unsigned(e.refusedTotal), double(e.unitsUsed));
    CHECK(maxTotal <= kMaxGrains);
    CHECK(maxCh <= kMaxCloudGrains);
    CHECK(maxTotal >= 12);   // the budget is actually used
    CHECK(e.refusedTotal > 0);
    // heavy chains take grains away, never below the floor
    for (int c = 0; c < kCh; ++c) {
        b.setParam(c, CP::FxMode, float(kFxTexture), false);
        b.setParam(c, CP::Flt, float(kFltLp), false);
        b.setParam(c, CP::Drive, 1.0f, false);
    }
    rig.blocks(300);
    CHECK(e.grainLimit >= kMinGrains);
    CHECK(e.grainLimit < kMaxGrains);
    int after = 0;
    for (int i = 0; i < 300; ++i) {
        rig.block();
        after = e.totalActive > after ? e.totalActive : after;
    }
    CHECK(after <= e.grainLimit + kSpawnsPerBlock);
    // lowering the budget takes effect
    e.grainBudget = 8;
    rig.blocks(400);
    after = 0;
    for (int i = 0; i < 200; ++i) {
        rig.block();
        after = e.totalActive > after ? e.totalActive : after;
    }
    CHECK(after <= 8);
}

LAB_TEST(bulut_pad_mapping) {
    CHECK_NEAR(padPos(0), 0.5f / 12.0f, 1e-6f);
    CHECK_NEAR(padPos(11), 11.5f / 12.0f, 1e-6f);
    CHECK_NEAR(padPos(47), 11.5f / 12.0f, 1e-6f);
    CHECK(padDensOff(0) > padDensOff(12));
    CHECK(padDensOff(12) > padDensOff(24));
    CHECK(padDensOff(24) > padDensOff(36));
    CHECK(padTapeLen(0) > padTapeLen(36));
}

// ---- window and pitch math -------------------------------------------------------
LAB_TEST(bulut_windows_are_windows) {
    for (int t = 0; t < kWindows; ++t) {
        CHECK_NEAR(windowValue(t, 0.0f), 0.0f, 1e-3f);
        CHECK(windowValue(t, 0.9999f) < 0.02f || t == kWinRexpodec);
        float mx = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const float v = windowValue(t, float(i) / 100.0f);
            CHECK(v >= 0.0f && v <= 1.0001f);
            mx = v > mx ? v : mx;
        }
        CHECK(mx > 0.9f);
    }
    for (int i = 1; i < 10; ++i) {
        const float x = float(i) / 10.0f;
        CHECK_NEAR(windowValue(5, x), 0.5f - 0.5f * cosf(6.2831853f * x), 2e-3f);
    }
}

LAB_TEST(bulut_pitch_and_scales) {
    CHECK_NEAR(pitchSemi(0.5f), 0.0f, 1e-6f);
    CHECK_NEAR(pitchSemi(1.0f), 24.0f, 1e-5f);
    CHECK_NEAR(fexp2(1.0f), 2.0f, 1e-5f);
    CHECK_NEAR(fexp2(-1.0f), 0.5f, 1e-6f);
    for (float x = 0.01f; x < 100.0f; x *= 1.7f) CHECK_NEAR(flog2(x), log2f(x), 2e-5f);
    for (float x = -10.0f; x < 10.0f; x += 0.37f) CHECK_NEAR(fexp2(x) / exp2f(x), 1.0f, 2e-5f);
    CHECK_NEAR(scaleSnap(5.4f, kScaleOct), 0.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(6.6f, kScaleOct), 12.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(6.0f, kScaleFifth), 7.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(3.4f, kScaleMinor), 3.0f, 1e-6f);
    CHECK_NEAR(sizeSec(0.0f), 0.01f, 1e-5f);
    CHECK_NEAR(sizeSec(1.0f), 1.0f, 1e-4f);
    CHECK_NEAR(densHz(1.0f), 120.0f, 0.01f);
    CHECK_NEAR(rateHz(1.0f), 1500.0f, 0.2f);
    CHECK_NEAR(tapeGrainSec(0.0f), 0.02f, 1e-5f);
    CHECK_NEAR(tapeGrainSec(1.0f), 0.5f, 1e-3f);
    CHECK_NEAR(tapeSpeed(0.5f), 0.0f, 1e-6f);
    CHECK(tapeSpeed(1.0f) > 1.5f && tapeSpeed(0.0f) < -1.5f);
}

// A grain at +12 semitones over a known tone comes out an octave up.
LAB_TEST(bulut_grain_pitch_shift_is_an_octave) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    solo(b, 0);
    b.setEngine(0, kGranular);
    b.setParam(0, CP::Rev, 0.0f, false);
    b.setParam(0, CP::Dly, 0.0f, false);
    b.setParam(0, CP::Pan, 0.0f, false);
    b.setNorm(0, K::Spread, 0.0f, false);
    b.setNorm(0, K::Scatter, 0.0f, false);
    b.setNorm(0, K::Rand, 0.0f, false);
    b.setNorm(0, K::Mode, 0.0f, false);       // no reverse
    b.setNorm(0, K::Feedback, 0.0f, false);
    b.setNorm(0, K::Extra, 0.0f, false);
    b.setNorm(0, K::Size, 1.0f, false);       // 1 s grains
    b.setNorm(0, K::Dens, 0.0f, false);       // sparse
    b.setNorm(0, K::Pitch, 0.75f, false);     // +12
    rig.blocks(500);
    e.padDown(0, 11, 127);
    std::vector<float> out;
    for (int i = 0; i < 240; ++i) {
        float l[lab::kBlock];
        rig.block(l);
        out.insert(out.end(), l, l + lab::kBlock);
    }
    int zc = 0;
    const size_t a = 16000, n = 12000;
    for (size_t i = a + 1; i < a + n; ++i)
        if ((out[i - 1] < 0.0f) != (out[i] < 0.0f)) ++zc;
    const float hz = float(zc) * 0.5f * 44100.0f / float(n);
    std::fprintf(stderr, "  shifted tone %.1f Hz (expected 660)\n", double(hz));
    CHECK_NEAR(hz, 660.0f, 20.0f);
}

// Every engine through every FX mode / filter, with feedback: finite and bounded.
LAB_TEST(bulut_every_engine_and_fx_mode_stays_finite) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    for (int c = 0; c < kCh; ++c) {
        b.state(c).drone = 1;
        b.setNorm(c, K::Feedback, 1.0f, false);
        b.setParam(c, CP::FxFb, 1.0f, false);
        b.setParam(c, CP::FxAmt, 1.0f, false);
        b.setParam(c, CP::Drive, 1.0f, false);
        b.setParam(c, CP::Reso, 1.0f, false);
        b.setParam(c, CP::Level, 1.0f, false);
    }
    bool finite = true;
    float pk = 0.0f;
    for (int eng = 0; eng < kEngines; ++eng)
        for (int fx = 0; fx < kFxModes; ++fx) {
            for (int c = 0; c < kCh; ++c) {
                b.setEngine(c, (eng + c) % kEngines);
                b.setParam(c, CP::FxMode, float((fx + c) % kFxModes), false);
                b.setParam(c, CP::Flt, float((fx + c) % kFltModes), false);
                b.setParam(c, CP::FxTime, float((fx * 3 + c) % 8) / 7.0f, false);
                b.setSource(c, (fx + c) % 2 == 0 ? kSrcInput : (c == 3 ? kSrcGen : kSrcOutput));
                b.setParam(c, CP::GenType, float((eng + fx) % kGenCount), false);
                e.padDown(c, (eng * 7 + fx * 5 + c * 3) % 48, 127);
            }
            for (int i = 0; i < 120; ++i) {
                float l[lab::kBlock], r[lab::kBlock];
                pk = std::fmax(pk, rig.block(l, r));
                for (uint32_t k = 0; k < lab::kBlock; ++k) finite = finite && std::isfinite(l[k]) && std::isfinite(r[k]);
            }
            for (int c = 0; c < kCh; ++c) e.releaseAll(c);
        }
    std::fprintf(stderr, "  sweep peak %.3f\n", double(pk));
    CHECK(finite);
    CHECK(pk > 0.01f && pk <= 1.0f);
}

// ---- spectral freeze ----------------------------------------------------------------
LAB_TEST(bulut_freeze_is_stable_and_keeps_the_spectrum) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    rig.blocks(200);
    CHECK(b.captureFreeze(3, kFromSource));
    FreezeSlot& f = b.freezeSlot(3);
    CHECK(f.used);
    int best = 0;
    for (int k = 1; k < kSpecBins; ++k)
        if (f.mag[k] > f.mag[best]) best = k;
    CHECK_NEAR(float(best) * 44100.0f / float(Spectral::kAna), 330.0f, 12.0f);
    b.recallFreeze(3);
    CHECK(e.rt[0].frzReady);
    CHECK(b.state(0).freeze);
    const int16_t* lp = e.rt[0].frz;
    const int jump = std::abs(int(lp[0]) - int(lp[kFrzLen - 1]));
    int maxStep = 0;
    for (uint32_t i = 1; i < kFrzLen; ++i) maxStep = std::max(maxStep, std::abs(int(lp[i]) - int(lp[i - 1])));
    CHECK(jump <= maxStep + 64);
    std::vector<int16_t> copy(lp, lp + kFrzLen);
    b.recallFreeze(3);
    CHECK(std::memcmp(copy.data(), lp, kFrzLen * 2) == 0);   // deterministic recall
    // a silent source is refused (slot untouched)
    rig.toneHz = 0.0f;
    b.setSource(1, kSrcInput);
    b.selectChannel(1);
    rig.blocks(400);
    CHECK(!b.captureFreeze(5, kFromSource));
    CHECK(!b.freezeSlot(5).used);
    b.selectChannel(0);
    // long render of a frozen channel stays bounded and finite
    b.setParam(0, CP::Bed, 1.0f, false);
    e.padDown(0, 30, 120);
    float pk = 0.0f;
    bool finite = true;
    for (int i = 0; i < 2000; ++i) {
        float l[lab::kBlock];
        pk = std::fmax(pk, rig.block(l));
        for (float v : l) finite = finite && std::isfinite(v);
    }
    CHECK(finite);
    CHECK(pk > 0.01f && pk <= 1.0f);
}

// The SHIFT tool moves the spectrum: +12 st doubles the peak frequency.
LAB_TEST(bulut_spectral_shift_tool) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    rig.blocks(200);
    CHECK(b.captureFreeze(0, kFromSource));
    SpecTools t;
    t.shift = 12.0f;
    static float out[kSpecBins];
    Spectral sp;
    std::vector<uint8_t> mem(4u * 1024u * 1024u);
    lab::Arena a;
    a.attach(mem.data(), uint32_t(mem.size()));
    CHECK(sp.init(a));
    sp.shape(b.freezeSlot(0), nullptr, t, out);
    int best = 1;
    for (int k = 2; k < kSpecBins; ++k)
        if (out[k] > out[best]) best = k;
    CHECK_NEAR(float(best) * 44100.0f / float(Spectral::kAna), 660.0f, 24.0f);
}

// ---- gestures --------------------------------------------------------------------
LAB_TEST(bulut_gesture_record_and_loop_timing) {
    GestureRecorder rec;
    Gesture out{};
    rec.arm(0, false);
    CHECK(rec.state() == GestureRecorder::kArmed);
    const uint32_t t0 = 1000000;
    rec.start(t0, nullptr, 0);
    rec.pad(true, 5, 100, t0);
    rec.param(K::Pos, 0.25f, t0 + 1000);
    rec.param(K::Pos, 0.30f, t0 + 1200);   // within 10 ms: replaces the last
    rec.pad(false, 5, 0, t0 + 22050);
    rec.pad(true, 7, 90, t0 + 30000);       // held at stop: closed at the loop end
    CHECK(rec.finish(t0 + 44100, 0, out, nullptr));
    CHECK(out.len == 44100);
    CHECK(out.count == 5);
    CHECK(out.ev[0].type == kGPadDown && out.ev[0].t == 0 && out.ev[0].a == 5);
    CHECK(out.ev[1].type == kGParam && out.ev[1].t == 1000 && out.ev[1].a == K::Pos);
    CHECK_NEAR(float(out.ev[1].v) / 65535.0f, 0.30f, 1e-4f);
    CHECK(out.ev[2].type == kGPadUp && out.ev[2].t == 22050);
    CHECK(out.ev[4].type == kGPadUp && out.ev[4].a == 7 && out.ev[4].t == 44099);
    for (int i = 1; i < out.count; ++i) CHECK(out.ev[i - 1].t <= out.ev[i].t);
    GestureRecorder q;
    Gesture qo{};
    q.arm(1, false);
    q.start(0, nullptr, 0);
    q.pad(true, 1, 100, 0);
    q.pad(false, 1, 0, 100);
    CHECK(q.finish(24255, 22050, qo, nullptr));
    CHECK(qo.len == 22050);
    GestureRecorder od;
    Gesture merged{};
    od.arm(0, true);
    od.start(500000, &out, 40000);
    od.pad(true, 9, 100, 500000 + 8000);
    od.pad(false, 9, 0, 500000 + 9000);
    CHECK(od.finish(500000 + 10000, 0, merged, &out));
    CHECK(merged.len == out.len);
    CHECK(merged.count == out.count + 2);
    bool found = false;
    for (int i = 0; i < merged.count; ++i)
        if (merged.ev[i].type == kGPadDown && merged.ev[i].a == 9) found = merged.ev[i].t == 3900;
    CHECK(found);
}

// The engine plays a gesture: ghost pads on time, looping, parameter moves
// land in the registry through the main loop, GEST SPEED doubles the passes.
LAB_TEST(bulut_gesture_playback_loops) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    b.setEngine(2, kGranular);
    Gesture& g = e.gestIdle(2);
    g.len = 128 * 100;
    g.quant = 0;
    g.count = 3;
    g.ev[0] = GestEvent{128 * 10, kGPadDown, 17, 100};
    g.ev[1] = GestEvent{128 * 30, kGParam, uint8_t(K::Pitch), 65535};
    g.ev[2] = GestEvent{128 * 50, kGPadUp, 17, 0};
    e.postGesture(2);
    b.state(2).gestOn = 1;
    auto ghostOn = [&] {
        for (int i = kLiveEmitters; i < kEmitters; ++i)
            if (e.rt[2].em[i].on && e.rt[2].em[i].pad == 17) return true;
        return false;
    };
    int onBlocks = 0, firstOn = -1;
    for (int blk = 0; blk < 300; ++blk) {
        rig.block();
        b.tick(uint32_t(blk * 3));
        if (ghostOn()) {
            ++onBlocks;
            if (firstOn < 0) firstOn = blk;
        }
        if (blk == 35) CHECK_NEAR(b.norm(2, K::Pitch), 1.0f, 1e-4f);
    }
    CHECK(firstOn == 10);
    CHECK(onBlocks >= 118 && onBlocks <= 122);
    b.setParam(2, CP::GestSpeed, 3.0f, false);   // 2 x
    onBlocks = 0;
    for (int blk = 0; blk < 300; ++blk) {
        rig.block();
        onBlocks += ghostOn() ? 1 : 0;
    }
    CHECK(onBlocks >= 116 && onBlocks <= 124);
}

// ---- scenes, engines -----------------------------------------------------------------
LAB_TEST(bulut_scene_store_morph_recall) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    kit::Registry& P = rig.P();
    b.setParam(1, CP::Level, 0.2f, false);
    b.setParam(1, CP::Cutoff, 500.0f, false);
    b.storeScene(0);
    CHECK(b.sceneUsed(0));
    b.setParam(1, CP::Level, 0.9f, false);
    b.setParam(1, CP::Cutoff, 8000.0f, false);
    P.set(b.pid(1, CP::Level), 0.9f, false);
    // morph over 1 s
    P.set(b.pid(0, CP::Level), b.params(0).level, false);
    b.tick(1000);
    b.recallScene(0, true);
    b.tick(1500);   // half way
    const float mid = b.params(1).level;
    CHECK(mid > 0.25f && mid < 0.85f);
    b.tick(2100);
    CHECK_NEAR(b.params(1).level, 0.2f, 1e-3f);
    CHECK_NEAR(b.params(1).cutoff, 500.0f, 1.0f);
    // a cut: at once
    b.setParam(1, CP::Level, 0.9f, false);
    b.recallScene(0, false);
    CHECK_NEAR(b.params(1).level, 0.2f, 1e-3f);
}

LAB_TEST(bulut_engine_switch_keeps_each_engines_knobs) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    kit::Registry& P = rig.P();
    b.setEngine(0, kGranular);
    b.setNorm(0, K::Size, 0.31f, false);
    b.setEngine(0, kQuanta);
    CHECK(b.params(0).engine == kQuanta);
    CHECK(std::strcmp(P.name(b.pid(0, K::Mode)), knobName(kQuanta, K::Mode)) == 0);
    b.setNorm(0, K::Size, 0.77f, false);
    b.setEngine(0, kGranular);
    CHECK_NEAR(b.norm(0, K::Size), 0.31f, 1e-4f);
    CHECK(std::strcmp(P.name(b.pid(0, K::Size)), knobName(kGranular, K::Size)) == 0);
    b.setEngine(0, kQuanta);
    CHECK_NEAR(b.norm(0, K::Size), 0.77f, 1e-4f);
    // a LABKIT route on a knob survives the switch (it is the same param)
    const int src = rig.shell.context().mod().findSource("C1 LFO");
    CHECK(src >= 0);
    CHECK(rig.shell.context().mod().route(src, b.pid(0, K::Pitch), 0.3f) >= 0);
    b.setEngine(0, kBeads);
    CHECK(rig.shell.context().mod().findRoute(src, b.pid(0, K::Pitch)) >= 0);
}

// ---- samples ----------------------------------------------------------------------------
LAB_TEST(bulut_sample_swap_and_region_pads) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    rig.fs.mkdir("Samples");
    rig.wav("Samples/a.wav", 0.5f, 220.0f);
    rig.wav("Samples/b.wav", 0.7f, 440.0f);
    rig.wav("Samples/c.wav", 0.9f, 880.0f);
    CHECK(b.loadSample(2, "Samples/a.wav"));
    CHECK(b.params(2).source == kSrcSample);
    CHECK(b.engine().rt[2].sampleLen == 22050);
    CHECK(std::strcmp(b.source(2).label, "a.wav") == 0);
    CHECK(b.swapSample(2, 1));
    CHECK(std::strcmp(b.source(2).path, "Samples/b.wav") == 0);
    CHECK(b.swapSample(2, 1));
    CHECK(std::strcmp(b.source(2).label, "c.wav") == 0);
    CHECK(b.swapSample(2, 1));   // wraps
    CHECK(std::strcmp(b.source(2).label, "a.wav") == 0);
    CHECK(b.swapSample(2, -1));
    CHECK(std::strcmp(b.source(2).label, "c.wav") == 0);
    CHECK(b.engine().rt[2].sampleLen == uint32_t(0.9f * 44100.0f));
    // REGION pads: two taps set start / end (either order)
    b.selectChannel(2);
    b.setPage(kPgSource);
    b.regionPad(30);
    b.regionPad(6);
    CHECK_NEAR(b.params(2).regStart, 6.0f / 48.0f, 1e-4f);
    CHECK_NEAR(b.params(2).regEnd, 31.0f / 48.0f, 1e-4f);
    // grains play inside the region
    b.setEngine(2, kGranular);
    b.engine().padDown(2, 0, 120);
    rig.blocks(200);
    CHECK(b.engine().rt[2].spawned > 0);
    // a missing file is refused, the channel keeps its sample
    CHECK(!b.loadSample(2, "Samples/missing.wav"));
    CHECK(std::strcmp(b.source(2).label, "c.wav") == 0);
}

// ---- resample loop -------------------------------------------------------------------
LAB_TEST(bulut_resample_into_a_channel) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    b.setParam(0, CP::Level, 0.8f, false);
    e.padDown(0, 6, 120);
    rig.blocks(100);
    b.startResample();
    CHECK(b.resampleBusy());
    for (int i = 0; i < 345; ++i) {
        rig.block();
        b.tick(uint32_t(i * 3));
    }
    const uint32_t captured = e.capLen;
    b.stopResample();
    for (int i = 0; i < 20 && b.resampleBusy(); ++i) b.tick(2000u + uint32_t(i));
    CHECK(!b.resampleBusy());
    CHECK(captured >= 44100);
    const char* path = b.lastResample();
    CHECK(std::strstr(path, "Resample/MIX-001.wav") != nullptr);
    CHECK(rig.fs.files.count(path) == 1);
    std::vector<int16_t> back(kBufLen);
    ot::WavImportResult res;
    const ot::FmtStatus st = ot::wavImportFile(rig.fs, path, ot::WavImportOptions(), back.data(), back.size(), res);
    CHECK(st == ot::FmtStatus::Ok);
    CHECK(res.frames == captured);
    CHECK(std::memcmp(back.data(), e.capBuf, captured * 2) == 0);
    // ... and became channel 2's sample with a clear name
    CHECK(b.params(1).source == kSrcSample);
    CHECK(std::strcmp(b.source(1).path, path) == 0);
    CHECK(std::strcmp(b.source(1).label, "RESAMPLE MIX 001") == 0);
    CHECK(e.rt[1].sampleLen == captured);
    // the next take gets the next number
    b.startResample();
    rig.blocks(200);
    b.stopResample();
    for (int i = 0; i < 20 && b.resampleBusy(); ++i) b.tick(3000u + uint32_t(i));
    CHECK(std::strstr(b.lastResample(), "MIX-002.wav") != nullptr);
}

// ---- projects -------------------------------------------------------------------------
LAB_TEST(bulut_project_save_load_roundtrip) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    kit::Registry& P = rig.P();
    kit::Mod& M = rig.shell.context().mod();
    // a state worth saving
    b.setNorm(0, K::Size, 0.123f, false);
    b.setEngine(3, kQuanta);
    b.setNorm(3, K::Mode, enumValue(kQTrainlet, kQTypes), false);
    b.setEngine(3, kBeads);
    b.setNorm(3, K::Size, 0.66f, false);
    b.setParam(2, CP::Pan, -0.4f, false);
    b.setParam(2, CP::FxMode, float(kFxFlanger), false);
    b.setParam(1, CP::Cutoff, 1234.0f, false);
    b.state(1).mute = 1;
    const int src = M.findSource("C2 BUF");
    CHECK(src >= 0);
    CHECK(M.route(src, b.pid(1, CP::Cutoff), -0.5f) >= 0);
    rig.blocks(150);
    CHECK(b.captureFreeze(2, kFromSource));
    b.recallFreeze(2);
    Gesture& g = e.gestIdle(1);
    g.len = 33333;
    g.count = 2;
    g.ev[0] = GestEvent{10, kGPadDown, 3, 99};
    g.ev[1] = GestEvent{2000, kGParam, uint8_t(CP::Cutoff), 1000};
    e.postGesture(1);
    rig.block();
    b.storeScene(4);
    std::vector<float> norms;
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kChParams; ++i) norms.push_back(b.norm(c, i));
    FreezeSlot frz = b.freezeSlot(2);
    CHECK(b.saveProjectTo("Lab/BULUT/Projects/T1.blt"));
    CHECK(rig.fs.files.count("Lab/BULUT/Projects/T1.blt") == 1);
    // change everything, then load
    b.setEngine(3, kTape);
    b.setNorm(0, K::Size, 0.9f, false);
    b.setParam(2, CP::Pan, 0.4f, false);
    b.setParam(1, CP::Cutoff, 300.0f, false);
    b.state(1).mute = 0;
    M.clearRoutes();
    b.freezeSlot(2).used = 0;
    Gesture& z = e.gestIdle(1);
    z.len = 0;
    z.count = 0;
    e.postGesture(1);
    rig.block();
    CHECK(b.loadProjectFrom("Lab/BULUT/Projects/T1.blt"));
    rig.block();
    int k = 0;
    int bad = 0;
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kChParams; ++i, ++k)
            if (std::fabs(b.norm(c, i) - norms[size_t(k)]) > 1e-4f) {
                if (bad++ < 5) std::fprintf(stderr, "  ch %d param %d: %g != %g\n", c, i, double(b.norm(c, i)), double(norms[size_t(k)]));
            }
    CHECK(bad == 0);
    CHECK(b.params(3).engine == kBeads);
    CHECK(std::strcmp(P.name(b.pid(3, K::Mode)), knobName(kBeads, K::Mode)) == 0);
    b.setEngine(3, kQuanta);   // the other engine's knobs came back too
    CHECK_NEAR(b.norm(3, K::Mode), enumValue(kQTrainlet, kQTypes), 1e-4f);
    CHECK(b.state(1).mute == 1);
    CHECK(M.findRoute(src, b.pid(1, CP::Cutoff)) >= 0);
    CHECK(b.freezeSlot(2).used);
    CHECK(std::memcmp(&frz, &b.freezeSlot(2), sizeof frz) == 0);
    CHECK(e.gestLive(1).len == 33333);
    CHECK(e.gestLive(1).count == 2);
    CHECK(e.gestLive(1).ev[1].a == CP::Cutoff);
    CHECK(b.sceneUsed(4));
    CHECK(!b.sceneUsed(3));
    CHECK(e.rt[0].frzReady);
    // a newer version is refused, garbage is refused, truncation does not crash
    auto& bytes = rig.fs.files["Lab/BULUT/Projects/T1.blt"];
    std::vector<uint8_t> keep = bytes;
    bytes[4] = 99;
    CHECK(!b.loadProjectFrom("Lab/BULUT/Projects/T1.blt"));
    bytes = keep;
    bytes.resize(bytes.size() / 3);
    b.loadProjectFrom("Lab/BULUT/Projects/T1.blt");
    rig.fs.files["Lab/BULUT/Projects/junk.blt"] = std::vector<uint8_t>(100, 0x5A);
    CHECK(!b.loadProjectFrom("Lab/BULUT/Projects/junk.blt"));
    rig.blocks(50);
}

LAB_TEST(bulut_runs_without_a_card) {
    Rig rig(false);
    CHECK(rig.ok);
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    b.engine().padDown(0, 20, 100);
    rig.blocks(100);
    CHECK(!b.saveProjectTo("Lab/BULUT/Projects/P01.blt"));
    CHECK(!b.loadSample(0, "Samples/x.wav"));
    CHECK(!b.swapSample(0, 1));
    b.startResample();
    rig.blocks(200);
    b.stopResample();
    for (int i = 0; i < 10; ++i) b.tick(uint32_t(i * 10));
    CHECK(!b.resampleBusy());
    CHECK(b.params(1).source == kSrcSample);   // the capture went straight into the channel
    CHECK(std::strncmp(b.source(1).label, "RESAMPLE MIX", 12) == 0);
    b.save();
}

LAB_TEST(bulut_text_is_ascii) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    kit::Registry& P = rig.P();
    auto ascii = [](const char* s) {
        for (const char* p = s; p && *p; ++p)
            if (*p < 32 || *p > 126) return false;
        return true;
    };
    char t[32];
    for (int en = 0; en < kEngines; ++en) {
        b.setEngine(0, en);
        for (int k = 0; k < kKnobs; ++k) {
            CHECK(ascii(knobName(en, k)));
            for (int s = 0; s <= 10; ++s) {
                knobText(en, k, float(s) / 10.0f, t, sizeof t);
                CHECK(ascii(t));
            }
        }
    }
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kChParams; ++i) {
            CHECK(ascii(P.name(b.pid(c, i))));
            P.format(b.pid(c, i), t, sizeof t);
            CHECK(ascii(t));
        }
    kit::PageInfo pages[16];
    const int n = b.navPages(pages, 16);
    CHECK(n == kNavPages);
    for (int i = 0; i < n; ++i) {
        CHECK(ascii(pages[i].name));
        CHECK(ascii(pages[i].help));
        for (int f = 0; f < 8; ++f) CHECK(ascii(pages[i].fkeys[f]));
    }
}
