// KONSOL drawing: a mixing-console identity (dark navy desk, amber
// indicators, coloured channel strips), every page, the pad LEDs and the
// boot intro. Rectangles go through Canvas batches (1 word per vertex).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/gen_glue.h"
#include "ws/konsol.h"

namespace lab {
namespace konsol {

namespace {
constexpr uint32_t kSubPerBeat = kPpq * 256;
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
const Rgb kDesk(0x0A0D13), kPanel(0x161C28), kPanel2(0x1E2636), kLine(0x2A3448), kAmber(0xFFB000),
    kTeal(0x2EC4B6), kText(0xECE6D6), kDim(0x66708A), kRed(0xFF4D4D), kGreen(0x7CDB5A), kBlue(0x4D9DE0),
    kWhite(0xFFFFFF);
const char* const kPageNames[kPgCount] = {"SESSION", "STEPS", "PIANO ROLL", "PLAYER", "SOUND",
                                          "MIXER",   "SONG",  "PROJECT",    "SAMPLES"};
Rgb stemColor(int s) {
    static const Rgb k[5] = {Rgb(0xFF7A59), Rgb(0x59C3FF), Rgb(0xB8FF59), Rgb(0xD780FF), Rgb(0x404040)};
    return k[s < 0 ? 4 : (s > 4 ? 4 : s)];
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
const char* clipLabel(const Clip& c) {
    if (c.gen != kGenNotes) return genName(c.gen);
    return "notes";
}
}  // namespace

// ---------------------------------------------------------------------------
// shared chrome
// ---------------------------------------------------------------------------
void Konsol::drawHeaderBar(Canvas& g, const char* page) {
    Shell& sh = *ctx_->shell;
    g.beginRects();
    rv(g, kPanel, 0, 0, 800, 34);
    rv(g, kAmber, 0, 34, 800, 2);
    // the logo plate
    rv(g, kAmber, 8, 6, 104, 22);
    g.endBatch();
    g.color(kDesk);
    g.text(60, 17, font::kBody, "KONSOL", Canvas::kCenter);
    g.color(kText);
    g.text(124, 17, font::kBody, page, Canvas::kCenterY);
    // selected track + scene
    g.color(trackColor(selT_));
    g.textf(262, 17, font::kSmall, Canvas::kCenterY, "T%d %s", selT_ + 1, engineName(P_->track[selT_].engine));
    g.color(kDim);
    g.textf(392, 17, font::kSmall, Canvas::kCenterY, "S%d", selS_ + 1);
    // transport: bar.beat, bpm, play, rec arm, cpu
    const uint32_t pos = posPub_;
    const uint32_t beat = pos / kSubPerBeat;
    g.color(sh.playing() ? kText : kDim);
    g.textf(440, 17, font::kBody, Canvas::kCenterY, "%3lu.%lu", static_cast<unsigned long>(beat / 4 + 1),
            static_cast<unsigned long>(beat % 4 + 1));
    g.color(kAmber);
    g.textf(560, 17, font::kBody, Canvas::kCenterY | Canvas::kRight, "%.1f", double(sh.bpm()));
    g.color(kDim);
    g.text(564, 19, font::kTiny, "BPM", Canvas::kCenterY);
    if (songMode_) {
        g.color(kTeal);
        g.text(598, 17, font::kTiny, "SONG", Canvas::kCenterY);
    }
    g.color(kDim);
    g.textf(692, 17, font::kTiny, Canvas::kCenterY | Canvas::kRight, "CPU %u%%", unsigned(sh.cpuPermille() / 10));
    if (sh.playing()) {
        g.color(kGreen);
        g.beginStrip();
        g.vtx(704, 9);
        g.vtx(720, 17);
        g.vtx(704, 25);
        g.vtx(704, 9);
        g.endBatch();
    } else {
        g.beginRects();
        rv(g, kDim, 705, 10, 13, 13);
        g.endBatch();
    }
    if (recArm_) {
        g.color(kRed);
        g.point(746, 17, 7);
        g.text(758, 17, font::kTiny, "ARM", Canvas::kCenterY);
    } else if (keysOverlay_) {
        g.color(kTeal);
        g.text(740, 17, font::kTiny, "KEYS", Canvas::kCenterY);
    }
}

void Konsol::drawFKeys(Canvas& g, const char* const k[8], int active) {
    g.beginRects();
    rv(g, kPanel, 0, 450, 800, 30);
    if (active >= 0 && active < 8) rv(g, kAmber, active * 100 + 3, 452, 94, 26);
    g.color(kLine);
    for (int i = 1; i < 8; ++i) rv2(g, i * 100, 455, 1, 20);
    g.endBatch();
    for (int i = 0; i < 8; ++i) {
        if (!k[i] || !k[i][0]) continue;
        g.color(i == active ? kDesk : kText);
        g.text(i * 100 + 50, 465, font::kSmall, k[i], Canvas::kCenter);
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
        rv(g, kLine, x + 26, 90, 4, 260);
    }
    g.endBatch();
    // faders: each starts 0.12 s after the previous and settles at a mix level
    static const float kLvl[8] = {0.78f, 0.62f, 0.7f, 0.5f, 0.66f, 0.45f, 0.58f, 0.72f};
    g.beginRects();
    for (int i = 0; i < 8; ++i) {
        const int x = 60 + i * 72;
        float k = clampf((s - 0.15f - 0.11f * float(i)) / 0.7f, 0.0f, 1.0f);
        k = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        const float lv = kLvl[i] * k;
        const int y = 350 - int(lv * 240.0f);
        // meter segments
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
    // the name plate
    const float a = clampf((s - 1.4f) / 0.5f, 0.0f, 1.0f);
    if (a > 0.0f) {
        const int w = int(360.0f * a);
        g.beginRects();
        rv(g, kAmber, 400 - w / 2, 190, w, 70);
        g.endBatch();
        if (a >= 1.0f) {
            g.color(kDesk);
            g.text(400, 225, font::kHuge, "K O N S O L", Canvas::kCenter);
        }
    }
    if (s > 2.0f) {
        g.color(kText);
        g.text(400, 400, font::kSmall, "8 channels  -  16 scenes  -  10 engines  -  10 players", Canvas::kCenter);
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
    g.beginRects();
    rv(g, kDesk, 0, 0, 800, 480);
    g.endBatch();
    drawHeaderBar(g, kPageNames[page_ < kPgCount ? page_ : 0]);
    if (help_) {
        drawHelp(g);
        return;
    }
    switch (page_) {
        case kPgSession: drawSession(g); break;
        case kPgSteps: drawSteps(g); break;
        case kPgRoll: drawRoll(g); break;
        case kPgGen: drawGen(g); break;
        case kPgSound: drawSound(g); break;
        case kPgMixer: drawMixer(g); break;
        case kPgSong: drawSong(g); break;
        case kPgProject: drawProject(g); break;
        default: drawSamples(g); break;
    }
}

// ---------------------------------------------------------------------------
// SESSION
// ---------------------------------------------------------------------------
void Konsol::drawSession(Canvas& g) {
    const int x0 = 6, cw = 81, top = 76, rh = 40, rows = 8;
    const int viewTop = clampi(sceneTop_ - 2, 0, kScenes - rows);
    const uint32_t ms = ctx_->millis();
    const bool blink = (ms / 250) % 2;
    // strip headers + cells (one batch)
    g.beginRects();
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        rv(g, kPanel, x, 40, cw - 3, 32);
        rv(g, trackColor(t), x, 40, cw - 3, 4);
        for (int r = 0; r < rows; ++r) {
            const int s = viewTop + r;
            const Clip& c = P_->clip[t][s];
            const int y = top + r * rh;
            const bool playing = rt_[t].play == s;
            const bool queued = rt_[t].queued == s;
            Rgb col = c.exists() ? scale(trackColor(t), playing ? 0.9f : 0.32f) : Rgb(0x121722);
            if (queued && blink) col = scale(trackColor(t), 0.65f);
            rv(g, col, x, y, cw - 3, rh - 4);
            if (playing && c.exists()) {   // progress along the bottom
                const float prog = float(rt_[t].tick) / float(c.lenSteps * kTps);
                rv(g, kWhite, x, y + rh - 7, int(float(cw - 3) * prog) + 1, 3);
            }
            if (rt_[t].queued == -1 && playing && blink) rv(g, kRed, x + cw - 12, y + 3, 7, 7);
        }
    }
    // scene column
    for (int r = 0; r < rows; ++r) {
        const int s = viewTop + r;
        const int y = top + r * rh;
        bool all = true, any = false;
        for (int t = 0; t < kTracks; ++t) {
            if (P_->clip[t][s].exists()) {
                any = true;
                if (rt_[t].play != s) all = false;
            }
        }
        rv(g, any && all && ctx_->playing() ? scale(kAmber, 0.75f) : kPanel, 660, y, 134, rh - 4);
    }
    // the pad window: a bracket on the left edge
    g.color(kAmber);
    rv2(g, 1, top + (sceneTop_ - viewTop) * rh, 3, kPadRows * rh - 4);
    rv2(g, 656, top + (sceneTop_ - viewTop) * rh, 2, kPadRows * rh - 4);
    g.endBatch();
    // selection frame
    if (selS_ >= viewTop && selS_ < viewTop + rows) {
        g.color(kWhite);
        g.frame(x0 + selT_ * cw - 1, top + (selS_ - viewTop) * rh - 1, cw - 1, rh - 2);
    }
    // texts
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        const Mix& m = P_->track[t].mix;
        g.color(m.mute ? kDim : kText);
        g.textf(x + 5, 58, font::kSmall, Canvas::kCenterY, "%d", t + 1);
        g.text(x + 20, 59, font::kTiny, engineName(P_->track[t].engine), Canvas::kCenterY);
        for (int r = 0; r < rows; ++r) {
            const int s = viewTop + r;
            const Clip& c = P_->clip[t][s];
            if (!c.exists()) continue;
            const int y = top + r * rh;
            g.color(rt_[t].play == s ? kDesk : kText);
            g.text(x + 5, y + 11, font::kTiny, clipLabel(c), Canvas::kCenterY);
            g.color(rt_[t].play == s ? kDesk : kDim);
            g.textf(x + 5, y + 25, font::kTiny, Canvas::kCenterY, "%d%s", c.lenSteps, c.live ? " LIVE" : "");
        }
    }
    for (int r = 0; r < rows; ++r) {
        const int s = viewTop + r;
        const int y = top + r * rh;
        g.color(kText);
        g.textf(672, y + 18, font::kBody, Canvas::kCenterY, "SCENE %d", s + 1);
        g.color(kAmber);
        g.beginStrip();
        g.vtx(770, float(y + 10));
        g.vtx(782, float(y + 18));
        g.vtx(770, float(y + 26));
        g.vtx(770, float(y + 10));
        g.endBatch();
    }
    // meter bridge
    g.beginRects();
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        const Mix& m = P_->track[t].mix;
        rv(g, Rgb(0x10141D), x, 400, cw - 3, 46);
        const float lv = clampf(meter_[t] * 1.1f, 0.0f, 1.0f);
        rv(g, Rgb(0x1A2130), x + 4, 404, cw - 11, 12);
        rv(g, lv > 0.9f ? kRed : trackColor(t), x + 4, 404, int(float(cw - 11) * lv) + 1, 12);
        if (m.mute) rv(g, Rgb(0xFF8C00), x + 4, 418, 22, 22);
        if (m.solo) rv(g, kBlue, x + 30, 418, 22, 22);
        rv(g, stemColor(m.stem), x + cw - 16, 418, 9, 22);
    }
    rv(g, Rgb(0x10141D), 660, 400, 134, 46);
    rv(g, Rgb(0x1A2130), 664, 404, 126, 12);
    rv(g, masterMeter_ > 0.9f ? kRed : kAmber, 664, 404, int(126.0f * clampf(masterMeter_, 0, 1)) + 1, 12);
    g.endBatch();
    for (int t = 0; t < kTracks; ++t) {
        const int x = x0 + t * cw;
        const Mix& m = P_->track[t].mix;
        if (m.mute) {
            g.color(kDesk);
            g.text(x + 15, 429, font::kTiny, "M", Canvas::kCenter);
        }
        if (m.solo) {
            g.color(kDesk);
            g.text(x + 41, 429, font::kTiny, "S", Canvas::kCenter);
        }
    }
    g.color(kDim);
    static const char* const kModes[6] = {"LAUNCH", "STOP", "MUTE", "SOLO", "SELECT", "DUP"};
    g.textf(668, 430, font::kTiny, Canvas::kCenterY, "MASTER  %s", kModes[gridMode_]);
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
    if (stepNote_ < 0) stepNote_ = keyPitch(padAt(0, 3), 4);
    const int playStep = rt_[selT_].play == selS_ ? rt_[selT_].tick / kTps : -1;
    const Rgb tc = trackColor(selT_);
    // overview: every step of the clip, notes as dots by pitch
    g.beginRects();
    rv(g, kPanel, 10, 44, 780, 60);
    const float sw = 772.0f / float(len);
    rv(g, kPanel2, 14 + int(float(stepPage_ * 16) * sw), 46, int(16.0f * sw), 56);
    int lo = 127, hi = 0;
    for (int i = 0; i < nb.count; ++i) {
        lo = nb.n[i].pitch < lo ? nb.n[i].pitch : lo;
        hi = nb.n[i].pitch > hi ? nb.n[i].pitch : hi;
    }
    if (hi < lo) lo = hi = 60;
    const float ph = 48.0f / float(hi - lo + 1 > 12 ? hi - lo + 1 : 12);
    g.color(scale(tc, 0.8f));
    for (int i = 0; i < nb.count && i < 128; ++i) {
        const Note& n = nb.n[i];
        const int x = 14 + int(float(n.start) / float(kTps) * sw);
        const int y = 98 - int(float(n.pitch - lo) * ph) - 3;
        rv2(g, x, y, int(float(n.len) / float(kTps) * sw) + 2, 3);
    }
    if (playStep >= 0) rv(g, kWhite, 14 + int(float(playStep) * sw), 46, 2, 56);
    // 16 step cells, 2 rows of 8 (like the pads)
    for (int i = 0; i < 16; ++i) {
        const int step = stepPage_ * 16 + i;
        const int x = 10 + (i % 8) * 98, y = 116 + (i / 8) * 112;
        const bool in = step < len;
        const int ni = in ? [&] {
            for (int k = 0; k < nb.count; ++k)
                if (nb.n[k].start / kTps == step && nb.n[k].pitch == stepNote_) return k;
            return -1;
        }()
                          : -1;
        bool other = false;
        if (in && ni < 0)
            for (int k = 0; k < nb.count; ++k)
                if (nb.n[k].start / kTps == step) other = true;
        Rgb bg = !in ? Rgb(0x0E121A) : (ni >= 0 ? scale(tc, 0.85f) : (other ? Rgb(0x283245) : kPanel));
        if (step == playStep) bg = mix(bg, kWhite, 0.45f);
        rv(g, bg, x, y, 92, 104);
        if (step == stepCursor_ || step == heldStep_) {
            g.color(kAmber);
            rv2(g, x, y + 100, 92, 4);
        }
        if (ni >= 0) {   // the lock value as a bar
            const Note& n = nb.n[ni];
            float v = 0;
            switch (stepLock_) {
                case 0: v = n.vel / 127.0f; break;
                case 1: v = clampf(float(n.len) / float(kTps * 4), 0, 1); break;
                case 2: v = n.chance / 100.0f; break;
                case 3: v = n.roll / 16.0f; break;
                case 4: v = (n.tone + 64) / 127.0f; break;
                default: v = (n.flags & kFlagSlide) ? 1.0f : 0.0f; break;
            }
            g.color(kDesk);
            rv2(g, x + 6, y + 50, 80, 10);
            g.color(kText);
            rv2(g, x + 6, y + 50, int(80.0f * v) + 1, 10);
        }
    }
    // pages
    const int pages = (len + 15) / 16;
    for (int p = 0; p < 8; ++p) rv(g, p < pages ? (p == stepPage_ ? kAmber : kPanel2) : Rgb(0x0E121A), 10 + p * 40, 344, 34, 14);
    g.endBatch();
    // texts
    static const char* const kLock[6] = {"VELOCITY", "LENGTH", "CHANCE", "RATCHET", "TONE", "SLIDE"};
    for (int i = 0; i < 16; ++i) {
        const int step = stepPage_ * 16 + i;
        if (step >= len) continue;
        const int x = 10 + (i % 8) * 98, y = 116 + (i / 8) * 112;
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
            case 4: std::snprintf(v, sizeof v, "%+d", n.tone); break;
            default: std::snprintf(v, sizeof v, (n.flags & kFlagSlide) ? "SLIDE" : "-"); break;
        }
        g.color(kDesk);
        g.text(x + 46, y + 80, font::kBody, v, Canvas::kCenter);
    }
    char nn[6];
    g.color(kText);
    g.textf(10, 380, font::kBody, Canvas::kCenterY, "Lane %s", noteText(stepNote_, nn));
    g.color(kAmber);
    g.textf(160, 380, font::kBody, Canvas::kCenterY, "Lock: %s", kLock[stepLock_]);
    g.color(kDim);
    g.textf(400, 380, font::kSmall, Canvas::kCenterY, "T%d S%d  %d steps  %d notes  page %d/%d", selT_ + 1, selS_ + 1, len,
            nb.count, stepPage_ + 1, pages);
    g.text(10, 412, font::kTiny,
           "Pads rows 1-2: steps (tap = on/off, hold + encoder = lock, hold + key = pitch).  Rows 3-4: keyboard picks "
           "the lane.",
           0);
    g.text(10, 430, font::kTiny, "Pads 9-12 top rows: step pages.  Encoder: cursor, push = toggle.  Shift + encoder: lock.", 0);
    const char* keys[8] = {"Velocity", "Length", "Chance", "Ratchet", "Tone", "Slide", "Length+", "Clear lane"};
    drawFKeys(g, keys, stepLock_);
}

// ---------------------------------------------------------------------------
// PIANO ROLL
// ---------------------------------------------------------------------------
void Konsol::drawRoll(Canvas& g) {
    const Clip& c = P_->clip[selT_][selS_];
    const int len = (c.exists() ? c.lenSteps : 16) * kTps;
    const NoteBuf& nb = c.notes();
    static const int kGrids[5] = {kTps / 2, kTps, kTps * 2, kTps * 4, kTps * 16};
    static const char* const kGridN[5] = {"1/32", "1/16", "1/8", "1/4", "1 bar"};
    const int g0 = kGrids[rollGrid_];
    // view: 16 pitches (rollTop_ .. rollTop_-15), 64 steps max visible
    const int visTicks = len < 64 * kTps ? len : 64 * kTps;
    if (rollTick_ < rollLeft_) rollLeft_ = (rollTick_ / (16 * kTps)) * 16 * kTps;
    if (rollTick_ >= rollLeft_ + visTicks) rollLeft_ = rollTick_ - visTicks + 16 * kTps;
    rollLeft_ = clampi((rollLeft_ / (16 * kTps)) * 16 * kTps, 0, len - visTicks > 0 ? len - visTicks : 0);
    const int gx = 52, gy = 44, gw = 740, gh = 352;
    const float px = float(gw) / float(visTicks);
    const int rowH = gh / 16;
    const uint16_t mask = scaleMask(selT_);
    const Rgb tc = trackColor(selT_);
    g.beginRects();
    for (int r = 0; r < 16; ++r) {
        const int p = rollTop_ - r;
        const int pc = ((p - P_->key) % 12 + 12) % 12;
        const bool black = (0x54A >> (p % 12)) & 1;
        Rgb rc = (mask >> pc) & 1 ? Rgb(0x1A2232) : Rgb(0x111621);
        if (pc == 0) rc = Rgb(0x2A2414);
        rv(g, rc, gx, gy + r * rowH, gw, rowH - 1);
        rv(g, black ? Rgb(0x222A38) : Rgb(0xC9C2B0), 6, gy + r * rowH, 42, rowH - 1);
        if (p == rollPitch_) rv(g, kAmber, 6, gy + r * rowH, 6, rowH - 1);
    }
    // beat lines
    g.color(kLine);
    for (int t = 0; t <= visTicks; t += kTps * 4) rv2(g, gx + int(float(t) * px), gy, (t % (kTps * 16)) ? 1 : 2, gh);
    // notes
    for (int i = 0; i < nb.count; ++i) {
        const Note& n = nb.n[i];
        const int r = rollTop_ - n.pitch;
        if (r < 0 || r > 15) continue;
        if (n.start + n.len < rollLeft_ || n.start >= rollLeft_ + visTicks) continue;
        const int x = gx + int(float(n.start - rollLeft_) * px);
        int w = int(float(n.len) * px);
        if (w < 3) w = 3;
        if (x + w > gx + gw) w = gx + gw - x;
        rv(g, mix(scale(tc, 0.45f), tc, n.vel / 127.0f), x, gy + r * rowH + 1, w, rowH - 3);
        if (n.flags & kFlagSlide) rv(g, kWhite, x + w - 3, gy + r * rowH + 1, 3, rowH - 3);
    }
    // playhead
    if (rt_[selT_].play == selS_) {
        const int t = rt_[selT_].tick;
        if (t >= rollLeft_ && t < rollLeft_ + visTicks) rv(g, kWhite, gx + int(float(t - rollLeft_) * px), gy, 2, gh);
    }
    g.endBatch();
    // cursor
    {
        const int r = rollTop_ - rollPitch_;
        const int x = gx + int(float(rollTick_ - rollLeft_) * px);
        g.color(kAmber);
        g.frame(x, gy + r * rowH, int(float(g0) * px) + 1, rowH);
    }
    // labels: Cs on the keys; drum tracks name their pads
    const int eng = P_->track[selT_].engine;
    static const char* const kDr[8] = {"KICK", "SNARE", "CLAP", "CHAT", "OHAT", "TOM", "RIM", "COW"};
    for (int r = 0; r < 16; ++r) {
        const int p = rollTop_ - r;
        if (eng == kEngDrums && p >= 36) {
            g.color(kDim);
            g.text(gx + 4, gy + r * rowH + rowH / 2, font::kTiny, kDr[(p - 36) % 8], Canvas::kCenterY);
        }
        if (p % 12) continue;
        char nn[6];
        g.color(kDesk);
        g.text(10, gy + r * rowH + rowH / 2, font::kTiny, noteText(p, nn), Canvas::kCenterY);
    }
    // info line
    const int i = nb.at(rollTick_, rollPitch_);
    char nn[6];
    g.color(kText);
    g.textf(10, 414, font::kSmall, Canvas::kCenterY, "%s  step %d.%d", noteText(rollPitch_, nn), rollTick_ / kTps + 1,
            (rollTick_ % kTps) * 4 / kTps + 1);
    if (i >= 0) {
        const Note& n = nb.n[i];
        g.color(tc);
        g.textf(200, 414, font::kSmall, Canvas::kCenterY, "len %d.%02d  vel %d%s", n.len / kTps, (n.len % kTps) * 100 / kTps,
                n.vel, (n.flags & kFlagSlide) ? "  slide" : "");
    }
    g.color(kDim);
    g.textf(470, 414, font::kSmall, Canvas::kCenterY, "grid %s  %d notes  T%d S%d", kGridN[rollGrid_], nb.count, selT_ + 1,
            selS_ + 1);
    g.text(10, 434, font::kTiny,
           "Arrows: cursor (Shift: octave)  Push / Insert: add or remove  Encoder: the F1-F5 function  Pads: pitch", 0);
    const char* keys[8] = {"Cursor", "Length", "Velocity", "Move", "Pitch", kGridN[rollGrid_], "Quantise", "Undo"};
    drawFKeys(g, keys, rollFn_);
}

// ---------------------------------------------------------------------------
// PLAYER / GENERATOR
// ---------------------------------------------------------------------------
void Konsol::drawGen(Canvas& g) {
    const Clip& c = P_->clip[selT_][selS_];
    const Rgb tc = trackColor(selT_);
    g.beginRects();
    rv(g, kPanel, 10, 44, 380, 400);
    rv(g, kPanel, 400, 44, 390, 400);
    rv(g, kPanel2, 14, 52 + genRow_ * 36, 372, 32);
    rv(g, kAmber, 14, 52 + genRow_ * 36, 4, 32);
    for (int i = 0; i < 6; ++i) {
        const ot::gen::ParamDesc& pd = genParam(c.gen, i, c.gp);
        if (!pd.name || pd.max <= pd.min) continue;
        const float f = float(c.gp[i] - pd.min) / float(pd.max - pd.min);
        rv(g, kLine, 190, 52 + (i + 1) * 36 + 26, 190, 3);
        rv(g, tc, 190, 52 + (i + 1) * 36 + 26, int(190.0f * f) + 1, 3);
    }
    g.endBatch();
    // rows
    g.color(kText);
    g.text(24, 68, font::kBody, "Player", Canvas::kCenterY);
    g.color(kAmber);
    g.text(380, 68, font::kLarge, genName(c.gen), Canvas::kCenterY | Canvas::kRight);
    for (int i = 0; i < 6; ++i) {
        const ot::gen::ParamDesc& pd = genParam(c.gen, i, c.gp);
        const int y = 52 + (i + 1) * 36 + 14;
        if (!pd.name) continue;
        g.color(kText);
        g.text(24, y, font::kBody, pd.name, Canvas::kCenterY);
        char v[24];
        const int x = c.gp[i];
        if (pd.labels && x >= pd.min && x <= pd.max) copyStr(v, sizeof v, pd.labels[x - pd.min]);
        else if (i == 5 && x == 0) copyStr(v, sizeof v, "random");
        else std::snprintf(v, sizeof v, "%d", x);
        g.color(tc);
        g.text(380, y, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
    g.color(kText);
    g.text(24, 52 + 7 * 36 + 14, font::kBody, "Length", Canvas::kCenterY);
    g.text(24, 52 + 8 * 36 + 14, font::kBody, "Live", Canvas::kCenterY);
    g.text(24, 52 + 9 * 36 + 14, font::kBody, "Every", Canvas::kCenterY);
    g.color(tc);
    g.textf(380, 52 + 7 * 36 + 14, font::kBody, Canvas::kCenterY | Canvas::kRight, "%d steps", c.exists() ? c.lenSteps : 0);
    g.color(c.live ? kGreen : kDim);
    g.text(380, 52 + 8 * 36 + 14, font::kBody, c.live ? "ON - varies while playing" : "off - write once",
           Canvas::kCenterY | Canvas::kRight);
    g.color(tc);
    g.textf(380, 52 + 9 * 36 + 14, font::kBody, Canvas::kCenterY | Canvas::kRight, "%d loop%s", c.every,
            c.every > 1 ? "s" : "");
    // right: what it does + the clip's notes
    g.color(kText);
    g.textf(410, 64, font::kSmall, Canvas::kCenterY, "T%d %s  S%d", selT_ + 1, engineName(P_->track[selT_].engine), selS_ + 1);
    g.color(kDim);
    g.text(410, 86, font::kTiny, genInfo(c.gen), Canvas::kCenterY);
    char nn[6];
    g.textf(410, 104, font::kTiny, Canvas::kCenterY, "Key %s  %s  root %s", noteText(P_->key + 60, nn) , scaleName(P_->scale),
            noteText(rootNote(selT_), nn + 0));
    // mini roll of the clip
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
    const int bx = 410, by = 122, bw = 370, bh = 300;
    const float px = float(bw) / float(len), py = float(bh) / float(hi - lo + 1);
    g.beginRects();
    rv(g, Rgb(0x0E121A), bx, by, bw, bh);
    g.color(kLine);
    for (int t = 0; t <= len; t += kTps * 4) rv2(g, bx + int(float(t) * px), by, 1, bh);
    for (int i = 0; i < nb.count; ++i) {
        const Note& n = nb.n[i];
        g.color(mix(scale(tc, 0.4f), tc, n.vel / 127.0f));
        const int w = int(float(n.len) * px) > 2 ? int(float(n.len) * px) : 2;
        rv2(g, bx + int(float(n.start) * px), by + bh - int(float(n.pitch - lo + 1) * py), w, int(py) > 2 ? int(py) - 1 : 2);
    }
    if (rt_[selT_].play == selS_) rv(g, kWhite, bx + int(float(rt_[selT_].tick) * px), by, 2, bh);
    if (c.pending) rv(g, kGreen, bx + bw - 10, by + 4, 6, 6);
    g.endBatch();
    g.color(kDim);
    g.textf(bx + 4, by + bh + 12, font::kTiny, Canvas::kCenterY, "%d notes %s", nb.count,
            c.live && rt_[selT_].play == selS_ ? "- live: next variation at the loop" : "");
    const char* keys[8] = {"Write", "New seed", c.live ? "Live ON" : "Live off", "Every", "To roll", "Undo", "Clear", "Steps"};
    drawFKeys(g, keys, c.live ? 2 : -1);
}

// ---------------------------------------------------------------------------
// SOUND (engine page)
// ---------------------------------------------------------------------------
void Konsol::drawSound(Canvas& g) {
    Engine* en = engine(selT_);
    const Rgb tc = trackColor(selT_);
    const int ns = en->sections();
    if (sndSec_ >= ns) sndSec_ = 0;
    uint8_t ids[96];
    const int n = en->sectionParams(sndSec_, ids, 96);
    if (sndRow_ >= n) sndRow_ = n - 1;
    // engine plate
    g.beginRects();
    rv(g, sndRow_ < 0 ? kPanel2 : kPanel, 10, 44, 780, 44);
    rv(g, tc, 10, 44, 6, 44);
    // section tabs
    const int tw = ns > 0 ? (780 / (ns < 7 ? 7 : ns)) : 100;
    for (int s = 0; s < ns; ++s) rv(g, s == sndSec_ ? kAmber : kPanel, 10 + s * tw, 94, tw - 4, 26);
    // rows: up to 9 visible
    const int rows = 9;
    int top = sndRow_ - rows + 1;
    if (top < 0) top = 0;
    for (int r = 0; r < rows && top + r < n; ++r) {
        const int y = 128 + r * 34;
        const int i = ids[top + r];
        rv(g, top + r == sndRow_ ? kPanel2 : kPanel, 10, y, 780, 30);
        if (top + r == sndRow_) rv(g, kAmber, 10, y, 4, 30);
        rv(g, kLine, 330, y + 12, 300, 6);
        rv(g, tc, 330, y + 12, int(300.0f * clampf(en->frac(i), 0, 1)) + 1, 6);
    }
    g.endBatch();
    g.color(sndRow_ < 0 ? kAmber : kText);
    g.text(26, 66, font::kLarge, engineName(P_->track[selT_].engine), Canvas::kCenterY);
    g.color(kDim);
    g.text(200, 66, font::kSmall, engineInfo(P_->track[selT_].engine), Canvas::kCenterY);
    g.color(kDim);
    g.textf(780, 66, font::kTiny, Canvas::kCenterY | Canvas::kRight, "T%d", selT_ + 1);
    for (int s = 0; s < ns; ++s) {
        g.color(s == sndSec_ ? kDesk : kText);
        g.text(10 + s * tw + (tw - 4) / 2, 107, font::kTiny, en->sectionName(s), Canvas::kCenter);
    }
    for (int r = 0; r < rows && top + r < n; ++r) {
        const int y = 128 + r * 34;
        const int i = ids[top + r];
        char nm[32], v[24];
        en->paramName(i, nm, sizeof nm);
        en->format(i, v, sizeof v);
        g.color(kText);
        g.text(24, y + 15, font::kBody, nm, Canvas::kCenterY);
        g.color(top + r == sndRow_ ? kAmber : kText);
        g.text(780, y + 15, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
    if (P_->track[selT_].engine <= kEngGrain) {
        g.color(kDim);
        const char* p = smp_[selT_].path[0] ? smp_[selT_].path : "built-in break";
        g.textf(10, 438, font::kTiny, Canvas::kCenterY, "Sample: %.60s  (%lu ms)", p,
                static_cast<unsigned long>(smp_[selT_].len / 44));
    } else {
        g.color(kDim);
        g.text(10, 438, font::kTiny, "Up/Down: row (top row = engine)  Left/Right: section  Encoder: value  Shift: x10", Canvas::kCenterY);
    }
    char oct[12];
    std::snprintf(oct, sizeof oct, "Oct %+d", keyOct_);
    const char* keys[8] = {"< Section", "Section >", "Engine", "Init", "Preset", "Samples", oct, keysOverlay_ ? "Keys ON" : "Keys"};
    drawFKeys(g, keys);
}

// ---------------------------------------------------------------------------
// MIXER
// ---------------------------------------------------------------------------
void Konsol::drawMixer(Canvas& g) {
    if (mixFx_) {
        const FxCfg& f = P_->fx;
        static const char* const kRow[13] = {"Delay time", "Delay feedback", "Delay tone", "Ping-pong", "Delay return",
                                             "Reverb size", "Reverb damping", "Reverb return", "Comp threshold",
                                             "Comp ratio", "Comp make-up", "Limiter", "Master level"};
        static const char* const kSync[6] = {"1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2"};
        float vals[13] = {f.delaySync / 5.0f, f.delayFb / 0.95f, f.delayTone, float(f.pingPong), f.retA / 1.5f, f.revSize,
                          f.revDamp, f.retB / 1.5f, f.compThr, f.compRatio, f.compMakeup, float(f.limiter), f.master / 1.25f};
        g.beginRects();
        for (int r = 0; r < 13; ++r) {
            const int y = 44 + r * 30;
            rv(g, r == fxRow_ ? kPanel2 : kPanel, 10, y, 560, 27);
            if (r == fxRow_) rv(g, kAmber, 10, y, 4, 27);
            rv(g, kLine, 300, y + 11, 160, 5);
            rv(g, r < 4 ? kTeal : (r < 8 ? Rgb(0xA77BFF) : kAmber), 300, y + 11, int(160.0f * clampf(vals[r], 0, 1)) + 1, 5);
        }
        // GR meter + master meter
        rv(g, kPanel, 590, 44, 200, 386);
        const float gr = clampf(compGr_ / 20.0f, 0, 1);
        rv(g, Rgb(0x10141D), 620, 90, 40, 300);
        rv(g, kRed, 620, 90, 40, int(300.0f * gr) + 1);
        rv(g, Rgb(0x10141D), 700, 90, 40, 300);
        const float lv = clampf(masterMeter_, 0, 1);
        rv(g, lv > 0.9f ? kRed : kAmber, 700, 390 - int(300.0f * lv), 40, int(300.0f * lv) + 1);
        g.endBatch();
        for (int r = 0; r < 13; ++r) {
            const int y = 44 + r * 30 + 13;
            g.color(kText);
            g.text(24, y, font::kSmall, kRow[r], Canvas::kCenterY);
            char v[16];
            switch (r) {
                case 0: copyStr(v, sizeof v, kSync[f.delaySync]); break;
                case 3: copyStr(v, sizeof v, f.pingPong ? "On" : "Off"); break;
                case 8: std::snprintf(v, sizeof v, "%d dB", int(-30.0f + 30.0f * f.compThr)); break;
                case 9: std::snprintf(v, sizeof v, "%.1f:1", double(1.0f + 9.0f * f.compRatio)); break;
                case 10: std::snprintf(v, sizeof v, "+%.1f dB", double(12.0f * f.compMakeup)); break;
                case 11: copyStr(v, sizeof v, f.limiter ? "On" : "Off"); break;
                default: std::snprintf(v, sizeof v, "%d%%", int(vals[r] * (r == 4 || r == 7 ? 150.0f : (r == 12 ? 125.0f : 100.0f)) + 0.5f));
            }
            g.color(r == fxRow_ ? kAmber : kText);
            g.text(560, y, font::kSmall, v, Canvas::kCenterY | Canvas::kRight);
        }
        g.color(kText);
        g.text(640, 70, font::kTiny, "GR", Canvas::kCenter);
        g.text(720, 70, font::kTiny, "OUT", Canvas::kCenter);
        g.textf(640, 410, font::kTiny, Canvas::kCenter, "-%.1f", double(compGr_));
        const char* keys[8] = {"Channels", "FX/Master", "", "", "", "", "", ""};
        drawFKeys(g, keys, 1);
        return;
    }
    // 8 strips + master
    const int sw = 86;
    g.beginRects();
    for (int t = 0; t < kTracks; ++t) {
        const int x = 8 + t * sw;
        const Mix& m = P_->track[t].mix;
        const bool sel = t == mixCol_;
        rv(g, sel ? kPanel2 : kPanel, x, 44, sw - 4, 400);
        rv(g, trackColor(t), x, 44, sw - 4, 5);
        // pan / sends as small bars (rows 1..3)
        for (int k = 0; k < 3; ++k) {
            const int y = 78 + k * 30;
            rv(g, (sel && mixRow_ == k + 1) ? kAmber : kLine, x + 6, y + 14, sw - 16, 4);
            if (k == 0) {
                const int cx = x + 6 + int(float(sw - 16) * (m.pan * 0.5f + 0.5f));
                rv(g, kText, cx - 2, y + 9, 4, 14);
            } else {
                const float v = k == 1 ? m.sendA : m.sendB;
                rv(g, k == 1 ? kTeal : Rgb(0xA77BFF), x + 6, y + 14, int(float(sw - 16) * v) + 1, 4);
            }
        }
        // fader slot, meter, cap
        rv(g, Rgb(0x0B0E15), x + 18, 182, 6, 200);
        rv(g, Rgb(0x10141D), x + 52, 182, 10, 200);
        const float lv = clampf(meter_[t], 0, 1);
        rv(g, lv > 0.9f ? kRed : trackColor(t), x + 52, 382 - int(200.0f * lv), 10, int(200.0f * lv) + 1);
        const int fy = 382 - int(200.0f * m.level / 1.25f);
        rv(g, (sel && mixRow_ == 0) ? kAmber : kText, x + 6, fy - 7, 30, 14);
        rv(g, kDesk, x + 6, fy - 1, 30, 2);
        // unity mark
        rv(g, kDim, x + 28, 382 - int(200.0f * 0.8f / 1.25f), 8, 1);
        // mute / solo / stem / midi
        rv(g, m.mute ? Rgb(0xFF8C00) : Rgb(0x2A2418), x + 4, 392, 24, 20);
        rv(g, m.solo ? kBlue : Rgb(0x18202E), x + 30, 392, 24, 20);
        rv(g, stemColor(m.stem), x + 56, 392, 22, 20);
        if (m.midiOut) rv(g, kGreen, x + 4, 418, 74, 3);
        if (sel && mixRow_ >= 4) rv(g, kAmber, x + 4, mixRow_ == 4 ? 414 : 424, 74, 2);
    }
    // master strip
    rv(g, kPanel, 698, 44, 94, 400);
    rv(g, kAmber, 698, 44, 94, 5);
    rv(g, Rgb(0x10141D), 712, 182, 12, 200);
    rv(g, Rgb(0x10141D), 730, 182, 12, 200);
    {
        const float lv = clampf(masterMeter_, 0, 1);
        rv(g, lv > 0.9f ? kRed : kAmber, 712, 382 - int(200.0f * lv), 12, int(200.0f * lv) + 1);
        rv(g, lv > 0.9f ? kRed : kAmber, 730, 382 - int(200.0f * lv), 12, int(200.0f * lv) + 1);
        const float gr = clampf(compGr_ / 20.0f, 0, 1);
        rv(g, kRed, 756, 182, 12, int(200.0f * gr) + 1);
    }
    g.endBatch();
    for (int t = 0; t < kTracks; ++t) {
        const int x = 8 + t * sw;
        const Mix& m = P_->track[t].mix;
        g.color(kText);
        g.textf(x + 6, 62, font::kSmall, Canvas::kCenterY, "%d %.5s", t + 1, engineName(P_->track[t].engine));
        g.color(kDim);
        g.text(x + 6, 84, font::kTiny, "PAN", Canvas::kCenterY);
        g.text(x + 6, 114, font::kTiny, "DLY", Canvas::kCenterY);
        g.text(x + 6, 144, font::kTiny, "REV", Canvas::kCenterY);
        g.color(kDesk);
        g.text(x + 16, 402, font::kTiny, "M", Canvas::kCenter);
        g.text(x + 42, 402, font::kTiny, "S", Canvas::kCenter);
        static const char* const kSt[5] = {"1", "2", "3", "4", "-"};
        g.text(x + 67, 402, font::kTiny, kSt[m.stem < 4 ? m.stem : 4], Canvas::kCenter);
        g.color(kText);
        g.textf(x + 6, 434, font::kTiny, Canvas::kCenterY, "%d", int(m.level * 100.0f + 0.5f));
    }
    g.color(kText);
    g.text(745, 62, font::kSmall, "MASTER", Canvas::kCenter);
    g.color(kDim);
    g.textf(745, 400, font::kTiny, Canvas::kCenter, "GR %.1f", double(compGr_));
    g.textf(745, 420, font::kTiny, Canvas::kCenter, "%d%%", int(P_->fx.master * 100.0f + 0.5f));
    static const char* const kRowN[6] = {"Level", "Pan", "Delay send", "Reverb send", "Stem group", "MIDI out"};
    const char* keys[8] = {"Channels", "FX/Master", "Mute", "Solo", "MIDI out", "Reset ch", "Unsolo all", "Unmute all"};
    drawFKeys(g, keys, 0);
    // row name of the cursor
    g.color(kAmber);
    g.textf(745, 440, font::kTiny, Canvas::kCenter, "edit: %s", kRowN[mixRow_]);
}

// ---------------------------------------------------------------------------
// SONG (arrangement)
// ---------------------------------------------------------------------------
void Konsol::drawSong(Canvas& g) {
    const Project& p = *P_;
    int total = 0;
    for (int i = 0; i < p.songLen; ++i) total += p.song[i].bars;
    const int bx = 10, by = 70, bw = 780, bh = 120;
    const float barW = total ? float(bw) / float(total > 32 ? total : 32) : 24.0f;
    g.beginRects();
    rv(g, kPanel, bx, by - 26, bw, bh + 30);
    int x = bx;
    for (int i = 0; i < p.songLen; ++i) {
        const int w = int(float(p.song[i].bars) * barW);
        const Rgb c = trackColor(p.song[i].scene);
        rv(g, i == songIdx_ ? c : scale(c, 0.55f), x + 1, by, w - 2, bh);
        if (i == songCur_) rv(g, kAmber, x + 1, by + bh - 6, w - 2, 6);
        x += w;
    }
    // bar ticks
    g.color(kLine);
    for (int b = 0; b <= (total > 32 ? total : 32); b += 4) rv2(g, bx + int(float(b) * barW), by - 6, 1, 6);
    // the playhead (song mode)
    if (songMode_ && songIdx_ >= 0 && ctx_->playing()) {
        int bars = 0;
        for (int i = 0; i < songIdx_; ++i) bars += p.song[i].bars;
        const uint32_t pos = posPub_;
        const float inEntry = float(pos >= songStartPos_ ? pos - songStartPos_ : 0) / float(kSubPerBeat * 4);
        rv(g, kWhite, bx + int((float(bars) + inEntry) * barW), by - 4, 2, bh + 8);
    }
    // the scene palette (like the pads)
    for (int s = 0; s < kScenes; ++s) {
        bool any = false;
        for (int t = 0; t < kTracks; ++t) any = any || p.clip[t][s].exists();
        const int px = 10 + (s % 4) * 70, py = 224 + (s / 4) * 52;
        rv(g, any ? scale(trackColor(s), 0.6f) : Rgb(0x121722), px, py, 64, 46);
        if (songCur_ < p.songLen && p.song[songCur_].scene == s) rv(g, kAmber, px, py + 42, 64, 4);
    }
    rv(g, kPanel, 300, 224, 490, 202);
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
    g.textf(bx + 6, by - 14, font::kTiny, Canvas::kCenterY, "ARRANGEMENT  %d entries  %d bars  %s", p.songLen, total,
            p.songLoop ? "loop" : "stop at end");
    for (int s = 0; s < kScenes; ++s) {
        g.color(kText);
        g.textf(10 + (s % 4) * 70 + 32, 224 + (s / 4) * 52 + 22, font::kSmall, Canvas::kCenter, "%d", s + 1);
    }
    // the cursor entry
    g.color(kText);
    if (songCur_ < p.songLen) {
        const SongEntry& e = p.song[songCur_];
        g.textf(316, 248, font::kLarge, Canvas::kCenterY, "Entry %d", songCur_ + 1);
        g.color(trackColor(e.scene));
        g.textf(316, 284, font::kBody, Canvas::kCenterY, "Scene %d", e.scene + 1);
        g.color(kText);
        g.textf(316, 312, font::kBody, Canvas::kCenterY, "%d bars", e.bars);
    } else {
        g.text(316, 260, font::kBody, "Empty arrangement: tap a scene pad", Canvas::kCenterY);
    }
    g.color(songMode_ ? kTeal : kDim);
    g.text(316, 346, font::kBody, songMode_ ? "SONG MODE: the arrangement plays" : "SESSION MODE: clips launch by hand",
           Canvas::kCenterY);
    g.color(kDim);
    g.text(316, 376, font::kTiny, "Encoder: scene  Shift+enc / Up-Down: bars  Left/Right: entry", Canvas::kCenterY);
    g.text(316, 394, font::kTiny, "Pads 1-4: scenes (tap = set, Shift = append)  Pads 5-12: entries", Canvas::kCenterY);
    g.text(316, 412, font::kTiny, "Push: hear the entry's scene", Canvas::kCenterY);
    const char* keys[8] = {"Insert", "Delete", songMode_ ? "Song ON" : "Song mode", "Append", p.songLoop ? "Loop on" : "Loop off",
                           "< Move", "Move >", "Play song"};
    drawFKeys(g, keys, songMode_ ? 2 : -1);
}

// ---------------------------------------------------------------------------
// PROJECT
// ---------------------------------------------------------------------------
void Konsol::drawProject(Canvas& g) {
    g.beginRects();
    rv(g, kPanel, 10, 44, 780, 50);
    if (projTab_ == 0) {
        for (int t = 0; t < kTracks; ++t) {
            const int y = 104 + t * 38;
            rv(g, t == trkRow_ ? kPanel2 : kPanel, 10, y, 780, 34);
            rv(g, trackColor(t), 10, y, 6, 34);
            if (t == trkRow_) rv(g, kAmber, 150 + trkCol_ * 160, y + 30, 150, 3);
        }
    } else {
        const int rows = 9;
        if (listSel_ < listTop_) listTop_ = listSel_;
        if (listSel_ >= listTop_ + rows) listTop_ = listSel_ - rows + 1;
        for (int i = 0; i < rows && listTop_ + i < listN_; ++i)
            rv(g, listTop_ + i == listSel_ ? kPanel2 : kPanel, 10, 104 + i * 36, 780, 32);
    }
    g.endBatch();
    g.color(kAmber);
    g.text(24, 69, font::kLarge, P_->name, Canvas::kCenterY);
    if (confirmKey_ == 99) {
        g.color(kWhite);
        g.beginRects();
        rv(g, kAmber, 24 + nameCur_ * 16, 84, 14, 3);
        g.endBatch();
    }
    g.color(kDim);
    g.textf(780, 60, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%s", ctx_->fs ? "card ok" : "NO CARD");
    g.textf(780, 78, font::kTiny, Canvas::kCenterY | Canvas::kRight, "Lab/KONSOL/Projects/%s.kon", P_->name);
    if (projTab_ == 0) {
        static const char* const kStem[5] = {"Stem 1", "Stem 2", "Stem 3", "Stem 4", "No stem"};
        g.color(kDim);
        g.text(150, 99, font::kTiny, "ENGINE", Canvas::kCenterY);
        g.text(310, 99, font::kTiny, "OCTAVE", Canvas::kCenterY);
        g.text(470, 99, font::kTiny, "STEM GROUP", Canvas::kCenterY);
        g.text(630, 99, font::kTiny, "MIDI OUT", Canvas::kCenterY);
        for (int t = 0; t < kTracks; ++t) {
            const TrackCfg& tc = P_->track[t];
            const int y = 104 + t * 38 + 17;
            g.color(kText);
            g.textf(26, y, font::kBody, Canvas::kCenterY, "Track %d", t + 1);
            g.color(trackColor(t));
            g.text(150, y, font::kBody, engineName(tc.engine), Canvas::kCenterY);
            g.color(kText);
            g.textf(310, y, font::kBody, Canvas::kCenterY, "%d", tc.octave);
            g.color(stemColor(tc.mix.stem));
            g.text(470, y, font::kBody, kStem[tc.mix.stem < 5 ? tc.mix.stem : 4], Canvas::kCenterY);
            g.color(tc.mix.midiOut ? kGreen : kDim);
            g.textf(630, y, font::kBody, Canvas::kCenterY, tc.mix.midiOut ? "ch %d" : "off",
                    (ctx_->midiChannel() + t) % 16 + 1);
        }
        g.color(kDim);
        g.text(10, 418, font::kTiny,
               "Stems: Settings > Record stems On, then Shift + Rec records the master + 4 stem groups.", 0);
    } else {
        if (!listN_) {
            g.color(kDim);
            g.text(400, 200, font::kBody, ctx_->fs ? "No saved projects yet (F3 saves)" : "No SD card", Canvas::kCenter);
        }
        for (int i = 0; i < 9 && listTop_ + i < listN_; ++i) {
            g.color(listTop_ + i == listSel_ ? kAmber : kText);
            g.text(26, 104 + i * 36 + 16, font::kBody, list_[listTop_ + i], Canvas::kCenterY);
        }
        g.color(kDim);
        g.text(10, 436, font::kTiny, "Encoder / Up-Down: choose   Push: load (stops playback)", 0);
    }
    const char* keys[8] = {"Tracks", "Open", "Save", "Save new", confirmKey_ == 4 ? "Sure?" : "New",
                           confirmKey_ == 5 ? "Sure?" : "Demo", "Rename", "Samples"};
    drawFKeys(g, keys, projTab_);
}

// ---------------------------------------------------------------------------
// SAMPLES (browser)
// ---------------------------------------------------------------------------
void Konsol::drawSamples(Canvas& g) {
    const int rows = 10;
    if (listSel_ < listTop_) listTop_ = listSel_;
    if (listSel_ >= listTop_ + rows) listTop_ = listSel_ - rows + 1;
    g.beginRects();
    rv(g, kPanel, 10, 44, 780, 34);
    for (int i = 0; i < rows && listTop_ + i < listN_; ++i)
        rv(g, listTop_ + i == listSel_ ? kPanel2 : kPanel, 10, 84 + i * 34, 560, 30);
    rv(g, kPanel, 580, 84, 210, 336);
    // waveform of the track's sample (64 columns)
    const SampleBuf& b = smp_[selT_];
    const uint32_t len = b.len;
    g.color(trackColor(selT_));
    if (len)
        for (int x = 0; x < 64; ++x) {
            const uint32_t a = uint32_t(uint64_t(len) * uint32_t(x) / 64u), e = uint32_t(uint64_t(len) * uint32_t(x + 1) / 64u);
            int pk = 0;
            for (uint32_t i = a; i < e; i += 37) {
                const int v = b.data[i] < 0 ? -b.data[i] : b.data[i];
                pk = v > pk ? v : pk;
            }
            const int h = pk * 60 / 32768 + 1;
            rv2(g, 590 + x * 3, 330 - h, 2, 2 * h);
        }
    g.endBatch();
    g.color(kText);
    g.textf(20, 61, font::kBody, Canvas::kCenterY, "Load into track %d  -  /%s", selT_ + 1, listDir_[0] ? listDir_ + 1 : "");
    if (!listN_) {
        g.color(kDim);
        g.text(290, 200, font::kBody, ctx_->fs ? "No .wav files here" : "No SD card", Canvas::kCenter);
    }
    for (int i = 0; i < rows && listTop_ + i < listN_; ++i) {
        const char* nm = list_[listTop_ + i];
        g.color(nm[0] == '/' ? kTeal : (listTop_ + i == listSel_ ? kAmber : kText));
        g.text(24, 84 + i * 34 + 15, font::kSmall, nm, Canvas::kCenterY);
    }
    g.color(kText);
    g.text(590, 104, font::kSmall, "Track sample", Canvas::kCenterY);
    g.color(kDim);
    g.textf(590, 124, font::kTiny, Canvas::kCenterY, "%.28s", b.path[0] ? b.path : "built-in break");
    g.textf(590, 140, font::kTiny, Canvas::kCenterY, "%lu.%02lu s", static_cast<unsigned long>(len / 44100),
            static_cast<unsigned long>((len % 44100) / 441));
    g.text(590, 404, font::kTiny, "Mono, 44.1 kHz, max 13.9 s", Canvas::kCenterY);
    const char* keys[8] = {"Load/Open", "Up folder", "Built-in", "Next track", "Sound", "", "", ""};
    drawFKeys(g, keys);
}

void Konsol::drawHelp(Canvas& g) {
    static const char* const kL[] = {
        "KONSOL - quick map (Help again closes)",
        "Pages: Pattern=Session  Note=Steps  FX1=Piano roll  FX2=Player  Instr=Sound",
        "       Master=Mixer  Song=Arrangement  File=Project  Sample keys=Samples",
        "       Shift + F1..F8 jumps to Session/Steps/Roll/Player/Sound/Mixer/Song/Project",
        "Session pads: cols 1-8 = clips of 8 tracks x 4 scenes (tap = launch, empty = stop)",
        "  col 9 = scene launch  col 10 = Stop / Mute / Solo / Select modes (tap latches)",
        "  col 11 = stop all, scenes up, scenes down, record arm",
        "  col 12 = hold: stutter, HP sweep, delay throw, reverb throw",
        "Encoder: scroll scenes (Shift: tempo), push = launch the selected clip",
        "Aux / Performance: pads become a keyboard for the selected track",
        "Rec: arm recording into the playing clip   Shift+Rec: Shell recorder (stems)",
        "Copy: duplicate the clip   Delete: delete the clip   Play: start / stop",
    };
    g.beginRects();
    rv(g, kPanel, 20, 50, 760, 380);
    g.endBatch();
    for (int i = 0; i < int(sizeof kL / sizeof kL[0]); ++i) {
        g.color(i == 0 ? kAmber : kText);
        g.text(36, 72 + i * 29, i == 0 ? font::kBody : font::kSmall, kL[i], Canvas::kCenterY);
    }
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
        for (int r = rowsFrom; r <= rowsTo; ++r)
            for (int c = 0; c < 12; ++c) {
                const int pad = padAt(c, r);
                const int p = keyPitch(pad, rowsTo - rowsFrom + 1);
                const int pc = ((p - P_->key) % 12 + 12) % 12;
                Rgb col = (mask >> pc) & 1 ? Rgb(0x0A3A36) : Rgb(0x080808);
                if (pc == 0) col = Rgb(0x5A3A00);
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
        static const Rgb kP[4] = {Rgb(0xFF40C0), Rgb(0x30E0FF), Rgb(0x60FF40), Rgb(0xA060FF)};
        const bool held[4] = {stutHeld_, hpHeld_, dlyThrow_, revThrow_};
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
            default: keyboard(0, 3); break;
        }
    }
    if (br < 0.99f)
        for (int i = 0; i < kPads; ++i) pads[i] = scale(pads[i], br);
}

}  // namespace konsol
}  // namespace lab
