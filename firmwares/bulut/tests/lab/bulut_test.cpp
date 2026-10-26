// BULUT host tests: grain scheduler counts and limits, window / pitch math,
// spectral freeze stability, gesture record / loop timing, the resample WAV
// round trip and the project save / load round trip.
#include <cmath>
#include <cstring>
#include <vector>

#include "formats/wav.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "mem_storage.h"
#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

using namespace bulut;

namespace {

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    MemStorage fs;
    lab::Shell shell;
    bool ok = false;
    lab::Transport tr;
    int16_t input[lab::kBlock * 2] = {};
    uint32_t n = 0;
    explicit Rig(bool card = true) {
        lab::Hal h;
        if (card) h.fs = &fs;
        ok = shell.init(h, mem.data(), uint32_t(mem.size()), "BULUT");
        tr.playing = true;
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
    }
    Bulut& ws() { return *static_cast<Bulut*>(shell.workstation()); }
    // One block with a 330 Hz test tone on the input; returns the block peak.
    float block(float* lOut = nullptr, float* rOut = nullptr) {
        for (uint32_t i = 0; i < lab::kBlock; ++i, ++n) {
            const int16_t v = int16_t(12000.0f * sinf(6.2831853f * 330.0f * float(n) / 44100.0f));
            input[2 * i] = input[2 * i + 1] = v;
        }
        lab::AudioIn in;
        in.frames = input;
        float l[lab::kBlock] = {}, r[lab::kBlock] = {};
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
};

}  // namespace

// ---- scheduler -----------------------------------------------------------------
LAB_TEST(bulut_grain_rate_follows_density) {
    Rig rig;
    CHECK(rig.ok);
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    CloudState& s = b.cloud(0);
    s.p[0][P::Size] = 0.0f;                         // 10 ms grains: no budget pressure
    s.p[0][P::Dens] = 0.5f;                         // + row offset of pad 12 (row 1) = 0.62
    e.padDown(0, 12, 100);
    rig.blocks(20);                                 // settle
    const uint32_t s0 = e.rt[0].spawned;
    const int kBlocks = 3445;                       // ~10 s
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
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    CloudState& s = b.cloud(1);
    s.set(0, P::Engine, 1.0f);
    s.p[1][P::QLaw] = enumValue(kLawSync, kLaws);
    s.p[1][P::QRate] = 0.4f;
    s.p[1][P::QDur] = 0.0f;                          // 0.5 ms particles
    const float hz = rateHz(0.4f);
    for (int i = 0; i < 8; ++i) CHECK_NEAR(e.nextInterval(1, s, hz), 44100.0f / hz, 0.01f);
    s.p[1][P::QLaw] = enumValue(kLawPoisson, kLaws);
    float sum = 0.0f;
    for (int i = 0; i < 4000; ++i) sum += e.nextInterval(1, s, hz);
    CHECK_NEAR(sum / 4000.0f, 44100.0f / hz, 44100.0f / hz * 0.06f);   // exponential mean
}

LAB_TEST(bulut_grain_budgets_hold) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    e.grainBudget = kMaxGrains;
    e.cloudBudget = kMaxCloudGrains;
    for (int c = 0; c < kClouds; ++c) {
        CloudState& s = b.cloud(c);
        s.p[0][P::Dens] = 1.0f;
        s.p[0][P::Size] = 1.0f;
        s.p[1][P::QRate] = 1.0f;
        s.p[1][P::QDur] = 1.0f;
        s.set(0, P::Engine, c & 1 ? 1.0f : 0.0f);
        s.drone = 1;
        for (int p = 0; p < 4; ++p) e.padDown(c, p * 11, 127);
    }
    int maxTotal = 0, maxCloud = 0;
    for (int i = 0; i < 2000; ++i) {
        rig.block();
        maxTotal = e.totalActive > maxTotal ? e.totalActive : maxTotal;
        for (int c = 0; c < kClouds; ++c) maxCloud = e.rt[c].active > maxCloud ? e.rt[c].active : maxCloud;
    }
    std::fprintf(stderr, "  max grains %d (limit %d), per cloud %d (limit %d), refused %u\n", maxTotal, kMaxGrains,
                 maxCloud, kMaxCloudGrains, unsigned(e.refusedTotal));
    CHECK(maxTotal <= kMaxGrains);
    CHECK(maxCloud <= kMaxCloudGrains);
    CHECK(maxTotal >= kMaxGrains - 2);   // the budget is actually used
    CHECK(e.refusedTotal > 0);
    // lowering the budget takes effect
    e.grainBudget = 8;
    rig.blocks(400);
    int after = 0;
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
}

// ---- window and pitch math -------------------------------------------------------
LAB_TEST(bulut_windows_are_windows) {
    for (int t = 0; t < kWindows; ++t) {
        CHECK_NEAR(windowValue(t, 0.0f), 0.0f, 1e-3f);
        CHECK(windowValue(t, 0.9999f) < 0.02f);
        float mx = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const float v = windowValue(t, float(i) / 100.0f);
            CHECK(v >= 0.0f && v <= 1.0001f);
            mx = v > mx ? v : mx;
        }
        CHECK(mx > 0.9f);
    }
    // texture 5 is the Hann window
    for (int i = 1; i < 10; ++i) {
        const float x = float(i) / 10.0f;
        CHECK_NEAR(windowValue(5, x), 0.5f - 0.5f * cosf(6.2831853f * x), 2e-3f);
    }
}

LAB_TEST(bulut_pitch_and_scales) {
    CHECK_NEAR(pitchSemi(0.5f), 0.0f, 1e-6f);
    CHECK_NEAR(pitchSemi(1.0f), 24.0f, 1e-5f);
    CHECK_NEAR(pitchSemi(0.0f), -24.0f, 1e-5f);
    CHECK_NEAR(fexp2(12.0f / 12.0f), 2.0f, 1e-5f);
    CHECK_NEAR(fexp2(-1.0f), 0.5f, 1e-6f);
    CHECK_NEAR(fexp2(7.0f / 12.0f), 1.4983071f, 4e-5f);
    for (float x = 0.01f; x < 100.0f; x *= 1.7f) CHECK_NEAR(flog2(x), log2f(x), 2e-5f);
    for (float x = -10.0f; x < 10.0f; x += 0.37f) CHECK_NEAR(fexp2(x) / exp2f(x), 1.0f, 2e-5f);
    CHECK_NEAR(scaleSnap(5.4f, kScaleOct), 0.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(6.6f, kScaleOct), 12.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(6.0f, kScaleFifth), 7.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(-1.2f, kScaleMajor), -1.0f, 1e-6f);   // B below
    CHECK_NEAR(scaleSnap(3.4f, kScaleMinor), 3.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(5.2f, kScalePent), 4.0f, 1e-6f);
    CHECK_NEAR(scaleSnap(3.3f, kScaleFree), 3.3f, 1e-6f);
    CHECK_NEAR(sizeSec(0.0f), 0.01f, 1e-5f);
    CHECK_NEAR(sizeSec(1.0f), 1.0f, 1e-4f);
    CHECK_NEAR(densHz(1.0f), 120.0f, 0.01f);
    CHECK_NEAR(rateHz(1.0f), 1500.0f, 0.2f);
    CHECK_NEAR(freqHz(1.0f), 8000.0f, 4.0f);
}

// A grain at +12 semitones over a known tone comes out an octave up.
LAB_TEST(bulut_grain_pitch_shift_is_an_octave) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    for (int c = 0; c < kClouds; ++c) b.cloud(c).p[0][P::Level] = 0.0f;
    CloudState& s = b.cloud(0);
    s.p[0][P::Level] = 0.7f;
    s.set(0, P::Reverb, 0.0f);
    s.set(0, P::Delay, 0.0f);
    s.p[0][P::Spread] = 0.0f;
    s.p[0][P::Jitter] = 0.0f;
    s.p[0][P::Size] = 1.0f;     // 1 s grains
    s.p[0][P::Dens] = 0.0f;     // sparse
    s.p[0][P::Pitch] = 0.75f;   // +12
    rig.blocks(500);            // 5.8 s of the 330 Hz tone in the buffer (an octave-up 1 s grain reads 1 s back)
    e.padDown(0, 11, 127);   // the newest column: the grain reads the last second
    std::vector<float> out;
    for (int i = 0; i < 240; ++i) {   // 0.7 s
        float l[lab::kBlock];
        rig.block(l);
        out.insert(out.end(), l, l + lab::kBlock);
    }
    // count zero crossings in the middle of the first grain
    int zc = 0;
    const size_t a = 16000, n = 12000;   // the middle of the first grain
    for (size_t i = a + 1; i < a + n; ++i)
        if ((out[i - 1] < 0.0f) != (out[i] < 0.0f)) ++zc;
    const float hz = float(zc) * 0.5f * 44100.0f / float(n);
    std::fprintf(stderr, "  shifted tone %.1f Hz (expected 660)\n", double(hz));
    CHECK_NEAR(hz, 660.0f, 20.0f);
}

// ---- spectral freeze ----------------------------------------------------------------
LAB_TEST(bulut_freeze_is_stable_and_keeps_the_spectrum) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    rig.blocks(200);
    b.captureFreeze(3, kFromSource);
    FreezeSlot& f = b.freezeSlot(3);
    CHECK(f.used);
    // the 330 Hz tone is the strongest bin
    int best = 0;
    for (int k = 1; k < kSpecBins; ++k)
        if (f.mag[k] > f.mag[best]) best = k;
    const float hz = float(best) * 44100.0f / float(Spectral::kAna);
    CHECK_NEAR(hz, 330.0f, 12.0f);
    b.recallFreeze(3);
    CHECK(e.rt[0].frzReady);
    CHECK(b.cloud(0).freeze);
    // the loop: periodic with no click at the seam, steady level over time
    const int16_t* lp = e.rt[0].frz;
    int jump = std::abs(int(lp[0]) - int(lp[kFrzLen - 1]));
    int maxStep = 0;
    for (uint32_t i = 1; i < kFrzLen; ++i) maxStep = std::max(maxStep, std::abs(int(lp[i]) - int(lp[i - 1])));
    CHECK(jump <= maxStep + 64);
    double e0 = 0, e1 = 0;
    for (uint32_t i = 0; i < kFrzLen / 2; ++i) e0 += double(lp[i]) * lp[i];
    for (uint32_t i = kFrzLen / 2; i < kFrzLen; ++i) e1 += double(lp[i]) * lp[i];
    CHECK(e0 > 0 && e1 / e0 > 0.7 && e1 / e0 < 1.4);
    // same slot, same cloud -> the same loop (deterministic recall)
    std::vector<int16_t> copy(lp, lp + kFrzLen);
    b.recallFreeze(3);
    CHECK(std::memcmp(copy.data(), lp, kFrzLen * 2) == 0);
    // rendering a frozen cloud for a long time stays bounded and finite
    b.cloud(0).bed = 1.0f;
    e.padDown(0, 30, 120);
    float pk = 0.0f;
    bool finite = true;
    for (int i = 0; i < 3000; ++i) {
        float l[lab::kBlock];
        const float p = rig.block(l);
        pk = std::fmax(pk, p);
        for (float v : l) finite = finite && std::isfinite(v);
    }
    CHECK(finite);
    CHECK(pk > 0.01f && pk <= 1.0f);
}

// ---- gestures --------------------------------------------------------------------
LAB_TEST(bulut_gesture_record_and_loop_timing) {
    GestureRecorder rec;
    Gesture out{};
    out.speed = 2;
    rec.arm(0, false);
    CHECK(rec.state() == GestureRecorder::kArmed);
    const uint32_t t0 = 1000000;
    rec.start(t0, nullptr, 0);
    rec.pad(true, 5, 100, t0);
    rec.param(0, P::Pos, 0.25f, t0 + 1000);
    rec.param(0, P::Pos, 0.30f, t0 + 1200);   // within 10 ms: replaces the last
    rec.pad(false, 5, 0, t0 + 22050);
    rec.pad(true, 7, 90, t0 + 30000);         // held at stop: closed at the loop end
    CHECK(rec.finish(t0 + 44100, 0, out, nullptr));
    CHECK(out.len == 44100);
    CHECK(out.count == 5);
    CHECK(out.ev[0].type == kGPadDown && out.ev[0].t == 0 && out.ev[0].a == 5);
    CHECK(out.ev[1].type == kGParam && out.ev[1].t == 1000);
    CHECK_NEAR(float(out.ev[1].v) / 65535.0f, 0.30f, 1e-4f);
    CHECK(out.ev[2].type == kGPadUp && out.ev[2].t == 22050);
    CHECK(out.ev[4].type == kGPadUp && out.ev[4].a == 7 && out.ev[4].t == 44099);
    for (int i = 1; i < out.count; ++i) CHECK(out.ev[i - 1].t <= out.ev[i].t);

    // quantised to beats: 1.1 beats at 120 BPM -> 1 beat
    GestureRecorder q;
    Gesture qo{};
    q.arm(1, false);
    q.start(0, nullptr, 0);
    q.pad(true, 1, 100, 0);
    q.pad(false, 1, 0, 100);
    CHECK(q.finish(24255, 22050, qo, nullptr));
    CHECK(qo.len == 22050);

    // overdub merges at the loop position, at the loop's speed
    GestureRecorder od;
    Gesture merged{};
    od.arm(0, true);
    od.start(500000, &out, 40000);   // the loop is at 40,000 of 44,100
    od.pad(true, 9, 100, 500000 + 8000);    // -> (40000 + 8000) % 44100 = 3900
    od.pad(false, 9, 0, 500000 + 9000);
    CHECK(od.finish(500000 + 10000, 0, merged, &out));
    CHECK(merged.len == out.len);
    CHECK(merged.count == out.count + 2);
    bool found = false;
    for (int i = 0; i < merged.count; ++i)
        if (merged.ev[i].type == kGPadDown && merged.ev[i].a == 9) found = merged.ev[i].t == 3900;
    CHECK(found);
}

// The engine plays a recorded gesture back: ghost pads at the right times, looping.
LAB_TEST(bulut_gesture_playback_loops) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    Gesture& g = e.gestIdle(2);
    g.len = 128 * 100;   // 100 blocks
    g.speed = 2;
    g.quant = 0;
    g.count = 3;
    g.ev[0] = GestEvent{128 * 10, kGPadDown, 17, 100};
    g.ev[1] = GestEvent{128 * 30, kGParam, uint8_t(P::Pitch), 65535};
    g.ev[2] = GestEvent{128 * 50, kGPadUp, 17, 0};
    e.postGesture(2);
    b.cloud(2).set(0, P::Engine, 0.0f);
    b.cloud(2).gestOn = 1;
    auto ghostOn = [&] {
        for (int i = kLiveEmitters; i < kEmitters; ++i)
            if (e.rt[2].em[i].on && e.rt[2].em[i].pad == 17) return true;
        return false;
    };
    int onBlocks = 0, firstOn = -1;
    for (int blk = 0; blk < 300; ++blk) {
        rig.block();
        if (ghostOn()) {
            ++onBlocks;
            if (firstOn < 0) firstOn = blk;
        }
        if (blk == 35) CHECK_NEAR(b.cloud(2).p[0][P::Pitch], 1.0f, 1e-4f);
    }
    // on during 40 of every 100 blocks, three loops
    CHECK(firstOn == 10);
    CHECK(onBlocks >= 118 && onBlocks <= 122);
    // double speed: twice as many passes
    e.gestLive(2).speed = 3;
    onBlocks = 0;
    for (int blk = 0; blk < 300; ++blk) {
        rig.block();
        onBlocks += ghostOn() ? 1 : 0;
    }
    CHECK(onBlocks >= 116 && onBlocks <= 124);
}

// ---- resample loop -------------------------------------------------------------------
LAB_TEST(bulut_resample_wav_roundtrip) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    b.cloud(0).p[0][P::Level] = 0.8f;
    e.padDown(0, 6, 120);
    rig.blocks(100);
    b.startResample();
    CHECK(b.resampleBusy());
    for (int i = 0; i < 345; ++i) {   // ~1 s
        rig.block();
        b.tick(uint32_t(i * 3));
    }
    const uint32_t captured = e.capLen;
    b.stopResample();
    for (int i = 0; i < 20 && b.resampleBusy(); ++i) b.tick(2000u + uint32_t(i));
    CHECK(!b.resampleBusy());
    CHECK(captured >= 44100);
    // the WAV on the card holds the captured mix exactly
    const char* path = b.lastResample();
    CHECK(path[0] != 0);
    CHECK(rig.fs.files.count(path) == 1);
    std::vector<int16_t> back(kBufLen);
    ot::WavImportResult res;
    const ot::FmtStatus st = ot::wavImportFile(rig.fs, path, ot::WavImportOptions(), back.data(), back.size(), res);
    CHECK(st == ot::FmtStatus::Ok);
    CHECK(res.frames == captured);
    CHECK(std::memcmp(back.data(), e.capBuf, captured * 2) == 0);
    // ... and became the source of cloud 2 (the default target)
    CHECK(b.source(1).type == kSrcSample);
    CHECK(std::strcmp(b.source(1).path, path) == 0);
    CHECK(e.rt[1].sampleLen == captured);
    CHECK(std::memcmp(e.rt[1].buf, e.capBuf, captured * 2) == 0);
}

// ---- projects -------------------------------------------------------------------------
LAB_TEST(bulut_project_save_load_roundtrip) {
    Rig rig;
    if (!rig.ok) return;
    Bulut& b = rig.ws();
    Engine& e = b.engine();
    // a state worth saving
    b.cloud(0).set(0, P::Size, 0.123f);
    b.cloud(3).set(0, P::Engine, 1.0f);
    b.cloud(3).set(1, P::QType, enumValue(kQTrainlet, kQTypes));
    b.cloud(4).pan = 0.2f;
    b.cloud(5).mute = 1;
    rig.blocks(150);
    b.captureFreeze(2, kFromSource);
    b.recallFreeze(2);
    Gesture& g = e.gestIdle(1);
    g.len = 33333;
    g.count = 2;
    g.speed = 1;
    g.ev[0] = GestEvent{10, kGPadDown, 3, 99};
    g.ev[1] = GestEvent{2000, kGPadUp, 3, 0};
    e.postGesture(1);
    rig.block();
    b.storeScene(4);
    std::vector<uint8_t> clouds(sizeof(CloudState) * kClouds);
    for (int c = 0; c < kClouds; ++c) std::memcpy(clouds.data() + c * sizeof(CloudState), &b.cloud(c), sizeof(CloudState));
    FreezeSlot frz = b.freezeSlot(2);
    CHECK(b.saveProjectTo("Lab/BULUT/Projects/T1.blt"));
    CHECK(rig.fs.files.count("Lab/BULUT/Projects/T1.blt") == 1);
    CHECK(rig.fs.files.count("Lab/BULUT/Projects/T1.blt.tmp") == 0);
    // change everything, then load
    for (int c = 0; c < kClouds; ++c) defaultCloud(b.cloud(c), c);
    b.freezeSlot(2).used = 0;
    Gesture& z = e.gestIdle(1);
    z.len = 0;
    z.count = 0;
    e.postGesture(1);
    rig.block();
    CHECK(b.loadProjectFrom("Lab/BULUT/Projects/T1.blt"));
    rig.block();
    for (int c = 0; c < kClouds; ++c)
        CHECK(std::memcmp(clouds.data() + c * sizeof(CloudState), &b.cloud(c), sizeof(CloudState)) == 0);
    CHECK(b.freezeSlot(2).used);
    CHECK(std::memcmp(&frz, &b.freezeSlot(2), sizeof frz) == 0);
    CHECK(e.gestLive(1).len == 33333);
    CHECK(e.gestLive(1).count == 2);
    CHECK(e.gestLive(1).speed == 1);
    CHECK(e.gestLive(1).ev[1].t == 2000);
    CHECK(b.sceneUsed(4));
    CHECK(!b.sceneUsed(3));
    CHECK(e.rt[0].frzReady);   // the frozen cloud gets its loop back

    // a newer major version is refused, garbage is refused, nothing crashes
    auto& bytes = rig.fs.files["Lab/BULUT/Projects/T1.blt"];
    std::vector<uint8_t> keep = bytes;
    bytes[4] = 99;
    CHECK(!b.loadProjectFrom("Lab/BULUT/Projects/T1.blt"));
    bytes = keep;
    bytes.resize(bytes.size() / 3);   // truncated: loads what is whole, no crash
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
    b.startResample();
    rig.blocks(200);
    b.stopResample();
    for (int i = 0; i < 10; ++i) b.tick(uint32_t(i * 10));
    CHECK(!b.resampleBusy());
    CHECK(b.source(1).type == kSrcSample);   // the capture went straight into the cloud
    b.save();
}

LAB_TEST(bulut_param_text_is_ascii) {
    char t[32];
    for (int en = 0; en < 2; ++en)
        for (int sl = 0; sl < kParams; ++sl)
            for (int k = 0; k <= 10; ++k) {
                paramText(en, sl, float(k) / 10.0f, t, sizeof t);
                for (const char* p = t; *p; ++p) CHECK(*p >= 32 && *p <= 126);
                for (const char* p = paramName(en, sl); *p; ++p) CHECK(*p >= 32 && *p <= 126);
            }
}
