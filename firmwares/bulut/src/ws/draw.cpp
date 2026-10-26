// BULUT drawing: intro, pages, particles, LEDs (bulut_ws.h).
//
// Display-list budget: a frame stays under kLimit words, leaving room for the
// Shell's REC badge and toast (~110 words) under Canvas::kBudgetWords. The
// particle renderer gets whatever is left and never asks for more.
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

namespace {
constexpr uint32_t kLimit = lab::Canvas::kBudgetWords - 120;

const Rgb kCloudCol[kClouds] = {Rgb(0x8EC9F0), Rgb(0xF2A7C0), Rgb(0x9BE3C3), Rgb(0xB9A8F0), Rgb(0xF5C49A),
                                Rgb(0xE8E09A)};
const Rgb kWhite(0xFFFFFF);

const char* const kPageName[kPages] = {"CLOUD", "GRAIN", "SOURCE", "GESTURE", "FREEZE", "MIX", "SCENE", "FILE"};

inline float hash01(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.0f / 16777216.0f);
}

const char* srcShort(int t) { return t == kSrcLiveIn ? "INPUT" : (t == kSrcLiveOut ? "OUTPUT" : "SAMPLE"); }

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
}  // namespace

Rgb Bulut::cloudColor(int c) const { return kCloudCol[c < 0 ? 0 : (c >= kClouds ? kClouds - 1 : c)]; }

// ---- intro: a drifting cloud of particles condenses into the name ----------------
void Bulut::drawIntro(Canvas& g, uint32_t t) {
    const float s = float(t) * 0.001f;
    // haze
    g.beginPoints();
    for (int i = 0; i < 9; ++i) {
        const float a = 0.25f * smooth(s / 0.8f) * (1.0f - 0.5f * smooth((s - 2.0f) / 0.8f));
        g.color(mix(theme_.bg, kCloudCol[i % kClouds], 0.5f));
        g.alpha(uint8_t(26.0f * a * 4.0f));
        g.pointSize(90.0f + 30.0f * hash01(uint32_t(i) * 31u));
        g.vtx(400.0f + 260.0f * (hash01(uint32_t(i) * 7u + 1u) - 0.5f) + 30.0f * sinf(s * 0.7f + float(i)),
              240.0f + 120.0f * (hash01(uint32_t(i) * 13u + 5u) - 0.5f));
    }
    g.endBatch();
    // targets: lit cells of the 5 x 7 letters, 3 particles each
    constexpr float kCell = 17.0f;
    const float x0 = 400.0f - (29.0f * kCell) * 0.5f + kCell * 0.5f, y0 = 205.0f - 3.0f * kCell;
    const float gather = smooth((s - 0.6f) / 1.3f);
    const float settle = smooth((s - 1.9f) / 0.5f);
    g.beginPoints();
    int n = 0;
    int lastBucket = -1;
    for (int pass = 0; pass < 3; ++pass) {
        for (int L = 0; L < 5; ++L)
            for (int r = 0; r < 7; ++r)
                for (int c = 0; c < 5; ++c) {
                    if (kGlyph[kWord[L]][r][c] != '1') continue;
                    const uint32_t id = uint32_t(n++);
                    const float tx = x0 + float(L * 6 + c) * kCell + (hash01(id * 3u + 1u) - 0.5f) * 7.0f * (1.0f - settle);
                    const float ty = y0 + float(r) * kCell + (hash01(id * 5u + 2u) - 0.5f) * 7.0f * (1.0f - settle);
                    // start: a wide swirling cloud
                    const float ang = hash01(id * 11u + 3u) * 6.2832f + s * (0.6f + hash01(id) * 0.8f);
                    const float rad = 60.0f + 230.0f * hash01(id * 17u + 4u);
                    const float sx = 400.0f + cosf(ang) * rad * 1.3f, sy = 240.0f + sinf(ang) * rad * 0.55f;
                    const float k = smooth(gather * (0.75f + 0.5f * hash01(id * 23u)) - 0.1f);
                    const float x = sx + (tx - sx) * k, y = sy + (ty - sy) * k;
                    const int bucket = pass;
                    if (bucket != lastBucket) {
                        lastBucket = bucket;
                        const Rgb col = pass == 0 ? kCloudCol[0] : (pass == 1 ? kCloudCol[3] : kWhite);
                        g.color(mix(col, kWhite, settle * 0.6f));
                        g.alpha(uint8_t(pass == 2 ? 230 : 150));
                        g.pointSize(pass == 0 ? 6.0f - 2.0f * settle : (pass == 1 ? 3.5f : 2.0f + 2.5f * settle));
                    }
                    g.vtx(x, y);
                }
    }
    // loose particles that keep drifting around the word
    g.color(kCloudCol[1]);
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
        g.text(400, 330, font::kBody, "granular clouds  -  bulut", Canvas::kCenter);
        g.color(mix(theme_.bg, theme_.accent, tag));
        g.text(400, 356, font::kTiny, "INPUT  *  RADIO  *  OUTPUT  *  SAMPLES", Canvas::kCenter);
    }
}

// ---- shared pieces ----------------------------------------------------------------
void Bulut::drawFooter(Canvas& g) {
    const char* keys[8];
    if (!shift_) {
        for (int i = 0; i < 8; ++i) keys[i] = kPageName[i];
        lab::drawFKeyBar(g, theme_, keys, page_);
        return;
    }
    static const char* const kAct[kPages][8] = {
        {"ENGINE", "DRONE", "FREEZE", "GEST REC", "MUTE", "SOLO", "RESET", "RANDOM"},
        {"ENGINE", "DRONE", "FREEZE", "GEST REC", "MUTE", "SOLO", "RESET", "RANDOM"},
        {"INPUT", "OUTPUT", "SAMPLE", "HOLD", "SEEK -", "SEEK +", "CLEAR", "LOAD"},
        {"REC/STOP", "NEW/ODUB", "CLEAR", "PLAY", "SLOWER", "FASTER", "ALL ON", "ALL OFF"},
        {"CAPTURE", "RECALL", "FREEZE", "DELETE", "FROM", "UNFREEZE", "", ""},
        {"MUTE", "SOLO", "UNSOLO", "", "", "", "", ""},
        {"STORE", "MORPH TO", "DELETE", "APPEND", "REMOVE", "SONG", "CUT TO", ""},
        {"SAVE", "LOAD", "NEW", "RESAMPLE", "", "", "", ""},
    };
    for (int i = 0; i < 8; ++i) keys[i] = kAct[page_][i];
    lab::drawFKeyBar(g, theme_, keys, -1);
    g.color(theme_.accent2);
    g.fill(0, lab::kScreenH - 31, lab::kScreenW, 1);
}

void Bulut::drawField(Canvas& g, int x, int y, int w, int h, int c, bool big) {
    const Rgb col = cloudColor(c);
    const CloudState& s = clouds_[c];
    const bool synth = s.engine() == kQuanta && enumIndex(s.p[1][P::QType], kQTypes) != kQSample;
    const bool frz = s.freeze && eng_->rt[c].frzReady;
    g.color(mix(theme_.bg, col, 0.05f));
    g.fill(x, y, w, h);
    // density rows (top = densest)
    g.color(mix(theme_.bg, col, 0.10f));
    g.beginRects();
    for (int r = 0; r < 4; r += 2) {
        g.vtx(float(x), float(y + r * h / 4));
        g.vtx(float(x + w - 1), float(y + (r + 1) * h / 4 - 1));
    }
    g.endBatch();
    // what the pads read: waveform overview (or the freeze loop / a pitch ruler)
    if (!synth) {
        g.color(mix(theme_.bg, frz ? Rgb(0xB9A8F0) : col, big ? 0.38f : 0.3f));
        g.beginLines();
        const float cy = float(y) + float(h) * 0.5f;
        for (int i = 0; i < 64; ++i) {
            const float hx = float(x) + (float(i) + 0.5f) * float(w) / 64.0f;
            const float a = frz ? 0.35f + 0.15f * sinf(float(i) * 0.7f) : float(wave_[c][i]) / 255.0f;
            const float hh = (0.04f + 0.92f * sqrtf(a)) * float(h) * 0.5f;
            g.vtx(hx, cy - hh);
            g.vtx(hx, cy + hh);
        }
        g.endBatch();
    }
    // grain glow per pad column
    g.color(col);
    g.beginRects();
    for (int k = 0; k < 12; ++k) {
        const float gl = colGlow_[c][k];
        if (gl < 0.04f) continue;
        g.alpha(uint8_t(gl * 70.0f));
        g.vtx(float(x + k * w / 12), float(y));
        g.vtx(float(x + (k + 1) * w / 12 - 1), float(y + h - 1));
    }
    g.endBatch();
    g.alpha(255);
    // pad grid lines
    g.color(mix(theme_.bg, col, 0.22f));
    g.beginLines();
    for (int k = 1; k < 12; ++k) {
        const float lx = float(x + k * w / 12);
        g.vtx(lx, float(y));
        g.vtx(lx, float(y + h));
    }
    for (int r = 1; r < 4 && big; ++r) {
        g.vtx(float(x), float(y + r * h / 4));
        g.vtx(float(x + w), float(y + r * h / 4));
    }
    g.endBatch();
    if (!big) return;
    // held pads and gesture ghosts
    const CloudRt& rt = eng_->rt[c];
    for (int i = 0; i < kEmitters; ++i) {
        const Emitter& e = rt.em[i];
        if (!e.on || e.pad == 0xFF) continue;
        const int px = x + (e.pad % 12) * w / 12, py = y + (e.pad / 12) * h / 4;
        g.color(i < kLiveEmitters ? kWhite : theme_.accent2);
        g.frame(px + 1, py + 1, w / 12 - 2, h / 4 - 2);
    }
    // the position parameter when no pad is held (drone / MIDI)
    const float pos = s.engine() == kGranular ? s.p[0][P::Pos] : s.p[1][P::QPos];
    if (!synth) {
        g.color(scale(col, 0.8f));
        g.fill(x + int(pos * float(w - 2)), y, 2, h);
    }
    g.color(theme_.dim);
    if (synth) {
        g.text(x + 6, y + h - 10, font::kTiny, "LOW", Canvas::kCenterY);
        g.text(x + w - 6, y + h - 10, font::kTiny, "HIGH  (column = pitch step)", Canvas::kCenterY | Canvas::kRight);
    } else if (frz) {
        g.text(x + 6, y + h - 10, font::kTiny, "FROZEN MOMENT", Canvas::kCenterY);
    } else if (sources_[c].type == kSrcSample) {
        g.text(x + 6, y + h - 10, font::kTiny, "START", Canvas::kCenterY);
        g.text(x + w - 6, y + h - 10, font::kTiny, "END", Canvas::kCenterY | Canvas::kRight);
    } else {
        g.text(x + 6, y + h - 10, font::kTiny, "-8 s", Canvas::kCenterY);
        g.text(x + w - 6, y + h - 10, font::kTiny, "NOW", Canvas::kCenterY | Canvas::kRight);
    }
}

// Particles of one cloud, bucketed by colour / size / age so the state words stay few.
void Bulut::drawParticles(Canvas& g, int x, int y, int w, int h, int c, bool big, int maxWords) {
    const Rgb col = cloudColor(c);
    const uint32_t limit = uint32_t(maxWords) < kLimit ? uint32_t(maxWords) : kLimit;
    if (g.words() + 40 > limit) return;
    g.clip(x, y, w, h);
    const float sizes[3] = {big ? 1.4f : 1.1f, big ? 2.4f : 1.7f, big ? 3.8f : 2.6f};
    // halos under the biggest live grains
    g.beginPoints();
    g.color(col);
    g.alpha(38);
    g.pointSize(big ? 11.0f : 7.0f);
    int halos = 0;
    for (int i = 0; i < kParticles && halos < 48; ++i) {
        const Particle& p = part_[i];
        if (p.life <= 0.0f || p.cloud != c || p.size < 2 || p.age > p.life * 0.5f) continue;
        if (g.words() + 30 > limit) break;
        g.vtx(float(x) + p.x * float(w), float(y) + p.y * float(h));
        ++halos;
    }
    for (int bright = 0; bright < 2; ++bright) {
        g.color(bright ? mix(col, kWhite, 0.55f) : col);
        for (int sz = 0; sz < 3; ++sz) {
            g.pointSize(sizes[sz]);
            for (int ab = 0; ab < 4; ++ab) {
                bool first = true;
                for (int i = 0; i < kParticles; ++i) {
                    const Particle& p = part_[i];
                    if (p.life <= 0.0f || p.cloud != c || p.size != sz || p.live != bright) continue;
                    const int b = int((p.age / p.life) * 4.0f);
                    if ((b > 3 ? 3 : b) != ab) continue;
                    if (g.words() + 6 > limit) break;
                    if (first) {
                        static const uint8_t kA[4] = {235, 170, 105, 50};
                        g.alpha(kA[ab]);
                        first = false;
                    }
                    g.vtx(float(x) + p.x * float(w), float(y) + p.y * float(h));
                }
            }
        }
    }
    g.endBatch();
    g.alpha(255);
    g.noClip();
}

void Bulut::drawCloudCards(Canvas& g, int x, int y, int w, int h) {
    const int ch = h / kClouds;
    for (int c = 0; c < kClouds; ++c) {
        const int cy = y + c * ch;
        const CloudState& s = clouds_[c];
        const CloudRt& r = eng_->rt[c];
        const Rgb col = cloudColor(c);
        const bool selc = c == sel_;
        g.color(selc ? mix(theme_.panel, col, 0.18f) : theme_.panel);
        g.fill(x, cy, w, ch - 4);
        if (selc) {
            g.color(col);
            g.fill(x, cy, 3, ch - 4);
        }
        // breathing dot: size follows the cloud's output level
        g.color(s.mute ? theme_.dim : col);
        const float lv = r.peak > 1.0f ? 1.0f : r.peak;
        g.point(float(x + 18), float(cy + 18), 4.0f + 7.0f * sqrtf(lv));
        g.color(selc ? theme_.fg : mix(theme_.fg, theme_.dim, 0.4f));
        g.textf(x + 34, cy + 12, font::kSmall, Canvas::kCenterY, "%d %s", c + 1, s.engine() == kGranular ? "GRAN" : "QUANTA");
        g.color(theme_.dim);
        g.text(x + 34, cy + 30, font::kTiny, srcShort(sources_[c].type), Canvas::kCenterY);
        // flags
        char f[12];
        int n = 0;
        if (s.freeze) f[n++] = 'F';
        if (s.drone) f[n++] = 'D';
        if (eng_->gestLive(c).len && s.gestOn) f[n++] = 'G';
        if (s.mute) f[n++] = 'M';
        f[n] = 0;
        if (n) {
            g.color(gestFlash_[c] > 0.3f ? kWhite : theme_.accent2);
            g.text(x + w - 8, cy + 12, font::kSmall, f, Canvas::kCenterY | Canvas::kRight);
        }
        g.color(theme_.dim);
        g.textf(x + w - 8, cy + 30, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d gr", r.active);
        // level meter
        g.color(mix(theme_.bg, col, 0.3f));
        g.fill(x + 10, cy + ch - 14, w - 20, 4);
        g.color(col);
        g.fill(x + 10, cy + ch - 14, int(float(w - 20) * sqrtf(lv)), 4);
    }
}

void Bulut::drawParamCell(Canvas& g, int x, int y, int w, int slot, bool selected) {
    const CloudState& s = clouds_[sel_];
    const int en = s.engine();
    const float v = s.p[en][slot];
    const Rgb col = cloudColor(sel_);
    if (selected) {
        g.color(mix(theme_.panel, col, 0.25f));
        g.fill(x + 2, y, w - 4, 56);
    }
    g.color(selected ? col : theme_.dim);
    g.text(x + 8, y + 10, font::kTiny, paramName(en, slot), Canvas::kCenterY);
    char t[24];
    paramText(en, slot, v, t, sizeof t);
    g.color(selected ? kWhite : theme_.fg);
    g.text(x + 8, y + 28, font::kBody, t, Canvas::kCenterY);
    g.color(mix(theme_.bg, col, 0.25f));
    g.fill(x + 8, y + 46, w - 16, 4);
    g.color(col);
    g.fill(x + 8, y + 46, int(float(w - 16) * v), 4);
}

// ---- pages ---------------------------------------------------------------------------
void Bulut::draw(Canvas& g) {
    char title[48];
    const CloudState& s = clouds_[sel_];
    std::snprintf(title, sizeof title, "%s  -  cloud %d %s", kPageName[page_], sel_ + 1,
                  s.engine() == kGranular ? "GRANULAR" : "QUANTA");
    lab::drawHeader(g, theme_, *ctx_->shell, title);
    switch (page_) {
        case kPgCloud: drawPageCloud(g); break;
        case kPgGrain: drawPageGrain(g); break;
        case kPgSource: drawPageSource(g); break;
        case kPgGesture: drawPageGesture(g); break;
        case kPgFreeze: drawPageFreeze(g); break;
        case kPgMix: drawPageMix(g); break;
        case kPgScene: drawPageScene(g); break;
        default: drawPageFile(g); break;
    }
    // gesture recording badge
    if (rec_->state() != GestureRecorder::kIdle) {
        const bool on = rec_->state() == GestureRecorder::kRecording;
        g.color(on ? theme_.warn : mix(theme_.panel, theme_.warn, 0.5f));
        g.fillRound(540, 36, 150, 22, 6);
        g.color(kWhite);
        if (on)
            g.textf(615, 47, font::kTiny, Canvas::kCenter, "GESTURE %d  %.1f s", rec_->cloud() + 1,
                    double(float(rec_->elapsed(eng_->sampleClock)) / kSr));
        else
            g.textf(615, 47, font::kTiny, Canvas::kCenter, "ARMED %d: MOVE", rec_->cloud() + 1);
    }
    if (shift_) {
        // what the pads do while Shift is held (the LEDs show the same map)
        g.color(theme_.panel);
        g.fill(0, 404, 800, 46);
        g.color(theme_.accent2);
        g.hline(0, 404, 800);
        g.color(theme_.fg);
        g.text(8, 416, font::kTiny, "SHIFT + PADS   row 1: select cloud 1-6, pad 12 = freeze   row 2: scenes 1-8 (empty = store)",
               Canvas::kCenterY);
        g.text(8, 436, font::kTiny, "row 3: freeze slots 1-12 (empty = capture)   row 4: mute cloud 1-6   Shift + F-key: actions below",
               Canvas::kCenterY);
    }
    drawFooter(g);
}

void Bulut::drawPageCloud(Canvas& g) {
    const int fx = 8, fy = 38, fw = 616, fh = 290;
    drawField(g, fx, fy, fw, fh, sel_, true);
    drawCloudCards(g, 632, 38, 160, 414 - 38 + 6);
    // parameter strip: 8 cells, arrows pick, encoder turns
    g.color(theme_.panel);
    g.fill(0, 334, 628, 112);
    for (int i = 0; i < 8; ++i) {
        const int cx = (i % 4) * 157, cy = 336 + (i / 4) * 56;
        drawParamCell(g, cx, cy, 157, stripParam(i), i == stripCur_);
    }
    drawParticles(g, fx, fy, fw, fh, sel_, true, int(kLimit));
}

void Bulut::drawPageGrain(Canvas& g) {
    const CloudState& s = clouds_[sel_];
    const int en = s.engine();
    const Rgb col = cloudColor(sel_);
    drawField(g, 8, 38, 560, 108, sel_, false);
    // engine picture: the grain window (GRANULAR) or one particle (QUANTA)
    g.color(theme_.panel);
    g.fill(576, 38, 216, 108);
    g.color(col);
    g.lineWidth(1.5f);
    g.beginStrip();
    const int type = enumIndex(s.p[1][P::QType], kQTypes);
    for (int i = 0; i <= 60; ++i) {
        const float u = float(i) / 60.0f;
        float v;
        if (en == kGranular) {
            v = windowValue(enumIndex(s.p[0][P::Texture], kTexWindows), u);
        } else {
            const float ch = s.p[1][P::QChar];
            switch (type) {
                case kQSample: v = windowValue(kWinGabor, u) * (0.5f + 0.5f * sinf(u * 40.0f)); break;
                case kQGlisson: v = windowValue(5, u) * sinf(6.2832f * u * (4.0f + u * (ch - 0.5f) * 16.0f)); break;
                case kQGabor: v = windowValue(6 + enumIndex(1.0f - ch, 10), u) * sinf(6.2832f * u * 6.0f); break;
                case kQPulsar: {
                    const float ph = fmodf(u * 4.0f, 1.0f);
                    const float ratio = 1.0f + ch * 7.0f;
                    v = ph * ratio < 1.0f ? sinf(6.2832f * ph * ratio) * windowValue(2, u) : 0.0f;
                    break;
                }
                default: v = windowValue(kWinPerc, u) * (fmodf(u * 5.0f, 1.0f) < 0.12f ? 1.0f : 0.0f); break;
            }
        }
        g.vtx(584.0f + u * 200.0f, 124.0f - v * 70.0f * (en == kGranular ? 1.0f : 0.5f) - (en == kGranular ? 0.0f : 35.0f));
    }
    g.endBatch();
    g.lineWidth(1.0f);
    g.color(theme_.dim);
    g.text(584, 136, font::kTiny, en == kGranular ? "GRAIN WINDOW (TEXTURE)" : qTypeName(type), Canvas::kCenterY);
    // all 16 parameters
    for (int i = 0; i < 16; ++i) {
        const int cx = 4 + (i % 4) * 198, cy = 154 + (i / 4) * 72;
        drawParamCell(g, cx, cy, 196, pageParam(i), i == grainCur_);
    }
    drawParticles(g, 8, 38, 560, 108, sel_, false, int(kLimit));
}

void Bulut::drawPageSource(Canvas& g) {
    const CloudSource& so = sources_[sel_];
    const CloudRt& r = eng_->rt[sel_];
    const Rgb col = cloudColor(sel_);
    static const char* const kIn[] = {"Line in", "Radio", "Mic", "Off"};
    const int shellIn = ctx_->shell ? ctx_->shell->inputSource() : 0;
    char v[64];
    const char* labels[5] = {"SOURCE", "INPUT LEVEL", "HOLD BUFFER", "RADIO", "SAMPLES"};
    for (int i = 0; i < 5; ++i) {
        const int y = 44 + i * 42;
        if (i == srcCur_) {
            g.color(mix(theme_.panel, col, 0.25f));
            g.fill(8, y, 384, 38);
            g.color(col);
            g.fill(8, y, 3, 38);
        }
        g.color(i == srcCur_ ? col : theme_.dim);
        g.text(20, y + 19, font::kSmall, labels[i], Canvas::kCenterY);
        switch (i) {
            case 0: std::snprintf(v, sizeof v, "%s", so.type == kSrcLiveIn ? "INPUT" : (so.type == kSrcLiveOut ? "BULUT OUTPUT" : "SAMPLE")); break;
            case 1: std::snprintf(v, sizeof v, "%d%%", int(so.inGain * 100.0f + 0.5f)); break;
            case 2: std::snprintf(v, sizeof v, "%s", so.hold ? "ON (frozen)" : "OFF"); break;
            case 3: std::snprintf(v, sizeof v, "%.1f MHz", double(radioMHz_)); break;
            default: {
                int files = 0;
                for (int k = 0; k < browseN_; ++k) files += browse_[k].dir ? 0 : 1;
                std::snprintf(v, sizeof v, "%d WAV  >", files);
                break;
            }
        }
        g.color(theme_.fg);
        g.text(384, y + 19, font::kBody, v, Canvas::kCenterY | Canvas::kRight);
    }
    // where the input comes from
    g.color(theme_.dim);
    g.textf(16, 264, font::kTiny, 0, "Shell input (Config > Input): %s", kIn[shellIn & 3]);
    if (shellIn != 1) g.text(16, 280, font::kTiny, "Choose Radio there to tune stations here.", 0);
    // buffer picture
    g.color(theme_.panel);
    g.fill(8, 300, 384, 146);
    g.color(col);
    g.beginLines();
    for (int i = 0; i < 64; ++i) {
        const float hx = 14.0f + float(i) * 5.9f;
        const float a = float(wave_[sel_][i]) / 255.0f;
        const float hh = 2.0f + 50.0f * sqrtf(a);
        g.vtx(hx, 370.0f - hh);
        g.vtx(hx, 370.0f + hh);
    }
    g.endBatch();
    g.color(theme_.fg);
    if (so.type == kSrcSample) {
        const char* nm = std::strrchr(so.path, '/');
        g.textf(16, 316, font::kTiny, 0, "%.44s  %.2f s", nm ? nm + 1 : (so.path[0] ? so.path : "(resampled, not saved)"),
                double(float(r.sampleLen) / kSr));
    } else {
        g.text(16, 316, font::kTiny, so.type == kSrcLiveIn ? "LIVE INPUT - last 8 s (pads: left old, right now)"
                                                            : "BULUT OUTPUT - feedback of the mix", 0);
    }
    // browser
    g.color(theme_.panel);
    g.fill(400, 40, 392, 406);
    g.color(srcCur_ == 4 ? col : theme_.dim);
    g.textf(410, 56, font::kSmall, Canvas::kCenterY, "/%s", browseDir_);
    if (!ctx_->fs) {
        g.color(theme_.warn);
        g.text(596, 240, font::kBody, "No SD card", Canvas::kCenter);
        return;
    }
    if (browseN_ == 0) {
        g.color(theme_.dim);
        g.text(596, 200, font::kSmall, "No WAV files here.", Canvas::kCenter);
        g.text(596, 222, font::kTiny, "Put samples in /Samples on the card.", Canvas::kCenter);
    }
    const int rows = 14;
    if (browseCur_ < browseTop_) browseTop_ = browseCur_;
    if (browseCur_ >= browseTop_ + rows) browseTop_ = browseCur_ - rows + 1;
    for (int i = 0; i < rows && browseTop_ + i < browseN_; ++i) {
        const int idx = browseTop_ + i;
        const int y = 72 + i * 26;
        const bool cur = idx == browseCur_ && srcCur_ == 4;
        if (cur) {
            g.color(mix(theme_.panel, col, 0.3f));
            g.fill(404, y, 384, 24);
        }
        g.color(browse_[idx].dir ? theme_.accent : (cur ? kWhite : theme_.fg));
        g.textf(414, y + 12, font::kSmall, Canvas::kCenterY, browse_[idx].dir ? "[%s]" : "%s", browse_[idx].name);
    }
}

void Bulut::drawPageGesture(Canvas& g) {
    for (int c = 0; c < kClouds; ++c) {
        const Gesture& ge = eng_->gestLive(c);
        const int y = 40 + c * 44;
        const Rgb col = cloudColor(c);
        const bool selc = c == sel_;
        g.color(selc ? mix(theme_.panel, col, 0.18f) : theme_.panel);
        g.fill(8, y, 784, 40);
        g.color(col);
        g.textf(16, y + 12, font::kSmall, Canvas::kCenterY, "%d", c + 1);
        g.color(clouds_[c].gestOn ? theme_.fg : theme_.dim);
        if (ge.len)
            g.textf(16, y + 30, font::kTiny, Canvas::kCenterY, "%.1fs", double(float(ge.len) / kSr));
        else
            g.text(16, y + 30, font::kTiny, "--", Canvas::kCenterY);
        if (!ge.len) continue;
        // events: pad presses as bars (row -> height), parameter moves as dots
        const float x0 = 64.0f, wl = 716.0f;
        const int maxEv = selc ? 220 : 70;
        g.color(mix(theme_.bg, col, clouds_[c].gestOn ? 0.9f : 0.4f));
        g.beginLines();
        int shown = 0;
        for (int i = 0; i < ge.count && shown < maxEv; ++i) {
            const GestEvent& e = ge.ev[i];
            if (e.type != kGPadDown) continue;
            if (g.words() + 60 > kLimit) break;
            const float ex = x0 + wl * float(e.t) / float(ge.len);
            const float top = float(y + 4 + (e.a / 12) * 7);
            g.vtx(ex, top);
            g.vtx(ex, top + 8.0f);
            ++shown;
        }
        g.endBatch();
        g.color(theme_.accent2);
        g.pointSize(1.5f);
        g.beginPoints();
        shown = 0;
        for (int i = 0; i < ge.count && shown < maxEv; ++i) {
            const GestEvent& e = ge.ev[i];
            if (e.type != kGParam) continue;
            if (g.words() + 60 > kLimit) break;
            g.vtx(x0 + wl * float(e.t) / float(ge.len), float(y + 36) - float(e.v) * (1.0f / 65535.0f) * 8.0f);
            ++shown;
        }
        g.endBatch();
        // playhead
        const float ph = eng_->rt[c].gphase / float(ge.len);
        g.color(gestFlash_[c] > 0.3f ? kWhite : col);
        g.fill(int(x0 + wl * (ph > 1.0f ? 1.0f : ph)), y + 2, 2, 36);
    }
    // options of the selected cloud's gesture
    const Gesture& ge = eng_->gestLive(sel_);
    const Rgb col = cloudColor(sel_);
    static const char* const kSpeed[kSpeeds] = {"1/4 x", "1/2 x", "1 x", "2 x", "4 x"};
    static const char* const kQ[kQuants] = {"FREE", "BEAT", "BAR"};
    const char* names[4] = {"PLAY", "SPEED", "LENGTH", "REC MODE"};
    char vals[4][20];
    std::snprintf(vals[0], 20, "%s", clouds_[sel_].gestOn ? "ON" : "OFF");
    std::snprintf(vals[1], 20, "%s", kSpeed[ge.speed < kSpeeds ? ge.speed : 2]);
    std::snprintf(vals[2], 20, "%s", kQ[ge.quant < kQuants ? ge.quant : 0]);
    std::snprintf(vals[3], 20, "%s", gestMode_ ? "OVERDUB" : "NEW");
    for (int i = 0; i < 4; ++i) {
        const int x = 8 + i * 160, y = 316;
        g.color(i == gestCur_ ? mix(theme_.panel, col, 0.3f) : theme_.panel);
        g.fill(x, y, 154, 56);
        g.color(i == gestCur_ ? col : theme_.dim);
        g.text(x + 10, y + 14, font::kTiny, names[i], Canvas::kCenterY);
        g.color(theme_.fg);
        g.text(x + 10, y + 36, font::kBody, vals[i], Canvas::kCenterY);
    }
    g.color(theme_.panel);
    g.fill(648, 316, 144, 56);
    g.color(theme_.dim);
    g.text(656, 330, font::kTiny, "MOVES", Canvas::kCenterY);
    g.color(theme_.fg);
    g.textf(656, 352, font::kBody, Canvas::kCenterY, "%u", unsigned(ge.count));
    g.color(theme_.dim);
    g.text(16, 392, font::kTiny, "Rec arms; the first pad or knob move starts it; Rec again closes the loop.", 0);
    g.text(16, 410, font::kTiny, "Pads, encoder and MIDI CC moves on this cloud are recorded and loop forever.", 0);
    g.text(16, 428, font::kTiny, "Up/Down: cloud   Left/Right: option   Encoder: change   Delete: clear", 0);
}

void Bulut::drawPageFreeze(Canvas& g) {
    const FreezeSlot& f = frz_[frzCur_];
    const Rgb lil(0xB9A8F0);
    g.color(theme_.panel);
    g.fill(8, 38, 560, 186);
    if (f.used) {
        // spectrum, log frequency (40 Hz .. 16 kHz)
        g.color(mix(theme_.bg, lil, 0.35f));
        g.beginLines();
        for (int i = 0; i < 140; ++i) {
            const float u = float(i) / 139.0f;
            const float hz = 40.0f * expf(u * 5.99f);
            const int b = int(hz * float(Spectral::kAna) / kSr);
            const float m = sqrtf(float(f.mag[b < kSpecBins ? b : kSpecBins - 1]) / 65535.0f);
            const float x = 14.0f + u * 548.0f;
            g.vtx(x, 214.0f);
            g.vtx(x, 214.0f - m * 165.0f);
        }
        g.endBatch();
        g.color(lil);
        g.beginStrip();
        for (int i = 0; i < 140; ++i) {
            const float u = float(i) / 139.0f;
            const float hz = 40.0f * expf(u * 5.99f);
            const int b = int(hz * float(Spectral::kAna) / kSr);
            const float m = sqrtf(float(f.mag[b < kSpecBins ? b : kSpecBins - 1]) / 65535.0f);
            g.vtx(14.0f + u * 548.0f, 214.0f - m * 165.0f);
        }
        g.endBatch();
    } else {
        g.color(theme_.dim);
        g.text(288, 120, font::kBody, "Empty slot", Canvas::kCenter);
        g.text(288, 146, font::kTiny, "Shift+F1 or Push captures a frozen moment here", Canvas::kCenter);
    }
    g.color(theme_.fg);
    g.textf(16, 50, font::kSmall, Canvas::kCenterY, "SLOT %d", frzCur_ + 1);
    if (f.used) {
        g.color(theme_.dim);
        g.textf(560, 50, font::kTiny, Canvas::kCenterY | Canvas::kRight, "from cloud %d %s", f.cloud + 1,
                f.from == kFromMix ? "(MIX)" : "(SOURCE)");
    }
    // the selected cloud's freeze
    const CloudState& s = clouds_[sel_];
    const Rgb col = cloudColor(sel_);
    g.color(theme_.panel);
    g.fill(576, 38, 216, 186);
    g.color(col);
    g.textf(588, 54, font::kSmall, Canvas::kCenterY, "CLOUD %d", sel_ + 1);
    g.color(s.freeze ? lil : theme_.dim);
    g.text(588, 82, font::kBody, s.freeze ? "FROZEN" : "LIVE", Canvas::kCenterY);
    g.color(theme_.dim);
    if (s.freezeSlot < kFreezeSlots) g.textf(588, 106, font::kTiny, Canvas::kCenterY, "loop of slot %d", s.freezeSlot + 1);
    g.text(588, 132, font::kTiny, "BED LEVEL (encoder)", Canvas::kCenterY);
    g.color(mix(theme_.bg, lil, 0.3f));
    g.fill(588, 144, 192, 6);
    g.color(lil);
    g.fill(588, 144, int(192.0f * s.bed), 6);
    g.color(theme_.dim);
    g.text(588, 172, font::kTiny, "CAPTURE FROM", Canvas::kCenterY);
    g.color(theme_.fg);
    g.text(588, 192, font::kSmall, frzFrom_ == kFromMix ? "THE MIX" : "CLOUD SOURCE", Canvas::kCenterY);
    // library
    for (int i = 0; i < kFreezeSlots; ++i) {
        const int x = 8 + (i % 8) * 98, y = 232 + (i / 8) * 108;
        const FreezeSlot& fs = frz_[i];
        const bool cur = i == frzCur_;
        g.color(cur ? mix(theme_.panel, lil, 0.3f) : theme_.panel);
        g.fill(x, y, 94, 104);
        if (fs.used) {
            g.color(cur ? kWhite : mix(lil, cloudColor(fs.cloud), 0.4f));
            g.beginStrip();
            for (int k = 0; k < 22; ++k) {
                const float u = float(k) / 21.0f;
                const float hz = 40.0f * expf(u * 5.99f);
                const int b = int(hz * float(Spectral::kAna) / kSr);
                const float m = sqrtf(float(fs.mag[b < kSpecBins ? b : kSpecBins - 1]) / 65535.0f);
                g.vtx(float(x + 4) + u * 86.0f, float(y + 90) - m * 66.0f);
            }
            g.endBatch();
        }
        g.color(fs.used ? theme_.fg : theme_.dim);
        g.textf(x + 6, y + 12, font::kTiny, Canvas::kCenterY, "%d", i + 1);
        if (s.freeze && s.freezeSlot == i) {
            g.color(col);
            g.point(float(x + 84), float(y + 12), 4.0f);
        }
    }
}

void Bulut::drawPageMix(Canvas& g) {
    static const char* const kRows[7] = {"LEVEL", "PAN", "DELAY", "REVERB", "TONE", "STEM", "MUTE"};
    static const char* const kFx[7] = {"DLY TIME", "DLY FB", "DLY TONE", "DLY RET", "REV SIZE", "REV DAMP", "REV RET"};
    const int x0 = 64, cw = 96, y0 = 82, rh = 48;
    for (int r = 0; r < 7; ++r) {
        g.color(r == mixRow_ ? theme_.fg : theme_.dim);
        g.text(8, y0 + r * rh + 20, font::kTiny, kRows[r], Canvas::kCenterY);
    }
    for (int c = 0; c <= kClouds; ++c) {
        const int x = x0 + c * cw;
        const bool fxCol = c == kClouds;
        const int colW = fxCol ? lab::kScreenW - 4 - x : cw - 6;   // the FX column takes the rest
        const Rgb col = fxCol ? theme_.accent : cloudColor(c);
        const bool selc = c == mixCol_;
        g.color(selc ? mix(theme_.panel, col, 0.2f) : theme_.panel);
        g.fill(x, 38, colW, 408);
        g.color(col);
        if (fxCol) g.text(x + 8, 52, font::kSmall, "FX", Canvas::kCenterY);
        else g.textf(x + 8, 52, font::kSmall, Canvas::kCenterY, "%d %s", c + 1, clouds_[c].engine() == kGranular ? "GRAN" : "QNTA");
        // meter
        const float pk = fxCol ? (eng_->meterL > eng_->meterR ? eng_->meterL : eng_->meterR) : eng_->rt[c].peak;
        g.color(mix(theme_.bg, col, 0.3f));
        g.fill(x + 8, 66, colW - 16, 4);
        g.color(pk > 0.95f ? theme_.warn : col);
        g.fill(x + 8, 66, int(float(colW - 16) * sqrtf(pk > 1.0f ? 1.0f : pk)), 4);
        for (int r = 0; r < 7; ++r) {
            const int y = y0 + r * rh;
            float v = 0.0f;
            char t[20] = "";
            if (fxCol) {
                switch (r) {
                    case 0: v = fx_.dlyDiv; std::snprintf(t, sizeof t, "%s", dlyDivName(enumIndex(fx_.dlyDiv, kDlyDivs))); break;
                    case 1: v = fx_.dlyFb; break;
                    case 2: v = fx_.dlyTone; break;
                    case 3: v = fx_.dlyRet; break;
                    case 4: v = fx_.revSize; break;
                    case 5: v = fx_.revDamp; break;
                    default: v = fx_.revRet; break;
                }
                if (!t[0]) std::snprintf(t, sizeof t, "%d%%", int(v * 100.0f + 0.5f));
            } else {
                const CloudState& s = clouds_[c];
                switch (r) {
                    case 0: v = s.p[0][P::Level]; break;
                    case 1: v = s.pan; break;
                    case 2: v = s.p[0][P::Delay]; break;
                    case 3: v = s.p[0][P::Reverb]; break;
                    case 4: v = s.p[0][P::Tone]; break;
                    case 5: v = float(s.stem) / 3.0f; break;
                    default: v = s.mute ? 1.0f : 0.0f; break;
                }
                if (r == 1) std::snprintf(t, sizeof t, "%s%d", v < 0.49f ? "L" : (v > 0.51f ? "R" : "C"), int(fabsf(v - 0.5f) * 200.0f));
                else if (r == 4) paramText(0, P::Tone, v, t, sizeof t);
                else if (r == 5) std::snprintf(t, sizeof t, "STEM %d", s.stem + 1);
                else if (r == 6) std::snprintf(t, sizeof t, "%s", s.mute ? "MUTED" : "-");
                else std::snprintf(t, sizeof t, "%d%%", int(v * 100.0f + 0.5f));
            }
            const bool cur = selc && r == mixRow_;
            if (cur) {
                g.color(col);
                g.frame(x + 2, y, colW - 4, rh - 4);
            }
            const bool bar = fxCol || r < 5;
            if (fxCol) {
                g.color(cur ? col : theme_.dim);
                g.text(x + 8, y + 10, font::kTiny, kFx[r], Canvas::kCenterY);
                g.color(cur ? kWhite : theme_.fg);
                g.text(x + colW - 8, y + 10, font::kTiny, t, Canvas::kCenterY | Canvas::kRight);
            } else if (cur || r >= 5) {
                g.color(cur ? kWhite : theme_.fg);
                g.text(x + 8, y + (bar ? 14 : 20), font::kSmall, t, Canvas::kCenterY);
            }
            if (bar) {
                g.color(mix(theme_.bg, col, 0.25f));
                g.fill(x + 8, y + 32, colW - 16, 5);
                g.color(col);
                if (r == 1 && !fxCol) g.fill(x + 8 + int(float(colW - 20) * v), y + 30, 4, 9);
                else g.fill(x + 8, y + 32, int(float(colW - 16) * v), 5);
            }
        }
    }
    if (eng_->soloMask) {
        g.color(theme_.warn);
        g.text(8, 60, font::kTiny, "SOLO", Canvas::kCenterY);
    }
}

void Bulut::drawPageScene(Canvas& g) {
    for (int i = 0; i < kScenes; ++i) {
        const int x = 8 + i * 98, y = 40;
        const Scene& sc = scenes_[i];
        const bool cur = i == scnCur_ && scnFocus_ == 0;
        const Rgb hue = kCloudCol[i % kClouds];
        g.color(cur ? mix(theme_.panel, hue, 0.3f) : theme_.panel);
        g.fill(x, y, 94, 124);
        if (i == curScene_) {
            g.color(hue);
            g.fill(x, y, 94, 3);
        }
        g.color(sc.used ? theme_.fg : theme_.dim);
        g.textf(x + 8, y + 16, font::kBody, Canvas::kCenterY, "%d", i + 1);
        if (!sc.used) {
            g.color(theme_.dim);
            g.text(x + 47, y + 70, font::kTiny, "empty", Canvas::kCenter);
            continue;
        }
        // a small cloud of six dots: size = level, ring = frozen
        for (int c = 0; c < kClouds; ++c) {
            const CloudState& s = sc.cloud[c];
            const float px = float(x + 20 + (c % 3) * 27), py = float(y + 56 + (c / 3) * 34);
            g.color(s.mute ? theme_.dim : kCloudCol[c]);
            g.point(px, py, 2.0f + 9.0f * s.p[0][P::Level]);
        }
        int moves = 0;
        for (int c = 0; c < kClouds; ++c) moves += sc.gest[c].count;
        g.color(theme_.dim);
        g.textf(x + 8, y + 112, font::kTiny, Canvas::kCenterY, "%d moves", moves);
    }
    // timeline
    g.color(theme_.dim);
    g.textf(8, 188, font::kSmall, Canvas::kCenterY, "TIMELINE  %s", songMode_ ? "- SONG MODE (Play runs it)" : "- song mode off (Shift+F6)");
    g.color(theme_.panel);
    g.fill(8, 202, 784, 90);
    int totalBars = 0;
    for (int i = 0; i < tlLen_; ++i) totalBars += timeline_[i].bars;
    if (tlLen_ == 0) {
        g.color(theme_.dim);
        g.text(400, 247, font::kSmall, "Empty: Shift+F4 appends the selected scene, Insert adds after the cursor", Canvas::kCenter);
    } else {
        const float scale = 776.0f / float(totalBars > 16 ? totalBars : 16);
        float x = 12.0f;
        for (int i = 0; i < tlLen_; ++i) {
            const TimeEntry& te = timeline_[i];
            const float w = float(te.bars) * scale;
            const Rgb hue = kCloudCol[te.scene % kClouds];
            const bool cur = i == tlCur_ && scnFocus_ == 1;
            g.color(i == tlPlaying_ ? hue : mix(theme_.panel, hue, cur ? 0.55f : 0.3f));
            g.fill(int(x), 208, int(w) - 3, 78);
            if (cur) {
                g.color(kWhite);
                g.frame(int(x), 208, int(w) - 3, 78);
            }
            if (w > 30.0f) {
                g.color(i == tlPlaying_ ? theme_.bg : theme_.fg);
                g.textf(int(x) + 6, 224, font::kSmall, Canvas::kCenterY, "S%d", te.scene + 1);
                g.textf(int(x) + 6, 246, font::kTiny, Canvas::kCenterY, "%d bar%s", te.bars, te.bars > 1 ? "s" : "");
            }
            x += w;
        }
        if (songMode_ && ctx_->playing() && totalBars > 0) {
            const double b = eng_->beat;
            const double tot = double(totalBars) * 4.0;
            const double pos = b - tot * double(int64_t(b / tot));
            g.color(kWhite);
            g.fill(12 + int(float(pos / 4.0) * scale), 204, 2, 86);
        }
    }
    g.color(theme_.dim);
    g.textf(8, 306, font::kTiny, 0, "%d entries, %d bars   Morph time: Config > BULUT scene morph", tlLen_, totalBars);
    g.text(8, 330, font::kTiny, "Up: scenes  Down: timeline   Left/Right: select   Push: store empty / morph to used", 0);
    g.text(8, 348, font::kTiny, "Timeline: encoder = bars, Shift+encoder = scene, Insert = add, Delete = remove", 0);
    g.text(8, 366, font::kTiny, "Shift + pad row 2: store / recall scenes 1-8 from anywhere", 0);
}

void Bulut::drawPageFile(Canvas& g) {
    g.color(theme_.dim);
    g.text(16, 50, font::kSmall, "PROJECTS", Canvas::kCenterY);
    if (!ctx_->fs) {
        g.color(theme_.warn);
        g.text(270, 200, font::kBody, "No SD card", Canvas::kCenter);
    }
    for (int i = 0; i < kProjects; ++i) {
        const int x = 8 + (i / 8) * 262, y = 64 + (i % 8) * 46;
        const bool cur = i == fileCur_ && fileFocus_ == 0;
        g.color(cur ? mix(theme_.panel, theme_.accent, 0.3f) : theme_.panel);
        g.fill(x, y, 256, 42);
        g.color(projUsed_[i] ? theme_.fg : theme_.dim);
        g.textf(x + 12, y + 21, font::kBody, Canvas::kCenterY, "P%02d", i + 1);
        g.color(projUsed_[i] ? theme_.accent : theme_.dim);
        g.text(x + 244, y + 21, font::kTiny, projUsed_[i] ? "saved" : "empty", Canvas::kCenterY | Canvas::kRight);
    }
    // resample loop
    const bool foc = fileFocus_ == 1;
    const int x = 540;
    g.color(foc ? mix(theme_.panel, theme_.accent2, 0.25f) : theme_.panel);
    g.fill(x, 40, 252, 406);
    g.color(theme_.accent2);
    g.text(x + 12, 56, font::kSmall, "RESAMPLE LOOP", Canvas::kCenterY);
    g.color(theme_.dim);
    g.text(x + 12, 86, font::kTiny, "RECORD (encoder)", Canvas::kCenterY);
    g.color(theme_.fg);
    if (rsSrc_ < 0) g.text(x + 12, 106, font::kBody, "THE MIX", Canvas::kCenterY);
    else g.textf(x + 12, 106, font::kBody, Canvas::kCenterY, "CLOUD %d", rsSrc_ + 1);
    g.color(theme_.dim);
    g.text(x + 12, 136, font::kTiny, "INTO CLOUD (Shift + encoder)", Canvas::kCenterY);
    g.color(cloudColor(rsDst_));
    g.textf(x + 12, 156, font::kBody, Canvas::kCenterY, "CLOUD %d", rsDst_ + 1);
    const float secs = float(eng_->capLen) / kSr;
    g.color(rsState_ == 1 ? theme_.warn : theme_.dim);
    g.text(x + 12, 190, font::kSmall, rsState_ == 1 ? "RECORDING" : (rsState_ == 2 ? "WRITING..." : "READY"), Canvas::kCenterY);
    if (rsState_ == 1) {
        g.color(mix(theme_.bg, theme_.warn, 0.3f));
        g.fill(x + 12, 206, 228, 6);
        g.color(theme_.warn);
        g.fill(x + 12, 206, int(228.0f * float(eng_->capLen) / float(kBufLen)), 6);
        g.color(theme_.fg);
        g.textf(x + 12, 226, font::kSmall, Canvas::kCenterY, "%.1f / %.1f s", double(secs), double(float(kBufLen) / kSr));
    }
    if (rsPath_[0]) {
        g.color(theme_.dim);
        const char* nm = std::strrchr(rsPath_, '/');
        g.textf(x + 12, 254, font::kTiny, Canvas::kCenterY, "last: %s", nm ? nm + 1 : rsPath_);
    }
    g.color(theme_.dim);
    g.text(x + 12, 300, font::kTiny, "Push or S.Rec key: start / stop.", Canvas::kCenterY);
    g.text(x + 12, 318, font::kTiny, "The WAV goes to Lab/BULUT/Resample", Canvas::kCenterY);
    g.text(x + 12, 336, font::kTiny, "and becomes the target cloud's", Canvas::kCenterY);
    g.text(x + 12, 354, font::kTiny, "sample: layer upon layer.", Canvas::kCenterY);
    g.text(x + 12, 390, font::kTiny, "Down from P08 or P16 selects this panel.", Canvas::kCenterY);
}

// ---- pad LEDs --------------------------------------------------------------------------
void Bulut::leds(Rgb* pads) {
    static const float kLvl[3] = {0.3f, 0.6f, 1.0f};
    const float k = kLvl[setLeds_ < 0 ? 0 : (setLeds_ > 2 ? 2 : setLeds_)];
    if (shift_) {
        for (int i = 0; i < lab::kPads; ++i) pads[i] = Rgb();
        for (int c = 0; c < kClouds; ++c) {
            pads[c] = scale(kCloudCol[c], (c == sel_ ? 1.0f : 0.2f) * k);
            pads[36 + c] = clouds_[c].mute ? scale(theme_.warn, 0.3f * k) : scale(kCloudCol[c], 0.6f * k);
        }
        pads[11] = scale(Rgb(0xB9A8F0), (clouds_[sel_].freeze ? 1.0f : 0.25f) * k);
        for (int s = 0; s < kScenes; ++s)
            pads[12 + s] = scenes_[s].used ? scale(kCloudCol[s % kClouds], (s == curScene_ ? 1.0f : 0.35f) * k)
                                           : scale(theme_.dim, 0.08f * k);
        for (int f = 0; f < kFreezeSlots && f < 12; ++f)
            pads[24 + f] = frz_[f].used ? scale(Rgb(0xB9A8F0), (clouds_[sel_].freezeSlot == f ? 1.0f : 0.35f) * k)
                                        : scale(theme_.dim, 0.06f * k);
        return;
    }
    const CloudState& s = clouds_[sel_];
    const Rgb col = kCloudCol[sel_];
    const bool synth = s.engine() == kQuanta && enumIndex(s.p[1][P::QType], kQTypes) != kQSample;
    const bool recording = rec_->state() != GestureRecorder::kIdle && rec_->cloud() == sel_;
    const CloudRt& r = eng_->rt[sel_];
    for (int p = 0; p < lab::kPads; ++p) {
        const int col12 = p % 12, row = p / 12;
        float base = synth ? 0.10f + 0.012f * float(col12) : 0.05f + 0.30f * float(colPeak_[sel_][col12]) / 255.0f;
        base *= 1.0f - 0.12f * float(row);
        Rgb c = scale(col, base);
        const float gl = colGlow_[sel_][col12];
        c = mix(c, col, gl * 0.8f);
        if (recording) c = mix(c, theme_.warn, 0.18f);
        pads[p] = scale(c, k);
    }
    for (int i = 0; i < kEmitters; ++i) {
        const Emitter& e = r.em[i];
        if (!e.on || e.pad >= lab::kPads) continue;
        const Rgb hi = i < kLiveEmitters ? (recording ? theme_.warn : mix(col, kWhite, 0.7f)) : theme_.accent2;
        pads[e.pad] = scale(hi, k);
    }
}

}  // namespace bulut
