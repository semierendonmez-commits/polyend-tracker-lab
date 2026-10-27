// KONSOL drawing. The identity: a dark mixing desk (navy panels, amber
// accents) where every page has its own colour; the header names the page,
// the selected track and the transport; the rail under it lists every page
// with the key that opens it; the F-key bar mirrors the eight keys as keycaps.
// Rectangles go through Canvas batches (2 words a rect); texts are short.
// Main loop only (no audio-interrupt code here): built for size.
#pragma GCC optimize("Os")
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/gen_glue.h"
#include "ws/kit.h"
#include "ws/konsol.h"
#include "ws/stretch.h"

namespace lab {
namespace konsol {

extern const char* const kPageName[kPgCount];
extern const char* const kPageKey[kPgCount];
Rgb pageColor(int p);

namespace {
constexpr uint32_t kSubPerBeat = kPpq * 256;
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
const Rgb kDesk(0x090C12), kPanel(0x121824), kPanel2(0x1B2333), kWell(0x0C1018), kLine(0x263045),
    kAmber(0xFFB000), kTeal(0x2EC4B6), kText(0xEDE8DA), kDim(0x6B7590), kFaint(0x3A4459), kRed(0xFF4D4D),
    kGreen(0x7CDB5A), kBlue(0x4D9DE0), kWhite(0xFFFFFF), kMute(0xFF8C00);
constexpr int kTop = 72;      // content area: kTop .. kBottom
constexpr int kBottom = 446;

Rgb stemColor(int s) {
    static const Rgb k[5] = {Rgb(0xFF7A59), Rgb(0x59C3FF), Rgb(0xB8FF59), Rgb(0xD780FF), Rgb(0x404040)};
    return k[s < 0 ? 4 : (s > 4 ? 4 : s)];
}
Rgb classColor(int c) {
    static const Rgb k[4] = {Rgb(0x9AA6BF), Rgb(0xFF5A4E), Rgb(0xFFB000), Rgb(0x2EC4B6)};
    return k[c & 3];
}
// rect in a batch: color + 2 vertices
inline void rv(Canvas& g, Rgb c, int x, int y, int w, int h) {
    g.color(c);
    g.vtx(float(x), float(y));
    g.vtx(float(x + w - 1), float(y + h - 1));
}
inline void rv2(Canvas& g, int x, int y, int w, int h) {
    g.vtx(float(x), float(y));
    g.vtx(float(x + w - 1), float(y + h - 1));
}
const char* clipLabel(const Clip& c) { return c.gen != kGenNotes ? genName(c.gen) : "notes"; }

// A knob: background arc, value arc, an optional modulation dot, name below,
// value inside. ~45 words.
void knob(Canvas& g, int cx, int cy, int r, float frac, float modFrac, Rgb col, const char* name, const char* val,
          bool focused, bool bipolar = false) {
    const float a0 = 2.3561945f, span = 4.712389f;
    g.lineWidth(focused ? 4.0f : 3.0f);
    g.color(kLine);
    g.arc(float(cx), float(cy), float(r), a0, a0 + span, 12);
    frac = clampf(frac, 0.0f, 1.0f);
    g.color(focused ? mix(col, kWhite, 0.25f) : col);
    if (bipolar) {
        const float c = a0 + span * 0.5f, e = a0 + span * frac;
        if (fabsf(e - c) > 0.03f) g.arc(float(cx), float(cy), float(r), c < e ? c : e, c < e ? e : c, 2 + int(10.0f * fabsf(frac - 0.5f)));
        else g.point(float(cx), float(cy - r), 2.5f);
    } else if (frac > 0.01f) {
        g.arc(float(cx), float(cy), float(r), a0, a0 + span * frac, 2 + int(10.0f * frac));
    }
    g.lineWidth(1.0f);
    if (modFrac >= 0.0f && fabsf(modFrac - frac) > 0.005f) {
        const float a = a0 + span * clampf(modFrac, 0.0f, 1.0f);
        g.color(kWhite);
        g.point(float(cx) + float(r) * cosf(a), float(cy) + float(r) * sinf(a), 3.5f);
    }
    g.color(focused ? kText : kDim);
    g.text(cx, cy, r >= 26 ? font::kSmall : font::kTiny, val, Canvas::kCenter);
    g.color(focused ? kAmber : kText);
    g.text(cx, cy + r + 13, font::kTiny, name, Canvas::kCenter);
}

// A horizontal value bar with a name and a value (list rows).
void barRow(Canvas& g, int x, int y, int w, const char* name, const char* val, float frac, Rgb col, bool focused,
            int h = 28) {
    g.beginRects();
    rv(g, focused ? kPanel2 : kPanel, x, y, w, h);
    if (focused) rv(g, kAmber, x, y, 3, h);
    rv(g, kWell, x + w / 2 - 20, y + h / 2 - 2, w / 2 - 70, 5);
    rv(g, col, x + w / 2 - 20, y + h / 2 - 2, int(float(w / 2 - 70) * clampf(frac, 0, 1)) + 1, 5);
    g.endBatch();
    g.color(focused ? kText : kDim);
    g.text(x + 12, y + h / 2, font::kSmall, name, Canvas::kCenterY);
    g.color(focused ? kAmber : kText);
    g.text(x + w - 10, y + h / 2, font::kSmall, val, Canvas::kCenterY | Canvas::kRight);
}
}  // namespace

// ---------------------------------------------------------------------------
// chrome: header, page rail, F-keys
// ---------------------------------------------------------------------------
void Konsol::drawChrome(Canvas& g) {
    Shell& sh = *ctx_->shell;
    const Rgb pc = pageColor(page_);
    const Rgb tc = trackColor(selT_);
    const uint32_t pos = posPub_;
    const uint32_t beat = pos / kSubPerBeat;
    const float beatPh = float(pos % kSubPerBeat) / float(kSubPerBeat);
    const bool playing = sh.playing();
    g.beginRects();
    rv(g, kDesk, 0, 0, 800, 480);
    rv(g, kPanel, 0, 0, 800, 40);
    rv(g, pc, 0, 0, 6, 40);
    // the track chip
    rv(g, scale(tc, 0.22f), 214, 8, 150, 24);
    rv(g, tc, 214, 8, 4, 24);
    // beat dots
    for (int b = 0; b < 4; ++b) {
        const bool on = playing && int(beat % 4) == b;
        rv(g, on ? (b == 0 ? kAmber : kText) : kFaint, 548 + b * 10, on && beatPh < 0.25f ? 13 : 15, 6, on && beatPh < 0.25f ? 10 : 6);
    }
    // the page rail
    rv(g, kWell, 0, 40, 800, 30);
    const int tw = 80;
    for (int p = 0; p < kPgCount; ++p) {
        if (p == page_) {
            rv(g, kPanel2, p * tw, 40, tw, 30);
            rv(g, pageColor(p), p * tw, 67, tw, 3);
        } else {
            rv(g, kPanel, p * tw + tw - 1, 44, 1, 22);
        }
    }
    // F-key caps
    rv(g, kWell, 0, 448, 800, 32);
    g.endBatch();
    // header texts
    g.color(kText);
    g.text(18, 20, font::kTitle, kPageName[page_], Canvas::kCenterY);
    g.color(tc);
    g.textf(226, 20, font::kSmall, Canvas::kCenterY, "T%d %s", selT_ + 1, engineName(P_->track[selT_].engine));
    g.color(kDim);
    g.textf(374, 20, font::kSmall, Canvas::kCenterY, "S%d", selS_ + 1);
    g.color(playing ? kText : kDim);
    g.textf(420, 20, font::kLarge, Canvas::kCenterY, "%lu.%lu", static_cast<unsigned long>(beat / 4 + 1),
            static_cast<unsigned long>(beat % 4 + 1));
    g.color(kAmber);
    g.textf(650, 20, font::kLarge, Canvas::kCenterY | Canvas::kRight, "%.1f", double(sh.bpm()));
    // play / stop
    if (playing) {
        g.color(kGreen);
        g.beginStrip();
        g.vtx(664, 12);
        g.vtx(678, 20);
        g.vtx(664, 28);
        g.vtx(664, 12);
        g.endBatch();
    } else {
        g.beginRects();
        rv(g, kDim, 664, 14, 12, 12);
        g.endBatch();
    }
    // badges: ARM / KEYS / SONG / CPU
    int bx = 690;
    if (recArm_) {
        g.color(kRed);
        g.point(float(bx + 4), 20, 5);
        g.text(bx + 12, 20, font::kTiny, "ARM", Canvas::kCenterY);
        bx += 42;
    } else if (keysOverlay_) {
        g.color(kTeal);
        g.text(bx, 20, font::kTiny, "KEYS", Canvas::kCenterY);
        bx += 38;
    } else if (songMode_) {
        g.color(kTeal);
        g.text(bx, 20, font::kTiny, "SONG", Canvas::kCenterY);
        bx += 38;
    }
    const unsigned cpu = unsigned(sh.cpuPermille() / 10);
    g.color(cpu > 85 ? kRed : kDim);
    g.textf(792, 20, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%u%%", cpu);
    // rail texts: the page and the key that opens it
    for (int p = 0; p < kPgCount; ++p) {
        const int x = p * tw + tw / 2;
        g.color(p == page_ ? kText : kDim);
        g.text(x, 49, font::kTiny, kPageName[p], Canvas::kCenter);
        g.color(p == page_ ? pageColor(p) : kFaint);
        g.text(x, 61, font::kTiny, kPageKey[p], Canvas::kCenter);
    }
    // stretch progress (a job runs in the main loop)
    const float sp = stretchProgress();
    if (sp >= 0.0f) {
        g.beginRects();
        rv(g, kWell, 214, 34, 150, 4);
        rv(g, kTeal, 214, 34, int(150.0f * sp) + 1, 4);
        g.endBatch();
    }
}

void Konsol::drawFKeys(Canvas& g, const char* const k[8], int active, uint32_t litMask) {
    g.beginRects();
    for (int i = 0; i < 8; ++i) {
        const bool on = i == active;
        rv(g, on ? pageColor(page_) : kPanel, i * 100 + 3, 451, 94, 26);
        if (!on && ((litMask >> i) & 1)) rv(g, kGreen, i * 100 + 8, 455, 4, 18);
    }
    g.endBatch();
    for (int i = 0; i < 8; ++i) {
        if (!k[i] || !k[i][0]) continue;
        g.color(i == active ? kDesk : kText);
        g.text(i * 100 + 50, 464, font::kSmall, k[i], Canvas::kCenter);
    }
}

// ---------------------------------------------------------------------------
// intro: faders rise across eight strips, the meters kick, the name stamps in
// ---------------------------------------------------------------------------
void Konsol::drawIntro(Canvas& g, uint32_t t) {
    const float s = float(t) / 1000.0f;
    g.beginRects();
    rv(g, kDesk, 0, 0, 800, 480);
    for (int i = 0; i < 8; ++i) {
        const int x = 60 + i * 72;
        rv(g, kPanel, x, 70, 56, 300);
        rv(g, trackColor(i), x, 70, 56, 3);
        rv(g, kWell, x + 26, 90, 4, 260);
    }
    g.endBatch();
    static const float kLvl[8] = {0.78f, 0.62f, 0.7f, 0.5f, 0.66f, 0.45f, 0.58f, 0.72f};
    g.beginRects();
    for (int i = 0; i < 8; ++i) {
        const int x = 60 + i * 72;
        float k = clampf((s - 0.15f - 0.11f * float(i)) / 0.7f, 0.0f, 1.0f);
        k = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        const float lv = kLvl[i] * k;
        const int y = 350 - int(lv * 240.0f);
        const float beat = s * 2.03f * 2.0f;
        const float ph = beat - floorf(beat);
        const float meter = k * kLvl[i] * (0.55f + 0.45f * (1.0f - ph)) * (0.8f + 0.2f * sinf(float(i) * 1.7f + s * 9.0f));
        const int segs = int(meter * 14.0f);
        for (int m = 0; m < 14; ++m) {
            const Rgb on = m >= 12 ? kRed : (m >= 9 ? kAmber : trackColor(i));
            rv(g, m < segs ? on : Rgb(0x1A2130), x + 42, 340 - m * 18, 8, 14);
        }
        rv(g, kText, x + 8, y - 9, 40, 18);
        rv(g, trackColor(i), x + 8, y - 1, 40, 3);
    }
    g.endBatch();
    const float a = clampf((s - 1.4f) / 0.5f, 0.0f, 1.0f);
    if (a > 0.0f) {
        const int w = int(360.0f * a);
        g.color(kAmber);
        g.fillRound(400 - w / 2, 190, w, 70, 8.0f);
        if (a >= 1.0f) {
            g.color(kDesk);
            g.text(400, 225, font::kHuge, "K O N S O L", Canvas::kCenter);
        }
    }
    if (s > 2.0f) {
        g.color(kText);
        g.text(400, 400, font::kSmall, "8 channels  -  16 scenes  -  16 engines  -  10 players", Canvas::kCenter);
        g.color(kDim);
        g.text(400, 424, font::kTiny, "clip / scene production console for the Polyend Tracker", Canvas::kCenter);
    }
    g.color(kDim);
    g.text(400, 40, font::kTiny, "polyend-tracker-lab", Canvas::kCenter);
}

// ---------------------------------------------------------------------------
// main draw
// ---------------------------------------------------------------------------
void Konsol::draw(Canvas& g) {
    drawChrome(g);
    switch (page_) {
        case kPgSession: drawSession(g); break;
        case kPgSteps: drawSteps(g); break;
        case kPgRoll: drawRoll(g); break;
        case kPgGen: drawGen(g); break;
        case kPgSound: drawSound(g); break;
        case kPgSample:
            if (smpTab_ == 1) drawSlices(g);
            else drawSample(g);
            break;
        case kPgKit: drawKit(g); break;
        case kPgMixer:
            if (mixFx_) drawFx(g);
            else drawMixer(g);
            break;
        case kPgSong: drawSong(g); break;
        default: drawProject(g); break;
    }
}

// ---------------------------------------------------------------------------
// SESSION
// ---------------------------------------------------------------------------
void Konsol::drawSession(Canvas& g) {
    const int x0 = 4, cw = 81, top = kTop + 33, rh = 40, rows = 8;
    const int viewTop = clampi(sceneTop_ - 2, 0, kScenes - rows);
    const uint32_t ms = ctx_->millis();
    const bool blink = (ms / 250) % 2;
    const float beatPh = float(posPub_ % kSubPerBeat) / float(kSubPerBeat);
    const float pulse = ctx_->playing() ? 0.78f + 0.22f * (1.0f - beatPh) : 0.85f;
    g.beginRects();
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        const Rgb tc = trackColor(t);
        const Mix& m = P_->track[t].mix;
        // channel header: colour cap, live meter along the bottom
        rv(g, t == selT_ ? kPanel2 : kPanel, x, kTop, cw - 3, 30);
        rv(g, tc, x, kTop, cw - 3, 3);
        const float lv = clampf(meter_[t] * 1.1f, 0.0f, 1.0f);
        rv(g, kWell, x + 4, kTop + 24, cw - 11, 3);
        rv(g, lv > 0.9f ? kRed : (m.mute ? kFaint : tc), x + 4, kTop + 24, int(float(cw - 11) * lv) + 1, 3);
        for (int r = 0; r < rows; ++r) {
            const int s = viewTop + r;
            const Clip& c = P_->clip[t][s];
            const int y = top + r * rh;
            const bool playing = rt_[t].play == s;
            const bool queued = rt_[t].queued == s;
            if (!c.exists()) {
                rv(g, kWell, x, y, cw - 3, rh - 4);
                continue;
            }
            Rgb col = playing ? scale(tc, pulse) : scale(tc, 0.26f);
            if (queued && blink) col = scale(tc, 0.6f);
            rv(g, col, x, y, cw - 3, rh - 4);
            if (!playing) rv(g, tc, x, y, 3, rh - 4);
            if (playing) {
                const float prog = float(rt_[t].tick) / float(c.lenSteps * kTps);
                rv(g, scale(kDesk, 1.0f), x + 3, y + rh - 9, cw - 9, 3);
                rv(g, kDesk, x + 3, y + rh - 9, int(float(cw - 9) * prog) + 1, 3);
            }
            if (rt_[t].queued == -1 && playing && blink) rv(g, kRed, x + cw - 13, y + 4, 7, 7);
        }
    }
    // scene column
    for (int r = 0; r < rows; ++r) {
        const int s = viewTop + r;
        const int y = top + r * rh;
        bool all = true, any = false;
        for (int t = 0; t < kTracks; ++t)
            if (P_->clip[t][s].exists()) {
                any = true;
                if (rt_[t].play != s) all = false;
            }
        const bool live = any && all && ctx_->playing();
        rv(g, live ? scale(kAmber, pulse) : (any ? kPanel2 : kPanel), 656, y, 140, rh - 4);
        if (s == selS_) rv(g, kAmber, 656, y, 3, rh - 4);
    }
    rv(g, kPanel, 656, kTop, 140, 30);
    // the pad window: a bracket on both edges of the grid
    g.color(kAmber);
    rv2(g, 0, top + (sceneTop_ - viewTop) * rh, 3, kPadRows * rh - 4);
    rv2(g, 651, top + (sceneTop_ - viewTop) * rh, 3, kPadRows * rh - 4);
    g.endBatch();
    if (selS_ >= viewTop && selS_ < viewTop + rows) {
        g.color(kWhite);
        g.frame(x0 + selT_ * cw - 1, top + (selS_ - viewTop) * rh - 1, cw - 1, rh - 2);
    }
    // texts
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        const Mix& m = P_->track[t].mix;
        g.color(m.mute ? kDim : kText);
        g.textf(x + 6, kTop + 13, font::kSmall, Canvas::kCenterY, "%d", t + 1);
        g.color(m.mute ? kFaint : kDim);
        g.text(x + 20, kTop + 13, font::kTiny, engineName(P_->track[t].engine), Canvas::kCenterY);
        if (m.mute || m.solo) {
            g.color(m.mute ? kMute : kBlue);
            g.text(x + cw - 8, kTop + 13, font::kTiny, m.mute ? "M" : "S", Canvas::kCenterY | Canvas::kRight);
        }
        for (int r = 0; r < rows; ++r) {
            const int s = viewTop + r;
            const Clip& c = P_->clip[t][s];
            if (!c.exists()) continue;
            const int y = top + r * rh;
            const bool playing = rt_[t].play == s;
            g.color(playing ? kDesk : kText);
            g.text(x + 9, y + 12, font::kTiny, clipLabel(c), Canvas::kCenterY);
            g.color(playing ? kDesk : kDim);
            g.textf(x + 9, y + 25, font::kTiny, Canvas::kCenterY, "%d%s", c.lenSteps, c.live ? " LIVE" : "");
        }
    }
    g.color(kDim);
    g.text(726, kTop + 15, font::kTiny, "SCENES", Canvas::kCenter);
    for (int r = 0; r < rows; ++r) {
        const int s = viewTop + r;
        const int y = top + r * rh;
        bool any = false, all = true;
        for (int t = 0; t < kTracks; ++t)
            if (P_->clip[t][s].exists()) {
                any = true;
                if (rt_[t].play != s) all = false;
            }
        const bool live = any && all && ctx_->playing();
        g.color(live ? kDesk : (any ? kText : kFaint));
        g.textf(672, y + 18, font::kBody, Canvas::kCenterY, "%d", s + 1);
        g.beginStrip();
        g.vtx(770, float(y + 10));
        g.vtx(782, float(y + 18));
        g.vtx(770, float(y + 26));
        g.vtx(770, float(y + 10));
        g.endBatch();
    }
    // status line: grid mode, the selected clip
    static const char* const kModes[6] = {"LAUNCH", "STOP", "MUTE", "SOLO", "SELECT", "DUP"};
    static const Rgb kModeC[6] = {Rgb(0x6B7590), Rgb(0xFF3020), Rgb(0xFF8C00), Rgb(0x2060FF), Rgb(0xE0E0E0), Rgb(0x6B7590)};
    const Clip& sc = P_->clip[selT_][selS_];
    g.beginRects();
    rv(g, kPanel, 4, 429, 792, 18);
    rv(g, kModeC[gridMode_], 4, 429, 70, 18);
    if (fillHeld_) rv(g, kAmber, 730, 429, 66, 18);
    g.endBatch();
    g.color(gridMode_ == kGmLaunch ? kText : kDesk);
    g.text(39, 438, font::kTiny, kModes[gridMode_], Canvas::kCenter);
    g.color(kDim);
    if (sc.exists())
        g.textf(84, 438, font::kTiny, Canvas::kCenterY, "T%d S%d  %s  %d steps  %d notes  launch %s   Insert: launch   Copy: dup",
                selT_ + 1, selS_ + 1, clipLabel(sc), sc.lenSteps, sc.notes().count,
                sc.launchQ ? launchQNames()[sc.launchQ - 1] : launchQNames()[P_->launchQ]);
    else
        g.textf(84, 438, font::kTiny, Canvas::kCenterY, "T%d S%d  empty slot   Note: draw steps   Fx2: a player", selT_ + 1,
                selS_ + 1);
    if (fillHeld_) {
        g.color(kDesk);
        g.text(763, 438, font::kTiny, "FILL", Canvas::kCenter);
    }
    const char* keys[8] = {"Stop all", "Steps", "Roll", "Player", "Sound", "Mixer", "Song", "Project"};
    drawFKeys(g, keys);
}

// ---------------------------------------------------------------------------
// STEPS
// ---------------------------------------------------------------------------
void Konsol::drawSteps(Canvas& g) {
    const Clip& c = P_->clip[selT_][selS_];
    const int len = c.exists() ? c.lenSteps : 16;
    const NoteBuf& nb = c.notes();
    if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3));
    const int playStep = rt_[selT_].play == selS_ ? rt_[selT_].tick / kTps : -1;
    const Rgb tc = trackColor(selT_);
    const int pages = (len + 15) / 16;
    // overview strip: the whole clip, notes as dashes by pitch, the visible page
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, 52);
    const float sw = 784.0f / float(len);
    rv(g, kPanel2, 8 + int(float(stepPage_ * 16) * sw), kTop + 4, int(16.0f * sw), 48);
    int lo = 127, hi = 0;
    for (int i = 0; i < nb.count; ++i) {
        lo = nb.n[i].pitch < lo ? nb.n[i].pitch : lo;
        hi = nb.n[i].pitch > hi ? nb.n[i].pitch : hi;
    }
    if (hi < lo) lo = hi = 60;
    const float ph = 40.0f / float(hi - lo + 1 > 12 ? hi - lo + 1 : 12);
    for (int i = 0; i < nb.count && i < 128; ++i) {
        const Note& n = nb.n[i];
        g.color(n.pitch == stepNote_ ? tc : scale(tc, 0.45f));
        rv2(g, 8 + int(float(n.start) / float(kTps) * sw), kTop + 48 - int(float(n.pitch - lo) * ph) - 3,
            int(float(n.len) / float(kTps) * sw) + 2, 3);
    }
    if (playStep >= 0) rv(g, kWhite, 8 + int(float(playStep) * sw), kTop + 4, 2, 48);
    // 16 step cells, 2 rows of 8 (like the pads)
    for (int i = 0; i < 16; ++i) {
        const int step = stepPage_ * 16 + i;
        const int x = 4 + (i % 8) * 74, y = kTop + 62 + (i / 8) * 118;
        const bool in = step < len;
        int ni = -1;
        bool other = false;
        if (in)
            for (int k = 0; k < nb.count; ++k)
                if (nb.n[k].start / kTps == step) {
                    if (nb.n[k].pitch == stepNote_) ni = k;
                    else other = true;
                }
        Rgb bg = !in ? kWell : (ni >= 0 ? scale(tc, 0.35f + 0.6f * nb.n[ni].vel / 127.0f) : (other ? kPanel2 : kPanel));
        if (step == playStep) bg = mix(bg, kWhite, 0.4f);
        rv(g, bg, x, y, 70, 112);
        if ((i % 4) == 0 && in) rv(g, kFaint, x, y, 70, 2);
        if (step == stepCursor_ || step == heldStep_) {
            g.color(step == heldStep_ ? kWhite : kAmber);
            rv2(g, x, y + 108, 70, 4);
        }
        if (ni >= 0) {   // the lock value as a bar
            const Note& n = nb.n[ni];
            float v = 0;
            switch (stepLock_) {
                case 0: v = n.vel / 127.0f; break;
                case 1: v = clampf(float(n.len) / float(kTps * 4), 0, 1); break;
                case 2: v = n.chance / 100.0f; break;
                case 3: v = n.roll / 16.0f; break;
                case 4: v = float(n.cond) / float(kCondCount - 1); break;
                case 5: v = (n.tone + 64) / 127.0f; break;
                default: v = (n.flags & kFlagSlide) ? 1.0f : 0.0f; break;
            }
            g.color(kDesk);
            rv2(g, x + 6, y + 56, 58, 8);
            g.color(kText);
            rv2(g, x + 6, y + 56, int(58.0f * v) + 1, 8);
            if (n.roll > 1)
                for (int k = 0; k < n.roll && k < 8; ++k) rv2(g, x + 6 + k * 7, y + 72, 4, 4);
        }
    }
    // the right column: lane + locks
    rv(g, kPanel, 600, kTop + 62, 196, 230);
    rv(g, kPanel, 600, kTop + 298, 196, 70);
    for (int p = 0; p < 8; ++p)
        rv(g, p < pages ? (p == stepPage_ ? kAmber : (playStep / 16 == p ? kFaint : kPanel2)) : kWell, 606 + p * 23, kTop + 340,
           19, 20);
    g.endBatch();
    static const char* const kLock[7] = {"Velocity", "Length", "Chance", "Ratchet", "Condition", "Tone", "Slide"};
    for (int i = 0; i < 16; ++i) {
        const int step = stepPage_ * 16 + i;
        if (step >= len) continue;
        const int x = 4 + (i % 8) * 74, y = kTop + 62 + (i / 8) * 118;
        int ni = -1;
        for (int k = 0; k < nb.count; ++k)
            if (nb.n[k].start / kTps == step && nb.n[k].pitch == stepNote_) ni = k;
        g.color(ni >= 0 ? kDesk : kDim);
        g.number(x + 6, y + 6, font::kSmall, step + 1);
        if (ni < 0) continue;
        const Note& n = nb.n[ni];
        char v[12];
        switch (stepLock_) {
            case 0: std::snprintf(v, sizeof v, "%d", n.vel); break;
            case 1: std::snprintf(v, sizeof v, "%d.%02d", n.len / kTps, (n.len % kTps) * 100 / kTps); break;
            case 2: std::snprintf(v, sizeof v, "%d%%", n.chance); break;
            case 3: std::snprintf(v, sizeof v, n.roll > 1 ? "x%d" : "-", n.roll); break;
            case 4: std::snprintf(v, sizeof v, "%s", condName(n.cond)); break;
            case 5: std::snprintf(v, sizeof v, "%+d", n.tone); break;
            default: std::snprintf(v, sizeof v, (n.flags & kFlagSlide) ? "SLIDE" : "-"); break;
        }
        g.color(kDesk);
        g.text(x + 35, y + 92, font::kBody, v, Canvas::kCenter);
        if (n.cond && stepLock_ != 4) g.text(x + 64, y + 14, font::kTiny, condName(n.cond), Canvas::kCenterY | Canvas::kRight);
    }
    char nn[6];
    g.color(kDim);
    g.text(612, kTop + 80, font::kTiny, "LANE", Canvas::kCenterY);
    g.color(tc);
    g.text(612, kTop + 108, font::kTitle, noteText(stepNote_, nn), Canvas::kCenterY);
    g.color(kDim);
    g.text(612, kTop + 138, font::kTiny, "EDIT (hold a step + turn)", Canvas::kCenterY);
    for (int i = 0; i < 7; ++i) {
        g.color(i == stepLock_ ? kAmber : kText);
        g.textf(612 + (i % 2) * 92, kTop + 160 + (i / 2) * 24, font::kSmall, Canvas::kCenterY, "%s%s", i == stepLock_ ? "> " : "",
                kLock[i]);
    }
    g.color(kText);
    g.textf(612, kTop + 316, font::kSmall, Canvas::kCenterY, "T%d S%d  %d steps", selT_ + 1, selS_ + 1, len);
    g.color(kDim);
    g.textf(612, kTop + 332, font::kTiny, Canvas::kCenterY, "%d notes  page %d/%d", nb.count, stepPage_ + 1, pages);
    g.text(4, 438, font::kTiny,
           "Pads 1-8 rows 1-2: steps   9-12: pages   rows 3-4: keyboard (lane)   Insert: toggle   Up/Down: lane   Shift+Copy: undo",
           Canvas::kCenterY);
    const char* keys[8] = {"Velocity", "Length", "Chance", "Ratchet", "Condition", stepLock_ == 6 ? "Slide" : "Tone",
                           "Length+", "Clear lane"};
    drawFKeys(g, keys, stepLock_ <= 4 ? stepLock_ : 5);
}

// ---------------------------------------------------------------------------
// PIANO ROLL
// ---------------------------------------------------------------------------
void Konsol::drawRoll(Canvas& g) {
    const Clip& c = P_->clip[selT_][selS_];
    const int len = (c.exists() ? c.lenSteps : 16) * kTps;
    const NoteBuf& nb = c.notes();
    static const int kGr[5] = {kTps / 2, kTps, kTps * 2, kTps * 4, kTps * 16};
    static const char* const kGridN[5] = {"1/32", "1/16", "1/8", "1/4", "1 bar"};
    const int g0 = kGr[rollGrid_];
    const int visTicks = len < 64 * kTps ? len : 64 * kTps;
    if (rollTick_ < rollLeft_) rollLeft_ = (rollTick_ / (16 * kTps)) * 16 * kTps;
    if (rollTick_ >= rollLeft_ + visTicks) rollLeft_ = rollTick_ - visTicks + 16 * kTps;
    rollLeft_ = clampi((rollLeft_ / (16 * kTps)) * 16 * kTps, 0, len - visTicks > 0 ? len - visTicks : 0);
    const int gx = 48, gy = kTop + 2, gw = 748, gh = 320;
    const float px = float(gw) / float(visTicks);
    const int rowH = gh / 16;
    const uint16_t mask = scaleMask(selT_);
    const Rgb tc = trackColor(selT_);
    g.beginRects();
    for (int r = 0; r < 16; ++r) {
        const int p = rollTop_ - r;
        const int pc = ((p - P_->key) % 12 + 12) % 12;
        const bool black = (0x54A >> (p % 12)) & 1;
        Rgb rc = (mask >> pc) & 1 ? Rgb(0x18202E) : Rgb(0x0F141D);
        if (pc == 0) rc = Rgb(0x272214);
        if (p == rollPitch_) rc = mix(rc, kAmber, 0.12f);
        rv(g, rc, gx, gy + r * rowH, gw, rowH - 1);
        rv(g, black ? Rgb(0x1E2533) : Rgb(0xCFC8B6), 4, gy + r * rowH, 40, rowH - 1);
        if (p == rollPitch_) rv(g, kAmber, 4, gy + r * rowH, 5, rowH - 1);
    }
    for (int t = 0; t <= visTicks; t += kTps * 4) {
        g.color((t % (kTps * 16)) ? kLine : kFaint);
        rv2(g, gx + int(float(t) * px), gy, (t % (kTps * 16)) ? 1 : 2, gh);
    }
    for (int i = 0; i < nb.count; ++i) {
        const Note& n = nb.n[i];
        const int r = rollTop_ - n.pitch;
        if (r < 0 || r > 15) continue;
        if (n.start + n.len < rollLeft_ || n.start >= rollLeft_ + visTicks) continue;
        const int x = gx + int(float(n.start - rollLeft_) * px);
        int w = int(float(n.len) * px);
        if (w < 3) w = 3;
        if (x + w > gx + gw) w = gx + gw - x;
        rv(g, mix(scale(tc, 0.4f), tc, n.vel / 127.0f), x, gy + r * rowH + 1, w, rowH - 3);
        rv(g, mix(tc, kWhite, 0.5f), x, gy + r * rowH + 1, 2, rowH - 3);
        if (n.flags & kFlagSlide) rv(g, kWhite, x + w - 3, gy + r * rowH + 1, 3, rowH - 3);
        if (n.cond) rv(g, kAmber, x + 3, gy + r * rowH + rowH - 5, w > 8 ? w - 6 : 2, 2);
    }
    if (rt_[selT_].play == selS_) {
        const int t = rt_[selT_].tick;
        if (t >= rollLeft_ && t < rollLeft_ + visTicks) rv(g, kWhite, gx + int(float(t - rollLeft_) * px), gy, 2, gh);
    }
    rv(g, kPanel, 4, kTop + 326, 792, 28);
    g.endBatch();
    {
        const int r = rollTop_ - rollPitch_;
        const int x = gx + int(float(rollTick_ - rollLeft_) * px);
        g.color(kAmber);
        g.frame(x, gy + r * rowH, int(float(g0) * px) + 1, rowH);
    }
    const int eng = P_->track[selT_].engine;
    static const char* const kDr[8] = {"KICK", "SNARE", "CLAP", "CHAT", "OHAT", "TOM", "RIM", "COW"};
    const KitPad* pads = eng == kEngKit ? kitPads(eng_[selT_].e[kEngKit]) : nullptr;
    for (int r = 0; r < 16; ++r) {
        const int p = rollTop_ - r;
        if (eng == kEngDrums && p >= 36) {
            g.color(kDim);
            g.text(gx + 4, gy + r * rowH + rowH / 2, font::kTiny, kDr[(p - 36) % 8], Canvas::kCenterY);
        } else if (pads && p >= kKitBase && p < kKitBase + kKitPads) {
            g.color(kDim);
            g.text(gx + 4, gy + r * rowH + rowH / 2, font::kTiny, pads[p - kKitBase].name, Canvas::kCenterY);
        }
        if (p % 12) continue;
        char nn[6];
        g.color(kDesk);
        g.text(8, gy + r * rowH + rowH / 2, font::kTiny, noteText(p, nn), Canvas::kCenterY);
    }
    const int i = nb.at(rollTick_, rollPitch_);
    char nn[6];
    g.color(kText);
    g.textf(14, kTop + 340, font::kSmall, Canvas::kCenterY, "%s  %d.%d", noteText(rollPitch_, nn), rollTick_ / kTps + 1,
            (rollTick_ % kTps) * 4 / kTps + 1);
    if (i >= 0) {
        const Note& n = nb.n[i];
        g.color(tc);
        g.textf(160, kTop + 340, font::kSmall, Canvas::kCenterY, "len %d.%02d  vel %d%s%s%s", n.len / kTps,
                (n.len % kTps) * 100 / kTps, n.vel, (n.flags & kFlagSlide) ? "  slide" : "", n.cond ? "  " : "",
                n.cond ? condName(n.cond) : "");
    }
    g.color(kDim);
    g.textf(786, kTop + 340, font::kSmall, Canvas::kCenterY | Canvas::kRight, "grid %s   %d notes", kGridN[rollGrid_], nb.count);
    g.text(4, 438, font::kTiny, "Arrows: cursor (Shift: octave)   Insert: add / remove   Delete: remove   Pads: pitch",
           Canvas::kCenterY);
    const char* keys[8] = {"Cursor", "Length", "Velocity", "Move", "Pitch", kGridN[rollGrid_], "Quantise", "Undo"};
    drawFKeys(g, keys, rollFn_);
}

// ---------------------------------------------------------------------------
// PLAYER / GENERATOR
// ---------------------------------------------------------------------------
void Konsol::drawGen(Canvas& g) {
    const Clip& c = P_->clip[selT_][selS_];
    const Rgb tc = trackColor(selT_);
    // left: the player card
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 384, 372);
    rv(g, scale(pageColor(kPgGen), 0.25f), 4, kTop + 2, 384, 50);
    rv(g, genRow_ == 0 ? kAmber : pageColor(kPgGen), 4, kTop + 2, 4, 50);
    for (int i = 0; i < 9; ++i) {
        const int y = kTop + 58 + i * 34;
        if (genRow_ == i + 1) {
            rv(g, kPanel2, 8, y, 376, 30);
            rv(g, kAmber, 8, y, 3, 30);
        }
        if (i < 6) {
            const ot::gen::ParamDesc& pd = genParam(c.gen, i, c.gp);
            if (!pd.name || pd.max <= pd.min) continue;
            const float f = float(c.gp[i] - pd.min) / float(pd.max - pd.min);
            rv(g, kWell, 180, y + 24, 196, 3);
            rv(g, tc, 180, y + 24, int(196.0f * f) + 1, 3);
        }
    }
    g.endBatch();
    g.color(kDim);
    g.text(20, kTop + 16, font::kTiny, "PLAYER", Canvas::kCenterY);
    g.color(genRow_ == 0 ? kAmber : kText);
    g.text(20, kTop + 36, font::kLarge, genName(c.gen), Canvas::kCenterY);
    g.color(kDim);
    g.textf(376, kTop + 36, font::kTiny, Canvas::kCenterY | Canvas::kRight, "T%d S%d", selT_ + 1, selS_ + 1);
    for (int i = 0; i < 6; ++i) {
        const ot::gen::ParamDesc& pd = genParam(c.gen, i, c.gp);
        const int y = kTop + 58 + i * 34 + 13;
        if (!pd.name) continue;
        g.color(genRow_ == i + 1 ? kText : kDim);
        g.text(20, y, font::kSmall, pd.name, Canvas::kCenterY);
        char v[24];
        const int x = c.gp[i];
        if (pd.labels && x >= pd.min && x <= pd.max) copyStr(v, sizeof v, pd.labels[x - pd.min]);
        else if (i == 5 && x == 0) copyStr(v, sizeof v, "random");
        else std::snprintf(v, sizeof v, "%d", x);
        g.color(genRow_ == i + 1 ? kAmber : kText);
        g.text(376, y, font::kSmall, v, Canvas::kCenterY | Canvas::kRight);
    }
    if (c.gen == kGenNotes) {   // no player yet: the menu of players
        for (int k = 1; k < kGenCount; ++k) {
            const int x = 20 + ((k - 1) % 2) * 182, y = kTop + 74 + ((k - 1) / 2) * 30;
            g.color(kText);
            g.text(x, y, font::kSmall, genName(k), Canvas::kCenterY);
        }
        g.color(kDim);
        g.text(20, kTop + 238, font::kTiny, "Row 1 + encoder: choose a player, then F1 writes a take", Canvas::kCenterY);
        g.text(20, kTop + 254, font::kTiny, "or F3 makes it live (a new variation every loop)", Canvas::kCenterY);
    }
    static const char* const kRowN[3] = {"Length", "Live", "Every"};
    for (int i = 0; i < 3; ++i) {
        const int y = kTop + 58 + (6 + i) * 34 + 13;
        g.color(genRow_ == 7 + i ? kText : kDim);
        g.text(20, y, font::kSmall, kRowN[i], Canvas::kCenterY);
    }
    g.color(genRow_ == 7 ? kAmber : kText);
    g.textf(376, kTop + 58 + 6 * 34 + 13, font::kSmall, Canvas::kCenterY | Canvas::kRight, "%d steps", c.exists() ? c.lenSteps : 0);
    g.color(c.live ? kGreen : kDim);
    g.text(376, kTop + 58 + 7 * 34 + 13, font::kSmall, c.live ? "ON - varies while it plays" : "off - write once",
           Canvas::kCenterY | Canvas::kRight);
    g.color(genRow_ == 9 ? kAmber : kText);
    g.textf(376, kTop + 58 + 8 * 34 + 13, font::kSmall, Canvas::kCenterY | Canvas::kRight, "%d loop%s", c.every,
            c.every > 1 ? "s" : "");
    // right: what it does + the clip's notes
    const NoteBuf& nb = c.notes();
    const int len = c.exists() ? c.lenSteps * kTps : 16 * kTps;
    int lo = 127, hi = 0;
    for (int i = 0; i < nb.count; ++i) {
        lo = nb.n[i].pitch < lo ? nb.n[i].pitch : lo;
        hi = nb.n[i].pitch > hi ? nb.n[i].pitch : hi;
    }
    if (hi < lo) lo = hi = 60;
    if (hi - lo < 12) {
        lo = (lo + hi) / 2 - 6;
        hi = lo + 12;
    }
    const int bx = 396, by = kTop + 70, bw = 400, bh = 300;
    const float px = float(bw) / float(len), py = float(bh) / float(hi - lo + 1);
    g.beginRects();
    rv(g, kPanel, bx, kTop + 2, bw, 64);
    rv(g, kWell, bx, by, bw, bh);
    g.color(kLine);
    for (int t = 0; t <= len; t += kTps * 4) rv2(g, bx + int(float(t) * px), by, 1, bh);
    for (int i = 0; i < nb.count; ++i) {
        const Note& n = nb.n[i];
        g.color(mix(scale(tc, 0.4f), tc, n.vel / 127.0f));
        const int w = int(float(n.len) * px) > 2 ? int(float(n.len) * px) : 2;
        rv2(g, bx + int(float(n.start) * px), by + bh - int(float(n.pitch - lo + 1) * py), w, int(py) > 2 ? int(py) - 1 : 2);
    }
    if (rt_[selT_].play == selS_) rv(g, kWhite, bx + int(float(rt_[selT_].tick) * px), by, 2, bh);
    if (c.pending) rv(g, kGreen, bx + bw - 12, by + 6, 6, 6);
    g.endBatch();
    g.color(kText);
    g.text(bx + 12, kTop + 20, font::kSmall, genInfo(c.gen), Canvas::kCenterY);
    char nn[6], nr[6];
    g.color(kDim);
    g.textf(bx + 12, kTop + 44, font::kTiny, Canvas::kCenterY, "Key %s %s   root %s   %d notes%s", noteText(P_->key + 60, nn),
            scaleName(P_->scale), noteText(rootNote(selT_), nr), nb.count,
            c.live && rt_[selT_].play == selS_ ? "   live: next take at the loop" : "");
    g.text(4, 438, font::kTiny, "Up/Down: row   Encoder: value (Shift x10)   Pads: clips (like the session)", Canvas::kCenterY);
    const char* keys[8] = {"Write", "New seed", c.live ? "Live ON" : "Live off", "Every", "To roll", "Undo", "Clear",
                           c.launchQ ? launchQNames()[c.launchQ - 1] : "Launch Q"};
    drawFKeys(g, keys, -1, c.live ? 4u : 0u);
}

// ---------------------------------------------------------------------------
// SOUND (the track's engine)
// ---------------------------------------------------------------------------
void Konsol::drawSound(Canvas& g) {
    Engine* en = engine(selT_);
    const int ek = P_->track[selT_].engine;
    const Rgb tc = trackColor(selT_);
    if (sndEngine_) {   // the engine chooser: 16 engines as cards
        g.beginRects();
        for (int i = 0; i < kEngCount; ++i) {
            const int x = 4 + (i % 4) * 198, y = kTop + 4 + (i / 4) * 66;
            rv(g, i == sndPick_ ? kPanel2 : kPanel, x, y, 194, 62);
            rv(g, i == sndPick_ ? kAmber : (i == ek ? tc : kFaint), x, y, 4, 62);
        }
        rv(g, kPanel, 4, kTop + 272, 792, 100);
        g.endBatch();
        for (int i = 0; i < kEngCount; ++i) {
            const int x = 4 + (i % 4) * 198, y = kTop + 4 + (i / 4) * 66;
            g.color(i == sndPick_ ? kAmber : kText);
            g.text(x + 16, y + 22, font::kBody, engineName(i), Canvas::kCenterY);
            if (i == ek) {
                g.color(tc);
                g.text(x + 184, y + 22, font::kTiny, "NOW", Canvas::kCenterY | Canvas::kRight);
            }
        }
        g.color(kText);
        g.text(20, kTop + 300, font::kLarge, engineName(sndPick_), Canvas::kCenterY);
        g.color(kDim);
        g.text(20, kTop + 330, font::kSmall, engineInfo(sndPick_), Canvas::kCenterY);
        g.text(20, kTop + 356, font::kTiny, "Encoder / arrows: choose   Insert: use it on this track   F3: back", Canvas::kCenterY);
        const char* keys[8] = {"", "", "Back", "", "", "", "", ""};
        drawFKeys(g, keys, 2);
        return;
    }
    const int ns = en->sections();
    if (sndSec_ >= ns) sndSec_ = 0;
    uint8_t ids[64];
    const int n = en->sectionParams(sndSec_, ids, 64);
    if (sndCol_ >= n) sndCol_ = n > 0 ? n - 1 : 0;
    // engine plate + section chips
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, 52);
    rv(g, tc, 4, kTop + 2, 5, 52);
    const int tw = ns > 0 ? (792 / (ns < 6 ? 6 : ns)) : 132;
    for (int s = 0; s < ns; ++s) rv(g, s == sndSec_ ? tc : kPanel, 4 + s * tw, kTop + 60, tw - 4, 26);
    g.endBatch();
    g.color(tc);
    g.text(22, kTop + 28, font::kTitle, engineName(ek), Canvas::kCenterY);
    g.color(kDim);
    g.text(200, kTop + 20, font::kSmall, engineInfo(ek), Canvas::kCenterY);
    g.textf(200, kTop + 40, font::kTiny, Canvas::kCenterY, "T%d   section %d/%d   %d parameters   F3: engine", selT_ + 1,
            sndSec_ + 1, ns, en->paramCount());
    for (int s = 0; s < ns; ++s) {
        g.color(s == sndSec_ ? kDesk : kText);
        g.text(4 + s * tw + (tw - 4) / 2, kTop + 73, font::kSmall, en->sectionName(s), Canvas::kCenter);
    }
    // knobs: 2 rows of 6 (a section of up to 12; more = the window around the cursor)
    const int per = 12;
    const int first = n > per ? (sndCol_ / per) * per : 0;
    const kit::Registry& R = ctx_->params();
    for (int k = 0; k < per && first + k < n; ++k) {
        const int i = ids[first + k];
        const int cx = 70 + (k % 6) * 132, cy = kTop + 150 + (k / 6) * 120;
        char nm[24], v[24];
        en->paramName(i, nm, sizeof nm);
        en->format(i, v, sizeof v);
        if (std::strlen(nm) > 12) nm[12] = 0;
        if (std::strlen(v) > 9) v[9] = 0;
        const kit::ParamId id = slotId(selT_, i);
        const float mod = id != kit::kNoParam && R.meta(id).mods ? R.effectiveNorm(id) : -1.0f;
        knob(g, cx, cy, 30, en->frac(i), mod, tc, nm, v, first + k == sndCol_, en->bipolar(i));
    }
    // the sample of sample-based engines
    if (ek == kEngSampler || ek == kEngGrain) {
        const TrackSample& ts = ts_[selT_];
        if (pool_.valid(ts.slot)) {
            const SampleSlot& sl = pool_.slot(ts.slot);
            drawWave(g, ts.slot, 0, sl.frames, 4, kTop + 334, 520, 36, scale(tc, 0.8f), 6);
            g.color(kDim);
            g.textf(534, kTop + 344, font::kTiny, Canvas::kCenterY, "%.26s", pool_.slot(ts.src).path[0] ? pool_.slot(ts.src).path : "built-in break");
            g.textf(534, kTop + 362, font::kTiny, Canvas::kCenterY, "%s  %d bars  %d BPM  %s", sl.ch == 2 ? "stereo" : "mono",
                    ts.bars, int(ts.outBpm + 0.5f), ts.stretched ? "stretched" : "");
        }
    } else {
        // voices and the engine's share of the CPU budget (the governor steals above it)
        const int32_t cst = en->cost();
        const int32_t bud = engineBudget(-1);
        const float f = bud > 0 ? float(cst) / float(bud) : 0.0f;
        g.beginRects();
        rv(g, kPanel, 4, kTop + 334, 792, 36);
        rv(g, kWell, 300, kTop + 348, 300, 8);
        rv(g, f > 0.8f ? kRed : (f > 0.5f ? kAmber : kGreen), 300, kTop + 348, int(300.0f * clampf(f, 0, 1)) + 1, 8);
        const float lv = clampf(meter_[selT_], 0, 1);
        rv(g, kWell, 640, kTop + 348, 140, 8);
        rv(g, tc, 640, kTop + 348, int(140.0f * lv) + 1, 8);
        g.endBatch();
        g.color(kDim);
        g.textf(16, kTop + 352, font::kSmall, Canvas::kCenterY, "voices %d   load %d.%dk", en->active(), int(cst / 1000),
                int((cst % 1000) / 100));
        g.text(296, kTop + 352, font::kTiny, "CPU", Canvas::kCenterY | Canvas::kRight);
        g.text(636, kTop + 352, font::kTiny, "OUT", Canvas::kCenterY | Canvas::kRight);
        g.text(4, 438, font::kTiny,
               "Up/Down: section   Left/Right: parameter   Encoder: value (Shift: coarse)   Delete: default   Pads: play",
               Canvas::kCenterY);
    }
    char oct[12];
    std::snprintf(oct, sizeof oct, "Oct %+d", keyOct_);
    const char* keys[8] = {"< Section", "Section >", "Engine", "Init", "Preset", "Sample", oct, "Keys"};
    drawFKeys(g, keys, -1, keysOverlay_ ? 128u : 0u);
}

// ---------------------------------------------------------------------------
// waveforms
// ---------------------------------------------------------------------------
void Konsol::drawWave(Canvas& g, int slot, uint32_t a, uint32_t e, int x, int y, int w, int h, Rgb c, int colW) {
    if (!pool_.valid(slot) || e <= a) return;
    const SampleSlot& sl = pool_.slot(slot);
    const int16_t* L = pool_.left(slot);
    const int16_t* R = pool_.right(slot);
    const int cols = w / colW;
    const uint32_t span = e - a;
    const int mid = y + h / 2;
    g.color(c);
    g.beginRects();
    for (int k = 0; k < cols; ++k) {
        const uint32_t s0 = a + uint32_t(uint64_t(span) * uint32_t(k) / uint32_t(cols));
        uint32_t s1 = a + uint32_t(uint64_t(span) * uint32_t(k + 1) / uint32_t(cols));
        if (s1 > sl.frames) s1 = sl.frames;
        const uint32_t stride = (s1 - s0) / 24 + 1;
        int pk = 0;
        for (uint32_t i = s0; i < s1; i += stride) {
            int v = L[i] < 0 ? -L[i] : L[i];
            if (sl.ch == 2) {
                const int r = R[i] < 0 ? -R[i] : R[i];
                v = r > v ? r : v;
            }
            pk = v > pk ? v : pk;
        }
        const int hh = pk * (h / 2) / 32768 + 1;
        rv2(g, x + k * colW, mid - hh, colW - 1, 2 * hh);
    }
    g.endBatch();
}

// ---------------------------------------------------------------------------
// SAMPLE: file, tempo, stretch
// ---------------------------------------------------------------------------
void Konsol::drawSample(Canvas& g) {
    const TrackSample& ts = ts_[selT_];
    const Rgb pc = pageColor(kPgSample);
    const Rgb tc = trackColor(selT_);
    const bool ok = pool_.valid(ts.slot);
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, 150);
    rv(g, kPanel, 4, kTop + 158, 388, 214);
    rv(g, kPanel, 398, kTop + 158, 398, 214);
    g.endBatch();
    if (ok) {
        const SampleSlot& sl = pool_.slot(ts.slot);
        drawWave(g, ts.slot, 0, sl.frames, 12, kTop + 30, 776, 116, scale(tc, 0.85f), 5);
        // slice markers (the playing version's onsets)
        g.beginRects();
        for (int k = 1; k < sl.nOn; ++k) {
            g.color(classColor(sl.cls[k]));
            rv2(g, 12 + int(uint64_t(776) * sl.onset[k] / (sl.frames ? sl.frames : 1)), kTop + 30, 1, 116);
        }
        // bar lines
        if (ts.bars > 0) {
            g.color(kFaint);
            for (int b = 1; b < ts.bars; ++b) rv2(g, 12 + 776 * b / ts.bars, kTop + 26, 2, 4);
        }
        g.endBatch();
        const SampleSlot& src = pool_.slot(pool_.valid(ts.src) ? ts.src : ts.slot);
        g.color(kText);
        g.textf(14, kTop + 16, font::kSmall, Canvas::kCenterY, "%.48s", src.path[0] ? src.path : "built-in break (172 BPM)");
        g.color(kDim);
        g.textf(786, kTop + 16, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%s  %lu.%02lu s  %d slices", sl.ch == 2 ? "STEREO" : "MONO",
                static_cast<unsigned long>(sl.frames / 44100), static_cast<unsigned long>((sl.frames % 44100) / 441), sl.nOn);
    } else {
        g.color(kDim);
        g.text(400, kTop + 80, font::kBody, "No sample: F3 loads one, F4 the built-in break", Canvas::kCenter);
    }
    // tempo card
    char v[24];
    g.color(pc);
    g.text(16, kTop + 174, font::kTiny, "TEMPO", Canvas::kCenterY);
    std::snprintf(v, sizeof v, ts.bars ? "%d bars" : "detect", ts.bars);
    barRow(g, 10, kTop + 186, 376, "Length", v, float(ts.bars) / 32.0f, pc, smpRow_ == 0);
    g.color(kText);
    g.textf(20, kTop + 244, font::kLarge, Canvas::kCenterY, "%.1f", double(ts.srcBpm));
    g.color(kDim);
    g.text(110, kTop + 248, font::kTiny, "BPM (file)", Canvas::kCenterY);
    g.color(kAmber);
    g.textf(200, kTop + 244, font::kLarge, Canvas::kCenterY, "%.1f", double(ctx_->bpm()));
    g.color(kDim);
    g.text(290, kTop + 248, font::kTiny, "BPM (project)", Canvas::kCenterY);
    g.textf(20, kTop + 280, font::kTiny, Canvas::kCenterY, "Plays at %.1f BPM: %s", double(ts.outBpm),
            ts.stretched ? "the stretched copy" : "the original file");
    g.text(20, kTop + 300, font::kTiny, "SAMPLER Warp (Sound page) follows the tempo live;", Canvas::kCenterY);
    g.text(20, kTop + 316, font::kTiny, "F5 renders a high-quality copy at the tempo.", Canvas::kCenterY);
    // stretch card
    g.color(pc);
    g.text(410, kTop + 174, font::kTiny, "TIME-STRETCH", Canvas::kCenterY);
    barRow(g, 404, kTop + 186, 386, "Mode", stretchModeName(ts.mode), float(ts.mode) / 2.0f, pc, smpRow_ == 1);
    std::snprintf(v, sizeof v, "%+d st", ts.pitch);
    barRow(g, 404, kTop + 218, 386, "Pitch", v, (float(ts.pitch) + 24.0f) / 48.0f, pc, smpRow_ == 2);
    static const char* const kModeInfo[3] = {"Beats: transient-locked WSOLA, hits stay crisp",
                                             "Tonal: phase vocoder, pads / melodies",
                                             "Repitch: tape-style, speed and pitch together"};
    g.color(kDim);
    g.text(414, kTop + 266, font::kTiny, kModeInfo[ts.mode % 3], Canvas::kCenterY);
    const float sp = stretchProgress();
    if (sp >= 0.0f && strTrack_ == selT_) {
        g.beginRects();
        rv(g, kWell, 414, kTop + 290, 366, 14);
        rv(g, pc, 414, kTop + 290, int(366.0f * sp) + 1, 14);
        g.endBatch();
        g.color(kText);
        g.textf(414, kTop + 320, font::kSmall, Canvas::kCenterY, "Stretching... %d%%", int(sp * 100.0f));
    } else {
        g.color(ts.stretched ? kGreen : kDim);
        g.textf(414, kTop + 300, font::kSmall, Canvas::kCenterY, "%s", ts.stretched ? "Stretched to the project tempo"
                                                                                     : "Original (F5: stretch)");
    }
    g.color(kDim);
    g.text(4, 438, font::kTiny, "Up/Down: row   Encoder: value   Pads rows 1-3: slices (hear them)   F2 / S.Edit: slice editor",
           Canvas::kCenterY);
    const char* keys[8] = {"Tempo", "Slices", "Load", "Built-in", "Stretch", "Original", "To KIT", "SAMPLER"};
    drawFKeys(g, keys, 0, ts.stretched ? 16u : 0u);
}

// SAMPLE: the slice editor
void Konsol::drawSlices(Canvas& g) {
    const TrackSample& ts = ts_[selT_];
    const int s = ts.src;
    const Rgb tc = trackColor(selT_);
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, 170);
    rv(g, kPanel, 4, kTop + 178, 520, 194);
    rv(g, kPanel, 530, kTop + 178, 266, 194);
    g.endBatch();
    if (!pool_.valid(s)) {
        g.color(kDim);
        g.text(400, kTop + 80, font::kBody, "No sample on this track", Canvas::kCenter);
        const char* keys[8] = {"Tempo", "Slices", "", "", "", "", "", ""};
        drawFKeys(g, keys, 1);
        return;
    }
    const SampleSlot& sl = pool_.slot(s);
    if (sliceSel_ >= sl.nOn) sliceSel_ = sl.nOn > 0 ? sl.nOn - 1 : 0;
    uint32_t a = 0, e = sl.frames;
    pool_.sliceRange(s, sliceSel_, a, e);
    // the whole sample with the slice points; the selected slice highlighted
    const int wx = 12, ww = 776;
    const float k = float(ww) / float(sl.frames ? sl.frames : 1);
    g.beginRects();
    rv(g, scale(tc, 0.18f), wx + int(float(a) * k), kTop + 12, int(float(e - a) * k) + 1, 150);
    g.endBatch();
    drawWave(g, s, 0, sl.frames, wx, kTop + 12, ww, 150, scale(tc, 0.85f), 5);
    g.beginRects();
    for (int i = 0; i < sl.nOn; ++i) {
        const int x = wx + int(float(sl.onset[i]) * k);
        g.color(i == sliceSel_ ? kWhite : classColor(sl.cls[i]));
        rv2(g, x, kTop + 8, i == sliceSel_ ? 2 : 1, 158);
        rv2(g, x, kTop + 8, 6, 5);
    }
    g.endBatch();
    // zoom: the selected slice point +-3000 frames (move it with the encoder)
    const uint32_t c = sl.onset[sliceSel_];
    const uint32_t za = c > 3000 ? c - 3000 : 0;
    const uint32_t ze = c + 3000 < sl.frames ? c + 3000 : sl.frames;
    drawWave(g, s, za, ze, 12, kTop + 190, 504, 150, scale(tc, 0.85f), 5);
    g.beginRects();
    rv(g, kWhite, 12 + int(uint64_t(504) * (c - za) / (ze - za ? ze - za : 1)), kTop + 186, 2, 158);
    g.endBatch();
    g.color(kDim);
    g.text(14, kTop + 356, font::kTiny, "ZOOM  +-68 ms around the point", Canvas::kCenterY);
    // info
    g.color(classColor(sl.cls[sliceSel_]));
    g.textf(546, kTop + 204, font::kTitle, Canvas::kCenterY, "%d", sliceSel_ + 1);
    g.color(kText);
    g.textf(600, kTop + 204, font::kSmall, Canvas::kCenterY, "of %d  %s", sl.nOn, hitClassName(sl.cls[sliceSel_]));
    g.color(kDim);
    g.textf(546, kTop + 236, font::kSmall, Canvas::kCenterY, "at %lu ms", static_cast<unsigned long>(c / 44));
    g.textf(546, kTop + 258, font::kSmall, Canvas::kCenterY, "length %lu ms", static_cast<unsigned long>((e - a) / 44));
    g.textf(546, kTop + 280, font::kSmall, Canvas::kCenterY, "strength %d%%", int(sl.strength[sliceSel_] * 100.0f));
    g.textf(546, kTop + 302, font::kTiny, Canvas::kCenterY, "sensitivity %d%% (Shift+F8)", int(sliceSens_ * 100.0f + 0.5f));
    g.text(546, kTop + 326, font::kTiny, "Left/Right: point   Encoder: move", Canvas::kCenterY);
    g.text(546, kTop + 342, font::kTiny, "Pads: hear / pick a slice", Canvas::kCenterY);
    g.text(546, kTop + 358, font::kTiny, "the first point stays at 0", Canvas::kCenterY);
    const char* keys[8] = {"Tempo", "Slices", "", "", "Add point", "Del point", "To KIT", "Detect"};
    drawFKeys(g, keys, 1);
}

// ---------------------------------------------------------------------------
// KIT
// ---------------------------------------------------------------------------
void Konsol::drawKit(Canvas& g) {
    Engine* ke = eng_[selT_].e[kEngKit];
    const KitPad* pads = kitPads(ke);
    const Rgb pc = pageColor(kPgKit);
    const bool isKit = P_->track[selT_].engine == kEngKit;
    const int hit = kitLastHit(ke);
    if (kitFollow_ && hit >= 0 && hit != kitSel_ && isKit) {
        kitSel_ = hit;
        kitSelect(ke, hit);
    }
    // 4 x 4 pads (pad 1 bottom left)
    g.beginRects();
    for (int i = 0; i < kKitPads; ++i) {
        const int col = i % 4, row = 3 - i / 4;
        const int x = 4 + col * 96, y = kTop + 2 + row * 93;
        const KitPad& p = pads[i];
        const bool has = pool_.valid(p.slot);
        Rgb bg = has ? scale(classColor(p.cls), 0.22f) : kWell;
        if (i == hit && rt_[selT_].flash) bg = scale(classColor(p.cls), 0.7f);
        rv(g, i == kitSel_ ? kPanel2 : kPanel, x, y, 92, 89);
        rv(g, bg, x + 3, y + 3, 86, 83);
        if (has) rv(g, classColor(p.cls), x + 3, y + 3, 86, 3);
        if (has) rv(g, kText, x + 8, y + 76, int(78.0f * clampf(p.gain / 2.0f, 0, 1)) + 1, 3);
        if (p.choke) rv(g, kAmber, x + 78, y + 10, 6, 6);
    }
    g.endBatch();
    if (kitSel_ >= 0) {
        const int col = kitSel_ % 4, row = 3 - kitSel_ / 4;
        g.color(kWhite);
        g.frame(4 + col * 96, kTop + 2 + row * 93, 92, 89);
    }
    for (int i = 0; i < kKitPads; ++i) {
        const int col = i % 4, row = 3 - i / 4;
        const int x = 4 + col * 96, y = kTop + 2 + row * 93;
        const KitPad& p = pads[i];
        g.color(kDim);
        g.number(x + 8, y + 10, font::kTiny, i + 1);
        char nm[11];
        std::snprintf(nm, sizeof nm, "%s", p.name);
        g.color(pool_.valid(p.slot) ? kText : kFaint);
        g.text(x + 46, y + 46, font::kTiny, nm, Canvas::kCenter);
    }
    // the selected pad
    const KitPad& sp = pads[kitSel_];
    g.beginRects();
    rv(g, kPanel, 392, kTop + 2, 404, 372);
    rv(g, scale(pc, 0.25f), 392, kTop + 2, 404, 60);
    rv(g, classColor(sp.cls), 392, kTop + 2, 5, 60);
    g.endBatch();
    g.color(kText);
    g.textf(408, kTop + 22, font::kLarge, Canvas::kCenterY, "%d  %s", kitSel_ + 1, sp.name);
    g.color(kDim);
    g.textf(408, kTop + 46, font::kTiny, Canvas::kCenterY, "%s   %s%s", hitClassName(sp.cls),
            pool_.valid(sp.slot) ? (pool_.slot(sp.slot).path[0] ? "file" : "built-in break") : "empty",
            isKit ? "" : "   (track engine is not KIT: F1 builds one)");
    if (pool_.valid(sp.slot)) {
        const uint32_t e = sp.e ? sp.e : pool_.slot(sp.slot).frames;
        drawWave(g, sp.slot, sp.a, e, 404, kTop + 66, 380, 34, scale(classColor(sp.cls), 0.9f), 8);
    }
    static const char* const kRowN[9] = {"Tune", "Fine", "Level", "Pan", "Decay", "Filter", "Resonance", "Choke", "Reverse"};
    for (int r = 0; r < 9; ++r) {
        char v[16];
        ke->format(kpTune + r, v, sizeof v);
        barRow(g, 398, kTop + 104 + r * 27, 392, kRowN[r], v, ke->frac(kpTune + r), pc, r == kitRow_, 25);
    }
    g.color(kDim);
    g.text(4, 438, font::kTiny,
           "Pads 1-16 (4x4): play + select   cols 5-12: tune the pad by ear   Up/Down: row   Left/Right: pad   Insert: from slices",
           Canvas::kCenterY);
    const char* keys[8] = {"From slices", "By class", "Folder", "Pad sample", "Load kit", "Save kit", "Follow", "Clear pad"};
    drawFKeys(g, keys, -1, kitFollow_ ? 64u : 0u);
}

// ---------------------------------------------------------------------------
// MIXER
// ---------------------------------------------------------------------------
void Konsol::drawMixer(Canvas& g) {
    const int sw = 86;
    g.beginRects();
    for (int t = 0; t < kTracks; ++t) {
        const int x = 4 + t * sw;
        const Mix& m = P_->track[t].mix;
        const bool sel = t == mixCol_;
        rv(g, sel ? kPanel2 : kPanel, x, kTop + 2, sw - 4, 372);
        rv(g, trackColor(t), x, kTop + 2, sw - 4, 4);
        // pan (row 1) and the two sends (rows 2, 3)
        for (int k = 0; k < 3; ++k) {
            const int y = kTop + 40 + k * 30;
            rv(g, (sel && mixRow_ == k + 1) ? kAmber : kWell, x + 6, y + 14, sw - 16, 4);
            if (k == 0) {
                const int cx = x + 6 + int(float(sw - 16) * (m.pan * 0.5f + 0.5f));
                rv(g, kText, cx - 2, y + 9, 4, 14);
            } else {
                const float v = k == 1 ? m.sendA : m.sendB;
                rv(g, k == 1 ? kTeal : Rgb(0xA77BFF), x + 6, y + 14, int(float(sw - 16) * v) + 1, 4);
            }
        }
        // fader slot, meter with peak hold, cap
        const int fy0 = kTop + 136, fh = 190;
        rv(g, kWell, x + 18, fy0, 6, fh);
        rv(g, kWell, x + 50, fy0, 12, fh);
        const float lv = clampf(meter_[t], 0, 1);
        const Rgb mc = lv > 0.9f ? kRed : (lv > 0.7f ? kAmber : trackColor(t));
        rv(g, mc, x + 50, fy0 + fh - int(float(fh) * lv), 12, int(float(fh) * lv) + 1);
        const float hold = clampf(meterHold_[t], 0, 1);
        rv(g, kText, x + 50, fy0 + fh - int(float(fh) * hold), 12, 2);
        rv(g, kFaint, x + 26, fy0 + fh - int(float(fh) * 0.8f / 1.25f), 8, 1);
        const int fy = fy0 + fh - int(float(fh) * m.level / 1.25f);
        rv(g, (sel && mixRow_ == 0) ? kAmber : kText, x + 6, fy - 8, 30, 16);
        rv(g, kDesk, x + 6, fy - 1, 30, 2);
        // mute / solo / stem
        rv(g, m.mute ? kMute : Rgb(0x2A2418), x + 4, kTop + 334, 24, 20);
        rv(g, m.solo ? kBlue : Rgb(0x18202E), x + 30, kTop + 334, 24, 20);
        rv(g, stemColor(m.stem), x + 56, kTop + 334, 22, 20);
        if (m.midiOut) rv(g, kGreen, x + 4, kTop + 358, 74, 3);
        if (sel && mixRow_ >= 4) rv(g, kAmber, x + 4, mixRow_ == 4 ? kTop + 356 : kTop + 364, 74, 2);
    }
    // master strip
    rv(g, kPanel, 696, kTop + 2, 100, 372);
    rv(g, kAmber, 696, kTop + 2, 100, 4);
    {
        const int fy0 = kTop + 136, fh = 190;
        rv(g, kWell, 712, fy0, 12, fh);
        rv(g, kWell, 730, fy0, 12, fh);
        const float lv = clampf(masterMeter_, 0, 1);
        const Rgb mc = lv > 0.9f ? kRed : kAmber;
        rv(g, mc, 712, fy0 + fh - int(float(fh) * lv), 12, int(float(fh) * lv) + 1);
        rv(g, mc, 730, fy0 + fh - int(float(fh) * lv), 12, int(float(fh) * lv) + 1);
        const float gr = clampf(compGr_ / 20.0f, 0, 1);
        rv(g, kWell, 758, fy0, 12, fh);
        rv(g, kRed, 758, fy0, 12, int(float(fh) * gr) + 1);
    }
    g.endBatch();
    for (int t = 0; t < kTracks; ++t) {
        const int x = 4 + t * sw;
        const Mix& m = P_->track[t].mix;
        g.color(kText);
        g.textf(x + 6, kTop + 22, font::kSmall, Canvas::kCenterY, "%d %.5s", t + 1, engineName(P_->track[t].engine));
        g.color(kDim);
        g.text(x + 6, kTop + 46, font::kTiny, "PAN", Canvas::kCenterY);
        g.text(x + 6, kTop + 76, font::kTiny, "DLY", Canvas::kCenterY);
        g.text(x + 6, kTop + 106, font::kTiny, "REV", Canvas::kCenterY);
        g.color(kDesk);
        g.text(x + 16, kTop + 344, font::kTiny, "M", Canvas::kCenter);
        g.text(x + 42, kTop + 344, font::kTiny, "S", Canvas::kCenter);
        static const char* const kSt[5] = {"1", "2", "3", "4", "-"};
        g.text(x + 67, kTop + 344, font::kTiny, kSt[m.stem < 4 ? m.stem : 4], Canvas::kCenter);
        g.color(t == mixCol_ ? kAmber : kText);
        g.textf(x + 41, kTop + 124, font::kTiny, Canvas::kCenter, "%d", int(m.level * 100.0f + 0.5f));
    }
    g.color(kText);
    g.text(746, kTop + 22, font::kSmall, "MASTER", Canvas::kCenter);
    g.color(kDim);
    g.text(727, kTop + 124, font::kTiny, "OUT", Canvas::kCenter);
    g.text(764, kTop + 124, font::kTiny, "GR", Canvas::kCenter);
    g.textf(746, kTop + 344, font::kTiny, Canvas::kCenter, "%d%%  -%.1f", int(P_->fx.master * 100.0f + 0.5f), double(compGr_));
    static const char* const kRowN[6] = {"Level", "Pan", "Delay send", "Reverb send", "Stem group", "MIDI out"};
    g.color(kAmber);
    g.textf(4, 438, font::kTiny, Canvas::kCenterY, "T%d  %s", mixCol_ + 1, kRowN[mixRow_]);
    g.color(kDim);
    g.text(140, 438, font::kTiny, "Arrows: strip / row   Encoder: value   Pads: mute, solo, select, stem   Insert: mute",
           Canvas::kCenterY);
    const char* keys[8] = {"Channels", "FX/Master", "Mute", "Solo", "MIDI out", "Reset ch", "Unsolo all", "Unmute all"};
    drawFKeys(g, keys, 0);
}

// MIXER: send effects + master bus
void Konsol::drawFx(Canvas& g) {
    const FxCfg& f = P_->fx;
    static const char* const kSync[6] = {"1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2"};
    struct Row {
        const char* name;
        float frac;
        char val[16];
    } rows[16];
    auto set = [&](int i, const char* n, float fr, const char* fmt, double v) {
        rows[i].name = n;
        rows[i].frac = fr;
        std::snprintf(rows[i].val, sizeof rows[i].val, fmt, v);
    };
    set(0, "Type", f.dlyType / 2.0f, "", 0.0);
    std::snprintf(rows[0].val, sizeof rows[0].val, "%s", dlyTypeName(f.dlyType));
    set(1, "Time", f.delaySync / 5.0f, "", 0.0);
    std::snprintf(rows[1].val, sizeof rows[1].val, "%s", kSync[f.delaySync % 6]);
    set(2, "Feedback", f.delayFb / 0.95f, "%.0f%%", double(f.delayFb * 100.0f));
    set(3, "Tone", f.delayTone, "%.0f%%", double(f.delayTone * 100.0f));
    set(4, f.dlyType == 2 ? "Crush" : "Wow", f.dlyMod, "%.0f%%", double(f.dlyMod * 100.0f));
    set(5, "Ping-pong", float(f.pingPong), "", 0.0);
    std::snprintf(rows[5].val, sizeof rows[5].val, "%s", f.pingPong ? "On" : "Off");
    set(6, "Return", f.retA / 1.5f, "%.0f%%", double(f.retA * 100.0f));
    set(7, "Type", f.revType / 3.0f, "", 0.0);
    std::snprintf(rows[7].val, sizeof rows[7].val, "%s", revTypeName(f.revType));
    set(8, "Size", f.revSize, "%.0f%%", double(f.revSize * 100.0f));
    set(9, "Damping", f.revDamp, "%.0f%%", double(f.revDamp * 100.0f));
    set(10, "Pre-delay", f.revPre, "%.0f ms", double(f.revPre * 120.0f));
    set(11, "Return", f.retB / 1.5f, "%.0f%%", double(f.retB * 100.0f));
    set(12, "Threshold", f.compThr, "%.0f dB", double(-30.0f + 30.0f * f.compThr));
    set(13, "Ratio", f.compRatio, "%.1f:1", double(1.0f + 9.0f * f.compRatio));
    set(14, "Make-up", f.compMakeup, "+%.1f dB", double(12.0f * f.compMakeup));
    set(15, "Master", f.master / 1.25f, "%.0f%%", double(f.master * 100.0f));
    static const struct {
        const char* title;
        int first, n, x;
        Rgb c;
    } kCard[3] = {{"DELAY", 0, 7, 4, Rgb(0x2EC4B6)}, {"REVERB", 7, 5, 270, Rgb(0xA77BFF)}, {"MASTER", 12, 4, 536, Rgb(0xFFB000)}};
    g.beginRects();
    for (const auto& cd : kCard) {
        rv(g, kPanel, cd.x, kTop + 2, 260, 372);
        rv(g, cd.c, cd.x, kTop + 2, 260, 4);
    }
    // GR + out meters on the master card
    const float gr = clampf(compGr_ / 20.0f, 0, 1), lv = clampf(masterMeter_, 0, 1);
    rv(g, kWell, 560, kTop + 200, 100, 12);
    rv(g, kRed, 560, kTop + 200, int(100.0f * gr) + 1, 12);
    rv(g, kWell, 560, kTop + 232, 100, 12);
    rv(g, lv > 0.9f ? kRed : kAmber, 560, kTop + 232, int(100.0f * lv) + 1, 12);
    if (f.limiter) rv(g, kGreen, 560, kTop + 264, 10, 10);
    g.endBatch();
    for (const auto& cd : kCard) {
        g.color(cd.c);
        g.text(cd.x + 12, kTop + 20, font::kSmall, cd.title, Canvas::kCenterY);
        for (int i = 0; i < cd.n; ++i) {
            const int r = cd.first + i;
            barRow(g, cd.x + 4, kTop + 34 + (cd.first == 12 && false ? 0 : i * 30), 252, rows[r].name, rows[r].val, rows[r].frac,
                   cd.c, r == fxRow_);
        }
    }
    g.color(kDim);
    g.text(560, kTop + 192, font::kTiny, "GAIN REDUCTION", Canvas::kCenterY);
    g.text(560, kTop + 224, font::kTiny, "OUT", Canvas::kCenterY);
    g.text(576, kTop + 269, font::kTiny, f.limiter ? "LIMITER ON" : "limiter off", Canvas::kCenterY);
    g.text(4, 438, font::kTiny, "Up/Down: row   Left/Right: card   Encoder: value (Shift: coarse)", Canvas::kCenterY);
    const char* keys[8] = {"Channels", "FX/Master", revTypeName(f.revType), dlyTypeName(f.dlyType), "Ping-pong", "Limiter", "", ""};
    drawFKeys(g, keys, 1, (f.pingPong ? 16u : 0u) | (f.limiter ? 32u : 0u));
}

// ---------------------------------------------------------------------------
// SONG (arrangement)
// ---------------------------------------------------------------------------
void Konsol::drawSong(Canvas& g) {
    const Project& p = *P_;
    int total = 0;
    for (int i = 0; i < p.songLen; ++i) total += p.song[i].bars;
    const int bx = 8, by = kTop + 30, bw = 784, bh = 110;
    const float barW = total ? float(bw) / float(total > 32 ? total : 32) : 24.0f;
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, bh + 50);
    int x = bx;
    for (int i = 0; i < p.songLen; ++i) {
        const int w = int(float(p.song[i].bars) * barW);
        const Rgb c = trackColor(p.song[i].scene);
        rv(g, i == songIdx_ ? c : scale(c, 0.45f), x + 1, by, w - 2, bh);
        if (i == songCur_) rv(g, kAmber, x + 1, by + bh - 6, w - 2, 6);
        x += w;
    }
    g.color(kLine);
    for (int b = 0; b <= (total > 32 ? total : 32); b += 4) rv2(g, bx + int(float(b) * barW), by - 6, 1, 6);
    if (songMode_ && songIdx_ >= 0 && ctx_->playing()) {
        int bars = 0;
        for (int i = 0; i < songIdx_; ++i) bars += p.song[i].bars;
        const uint32_t pos = posPub_;
        const float inEntry = float(pos >= songStartPos_ ? pos - songStartPos_ : 0) / float(kSubPerBeat * 4);
        rv(g, kWhite, bx + int((float(bars) + inEntry) * barW), by - 4, 2, bh + 8);
    }
    for (int s = 0; s < kScenes; ++s) {
        bool any = false;
        for (int t = 0; t < kTracks; ++t) any = any || p.clip[t][s].exists();
        const int px = 4 + (s % 4) * 72, py = kTop + 170 + (s / 4) * 50;
        rv(g, any ? scale(trackColor(s), 0.55f) : kWell, px, py, 68, 46);
        if (songCur_ < p.songLen && p.song[songCur_].scene == s) rv(g, kAmber, px, py + 42, 68, 4);
    }
    rv(g, kPanel, 296, kTop + 170, 500, 196);
    g.endBatch();
    x = bx;
    for (int i = 0; i < p.songLen; ++i) {
        const int w = int(float(p.song[i].bars) * barW);
        if (w > 26) {
            g.color(i == songIdx_ ? kDesk : kText);
            g.textf(x + 5, by + 16, font::kTiny, Canvas::kCenterY, "S%d", p.song[i].scene + 1);
            g.textf(x + 5, by + 34, font::kTiny, Canvas::kCenterY, "%db", p.song[i].bars);
        }
        x += w;
    }
    g.color(kDim);
    g.textf(12, kTop + 14, font::kTiny, Canvas::kCenterY, "ARRANGEMENT  %d entries  %d bars  %s", p.songLen, total,
            p.songLoop ? "loops" : "stops at the end");
    for (int s = 0; s < kScenes; ++s) {
        g.color(kText);
        g.textf(4 + (s % 4) * 72 + 34, kTop + 170 + (s / 4) * 50 + 22, font::kSmall, Canvas::kCenter, "%d", s + 1);
    }
    g.color(kText);
    if (songCur_ < p.songLen) {
        const SongEntry& e = p.song[songCur_];
        g.textf(312, kTop + 196, font::kLarge, Canvas::kCenterY, "Entry %d", songCur_ + 1);
        g.color(trackColor(e.scene));
        g.textf(312, kTop + 232, font::kBody, Canvas::kCenterY, "Scene %d", e.scene + 1);
        g.color(kText);
        g.textf(312, kTop + 258, font::kBody, Canvas::kCenterY, "%d bars", e.bars);
    } else {
        g.text(312, kTop + 210, font::kBody, "Empty: tap a scene pad (cols 1-4)", Canvas::kCenterY);
    }
    g.color(songMode_ ? kTeal : kDim);
    g.text(312, kTop + 292, font::kBody, songMode_ ? "SONG MODE: the arrangement plays" : "SESSION MODE: clips by hand",
           Canvas::kCenterY);
    g.color(kDim);
    g.text(312, kTop + 322, font::kTiny, "Encoder: scene   Shift+enc / Up-Down: bars   Left/Right: entry", Canvas::kCenterY);
    g.text(312, kTop + 340, font::kTiny, "Insert: hear the entry   Delete: remove it   Shift + scene pad: append", Canvas::kCenterY);
    const char* keys[8] = {"Insert", "Delete", "Song mode", "Append", p.songLoop ? "Loop on" : "Loop off", "< Move", "Move >",
                           "Play song"};
    drawFKeys(g, keys, songMode_ ? 2 : -1, p.songLoop ? 16u : 0u);
}

// ---------------------------------------------------------------------------
// PROJECT
// ---------------------------------------------------------------------------
void Konsol::drawProject(Canvas& g) {
    g.beginRects();
    rv(g, kPanel, 4, kTop + 2, 792, 56);
    rv(g, pageColor(kPgProject), 4, kTop + 2, 5, 56);
    for (int t = 0; t < kTracks; ++t) {
        const int y = kTop + 92 + t * 34;
        rv(g, t == trkRow_ ? kPanel2 : kPanel, 4, y, 792, 31);
        rv(g, trackColor(t), 4, y, 5, 31);
        if (t == trkRow_) rv(g, kAmber, 120 + trkCol_ * 136, y + 27, 126, 3);
    }
    // sample memory gauge
    const float used = pool_.capSamples() ? float(pool_.usedSamples()) / float(pool_.capSamples()) : 0.0f;
    rv(g, kWell, 520, kTop + 40, 266, 8);
    rv(g, used > 0.9f ? kRed : kTeal, 520, kTop + 40, int(266.0f * used) + 1, 8);
    g.endBatch();
    g.color(kAmber);
    g.text(22, kTop + 24, font::kTitle, P_->name, Canvas::kCenterY);
    g.color(dirty_ ? kAmber : kDim);
    g.text(22, kTop + 48, font::kTiny, dirty_ ? "unsaved changes" : "saved", Canvas::kCenterY);
    g.color(kDim);
    g.textf(786, kTop + 14, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%s   Lab/KONSOL/Projects/%s.kon",
            ctx_->fs ? "card ok" : "NO CARD", P_->name);
    g.textf(520, kTop + 30, font::kTiny, Canvas::kCenterY, "sample memory %lu / %lu s", static_cast<unsigned long>(pool_.usedSamples() / 44100),
            static_cast<unsigned long>(pool_.capSamples() / 44100));
    static const char* const kStem[5] = {"Stem 1", "Stem 2", "Stem 3", "Stem 4", "No stem"};
    static const char* const kHead[5] = {"ENGINE", "OCTAVE", "STEM GROUP", "MIDI OUT", "MIDI IN"};
    g.color(kDim);
    for (int c = 0; c < 5; ++c) g.text(124 + c * 136, kTop + 80, font::kTiny, kHead[c], Canvas::kCenterY);
    for (int t = 0; t < kTracks; ++t) {
        const TrackCfg& tc = P_->track[t];
        const int y = kTop + 92 + t * 34 + 15;
        g.color(kText);
        g.textf(20, y, font::kBody, Canvas::kCenterY, "Track %d", t + 1);
        g.color(trackColor(t));
        g.text(124, y, font::kBody, engineName(tc.engine), Canvas::kCenterY);
        g.color(kText);
        g.textf(260, y, font::kBody, Canvas::kCenterY, "%d", tc.octave);
        g.color(stemColor(tc.mix.stem));
        g.text(396, y, font::kBody, kStem[tc.mix.stem < 5 ? tc.mix.stem : 4], Canvas::kCenterY);
        g.color(tc.mix.midiOut ? kGreen : kDim);
        g.textf(532, y, font::kBody, Canvas::kCenterY, tc.mix.midiOut ? "ch %d" : "off", (ctx_->midiChannel() + t) % 16 + 1);
        g.color(tc.midiCh ? kText : kDim);
        if (tc.midiCh) g.textf(668, y, font::kBody, Canvas::kCenterY, "ch %d", tc.midiCh);
        else g.text(668, y, font::kBody, "selected", Canvas::kCenterY);
    }
    g.color(kDim);
    g.text(4, 438, font::kTiny, "Arrows: cell   Encoder: value   MIDI in 'selected' = plays the selected track on any channel",
           Canvas::kCenterY);
    const char* keys[8] = {"Tracks", "Open", "Save", "Save as", confirmKey_ == 4 ? "Sure?" : "New",
                           confirmKey_ == 5 ? "Sure?" : "Demo", "Rename", "Clean up"};
    drawFKeys(g, keys);
}

// ---------------------------------------------------------------------------
// pad LEDs
// ---------------------------------------------------------------------------
void Konsol::leds(Rgb* pads) {
    const float br = setPadBright_ == 0 ? 0.35f : (setPadBright_ == 1 ? 0.65f : 1.0f);
    const uint32_t ms = ctx_->millis();
    const bool blink = (ms / 200) % 2;
    const uint32_t pos = posPub_;
    const float beatPh = float(pos % kSubPerBeat) / float(kSubPerBeat);
    const float pulse = ctx_->playing() ? 0.65f + 0.35f * (1.0f - beatPh) : 0.85f;
    for (int i = 0; i < kPads; ++i) pads[i] = Rgb();
    auto keyboard = [&](int rowsFrom, int rowsTo) {
        const uint16_t mask = scaleMask(selT_);
        const bool drums = drumLike(selT_);
        for (int r = rowsFrom; r <= rowsTo; ++r)
            for (int c = 0; c < 12; ++c) {
                const int pad = padAt(c, r);
                const int p = keyPitch(pad);
                const int pc = ((p - P_->key) % 12 + 12) % 12;
                Rgb col;
                if (drums) col = (c / 4) % 2 ? Rgb(0x14202A) : Rgb(0x0A1A14);
                else col = (mask >> pc) & 1 ? Rgb(0x0A3A36) : Rgb(0x080808);
                if (!drums && pc == 0) col = Rgb(0x5A3A00);
                if ((page_ == kPgSteps && p == stepNote_) || (page_ == kPgRoll && p == rollPitch_)) col = trackColor(selT_);
                if (padPitch_[pad] >= 0) col = Rgb(0xFFFFFF);
                pads[pad] = col;
            }
    };
    auto controls = [&]() {
        for (int r = 0; r < 4; ++r) {
            const int s = sceneTop_ + r;
            bool any = false, all = true;
            for (int t = 0; t < kTracks; ++t)
                if (P_->clip[t][s].exists()) {
                    any = true;
                    if (rt_[t].play != s) all = false;
                }
            pads[padAt(8, r)] = any ? (all && ctx_->playing() ? scale(Rgb(0xFFB000), pulse) : Rgb(0x402800)) : Rgb(0x0A0A0A);
        }
        static const uint8_t kM[4] = {kGmStop, kGmMute, kGmSolo, kGmSelect};
        static const Rgb kMc[4] = {Rgb(0xFF3020), Rgb(0xFF8C00), Rgb(0x2060FF), Rgb(0xE0E0E0)};
        for (int r = 0; r < 4; ++r) pads[padAt(9, r)] = gridMode_ == kM[r] ? kMc[r] : scale(kMc[r], 0.15f);
        pads[padAt(10, 0)] = Rgb(0x601010);
        pads[padAt(10, 1)] = sceneTop_ > 0 ? Rgb(0x106058) : Rgb(0x081010);
        pads[padAt(10, 2)] = sceneTop_ < kScenes - kPadRows ? Rgb(0x106058) : Rgb(0x081010);
        pads[padAt(10, 3)] = recArm_ ? (blink ? Rgb(0xFF0000) : Rgb(0x600000)) : Rgb(0x200000);
        static const Rgb kP[4] = {Rgb(0xFF40C0), Rgb(0x30E0FF), Rgb(0xA060FF), Rgb(0xFFB000)};
        const bool held[4] = {stutHeld_, hpHeld_, dlyThrow_, fillHeld_};
        for (int r = 0; r < 4; ++r) pads[padAt(11, r)] = held[r] ? kP[r] : scale(kP[r], 0.12f);
    };
    if (keysOverlay_ && page_ != kPgSteps) {
        keyboard(0, 3);
    } else {
        switch (page_) {
            case kPgSession:
            case kPgGen:
                for (int t = 0; t < kTracks; ++t) {
                    const Mix& m = P_->track[t].mix;
                    for (int r = 0; r < 4; ++r) {
                        const int s = sceneTop_ + r;
                        const Clip& c = P_->clip[t][s];
                        Rgb col;
                        if (gridMode_ == kGmMute) col = m.mute ? Rgb(0xFF8C00) : scale(trackColor(t), 0.2f);
                        else if (gridMode_ == kGmSolo) col = m.solo ? Rgb(0x2060FF) : scale(trackColor(t), 0.2f);
                        else if (gridMode_ == kGmStop) col = rt_[t].play >= 0 ? Rgb(0xFF2010) : Rgb(0x100404);
                        else if (rt_[t].play == s) col = scale(trackColor(t), pulse * (rt_[t].flash ? 1.0f : 0.85f));
                        else if (rt_[t].queued == s) col = blink ? Rgb(0xFFFFFF) : scale(trackColor(t), 0.4f);
                        else if (c.exists()) col = scale(trackColor(t), 0.16f);
                        if (t == selT_ && s == selS_ && gridMode_ == kGmSelect) col = Rgb(0xFFFFFF);
                        pads[padAt(t, r)] = col;
                    }
                }
                controls();
                break;
            case kPgMixer:
                for (int t = 0; t < kTracks; ++t) {
                    const Mix& m = P_->track[t].mix;
                    pads[padAt(t, 0)] = m.mute ? Rgb(0xFF8C00) : Rgb(0x103010);
                    pads[padAt(t, 1)] = m.solo ? Rgb(0x2060FF) : Rgb(0x080C18);
                    pads[padAt(t, 2)] = t == mixCol_ ? trackColor(t) : scale(trackColor(t), 0.15f);
                    pads[padAt(t, 3)] = scale(stemColor(m.stem), m.stem < 4 ? 0.6f : 0.1f);
                }
                controls();
                break;
            case kPgSteps: {
                const Clip& c = P_->clip[selT_][selS_];
                const int len = c.exists() ? c.lenSteps : 16;
                const NoteBuf& nb = c.notes();
                const int playStep = rt_[selT_].play == selS_ ? rt_[selT_].tick / kTps : -1;
                for (int i = 0; i < 16; ++i) {
                    const int step = stepPage_ * 16 + i;
                    Rgb col = step < len ? Rgb(0x0C0C10) : Rgb();
                    for (int k = 0; k < nb.count; ++k)
                        if (nb.n[k].start / kTps == step) {
                            if (nb.n[k].pitch == stepNote_) {
                                col = scale(trackColor(selT_), 0.3f + 0.7f * nb.n[k].vel / 127.0f);
                                break;
                            }
                            col = Rgb(0x202838);
                        }
                    if (step == heldStep_ || (step == stepCursor_ && heldStep_ < 0 && !ctx_->playing())) col = Rgb(0xFFB000);
                    if (step == playStep) col = Rgb(0xFFFFFF);
                    pads[padAt(i % 8, i / 8)] = col;
                }
                const int pages = (len + 15) / 16;
                for (int p = 0; p < 8; ++p)
                    pads[padAt(8 + p % 4, p / 4)] =
                        p < pages ? (p == stepPage_ ? Rgb(0xFFB000) : (playStep / 16 == p ? Rgb(0x605040) : Rgb(0x201810))) : Rgb();
                keyboard(2, 3);
                break;
            }
            case kPgSong: {
                for (int s = 0; s < kScenes; ++s) {
                    bool any = false;
                    for (int t = 0; t < kTracks; ++t) any = any || P_->clip[t][s].exists();
                    Rgb col = any ? scale(trackColor(s), 0.25f) : Rgb(0x060606);
                    if (songCur_ < P_->songLen && P_->song[songCur_].scene == s) col = trackColor(s);
                    pads[padAt(s % 4, s / 4)] = col;
                }
                for (int i = 0; i < 32; ++i) {
                    Rgb col;
                    if (i < P_->songLen) {
                        col = scale(trackColor(P_->song[i].scene), 0.25f);
                        if (i == songCur_) col = Rgb(0xFFB000);
                        if (i == songIdx_ && ctx_->playing()) col = Rgb(0xFFFFFF);
                    }
                    pads[padAt(4 + i % 8, i / 8)] = col;
                }
                break;
            }
            case kPgSample: {
                const TrackSample& ts = ts_[selT_];
                const int s = smpTab_ == 1 ? ts.src : ts.slot;
                if (pool_.valid(s)) {
                    const SampleSlot& sl = pool_.slot(s);
                    for (int k = 0; k < 36 && k < sl.nOn; ++k)
                        pads[k] = k == sliceSel_ ? Rgb(0xFFFFFF) : scale(classColor(sl.cls[k]), 0.5f);
                }
                keyboard(3, 3);
                break;
            }
            case kPgKit: {
                const KitPad* kp = kitPads(eng_[selT_].e[kEngKit]);
                const int hit = kitLastHit(eng_[selT_].e[kEngKit]);
                for (int i = 0; i < kKitPads; ++i) {
                    const int pad = padAt(i % 4, 3 - i / 4);
                    Rgb col = pool_.valid(kp[i].slot) ? scale(classColor(kp[i].cls), 0.35f) : Rgb(0x080808);
                    if (i == hit && rt_[selT_].flash) col = classColor(kp[i].cls);
                    if (i == kitSel_) col = mix(col, Rgb(0xFFFFFF), 0.5f);
                    if (padPitch_[pad] >= 0) col = Rgb(0xFFFFFF);
                    pads[pad] = col;
                }
                const int tune = int(kp[kitSel_].tune);
                for (int r = 0; r < 4; ++r)
                    for (int c = 4; c < 12; ++c) {
                        const int semi = (3 - r) * 8 + (c - 4) - 12;
                        pads[padAt(c, r)] = semi == tune ? Rgb(0x8BE36B) : (semi == 0 ? Rgb(0x304020) : Rgb(0x0A120A));
                    }
                break;
            }
            default: keyboard(0, 3); break;
        }
    }
    if (br < 0.99f)
        for (int i = 0; i < kPads; ++i) pads[i] = scale(pads[i], br);
}

}  // namespace konsol
}  // namespace lab
