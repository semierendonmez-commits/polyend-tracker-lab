// TILT -- the engine (tilt_engine.h).
#include "ws/tilt_engine.h"

#include <cmath>
#include <cstring>

namespace tilt {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Fast log2 for the entropy (exponent + a quadratic on the mantissa, ~0.01 error).
inline float log2f_(float x) {
    union {
        float f;
        uint32_t i;
    } u{x};
    const float e = float(int32_t((u.i >> 23) & 0xFF) - 128);
    u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;   // mantissa in [1, 2)
    const float m = u.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;   // = log2(m) + 1
}
}  // namespace

double Engine::moveBeats() const {
    const int r = p_ ? clampi(p_->table.rate, 0, kRateCount - 1) : 2;
    return double(kRates[r].beats);
}

void Engine::reset() {
    for (auto& b : balls) b = Ball();
    for (auto& h : heat) h = 0.0f;
    for (int r = 0; r < kStates; ++r)
        for (int c = 0; c < kStates; ++c) mem[r][c] = 1.0f;
    memActive_ = false;
    std::memset(mut, 0, sizeof mut);
    for (auto& o : odds) o = 0.0f;
    entropy = 0.0f;
    flipsBar = tiltLeft = shakeLeft = fillLeft = fillHits = 0;
    saves = drains = 0;
    moveCount = 0;
    lastBar_ = -1;
    barsSinceLaunch_ = 0;
    nHits = nEvents = 0;
    lastFrom = lastTo = 0;
    for (auto& c : comboProgress) c = 0;
}

int Engine::ballsActive() const {
    int n = 0;
    for (const auto& b : balls) n += b.active ? 1 : 0;
    return n;
}

bool Engine::freezing() const {
    if (!p_ || tiltLeft <= 0) return false;
    const int a = p_->table.tiltAction;
    if (a == 0) return true;
    if (a == 2) {   // BOTH: shake for the first half, then freeze
        const int n = int(double(clampi(p_->table.tiltBeats, 1, 16)) / moveBeats() + 0.5);
        return tiltLeft <= n / 2;
    }
    return false;
}

void Engine::event(uint8_t e) {
    if (nEvents < kMaxEvents) events[nEvents++] = e;
}

void Engine::addHit(const Hit& h) {
    if (nHits < kMaxHits) hits[nHits++] = h;
}

void Engine::launch(double beat) {
    if (!p_) return;
    for (int i = 1; i < kBalls; ++i) balls[i].active = false;
    Ball& b = balls[0];
    b = Ball();
    b.active = true;
    b.state = uint8_t(clampi(p_->table.startStep - 1, 0, kSteps - 1));
    b.launchBeat = beat;
    b.lastMove = float(beat);
    barsSinceLaunch_ = 0;
    tiltLeft = shakeLeft = fillLeft = 0;
    flipsBar = 0;
    event(kEvLaunch);
}

void Engine::halt() {
    for (auto& b : balls) b.active = false;
    event(kEvHalt);
}

void Engine::setPage(int pg) {
    if (p_) p_->ev.page = uint8_t(clampi(pg, 0, kPages - 1));
}

void Engine::tilt() {
    if (!p_) return;
    const int n = int(double(clampi(p_->table.tiltBeats, 1, 16)) / moveBeats() + 0.5);
    tiltLeft = n < 1 ? 1 : n;
    for (auto& b : balls) b.forced = 0;
    event(kEvTilt);
}

void Engine::flip(int step, int vel) {
    if (!p_ || step < 1 || step > kSteps) return;
    if (tiltLeft > 0) return;   // flippers are dead while tilted
    ++flipsBar;
    const Table& t = p_->table;
    if ((t.tiltVel && vel >= t.tiltVel) || (t.tiltFlips && flipsBar > t.tiltFlips)) {
        tilt();
        return;
    }
    Ball& b = balls[0];
    if (!b.active) {   // a held / halted ball comes back on a flip
        b.active = true;
        b.resting = false;
        b.state = 0;
        b.launchBeat = tickBeat_;
    }
    b.forced = uint8_t(step);
    event(kEvFlip);
}

void Engine::nudge(int vel) {
    if (!p_ || tiltLeft > 0) return;
    ++flipsBar;
    const Table& t = p_->table;
    if ((t.tiltVel && vel >= t.tiltVel) || (t.tiltFlips && flipsBar > t.tiltFlips)) {
        tilt();
        return;
    }
    if (shakeLeft < 1) shakeLeft = 1;
    event(kEvShake);
}

void Engine::addBall() {
    if (!p_) return;
    const int maxB = clampi(p_->table.mbMax, 1, kBalls);
    if (ballsActive() >= maxB) return;
    for (int i = 1; i < kBalls; ++i)
        if (!balls[i].active) {
            Ball& b = balls[i];
            b = Ball();
            b.active = true;
            // split off the main ball (or from S when the main ball is not in play)
            b.state = balls[0].active ? balls[0].state : 0;
            b.prev = b.state;
            b.life = int16_t(clampi(p_->table.mbLife, 4, 64));
            b.launchBeat = tickBeat_;
            b.lastMove = float(tickBeat_);
            event(kEvMultiball);
            return;
        }
}

void Engine::fireCombo(int i) {
    if (!p_ || i < 0 || i >= kCombos) return;
    const Combo& c = p_->combos[i];
    event(uint8_t(kEvCombo1 + i));
    switch (c.action) {
        case kCaMultiball:
            for (int k = 0; k < clampi(c.a, 1, 3); ++k) addBall();
            break;
        case kCaFill:
            fillLeft = clampi(c.a, 1, 32);
            fillHits = clampi(c.b, 2, 4);
            event(kEvFill);
            break;
        case kCaNote: {
            Hit h;
            h.ch = uint8_t(clampi(c.b, 1, 8) - 1);
            h.note = uint8_t(clampi(c.a, 0, 127));
            h.vel = 110;
            h.len = float(moveBeats());
            addHit(h);
            record(h, tickBeat_);
            break;
        }
        case kCaCc: {
            Hit h;
            h.kind = Hit::kCc;
            h.note = uint8_t(clampi(c.a, 0, 127));
            h.vel = uint8_t(clampi(c.b, 0, 127));
            addHit(h);
            break;
        }
        case kCaShake:
            shakeLeft = clampi(c.a, 1, 32);
            event(kEvShake);
            break;
        case kCaScene:
            setPage(c.a >= kPages ? (p_->ev.page + 1) % kPages : c.a);
            event(kEvScene);
            break;
        default: break;
    }
}

// ---- the move ---------------------------------------------------------------------
void Engine::tick(double beat) {
    nHits = 0;
    nEvents = 0;
    if (!p_) return;
    tickBeat_ = beat;
    const double mb = moveBeats();
    const int moveIndex = int(floorD(beat / mb + 0.5));
    const int64_t bar = int64_t(floorD((beat + 1e-6) / 4.0));
    if (bar != lastBar_) {
        if (lastBar_ >= 0) ++barsSinceLaunch_;
        lastBar_ = bar;
        onBar(bar, beat);
    }
    ++moveCount;
    if (mutRow_ < kStates) mutateRows(5);
    for (int b = 0; b < kBalls; ++b)
        if (balls[b].active) moveBall(b, beat, moveIndex);
    if (tiltLeft > 0) --tiltLeft;
    if (shakeLeft > 0) --shakeLeft;
    if (fillLeft > 0) --fillLeft;
    // memory back to neutral (only while some edge remembers something)
    const float rec = float(p_->ev.recover) * 0.0005f;
    if (rec > 0.0f && memActive_) {
        float dev = 0.0f;
        for (int r = 0; r < kStates; ++r)
            for (int c = 0; c < kStates; ++c) {
                float& m = mem[r][c];
                m += (1.0f - m) * rec;
                const float d = m > 1.0f ? m - 1.0f : 1.0f - m;
                dev = d > dev ? d : dev;
            }
        if (dev < 1e-3f) {
            for (int r = 0; r < kStates; ++r)
                for (int c = 0; c < kStates; ++c) mem[r][c] = 1.0f;
            memActive_ = false;
        }
    }
    for (auto& h : heat) h *= 0.94f;
    if (replay_) replayHits(beat);
    computeOdds();
}

void Engine::onBar(int64_t bar, double beat) {
    flipsBar = 0;
    const Table& t = p_->table;
    // Pattern Length reset (the device's Manual Reset every N bars of the transport)
    if (t.resetBars && bar > 0 && bar % t.resetBars == 0) launch(beat);
    Evolve& ev = p_->ev;
    // meta: the next page, every N bars
    if (ev.metaOn && barsSinceLaunch_ > 0 && barsSinceLaunch_ % clampi(ev.metaBars, 1, 16) == 0) {
        const uint8_t* row = ev.meta[clampi(ev.page, 0, kPages - 1)];
        int total = 0;
        for (int i = 0; i < kPages; ++i) total += row[i];
        if (total > 0) {
            int pick = rng_.below(total);
            for (int i = 0; i < kPages; ++i) {
                pick -= row[i];
                if (pick < 0) {
                    if (i != ev.page) event(kEvScene);
                    ev.page = uint8_t(i);
                    break;
                }
            }
        }
    }
    // mutation: the page's weights drift, and are pulled back home. The rows
    // are worked through over the next moves (a few per move), not all at once.
    mutPage_ = uint8_t(clampi(ev.page, 0, kPages - 1));
    mutRow_ = 0;
}

void Engine::mutateRows(int rows) {
    const Evolve& ev = p_->ev;
    const int pg = mutPage_;
    const float amt = float(ev.mutate) * 0.35f;
    const float home = float(ev.mutHome) * 0.01f;
    for (; rows > 0 && mutRow_ < kStates; --rows, ++mutRow_) {
        if (amt <= 0.0f && home <= 0.0f) continue;
        const int r = mutRow_;
        for (int c = 0; c < kStates; ++c) {
            float& m = mut[pg][r][c];
            const int base = p_->w[pg][r][c];
            if (amt > 0.0f && (base > 0 || m != 0.0f || (ev.mutSpread && rng_.pct(ev.mutSpread / 4))))
                m += (rng_.uni() * 2.0f - 1.0f) * amt;
            m -= m * home;
            m = clampf(m, -float(base), 100.0f - float(base));
            if (m > -0.05f && m < 0.05f) m = 0.0f;
        }
    }
}

void Engine::rowWeights(const Ball& ball, float* out) const {
    const int pg = clampi(p_->ev.page, 0, kPages - 1);
    const int r = clampi(ball.state, 0, kStates - 1);
    float total = 0.0f;
    for (int c = 0; c < kStates; ++c) {
        const float w = clampf(float(p_->w[pg][r][c]) + mut[pg][r][c], 0.0f, 100.0f) * mem[r][c];
        out[c] = w;
        total += w;
    }
    // rules: no bounce back, max repeats (fall back to the plain row if they empty it)
    const Evolve& ev = p_->ev;
    float keep[kStates];
    for (int c = 0; c < kStates; ++c) keep[c] = out[c];
    if (ev.noBounce && ball.prev >= 1 && ball.prev != ball.state) out[ball.prev] = 0.0f;
    if (ev.maxRepeat && ball.state >= 1 && ball.repeats >= ev.maxRepeat) out[ball.state] = 0.0f;
    float t2 = 0.0f;
    for (int c = 0; c < kStates; ++c) t2 += out[c];
    if (t2 <= 0.0f && total > 0.0f)
        for (int c = 0; c < kStates; ++c) out[c] = keep[c];
}

int Engine::chooseNext(Ball& ball, double beat) {
    const int cur = ball.state;
    if (freezing() && cur >= 1) return cur;   // TILT freeze: the ball is stuck, the step repeats
    if (tiltLeft > 0 || shakeLeft > 0) return 1 + rng_.below(kSteps);   // shake: anywhere
    if (&ball == &balls[0] && ball.forced) {
        const int f = ball.forced;
        ball.forced = 0;
        return f;
    }
    if (cur >= 1 && p_->steps[cur - 1].ramp) {
        event(kEvRamp);
        return clampi(p_->steps[cur - 1].ramp, 1, kSteps);
    }
    const Evolve& ev = p_->ev;
    if (ev.anchorStep && ev.anchorEvery && (ball.moves + 1) % ev.anchorEvery == 0) {
        event(kEvAnchor);
        return ev.anchorStep;
    }
    const Table& t = p_->table;
    if (t.gravity) {
        const double grid = t.gravityGrid == 0 ? 4.0 : (t.gravityGrid == 1 ? 2.0 : 1.0);
        const double q = beat / grid;
        if (std::fabs(q - floorD(q + 0.5)) < 1e-4 && rng_.pct(t.gravity)) {
            event(kEvGravity);
            return clampi(t.startStep, 1, kSteps);
        }
    }
    float w[kStates];
    rowWeights(ball, w);
    float total = 0.0f;
    for (int c = 0; c < kStates; ++c) total += w[c];
    if (total <= 0.0f) return -1;   // empty row: the ball stops (the device's behaviour)
    float pick = rng_.uni() * total;
    for (int c = 0; c < kStates; ++c) {
        pick -= w[c];
        if (pick < 0.0f) return c;
    }
    for (int c = kStates - 1; c >= 0; --c)
        if (w[c] > 0.0f) return c;
    return -1;
}

void Engine::moveBall(int b, double beat, int moveIndex) {
    Ball& ball = balls[b];
    ball.lastMove = float(beat);
    if (ball.resting) {   // REST after a drain: silent move, back at S
        ball.resting = false;
        ball.state = 0;
        ball.prev = 0;
        return;
    }
    if (ball.life > 0 && --ball.life == 0) {
        ball.active = false;
        event(kEvBallLost);
        return;
    }
    const int next = chooseNext(ball, beat);
    if (next < 0) {   // empty row
        ball.active = false;
        return;
    }
    if (next == 0) {
        drained(b, beat, moveIndex);
        return;
    }
    land(b, next, beat, moveIndex);
}

void Engine::drained(int b, double beat, int moveIndex) {
    Ball& ball = balls[b];
    ++drains;
    if (b != 0) {   // extra balls are gone
        ball.active = false;
        event(kEvBallLost);
        return;
    }
    const Table& t = p_->table;
    if (t.saveBars && beat - ball.launchBeat < double(t.saveBars) * 4.0) {
        // ball save: straight back in from S, no move lost
        ++saves;
        event(kEvSave);
        ball.prev = ball.state;
        ball.state = 0;
        const int next = chooseNext(ball, beat);
        if (next >= 1) {
            land(b, next, beat, moveIndex);
            return;
        }
    }
    event(kEvDrain);
    lastFrom = ball.state;
    lastTo = 0;
    ball.prev = ball.state;
    ball.state = 0;
    ball.histN = 0;
    for (int i = Ball::kTrail - 1; i > 0; --i) ball.trail[i] = ball.trail[i - 1];
    ball.trail[0] = 0;
    if (t.drain == 0) {
        ball.resting = true;
    } else {
        ball.active = false;   // HOLD: wait for a relaunch (or a flip)
    }
}

void Engine::land(int b, int to, double beat, int moveIndex) {
    (void)beat;
    Ball& ball = balls[b];
    const int from = ball.state;
    const int pg = clampi(p_->ev.page, 0, kPages - 1);
    const Edge edge = p_->e[pg][from][to];
    // memory: the edge gets tired / stronger
    if (p_->ev.fatigue || p_->ev.reinforce) {
        float& m = mem[from][to];
        m *= 1.0f - float(p_->ev.fatigue) * 0.006f;
        m *= 1.0f + float(p_->ev.reinforce) * 0.006f;
        m = clampf(m, 0.05f, 4.0f);
        memActive_ = true;
    }
    ball.repeats = (to == from) ? uint8_t(ball.repeats < 250 ? ball.repeats + 1 : 250) : 0;
    ball.prev = uint8_t(from);
    ball.state = uint8_t(to);
    ++ball.moves;
    if (b == 0) {
        lastFrom = uint8_t(from);
        lastTo = uint8_t(to);
    }
    for (int i = Ball::kTrail - 1; i > 0; --i) ball.trail[i] = ball.trail[i - 1];
    ball.trail[0] = uint8_t(to);
    if (ball.histN < kComboLen) {
        ball.hist[ball.histN++] = uint8_t(to);
    } else {
        for (int i = 0; i < kComboLen - 1; ++i) ball.hist[i] = ball.hist[i + 1];
        ball.hist[kComboLen - 1] = uint8_t(to);
    }
    heat[to - 1] += 1.0f;
    fireStep(b, to, edge, moveIndex);
    checkCombos(b);
}

int Engine::noteFor(int ch, int degree, int transpose) {
    const Channel& c = p_->ch[ch];
    int n = c.note + transpose;
    if (p_->mel.on && c.follow) {
        n += p_->mel.root + scaleSemis(p_->mel.scale, degree);
        if (p_->mel.octUp && rng_.pct(p_->mel.octUp)) n += 12;
    }
    return clampi(n, 0, 127);
}

void Engine::fireStep(int b, int step, const Edge& edge, int moveIndex) {
    const Step& sp = p_->steps[step - 1];
    if (sp.trig == kTrigNone) return;
    if (replay_) return;   // the clip plays instead
    const float mb = float(moveBeats());
    if (sp.trig == kTrigMute) {
        Hit c;
        c.kind = Hit::kChoke;
        c.ball = uint8_t(b);
        c.step = uint8_t(step);
        for (int i = 0; i < kCh; ++i)
            if (p_->ch[i].choke) c.chokeMask |= uint8_t(1u << i);
        addHit(c);
        return;
    }
    if (!rng_.pct(sp.trigP)) return;
    const int ch = clampi(sp.trig - 1, 0, kCh - 1);
    float at = float(edge.dly) * mb / 12.0f;
    if (sp.delay && rng_.pct(sp.delayP)) at += float(sp.delay) * mb / 12.0f;
    if ((moveIndex & 1) && p_->table.swing) at += float(p_->table.swing) * 0.01f * 0.5f * mb;
    float vel = float(clampi(int(sp.vel) + edge.vel, 1, 127));
    if (b != 0) vel *= 0.85f;
    const int note = noteFor(ch, sp.degree, edge.tr);
    // choke group: this channel cuts the others of its group
    const int g = p_->ch[ch].choke;
    if (g) {
        Hit c;
        c.kind = Hit::kChoke;
        c.ball = uint8_t(b);
        c.step = uint8_t(step);
        c.at = at;
        for (int i = 0; i < kCh; ++i)
            if (i != ch && p_->ch[i].choke == g) c.chokeMask |= uint8_t(1u << i);
        if (c.chokeMask) addHit(c);
    }
    int n = 1 + clampi(sp.bumper, 0, 4);
    if (fillLeft > 0 && fillHits > n) n = fillHits;
    const float spacing = mb / float(n);
    float len = float(clampi(sp.len, 1, 64)) * 0.25f * mb;
    if (n > 1 && len > spacing) len = spacing;
    for (int k = 0; k < n; ++k) {
        Hit h;
        h.ch = uint8_t(ch);
        h.note = uint8_t(note);
        h.vel = uint8_t(clampi(int(vel + 0.5f), 1, 127));
        h.ball = uint8_t(b);
        h.step = uint8_t(step);
        h.at = at + spacing * float(k);
        h.len = len;
        addHit(h);
        record(h, tickBeat_);
        vel *= 0.8f;   // bumper hits at 0.8 velocity each
    }
}

void Engine::checkCombos(int b) {
    Ball& ball = balls[b];
    for (int i = 0; i < kCombos; ++i) {
        const Combo& c = p_->combos[i];
        if (!c.on || c.len < 2 || c.len > kComboLen) {
            if (b == 0) comboProgress[i] = 0;
            continue;
        }
        // longest prefix of the path that ends the history
        int best = 0;
        for (int k = c.len; k >= 1; --k) {
            if (k > ball.histN) continue;
            bool ok = true;
            for (int j = 0; j < k && ok; ++j) ok = ball.hist[ball.histN - k + j] == c.path[j];
            if (ok) {
                best = k;
                break;
            }
        }
        if (b == 0) comboProgress[i] = uint8_t(best);
        if (best == c.len) {
            ball.histN = 0;
            if (b == 0) comboProgress[i] = 0;
            fireCombo(i);
            return;
        }
    }
}

void Engine::computeOdds() {
    const Ball& b = balls[0];
    for (auto& o : odds) o = 0.0f;
    entropy = 0.0f;
    if (!b.active) return;
    if (b.resting) {
        odds[0] = 0.0f;
    }
    int forced = 0;
    if (b.forced) forced = b.forced;
    else if (freezing() && b.state >= 1) forced = b.state;
    else if (b.state >= 1 && p_->steps[b.state - 1].ramp) forced = p_->steps[b.state - 1].ramp;
    else if (p_->ev.anchorStep && p_->ev.anchorEvery && (b.moves + 1) % p_->ev.anchorEvery == 0)
        forced = p_->ev.anchorStep;
    if (tiltLeft > 0 && !freezing()) {
        for (int c = 1; c < kStates; ++c) odds[c] = 1.0f / 16.0f;
        entropy = 4.0f / 4.08746284f;
        return;
    }
    if (forced) {
        odds[clampi(forced, 1, kSteps)] = 1.0f;
        return;
    }
    Ball tmp = b;
    if (b.resting) tmp.state = 0;
    float w[kStates];
    rowWeights(tmp, w);
    float total = 0.0f;
    for (float v : w) total += v;
    if (total <= 0.0f) return;
    float h = 0.0f;
    for (int c = 0; c < kStates; ++c) {
        const float p = w[c] / total;
        odds[c] = p;
        if (p > 0.0f) h -= p * log2f_(p);
    }
    entropy = h / 4.08746284f;   // log2(17)
}

// ---- capture -------------------------------------------------------------------------
void Engine::record(const Hit& h, double beat) {
    if (h.kind != Hit::kNote) return;
    CapEvent& e = cap_[capHead_];
    const double t = (beat + double(h.at)) * 96.0;
    e.t96 = t > 0.0 ? uint32_t(t + 0.5) : 0u;
    const float l = h.len * 96.0f;
    e.len96 = uint16_t(l < 1.0f ? 1.0f : (l > 60000.0f ? 60000.0f : l));
    e.ch = h.ch;
    e.note = h.note;
    e.vel = h.vel;
    capHead_ = (capHead_ + 1) % kCapRing;
    if (capCount_ < kCapRing) ++capCount_;
}

int Engine::capture(int bars, double nowBeat) {
    bars = clampi(bars, 1, 8);
    const double end = floorD(nowBeat / 4.0 + 1e-6) * 4.0;
    double start = end - double(bars) * 4.0;
    if (start < 0.0) start = 0.0;
    const uint32_t s96 = uint32_t(start * 96.0 + 0.5), e96 = uint32_t(end * 96.0 + 0.5);
    clipN_ = 0;
    for (int i = capCount_; i >= 1 && clipN_ < kClipCap; --i) {   // oldest first
        const CapEvent& e = cap_[(capHead_ - i + kCapRing) % kCapRing];
        if (e.t96 >= s96 && e.t96 < e96) {
            clip_[clipN_] = e;
            clip_[clipN_].t96 = e.t96 - s96;
            ++clipN_;
        }
    }
    clipBars_ = bars;
    clipStart96_ = s96;
    event(kEvCapture);
    return clipN_;
}

void Engine::replayHits(double beat) {
    if (!clipN_ || !clipBars_) return;
    const double len = double(clipBars_) * 4.0;
    const double mb = moveBeats();
    double ws = fmodD(beat - double(clipStart96_) / 96.0, len);
    if (ws < 0.0) ws += len;
    for (int i = 0; i < clipN_; ++i) {
        double d = double(clip_[i].t96) / 96.0 - ws;
        if (d < 0.0) d += len;
        if (d >= mb) continue;
        Hit h;
        h.ch = clip_[i].ch;
        h.note = clip_[i].note;
        h.vel = clip_[i].vel;
        h.at = float(d);
        h.len = float(clip_[i].len96) / 96.0f;
        addHit(h);
    }
}

}  // namespace tilt
