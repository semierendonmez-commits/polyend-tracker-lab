// TILT host tests: the engine feature by feature (matrix walk and determinism,
// drain / ball save, flippers, tilt, bumpers, ramps, combos, multiball,
// gravity, edge properties, memory, mutation, meta, melody, rules, swing /
// delay, choke, capture / replay), the model (shapes, randomizer and locks,
// pattern bytes, parameter table) and the workstation (sound, MIDI, files).
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "lab/lab.h"
#include "lab_test.h"
#include "mem_storage.h"
#include "ws/tilt_engine.h"
#include "ws/tilt_model.h"
#include "ws/tilt_ws.h"

using namespace tilt;

namespace {

struct Rig {
    std::unique_ptr<Pattern> p{new Pattern()};
    std::unique_ptr<Engine> e{new Engine()};
    double beat = 0.0;
    Rig() {
        defaultPattern(*p);
        p->table.saveBars = 0;
        p->table.tiltFlips = 0;
        p->table.tiltVel = 0;
        e->attach(p.get());
        e->reset();
        e->seed(1234);
    }
    void launch() {
        beat = 0.0;
        e->launch(0.0);
    }
    // one move; returns the main ball's state after it (0 = S / drained)
    int move() {
        e->tick(beat);
        beat += e->moveBeats();
        return e->balls[0].active ? e->balls[0].state : -1;
    }
    int notes() const {
        int n = 0;
        for (int i = 0; i < e->nHits; ++i) n += e->hits[i].kind == Hit::kNote;
        return n;
    }
    bool hadEvent(uint8_t ev) const {
        for (int i = 0; i < e->nEvents; ++i)
            if (e->events[i] == ev) return true;
        return false;
    }
};

void clearAll(Pattern& p) {
    std::memset(p.w, 0, sizeof p.w);
    std::memset(p.e, 0, sizeof p.e);
}

class FakeMidi : public ot::IMidiPort {
public:
    int noteOns = 0, noteOffs = 0, ccs = 0;
    std::vector<uint8_t> in;
    bool send(ot::MidiPortId port, const uint8_t* d, uint32_t n) override {
        if (port != ot::MidiPortId::kDin) return true;
        if (n == 3 && (d[0] & 0xF0) == 0x90 && d[2]) ++noteOns;
        if (n == 3 && ((d[0] & 0xF0) == 0x80 || ((d[0] & 0xF0) == 0x90 && !d[2]))) ++noteOffs;
        if (n == 3 && (d[0] & 0xF0) == 0xB0) ++ccs;
        return true;
    }
    uint32_t receive(ot::MidiPortId port, uint8_t* dst, uint32_t cap) override {
        if (port != ot::MidiPortId::kDin || in.empty()) return 0;
        uint32_t n = uint32_t(in.size() < cap ? in.size() : cap);
        std::memcpy(dst, in.data(), n);
        in.erase(in.begin(), in.begin() + n);
        return n;
    }
};

struct Station {
    std::vector<uint8_t> mem = std::vector<uint8_t>(64u * 1024u * 1024u);
    MemStorage fs;
    FakeMidi midi;
    lab::Shell shell;
    Tilt* ws = nullptr;
    lab::Transport tr;
    float peak = 0.0f;
    bool finite = true;
    explicit Station(bool card = true) {
        lab::Hal h;
        if (card) h.fs = &fs;
        h.midi = &midi;
        if (shell.init(h, mem.data(), uint32_t(mem.size()), "TILT")) ws = static_cast<Tilt*>(shell.workstation());
        tr.playing = true;
        tr.bpm = 150.0f;
        tr.beatsPerSample = 150.0 / 60.0 / 44100.0;
        tr.startedThisBlock = true;
    }
    void run(int blocks) {
        lab::AudioIn in;
        float l[lab::kBlock], r[lab::kBlock];
        for (int b = 0; b < blocks; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            ws->render(tr, in, l, r);
            tr.startedThisBlock = false;
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i])) finite = false;
                peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            }
            tr.beat += tr.beatsPerSample * lab::kBlock;
            if ((b & 7) == 0) shell.loop();
        }
        shell.loop();
    }
};

}  // namespace

// ---- the walk -----------------------------------------------------------------------------------
LAB_TEST(tilt_chain_walks_1_to_16_like_pinball) {
    Rig r;
    r.launch();
    for (int s = 1; s <= 32; ++s) {
        const int st = r.move();
        CHECK(st == ((s - 1) % 16) + 1);
        // the default groove: steps with a trig make one note
        CHECK(r.notes() == (r.p->steps[st - 1].trig >= 1 && r.p->steps[st - 1].trig <= 8 ? 1 : 0));
    }
    // start step 5: the first move from row 4 lands on 5
    r.p->table.startStep = 5;
    r.launch();
    CHECK(r.move() == 5);
}

LAB_TEST(tilt_walk_is_deterministic_with_a_seed) {
    auto walk = [](uint32_t seed, std::vector<int>& out) {
        Rig r;
        Rng32 g;
        g.seed(77);
        r.p->rnd.density = 80;
        applyShape(*r.p, 0, kShRandom, g);
        r.e->seed(seed);
        r.launch();
        for (int i = 0; i < 512; ++i) out.push_back(r.move());
    };
    std::vector<int> a, b, c;
    walk(42, a);
    walk(42, b);
    walk(43, c);
    CHECK(a == b);
    CHECK(a != c);
}

LAB_TEST(tilt_walk_follows_the_weights) {
    Rig r;
    clearAll(*r.p);
    r.p->w[0][0][1] = 100;
    r.p->w[0][1][2] = 75;
    r.p->w[0][1][3] = 25;
    r.p->w[0][2][1] = 100;
    r.p->w[0][3][1] = 100;
    r.launch();
    int to2 = 0, to3 = 0;
    for (int i = 0; i < 4000; ++i) {
        const int s = r.move();
        if (r.e->lastFrom == 1) (s == 2 ? to2 : to3)++;
    }
    const float f = float(to2) / float(to2 + to3);
    CHECK(f > 0.70f && f < 0.80f);
    // an empty row stops the ball (the device's behaviour)
    clearAll(*r.p);
    r.p->w[0][0][1] = 100;
    r.launch();
    r.move();
    r.move();
    CHECK(!r.e->balls[0].active);
}

LAB_TEST(tilt_drain_rest_and_hold) {
    Rig r;
    r.p->w[0][4][5] = 0;
    r.p->w[0][4][0] = 100;   // 4 drains
    r.launch();
    for (int i = 0; i < 4; ++i) r.move();
    CHECK(r.e->balls[0].state == 4);
    r.move();   // drain
    CHECK(r.hadEvent(kEvDrain));
    CHECK(r.notes() == 0);
    CHECK(r.e->balls[0].active && r.e->balls[0].resting);
    r.move();   // rest
    CHECK(r.notes() == 0 && r.e->balls[0].state == 0);
    CHECK(r.move() == 1);   // back in from S
    // HOLD: the ball waits; a flip brings it back
    r.p->table.drain = 1;
    r.launch();
    for (int i = 0; i < 5; ++i) r.move();
    CHECK(!r.e->balls[0].active);
    r.move();
    CHECK(r.e->nHits == 0);
    r.e->flip(9);
    CHECK(r.move() == 9);
}

LAB_TEST(tilt_ball_save_inside_the_window) {
    Rig r;
    r.p->w[0][4][5] = 0;
    r.p->w[0][4][0] = 100;
    r.p->table.saveBars = 2;   // 8 beats = 32 moves of 1/16
    r.launch();
    for (int i = 0; i < 4; ++i) r.move();
    const int s = r.move();   // drains at move 5, saved: back from S in the same move
    CHECK(r.hadEvent(kEvSave));
    CHECK(s == 1);
    CHECK(r.e->saves == 1);
    // after the window the drain is real
    for (int i = 0; i < 40; ++i) r.move();
    int drains = r.e->drains;
    bool saved = false;
    for (int i = 0; i < 20; ++i) {
        r.move();
        saved |= r.hadEvent(kEvSave);
    }
    CHECK(!saved);
    CHECK(r.e->drains > drains);
}

// ---- the player ---------------------------------------------------------------------------------
LAB_TEST(tilt_flippers_send_the_next_move) {
    Rig r;
    r.launch();
    r.move();
    r.move();
    r.e->flip(11, 90);
    CHECK(r.move() == 11);
    CHECK(r.move() == 12);   // then the matrix again
    r.p->flips[2].step = 6;
    r.e->flipper(2);
    CHECK(r.move() == 6);
}

LAB_TEST(tilt_too_many_flips_tilt_and_freeze) {
    Rig r;
    r.p->table.tiltFlips = 3;
    r.p->table.tiltAction = 0;   // FREEZE
    r.p->table.tiltBeats = 2;    // 8 moves
    r.launch();
    r.move();
    for (int i = 0; i < 3; ++i) r.e->flip(5);
    CHECK(!r.e->tilted());
    r.e->flip(7);   // the 4th flip in the bar
    CHECK(r.e->tilted());
    r.e->flip(9);   // dead flippers
    CHECK(r.e->balls[0].forced == 0);
    const int at = r.move();
    for (int i = 0; i < 6; ++i) CHECK(r.move() == at);   // frozen: the step repeats
    for (int i = 0; i < 3; ++i) r.move();
    CHECK(!r.e->tilted());
    // flips counted per bar
    for (int i = 0; i < 20; ++i) r.move();
    CHECK(r.e->flipsBar == 0);
}

LAB_TEST(tilt_a_hard_flip_tilts_and_shakes) {
    Rig r;
    r.p->table.tiltVel = 120;
    r.p->table.tiltAction = 1;   // SHAKE
    r.p->table.tiltBeats = 8;
    r.launch();
    r.move();
    r.e->flip(4, 100);
    CHECK(!r.e->tilted());
    r.move();
    r.e->flip(4, 127);
    CHECK(r.e->tilted());
    // shaking: the walk leaves the chain
    int off = 0, prev = r.e->balls[0].state;
    for (int i = 0; i < 24; ++i) {
        const int s = r.move();
        if (s != prev % 16 + 1) ++off;
        prev = s;
    }
    CHECK(off > 8);
    // a nudge is one random kick
    Rig q;
    q.launch();
    q.move();
    q.e->nudge(60);
    CHECK(q.e->shakeLeft == 1);
}

// ---- the table -------------------------------------------------------------------------------------
LAB_TEST(tilt_bumpers_ratchet_at_0_8_velocity) {
    Rig r;
    r.p->steps[0].trig = 2;
    r.p->steps[0].vel = 100;
    r.p->steps[0].bumper = 3;
    r.launch();
    r.move();
    CHECK(r.notes() == 4);
    const float mb = float(r.e->moveBeats());
    float v = 100.0f;
    for (int k = 0; k < 4; ++k) {
        CHECK_NEAR(r.e->hits[k].at, mb * 0.25f * float(k), 1e-5);
        CHECK(std::abs(int(r.e->hits[k].vel) - int(v + 0.5f)) <= 1);
        v *= 0.8f;
    }
}

LAB_TEST(tilt_ramps_force_the_next_move) {
    Rig r;
    r.p->steps[3].ramp = 9;
    r.launch();
    for (int i = 0; i < 3; ++i) r.move();
    CHECK(r.move() == 4);
    CHECK(r.move() == 9);
    CHECK(r.move() == 10);
}

LAB_TEST(tilt_combo_path_fires_multiball) {
    Rig r;
    r.p->table.mbMax = 4;
    Combo& c = r.p->combos[0];
    c.len = 3;
    c.path[0] = 2;
    c.path[1] = 3;
    c.path[2] = 4;
    c.action = kCaMultiball;
    c.a = 2;
    r.launch();
    r.move();
    r.move();
    CHECK(r.e->comboProgress[0] == 1);
    r.move();
    CHECK(r.e->comboProgress[0] == 2);
    r.move();   // lands on 4: combo
    CHECK(r.hadEvent(kEvCombo1));
    CHECK(r.hadEvent(kEvMultiball));
    CHECK(r.e->ballsActive() == 3);
    // extra balls play too, at 0.85 velocity
    r.move();
    int extra = 0;
    for (int i = 0; i < r.e->nHits; ++i) extra += r.e->hits[i].ball != 0 && r.e->hits[i].kind == Hit::kNote;
    CHECK(extra >= 1);
}

LAB_TEST(tilt_combo_actions) {
    Rig r;
    r.p->steps[3].trig = 1;   // step 4: KICK
    r.p->steps[3].bumper = 0;
    Combo& c = r.p->combos[1];
    c.len = 2;
    // FILL: every hit of the next moves ratchets
    c.action = kCaFill;
    c.a = 4;
    c.b = 3;
    r.e->fireCombo(1);
    CHECK(r.e->fillLeft == 4 && r.e->fillHits == 3);
    r.launch();
    r.e->fillLeft = 4;
    for (int i = 0; i < 4; ++i) r.move();
    CHECK(r.notes() == 3);   // step 4 has a trig: three hits
    // SHAKE
    c.action = kCaShake;
    c.a = 5;
    r.e->fireCombo(1);
    CHECK(r.e->shakeLeft == 5);
    // SCENE: a page, or the next page
    c.action = kCaScene;
    c.a = 2;
    r.e->fireCombo(1);
    CHECK(r.p->ev.page == 2);
    c.a = kPages;
    r.e->fireCombo(1);
    CHECK(r.p->ev.page == 3);
    // NOTE and CC
    r.e->nHits = 0;
    c.action = kCaNote;
    c.a = 72;
    c.b = 7;
    r.e->fireCombo(1);
    CHECK(r.e->nHits == 1 && r.e->hits[0].note == 72 && r.e->hits[0].ch == 6);
    r.e->nHits = 0;
    c.action = kCaCc;
    c.a = 20;
    c.b = 99;
    r.e->fireCombo(1);
    CHECK(r.e->nHits == 1 && r.e->hits[0].kind == Hit::kCc && r.e->hits[0].note == 20 && r.e->hits[0].vel == 99);
}

LAB_TEST(tilt_multiball_max_and_life) {
    Rig r;
    r.p->table.mbMax = 3;
    r.p->table.mbLife = 6;
    r.launch();
    r.move();
    for (int i = 0; i < 5; ++i) r.e->addBall();
    CHECK(r.e->ballsActive() == 3);
    for (int i = 0; i < 6; ++i) r.move();
    CHECK(r.e->ballsActive() == 1);
}

LAB_TEST(tilt_gravity_pulls_home_on_the_bar) {
    Rig r;
    Rng32 g;
    g.seed(5);
    r.p->rnd.density = 90;
    applyShape(*r.p, 0, kShRandom, g);
    r.p->table.gravity = 100;
    r.p->table.gravityGrid = 0;
    r.p->table.startStep = 3;
    r.launch();
    int bars = 0, home = 0;
    for (int i = 0; i < 16 * 12; ++i) {
        const bool downbeat = (i % 16) == 0 && i > 0;
        const int s = r.move();
        if (downbeat && s >= 1) {
            ++bars;
            home += s == 3;
        }
    }
    CHECK(bars >= 10);
    CHECK(home == bars);
}

LAB_TEST(tilt_edge_properties_change_the_note) {
    Rig r;
    r.p->steps[1].trig = 6;   // ACID on step 2
    r.p->steps[1].vel = 90;
    Edge& ed = r.p->e[0][1][2];
    ed.tr = 5;
    ed.dly = 6;
    ed.vel = 20;
    r.launch();
    r.move();
    r.move();   // 1 -> 2 takes the edge
    CHECK(r.notes() == 1);
    const Hit& h = r.e->hits[r.e->nHits - 1];
    CHECK(h.note == r.p->ch[5].note + 5);
    CHECK(h.vel == 110);
    CHECK_NEAR(h.at, r.e->moveBeats() * 0.5, 1e-5);
}

// ---- evolution ------------------------------------------------------------------------------------------
LAB_TEST(tilt_memory_fatigue_reinforce_recover) {
    Rig r;
    clearAll(*r.p);
    r.p->w[0][0][1] = 100;
    r.p->w[0][1][2] = 50;
    r.p->w[0][1][3] = 50;
    r.p->w[0][2][1] = 100;
    r.p->w[0][3][1] = 100;
    r.p->ev.fatigue = 100;
    r.p->ev.recover = 0;
    r.launch();
    for (int i = 0; i < 200; ++i) r.move();
    CHECK(r.e->mem[1][2] < 0.9f && r.e->mem[1][3] < 0.9f);
    CHECK(r.e->mem[2][1] < 0.3f);
    // reinforcement makes used edges stronger
    Rig q;
    q.p->ev.reinforce = 60;
    q.p->ev.recover = 0;
    q.launch();
    for (int i = 0; i < 40; ++i) q.move();
    CHECK(q.e->mem[1][2] > 1.5f);
    // recover pulls back to neutral
    q.p->ev.reinforce = 0;
    q.p->ev.recover = 100;
    const float before = q.e->mem[1][2];
    for (int i = 0; i < 200; ++i) q.move();
    CHECK(std::fabs(q.e->mem[5][6] - 1.0f) < std::fabs(before - 1.0f));
}

LAB_TEST(tilt_mutation_drifts_and_comes_home) {
    Rig r;
    r.p->ev.mutate = 60;
    r.p->ev.mutHome = 5;
    r.launch();
    for (int i = 0; i < 16 * 8; ++i) r.move();
    float drift = 0.0f;
    for (int a = 0; a < kStates; ++a)
        for (int b = 0; b < kStates; ++b) drift += std::fabs(r.e->mut[0][a][b]);
    CHECK(drift > 50.0f);
    // weights stay inside 0..100
    for (int a = 0; a < kStates; ++a)
        for (int b = 0; b < kStates; ++b) {
            const float w = float(r.p->w[0][a][b]) + r.e->mut[0][a][b];
            CHECK(w >= -0.01f && w <= 100.01f);
        }
    r.p->ev.mutate = 0;
    r.p->ev.mutHome = 100;
    for (int i = 0; i < 16 * 2; ++i) r.move();
    float left = 0.0f;
    for (int a = 0; a < kStates; ++a)
        for (int b = 0; b < kStates; ++b) left += std::fabs(r.e->mut[0][a][b]);
    CHECK(left < 0.01f);
}

LAB_TEST(tilt_meta_matrix_switches_pages) {
    Rig r;
    r.p->ev.metaOn = 1;
    r.p->ev.metaBars = 1;
    std::memset(r.p->ev.meta, 0, sizeof r.p->ev.meta);
    r.p->ev.meta[0][1] = 100;
    r.p->ev.meta[1][2] = 100;
    r.p->ev.meta[2][0] = 100;
    r.launch();
    std::vector<int> pages;
    for (int i = 0; i < 16 * 4; ++i) {
        r.move();
        if (i % 16 == 0) pages.push_back(r.p->ev.page);
    }
    CHECK(pages.size() == 4);
    CHECK(pages[0] == 0 && pages[1] == 1 && pages[2] == 2 && pages[3] == 0);
}

LAB_TEST(tilt_melody_plays_scale_degrees) {
    Rig r;
    Channel& c = r.p->ch[5];   // ACID, follows
    c.follow = 1;
    c.note = 36;
    r.p->mel.on = 1;
    r.p->mel.scale = 0;   // Major
    r.p->mel.root = 2;    // D
    static const int8_t deg[4] = {0, 2, 7, -1};
    for (int s = 0; s < 4; ++s) {
        r.p->steps[s].trig = 6;
        r.p->steps[s].degree = deg[s];
    }
    r.launch();
    const int expect[4] = {36 + 2 + 0, 36 + 2 + 4, 36 + 2 + 12, 36 + 2 - 1};
    for (int s = 0; s < 4; ++s) {
        r.move();
        CHECK(r.notes() == 1 && r.e->hits[0].note == expect[s]);
    }
    // octave up 100 %
    r.p->mel.octUp = 100;
    r.launch();
    r.move();
    CHECK(r.e->hits[0].note == 36 + 2 + 12);
    // a channel that does not follow keeps its note
    r.p->steps[0].trig = 1;
    r.p->ch[0].follow = 0;
    r.launch();
    r.move();
    CHECK(r.e->hits[0].note == r.p->ch[0].note);
    CHECK(scaleSemis(7, 5) == 12 && scaleSemis(1, -1) == -2);
}

LAB_TEST(tilt_rules_no_bounce_max_repeat_anchor) {
    Rig r;
    clearAll(*r.p);
    r.p->w[0][0][5] = 100;
    for (int s = 1; s <= 16; ++s) {
        r.p->w[0][s][s % 16 + 1] = 50;
        r.p->w[0][s][(s + 14) % 16 + 1] = 50;
    }
    r.p->ev.noBounce = 1;
    r.launch();
    int prev = -1, cur = r.move();
    bool bounced = false;
    for (int i = 0; i < 300; ++i) {
        const int s = r.move();
        if (s == prev) bounced = true;
        prev = cur;
        cur = s;
    }
    CHECK(!bounced);
    // max repeats
    Rig q;
    for (int s = 1; s <= 16; ++s) {
        q.p->w[0][s][s] = 95;
        q.p->w[0][s][s % 16 + 1] = 5;
    }
    q.p->ev.maxRepeat = 2;
    q.launch();
    int run = 0, worst = 0;
    prev = -1;
    for (int i = 0; i < 400; ++i) {
        const int s = q.move();
        run = s == prev ? run + 1 : 0;
        if (run > worst) worst = run;
        prev = s;
    }
    CHECK(worst == 2);
    // anchor: every 4th move lands on 1
    Rig a;
    Rng32 g;
    g.seed(3);
    applyShape(*a.p, 0, kShJumps, g);
    a.p->ev.anchorStep = 1;
    a.p->ev.anchorEvery = 4;
    a.launch();
    bool ok = true;
    for (int i = 1; i <= 64; ++i) {
        const int s = a.move();
        if (i % 4 == 0 && s != 1) ok = false;
    }
    CHECK(ok);
}

LAB_TEST(tilt_swing_and_step_delay) {
    Rig r;
    r.p->table.swing = 50;
    for (auto& s : r.p->steps) s.trig = 1;
    r.launch();
    const float mb = float(r.e->moveBeats());
    r.move();
    CHECK_NEAR(r.e->hits[0].at, 0.0, 1e-6);
    r.move();
    CHECK_NEAR(r.e->hits[0].at, mb * 0.25f, 1e-5);
    r.p->table.swing = 0;
    r.p->steps[2].delay = 6;
    r.p->steps[2].delayP = 100;
    r.move();
    CHECK_NEAR(r.e->hits[0].at, mb * 0.5f, 1e-5);
    r.p->steps[3].delay = 6;
    r.p->steps[3].delayP = 0;
    r.move();
    CHECK_NEAR(r.e->hits[0].at, 0.0, 1e-6);
    // trig probability 0: silent
    r.p->steps[4].trigP = 0;
    r.move();
    CHECK(r.notes() == 0);
}

LAB_TEST(tilt_choke_groups_and_mute_steps) {
    Rig r;
    r.p->ch[2].choke = 1;
    r.p->ch[3].choke = 1;
    r.p->ch[0].choke = 0;
    r.p->steps[0].trig = 3;
    r.p->steps[1].trig = kTrigMute;
    r.launch();
    r.move();
    bool choke = false;
    for (int i = 0; i < r.e->nHits; ++i)
        if (r.e->hits[i].kind == Hit::kChoke) choke = r.e->hits[i].chokeMask == (1u << 3);
    CHECK(choke);
    r.move();
    CHECK(r.e->nHits == 1 && r.e->hits[0].kind == Hit::kChoke);
    uint8_t want = 0;
    for (int i = 0; i < kCh; ++i)
        if (r.p->ch[i].choke) want |= uint8_t(1u << i);
    CHECK(r.e->hits[0].chokeMask == want);
}

LAB_TEST(tilt_capture_and_replay) {
    Rig r;
    r.launch();
    std::vector<std::pair<int, int>> heard;   // (t96, note) of the second bar
    for (int i = 0; i < 16 * 6; ++i) {
        const double b = r.beat;
        r.move();
        for (int k = 0; k < r.e->nHits; ++k)
            if (r.e->hits[k].kind == Hit::kNote && b >= 16.0 && b < 20.0)
                heard.push_back({int((b + r.e->hits[k].at) * 96.0 + 0.5), r.e->hits[k].note});
    }
    // at beat 24: the last 2 bars are beats 16..24
    const int n = r.e->capture(2, 24.0);
    CHECK(n > 0);
    CHECK(r.e->clipBars() == 2);
    CHECK(r.e->clipStart96() == 16u * 96u);
    for (int i = 0; i < n; ++i) CHECK(r.e->clip()[i].t96 < 2u * 4u * 96u);
    // replay plays the clip in place of the ball
    r.e->setReplay(true);
    r.beat = 32.0;   // a multiple of the clip: bar 1 of the clip again
    std::vector<std::pair<int, int>> played;
    for (int i = 0; i < 16; ++i) {
        const double b = r.beat;
        r.move();
        for (int k = 0; k < r.e->nHits; ++k)
            played.push_back({int((b - 32.0 + 16.0 + r.e->hits[k].at) * 96.0 + 0.5), r.e->hits[k].note});
    }
    CHECK(played == heard);
}

// ---- the model -----------------------------------------------------------------------------------------------
LAB_TEST(tilt_shapes_fill_every_row) {
    Pattern p;
    defaultPattern(p);
    Rng32 g;
    for (int sh = 0; sh < kShapeCount; ++sh) {
        applyShape(p, 1, sh, g);
        for (int r = 0; r < kStates; ++r) {
            int sum = 0;
            for (int c = 0; c < kStates; ++c) sum += p.w[1][r][c];
            if (sh == kShClear) CHECK(sum == 0);
            else CHECK(sum > 0);
        }
    }
    // PINGPONG visits all 16 steps in one cycle
    Rig r;
    applyShape(*r.p, 0, kShPingPong, g);
    r.launch();
    bool seen[17] = {};
    for (int i = 0; i < 16; ++i) seen[r.move()] = true;
    int all = 0;
    for (int s = 1; s <= 16; ++s) all += seen[s];
    CHECK(all == 16);
}

LAB_TEST(tilt_randomizer_respects_locks) {
    Pattern p;
    defaultPattern(p);
    p.rnd.amount = 100;
    p.steps[2].lock = 1;
    p.rnd.rowLocks = uint16_t(1u << kRowVel);
    Pattern before = p;
    Rng32 g;
    g.seed(9);
    randomize(p, g, kRtMatrix | kRtSteps | kRtLanes | kRtRules | kRtSound);
    CHECK(std::memcmp(&p.steps[2], &before.steps[2], sizeof(Step)) == 0);
    CHECK(std::memcmp(p.w[0][3], before.w[0][3], kStates) == 0);   // the locked step's row
    for (int s = 0; s < kSteps; ++s) CHECK(p.steps[s].vel == before.steps[s].vel);
    int changed = 0;
    for (int s = 0; s < kSteps; ++s)
        if (s != 2) changed += std::memcmp(&p.steps[s], &before.steps[s], sizeof(Step)) != 0;
    CHECK(changed >= 10);
    CHECK(std::memcmp(p.w[0][5], before.w[0][5], kStates) != 0 || std::memcmp(p.w[0][6], before.w[0][6], kStates) != 0);
    for (int r = 0; r < kStates; ++r) {
        int sum = 0;
        for (int c = 0; c < kStates; ++c) sum += p.w[0][r][c];
        CHECK(sum > 0);
    }
    // amount 0 changes nothing
    Pattern q = before;
    q.rnd.amount = 0;
    randomize(q, g, kRtMatrix | kRtSteps | kRtLanes | kRtRules | kRtSound);
    CHECK(std::memcmp(&q, &before, sizeof q) == 0 || std::memcmp(q.steps, before.steps, sizeof q.steps) == 0);
    // Pinball's Randomize Trigs: 0..9, locks kept
    randomizeTrigs(q, g);
    for (int s = 0; s < kSteps; ++s) CHECK(q.steps[s].trig <= 9);
    CHECK(q.steps[2].trig == before.steps[2].trig);
}

LAB_TEST(tilt_pattern_bytes_roundtrip_and_versioning) {
    std::unique_ptr<Pattern> a(new Pattern()), b(new Pattern());
    factoryPattern(6, *a);
    a->e[2][3][4].tr = -7;
    std::strcpy(a->ch[7].sample, "Samples/amen.wav");
    std::vector<uint8_t> buf(sizeof(Pattern) + 16);
    const uint32_t n = patternToBytes(*a, buf.data(), uint32_t(buf.size()));
    CHECK(n == sizeof(Pattern));
    CHECK(patternFromBytes(*b, buf.data(), n));
    CHECK(std::memcmp(a.get(), b.get(), sizeof(Pattern)) == 0);
    // bad magic, too short
    std::vector<uint8_t> bad = buf;
    bad[0] ^= 0xFF;
    CHECK(!patternFromBytes(*b, bad.data(), n));
    CHECK(!patternFromBytes(*b, buf.data(), 4));
    // an older, shorter file: what it has loads, the rest defaults
    std::vector<uint8_t> old(buf.begin(), buf.begin() + offsetof(Pattern, ch));
    const uint16_t oldSize = uint16_t(old.size());
    std::memcpy(old.data() + 6, &oldSize, 2);
    CHECK(patternFromBytes(*b, old.data(), uint32_t(old.size())));
    CHECK(b->e[2][3][4].tr == -7);
    Pattern d;
    defaultPattern(d);
    CHECK(std::memcmp(b->ch, d.ch, sizeof d.ch) == 0);
    // garbage values are clamped
    std::memset(buf.data() + 8, 0xEE, n - 8);
    CHECK(patternFromBytes(*b, buf.data(), n));
    for (const auto& s : b->steps) CHECK(s.trig <= 9 && s.ramp <= 16 && s.bumper <= 4);
    for (const auto& c : b->ch) CHECK(c.type < kEngines);
    CHECK(b->table.rate < kRateCount);
}

LAB_TEST(tilt_parameter_table_is_sane) {
    Pattern p;
    defaultPattern(p);
    for (int i = 0; i < kParamCount; ++i) {
        const ParamDef& d = kParams[i];
        CHECK(d.min <= d.def && d.def <= d.max);
        CHECK(uint32_t(d.offset) + uint32_t(d.stride) * uint32_t(d.count > 0 ? d.count - 1 : 0) < sizeof(Pattern));
        char buf[32];
        for (int k = 0; k < d.count; ++k) {
            paramSet(p, i, d.max + 5, k);
            CHECK(paramGet(p, i, k) == d.max);
            paramText(p, i, k, buf, sizeof buf);
            CHECK(buf[0] != 0);
        }
    }
    for (int i = 0; i < kFactoryCount; ++i) {
        factoryPattern(i, p);
        CHECK(std::strcmp(p.name, kFactoryNames[i]) == 0);
    }
}

// ---- the workstation -------------------------------------------------------------------------------------------
LAB_TEST(tilt_workstation_plays_sounds_and_midi) {
    Station st;
    CHECK(st.ws != nullptr);
    if (!st.ws) return;
    st.ws->loadFactory(0);
    st.run(1600);   // ~4.6 s
    CHECK(st.finite);
    CHECK(st.peak > 0.05f && st.peak < 4.0f);
    CHECK(st.midi.noteOns > 20);
    CHECK(st.midi.noteOffs > 10);
    std::fprintf(stderr, "  peak %.3f, %d note-ons\n", double(st.peak), st.midi.noteOns);
    // every engine makes sound on its own
    for (int t = 0; t < kEngines; ++t) {
        Pattern& p = st.ws->pattern();
        engineDefaults(p.ch[0], t);
        p.ch[0].sample[0] = 0;
        p.ch[0].mute = 0;
        for (int c = 1; c < kCh; ++c) p.ch[c].mute = 1;
        for (auto& s : p.steps) s.trig = 1;
        st.peak = 0.0f;
        st.run(300);
        CHECK(st.peak > 0.01f);
        std::fprintf(stderr, "  %s peak %.3f\n", kEngineNames[t], double(st.peak));
    }
}

LAB_TEST(tilt_every_factory_pattern_plays) {
    Station st;
    if (!st.ws) return;
    for (int i = 0; i < kFactoryCount; ++i) {
        st.ws->loadFactory(i);
        st.tr.startedThisBlock = true;
        st.peak = 0.0f;
        const int before = st.midi.noteOns;
        st.run(900);
        CHECK(st.finite);
        CHECK(st.peak > 0.02f);
        CHECK(st.midi.noteOns > before);
        CHECK(std::strcmp(st.ws->pattern().name, kFactoryNames[i]) == 0);
    }
}

LAB_TEST(tilt_midi_in_flips_and_flippers) {
    Station st;
    if (!st.ws) return;
    st.ws->loadFactory(0);
    st.run(200);
    // step note: base + 6 = step 7
    st.ws->onMidi(lab::MidiMsg{0x90, uint8_t(st.ws->pattern().table.stepNoteBase + 6), 100});
    st.run(1);
    Engine& e = st.ws->engine();
    CHECK(e.balls[0].forced == 7 || e.lastTo == 7);
    st.run(200);
    // flipper note 38 = flipper 3
    st.ws->pattern().flips[2].step = 13;
    st.ws->onMidi(lab::MidiMsg{0x90, 38, 100});
    st.run(1);
    CHECK(e.balls[0].forced == 13 || e.lastTo == 13);
}

LAB_TEST(tilt_files_patterns_kits_midi) {
    Station st;
    if (!st.ws) return;
    Tilt& ws = *st.ws;
    ws.loadFactory(10);
    st.run(4);
    ws.pattern().steps[5].ramp = 12;
    ws.pattern().ch[2].p[1] = 99;
    CHECK(ws.savePattern(3));
    CHECK(st.fs.exists("Lab/TILT/Patterns/PAT04.TLT"));
    CHECK(ws.saveKit(1));
    ws.loadFactory(0);
    st.run(4);
    CHECK(ws.pattern().steps[5].ramp == 0);
    CHECK(ws.loadPattern(3));
    st.run(4);
    CHECK(ws.pattern().steps[5].ramp == 12);
    CHECK(std::strcmp(ws.pattern().name, "META") == 0);
    ws.pattern().ch[2].p[1] = 3;
    CHECK(ws.loadKit(1));
    CHECK(ws.pattern().ch[2].p[1] == 99);
    CHECK(!ws.loadPattern(9));
    // capture + replay + a MIDI file
    st.run(2400);
    ws.post(Cmd::kCapture, 2, 1);
    st.run(2);
    CHECK(ws.engine().clipCount() > 0);
    CHECK(ws.engine().replaying());
    const int before = st.midi.noteOns;
    st.run(600);
    CHECK(st.midi.noteOns > before);
    char path[64];
    CHECK(ws.exportMidi(path, sizeof path));
    CHECK(st.fs.exists(path));
    const auto& f = st.fs.files[path];
    CHECK(f.size() > 30 && std::memcmp(f.data(), "MThd", 4) == 0 && std::memcmp(f.data() + 14, "MTrk", 4) == 0);
    // autosave + the next boot loads the last session
    ws.pattern().steps[1].bumper = 3;
    ws.save();
    CHECK(st.fs.exists("Lab/TILT/state.tlt"));
    // randomize + undo
    Pattern keep = ws.pattern();
    ws.randomizeNow(kRtMatrix | kRtSteps);
    CHECK(std::memcmp(&keep, &ws.pattern(), sizeof keep) != 0);
    ws.undoRandom();
    CHECK(std::memcmp(&keep, &ws.pattern(), sizeof keep) == 0);
}

LAB_TEST(tilt_runs_without_a_card) {
    Station st(false);
    CHECK(st.ws != nullptr);
    if (!st.ws) return;
    st.run(800);
    CHECK(st.peak > 0.05f);
    CHECK(!st.ws->savePattern(0));
    CHECK(!st.ws->loadPattern(0));
    char p[32];
    CHECK(!st.ws->exportMidi(p, sizeof p));
    st.ws->save();
}

LAB_TEST(tilt_modulation_sources_and_pages_table) {
    Station st;
    if (!st.ws) return;
    st.ws->loadFactory(6);
    st.run(1500);
    for (int i = 0; i < Tilt::kModSources; ++i) {
        const float* v = st.ws->modSourceValue(i);
        CHECK(v != nullptr);
        if (v) CHECK(*v >= 0.0f && *v <= 1.0f);
        CHECK(Tilt::modSourceName(i)[0] != 0);
    }
    CHECK(st.ws->modSourceValue(Tilt::kModSources) == nullptr);
    CHECK(*st.ws->modSourceValue(0) > 0.0f);   // the ball is somewhere
    for (int p = 0; p < kPgCount; ++p) {
        CHECK(kPages_[p].name && kPages_[p].name[0]);
        CHECK(kPages_[p].help && kPages_[p].help[0]);
    }
}
