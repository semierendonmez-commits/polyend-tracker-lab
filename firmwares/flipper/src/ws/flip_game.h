// FLIPPER -- the game: balls on a table, stepped on the tempo (Markov) and,
// in hybrid mode, also simulated with real physics. Pure logic: no I/O, no
// allocation; the workstation drives it from the audio interrupt (step() on
// every musical step, tick() once per audio block) and plays what it emits.
#pragma once

#include <cstdint>

#include "lab/dsp.h"
#include "ws/flip_model.h"

namespace flip {

// Playfield geometry in screen pixels (the physics works in the same units).
constexpr float kFieldW = 600.0f, kFieldH = 380.0f;
constexpr float kCellW = kFieldW / kCols, kCellH = kFieldH / kRows;
constexpr float kBallR = 7.0f, kBumperR = 17.0f, kHoleR = 11.0f;
constexpr float kFlipLen = 38.0f, kFlipR = 5.0f, kFlipRest = 0.5f, kFlipUp = -0.55f;
constexpr float kMaxSpeed = 1100.0f;

inline float cellX(int c) { return (float(cellCol(c)) + 0.5f) * kCellW; }
inline float cellY(int c) { return (float(cellRow(c)) + 0.5f) * kCellH; }
int cellFromXY(float x, float y);

enum Mode : uint8_t { kHybrid = 0, kMarkov = 1 };

struct NoteOut {
    uint8_t inst = 0, note = 60, vel = 100, ball = kNone, cell = kNone;
    bool ghost = false;
};
struct FxOut {
    uint8_t fx = 0, groups = 7, amount = 50, steps = 8;
};
enum Banner : uint8_t {
    kBnNone, kBnMultiball, kBnJackpot, kBnTilt, kBnTiltWarn, kBnBallSave, kBnDrain, kBnExtraBall, kBnMultiplier,
    kBnBank, kBnMutate, kBnTable, kBnGameOver, kBnLaunch, kBnCount
};
extern const char* const kBannerText[kBnCount];

struct Ball {
    bool active = false;
    uint8_t cell = 0, prevCell = 0;
    uint8_t lastDir = kNone;      // 0..7 of the last move
    uint8_t forcedDir = 0, forcedSteps = 0;
    uint8_t hold = 0;             // steps left in a hole
    uint8_t momentum = 0;         // consecutive moves in one direction
    bool bounce = false;          // hit a bumper on the last step
    bool onFlipper = false;
    int16_t save = 0;             // ball-save steps left
    uint32_t movedAt = 0;         // step count of the last move (UI tween)
    // physics (hybrid)
    bool phys = false, captured = false;
    float x = 0, y = 0, vx = 0, vy = 0;
    float capT = 0, stuckT = 0;
    uint8_t capCell = kNone;
    bool jump = false;            // the grid ball jumps to capCell on the next step
    // what this ball played on the last step (capture)
    uint8_t lastNote = 0, lastVel = 0, lastInst = kNone;
};

// Pure distribution of the next grid move from `cell`: out[0..3] = N E S W,
// out[4] = stay. `attract` (or -1) pulls with factor `pull` (0..1).
// drainOk: a move off the bottom edge counts (Markov mode) as a possible move.
void moveWeights(const Table& t, int cell, int attract, float pull, bool drainOk, float out[5]);
// Entropy in bits of a distribution of n weights.
float entropyBits(const float* w, int n);
// Destination of a move; -1 = blocked, -2 = drain (off the bottom edge).
int moveDest(const Table& t, int cell, int dir, bool drainOk);

class Game {
public:
    static constexpr int kNoteCap = 40, kFxCap = 8, kBannerCap = 8;

    void init(Machine* m, uint32_t seed);
    void setMachine(Machine* m) { m_ = m; }
    Machine* machine() const { return m_; }
    const Table& table() const { return m_->tab[tab_]; }
    int tableIndex() const { return tab_; }

    void newGame(int table);
    void stopGame();     // all balls gone, no auto launch
    void rebuild();      // physics geometry from the current table

    // player input
    void flip(int side, bool down, uint8_t vel);
    void launch();
    void tilt();
    void setMagnet(int cell) { magnet_ = cell < 0 || cell >= kCells ? -1 : cell; }
    void dropBall(int cell);
    void gotoTable(int t, bool now);
    void setReplay(bool on);
    bool replaying() const { return replay_; }
    void setClipLoop(uint32_t a, uint32_t b) { clipA_ = a; clipB_ = b; }
    void setReplayLive(bool on) { replayLive_ = on; }
    bool running() const { return !stopped_; }
    int segments() const { return segCount_; }
    void segment(int i, float& x0, float& y0, float& x1, float& y1) const {
        x0 = seg_[i].x0; y0 = seg_[i].y0; x1 = seg_[i].x1; y1 = seg_[i].y1;
    }
    void flipperGeom(int cell, float& px, float& py, float& tx, float& ty) const;
    void markPlayer() { idleT_ = 0.0f; }
    void setStepSeconds(float s) { stepSec_ = s > 0.01f ? s : 0.01f; }
    void setMode(int mode) { mode_ = mode; }
    int mode() const { return mode_; }

    // time
    void step();            // one musical step
    void tick(float dt);    // per audio block: physics, flippers, timers

    // outputs of the last step() / tick() calls (clearOut() empties them)
    NoteOut notes[kNoteCap];
    int noteCount = 0;
    FxOut fx[kFxCap];
    int fxCount = 0;
    uint8_t banners[kBannerCap] = {};
    int bannerCount = 0;
    bool switched = false;  // step() moved to another table on this step
    void clearOut() { noteCount = fxCount = bannerCount = 0; switched = false; }

    // run recording (capture to clip): every step is written to `buf` (a ring)
    void attachRun(ClipStep* buf, uint32_t cap) { run_ = buf; runCap_ = cap; }
    uint32_t runCount() const { return runCount_; }
    uint32_t runGameStart() const { return runGameStart_; }

    // state for the UI (read-only)
    Ball balls[kMaxBalls];
    int ballsInPlay() const;
    int ballsLeft() const { return ballsLeft_; }
    uint32_t score() const { return score_; }
    uint32_t tableScore() const { return tableScore_; }
    int multiplier() const { return mult_; }
    int combo() const { return combo_; }
    int heat() const { return heat_; }
    float heatLevel() const { return heatAcc_; }
    int tiltMeter() const { return tiltMeter_; }
    bool tiltLocked() const { return tiltLock_ > 0; }
    bool autoActive() const;
    int pendingTable() const { return pending_; }
    uint32_t stepIndex() const { return stepIdx_; }
    uint32_t steps() const { return stepCount_; }
    int bar() const;
    int stepsPerBar() const;
    bool gameOver() const { return over_; }
    bool lit(int cell) const { return lit_[cell] != 0; }
    float flipperAngle(int cell) const { return ang_[cell]; }
    bool flipperUp(int side) const;
    int magnet() const { return magnet_; }
    uint32_t hitStep(int cell) const { return hitAt_[cell]; }
    uint32_t visits(int cell) const { return visit_[cell]; }
    uint32_t clipPos() const { return clipPos_; }
    int launchWait() const { return launchWait_; }
    float fxActiveUntil(int cell) const { return float(fxUntil_[cell]); }

private:
    struct Shape {
        uint8_t kind = 0;   // 0 none, 1 circle (bumper), 2 rect, 3 hole, 4 flipper, 5 segment, 6 ramp
        float x = 0, y = 0, a = 0, b = 0;   // centre / half sizes / radius
        int8_t seg = -1;
    };
    struct Seg {
        float x0, y0, x1, y1;
    };

    // steps
    void stepMarkov(int bi);
    void stepHybrid(int bi);
    void moveTo(int bi, int dest, int dir);
    void hit(int bi, int cell);
    void event(int bi, int cell, uint8_t ev, uint8_t arg);
    void rails();
    void replayStep();
    void record();
    void switchTable(int t);
    int pickGraph();
    void doAction(uint8_t act, uint8_t arg);
    void addScore(uint32_t pts);
    void addBalls(int n, int fromCell);
    void spawn(int bi, int cell, bool fromLaunch);
    void drain(int bi);
    void checkOver();
    void note(int inst, int note, int vel, int ball, int cell, bool ghost);
    void banner(uint8_t b);
    int pitchFor(const Cell& c, int cell, int ball);
    int velFor(const Cell& c, int ball);
    bool flipActiveMarkov(int side, float& power);
    int freeSlot() const;
    void fixBallCell(Ball& b);

    // physics
    void physBall(Ball& b, float dt);
    void collideCircle(Ball& b, float cx, float cy, float r, float kick, float e);
    bool collideSeg(Ball& b, float x0, float y0, float x1, float y1, float r, float e);
    void collideRect(Ball& b, float cx, float cy, float hw, float hh, float e);
    void collideFlipper(Ball& b, int cell);
    void physHit(Ball& b, int cell);
    void autoFlip(float dt);
    void flipperPivot(int cell, float& px, float& py, float& dirSign) const;

    Machine* m_ = nullptr;
    lab::dsp::Rng rng_;
    int tab_ = 0;
    int pending_ = -1;
    bool pendingNow_ = false;
    uint32_t stepIdx_ = 0, stepCount_ = 0;
    int ballsLeft_ = 3;
    uint32_t score_ = 0, tableScore_ = 0;
    int mult_ = 1, combo_ = 0, comboIdle_ = 0;
    float heatAcc_ = 0.0f;
    int heat_ = 0;
    int tiltMeter_ = 0, tiltLock_ = 0;
    int magnet_ = -1;
    bool flipHeld_[2] = {}, flipPrev_[2] = {};
    int flipArmed_[2] = {};
    uint8_t flipVel_[2] = {100, 100};
    float flipVis_[2] = {};
    float autoHold_[2] = {}, autoDelay_[2] = {}, autoCool_[2] = {};
    float idleT_ = 100.0f;
    bool jackpotDone_ = false, over_ = false, launchQueued_ = false, stopped_ = true;
    int launchWait_ = 0;
    uint8_t lit_[kCells] = {};
    int8_t walk_[kCells] = {};
    uint32_t hitAt_[kCells] = {};
    uint32_t visit_[kCells] = {};
    uint32_t fxUntil_[kCells] = {};
    float ang_[kCells] = {}, angPrev_[kCells] = {};
    float bumpCool_[kCells] = {};
    Shape shape_[kCells];
    Seg seg_[kCells];
    int segCount_ = 0;
    int lastMode_ = -1;
    bool replay_ = false, pendingNew_ = false, replayLive_ = false;
    uint32_t clipPos_ = 0, clipA_ = 0, clipB_ = 0;
    int mode_ = kHybrid;
    float stepSec_ = 0.125f;
    int flipCell_[2] = {-1, -1};
    uint8_t stepFx_ = kNone;
    bool tiltReq_ = false;
    ClipStep* run_ = nullptr;
    uint32_t runCap_ = 0, runCount_ = 0, runGameStart_ = 0;
};

}  // namespace flip
