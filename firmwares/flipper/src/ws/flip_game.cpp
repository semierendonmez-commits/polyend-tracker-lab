// FLIPPER -- the game (flip_game.h).
#include "ws/flip_game.h"

#include <cmath>

namespace flip {

using lab::dsp::clamp;

const char* const kBannerText[kBnCount] = {"",          "MULTIBALL",  "JACKPOT",  "TILT",    "DANGER",
                                           "BALL SAVED", "DRAIN",     "EXTRA BALL", "MULTIPLIER", "BANK BONUS",
                                           "MUTATION",  "NEW TABLE", "GAME OVER", "LAUNCH"};

namespace {
const uint8_t kDir4[4] = {kN, kE, kS, kW};
inline float sq(float v) { return v * v; }
inline bool finite(float v) { return v == v && v < 1e9f && v > -1e9f; }
constexpr float kLaneX = kFieldW - kCellW;   // left side of the plunger lane (launch column 11)
}  // namespace

int cellFromXY(float x, float y) {
    int c = int(x / kCellW), r = int(y / kCellH);
    if (c < 0) c = 0;
    if (c >= kCols) c = kCols - 1;
    if (r < 0) r = 0;
    if (r >= kRows) r = kRows - 1;
    return cellAt(c, r);
}

int moveDest(const Table& t, int cell, int dir, bool drainOk) {
    int c = cellCol(cell) + dirDx(dir), r = cellRow(cell) + dirDy(dir);
    if (r < 0) return -1;
    if (r >= kRows) return drainOk ? -2 : -1;
    if (c < 0 || c >= kCols) {
        if (!t.sides) return -1;
        c = (c + kCols) % kCols;
    }
    const int d = cellAt(c, r);
    if (t.cells[d].type == kWall) return -1;
    return d;
}

void moveWeights(const Table& t, int cell, int attract, float pull, bool drainOk, float out[5]) {
    const Cell& c = t.cells[cell];
    const float g = float(t.gravity);
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const int d = moveDest(t, cell, kDir4[i], drainOk);
        float w = d == -1 ? 0.0f : float(c.w[i]);
        if (i == 2) w *= 1.0f + g * 0.12f;          // gravity: down is likelier
        if (i == 0) w *= 1.0f - g * 0.05f;          // ... and up less likely
        if (d == -2) w *= 0.6f;                     // drains are a little less likely than a cell
        if (attract >= 0 && attract != cell && w > 0.0f) {
            const int dc = cellCol(attract) - cellCol(cell), dr = cellRow(attract) - cellRow(cell);
            const bool toward = (i == 0 && dr < 0) || (i == 2 && dr > 0) || (i == 1 && dc > 0) || (i == 3 && dc < 0);
            w *= toward ? 1.0f + pull * 8.0f : 1.0f - pull * 0.85f;
        }
        out[i] = w;
        sum += w;
    }
    out[4] = 0.0f;
    if (attract == cell && attract >= 0) out[4] = (sum > 0.0f ? sum : 1.0f) * pull * 3.0f;
    if (sum <= 0.0f && out[4] <= 0.0f) out[4] = 1.0f;
}

float entropyBits(const float* w, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += w[i];
    if (sum <= 0.0f) return 0.0f;
    float h = 0.0f;
    for (int i = 0; i < n; ++i)
        if (w[i] > 0.0f) {
            const float p = w[i] / sum;
            h -= p * log2f(p);
        }
    return h;
}

// ---- setup ---------------------------------------------------------------------------
void Game::init(Machine* m, uint32_t seed) {
    m_ = m;
    rng_ = lab::dsp::Rng(seed);
    tab_ = 0;
    for (auto& b : balls) b = Ball();
    stopped_ = true;
    rebuild();
}

void Game::rebuild() {
    const Table& t = table();
    segCount_ = 0;
    auto addSeg = [&](float x0, float y0, float x1, float y1) {
        if (segCount_ < kCells) seg_[segCount_++] = Seg{x0, y0, x1, y1};
    };
    // corner deflectors (top) and the plunger lane wall
    addSeg(0.0f, 64.0f, 64.0f, 0.0f);
    addSeg(kFieldW - 64.0f, 0.0f, kFieldW, 64.0f);
    if (t.launchCol == kCols - 1) addSeg(kLaneX, kCellH * 0.9f, kLaneX, kFieldH);
    else if (t.launchCol == 0) addSeg(kCellW, kCellH * 0.9f, kCellW, kFieldH);
    flipCell_[0] = flipCell_[1] = -1;
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        Shape& s = shape_[c];
        s = Shape();
        s.x = cellX(c);
        s.y = cellY(c);
        switch (cl.type) {
            case kBumper: s.kind = 1; s.a = kBumperR; break;
            case kTarget: s.kind = 2; s.a = 17.0f; s.b = 5.0f; break;
            case kHole: s.kind = 3; s.a = kHoleR; break;
            case kRamp: s.kind = 6; break;
            case kFlipper: {
                s.kind = 4;
                const int side = cl.dir ? 1 : 0;
                if (flipCell_[side] < 0) flipCell_[side] = c;
                ang_[c] = angPrev_[c] = kFlipRest;
                break;
            }
            case kWall:
                if (cl.dir == kWallBlock) {
                    s.kind = 2;
                    s.a = kCellW * 0.5f - 1.0f;
                    s.b = kCellH * 0.5f - 1.0f;
                }
                break;
            default: break;
        }
    }
    // slopes: one segment per run of equal slope cells in a row
    for (int r = 0; r < kRows; ++r) {
        int c = 0;
        while (c < kCols) {
            const Cell& cl = t.cells[cellAt(c, r)];
            if (cl.type != kWall || cl.dir == kWallBlock) {
                ++c;
                continue;
            }
            const uint8_t kind = cl.dir;
            int e = c;
            while (e + 1 < kCols && t.cells[cellAt(e + 1, r)].type == kWall && t.cells[cellAt(e + 1, r)].dir == kind) ++e;
            const float top = float(r) * kCellH, x0 = float(c) * kCellW, x1 = float(e + 1) * kCellW;
            const float hi = top + kCellH * 0.12f, lo = top + kCellH * 0.55f;
            if (kind == kWallSlopeR) addSeg(x0, hi, x1, lo);
            else addSeg(x0, lo, x1, hi);
            c = e + 1;
        }
    }
}

int Game::freeSlot() const {
    for (int i = 0; i < kMaxBalls; ++i)
        if (!balls[i].active) return i;
    return -1;
}

int Game::ballsInPlay() const {
    int n = 0;
    for (const auto& b : balls) n += b.active ? 1 : 0;
    return n;
}

bool Game::autoActive() const {
    return m_ && m_->g.autoOn && !replay_ && idleT_ >= float(m_->g.autoAfter);
}

int Game::stepsPerBar() const { return flip::stepsPerBar(table().stepRate); }
int Game::bar() const { return int(stepIdx_ / uint32_t(stepsPerBar())); }

bool Game::flipperUp(int side) const {
    side &= 1;
    return (flipHeld_[side] || autoHold_[side] > 0.0f || flipVis_[side] > 0.0f) && tiltLock_ <= 0;
}

void Game::newGame(int t) {
    stopped_ = false;
    over_ = false;
    score_ = 0;
    mult_ = 1;
    combo_ = 0;
    heatAcc_ = 0.0f;
    heat_ = 0;
    tiltMeter_ = tiltLock_ = 0;
    pending_ = -1;
    replay_ = false;
    for (auto& b : balls) b = Ball();
    for (int c = 0; c < kCells; ++c) {
        visit_[c] = 0;
        walk_[c] = 0;
    }
    runGameStart_ = runCount_;
    switchTable(t < 0 || t >= kTables ? 0 : t);
    launchWait_ = 2;
}

void Game::stopGame() {
    stopped_ = true;
    for (auto& b : balls) b.active = false;
}

void Game::switchTable(int t) {
    if (t < 0 || t >= kTables) t = 0;
    if (!m_->tab[t].enabled) t = tab_;
    const bool changed = t != tab_;
    tab_ = t;
    pending_ = -1;
    pendingNow_ = false;
    stepIdx_ = 0;
    tableScore_ = 0;
    jackpotDone_ = false;
    const Table& tb = table();
    ballsLeft_ = tb.balls >= 10 ? 99 : tb.balls;
    for (int c = 0; c < kCells; ++c) {
        lit_[c] = 0;
        fxUntil_[c] = 0;
        bumpCool_[c] = 0.0f;
    }
    rebuild();
    // balls in play stay on the table; a ball inside a wall moves up
    for (auto& b : balls) {
        if (!b.active) continue;
        b.hold = 0;
        b.forcedSteps = 0;
        b.captured = false;
        fixBallCell(b);
    }
    if (changed) {
        banner(kBnTable);
        switched = true;
    }
}

void Game::fixBallCell(Ball& b) {
    const Table& t = table();
    int c = b.cell;
    while (t.cells[c].type == kWall && cellRow(c) > 0) c -= kCols;
    if (t.cells[c].type == kWall) {
        for (int i = 0; i < kCells; ++i)
            if (t.cells[i].type != kWall) {
                c = i;
                break;
            }
    }
    b.cell = b.prevCell = uint8_t(c);
    if (b.phys && t.cells[cellFromXY(b.x, b.y)].type == kWall && t.cells[cellFromXY(b.x, b.y)].dir == kWallBlock) {
        b.x = cellX(c);
        b.y = cellY(c);
    }
}

int Game::pickGraph() {
    const Table& t = table();
    float sum = 0.0f;
    for (int i = 0; i < kTables; ++i)
        if (m_->tab[i].enabled) sum += float(t.graph[i]);
    if (sum <= 0.0f) return tab_;
    float r = rng_.uni() * sum;
    for (int i = 0; i < kTables; ++i) {
        if (!m_->tab[i].enabled) continue;
        r -= float(t.graph[i]);
        if (r < 0.0f && t.graph[i]) return i;
    }
    return tab_;
}

void Game::gotoTable(int t, bool now) {
    if (t < 0 || t >= kTables || !m_->tab[t].enabled) return;
    pending_ = t;
    pendingNow_ = now;
    if (stopped_ || over_) {
        pendingNew_ = true;
    }
}

void Game::doAction(uint8_t act, uint8_t arg) {
    switch (act) {
        case kActGraph: pending_ = pickGraph(); break;
        case kActGoto: pending_ = arg; break;
        case kActMultiball: addBalls(kMaxBalls, -1); break;
        default: break;
    }
}

void Game::banner(uint8_t b) {
    if (bannerCount < kBannerCap) banners[bannerCount++] = b;
}

void Game::note(int inst, int n, int vel, int ball, int cell, bool ghost) {
    if (inst < 0 || inst >= kInsts || noteCount >= kNoteCap) return;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;
    NoteOut& o = notes[noteCount++];
    o.inst = uint8_t(inst);
    o.note = uint8_t(n);
    o.vel = uint8_t(vel);
    o.ball = uint8_t(ball < 0 ? kNone : ball);
    o.cell = uint8_t(cell < 0 ? kNone : cell);
    o.ghost = ghost;
    if (ball >= 0 && ball < kMaxBalls) {
        balls[ball].lastNote = uint8_t(n);
        balls[ball].lastVel = uint8_t(vel);
        balls[ball].lastInst = uint8_t(inst);
    }
}

void Game::addScore(uint32_t pts) {
    const uint32_t p = pts * uint32_t(mult_) * uint32_t(8 + (combo_ > 24 ? 24 : combo_)) / 8u;
    score_ += p;
    tableScore_ += p;
    const Table& t = table();
    if (t.jackpot && !jackpotDone_ && tableScore_ >= uint32_t(t.jackpot) * 1000u) {
        jackpotDone_ = true;
        banner(kBnJackpot);
        heatAcc_ += 1.0f;
        doAction(t.onJackpot, t.jackpotArg);
    }
}

int Game::pitchFor(const Cell& c, int cell, int ball) {
    const Table& t = table();
    switch (c.pitchMode) {
        case kPmWalk: {
            int w = walk_[cell] + (rng_.chance(0.5f) ? 1 : -1);
            if (w > 7) w = 5;
            if (w < -7) w = -5;
            walk_[cell] = int8_t(w);
            return scaleNote(c.note, w, t.root, t.scale);
        }
        case kPmCombo: return scaleNote(c.note, combo_ % 8, t.root, t.scale);
        case kPmBall: return scaleNote(c.note, (ball < 0 ? 0 : ball) * 2, t.root, t.scale);
        case kPmRandom: return scaleNote(c.note, rng_.range(5), t.root, t.scale);
        default: return c.note;
    }
}

int Game::velFor(const Cell& c, int ball) {
    float v = float(c.vel);
    const Ball* b = ball >= 0 && ball < kMaxBalls ? &balls[ball] : nullptr;
    switch (c.velMode) {
        case kVmSpeed:
            if (b && b->phys) v *= clamp(0.45f + sqrtf(b->vx * b->vx + b->vy * b->vy) / 900.0f, 0.4f, 1.15f);
            else if (b) v *= 0.6f + 0.1f * float(b->momentum > 4 ? 4 : b->momentum);
            break;
        case kVmAccent: v *= (stepIdx_ % 4 == 0) ? 1.0f : 0.68f; break;
        case kVmRandom: v *= 0.5f + 0.5f * rng_.uni(); break;
        default: break;
    }
    v += float(heat_) * 4.0f;
    return int(v);
}

// ---- hits and events -------------------------------------------------------------------
void Game::hit(int bi, int cell) {
    Table& t = m_->tab[tab_];
    const Cell& c = t.cells[cell];
    Ball& b = balls[bi];
    ++visit_[cell];
    hitAt_[cell] = stepCount_;
    const bool plays = c.chance >= 100 || rng_.uni() * 100.0f < float(c.chance);
    switch (c.type) {
        case kBumper:
            if (plays) note(c.inst, pitchFor(c, cell, bi), velFor(c, bi), bi, cell, false);
            addScore(c.points * 10u);
            ++combo_;
            comboIdle_ = 0;
            heatAcc_ += 0.1f;
            b.bounce = true;
            break;
        case kRamp:
            if (plays) note(c.inst, pitchFor(c, cell, bi), velFor(c, bi), bi, cell, false);
            if (fxCount < kFxCap) {
                FxOut& f = fx[fxCount++];
                f.fx = c.fx;
                f.groups = c.groups;
                f.amount = c.amount;
                f.steps = c.len;
            }
            stepFx_ = uint8_t(cell);
            fxUntil_[cell] = stepCount_ + c.len;
            if (c.dir < kDirCount && mode_ == kMarkov) {
                b.forcedDir = c.dir;
                b.forcedSteps = 2;
            }
            addScore(c.points * 10u);
            combo_ += 2;
            comboIdle_ = 0;
            heatAcc_ += 0.2f;
            break;
        case kHole:
            if (plays) note(c.inst, pitchFor(c, cell, bi), velFor(c, bi), bi, cell, false);
            b.hold = c.len;
            addScore(c.points * 10u);
            heatAcc_ += 0.1f;
            event(bi, cell, c.event, c.arg);
            break;
        case kTarget: {
            if (plays) note(c.inst, pitchFor(c, cell, bi), velFor(c, bi), bi, cell, false);
            lit_[cell] = 1;
            addScore(c.points * 10u);
            ++combo_;
            comboIdle_ = 0;
            heatAcc_ += 0.1f;
            event(bi, cell, c.event, c.arg);
            bool all = true;
            for (int i = 0; i < kCells; ++i)
                if (t.cells[i].type == kTarget && !lit_[i]) all = false;
            if (all) {
                banner(kBnBank);
                addScore(5000u);
                heatAcc_ += 0.5f;
                for (int i = 0; i < kCells; ++i) lit_[i] = 0;
            }
            break;
        }
        default: break;
    }
    if (heatAcc_ > 4.0f) heatAcc_ = 4.0f;
}

void Game::event(int bi, int cell, uint8_t ev, uint8_t arg) {
    const Table& t = table();
    switch (ev) {
        case kEvGraph: pending_ = pickGraph(); if (pending_ == tab_) pending_ = -1; break;
        case kEvGoto: pending_ = arg; break;
        case kEvMultiball: addBalls(kMaxBalls, cell); break;
        case kEvBallSave:
            for (auto& b : balls)
                if (b.active) b.save = int16_t(t.ballSave ? t.ballSave : 16);
            banner(kBnBallSave);
            break;
        case kEvMultiplier:
            if (mult_ < 8) mult_ *= 2;
            banner(kBnMultiplier);
            break;
        case kEvMutate: {
            Table& tw = m_->tab[tab_];
            for (int k = 0; k < 6; ++k) {
                Cell& c = tw.cells[rng_.range(kCells)];
                if (c.type == kWall || c.type == kFlipper) continue;
                const int d = rng_.range(4);
                int w = int(c.w[d]) + (rng_.chance(0.5f) ? 2 : -2);
                c.w[d] = uint8_t(w < 0 ? 0 : (w > 9 ? 9 : w));
            }
            for (int c = 0; c < kCells; ++c)
                if (tw.cells[c].type == kBumper) walk_[c] = int8_t(rng_.range(7) - 3);
            banner(kBnMutate);
            break;
        }
        case kEvExtraBall:
            if (ballsLeft_ < 99) ++ballsLeft_;
            banner(kBnExtraBall);
            break;
        case kEvJackpot:
            banner(kBnJackpot);
            addScore(20000u);
            doAction(t.onJackpot, t.jackpotArg);
            break;
        default: break;
    }
    (void)bi;
}

void Game::addBalls(int n, int fromCell) {
    int added = 0;
    for (int k = 0; k < n; ++k) {
        const int s = freeSlot();
        if (s < 0) break;
        spawn(s, fromCell >= 0 ? fromCell : -1, fromCell < 0);
        ++added;
    }
    if (added) {
        banner(kBnMultiball);
        heatAcc_ += 0.5f;
        const Table& t = table();
        if (t.onMultiball != kActMultiball) doAction(t.onMultiball, t.multiballArg);
    }
}

void Game::spawn(int bi, int cell, bool fromLaunch) {
    const Table& t = table();
    Ball& b = balls[bi];
    b = Ball();
    b.active = true;
    b.save = int16_t(t.ballSave);
    b.phys = mode_ == kHybrid;
    if (fromLaunch || cell < 0) {
        b.cell = uint8_t(cellAt(t.launchCol, kRows - 1));
        if (t.cells[b.cell].type == kWall) fixBallCell(b);
        b.forcedDir = kN;
        b.forcedSteps = 3;
        b.x = cellX(cellAt(t.launchCol, 0));
        b.y = kFieldH - kBallR - 2.0f;
        b.vx = -rng_.uni() * 30.0f;
        b.vy = -(780.0f + rng_.uni() * 160.0f);
        banner(kBnLaunch);
    } else {
        b.cell = uint8_t(cell);
        b.forcedDir = rng_.chance(0.5f) ? kSE : kSW;
        b.forcedSteps = 1;
        b.x = cellX(cell);
        b.y = cellY(cell) + kHoleR + kBallR + 1.0f;
        b.vx = rng_.bi() * 200.0f;
        b.vy = 120.0f;
    }
    b.prevCell = b.cell;
    b.movedAt = stepCount_;
    if (b.phys) b.forcedSteps = 0;
}

void Game::drain(int bi) {
    Ball& b = balls[bi];
    if (b.save > 0) {
        banner(kBnBallSave);
        spawn(bi, -1, true);
        return;
    }
    b.active = false;
    banner(kBnDrain);
    heatAcc_ -= 0.4f;
    if (heatAcc_ < 0.0f) heatAcc_ = 0.0f;
    if (ballsInPlay() == 0) {
        // ballsLeft_ counts the ball that just drained: the game ends with the last one
        if (ballsLeft_ < 99) --ballsLeft_;
        if (ballsLeft_ > 0) launchWait_ = 6;
        else checkOver();
    }
}

void Game::checkOver() {
    const Table& t = table();
    banner(kBnGameOver);
    // The song goes on: the game-over rule picks the next table (or restarts this one).
    int next = tab_;
    if (t.onGameOver == kActGraph) next = pickGraph();
    else if (t.onGameOver == kActGoto) next = t.gameOverArg;
    switchTable(next);
    launchWait_ = 6;
}

void Game::flip(int side, bool down, uint8_t vel) {
    side &= 1;
    flipHeld_[side] = down;
    if (down) {
        flipArmed_[side] = 2;
        flipVel_[side] = vel;
    }
    markPlayer();
}

void Game::launch() {
    markPlayer();
    launchQueued_ = true;
}

void Game::tilt() {
    markPlayer();
    tiltReq_ = true;
}

void Game::dropBall(int cell) {
    if (cell < 0 || cell >= kCells || table().cells[cell].type == kWall) return;
    const int s = freeSlot();
    if (s < 0) return;
    spawn(s, cell, false);
    stopped_ = false;
}

void Game::setReplay(bool on) {
    replay_ = on && m_->clipLen > 0;
    clipPos_ = clipA_ < m_->clipLen ? clipA_ : 0;
    if (replay_) {
        stopped_ = false;
        over_ = false;
        for (auto& b : balls) b = Ball();
        if (m_->clip[clipPos_].table != tab_) switchTable(m_->clip[clipPos_].table);
        stepIdx_ = 0;
    } else {
        for (auto& b : balls) b.active = false;
        launchWait_ = 2;
    }
}

// ---- stepping ------------------------------------------------------------------------------
bool Game::flipActiveMarkov(int side, float& power) {
    if (tiltLock_ > 0) return false;
    power = 1.0f;
    if (flipHeld_[side] || flipArmed_[side] > 0) {
        flipArmed_[side] = 0;
        power = 0.6f + float(flipVel_[side]) / 300.0f;
        return true;
    }
    if (autoActive()) {
        const float skill = float(m_->g.autoSkill) * 0.01f;
        if (rng_.uni() < 0.3f + 0.7f * skill) {
            autoHold_[side] = 0.12f;
            power = 0.7f + 0.3f * skill;
            return true;
        }
    }
    return false;
}

void Game::moveTo(int bi, int dest, int dir) {
    Ball& b = balls[bi];
    if (b.lastDir == dir) {
        if (b.momentum < 8) ++b.momentum;
    } else {
        b.momentum = 0;
    }
    b.lastDir = uint8_t(dir);
    b.prevCell = b.cell;
    b.cell = uint8_t(dest);
    b.movedAt = stepCount_;
}

void Game::stepMarkov(int bi) {
    Ball& b = balls[bi];
    const Table& t = table();
    b.bounce = false;
    b.prevCell = b.cell;
    if (b.hold > 0) {
        if (--b.hold == 0) {
            b.forcedDir = rng_.chance(0.5f) ? kSE : kSW;
            b.forcedSteps = 1;
        }
        return;
    }
    const Cell& here = t.cells[b.cell];
    if (here.type == kFlipper && b.forcedSteps == 0) {
        const int side = here.dir ? 1 : 0;
        float power = 1.0f;
        if (flipActiveMarkov(side, power)) {
            flipVis_[side] = 0.1f;
            b.forcedDir = side == 0 ? kNE : kNW;
            b.forcedSteps = uint8_t(1 + int(float(here.amount) * 0.5f * power + 0.5f));
            note(here.inst, here.note, int(float(here.vel) * (0.7f + 0.3f * power)), bi, b.cell, false);
            addScore(here.points * 10u);
            ++combo_;
            comboIdle_ = 0;
        }
    }
    if (b.forcedSteps > 0) {
        --b.forcedSteps;
        int d = b.forcedDir;
        int dest = moveDest(t, b.cell, d, false);
        if (dest < 0 && (d & 1)) {   // diagonal blocked: try its vertical, then its horizontal part
            const int v = dirDy(d) < 0 ? kN : kS, h = dirDx(d) > 0 ? kE : kW;
            dest = moveDest(t, b.cell, v, false);
            if (dest >= 0) d = v;
            else {
                dest = moveDest(t, b.cell, h, false);
                d = h;
                b.forcedDir = uint8_t(dirDy(b.forcedDir) < 0 ? (h == kE ? kNE : kNW) : (h == kE ? kSE : kSW));
            }
            if (dest >= 0 && d == v) b.forcedDir = uint8_t(v == kN ? (dirDx(b.forcedDir) > 0 ? kNW : kNE)
                                                               : (dirDx(b.forcedDir) > 0 ? kSW : kSE));
        }
        if (dest >= 0) {
            moveTo(bi, dest, d);
            hit(bi, dest);
            return;
        }
        b.forcedSteps = 0;
    }
    float w[5];
    moveWeights(t, b.cell, magnet_, 0.6f, true, w);
    if (b.lastDir < kDirCount && !(b.lastDir & 1)) {   // momentum keeps the ball going
        const int i = b.lastDir / 2;
        w[i] *= 1.0f + 0.2f * float(b.momentum > 4 ? 4 : b.momentum);
    }
    float sum = 0.0f;
    for (float v : w) sum += v;
    float r = rng_.uni() * sum;
    int pick = 4;
    for (int i = 0; i < 5; ++i) {
        r -= w[i];
        if (r < 0.0f && w[i] > 0.0f) {
            pick = i;
            break;
        }
    }
    if (pick == 4) return;
    const int dest = moveDest(t, b.cell, kDir4[pick], true);
    if (dest == -2) {
        if (cellCol(b.cell) == t.launchCol) {   // back into the plunger: launched again
            b.forcedDir = kN;
            b.forcedSteps = 3;
            return;
        }
        drain(bi);
        return;
    }
    if (dest < 0) return;
    moveTo(bi, dest, kDir4[pick]);
    hit(bi, dest);
}

void Game::stepHybrid(int bi) {
    Ball& b = balls[bi];
    const Table& t = table();
    b.bounce = false;
    b.prevCell = b.cell;
    b.forcedSteps = 0;
    if (b.hold > 0) {
        --b.hold;
        return;
    }
    if (b.jump) {
        b.jump = false;
        const int dest = b.capCell;
        b.capCell = kNone;
        if (dest < kCells) {
            const int dc = cellCol(dest) - cellCol(b.cell), dr = cellRow(dest) - cellRow(b.cell);
            int d = dr < 0 ? kN : (dr > 0 ? kS : (dc > 0 ? kE : kW));
            if (dest != b.cell) moveTo(bi, dest, d);
            if (t.cells[dest].type == kFlipper) {
                const Cell& c = t.cells[dest];
                note(c.inst, c.note, c.vel, bi, dest, false);
                ++combo_;
                comboIdle_ = 0;
                addScore(c.points * 10u);
            } else {
                hit(bi, dest);
            }
            return;
        }
    }
    const int target = cellFromXY(b.x, b.y);
    const float follow = float(m_->g.follow) * 0.01f;
    int dest = -1, dir = kN;
    const int dist = abs(cellCol(target) - cellCol(b.cell)) + abs(cellRow(target) - cellRow(b.cell));
    if (dist > 2 && t.cells[target].type != kWall) {
        dest = target;   // fast ball: the grid ball catches up
    } else if (target != b.cell || rng_.uni() > follow) {
        float w[5];
        moveWeights(t, b.cell, target, follow, false, w);
        float sum = 0.0f;
        for (float v : w) sum += v;
        float r = rng_.uni() * sum;
        for (int i = 0; i < 5; ++i) {
            r -= w[i];
            if (r < 0.0f && w[i] > 0.0f) {
                if (i < 4) {
                    dest = moveDest(t, b.cell, kDir4[i], false);
                    dir = kDir4[i];
                }
                break;
            }
        }
    }
    if (dest < 0 || dest == b.cell) return;
    moveTo(bi, dest, dir);
    // the grid ball wandered onto an element the real ball did not touch: a ghost note
    const Cell& c = t.cells[dest];
    ++visit_[dest];
    // the grid ball is the sequencer: it plays what it lands on (softer than a real hit)
    if (m_->g.ghost && (c.type == kBumper || c.type == kTarget) && c.inst != kNone &&
        (c.chance >= 100 || rng_.uni() * 100.0f < float(c.chance))) {
        note(c.inst, pitchFor(c, dest, bi), int(float(velFor(c, bi)) * 0.7f), bi, dest, false);
        hitAt_[dest] = stepCount_;
        heatAcc_ += 0.03f;
    }
}

void Game::rails() {
    const Table& t = table();
    for (int i = 0; i < kRails; ++i) {
        const Rail& r = t.rails[i];
        if (r.inst == kNone || r.hits == 0 || heat_ < r.heat) continue;
        const int pos = int(stepIdx_ % uint32_t(r.steps));
        if (lab::dsp::euclid(pos, r.steps, r.hits, r.rotate)) {
            int v = r.vel;
            if (t.stepRate != kRate4 && (pos & 1)) v = v * 3 / 4;
            note(r.inst, r.note, v, -1, -1, false);
        }
    }
}

void Game::record() {
    if (!run_ || !runCap_) return;
    ClipStep& s = run_[runCount_ % runCap_];
    s = ClipStep();
    s.table = uint8_t(tab_);
    s.heat = uint8_t(heat_);
    s.flags = tiltLock_ > 0 ? 1 : 0;
    s.fxCell = stepFx_;
    for (int i = 0; i < kMaxBalls; ++i) {
        const Ball& b = balls[i];
        if (!b.active) continue;
        s.cell[i] = b.cell;
        if (b.lastNote) {
            s.inst[i] = b.lastInst;
            s.note[i] = b.lastNote;
            s.vel[i] = b.lastVel;
        }
    }
    ++runCount_;
}

void Game::replayStep() {
    ++stepCount_;
    const uint32_t len = m_->clipLen;
    if (!len) {
        replay_ = false;
        return;
    }
    const uint32_t a = clipA_ < len ? clipA_ : 0;
    const uint32_t e = (clipB_ > a && clipB_ <= len) ? clipB_ : len;
    if (clipPos_ < a || clipPos_ >= e) {
        clipPos_ = a;
        stepIdx_ = 0;
    }
    const ClipStep& s = m_->clip[clipPos_];
    if (s.table != tab_) {
        const uint32_t keep = stepIdx_;
        switchTable(s.table);
        stepIdx_ = keep;
    }
    heat_ = s.heat;
    for (int i = 0; i < kMaxBalls; ++i) {
        Ball& b = balls[i];
        b.lastNote = 0;
        if (s.cell[i] == kNone) {
            b.active = false;
            continue;
        }
        if (!b.active) {
            b = Ball();
            b.active = true;
            b.cell = b.prevCell = s.cell[i];
        }
        if (b.cell != s.cell[i]) {
            const int dc = cellCol(s.cell[i]) - cellCol(b.cell), dr = cellRow(s.cell[i]) - cellRow(b.cell);
            moveTo(i, s.cell[i], dr < 0 ? kN : (dr > 0 ? kS : (dc > 0 ? kE : kW)));
        } else {
            b.prevCell = b.cell;
        }
        b.phys = false;
        if (s.note[i] && s.inst[i] != kNone) {
            const Cell& c = table().cells[s.cell[i]];
            if (replayLive_ && c.type != kEmpty && c.type != kWall) {
                if (c.inst != kNone) note(c.inst, c.note, c.vel, i, s.cell[i], false);
            } else {
                note(s.inst[i], s.note[i], s.vel[i], i, s.cell[i], false);
            }
            hitAt_[s.cell[i]] = stepCount_;
        }
    }
    if (s.fxCell < kCells) {
        const Cell& c = table().cells[s.fxCell];
        if (c.type == kRamp && fxCount < kFxCap) {
            FxOut& f = fx[fxCount++];
            f.fx = c.fx;
            f.groups = c.groups;
            f.amount = c.amount;
            f.steps = c.len;
            fxUntil_[s.fxCell] = stepCount_ + c.len;
        }
    }
    rails();
    ++clipPos_;
    ++stepIdx_;
}

void Game::step() {
    if (!m_) return;
    if (replay_) {
        replayStep();
        return;
    }
    ++stepCount_;
    stepFx_ = kNone;
    const Table& t0 = table();
    const int spb = stepsPerBar();
    // end of the section: the graph picks the next table
    const int bars = kLengthBars[t0.lengthBars];
    if (bars > 0 && stepIdx_ > 0 && stepIdx_ % uint32_t(bars * spb) == 0 && pending_ < 0) {
        const int n = pickGraph();
        if (n != tab_) {
            pending_ = n;
            pendingNow_ = true;
        }
    }
    if (pending_ >= 0) {
        static const int kQ[3] = {1, 2, 4};
        const uint32_t q = uint32_t(spb * kQ[m_->g.quant < 0 || m_->g.quant > 2 ? 0 : m_->g.quant]);
        if (pendingNew_) {
            pendingNew_ = false;
            newGame(pending_);
        } else if (pendingNow_ || stepIdx_ % q == 0 || pending_ == tab_) {
            switchTable(pending_);
        }
    }
    if (stopped_) {   // no balls: the rails keep the groove going
        if (launchQueued_) {
            stopped_ = false;
        } else {
            rails();
            record();
            ++stepIdx_;
            return;
        }
    }
    if (tiltReq_) {
        tiltReq_ = false;
        ++tiltMeter_;
        if (fxCount < kFxCap) fx[fxCount++] = FxOut{kFxStutter, 7, 80, 4};
        for (int i = 0; i < kMaxBalls; ++i) {
            Ball& b = balls[i];
            if (!b.active || b.hold) continue;
            if (b.phys) {
                b.vx += rng_.bi() * 420.0f;
                b.vy -= 200.0f + rng_.uni() * 350.0f;
            } else {
                for (int k = 0; k < 8; ++k) {
                    const int c = cellAt(rng_.range(kCols), rng_.range(kRows - 1));
                    if (table().cells[c].type != kWall) {
                        moveTo(i, c, kN);
                        break;
                    }
                }
            }
        }
        if (tiltMeter_ >= 3) {
            tiltLock_ = spb;
            tiltMeter_ = 0;
            banner(kBnTilt);
        } else {
            banner(kBnTiltWarn);
        }
    }
    // launching
    if (ballsInPlay() == 0) {
        if (launchQueued_ || (launchWait_ <= 0 && autoActive())) {
            const int s = freeSlot();
            if (s >= 0) spawn(s, -1, true);
        } else if (launchWait_ > 0) {
            --launchWait_;
        }
    } else if (launchQueued_ && m_->g.mode == kMarkov) {
        // the plunger press is only for a waiting ball
    }
    launchQueued_ = false;

    for (int i = 0; i < kMaxBalls; ++i) {
        Ball& b = balls[i];
        if (!b.active) continue;
        b.lastNote = 0;
        if (mode_ == kMarkov) stepMarkov(i);
        else stepHybrid(i);
        if (b.active && b.save > 0) --b.save;
    }
    for (int s = 0; s < 2; ++s)
        if (flipArmed_[s] > 0) --flipArmed_[s];
    if (++comboIdle_ > spb) combo_ = 0;
    heatAcc_ -= 0.006f;
    if (heatAcc_ < 0.0f) heatAcc_ = 0.0f;
    int h = int(heatAcc_) + (ballsInPlay() > 1 ? ballsInPlay() - 1 : 0);
    heat_ = h > 4 ? 4 : h;
    rails();
    if (stepIdx_ % uint32_t(spb * 2) == 0 && tiltMeter_ > 0) --tiltMeter_;
    if (tiltLock_ > 0) --tiltLock_;
    record();
    ++stepIdx_;
}

// ---- physics ------------------------------------------------------------------------------------
void Game::flipperPivot(int cell, float& px, float& py, float& dirSign) const {
    const int side = table().cells[cell].dir ? 1 : 0;
    const float left = float(cellCol(cell)) * kCellW, top = float(cellRow(cell)) * kCellH;
    px = side == 0 ? left + 2.0f : left + kCellW - 2.0f;
    py = top + kCellH * 0.55f;
    dirSign = side == 0 ? 1.0f : -1.0f;
}

void Game::flipperGeom(int cell, float& px, float& py, float& tx, float& ty) const {
    float sg;
    flipperPivot(cell, px, py, sg);
    const float a = ang_[cell];
    tx = px + sg * kFlipLen * cosf(a);
    ty = py + kFlipLen * sinf(a);
}

void Game::collideCircle(Ball& b, float cx, float cy, float r, float kick, float e) {
    const float dx = b.x - cx, dy = b.y - cy, R = r + kBallR;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= R * R || d2 < 1e-6f) return;
    const float d = sqrtf(d2), nx = dx / d, ny = dy / d;
    b.x = cx + nx * R;
    b.y = cy + ny * R;
    const float vn = b.vx * nx + b.vy * ny;
    if (vn < 0.0f) {
        b.vx -= (1.0f + e) * vn * nx;
        b.vy -= (1.0f + e) * vn * ny;
    }
    if (kick > 0.0f) {
        const float out = b.vx * nx + b.vy * ny;
        if (out < kick) {
            b.vx += (kick - out) * nx;
            b.vy += (kick - out) * ny;
        }
    }
}

bool Game::collideSeg(Ball& b, float x0, float y0, float x1, float y1, float r, float e) {
    const float ex = x1 - x0, ey = y1 - y0;
    const float l2 = ex * ex + ey * ey;
    float u = l2 > 0.0f ? ((b.x - x0) * ex + (b.y - y0) * ey) / l2 : 0.0f;
    u = clamp(u, 0.0f, 1.0f);
    const float qx = x0 + ex * u, qy = y0 + ey * u;
    const float dx = b.x - qx, dy = b.y - qy, R = r + kBallR;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= R * R) return false;
    float nx, ny;
    if (d2 < 1e-6f) {
        nx = -ey;
        ny = ex;
        const float l = sqrtf(l2 > 0.0f ? l2 : 1.0f);
        nx /= l;
        ny /= l;
        if (ny > 0.0f) {
            nx = -nx;
            ny = -ny;
        }
    } else {
        const float d = sqrtf(d2);
        nx = dx / d;
        ny = dy / d;
    }
    b.x = qx + nx * R;
    b.y = qy + ny * R;
    const float vn = b.vx * nx + b.vy * ny;
    if (vn < 0.0f) {
        b.vx -= (1.0f + e) * vn * nx;
        b.vy -= (1.0f + e) * vn * ny;
    }
    return true;
}

void Game::collideRect(Ball& b, float cx, float cy, float hw, float hh, float e) {
    const float qx = clamp(b.x, cx - hw, cx + hw), qy = clamp(b.y, cy - hh, cy + hh);
    float dx = b.x - qx, dy = b.y - qy;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= kBallR * kBallR) return;
    float nx, ny, d;
    if (d2 < 1e-6f) {   // centre inside: push out along the shallowest axis
        const float px = hw - fabsf(b.x - cx), py = hh - fabsf(b.y - cy);
        if (px < py) {
            nx = b.x < cx ? -1.0f : 1.0f;
            ny = 0.0f;
            b.x = cx + nx * (hw + kBallR);
        } else {
            nx = 0.0f;
            ny = b.y < cy ? -1.0f : 1.0f;
            b.y = cy + ny * (hh + kBallR);
        }
    } else {
        d = sqrtf(d2);
        nx = dx / d;
        ny = dy / d;
        b.x = qx + nx * kBallR;
        b.y = qy + ny * kBallR;
    }
    const float vn = b.vx * nx + b.vy * ny;
    if (vn < 0.0f) {
        b.vx -= (1.0f + e) * vn * nx;
        b.vy -= (1.0f + e) * vn * ny;
    }
}

void Game::collideFlipper(Ball& b, int cell) {
    float px, py, sg;
    flipperPivot(cell, px, py, sg);
    const float a = ang_[cell];
    const float ca = cosf(a), sa = sinf(a);
    const float tx = px + sg * kFlipLen * ca, ty = py + kFlipLen * sa;
    const float ex = tx - px, ey = ty - py;
    float u = ((b.x - px) * ex + (b.y - py) * ey) / (kFlipLen * kFlipLen);
    u = clamp(u, 0.0f, 1.0f);
    const float qx = px + ex * u, qy = py + ey * u;
    const float dx = b.x - qx, dy = b.y - qy, R = kFlipR + kBallR;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= R * R) return;
    float nx = 0.0f, ny = -1.0f;
    if (d2 > 1e-6f) {
        const float d = sqrtf(d2);
        nx = dx / d;
        ny = dy / d;
    }
    b.x = qx + nx * R;
    b.y = qy + ny * R;
    // surface velocity of the contact point (rotation about the pivot)
    const float w = (ang_[cell] - angPrev_[cell]) * (44100.0f / 128.0f);
    const float rx = qx - px, ry = qy - py;
    float sx, sy;
    if (sg > 0.0f) {
        sx = -w * ry;
        sy = w * rx;
    } else {
        sx = w * ry;
        sy = -w * rx;
    }
    const float rvx = b.vx - sx, rvy = b.vy - sy;
    const float vn = rvx * nx + rvy * ny;
    if (vn < 0.0f) {
        b.vx -= (1.0f + 0.25f) * vn * nx;
        b.vy -= (1.0f + 0.25f) * vn * ny;
        if (fabsf(w) > 6.0f && w < 0.0f && bumpCool_[cell] <= 0.0f) physHit(b, cell);
    }
}

void Game::physHit(Ball& b, int cell) {
    if (bumpCool_[cell] > 0.0f) return;
    bumpCool_[cell] = 0.12f;
    b.capCell = uint8_t(cell);
    b.jump = true;
}

void Game::physBall(Ball& b, float h) {
    const Table& t = table();
    if (b.captured) {
        b.capT -= h;
        if (b.capT <= 0.0f) {
            b.captured = false;
            b.vx = rng_.bi() * 160.0f;
            b.vy = 260.0f;
            b.y += kHoleR;
        }
        return;
    }
    const float g = 150.0f + 50.0f * float(t.gravity);
    b.vy += g * h;
    if (magnet_ >= 0) {
        const float dx = cellX(magnet_) - b.x, dy = cellY(magnet_) - b.y;
        const float d = sqrtf(dx * dx + dy * dy) + 20.0f;
        b.vx += dx / d * 1400.0f * h;
        b.vy += dy / d * 1400.0f * h;
    }
    const float sp2 = b.vx * b.vx + b.vy * b.vy;
    if (sp2 > kMaxSpeed * kMaxSpeed) {
        const float k = kMaxSpeed / sqrtf(sp2);
        b.vx *= k;
        b.vy *= k;
    }
    b.x += b.vx * h;
    b.y += b.vy * h;
    // table edges
    if (b.x < kBallR) {
        if (t.sides) b.x += kFieldW - 2.0f * kBallR;
        else {
            b.x = kBallR;
            b.vx = fabsf(b.vx) * 0.75f;
        }
    } else if (b.x > kFieldW - kBallR) {
        if (t.sides) b.x -= kFieldW - 2.0f * kBallR;
        else {
            b.x = kFieldW - kBallR;
            b.vx = -fabsf(b.vx) * 0.75f;
        }
    }
    if (b.y < kBallR) {
        b.y = kBallR;
        b.vy = fabsf(b.vy) * 0.7f;
    }
    for (int i = 0; i < segCount_; ++i) collideSeg(b, seg_[i].x0, seg_[i].y0, seg_[i].x1, seg_[i].y1, 1.5f, 0.35f);
    // elements in the 3 x 3 cells around the ball
    const int bc = cellFromXY(b.x, b.y);
    const int c0 = cellCol(bc), r0 = cellRow(bc);
    for (int r = r0 - 1; r <= r0 + 1; ++r) {
        if (r < 0 || r >= kRows) continue;
        for (int c = c0 - 1; c <= c0 + 1; ++c) {
            if (c < 0 || c >= kCols) continue;
            const int cell = cellAt(c, r);
            const Shape& s = shape_[cell];
            switch (s.kind) {
                case 1: {   // bumper
                    const float R = s.a + kBallR;
                    if (sq(b.x - s.x) + sq(b.y - s.y) < R * R) {
                        collideCircle(b, s.x, s.y, s.a, 420.0f, 0.5f);
                        physHit(b, cell);
                    }
                    break;
                }
                case 2: {   // target / block
                    const bool target = t.cells[cell].type == kTarget;
                    const float qx = clamp(b.x, s.x - s.a, s.x + s.a), qy = clamp(b.y, s.y - s.b, s.y + s.b);
                    if (sq(b.x - qx) + sq(b.y - qy) < kBallR * kBallR) {
                        collideRect(b, s.x, s.y, s.a, s.b, target ? 0.55f : 0.4f);
                        if (target) physHit(b, cell);
                    }
                    break;
                }
                case 3: {   // hole: a slow ball sinks
                    const float d2 = sq(b.x - s.x) + sq(b.y - s.y);
                    if (d2 < s.a * s.a && bumpCool_[cell] <= 0.0f) {
                        if (b.vx * b.vx + b.vy * b.vy < 520.0f * 520.0f) {
                            b.captured = true;
                            b.capT = float(t.cells[cell].len) * stepSec_;
                            b.x = s.x;
                            b.y = s.y;
                            b.vx = b.vy = 0.0f;
                            physHit(b, cell);
                            bumpCool_[cell] = b.capT + 0.5f;
                        }
                    }
                    break;
                }
                case 4: collideFlipper(b, cell); break;
                case 6: {   // ramp: passing its centre throws the ball along the ramp
                    const float d2 = sq(b.x - s.x) + sq(b.y - s.y);
                    if (d2 < 16.0f * 16.0f && bumpCool_[cell] <= 0.0f) {
                        const uint8_t dir = t.cells[cell].dir;
                        if (dir < kDirCount) {
                            const float l = (dir & 1) ? 0.7071f : 1.0f;
                            const float sp = fmaxf(sqrtf(b.vx * b.vx + b.vy * b.vy), 560.0f);
                            b.vx = float(dirDx(dir)) * l * sp;
                            b.vy = float(dirDy(dir)) * l * sp;
                        }
                        physHit(b, cell);
                        bumpCool_[cell] = 0.35f;
                    }
                    break;
                }
                default: break;
            }
        }
    }
    // the plunger lane: a ball rolling back down is launched again
    if (t.launchCol == kCols - 1 && b.x > kLaneX && b.y > kFieldH - kBallR - 3.0f && b.vy > 0.0f) {
        b.y = kFieldH - kBallR - 3.0f;
        b.vy = -(700.0f + rng_.uni() * 200.0f);
        b.vx = -rng_.uni() * 30.0f;
    }
    if (!finite(b.x) || !finite(b.y) || !finite(b.vx) || !finite(b.vy)) {
        b.x = cellX(cellAt(t.launchCol, 0));
        b.y = kFieldH - 20.0f;
        b.vx = 0.0f;
        b.vy = -800.0f;
    }
    // stuck balls get a nudge
    if (b.vx * b.vx + b.vy * b.vy < 20.0f * 20.0f) {
        b.stuckT += h;
        if (b.stuckT > 4.0f) {
            b.stuckT = 0.0f;
            b.vy = -380.0f;
            b.vx = rng_.bi() * 200.0f;
        }
    } else {
        b.stuckT = 0.0f;
    }
}

void Game::autoFlip(float dt) {
    for (int s = 0; s < 2; ++s) {
        if (autoDelay_[s] > 0.0f) {
            autoDelay_[s] -= dt;
            if (autoDelay_[s] <= 0.0f) {
                autoHold_[s] = 0.16f;
                autoCool_[s] = 0.32f;
            }
        }
        if (autoCool_[s] > 0.0f) autoCool_[s] -= dt;
    }
    if (!autoActive() || mode_ != kHybrid) return;
    const float skill = float(m_->g.autoSkill) * 0.01f;
    const Table& t = table();
    for (int cell = 0; cell < kCells; ++cell) {
        if (t.cells[cell].type != kFlipper) continue;
        const int s = t.cells[cell].dir ? 1 : 0;
        if (autoCool_[s] > 0.0f || autoDelay_[s] > 0.0f || autoHold_[s] > 0.0f || flipHeld_[s]) continue;
        float px, py, sg;
        flipperPivot(cell, px, py, sg);
        for (const auto& b : balls) {
            if (!b.active || !b.phys || b.captured) continue;
            const float along = (b.x - px) * sg, dy = b.y - py;
            if (along < 4.0f || along > kFlipLen + 6.0f || dy < -30.0f || dy > 18.0f || b.vy < -60.0f) continue;
            // aim: a good player waits for the ball to reach the middle of the flipper
            const float want = kFlipLen * (0.35f + 0.3f * skill);
            if (along > want + 10.0f * (1.0f - skill) && b.vy > 40.0f && along < kFlipLen * 0.8f) continue;
            if (rng_.uni() > 0.25f + 0.75f * skill) {
                autoCool_[s] = 0.4f;   // missed this one
                break;
            }
            autoDelay_[s] = 0.001f + (1.0f - skill) * 0.06f * rng_.uni();
            break;
        }
    }
}

void Game::tick(float dt) {
    if (!m_) return;
    idleT_ += dt;
    const Table& t = table();
    if (lastMode_ != mode_) {
        lastMode_ = mode_;
        for (auto& b : balls) {
            if (!b.active) continue;
            b.phys = mode_ == kHybrid;
            b.x = cellX(b.cell);
            b.y = cellY(b.cell);
            b.vx = b.vy = 0.0f;
            b.captured = false;
            b.jump = false;
        }
    }
    for (int s = 0; s < 2; ++s) {
        if (autoHold_[s] > 0.0f) autoHold_[s] -= dt;
        if (flipVis_[s] > 0.0f) flipVis_[s] -= dt;
    }
    for (int c = 0; c < kCells; ++c) {
        if (bumpCool_[c] > 0.0f) bumpCool_[c] -= dt;
        if (t.cells[c].type != kFlipper) continue;
        const int side = t.cells[c].dir ? 1 : 0;
        const float target = flipperUp(side) ? kFlipUp : kFlipRest;
        angPrev_[c] = ang_[c];
        const float speed = target < ang_[c] ? 26.0f : 11.0f;
        const float step = speed * dt;
        if (ang_[c] > target) ang_[c] = ang_[c] - step < target ? target : ang_[c] - step;
        else ang_[c] = ang_[c] + step > target ? target : ang_[c] + step;
    }
    if (mode_ != kHybrid || replay_ || stopped_) return;
    autoFlip(dt);
    constexpr int kSub = 3;
    const float h = dt / float(kSub);
    for (int k = 0; k < kSub; ++k) {
        for (int i = 0; i < kMaxBalls; ++i) {
            Ball& b = balls[i];
            if (!b.active || !b.phys) continue;
            physBall(b, h);
        }
        // ball against ball
        for (int i = 0; i < kMaxBalls; ++i)
            for (int j = i + 1; j < kMaxBalls; ++j) {
                Ball &a = balls[i], &b = balls[j];
                if (!a.active || !b.active || !a.phys || !b.phys || a.captured || b.captured) continue;
                const float dx = b.x - a.x, dy = b.y - a.y, d2 = dx * dx + dy * dy;
                if (d2 >= 4.0f * kBallR * kBallR || d2 < 1e-6f) continue;
                const float d = sqrtf(d2), nx = dx / d, ny = dy / d, o = (2.0f * kBallR - d) * 0.5f;
                a.x -= nx * o;
                a.y -= ny * o;
                b.x += nx * o;
                b.y += ny * o;
                const float vn = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny;
                if (vn < 0.0f) {
                    a.vx += vn * nx;
                    a.vy += vn * ny;
                    b.vx -= vn * nx;
                    b.vy -= vn * ny;
                }
            }
    }
    for (int i = 0; i < kMaxBalls; ++i) {
        Ball& b = balls[i];
        if (b.active && b.phys && b.y > kFieldH + kBallR) drain(i);
    }
}

}  // namespace flip
