// MAKAM-JUNGLE -- drawing: the usul dial, the perde ring, the break slicer,
// the seyir rose, synth, arrangement, mixer, files and the boot intro.
// Every page stays well under Canvas::kBudgetWords (sim "budget").
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/gen.h"
#include "ws/ws.h"

namespace makam {

using namespace lab;

namespace {
constexpr float kPi = 3.14159265f;
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// angle of position f (0..1) on a dial, 0 at 12 o'clock, clockwise
inline float ang(float f) { return (f - 0.25f) * 2.0f * kPi; }
const char* const kPageNames[kPages] = {"Usul", "Makam", "Break", "Seyir", "Synth", "Arrange", "Mixer", "File"};

Rgb strokeCol(int s) {
    return s == kDum ? col::kRed : s == kTek ? col::kGold : s == kKe ? col::kTurq : col::kMuted;
}
Rgb degreeCol(const Makam& m, int deg) {
    int d = deg % 7;
    if (d < 0) d += 7;
    if (deg == -1) return col::kViolet;
    if (d == 0) return col::kRed;
    if (d == m.guclu % 7) return col::kGold;
    return col::kCobalt;
}
// Eight-point Iznik star (two squares) as one line strip pair.
void star(Canvas& g, float cx, float cy, float r, float rot) {
    for (int sq = 0; sq < 2; ++sq) {
        g.beginStrip();
        for (int i = 0; i <= 4; ++i) {
            const float a = rot + float(sq) * kPi * 0.25f + float(i) * kPi * 0.5f;
            g.vtx(cx + r * cosf(a), cy + r * sinf(a));
        }
        g.endBatch();
    }
}
}  // namespace

float MakamWs::playFrac() const {
    const int len = usulLength(song_->usul);
    if (!ctx_->shell || !ctx_->playing() || eng_.st.unit < 0) return -1.0f;
    const double ub = song_->unit ? 0.5 : 0.25;
    const double u = ctx_->shell->transport().beat / ub;
    const long ui = long(u);
    const float fr = float(u - double(ui));
    return (float(ui % len) + fr) / float(len);
}

// ---------------------------------------------------------------------------
void MakamWs::drawTop(Canvas& g, const char* title) {
    drawHeader(g, theme_, *ctx_->shell, title);
    // makam / usul badge in the header
    g.color(col::kGold);
    g.textf(300, 15, font::kSmall, Canvas::kCenterY, "%s  %s", kMakams[song_->makam].name, kUsuls[song_->usul].meter);
}

void MakamWs::drawRows(Canvas& g, int x, int y, int w, int rowH, int first, int count) {
    const int sel = row_[page_];
    for (int i = 0; i < count && first + i < rowCount_; ++i) {
        const int idx = first + i;
        const PRow& r = rows_[idx];
        const int yy = y + i * rowH;
        if (idx == sel) {
            g.color(col::kPanel2);
            g.fill(x, yy, w, rowH - 2);
            g.color(col::kTurq);
            g.fill(x, yy, 3, rowH - 2);
        }
        g.color(idx == sel ? col::kCream : col::kDim);
        g.text(x + 10, yy + rowH / 2 - 1, font::kSmall, r.label, Canvas::kCenterY);
        char v[40];
        const int val = r.v ? *r.v : 0;
        if (r.names) copyStr(v, sizeof v, r.names[clampi(val - r.min, 0, r.max - r.min)]);
        else std::snprintf(v, sizeof v, "%d", val);
        g.color(idx == sel ? col::kTurq : col::kCream);
        g.text(x + w - 8, yy + rowH / 2 - 1, font::kSmall, v, Canvas::kCenterY | Canvas::kRight);
    }
}

// The usul as a clock dial: unit ticks on the rim, group arcs, stroke dots,
// the sweeping playhead.
void MakamWs::drawDial(Canvas& g, float cx, float cy, float r, bool showVar, int selStep) {
    const Song& s = *song_;
    const int len = usulLength(s.usul);
    int starts[8];
    const int groups = usulGroups(s.usul, starts, 8);
    const BreakVar& bv = s.brk[s.editVar];
    // background disc + tile ring
    g.color(col::kPanel);
    g.point(cx, cy, r + 14.0f);
    g.color(col::kNight);
    g.point(cx, cy, r - 18.0f);
    // group arcs (alternating cobalt / turquoise)
    g.lineWidth(3.0f);
    for (int k = 0; k < groups; ++k) {
        const int a = starts[k], b = k + 1 < groups ? starts[k + 1] : len;
        g.color(k & 1 ? col::kTurq : col::kCobalt);
        g.arc(cx, cy, r + 6.0f, ang(float(a) / float(len)) + 0.04f, ang(float(b) / float(len)) - 0.04f, 10);
    }
    g.lineWidth(1.0f);
    // unit ticks
    g.color(col::kDim);
    g.beginLines();
    for (int i = 0; i < len; ++i) {
        const float a = ang(float(i) / float(len));
        g.vtx(cx + (r - 10.0f) * cosf(a), cy + (r - 10.0f) * sinf(a));
        g.vtx(cx + (r + 2.0f) * cosf(a), cy + (r + 2.0f) * sinf(a));
    }
    g.endBatch();
    // stroke dots: dum big red, tek gold, ke small turquoise
    const int cur = eng_.st.unit >= 0 && ctx_->playing() ? eng_.st.step : -1;
    for (int i = 0; i < len; ++i) {
        const int st = showVar ? bv.s[i].stroke : strokeOf(kUsuls[s.usul].strokes[i]);
        const float a = ang(float(i) / float(len));
        const float x = cx + (r - 34.0f) * cosf(a), y = cy + (r - 34.0f) * sinf(a);
        const float rad = st == kDum ? 17.0f : st == kTek ? 13.0f : st == kKe ? 9.0f : 4.0f;
        if (i == selStep) {
            g.color(col::kCream);
            g.ring(x, y, rad + 5.0f, 12);
        }
        g.color(i == cur ? col::kCream : strokeCol(st));
        g.point(x, y, rad);
        if (showVar && (bv.s[i].roll > 1 || bv.s[i].rev || bv.s[i].stutter || bv.s[i].pitch)) {
            g.color(col::kViolet);
            g.point(cx + (r - 64.0f) * cosf(a), cy + (r - 64.0f) * sinf(a), 3.5f);
        }
    }
    // playhead
    const float f = playFrac();
    if (f >= 0.0f) {
        const float a = ang(f);
        g.color(col::kSaffron);
        g.lineWidth(2.5f);
        g.line(cx, cy, cx + (r + 12.0f) * cosf(a), cy + (r + 12.0f) * sinf(a));
        g.lineWidth(1.0f);
    }
    g.color(col::kGold);
    g.point(cx, cy, 6.0f);
}

// ---- pages --------------------------------------------------------------------------
void MakamWs::drawUsul(Canvas& g) {
    const Song& s = *song_;
    const Usul& u = kUsuls[s.usul];
    drawDial(g, 230.0f, 245.0f, 175.0f, true, -1);
    g.color(col::kCream);
    g.text(230, 230, font::kTitle, u.name, Canvas::kCenter);
    g.color(col::kGold);
    g.textf(230, 262, font::kBody, Canvas::kCenter, "%s  %s", u.meter, u.groups);
    // right panel
    g.color(col::kPanel);
    g.fill(460, 40, 330, 400);
    g.color(col::kDim);
    g.text(475, 56, font::kTiny, "USUL / TEMPO", 0);
    drawRows(g, 466, 72, 318, 30, 0, rowCount_);
    // stroke legend + pattern string
    int y = 300;
    g.color(col::kRed);
    g.point(482, float(y), 7);
    g.color(col::kCream);
    g.text(495, y, font::kSmall, "Dum: kick slices", Canvas::kCenterY);
    g.color(col::kGold);
    g.point(482, float(y + 22), 6);
    g.color(col::kCream);
    g.text(495, y + 22, font::kSmall, "Tek: snare slices", Canvas::kCenterY);
    g.color(col::kTurq);
    g.point(482, float(y + 44), 4);
    g.color(col::kCream);
    g.text(495, y + 44, font::kSmall, "Ke: ghost slices", Canvas::kCenterY);
    g.color(col::kDim);
    g.textf(475, 380, font::kSmall, 0, "Kudum: %s", u.strokes);
    const int sec = eng_.st.section;
    g.textf(475, 404, font::kSmall, 0, "%s  %s  cycle %d", s.songMode ? "SONG" : "LOOP",
            kSectionNames[s.sec[clampi(sec, 0, kSections - 1)].type], int(eng_.st.secCycle) + 1);
}

void MakamWs::drawMakam(Canvas& g) {
    const Song& s = *song_;
    const Makam& m = kMakams[s.makam];
    const float cx = 230.0f, cy = 245.0f, R = 165.0f;
    const Tuning tu = s.tuning ? kTune53 : kTuneAEU;
    // the octave ring starts at the karar's pitch class (12 o'clock)
    const int base = m.asc[0];
    auto posA = [&](int comma) { return ang(float(((comma - base) % 53 + 53) % 53) / 53.0f); };
    g.color(col::kPanel);
    g.point(cx, cy, R + 22.0f);
    g.color(col::kNight);
    g.point(cx, cy, R - 6.0f);
    // 53 comma ticks
    g.color(col::kMuted);
    g.beginLines();
    for (int c = 0; c < 53; ++c) {
        const float a = posA(base + c);
        const float r0 = isAeuPitch(base + c) ? R - 2.0f : R + 6.0f;
        g.vtx(cx + r0 * cosf(a), cy + r0 * sinf(a));
        g.vtx(cx + (R + 14.0f) * cosf(a), cy + (R + 14.0f) * sinf(a));
    }
    g.endBatch();
    // AEU 24 positions (or every comma in 53-TET)
    g.color(tu == kTune53 ? col::kDim : col::kCobalt);
    g.pointSize(2.5f);
    g.beginPoints();
    for (int c = 0; c < 53; ++c)
        if (tu == kTune53 || isAeuPitch(base + c)) {
            const float a = posA(base + c);
            g.vtx(cx + (R + 4.0f) * cosf(a), cy + (R + 4.0f) * sinf(a));
        }
    g.endBatch();
    // the makam polygon (ascending degrees) and its perdes
    g.color(col::kTurq);
    g.lineWidth(1.5f);
    g.beginStrip();
    for (int d = 0; d <= 7; ++d) {
        const float a = posA(m.asc[d % 7]);
        g.vtx(cx + (R - 44.0f) * cosf(a), cy + (R - 44.0f) * sinf(a));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    // descending differences (dashed in violet)
    for (int d = 0; d < 8; ++d)
        if (m.desc[d] != m.asc[d]) {
            const float a = posA(m.desc[d]);
            g.color(col::kViolet);
            g.point(cx + (R - 44.0f) * cosf(a), cy + (R - 44.0f) * sinf(a), 6.0f);
        }
    const int sounding = eng_.st.leadDeg != -100 ? eng_.st.leadComma : -1000;
    for (int d = 0; d < 7; ++d) {
        const int c = m.asc[d];
        const float a = posA(c);
        const float x = cx + (R - 44.0f) * cosf(a), y = cy + (R - 44.0f) * sinf(a);
        const bool on = sounding != -1000 && ((sounding - c) % 53 + 53) % 53 == 0;
        g.color(on ? col::kCream : degreeCol(m, d));
        g.point(x, y, d == 0 || d == m.guclu ? 11.0f : 8.0f);
        const int idx = aeuIndex(c);
        g.color(col::kCream);
        g.text(int(cx + (R + 34.0f) * cosf(a)), int(cy + (R + 30.0f) * sinf(a)), font::kTiny, idx >= 0 ? kAeuShort[idx] : "?",
               Canvas::kCenter);
        // step size in commas between this and the next degree
        const int st = stepCommas(m, d);
        const float am = posA(c) + float(st) / 53.0f * kPi;
        g.color(col::kGold);
        g.number(int(cx + (R - 74.0f) * cosf(am)), int(cy + (R - 74.0f) * sinf(am)), font::kTiny, st, Canvas::kCenter);
    }
    // yeden
    {
        const float a = posA(m.yeden);
        g.color(col::kViolet);
        g.ring(cx + (R - 44.0f) * cosf(a), cy + (R - 44.0f) * sinf(a), 7.0f, 10);
    }
    g.color(col::kCream);
    g.text(int(cx), int(cy) - 12, font::kTitle, m.name, Canvas::kCenter);
    g.color(col::kDim);
    g.text(int(cx), int(cy) + 18, font::kTiny, tu == kTune53 ? "53-TET" : "Arel-Ezgi-Uzdilek", Canvas::kCenter);

    // right panel
    g.color(col::kPanel);
    g.fill(460, 40, 330, 400);
    drawRows(g, 466, 46, 318, 26, 0, rowCount_);
    char n1[28], n2[28], n3[28];
    int y = 162;
    g.color(col::kDim);
    g.text(475, y, font::kTiny, m.parts, 0);
    y += 22;
    g.color(col::kRed);
    g.textf(475, y, font::kSmall, 0, "Karar  %s", perdeName(m.asc[0], n1, sizeof n1));
    g.color(col::kGold);
    g.textf(475, y + 20, font::kSmall, 0, "Guclu  %s", perdeName(m.asc[m.guclu], n2, sizeof n2));
    g.color(col::kViolet);
    g.textf(475, y + 40, font::kSmall, 0, "Yeden  %s", perdeName(m.yeden, n3, sizeof n3));
    g.color(col::kCream);
    g.textf(475, y + 60, font::kSmall, 0, "Seyir  %s", seyirName(m.seyir));
    // degree table: name / cents above the karar (practice intonation applied)
    y += 86;
    const bool prac = s.intonation != 0;
    for (int d = 0; d < 8; ++d) {
        const int c0 = m.asc[d];
        const int c = prac ? practiceComma(s.makam, c0) : c0;
        const int yy = y + (d & 3) * 18;
        const int xx = d < 4 ? 475 : 632;
        g.color(degreeCol(m, d));
        g.textf(xx, yy, font::kTiny, 0, "%-11s %4.0f%s", perdeName(c0, n1, sizeof n1),
                double(cents(c, tu) - cents(m.asc[0], tu)), c != c0 ? (c > c0 ? "+" : "-") : "");
    }
    // the comma steps between the degrees (the interval structure in commas)
    y += 80;
    g.color(col::kGold);
    {
        char buf[48];
        int at = 0;
        for (int d = 0; d < 7 && at < 40; ++d) at += std::snprintf(buf + at, sizeof buf - size_t(at), d ? "-%d" : "%d", stepCommas(m, d));
        g.textf(475, y, font::kSmall, 0, "Commas %s", buf);
    }
    g.color(col::kDim);
    g.text(475, 420, font::kTiny, prac ? "Practice pitches (+/- = off the written perde)" : "Insert: bass + phrases follow the makam", 0);
}

void MakamWs::drawBreak(Canvas& g) {
    const Song& s = *song_;
    const Kit& k = kits_[s.kit == kKits && eng_.wavReady ? kKits : (s.kit < kKits ? s.kit : 0)];
    // waveform with the 16 slices
    const int wx = 20, wy = 44, ww = 760, wh = 100;
    g.color(col::kPanel);
    g.fill(wx, wy, ww, wh);
    const float fpx = k.frames ? float(ww) / float(k.frames) : 0.0f;
    for (int sl = 0; sl < kSlices; ++sl) {   // the slices where they really are (onsets on a card break)
        const int x0 = wx + int(float(k.start[sl]) * fpx);
        const int x1 = wx + int(float(k.start[sl + 1]) * fpx);
        const int cl = k.cls[sl];
        g.color(scale(cl == kClsKick ? col::kRed : cl == kClsSnare ? col::kGold : col::kTurq, sl == eng_.st.slice ? 0.55f : 0.18f));
        g.fill(x0 + 1, wy + wh - 8, x1 - x0 - 2 > 1 ? x1 - x0 - 2 : 1, 8);
    }
    g.color(col::kTurq);
    g.beginLines();
    for (int c = 0; c < 200; c += 2) {
        const float x = float(wx) + float(c) * float(ww) / 200.0f;
        const float a = float(k.peak[c]) * (float(wh) * 0.42f / 127.0f);
        g.vtx(x, float(wy + wh / 2) - a);
        g.vtx(x, float(wy + wh / 2) + a + 1.0f);
    }
    g.endBatch();
    if (eng_.st.slicePos > 0.0f) {
        g.color(col::kCream);
        g.vline(wx + int(eng_.st.slicePos * float(ww)), wy, wh);
    }
    g.color(col::kCream);
    g.textf(wx + 6, wy + 12, font::kTiny, 0, "%s  %.0f BPM", k.name, double(k.bpm));
    {   // what cuts the break now: the section's cut mode and the phrase-end fill
        const int sec = s.songMode ? clampi(int(eng_.st.section), 0, kSections - 1) : clampi(s.curSection, 0, kSections - 1);
        g.color(col::kGold);
        g.textf(wx + ww - 8, wy + 12, font::kTiny, Canvas::kRight, "CUT %s   FILL %d", kCutNames[s.sec[sec].cut], s.sec[sec].fill);
        if (eng_.st.fill && ctx_->playing()) {
            g.color(col::kRed);
            g.fillRound(wx + ww - 150, wy + 24, 142, 20, 5);
            g.color(col::kCream);
            g.text(wx + ww - 79, wy + 34, font::kTiny, kFillStyleNames[eng_.st.fillStyle % kFillStyles], Canvas::kCenter);
        }
    }
    // dial with the variation, selected step ringed
    drawDial(g, 170.0f, 300.0f, 125.0f, true, stepSel_);
    g.color(col::kCream);
    g.textf(170, 300, font::kLarge, Canvas::kCenter, "%c", 'A' + s.editVar);
    // rows
    g.color(col::kPanel);
    g.fill(330, 152, 460, 290);
    g.color(col::kDim);
    g.textf(340, 160, font::kTiny, 0, "STEP %d / %d", stepSel_ + 1, usulLength(s.usul));
    drawRows(g, 336, 180, 220, 26, 0, 9);
    drawRows(g, 566, 180, 220, 26, 9, rowCount_ - 9);
    g.color(col::kDim);
    g.text(570, 318, font::kTiny, "Row 1 steps, 2-3 slices (Shift:", 0);
    g.text(570, 332, font::kTiny, "assign), var A-D, kits. Row 4", 0);
    g.text(570, 346, font::kTiny, "held: rolls rev stut half mute", 0);
    g.text(570, 360, font::kTiny, "CHAOS dum tek ke. Insert: re-cut", 0);
    if (eng_.perfRoll || eng_.perfRev || eng_.perfStutter || eng_.perfHalf || eng_.perfMute || eng_.perfChaos) {
        g.color(col::kRed);
        g.fillRound(566, 396, 210, 30, 6);
        g.color(col::kCream);
        g.text(671, 411, font::kSmall, eng_.perfChaos ? "CHAOS" : eng_.perfRoll ? "ROLL" : "EDIT", Canvas::kCenter);
    }
}

void MakamWs::drawMelody(Canvas& g) {
    const Song& s = *song_;
    const Makam& m = kMakams[s.makam];
    const Phrase& ph = s.phr[s.editPhrase];
    const int n = phraseSteps(ph, s.usul);
    const int hi = s.gen.range > 7 ? s.gen.range : 7;
    // seyir rose: angle = step, radius = degree
    const float cx = 230.0f, cy = 245.0f, r0 = 40.0f, r1 = 185.0f;
    auto rad = [&](int deg) { return r0 + float(deg + 1) * (r1 - r0) / float(hi + 2); };
    g.color(col::kPanel);
    g.point(cx, cy, r1 + 8.0f);
    // degree circles: karar, guclu, tiz durak
    g.color(scale(col::kRed, 0.6f));
    g.ring(cx, cy, rad(0), 40);
    g.color(scale(col::kGold, 0.7f));
    g.ring(cx, cy, rad(m.guclu), 40);
    g.color(col::kMuted);
    g.ring(cx, cy, rad(7), 40);
    g.color(scale(col::kViolet, 0.6f));
    g.ring(cx, cy, rad(-1), 24);
    // cycle spokes
    g.color(col::kMuted);
    g.beginLines();
    for (int c = 0; c < ph.cycles; ++c) {
        const float a = ang(float(c) / float(ph.cycles));
        g.vtx(cx + r0 * 0.5f * cosf(a), cy + r0 * 0.5f * sinf(a));
        g.vtx(cx + r1 * cosf(a), cy + r1 * sinf(a));
    }
    g.endBatch();
    // the melody line
    g.color(col::kTurq);
    g.lineWidth(2.0f);
    g.beginStrip();
    for (int i = 0; i < n; ++i) {
        const NoteStep& x = ph.s[i];
        if (!(x.flags & (kNoteOn | kNoteTie))) continue;
        const float a = ang((float(i) + 0.5f) / float(n));
        const float rr = rad(x.deg);
        g.vtx(cx + rr * cosf(a), cy + rr * sinf(a));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    for (int i = 0; i < n; ++i) {
        const NoteStep& x = ph.s[i];
        if (!(x.flags & kNoteOn)) continue;
        const float a = ang((float(i) + 0.5f) / float(n));
        const float rr = rad(x.deg);
        g.color(i == eng_.st.phraseStep && ctx_->playing() ? col::kCream : degreeCol(m, x.deg));
        g.point(cx + rr * cosf(a), cy + rr * sinf(a), (x.flags & kNoteAccent) ? 6.0f : 4.0f);
    }
    if (s.gen.form == 1 && ph.cycles >= 2) {   // call | response: the yarim karar line
        const float a = ang(float(ph.cycles / 2) / float(ph.cycles));
        g.color(col::kGold);
        g.lineWidth(2.0f);
        g.line(cx, cy, cx + (r1 + 6.0f) * cosf(a), cy + (r1 + 6.0f) * sinf(a));
        g.lineWidth(1.0f);
        g.text(int(cx + (r1 - 30.0f) * cosf(a + 0.25f)), int(cy + (r1 - 30.0f) * sinf(a + 0.25f)), font::kTiny, "RESP", Canvas::kCenter);
        g.text(int(cx + (r1 - 30.0f) * cosf(a - 0.25f)), int(cy + (r1 - 30.0f) * sinf(a - 0.25f)), font::kTiny, "CALL", Canvas::kCenter);
    }
    {   // cursor
        const float a = ang((float(phraseCur_) + 0.5f) / float(n));
        g.color(col::kCream);
        g.line(cx + (r1 - 6.0f) * cosf(a), cy + (r1 - 6.0f) * sinf(a), cx + (r1 + 8.0f) * cosf(a), cy + (r1 + 8.0f) * sinf(a));
    }
    g.color(col::kCream);
    g.textf(int(cx), int(cy), font::kLarge, Canvas::kCenter, "%c", 'A' + s.editPhrase);
    // right panel
    g.color(col::kPanel);
    g.fill(460, 40, 330, 400);
    g.color(col::kDim);
    g.textf(475, 54, font::kTiny, 0, "SEYIR  %s  %s", m.name, seyirName(m.seyir));
    drawRows(g, 466, 64, 318, 26, 0, rowCount_);
    char nm[28];
    const NoteStep& cur = ph.s[clampi(phraseCur_, 0, kPhraseMax - 1)];
    g.color(col::kCream);
    if (cur.flags & kNoteOn)
        g.textf(475, 308, font::kSmall, 0, "Step %d: %s%s%s", phraseCur_ + 1, perdeName(degreeComma(m, cur.deg), nm, sizeof nm),
                (cur.flags & kNoteGlide) ? " gliss" : "", (cur.flags & kNoteGrace) ? " grace" : "");
    else
        g.textf(475, 308, font::kSmall, 0, "Step %d: %s", phraseCur_ + 1, (cur.flags & kNoteTie) ? "hold" : "rest");
    g.color(col::kDim);
    g.text(475, 332, font::kTiny, "Insert: generate   Pads: steps (Live off) / play", 0);
    g.text(475, 348, font::kTiny, "Shift+pad: write the note at the cursor", 0);
    g.text(475, 364, font::kTiny, "Shift+Up/Down: degree  Shift+Insert: gliss", 0);
    g.text(475, 380, font::kTiny, "Delete: rest   Copy: to the next phrase", 0);
    if (s.gen.live) {
        g.color(col::kEmerald);
        g.fillRound(475, 398, 140, 26, 6);
        g.color(col::kNight);
        g.text(545, 411, font::kSmall, "LIVE SEYIR", Canvas::kCenter);
    }
}

void MakamWs::drawSynth(Canvas& g) {
    const Song& s = *song_;
    const Makam& m = kMakams[s.makam];
    g.color(col::kPanel);
    g.fill(10, 40, 385, 400);
    g.fill(405, 40, 385, 400);
    g.color(col::kRed);
    g.text(20, 56, font::kSmall, "BASS  (bottom pad row)", 0);
    g.color(col::kTurq);
    g.text(415, 56, font::kSmall, "LEAD  (pad rows 1-3)", 0);
    drawRows(g, 16, 72, 373, 29, 0, 11);
    drawRows(g, 411, 72, 373, 29, 11, 6);
    // mini perde ring: what sounds now
    const float cx = 600.0f, cy = 365.0f, R = 58.0f;
    g.color(col::kMuted);
    g.ring(cx, cy, R, 32);
    for (int d = 0; d < 7; ++d) {
        const float a = ang(float(((m.asc[d] - m.asc[0]) % 53 + 53) % 53) / 53.0f);
        const bool lead = eng_.st.leadDeg != -100 && (((eng_.st.leadDeg % 7) + 7) % 7) == d;
        const bool bass = eng_.st.bassDeg != -100 && (((eng_.st.bassDeg % 7) + 7) % 7) == d;
        g.color(lead ? col::kTurq : bass ? col::kRed : degreeCol(m, d));
        g.point(cx + R * cosf(a), cy + R * sinf(a), lead || bass ? 9.0f : 5.0f);
    }
    char nm[28];
    if (eng_.st.leadDeg != -100) {
        g.color(col::kCream);
        g.text(int(cx), int(cy), font::kTiny, perdeName(eng_.st.leadComma, nm, sizeof nm), Canvas::kCenter);
    }
    g.color(col::kDim);
    g.text(20, 432, font::kTiny, "Pads: row 4 bass, rows 3-1 lead, column 1 = yeden", 0);
    g.color(col::kDim);
    g.text(415, 262, font::kTiny, "Neuro bass after NEUROSTACK: Talk, FM, Hoover", 0);
    g.text(415, 276, font::kTiny, "over a clean sine Sub; Motion moves them.", 0);
}

void MakamWs::drawArrange(Canvas& g) {
    const Song& s = *song_;
    static const uint32_t kSecCol[kSecTypes] = {0x2F5FD8, 0x2FD0C0, 0xE5484D, 0x9B6BE0, 0xE9B949, 0xF4A261, 0x2A9D6F};
    // a circular timeline: the song as a ring of sections
    const float cx = 200.0f, cy = 245.0f, R = 160.0f;
    const uint32_t total = songCycles(s);
    uint32_t at = 0;
    g.lineWidth(16.0f);
    for (int i = 0; i < s.sectionCount; ++i) {
        const float a0 = ang(float(at) / float(total)), a1 = ang(float(at + uint32_t(s.sec[i].cycles)) / float(total));
        g.color(scale(Rgb(kSecCol[s.sec[i].type]), i == secSel_ ? 1.0f : 0.5f));
        g.arc(cx, cy, R, a0 + 0.01f, a1 - 0.01f, 8);
        at += uint32_t(s.sec[i].cycles);
    }
    g.lineWidth(1.0f);
    // song position
    if (ctx_->playing() && s.songMode) {
        uint32_t st = 0;
        songSectionAt(s, uint32_t(eng_.st.cycle), &st);
        const float f = float(uint32_t(eng_.st.cycle) % total) / float(total) + playFrac() / float(total);
        const float a = ang(f);
        g.color(col::kCream);
        g.lineWidth(2.0f);
        g.line(cx, cy, cx + (R + 14.0f) * cosf(a), cy + (R + 14.0f) * sinf(a));
        g.lineWidth(1.0f);
    }
    const Section& x = s.sec[clampi(secSel_, 0, kSections - 1)];
    g.color(col::kCream);
    g.text(int(cx), int(cy) - 14, font::kTitle, kSectionNames[x.type], Canvas::kCenter);
    g.color(col::kDim);
    g.textf(int(cx), int(cy) + 16, font::kSmall, Canvas::kCenter, "%d / %d   %lu cycles", secSel_ + 1, s.sectionCount,
            static_cast<unsigned long>(total));
    const float secs = float(total) * float(usulLength(s.usul)) * (s.unit ? 0.5f : 0.25f) * 60.0f / float(s.bpm);
    g.textf(int(cx), int(cy) + 36, font::kSmall, Canvas::kCenter, "%d:%02d", int(secs) / 60, int(secs) % 60);
    // linear strip of sections
    g.color(col::kPanel);
    g.fill(400, 40, 390, 400);
    for (int i = 0; i < s.sectionCount; ++i) {
        const int yy = 48 + (i % 8) * 22, xx = i < 8 ? 408 : 600;
        g.color(i == secSel_ ? col::kPanel2 : col::kPanel);
        g.fill(xx, yy, 184, 20);
        g.color(Rgb(kSecCol[s.sec[i].type]));
        g.fill(xx, yy, 5, 20);
        g.color(i == eng_.st.section && ctx_->playing() ? col::kCream : col::kDim);
        g.textf(xx + 10, yy + 10, font::kTiny, Canvas::kCenterY, "%2d %-7s%2d %c%c%c %c", i + 1, kSectionNames[s.sec[i].type],
                s.sec[i].cycles, s.sec[i].brk ? 'A' + s.sec[i].brk - 1 : '-', s.sec[i].bass ? 'A' + s.sec[i].bass - 1 : '-',
                s.sec[i].lead ? 'A' + s.sec[i].lead - 1 : '-', kCutNames[s.sec[i].cut][0]);
        if (s.sec[i].scene) {
            g.color(col::kSaffron);
            g.textf(xx + 178, yy + 10, font::kTiny, Canvas::kCenterY | Canvas::kRight, "S%d", s.sec[i].scene);
        }
    }
    drawRows(g, 404, 230, 190, 25, 0, 6);
    drawRows(g, 596, 230, 190, 25, 6, rowCount_ - 6);
    g.color(col::kDim);
    g.text(408, 396, font::kTiny, "Insert: loop this section  Song: song / loop", 0);
    g.text(408, 412, font::kTiny, "Shift+Insert add, Copy, Delete: sections", 0);
    g.text(408, 428, font::kTiny, "Cut: Usul BBCut Warp DNA  Fill: phrase ends", 0);
}

void MakamWs::drawMixer(Canvas& g) {
    const Song& s = *song_;
    static const char* const kCh[5] = {"BREAK", "BASS", "LEAD", "FX RET", "SENDS"};
    static const uint32_t kChCol[5] = {0xE9B949, 0xE5484D, 0x2FD0C0, 0x9B6BE0, 0x2F5FD8};
    for (int c = 0; c < 5; ++c) {
        const int x = 14 + c * 112;
        g.color(c == mixCol_ ? col::kPanel2 : col::kPanel);
        g.fill(x, 40, 104, 400);
        g.color(Rgb(kChCol[c]));
        g.text(x + 52, 56, font::kSmall, kCh[c], Canvas::kCenter);
        if (c < 4) {
            const Channel& ch = s.ch[c];
            // send knobs as arcs (delay, reverb)
            const float kx = float(x + 52);
            const float sends[2] = {float(ch.dly) * 0.01f, float(ch.rvb) * 0.01f};
            for (int k = 0; k < (c < 3 ? 2 : 0); ++k) {
                const float ky = 100.0f + float(k) * 62.0f;
                g.color(col::kMuted);
                g.arc(kx, ky, 22.0f, 0.75f * kPi, 2.25f * kPi, 12);
                g.color(k ? col::kViolet : col::kCobalt);
                g.lineWidth(3.0f);
                if (sends[k] > 0.01f) g.arc(kx, ky, 22.0f, 0.75f * kPi, 0.75f * kPi + 1.5f * kPi * sends[k], 10);
                g.lineWidth(1.0f);
                g.color(col::kDim);
                g.text(int(kx), int(ky), font::kTiny, k ? "RVB" : "DLY", Canvas::kCenter);
            }
            // fader + meter
            const int fy = 230, fh = 170;
            g.color(col::kNight);
            g.fill(x + 30, fy, 12, fh);
            g.fill(x + 62, fy, 12, fh);
            const int lv = fh * ch.level / 100;
            g.color(ch.mute ? col::kRed : Rgb(kChCol[c]));
            g.fill(x + 30, fy + fh - lv, 12, lv);
            float m = eng_.st.meter[c];
            if (m > 1.0f) m = 1.0f;
            const int mh = int(float(fh) * sqrtf(m));
            g.color(m > 0.9f ? col::kRed : col::kEmerald);
            g.fill(x + 62, fy + fh - mh, 12, mh);
            g.color(col::kCream);
            g.textf(x + 52, 416, font::kTiny, Canvas::kCenter, "%s %d", ch.mute ? "MUTE" : "", ch.level);
        } else {
            static const char* const kDly[kDlyTypes] = {"CLEAN", "TAPE", "LO-FI", "PING"};
            static const char* const kRvb[kRvbTypes] = {"PLATE", "HALL", "GATED"};
            g.color(col::kCobalt);
            g.text(x + 52, 92, font::kSmall, kDly[clampi(s.dlyType, 0, kDlyTypes - 1)], Canvas::kCenter);
            g.color(col::kDim);
            g.textf(x + 52, 112, font::kTiny, Canvas::kCenter, "%s  fb %d", kDelaySyncNames[s.dlyTime], s.dlyFb);
            g.color(col::kViolet);
            g.text(x + 52, 146, font::kSmall, kRvb[clampi(s.rvbType, 0, kRvbTypes - 1)], Canvas::kCenter);
            g.color(col::kDim);
            g.textf(x + 52, 166, font::kTiny, Canvas::kCenter, "size %d", s.rvbSize);
            g.textf(x + 52, 200, font::kTiny, Canvas::kCenter, "duck %d", s.duck);
            g.textf(x + 52, 218, font::kTiny, Canvas::kCenter, "filter %d", s.djFilter);
            // the scenes (Perform + pad 1-8)
            g.color(col::kSaffron);
            g.text(x + 52, 262, font::kTiny, "SCENES", Canvas::kCenter);
            for (int k = 0; k < kScenes; ++k) {
                const int sx = x + 14 + (k & 3) * 25, sy = 282 + (k >> 2) * 26;
                g.color(sceneUsed_[k] ? (k == morphTo_ ? col::kCream : col::kSaffron) : col::kMuted);
                g.fillRound(sx, sy, 20, 20, 4);
                g.color(col::kNight);
                g.number(sx + 10, sy + 10, font::kTiny, k + 1, Canvas::kCenter);
            }
            if (morphOn_) {
                g.color(col::kCream);
                g.text(x + 52, 352, font::kTiny, "MORPH", Canvas::kCenter);
            }
        }
    }
    g.color(col::kPanel);
    g.fill(578, 40, 212, 400);
    drawRows(g, 580, 46, 208, 26, 0, rowCount_);
    g.color(col::kDim);
    g.text(586, 300, font::kTiny, "Left/Right: channel", 0);
    g.text(586, 314, font::kTiny, "Pads 1-4 row 1: mute, 5: sends", 0);
    g.text(586, 328, font::kTiny, "Perform+pad 1-8: scene", 0);
    g.text(586, 342, font::kTiny, "Perform+Shift+pad: store it", 0);
    g.text(586, 366, font::kTiny, "Stems (Shift+Rec):", 0);
    g.text(586, 380, font::kTiny, "1 break 2 bass 3 lead 4 fx", 0);
}

void MakamWs::drawFile(Canvas& g) {
    g.color(col::kPanel);
    g.fill(14, 40, 380, 400);
    g.fill(406, 40, 380, 400);
    g.color(fileCol_ == 0 ? col::kTurq : col::kDim);
    g.text(24, 58, font::kSmall, "SONGS  /Lab/MAKAM/Projects", 0);
    g.color(fileCol_ == 1 ? col::kTurq : col::kDim);
    g.text(416, 58, font::kSmall, "BREAKS  /Lab/MAKAM/Breaks", 0);
    for (int i = 0; i < 8; ++i) {
        const int y = 76 + i * 36;
        const bool sel = fileCol_ == 0 && fileSel_[0] == i;
        g.color(sel ? col::kPanel2 : col::kNight);
        g.fill(20, y, 368, 32);
        g.color(slotUsed_[i] ? col::kGold : col::kMuted);
        g.point(38, float(y + 16), 6.0f);
        g.color(sel ? col::kCream : col::kDim);
        g.textf(54, y + 16, font::kBody, Canvas::kCenterY, "SONG%d.mkm  %s", i + 1, slotUsed_[i] ? "" : "(empty)");
    }
    {   // entry 0: the LABKIT browser (any folder, preview, favourites)
        const bool sel = fileCol_ == 1 && fileSel_[1] == 0;
        g.color(sel ? col::kCobalt : col::kNight);
        g.fill(412, 76, 368, 32);
        g.color(sel ? col::kCream : col::kTurq);
        g.text(424, 92, font::kBody, "Browse the card (preview)...", Canvas::kCenterY);
    }
    if (!wavCount_) {
        g.color(col::kDim);
        g.text(596, 200, font::kSmall, "No WAV breaks in the folder", Canvas::kCenter);
        g.text(596, 222, font::kTiny, "Copy WAVs to /Lab/MAKAM/Breaks or browse", Canvas::kCenter);
        g.text(596, 240, font::kTiny, "The built-in kits work without the card", Canvas::kCenter);
    }
    const int top = fileSel_[1] > 8 ? fileSel_[1] - 8 : 0;
    const char* cur = song_->wav;
    for (const char* p = song_->wav; *p; ++p)
        if (*p == '/') cur = p + 1;
    for (int i = 0; i < 8 && top + i < wavCount_; ++i) {
        const int idx = top + i, y = 112 + i * 36;
        const bool sel = fileCol_ == 1 && fileSel_[1] == idx + 1;
        g.color(sel ? col::kPanel2 : col::kNight);
        g.fill(412, y, 368, 32);
        g.color(std::strcmp(wavs_[idx], cur) == 0 ? col::kSaffron : (sel ? col::kCream : col::kDim));
        g.text(424, y + 16, font::kBody, wavs_[idx], Canvas::kCenterY);
    }
    g.color(col::kDim);
    g.text(24, 404, font::kTiny, "Left/Right: column  Encoder: select", 0);
    g.text(24, 420, font::kTiny, "Insert: load   Shift+Insert or Rec: save   Shift+Delete: new", 0);
    if (!ctx_->fs) {
        g.color(col::kRed);
        g.text(400, 380, font::kSmall, "No SD card", Canvas::kCenter);
    }
}

// ---------------------------------------------------------------------------
void MakamWs::draw(Canvas& g) {
    rowCount_ = buildRows(page_, rows_, kMaxRows);
    row_[page_] = clampi(row_[page_], 0, rowCount_ > 0 ? rowCount_ - 1 : 0);
    drawTop(g, kPageNames[page_]);
    switch (page_) {
        case kPgUsul: drawUsul(g); break;
        case kPgMakam: drawMakam(g); break;
        case kPgBreak: drawBreak(g); break;
        case kPgMelody: drawMelody(g); break;
        case kPgSynth: drawSynth(g); break;
        case kPgArrange: drawArrange(g); break;
        case kPgMixer: drawMixer(g); break;
        default: drawFile(g); break;
    }
    drawFKeyBar(g, theme_, kPageNames, page_);
}

// Boot intro: Iznik stars turn around the usul dial, the aksak strokes light
// up one by one, then the name resolves.
void MakamWs::drawIntro(Canvas& g, uint32_t t) {
    const float k = float(t) / float(introMs());
    const float cx = 400.0f, cy = 230.0f;
    // tile field
    const float rot = k * kPi * 0.75f;
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * kPi * 0.25f + rot * 0.5f;
        const float rr = 300.0f - 90.0f * k;
        g.color(mix(col::kNight, i & 1 ? col::kCobalt : col::kTurq, 0.35f + 0.4f * (1.0f - k)));
        star(g, cx + rr * cosf(a), cy + rr * 0.62f * sinf(a), 46.0f, -rot + float(i));
    }
    // the usul dial: aksak 9/8
    const char* aks = kUsuls[0].strokes;
    const float R = 120.0f;
    g.color(col::kPanel);
    g.point(cx, cy, R + 20.0f);
    g.color(col::kNight);
    g.point(cx, cy, R - 30.0f);
    g.lineWidth(3.0f);
    g.color(col::kGold);
    g.arc(cx, cy, R + 8.0f, ang(0.0f), ang(k < 0.6f ? k / 0.6f : 1.0f), 24);
    g.lineWidth(1.0f);
    const int lit = int(k * 14.0f);
    for (int i = 0; i < 9; ++i) {
        const int st = strokeOf(aks[i]);
        const float a = ang(float(i) / 9.0f);
        const float rad = st == kDum ? 15.0f : st == kTek ? 11.0f : st == kKe ? 8.0f : 3.0f;
        g.color(i < lit ? strokeCol(st) : col::kMuted);
        g.point(cx + (R - 8.0f) * cosf(a), cy + (R - 8.0f) * sinf(a), rad);
    }
    // sweeping hand
    const float ha = ang(k * 2.0f);
    g.color(col::kSaffron);
    g.lineWidth(2.0f);
    g.line(cx, cy, cx + (R + 16.0f) * cosf(ha), cy + (R + 16.0f) * sinf(ha));
    g.lineWidth(1.0f);
    star(g, cx, cy, 34.0f + 10.0f * k, rot);
    // name
    if (k > 0.35f) {
        const float a = k > 0.6f ? 1.0f : (k - 0.35f) / 0.25f;
        g.color(mix(col::kNight, col::kCream, a));
        g.text(400, 400, font::kHuge, "MAKAM", Canvas::kCenter);
        g.color(mix(col::kNight, col::kRed, a));
        g.text(400, 436, font::kLarge, "- J U N G L E -", Canvas::kCenter);
    }
    if (k > 0.7f) {
        g.color(mix(col::kNight, col::kGold, (k - 0.7f) / 0.3f));
        g.text(400, 462, font::kTiny, "usul  -  perde  -  seyir  -  172 BPM", Canvas::kCenter);
    }
}

}  // namespace makam
