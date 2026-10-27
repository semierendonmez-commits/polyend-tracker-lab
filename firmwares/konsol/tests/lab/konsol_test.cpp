// KONSOL host tests: clip timing, launch quantisation, generators -> clips,
// piano-roll edits, mixer sanity, project save / load.
#include <cmath>
#include <cstring>
#include <vector>

#include "../mem_storage.h"
#include "gen/gen.h"
#include "lab/lab.h"
#include "lab_test.h"
#include "model/pattern.h"
#include "ws/gen_glue.h"
#include "ws/konsol.h"

using namespace lab;
using namespace lab::konsol;

namespace {

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    MemStorage fs;
    Shell shell;
    Konsol* k = nullptr;
    Transport tr;
    float l[kBlock], r[kBlock];
    float peak = 0.0f;
    explicit Rig(bool card = false) {
        Hal h;
        if (card) h.fs = &fs;
        shell.init(h, mem.data(), uint32_t(mem.size()), "KONSOL");
        k = static_cast<Konsol*>(shell.workstation());
        tr.bpm = 120.0f;
        tr.beatsPerSample = 120.0 / 60.0 / 44100.0;
    }
    // runs n blocks; playing from now on
    void run(int n, bool playing = true) {
        if (shell.playing() != playing) shell.setPlaying(playing);
        for (int b = 0; b < n; ++b) {
            tr.startedThisBlock = playing && !tr.playing;
            tr.stoppedThisBlock = !playing && tr.playing;
            if (tr.startedThisBlock) tr.beat = 0.0;
            tr.playing = playing;
            for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = 0.0f;
            AudioIn in;
            k->render(tr, in, l, r);
            for (uint32_t i = 0; i < kBlock; ++i) peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            if (playing) tr.beat += tr.beatsPerSample * kBlock;
            k->update(uint32_t(b * 3));
        }
    }
    double beat() const { return tr.beat; }
    // Empties the project: no clips, nothing playing.
    void blank() {
        Project& p = k->project();
        for (int t = 0; t < kTracks; ++t)
            for (int s = 0; s < kScenes; ++s) p.clip[t][s].lenSteps = 0;
        for (int t = 0; t < kTracks; ++t) k->rt(t) = TrackRt();
        p.launchQ = 0;
        p.swing = 50;
    }
};

Note mk(int step, int pitch, int lenTicks = kTps - 1) {
    Note n;
    n.start = uint16_t(step * kTps);
    n.len = uint16_t(lenTicks);
    n.pitch = uint8_t(pitch);
    n.vel = 100;
    return n;
}

}  // namespace

LAB_TEST(konsol_boots_with_demo) {
    Rig g;
    CHECK(g.k != nullptr);
    CHECK(std::strcmp(g.k->name(), "KONSOL") == 0);
    const Project& p = g.k->project();
    int clips = 0;
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kScenes; ++s) clips += p.clip[t][s].exists();
    CHECK(clips >= 20);
    CHECK(p.songLen == 6);
    // every generated demo clip has notes
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kScenes; ++s) {
            const Clip& c = p.clip[t][s];
            if (c.exists() && c.gen != kGenNotes) CHECK(c.notes().count > 0);
        }
}

LAB_TEST(konsol_clip_timing_sample_accurate) {
    Rig g;
    g.blank();
    g.k->setEngine(0, kEngIkeda);
    Clip& c = g.k->project().clip[0][0];
    c.lenSteps = 16;
    c.buf[0].clear();
    c.buf[0].add(mk(0, 60));
    c.buf[0].add(mk(4, 62));
    c.buf[0].add(mk(8, 64));
    c.cur = 0;
    g.k->launch(0, 0);
    g.run(1);
    CHECK(g.k->rt(0).play == 0);
    // after 2 s (4 bars of beats = 1 loop of 16 steps = 2 s at 120 BPM) one loop has passed
    g.run(int(44100 * 2 / kBlock) + 2);
    CHECK(g.k->rt(0).loops >= 1);
    // tick runs inside the clip
    CHECK(g.k->rt(0).tick < 16 * kTps);
}

LAB_TEST(konsol_note_offsets_inside_block) {
    // A note at step 1 must start at sample 5512 or 5513 from the clip start.
    Rig g;
    g.blank();
    Clip& c = g.k->project().clip[1][0];
    c.lenSteps = 16;
    c.buf[0].clear();
    c.buf[0].add(mk(1, 60));
    c.cur = 0;
    g.k->setEngine(1, kEngSampler);
    g.k->engine(1)->set(0, 0.0f);   // one-shot from the start (the break's first kick)
    g.k->engine(1)->set(8, 0.0f);   // no attack ramp
    g.k->launch(1, 0);
    // The sampler voice starts at the offset: measure the first non-zero sample.
    int first = -1;
    for (int b = 0; b < 60 && first < 0; ++b) {
        g.tr.startedThisBlock = !g.tr.playing;
        if (g.tr.startedThisBlock) g.tr.beat = 0.0;
        g.tr.playing = true;
        for (uint32_t i = 0; i < kBlock; ++i) g.l[i] = g.r[i] = 0.0f;
        AudioIn in;
        g.k->render(g.tr, in, g.l, g.r);
        for (uint32_t i = 0; i < kBlock; ++i)
            if (g.l[i] != 0.0f) {
                first = b * int(kBlock) + int(i);
                break;
            }
        g.tr.beat += g.tr.beatsPerSample * kBlock;
    }
    CHECK(first >= 5511 && first <= 5514);
}

LAB_TEST(konsol_launch_quantisation) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    for (int s : {0, 1}) {
        Clip& c = p.clip[2][s];
        c.lenSteps = 16;
        c.buf[0].clear();
        c.buf[0].add(mk(0, 48));
        c.cur = 0;
    }
    p.launchQ = 2;   // 1 bar
    g.k->launch(2, 0);
    g.run(2);
    CHECK(g.k->rt(2).play == 0);   // the first launch starts with the transport
    // half a bar later launch scene 1: it must wait for the bar line
    g.run(int(44100 * 1.0 / kBlock));   // 1 s = 2 beats at 120
    g.k->launch(2, 1);
    g.run(2);
    CHECK(g.k->rt(2).play == 0);
    CHECK(g.k->rt(2).queued == 1);
    while (g.beat() < 4.0 - 1e-9) g.run(1);
    g.run(1);
    CHECK(g.k->rt(2).play == 1);
    CHECK(g.k->rt(2).queued == -2);
    // the new clip's tick 0 is exactly the bar line
    CHECK(g.k->rt(2).startPos == uint32_t(4 * kPpq * 256));
    // stop is quantised too
    g.k->stopTrack(2);
    g.run(2);
    CHECK(g.k->rt(2).play == 1);
    while (g.beat() < 8.0) g.run(1);
    g.run(1);
    CHECK(g.k->rt(2).play == -1);
    // immediate launch with quantise None
    p.launchQ = 0;
    g.k->launch(2, 0);
    g.run(1);
    CHECK(g.k->rt(2).play == 0);
}

LAB_TEST(konsol_scene_launch_and_song_mode) {
    Rig g;
    Project& p = g.k->project();
    p.launchQ = 0;
    g.k->launchScene(3);
    g.run(2);
    for (int t = 0; t < kTracks; ++t) CHECK(g.k->rt(t).play == (p.clip[t][3].exists() ? 3 : -1));
    // song mode: entries advance on their bar counts
    g.run(1, false);
    p.song[0] = SongEntry{0, 1};
    p.song[1] = SongEntry{2, 1};
    p.songLen = 2;
    p.songLoop = 0;
    g.k->setSongMode(true);
    g.run(2, true);
    CHECK(g.k->songIndex() == 0);
    while (g.beat() < 4.0) g.run(1);
    g.run(1);
    CHECK(g.k->songIndex() == 1);
    for (int t = 0; t < kTracks; ++t) CHECK(g.k->rt(t).play == (p.clip[t][2].exists() ? 2 : -1));
    while (g.beat() < 8.0) g.run(1);
    g.run(1);
    CHECK(g.k->songIndex() == -1);   // no loop: the song ends
    for (int t = 0; t < kTracks; ++t) CHECK(g.k->rt(t).play == -1);
}

LAB_TEST(konsol_generator_to_clip_conversion) {
    // ot::Pattern steps -> notes: pitch, velocity, gate, slide, ratchet, slice.
    ot::Pattern* p = new ot::Pattern();
    p->setDefault();
    ot::Track& tr = p->tracks[0];
    tr.steps[0].note = 48;
    tr.steps[0].fx[0] = ot::StepFx{18, 100};   // Volume 100 -> accent
    tr.steps[2].note = 50;
    tr.steps[2].fx[0] = ot::StepFx{20, 50};    // Gate 50 %
    tr.steps[3].note = 53;
    tr.steps[3].fx[0] = ot::StepFx{19, 30};    // Glide into it
    tr.steps[6].note = 24 + 5;                 // beat slice 5
    tr.steps[6].fx[0] = ot::StepFx{3, 4};      // Roll rate index 3 -> 4 hits
    tr.steps[10].note = ot::kNoteOff;
    NoteBuf nb;
    Konsol::patternToNotes(*p, 0, 16, nb, false);   // melodic: pitches as written
    CHECK(nb.count == 4);
    CHECK(nb.n[0].start == 0 && nb.n[0].pitch == 48 && nb.n[0].vel == 127);
    CHECK(nb.n[1].start == 2 * kTps && nb.n[1].vel < 127);
    // the note before a slide holds into it (303 legato), even with a gate
    CHECK(nb.n[2].start == 3 * kTps && (nb.n[2].flags & kFlagSlide));
    CHECK(nb.n[1].start + nb.n[1].len > nb.n[2].start);
    CHECK(nb.n[3].pitch == 24 + 5 && nb.n[3].roll == 4);
    CHECK(nb.n[3].start + nb.n[3].len <= 10 * kTps);   // ends at the Off
    // a plain gate without a following slide
    p->tracks[0].steps[3].fx[0] = ot::StepFx{0, 0};
    Konsol::patternToNotes(*p, 0, 16, nb, false);
    CHECK(nb.n[1].len == kTps / 2);
    // BBCut / slices: Beat Slice notes 24 + n become slice pads 36 + n
    Konsol::patternToNotes(*p, 0, 16, nb, true);
    CHECK(nb.n[3].pitch == 36 + 5);
    Konsol::patternToNotes(*p, 0, 16, nb, false);
    // and back: notes -> pattern keep pitch / gate
    ot::Pattern* q = new ot::Pattern();
    q->setDefault();
    Konsol::notesToPattern(nb, 16, *q, 0, 0);
    CHECK(q->tracks[0].steps[0].note == 48);
    CHECK(q->tracks[0].steps[2].note == 50);
    delete p;
    delete q;
}

LAB_TEST(konsol_every_generator_writes_a_clip) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    for (int kind = kGenAcid; kind < kGenCount; ++kind) {
        const int t = kind % kTracks;
        g.k->setEngine(t, kind == kGenBBCut ? kEngSampler : (kind == kGenUsul ? kEngDrums : kEngAcd));
        Clip& c = p.clip[t][1];
        c.lenSteps = 16;
        c.buf[0].clear();
        c.buf[1].clear();
        c.cur = 0;
        g.k->defaultGen(c, kind);
        if (kind == kGenGenotype || kind == kGenTurnaround)   // vary existing material
            for (int s = 0; s < 16; s += 2) c.buf[0].add(mk(s, 48 + s));
        char msg[48] = {};
        const bool ok = g.k->runGenerator(t, 1, 1234, msg, sizeof msg);
        if (!ok) std::fprintf(stderr, "  %s: %s\n", genName(kind), msg);
        CHECK(ok);
        const NoteBuf& nb = c.notes();
        CHECK(nb.count > 0);
        for (int i = 0; i < nb.count; ++i) {
            CHECK(nb.n[i].start < c.lenSteps * kTps);
            CHECK(nb.n[i].len >= 1);
            CHECK(nb.n[i].pitch <= 127);
            CHECK(nb.n[i].vel >= 1 && nb.n[i].vel <= 127);
        }
        // deterministic for a fixed seed parameter
        c.gp[kGenParams - 1] = 77;
        g.k->runGenerator(t, 1, 1, nullptr, 0);
        NoteBuf a;
        std::memcpy(&a, &c.notes(), sizeof a);
        g.k->runGenerator(t, 1, 999, nullptr, 0);
        if (kind != kGenStation && kind != kGenGenotype && kind != kGenTurnaround) {
            CHECK(a.count == c.notes().count);
            bool same = a.count == c.notes().count;
            for (int i = 0; same && i < a.count; ++i)
                same = a.n[i].start == c.notes().n[i].start && a.n[i].pitch == c.notes().n[i].pitch;
            CHECK(same);
        }
    }
    // Acid notes are in the key's scale (A minor)
    g.k->setEngine(1, kEngAcd);
    Clip& ac = p.clip[1][2];
    ac.lenSteps = 16;
    g.k->defaultGen(ac, kGenAcid);
    ac.gp[0] = 100;
    CHECK(g.k->runGenerator(1, 2, 5, nullptr, 0));
    const uint16_t minor = 0x05AD;
    for (int i = 0; i < ac.notes().count; ++i) {
        const int pc = ((ac.notes().n[i].pitch - p.key) % 12 + 12) % 12;
        CHECK((minor >> pc) & 1);
    }
    // usul: the clip length follows the cycle (9/8 x 2 = 36 steps)
    CHECK(usulSteps(0, 2) == 36);
}

LAB_TEST(konsol_live_generator_swaps_at_loop) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    g.k->setEngine(2, kEngAcd);
    Clip& c = p.clip[2][0];
    c.lenSteps = 16;
    g.k->defaultGen(c, kGenAcid);
    c.gp[4] = 100;   // mutate everything
    c.live = 1;
    c.every = 1;
    CHECK(g.k->runGenerator(2, 0, 3, nullptr, 0));
    g.k->launch(2, 0);
    g.run(3);   // started: the first live variation is queued (update())
    CHECK(c.pending == 1);
    const uint8_t cur0 = c.cur;
    // before the loop end the playing buffer stays
    g.run(int(44100 * 1.5 / kBlock));
    CHECK(c.cur == cur0);
    // after the loop point the audio swapped it in, and update() queued the next
    g.run(int(44100 * 0.6 / kBlock));
    CHECK(c.cur != cur0);
    CHECK(g.k->rt(2).loops >= 1);
    g.run(3);
    CHECK(c.pending == 1);
}

LAB_TEST(konsol_piano_roll_edits) {
    Clip* c = new Clip();
    c->lenSteps = 16;
    NoteBuf& b = c->beginEdit();
    b.add(mk(4, 60));
    b.add(mk(0, 64));
    b.add(mk(0, 60));
    c->commit();
    const NoteBuf& n = c->notes();
    CHECK(n.count == 3);
    // sorted by start, then pitch
    CHECK(n.n[0].start == 0 && n.n[0].pitch == 60);
    CHECK(n.n[1].start == 0 && n.n[1].pitch == 64);
    CHECK(n.n[2].start == 4 * kTps);
    CHECK(n.at(4 * kTps + 3, 60) == 2);
    CHECK(n.at(5 * kTps, 60) == -1);
    CHECK(n.find(0, 64) == 1);
    // move + resize + delete through an edit copy: the live buffer is untouched until commit
    NoteBuf& e = c->beginEdit();
    e.n[2].start = 8 * kTps;
    e.n[2].len = 3 * kTps;
    e.remove(0);
    e.sort();
    CHECK(c->notes().count == 3);
    c->commit();
    CHECK(c->notes().count == 2);
    CHECK(c->notes().n[1].start == 8 * kTps && c->notes().n[1].len == 3 * kTps);
    // a full clip refuses more notes
    NoteBuf& f = c->beginEdit();
    for (int i = 0; i < 200; ++i) f.add(mk(i % 16, 30 + i % 60));
    CHECK(f.count == kMaxNotes);
    c->commit();
    delete c;
}

LAB_TEST(konsol_mixer_sanity) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    g.k->setEngine(3, kEngIkeda);
    Clip& c = p.clip[3][0];
    c.lenSteps = 4;
    for (int s = 0; s < 4; ++s) c.buf[0].add(mk(s, 60, kTps * 2));
    g.k->launch(3, 0);
    Mix& m = p.track[3].mix;
    m.level = 0.8f;
    m.pan = 0.0f;
    m.sendA = m.sendB = 0.0f;   // no send tails in the mute / solo checks
    g.run(200);
    const float open = g.peak;
    CHECK(open > 0.01f);
    CHECK(open < 1.0f);   // master limiter
    // mute: silence after the ramp
    m.mute = 1;
    g.run(4);
    g.peak = 0;
    g.run(100);
    CHECK(g.peak < 1e-4f);
    m.mute = 0;
    // solo another track: this one goes quiet
    p.track[5].mix.solo = 1;
    g.run(4);
    g.peak = 0;
    g.run(100);
    CHECK(g.peak < 1e-4f);
    p.track[5].mix.solo = 0;
    // hard pan right: the left channel stays empty
    m.pan = 1.0f;
    p.fx.retA = p.fx.retB = 0.0f;
    m.sendA = m.sendB = 0.0f;
    g.run(8);
    float lmax = 0, rmax = 0;
    for (int b = 0; b < 100; ++b) {
        g.run(1);
        for (uint32_t i = 0; i < kBlock; ++i) {
            lmax = std::fmax(lmax, std::fabs(g.l[i]));
            rmax = std::fmax(rmax, std::fabs(g.r[i]));
        }
    }
    CHECK(rmax > 0.01f);
    CHECK(lmax < 1e-4f);
    // finite everywhere with the busiest bench state
    for (int s = 0; s < g.k->benchScenarios(); ++s) {
        g.k->benchSetup(s);
        g.peak = 0;
        g.run(400);
        CHECK(std::isfinite(g.peak));
        CHECK(g.peak <= 1.0f);
    }
}

LAB_TEST(konsol_save_load_roundtrip) {
    Rig g(true);
    Project& p = g.k->project();
    // edit some state
    p.bpm = 133.0f;
    g.shell.setBpm(133.0f);
    p.key = 2;
    p.scale = 3;
    p.swing = 61;
    p.track[4].mix.level = 0.33f;
    p.track[4].mix.pan = -0.5f;
    p.track[4].mix.stem = 2;
    p.track[4].mix.midiOut = 1;
    p.fx.delayFb = 0.7f;
    p.fx.master = 1.1f;
    g.k->setEngine(6, kEngWtfm);
    g.k->engine(6)->set(16, 2345.0f);   // WTFM cutoff
    g.k->engineOf(2, kEngMakam)->set(0, 5.0f);
    Clip& c = p.clip[7][9];
    c.lenSteps = 24;
    c.gen = kGenMarkov;
    c.live = 1;
    c.every = 3;
    c.gp[2] = 42;
    c.buf[c.cur].clear();
    Note n = mk(5, 70, 30);
    n.tone = -12;
    n.chance = 60;
    n.roll = 4;
    n.flags = kFlagSlide;
    c.buf[c.cur].add(n);
    p.song[0] = SongEntry{9, 12};
    p.songLen = 1;
    std::snprintf(p.name, sizeof p.name, "ROUND");
    CHECK(g.k->saveProject("ROUND"));
    // a different project in between
    g.k->newProject(false);
    CHECK(g.k->project().clip[7][9].lenSteps == 0);
    CHECK(g.k->loadProject("ROUND"));
    Project& q = g.k->project();
    CHECK(std::strcmp(q.name, "ROUND") == 0);
    CHECK_NEAR(q.bpm, 133.0, 0.01);
    CHECK(q.key == 2 && q.scale == 3 && q.swing == 61);
    CHECK_NEAR(q.track[4].mix.level, 0.33, 1e-6);
    CHECK_NEAR(q.track[4].mix.pan, -0.5, 1e-6);
    CHECK(q.track[4].mix.stem == 2 && q.track[4].mix.midiOut == 1);
    CHECK_NEAR(q.fx.delayFb, 0.7, 1e-6);
    CHECK_NEAR(q.fx.master, 1.1, 1e-6);
    CHECK(q.track[6].engine == kEngWtfm);
    CHECK_NEAR(g.k->engine(6)->get(16), 2345.0, 0.5);
    CHECK_NEAR(g.k->engineOf(2, kEngMakam)->get(0), 5.0, 1e-6);
    const Clip& d = q.clip[7][9];
    CHECK(d.lenSteps == 24 && d.gen == kGenMarkov && d.live == 1 && d.every == 3 && d.gp[2] == 42);
    CHECK(d.notes().count == 1);
    const Note& m = d.notes().n[0];
    CHECK(m.start == 5 * kTps && m.len == 30 && m.pitch == 70 && m.tone == -12 && m.chance == 60 && m.roll == 4 &&
          m.flags == kFlagSlide);
    CHECK(q.songLen == 1 && q.song[0].scene == 9 && q.song[0].bars == 12);
    // a damaged file does not load
    g.fs.files["Lab/KONSOL/Projects/BAD.kon"] = std::vector<uint8_t>{'N', 'O', 'P', 'E', 0, 0, 0, 0, 0};
    CHECK(!g.k->loadProject("BAD"));
    // a truncated file loads its complete chunks and stays sane
    std::vector<uint8_t> f = g.fs.files["Lab/KONSOL/Projects/ROUND.kon"];
    f.resize(f.size() / 2);
    g.fs.files["Lab/KONSOL/Projects/HALF.kon"] = f;
    CHECK(g.k->loadProject("HALF"));
    CHECK(std::strcmp(g.k->project().name, "HALF") == 0);
}

LAB_TEST(konsol_no_card_is_safe) {
    Rig g(false);
    CHECK(!g.k->saveProject("X"));
    CHECK(!g.k->loadProject("X"));
    g.k->save();
    g.run(50);
}

LAB_TEST(konsol_live_recording_quantised) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    p.recQ = 1;   // 1/16
    g.k->setEngine(0, kEngIkeda);
    Clip& c = p.clip[0][0];
    c.lenSteps = 16;
    c.buf[0].clear();
    c.cur = 0;
    g.k->select(0, 0);
    g.k->launch(0, 0);
    g.run(2);
    Event e;
    e.type = Event::kPress;
    e.id = btn::Rec;
    g.k->onEvent(e);   // arm (the clip already plays)
    // advance to just after step 4 (2.0 beats = step 8; step 4 = 1 beat) and play a note
    while (g.beat() < 1.02) g.run(1);
    MidiMsg on{0x90, 64, 90};
    g.k->onMidi(on);
    while (g.beat() < 1.5) g.run(1);
    MidiMsg off{0x80, 64, 0};
    g.k->onMidi(off);
    g.run(2);
    CHECK(c.notes().count == 1);
    if (c.notes().count == 1) {
        const Note& n = c.notes().n[0];
        CHECK(n.start == 4 * kTps);   // quantised back to step 5
        CHECK(n.pitch == 64 && n.vel == 90);
        CHECK(n.len == 2 * kTps);
    }
    g.k->onEvent(e);   // disarm
    g.k->onMidi(on);
    g.run(5);
    g.k->onMidi(off);
    CHECK(c.notes().count == 1);
}

LAB_TEST(konsol_every_player_on_every_engine_renders) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    p.launchQ = 0;
    for (int eng = 0; eng < kEngCount; ++eng) {
        const int t = eng % kTracks;
        g.k->setEngine(t, eng);
        for (int kind = kGenAcid; kind < kGenCount; ++kind) {
            Clip& c = p.clip[t][kind];
            c.lenSteps = 16;
            c.buf[0].clear();
            c.buf[1].clear();
            c.cur = 0;
            g.k->defaultGen(c, kind);
            for (int s = 0; s < 16; s += 3) c.buf[0].add(mk(s, 40 + s));
            g.k->runGenerator(t, kind, 99, nullptr, 0);
        }
        // play each clip a little, all must stay finite and bounded
        for (int kind = kGenAcid; kind < kGenCount; ++kind) {
            g.k->launch(t, kind);
            g.peak = 0;
            g.run(60);
            CHECK(std::isfinite(g.peak) && g.peak <= 1.0f);
        }
        g.k->stopTrack(t);
        g.run(2);
    }
}

// ============================================================================
// round 2: samples, stretch, kits, new engines, FX, conditions, undo, LABKIT
// ============================================================================
#include "ws/fx.h"
#include "ws/kit.h"
#include "ws/samples.h"
#include "ws/stretch.h"

namespace {
// A 16-bit PCM WAV (interleaved), n frames of a decaying sine burst every 0.25 s.
std::vector<uint8_t> makeWav(int ch, uint32_t frames, float hz) {
    std::vector<uint8_t> w(44 + frames * 2u * uint32_t(ch));
    auto u32 = [&](int at, uint32_t v) { std::memcpy(&w[size_t(at)], &v, 4); };
    auto u16 = [&](int at, uint16_t v) { std::memcpy(&w[size_t(at)], &v, 2); };
    std::memcpy(&w[0], "RIFF", 4);
    u32(4, uint32_t(w.size() - 8));
    std::memcpy(&w[8], "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, uint16_t(ch));
    u32(24, 44100);
    u32(28, 44100u * 2u * uint32_t(ch));
    u16(32, uint16_t(2 * ch));
    u16(34, 16);
    std::memcpy(&w[36], "data", 4);
    u32(40, frames * 2u * uint32_t(ch));
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = float(i % 11025) / 44100.0f;
        const float v = std::sin(6.2831853f * hz * t) * std::exp(-t * 20.0f);
        for (int c = 0; c < ch; ++c) {
            const int16_t s = int16_t(v * (c ? 12000.0f : 20000.0f));
            std::memcpy(&w[44 + (i * uint32_t(ch) + uint32_t(c)) * 2], &s, 2);
        }
    }
    return w;
}
bool finiteAll(const float* x, int n) {
    for (int i = 0; i < n; ++i)
        if (!(x[i] == x[i]) || std::fabs(x[i]) > 100.0f) return false;
    return true;
}
}  // namespace

LAB_TEST(konsol_pool_stereo_break_and_onsets) {
    Rig g;
    SamplePool& pool = g.k->pool();
    const TrackSample& ts = g.k->trackSample(0);
    CHECK(pool.valid(ts.slot));
    const SampleSlot& sl = pool.slot(ts.slot);
    CHECK(sl.ch == 2);
    CHECK(std::abs(int(sl.frames) - int(8.0f * 60.0f / 172.0f * 44100.0f)) < 4);
    // 22 hits in the built-in break; the detector finds most of them
    CHECK(sl.nOn >= 16 && sl.nOn <= 30);
    // the kicks start the bar: the first onset is a kick
    CHECK(sl.cls[0] == kClsKick);
    int kicks = 0, hats = 0, snares = 0;
    for (int k = 0; k < sl.nOn; ++k) {
        kicks += sl.cls[k] == kClsKick;
        hats += sl.cls[k] == kClsHat;
        snares += sl.cls[k] == kClsSnare;
    }
    std::printf("    break: %d onsets, %d kicks %d snares %d hats\n", sl.nOn, kicks, snares, hats);
    CHECK(kicks >= 3 && kicks <= 6 && snares >= 5 && hats >= 6);
    int bars = 0;
    CHECK_NEAR(loopBpm(sl.frames, bars, 120.0f, &sl), 172.0, 0.5);
    CHECK(bars == 2);
}

LAB_TEST(konsol_stereo_wav_load_and_slices) {
    Rig g(true);
    g.fs.files["Lab/KONSOL/Samples/loop.wav"] = makeWav(2, 44100 * 2, 220.0f);
    CHECK(g.k->loadSample(1, "Lab/KONSOL/Samples/loop.wav"));
    const TrackSample& ts = g.k->trackSample(1);
    SamplePool& pool = g.k->pool();
    CHECK(pool.valid(ts.src) && pool.slot(ts.src).ch == 2);
    CHECK(pool.slot(ts.src).frames == 44100u * 2u);
    // 8 bursts: one onset at each
    CHECK(pool.slot(ts.src).nOn >= 6 && pool.slot(ts.src).nOn <= 10);
    // the right channel is its own (12000 vs 20000 peak)
    const int16_t* L = pool.left(ts.src);
    const int16_t* R = pool.right(ts.src);
    int pl = 0, pr = 0;
    for (uint32_t i = 0; i < 4000; ++i) {
        pl = std::max(pl, std::abs(int(L[i])));
        pr = std::max(pr, std::abs(int(R[i])));
    }
    CHECK(pl > 15000 && pr > 9000 && pr < 13000);
    // the slice editor: add / move / remove a point
    const int s = ts.src;
    const int n0 = pool.slot(s).nOn;
    uint32_t a, e;
    CHECK(pool.sliceRange(s, 1, a, e));
    const int k = pool.addOnset(s, a + (e - a) / 2);
    CHECK(k == 2 && pool.slot(s).nOn == n0 + 1);
    const uint32_t before = pool.slot(s).onset[2];
    pool.moveOnset(s, 2, 300);
    CHECK(pool.slot(s).onset[2] == before + 300);
    pool.removeOnset(s, 2);
    CHECK(pool.slot(s).nOn == n0);
    pool.removeOnset(s, 0);   // the first point stays
    CHECK(pool.slot(s).nOn == n0 && pool.slot(s).onset[0] == 0);
}

LAB_TEST(konsol_stretch_to_tempo_every_mode) {
    for (int mode = 0; mode < kStrModes; ++mode) {
        Rig g;
        g.shell.setBpm(122.0f);
        TrackSample& ts = g.k->trackSample(1);
        ts.mode = uint8_t(mode);
        ts.pitch = mode == 1 ? 3 : 0;
        const uint32_t srcFrames = g.k->pool().slot(ts.src).frames;
        CHECK(g.k->stretchTrack(1));
        int guard = 0;
        while (g.k->stretchProgress() >= 0.0f && guard++ < 10000) g.k->stretchService(4096);
        CHECK(ts.stretched == 1);
        CHECK(g.k->pool().valid(ts.slot) && ts.slot != ts.src);
        const SampleSlot& o = g.k->pool().slot(ts.slot);
        const float want = float(srcFrames) * 172.0f / 122.0f;
        CHECK(std::fabs(float(o.frames) - want) < want * 0.01f);
        CHECK_NEAR(ts.outBpm, 122.0, 0.01);
        // not silent, no clipping garbage
        int pk = 0;
        for (uint32_t i = 0; i < o.frames; ++i) pk = std::max(pk, std::abs(int(g.k->pool().left(ts.slot)[i])));
        CHECK(pk > 3000);
        // onsets came along, time-scaled
        CHECK(o.nOn >= 10);
        g.k->unstretch(1);
        CHECK(ts.slot == ts.src && !ts.stretched);
    }
}

LAB_TEST(konsol_kit_from_slices_plays_and_chokes) {
    Rig g;
    g.blank();
    const int n = g.k->kitFromSlices(2, true);
    CHECK(n == 16);
    g.k->setEngine(2, kEngKit);
    KitPad* pads = kitPads(g.k->engineOf(2, kEngKit));
    CHECK(pads != nullptr);
    // by class: kicks first
    CHECK(pads[0].cls == kClsKick);
    for (int i = 0; i < 16; ++i) CHECK(g.k->pool().valid(pads[i].slot) && pads[i].e > pads[i].a);
    Engine* e = g.k->engine(2);
    float l[kBlock], r[kBlock];
    // two hats of one choke group: the second fades the first
    int h1 = -1, h2 = -1;
    for (int i = 0; i < 16; ++i)
        if (pads[i].cls == kClsHat) (h1 < 0 ? h1 : h2) = i;
    if (h1 >= 0 && h2 >= 0) {
        e->noteOn(kKitBase + h1, 1.0f, 0, 0, 0);
        e->noteOn(kKitBase + h2, 1.0f, 0, 0, 0);
        for (int b = 0; b < 40; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = 0;
            e->render(l, r);
        }
        CHECK(e->active() <= 1);
    }
    // every pad sounds
    float pk = 0;
    for (int p = 0; p < 16; ++p) {
        e->allOff(true);
        e->noteOn(kKitBase + p, 1.0f, 0, 0, 0);
        for (int b = 0; b < 10; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = 0;
            e->render(l, r);
            CHECK(finiteAll(l, kBlock) && finiteAll(r, kBlock));
            for (uint32_t i = 0; i < kBlock; ++i) pk = std::fmax(pk, std::fabs(l[i]));
        }
    }
    CHECK(pk > 0.05f);
}

LAB_TEST(konsol_kit_save_load_card) {
    Rig g(true);
    g.fs.files["Lab/KONSOL/Samples/Kit1/01 kick.wav"] = makeWav(1, 8000, 60.0f);
    g.fs.files["Lab/KONSOL/Samples/Kit1/02 snare.wav"] = makeWav(1, 8000, 200.0f);
    g.fs.files["Lab/KONSOL/Samples/Kit1/03 hat.wav"] = makeWav(2, 4000, 5000.0f);
    g.fs.files["Lab/KONSOL/Samples/Kit1/04 tom.wav"] = makeWav(1, 8000, 120.0f);
    const int n = g.k->kitFromFolder(3, "Lab/KONSOL/Samples/Kit1");
    CHECK(n == 4);
    KitPad* pads = kitPads(g.k->engineOf(3, kEngKit));
    CHECK(pads[0].cls == kClsKick && std::strcmp(pads[0].name, "01 KICK") == 0);
    int sn = -1, ht = -1;
    for (int i = 0; i < 4; ++i) {
        if (!std::strcmp(pads[i].name, "02 SNARE")) sn = i;
        if (!std::strcmp(pads[i].name, "03 HAT")) ht = i;
    }
    CHECK(sn > 0 && ht > sn && pads[sn].cls == kClsSnare && pads[ht].cls == kClsHat);
    pads[sn].tune = 5.0f;
    pads[ht].choke = 2;
    CHECK(g.k->saveKit(3, "MYKIT"));
    // clear and load back on another track
    CHECK(g.k->loadKit(5, "Lab/KONSOL/Kits/MYKIT.kit"));
    KitPad* q = kitPads(g.k->engineOf(5, kEngKit));
    CHECK(q[0].cls == kClsKick && q[sn].tune == 5.0f && q[ht].choke == 2);
    CHECK(g.k->pool().valid(q[3].slot));
    CHECK(q[0].slot == pads[0].slot);   // the same file is loaded once
    // the project keeps kits and the sample settings
    TrackSample& ts = g.k->trackSample(5);
    ts.bars = 4;
    ts.mode = kStrTonal;
    g.k->setEngine(5, kEngKit);
    CHECK(g.k->saveProject("KITS"));
    g.k->newProject(false);
    CHECK(!g.k->pool().valid(kitPads(g.k->engineOf(5, kEngKit))[0].slot));
    CHECK(g.k->loadProject("KITS"));
    q = kitPads(g.k->engineOf(5, kEngKit));
    CHECK(q[sn].tune == 5.0f && g.k->pool().valid(q[3].slot));
    CHECK(g.k->trackSample(5).bars == 4 && g.k->trackSample(5).mode == kStrTonal);
    CHECK(g.k->project().track[5].engine == kEngKit);
}

LAB_TEST(konsol_new_engines_render_finite) {
    Rig g;
    float l[kBlock], r[kBlock];
    static const int kNew[6] = {kEngPlaits, kEngFloater, kEngTb303, kEngNeuro, kEngQuanta, kEngSampler};
    for (int kind : kNew) {
        Engine* e = g.k->engineOf(4, kind);
        e->preset(0);
        const int nModels = kind == kEngPlaits ? 24 : (kind == kEngNeuro ? 12 : (kind == kEngQuanta ? 5 : 1));
        for (int m = 0; m < nModels; ++m) {
            if (nModels > 1) e->set(0, float(m));
            if (kind == kEngSampler) e->set(5, 1.0f + float(m % 2));   // warp
            e->allOff(true);
            float pk = 0;
            for (int b = 0; b < 120; ++b) {
                if (b % 30 == 0) e->noteOn(kind == kEngFloater ? 36 + (b / 30) * 2 : 48 + b / 30, 0.9f, 0, 0, b % 60 ? kFlagSlide : 0);
                if (b % 30 == 20) e->noteOff(48 + b / 30);
                for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = 0;
                e->render(l, r);
                CHECK(finiteAll(l, kBlock) && finiteAll(r, kBlock));
                for (uint32_t i = 0; i < kBlock; ++i) pk = std::fmax(pk, std::fabs(l[i]) + std::fabs(r[i]));
            }
            if (pk < 0.005f) std::printf("    silent: %s model %d\n", engineName(kind), m);
            CHECK(pk > 0.005f);
        }
    }
}

LAB_TEST(konsol_send_fx_types_finite) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    g.k->setEngine(0, kEngDrums);
    Clip& c = p.clip[0][0];
    c.lenSteps = 16;
    c.buf[0].clear();
    for (int s = 0; s < 16; s += 2) c.buf[0].add(mk(s, 36 + (s % 3)));
    p.track[0].mix.sendA = 1.0f;
    p.track[0].mix.sendB = 1.0f;
    for (int rt = 0; rt < kRevTypes; ++rt)
        for (int dt = 0; dt < kDlyTypes; ++dt) {
            p.fx.revType = uint8_t(rt);
            p.fx.dlyType = uint8_t(dt);
            p.fx.delayFb = 0.9f;
            p.fx.revSize = 1.0f;
            g.k->launch(0, 0);
            g.peak = 0;
            g.run(200);
            CHECK(finiteAll(g.l, kBlock) && finiteAll(g.r, kBlock));
            CHECK(g.peak > 0.01f && g.peak < 4.0f);
        }
}

LAB_TEST(konsol_trig_conditions_and_fill) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    g.k->setEngine(0, kEngIkeda);
    Clip& c = p.clip[0][0];
    c.lenSteps = 4;
    c.buf[0].clear();
    Note n = mk(0, 60);
    n.cond = kCond12;   // first of every two loops
    c.buf[0].add(n);
    g.k->launch(0, 0);
    // count note-ons per loop through the track flash
    int fired[6] = {};
    for (int loop = 0; loop < 6; ++loop) {
        const double end = double(loop) + 0.5;   // 4 steps = 1 beat; a window around each loop start
        while (g.beat() < end) {
            g.run(1);
            if (g.k->rt(0).flash == 5) ++fired[loop];
        }
    }
    std::printf("    fired %d %d %d %d %d %d\n", fired[0], fired[1], fired[2], fired[3], fired[4], fired[5]);
    CHECK(fired[0] && !fired[1] && fired[2] && !fired[3] && fired[4] && !fired[5]);
    CHECK(condPasses(kCondFill, 3, true) && !condPasses(kCondFill, 3, false));
    CHECK(condPasses(kCondFirst, 0, false) && !condPasses(kCondFirst, 1, false));
    CHECK(condPasses(kCond34, 6, false) && !condPasses(kCond34, 7, false));
}

LAB_TEST(konsol_undo_redo_many_levels) {
    Rig g;
    g.blank();
    g.k->select(1, 2);
    g.k->setPage(kPgSteps);
    // ten step toggles, one undo record each
    for (int s = 0; s < 10; ++s) {
        Event d;
        d.type = Event::kPadDown;
        d.id = uint8_t(s < 8 ? s : 12 + s - 8);
        d.velocity = 100;
        g.k->onEvent(d);
        d.type = Event::kPadUp;
        g.k->onEvent(d);
    }
    CHECK(g.k->project().clip[1][2].notes().count == 10);
    Event u;
    u.type = Event::kPress;
    u.id = btn::Copy;
    u.shift = true;   // Shift + Copy = undo
    for (int i = 0; i < 4; ++i) g.k->onEvent(u);
    CHECK(g.k->project().clip[1][2].notes().count == 6);
    Event r;
    r.type = Event::kPress;
    r.id = btn::Delete;
    r.shift = true;   // Shift + Delete = redo
    g.k->onEvent(r);
    g.k->onEvent(r);
    CHECK(g.k->project().clip[1][2].notes().count == 8);
}

LAB_TEST(konsol_labkit_slots_follow_the_engine) {
    Rig g;
    kit::Registry& R = g.shell.context().params();
    // track 3's slots follow its engine (ACD in the demo)
    g.k->setEngine(2, kEngTb303);
    g.k->refreshSlots(2);
    const kit::ParamId cut = g.k->slotId(2, 1);   // TB303 "Cutoff"
    CHECK(R.valid(cut));
    CHECK(std::strcmp(R.name(cut), "Cutoff") == 0);
    // a registry change reaches the engine on the next audio block
    R.set(cut, 0.25f);
    R.applyNow();   // what the Shell does before every render()
    CHECK_NEAR(g.k->engine(2)->get(1), 0.25, 0.01);
    // a Sound-page edit reaches the registry
    g.k->engine(2)->set(1, 0.8f);
    g.k->slotEdited(2, 1);
    CHECK_NEAR(R.get(cut), 0.8, 0.01);
    // the mixer and the send FX are registered
    CHECK(R.valid(R.find("t1.lvl")) && R.valid(R.find("fx.rtype")));
    R.set(R.find("fx.rtype"), 2.0f);
    R.applyNow();
    CHECK(g.k->project().fx.revType == 2);
}

LAB_TEST(konsol_midi_in_channel_per_track) {
    Rig g;
    g.blank();
    Project& p = g.k->project();
    p.track[5].midiCh = 3;
    g.k->select(0, 0);
    MidiMsg on{0x92, 60, 100};   // channel 3
    g.k->onMidi(on);
    g.run(1, false);
    CHECK(g.k->engine(5)->active() > 0);
    MidiMsg off{0x82, 60, 0};
    g.k->onMidi(off);
    MidiMsg on1{0x90, 62, 100};  // channel 1: nobody claims it -> the selected track
    g.k->onMidi(on1);
    g.run(1, false);
    CHECK(g.k->engine(0)->active() > 0);
}
