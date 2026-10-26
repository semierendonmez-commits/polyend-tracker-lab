// FLIPPER host tests: physics stability, Markov determinism, transitions,
// auto-player, capture / replay, save / load roundtrip, the workstation in
// every bench scenario.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "lab/lab.h"
#include "lab_test.h"
#include "mem_storage.h"
#include "ws/flip_game.h"
#include "ws/flip_model.h"
#include "ws/flip_ws.h"

using namespace flip;

namespace {

std::unique_ptr<Machine> factory() {
    std::unique_ptr<Machine> m(new Machine());
    factoryMachine(*m);
    return m;
}

struct Stats {
    int notes = 0, fx = 0, drains = 0, multiballs = 0, switches = 0, games = 0;
    bool finite = true, inside = true;
};

// Runs `steps` musical steps with `blocksPerStep` physics ticks in between.
Stats run(Game& g, int steps, int blocksPerStep = 22) {
    Stats s;
    for (int i = 0; i < steps; ++i) {
        g.clearOut();
        g.step();
        s.notes += g.noteCount;
        s.fx += g.fxCount;
        if (g.switched) ++s.switches;
        for (int b = 0; b < g.bannerCount; ++b) {
            if (g.banners[b] == kBnDrain) ++s.drains;
            if (g.banners[b] == kBnMultiball) ++s.multiballs;
            if (g.banners[b] == kBnGameOver) ++s.games;
        }
        for (int k = 0; k < blocksPerStep; ++k) {
            g.clearOut();
            g.tick(128.0f / 44100.0f);
            s.notes += g.noteCount;
            for (int b = 0; b < g.bannerCount; ++b) {
                if (g.banners[b] == kBnDrain) ++s.drains;
                if (g.banners[b] == kBnMultiball) ++s.multiballs;
                if (g.banners[b] == kBnGameOver) ++s.games;
            }
            for (const Ball& b : g.balls) {
                if (!b.active) continue;
                if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.vx) || !std::isfinite(b.vy))
                    s.finite = false;
                if (b.phys && !b.captured &&
                    (b.x < -1.0f || b.x > kFieldW + 1.0f || b.y < -1.0f || b.y > kFieldH + 3.0f * kBallR))
                    s.inside = false;
                if (b.cell >= kCells) s.inside = false;
            }
        }
    }
    return s;
}

}  // namespace

LAB_TEST(flipper_scale_and_names) {
    CHECK(scaleNote(60, 0, 0, 1) == 60);
    CHECK(scaleNote(61, 0, 0, 1) == 60);   // C# snaps down to C in C major
    CHECK(scaleNote(60, 1, 0, 1) == 62);
    CHECK(scaleNote(60, 7, 0, 1) == 72);
    CHECK(scaleNote(60, -1, 0, 1) == 59);
    CHECK(scaleNote(57, 2, 9, 2) == 60);   // A minor: A B C
    char n[8];
    noteName(60, n);
    CHECK(std::strcmp(n, "C4") == 0);
    CHECK(stepsPerBar(kRate16) == 16);
    CHECK(stepsPerBar(kRate8T) == 12);
}

LAB_TEST(flipper_move_weights_respect_walls_and_gravity) {
    auto m = factory();
    Table& t = m->tab[0];
    clearTable(t);
    const int c = cellAt(5, 1);
    t.cells[c].w[0] = t.cells[c].w[1] = t.cells[c].w[2] = t.cells[c].w[3] = 5;
    t.cells[cellAt(6, 1)].type = kWall;
    t.gravity = 0;
    float w[5];
    moveWeights(t, c, -1, 0.0f, false, w);
    CHECK(w[1] == 0.0f);                    // wall on the right
    CHECK(w[0] > 0.0f && w[2] > 0.0f && w[3] > 0.0f);
    CHECK_NEAR(w[0], w[2], 1e-6);          // no gravity: up == down
    t.gravity = 9;
    moveWeights(t, c, -1, 0.0f, false, w);
    CHECK(w[2] > w[0] * 2.0f);             // gravity pulls down
    // top row: no move up
    moveWeights(t, cellAt(3, 0), -1, 0.0f, false, w);
    CHECK(w[0] == 0.0f);
    // bottom row: drain only when allowed
    moveWeights(t, cellAt(3, 3), -1, 0.0f, false, w);
    CHECK(w[2] == 0.0f);
    moveWeights(t, cellAt(3, 3), -1, 0.0f, true, w);
    CHECK(w[2] > 0.0f);
    // magnet pulls toward the attractor
    float wm[5];
    t.gravity = 0;
    moveWeights(t, c, cellAt(0, 1), 1.0f, false, wm);
    CHECK(wm[3] > wm[0] * 3.0f);
    CHECK(entropyBits(w, 5) > 0.5f);
    const float one[2] = {1.0f, 0.0f};
    CHECK_NEAR(entropyBits(one, 2), 0.0, 1e-6);
}

LAB_TEST(flipper_markov_is_deterministic_with_a_seed) {
    auto m1 = factory();
    auto m2 = factory();
    m1->g.mode = m2->g.mode = kMarkov;
    Game a, b;
    a.init(m1.get(), 1234);
    b.init(m2.get(), 1234);
    a.setMode(kMarkov);
    b.setMode(kMarkov);
    a.newGame(1);
    b.newGame(1);
    a.launch();
    b.launch();
    bool same = true;
    int notes = 0;
    for (int i = 0; i < 2000; ++i) {
        a.clearOut();
        b.clearOut();
        a.step();
        b.step();
        a.tick(0.003f);
        b.tick(0.003f);
        if (a.noteCount != b.noteCount || a.tableIndex() != b.tableIndex() || a.score() != b.score()) same = false;
        for (int k = 0; k < a.noteCount && k < b.noteCount; ++k)
            if (a.notes[k].note != b.notes[k].note || a.notes[k].inst != b.notes[k].inst) same = false;
        for (int k = 0; k < kMaxBalls; ++k)
            if (a.balls[k].active != b.balls[k].active || (a.balls[k].active && a.balls[k].cell != b.balls[k].cell))
                same = false;
        notes += a.noteCount;
    }
    CHECK(same);
    CHECK(notes > 200);   // it makes music
    // another seed gives another run
    auto m3 = factory();
    Game c;
    c.init(m3.get(), 99);
    c.setMode(kMarkov);
    c.newGame(1);
    c.launch();
    int diff = 0;
    Game d;
    auto m4 = factory();
    d.init(m4.get(), 1234);
    d.setMode(kMarkov);
    d.newGame(1);
    d.launch();
    for (int i = 0; i < 300; ++i) {
        c.clearOut();
        d.clearOut();
        c.step();
        d.step();
        for (int k = 0; k < kMaxBalls; ++k)
            if (c.balls[k].active != d.balls[k].active || c.balls[k].cell != d.balls[k].cell) ++diff;
    }
    CHECK(diff > 0);
}

LAB_TEST(flipper_markov_ball_moves_one_cell_per_step) {
    auto m = factory();
    Game g;
    g.init(m.get(), 77);
    g.setMode(kMarkov);
    g.newGame(0);
    g.launch();
    int bad = 0;
    uint8_t prev[kMaxBalls];
    bool act[kMaxBalls];
    for (int k = 0; k < kMaxBalls; ++k) {
        prev[k] = g.balls[k].cell;
        act[k] = g.balls[k].active;
    }
    for (int i = 0; i < 3000; ++i) {
        g.clearOut();
        g.step();
        for (int k = 0; k < kMaxBalls; ++k) {
            const Ball& b = g.balls[k];
            if (b.active && act[k] && b.cell != prev[k]) {
                const int dc = std::abs(cellCol(b.cell) - cellCol(prev[k])), dr = std::abs(cellRow(b.cell) - cellRow(prev[k]));
                const bool wrap = g.table().sides && dc == kCols - 1;
                if (!((dc <= 1 || wrap) && dr <= 1)) ++bad;   // tilt may teleport: counted below
            }
            if (b.active) CHECK(g.table().cells[b.cell].type != kWall);
            prev[k] = b.cell;
            act[k] = b.active;
        }
    }
    CHECK(bad == 0);
}

LAB_TEST(flipper_physics_is_stable_and_contained) {
    for (int table = 0; table < kTables; ++table) {
        auto m = factory();
        Game g;
        g.init(m.get(), 4242u + uint32_t(table));
        g.setMode(kHybrid);
        g.newGame(table);
        g.launch();
        g.dropBall(cellAt(3, 0));
        g.dropBall(cellAt(8, 1));
        const Stats s = run(g, 1500);
        CHECK(s.finite);
        CHECK(s.inside);
        if (!s.finite || !s.inside) std::fprintf(stderr, "  table %d unstable\n", table);
    }
    // a pathological table: walls everywhere but one cell, max gravity, wrap
    auto m = factory();
    Table& t = m->tab[0];
    for (auto& c : t.cells) {
        c = Cell();
        c.type = kWall;
    }
    t.cells[cellAt(5, 1)].type = kBumper;
    t.cells[cellAt(5, 1)].inst = 1;
    t.cells[cellAt(6, 1)].type = kEmpty;
    t.gravity = 9;
    t.sides = 1;
    Game g;
    g.init(m.get(), 7);
    g.setMode(kHybrid);
    g.newGame(0);
    g.launch();
    g.dropBall(cellAt(6, 1));
    const Stats s = run(g, 800);
    CHECK(s.finite);
    CHECK(s.inside);
}

LAB_TEST(flipper_hybrid_makes_music) {
    auto m = factory();
    Game g;
    g.init(m.get(), 31337);
    g.setMode(kHybrid);
    g.newGame(1);
    g.launch();
    const Stats s = run(g, 512);   // 32 bars of 1/16
    std::fprintf(stderr, "  hybrid: %d notes, %d fx, %d drains in 512 steps\n", s.notes, s.fx, s.drains);
    CHECK(s.notes > 150);
}

LAB_TEST(flipper_auto_player_skill_matters) {
    // the auto-player keeps the ball up: a high skill drains less than skill 0
    int drains[2] = {};
    for (int k = 0; k < 2; ++k) {
        for (int seed = 0; seed < 4; ++seed) {
            auto m = factory();
            m->g.autoOn = 1;
            m->g.autoAfter = 1;
            m->g.autoSkill = k ? 100 : 0;
            for (auto& t : m->tab) t.balls = 10;   // endless: count drains, no game over
            Game g;
            g.init(m.get(), 1000u + uint32_t(seed));
            g.setMode(kHybrid);
            g.newGame(1);
            const Stats s = run(g, 800);
            drains[k] += s.drains;
        }
    }
    std::fprintf(stderr, "  drains: skill 0 -> %d, skill 100 -> %d\n", drains[0], drains[1]);
    CHECK(drains[1] < drains[0]);
    // with the auto-player off and no player, nothing launches
    auto m = factory();
    m->g.autoOn = 0;
    Game g;
    g.init(m.get(), 5);
    g.setMode(kHybrid);
    g.newGame(1);
    run(g, 64);
    CHECK(g.ballsInPlay() == 0);
    g.launch();
    run(g, 2);
    CHECK(g.ballsInPlay() == 1);
}

LAB_TEST(flipper_player_flip_launches_the_ball_up) {
    auto m = factory();
    m->g.autoOn = 0;
    Table& t = m->tab[0];
    Game g;
    g.init(m.get(), 3);
    g.setMode(kMarkov);
    g.newGame(0);
    int flipCell = -1;
    for (int c = 0; c < kCells; ++c)
        if (t.cells[c].type == kFlipper && t.cells[c].dir == 0) flipCell = c;
    CHECK(flipCell >= 0);
    if (flipCell < 0) return;
    g.dropBall(flipCell);
    // the drop kicks the ball sideways one step; put it back on the flipper
    g.balls[0].forcedSteps = 0;
    g.balls[0].cell = uint8_t(flipCell);
    g.flip(0, true, 127);
    g.clearOut();
    g.step();
    CHECK(cellRow(g.balls[0].cell) < cellRow(flipCell));   // moved up
    CHECK(g.noteCount >= 1);
    g.flip(0, false, 0);
}

LAB_TEST(flipper_transitions_graph_goto_and_length) {
    // Go-to event on a hole switches at the next bar
    {
        auto m = factory();
        Table& t = m->tab[0];
        clearTable(t);
        t.lengthBars = 0;
        const int h = cellAt(5, 1);
        t.cells[h].type = kHole;
        t.cells[h].event = kEvGoto;
        t.cells[h].arg = 5;
        t.cells[h].len = 2;
        t.cells[h].inst = 4;
        Game g;
        g.init(m.get(), 11);
        g.setMode(kMarkov);
        g.newGame(0);
        g.dropBall(cellAt(5, 0));
        g.balls[0].forcedSteps = 1;
        g.balls[0].forcedDir = kS;
        g.clearOut();
        g.step();
        CHECK(g.balls[0].cell == h);
        CHECK(g.pendingTable() == 5);
        for (int i = 0; i < 20 && g.tableIndex() == 0; ++i) {
            g.clearOut();
            g.step();
        }
        CHECK(g.tableIndex() == 5);
    }
    // a section length ends the table along the graph (only edges to table 3)
    {
        auto m = factory();
        for (auto& t : m->tab) {
            std::memset(t.graph, 0, sizeof t.graph);
            t.graph[3] = 9;
            t.lengthBars = 1;   // 4 bars
            t.balls = 10;
            t.jackpot = 0;      // only the length ends a table here
            for (auto& c : t.cells) c.event = kEvNone;
        }
        m->tab[3].graph[3] = 0;
        m->tab[3].graph[6] = 9;
        Game g;
        g.init(m.get(), 12);
        g.setMode(kMarkov);
        g.newGame(0);
        g.launch();
        int seen3 = -1, seen6 = -1;
        for (int i = 0; i < 16 * 9; ++i) {
            g.clearOut();
            g.step();
            if (g.tableIndex() == 3 && seen3 < 0) seen3 = i;
            if (g.tableIndex() == 6 && seen6 < 0) seen6 = i;
        }
        std::fprintf(stderr, "  section end at step %d, next at %d\n", seen3, seen6);
        CHECK(seen3 == 64);   // after 4 bars of 1/16
        CHECK(seen6 > seen3);
    }
    // disabled tables are never picked by the graph
    {
        auto m = factory();
        for (int i = 1; i < kTables; ++i) m->tab[i].enabled = 0;
        for (auto& t : m->tab) {
            for (auto& w : t.graph) w = 9;
            t.lengthBars = 1;
        }
        Game g;
        g.init(m.get(), 13);
        g.setMode(kMarkov);
        g.newGame(0);
        g.launch();
        bool stayed = true;
        for (int i = 0; i < 1000; ++i) {
            g.clearOut();
            g.step();
            if (g.tableIndex() != 0) stayed = false;
        }
        CHECK(stayed);
    }
    // the score jackpot fires its action
    {
        auto m = factory();
        Table& t = m->tab[0];
        t.jackpot = 1;
        t.onJackpot = kActGoto;
        t.jackpotArg = 2;
        t.lengthBars = 0;
        Game g;
        g.init(m.get(), 14);
        g.setMode(kMarkov);
        g.newGame(0);
        g.launch();
        bool jackpot = false;
        for (int i = 0; i < 4000 && g.tableIndex() == 0; ++i) {
            g.clearOut();
            g.step();
            for (int b = 0; b < g.bannerCount; ++b) jackpot = jackpot || g.banners[b] == kBnJackpot;
        }
        CHECK(jackpot);
        CHECK(g.tableIndex() == 2);
    }
}

LAB_TEST(flipper_multiball_and_tilt) {
    auto m = factory();
    Table& t = m->tab[0];
    clearTable(t);
    const int h = cellAt(4, 1);
    t.cells[h].type = kHole;
    t.cells[h].event = kEvMultiball;
    t.cells[h].len = 1;
    Game g;
    g.init(m.get(), 15);
    g.setMode(kMarkov);
    g.newGame(0);
    g.dropBall(cellAt(4, 0));
    g.balls[0].forcedSteps = 1;
    g.balls[0].forcedDir = kS;
    g.clearOut();
    g.step();
    CHECK(g.ballsInPlay() == kMaxBalls);
    bool mb = false;
    for (int b = 0; b < g.bannerCount; ++b) mb = mb || g.banners[b] == kBnMultiball;
    CHECK(mb);
    CHECK(g.heat() >= 1);
    // three tilts lock the flippers
    for (int i = 0; i < 3; ++i) {
        g.tilt();
        g.clearOut();
        g.step();
    }
    CHECK(g.tiltLocked());
    CHECK(!g.flipperUp(0));
}

LAB_TEST(flipper_capture_and_replay_reproduce_the_notes) {
    auto m = factory();
    std::vector<ClipStep> runBuf(kRunCap);
    Game g;
    g.init(m.get(), 21);
    g.attachRun(runBuf.data(), kRunCap);
    g.setMode(kMarkov);
    g.newGame(1);
    g.launch();
    std::vector<int> played;
    for (int i = 0; i < 256; ++i) {
        g.clearOut();
        g.step();
        int sum = 0;
        for (int k = 0; k < g.noteCount; ++k)
            if (g.notes[k].ball != kNone) sum += g.notes[k].note * 131 + g.notes[k].inst;
        played.push_back(sum);
    }
    CHECK(g.runCount() == 256);
    // capture the whole run
    m->clipLen = g.runCount();
    for (uint32_t i = 0; i < m->clipLen; ++i) m->clip[i] = runBuf[i];
    g.setClipLoop(0, m->clipLen);
    g.setReplay(true);
    CHECK(g.replaying());
    int same = 0;
    for (int i = 0; i < 256; ++i) {
        g.clearOut();
        g.step();
        int sum = 0;
        for (int k = 0; k < g.noteCount; ++k)
            if (g.notes[k].ball != kNone) sum += g.notes[k].note * 131 + g.notes[k].inst;
        same += sum == played[size_t(i)] ? 1 : 0;
    }
    std::fprintf(stderr, "  replay matched %d / 256 steps\n", same);
    CHECK(same == 256);
    // it loops
    g.clearOut();
    g.step();
    CHECK(g.clipPos() == 1);
}

LAB_TEST(flipper_save_load_roundtrip) {
    MemStorage fs;
    auto m = factory();
    m->g.mode = kMarkov;
    m->g.autoSkill = 33;
    m->tab[2].cells[7].type = kRamp;
    m->tab[2].cells[7].fx = kFxCrush;
    m->tab[5].graph[1] = 7;
    std::strcpy(m->tab[4].name, "MYTABLE");
    m->inst[3].engine = kEnSample;
    std::strcpy(m->sample[3], "Lab/FLIPPER/Samples/break.wav");
    m->clipLen = 3;
    m->clip[1].cell[0] = 17;
    m->clip[1].note[0] = 64;
    CHECK(saveMachine(fs, "M.FLP", *m));
    auto r = factory();
    CHECK(loadMachine(fs, "M.FLP", *r));
    CHECK(std::memcmp(&r->g, &m->g, sizeof m->g) == 0);
    CHECK(std::memcmp(r->tab, m->tab, sizeof m->tab) == 0);
    CHECK(std::memcmp(r->inst, m->inst, sizeof m->inst) == 0);
    CHECK(std::strcmp(r->sample[3], m->sample[3]) == 0);
    CHECK(r->clipLen == 3);
    CHECK(r->clip[1].cell[0] == 17 && r->clip[1].note[0] == 64);
    // a corrupt file is rejected, a truncated one keeps the defaults it could not read
    auto x = factory();
    CHECK(!loadMachine(fs, "missing.flp", *x));
    fs.files["BAD.FLP"] = {'N', 'O', 'P', 'E', 1, 0, 0, 0};
    CHECK(!loadMachine(fs, "BAD.FLP", *x));
    std::vector<uint8_t> half = fs.files["M.FLP"];
    half.resize(half.size() / 2);
    fs.files["HALF.FLP"] = half;
    auto y = factory();
    loadMachine(fs, "HALF.FLP", *y);   // no crash; values sanitized
    CHECK(y->g.mode == kMarkov);
    // unknown chunks are skipped (forward compatibility)
    std::vector<uint8_t> f = fs.files["M.FLP"];
    const uint8_t extra[] = {'Z', 'Z', 'Z', 'Z', 3, 0, 0, 0, 0, 1, 2};
    f.insert(f.begin() + 8, extra, extra + sizeof extra);
    fs.files["EXTRA.FLP"] = f;
    auto z = factory();
    CHECK(loadMachine(fs, "EXTRA.FLP", *z));
    CHECK(std::strcmp(z->tab[4].name, "MYTABLE") == 0);
    // out-of-range bytes are clamped
    auto w = factory();
    w->tab[0].cells[0].type = 200;
    w->tab[0].gravity = 99;
    w->g.mode = 7;
    sanitize(*w);
    CHECK(w->tab[0].cells[0].type < kElemCount);
    CHECK(w->tab[0].gravity <= 9);
    CHECK(w->g.mode <= 1);
}

LAB_TEST(flipper_workstation_boots_and_plays) {
    std::vector<uint8_t> mem(64u * 1024u * 1024u);
    MemStorage fs;
    lab::Shell shell;
    lab::Hal h;
    h.fs = &fs;
    CHECK(shell.init(h, mem.data(), uint32_t(mem.size()), "FLIPPER"));
    auto* ws = static_cast<Flipper*>(shell.workstation());
    CHECK(ws != nullptr);
    if (!ws) return;
    lab::Transport tr;
    tr.playing = true;
    tr.bpm = 140.0f;
    tr.beatsPerSample = 140.0 / 60.0 / 44100.0;
    tr.startedThisBlock = true;
    ws->onTransport(true);
    lab::AudioIn in;
    float l[lab::kBlock], r[lab::kBlock];
    float peak = 0.0f;
    bool finite = true;
    for (int mode = 0; mode < 2; ++mode) {
        ws->machine().g.mode = mode;
        for (int b = 0; b < 3000; ++b) {
            for (uint32_t i = 0; i < lab::kBlock; ++i) l[i] = r[i] = 0.0f;
            ws->render(tr, in, l, r);
            tr.startedThisBlock = false;
            for (uint32_t i = 0; i < lab::kBlock; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i])) finite = false;
                peak = std::fmax(peak, std::fabs(l[i]));
            }
            tr.beat += tr.beatsPerSample * lab::kBlock;
            ws->update(uint32_t(b * 3));
        }
    }
    CHECK(finite);
    CHECK(peak > 0.05f && peak < 4.0f);
    std::fprintf(stderr, "  workstation peak %.3f, %lu steps\n", double(peak),
                 static_cast<unsigned long>(ws->game().steps()));
    // capture + save + load through the workstation
    CHECK(ws->capture(0));
    CHECK(ws->machine().clipLen > 0);
    CHECK(ws->saveSlot(2));
    CHECK(fs.exists("Lab/FLIPPER/MACHINE3.FLP"));
    const uint32_t len = ws->machine().clipLen;
    CHECK(ws->loadSlot(2));
    ws->pumpCommands();
    CHECK(ws->machine().clipLen == len);
}

LAB_TEST(flipper_game_over_after_the_last_ball) {
    auto m = factory();
    m->g.autoOn = 1;
    m->g.autoAfter = 1;
    m->g.autoSkill = 0;
    Table& t = m->tab[0];
    t.balls = 2;
    t.ballSave = 0;
    t.onGameOver = kActGoto;
    t.gameOverArg = 4;
    t.lengthBars = 0;
    t.jackpot = 0;
    for (auto& c : t.cells) c.event = kEvNone;
    Game g;
    g.init(m.get(), 8);
    g.setMode(kMarkov);
    g.newGame(0);
    CHECK(g.ballsLeft() == 2);
    int drains = 0, overAt = -1;
    for (int i = 0; i < 20000 && overAt < 0; ++i) {
        g.clearOut();
        g.step();
        for (int b = 0; b < g.bannerCount; ++b) {
            if (g.banners[b] == kBnDrain) ++drains;
            if (g.banners[b] == kBnGameOver) overAt = i;
        }
    }
    CHECK(overAt > 0);
    CHECK(drains == 2);              // exactly the table's two balls
    CHECK(g.tableIndex() == 4);      // the game-over action went to table 5
    CHECK(g.ballsLeft() == m->tab[4].balls);
    // with no ball in play the rails keep playing
    g.stopGame();
    int notes = 0;
    for (int i = 0; i < 64; ++i) {
        g.clearOut();
        g.step();
        notes += g.noteCount;
    }
    CHECK(notes > 0);
}
