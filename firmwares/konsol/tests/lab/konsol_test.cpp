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
