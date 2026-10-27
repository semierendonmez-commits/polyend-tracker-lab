// GRAF -- drawing: intro, the seven pages, pad LEDs.
//
// Look: a dark instrument panel. Modules are boxes on a 12 x 4 grid (the pads),
// coloured by category; cables sag between output jacks (right edge) and
// input jacks (left edge), coloured by signal: orange audio, blue CV, yellow
// gate, pink pitch. Every page keeps under the 1,800-word display budget:
// dense canvases switch to flat boxes and straight cables.
#include <cstdio>
#include <cstring>

#include "lab/nav.h"
#include "ws/graf_ws.h"

namespace graf {

const char* templateAbout(int i);

using lab::font::kBody;
using lab::font::kHuge;
using lab::font::kLarge;
using lab::font::kSmall;
using lab::font::kTiny;
using lab::font::kTitle;

namespace {
constexpr int kCellW = 66, kCellH = 96, kGridX = 4, kGridY = 36;
constexpr int kBoxW = 56, kBoxH = 60, kBoxDx = 5, kBoxDy = 18;
inline Rgb sigCol(int s) { return col::kSig[s & 3]; }
inline Rgb catCol(int c) { return col::kCat[c < kCatCount ? c : 0]; }
float peakOf(const float* b) {
    float p = 0.0f;
    for (uint32_t i = 0; i < kBlock; i += 4) {
        const float a = fabsf(b[i]);
        if (a > p) p = a;
    }
    return p;
}
const char* const kPageNames[kPgCount] = {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File"};
const char* const kSigNames[4] = {"audio", "cv", "gate", "pitch"};
// The four signal colours as a legend (x, y = left / centre line).
void legend(Canvas& g, int x, int y) {
    for (int s = 0; s < 4; ++s) {
        g.color(sigCol(s));
        g.fill(x + s * 62, y - 3, 12, 6);
        g.text(x + s * 62 + 16, y, kTiny, kSigNames[s], Canvas::kCenterY);
    }
}
}  // namespace

void Graf::cellXY(int cell, float* x, float* y) const {
    *x = float(kGridX + lab::padCol(cell) * kCellW);
    *y = float(kGridY + lab::padRow(cell) * kCellH);
}

void Graf::jackXY(int slot, bool out, int jack, float* x, float* y) const {
    const Module& m = eng_.mod(slot);
    const ModDef& d = def(m.type);
    float cx, cy;
    cellXY(m.cell, &cx, &cy);
    const float bx = cx + kBoxDx, by = cy + kBoxDy;
    const int n = out ? d.nOut : d.nIn;
    *x = out ? bx + kBoxW : bx;
    *y = n <= 1 ? by + kBoxH * 0.5f : by + 9.0f + float(jack) * float(kBoxH - 18) / float(n - 1);
}

// ------------------------------------------------------------------ intro
// A patch assembling itself: grid dots, modules popping in, cables drawing
// from jack to jack, then the name.
void Graf::drawIntro(Canvas& g, uint32_t t) {
    const float s = float(t) * 0.001f;
    g.color(col::kFaint);
    g.pointSize(1.5f);
    g.beginPoints();
    for (int c = 0; c < 48; ++c) {
        const float appear = float(c % 12) * 0.03f + float(c / 12) * 0.05f;
        if (s > appear) g.vtx(float(kGridX + (c % 12) * kCellW + 33), float(kGridY + (c / 12) * kCellH + 48));
    }
    g.endBatch();
    struct N {
        int cell, cat;
        const char* tag;
    };
    static const N nodes[] = {{13, kCatSeq, "SEQ"},   {15, kCatOsc, "VCO"},  {17, kCatShape, "VCF"},
                              {19, kCatShape, "VCA"}, {21, kCatFx, "DLY"},   {27, kCatMod, "ENV"},
                              {3, kCatMacro, "RETE"}, {33, kCatMod, "LFO"},  {23, kCatIo, "OUT"}};
    static const int links[][3] = {{0, 1, 3}, {1, 2, 0}, {2, 3, 0}, {3, 4, 0}, {4, 8, 0},
                                   {0, 5, 2}, {5, 3, 1}, {7, 2, 1}, {6, 4, 0}};
    auto center = [&](int cell, float* x, float* y) {
        *x = float(kGridX + (cell % 12) * kCellW + 33);
        *y = float(kGridY + (cell / 12) * kCellH + 48);
    };
    // cables
    g.lineWidth(2.5f);
    for (int i = 0; i < 9; ++i) {
        const float t0 = 0.55f + float(i) * 0.11f;
        if (s < t0) continue;
        const float k = clampf((s - t0) / 0.35f, 0.0f, 1.0f);
        float x0, y0, x1, y1;
        center(nodes[links[i][0]].cell, &x0, &y0);
        center(nodes[links[i][1]].cell, &x1, &y1);
        x0 += 24;
        x1 -= 24;
        const float sag = 18.0f + fabsf(x1 - x0) * 0.12f;
        g.color(sigCol(links[i][2]));
        g.beginStrip();
        const int segs = 8;
        for (int j = 0; j <= segs; ++j) {
            const float u = float(j) / float(segs) * k;
            const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f + sag;
            const float a = (1 - u) * (1 - u), b = 2 * u * (1 - u), c = u * u;
            g.vtx(a * x0 + b * mx + c * x1, a * y0 + b * my + c * y1);
        }
        g.endBatch();
    }
    g.lineWidth(1.0f);
    // nodes
    for (int i = 0; i < 9; ++i) {
        const float t0 = 0.15f + float(i) * 0.07f;
        if (s < t0) continue;
        const float k = clampf((s - t0) / 0.2f, 0.0f, 1.0f);
        float x, y;
        center(nodes[i].cell, &x, &y);
        const float w = 48.0f * k, h = 40.0f * k;
        g.color(col::kPanel2);
        g.fillRound(int(x - w * 0.5f), int(y - h * 0.5f), int(w), int(h), 5);
        g.color(catCol(nodes[i].cat));
        g.fill(int(x - w * 0.5f), int(y - h * 0.5f), int(w), 3);
        if (k >= 1.0f) {
            g.color(col::kFg);
            g.text(int(x), int(y + 2), kTiny, nodes[i].tag, Canvas::kCenter);
        }
    }
    // name, in the empty bottom row
    if (s > 1.5f) {
        const float k = clampf((s - 1.5f) / 0.6f, 0.0f, 1.0f);
        g.color(mix(col::kBg, col::kAccent, k));
        g.text(400, 382, kHuge, "G R A F", Canvas::kCenter);
        g.fill(int(400 - 130 * k), 406, int(260 * k), 2);
        g.color(mix(col::kBg, col::kDim, k));
        g.text(400, 426, kSmall, "modular synthesis workstation", Canvas::kCenter);
    }
}

// ------------------------------------------------------------------ frame parts
void Graf::drawTop(Canvas& g, const char* title) {
    g.color(col::kPanel);
    g.fill(0, 0, 800, 32);
    // logo: two nodes and a cable
    g.color(col::kAccent);
    g.fill(8, 9, 10, 14);
    g.fill(34, 9, 10, 14);
    g.color(col::kSig[0]);
    g.lineWidth(2.0f);
    g.line(18, 13, 34, 19);
    g.lineWidth(1.0f);
    g.color(col::kAccent);
    g.text(52, 16, kBody, "GRAF", Canvas::kCenterY);
    g.color(col::kFg);
    g.text(118, 16, kBody, title, Canvas::kCenterY);
    char b[32];
    const int pn = (patch_ & 7) + 1;
    if (eng_.rack) std::snprintf(b, sizeof b, "RACK  canvas %d", pn);
    else std::snprintf(b, sizeof b, "PATCH %d  ch %d", pn, pn);
    g.color(eng_.rack ? col::kSig[1] : col::kSig[3]);
    g.text(270, 16, kSmall, b, Canvas::kCenterY);
    // the DSP budget bar
    const float load = float(eng_.cpuCost()) / float(kCpuBudget);
    g.color(col::kDim);
    g.text(436, 16, kTiny, "DSP", Canvas::kCenterY);
    g.color(col::kRail);
    g.fill(466, 11, 80, 10);
    g.color(load > 0.9f ? col::kWarn : (load > 0.7f ? col::kAccent : col::kOk));
    g.fill(466, 11, int(80.0f * clampf(load, 0.0f, 1.0f)), 10);
    g.color(col::kDim);
    g.textf(556, 16, kTiny, Canvas::kCenterY, "%d/%d", eng_.moduleCount(), kMaxSlots);
    if (xfOn_) {
        g.color(col::kAccent);
        g.textf(612, 16, kTiny, Canvas::kCenterY, "XF %d%%", int(xf_ * 100.0f));
    } else if (eng_.songOn) {
        g.color(col::kOk);
        g.text(612, 16, kTiny, "SONG", Canvas::kCenterY);
    } else if (eng_.morphing()) {
        g.color(col::kAccent);
        g.textf(612, 16, kTiny, Canvas::kCenterY, "S%d %d%%", eng_.morphTarget() + 1, int(eng_.morphPos() * 100.0f));
    }
    g.color(col::kFg);
    g.textf(712, 16, kBody, Canvas::kCenterY | Canvas::kRight, "%.0f", double(ctx_->bpm()));
    if (ctx_->playing()) {
        g.color(col::kOk);
        g.beginStrip();
        g.vtx(726, 9);
        g.vtx(740, 16);
        g.vtx(726, 23);
        g.vtx(726, 9);
        g.endBatch();
    } else {
        g.color(col::kDim);
        g.fill(727, 10, 12, 12);
    }
    g.color(col::kRail);
    g.fill(752, 12, 40, 8);
    g.color(masterShow_ > 0.95f ? col::kWarn : col::kOk);
    g.fill(752, 12, int(40.0f * clampf(masterShow_, 0.0f, 1.0f)), 8);
}

void Graf::drawScope(Canvas& g, int x, int y, int w, int h, Rgb c, float gain) {
    g.color(col::kPanel);
    g.fill(x, y, w, h);
    g.color(col::kRail);
    g.hline(x, y + h / 2, w);
    g.color(c);
    g.lineWidth(1.5f);
    g.beginStrip();
    const float my = float(y + h / 2), a = float(h) * 0.45f;
    for (int i = 0; i < 64; ++i)
        g.vtx(float(x) + float(i) * float(w - 1) / 63.0f, my - clampf(eng_.scope[i * 2] * gain, -1.1f, 1.1f) * a);
    g.endBatch();
    g.lineWidth(1.0f);
}

// ------------------------------------------------------------------ draw
void Graf::draw(Canvas& g) {
    blinkMs_ = ctx_->millis();
    char title[40];
    if (page_ == kPgModule && sel_ >= 0 && eng_.mod(sel_).used) {
        char l[12];
        modLabel(l, sizeof l, sel_);
        std::snprintf(title, sizeof title, "Module %s", l);
    } else {
        lab::copyStr(title, sizeof title, kPageNames[page_]);
    }
    drawTop(g, title);
    switch (page_) {
        case kPgGraph: drawGraph(g); break;
        case kPgMatrix: drawMatrix(g); break;
        case kPgModule: drawModule(g); break;
        case kPgAdd: drawAdd(g); break;
        case kPgSnap: drawSnap(g); break;
        case kPgPlay: drawPlay(g); break;
        case kPgFile: drawFile(g); break;
        default: break;
    }
    const char* f8 = eng_.rack ? "Canvas+" : "Patch+";
    if (page_ == kPgAdd) f8 = addCat_ >= kCatCount ? "Template" : "Add";
    if (page_ == kPgSnap) f8 = "Store";
    if (page_ == kPgFile) f8 = "Save";
    const char* keys[8] = {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", f8};
    lab::drawFKeyBar(g, theme_, keys, page_);
}

// ------------------------------------------------------------------ graph
// The patch strip: the cable the pad gesture made, its scope, the keys.
void Graf::drawPatchStrip(Canvas& g, int y) {
    const int k = gestCable_ >= 0 ? gestCable_ : selCable_;
    if (k < 0 || !eng_.cable(k).used) return;
    const Cable& cb = eng_.cable(k);
    const int sig = def(eng_.mod(cb.src).type).out[cb.srcOut].sig;
    g.color(col::kPanel2);
    g.fill(0, y, 800, 50);
    g.color(sigCol(sig));
    g.fill(0, y, 4, 50);
    char a[24], b[24];
    jackLabel(a, sizeof a, Jack{cb.src, cb.srcOut}, true);
    jackLabel(b, sizeof b, Jack{cb.dst, cb.dstIn}, false);
    g.textf(12, y + 14, kBody, Canvas::kCenterY, "%s  >  %s", a, b);
    // amount bar
    g.color(col::kRail);
    g.fill(12, y + 32, 200, 8);
    g.color(col::kFg);
    g.vline(112, y + 29, 14);
    g.color(sigCol(sig));
    const float am = clampf(cb.amount, -2.0f, 2.0f) * 50.0f;
    if (am >= 0) g.fill(112, y + 32, int(am), 8);
    else g.fill(112 + int(am), y + 32, int(-am), 8);
    g.color(col::kFg);
    g.textf(222, y + 36, kSmall, Canvas::kCenterY, "%+.2f", double(cb.amount));
    // the source signal
    drawScope(g, 300, y + 4, 180, 42, sigCol(sig));
    g.color(col::kDim);
    g.text(492, y + 14, kTiny, "hold: Left/Right output, Up/Down input", Canvas::kCenterY);
    g.text(492, y + 30, kTiny, "encoder: amount, tap again / Delete: remove", Canvas::kCenterY);
}

void Graf::drawGraph(Canvas& g) {
    // grid
    g.color(col::kRail);
    g.pointSize(1.2f);
    g.beginPoints();
    for (int c = 0; c < kCells; ++c) {
        float x, y;
        cellXY(c, &x, &y);
        g.vtx(x + kCellW * 0.5f, y + kCellH * 0.5f);
    }
    g.endBatch();
    const int vp = patch_;
    int slots[kCells], n = 0;
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(vp, c);
        if (s >= 0) slots[n++] = s;
    }
    const bool dense = n > 26;
    const bool gesture = holdCell_ >= 0 && gestCable_ >= 0;
    // neighbours of the selection (follow the signal)
    int up[8], dn[8], nu = 0, nd = 0;
    if (sel_ >= 0 && eng_.mod(sel_).used) {
        nu = eng_.upstream(sel_, up, 8);
        nd = eng_.downstream(sel_, dn, 8);
    }
    // info strip (bottom)
    if (!gesture) {
        g.color(col::kPanel);
        g.fill(0, 422, 800, 26);
        if (sel_ >= 0 && eng_.mod(sel_).used) {
            const Module& m = eng_.mod(sel_);
            const ModDef& d = def(m.type);
            g.color(catCol(d.cat));
            g.textf(10, 435, kSmall, Canvas::kCenterY, "%s%d  %s", d.tag, m.number, d.title);
            char l[12];
            g.color(col::kSig[1]);
            if (nu) {
                modLabel(l, sizeof l, up[0]);
                g.textf(250, 435, kTiny, Canvas::kCenterY, "Note < %s%s", l, nu > 1 ? " +" : "");
            }
            g.color(col::kSig[0]);
            if (nd) {
                modLabel(l, sizeof l, dn[0]);
                g.textf(370, 435, kTiny, Canvas::kCenterY, "Instr > %s%s", l, nd > 1 ? " +" : "");
            }
            g.color(col::kDim);
            g.text(490, 435, kTiny, "hold + tap: cable  Insert: open  Shift+Insert: tidy", Canvas::kCenterY);
        } else {
            g.color(col::kDim);
            g.text(10, 435, kTiny,
                   "Pad: select   hold A + tap B: cable   hold + tap empty: move   2nd tap on empty: add   Shift+Insert: tidy",
                   Canvas::kCenterY);
        }
    }
    // cables touching this canvas
    int cab[kMaxCables], nc = 0;
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = eng_.cable(c);
        if (!k.used) continue;
        if (eng_.mod(k.src).patch == vp || eng_.mod(k.dst).patch == vp) cab[nc++] = c;
    }
    const uint32_t modCost = uint32_t(n) * (dense ? 20u : 34u) + 40u + (gesture ? 200u : 0u);
    const bool sag = prefSag_ && g.words() + modCost + uint32_t(nc) * 13u + 60u < lab::Canvas::kBudgetWords;
    for (int sig = 0; sig < 4; ++sig) {
        bool first = true;
        for (int i = 0; i < nc; ++i) {
            const Cable& k = eng_.cable(cab[i]);
            const int ksig = def(eng_.mod(k.src).type).out[k.srcOut].sig;
            if (ksig != sig) continue;
            const bool srcHere = eng_.mod(k.src).patch == vp, dstHere = eng_.mod(k.dst).patch == vp;
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (srcHere) jackXY(k.src, true, k.srcOut, &x0, &y0);
            if (dstHere) jackXY(k.dst, false, k.dstIn, &x1, &y1);
            if (!srcHere) {
                x0 = x1 - 14.0f;
                y0 = y1 - 8.0f;
            }
            if (!dstHere) {
                x1 = x0 + 14.0f;
                y1 = y0 + 8.0f;
            }
            const float a = clampf(fabsf(k.amount), 0.0f, 1.0f);
            const bool hi = cab[i] == gestCable_ || cab[i] == selCable_ || (sel_ >= 0 && (k.src == sel_ || k.dst == sel_));
            const Rgb c = mix(col::kBg, sigCol(sig), hi ? 1.0f : 0.3f + 0.45f * a);
            if (sag && srcHere && dstHere) {
                g.color(c);
                g.lineWidth(cab[i] == gestCable_ ? 3.5f : (hi ? 2.5f : 1.8f));
                const float d = fabsf(x1 - x0) + fabsf(y1 - y0);
                const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f + 10.0f + d * 0.12f;
                g.beginStrip();
                for (int j = 0; j <= 5; ++j) {
                    const float u = float(j) * 0.2f;
                    const float aa = (1 - u) * (1 - u), bb = 2 * u * (1 - u), cc = u * u;
                    g.vtx(aa * x0 + bb * mx + cc * x1, aa * y0 + bb * my + cc * y1);
                }
                g.endBatch();
            } else {
                if (first) {
                    g.color(mix(col::kBg, sigCol(sig), 0.8f));
                    g.lineWidth(1.5f);
                    g.beginLines();
                    first = false;
                }
                g.vtx(x0, y0);
                g.vtx(x1, y1);
            }
        }
        if (!first) g.endBatch();
    }
    g.lineWidth(1.0f);
    // modules
    for (int i = 0; i < n; ++i) {
        const int s = slots[i];
        const Module& m = eng_.mod(s);
        const ModDef& d = def(m.type);
        float x, y;
        cellXY(m.cell, &x, &y);
        const int bx = int(x) + kBoxDx, by = int(y) + kBoxDy;
        const bool isSel = s == sel_;
        bool isUp = false, isDn = false;
        for (int k = 0; k < nu; ++k) isUp = isUp || up[k] == s;
        for (int k = 0; k < nd; ++k) isDn = isDn || dn[k] == s;
        if (isSel || isUp || isDn) {
            g.color(isSel ? col::kAccent : (isUp ? scale(col::kSig[1], 0.8f) : scale(col::kSig[0], 0.8f)));
            if (dense) g.fill(bx - 2, by - 2, kBoxW + 4, kBoxH + 4);
            else g.fillRound(bx - 3, by - 3, kBoxW + 6, kBoxH + 6, 7);
        }
        g.color(isSel ? col::kPanel2 : col::kPanel);
        if (dense) g.fill(bx, by, kBoxW, kBoxH);
        else g.fillRound(bx, by, kBoxW, kBoxH, 5);
        g.color(catCol(d.cat));
        g.fill(bx + 2, by, kBoxW - 4, 4);
        g.color(isSel ? col::kFg : mix(col::kDim, col::kFg, 0.6f));
        char lb[12];
        std::snprintf(lb, sizeof lb, "%s%d", d.tag, m.number);
        g.text(bx + kBoxW / 2, by + 18, kSmall, lb, Canvas::kCenter);
        if (!dense && d.nOut) {   // activity of the first patched output
            int o = 0;
            for (int c = 0; c < nc; ++c) {
                const Cable& k = eng_.cable(cab[c]);
                if (k.src == s) {
                    o = k.srcOut;
                    break;
                }
            }
            const float pk = clampf(peakOf(m.out[o]), 0.0f, 1.0f);
            g.color(col::kRail);
            g.fill(bx + 8, by + 40, kBoxW - 16, 5);
            g.color(sigCol(d.out[o].sig));
            g.fill(bx + 8, by + 40, int(float(kBoxW - 16) * pk), 5);
        }
        if (!dense && eng_.tapOf(s, 0) >= 0) {
            g.color(col::kSig[1]);
            g.text(bx + kBoxW - 4, by + 52, kTiny, "T", Canvas::kRight | Canvas::kCenterY);
        }
    }
    // jacks
    if (!dense) {
        g.pointSize(2.6f);
        for (int pass = 0; pass < 5; ++pass) {
            bool open = false;
            for (int i = 0; i < n; ++i) {
                const int s = slots[i];
                const ModDef& d = def(eng_.mod(s).type);
                for (int dir = 0; dir < 2; ++dir) {
                    const int nj = dir ? d.nOut : d.nIn;
                    for (int j = 0; j < nj; ++j) {
                        const int sig = dir ? d.out[j].sig : d.in[j].sig;
                        bool conn = false;
                        for (int c = 0; c < nc && !conn; ++c) {
                            const Cable& k = eng_.cable(cab[c]);
                            conn = dir ? (k.src == s && k.srcOut == j) : (k.dst == s && k.dstIn == j);
                        }
                        const int want = conn ? sig : 4;
                        if (want != pass) continue;
                        if (!open) {
                            g.color(pass < 4 ? sigCol(pass) : col::kFaint);
                            g.beginPoints();
                            open = true;
                        }
                        float jx, jy;
                        jackXY(s, dir == 1, j, &jx, &jy);
                        g.vtx(jx, jy);
                    }
                }
            }
            if (open) g.endBatch();
        }
    }
    // cursor on an empty cell / the held source
    float cx, cy;
    cellXY(cursor_, &cx, &cy);
    if (eng_.slotAtCell(vp, cursor_) < 0) {
        g.color((blinkMs_ / 300) & 1 ? col::kAccent : col::kDim);
        g.frame(int(cx) + kBoxDx, int(cy) + kBoxDy, kBoxW, kBoxH);
        g.text(int(cx) + kCellW / 2, int(cy) + kBoxDy + kBoxH / 2, kBody, "+", Canvas::kCenter);
    }
    if (holdCell_ >= 0) {
        cellXY(holdCell_, &cx, &cy);
        g.color(col::kSig[0]);
        g.text(int(cx) + kCellW / 2, int(cy) + 9, kTiny, "patch to...", Canvas::kCenter);
    }
    if (gesture || (holdCell_ < 0 && gestCable_ >= 0 && eng_.cable(gestCable_).used)) drawPatchStrip(g, 398);
    if (n == 0) {
        g.color(col::kDim);
        g.text(400, 190, kBody, "Empty canvas. Tap a pad twice (or Insert) to add a module,", Canvas::kCenter);
        g.text(400, 218, kBody, "or pick a template on the Add page (F4, pad 8).", Canvas::kCenter);
    }
}

// ------------------------------------------------------------------ matrix
void Graf::drawMatrix(Canvas& g) {
    constexpr int gx = 160, gy = 112, cw = 52, ch = 50;
    const int r1 = mRow_ + 4 < nSrc_ ? mRow_ + 4 : nSrc_;
    const int c1 = mCol_ + 12 < nDst_ ? mCol_ + 12 : nDst_;
    // crosshair: the held pin, else the selected cable
    int hr = -1, hc = -1;
    if (heldPin_ >= 0) {
        hr = mRow_ + lab::padRow(heldPin_);
        hc = mCol_ + lab::padCol(heldPin_);
    } else if (selCable_ >= 0 && eng_.cable(selCable_).used) {
        const Cable& k = eng_.cable(selCable_);
        for (int r = mRow_; r < r1; ++r)
            if (srcs_[r].slot == k.src && srcs_[r].jack == k.srcOut) hr = r;
        for (int c = mCol_; c < c1; ++c)
            if (dsts_[c].slot == k.dst && dsts_[c].jack == k.dstIn) hc = c;
    }
    // column heads: destinations (module, input)
    int lastSlot = -1;
    for (int c = mCol_; c < c1; ++c) {
        const Jack& j = dsts_[c];
        const Module& m = eng_.mod(j.slot);
        const ModDef& d = def(m.type);
        const int x = gx + (c - mCol_) * cw + cw / 2;
        if (c == hc) {
            g.color(col::kPanel2);
            g.fill(x - cw / 2, 40, cw, 72);
        }
        if (j.slot != lastSlot) {
            char l[12];
            modLabel(l, sizeof l, j.slot);
            g.color(catCol(d.cat));
            g.fill(x - cw / 2 + 1, 42, 3, 64);
            g.text(x - cw / 2 + 7, 52, kTiny, l, Canvas::kCenterY);
            lastSlot = j.slot;
        }
        g.color(sigCol(d.in[j.jack].sig));
        g.text(x, 96, kTiny, d.in[j.jack].name, Canvas::kCenter);
    }
    g.color(col::kFg);
    g.text(10, 50, kSmall, "OUT \\ IN", Canvas::kCenterY);
    g.color(col::kDim);
    g.textf(10, 72, kTiny, Canvas::kCenterY, "rows %d-%d of %d", nSrc_ ? mRow_ + 1 : 0, r1, nSrc_);
    g.textf(10, 90, kTiny, Canvas::kCenterY, "cols %d-%d of %d", nDst_ ? mCol_ + 1 : 0, c1, nDst_);
    // row heads: sources
    lastSlot = -1;
    for (int r = mRow_; r < r1; ++r) {
        const Jack& j = srcs_[r];
        const Module& m = eng_.mod(j.slot);
        const ModDef& d = def(m.type);
        const int y = gy + (r - mRow_) * ch + ch / 2;
        if (r == hr) {
            g.color(col::kPanel2);
            g.fill(4, y - ch / 2, gx - 6, ch);
        }
        char l[12];
        modLabel(l, sizeof l, j.slot);
        g.color(catCol(d.cat));
        g.fill(6, y - ch / 2 + 4, 3, ch - 8);
        if (j.slot != lastSlot || r == mRow_) g.text(14, y - 9, kTiny, l, Canvas::kCenterY);
        lastSlot = j.slot;
        g.color(sigCol(d.out[j.jack].sig));
        g.text(14, y + 9, kSmall, d.out[j.jack].name, Canvas::kCenterY);
        if (eng_.rack && m.patch != patch_) {
            g.color(col::kDim);
            g.textf(150, y, kTiny, Canvas::kCenterY | Canvas::kRight, "c%d", m.patch + 1);
        }
    }
    // grid, module boundaries brighter
    g.color(col::kPanel);
    g.fill(gx, gy, cw * 12, ch * 4);
    if (hr >= mRow_ && hr < r1) {
        g.color(col::kPanel2);
        g.fill(gx, gy + (hr - mRow_) * ch, cw * 12, ch);
    }
    if (hc >= mCol_ && hc < c1) {
        g.color(col::kPanel2);
        g.fill(gx + (hc - mCol_) * cw, gy, cw, ch * 4);
    }
    g.color(col::kRail);
    g.beginLines();
    for (int c = 0; c <= 12; ++c) {
        g.vtx(float(gx + c * cw), float(gy));
        g.vtx(float(gx + c * cw), float(gy + ch * 4));
    }
    for (int r = 0; r <= 4; ++r) {
        g.vtx(float(gx), float(gy + r * ch));
        g.vtx(float(gx + 12 * cw), float(gy + r * ch));
    }
    g.endBatch();
    g.color(col::kDim);
    g.beginLines();
    for (int c = mCol_ + 1; c < c1; ++c)
        if (dsts_[c].slot != dsts_[c - 1].slot) {
            g.vtx(float(gx + (c - mCol_) * cw), float(gy));
            g.vtx(float(gx + (c - mCol_) * cw), float(gy + ch * 4));
        }
    for (int r = mRow_ + 1; r < r1; ++r)
        if (srcs_[r].slot != srcs_[r - 1].slot) {
            g.vtx(float(gx), float(gy + (r - mRow_) * ch));
            g.vtx(float(gx + 12 * cw), float(gy + (r - mRow_) * ch));
        }
    g.endBatch();
    // scrollbars
    if (nSrc_ > 4) {
        const int h = ch * 4, th = h * 4 / nSrc_ > 8 ? h * 4 / nSrc_ : 8;
        g.color(col::kRail);
        g.fill(gx + 12 * cw + 4, gy, 5, h);
        g.color(col::kAccent);
        g.fill(gx + 12 * cw + 4, gy + (h - th) * mRow_ / (nSrc_ - 4 > 0 ? nSrc_ - 4 : 1), 5, th);
    }
    if (nDst_ > 12) {
        const int w = cw * 12, tw = w * 12 / nDst_ > 8 ? w * 12 / nDst_ : 8;
        g.color(col::kRail);
        g.fill(gx, gy + ch * 4 + 4, w, 5);
        g.color(col::kAccent);
        g.fill(gx + (w - tw) * mCol_ / (nDst_ - 12 > 0 ? nDst_ - 12 : 1), gy + ch * 4 + 4, tw, 5);
    }
    // pins
    g.color(col::kFaint);
    g.pointSize(2.0f);
    g.beginPoints();
    for (int r = mRow_; r < r1; ++r)
        for (int c = mCol_; c < c1; ++c)
            if (matrixCable(r, c) < 0) g.vtx(float(gx + (c - mCol_) * cw + cw / 2), float(gy + (r - mRow_) * ch + ch / 2));
    g.endBatch();
    for (int r = mRow_; r < r1; ++r)
        for (int c = mCol_; c < c1; ++c) {
            const int k = matrixCable(r, c);
            if (k < 0) continue;
            const Cable& cb = eng_.cable(k);
            const int sig = def(eng_.mod(cb.src).type).out[cb.srcOut].sig;
            const float px = float(gx + (c - mCol_) * cw + cw / 2), py = float(gy + (r - mRow_) * ch + ch / 2);
            const float a = clampf(fabsf(cb.amount), 0.0f, 1.0f);
            if (k == selCable_) {
                g.color(col::kFg);
                g.point(px, py, 17);
            }
            g.color(mix(col::kPanel2, sigCol(sig), 0.4f + 0.6f * a));
            g.point(px, py, 6.0f + 9.0f * a);
            if (cb.amount < 0.0f) {
                g.color(col::kBg);
                g.fill(int(px) - 5, int(py) - 1, 10, 3);
            }
        }
    // overview: the whole matrix, window outlined
    constexpr int ox = 10, oy = 334, ow = 140, oh = 82;
    g.color(col::kPanel);
    g.fill(ox, oy, ow, oh);
    if (nSrc_ && nDst_) {
        const float sx = float(ow) / float(nDst_), sy = float(oh) / float(nSrc_);
        g.color(col::kAccent);
        g.frame(ox + int(float(mCol_) * sx), oy + int(float(mRow_) * sy), int(float(c1 - mCol_) * sx) + 1,
                int(float(r1 - mRow_) * sy) + 1);
        g.color(col::kSig[0]);
        g.pointSize(1.5f);
        g.beginPoints();
        for (int r = 0; r < nSrc_; ++r)
            for (int c = 0; c < nDst_; ++c)
                if (matrixCable(r, c) >= 0) g.vtx(ox + (float(c) + 0.5f) * sx, oy + (float(r) + 0.5f) * sy);
        g.endBatch();
    }
    // the selected cable with its signal, or the legend + help
    g.color(col::kPanel);
    g.fill(160, 334, 630, 82);
    if (selCable_ >= 0 && eng_.cable(selCable_).used) {
        const Cable& cb = eng_.cable(selCable_);
        char a[28], b[28];
        jackLabel(a, sizeof a, Jack{cb.src, cb.srcOut}, true);
        jackLabel(b, sizeof b, Jack{cb.dst, cb.dstIn}, false);
        const int sig = def(eng_.mod(cb.src).type).out[cb.srcOut].sig;
        g.color(sigCol(sig));
        g.textf(170, 348, kBody, Canvas::kCenterY, "%s  >  %s", a, b);
        g.color(col::kRail);
        g.fill(170, 370, 240, 10);
        g.color(col::kFg);
        g.vline(290, 366, 18);
        g.color(sigCol(sig));
        const float am = clampf(cb.amount, -2.0f, 2.0f) * 60.0f;
        if (am >= 0) g.fill(290, 370, int(am), 10);
        else g.fill(290 + int(am), 370, int(-am), 10);
        g.color(col::kFg);
        g.textf(420, 375, kBody, Canvas::kCenterY, "%+.2f", double(cb.amount));
        g.color(col::kDim);
        g.text(170, 400, kTiny, "encoder: amount (Shift fine)  Insert: + / -  Delete: remove", Canvas::kCenterY);
        drawScope(g, 520, 340, 262, 70, sigCol(sig));
    } else {
        g.color(col::kDim);
        g.text(170, 350, kSmall, "Pads = pins: rows are outputs, columns are inputs.", Canvas::kCenterY);
        g.text(170, 372, kSmall, "Tap: patch / unpatch.  Hold + turn: amount.  Arrows: scroll (Shift: page).",
               Canvas::kCenterY);
        legend(g, 170, 398);
    }
    if (!nSrc_ || !nDst_) {
        g.color(col::kDim);
        g.text(480, 210, kBody, "Nothing to patch on this canvas yet", Canvas::kCenter);
    }
}

// ------------------------------------------------------------------ module
void Graf::drawKnob(Canvas& g, int x, int y, int r, int slot, int p, bool sel) {
    const Module& m = eng_.mod(slot);
    const ModDef& d = def(m.type);
    constexpr float a0 = 2.356f, span = 4.712f;
    const bool held = heldKnob_ == p;
    g.color(held ? scale(col::kAccent, 0.25f) : (sel ? col::kPanel2 : col::kPanel));
    g.fill(x - 49, y - r - 12, 98, r * 2 + 58);
    g.color(col::kDim);
    g.textf(x - 44, y - r - 4, kTiny, Canvas::kCenterY, "%d", p + 1);
    g.color(col::kRail);
    g.lineWidth(4.0f);
    g.arc(float(x), float(y), float(r), a0, a0 + span, 14);
    const float k = m.k[p];
    const Rgb c = sel ? col::kAccent : catCol(d.cat);
    g.color(c);
    g.arc(float(x), float(y), float(r), a0, a0 + span * (k > 0.01f ? k : 0.01f), 1 + int(k * 14.0f));
    g.lineWidth(1.0f);
    // the modulated (effective) value as a dot on the ring
    const float ke = m.ke[p];
    if (fabsf(ke - k) > 0.005f) {
        g.color(col::kSig[1]);
        g.point(float(x) + float(r) * cosf(a0 + span * ke), float(y) + float(r) * sinf(a0 + span * ke), 4.0f);
    }
    g.color(sel ? col::kFg : col::kDim);
    g.point(float(x) + float(r) * 0.55f * cosf(a0 + span * k), float(y) + float(r) * 0.55f * sinf(a0 + span * k), 3.5f);
    g.text(x, y + r + 14, kTiny, d.p[p].name, Canvas::kCenter);
    char v[24];
    fmtParam(v, sizeof v, slot, p);
    g.color(sel ? col::kAccent : col::kFg);
    g.text(x, y + r + 32, kBody, v, Canvas::kCenter);
}

void Graf::drawModule(Canvas& g) {
    if (sel_ < 0 || !eng_.mod(sel_).used) {
        g.color(col::kDim);
        g.text(400, 220, kBody, "No module selected. Press Insert to add one.", Canvas::kCenter);
        return;
    }
    Module& m = eng_.mod(sel_);
    const ModDef& d = def(m.type);
    g.color(catCol(d.cat));
    g.fill(0, 34, 6, 46);
    g.textf(16, 48, kLarge, Canvas::kCenterY, "%s%d", d.tag, m.number);
    g.color(col::kFg);
    g.text(110, 48, kBody, d.title, Canvas::kCenterY);
    g.color(col::kDim);
    g.text(16, 70, kTiny, d.about, Canvas::kCenterY);
    // knobs: two rows of five, big
    for (int p = 0; p < d.nParam; ++p) {
        const int x = 54 + (p % 5) * 102, y = 132 + (p / 5) * 132;
        drawKnob(g, x, y, 30, sel_, p, p == param_);
    }
    // right panel: neighbours, scope, jacks
    const int px = 528;
    int up[8], dn[8];
    const int nu = eng_.upstream(sel_, up, 8), nd = eng_.downstream(sel_, dn, 8);
    char l[12];
    g.color(col::kSig[1]);
    if (nu) {
        modLabel(l, sizeof l, up[0]);
        g.textf(px, 42, kTiny, Canvas::kCenterY, "< %s (pad 11)", l);
    }
    g.color(col::kSig[0]);
    if (nd) {
        modLabel(l, sizeof l, dn[0]);
        g.textf(790, 42, kTiny, Canvas::kCenterY | Canvas::kRight, "%s (pad 12) >", l);
    }
    const int so = eng_.scopeOut;
    drawScope(g, px, 54, 262, 62, d.nOut ? sigCol(d.out[so < d.nOut ? so : 0].sig) : col::kDim);
    g.color(col::kDim);
    if (d.nOut) {
        const int t = eng_.tapOf(sel_, so < d.nOut ? so : 0);
        if (t >= 0) g.textf(px + 4, 62, kTiny, Canvas::kCenterY, "%s  = TAP%d", d.out[so < d.nOut ? so : 0].name, t + 1);
        else g.textf(px + 4, 62, kTiny, Canvas::kCenterY, "%s  (FX2, Shift: tap)", d.out[so < d.nOut ? so : 0].name);
    }
    int y = 128;
    auto conn = [&](bool out, int j, char* buf, int cap) {
        buf[0] = 0;
        int count = 0;
        for (int c = 0; c < kMaxCables; ++c) {
            const Cable& k = eng_.cable(c);
            if (!k.used) continue;
            if (out ? (k.src == sel_ && k.srcOut == j) : (k.dst == sel_ && k.dstIn == j)) {
                if (!count) jackLabel(buf, cap, out ? Jack{k.dst, k.dstIn} : Jack{k.src, k.srcOut}, !out);
                ++count;
            }
        }
        if (count > 1) {
            const size_t ln = std::strlen(buf);
            std::snprintf(buf + ln, size_t(cap) - ln, " +%d", count - 1);
        }
    };
    char b[40];
    for (int i = 0; i < d.nIn; ++i, y += 17) {
        conn(false, i, b, sizeof b);
        g.color(sigCol(d.in[i].sig));
        g.text(px, y, kTiny, d.in[i].name, Canvas::kCenterY);
        g.color(b[0] ? col::kFg : col::kFaint);
        g.text(px + 60, y, kTiny, b[0] ? b : "-", Canvas::kCenterY);
    }
    y += 4;
    for (int i = 0; i < d.nOut; ++i, y += 17) {
        conn(true, i, b, sizeof b);
        g.color(sigCol(d.out[i].sig));
        g.textf(px, y, kTiny, Canvas::kCenterY, "%s >", d.out[i].name);
        g.color(b[0] ? col::kFg : col::kFaint);
        g.text(px + 60, y, kTiny, b[0] ? b : "-", Canvas::kCenterY);
    }
    // module views (right panel bottom)
    const int vx = px, vy = 330;
    switch (m.type) {
        case kSeq: {
            const int pos = seqPosition(m), steps = int(m.v[0]);
            for (int i = 0; i < 16; ++i) {
                const int x = vx + i * 16;
                const bool on = seqGate(m, i);
                const int n = seqNote(m, i);
                g.color(i == pos ? col::kFg : (i < steps ? (on ? catCol(d.cat) : col::kRail) : col::kPanel));
                g.fill(x, vy + 60 - (on ? (n + 14) * 2 : 2), 13, on ? (n + 14) * 2 + 2 : 4);
            }
            g.color(col::kDim);
            g.text(vx, vy + 76, kTiny, "steps: pad rows 3-4 / hold + turn", Canvas::kCenterY);
            break;
        }
        case kPinb: {
            for (int c = 0; c < kCells; ++c) {
                const int x = vx + (c % 12) * 22, yy = vy + (c / 12) * 16;
                g.color((m.ext[c] & 2) ? col::kSig[3] : ((m.ext[c] & 1) ? catCol(d.cat) : col::kRail));
                g.fill(x, yy, 20, 14);
            }
            g.color(col::kFg);
            for (int b2 = 0; b2 < 3; ++b2) {
                const int c = pinbBall(m, b2);
                if (c >= 0) g.point(float(vx + (c % 12) * 22 + 10), float(vy + (c / 12) * 16 + 7), 5);
            }
            g.color(col::kDim);
            g.text(vx, vy + 76, kTiny, "pads: notes, Shift: bumpers, Rec: new", Canvas::kCenterY);
            break;
        }
        case kMarkov: {
            const int n = int(m.v[0]), st = markovState(m);
            for (int i = 0; i < n; ++i) {
                const float a = kTwoPi * float(i) / float(n);
                g.color(i == st ? col::kFg : catCol(d.cat));
                g.point(float(vx + 130) + 110.0f * cosf(a), float(vy + 34) + 28.0f * sinf(a), i == st ? 9 : 5);
            }
            break;
        }
        case kTuring: {
            const uint32_t reg = turingRegister(m);
            const int len = int(m.v[1]);
            for (int i = 0; i < 16; ++i) {
                g.color(i < len ? (((reg >> i) & 1) ? catCol(d.cat) : col::kRail) : col::kPanel);
                g.fill(vx + i * 16, vy + 20, 13, 24);
            }
            break;
        }
        case kEucl: {
            const int steps = int(m.v[0]), hits = int(m.v[1]), rot = int(m.v[2]), pos = euclStep(m);
            for (int i = 0; i < steps; ++i) {
                const float a = kTwoPi * float(i) / float(steps) - 1.5708f;
                const bool h = lab::dsp::euclid(i, steps, hits, rot);
                g.color(i == pos ? col::kFg : (h ? catCol(d.cat) : col::kRail));
                g.point(float(vx + 130) + 110.0f * cosf(a), float(vy + 34) + 30.0f * sinf(a), h ? 5 : 3);
            }
            break;
        }
        case kRete:
        case kNet: {
            const int nn = m.type == kRete ? 4 : 8;
            for (int i = 0; i < nn; ++i) {
                const float a = clampf((m.type == kRete ? reteNode(m, i) : netNode(m, i)) * 3.0f, 0.0f, 1.0f);
                g.color(mix(col::kRail, catCol(d.cat), a));
                g.point(float(vx + 16 + i * (240 / nn)), float(vy + 30), 5.0f + 12.0f * a);
            }
            g.color(col::kDim);
            g.text(vx, vy + 76, kTiny, m.type == kNet ? "Roll / Rec: new gains, Rnd: new matrix" : "Gate or Rec re-rolls",
                   Canvas::kCenterY);
            break;
        }
        case kMgate: {
            const bool on = eng_.manualGate || m.ext[0];
            g.color(on ? col::kAccent : col::kPanel2);
            g.fillRound(vx, vy, 262, 56, 8);
            g.color(on ? col::kBg : col::kFg);
            g.text(vx + 131, vy + 28, kBody, "Perform key / pads rows 2-4", Canvas::kCenter);
            break;
        }
        case kPlts: {
            g.color(catCol(d.cat));
            g.textf(vx, vy + 14, kBody, Canvas::kCenterY, "%s", kPlaitsModels[(int(m.v[0]) & 31) < 24 ? int(m.v[0]) : 0]);
            g.color(col::kDim);
            g.textf(vx, vy + 40, kTiny, Canvas::kCenterY, "this model: %.1f%% of the DSP budget",
                    double(float(plaitsCost(int(m.v[0]))) * 100.0f / float(kCpuBudget)));
            break;
        }
        case kSmpl: {
            g.color(col::kFg);
            g.textf(vx, vy + 10, kSmall, Canvas::kCenterY, "%s", eng_.sampleName[sel_][0] ? eng_.sampleName[sel_] : "-");
            g.textf(vx, vy + 30, kTiny, Canvas::kCenterY, "%.2f s %s", double(float(eng_.sampleFrames[sel_]) / 44100.0f),
                    eng_.sampleStereo[sel_] ? "stereo" : "mono");
            g.color(col::kDim);
            g.text(vx, vy + 52, kTiny, "S.Load: browse /Lab/GRAF/Samples", Canvas::kCenterY);
            break;
        }
        default:
            g.color(col::kDim);
            g.textf(vx, vy + 60, kTiny, Canvas::kCenterY, "pad rows 2-4: keys C%d.. (oct %+d)", keyBase() / 12 - 1,
                    prefOctave_);
            break;
    }
    g.color(col::kDim);
    g.text(16, 422, kTiny, "Pads 1-10: knobs (hold + turn)   Delete: default   Shift+Delete: remove   Insert: add",
           Canvas::kCenterY);
}

// ------------------------------------------------------------------ add
void Graf::drawAdd(Canvas& g) {
    // tabs: the pad row 1
    for (int c = 0; c < 12; ++c) {
        const int x = 4 + c * 66, y = 38;
        if (c <= kCatCount) {
            const bool on = c == addCat_;
            const Rgb cc = c < kCatCount ? catCol(c) : col::kFg;
            g.color(on ? cc : col::kPanel);
            g.fill(x, y, 62, 30);
            if (!on) {
                g.color(cc);
                g.fill(x, y + 27, 62, 3);
            }
            g.color(on ? col::kBg : col::kFg);
            static const char* const kTab[kCatCount + 1] = {"SOURCE", "SHAPE", "MOD", "SEQ", "FX", "I/O", "MACRO", "TEMPL"};
            g.text(x + 31, y + 14, kTiny, kTab[c], Canvas::kCenter);
        } else if (c >= 9) {
            const int t = recent_[c - 9];
            g.color(col::kPanel);
            g.fill(x, y, 62, 30);
            g.color(catCol(kDefs[t].cat));
            g.text(x + 31, y + 14, kTiny, kDefs[t].tag, Canvas::kCenter);
        } else {
            g.color(col::kDim);
            g.text(x + 31, y + 14, kTiny, "recent:", Canvas::kCenter);
        }
    }
    // items: pad rows 2-4
    const int n = addOrderCount(addCat_);
    for (int i = 0; i < n && i < 36; ++i) {
        const int x = 4 + (i % 12) * 66, y = 76 + (i / 12) * 46;
        if (addCat_ >= kCatCount) {
            const bool sel = i == addTpl_;
            g.color(sel ? col::kFg : col::kPanel);
            g.fill(x, y, 62, 42);
            g.color(sel ? col::kBg : col::kFg);
            char nm[16];
            kit::fitText(nm, sizeof nm, kTiny, templateName(i), 58);
            g.text(x + 31, y + 21, kTiny, nm, Canvas::kCenter);
            continue;
        }
        const int t = addTypeAt(addCat_, i);
        const ModDef& d = def(t);
        const bool sel = t == addType_;
        const bool ok = eng_.fits(t == kPlts ? plaitsCost(8) : d.cost);
        g.color(sel ? catCol(d.cat) : col::kPanel);
        g.fill(x, y, 62, 42);
        g.color(sel ? col::kBg : (ok ? col::kFg : col::kFaint));
        g.text(x + 31, y + 15, kSmall, d.tag, Canvas::kCenter);
        g.color(sel ? col::kBg : col::kDim);
        g.textf(x + 31, y + 33, kTiny, Canvas::kCenter, "%.0f%%",
                double(float(t == kPlts ? plaitsCost(8) : d.cost) * 100.0f / float(kCpuBudget)));
    }
    // the selected entry
    const int y = 222;
    if (addCat_ >= kCatCount) {
        g.color(col::kFg);
        g.fill(8, y, 6, 150);
        g.text(26, y + 14, kLarge, templateName(addTpl_), Canvas::kCenterY);
        g.color(col::kDim);
        g.text(26, y + 42, kSmall, templateAbout(addTpl_), Canvas::kCenterY);
        g.text(26, y + 72, kSmall, "Added next to the cursor where the block fits; an OUT already on the", Canvas::kCenterY);
        g.text(26, y + 94, kSmall, "canvas is used instead of a new one. FX chain takes the selected module.", Canvas::kCenterY);
    } else {
        const ModDef& d = def(addType_);
        g.color(catCol(d.cat));
        g.fill(8, y, 6, 150);
        g.text(26, y + 14, kLarge, d.title, Canvas::kCenterY);
        g.color(col::kFg);
        g.text(26, y + 40, kSmall, d.about, Canvas::kCenterY);
        char line[120];
        int k = 0;
        line[0] = 0;
        for (int i = 0; i < d.nIn; ++i)
            k += std::snprintf(line + k, sizeof line - size_t(k), "%s%s", i ? ", " : "", d.in[i].name);
        g.color(col::kDim);
        g.text(26, y + 66, kSmall, "In:", Canvas::kCenterY);
        g.color(col::kFg);
        g.text(70, y + 66, kSmall, d.nIn ? line : "-", Canvas::kCenterY);
        k = 0;
        line[0] = 0;
        for (int i = 0; i < d.nOut; ++i)
            k += std::snprintf(line + k, sizeof line - size_t(k), "%s%s", i ? ", " : "", d.out[i].name);
        g.color(col::kDim);
        g.text(26, y + 88, kSmall, "Out:", Canvas::kCenterY);
        g.color(col::kFg);
        g.text(70, y + 88, kSmall, d.nOut ? line : "-", Canvas::kCenterY);
        const float cost = float(addType_ == kPlts ? plaitsCost(8) : d.cost);
        const float used = float(eng_.cpuCost()) / float(kCpuBudget), add = cost / float(kCpuBudget);
        g.color(col::kDim);
        g.text(26, y + 118, kSmall, "DSP", Canvas::kCenterY);
        g.color(col::kRail);
        g.fill(70, y + 110, 400, 16);
        g.color(col::kOk);
        g.fill(70, y + 110, int(400 * clampf(used, 0, 1)), 16);
        g.color(used + add > 1.0f ? col::kWarn : col::kAccent);
        g.fill(70 + int(400 * clampf(used, 0, 1)), y + 110, int(400 * clampf(add, 0, 1 - clampf(used, 0, 1))), 16);
        g.color(col::kFg);
        g.textf(486, y + 118, kSmall, Canvas::kCenterY, "this module %.1f%%", double(add * 100.0f));
    }
    g.color(col::kDim);
    char where[24];
    if (addCell_ >= 0) std::snprintf(where, sizeof where, "cell %d", addCell_ + 1);
    else lab::copyStr(where, sizeof where, "a free cell");
    g.textf(26, 404, kTiny, Canvas::kCenterY,
            "Insert / F8 / double tap: add at %s.  Shift+F8: add and patch from the selected module.", where);
}

// ------------------------------------------------------------------ snapshots
void Graf::drawSnap(Canvas& g) {
    static const Rgb kSnapCol[kSnaps] = {Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0xFFD23A), Rgb(0xFF4FA0),
                                         Rgb(0x3CE07A), Rgb(0xA060F0), Rgb(0x20D0C0), Rgb(0xE0484E)};
    for (int i = 0; i < kSnaps; ++i) {
        const Snapshot& s = eng_.snap(i);
        const int x = 4 + i * 66, y = 40;
        const bool sel = i == snapSel_;
        g.color(sel ? col::kPanel2 : col::kPanel);
        g.fill(x, y, 62, 56);
        g.color(s.valid ? kSnapCol[i] : col::kFaint);
        g.fill(x, y, 62, 4);
        g.textf(x + 6, y + 18, kLarge, Canvas::kCenterY, "%d", i + 1);
        g.color(s.valid ? col::kDim : col::kFaint);
        if (s.valid) g.textf(x + 6, y + 42, kTiny, Canvas::kCenterY, "%dm %dc", s.nMod, s.nCab);
        else g.text(x + 6, y + 42, kTiny, "empty", Canvas::kCenterY);
        if (eng_.morphTarget() == i) {
            g.color(col::kAccent);
            g.fill(x, y + 52, int(62.0f * eng_.morphPos()), 4);
        }
        if (xfA_ == i || xfB_ == i) {
            g.color(col::kFg);
            g.text(x + 56, y + 18, kSmall, xfA_ == i ? "A" : "B", Canvas::kCenterY | Canvas::kRight);
        }
    }
    // pads 9-12 of row 1
    const char* tl[4] = {"XF on", "set A", "set B", "SONG"};
    for (int c = 0; c < 4; ++c) {
        const int x = 4 + (8 + c) * 66, y = 40;
        const bool on = (c == 0 && xfOn_) || (c == 3 && eng_.songOn);
        g.color(on ? col::kAccent : col::kPanel);
        g.fill(x, y, 62, 56);
        g.color(on ? col::kBg : col::kFg);
        g.text(x + 31, y + 28, kSmall, tl[c], Canvas::kCenter);
    }
    // timeline
    const int ty = 122, th = 56;
    const int bars = eng_.songBars();
    g.color(col::kDim);
    g.textf(8, ty - 12, kSmall, Canvas::kCenterY, "Song: %d steps, %d bars %s", eng_.songLen, bars,
            eng_.songOn ? "- PLAYING with the transport" : "- off (Song key / pad 12)");
    g.color(col::kPanel);
    g.fill(8, ty, 784, th);
    if (bars > 0) {
        const float px = 784.0f / float(bars);
        float x = 8.0f;
        for (int i = 0; i < eng_.songLen; ++i) {
            const SongStep& st = eng_.song[i];
            const int prev = i == 0 ? eng_.songLen - 1 : i - 1;
            const Rgb ca = kSnapCol[eng_.song[prev].snap & 7], cb = kSnapCol[st.snap & 7];
            const float wm = px * float(st.morphBars), wh = px * float(st.holdBars);
            for (int k = 0; k < 3 && wm > 1.0f; ++k) {
                g.color(scale(mix(ca, cb, (float(k) + 0.5f) / 3.0f), 0.55f));
                g.fill(int(x + wm * float(k) / 3.0f), ty + 8, int(wm / 3.0f) + 1, th - 16);
            }
            g.color(scale(cb, 0.85f));
            g.fill(int(x + wm), ty + 8, int(wh), th - 16);
            if (i == songSel_) {
                g.color(col::kFg);
                g.frame(int(x), ty + 2, int(wm + wh), th - 4);
            }
            if (wm + wh > 18.0f) {
                g.color(col::kBg);
                g.textf(int(x + wm + 4), ty + th / 2, kSmall, Canvas::kCenterY, "%d", st.snap + 1);
            }
            x += wm + wh;
        }
        if (ctx_->playing()) {
            double b = eng_.beat() * 0.25;
            b -= double(int32_t(b / double(bars))) * double(bars);
            g.color(col::kFg);
            g.fill(8 + int(float(b) * px), ty - 4, 2, th + 8);
        }
    }
    // fields: song step (left), crossfader (right)
    const SongStep& st = eng_.song[songSel_];
    char xa[8], xb[8];
    std::snprintf(xa, sizeof xa, "%d", xfA_ + 1);
    std::snprintf(xb, sizeof xb, "%d", xfB_ + 1);
    const char* labels[8] = {"Snapshot", "Morph bars", "Hold bars", "Song length", "XF A", "XF B", "X-fade", "XF on"};
    char vals[8][12];
    std::snprintf(vals[0], 12, "%d", st.snap + 1);
    std::snprintf(vals[1], 12, "%d", st.morphBars);
    std::snprintf(vals[2], 12, "%d", st.holdBars);
    std::snprintf(vals[3], 12, "%d", eng_.songLen);
    std::snprintf(vals[4], 12, "%s", xa);
    std::snprintf(vals[5], 12, "%s", xb);
    std::snprintf(vals[6], 12, "%d%%", int(xf_ * 100.0f + 0.5f));
    std::snprintf(vals[7], 12, "%s", xfOn_ ? "On" : "Off");
    g.color(col::kFg);
    g.textf(22, 198, kSmall, Canvas::kCenterY, "Song step %d", songSel_ + 1);
    g.color(col::kAccent);
    g.text(420, 198, kSmall, "A / B crossfader (a LABKIT parameter)", Canvas::kCenterY);
    for (int f = 0; f < 8; ++f) {
        const int x = f < 4 ? 8 : 408, y = 222 + (f % 4) * 28;
        if (f == songField_) {
            g.color(col::kPanel2);
            g.fill(x, y - 12, 380, 25);
            g.color(col::kAccent);
            g.fill(x, y - 12, 4, 25);
        }
        g.color(f == songField_ ? col::kFg : col::kDim);
        g.text(x + 14, y, kBody, labels[f], Canvas::kCenterY);
        g.color(f == 0 ? kSnapCol[st.snap & 7] : col::kFg);
        g.text(x + 300, y, kBody, vals[f], Canvas::kCenterY);
    }
    // the crossfader itself
    const int fy = 326;
    g.color(kSnapCol[xfA_ & 7]);
    g.textf(408, fy + 8, kBody, Canvas::kCenterY, "A%d", xfA_ + 1);
    g.color(kSnapCol[xfB_ & 7]);
    g.textf(782, fy + 8, kBody, Canvas::kCenterY | Canvas::kRight, "B%d", xfB_ + 1);
    g.color(col::kRail);
    g.fill(450, fy + 4, 290, 8);
    const float xe = xfId_ >= 0 ? ctx_->params().effective(xfId_) : xf_;
    g.color(xfOn_ ? col::kAccent : col::kDim);
    g.fill(450 + int(xe * 282.0f), fy - 4, 8, 24);
    g.color(col::kDim);
    g.text(8, 372, kTiny, "Pads 1-8: recall (Shift: store)   9: XF on   10/11: A / B = selected   12: song", Canvas::kCenterY);
    g.text(8, 392, kTiny, "Rows 2-4: song steps (Shift: set to the selected snapshot)   Up/Down: field   encoder: value",
           Canvas::kCenterY);
    g.text(8, 412, kTiny, "Insert: song on / off   Delete: remove the step   F8: store", Canvas::kCenterY);
}

// ------------------------------------------------------------------ play
void Graf::drawPlay(Canvas& g) {
    // which strips have something on them
    bool live[kPatches] = {};
    for (int s = 0; s < kMaxSlots; ++s) {
        const Module& m = eng_.mod(s);
        if (!m.used) continue;
        if (eng_.rack) {
            if (m.type == kOut) live[(m.number - 1) & 7] = true;
        } else {
            live[m.patch] = true;
        }
    }
    for (int i = 0; i < kPatches; ++i) {
        const int x = 8 + i * 98, y = 40;
        const bool sel = i == stripSel_;
        const MixStrip& ms = eng_.mix[i];
        char name[20];
        std::snprintf(name, sizeof name, eng_.rack ? "OUT%d" : "PATCH %d", (i & 7) + 1);
        const float meter = meterShow_[i];
        g.color(sel ? col::kPanel2 : col::kPanel);
        g.fill(x, y, 92, 220);
        g.color(!eng_.rack && i == patch_ ? col::kSig[3] : (live[i] ? col::kFg : col::kFaint));
        g.text(x + 46, y + 14, kSmall, name, Canvas::kCenter);
        g.color(col::kRail);
        g.fill(x + 14, y + 34, 14, 150);
        g.color(ms.mute ? col::kFaint : (meter > 0.95f ? col::kWarn : col::kOk));
        const int mh = int(150.0f * clampf(meter, 0.0f, 1.0f));
        g.fill(x + 14, y + 184 - mh, 14, mh);
        g.color(col::kRail);
        g.fill(x + 52, y + 34, 6, 150);
        g.color(sel && stripField_ == 0 ? col::kAccent : col::kFg);
        g.fill(x + 42, y + 180 - int(146.0f * clampf(ms.level, 0.0f, 1.0f)), 26, 8);
        g.color(col::kRail);
        g.fill(x + 10, y + 198, 72, 4);
        g.color(sel && stripField_ == 1 ? col::kAccent : col::kFg);
        g.fill(x + 44 + int(ms.pan * 34.0f), y + 193, 4, 14);
        if (ms.mute) {
            g.color(col::kWarn);
            g.text(x + 46, y + 120, kBody, "MUTE", Canvas::kCenter);
        }
    }
    drawScope(g, 8, 272, 500, 140, col::kSig[0]);
    g.color(col::kDim);
    g.text(520, 284, kSmall, "Pads rows 2-4: keyboard", Canvas::kCenterY);
    g.textf(520, 306, kSmall, Canvas::kCenterY, "Octave %+d (pads 11 / 12 of row 1)", prefOctave_);
    g.text(520, 328, kSmall, "Row 1: strip (Shift: mute)", Canvas::kCenterY);
    g.text(520, 350, kSmall, "Encoder: level / pan (Up/Down)", Canvas::kCenterY);
    g.text(520, 372, kSmall, "Insert: mute  Delete: reset", Canvas::kCenterY);
    g.text(520, 394, kSmall, eng_.rack ? "Strip n = OUTn. MIDI: any channel" : "Strip n = patch n = MIDI ch n",
           Canvas::kCenterY);
    int held = 0;
    for (int p = 0; p < lab::kPads; ++p)
        if (heldNote_[p] >= 0) ++held;
    g.color(held ? col::kSig[3] : col::kFaint);
    g.textf(520, 418, kSmall, Canvas::kCenterY, "%d key%s down", held, held == 1 ? "" : "s");
}

// ------------------------------------------------------------------ file
void Graf::drawFile(Canvas& g) {
    g.color(col::kDim);
    g.text(8, 48, kSmall, ctx_->fs ? "Patches on the card: /Lab/GRAF/Patches" : "No SD card: saving is off",
           Canvas::kCenterY);
    for (int i = 0; i < 24; ++i) {
        const int x = 8 + (i % 12) * 66, y = 64 + (i / 12) * 54;
        const bool sel = i == fileSel_ && !demoPicked_;
        g.color(sel ? col::kAccent : (fileExists_[i] ? col::kPanel2 : col::kPanel));
        g.fill(x, y, 62, 48);
        g.color(sel ? col::kBg : (fileExists_[i] ? col::kFg : col::kFaint));
        g.textf(x + 31, y + 24, kBody, Canvas::kCenter, "P%02d", i + 1);
    }
    g.color(col::kDim);
    g.text(8, 192, kSmall, "Demos (pad row 4):", Canvas::kCenterY);
    for (int i = 0; i < demoCount(); ++i) {
        const int x = 8 + i * 98, y = 208;
        const bool sel = i == demoSel_ && demoPicked_;
        g.color(sel ? col::kSig[1] : col::kPanel);
        g.fill(x, y, 94, 36);
        g.color(sel ? col::kBg : col::kFg);
        char nm[16];
        kit::fitText(nm, sizeof nm, kTiny, demoName(i) + 6, 90);
        g.text(x + 47, y + 18, kTiny, nm, Canvas::kCenter);
    }
    const int y = 270;
    g.color(col::kFg);
    if (demoPicked_) g.textf(8, y, kSmall, Canvas::kCenterY, "Selected: %s", demoName(demoSel_));
    else g.textf(8, y, kSmall, Canvas::kCenterY, "Selected: P%02d  %s", fileSel_ + 1, fileExists_[fileSel_] ? "(saved)" : "(empty)");
    g.color(col::kDim);
    g.text(8, y + 26, kSmall, "Insert: load the selected slot or demo     F8: save here (twice to overwrite)", Canvas::kCenterY);
    g.text(8, y + 48, kSmall, "Delete twice: new empty patch     Rec: rescan /Lab/GRAF/Samples", Canvas::kCenterY);
    g.textf(8, y + 78, kSmall, Canvas::kCenterY, "%d samples on the card.  Autosave: /Lab/GRAF/autosave.grf",
            eng_.sampleFileCount());
    g.textf(8, y + 100, kSmall, Canvas::kCenterY, "This patch: %d modules, %d cables, %d LABKIT routes, model %s",
            eng_.moduleCount(), eng_.cableCount(), ctx_->mod().routeCount(), eng_.rack ? "Rack" : "8 patches");
    g.textf(8, y + 122, kSmall, Canvas::kCenterY, "Undo: %d steps  (Pattern; Shift + Pattern: redo %d)", nUndo_, nRedo_);
}

// ------------------------------------------------------------------ LEDs
void Graf::leds(Rgb* pads) {
    const uint32_t ms = ctx_->millis();
    const bool blink = (ms / 250) & 1;
    for (int i = 0; i < lab::kPads; ++i) pads[i] = Rgb();
    auto keyLeds = [&](int fromRow) {
        for (int p = fromRow * 12; p < lab::kPads; ++p) {
            const int n = keyNote(p);
            if (n < 0) continue;
            const int pc = n % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            pads[p] = heldNote_[p] >= 0 ? Rgb(0xFF4FA0) : (pc == 0 ? Rgb(0x402810) : (black ? Rgb(0x040406) : Rgb(0x141418)));
        }
    };
    switch (page_) {
        case kPgGraph: {
            int up[8], dn[8], nu = 0, nd = 0;
            if (sel_ >= 0 && eng_.mod(sel_).used) {
                nu = eng_.upstream(sel_, up, 8);
                nd = eng_.downstream(sel_, dn, 8);
            }
            for (int c = 0; c < kCells; ++c) {
                const int s = eng_.slotAtCell(patch_, c);
                if (s < 0) continue;
                pads[c] = scale(catCol(def(eng_.mod(s).type).cat), s == sel_ ? 1.0f : 0.22f);
                for (int k = 0; k < nu; ++k)
                    if (up[k] == s) pads[c] = scale(col::kSig[1], 0.6f);
                for (int k = 0; k < nd; ++k)
                    if (dn[k] == s) pads[c] = scale(col::kSig[0], 0.6f);
            }
            if (eng_.slotAtCell(patch_, cursor_) < 0 && blink) pads[cursor_] = Rgb(0x303030);
            if (holdCell_ >= 0) {   // patch targets blink
                for (int c = 0; c < kCells; ++c)
                    if (c != holdCell_ && eng_.slotAtCell(patch_, c) >= 0 && blink) pads[c] = Rgb(0x804000);
                pads[holdCell_] = Rgb(0xFFFFFF);
            }
            break;
        }
        case kPgMatrix:
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 12; ++c) {
                    const int R = mRow_ + r, C = mCol_ + c;
                    if (R >= nSrc_ || C >= nDst_) continue;
                    const int k = matrixCable(R, C);
                    const int p = lab::padAt(c, r);
                    if (k < 0) {
                        pads[p] = srcs_[R].slot == dsts_[C].slot ? Rgb(0x080000) : Rgb(0x060606);
                        continue;
                    }
                    const Cable& cb = eng_.cable(k);
                    const int sig = def(eng_.mod(cb.src).type).out[cb.srcOut].sig;
                    pads[p] = scale(sigCol(sig), 0.25f + 0.75f * clampf(fabsf(cb.amount), 0.0f, 1.0f));
                    if (k == selCable_ && blink) pads[p] = Rgb(0xFFFFFF);
                }
            break;
        case kPgModule: {
            if (sel_ < 0 || !eng_.mod(sel_).used) break;
            const Module& m = eng_.mod(sel_);
            const ModDef& d = def(m.type);
            if (m.type == kPinb) {
                for (int c = 0; c < kCells; ++c)
                    pads[c] = (m.ext[c] & 2) ? Rgb(0x601030) : ((m.ext[c] & 1) ? Rgb(0x103050) : Rgb());
                for (int b = 0; b < 3; ++b) {
                    const int c = pinbBall(m, b);
                    if (c >= 0) pads[c] = Rgb(0xFFFFFF);
                }
                break;
            }
            for (int p = 0; p < d.nParam; ++p)   // brightness = the knob's value
                pads[p] = p == param_ ? Rgb(0xFFFFFF) : scale(catCol(d.cat), 0.12f + 0.75f * m.k[p]);
            pads[10] = Rgb(0x0A2838);
            pads[11] = Rgb(0x381A08);
            if (m.type == kMgate) {
                const bool on = eng_.manualGate || m.ext[0];
                for (int p = 12; p < lab::kPads; ++p) pads[p] = on ? Rgb(0xFFB000) : Rgb(0x201400);
                break;
            }
            keyLeds(1);
            if (m.type == kSeq) {
                const int pos = seqPosition(m), steps = int(m.v[0]);
                for (int i = 0; i < 16; ++i) {
                    const int p = 24 + (i / 8) * 12 + (i % 8);
                    pads[p] = i >= steps ? Rgb() : (seqGate(m, i) ? scale(catCol(d.cat), 0.6f) : Rgb(0x080808));
                    if (i == pos) pads[p] = Rgb(0xFFFFFF);
                }
            }
            break;
        }
        case kPgAdd: {
            for (int c = 0; c <= kCatCount; ++c)
                pads[c] = c == addCat_ ? Rgb(0xFFFFFF) : scale(c < kCatCount ? catCol(c) : col::kFg, 0.3f);
            for (int c = 9; c < 12; ++c) pads[c] = scale(catCol(kDefs[recent_[c - 9]].cat), 0.5f);
            const int n = addOrderCount(addCat_);
            for (int i = 0; i < n && i < 36; ++i) {
                if (addCat_ >= kCatCount) {
                    pads[12 + i] = i == addTpl_ ? Rgb(0xFFFFFF) : Rgb(0x202020);
                    continue;
                }
                const int t = addTypeAt(addCat_, i);
                pads[12 + i] = t == addType_ ? Rgb(0xFFFFFF) : scale(catCol(def(t).cat), eng_.fits(def(t).cost) ? 0.4f : 0.05f);
            }
            break;
        }
        case kPgSnap: {
            static const Rgb kSnapCol[kSnaps] = {Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0xFFD23A), Rgb(0xFF4FA0),
                                                 Rgb(0x3CE07A), Rgb(0xA060F0), Rgb(0x20D0C0), Rgb(0xE0484E)};
            for (int i = 0; i < kSnaps; ++i) {
                pads[i] = eng_.snap(i).valid ? scale(kSnapCol[i], i == snapSel_ ? 1.0f : 0.3f) : Rgb(0x040404);
                if (eng_.morphTarget() == i && blink) pads[i] = Rgb(0xFFFFFF);
            }
            pads[8] = xfOn_ ? Rgb(0xFFB000) : Rgb(0x201400);
            pads[9] = scale(kSnapCol[xfA_ & 7], 0.5f);
            pads[10] = scale(kSnapCol[xfB_ & 7], 0.5f);
            pads[11] = eng_.songOn ? Rgb(0x00FF40) : Rgb(0x300000);
            float t = 0;
            const int cur = (eng_.songOn && ctx_->playing()) ? eng_.songStepAt(eng_.beat(), &t, nullptr) : -1;
            for (int i = 0; i < kSongSteps; ++i) {
                const int p = 12 + i;
                if (i >= eng_.songLen) continue;
                pads[p] = scale(kSnapCol[eng_.song[i].snap & 7], i == songSel_ ? 0.9f : 0.2f);
                if (i == cur) pads[p] = Rgb(0xFFFFFF);
            }
            break;
        }
        case kPgPlay:
            for (int i = 0; i < kPatches; ++i)
                pads[i] = eng_.mix[i].mute ? Rgb(0x300000) : (i == stripSel_ ? Rgb(0x38C8FF) : Rgb(0x081820));
            if (!eng_.rack) pads[patch_] = eng_.mix[patch_].mute ? Rgb(0x600010) : Rgb(0xFF4FA0);
            pads[10] = pads[11] = Rgb(0x303030);
            keyLeds(1);
            break;
        case kPgFile:
            for (int i = 0; i < 24; ++i)
                pads[i] = (i == fileSel_ && !demoPicked_) ? Rgb(0xFFFFFF) : (fileExists_[i] ? Rgb(0x403000) : Rgb(0x040404));
            for (int i = 0; i < demoCount(); ++i) pads[36 + i] = (i == demoSel_ && demoPicked_) ? Rgb(0x38C8FF) : Rgb(0x082030);
            break;
        default: break;
    }
}

}  // namespace graf
