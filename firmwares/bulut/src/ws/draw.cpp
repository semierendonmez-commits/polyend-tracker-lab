// BULUT drawing: intro, the seven pages, particles, pad LEDs (bulut_ws.h).
//
// Display-list budget: the Shell may reserve words for its overlays
// (Canvas::setLimit); every batch here asks g.room() first and the particle
// renderer only gets what is left, so a frame never overflows.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/dsp.h"
#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

namespace bulut {

using lab::Canvas;
using lab::Rgb;
namespace font = lab::font;
namespace kit = lab::kit;

namespace {
const Rgb kWhite(0xFFFFFF);
const Rgb kLilac(0xB9A8F0);
const Rgb kRed(0xFF6B6B);

const char* const kPageName[kPages] = {"CLOUDS", "SOUND", "SOURCE", "FX", "MOTION", "FREEZE", "FILE"};
const char* const kF8[kPages] = {"GEST REC", "ENGINE >", "REGION", "+ ROUTE", "GEST REC", "CAPTURE", "SAVE"};

inline float hash01(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.0f / 16777216.0f);
}

// 5 x 7 letters for the intro: B U L U T
const char* const kGlyph[4][7] = {
    {"11110", "10001", "10001", "11110", "10001", "10001", "11110"},   // B
    {"10001", "10001", "10001", "10001", "10001", "10001", "01110"},   // U
    {"10000", "10000", "10000", "10000", "10000", "10000", "11111"},   // L
    {"11111", "00100", "00100", "00100", "00100", "00100", "00100"},   // T
};
const int kWord[5] = {0, 1, 2, 1, 3};

inline float smooth(float x) {
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// What FX TIME / AMOUNT / FEEDBACK mean in each FX mode.
const char* fxLabel(int mode, int which) {
    static const char* const k[kFxModes][3] = {
        {"FX TIME", "FX AMOUNT", "FX FEEDBACK"}, {"TONE", "DIFFUSION", "FEEDBACK"}, {"WINDOW", "PITCH", "SHIMMER"},
        {"RATE", "DEPTH", "FEEDBACK"},          {"RATE", "DEPTH/RING", "-"},        {"RATE", "BITS/XOR", "LOOP"},
        {"PATTERN", "DEPTH", "RANDOM"},         {"LENGTH", "CATCH", "ROLL"}};
    return k[mode < 0 || mode >= kFxModes ? 0 : mode][which];
}

const char* genLabel(int gen, int which) {
    static const char* const k[kGenCount][2] = {{"LEAK", "SCALE"}, {"PITCH", "SHAPE"}, {"COLOUR", "DENSITY"}};
    return k[gen < 0 || gen >= kGenCount ? 0 : gen][which];
}

const char* srcShort(int s) {
    static const char* const k[kSrcCount] = {"INPUT", "OUTPUT", "SAMPLE", "GEN"};
    return k[s < 0 || s >= kSrcCount ? 0 : s];
}

// Log-frequency read of a magnitude array (40 Hz .. 16 kHz), 0..1.
inline float specAt(const float* m, float u) {
    const float hz = 40.0f * expf(u * 5.99f);
    int b = int(hz * float(Spectral::kAna) / kSr);
    b = b < 0 ? 0 : (b >= kSpecBins ? kSpecBins - 1 : b);
    const float v = m[b];
    return sqrtf(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
}
inline float specAt(const FreezeSlot& f, float u) {
    const float hz = 40.0f * expf(u * 5.99f);
    int b = int(hz * float(Spectral::kAna) / kSr);
    b = b < 0 ? 0 : (b >= kSpecBins ? kSpecBins - 1 : b);
    return sqrtf(float(f.mag[b]) / 65535.0f);
}
}  // namespace

// ---- intro: a drifting cloud of particles condenses into the name ----------------
void Bulut::drawIntro(Canvas& g, uint32_t t) {
    const float s = float(t) * 0.001f;
    g.beginPoints();
    for (int i = 0; i < 8; ++i) {
        const float a = 0.25f * smooth(s / 0.8f) * (1.0f - 0.5f * smooth((s - 2.0f) / 0.8f));
        g.color(mix(theme_.bg, chColor(i % kCh), 0.5f));
        g.alpha(uint8_t(104.0f * a));
        g.pointSize(90.0f + 30.0f * hash01(uint32_t(i) * 31u));
        g.vtx(400.0f + 260.0f * (hash01(uint32_t(i) * 7u + 1u) - 0.5f) + 30.0f * sinf(s * 0.7f + float(i)),
              240.0f + 120.0f * (hash01(uint32_t(i) * 13u + 5u) - 0.5f));
    }
    g.endBatch();
    constexpr float kCell = 17.0f;
    const float x0 = 400.0f - (29.0f * kCell) * 0.5f + kCell * 0.5f, y0 = 205.0f - 3.0f * kCell;
    const float gather = smooth((s - 0.6f) / 1.3f);
    const float settle = smooth((s - 1.9f) / 0.5f);
    g.beginPoints();
    int n = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const Rgb col = pass == 0 ? chColor(0) : (pass == 1 ? chColor(1) : kWhite);
        g.color(mix(col, kWhite, settle * 0.6f));
        g.alpha(uint8_t(pass == 2 ? 230 : 150));
        g.pointSize(pass == 0 ? 6.0f - 2.0f * settle : (pass == 1 ? 3.5f : 2.0f + 2.5f * settle));
        for (int L = 0; L < 5; ++L)
            for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c) {
                    if (kGlyph[kWord[L]][r][c] != '1') continue;
                    const uint32_t id = uint32_t(n++);
                    const float tx = x0 + float(L * 6 + c) * kCell + (hash01(id * 3u + 1u) - 0.5f) * 7.0f * (1.0f - settle);
                    const float ty = y0 + float(r) * kCell + (hash01(id * 5u + 2u) - 0.5f) * 7.0f * (1.0f - settle);
                    const float ang = hash01(id * 11u + 3u) * 6.2832f + s * (0.6f + hash01(id) * 0.8f);
                    const float rad = 60.0f + 230.0f * hash01(id * 17u + 4u);
                    const float sx = 400.0f + cosf(ang) * rad * 1.3f, sy = 240.0f + sinf(ang) * rad * 0.55f;
                    const float k = smooth(gather * (0.75f + 0.5f * hash01(id * 23u)) - 0.1f);
                    g.vtx(sx + (tx - sx) * k, sy + (ty - sy) * k);
                }
    }
    g.color(chColor(2));
    g.alpha(uint8_t(110.0f * (1.0f - 0.6f * settle)));
    g.pointSize(1.6f);
    for (int i = 0; i < 70; ++i) {
        const float ang = hash01(uint32_t(i) * 41u) * 6.2832f + s * 0.4f;
        const float rad = 150.0f + 200.0f * hash01(uint32_t(i) * 43u + 9u);
        g.vtx(400.0f + cosf(ang) * rad, 220.0f + sinf(ang) * rad * 0.42f + 10.0f * sinf(s * 2.0f + float(i)));
    }
    g.endBatch();
    g.alpha(255);
    const float tag = smooth((s - 1.9f) / 0.6f);
    if (tag > 0.01f) {
        g.color(mix(theme_.bg, theme_.dim, tag));
        g.text(400, 330, font::kBody, "four deep granular channels", Canvas::kCenter);
        g.color(mix(theme_.bg, theme_.accent, tag));
        g.text(400, 356, font::kTiny, "GRANULAR  *  QUANTA  *  BEADS  *  TAPE", Canvas::kCenter);
    }
}

// ---- shared pieces ----------------------------------------------------------------
void Bulut::drawFooter(Canvas& g) {
    const char* keys[8];
    if (!shift_) {
        for (int i = 0; i < 7; ++i) keys[i] = kPageName[i];
        keys[7] = kF8[page_];
        lab::drawFKeyBar(g, theme_, keys, page_);
        g.color(theme_.accent2);
        g.fill(702, lab::kScreenH - 4, 96, 2);   // F8 = the page's action
        return;
    }
    static const char* const kAct[kPages][8] = {
        {"MUTE", "SOLO", "FREEZE", "DRONE", "HOLD", "STEM", "GEST ON", "STORE SC"},
        {"GRANULAR", "QUANTA", "BEADS", "TAPE", "RESET", "RANDOM", "DRONE", "FREEZE"},
        {"INPUT", "OUTPUT", "SAMPLE", "GEN", "BROWSE", "< PREV", "NEXT >", "HOLD"},
        {"FX OFF", "FX MODE >", "FILTER >", "DRIVE", "", "", "", "UNROUTE"},
        {"NEW/ODUB", "CLEAR", "PLAY", "ALL ON", "ALL OFF", "SONG", "ADD TL", "STORE SC"},
        {"RECALL", "DELETE", "FROM", "UNFREEZE", "MORPH SL", "RESET T", "", ""},
        {"LOAD", "NEW", "RESAMPLE", "", "", "", "", "SAVE"},
    };
    for (int i = 0; i < 8; ++i) keys[i] = kAct[page_][i];
    lab::drawFKeyBar(g, theme_, keys, -1);
    g.color(theme_.accent2);
    g.fill(0, lab::kScreenH - 31, lab::kScreenW, 1);
}

// A small status pill; returns its width.
static int pill(Canvas& g, const lab::Theme& t, int x, int y, const char* s, Rgb col, bool solid) {
    const int w = 8 + int(std::strlen(s)) * 7;
    g.color(solid ? col : mix(t.panel, col, 0.25f));
    g.fillRound(x, y, w, 15, 4.0f);
    g.color(solid ? t.bg : col);
    g.text(x + w / 2, y + 8, font::kTiny, s, Canvas::kCenter);
    return w + 4;
}

// What a channel is doing, at a glance: REC, GEST, FROZEN, DRONE, HOLD, MUTE, SOLO.
void Bulut::drawBadges(Canvas& g, int c, int x, int y) {
    const ChState& s = st_[c];
    const bool recThis = rec_->state() != GestureRecorder::kIdle && rec_->cloud() == c;
    if (recThis) x += pill(g, theme_, x, y, rec_->state() == GestureRecorder::kRecording ? "REC" : "ARMED", kRed, true);
    if (eng_->gestLive(c).len && s.gestOn) x += pill(g, theme_, x, y, "GEST", theme_.accent2, gestFlash_[c] > 0.3f);
    if (s.freeze && eng_->rt[c].frzReady) x += pill(g, theme_, x, y, "FROZEN", kLilac, true);
    if (s.drone) x += pill(g, theme_, x, y, "DRONE", chColor(c), false);
    if (s.hold && par_[c].source != kSrcSample) x += pill(g, theme_, x, y, "HOLD", theme_.fg, false);
    if (s.mute) x += pill(g, theme_, x, y, "MUTE", theme_.dim, true);
    if (eng_->soloMask & (1u << c)) pill(g, theme_, x, y, "SOLO", theme_.ok, true);
}

// The grain field of a channel: the region's waveform (what the pads read),
// soft glows where grains are born, held pads, the position.
void Bulut::drawField(Canvas& g, int c, int x, int y, int w, int h, bool big) {
    const Rgb col = chColor(c);
    const int e = par_[c].engine;
    const bool synth = e == kQuanta && enumIndex(par_[c].k[K::Mode], kQTypes) != kQSample;
    const bool frz = st_[c].freeze && eng_->rt[c].frzReady;
    const bool empty = par_[c].source == kSrcSample && eng_->rt[c].sampleLen == 0;
    g.color(mix(theme_.bg, frz ? kLilac : col, 0.06f));
    g.fill(x, y, w, h);
    // soft glow under the pad columns where grains are born
    if (g.room(40)) {
        g.beginPoints();
        g.color(frz ? kLilac : col);
        for (int k = 0; k < 12; ++k) {
            const float gl = colGlow_[c][k];
            if (gl < 0.05f) continue;
            g.alpha(uint8_t(gl * 46.0f));
            g.pointSize(float(h) * (big ? 0.42f : 0.55f));
            g.vtx(float(x) + (float(k) + 0.5f) * float(w) / 12.0f, float(y) + float(h) * 0.5f);
        }
        g.endBatch();
        g.alpha(255);
    }
    // waveform of the region (or the freeze loop / a pitch ruler)
    const int bars = big ? 64 : 48;
    if (!synth && !empty && g.room(bars * 2 + 10)) {
        g.color(mix(theme_.bg, frz ? kLilac : col, big ? 0.42f : 0.34f));
        g.beginLines();
        const float cy = float(y) + float(h) * 0.5f;
        for (int i = 0; i < bars; ++i) {
            const float hx = float(x) + (float(i) + 0.5f) * float(w) / float(bars);
            const float a = frz ? 0.30f + 0.12f * sinf(float(i) * 0.7f) * sinf(float(i) * 0.13f)
                                : float(regWave_[c][i * 64 / bars]) / 255.0f;
            const float hh = (0.03f + 0.9f * sqrtf(a)) * float(h) * 0.46f;
            g.vtx(hx, cy - hh);
            g.vtx(hx, cy + hh);
        }
        g.endBatch();
    } else if (synth && g.room(30)) {
        g.color(mix(theme_.bg, col, 0.25f));
        g.beginLines();
        for (int k = 0; k <= 12; ++k) {
            const float lx = float(x) + float(k) * float(w) / 12.0f;
            const float hh = float(h) * (0.1f + 0.035f * float(k));
            g.vtx(lx, float(y + h) - hh);
            g.vtx(lx, float(y + h));
        }
        g.endBatch();
    }
    // pad column grid (the pads read the region left to right)
    if (big && g.room(40)) {
        g.color(mix(theme_.bg, col, 0.18f));
        g.beginLines();
        for (int k = 1; k < 12; ++k) {
            const float lx = float(x + k * w / 12);
            g.vtx(lx, float(y));
            g.vtx(lx, float(y + h));
        }
        for (int r = 1; r < 4; ++r) {
            g.vtx(float(x), float(y + r * h / 4));
            g.vtx(float(x + w), float(y + r * h / 4));
        }
        g.endBatch();
    }
    // held pads, gesture ghosts
    const ChRt& rt = eng_->rt[c];
    for (int i = 0; i < kEmitters; ++i) {
        const Emitter& em = rt.em[i];
        if (!em.on || em.pad >= 48) continue;
        const int px = x + (em.pad % 12) * w / 12;
        const int py = big ? y + (em.pad / 12) * h / 4 : y;
        const int ph = big ? h / 4 : h;
        g.color(i < kLiveEmitters ? kWhite : theme_.accent2);
        g.frame(px + 1, py + 1, w / 12 - 2, ph - 2);
    }
    // TAPE: the playhead; others: the POSITION knob (drone / MIDI)
    if (!synth && !empty) {
        float pos = par_[c].k[K::Pos];
        if (e == kTape && rt.tapeHead >= 0.0f) pos = rt.tapeHead;
        g.color(scale(col, 0.85f));
        g.fill(x + int(clamp01(pos) * float(w - 2)), y, 2, h);
    }
    if (empty) {
        g.color(theme_.dim);
        g.text(x + w / 2, y + h / 2, big ? font::kSmall : font::kTiny, "NO SAMPLE  -  S.Load or Copy", Canvas::kCenter);
    }
}

// Particles of one channel: a soft halo, a bright core, fading motes; at most
// `limit` vertices and never past the room the Shell leaves.
void Bulut::drawParticles(Canvas& g, int c, int x, int y, int w, int h, bool big, uint32_t limit) {
    if (!g.room(60)) return;
    const Rgb col = st_[c].freeze ? mix(chColor(c), kLilac, 0.5f) : chColor(c);
    g.clip(x, y, w, h);
    uint32_t used = 0;
    // halos of the young, big grains
    g.beginPoints();
    g.color(col);
    g.alpha(34);
    g.pointSize(big ? 12.0f : 8.0f);
    int halos = 0;
    for (int i = 0; i < kParticles && halos < (big ? 40 : 14); ++i) {
        const Particle& p = part_[i];
        if (p.life <= 0.0f || p.cloud != c || p.size < 2 || p.age > p.life * 0.4f) continue;
        if (!g.room(30) || used >= limit) break;
        g.vtx(float(x) + p.x * float(w), float(y) + p.y * float(h));
        ++halos;
        ++used;
    }
    const float sizes[3] = {big ? 1.3f : 1.0f, big ? 2.3f : 1.6f, big ? 3.6f : 2.4f};
    static const uint8_t kA[4] = {235, 165, 100, 45};
    for (int bright = 1; bright >= 0; --bright) {
        g.color(bright ? mix(col, kWhite, 0.55f) : col);
        for (int sz = 2; sz >= 0; --sz) {
            g.pointSize(sizes[sz]);
            for (int ab = 0; ab < 4; ++ab) {
                bool first = true;
                for (int i = 0; i < kParticles; ++i) {
                    const Particle& p = part_[i];
                    if (p.life <= 0.0f || p.cloud != c || p.size != sz || p.live != bright) continue;
                    const int b = int((p.age / p.life) * 4.0f);
                    if ((b > 3 ? 3 : b) != ab) continue;
                    if (!g.room(12) || used >= limit) break;
                    if (first) {
                        g.alpha(kA[ab]);
                        first = false;
                    }
                    g.vtx(float(x) + p.x * float(w), float(y) + p.y * float(h));
                    ++used;
                }
            }
        }
    }
    g.endBatch();
    g.alpha(255);
    g.noClip();
}

// One channel lane on CLOUDS: field on the left, state + mini mixer on the right.
void Bulut::drawLane(Canvas& g, int c, int x, int y, int w, int h, bool big) {
    (void)big;
    const Rgb col = chColor(c);
    const bool selc = c == sel_;
    const int fw = 470;
    drawField(g, c, x, y, fw, h, false);
    const int ix = x + fw + 6, iw = w - fw - 6;
    g.color(selc ? mix(theme_.panel, col, 0.16f) : theme_.panel);
    g.fill(ix, y, iw, h);
    if (selc) {
        g.color(col);
        g.fill(ix, y, 3, h);
    }
    // level: a breathing dot
    const float lv = eng_->rt[c].peak > 1.0f ? 1.0f : eng_->rt[c].peak;
    g.color(st_[c].mute ? theme_.dim : col);
    g.point(float(ix + 18), float(y + 14), 3.0f + 6.0f * sqrtf(lv));
    g.color(selc ? kWhite : theme_.fg);
    g.textf(ix + 32, y + 13, font::kSmall, Canvas::kCenterY, "%d %s", c + 1, engineName(par_[c].engine));
    // where its sound comes from
    char srcTxt[48];
    if (par_[c].source == kSrcSample)
        std::snprintf(srcTxt, sizeof srcTxt, "%.22s", src_[c].label[0] ? src_[c].label : "no sample");
    else if (par_[c].source == kSrcGen)
        std::snprintf(srcTxt, sizeof srcTxt, "GEN %s", genName(par_[c].genType));
    else
        std::snprintf(srcTxt, sizeof srcTxt, "%s", par_[c].source == kSrcInput ? "LIVE INPUT" : "BULUT OUTPUT");
    g.color(theme_.dim);
    g.text(ix + iw - 8, y + 13, font::kTiny, srcTxt, Canvas::kCenterY | Canvas::kRight);
    drawBadges(g, c, ix + 10, y + 26);
    // mini mixer: LEVEL PAN DLY REV
    static const char* const kMix[4] = {"LEVEL", "PAN", "DLY", "REV"};
    static const uint8_t kIdx[4] = {CP::Level, CP::Pan, CP::Dly, CP::Rev};
    const int cw = (iw - 16) / 4;
    for (int k = 0; k < 4; ++k) {
        const int cx = ix + 8 + k * cw, cy = y + h - 34;
        const bool cur = selc && cloudsRow_ == c && (cur_[kPgClouds] & 3) == k;
        if (cur) {
            g.color(mix(theme_.panel, col, 0.35f));
            g.fill(cx, cy, cw - 4, 30);
        }
        const float v = ctx_->params().norm(pid_[c][kIdx[k]]);
        g.color(cur ? kWhite : theme_.dim);
        if (cur) {
            char t[16];
            ctx_->params().format(pid_[c][kIdx[k]], t, sizeof t);
            g.text(cx + 4, cy + 9, font::kTiny, t, Canvas::kCenterY);
        } else {
            g.text(cx + 4, cy + 9, font::kTiny, kMix[k], Canvas::kCenterY);
        }
        g.color(mix(theme_.bg, col, 0.3f));
        g.fill(cx + 4, cy + 21, cw - 12, 4);
        g.color(col);
        if (k == 1) g.fill(cx + 4 + int(float(cw - 16) * v), cy + 19, 4, 8);
        else g.fill(cx + 4, cy + 21, int(float(cw - 12) * v), 4);
    }
}

void Bulut::drawKnob(Canvas& g, int c, int idx, int x, int y, int w, bool focused) {
    const Rgb col = chColor(c);
    kit::Registry& P = ctx_->params();
    const kit::ParamId id = pid_[c][idx];
    const float v = P.norm(id);
    const float ev = P.effectiveNorm(id);
    const int h = 62;
    g.color(focused ? mix(theme_.panel, col, 0.28f) : theme_.panel);
    g.fill(x, y, w - 4, h);
    if (focused) {
        g.color(col);
        g.fill(x, y, 3, h);
    }
    g.color(focused ? col : theme_.dim);
    g.text(x + 10, y + 11, font::kTiny, paramName(c, idx), Canvas::kCenterY);
    int routes[4];
    if (ctx_->mod().routesTo(id, routes, 4) > 0) {
        g.color(theme_.accent2);
        g.point(float(x + w - 14), float(y + 11), 3.0f);
    }
    char t[24];
    paramText(c, idx, t, sizeof t);
    g.color(focused ? kWhite : theme_.fg);
    g.text(x + 10, y + 32, font::kBody, t, Canvas::kCenterY);
    // bar: base value, and a tick where modulation takes it
    const int bw = w - 24;
    g.color(mix(theme_.bg, col, 0.25f));
    g.fill(x + 10, y + 50, bw, 4);
    const bool bip = idx == CP::Pan;
    g.color(col);
    if (bip) g.fill(x + 10 + int(float(bw - 4) * v), y + 48, 4, 8);
    else g.fill(x + 10, y + 50, int(float(bw) * v), 4);
    if (ev < v - 0.005f || ev > v + 0.005f) {
        g.color(theme_.accent2);
        g.fill(x + 10 + int(float(bw - 2) * clamp01(ev)), y + 46, 2, 12);
    }
}

// ---- the frame ----------------------------------------------------------------------------
void Bulut::draw(Canvas& g) {
    char title[48];
    if (page_ == kPgSound || page_ == kPgSource || page_ == kPgFx)
        std::snprintf(title, sizeof title, "%s  CH %d %s", kPageName[page_], sel_ + 1, engineName(par_[sel_].engine));
    else if (page_ == kPgClouds)
        std::snprintf(title, sizeof title, "CLOUDS  %d grains", eng_->totalActive);
    else
        std::snprintf(title, sizeof title, "%s", kPageName[page_]);
    lab::drawHeader(g, theme_, *ctx_->shell, title);
    switch (page_) {
        case kPgClouds: drawPageClouds(g); break;
        case kPgSound: drawPageSound(g); break;
        case kPgSource: drawPageSource(g); break;
        case kPgFx: drawPageFx(g); break;
        case kPgMotion: drawPageMotion(g); break;
        case kPgFreeze: drawPageFreeze(g); break;
        default: drawPageFile(g); break;
    }
    // recording states that matter on every page
    int px = 404;
    if (rec_->state() != GestureRecorder::kIdle) {
        const bool on = rec_->state() == GestureRecorder::kRecording;
        g.color(on ? kRed : mix(theme_.panel, kRed, 0.5f));
        g.fillRound(px, 6, 146, 18, 6.0f);
        g.color(kWhite);
        if (on)
            g.textf(px + 73, 15, font::kTiny, Canvas::kCenter, "GESTURE %d  %.1f s", rec_->cloud() + 1,
                    double(float(rec_->elapsed(eng_->sampleClock)) / kSr));
        else
            g.textf(px + 73, 15, font::kTiny, Canvas::kCenter, "ARMED CH %d: MOVE", rec_->cloud() + 1);
    } else if (rsState_) {
        g.color(kRed);
        g.fillRound(px, 6, 146, 18, 6.0f);
        g.color(kWhite);
        g.textf(px + 73, 15, font::kTiny, Canvas::kCenter, rsState_ == 1 ? "RESAMPLE %.1f s" : "WRITING %.1f s",
                double(float(eng_->capLen) / kSr));
    }
    if (shift_) {
        // what the pads do while Shift is held (the LEDs show the same map)
        g.color(theme_.panel);
        g.fill(0, 396, 800, 52);
        g.color(theme_.accent2);
        g.hline(0, 396, 800);
        g.color(theme_.fg);
        g.text(8, 408, font::kTiny, "SHIFT + PADS  row 1: select CH 1-4 | mute 1-4 | solo 1-4", Canvas::kCenterY);
        g.text(8, 424, font::kTiny, "row 2: scenes 1-8 (empty = store, lit = morph) | gesture REC CH 1-4", Canvas::kCenterY);
        g.text(8, 440, font::kTiny, "row 3: freeze slots 1-12 (empty = capture)   row 4: freeze 1-4 | drone 1-4 | gesture play 1-4",
               Canvas::kCenterY);
    }
    drawFooter(g);
}

// ---- CLOUDS: the four channels ---------------------------------------------------------------
void Bulut::drawPageClouds(Canvas& g) {
    const int lh = 84;
    for (int c = 0; c < kCh; ++c) drawLane(g, c, 8, 36 + c * (lh + 3), 784, lh, false);
    // send FX row
    const int y = 386;
    const bool rowSel = cloudsRow_ == kCh;
    g.color(rowSel ? mix(theme_.panel, theme_.accent, 0.12f) : theme_.panel);
    g.fill(8, y, 784, 60);
    g.color(theme_.accent);
    g.text(16, y + 12, font::kTiny, "SEND FX", Canvas::kCenterY);
    // master meter
    const float pk = eng_->meterL > eng_->meterR ? eng_->meterL : eng_->meterR;
    g.color(mix(theme_.bg, theme_.accent, 0.3f));
    g.fill(16, y + 26, 60, 4);
    g.color(pk > 0.95f ? kRed : theme_.accent);
    g.fill(16, y + 26, int(60.0f * sqrtf(pk > 1.0f ? 1.0f : pk)), 4);
    g.color(theme_.dim);
    g.textf(16, y + 44, font::kTiny, Canvas::kCenterY, "%d/%d gr", eng_->totalActive, eng_->grainLimit);
    static const char* const kFx[7] = {"DLY TIME", "DLY FB", "DLY TONE", "DLY RET", "REV SIZE", "REV DAMP", "REV RET"};
    const int cw = 100;
    for (int i = 0; i < 7; ++i) {
        const int cx = 92 + i * cw;
        const bool cur = rowSel && (cur_[kPgClouds] % 7) == i;
        if (cur) {
            g.color(mix(theme_.panel, theme_.accent, 0.35f));
            g.fill(cx, y + 4, cw - 4, 52);
        }
        g.color(cur ? theme_.accent : theme_.dim);
        g.text(cx + 6, y + 14, font::kTiny, kFx[i], Canvas::kCenterY);
        char t[16];
        ctx_->params().format(mid_[i], t, sizeof t);
        g.color(cur ? kWhite : theme_.fg);
        g.text(cx + 6, y + 32, font::kSmall, t, Canvas::kCenterY);
        g.color(mix(theme_.bg, theme_.accent, 0.25f));
        g.fill(cx + 6, y + 46, cw - 16, 3);
        g.color(theme_.accent);
        g.fill(cx + 6, y + 46, int(float(cw - 16) * ctx_->params().norm(mid_[i])), 3);
    }
    // particles last: they get the words that are left, shared by the lanes
    for (int c = 0; c < kCh; ++c) drawParticles(g, c, 8, 36 + c * (lh + 3), 470, lh, false, c == sel_ ? 160u : 110u);
}

// ---- SOUND: the engine's 12 knobs --------------------------------------------------------------
void Bulut::drawPageSound(Canvas& g) {
    const int c = sel_;
    const int e = par_[c].engine;
    const Rgb col = chColor(c);
    const int fx = 8, fy = 36, fw = 560, fh = 132;
    drawField(g, c, fx, fy, fw, fh, true);
    // engine picture
    g.color(theme_.panel);
    g.fill(576, fy, 216, fh);
    const float* k = par_[c].k;
    g.color(col);
    g.lineWidth(1.5f);
    g.beginStrip();
    const int type = enumIndex(k[K::Mode], kQTypes);
    for (int i = 0; i <= 60; ++i) {
        const float u = float(i) / 60.0f;
        float v = 0.0f;
        switch (e) {
            case kGranular: v = windowValue(enumIndex(k[K::Shape], kTexWindows), u); break;
            case kBeads: {
                // Beads' envelope: SHAPE goes from a click to a slow swell
                const float sh = k[K::Shape];
                const float att = 0.02f + 0.48f * sh, rel = 0.5f - 0.3f * fabsf(sh - 0.5f);
                v = u < att ? u / att : (u > 1.0f - rel ? (1.0f - u) / rel : 1.0f);
                v = v < 0.0f ? 0.0f : v;
                break;
            }
            case kTape: {
                // the playhead over time: loops of GRAIN at SPEED
                const float sp = tapeSpeed(k[K::Dens]);
                const float ph = fmodf(u * 4.0f, 1.0f);
                v = 0.5f + 0.45f * (sp >= 0.0f ? ph : 1.0f - ph) * (fabsf(sp) > 1.0f ? 1.0f : fabsf(sp)) - 0.2f;
                break;
            }
            default: {
                const float ch = k[K::Shape];
                switch (type) {
                    case kQGlisson: v = 0.5f + 0.5f * windowValue(5, u) * sinf(6.2832f * u * (4.0f + u * (ch - 0.5f) * 16.0f)); break;
                    case kQGabor:
                    case kQGrainlet: v = 0.5f + 0.5f * windowValue(6 + enumIndex(1.0f - ch, 10), u) * sinf(6.2832f * u * 6.0f); break;
                    case kQPulsar: {
                        const float ph = fmodf(u * 4.0f, 1.0f);
                        const float ratio = 1.0f + ch * 7.0f;
                        v = 0.5f + 0.5f * (ph * ratio < 1.0f ? sinf(6.2832f * ph * ratio) * windowValue(2, u) : 0.0f);
                        break;
                    }
                    case kQTrainlet: v = 0.5f + 0.5f * windowValue(kWinPerc, fmodf(u * 5.0f, 1.0f)) * cosf(u * 60.0f); break;
                    default: v = 0.5f + 0.5f * windowValue(kWinGabor, u) * sinf(u * 40.0f); break;
                }
                break;
            }
        }
        g.vtx(586.0f + u * 196.0f, float(fy + fh - 26) - v * float(fh - 46));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    g.color(theme_.dim);
    const char* cap = e == kGranular ? "GRAIN WINDOW" : (e == kBeads ? "BEADS ENVELOPE" : (e == kTape ? "TAPE PLAYHEAD" : qTypeName(type)));
    g.text(586, fy + fh - 12, font::kTiny, cap, Canvas::kCenterY);
    g.textf(782, fy + 12, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d grains", eng_->rt[c].active);
    drawBadges(g, c, 586, fy + 6);
    // the 12 knobs: 3 rows of 4
    for (int i = 0; i < kKnobs; ++i) drawKnob(g, c, i, 8 + (i % 4) * 196, 176 + (i / 4) * 68, 196, i == cur_[kPgSound]);
    drawParticles(g, c, fx, fy, fw, fh, true, 420u);
}

// ---- SOURCE: what feeds the channel, the sample, the region -------------------------------------
void Bulut::drawPageSource(Canvas& g) {
    const int c = sel_;
    const Rgb col = chColor(c);
    const ChRt& r = eng_->rt[c];
    const int wx = 8, wy = 36, ww = 784, wh = 168;
    g.color(theme_.panel);
    g.fill(wx, wy, ww, wh);
    const float a0 = par_[c].regStart < par_[c].regEnd ? par_[c].regStart : par_[c].regEnd;
    const float a1 = par_[c].regStart < par_[c].regEnd ? par_[c].regEnd : par_[c].regStart;
    // region band
    g.color(mix(theme_.panel, col, 0.18f));
    g.fill(wx + int(a0 * float(ww)), wy, int((a1 - a0) * float(ww)) + 1, wh);
    // the whole source, 96 columns
    const bool empty = par_[c].source == kSrcSample && r.sampleLen == 0;
    if (!empty && g.room(220)) {
        g.beginLines();
        const float cy = float(wy) + float(wh) * 0.5f;
        bool in = false;
        g.color(mix(theme_.bg, col, 0.45f));
        for (int i = 0; i < 96; ++i) {
            const float u = (float(i) + 0.5f) / 96.0f;
            const bool inside = u >= a0 && u <= a1;
            if (inside != in) {
                g.color(inside ? col : mix(theme_.bg, col, 0.45f));
                in = inside;
            }
            const float hx = float(wx) + u * float(ww);
            const float hh = 2.0f + float(wh) * 0.44f * sqrtf(float(wave_[c][i]) / 255.0f);
            g.vtx(hx, cy - hh);
            g.vtx(hx, cy + hh);
        }
        g.endBatch();
    }
    // region edges + handles
    g.color(kWhite);
    g.fill(wx + int(a0 * float(ww - 2)), wy, 2, wh);
    g.fill(wx + int(a1 * float(ww - 2)), wy, 2, wh);
    // where grains are born right now (particles mapped into the region)
    if (g.room(140)) {
        g.clip(wx, wy, ww, wh);
        g.beginPoints();
        g.color(mix(col, kWhite, 0.5f));
        g.pointSize(2.0f);
        int n = 0;
        for (int i = 0; i < kParticles && n < 90; ++i) {
            const Particle& p = part_[i];
            if (p.life <= 0.0f || p.cloud != c || p.size == 0) continue;
            if (!g.room(40)) break;
            g.alpha(uint8_t(220.0f * (1.0f - p.age / p.life)));
            g.vtx(float(wx) + (a0 + p.x * (a1 - a0)) * float(ww), float(wy) + p.y * float(wh));
            ++n;
        }
        g.endBatch();
        g.alpha(255);
        g.noClip();
    }
    // REGION pads: the 48 steps
    if (regionMode_) {
        g.color(theme_.accent2);
        g.frame(wx, wy, ww, wh);
        g.beginLines();
        for (int k = 1; k < 48; ++k) {
            const float lx = float(wx) + float(k) * float(ww) / 48.0f;
            g.vtx(lx, float(wy + wh - (k % 12 == 0 ? 22 : 8)));
            g.vtx(lx, float(wy + wh));
        }
        g.endBatch();
        if (regionFirst_ >= 0) g.fill(wx + regionFirst_ * ww / 48, wy, ww / 48, wh);
        g.color(theme_.accent2);
        g.text(wx + ww / 2, wy + 14, font::kSmall,
               regionFirst_ < 0 ? "REGION: tap the first pad" : "REGION: tap the last pad", Canvas::kCenter);
    }
    if (empty) {
        g.color(theme_.dim);
        g.text(wx + ww / 2, wy + wh / 2 - 10, font::kBody, "No sample in this channel", Canvas::kCenter);
        g.text(wx + ww / 2, wy + wh / 2 + 14, font::kSmall, "S.Load (or Insert) opens the browser", Canvas::kCenter);
    }
    // the name line
    g.color(theme_.panel);
    g.fill(8, 208, 784, 28);
    char nm[64];
    if (par_[c].source == kSrcSample)
        std::snprintf(nm, sizeof nm, "%.40s   %.2f s", src_[c].label[0] ? src_[c].label : "(none)", double(float(r.sampleLen) / kSr));
    else if (par_[c].source == kSrcGen)
        std::snprintf(nm, sizeof nm, "GENERATOR %s - writes the buffer", genName(par_[c].genType));
    else if (par_[c].source == kSrcInput)
        std::snprintf(nm, sizeof nm, "LIVE INPUT - last 8 s%s", st_[c].hold ? " (HELD)" : "");
    else
        std::snprintf(nm, sizeof nm, "BULUT OUTPUT - feedback of the mix%s", st_[c].hold ? " (HELD)" : "");
    g.color(col);
    g.text(16, 222, font::kSmall, nm, Canvas::kCenterY);
    g.color(theme_.dim);
    g.text(784, 222, font::kTiny, "Copy: next file  Shift+Copy: prev  S.Load: browse", Canvas::kCenterY | Canvas::kRight);
    // the rows: two columns
    for (int i = 0; i < 9; ++i) {
        const int x = i < 5 ? 8 : 404, y = 242 + (i < 5 ? i : i - 5) * 41;
        const bool cur = cur_[kPgSource] == i;
        g.color(cur ? mix(theme_.panel, col, 0.28f) : theme_.panel);
        g.fill(x, y, 388, 37);
        if (cur) {
            g.color(col);
            g.fill(x, y, 3, 37);
        }
        const char* label;
        char v[40];
        if (i < 8) {
            const int idx = (i == 0 ? CP::Source : i == 1 ? CP::Gain : i == 2 ? CP::Start : i == 3 ? CP::End :
                             i == 4 ? CP::GenType : i == 5 ? CP::GenA : i == 6 ? CP::GenB : CP::Bed);
            label = idx == CP::GenA ? genLabel(par_[c].genType, 0) : (idx == CP::GenB ? genLabel(par_[c].genType, 1) : paramName(c, idx));
            paramText(c, idx, v, sizeof v);
            if (idx == CP::GenA || idx == CP::GenB || idx == CP::GenType) {
                if (par_[c].source != kSrcGen) {
                    g.color(theme_.dim);
                    g.text(x + 12, y + 18, font::kTiny, label, Canvas::kCenterY);
                    g.text(x + 376, y + 18, font::kTiny, v, Canvas::kCenterY | Canvas::kRight);
                    continue;
                }
            }
        } else {
            label = "RADIO";
            std::snprintf(v, sizeof v, "%.1f MHz", double(radioMHz_));
        }
        g.color(cur ? col : theme_.dim);
        g.text(x + 12, y + 18, font::kSmall, label, Canvas::kCenterY);
        g.color(cur ? kWhite : theme_.fg);
        g.text(x + 376, y + 18, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
}

// ---- FX: drive, filter, FX mode, LFO, routes -------------------------------------------------------
void Bulut::drawPageFx(Canvas& g) {
    const int c = sel_;
    const Rgb col = chColor(c);
    const ChParams& p = par_[c];
    // the chain, left to right
    static const char* const kBox[6] = {"SOURCE", "ENGINE", "DRIVE", "FILTER", "FX", "FADER"};
    for (int i = 0; i < 6; ++i) {
        const int x = 8 + i * 131, y = 38;
        bool on = true;
        char v[24];
        switch (i) {
            case 0: std::snprintf(v, sizeof v, "%s", srcShort(p.source)); break;
            case 1: std::snprintf(v, sizeof v, "%s", engineShort(p.engine)); break;
            case 2: on = p.drive > 0.01f; std::snprintf(v, sizeof v, "%d%%", int(p.drive * 100.0f + 0.5f)); break;
            case 3:
                on = p.fltMode != kFltOff;
                if (on) std::snprintf(v, sizeof v, "%s %d", fltName(p.fltMode), int(p.cutoff + 0.5f));
                else std::snprintf(v, sizeof v, "OFF");
                break;
            case 4: on = p.fxMode != kFxOff; std::snprintf(v, sizeof v, "%s", fxName(p.fxMode)); break;
            default: std::snprintf(v, sizeof v, "%d%%", int(p.level * 100.0f + 0.5f)); break;
        }
        g.color(on ? mix(theme_.panel, col, 0.3f) : theme_.panel);
        g.fillRound(x, y, 118, 48, 6.0f);
        g.color(on ? col : theme_.dim);
        g.text(x + 59, y + 14, font::kTiny, kBox[i], Canvas::kCenter);
        g.color(on ? kWhite : theme_.dim);
        g.text(x + 59, y + 33, font::kSmall, v, Canvas::kCenter);
        if (i < 5) {
            g.color(theme_.dim);
            g.fill(x + 119, y + 24, 12, 1);
        }
    }
    g.color(theme_.dim);
    g.textf(792, 96, font::kTiny, Canvas::kCenterY | Canvas::kRight, "then sends: DLY %d%%  REV %d%%  -> master",
            int(p.dly * 100.0f + 0.5f), int(p.rev * 100.0f + 0.5f));
    g.text(8, 96, font::kTiny, "INSERT", Canvas::kCenterY);
    g.text(270, 96, font::kTiny, "FX MODE", Canvas::kCenterY);
    g.text(532, 96, font::kTiny, "CHANNEL LFO + ROUTES", Canvas::kCenterY);
    // three columns of param cells (cursor runs down a column, Left/Right jumps)
    const int cur = cur_[kPgFx];
    for (int i = 0; i < 10; ++i) {
        static const uint8_t kRows[10] = {CP::Drive, CP::Flt, CP::Cutoff, CP::Reso, CP::FxMode,
                                          CP::FxTime, CP::FxAmt, CP::FxFb, CP::LfoRate, CP::LfoShape};
        const int col3 = i / 4, row = i % 4;
        const int x = 8 + col3 * 262, y = 106 + row * 68;
        const int idx = kRows[i];
        // the FX mode names its three knobs
        if (idx >= CP::FxTime && idx <= CP::FxFb) {
            const bool fc = cur == i;
            const kit::ParamId id = pid_[c][idx];
            g.color(fc ? mix(theme_.panel, col, 0.28f) : theme_.panel);
            g.fill(x, y, 258, 62);
            if (fc) {
                g.color(col);
                g.fill(x, y, 3, 62);
            }
            g.color(fc ? col : theme_.dim);
            g.text(x + 10, y + 11, font::kTiny, fxLabel(p.fxMode, idx - CP::FxTime), Canvas::kCenterY);
            char t[24];
            ctx_->params().format(id, t, sizeof t);
            g.color(fc ? kWhite : (p.fxMode ? theme_.fg : theme_.dim));
            g.text(x + 10, y + 32, font::kBody, t, Canvas::kCenterY);
            g.color(mix(theme_.bg, col, 0.25f));
            g.fill(x + 10, y + 50, 234, 4);
            g.color(col);
            g.fill(x + 10, y + 50, int(234.0f * ctx_->params().norm(id)), 4);
            const float ev = ctx_->params().effectiveNorm(id), bv = ctx_->params().norm(id);
            if (ev < bv - 0.005f || ev > bv + 0.005f) {
                g.color(theme_.accent2);
                g.fill(x + 10 + int(232.0f * clamp01(ev)), y + 46, 2, 12);
            }
            continue;
        }
        drawKnob(g, c, idx, x, y, 262, cur == i);
    }
    // the LFO, drawn
    const int lx = 532, ly = 242;
    g.color(theme_.panel);
    g.fill(lx, ly, 258, 30);
    g.color(col);
    g.lineWidth(1.2f);
    g.beginStrip();
    for (int i = 0; i <= 40; ++i) {
        const float u = float(i) / 40.0f, ph = fmodf(u * 2.0f, 1.0f);
        float v;
        switch (p.lfoShape) {
            case kLfoTri: v = ph < 0.5f ? ph * 4.0f - 1.0f : 3.0f - ph * 4.0f; break;
            case kLfoSaw: v = ph * 2.0f - 1.0f; break;
            case kLfoSquare: v = ph < 0.5f ? 1.0f : -1.0f; break;
            case kLfoSh: v = hash01(uint32_t(u * 2.0f * 6.0f)) * 2.0f - 1.0f; break;
            case kLfoDrift: v = sinf(u * 9.0f) * 0.6f + sinf(u * 23.0f) * 0.3f; break;
            default: v = sinf(ph * 6.2832f); break;
        }
        g.vtx(float(lx + 6) + u * 200.0f, float(ly + 15) - v * 11.0f);
    }
    g.endBatch();
    g.lineWidth(1.0f);
    const float lv = eng_->rt[c].lfo;
    g.color(kWhite);
    g.point(float(lx + 236), float(ly + 15) - lv * 11.0f, 4.0f);
    // routes (every LABKIT route that lands on this channel)
    kit::Mod& M = ctx_->mod();
    int shown = 0;
    g.color(theme_.dim);
    g.textf(lx, 284, font::kTiny, Canvas::kCenterY, "ROUTES (%d)  F8: LFO -> focused", M.routeCount());
    for (int i = 0; i < M.routeCount() && shown < 6; ++i) {
        const kit::Route& rt = M.routeAt(i);
        int idx = 0;
        if (chOf(rt.param, &idx) != c) continue;
        const bool rc = cur == 10 + i;
        const int y = 296 + shown * 25;
        g.color(rc ? mix(theme_.panel, theme_.accent2, 0.35f) : theme_.panel);
        g.fill(lx, y, 258, 23);
        g.color(rc ? kWhite : theme_.fg);
        g.textf(lx + 6, y + 12, font::kTiny, Canvas::kCenterY, "%.10s > %.12s", M.sourceName(rt.src), paramName(c, idx));
        g.color(theme_.accent2);
        g.textf(lx + 252, y + 12, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%+d%%", int(rt.depth * 100.0f));
        ++shown;
    }
    if (cur >= 10 && shown == 0) {
        g.color(theme_.dim);
        g.text(lx + 6, 308, font::kTiny, "(the cursor is on another channel's route)", Canvas::kCenterY);
    }
    if (!shown && M.routeCount() == 0) {
        g.color(theme_.dim);
        g.text(lx + 6, 308, font::kTiny, "none yet: F8 or Insert adds one", Canvas::kCenterY);
    }
}

// ---- MOTION: gestures, scenes, timeline -------------------------------------------------------------
void Bulut::drawPageMotion(Canvas& g) {
    // gesture lanes
    const bool fG = motionFocus_ == 0;
    for (int c = 0; c < kCh; ++c) {
        const Gesture& ge = eng_->gestLive(c);
        const int y = 36 + c * 42;
        const Rgb col = chColor(c);
        const bool selc = c == sel_;
        g.color(selc && fG ? mix(theme_.panel, col, 0.18f) : theme_.panel);
        g.fill(8, y, 592, 38);
        g.color(col);
        g.textf(16, y + 11, font::kSmall, Canvas::kCenterY, "CH %d", c + 1);
        const bool recThis = rec_->state() != GestureRecorder::kIdle && rec_->cloud() == c;
        g.color(recThis ? kRed : (st_[c].gestOn ? theme_.fg : theme_.dim));
        if (recThis) g.text(16, y + 28, font::kTiny, rec_->state() == GestureRecorder::kRecording ? "REC" : "ARMED", Canvas::kCenterY);
        else if (ge.len) g.textf(16, y + 28, font::kTiny, Canvas::kCenterY, "%.1fs %s", double(float(ge.len) / kSr), st_[c].gestOn ? "" : "off");
        else g.text(16, y + 28, font::kTiny, "empty", Canvas::kCenterY);
        if (!ge.len) continue;
        const float x0 = 76.0f, wl = 516.0f;
        const int maxEv = selc ? 160 : 60;
        if (g.room(maxEv * 2 + 40)) {
            g.color(mix(theme_.bg, col, st_[c].gestOn ? 0.9f : 0.4f));
            g.beginLines();
            int shown = 0;
            for (int i = 0; i < ge.count && shown < maxEv; ++i) {
                const GestEvent& e = ge.ev[i];
                if (e.type != kGPadDown) continue;
                const float ex = x0 + wl * float(e.t) / float(ge.len);
                const float top = float(y + 4 + (e.a / 12) * 6);
                g.vtx(ex, top);
                g.vtx(ex, top + 7.0f);
                ++shown;
            }
            g.endBatch();
        }
        if (g.room(maxEv + 40)) {
            g.color(theme_.accent2);
            g.pointSize(1.5f);
            g.beginPoints();
            int shown = 0;
            for (int i = 0; i < ge.count && shown < maxEv; ++i) {
                const GestEvent& e = ge.ev[i];
                if (e.type != kGParam) continue;
                g.vtx(x0 + wl * float(e.t) / float(ge.len), float(y + 35) - float(e.v) * (1.0f / 65535.0f) * 9.0f);
                ++shown;
            }
            g.endBatch();
        }
        const float ph = eng_->rt[c].gphase / float(ge.len);
        g.color(gestFlash_[c] > 0.3f ? kWhite : col);
        g.fill(int(x0 + wl * (ph > 1.0f ? 1.0f : ph)), y + 2, 2, 34);
    }
    // options of the selected channel's gesture
    const Gesture& ge = eng_->gestLive(sel_);
    const Rgb col = chColor(sel_);
    static const char* const kQ[kQuants] = {"FREE", "BEAT", "BAR"};
    static const char* const kSpd[kSpeeds] = {"1/4 x", "1/2 x", "1 x", "2 x", "4 x"};
    const char* names[4] = {"PLAY", "SPEED", "LENGTH", "REC MODE"};
    char vals[4][16];
    std::snprintf(vals[0], 16, "%s", st_[sel_].gestOn ? "ON" : "OFF");
    std::snprintf(vals[1], 16, "%s", kSpd[int(par_[sel_].gestSpeed + 0.5f) % kSpeeds]);
    std::snprintf(vals[2], 16, "%s", kQ[ge.quant < kQuants ? ge.quant : 0]);
    std::snprintf(vals[3], 16, "%s", gestMode_ ? "OVERDUB" : "NEW");
    for (int i = 0; i < 4; ++i) {
        const int x = 606 + (i % 2) * 94, y = 36 + (i / 2) * 84;
        const bool cur = fG && (cur_[kPgMotion] & 3) == i;
        g.color(cur ? mix(theme_.panel, col, 0.3f) : theme_.panel);
        g.fill(x, y, 90, 80);
        g.color(cur ? col : theme_.dim);
        g.text(x + 8, y + 14, font::kTiny, names[i], Canvas::kCenterY);
        g.color(cur ? kWhite : theme_.fg);
        g.text(x + 8, y + 44, font::kSmall, vals[i], Canvas::kCenterY);
        if (i == 0) {
            g.color(theme_.dim);
            g.textf(x + 8, y + 66, font::kTiny, Canvas::kCenterY, "%u moves", unsigned(ge.count));
        }
    }
    // scenes
    const bool fS = motionFocus_ == 1;
    g.color(fS ? theme_.fg : theme_.dim);
    char mt[16];
    ctx_->params().format(mid_[kMMorph], mt, sizeof mt);
    g.textf(8, 214, font::kTiny, Canvas::kCenterY, "SCENES   morph %s (Shift+encoder)   Shift + pad row 2 anywhere", mt);
    for (int i = 0; i < kScenes; ++i) {
        const int x = 8 + i * 98, y = 222;
        const Scene& sc = scenes_[i];
        const bool cur = i == scnCur_ && fS;
        g.color(cur ? mix(theme_.panel, theme_.accent2, 0.3f) : theme_.panel);
        g.fill(x, y, 94, 92);
        if (i == curScene_) {
            g.color(theme_.accent2);
            g.fill(x, y, 94, 3);
        }
        g.color(sc.used ? theme_.fg : theme_.dim);
        g.textf(x + 8, y + 16, font::kBody, Canvas::kCenterY, "%d", i + 1);
        if (!sc.used) {
            g.color(theme_.dim);
            g.text(x + 47, y + 56, font::kTiny, "empty", Canvas::kCenter);
            continue;
        }
        // four dots: size = level, ring = frozen, dim = muted
        for (int c = 0; c < kCh; ++c) {
            const float px = float(x + 18 + (c % 2) * 40), py = float(y + 44 + (c / 2) * 28);
            const float lvl = sc.norm[c][CP::Level];
            g.color(sc.st[c].mute ? theme_.dim : chColor(c));
            g.point(px, py, 2.0f + 8.0f * lvl);
            if (sc.st[c].freeze) {
                g.color(kLilac);
                g.ring(px, py, 11.0f, 12);
            }
        }
        if (morphOn_ && i == curScene_) {
            g.color(theme_.accent2);
            g.fill(x, y + 88, int(94.0f * (morphDur_ > 0.0f ? morphT_ / morphDur_ : 1.0f)), 4);
        }
    }
    // timeline
    const bool fT = motionFocus_ == 2;
    g.color(fT ? theme_.fg : theme_.dim);
    g.textf(8, 326, font::kTiny, Canvas::kCenterY, "TIMELINE  %s", songMode_ ? "SONG MODE: Play runs it" : "song mode off (Shift+F6 / Shift+Song)");
    g.color(theme_.panel);
    g.fill(8, 334, 784, 76);
    int totalBars = 0;
    for (int i = 0; i < tlLen_; ++i) totalBars += timeline_[i].bars;
    if (tlLen_ == 0) {
        g.color(theme_.dim);
        g.text(400, 372, font::kSmall, "Empty: Insert adds the scene, Shift+F7 appends", Canvas::kCenter);
    } else {
        const float sc = 776.0f / float(totalBars > 16 ? totalBars : 16);
        float x = 12.0f;
        for (int i = 0; i < tlLen_; ++i) {
            const TimeEntry& te = timeline_[i];
            const float w = float(te.bars) * sc;
            const Rgb hue = chColor(te.scene % kCh);
            const bool cur = i == tlCur_ && fT;
            g.color(i == tlPlaying_ ? hue : mix(theme_.panel, hue, cur ? 0.55f : 0.3f));
            g.fill(int(x), 338, int(w) - 3, 68);
            if (cur) {
                g.color(kWhite);
                g.frame(int(x), 338, int(w) - 3, 68);
            }
            if (w > 30.0f) {
                g.color(i == tlPlaying_ ? theme_.bg : theme_.fg);
                g.textf(int(x) + 6, 352, font::kSmall, Canvas::kCenterY, "S%d", te.scene + 1);
                g.textf(int(x) + 6, 374, font::kTiny, Canvas::kCenterY, "%d bar%s", te.bars, te.bars > 1 ? "s" : "");
            }
            x += w;
        }
        if (songMode_ && ctx_->playing() && totalBars > 0) {
            const double b = eng_->beat;
            const double tot = double(totalBars) * 4.0;
            const double pos = b - tot * double(int64_t(b / tot));
            g.color(kWhite);
            g.fill(12 + int(float(pos / 4.0) * sc), 336, 2, 72);
        }
    }
    g.color(theme_.dim);
    g.text(8, 424, font::kTiny, "Up/Down: gestures / scenes / timeline   Left/Right: select   Insert: rec | store-morph | add",
           Canvas::kCenterY);
    g.text(8, 440, font::kTiny, "Rec key: arm the selected channel, first move starts, Rec again closes the loop   Delete: clear",
           Canvas::kCenterY);
}

// ---- FREEZE: the library of frozen moments ---------------------------------------------------------
void Bulut::drawPageFreeze(Canvas& g) {
    const FreezeSlot& f = frz_[frzCur_];
    g.color(theme_.panel);
    g.fill(8, 36, 560, 170);
    if (f.used) {
        // the slot after the spectral tools (recomputed when anything changes)
        const int ms = tools_.morphSlot;
        const FreezeSlot* morph = (ms >= 0 && ms < kFreezeSlots && frz_[ms].used && ms != frzCur_) ? &frz_[ms] : nullptr;
        const float key = tools_.blur * 3.1f + tools_.shift * 7.3f + tools_.tilt * 11.7f + tools_.sparse * 13.1f +
                          tools_.morph * 17.9f + float(ms) * 19.3f + float(frzCur_) * 23.7f + (morph ? 0.5f : 0.0f);
        if (specViewSlot_ != frzCur_ || key != specViewKey_) {
            spec_.shape(f, morph, tools_, specView_);
            float mx = 1e-9f;
            for (int b = 0; b < kSpecBins; ++b) mx = specView_[b] > mx ? specView_[b] : mx;
            for (int b = 0; b < kSpecBins; ++b) specView_[b] /= mx;
            specViewSlot_ = frzCur_;
            specViewKey_ = key;
        }
        // raw: an outline; shaped: filled
        g.color(mix(theme_.bg, kLilac, 0.4f));
        g.beginLines();
        for (int i = 0; i < 128; ++i) {
            const float u = float(i) / 127.0f;
            const float x = 14.0f + u * 548.0f;
            g.vtx(x, 200.0f);
            g.vtx(x, 200.0f - specAt(specView_, u) * 150.0f);
        }
        g.endBatch();
        g.color(theme_.dim);
        g.beginStrip();
        for (int i = 0; i < 64; ++i) {
            const float u = float(i) / 63.0f;
            g.vtx(14.0f + u * 548.0f, 200.0f - specAt(f, u) * 150.0f);
        }
        g.endBatch();
        g.color(theme_.dim);
        g.textf(560, 48, font::kTiny, Canvas::kCenterY | Canvas::kRight, "from CH %d %s   outline = raw, filled = tools", f.cloud + 1,
                f.from == kFromMix ? "(MIX)" : "(SOURCE)");
    } else {
        g.color(theme_.dim);
        g.text(288, 112, font::kBody, "Empty slot", Canvas::kCenter);
        g.text(288, 138, font::kTiny, "F8 or Insert captures a frozen moment of the channel here", Canvas::kCenter);
    }
    g.color(theme_.fg);
    g.textf(16, 48, font::kSmall, Canvas::kCenterY, "SLOT %d", frzCur_ + 1);
    // the selected channel's freeze
    const int c = sel_;
    const Rgb col = chColor(c);
    g.color(theme_.panel);
    g.fill(576, 36, 216, 170);
    g.color(col);
    g.textf(588, 52, font::kSmall, Canvas::kCenterY, "CH %d", c + 1);
    const bool fz = st_[c].freeze && eng_->rt[c].frzReady;
    g.color(fz ? kLilac : theme_.dim);
    g.text(588, 80, font::kBody, fz ? "FROZEN" : "LIVE", Canvas::kCenterY);
    g.color(theme_.dim);
    if (st_[c].freezeSlot < kFreezeSlots) g.textf(588, 102, font::kTiny, Canvas::kCenterY, "loop of slot %d", st_[c].freezeSlot + 1);
    g.text(588, 128, font::kTiny, "CAPTURE FROM (Shift+F3)", Canvas::kCenterY);
    g.color(theme_.fg);
    g.text(588, 146, font::kSmall, frzFrom_ == kFromMix ? "THE MIX" : "CHANNEL SOURCE", Canvas::kCenterY);
    g.color(theme_.dim);
    g.text(588, 176, font::kTiny, "FX1: freeze on/off", Canvas::kCenterY);
    g.text(588, 192, font::kTiny, "Shift+pads row 3: slots", Canvas::kCenterY);
    // library: 2 rows of 8
    for (int i = 0; i < kFreezeSlots; ++i) {
        const int x = 8 + (i % 8) * 98, y = 212 + (i / 8) * 86;
        const FreezeSlot& fs = frz_[i];
        const bool cur = i == frzCur_ && frzFocus_ == 0;
        g.color(cur ? mix(theme_.panel, kLilac, 0.3f) : theme_.panel);
        g.fill(x, y, 94, 82);
        if (fs.used && g.room(40)) {
            g.color(cur ? kWhite : mix(kLilac, chColor(fs.cloud), 0.4f));
            g.beginStrip();
            for (int k = 0; k < 22; ++k) {
                const float u = float(k) / 21.0f;
                g.vtx(float(x + 4) + u * 86.0f, float(y + 74) - specAt(fs, u) * 54.0f);
            }
            g.endBatch();
        }
        g.color(fs.used ? theme_.fg : theme_.dim);
        g.textf(x + 6, y + 11, font::kTiny, Canvas::kCenterY, "%d", i + 1);
        for (int ch = 0; ch < kCh; ++ch)
            if (st_[ch].freeze && st_[ch].freezeSlot == i) {
                g.color(chColor(ch));
                g.point(float(x + 84 - ch * 10), float(y + 11), 3.5f);
            }
    }
    // tools row
    static const char* const kTool[7] = {"BED", "BLUR", "SHIFT", "TILT", "SPARSE", "MORPH", "M.SLOT"};
    for (int i = 0; i < 7; ++i) {
        const int x = 8 + i * 112, y = 386;
        const bool cur = frzFocus_ == 1 && frzTool_ == i;
        g.color(cur ? mix(theme_.panel, kLilac, 0.35f) : theme_.panel);
        g.fill(x, y, 108, 60);
        g.color(cur ? kLilac : theme_.dim);
        g.text(x + 8, y + 12, font::kTiny, kTool[i], Canvas::kCenterY);
        char t[16];
        float v;
        if (i == 0) {
            paramText(c, CP::Bed, t, sizeof t);
            v = ctx_->params().norm(pid_[c][CP::Bed]);
        } else {
            ctx_->params().format(tid_[i - 1], t, sizeof t);
            v = ctx_->params().norm(tid_[i - 1]);
            if (i == 6) std::snprintf(t, sizeof t, "%d", tools_.morphSlot + 1);
        }
        g.color(cur ? kWhite : theme_.fg);
        g.text(x + 8, y + 32, font::kSmall, t, Canvas::kCenterY);
        g.color(mix(theme_.bg, kLilac, 0.25f));
        g.fill(x + 8, y + 48, 92, 3);
        g.color(kLilac);
        g.fill(x + 8, y + 48, int(92.0f * v), 3);
    }
}

// ---- FILE: projects + resample --------------------------------------------------------------------
void Bulut::drawPageFile(Canvas& g) {
    g.color(theme_.dim);
    g.text(16, 48, font::kSmall, "PROJECTS", Canvas::kCenterY);
    g.text(530, 48, font::kTiny, "F8 save  Insert load", Canvas::kCenterY | Canvas::kRight);
    if (!ctx_->fs) {
        g.color(kRed);
        g.text(270, 200, font::kBody, "No SD card", Canvas::kCenter);
    }
    for (int i = 0; i < kProjects; ++i) {
        const int x = 8 + (i / 8) * 262, y = 62 + (i % 8) * 47;
        const bool cur = i == fileCur_ && fileFocus_ == 0;
        g.color(cur ? mix(theme_.panel, theme_.accent, 0.3f) : theme_.panel);
        g.fill(x, y, 256, 43);
        g.color(projUsed_[i] ? theme_.fg : theme_.dim);
        g.textf(x + 12, y + 21, font::kBody, Canvas::kCenterY, "P%02d", i + 1);
        g.color(projUsed_[i] ? theme_.accent : theme_.dim);
        g.text(x + 244, y + 21, font::kTiny, projUsed_[i] ? "saved" : "empty", Canvas::kCenterY | Canvas::kRight);
    }
    // resample loop
    const bool foc = fileFocus_ == 1;
    const int x = 540;
    g.color(foc ? mix(theme_.panel, theme_.accent2, 0.25f) : theme_.panel);
    g.fill(x, 36, 252, 410);
    g.color(theme_.accent2);
    g.text(x + 12, 52, font::kSmall, "RESAMPLE", Canvas::kCenterY);
    g.color(theme_.dim);
    g.text(x + 12, 80, font::kTiny, "RECORD (encoder)", Canvas::kCenterY);
    g.color(rsSrc_ < 0 ? theme_.fg : chColor(rsSrc_));
    if (rsSrc_ < 0) g.text(x + 12, 100, font::kBody, "THE MIX", Canvas::kCenterY);
    else g.textf(x + 12, 100, font::kBody, Canvas::kCenterY, "CH %d (post FX)", rsSrc_ + 1);
    g.color(theme_.dim);
    g.text(x + 12, 128, font::kTiny, "INTO (Shift + encoder)", Canvas::kCenterY);
    g.color(chColor(rsDst_));
    g.textf(x + 12, 148, font::kBody, Canvas::kCenterY, "CH %d", rsDst_ + 1);
    const float secs = float(eng_->capLen) / kSr;
    g.color(rsState_ == 1 ? kRed : theme_.dim);
    g.text(x + 12, 182, font::kSmall, rsState_ == 1 ? "RECORDING" : (rsState_ == 2 ? "WRITING..." : "READY"), Canvas::kCenterY);
    if (rsState_ == 1) {
        g.color(mix(theme_.bg, kRed, 0.3f));
        g.fill(x + 12, 198, 228, 6);
        g.color(kRed);
        g.fill(x + 12, 198, int(228.0f * float(eng_->capLen) / float(kBufLen)), 6);
        g.color(theme_.fg);
        g.textf(x + 12, 218, font::kSmall, Canvas::kCenterY, "%.1f / %.1f s", double(secs), double(float(kBufLen) / kSr));
    }
    if (rsLabel_[0]) {
        g.color(theme_.dim);
        g.text(x + 12, 246, font::kTiny, "LAST", Canvas::kCenterY);
        g.color(theme_.fg);
        g.text(x + 12, 262, font::kTiny, rsLabel_, Canvas::kCenterY);
    }
    g.color(theme_.dim);
    g.text(x + 12, 300, font::kTiny, "S.Rec (any page) or Insert here:", Canvas::kCenterY);
    g.text(x + 12, 316, font::kTiny, "start / stop. The take is saved", Canvas::kCenterY);
    g.text(x + 12, 332, font::kTiny, "in Resample/ as MIX-001.wav etc.", Canvas::kCenterY);
    g.text(x + 12, 348, font::kTiny, "and loads straight into the", Canvas::kCenterY);
    g.text(x + 12, 364, font::kTiny, "target channel as its sample.", Canvas::kCenterY);
    g.text(x + 12, 396, font::kTiny, "Down from P08 / P16: this panel", Canvas::kCenterY);
}

// ---- pad LEDs ------------------------------------------------------------------------------
void Bulut::leds(Rgb* pads) {
    static const float kLvl[3] = {0.3f, 0.6f, 1.0f};
    const float k = kLvl[setLeds_ < 0 ? 0 : (setLeds_ > 2 ? 2 : setLeds_)];
    for (int i = 0; i < lab::kPads; ++i) pads[i] = Rgb();
    if (shift_) {
        for (int c = 0; c < kCh; ++c) {
            const Rgb col = chColor(c);
            pads[c] = scale(col, (c == sel_ ? 1.0f : 0.2f) * k);
            pads[4 + c] = st_[c].mute ? scale(kRed, 0.7f * k) : scale(col, 0.12f * k);
            pads[8 + c] = (eng_->soloMask >> c) & 1 ? scale(theme_.ok, k) : scale(col, 0.12f * k);
            const bool rec = rec_->state() != GestureRecorder::kIdle && rec_->cloud() == c;
            pads[20 + c] = rec ? scale(kRed, k) : scale(kRed, 0.15f * k);
            pads[36 + c] = st_[c].freeze ? scale(kLilac, k) : scale(kLilac, 0.12f * k);
            pads[40 + c] = st_[c].drone ? scale(col, k) : scale(col, 0.12f * k);
            pads[44 + c] = st_[c].gestOn && eng_->gestLive(c).len ? scale(theme_.accent2, k) : scale(theme_.accent2, 0.08f * k);
        }
        for (int s = 0; s < kScenes; ++s)
            pads[12 + s] = scenes_[s].used ? scale(theme_.accent2, (s == curScene_ ? 1.0f : 0.35f) * k) : scale(theme_.dim, 0.08f * k);
        for (int f = 0; f < 12; ++f)
            pads[24 + f] = frz_[f].used ? scale(kLilac, (st_[sel_].freezeSlot == f ? 1.0f : 0.35f) * k) : scale(theme_.dim, 0.06f * k);
        return;
    }
    const int c = sel_;
    const Rgb col = chColor(c);
    if (page_ == kPgSource && regionMode_) {
        const float a0 = par_[c].regStart < par_[c].regEnd ? par_[c].regStart : par_[c].regEnd;
        const float a1 = par_[c].regStart < par_[c].regEnd ? par_[c].regEnd : par_[c].regStart;
        for (int p = 0; p < lab::kPads; ++p) {
            const float u = (float(p) + 0.5f) / 48.0f;
            const float w = float(wave_[c][p * 2]) / 255.0f;
            pads[p] = scale(u >= a0 && u <= a1 ? col : theme_.dim, (0.08f + 0.5f * sqrtf(w)) * k);
        }
        if (regionFirst_ >= 0) pads[regionFirst_] = scale(theme_.accent2, k);
        return;
    }
    const bool synth = par_[c].engine == kQuanta && enumIndex(par_[c].k[K::Mode], kQTypes) != kQSample;
    const bool recording = rec_->state() != GestureRecorder::kIdle && rec_->cloud() == c;
    const Rgb base0 = st_[c].freeze ? mix(col, kLilac, 0.6f) : col;
    for (int p = 0; p < lab::kPads; ++p) {
        const int col12 = p % 12, row = p / 12;
        float b = synth ? 0.10f + 0.012f * float(col12) : 0.05f + 0.30f * float(colPeak_[c][col12]) / 255.0f;
        b *= 1.0f - 0.12f * float(row);
        Rgb x = scale(base0, b);
        x = mix(x, base0, colGlow_[c][col12] * 0.8f);
        if (recording) x = mix(x, kRed, 0.18f);
        pads[p] = scale(x, k);
    }
    const ChRt& r = eng_->rt[c];
    for (int i = 0; i < kEmitters; ++i) {
        const Emitter& e = r.em[i];
        if (!e.on || e.pad >= lab::kPads) continue;
        const Rgb hi = i < kLiveEmitters ? (recording ? kRed : mix(col, kWhite, 0.7f)) : theme_.accent2;
        pads[e.pad] = scale(hi, k);
    }
}

}  // namespace bulut
