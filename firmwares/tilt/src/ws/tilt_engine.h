// TILT -- the engine: Pinball 2's ball walking a 17 x 17 Markov matrix, with
// TILT's table around it (flippers, bumpers, ramps, combos, multiball, tilt,
// ball save, gravity, edge properties, memory, mutation, meta pages, melody,
// rules) and the capture of what it played.
//
// The engine knows nothing about samples or the display. The caller calls
// tick(beat) once per move (the move grid is kRates[table.rate] quarter notes)
// and turns the hits of that move into sound and MIDI. Times inside a move are
// in beats after the move, so the engine runs the same in the audio interrupt,
// in tests and in the bench.
//
// Differences from the Max device (Pinball.h): the ball stays on the move grid
// (a step's delay only moves that step's note, it does not push the next move),
// and a drawn D (column 0) is a drain handled by Table::drain / ball save.
#pragma once

#include <cstdint>

#include "ws/tilt_model.h"

namespace tilt {

// Beat arithmetic without the double libm (the image must not link floor / fmod).
inline double floorD(double x) {
    const double t = double(int64_t(x));
    return t > x ? t - 1.0 : t;
}
inline double ceilD(double x) {
    const double t = double(int64_t(x));
    return t < x ? t + 1.0 : t;
}
inline double fmodD(double x, double y) { return x - floorD(x / y) * y; }

struct Hit {
    enum Kind : uint8_t { kNote, kChoke, kCc };
    uint8_t kind = kNote;
    uint8_t ch = 0;          // kNote: channel 0..7
    uint8_t note = 60;       // kCc: controller number
    uint8_t vel = 100;       // kCc: value
    uint8_t ball = 0;
    uint8_t step = 0;        // 1..16 (0: not from a step)
    uint8_t chokeMask = 0;   // kChoke: channels to cut
    uint8_t pad = 0;
    float at = 0.0f;         // beats after the move
    float len = 0.25f;       // beats
};

enum EngineEvent : uint8_t {
    kEvNone, kEvLaunch, kEvDrain, kEvSave, kEvTilt, kEvMultiball, kEvCombo1, kEvCombo2, kEvCombo3, kEvCombo4,
    kEvScene, kEvHalt, kEvFill, kEvShake, kEvAnchor, kEvGravity, kEvFlip, kEvRamp, kEvCapture, kEvBallLost
};

struct Ball {
    bool active = false;
    bool resting = false;     // drained (REST): one silent move, then from S
    uint8_t state = 0;        // 0 = S, 1..16
    uint8_t prev = 0;         // the state it came from
    uint8_t forced = 0;       // a flip queued: 1..16
    uint8_t repeats = 0;      // moves in a row on the same step
    uint16_t moves = 0;
    int16_t life = -1;        // extra balls: moves left (-1 = the main ball)
    uint8_t hist[kComboLen] = {};   // last landed steps (oldest first)
    uint8_t histN = 0;
    static constexpr int kTrail = 8;
    uint8_t trail[kTrail] = {};     // last states, newest at trail[0]
    float lastMove = 0.0f;          // beat of the last move (screen animation)
    double launchBeat = 0.0;
};

struct CapEvent {
    uint32_t t96 = 0;     // 1/96 quarter notes since Play
    uint16_t len96 = 24;
    uint8_t ch = 0, note = 60, vel = 100, pad[3] = {};
};

class Engine {
public:
    static constexpr int kMaxHits = 72, kMaxEvents = 16;
    static constexpr int kCapRing = 2048, kClipCap = 1024;

    void attach(Pattern* p) { p_ = p; }
    Pattern* pattern() { return p_; }
    void seed(uint32_t s) { rng_.seed(s); }
    void reset();                 // forget memory, mutation, heat, balls (keeps the capture)
    double moveBeats() const;

    // ---- the walk ----------------------------------------------------------------
    // Manual Reset: extra balls gone, the main ball at state start-1; its first
    // move is drawn at the next tick (from that row, like the device).
    void launch(double beat);
    void halt();                  // "Hault": every ball stops until the next launch
    // One move at `beat` (on the move grid). Fills hits / events.
    void tick(double beat);

    // ---- the player --------------------------------------------------------------
    // The next move of the main ball goes to `step` (1..16). Counts flips for
    // tilt; a flip this hard (Table::tiltVel) tilts. Relaunches a held ball.
    void flip(int step, int vel = 100);
    void flipper(int i, int vel = 100) { if (p_ && i >= 0 && i < kFlips) flip(p_->flips[i].step, vel); }
    void nudge(int vel);          // one random kick; hard = tilt; counts as a flip
    void addBall();               // multiball: split the main ball
    void fireCombo(int i);        // run a combo's action now
    void setPage(int pg);
    void tilt();                  // TILT now

    // ---- capture / replay --------------------------------------------------------
    // The last `bars` complete bars before `nowBeat` become the clip. Returns its events.
    int capture(int bars, double nowBeat);
    void setReplay(bool on) { replay_ = on && clipN_ > 0; }
    bool replaying() const { return replay_; }
    int clipCount() const { return clipN_; }
    int clipBars() const { return clipBars_; }
    const CapEvent* clip() const { return clip_; }
    uint32_t clipStart96() const { return clipStart96_; }
    void clearCapture() { capHead_ = 0; capCount_ = 0; }
    int capturedEvents() const { return capCount_; }

    // ---- output of the last tick -----------------------------------------------
    Hit hits[kMaxHits];
    int nHits = 0;
    uint8_t events[kMaxEvents] = {};
    int nEvents = 0;

    // ---- state (the screen reads it) -------------------------------------------
    Ball balls[kBalls];
    float heat[kSteps] = {};
    float odds[kStates] = {};     // next-move probabilities of the main ball (column order: D, 1..16)
    float entropy = 0.0f;         // 0..1 (bits / log2(17))
    uint8_t lastFrom = 0, lastTo = 0;   // the main ball's last edge
    uint8_t comboProgress[kCombos] = {};
    int flipsBar = 0;
    int tiltLeft = 0;             // moves of TILT left (flippers dead)
    int shakeLeft = 0, fillLeft = 0, fillHits = 0;
    int saves = 0, drains = 0;
    uint32_t moveCount = 0;
    float mem[kStates][kStates];
    float mut[kPages][kStates][kStates];
    int ballsActive() const;
    bool tilted() const { return tiltLeft > 0; }
    bool freezing() const;

private:
    void onBar(int64_t bar, double beat);
    void mutateRows(int rows);
    void moveBall(int b, double beat, int moveIndex);
    int chooseNext(Ball& ball, double beat);
    void rowWeights(const Ball& ball, float* out) const;   // effective weights of the ball's row
    void land(int b, int to, double beat, int moveIndex);
    void fireStep(int b, int step, const Edge& edge, int moveIndex);
    void drained(int b, double beat, int moveIndex);
    void checkCombos(int b);
    void computeOdds();
    void event(uint8_t e);
    void addHit(const Hit& h);
    void record(const Hit& h, double beat);
    void replayHits(double beat);
    int noteFor(int ch, int degree, int transpose);

    Pattern* p_ = nullptr;
    Rng32 rng_;
    int64_t lastBar_ = -1;
    int64_t barsSinceLaunch_ = 0;
    double tickBeat_ = 0.0;
    bool replay_ = false;
    bool memActive_ = false;
    uint8_t mutPage_ = 0, mutRow_ = kStates;
    // capture ring + clip
    CapEvent cap_[kCapRing];
    int capHead_ = 0, capCount_ = 0;
    CapEvent clip_[kClipCap];
    int clipN_ = 0, clipBars_ = 0;
    uint32_t clipStart96_ = 0;
};

}  // namespace tilt
