// GRAF -- drawing: intro, the seven pages, pad LEDs.
//
// Look: a dark instrument panel. Modules are boxes on a 12 x 4 grid (the pads),
// coloured by category; cables sag between output jacks (right edge) and
// input jacks (left edge), coloured by signal: orange audio, blue CV, yellow
// gate, pink pitch. Every page keeps under the 1,800-word display budget:
// dense canvases switch to flat boxes and straight cables.
#include <cstdio>
#include <cstring>

#include "ws/graf_ws.h"

namespace graf {

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
    // patch / canvas
    char b[32];
    const int pn = (patch_ & 7) + 1;
    if (eng_.rack) std::snprintf(b, sizeof b, "RACK  canvas %d", pn);
    else std::snprintf(b, sizeof b, "PATCH %d  ch %d", pn, pn);
    g.color(eng_.rack ? col::kSig[1] : col::kSig[3]);
    g.text(270, 16, kSmall, b, Canvas::kCenterY);
    // module CPU budget bar
    const float load = float(eng_.cpuCost()) / float(kCpuBudget);
    g.color(col::kDim);
    g.text(436, 16, kTiny, "DSP", Canvas::kCenterY);
    g.color(col::kRail);
    g.fill(466, 11, 80, 10);
    g.color(load > 0.9f ? col::kWarn : (load > 0.7f ? col::kAccent : col::kOk));
    g.fill(466, 11, int(80.0f * clampf(load, 0.0f, 1.0f)), 10);
    g.color(col::kDim);
    g.textf(556, 16, kTiny, Canvas::kCenterY, "%d/%d", eng_.moduleCount(), kMaxSlots);
    // song / morph state
    if (eng_.songOn) {
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
    // master meter
    g.color(col::kRail);
    g.fill(752, 12, 40, 8);
    g.color(masterShow_ > 0.95f ? col::kWarn : col::kOk);
    g.fill(752, 12, int(40.0f * clampf(masterShow_, 0.0f, 1.0f)), 8);
}

void Graf::drawScope(Canvas& g, int x, int y, int w, int h, Rgb c) {
    g.color(col::kPanel);
    g.fill(x, y, w, h);
    g.color(col::kRail);
    g.hline(x, y + h / 2, w);
    g.color(c);
    g.lineWidth(1.5f);
    g.beginStrip();
    const float my = float(y + h / 2), a = float(h) * 0.45f;
    for (int i = 0; i < 64; ++i)
        g.vtx(float(x) + float(i) * float(w - 1) / 63.0f, my - clampf(eng_.scope[i * 2], -1.1f, 1.1f) * a);
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
    if (page_ == kPgAdd) f8 = "Add";
    if (page_ == kPgSnap) f8 = "Store";
    if (page_ == kPgFile) f8 = "Save";
    const char* keys[8] = {"Graph", "Matrix", "Module", "Add", "Snaps", "Play", "File", f8};
    lab::drawFKeyBar(g, theme_, keys, page_);
}

// ------------------------------------------------------------------ graph
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
    // info strip (bottom)
    g.color(col::kPanel);
    g.fill(0, 422, 800, 26);
    if (sel_ >= 0 && eng_.mod(sel_).used) {
        const Module& m = eng_.mod(sel_);
        const ModDef& d = def(m.type);
        g.color(catCol(d.cat));
        g.textf(10, 435, kSmall, Canvas::kCenterY, "%s%d  %s", d.tag, m.number, d.title);
        g.color(col::kDim);
        g.text(230, 435, kTiny, d.about, Canvas::kCenterY);
    } else {
        g.color(col::kDim);
        g.text(10, 435, kTiny,
               "Pad: select   hold A + tap B: patch A > B   Shift+pad: move   Insert / 2nd tap: add   Delete: remove",
               Canvas::kCenterY);
    }
    // cables touching this canvas
    int cab[kMaxCables], nc = 0;
    for (int c = 0; c < kMaxCables; ++c) {
        const Cable& k = eng_.cable(c);
        if (!k.used) continue;
        if (eng_.mod(k.src).patch == vp || eng_.mod(k.dst).patch == vp) cab[nc++] = c;
    }
    const uint32_t modCost = uint32_t(n) * (dense ? 20u : 34u) + 40u;
    const bool sag = prefSag_ && g.words() + modCost + uint32_t(nc) * 13u + 60u < lab::Canvas::kBudgetWords;
    // cables, grouped by signal colour
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
            if (!srcHere) {   // from another canvas: a stub to the left
                x0 = x1 - 14.0f;
                y0 = y1 - 8.0f;
            }
            if (!dstHere) {
                x1 = x0 + 14.0f;
                y1 = y0 + 8.0f;
            }
            const float a = clampf(fabsf(k.amount), 0.0f, 1.0f);
            const bool hi = cab[i] == selCable_ || (sel_ >= 0 && (k.src == sel_ || k.dst == sel_));
            const Rgb c = mix(col::kBg, sigCol(sig), hi ? 1.0f : 0.35f + 0.5f * a);
            if (sag && srcHere && dstHere) {
                g.color(c);
                g.lineWidth(hi ? 2.5f : 1.8f);
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
        if (isSel) {
            g.color(col::kAccent);
            g.fillRound(bx - 3, by - 3, kBoxW + 6, kBoxH + 6, 7);
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
        if (!dense && d.nOut) {   // output activity (the first patched output)
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
    // cursor on an empty cell / held source
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
    if (n == 0) {
        g.color(col::kDim);
        g.text(400, 200, kBody, "Empty canvas. Press Insert (or F4) to add a module.", Canvas::kCenter);
    }
}

// ------------------------------------------------------------------ matrix
void Graf::drawMatrix(Canvas& g) {
    constexpr int gx = 160, gy = 112, cw = 53, ch = 52;
    const int r1 = mRow_ + 4 < nSrc_ ? mRow_ + 4 : nSrc_;
    const int c1 = mCol_ + 12 < nDst_ ? mCol_ + 12 : nDst_;
    // column heads: destinations (module, input)
    int lastSlot = -1;
    for (int c = mCol_; c < c1; ++c) {
        const Jack& j = dsts_[c];
        const Module& m = eng_.mod(j.slot);
        const ModDef& d = def(m.type);
        const int x = gx + (c - mCol_) * cw + cw / 2;
        if (j.slot != lastSlot) {
            char l[12];
            modLabel(l, sizeof l, j.slot);
            g.color(catCol(d.cat));
            g.fill(x - cw / 2 + 2, 42, 3, 64);
            g.text(x - cw / 2 + 8, 52, kTiny, l, Canvas::kCenterY);
            lastSlot = j.slot;
        }
        g.color(sigCol(d.in[j.jack].sig));
        g.text(x, 96, kTiny, d.in[j.jack].name, Canvas::kCenter);
    }
    g.color(col::kDim);
    g.text(10, 52, kTiny, "out \\ in", Canvas::kCenterY);
    g.textf(10, 72, kTiny, Canvas::kCenterY, "rows %d-%d / %d", nSrc_ ? mRow_ + 1 : 0, r1, nSrc_);
    g.textf(10, 90, kTiny, Canvas::kCenterY, "cols %d-%d / %d", nDst_ ? mCol_ + 1 : 0, c1, nDst_);
    // row heads: sources
    for (int r = mRow_; r < r1; ++r) {
        const Jack& j = srcs_[r];
        const Module& m = eng_.mod(j.slot);
        const ModDef& d = def(m.type);
        const int y = gy + (r - mRow_) * ch + ch / 2;
        char l[12];
        modLabel(l, sizeof l, j.slot);
        g.color(catCol(d.cat));
        g.fill(6, y - ch / 2 + 4, 3, ch - 8);
        g.text(14, y - 9, kTiny, l, Canvas::kCenterY);
        g.color(sigCol(d.out[j.jack].sig));
        g.text(14, y + 9, kSmall, d.out[j.jack].name, Canvas::kCenterY);
        if (eng_.rack && m.patch != patch_) {
            g.color(col::kDim);
            g.textf(150, y, kTiny, Canvas::kCenterY | Canvas::kRight, "c%d", m.patch + 1);
        }
    }
    // grid
    g.color(col::kPanel);
    g.fill(gx, gy, cw * 12, ch * 4);
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
    constexpr int ox = 10, oy = 336, ow = 230, oh = 80;
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
    // selected cable
    g.color(col::kPanel);
    g.fill(250, 336, 540, 80);
    if (selCable_ >= 0 && eng_.cable(selCable_).used) {
        const Cable& cb = eng_.cable(selCable_);
        char a[28], b[28];
        jackLabel(a, sizeof a, Jack{cb.src, cb.srcOut}, true);
        jackLabel(b, sizeof b, Jack{cb.dst, cb.dstIn}, false);
        const int sig = def(eng_.mod(cb.src).type).out[cb.srcOut].sig;
        g.color(sigCol(sig));
        g.textf(262, 352, kBody, Canvas::kCenterY, "%s  >  %s", a, b);
        g.color(col::kRail);
        g.fill(262, 376, 400, 12);
        g.color(col::kFg);
        g.vline(462, 372, 20);
        g.color(sigCol(sig));
        const float am = clampf(cb.amount, -2.0f, 2.0f) * 100.0f;
        if (am >= 0) g.fill(462, 376, int(am), 12);
        else g.fill(462 + int(am), 376, int(-am), 12);
        g.color(col::kFg);
        g.textf(680, 382, kBody, Canvas::kCenterY, "%+.2f", double(cb.amount));
        g.color(col::kDim);
        g.text(262, 404, kTiny, "Encoder: amount (Shift fine)   Push: +1 / -1   Delete: remove", Canvas::kCenterY);
    } else {
        g.color(col::kDim);
        g.text(262, 356, kSmall, "Pads = pins: rows are outputs, columns are inputs.", Canvas::kCenterY);
        g.text(262, 378, kSmall, "Tap a pin to patch, tap a lit pin to unpatch, hold + turn: amount.", Canvas::kCenterY);
        g.text(262, 400, kSmall, "Arrows scroll (Shift: a page at a time).", Canvas::kCenterY);
    }
    if (!nSrc_ || !nDst_) {
        g.color(col::kDim);
        g.text(480, 210, kBody, "Nothing to patch on this canvas yet", Canvas::kCenter);
    }
}

// ------------------------------------------------------------------ module
void Graf::drawKnob(Canvas& g, int x, int y, int slot, int p, bool sel) {
    const Module& m = eng_.mod(slot);
    const ModDef& d = def(m.type);
    constexpr float r = 22.0f, a0 = 2.356f, span = 4.712f;
    g.color(sel ? col::kPanel2 : col::kPanel);
    g.fill(x - 46, y - 32, 92, 92);
    g.color(col::kRail);
    g.arc(float(x), float(y), r, a0, a0 + span, 10);
    const float k = m.k[p];
    const Rgb c = sel ? col::kAccent : catCol(d.cat);
    g.color(c);
    g.lineWidth(3.0f);
    g.arc(float(x), float(y), r, a0, a0 + span * (k > 0.01f ? k : 0.01f), 1 + int(k * 10.0f));
    g.lineWidth(1.0f);
    g.point(float(x) + r * 0.6f * cosf(a0 + span * k), float(y) + r * 0.6f * sinf(a0 + span * k), 3.0f);
    g.color(sel ? col::kFg : col::kDim);
    g.text(x, y + 34, kTiny, d.p[p].name, Canvas::kCenter);
    char v[24];
    fmtParam(v, sizeof v, slot, p);
    g.color(sel ? col::kAccent : col::kFg);
    g.text(x, y + 50, kSmall, v, Canvas::kCenter);
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
    g.fill(0, 34, 6, 50);
    g.textf(16, 48, kLarge, Canvas::kCenterY, "%s%d", d.tag, m.number);
    g.color(col::kFg);
    g.text(110, 48, kBody, d.title, Canvas::kCenterY);
    g.color(col::kDim);
    g.text(16, 72, kTiny, d.about, Canvas::kCenterY);
    // knobs, 5 per row
    for (int p = 0; p < d.nParam; ++p) {
        const int x = 56 + (p % 5) * 100, y = 128 + (p / 5) * 110;
        drawKnob(g, x, y, sel_, p, p == param_);
    }
    // right panel: scope + jacks
    const int px = 520;
    const int so = eng_.scopeOut;
    drawScope(g, px, 40, 270, 70, d.nOut ? sigCol(d.out[so < d.nOut ? so : 0].sig) : col::kDim);
    g.color(col::kDim);
    if (d.nOut) g.textf(px + 4, 48, kTiny, Canvas::kCenterY, "scope: %s (Fx2)", d.out[so < d.nOut ? so : 0].name);
    int y = 124;
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
            const size_t l = std::strlen(buf);
            std::snprintf(buf + l, size_t(cap) - l, " +%d", count - 1);
        }
    };
    char b[40];
    for (int i = 0; i < d.nIn; ++i, y += 18) {
        conn(false, i, b, sizeof b);
        g.color(sigCol(d.in[i].sig));
        g.text(px, y, kTiny, d.in[i].name, Canvas::kCenterY);
        g.color(b[0] ? col::kFg : col::kFaint);
        g.text(px + 64, y, kTiny, b[0] ? b : "-", Canvas::kCenterY);
    }
    y += 4;
    for (int i = 0; i < d.nOut; ++i, y += 18) {
        conn(true, i, b, sizeof b);
        g.color(sigCol(d.out[i].sig));
        g.textf(px, y, kTiny, Canvas::kCenterY, "%s >", d.out[i].name);
        g.color(b[0] ? col::kFg : col::kFaint);
        g.text(px + 64, y, kTiny, b[0] ? b : "-", Canvas::kCenterY);
    }
    // module views
    const int vy = 352;
    switch (m.type) {
        case kSeq: {
            const int pos = seqPosition(m), steps = int(m.v[0]);
            for (int i = 0; i < 16; ++i) {
                const int x = 16 + i * 31;
                const bool on = seqGate(m, i);
                const int n = seqNote(m, i);
                g.color(i == pos ? col::kFg : (i < steps ? (on ? catCol(d.cat) : col::kRail) : col::kPanel));
                g.fill(x, vy + 50 - (on ? (n + 14) * 2 : 2), 26, on ? (n + 14) * 2 + 2 : 4);
            }
            g.color(col::kDim);
            g.text(520, vy + 56, kTiny, "steps: pads rows 3-4 / hold + turn", Canvas::kCenterY);
            break;
        }
        case kPinb: {
            for (int c = 0; c < kCells; ++c) {
                const int x = 16 + (c % 12) * 40, yy = vy - 20 + (c / 12) * 18;
                g.color((m.ext[c] & 2) ? col::kSig[3] : ((m.ext[c] & 1) ? catCol(d.cat) : col::kRail));
                g.fill(x, yy, 36, 15);
            }
            g.color(col::kFg);
            for (int b2 = 0; b2 < 3; ++b2) {
                const int c = pinbBall(m, b2);
                if (c >= 0) g.point(float(16 + (c % 12) * 40 + 18), float(vy - 20 + (c / 12) * 18 + 7), 6);
            }
            g.color(col::kDim);
            g.text(520, vy + 56, kTiny, "pads: notes, Shift: bumpers, Rec: new", Canvas::kCenterY);
            break;
        }
        case kMarkov: {
            const int n = int(m.v[0]), st = markovState(m);
            for (int i = 0; i < n; ++i) {
                const float a = kTwoPi * float(i) / float(n);
                const float x = 260 + 120 * cosf(a), yy = float(vy + 20) + 30 * sinf(a);
                g.color(i == st ? col::kFg : catCol(d.cat));
                g.point(x, yy, i == st ? 10 : 6);
            }
            break;
        }
        case kTuring: {
            const uint32_t reg = turingRegister(m);
            const int len = int(m.v[1]);
            for (int i = 0; i < 16; ++i) {
                g.color(i < len ? (((reg >> i) & 1) ? catCol(d.cat) : col::kRail) : col::kPanel);
                g.fill(16 + i * 31, vy + 10, 26, 26);
            }
            break;
        }
        case kEucl: {
            const int steps = int(m.v[0]), hits = int(m.v[1]), rot = int(m.v[2]), pos = euclStep(m);
            for (int i = 0; i < steps; ++i) {
                const float a = kTwoPi * float(i) / float(steps) - 1.5708f;
                const bool h = lab::dsp::euclid(i, steps, hits, rot);
                g.color(i == pos ? col::kFg : (h ? catCol(d.cat) : col::kRail));
                g.point(260 + 34 * cosf(a) * 3.0f, float(vy + 20) + 34 * sinf(a), h ? 6 : 3);
            }
            break;
        }
        case kRete: {
            for (int i = 0; i < 4; ++i) {
                const float a = reteNode(m, i);
                const float x = 70.0f + float(i) * 120.0f;
                g.color(mix(col::kRail, catCol(d.cat), clampf(a * 3.0f, 0.0f, 1.0f)));
                g.point(x, float(vy + 20), 8.0f + 18.0f * clampf(a * 3.0f, 0.0f, 1.0f));
            }
            g.color(col::kDim);
            g.text(520, vy + 56, kTiny, "Gate or Rec re-rolls the net", Canvas::kCenterY);
            break;
        }
        case kSmpl: {
            g.color(col::kFg);
            g.textf(16, vy + 10, kSmall, Canvas::kCenterY, "%s  %.2f s %s", eng_.sampleName[sel_][0] ? eng_.sampleName[sel_] : "-",
                    double(float(eng_.sampleFrames[sel_]) / 44100.0f), eng_.sampleStereo[sel_] ? "stereo" : "mono");
            g.color(col::kDim);
            g.text(16, vy + 34, kTiny, "WAVs go in /Lab/GRAF/Samples (File page, Rec rescans)", Canvas::kCenterY);
            break;
        }
        default: break;
    }
    if (m.type != kSeq && m.type != kPinb && m.type != kRete && m.type != kSmpl) drawKeys(g);
}

void Graf::drawKeys(Canvas& g) {
    // small keyboard legend for pad rows 2-4
    g.color(col::kDim);
    g.textf(520, 410, kTiny, Canvas::kCenterY, "pads rows 2-4: keys C%d..  (oct %+d)", keyBase() / 12 - 1, prefOctave_);
}

// ------------------------------------------------------------------ add
void Graf::drawAdd(Canvas& g) {
    for (int i = 0; i < kTypeCount; ++i) {
        const int t = addOrder_[i];
        const ModDef& d = def(t);
        const int x = 8 + (i % 12) * 66, y = 40 + (i / 12) * 44;
        const bool sel = t == addType_;
        const bool ok = eng_.fits(d.cost);
        g.color(sel ? catCol(d.cat) : col::kPanel);
        g.fill(x, y, 62, 40);
        if (!sel) {
            g.color(catCol(d.cat));
            g.fill(x, y, 62, 3);
        }
        g.color(sel ? col::kBg : (ok ? col::kFg : col::kFaint));
        g.text(x + 31, y + 22, kSmall, d.tag, Canvas::kCenter);
    }
    const ModDef& d = def(addType_);
    const int y = 230;
    g.color(catCol(d.cat));
    g.fill(8, y, 6, 180);
    g.text(26, y + 14, kLarge, d.title, Canvas::kCenterY);
    g.color(col::kFg);
    g.text(26, y + 42, kSmall, d.about, Canvas::kCenterY);
    char line[96];
    int n = 0;
    line[0] = 0;
    for (int i = 0; i < d.nIn; ++i)
        n += std::snprintf(line + n, sizeof line - size_t(n), "%s%s", i ? ", " : "", d.in[i].name);
    g.color(col::kDim);
    g.text(26, y + 72, kSmall, "Inputs:", Canvas::kCenterY);
    g.color(col::kFg);
    g.text(110, y + 72, kSmall, d.nIn ? line : "-", Canvas::kCenterY);
    n = 0;
    line[0] = 0;
    for (int i = 0; i < d.nOut; ++i)
        n += std::snprintf(line + n, sizeof line - size_t(n), "%s%s", i ? ", " : "", d.out[i].name);
    g.color(col::kDim);
    g.text(26, y + 96, kSmall, "Outputs:", Canvas::kCenterY);
    g.color(col::kFg);
    g.text(110, y + 96, kSmall, d.nOut ? line : "-", Canvas::kCenterY);
    // CPU cost against what is left
    const float used = float(eng_.cpuCost()) / float(kCpuBudget), add = float(d.cost) / float(kCpuBudget);
    g.color(col::kDim);
    g.text(26, y + 128, kSmall, "DSP budget", Canvas::kCenterY);
    g.color(col::kRail);
    g.fill(130, y + 120, 400, 16);
    g.color(col::kOk);
    g.fill(130, y + 120, int(400 * clampf(used, 0, 1)), 16);
    g.color(used + add > 1.0f ? col::kWarn : col::kAccent);
    g.fill(130 + int(400 * clampf(used, 0, 1)), y + 120, int(400 * clampf(add, 0, 1 - clampf(used, 0, 1))), 16);
    g.color(col::kFg);
    g.textf(546, y + 128, kSmall, Canvas::kCenterY, "this module %.1f%%", double(add * 100.0f));
    g.color(col::kDim);
    char where[24];
    if (addCell_ >= 0) std::snprintf(where, sizeof where, "cell %d", addCell_ + 1);
    else lab::copyStr(where, sizeof where, "first free cell");
    g.textf(26, y + 160, kTiny, Canvas::kCenterY,
            "Pads / encoder / arrows pick a module. F8, Push or a double tap adds it at %s.", where);
}

// ------------------------------------------------------------------ snapshots
void Graf::drawSnap(Canvas& g) {
    static const Rgb kSnapCol[kSnaps] = {Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0xFFD23A), Rgb(0xFF4FA0),
                                         Rgb(0x3CE07A), Rgb(0xA060F0), Rgb(0x20D0C0), Rgb(0xE0484E)};
    for (int i = 0; i < kSnaps; ++i) {
        const Snapshot& s = eng_.snap(i);
        const int x = 8 + i * 98, y = 40;
        const bool sel = i == snapSel_;
        g.color(sel ? col::kPanel2 : col::kPanel);
        g.fill(x, y, 92, 64);
        g.color(s.valid ? kSnapCol[i] : col::kFaint);
        g.fill(x, y, 92, 4);
        g.textf(x + 8, y + 20, kLarge, Canvas::kCenterY, "%d", i + 1);
        if (s.valid) {
            g.color(col::kDim);
            g.textf(x + 8, y + 46, kTiny, Canvas::kCenterY, "%dm %dc", s.nMod, s.nCab);
        } else {
            g.color(col::kFaint);
            g.text(x + 8, y + 46, kTiny, "empty", Canvas::kCenterY);
        }
        if (eng_.morphTarget() == i) {
            g.color(col::kAccent);
            g.fill(x, y + 58, int(92.0f * eng_.morphPos()), 6);
        }
    }
    // timeline
    const int ty = 140, th = 64;
    const int bars = eng_.songBars();
    g.color(col::kDim);
    g.textf(8, ty - 16, kSmall, Canvas::kCenterY, "Song: %d steps, %d bars   %s", eng_.songLen, bars,
            eng_.songOn ? "PLAYING with the transport" : "off (pad 12 / Push)");
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
            // morph part: three bands blending from the previous snapshot
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
    // selected step
    const SongStep& st = eng_.song[songSel_];
    const char* labels[4] = {"Snapshot", "Morph bars", "Hold bars", "Song length"};
    const int vals[4] = {st.snap + 1, st.morphBars, st.holdBars, eng_.songLen};
    for (int f = 0; f < 4; ++f) {
        const int y = 236 + f * 30;
        if (f == songField_) {
            g.color(col::kPanel2);
            g.fill(8, y - 12, 380, 26);
            g.color(col::kAccent);
            g.fill(8, y - 12, 4, 26);
        }
        g.color(f == songField_ ? col::kFg : col::kDim);
        g.text(22, y, kBody, labels[f], Canvas::kCenterY);
        g.color(f == 0 ? kSnapCol[st.snap & 7] : col::kFg);
        g.number(300, y, kBody, vals[f], Canvas::kCenterY);
    }
    g.color(col::kFg);
    g.textf(22, 214, kSmall, Canvas::kCenterY, "Step %d", songSel_ + 1);
    g.color(col::kDim);
    const int y0 = 236;
    g.text(410, y0, kTiny, "Pads row 1: recall snapshots 1-8 (morphs over the", Canvas::kCenterY);
    g.text(410, y0 + 18, kTiny, "Settings 'live morph time'); Shift+pad: store.", Canvas::kCenterY);
    g.text(410, y0 + 36, kTiny, "Pad 12 of row 1: song on / off. Rows 2-4: song steps;", Canvas::kCenterY);
    g.text(410, y0 + 54, kTiny, "Shift+step: set it to the selected snapshot.", Canvas::kCenterY);
    g.text(410, y0 + 72, kTiny, "Up/Down: field, encoder: value, Left/Right: step.", Canvas::kCenterY);
    g.text(410, y0 + 90, kTiny, "F8 stores the selected snapshot. Delete: remove step.", Canvas::kCenterY);
    g.text(410, y0 + 108, kTiny, "A step morphs from the step before it, then holds.", Canvas::kCenterY);
}

// ------------------------------------------------------------------ play
void Graf::drawPlay(Canvas& g) {
    int outs[kPatches], nOut = 0;
    if (eng_.rack)
        for (int s = 0; s < kMaxSlots && nOut < kPatches; ++s)
            if (eng_.mod(s).used && eng_.mod(s).type == kOut) outs[nOut++] = s;
    const int n = eng_.rack ? nOut : kPatches;
    for (int i = 0; i < n; ++i) {
        const int x = 8 + i * 98, y = 40;
        const bool sel = i == stripSel_;
        float level, pan;
        bool mute = false;
        char name[20];
        float meter;
        if (eng_.rack) {
            const Module& m = eng_.mod(outs[i]);
            level = m.v[0];
            pan = m.v[1];
            std::snprintf(name, sizeof name, "OUT%d", m.number);
            meter = meterShow_[(m.number - 1) & 7];
        } else {
            level = eng_.mix[i].level;
            pan = eng_.mix[i].pan;
            mute = eng_.mix[i].mute;
            std::snprintf(name, sizeof name, "PATCH %d", (i + 1) & 15);
            meter = meterShow_[i];
        }
        g.color(sel ? col::kPanel2 : col::kPanel);
        g.fill(x, y, 92, 220);
        g.color(i == patch_ && !eng_.rack ? col::kSig[3] : col::kDim);
        g.text(x + 46, y + 14, kSmall, name, Canvas::kCenter);
        // meter + fader
        g.color(col::kRail);
        g.fill(x + 14, y + 34, 14, 150);
        g.color(mute ? col::kFaint : (meter > 0.95f ? col::kWarn : col::kOk));
        const int mh = int(150.0f * clampf(meter, 0.0f, 1.0f));
        g.fill(x + 14, y + 184 - mh, 14, mh);
        g.color(col::kRail);
        g.fill(x + 52, y + 34, 6, 150);
        g.color(sel && stripField_ == 0 ? col::kAccent : col::kFg);
        g.fill(x + 42, y + 180 - int(146.0f * level), 26, 8);
        // pan
        g.color(col::kRail);
        g.fill(x + 10, y + 198, 72, 4);
        g.color(sel && stripField_ == 1 ? col::kAccent : col::kFg);
        g.fill(x + 44 + int(pan * 34.0f), y + 193, 4, 14);
        if (mute) {
            g.color(col::kWarn);
            g.text(x + 46, y + 120, kBody, "MUTE", Canvas::kCenter);
        }
    }
    if (eng_.rack && n == 0) {
        g.color(col::kDim);
        g.text(400, 140, kBody, "Add an OUT module to hear the rack.", Canvas::kCenter);
    }
    drawScope(g, 8, 272, 500, 140, col::kSig[0]);
    g.color(col::kDim);
    g.text(520, 284, kSmall, "Pads rows 2-4: keyboard", Canvas::kCenterY);
    g.textf(520, 306, kSmall, Canvas::kCenterY, "Octave %+d (pads 11 / 12 of row 1)", prefOctave_);
    g.text(520, 328, kSmall, eng_.rack ? "Row 1: pick an output strip" : "Row 1: pick a patch, Shift: mute", Canvas::kCenterY);
    g.text(520, 350, kSmall, "Encoder: level / pan (Up/Down)", Canvas::kCenterY);
    g.text(520, 372, kSmall, eng_.rack ? "MIDI: every channel plays the rack" : "MIDI channel n plays patch n",
           Canvas::kCenterY);
    // held notes
    int held = 0;
    for (int p = 0; p < lab::kPads; ++p)
        if (heldNote_[p] >= 0) ++held;
    g.color(held ? col::kSig[3] : col::kFaint);
    g.textf(520, 398, kSmall, Canvas::kCenterY, "%d key%s down", held, held == 1 ? "" : "s");
}

// ------------------------------------------------------------------ file
void Graf::drawFile(Canvas& g) {
    g.color(col::kDim);
    g.text(8, 48, kSmall, ctx_->fs ? "Patches on the card: /Lab/GRAF/Patches" : "No SD card: saving is off",
           Canvas::kCenterY);
    for (int i = 0; i < 24; ++i) {
        const int x = 8 + (i % 12) * 66, y = 64 + (i / 12) * 54;
        const bool sel = i == fileSel_;
        g.color(sel ? col::kAccent : (fileExists_[i] ? col::kPanel2 : col::kPanel));
        g.fill(x, y, 62, 48);
        g.color(sel ? col::kBg : (fileExists_[i] ? col::kFg : col::kFaint));
        g.textf(x + 31, y + 24, kBody, Canvas::kCenter, "P%02d", i + 1);
    }
    g.color(col::kDim);
    g.text(8, 192, kSmall, "Demos (pad row 4, Insert loads):", Canvas::kCenterY);
    for (int i = 0; i < demoCount(); ++i) {
        const int x = 8 + i * 131, y = 208;
        g.color(i == demoSel_ ? col::kSig[1] : col::kPanel);
        g.fill(x, y, 127, 36);
        g.color(i == demoSel_ ? col::kBg : col::kFg);
        g.text(x + 64, y + 18, kTiny, demoName(i) + 6, Canvas::kCenter);
    }
    const int y = 270;
    g.color(col::kFg);
    g.textf(8, y, kSmall, Canvas::kCenterY, "Selected: P%02d  %s", fileSel_ + 1, fileExists_[fileSel_] ? "(saved)" : "(empty)");
    g.color(col::kDim);
    g.text(8, y + 26, kSmall, "F8: save here (twice to overwrite)    Push: load", Canvas::kCenterY);
    g.text(8, y + 48, kSmall, "Insert: load the selected demo    Delete twice: new empty patch", Canvas::kCenterY);
    g.text(8, y + 70, kSmall, "Rec: rescan /Lab/GRAF/Samples for WAV files", Canvas::kCenterY);
    g.textf(8, y + 100, kSmall, Canvas::kCenterY, "%d samples on the card.  Autosave: /Lab/GRAF/autosave.grf",
            eng_.sampleFileCount());
    g.textf(8, y + 122, kSmall, Canvas::kCenterY, "This patch: %d modules, %d cables, model %s", eng_.moduleCount(),
            eng_.cableCount(), eng_.rack ? "Rack" : "8 patches");
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
        case kPgGraph:
            for (int c = 0; c < kCells; ++c) {
                const int s = eng_.slotAtCell(patch_, c);
                if (s >= 0) pads[c] = scale(catCol(def(eng_.mod(s).type).cat), s == sel_ ? 1.0f : 0.25f);
            }
            if (eng_.slotAtCell(patch_, cursor_) < 0 && blink) pads[cursor_] = Rgb(0x303030);
            if (holdCell_ >= 0)
                for (int c = 0; c < kCells; ++c)
                    if (c != holdCell_ && eng_.slotAtCell(patch_, c) >= 0 && blink) pads[c] = Rgb(0x804000);
            break;
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
            for (int p = 0; p < d.nParam; ++p) pads[p] = scale(catCol(d.cat), p == param_ ? 1.0f : 0.2f);
            pads[10] = pads[11] = Rgb(0x202020);
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
        case kPgAdd:
            for (int i = 0; i < kTypeCount && i < lab::kPads; ++i) {
                const int t = addOrder_[i];
                pads[i] = t == addType_ ? Rgb(0xFFFFFF) : scale(catCol(def(t).cat), eng_.fits(def(t).cost) ? 0.35f : 0.05f);
            }
            break;
        case kPgSnap: {
            static const Rgb kSnapCol[kSnaps] = {Rgb(0xFF8A2A), Rgb(0x38C8FF), Rgb(0xFFD23A), Rgb(0xFF4FA0),
                                                 Rgb(0x3CE07A), Rgb(0xA060F0), Rgb(0x20D0C0), Rgb(0xE0484E)};
            for (int i = 0; i < kSnaps; ++i) {
                pads[i] = eng_.snap(i).valid ? scale(kSnapCol[i], i == snapSel_ ? 1.0f : 0.3f) : Rgb(0x040404);
                if (eng_.morphTarget() == i && blink) pads[i] = Rgb(0xFFFFFF);
            }
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
            for (int i = 0; i < kPatches; ++i) {
                if (eng_.rack) pads[i] = i == stripSel_ ? Rgb(0x38C8FF) : Rgb(0x081820);
                else pads[i] = eng_.mix[i].mute ? Rgb(0x300000) : (i == patch_ ? Rgb(0xFF4FA0) : Rgb(0x200818));
            }
            pads[10] = pads[11] = Rgb(0x303030);
            keyLeds(1);
            break;
        case kPgFile:
            for (int i = 0; i < 24; ++i) pads[i] = i == fileSel_ ? Rgb(0xFFFFFF) : (fileExists_[i] ? Rgb(0x403000) : Rgb(0x040404));
            for (int i = 0; i < demoCount(); ++i) pads[36 + i] = i == demoSel_ ? Rgb(0x38C8FF) : Rgb(0x082030);
            break;
        default: break;
    }
}

}  // namespace graf
