// FLIPPER -- drawing: the boot intro, the playfield (hybrid and Markov views),
// the HUD, the editor, sounds, the table graph, the run page and help.
#include <cmath>
#include <cstdio>

#include "ws/flip_ws.h"

namespace flip {

using lab::Canvas;
using lab::Rgb;
namespace font = lab::font;

namespace {

const Rgb kCyan(0x00F0FF), kPink(0xFF2E97), kYellow(0xFFD400), kWhite(0xFFFFFF), kOrange(0xFF8A1E),
    kViolet(0x7A3CFF), kField(0x120726), kFieldEdge(0x2A1450), kWallC(0x241A4A), kDmd(0x1E0E00);

// ---- vector neon letters (4 x 6 grid) ---------------------------------------------------------
struct Stroke {
    int8_t x0, y0, x1, y1;
};
const Stroke kF[] = {{0, 0, 0, 6}, {0, 0, 4, 0}, {0, 3, 3, 3}};
const Stroke kL[] = {{0, 0, 0, 6}, {0, 6, 4, 6}};
const Stroke kI[] = {{2, 0, 2, 6}, {1, 0, 3, 0}, {1, 6, 3, 6}};
const Stroke kP[] = {{0, 6, 0, 0}, {0, 0, 4, 0}, {4, 0, 4, 3}, {4, 3, 0, 3}};
const Stroke kE[] = {{0, 0, 0, 6}, {0, 0, 4, 0}, {0, 3, 3, 3}, {0, 6, 4, 6}};
const Stroke kR[] = {{0, 6, 0, 0}, {0, 0, 4, 0}, {4, 0, 4, 3}, {4, 3, 0, 3}, {1, 3, 4, 6}};
struct Glyph {
    const Stroke* s;
    int n;
};
const Glyph kWord[7] = {{kF, 3}, {kL, 2}, {kI, 3}, {kP, 4}, {kP, 4}, {kE, 4}, {kR, 5}};

// Draws "FLIPPER" with letter cell size `u` (letter = 4u x 6u, gap 2u); `lit` = letters lit (0..7),
// `glow` draws a wide dim pass first.
void neonWord(Canvas& g, float x, float y, float u, int lit, Rgb on, Rgb off, bool glow) {
    for (int pass = glow ? 0 : 1; pass < 2; ++pass) {
        g.lineWidth(pass == 0 ? u * 1.1f : u * 0.38f);
        if (pass == 0) g.alpha(70);
        g.beginLines();
        for (int k = 0; k < 7; ++k) {
            g.color(k < lit ? (pass == 0 ? on : lab::mix(on, kWhite, 0.45f)) : off);
            const float lx = x + float(k) * 6.0f * u;
            for (int i = 0; i < kWord[k].n; ++i) {
                const Stroke& s = kWord[k].s[i];
                g.vtx(lx + float(s.x0) * u, y + float(s.y0) * u);
                g.vtx(lx + float(s.x1) * u, y + float(s.y1) * u);
            }
        }
        g.endBatch();
        if (pass == 0) g.alpha(255);
    }
    g.lineWidth(1.0f);
}

const char* elemIcon(uint8_t t) {
    switch (t) {
        case kHole: return "o";
        case kTarget: return "=";
        default: return "";
    }
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---- intro ----------------------------------------------------------------------------------------
void Flipper::drawIntro(Canvas& g, uint32_t t) {
    const float s = float(t) * 0.001f;
    // the cabinet: a dark playfield with neon rails
    g.color(kField);
    g.fillRound(40, 20, 720, 440, 18);
    g.color(kFieldEdge);
    g.lineWidth(3.0f);
    g.beginStrip();
    g.vtx(60, 440);
    g.vtx(60, 90);
    g.vtx(130, 30);
    g.vtx(670, 30);
    g.vtx(740, 90);
    g.vtx(740, 440);
    g.endBatch();
    g.lineWidth(1.0f);
    // plunger lane
    g.color(kWallC);
    g.fill(700, 100, 3, 340);
    // the ball path: plunger pull (0..0.5 s), shot up the lane (0.5..0.9), arc over the top (0.9..1.3),
    // through the logo left to right (1.3..2.0), down to the flippers (2.0..2.35), flip (2.35..2.7)
    float bx = 720, by = 420;
    int lit = 0;
    const float logoX = 190.0f, logoY = 170.0f, u = 13.0f;   // word width 7 * 6u - 2u = 520
    if (s < 0.5f) {
        by = 420.0f + 18.0f * (s / 0.5f);
    } else if (s < 0.9f) {
        const float k = (s - 0.5f) / 0.4f;
        by = 438.0f - k * k * 360.0f;
    } else if (s < 1.3f) {
        const float k = (s - 0.9f) / 0.4f, a = k * 1.5708f;
        bx = 720.0f - 70.0f * sinf(a) - 40.0f * k;
        by = 78.0f - 30.0f * sinf(a) + 40.0f * k * k;
    } else if (s < 2.0f) {
        const float k = (s - 1.3f) / 0.7f;
        bx = 610.0f - k * 480.0f;
        by = 120.0f + 60.0f * sinf(k * 9.42f) * (1.0f - k) + 80.0f * k;
        lit = clampi(int((610.0f - bx) / 74.0f) + 1, 0, 7);
        lit = 7 - (7 - lit);   // lights from the right
    } else if (s < 2.35f) {
        const float k = (s - 2.0f) / 0.35f;
        bx = 130.0f + k * 260.0f;
        by = 200.0f + k * k * 210.0f;
        lit = 7;
    } else {
        const float k = (s - 2.35f) / 0.35f;
        bx = 390.0f + k * 20.0f;
        by = 410.0f - k * 330.0f;
        lit = 7;
    }
    // letters light in the order the ball passes them (right to left while it travels left)
    int litMask = 0;
    if (s >= 1.3f) {
        for (int k = 0; k < 7; ++k) {
            const float lx = logoX + float(k) * 6.0f * u + 2.0f * u;
            if (s >= 2.0f || bx < lx + 30.0f) litMask |= 1 << k;
        }
    }
    (void)lit;
    // logo: unlit strokes first, then the lit ones bright (two calls keep it simple)
    for (int pass = 0; pass < 2; ++pass) {
        g.lineWidth(pass == 0 ? u * 1.2f : u * 0.4f);
        if (pass == 0) g.alpha(60);
        g.beginLines();
        for (int k = 0; k < 7; ++k) {
            const bool on = (litMask >> k) & 1;
            if (pass == 0 && !on) continue;
            g.color(on ? (pass == 0 ? kPink : Rgb(0xFFC0E4)) : Rgb(0x3A1838));
            const float lx = logoX + float(k) * 6.0f * u;
            for (int i = 0; i < kWord[k].n; ++i) {
                const Stroke& st = kWord[k].s[i];
                g.vtx(lx + float(st.x0) * u, logoY + float(st.y0) * u);
                g.vtx(lx + float(st.x1) * u, logoY + float(st.y1) * u);
            }
        }
        g.endBatch();
        if (pass == 0) g.alpha(255);
    }
    g.lineWidth(1.0f);
    // bumpers under the logo flash when the ball is near
    static const float kBump[3][2] = {{250, 330}, {400, 300}, {550, 330}};
    for (const auto& p : kBump) {
        const float d = sqrtf((bx - p[0]) * (bx - p[0]) + (by - p[1]) * (by - p[1]));
        const float f = d < 90.0f ? 1.0f - d / 90.0f : 0.0f;
        g.color(lab::mix(Rgb(0x30104A), kCyan, f));
        g.point(p[0], p[1], 22);
        g.color(lab::mix(Rgb(0x6020A0), kWhite, f));
        g.point(p[0], p[1], 9);
    }
    // flippers
    const bool up = s > 2.3f && s < 2.6f;
    g.color(kPink);
    g.lineWidth(7.0f);
    g.beginLines();
    g.vtx(330, 418);
    g.vtx(388, up ? 396 : 436);
    g.vtx(470, 418);
    g.vtx(412, up ? 396 : 436);
    g.endBatch();
    g.lineWidth(1.0f);
    // ball with a short trail
    g.alpha(90);
    g.color(kCyan);
    g.point(bx, by, 16);
    g.alpha(255);
    g.color(Rgb(0xE8E8F0));
    g.point(bx, by, 10);
    g.color(kWhite);
    g.point(bx - 3, by - 3, 3);
    // title card
    if (s > 2.05f) {
        const float k = (s - 2.05f) / 0.4f;
        g.color(lab::mix(kField, kYellow, k > 1.0f ? 1.0f : k));
        g.text(400, 290, font::kTitle, "THE SONG IS A PINBALL MACHINE", Canvas::kCenter);
    }
    g.color(Rgb(0x6A5A8A));
    g.text(400, 452, font::kTiny, "polyend-tracker-lab", Canvas::kCenter);
}

// ---- frame pieces -----------------------------------------------------------------------------------
void Flipper::drawTop(Canvas& g, const char* title) {
    g.color(Rgb(0x12082A));
    g.fill(0, 0, lab::kScreenW, 32);
    g.color(kPink);
    g.fill(0, 31, lab::kScreenW, 1);
    neonWord(g, 10, 7, 3.0f, 7, kPink, kPink, false);
    g.color(kCyan);
    g.text(150, 16, font::kBody, title, Canvas::kCenterY);
    const int ti = game_.tableIndex();
    const Rgb tc = tableColor(ti);
    g.color(lab::scale(tc, 0.3f));
    g.fillRound(300, 5, 160, 22, 6);
    g.color(tc);
    g.textf(380, 16, font::kSmall, Canvas::kCenter, "%d  %s", ti + 1, m_->tab[ti].name);
    lab::Shell& sh = *ctx_->shell;
    g.color(theme_.dim);
    g.textf(560, 16, font::kTiny, Canvas::kCenterY, "CPU %u%%", unsigned(sh.cpuPermille() / 10));
    g.color(theme_.fg);
    g.textf(690, 16, font::kBody, Canvas::kCenterY | Canvas::kRight, "%d BPM", int(sh.bpm() + 0.5f));
    if (sh.playing()) {
        g.color(theme_.ok);
        g.beginStrip();
        g.vtx(712, 8);
        g.vtx(726, 16);
        g.vtx(712, 24);
        g.vtx(712, 8);
        g.endBatch();
    } else {
        g.color(theme_.dim);
        g.fill(712, 10, 12, 12);
    }
    if (sh.recording()) {
        g.color(theme_.warn);
        g.point(760, 16, 7);
    } else {
        g.color(theme_.dim);
        g.ring(760, 16, 6, 10);
    }
}

void Flipper::drawList(Canvas& g, int x, int y, int w, int rows, int sel, int top, int count,
                       void (Flipper::*row)(int, char*, char*) const) {
    for (int i = 0; i < rows && top + i < count; ++i) {
        const int idx = top + i;
        const int yy = y + i * 30;
        char label[32], value[32];
        (this->*row)(idx, label, value);
        if (idx == sel) {
            g.color(Rgb(0x2A1250));
            g.fill(x, yy, w, 28);
            g.color(kCyan);
            g.fill(x, yy, 4, 28);
        }
        g.color(idx == sel ? kWhite : theme_.dim);
        g.text(x + 12, yy + 14, font::kSmall, label, Canvas::kCenterY);
        g.color(idx == sel ? kCyan : theme_.fg);
        g.text(x + w - 8, yy + 14, font::kSmall, value, Canvas::kCenterY | Canvas::kRight);
    }
    if (count > rows) {   // scroll bar
        const int h = rows * 30;
        const int bh = h * rows / count, by = y + h * top / count;
        g.color(Rgb(0x2A1250));
        g.fill(x + w + 3, y, 3, h);
        g.color(kPink);
        g.fill(x + w + 3, by, 3, bh < 8 ? 8 : bh);
    }
}

void Flipper::drawBanner(Canvas& g) {
    if (banner_ == kBnNone) return;
    const uint32_t age = now_ - bannerAt_;
    if (age > 1400) {
        banner_ = kBnNone;
        return;
    }
    Rgb c = kYellow;
    if (banner_ == kBnTilt || banner_ == kBnDrain || banner_ == kBnTiltWarn) c = Rgb(0xFF3B30);
    if (banner_ == kBnMultiball || banner_ == kBnJackpot) c = kPink;
    if (banner_ == kBnTable || banner_ == kBnLaunch) c = kCyan;
    const bool blink = banner_ != kBnLaunch && (age / 120) % 2 == 0 && age < 600;
    const char* text = kBannerText[banner_];
    char buf[40];
    if (banner_ == kBnTable) {
        std::snprintf(buf, sizeof buf, "TABLE %d  %s", game_.tableIndex() + 1, m_->tab[game_.tableIndex()].name);
        text = buf;
    }
    g.color(Rgb(0x000000));
    g.alpha(180);
    g.fillRound(140, 186, 340, 52, 10);
    g.alpha(255);
    g.color(blink ? kWhite : c);
    g.text(310, 212, font::kHuge, text, Canvas::kCenter);
}

// ---- the playfield ---------------------------------------------------------------------------------
void Flipper::drawField(Canvas& g, int ox, int oy, float sc, bool markov, bool editor) {
    const Table& t = game_.table();
    const float W = kFieldW * sc, H = kFieldH * sc, cw = kCellW * sc, ch = kCellH * sc;
    auto X = [&](float x) { return float(ox) + x * sc; };
    auto Y = [&](float y) { return float(oy) + y * sc; };
    const uint32_t steps = game_.steps();
    // board
    g.color(kField);
    g.fill(ox, oy, int(W), int(H));
    // heat trail (cells the balls have visited), Markov view and editor
    if (markov || editor) {
        uint32_t mx = 1;
        for (int c = 0; c < kCells; ++c) mx = game_.visits(c) > mx ? game_.visits(c) : mx;
        g.beginRects();
        for (int c = 0; c < kCells; ++c) {
            const uint32_t v = game_.visits(c);
            if (!v || t.cells[c].type == kWall) continue;
            g.color(lab::mix(kField, Rgb(0x5A1A6A), float(v) / float(mx)));
            const float x = X(float(cellCol(c)) * kCellW), y = Y(float(cellRow(c)) * kCellH);
            g.vtx(x + 1, y + 1);
            g.vtx(x + cw - 2, y + ch - 2);
        }
        g.endBatch();
    }
    // cell grid
    g.color(Rgb(0x24104A));
    g.beginLines();
    for (int c = 1; c < kCols; ++c) {
        g.vtx(X(float(c) * kCellW), Y(0));
        g.vtx(X(float(c) * kCellW), Y(kFieldH));
    }
    for (int r = 1; r < kRows; ++r) {
        g.vtx(X(0), Y(float(r) * kCellH));
        g.vtx(X(kFieldW), Y(float(r) * kCellH));
    }
    g.endBatch();
    // walls (blocks) and slopes / rails
    g.beginRects();
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        if (cl.type != kWall || cl.dir != kWallBlock) continue;
        g.color(kWallC);
        const float x = X(float(cellCol(c)) * kCellW), y = Y(float(cellRow(c)) * kCellH);
        g.vtx(x + 2, y + 2);
        g.vtx(x + cw - 3, y + ch - 3);
    }
    g.endBatch();
    g.color(kViolet);
    g.lineWidth(markov ? 2.0f : 3.0f);
    g.beginLines();
    for (int i = 0; i < game_.segments(); ++i) {
        float x0, y0, x1, y1;
        game_.segment(i, x0, y0, x1, y1);
        g.vtx(X(x0), Y(y0));
        g.vtx(X(x1), Y(y1));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    // ramps: lanes with an arrow, glowing while their effect sweeps
    g.lineWidth(2.5f * (sc < 0.8f ? 0.8f : 1.0f));
    g.beginLines();
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        if (cl.type != kRamp) continue;
        const bool on = game_.fxActiveUntil(c) > float(steps);
        g.color(on ? kWhite : kCyan);
        const float cx = X(cellX(c)), cy = Y(cellY(c));
        const int d = cl.dir < kDirCount ? int(cl.dir) : int(kN);
        const float dx = float(dirDx(d)), dy = float(dirDy(d));
        const float l = (d & 1) ? 0.7071f : 1.0f;
        const float ux = dx * l, uy = dy * l, r = 17.0f * sc;
        g.vtx(cx - ux * r, cy - uy * r);
        g.vtx(cx + ux * r, cy + uy * r);
        // arrow head
        const float px = -uy, py = ux;
        g.vtx(cx + ux * r, cy + uy * r);
        g.vtx(cx + ux * r * 0.3f + px * r * 0.55f, cy + uy * r * 0.3f + py * r * 0.55f);
        g.vtx(cx + ux * r, cy + uy * r);
        g.vtx(cx + ux * r * 0.3f - px * r * 0.55f, cy + uy * r * 0.3f - py * r * 0.55f);
    }
    g.endBatch();
    g.lineWidth(1.0f);
    // targets (rects)
    g.beginRects();
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        if (cl.type != kTarget) continue;
        const bool flash = game_.hitStep(c) && steps - game_.hitStep(c) < 2;
        g.color(flash ? kWhite : (game_.lit(c) ? kYellow : Rgb(0x6A5200)));
        const float cx = X(cellX(c)), cy = Y(cellY(c));
        g.vtx(cx - 17.0f * sc, cy - 5.0f * sc);
        g.vtx(cx + 17.0f * sc, cy + 5.0f * sc);
    }
    g.endBatch();
    // bumpers and holes (points)
    g.pointSize((kBumperR + 4.0f) * sc);
    g.alpha(110);
    g.beginPoints();
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        if (cl.type != kBumper) continue;
        const bool flash = game_.hitStep(c) && steps - game_.hitStep(c) < 2;
        g.color(flash ? kWhite : instColor(cl.inst));
        g.vtx(X(cellX(c)), Y(cellY(c)));
    }
    g.endBatch();
    g.alpha(255);
    g.pointSize(kBumperR * 0.72f * sc);
    g.beginPoints();
    for (int c = 0; c < kCells; ++c) {
        const Cell& cl = t.cells[c];
        if (cl.type == kBumper) {
            const bool flash = game_.hitStep(c) && steps - game_.hitStep(c) < 2;
            g.color(flash ? kWhite : lab::mix(instColor(cl.inst), kWhite, 0.35f));
            g.vtx(X(cellX(c)), Y(cellY(c)));
        }
    }
    g.endBatch();
    g.pointSize(kHoleR * sc);
    g.beginPoints();
    for (int c = 0; c < kCells; ++c)
        if (t.cells[c].type == kHole) {
            const bool flash = game_.hitStep(c) && steps - game_.hitStep(c) < 4;
            g.color(flash ? kPink : Rgb(0x6A1A60));
            g.vtx(X(cellX(c)), Y(cellY(c)));
        }
    g.endBatch();
    g.pointSize((kHoleR - 3.0f) * sc);
    g.color(Rgb(0x000000));
    g.beginPoints();
    for (int c = 0; c < kCells; ++c)
        if (t.cells[c].type == kHole) g.vtx(X(cellX(c)), Y(cellY(c)));
    g.endBatch();
    // flippers
    g.lineWidth(kFlipR * 1.6f * sc);
    g.beginLines();
    for (int c = 0; c < kCells; ++c) {
        if (t.cells[c].type != kFlipper) continue;
        float px, py, tx, ty;
        game_.flipperGeom(c, px, py, tx, ty);
        g.color(game_.flipperUp(t.cells[c].dir ? 1 : 0) ? kWhite : kPink);
        g.vtx(X(px), Y(py));
        g.vtx(X(tx), Y(ty));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    // labels: bumper notes, hole / target events (only at full size)
    if (sc > 0.9f) {
        for (int c = 0; c < kCells; ++c) {
            const Cell& cl = t.cells[c];
            if (cl.type == kBumper && cl.inst != kNone) {
                char nn[8];
                noteName(cl.note, nn);
                g.color(Rgb(0x10041E));
                g.text(int(X(cellX(c))), int(Y(cellY(c))), font::kTiny, nn, Canvas::kCenter);
            } else if (cl.type == kHole && cl.event != kEvNone) {
                g.color(kPink);
                g.text(int(X(cellX(c))), int(Y(cellY(c)) + 22.0f), font::kTiny, kEventNames[cl.event], Canvas::kCenter);
            } else if (cl.type == kRamp) {
                g.color(lab::scale(kCyan, 0.8f));
                g.text(int(X(cellX(c))), int(Y(cellY(c)) + 25.0f), font::kTiny, kFxNames[cl.fx], Canvas::kCenter);
            }
        }
        (void)elemIcon;
    }
    // Markov preview: arrows with probabilities from the first ball (next step + entropy)
    if (markov && !editor) {
        int shown = 0;
        for (int bi = 0; bi < kMaxBalls && shown < 2; ++bi) {
            const Ball& b = game_.balls[bi];
            if (!b.active) continue;
            const bool labels = shown == 0;
            ++shown;
            float w[5];
            moveWeights(t, b.cell, game_.magnet(), 0.6f, true, w);
            float sum = 0.0f;
            for (float v : w) sum += v;
            if (sum <= 0.0f) continue;
            static const int8_t kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
            const float cx = X(cellX(b.cell)), cy = Y(cellY(b.cell));
            for (int i = 0; i < 4; ++i) {
                const float p = w[i] / sum;
                if (p < 0.02f) continue;
                g.color(lab::mix(Rgb(0x402060), kCyan, p));
                g.lineWidth(1.0f + 6.0f * p);
                const float ex = cx + float(kDx[i]) * cw * 0.62f, ey = cy + float(kDy[i]) * ch * 0.62f;
                g.line(cx + float(kDx[i]) * 14.0f, cy + float(kDy[i]) * 14.0f, ex, ey);
                if (labels) {
                    g.color(kWhite);
                    g.textf(int(ex + float(kDx[i]) * 4.0f), int(ey + float(kDy[i]) * 9.0f), font::kTiny,
                            Canvas::kCenter, "%d", int(p * 100.0f + 0.5f));
                }
            }
            g.lineWidth(1.0f);
        }
    }
    // magnet
    if (game_.magnet() >= 0) {
        g.color(kCyan);
        g.ring(X(cellX(game_.magnet())), Y(cellY(game_.magnet())), 24.0f * sc, 16);
    }
    // balls: trails, grid shadow (hybrid), the ball
    const float phase = stepPhase_;
    for (int bi = 0; bi < kMaxBalls; ++bi) {
        const Ball& b = game_.balls[bi];
        if (!b.active) continue;
        float bx, by;
        if (b.phys && !markov) {
            bx = b.x;
            by = b.y;
            // the grid ball (what the pads show) as a square
            g.color(lab::scale(kCyan, 0.55f));
            const float gx = X(float(cellCol(b.cell)) * kCellW), gy = Y(float(cellRow(b.cell)) * kCellH);
            g.frame(int(gx + 3), int(gy + 3), int(cw - 6), int(ch - 6));
        } else {
            float k = b.movedAt == steps ? phase * 3.0f : 1.0f;
            if (k > 1.0f) k = 1.0f;
            const float k2 = 1.0f - (1.0f - k) * (1.0f - k);
            bx = cellX(b.prevCell) + (cellX(b.cell) - cellX(b.prevCell)) * k2;
            by = cellY(b.prevCell) + (cellY(b.cell) - cellY(b.prevCell)) * k2;
            if (b.hold) {
                g.color(kPink);
                g.ring(X(bx), Y(by), 16.0f * sc, 12);
            }
        }
        if (m_->g.trails && !editor) {
            // two bands (near / far), one size + colour each: 4 words + 1 per point
            g.alpha(120);
            for (int band = 0; band < 2; ++band) {
                g.pointSize((band ? kBallR * 0.45f : kBallR * 0.8f) * sc);
                g.color(band ? kViolet : kCyan);
                g.beginPoints();
                for (int k = 1 + band * (kTrail / 2); k < (band ? kTrail : kTrail / 2 + 1); ++k) {
                    const int idx = (trailHead_ - k + kTrail) % kTrail;
                    const float tx = trailX_[bi][idx], ty = trailY_[bi][idx];
                    if (tx < -100.0f) continue;
                    g.vtx(X(tx), Y(ty));
                }
                g.endBatch();
            }
            g.alpha(255);
        }
        g.color(Rgb(0xC8C8D8));
        g.point(X(bx), Y(by), kBallR * sc);
        g.color(kWhite);
        g.point(X(bx) - 2.0f * sc, Y(by) - 2.0f * sc, 2.5f * sc);
    }
    // frame
    g.color(markov ? kPink : kCyan);
    g.frame(ox - 1, oy - 1, int(W) + 2, int(H) + 2);
    if (game_.tiltLocked()) {
        g.color(Rgb(0xFF3B30));
        g.frame(ox - 3, oy - 3, int(W) + 6, int(H) + 6);
    }
}

void Flipper::drawHud(Canvas& g) {
    const int x = 622, w = 170;
    // DMD score
    g.color(kDmd);
    g.fillRound(x, 40, w, 74, 6);
    g.color(Rgb(0x5A2A00));
    g.text(x + 8, 50, font::kTiny, "SCORE", Canvas::kCenterY);
    g.textf(x + w - 8, 50, font::kTiny, Canvas::kCenterY | Canvas::kRight, "x%d", game_.multiplier());
    g.color(kOrange);
    g.textf(x + w - 8, 84, font::kHuge, Canvas::kCenterY | Canvas::kRight, "%lu",
            static_cast<unsigned long>(game_.score()));
    g.color(Rgb(0x8A4A10));
    g.textf(x + 8, 106, font::kTiny, Canvas::kCenterY, "table %lu", static_cast<unsigned long>(game_.tableScore()));
    // balls left + in play
    int y = 128;
    g.color(theme_.dim);
    g.text(x, y, font::kTiny, "BALLS", Canvas::kCenterY);
    const int left = game_.ballsLeft() > 9 ? 9 : game_.ballsLeft();
    g.pointSize(5.0f);
    g.beginPoints();
    for (int i = 0; i < left; ++i) {
        g.color(Rgb(0xC8C8D8));
        g.vtx(float(x + 56 + i * 13), float(y));
    }
    g.endBatch();
    if (game_.ballsLeft() >= 99) {
        g.color(Rgb(0xC8C8D8));
        g.text(x + 52, y, font::kTiny, "endless", Canvas::kCenterY);
    }
    g.color(kYellow);
    g.textf(x + w, y, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d in play", game_.ballsInPlay());
    // heat meter
    y = 152;
    g.color(theme_.dim);
    g.text(x, y, font::kTiny, "HEAT", Canvas::kCenterY);
    const float heat = game_.heatLevel();
    g.beginRects();
    for (int i = 0; i < 4; ++i) {
        const float f = heat - float(i);
        static const uint32_t kHeat[4] = {0x2EFF6A, 0xFFE600, 0xFF8A00, 0xFF2E2E};
        g.color(f >= 1.0f || i < game_.heat() ? Rgb(kHeat[i]) : (f > 0.0f ? lab::scale(Rgb(kHeat[i]), 0.25f + 0.5f * f)
                                                                       : Rgb(0x2A1A3A)));
        g.vtx(float(x + 50 + i * 30), float(y - 6));
        g.vtx(float(x + 50 + i * 30 + 26), float(y + 6));
    }
    g.endBatch();
    // combo / tilt
    y = 176;
    g.color(theme_.dim);
    g.text(x, y, font::kTiny, "COMBO", Canvas::kCenterY);
    g.color(game_.combo() > 8 ? kPink : theme_.fg);
    g.textf(x + 56, y, font::kSmall, Canvas::kCenterY, "%d", game_.combo());
    g.color(theme_.dim);
    g.text(x + 100, y, font::kTiny, "TILT", Canvas::kCenterY);
    for (int i = 0; i < 3; ++i) {
        g.color(i < game_.tiltMeter() || game_.tiltLocked() ? Rgb(0xFF3B30) : Rgb(0x3A1A2A));
        g.point(float(x + 138 + i * 12), float(y), 4);
    }
    // table, bar, next
    y = 204;
    const Table& t = game_.table();
    g.color(theme_.dim);
    const int bars = kLengthBars[t.lengthBars];
    if (bars)
        g.textf(x, y, font::kTiny, Canvas::kCenterY, "BAR %d / %d   %s", game_.bar() % bars + 1, bars,
                kRateNames[t.stepRate]);
    else
        g.textf(x, y, font::kTiny, Canvas::kCenterY, "BAR %d   %s", game_.bar() + 1, kRateNames[t.stepRate]);
    y = 224;
    const int pend = game_.pendingTable();
    if (pend >= 0 && pend != game_.tableIndex()) {
        g.color(tableColor(pend));
        g.textf(x, y, font::kSmall, Canvas::kCenterY, "NEXT > %d %s", pend + 1, m_->tab[pend].name);
    } else {
        g.color(Rgb(0x4A3A6A));
        g.text(x, y, font::kSmall, "NEXT > graph", Canvas::kCenterY);
    }
    // mode + entropy
    y = 252;
    const bool markov = m_->g.mode == kMarkov;
    g.color(markov ? kPink : kCyan);
    g.text(x, y, font::kSmall, markov ? "MARKOV" : "HYBRID", Canvas::kCenterY);
    if (game_.balls[0].active || game_.ballsInPlay()) {
        int bi = 0;
        while (bi < kMaxBalls && !game_.balls[bi].active) ++bi;
        if (bi < kMaxBalls) {
            float wts[5];
            moveWeights(t, game_.balls[bi].cell, game_.magnet(), 0.6f, markov, wts);
            g.color(theme_.dim);
            g.textf(x + w, y, font::kTiny, Canvas::kCenterY | Canvas::kRight, "H %.2f bit",
                    double(entropyBits(wts, 5)));
        }
    }
    // player / auto-player
    y = 276;
    if (game_.replaying()) {
        g.color(kYellow);
        g.textf(x, y, font::kSmall, Canvas::kCenterY, "REPLAY %lu/%lu", static_cast<unsigned long>(game_.clipPos()),
                static_cast<unsigned long>(m_->clipLen));
    } else if (game_.autoActive()) {
        g.color(Rgb(0x7CFF4F));
        g.textf(x, y, font::kSmall, Canvas::kCenterY, "AUTO-PLAYER %d%%", int(m_->g.autoSkill));
    } else {
        g.color(kWhite);
        g.text(x, y, font::kSmall, "PLAYER", Canvas::kCenterY);
    }
    // groups and effects
    y = 304;
    for (int gi = 0; gi < kGroups; ++gi) {
        const int yy = y + gi * 22;
        g.color(theme_.dim);
        g.text(x, yy, font::kTiny, kGroupNames[gi], Canvas::kCenterY);
        const float lv = audio_.groupLevel[gi];
        int bw = int(lv * 90.0f);
        bw = bw > 90 ? 90 : bw;
        g.color(Rgb(0x2A1A3A));
        g.fill(x + 44, yy - 4, 90, 8);
        g.color(gi == 0 ? kOrange : (gi == 1 ? kPink : kCyan));
        g.fill(x + 44, yy - 4, bw, 8);
        // the strongest effect on this group
        int best = -1;
        float bv = 0.05f;
        for (int f = 0; f < kFxCount; ++f)
            if (audio_.fxLevel(gi, f) > bv) {
                bv = audio_.fxLevel(gi, f);
                best = f;
            }
        if (best >= 0) {
            g.color(kWhite);
            g.text(x + w, yy, font::kTiny, kFxNames[best], Canvas::kCenterY | Canvas::kRight);
        }
    }
    // stepping dots: the current bar
    y = 380;
    const int spb = game_.stepsPerBar();
    const int pos = int(game_.stepIndex() % uint32_t(spb));
    const float dx = float(w - 8) / float(spb);
    g.pointSize(spb > 16 ? 2.0f : 3.0f);
    g.beginPoints();
    for (int i = 0; i < spb; ++i) {
        g.color(i == (pos + spb - 1) % spb ? kYellow : (i % 4 == 0 ? Rgb(0x6A5A8A) : Rgb(0x3A2A5A)));
        g.vtx(float(x + 4) + dx * (float(i) + 0.5f), float(y));
    }
    g.endBatch();
    g.color(Rgb(0x6A5A8A));
    g.textf(x, 404, font::kTiny, Canvas::kCenterY, "voices %d", audio_.voicesOn());
}

void Flipper::drawPlay(Canvas& g) {
    const bool markov = m_->g.mode == kMarkov;
    drawTop(g, markov ? "PLAY  markov" : "PLAY  hybrid");
    drawField(g, 10, 38, 1.0f, markov, false);
    drawHud(g);
    drawBanner(g);
    g.color(Rgb(0x6A5A8A));
    g.text(10, 432, font::kTiny,
           "Bottom pads / Left Right: flippers   Down: launch   Up: tilt   hold pad: magnet   Shift+pad: drop ball",
           Canvas::kCenterY);
    const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "New game", markov ? "> Hybrid" : "> Markov",
                           "Capture"};
    lab::drawFKeyBar(g, theme_, keys, 0);
}

void Flipper::drawEdit(Canvas& g) {
    drawTop(g, "EDIT");
    const float sc = 0.62f;
    drawField(g, 10, 40, sc, m_->g.mode == kMarkov, true);
    // the selected cell
    const float cw = kCellW * sc, ch = kCellH * sc;
    const int cx = 10 + int(float(cellCol(editCell_)) * cw), cy = 40 + int(float(cellRow(editCell_)) * ch);
    g.color((now_ / 300) & 1 ? kWhite : kCyan);
    g.lineWidth(2.0f);
    g.frame(cx, cy, int(cw), int(ch));
    g.lineWidth(1.0f);
    const Table& t = m_->tab[game_.tableIndex()];
    const Cell& c = t.cells[editCell_];
    // compass: the Markov weights of the cell and the entropy
    const int bx = 10, by = 290;
    g.color(Rgb(0x14082A));
    g.fillRound(bx, by, 372, 140, 8);
    g.color(kCyan);
    g.textf(bx + 10, by + 14, font::kSmall, Canvas::kCenterY, "CELL %c%d  %s", 'A' + cellRow(editCell_),
            cellCol(editCell_) + 1, kElemNames[c.type]);
    if (c.type != kWall) {
        float w[5];
        moveWeights(t, editCell_, -1, 0.0f, m_->g.mode == kMarkov, w);
        float sum = 0.0f;
        for (float v : w) sum += v;
        const float ox = float(bx + 90), oy = float(by + 80);
        static const int8_t kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
        static const char* const kD[4] = {"up", "right", "down", "left"};
        for (int i = 0; i < 4; ++i) {
            const float p = sum > 0.0f ? w[i] / sum : 0.0f;
            g.color(lab::mix(Rgb(0x402060), kCyan, p));
            g.lineWidth(1.5f + 7.0f * p);
            g.line(ox, oy, ox + float(kDx[i]) * 44.0f, oy + float(kDy[i]) * 40.0f);
            g.color(theme_.fg);
            g.textf(int(ox + float(kDx[i]) * 64.0f), int(oy + float(kDy[i]) * 50.0f), font::kTiny, Canvas::kCenter,
                    "%s %d%%", kD[i], int(p * 100.0f + 0.5f));
        }
        g.lineWidth(1.0f);
        g.color(kWhite);
        g.point(ox, oy, 6);
        g.color(theme_.dim);
        g.textf(bx + 210, by + 50, font::kSmall, Canvas::kCenterY, "entropy %.2f bits", double(entropyBits(w, 5)));
        g.textf(bx + 210, by + 74, font::kSmall, Canvas::kCenterY, "gravity %d", t.gravity);
        g.textf(bx + 210, by + 98, font::kSmall, Canvas::kCenterY, "visits %lu",
                static_cast<unsigned long>(game_.visits(editCell_)));
    }
    g.color(Rgb(0x6A5A8A));
    g.text(bx + 210, by + 124, font::kTiny, "Copy / Insert: cell  Shift: table", Canvas::kCenterY);
    drawList(g, 396, 42, 390, 12, editRow_, editTop_, editRows(), &Flipper::editRowText);
    g.color(Rgb(0x6A5A8A));
    g.text(400, 422, font::kTiny, "Pad: pick cell   Shift+pad: stamp   Up/Down: row   Encoder: value   Delete: clear",
           Canvas::kCenterY);
    const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "Element", "Drop ball",
                           confirm_ == 4 ? "Sure?" : "Clear"};
    lab::drawFKeyBar(g, theme_, keys, 1);
}

void Flipper::drawSounds(Canvas& g) {
    drawTop(g, "SOUNDS");
    // eight sound cards
    for (int i = 0; i < kInsts; ++i) {
        const Instrument& in = m_->inst[i];
        const int x = 10 + (i % 2) * 190, y = 42 + (i / 2) * 96;
        const bool sel = i == sndInst_;
        g.color(sel ? Rgb(0x2A1250) : Rgb(0x14082A));
        g.fillRound(x, y, 182, 88, 8);
        const Rgb c = instColor(i);
        g.color(c);
        g.point(float(x + 22), float(y + 24), 12);
        g.color(Rgb(0x10041E));
        g.textf(x + 22, y + 24, font::kSmall, Canvas::kCenter, "%d", i + 1);
        g.color(sel ? kWhite : theme_.fg);
        g.text(x + 42, y + 18, font::kBody, in.name, Canvas::kCenterY);
        g.color(theme_.dim);
        g.textf(x + 42, y + 40, font::kTiny, Canvas::kCenterY, "%s  %s", kEngineNames[in.engine],
                kGroupNames[in.group]);
        if (in.engine == kEnSample) {
            g.color(audio_.samples[i].frames ? Rgb(0x7CFF4F) : Rgb(0xFF3B30));
            g.text(x + 174, y + 40, font::kTiny, audio_.samples[i].frames ? "WAV" : "no WAV",
                   Canvas::kCenterY | Canvas::kRight);
        }
        // decay shape
        g.color(lab::scale(c, 0.8f));
        g.beginStrip();
        const float dec = 0.15f + float(in.decay) * 0.0085f;
        for (int k = 0; k <= 8; ++k) {
            const float tt = float(k) / 8.0f;
            g.vtx(float(x + 12) + tt * 158.0f, float(y + 80) - 26.0f * float(in.level) * 0.01f * expf(-tt / dec * 1.2f));
        }
        g.endBatch();
        if (sel) {
            g.color(kCyan);
            g.frame(x, y, 182, 88);
        }
    }
    drawList(g, 396, 42, 390, 12, sndRow_, sndTop_, 14, &Flipper::soundRowText);
    g.color(Rgb(0x6A5A8A));
    g.text(400, 422, font::kTiny, "Pads 1-8: pick sound   pad rows 2-4: play it   Shift+F8: rescan Samples/",
           Canvas::kCenterY);
    const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "Audition", "< Sound", "Sound >"};
    lab::drawFKeyBar(g, theme_, keys, 2);
}

void Flipper::drawGraph(Canvas& g, int cx, int cy, int rad) {
    float px[kTables], py[kTables];
    for (int i = 0; i < kTables; ++i) {
        const float a = -1.5708f + 6.2831853f * float(i) / float(kTables);
        px[i] = float(cx) + float(rad) * cosf(a) * 1.25f;
        py[i] = float(cy) + float(rad) * sinf(a);
    }
    const int cur = game_.tableIndex(), pend = game_.pendingTable();
    // edges: dim for every table, bright from the selected one
    for (int pass = 0; pass < 2; ++pass) {
        g.lineWidth(pass ? 2.5f : 1.0f);
        g.beginLines();
        for (int a = 0; a < kTables; ++a) {
            if (!m_->tab[a].enabled) continue;
            if ((a == tabSel_) != (pass == 1)) continue;
            for (int b = 0; b < kTables; ++b) {
                const int w = m_->tab[a].graph[b];
                if (!w || a == b || !m_->tab[b].enabled) continue;
                const float dx = px[b] - px[a], dy = py[b] - py[a];
                const float l = sqrtf(dx * dx + dy * dy);
                if (l < 1.0f) continue;
                const float ux = dx / l, uy = dy / l, nx = -uy * 4.0f, ny = ux * 4.0f;
                const float x0 = px[a] + ux * 40.0f + nx, y0 = py[a] + uy * 22.0f + ny;
                const float x1 = px[b] - ux * 44.0f + nx, y1 = py[b] - uy * 26.0f + ny;
                g.color(pass ? lab::mix(Rgb(0x402060), tableColor(a), 0.4f + 0.06f * float(w))
                             : Rgb(0x2E1A4A));
                g.vtx(x0, y0);
                g.vtx(x1, y1);
                if (pass) {   // arrow head
                    g.vtx(x1, y1);
                    g.vtx(x1 - ux * 9.0f - uy * 5.0f, y1 - uy * 9.0f + ux * 5.0f);
                    g.vtx(x1, y1);
                    g.vtx(x1 - ux * 9.0f + uy * 5.0f, y1 - uy * 9.0f - ux * 5.0f);
                }
            }
        }
        g.endBatch();
    }
    g.lineWidth(1.0f);
    // probabilities on the selected table's edges
    float sum = 0.0f;
    for (int b = 0; b < kTables; ++b)
        if (m_->tab[b].enabled) sum += float(m_->tab[tabSel_].graph[b]);
    for (int b = 0; b < kTables; ++b) {
        const int w = m_->tab[tabSel_].graph[b];
        if (!w || !m_->tab[b].enabled || sum <= 0.0f) continue;
        float lx, ly;
        if (b == tabSel_) {
            lx = px[b];
            ly = py[b] - 34.0f;
        } else {
            lx = px[tabSel_] + (px[b] - px[tabSel_]) * 0.55f;
            ly = py[tabSel_] + (py[b] - py[tabSel_]) * 0.55f;
        }
        g.color(Rgb(0x07030F));
        g.fillRound(int(lx) - 18, int(ly) - 9, 36, 18, 5);
        g.color(kWhite);
        g.textf(int(lx), int(ly), font::kTiny, Canvas::kCenter, "%d%%", int(float(w) * 100.0f / sum + 0.5f));
    }
    // boxes
    for (int i = 0; i < kTables; ++i) {
        const Table& t = m_->tab[i];
        const Rgb c = tableColor(i);
        const int x = int(px[i]) - 40, y = int(py[i]) - 20;
        g.color(t.enabled ? lab::scale(c, i == cur ? 0.55f : 0.18f) : Rgb(0x120A1A));
        g.fillRound(x, y, 80, 40, 7);
        if (i == tabSel_) {
            g.color(kWhite);
            g.frame(x - 2, y - 2, 84, 44);
        }
        if (i == pend && pend != cur) {
            g.color(kYellow);
            g.frame(x - 4, y - 4, 88, 48);
        }
        g.color(t.enabled ? (i == cur ? kWhite : c) : Rgb(0x4A3A5A));
        g.textf(x + 40, y + 14, font::kSmall, Canvas::kCenter, "%d %s", i + 1, t.name);
        if (t.graph[i] && t.enabled && i != tabSel_) {
            g.color(theme_.dim);
            g.text(x + 40, y + 31, font::kTiny, "loops", Canvas::kCenter);
        } else if (i == cur) {
            g.color(kWhite);
            g.text(x + 40, y + 31, font::kTiny, "playing", Canvas::kCenter);
        }
    }
}

void Flipper::drawTables(Canvas& g) {
    drawTop(g, "TABLES");
    g.color(Rgb(0x0E0620));
    g.fillRound(8, 38, 380, 384, 10);
    drawGraph(g, 198, 230, 140);
    drawList(g, 396, 42, 390, 12, tabRow_, tabTop_, 18 + kTables + 18 + 2, &Flipper::tableRowText);
    g.color(Rgb(0x6A5A8A));
    g.text(400, 422, font::kTiny, "Top pads: pick table (Shift: go now)   2nd row: graph weight to that table",
           Canvas::kCenterY);
    const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "Cue table", "Save table", "Next >"};
    lab::drawFKeyBar(g, theme_, keys, 3);
}

void Flipper::drawRun(Canvas& g) {
    drawTop(g, "RUN");
    const int x = 10, y = 42, w = 376, h = 150;
    g.color(Rgb(0x0E0620));
    g.fillRound(x, y, w, h + 120, 10);
    g.color(kCyan);
    g.text(x + 10, y + 14, font::kSmall, "CAPTURED RUN", Canvas::kCenterY);
    const uint32_t len = m_->clipLen;
    if (!len) {
        g.color(theme_.dim);
        g.text(x + w / 2, y + 80, font::kSmall, "Play a game, then F6 Capture", Canvas::kCenter);
    } else {
        g.color(theme_.dim);
        g.textf(x + w - 10, y + 14, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%lu steps",
                static_cast<unsigned long>(len));
        // timeline: 96 columns, table colour + note density
        constexpr int kColsT = 96;
        const float cw = float(w - 20) / float(kColsT);
        g.beginRects();
        for (int i = 0; i < kColsT; ++i) {
            const uint32_t a = uint32_t(i) * len / kColsT, b = uint32_t(i + 1) * len / kColsT;
            int notes = 0;
            for (uint32_t s = a; s < b && s < len; ++s)
                for (int k = 0; k < kMaxBalls; ++k) notes += m_->clip[s].note[k] ? 1 : 0;
            const uint32_t span = b > a ? b - a : 1;
            float d = float(notes) / float(span);
            d = d > 1.0f ? 1.0f : d;
            const Rgb c = tableColor(m_->clip[a < len ? a : len - 1].table);
            g.color(lab::scale(c, 0.25f));
            const float cx = float(x + 10) + float(i) * cw;
            g.vtx(cx, float(y + 30));
            g.vtx(cx + cw - 1.0f, float(y + h - 10));
            g.color(c);
            g.vtx(cx, float(y + h - 10) - d * float(h - 50));
            g.vtx(cx + cw - 1.0f, float(y + h - 10));
        }
        g.endBatch();
        // loop markers and playhead
        const float sx = float(w - 20) / float(len);
        g.color(kYellow);
        g.fill(x + 10 + int(float(clipA_) * sx), y + 26, 2, h - 32);
        g.fill(x + 10 + int(float(clipB_) * sx) - 2, y + 26, 2, h - 32);
        if (game_.replaying()) {
            g.color(kWhite);
            g.fill(x + 10 + int(float(game_.clipPos()) * sx), y + 22, 2, h - 26);
        }
    }
    g.color(theme_.fg);
    g.textf(x + 10, y + h + 18, font::kSmall, Canvas::kCenterY, "This game: %lu steps, score %lu",
            static_cast<unsigned long>(game_.runCount() - game_.runGameStart()),
            static_cast<unsigned long>(game_.score()));
    g.color(theme_.dim);
    g.text(x + 10, y + h + 44, font::kTiny, "F6 copies the run into the clip (the rows say how much).",
           Canvas::kCenterY);
    g.text(x + 10, y + h + 62, font::kTiny, "F7 replays it in a loop: same balls, same notes, every time.",
           Canvas::kCenterY);
    g.text(x + 10, y + h + 80, font::kTiny, "Pads jump inside the clip. Shift+Rec records audio + stems.",
           Canvas::kCenterY);
    g.text(x + 10, y + h + 98, font::kTiny, "F8 saves the machine (tables, sounds, clip) to the card.",
           Canvas::kCenterY);
    // machine slots
    g.color(kCyan);
    g.text(x, 336, font::kSmall, "MACHINES", Canvas::kCenterY);
    for (int i = 0; i < kSlots; ++i) {
        const int sx = x + i * 47, sy = 352;
        g.color(i == slot_ ? kPink : (slotUsed_[i] ? Rgb(0x3A1A5A) : Rgb(0x1A0E2A)));
        g.fillRound(sx, sy, 42, 36, 6);
        g.color(slotUsed_[i] ? kWhite : theme_.dim);
        g.textf(sx + 21, sy + 18, font::kBody, Canvas::kCenter, "%d", i + 1);
    }
    drawList(g, 396, 42, 390, 6, runRow_, 0, 6, &Flipper::runRowText);
    const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "Capture",
                           game_.replaying() ? "Stop replay" : "Replay", "Save"};
    lab::drawFKeyBar(g, theme_, keys, 4);
}

void Flipper::drawHelp(Canvas& g) {
    g.color(Rgb(0x07030F));
    g.alpha(235);
    g.fillRound(30, 44, 740, 370, 12);
    g.alpha(255);
    g.color(kPink);
    g.frame(30, 44, 740, 370);
    g.color(kYellow);
    g.text(400, 64, font::kBody, "FLIPPER  -  HELP  (Help closes)", Canvas::kCenter);
    static const char* const kPlay[] = {
        "Play starts the game (Play button). Each table is a part of the song.",
        "Bumpers play sounds (+ MIDI notes), ramps sweep effects, holes / targets fire events.",
        "Bottom pad row or Left / Right: flippers.  Down or encoder push: launch.  Up: tilt.",
        "Hold a pad: magnet pulls the ball there.  Shift + pad: drop an extra ball.",
        "Let go and the auto-player takes over (Config: skill, delay).",
        "Encoder: cue the next table.  F7: hybrid physics <-> Markov only.  F8: capture the run.",
        "Hold Note / Instr / FX1 / FX2 / Pattern: filter / crush / stutter / delay / reverb throw.",
        "Shift + Rec: record audio (Config > Record stems: drums, bass, keys, effects).",
        "Config: settings, firmware update, about.  Delete: stop the balls."};
    static const char* const kEdit[] = {
        "Pads pick a cell of the table.  F6 / encoder on Element changes what is there.",
        "Up / Down: row.  Encoder or Left / Right: value.  Shift: x10 / octave.",
        "Move up / right / down / left: the Markov weights of the cell (0 = never).",
        "Bumper: sound, note, velocity, pitch mode (fixed / walk / combo / ball / random).",
        "Ramp: effect, amount, sweep length, groups, throw direction.",
        "Hole / target: event (graph, go to, multiball, ball save, multiply, mutate, extra ball, jackpot).",
        "Flipper: side, power.  Wall: block or slope.",
        "Shift + pad: stamp the cell.  Copy / Insert: cell (Shift: whole table).  Delete: clear the cell.",
        "F7 drops a ball on the selected cell.  F8 twice clears the table."};
    static const char* const kSnd[] = {
        "Eight sounds. Pads 1-8 pick one, the lower three pad rows play it chromatically.",
        "Engines: kick, snare, hat, zap, pluck, bell, lead (bass), sampler.",
        "Sampler: put WAV files in Lab/FLIPPER/Samples on the card, pick one in the Sample row.",
        "Group: drums / bass / keys -- ramps and stems work per group.",
        "MIDI out: off, base channel (Config), or a channel of its own."};
    static const char* const kTab[] = {
        "Eight tables = eight song parts. The graph picks the next table when a table ends.",
        "Graph weights 0..9 from the selected table to each table (2nd pad row cycles them).",
        "A table ends at its length, on game over, or by events (holes, jackpot, multiball).",
        "Step, swing, scale, root, gravity, balls, ball save, rails (Euclidean backbeat by heat).",
        "F6 cues the selected table (Shift: now).  F7 saves it to Tables/, the last rows load presets or files."};
    static const char* const kRun[] = {
        "Every step of the game is recorded in memory.  F6 copies it into the clip.",
        "F7 replays the clip in a loop: a fixed sequence you can trust in a set.",
        "Clip start / end set the loop.  Pads jump to 48 points of the clip.",
        "Replay sound: as recorded, or with the table's current sounds (edit while it loops).",
        "Machine slot + F8 saves; push the encoder on the slot row twice to load."};
    const char* const* lines = kPlay;
    int n = 9;
    if (page_ == kPgEdit) { lines = kEdit; n = 9; }
    if (page_ == kPgSounds) { lines = kSnd; n = 5; }
    if (page_ == kPgTables) { lines = kTab; n = 5; }
    if (page_ == kPgRun) { lines = kRun; n = 5; }
    g.color(theme_.fg);
    for (int i = 0; i < n; ++i) g.text(50, 100 + i * 32, font::kSmall, lines[i], Canvas::kCenterY);
}

void Flipper::draw(Canvas& g) {
    if (help_) {   // the help card replaces the page (keeps the frame small)
        static const char* const kTitles[kPgCount] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN"};
        drawTop(g, kTitles[page_]);
        drawHelp(g);
        const char* keys[8] = {"PLAY", "EDIT", "SOUNDS", "TABLES", "RUN", "", "", ""};
        lab::drawFKeyBar(g, theme_, keys, page_);
        return;
    }
    switch (page_) {
        case kPgPlay: drawPlay(g); break;
        case kPgEdit: drawEdit(g); break;
        case kPgSounds: drawSounds(g); break;
        case kPgTables: drawTables(g); break;
        default: drawRun(g); break;
    }
    if (page_ != kPgPlay) drawBanner(g);
}

void Flipper::drawKeys(Canvas&, const char* const*) {}

}  // namespace flip
