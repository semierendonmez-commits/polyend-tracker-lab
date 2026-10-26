// TILT -- drawing: the intro, the GRAPH, the 17 x 17 MATRIX, every page, the
// FILE and HELP overlays, and the pad LEDs. Every frame stays under the
// 1,800-word display-list budget (sim "budget" check).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/tilt_ws.h"

namespace tilt {

using lab::Canvas;
using lab::Rgb;
namespace font = lab::font;

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
const Rgb kGold(0xFFC83A), kBallCore(0xFFF6D8), kCyan(0x2EE6FF), kPink(0xFF2E88), kViolet(0x7B5CFF);
const Rgb kBallColors[kBalls] = {Rgb(0xFFC83A), Rgb(0x2EE6FF), Rgb(0xFF5FD2), Rgb(0xC6FF3D)};
constexpr int kMetaCellsTotal = 16;
const char* const kRowNames[kStepRows + 1] = {"TRIG", "TRIG %", "DELAY", "DELAY %", "VELO", "LENGTH",
                                              "BUMPER", "RAMP", "DEGREE", "LOCK", "MELODY"};
float gCos[kSteps], gSin[kSteps];
bool gTrig = false;
void initTrig() {
    if (gTrig) return;
    for (int s = 0; s < kSteps; ++s) {
        const float a = -1.5707963f + float(s) * (6.2831853f / float(kSteps));
        gCos[s] = cosf(a);
        gSin[s] = sinf(a);
    }
    gTrig = true;
}
// The neon "TILT" logo as line segments in a 4 x 5 box per letter (unit grid).
struct Seg {
    float x0, y0, x1, y1;
};
const Seg kLogo[] = {
    // T
    {0, 0, 4, 0}, {2, 0, 2, 5},
    // I
    {6, 0, 8, 0}, {7, 0, 7, 5}, {6, 5, 8, 5},
    // L
    {10, 0, 10, 5}, {10, 5, 13.5f, 5},
    // T
    {15, 0, 19, 0}, {17, 0, 17, 5},
};
void logo(Canvas& g, float x, float y, float unit, Rgb c, float glow, float lit) {
    const int n = int(sizeof kLogo / sizeof kLogo[0]);
    const int shown = int(lit * float(n) + 0.999f);
    if (glow > 0.0f) {
        g.color(c);
        g.alpha(uint8_t(clampf(glow, 0.0f, 1.0f) * 70.0f));
        g.lineWidth(unit * 0.9f);
        g.beginLines();
        for (int i = 0; i < shown; ++i) {
            g.vtx(x + kLogo[i].x0 * unit, y + kLogo[i].y0 * unit);
            g.vtx(x + kLogo[i].x1 * unit, y + kLogo[i].y1 * unit);
        }
        g.endBatch();
        g.alpha(255);
    }
    g.color(lab::mix(c, Rgb(0xFFFFFF), 0.35f));
    g.lineWidth(unit * 0.32f);
    g.beginLines();
    for (int i = 0; i < shown; ++i) {
        g.vtx(x + kLogo[i].x0 * unit, y + kLogo[i].y0 * unit);
        g.vtx(x + kLogo[i].x1 * unit, y + kLogo[i].y1 * unit);
    }
    g.endBatch();
    g.lineWidth(1.0f);
}
// A number as text: about 6 display-list words instead of CMD_NUMBER's 16.
void num(Canvas& g, int x, int y, uint8_t fnt, int v, uint16_t opt = 0) {
    char b[12];
    std::snprintf(b, sizeof b, "%d", v);
    g.text(x, y, fnt, b, opt);
}
float liveWeight(const Pattern& p, const Engine& e, int pg, int r, int c, bool live) {
    float w = float(p.w[pg][r][c]);
    if (live) w = clampf(w + e.mut[pg][r][c], 0.0f, 100.0f) * e.mem[r][c];
    return clampf(w, 0.0f, 100.0f);
}
}  // namespace

float Tilt::nodeX(int s, int cx, float rad) const {
    if (s <= 0) return float(cx) - 34.0f;
    return float(cx) + rad * gCos[clampi(s - 1, 0, kSteps - 1)];
}
float Tilt::nodeY(int s, int cy, float rad) const {
    if (s <= 0) return float(cy);
    return float(cy) + rad * gSin[clampi(s - 1, 0, kSteps - 1)];
}

// ---- intro -----------------------------------------------------------------------------------------
void Tilt::drawIntro(Canvas& g, uint32_t t) {
    initTrig();
    const float ts = float(t) * 0.001f;
    const int cx = 400, cy = 222;
    const float rad = 150.0f;
    // a growing Markov graph: nodes pop in, edges grow
    const int nodes = clampi(int(ts / 0.06f), 0, kSteps);
    static const uint8_t kPath[] = {1, 2, 3, 7, 8, 12, 5, 6, 10, 11, 15, 16, 4, 9, 13, 14, 1, 5, 9, 13};
    const int npath = int(sizeof kPath);
    const float hop = 0.11f;
    const float tb = ts - 0.35f;
    const int hopI = tb < 0.0f ? -1 : int(tb / hop);
    // edges along the path so far (thin, violet), the last one pink
    g.lineWidth(1.5f);
    g.color(lab::scale(kViolet, 0.8f));
    g.beginLines();
    for (int i = 1; i <= hopI && i < npath; ++i) {
        g.vtx(nodeX(kPath[i - 1], cx, rad), nodeY(kPath[i - 1], cy, rad));
        g.vtx(nodeX(kPath[i], cx, rad), nodeY(kPath[i], cy, rad));
    }
    g.endBatch();
    // chain ring
    g.color(lab::scale(theme_.dim, 0.5f));
    g.ring(float(cx), float(cy), rad, 48);
    // nodes
    g.pointSize(13.0f);
    g.beginPoints();
    for (int s = 1; s <= nodes; ++s) {
        bool hit = false;
        for (int i = 0; i <= hopI && i < npath; ++i) hit |= kPath[i] == s;
        const Rgb c = chColor((s * 5) % kCh);
        g.color(hit ? c : lab::scale(c, 0.3f));
        g.vtx(nodeX(s, cx, rad), nodeY(s, cy, rad));
    }
    g.endBatch();
    // the ball, hopping with a parabola, and its trail
    if (hopI >= 0 && hopI + 1 < npath) {
        const float f = (tb - float(hopI) * hop) / hop;
        const int a = kPath[hopI], b = kPath[hopI + 1];
        const float x = nodeX(a, cx, rad) + (nodeX(b, cx, rad) - nodeX(a, cx, rad)) * f;
        const float y = nodeY(a, cy, rad) + (nodeY(b, cy, rad) - nodeY(a, cy, rad)) * f - sinf(f * 3.14159f) * 26.0f;
        g.color(kGold);
        g.alpha(70);
        g.point(x, y, 18.0f);
        g.alpha(255);
        g.color(kBallCore);
        g.point(x, y, 8.0f);
    }
    // the logo switches on like neon
    if (ts > 1.25f) {
        const float k = clampf((ts - 1.25f) / 0.6f, 0.0f, 1.0f);
        const bool flick = (ts > 1.5f && ts < 1.56f) || (ts > 1.68f && ts < 1.72f);
        g.color(theme_.bg);
        g.alpha(uint8_t(170.0f * k));
        g.fill(0, 150, 800, 150);
        g.alpha(255);
        if (!flick) logo(g, 400.0f - 19.0f * 13.0f * 0.5f, 172.0f, 13.0f, kPink, k, k);
    }
    if (ts > 1.9f) {
        g.color(lab::mix(theme_.bg, theme_.fg, clampf((ts - 1.9f) / 0.4f, 0.0f, 1.0f)));
        g.text(400, 408, font::kBody, "17 x 17 PINBALL  -  8 CHANNELS  -  RENEWED PINBALL 2", Canvas::kCenter);
        g.color(lab::mix(theme_.bg, theme_.dim, clampf((ts - 2.1f) / 0.4f, 0.0f, 1.0f)));
        g.text(400, 440, font::kSmall, "polyend-tracker-lab", Canvas::kCenter);
    }
}

// ---- the frame ------------------------------------------------------------------------------------------
void Tilt::draw(Canvas& g) {
    initTrig();
    now_ = ctx_->millis();
    if (file_) {
        drawFile(g);
        return;
    }
    if (help_) {
        drawHelp(g);
        return;
    }
    char title[48];
    std::snprintf(title, sizeof title, "%s   page %c%s", kPages_[page_].title, 'A' + pat_->ev.page,
                  eng_->replaying() ? "   REPLAY" : "");
    lab::drawHeader(g, theme_, *ctx_->shell, title);
    switch (page_) {
        case kPgGraph: drawGraph(g); break;
        case kPgMatrix: drawMatrix(g); break;
        case kPgSteps: drawSteps(g); break;
        case kPgTable:
        case kPgEvolve: drawParams(g); break;
        case kPgCombos: drawCombos(g); break;
        case kPgChannels: drawChannels(g); break;
        case kPgMix: drawMix(g); break;
        default: break;
    }
    drawBanner(g);
    drawKeys(g);
}

void Tilt::drawKeys(Canvas& g) {
    if (shift_) {
        const char* const* k = kPages_[page_].shift;
        // Shift: the page's actions on a cyan strip
        g.color(lab::scale(kCyan, 0.25f));
        g.fill(0, lab::kScreenH - 30, lab::kScreenW, 30);
        for (int i = 0; i < 8; ++i) {
            if (!k[i] || !k[i][0]) continue;
            g.color(theme_.fg);
            g.text(i * 100 + 50, lab::kScreenH - 15, font::kSmall, k[i], Canvas::kCenter);
        }
        return;
    }
    const char* labels[8];
    for (int i = 0; i < 8; ++i) labels[i] = kPages_[i].name;
    lab::drawFKeyBar(g, theme_, labels, page_);
}

void Tilt::drawBanner(Canvas& g) {
    if (!banner_) return;
    const uint32_t age = now_ - bannerAt_;
    if (age > 1400) {
        banner_ = 0;
        return;
    }
    char txt[32];
    Rgb c = theme_.fg;
    switch (banner_) {
        case kEvTilt: std::snprintf(txt, sizeof txt, "TILT!"); c = theme_.warn; break;
        case kEvSave: std::snprintf(txt, sizeof txt, "BALL SAVED"); c = theme_.ok; break;
        case kEvMultiball: std::snprintf(txt, sizeof txt, "MULTIBALL"); c = kGold; break;
        case kEvCombo1:
        case kEvCombo2:
        case kEvCombo3:
        case kEvCombo4: {
            const int i = banner_ - kEvCombo1;
            std::snprintf(txt, sizeof txt, "COMBO %d  %s", i + 1, kComboActionNames[pat_->combos[i].action]);
            c = kPink;
            break;
        }
        case kEvScene: std::snprintf(txt, sizeof txt, "PAGE %c", 'A' + pat_->ev.page); c = kCyan; break;
        case kEvHalt: std::snprintf(txt, sizeof txt, "HALT"); c = theme_.dim; break;
        case kEvDrain: std::snprintf(txt, sizeof txt, "DRAIN"); c = theme_.dim; break;
        case kEvLaunch: std::snprintf(txt, sizeof txt, "LAUNCH"); c = theme_.ok; break;
        case kEvCapture:
            std::snprintf(txt, sizeof txt, "CAPTURED %d BAR%s", eng_->clipBars(), eng_->clipBars() > 1 ? "S" : "");
            c = kCyan;
            break;
        default: return;
    }
    const float k = age < 900 ? 1.0f : 1.0f - float(age - 900) / 500.0f;
    const int x = page_ == kPgGraph ? 286 : 400;
    g.color(theme_.bg);
    g.alpha(uint8_t(200.0f * k));
    g.fillRound(x - 150, 40, 300, 40, 8);
    g.alpha(uint8_t(255.0f * k));
    g.color(c);
    g.text(x, 60, font::kTitle, txt, Canvas::kCenter);
    g.alpha(255);
}

// ---- GRAPH ---------------------------------------------------------------------------------------------
void Tilt::drawGraphView(Canvas& g, int cx, int cy, float rad, bool big) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    const int pg = clampi(p.ev.page, 0, kPages - 1);
    const float nr = big ? 17.0f : 9.0f;
    const Ball& b0 = e.balls[0];
    const int ballRow = b0.active ? (b0.resting ? 0 : b0.state) : -1;
    // playfield ring
    g.color(lab::scale(theme_.panel, 1.4f));
    g.lineWidth(1.0f);
    g.ring(float(cx), float(cy), rad, 40);
    // edges by weight level (the ball's row last, in pink with heads)
    static const float kW[4] = {1.0f, 2.0f, 3.2f, 4.6f};
    int edges = 0;
    const int kMaxEdges = big ? 190 : 120;   // display budget: the lightest edges go first
    for (int lv = 3; lv >= 0; --lv) {
        g.lineWidth(big ? kW[lv] : kW[lv] * 0.6f);
        g.color(lab::mix(lab::scale(kViolet, 0.45f), kViolet, float(lv) / 3.0f));
        g.beginLines();
        for (int r = 0; r < kStates; ++r) {
            if (r == ballRow) continue;
            for (int c = 1; c < kStates; ++c) {
                if (c == r) continue;
                const float w = liveWeight(p, e, pg, r, c, true);
                if (w <= 0.5f) continue;
                const int l = w < 25.0f ? 0 : (w < 50.0f ? 1 : (w < 75.0f ? 2 : 3));
                if (l != lv || edges >= kMaxEdges) continue;
                ++edges;
                const float x0 = nodeX(r, cx, rad), y0 = nodeY(r, cy, rad);
                const float x1 = nodeX(c, cx, rad), y1 = nodeY(c, cy, rad);
                const float dx = x1 - x0, dy = y1 - y0;
                const float len = sqrtf(dx * dx + dy * dy) + 1e-3f;
                const float ux = dx / len, uy = dy / len;
                const float ox = -uy * 3.0f, oy = ux * 3.0f;   // A->B and B->A side by side
                g.vtx(x0 + ux * nr + ox, y0 + uy * nr + oy);
                g.vtx(x1 - ux * nr + ox, y1 - uy * nr + oy);
            }
        }
        g.endBatch();
    }
    // ramps: forced moves in cyan
    g.lineWidth(big ? 2.0f : 1.2f);
    g.color(kCyan);
    g.beginLines();
    for (int s = 1; s <= kSteps; ++s) {
        const int to = p.steps[s - 1].ramp;
        if (!to || to == s) continue;
        const float x0 = nodeX(s, cx, rad), y0 = nodeY(s, cy, rad), x1 = nodeX(to, cx, rad), y1 = nodeY(to, cy, rad);
        const float mx = (x0 + x1) * 0.5f + (float(cx) - (x0 + x1) * 0.5f) * 0.35f;
        const float my = (y0 + y1) * 0.5f + (float(cy) - (y0 + y1) * 0.5f) * 0.35f;
        g.vtx(x0, y0);
        g.vtx(mx, my);
        g.vtx(mx, my);
        g.vtx(x1, y1);
    }
    g.endBatch();
    // the ball's row: where it can go next
    if (ballRow >= 0) {
        g.color(kPink);
        float tot = 0.0f;
        for (int c = 0; c < kStates; ++c) tot += liveWeight(p, e, pg, ballRow, c, true);
        for (int c = 0; c < kStates; ++c) {
            const float w = liveWeight(p, e, pg, ballRow, c, true);
            if (w <= 0.5f || c == ballRow) continue;
            const float pr = tot > 0.0f ? w / tot : 0.0f;
            g.lineWidth((big ? 1.5f : 1.0f) + pr * (big ? 7.0f : 3.0f));
            const float x0 = nodeX(ballRow, cx, rad), y0 = nodeY(ballRow, cy, rad);
            const float x1 = c == 0 ? float(cx) + 34.0f : nodeX(c, cx, rad);
            const float y1 = c == 0 ? float(cy) : nodeY(c, cy, rad);
            const float dx = x1 - x0, dy = y1 - y0;
            const float len = sqrtf(dx * dx + dy * dy) + 1e-3f;
            const float ux = dx / len, uy = dy / len;
            const float ex = x1 - ux * (nr + 2.0f), ey = y1 - uy * (nr + 2.0f);
            g.beginLines();
            g.vtx(x0 + ux * nr, y0 + uy * nr);
            g.vtx(ex, ey);
            if (big) {   // arrow head
                g.vtx(ex, ey);
                g.vtx(ex - ux * 10.0f - uy * 6.0f, ey - uy * 10.0f + ux * 6.0f);
                g.vtx(ex, ey);
                g.vtx(ex - ux * 10.0f + uy * 6.0f, ey - uy * 10.0f - ux * 6.0f);
            }
            g.endBatch();
        }
    }
    g.lineWidth(1.0f);
    // self loops
    g.color(lab::scale(kViolet, 1.1f));
    for (int s = 1; s <= kSteps; ++s) {
        if (liveWeight(p, e, pg, s, s, false) < 1.0f) continue;
        const float x = float(cx) + (rad + nr + 6.0f) * gCos[s - 1], y = float(cy) + (rad + nr + 6.0f) * gSin[s - 1];
        g.ring(x, y, big ? 7.0f : 4.0f, 8);
    }
    // S and D
    g.color(lab::scale(theme_.fg, 0.55f));
    g.ring(float(cx) - 34.0f, float(cy), big ? 12.0f : 7.0f, 12);
    g.color(lab::scale(theme_.warn, 0.7f));
    g.ring(float(cx) + 34.0f, float(cy), big ? 12.0f : 7.0f, 12);
    if (big) {
        g.color(theme_.dim);
        g.text(cx - 34, cy, font::kTiny, "S", Canvas::kCenter);
        g.text(cx + 34, cy, font::kTiny, "D", Canvas::kCenter);
    }
    // nodes: channel colour ring, heat inside
    g.pointSize(nr + 1.0f);
    g.beginPoints();
    for (int s = 1; s <= kSteps; ++s) {
        g.color(trigColor(p.steps[s - 1].trig));
        g.vtx(nodeX(s, cx, rad), nodeY(s, cy, rad));
    }
    g.endBatch();
    g.pointSize(nr - 3.0f);
    g.beginPoints();
    for (int s = 1; s <= kSteps; ++s) {
        const float h = clampf(e.heat[s - 1] * 0.4f, 0.0f, 1.0f);
        g.color(lab::mix(theme_.bg, trigColor(p.steps[s - 1].trig), 0.15f + 0.85f * h));
        g.vtx(nodeX(s, cx, rad), nodeY(s, cy, rad));
    }
    g.endBatch();
    // bumpers: dots around the node
    g.color(kGold);
    g.pointSize(big ? 2.2f : 1.5f);
    g.beginPoints();
    for (int s = 1; s <= kSteps; ++s)
        for (int k = 0; k < p.steps[s - 1].bumper; ++k) {
            const float a = -1.5707963f + float(s - 1) * 0.3926991f + (float(k) - 1.5f) * 0.28f;
            g.vtx(float(cx) + (rad - nr - 6.0f) * cosf(a), float(cy) + (rad - nr - 6.0f) * sinf(a));
        }
    g.endBatch();
    if (big) {
        g.color(theme_.fg);
        for (int s = 1; s <= kSteps; ++s) num(g, int(nodeX(s, cx, rad)), int(nodeY(s, cy, rad)), font::kTiny, s, Canvas::kCenter);
        // flipper targets
        g.color(kGold);
        for (int i = 0; i < kFlips; ++i) {
            const int s = p.flips[i].step;
            const float x = float(cx) + (rad + 40.0f) * gCos[s - 1], y = float(cy) + (rad + 36.0f) * gSin[s - 1];
            char f[4] = {'F', char('1' + i), 0, 0};
            g.text(int(x), int(y), font::kTiny, f, Canvas::kCenter);
        }
    }
    // the balls (moving along their last edge), with trails
    const double mb = eng_->moveBeats();
    const double beat = beatNow_;
    for (int bi = kBalls - 1; bi >= 0; --bi) {
        const Ball& b = e.balls[bi];
        if (!b.active) continue;
        const int cur = b.resting ? 0 : b.state;
        const int prv = b.trail[1];
        float f = mb > 0.0 ? float((beat - double(b.lastMove)) / (mb * 0.45)) : 1.0f;
        f = clampf(f, 0.0f, 1.0f);
        f = 1.0f - (1.0f - f) * (1.0f - f);
        const float x = nodeX(prv, cx, rad) + (nodeX(cur, cx, rad) - nodeX(prv, cx, rad)) * f;
        const float y = nodeY(prv, cy, rad) + (nodeY(cur, cy, rad) - nodeY(prv, cy, rad)) * f;
        if (setTrail_) {
            g.color(kBallColors[bi]);
            g.pointSize(big ? 4.0f : 2.5f);
            g.beginPoints();
            for (int k = 1; k < Ball::kTrail; ++k) {
                if (!b.trail[k] && k > 1) break;
                g.alpha(uint8_t(200 - k * 24));
                g.vtx(nodeX(b.trail[k], cx, rad), nodeY(b.trail[k], cy, rad));
            }
            g.endBatch();
            g.alpha(255);
        }
        g.color(kBallColors[bi]);
        g.alpha(e.tilted() ? 140 : 80);
        g.point(x, y, big ? 15.0f : 9.0f);
        g.alpha(255);
        g.color(e.tilted() ? theme_.warn : kBallCore);
        g.point(x, y, big ? 7.0f : 4.0f);
    }
}

void Tilt::drawSidePanel(Canvas& g, int x) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    int y = 44;
    // pages A-D
    g.color(theme_.dim);
    g.text(x, y, font::kSmall, "PAGE", Canvas::kCenterY);
    for (int i = 0; i < kPages; ++i) {
        const bool on = p.ev.page == i;
        g.color(on ? kPink : theme_.panel);
        g.fillRound(x + 56 + i * 34, y - 11, 28, 22, 4);
        g.color(on ? theme_.bg : theme_.dim);
        const char s[2] = {char('A' + i), 0};
        g.text(x + 70 + i * 34, y, font::kSmall, s, Canvas::kCenter);
    }
    if (p.ev.metaOn) {
        g.color(kCyan);
        g.text(x + 196, y, font::kTiny, "META", Canvas::kCenterY);
    }
    // next-move odds
    y = 76;
    g.color(theme_.dim);
    g.text(x, y, font::kSmall, "NEXT MOVE", Canvas::kCenterY);
    int order[kStates];
    int n = 0;
    for (int c = 0; c < kStates; ++c)
        if (e.odds[c] > 0.004f) order[n++] = c;
    for (int i = 1; i < n; ++i) {
        const int v = order[i];
        int j = i;
        while (j > 0 && e.odds[order[j - 1]] < e.odds[v]) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = v;
    }
    for (int i = 0; i < 5; ++i) {
        const int yy = y + 22 + i * 22;
        if (i >= n) break;
        const int c = order[i];
        const float pr = e.odds[c];
        g.color(c == 0 ? theme_.warn : trigColor(p.steps[c - 1].trig));
        g.fill(x + 50, yy - 7, int(128.0f * pr) + 2, 14);
        g.color(theme_.fg);
        if (c == 0) g.text(x, yy, font::kSmall, "-> D", Canvas::kCenterY);
        else g.textf(x, yy, font::kSmall, Canvas::kCenterY, "-> %d", c);
        g.textf(x + 236, yy, font::kSmall, Canvas::kCenterY | Canvas::kRight, "%d%%", int(pr * 100.0f + 0.5f));
    }
    // entropy
    y = 206;
    g.color(theme_.dim);
    g.text(x, y, font::kSmall, "ENTROPY", Canvas::kCenterY);
    g.color(theme_.panel);
    g.fill(x + 90, y - 6, 110, 12);
    g.color(lab::mix(kCyan, kPink, e.entropy));
    g.fill(x + 90, y - 6, int(110.0f * clampf(e.entropy, 0.0f, 1.0f)) + 1, 12);
    g.color(theme_.fg);
    g.textf(x + 236, y, font::kSmall, Canvas::kCenterY | Canvas::kRight, "%.2f", double(e.entropy));
    // lamps: tilt, save, balls, replay
    y = 238;
    struct Lamp {
        const char* name;
        bool on;
        Rgb c;
    };
    const bool saveOn = p.table.saveBars && e.balls[0].active &&
                        beatNow_ - e.balls[0].launchBeat < double(p.table.saveBars) * 4.0;
    const Lamp lamps[4] = {{"TILT", e.tilted(), theme_.warn},
                           {"SAVE", saveOn, theme_.ok},
                           {"MULTI", e.ballsActive() > 1, kGold},
                           {"REPLAY", e.replaying(), kCyan}};
    for (int i = 0; i < 4; ++i) {
        const int lx = x + i * 60;
        g.color(lamps[i].on ? lamps[i].c : theme_.panel);
        g.fillRound(lx, y - 10, 54, 20, 5);
        g.color(lamps[i].on ? theme_.bg : theme_.dim);
        g.text(lx + 27, y, font::kTiny, lamps[i].name, Canvas::kCenter);
    }
    // flips this bar vs the tilt limit
    y = 266;
    g.color(theme_.dim);
    g.text(x, y, font::kTiny, "FLIPS", Canvas::kCenterY);
    const int lim = p.table.tiltFlips ? p.table.tiltFlips : 8;
    g.pointSize(4.0f);
    g.beginPoints();
    for (int i = 0; i < lim && i < 16; ++i) {
        g.color(i < e.flipsBar ? (i + 1 >= lim && p.table.tiltFlips ? theme_.warn : kGold) : theme_.panel);
        g.vtx(float(x + 56 + i * 11), float(y));
    }
    g.endBatch();
    g.color(theme_.dim);
    g.textf(x + 236, y, font::kTiny, Canvas::kCenterY | Canvas::kRight, "balls %d  saves %d", e.ballsActive(), e.saves);
    // combos
    y = 296;
    g.color(theme_.dim);
    g.text(x, y, font::kSmall, "COMBOS", Canvas::kCenterY);
    for (int i = 0; i < kCombos; ++i) {
        const Combo& c = p.combos[i];
        const int yy = y + 24 + i * 26;
        if (c.len < 2) {
            g.color(lab::scale(theme_.dim, 0.6f));
            g.textf(x, yy, font::kTiny, Canvas::kCenterY, "%d  --", i + 1);
            continue;
        }
        g.color(c.on ? theme_.fg : theme_.dim);
        num(g, x, yy, font::kTiny, i + 1, Canvas::kCenterY);
        for (int k = 0; k < c.len; ++k) {
            const bool lit = k < e.comboProgress[i];
            g.color(lit ? kPink : theme_.panel);
            g.point(float(x + 26 + k * 26), float(yy), 10.0f);
            g.color(lit ? theme_.bg : theme_.fg);
            num(g, x + 26 + k * 26, yy, font::kTiny, c.path[k], Canvas::kCenter);
        }
        g.color(kPink);
        g.text(x + 236, yy, font::kTiny, kComboActionNames[c.action], Canvas::kCenterY | Canvas::kRight);
    }
    // rate and transport hint
    y = 428;
    g.color(theme_.dim);
    g.textf(x, y, font::kTiny, Canvas::kCenterY, "MOVE %s   SWING %d%%   %s", kRates[p.table.rate].name, p.table.swing,
            ctx_->playing() ? "" : "PLAY to launch");
}

void Tilt::drawGraph(Canvas& g) {
    drawGraphView(g, 276, 240, 160.0f, true);
    drawSidePanel(g, 548);
}

// ---- MATRIX --------------------------------------------------------------------------------------------
void Tilt::drawMatrix(Canvas& g) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    const int pg = clampi(p.ev.page, 0, kPages - 1);
    const int x0 = 40, y0 = 62, cs = 21;
    const int ballRow = e.balls[0].active ? (e.balls[0].resting ? 0 : e.balls[0].state) : -1;
    // ball row band, cursor cross
    if (ballRow >= 0) {
        g.color(kGold);
        g.alpha(40);
        g.fill(x0, y0 + ballRow * cs, kStates * cs, cs);
        g.alpha(255);
    }
    g.color(kCyan);
    g.alpha(28);
    g.fill(x0, y0 + mxRow_ * cs, kStates * cs, cs);
    g.fill(x0 + mxCol_ * cs, y0, cs, kStates * cs);
    g.alpha(255);
    // the drain column
    g.color(theme_.warn);
    g.alpha(30);
    g.fill(x0, y0, cs, kStates * cs);
    g.alpha(255);
    // grid
    g.color(lab::scale(theme_.panel, 1.5f));
    g.beginLines();
    for (int i = 0; i <= kStates; ++i) {
        g.vtx(float(x0 + i * cs), float(y0));
        g.vtx(float(x0 + i * cs), float(y0 + kStates * cs));
        g.vtx(float(x0), float(y0 + i * cs));
        g.vtx(float(x0 + kStates * cs), float(y0 + i * cs));
    }
    g.endBatch();
    // labels
    for (int c = 0; c < kStates; ++c) {
        g.color(c == mxCol_ ? kCyan : (c == 0 ? theme_.warn : theme_.dim));
        if (c == 0) g.text(x0 + cs / 2, y0 - 10, font::kTiny, "D", Canvas::kCenter);
        else num(g, x0 + c * cs + cs / 2, y0 - 10, font::kTiny, c, Canvas::kCenter);
    }
    for (int r = 0; r < kStates; ++r) {
        g.color(r == mxRow_ ? kCyan : (r == ballRow ? kGold : theme_.dim));
        if (r == 0) g.text(x0 - 12, y0 + cs / 2, font::kTiny, "S", Canvas::kCenter);
        else num(g, x0 - 12, y0 + r * cs + cs / 2, font::kTiny, r, Canvas::kCenter);
    }
    // cells: size and colour by weight, four levels
    static const Rgb kLv[4] = {Rgb(0x5B3FB0), Rgb(0xB0309A), Rgb(0xFF2E88), Rgb(0xFFD0E4)};
    for (int lv = 0; lv < 4; ++lv) {
        g.color(kLv[lv]);
        g.beginRects();
        for (int r = 0; r < kStates; ++r)
            for (int c = 0; c < kStates; ++c) {
                const float w = liveWeight(p, e, pg, r, c, mxLive_);
                if (w < 0.5f) continue;
                const int l = w < 25.0f ? 0 : (w < 50.0f ? 1 : (w < 75.0f ? 2 : 3));
                if (l != lv) continue;
                const float half = 2.5f + w * 0.075f;
                const float cx = float(x0 + c * cs) + cs * 0.5f, cy = float(y0 + r * cs) + cs * 0.5f;
                g.vtx(cx - half, cy - half);
                g.vtx(cx + half, cy + half);
            }
        g.endBatch();
    }
    // edge properties: a cyan dot in the corner
    g.color(kCyan);
    g.pointSize(2.0f);
    g.beginPoints();
    for (int r = 0; r < kStates; ++r)
        for (int c = 0; c < kStates; ++c) {
            const Edge& ed = p.e[pg][r][c];
            if (ed.tr || ed.dly || ed.vel) g.vtx(float(x0 + c * cs + 4), float(y0 + r * cs + 4));
        }
    g.endBatch();
    // the last move and the cursor
    if (e.lastTo || e.lastFrom) {
        g.color(kGold);
        g.frame(x0 + e.lastTo * cs, y0 + e.lastFrom * cs, cs + 1, cs + 1);
    }
    g.color(theme_.fg);
    g.frame(x0 + mxCol_ * cs - 1, y0 + mxRow_ * cs - 1, cs + 3, cs + 3);

    // right panel
    const int px = 430;
    const uint8_t w = p.w[pg][mxRow_][mxCol_];
    int sum = 0;
    for (int c = 0; c < kStates; ++c) sum += p.w[pg][mxRow_][c];
    char from[8], to[8];
    if (mxRow_ == 0) std::snprintf(from, sizeof from, "S");
    else std::snprintf(from, sizeof from, "%d", mxRow_);
    if (mxCol_ == 0) std::snprintf(to, sizeof to, "D");
    else std::snprintf(to, sizeof to, "%d", mxCol_);
    g.color(theme_.fg);
    g.textf(px, 62, font::kHuge, Canvas::kCenterY, "%s -> %s", from, to);
    g.color(theme_.dim);
    g.textf(px + 360, 62, font::kSmall, Canvas::kCenterY | Canvas::kRight, "row sum %d", sum);
    // page tabs
    for (int i = 0; i < kPages; ++i) {
        const bool on = p.ev.page == i;
        g.color(on ? kPink : theme_.panel);
        g.fillRound(px + 230 + i * 32, 88, 26, 20, 4);
        g.color(on ? theme_.bg : theme_.dim);
        const char s[2] = {char('A' + i), 0};
        g.text(px + 243 + i * 32, 98, font::kSmall, s, Canvas::kCenter);
    }
    g.color(theme_.dim);
    g.text(px, 98, font::kSmall, mxLive_ ? "VIEW: live (mutation, memory)" : "VIEW: stored", Canvas::kCenterY);
    // fields
    const Edge& ed = p.e[pg][mxRow_][mxCol_];
    char v[6][24];
    std::snprintf(v[0], sizeof v[0], "%d   (p %d%%)", w, sum ? (w * 100 + sum / 2) / sum : 0);
    std::snprintf(v[1], sizeof v[1], "%+d st", ed.tr);
    std::snprintf(v[2], sizeof v[2], "%d/12 move", ed.dly);
    std::snprintf(v[3], sizeof v[3], "%+d", ed.vel);
    std::snprintf(v[4], sizeof v[4], "%s", kShapeNames[mxShape_]);
    std::snprintf(v[5], sizeof v[5], "%s", kRowFnNames[mxRowFn_]);
    static const char* const kF[6] = {"WEIGHT", "TRANSPOSE", "DELAY", "VELOCITY", "SHAPE (push)", "ROW FN (push)"};
    for (int i = 0; i < 6; ++i) {
        const int yy = 130 + i * 30;
        if (i == mxField_) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(px - 6, yy - 13, 362, 26, 5);
        }
        g.color(i == mxField_ ? theme_.fg : theme_.dim);
        g.text(px, yy, font::kBody, kF[i], Canvas::kCenterY);
        g.color(i == 4 || i == 5 ? kCyan : theme_.fg);
        g.text(px + 354, yy, font::kBody, v[i], Canvas::kCenterY | Canvas::kRight);
    }
    // the row's odds
    const int by = 412;
    g.color(theme_.dim);
    g.text(px, 322, font::kSmall, "THIS ROW", Canvas::kCenterY);
    g.beginRects();
    for (int c = 0; c < kStates; ++c) {
        const int h = sum ? p.w[pg][mxRow_][c] * 70 / sum : 0;
        g.color(c == mxCol_ ? kCyan : (c == 0 ? theme_.warn : kPink));
        g.vtx(float(px + c * 21), float(by - h - 1));
        g.vtx(float(px + c * 21 + 15), float(by));
    }
    g.endBatch();
    g.color(theme_.dim);
    g.text(px, 432, font::kTiny, "pads 1-16: columns   lower pads: source row", Canvas::kCenterY);
}

// ---- STEPS ---------------------------------------------------------------------------------------------
void Tilt::drawSteps(Canvas& g) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    const int x0 = 112, cw = 42, y0 = 70, rh = 33;
    const int ballStep = e.balls[0].active && !e.balls[0].resting ? e.balls[0].state : 0;
    // ball column, cursor column
    if (ballStep) {
        g.color(kGold);
        g.alpha(45);
        g.fill(x0 + (ballStep - 1) * cw, y0 - 22, cw - 2, kStepRows * rh + 22);
        g.alpha(255);
    }
    g.color(kCyan);
    g.alpha(30);
    g.fill(x0 + stStep_ * cw, y0 - 22, cw - 2, kStepRows * rh + 22);
    if (stRow_ < kStepRows) g.fill(0, y0 + stRow_ * rh, 800, rh - 2);
    g.alpha(255);
    // header: step numbers (flipper targets in gold)
    for (int s = 0; s < kSteps; ++s) {
        bool fl = false;
        for (const auto& f : p.flips) fl |= f.step == s + 1;
        g.color(s == stStep_ ? kCyan : (fl ? kGold : theme_.dim));
        num(g, x0 + s * cw + cw / 2 - 1, y0 - 11, font::kSmall, s + 1, Canvas::kCenter);
    }
    for (int r = 0; r < kStepRows; ++r) {
        const bool locked = (p.rnd.rowLocks >> r) & 1;
        g.color(r == stRow_ ? theme_.fg : theme_.dim);
        g.text(8, y0 + r * rh + rh / 2, font::kSmall, kRowNames[r], Canvas::kCenterY);
        if (locked) {
            g.color(kGold);
            g.fill(96, y0 + r * rh + rh / 2 - 4, 8, 8);
        }
    }
    // TRIG row: channel chips
    for (int s = 0; s < kSteps; ++s) {
        const Step& st = p.steps[s];
        const int x = x0 + s * cw, y = y0 + 3;
        g.color(trigColor(st.trig));
        g.fillRound(x + 2, y, cw - 6, rh - 8, 4);
        if (st.trig) {
            g.color(theme_.bg);
            const char t[2] = {st.trig == kTrigMute ? 'M' : char('0' + st.trig), 0};
            g.text(x + cw / 2 - 1, y + (rh - 8) / 2, font::kSmall, t, Canvas::kCenter);
        }
    }
    // value rows as bars
    auto norm = [](const Step& st, int row) -> float {
        switch (row) {
            case kRowTrigP: return float(st.trigP) / 100.0f;
            case kRowDelay: return float(st.delay) / 23.0f;
            case kRowDelayP: return float(st.delayP) / 100.0f;
            case kRowVel: return float(st.vel) / 127.0f;
            case kRowLen: return st.len >= 16 ? 1.0f : float(st.len) / 16.0f;
            case kRowBumper: return float(st.bumper) / 4.0f;
            default: return 0.0f;
        }
    };
    static const Rgb kRowC[kStepRows] = {Rgb(0), Rgb(0xFF2E88), Rgb(0x2EE6FF), Rgb(0x2E9AFF), Rgb(0xFFC83A),
                                         Rgb(0x4DFFA6), Rgb(0xFFC83A), Rgb(0x2EE6FF), Rgb(0xC6FF3D), Rgb(0)};
    for (int r = kRowTrigP; r <= kRowBumper; ++r) {
        const int y = y0 + r * rh;
        g.color(lab::scale(kRowC[r], 0.85f));
        g.beginRects();
        for (int s = 0; s < kSteps; ++s) {
            const float v = norm(p.steps[s], r);
            const int h = int(v * float(rh - 8));
            if (r == kRowDelay && p.steps[s].delay == 0) continue;
            if (r == kRowBumper && p.steps[s].bumper == 0) continue;
            g.vtx(float(x0 + s * cw + 4), float(y + rh - 4 - h - 1));
            g.vtx(float(x0 + s * cw + cw - 8), float(y + rh - 4));
        }
        g.endBatch();
    }
    // degree: bars from the middle
    {
        const int y = y0 + kRowDegree * rh + rh / 2;
        g.color(lab::scale(theme_.dim, 0.5f));
        g.hline(x0, y, kSteps * cw - 4);
        g.color(kRowC[kRowDegree]);
        g.beginRects();
        for (int s = 0; s < kSteps; ++s) {
            const int d = p.steps[s].degree;
            if (!d) continue;
            const int h = d * 13 / 21;
            g.vtx(float(x0 + s * cw + 8), float(d > 0 ? y - h : y));
            g.vtx(float(x0 + s * cw + cw - 12), float(d > 0 ? y : y - h));
        }
        g.endBatch();
    }
    // ramps and locks
    for (int s = 0; s < kSteps; ++s) {
        const Step& st = p.steps[s];
        if (st.ramp) {
            g.color(kCyan);
            g.textf(x0 + s * cw + cw / 2 - 1, y0 + kRowRamp * rh + rh / 2, font::kTiny, Canvas::kCenter, ">%d", st.ramp);
        }
        if (st.lock) {
            g.color(kGold);
            g.fillRound(x0 + s * cw + 12, y0 + kRowLock * rh + 8, 16, 14, 3);
        }
    }
    // the cursor cell and its value
    if (stRow_ < kStepRows) {
        const Step& st = p.steps[stStep_];
        char v[16];
        switch (stRow_) {
            case kRowTrig:
                if (st.trig == 0) std::snprintf(v, sizeof v, "none");
                else if (st.trig == kTrigMute) std::snprintf(v, sizeof v, "MUTE");
                else std::snprintf(v, sizeof v, "%d %s", st.trig, kEngineNames[p.ch[st.trig - 1].type]);
                break;
            case kRowTrigP: std::snprintf(v, sizeof v, "%d%%", st.trigP); break;
            case kRowDelay: std::snprintf(v, sizeof v, "%d/12", st.delay); break;
            case kRowDelayP: std::snprintf(v, sizeof v, "%d%%", st.delayP); break;
            case kRowVel: std::snprintf(v, sizeof v, "%d", st.vel); break;
            case kRowLen: std::snprintf(v, sizeof v, "%d/4 move", st.len); break;
            case kRowBumper: std::snprintf(v, sizeof v, "+%d hits", st.bumper); break;
            case kRowRamp:
                if (st.ramp) std::snprintf(v, sizeof v, "-> %d", st.ramp);
                else std::snprintf(v, sizeof v, "off");
                break;
            case kRowDegree: std::snprintf(v, sizeof v, "%+d", st.degree); break;
            default: std::snprintf(v, sizeof v, "%s", st.lock ? "locked" : "free"); break;
        }
        g.color(theme_.fg);
        g.frame(x0 + stStep_ * cw - 1, y0 + stRow_ * rh - 1, cw, rh);
        g.color(kCyan);
        g.textf(790, 44, font::kBody, Canvas::kCenterY | Canvas::kRight, "step %d  %s  %s", stStep_ + 1,
                kRowNames[stRow_], v);
    }
    // melody strip
    const int my = 418;
    g.color(stRow_ == kStepRows ? lab::scale(kPink, 0.3f) : theme_.panel);
    g.fillRound(4, my - 14, 792, 28, 6);
    int ids[8];
    const int n = paramsOfPage(kPpMelody, ids, 8);
    for (int i = 0; i < n; ++i) {
        char v[20];
        paramText(p, ids[i], 0, v, sizeof v);
        const bool f = stRow_ == kStepRows && melField_ == i;
        g.color(f ? theme_.fg : theme_.dim);
        g.text(16 + i * 196, my, font::kSmall, kParams[ids[i]].name, Canvas::kCenterY);
        g.color(f ? kCyan : (i == 0 && p.mel.on ? kPink : theme_.fg));
        g.text(16 + i * 196 + 170, my, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
}

// ---- TABLE / EVOLVE -------------------------------------------------------------------------------------
void Tilt::drawParamList(Canvas& g, const int* ids, int n, int x0, int y0, int colW, int focus, int index) {
    int y = y0;
    const char* group = nullptr;
    for (int i = 0; i < n; ++i) {
        const ParamDef& d = kParams[ids[i]];
        if (!group || std::strcmp(group, d.group)) {
            group = d.group;
            if (i) y += 8;
            g.color(kCyan);
            g.text(x0, y, font::kSmall, group, Canvas::kCenterY);
            g.color(lab::scale(kCyan, 0.4f));
            g.hline(x0, y + 11, colW - 12);
            y += 26;
        }
        if (i == focus) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(x0 - 6, y - 12, colW - 6, 24, 5);
        }
        char v[24];
        paramText(*pat_, ids[i], index, v, sizeof v);
        g.color(i == focus ? theme_.fg : theme_.dim);
        g.text(x0, y, font::kSmall, d.name, Canvas::kCenterY);
        g.color(theme_.fg);
        g.text(x0 + colW - 18, y, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
        y += 25;
    }
}

void Tilt::drawParams(Canvas& g) {
    int ids[48];
    const int n = paramListOf(page_, ids, 48);
    const Engine& e = *eng_;
    if (page_ == kPgTable) {
        const int f = parFocus_[0];
        // three columns by group
        int c0[16], c1[16], c2[16], n0 = 0, n1 = 0, n2 = 0;
        int f0 = -1, f1 = -1, f2 = -1;
        for (int i = 0; i < n; ++i) {
            const char* gname = kParams[ids[i]].group;
            if (!std::strcmp(gname, "CLOCK") || !std::strcmp(gname, "TILT")) {
                if (i == f) f0 = n0;
                c0[n0++] = ids[i];
            } else if (!std::strcmp(gname, "FLIPPERS")) {
                if (i == f) f1 = n1;
                c1[n1++] = ids[i];
            } else {
                if (i == f) f2 = n2;
                c2[n2++] = ids[i];
            }
        }
        drawParamList(g, c0, n0, 20, 50, 250, f0, 0);
        drawParamList(g, c1, n1, 284, 50, 250, f1, 0);
        drawParamList(g, c2, n2, 548, 50, 246, f2, 0);
        // the table, live: tilt meter, ball save, balls
        const int x = 548, y = 236;
        g.color(theme_.panel);
        g.fillRound(x - 6, y, 246, 176, 8);
        g.color(theme_.dim);
        g.text(x + 4, y + 18, font::kSmall, "THE TABLE NOW", Canvas::kCenterY);
        const int lim = pat_->table.tiltFlips ? pat_->table.tiltFlips : 8;
        g.text(x + 4, y + 46, font::kTiny, "flips this bar", Canvas::kCenterY);
        g.pointSize(5.0f);
        g.beginPoints();
        for (int i = 0; i < lim && i < 16; ++i) {
            g.color(i < e.flipsBar ? (i + 1 >= lim ? theme_.warn : kGold) : lab::scale(theme_.dim, 0.5f));
            g.vtx(float(x + 110 + i * 13), float(y + 46));
        }
        g.endBatch();
        g.color(e.tilted() ? theme_.warn : theme_.dim);
        g.text(x + 4, y + 76, font::kTitle, e.tilted() ? "TILT!" : "tilt ok", Canvas::kCenterY);
        g.color(theme_.fg);
        g.textf(x + 4, y + 108, font::kSmall, Canvas::kCenterY, "balls in play %d / %d", e.ballsActive(), pat_->table.mbMax);
        g.textf(x + 4, y + 132, font::kSmall, Canvas::kCenterY, "ball saves %d   drains %d", e.saves, e.drains);
        g.color(theme_.dim);
        g.text(x + 4, y + 158, font::kTiny, "lower pads: flippers, nudge, relaunch", Canvas::kCenterY);
        // flipper bats drawn under the flipper column
        const int fx = 284, fy = 330;
        g.lineWidth(7.0f);
        for (int i = 0; i < kFlips; ++i) {
            const float bx = float(fx + 20 + i * 60);
            g.color(lab::mix(kGold, theme_.bg, e.tilted() ? 0.7f : 0.0f));
            g.line(bx, float(fy + 60), bx + 36.0f, float(fy + 72 - (i % 2) * 24));
            g.color(theme_.fg);
            g.textf(int(bx) + 18, fy + 100, font::kTiny, Canvas::kCenter, "F%d>%d", i + 1, pat_->flips[i].step);
        }
        g.lineWidth(1.0f);
        g.color(theme_.dim);
        g.text(fx, fy + 126, font::kTiny, "Note / Instr / FX1 / FX2 keys flip", Canvas::kCenterY);
        return;
    }
    // EVOLVE: [memory, mutation, rules] [meta + grid + evolution map] [randomizer + locks]
    const int f = parFocus_[1];
    int c0[16], c1[8], c2[8], n0 = 0, n1 = 0, n2 = 0, f0 = -1, f1 = -1, f2 = -1;
    for (int i = 0; i < n; ++i) {
        if (i < 6 || (i >= 9 && i < 13)) {
            if (i == f) f0 = n0;
            c0[n0++] = ids[i];
        } else if (i < 9) {
            if (i == f) f1 = n1;
            c1[n1++] = ids[i];
        } else {
            if (i == f) f2 = n2;
            c2[n2++] = ids[i];
        }
    }
    drawParamList(g, c0, n0, 20, 50, 250, f0, 0);
    drawParamList(g, c1, n1, 284, 50, 250, f1, 0);
    drawParamList(g, c2, n2, 548, 50, 246, f2, 0);
    // meta grid
    const int gx = 300, gy = 176, cs = 34;
    g.color(theme_.dim);
    g.text(gx - 16, gy - 20, font::kTiny, "META: from page (rows) to page (cols)", Canvas::kCenterY);
    for (int r = 0; r < kPages; ++r)
        for (int c = 0; c < kPages; ++c) {
            const int k = r * 4 + c;
            const uint8_t v = pat_->ev.meta[r][c];
            g.color(lab::mix(theme_.panel, kViolet, float(v) / 100.0f));
            g.fill(gx + 20 + c * cs, gy + 6 + r * 26, cs - 4, 22);
            if (f == n + k) {
                g.color(theme_.fg);
                g.frame(gx + 19 + c * cs, gy + 5 + r * 26, cs - 2, 24);
            }
            g.color(theme_.fg);
            num(g, gx + 20 + c * cs + (cs - 4) / 2, gy + 17 + r * 26, font::kTiny, v, Canvas::kCenter);
        }
    for (int i = 0; i < kPages; ++i) {
        const char s[2] = {char('A' + i), 0};
        g.color(pat_->ev.page == i ? kPink : theme_.dim);
        g.text(gx + 6, gy + 17 + i * 26, font::kTiny, s, Canvas::kCenter);
        g.text(gx + 20 + i * cs + (cs - 4) / 2, gy - 4, font::kTiny, s, Canvas::kCenter);
    }
    // evolution map: memory (pink tired, cyan strong) and mutation (gold)
    const int mx = 300, my = 314, ms = 7;
    g.color(theme_.dim);
    g.text(mx - 16, my - 12, font::kTiny, "EVOLUTION: memory + drift", Canvas::kCenterY);
    g.color(kPink);
    g.text(mx + 128, my + 20, font::kTiny, "tired", Canvas::kCenterY);
    g.color(kCyan);
    g.text(mx + 128, my + 40, font::kTiny, "stronger", Canvas::kCenterY);
    g.color(kGold);
    g.text(mx + 128, my + 60, font::kTiny, "drifted", Canvas::kCenterY);
    g.color(theme_.panel);
    g.fill(mx, my, kStates * ms, kStates * ms);
    const int pg = clampi(pat_->ev.page, 0, kPages - 1);
    g.pointSize(3.2f);
    g.beginPoints();
    int drawn = 0;
    for (int r = 0; r < kStates && drawn < 140; ++r)
        for (int c = 0; c < kStates && drawn < 140; ++c) {
            const float m = e.mem[r][c];
            const float d = e.mut[pg][r][c];
            if (m < 0.9f) g.color(lab::mix(theme_.panel, kPink, clampf((1.0f - m) * 2.0f, 0.2f, 1.0f)));
            else if (m > 1.1f) g.color(lab::mix(theme_.panel, kCyan, clampf((m - 1.0f), 0.2f, 1.0f)));
            else if (d > 2.0f || d < -2.0f) g.color(lab::mix(theme_.panel, kGold, clampf(fabsf(d) / 25.0f, 0.25f, 1.0f)));
            else continue;
            g.vtx(float(mx + c * ms) + 3.5f, float(my + r * ms) + 3.5f);
            ++drawn;
        }
    g.endBatch();
    // row locks and step locks
    const int lx = 548, ly = 252;
    g.color(kCyan);
    g.text(lx, ly, font::kSmall, "LOCKS", Canvas::kCenterY);
    for (int r = 0; r < kStepRows; ++r) {
        const bool on = (pat_->rnd.rowLocks >> r) & 1;
        const int item = n + kMetaCellsTotal + r;
        const int yy = ly + 22 + (r / 2) * 22;
        const int xx = lx + (r % 2) * 122;
        if (f == item) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(xx - 4, yy - 10, 118, 20, 4);
        }
        g.color(on ? kGold : theme_.panel);
        g.fill(xx, yy - 6, 12, 12);
        g.color(on ? theme_.fg : theme_.dim);
        g.text(xx + 18, yy, font::kTiny, kRowNames[r], Canvas::kCenterY);
    }
    g.color(theme_.dim);
    g.text(lx, ly + 140, font::kTiny, "STEP LOCKS (upper pads)", Canvas::kCenterY);
    g.beginRects();
    for (int s = 0; s < kSteps; ++s) {
        g.color(pat_->steps[s].lock ? kGold : theme_.panel);
        g.vtx(float(lx + s * 15), float(ly + 152));
        g.vtx(float(lx + s * 15 + 11), float(ly + 166));
    }
    g.endBatch();
}

// ---- COMBOS ------------------------------------------------------------------------------------------------
void Tilt::drawCombos(Canvas& g) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    for (int i = 0; i < kCombos; ++i) {
        const Combo& c = p.combos[i];
        const int y = 40 + i * 100;
        const bool sel = i == cbSel_;
        g.color(sel ? lab::scale(theme_.panel, 1.5f) : theme_.panel);
        g.fillRound(8, y, 784, 92, 10);
        if (sel) {
            g.color(kPink);
            g.fill(8, y + 10, 4, 72);
        }
        g.color(c.on ? kPink : theme_.dim);
        num(g, 44, y + 46, font::kHuge, i + 1, Canvas::kCenter);
        g.color(sel && cbField_ == 0 ? kCyan : theme_.dim);
        g.text(44, y + 78, font::kTiny, c.on ? "ON" : "off", Canvas::kCenter);
        // the path
        const int px = 110;
        g.lineWidth(2.0f);
        for (int k = 0; k < kComboLen; ++k) {
            const int cx = px + k * 82, cy = y + 46;
            const bool used = k < c.len;
            const bool lit = used && k < e.comboProgress[i];
            if (k && k < c.len) {
                g.color(lit ? kPink : theme_.dim);
                g.line(float(cx - 82 + 22), float(cy), float(cx - 22), float(cy));
            }
            if (used) {
                g.color(lit ? kPink : trigColor(p.steps[c.path[k] - 1].trig));
                g.point(float(cx), float(cy), 21.0f);
                if (!lit) {
                    g.color(theme_.bg);
                    g.point(float(cx), float(cy), 17.0f);
                }
                g.color(lit ? theme_.bg : theme_.fg);
                num(g, cx, cy, font::kLarge, c.path[k], Canvas::kCenter);
            } else {
                g.color(lab::scale(theme_.dim, 0.5f));
                g.ring(float(cx), float(cy), 20.0f, 16);
            }
            if (sel && cbField_ == k + 1) {
                g.color(kCyan);
                g.ring(float(cx), float(cy), 25.0f, 20);
            }
        }
        g.lineWidth(1.0f);
        // action
        char a[24], b[24];
        comboParamText(c, 0, a, sizeof a);
        comboParamText(c, 1, b, sizeof b);
        g.color(sel && cbField_ == 5 ? kCyan : kPink);
        g.text(450, y + 30, font::kLarge, kComboActionNames[c.action], Canvas::kCenterY);
        g.color(sel && cbField_ == 6 ? kCyan : theme_.fg);
        g.text(450, y + 62, font::kBody, a, Canvas::kCenterY);
        g.color(sel && cbField_ == 7 ? kCyan : theme_.fg);
        g.text(620, y + 62, font::kBody, b, Canvas::kCenterY);
        if (c.len < 2) {
            g.color(theme_.dim);
            g.text(620, y + 30, font::kTiny, "tap 2-4 step pads", Canvas::kCenterY);
        }
    }
}

// ---- CHANNELS ----------------------------------------------------------------------------------------------
void Tilt::drawChannels(Canvas& g) {
    const Pattern& p = *pat_;
    // tabs
    for (int i = 0; i < kCh; ++i) {
        const Channel& c = p.ch[i];
        const int x = i * 100;
        const bool sel = i == chSel_;
        g.color(sel ? lab::scale(theme_.panel, 1.6f) : theme_.panel);
        g.fill(x + 2, 34, 96, 44);
        g.color(lab::mix(lab::scale(chColor(i), 0.5f), Rgb(0xFFFFFF), float(chFlash_[i]) / 400.0f));
        g.fill(x + 2, 34, 96, sel ? 6 : 3);
        g.color(sel ? theme_.fg : theme_.dim);
        g.textf(x + 10, 54, font::kSmall, Canvas::kCenterY, "%d %s", i + 1, kEngineNames[c.type]);
        g.color(c.mute ? theme_.warn : theme_.dim);
        char nn[8];
        noteName(c.note, nn, sizeof nn);
        g.textf(x + 10, 70, font::kTiny, Canvas::kCenterY, "%s%s", nn, c.mute ? " MUTE" : "");
    }
    const Channel& c = p.ch[chSel_];
    // engine + knobs
    g.color(chColor(chSel_));
    g.text(16, 104, font::kTitle, kEngineNames[c.type], Canvas::kCenterY);
    g.color(chField_ == 0 ? kCyan : theme_.dim);
    g.text(16, 128, font::kTiny, chField_ == 0 ? "ENGINE  (encoder)" : "ENGINE", Canvas::kCenterY);
    static const char* const kSub[kEngines] = {"WAV one-shot / slices", "Plaits analog / synth bass drum",
                                               "Plaits analog / synth snare", "Plaits 808 metallic hat",
                                               "303-style acid voice", "two-operator FM"};
    g.color(theme_.dim);
    g.text(150, 104, font::kSmall, kSub[c.type], Canvas::kCenterY);
    for (int i = 0; i < kParamsPerEngine; ++i) {
        const EngParam& ep = kEngineParams[c.type][i];
        const int kx = 66 + (i % 4) * 110, ky = 200 + (i / 4) * 120;
        if (!ep.name) continue;
        const bool f = chField_ == i + 1;
        const float v = ep.max > ep.min ? float(c.p[i] - ep.min) / float(ep.max - ep.min) : 0.0f;
        g.lineWidth(4.0f);
        g.color(theme_.panel);
        g.arc(float(kx), float(ky), 30.0f, 2.356f, 7.069f, 14);
        g.color(f ? kCyan : chColor(chSel_));
        if (v > 0.01f) g.arc(float(kx), float(ky), 30.0f, 2.356f, 2.356f + 4.712f * v, 2 + int(v * 12.0f));
        g.lineWidth(1.0f);
        char val[16];
        engineParamText(c, i, val, sizeof val);
        g.color(theme_.fg);
        g.text(kx, ky, font::kSmall, val, Canvas::kCenter);
        g.color(f ? kCyan : theme_.dim);
        g.text(kx, ky + 44, font::kTiny, ep.name, Canvas::kCenter);
    }
    // common fields
    int ids[8];
    const int n = paramsOfPage(kPpChannel, ids, 8);
    const int rx = 480;
    g.color(kCyan);
    g.text(rx, 104, font::kSmall, "CHANNEL", Canvas::kCenterY);
    for (int i = 0; i < n; ++i) {
        const int y = 130 + i * 25;
        const bool f = chField_ == 9 + i;
        if (f) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(rx - 6, y - 12, 312, 24, 5);
        }
        char v[16];
        paramText(p, ids[i], chSel_, v, sizeof v);
        g.color(f ? theme_.fg : theme_.dim);
        g.text(rx, y, font::kSmall, kParams[ids[i]].name, Canvas::kCenterY);
        g.color(theme_.fg);
        g.text(rx + 296, y, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
    // sample: source, browser, waveform with slices
    if (c.type == kEnSample) {
        const int y = 266;
        const bool f = chField_ == 14;
        g.color(f ? kCyan : theme_.dim);
        g.text(rx, y, font::kSmall, "SAMPLE", Canvas::kCenterY);
        g.color(theme_.fg);
        const char* nm = c.sample[0] ? c.sample : kBuiltinNames[clampi(c.p[0], 0, kBuiltins - 1)];
        const char* base = std::strrchr(nm, '/');
        g.text(rx + 70, y, font::kSmall, base ? base + 1 : nm, Canvas::kCenterY);
        if (f) {
            if (!wavCount_) {
                g.color(theme_.dim);
                g.text(rx, y + 24, font::kTiny, ctx_->fs ? "no WAV in /Lab/TILT/Samples or /Samples" : "no card",
                       Canvas::kCenterY);
            }
            for (int k = -1; k <= 1 && wavCount_; ++k) {
                const int i = wavSel_ + k;
                if (i < 0 || i >= wavCount_) continue;
                const char* b = std::strrchr(wavs_[i], '/');
                g.color(k == 0 ? theme_.fg : theme_.dim);
                g.textf(rx, y + 24 + (k + 1) * 18, font::kTiny, Canvas::kCenterY, "%s %s", k == 0 ? ">" : " ",
                        b ? b + 1 : wavs_[i]);
            }
        }
        // waveform: 96 columns of peaks
        const SampleSlot* src = audio_.sourceOf(chSel_);
        const int wx = rx, wy = 380, ww = 300, wh = 52;
        g.color(theme_.panel);
        g.fill(wx, wy - wh / 2, ww, wh);
        if (src && src->ready && src->frames > 96) {
            g.color(lab::scale(chColor(chSel_), 0.9f));
            g.beginLines();
            const uint32_t per = src->frames / 96;
            for (int k = 0; k < 96; ++k) {
                int mx = 0;
                const uint32_t a = uint32_t(k) * per;
                for (uint32_t j = 0; j < per; j += (per > 64 ? per / 64 : 1)) {
                    const int v = src->data[a + j];
                    const int av = v < 0 ? -v : v;
                    if (av > mx) mx = av;
                }
                const float h = float(mx) / 32768.0f * float(wh / 2 - 2);
                const float xx = float(wx + 2 + k * 3);
                g.vtx(xx, float(wy) - h - 0.5f);
                g.vtx(xx, float(wy) + h + 0.5f);
            }
            g.endBatch();
            const int sl = clampi(c.p[1], 1, 16);
            if (sl > 1) {
                g.color(kGold);
                g.beginLines();
                for (int k = 1; k < sl; ++k) {
                    const float xx = float(wx) + float(ww) * float(k) / float(sl);
                    g.vtx(xx, float(wy - wh / 2));
                    g.vtx(xx, float(wy + wh / 2));
                }
                g.endBatch();
            }
        }
    }
    g.color(theme_.dim);
    g.text(16, 432, font::kTiny, "pads rows 1-3: play this channel   row 4: pick channel / KICK SNARE HAT ACID",
           Canvas::kCenterY);
}

// ---- MIX ---------------------------------------------------------------------------------------------------
void Tilt::drawMix(Canvas& g) {
    const Pattern& p = *pat_;
    for (int i = 0; i < kCh; ++i) {
        const Channel& c = p.ch[i];
        const int x = 10 + i * 70;
        const bool sel = i == mixCh_ && !mixFx_;
        g.color(sel ? lab::scale(theme_.panel, 1.5f) : theme_.panel);
        g.fillRound(x, 36, 64, 404, 6);
        g.color(chColor(i));
        g.fill(x + 4, 40, 56, 4);
        g.color(theme_.fg);
        g.textf(x + 32, 56, font::kSmall, Canvas::kCenter, "%d", i + 1);
        g.color(theme_.dim);
        g.text(x + 32, 74, font::kTiny, kEngineNames[c.type], Canvas::kCenter);
        // fader + meter
        const int fy = 90, fh = 190;
        const int lv = c.level * fh / 127;
        g.color(lab::scale(theme_.bg, 1.0f));
        g.fill(x + 14, fy, 16, fh);
        g.color(sel && mixRow_ == 0 ? kCyan : lab::scale(chColor(i), 0.8f));
        g.fill(x + 14, fy + fh - lv, 16, lv);
        const float pk = clampf(audio_.chPeak[i] * 1.4f, 0.0f, 1.0f);
        g.color(pk > 0.95f ? theme_.warn : theme_.ok);
        g.fill(x + 36, fy + fh - int(pk * float(fh)), 8, int(pk * float(fh)) + 1);
        g.color(theme_.fg);
        num(g, x + 32, fy + fh + 14, font::kTiny, c.level, Canvas::kCenter);
        // pan, sends
        const int rows[3] = {c.pan, c.sendA, c.sendB};
        static const char* const kN[3] = {"PAN", "A", "B"};
        for (int k = 0; k < 3; ++k) {
            const int y = 316 + k * 22;
            const bool f = sel && mixRow_ == k + 1;
            if (sel) {   // row names only on the selected strip (display budget)
                g.color(f ? kCyan : theme_.dim);
                g.text(x + 4, y, font::kTiny, kN[k], Canvas::kCenterY);
            }
            g.color(theme_.bg);
            g.fill(x + 26, y - 4, 34, 8);
            g.color(f ? kCyan : (k ? kViolet : theme_.fg));
            if (k == 0) g.fill(x + 26 + rows[k] * 30 / 127, y - 5, 4, 10);
            else g.fill(x + 26, y - 4, rows[k] * 34 / 127 + 1, 8);
        }
        // choke, stem
        if (c.choke || sel) {
            g.color(sel && mixRow_ == 4 ? kCyan : theme_.dim);
            g.textf(x + 4, 384, font::kTiny, Canvas::kCenterY, c.choke ? "C%d" : "C-", c.choke);
        }
        g.color(sel && mixRow_ == 5 ? kCyan : theme_.dim);
        g.textf(x + 60, 384, font::kTiny, Canvas::kCenterY | Canvas::kRight, "S%d", c.stem + 1);
        // mute, solo
        g.color(c.mute ? theme_.warn : (sel && mixRow_ == 6 ? lab::scale(kCyan, 0.5f) : theme_.bg));
        g.fillRound(x + 4, 400, 27, 30, 4);
        g.color(c.solo ? kGold : (sel && mixRow_ == 7 ? lab::scale(kCyan, 0.5f) : theme_.bg));
        g.fillRound(x + 33, 400, 27, 30, 4);
        g.color(c.mute ? theme_.bg : theme_.dim);
        g.text(x + 17, 415, font::kSmall, "M", Canvas::kCenter);
        g.color(c.solo ? theme_.bg : theme_.dim);
        g.text(x + 46, 415, font::kSmall, "S", Canvas::kCenter);
    }
    // FX
    int ids[16];
    const int n = paramsOfPage(kPpFx, ids, 16);
    drawParamList(g, ids, n, 584, 50, 210, mixFx_ ? parFocus_[2] : -1, 0);
    // master meter
    const float pk = clampf(audio_.outPeak, 0.0f, 1.0f);
    g.color(theme_.dim);
    g.text(584, 424, font::kTiny, "OUT", Canvas::kCenterY);
    g.color(theme_.bg);
    g.fill(620, 418, 160, 12);
    g.color(pk > 0.95f ? theme_.warn : theme_.ok);
    g.fill(620, 418, int(160.0f * pk) + 1, 12);
}

// ---- FILE / HELP ---------------------------------------------------------------------------------------------
void Tilt::drawFile(Canvas& g) {
    lab::drawHeader(g, theme_, *ctx_->shell, "Files   patterns, kits, capture");
    static const char* const kCol[4] = {"FACTORY", "USER PATTERNS", "KITS", "CAPTURE"};
    static const int kX[4] = {12, 220, 430, 600};
    for (int c = 0; c < 4; ++c) {
        g.color(fileCol_ == c ? kPink : kCyan);
        g.text(kX[c], 46, font::kSmall, kCol[c], Canvas::kCenterY);
    }
    for (int i = 0; i < kFactoryCount; ++i) {
        const int y = 70 + i * 24;
        const bool f = fileCol_ == 0 && fileSel_[0] == i;
        if (f) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(6, y - 11, 200, 22, 4);
        }
        g.color(f ? theme_.fg : theme_.dim);
        g.textf(14, y, font::kSmall, Canvas::kCenterY, "%2d %s", i + 1, kFactoryNames[i]);
    }
    for (int i = 0; i < kUserSlots; ++i) {
        const int y = 70 + i * 22;
        const bool f = fileCol_ == 1 && fileSel_[1] == i;
        if (f) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(214, y - 10, 200, 20, 4);
        }
        g.color(slotUsed_[i] ? (f ? theme_.fg : theme_.ok) : (f ? theme_.fg : lab::scale(theme_.dim, 0.7f)));
        g.textf(222, y, font::kSmall, Canvas::kCenterY, "U%02d %s%s", i + 1, slotUsed_[i] ? slotName_[i] : "--",
                activeSlot_ == i ? " *" : "");
    }
    for (int i = 0; i < kKitSlots; ++i) {
        const int y = 70 + i * 24;
        const bool f = fileCol_ == 2 && fileSel_[2] == i;
        if (f) {
            g.color(lab::scale(kPink, 0.35f));
            g.fillRound(424, y - 11, 160, 22, 4);
        }
        g.color(kitUsed_[i] ? (f ? theme_.fg : theme_.ok) : (f ? theme_.fg : lab::scale(theme_.dim, 0.7f)));
        g.textf(432, y, font::kSmall, Canvas::kCenterY, "KIT %d %s", i + 1, kitUsed_[i] ? "" : "--");
    }
    // capture
    const Engine& e = *eng_;
    g.color(fileCol_ == 3 ? theme_.fg : theme_.dim);
    g.textf(606, 74, font::kBody, Canvas::kCenterY, "last %d bar%s", capBars_, capBars_ > 1 ? "s" : "");
    g.color(theme_.dim);
    g.textf(606, 100, font::kSmall, Canvas::kCenterY, "clip: %d notes, %d bars", e.clipCount(), e.clipBars());
    g.textf(606, 122, font::kSmall, Canvas::kCenterY, "replay: %s", e.replaying() ? "ON" : "off");
    g.textf(606, 144, font::kSmall, Canvas::kCenterY, "heard: %d notes", e.capturedEvents());
    if (lastMidi_[0]) {
        const char* b = std::strrchr(lastMidi_, '/');
        g.color(theme_.ok);
        g.textf(606, 166, font::kSmall, Canvas::kCenterY, "saved %s", b ? b + 1 : lastMidi_);
    }
    // the selected factory pattern's idea
    g.color(theme_.panel);
    g.fillRound(424, 290, 368, 110, 8);
    g.color(kCyan);
    g.text(436, 306, font::kSmall, kFactoryNames[fileSel_[0]], Canvas::kCenterY);
    g.color(theme_.fg);
    g.text(436, 330, font::kTiny, kFactoryIdeas[fileSel_[0]], Canvas::kCenterY);
    g.color(theme_.dim);
    g.text(436, 356, font::kTiny, "pads: top row + 2 = factory, row 3 + 4 = user", Canvas::kCenterY);
    g.text(436, 374, font::kTiny, "Shift + pad = save there; row 2 right = kits", Canvas::kCenterY);
    static const char* const kKeys[8] = {"LOAD", "SAVE", "LOAD KIT", "SAVE KIT", "CAPTURE", "REPLAY", "MIDI FILE", "CLOSE"};
    lab::drawFKeyBar(g, theme_, kKeys, -1);
}

void Tilt::drawHelp(Canvas& g) {
    lab::drawHeader(g, theme_, *ctx_->shell, "Help");
    g.color(kPink);
    g.text(16, 50, font::kLarge, kPages_[page_].title, Canvas::kCenterY);
    g.color(theme_.fg);
    g.text(16, 78, font::kSmall, kPages_[page_].help, Canvas::kCenterY);
    static const char* const kLines[] = {
        "F1..F8          pages          Shift + F1..F8   the page's actions (shown while Shift is held)",
        "Play            launch / stop  Rec              capture the last bars and replay (again: stop)",
        "Note Instr FX1 FX2   flippers 1-4 (Shift = hard: may TILT)      Aux   nudge (Shift = hard)",
        "Performance (hold)   the perform pads on any page        Pattern   next matrix page A-D",
        "File            patterns, kits, capture, MIDI file    Master  MIX    S.Load  sample browser",
        "Arrows / encoder     move / change     Push   toggle, apply     Delete   reset value",
        "",
        "PERFORM PADS (GRAPH page)",
        "rows 1-2 left: 16 steps = flip the ball there      right: flippers 1-4, pages A-D",
        "row 3 left: channels (tap = play, Shift = mute)     right: fire combos 1-4",
        "row 4: relaunch, halt, +ball, nudge, capture 1/2/4/8 bars, replay, random, undo, TILT",
    };
    for (int i = 0; i < int(sizeof kLines / sizeof kLines[0]); ++i) {
        g.color(i == 7 ? kCyan : theme_.dim);
        g.text(16, 114 + i * 26, font::kSmall, kLines[i], Canvas::kCenterY);
    }
    g.color(theme_.dim);
    g.text(16, 436, font::kTiny, "any key closes", Canvas::kCenterY);
}

// ---- pad LEDs ---------------------------------------------------------------------------------------------
void Tilt::ledsPerform(Rgb* pads) {
    const Pattern& p = *pat_;
    const Engine& e = *eng_;
    const Ball& b = e.balls[0];
    for (int s = 0; s < kSteps; ++s) {
        const int pad = (s / 8) * 12 + (s % 8);
        const float h = clampf(e.heat[s] * 0.35f, 0.0f, 1.0f);
        const float odd = e.odds[s + 1];
        Rgb c = lab::scale(trigColor(p.steps[s].trig), 0.12f + 0.45f * h + 0.6f * odd);
        for (int k = 1; k < kBalls; ++k)
            if (e.balls[k].active && e.balls[k].state == s + 1) c = kBallColors[k];
        if (b.active && !b.resting && b.state == s + 1) c = Rgb(0xFFFFFF);
        pads[pad] = c;
    }
    for (int i = 0; i < kFlips; ++i) pads[8 + i] = e.tilted() ? Rgb(0x500000) : lab::scale(kGold, 0.8f);
    for (int i = 0; i < kPages; ++i) pads[20 + i] = p.ev.page == i ? kPink : lab::scale(kPink, 0.12f);
    for (int c = 0; c < kCh; ++c)
        pads[24 + c] = p.ch[c].mute ? Rgb(0x300000) : lab::scale(chColor(c), 0.15f + float(chFlash_[c]) / 300.0f);
    for (int i = 0; i < kCombos; ++i) {
        const Combo& cb = p.combos[i];
        const float k = cb.len >= 2 ? 0.15f + 0.85f * float(e.comboProgress[i]) / float(cb.len) : 0.03f;
        pads[32 + i] = lab::scale(kViolet, k);
    }
    static const Rgb kRow3[12] = {Rgb(0x30FF70), Rgb(0xFF3030), Rgb(0xFFC83A), Rgb(0xFF8020),
                                  Rgb(0x2EE6FF), Rgb(0x2EE6FF), Rgb(0x2EE6FF), Rgb(0x2EE6FF),
                                  Rgb(0x2E9AFF), Rgb(0xFF2E88), Rgb(0x7B5CFF), Rgb(0xFF0000)};
    for (int c = 0; c < 12; ++c) pads[36 + c] = lab::scale(kRow3[c], 0.35f);
    if (e.replaying()) pads[44] = kCyan;
    static const int kBars[4] = {1, 2, 4, 8};
    for (int i = 0; i < 4; ++i)
        if (kBars[i] == capBars_) pads[40 + i] = kCyan;
    if (e.tilted()) pads[47] = Rgb(0xFF0000);
}

void Tilt::leds(Rgb* pads) {
    for (int i = 0; i < lab::kPads; ++i) pads[i] = Rgb();
    const Pattern& p = *pat_;
    if (file_) {
        for (int i = 0; i < kFactoryCount; ++i) pads[i] = lab::scale(kCyan, 0.4f);
        for (int i = 0; i < kKitSlots; ++i) pads[16 + i] = kitUsed_[i] ? lab::scale(kGold, 0.6f) : lab::scale(kGold, 0.06f);
        for (int i = 0; i < kUserSlots; ++i)
            pads[(i < 12 ? 24 + i : 36 + i - 12)] = slotUsed_[i] ? lab::scale(theme_.ok, 0.6f) : lab::scale(theme_.ok, 0.05f);
        for (int i = 0; i < 4; ++i) pads[44 + i] = lab::scale(kCyan, 0.5f);
        return;
    }
    if (perform_ || page_ == kPgGraph) {
        ledsPerform(pads);
        return;
    }
    const Engine& e = *eng_;
    const int pg = clampi(p.ev.page, 0, kPages - 1);
    switch (page_) {
        case kPgMatrix: {
            for (int c = 1; c < kStates; ++c) {
                const int s = c - 1;
                const float w = float(p.w[pg][mxRow_][c]) / 100.0f;
                pads[(s / 8) * 12 + (s % 8)] = c == mxCol_ ? kCyan : lab::scale(kPink, 0.05f + 0.95f * w);
            }
            pads[8] = lab::scale(theme_.warn, 0.05f + 0.95f * float(p.w[pg][mxRow_][0]) / 100.0f);
            const int ballRow = e.balls[0].active ? e.balls[0].state : -1;
            for (int r = 1; r < kStates; ++r) {
                const int s = r - 1;
                pads[24 + (s / 8) * 12 + (s % 8)] = r == mxRow_ ? kCyan : (r == ballRow ? kGold : lab::scale(kViolet, 0.18f));
            }
            pads[32] = mxRow_ == 0 ? kCyan : lab::scale(kViolet, 0.18f);
            pads[20] = lab::scale(kViolet, 0.3f);
            pads[44] = lab::scale(theme_.warn, 0.3f);
            for (int r = 0; r < 4; ++r) {
                pads[r * 12 + 9] = mxField_ == r ? Rgb(0xFFFFFF) : lab::scale(kCyan, 0.2f);
                pads[r * 12 + 10] = lab::scale(kGold, 0.3f);
                pads[r * 12 + 11] = p.ev.page == r ? kPink : lab::scale(kPink, 0.12f);
            }
            break;
        }
        case kPgSteps: {
            const int ball = e.balls[0].active && !e.balls[0].resting ? e.balls[0].state - 1 : -1;
            for (int s = 0; s < kSteps; ++s) {
                const int pad = (s / 8) * 12 + (s % 8);
                pads[pad] = s == ball ? Rgb(0xFFFFFF) : (s == stStep_ ? kCyan : lab::scale(trigColor(p.steps[s].trig), 0.35f));
            }
            const int cur = p.steps[stStep_].trig;
            for (int c = 0; c < kCh; ++c) pads[24 + c] = lab::scale(chColor(c), cur == c + 1 ? 1.0f : 0.2f);
            pads[32] = cur == 0 ? Rgb(0x808080) : Rgb(0x101010);
            pads[33] = cur == kTrigMute ? Rgb(0xA0A0B0) : Rgb(0x181820);
            for (int r = 0; r < 10; ++r) pads[36 + r] = stRow_ == r ? kPink : lab::scale(kPink, 0.12f);
            pads[46] = stRow_ == kStepRows ? kCyan : lab::scale(kCyan, 0.15f);
            pads[47] = lab::scale(theme_.ok, 0.4f);
            break;
        }
        case kPgTable: {
            int ids[48];
            const int n = paramListOf(page_, ids, 48);
            for (int i = 0; i < n && i < 24; ++i) pads[i] = i == parFocus_[0] ? kCyan : lab::scale(kViolet, 0.2f);
            for (int i = 0; i < 4; ++i) pads[36 + i] = e.tilted() ? Rgb(0x500000) : kGold;
            pads[40] = lab::scale(Rgb(0xFF8020), 0.5f);
            pads[41] = lab::scale(theme_.ok, 0.5f);
            pads[42] = lab::scale(theme_.warn, 0.5f);
            pads[43] = lab::scale(kGold, 0.5f);
            pads[44] = e.tilted() ? Rgb(0xFF0000) : Rgb(0x400000);
            break;
        }
        case kPgEvolve: {
            for (int s = 0; s < kSteps; ++s) pads[(s / 8) * 12 + (s % 8)] = p.steps[s].lock ? kGold : lab::scale(kGold, 0.06f);
            for (int r = 0; r < kStepRows; ++r) pads[24 + r] = (p.rnd.rowLocks >> r) & 1 ? kGold : lab::scale(kCyan, 0.12f);
            pads[36] = kPink;
            pads[37] = kViolet;
            for (int i = 0; i < 5; ++i) pads[38 + i] = lab::scale(kPink, 0.35f);
            pads[43] = lab::scale(theme_.warn, 0.4f);
            for (int i = 0; i < kPages; ++i) pads[44 + i] = p.ev.page == i ? kPink : lab::scale(kPink, 0.1f);
            for (int i = 0; i < 8; ++i) {
                const int r = i / 4, c = i % 4;
                pads[r * 12 + 8 + c] = lab::scale(kViolet, 0.1f + 0.9f * float(p.ev.meta[r][c]) / 100.0f);
            }
            break;
        }
        case kPgCombos: {
            const Combo& cb = p.combos[cbSel_];
            for (int s = 0; s < kSteps; ++s) {
                int at = -1;
                for (int k = 0; k < cb.len; ++k)
                    if (cb.path[k] == s + 1) at = k;
                pads[(s / 8) * 12 + (s % 8)] = at >= 0 ? lab::scale(kPink, 0.4f + 0.2f * float(at)) : lab::scale(trigColor(p.steps[s].trig), 0.12f);
            }
            for (int i = 0; i < kCombos; ++i) pads[24 + i] = i == cbSel_ ? kCyan : lab::scale(kCyan, 0.15f);
            for (int a = 0; a < kComboActions; ++a) pads[28 + a] = cb.action == a ? kPink : lab::scale(kPink, 0.12f);
            for (int i = 0; i < kCombos; ++i)
                pads[36 + i] = lab::scale(kViolet, 0.2f + 0.8f * float(e.comboProgress[i]) / float(p.combos[i].len > 1 ? p.combos[i].len : 1));
            pads[46] = cb.on ? theme_.ok : Rgb(0x102010);
            pads[47] = lab::scale(theme_.warn, 0.4f);
            break;
        }
        case kPgChannels: {
            const Rgb c = chColor(chSel_);
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 12; ++k) {
                    const int idx = (2 - r) * 12 + k;
                    pads[r * 12 + k] = lab::scale(c, idx % 12 == 0 ? 0.7f : (padDown_[r * 12 + k] ? 1.0f : 0.14f));
                }
            for (int i = 0; i < kCh; ++i) pads[36 + i] = i == chSel_ ? Rgb(0xFFFFFF) : lab::scale(chColor(i), 0.3f + float(chFlash_[i]) / 400.0f);
            for (int i = 0; i < 4; ++i) pads[44 + i] = lab::scale(kGold, 0.25f);
            break;
        }
        case kPgMix: {
            for (int c = 0; c < kCh; ++c) {
                pads[c] = p.ch[c].mute ? theme_.warn : lab::scale(theme_.warn, 0.06f);
                pads[12 + c] = p.ch[c].solo ? kGold : lab::scale(kGold, 0.06f);
                pads[24 + c] = c == mixCh_ ? Rgb(0xFFFFFF) : lab::scale(chColor(c), 0.25f);
                pads[36 + c] = lab::scale(chColor(c), 0.12f + float(chFlash_[c]) / 300.0f);
            }
            for (int f = 0; f < 9; ++f) pads[(f / 4) * 12 + 8 + (f % 4)] = mixFx_ && parFocus_[2] == f ? kCyan : lab::scale(kViolet, 0.2f);
            break;
        }
        default: break;
    }
}

}  // namespace tilt
