// DATAMATICS -- drawing: intro, pages, RGB332 visuals in RAM_G, pad LEDs.
//
// RAM_G (free lab range 0x000000..0x06CBFF):
//   0x000000  waterfall ring, 400 x 240 RGB332 (96,000 B), drawn x2 full screen
//   0x020000  slot preview, 150 x 50 (7,500 B), drawn x2
//   0x028000  matrix bands, 160 x 64 (10,240 B), drawn x4
// Uploads happen in update() (main loop, outside a display list); only new
// waterfall rows are sent, the matrix ~12 times a second while visible.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/dm_ws.h"

namespace dm {

using namespace lab;

namespace {
constexpr uint8_t kMono = 18;    // FT81x ROM 8 x 16 monospace
constexpr uint8_t kMonoS = 16;   // FT81x ROM 8 x 8 monospace
const Rgb kWhite(0xFFFFFF), kBlack(0x000000), kGrey(0x8A8A8A), kDark(0x3A3A3A), kFaint(0x1C1C1C), kRed(0xFF2A00);

// RGB332 has 2 blue bits: only these four levels are (near) neutral grey.
constexpr uint8_t kW332 = 0xFF, kG332 = 0xB6, kD332 = 0x49, kR332 = 0xE0;
inline uint8_t grey332(uint8_t v) {
    static const uint8_t lv[4] = {0x00, kD332, kG332, kW332};
    return lv[v >> 6];
}

const char* const kSoundShort[] = {"SIN", "CLK", "NOI", "IMP"};

const char* const kSecDesc[T_COUNT] = {
    "PLAY     the masked streams play as set",
    "SCAN     read heads sweep the data across the section",
    "DENSIFY  layers enter one by one, thresholds fall",
    "THIN     layers leave one by one, thresholds rise",
    "SILENCE  hard cut to pure digital silence",
    "CUT      a data gate opens / shuts on 1/16 steps",
    "FREEZE   read heads loop a short window of units",
};

void fmtTime(char* out, uint32_t cap, uint32_t samples) {
    const uint32_t ms = uint32_t((uint64_t(samples) * 1000u) / 44100u);
    std::snprintf(out, cap, "%02lu:%02lu.%03lu", static_cast<unsigned long>(ms / 60000u),
                  static_cast<unsigned long>((ms / 1000u) % 60u), static_cast<unsigned long>(ms % 1000u));
}

void fmtSize(char* out, uint32_t cap, uint32_t b) {
    if (b < 10000) std::snprintf(out, cap, "%lu", static_cast<unsigned long>(b));
    else if (b < 10000000) std::snprintf(out, cap, "%luK", static_cast<unsigned long>(b / 1024));
    else std::snprintf(out, cap, "%luM", static_cast<unsigned long>(b / (1024 * 1024)));
}

void bits8(char* out, uint8_t v) {
    for (int i = 0; i < 8; ++i) out[i] = ((v >> (7 - i)) & 1) ? '1' : '0';
    out[8] = 0;
}

// Value text of a stream parameter.
void paramText(char* out, uint32_t cap, int p, int v) {
    const ParamInfo& pi = paramInfo(p);
    if (pi.names) {
        copyStr(out, cap, pi.names[v - pi.min]);
        return;
    }
    switch (p) {
        case P_START: std::snprintf(out, cap, "%d.%d%%", v / 10, v % 10); break;
        case P_RATE_MS: std::snprintf(out, cap, "%d MS", v); break;
        case P_LO:
        case P_HI: std::snprintf(out, cap, "%d HZ", v); break;
        case P_DUR: std::snprintf(out, cap, "%d.%d MS", v / 10, v % 10); break;
        case P_DURMOD:
        case P_LEVEL:
        case P_VEL: std::snprintf(out, cap, "%d%%", v); break;
        case P_STRIDE: std::snprintf(out, cap, "x%d", v); break;
        default: std::snprintf(out, cap, "%d", v); break;
    }
}
}  // namespace

bool Datamatics::wouldHit(int s, uint8_t val) const {
    const int thr = eng_.thrFor(s);
    return !(int(val) < thr || (thr > 0 && val == 0));
}

// ---- intro ---------------------------------------------------------------------------
void Datamatics::drawIntroBars(Canvas& g, float reveal, float height, int cy) {
    // deterministic barcode: widths 1..9 px, alternating bar / gap
    uint32_t s = 0x9E3779B9u;
    int x = 0;
    const int edge = int(reveal * 800.0f);
    const int h2 = int(height * 0.5f);
    g.color(kWhite);
    g.beginRects();
    bool on = true;
    while (x < 800) {
        s = s * 1664525u + 1013904223u;
        const int w = 1 + int((s >> 24) % 9u);
        if (on && x < edge) {
            const int x1 = x + w > edge ? edge : x + w;
            g.vtx(float(x), float(cy - h2));
            g.vtx(float(x1 - 1), float(cy + h2));
        }
        x += w;
        on = !on;
    }
    g.endBatch();
}

void Datamatics::drawIntro(Canvas& g, uint32_t t) {
    if (!disp_) disp_ = g.display();
    char buf[48];
    if (t < 1000) {   // phase 1: the test barcode sweeps in
        const float u = float(t) / 1000.0f;
        drawIntroBars(g, u * (2.0f - u), 360.0f, 240);
        g.color(kRed);
        g.fill(int(u * (2.0f - u) * 800.0f), 60, 2, 361);
    } else if (t < 1800) {   // phase 2: it collapses into a line of data
        const float u = float(t - 1000) / 800.0f;
        const float e = u * u * (3.0f - 2.0f * u);
        drawIntroBars(g, 1.0f, 360.0f - 352.0f * e, 240);
    } else {   // phase 3: the line resolves into the name
        const float u = float(t - 1800) / 1000.0f;
        const float k = u > 1.0f ? 1.0f : u;
        // the line splits outwards
        // the line retracts to the edges, clear of the letters
        const int half = int(140.0f * (1.0f - k));
        g.color(kWhite);
        if (half > 0) {
            g.fill(0, 236, half, 8);
            g.fill(800 - half, 236, half, 8);
        }
        static const char kName[] = "DATAMATICS";
        const int n = int(k * 14.0f);
        for (int i = 0; i < 10 && i < n; ++i) {
            char c[2] = {kName[i], 0};
            g.color(i == n - 1 && k < 1.0f ? kRed : kWhite);
            g.text(400 - 9 * 26 + i * 52, 236, font::kHuge, c, Canvas::kCenter);
        }
        if (k > 0.6f) {
            g.color(kWhite);
            g.hline(140, 270, 520);
            g.color(kGrey);
            g.text(400, 290, kMono, "ANY FILE AS DATA  /  SINE CLICK NOISE IMPULSE", Canvas::kCenter);
            g.color(kRed);
            g.fill(126, 232, 8, 8);
        }
    }
    // counters in the corners: sample clock and a hash of it
    g.color(kDark);
    const uint32_t smp = uint32_t((uint64_t(t) * 44100u) / 1000u);
    std::snprintf(buf, sizeof buf, "%09lu", static_cast<unsigned long>(smp));
    g.text(16, 20, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(smp * 2654435761u));
    g.text(784, 452, kMonoS, buf, Canvas::kRight);
}

// ---- frame parts ---------------------------------------------------------------------------
void Datamatics::drawTop(Canvas& g, const char* title) {
    g.color(kWhite);
    g.text(8, 4, kMono, "DATAMATICS");
    g.color(kRed);
    g.fill(92, 8, 6, 6);
    g.color(kGrey);
    g.text(108, 4, kMono, title);
    char buf[48];
    // right: section, time, bpm, transport, cpu
    const int cs = eng_.curSec;
    if (eng_.arrange)
        std::snprintf(buf, sizeof buf, "%02d %s", cs + 1, secTypeShort(eng_.sec[cs < eng_.secCount ? cs : 0].type));
    else
        copyStr(buf, sizeof buf, "FREE");
    g.color(kWhite);
    g.text(380, 4, kMono, buf);
    fmtTime(buf, sizeof buf, ctx_->playing() ? eng_.clock : 0);
    g.text(470, 4, kMono, buf);
    const int b10 = int(ctx_->bpm() * 10.0f + 0.5f);
    std::snprintf(buf, sizeof buf, "%d.%d", b10 / 10, b10 % 10);
    g.text(580, 4, kMono, buf);
    std::snprintf(buf, sizeof buf, "%lu%%", static_cast<unsigned long>(ctx_->shell->cpuPermille() / 10));
    g.color(kGrey);
    g.text(700, 4, kMono, buf, Canvas::kRight);
    if (ctx_->playing()) {
        g.color(kRed);
        g.fill(770, 6, 12, 12);
    } else {
        g.color(kWhite);
        g.frame(770, 6, 12, 12);
    }
    g.color(kWhite);
    g.hline(0, 26, 800);
}

void Datamatics::drawKeys(Canvas& g, const char* f7, const char* f8) {
    static const char* const kNames[6] = {"MATRIX", "STREAM", "DATA", "SECTIONS", "MIXER", "PIECE"};
    g.color(kWhite);
    g.hline(0, 452, 800);
    for (int i = 0; i < 8; ++i) {
        const char* s = i < 6 ? kNames[i] : (i == 6 ? f7 : f8);
        const bool active = i < 6 && i == page_;
        if (active) {
            g.color(kWhite);
            g.fill(i * 100 + 2, 455, 96, 23);
        }
        if (s && *s) {
            g.color(active ? kBlack : (i < 6 ? kGrey : kWhite));
            g.text(i * 100 + 50, 467, kMono, s, Canvas::kCenter);
        }
        if (i == 6) {
            g.color(kDark);
            g.vline(600, 456, 22);
        }
    }
}

// ---- visuals: matrix bands ---------------------------------------------------------------------
void Datamatics::composeMatrix() {
    for (int s = 0; s < kStreams; ++s) {
        const Stream& x = eng_.st[s];
        const Slot& sl = eng_.slot[x.p[P_SLOT] & 3];
        const bool live = eng_.streamAudible(s) && (!ctx_->playing() || eng_.sectionAllows(s));
        const uint32_t k0 = x.k;
        const uint32_t scan = eng_.scanOffset(s);
        uint8_t* band = mx_ + s * 8 * kMxW;
        std::memset(band, 0, 8 * kMxW);
        if (!sl.n) {
            for (int c = 0; c < kMxW; c += 4) band[4 * kMxW + c] = kD332;
            continue;
        }
        for (int c = 0; c < kMxW; ++c) {
            const int64_t kk = int64_t(k0) + c - kMxPast;
            if (kk < 0) continue;
            const uint32_t idx = readIndex(x, sl, uint32_t(kk), scan);
            uint8_t v, a;
            readValue(sl, x.p[P_UNIT], idx, v, a);
            const bool hit = wouldHit(s, v);
            if (hit) {
                const uint8_t col = !live ? kD332 : (c < kMxPast ? kG332 : kW332);
                for (int r = 1; r <= 6; ++r) band[r * kMxW + c] = col;
            } else {
                const int h = (v * 5) >> 8;   // 0..4 rows of dark grey: the value under the threshold
                for (int r = 6 - h; r <= 6; ++r) band[r * kMxW + c] = kD332;
            }
        }
    }
}

// ---- visuals: waterfall rows ----------------------------------------------------------------------
void Datamatics::composeRow() {
    // One row per time slice. Each stream owns a 50 px band: the data under its
    // read head (48 units centred on it) as a barcode. White = the stream sounded
    // in this slice, grey = it read but stayed silent (section / threshold),
    // near-black = muted or out of the section. A section change writes a red row.
    uint8_t* row = wf_ + wfHead_ * kWfW;
    std::memset(row, 0, kWfW);
    if (wfSecMark_) {
        std::memset(row, kR332, kWfW);
        wfSecMark_ = false;
    } else {
        for (int s = 0; s < kStreams; ++s) {
            const Stream& x = eng_.st[s];
            const Slot& sl = eng_.slot[x.p[P_SLOT] & 3];
            if (!sl.n) continue;
            const bool fired = (wfPend_[s] >> 16) != 0;
            wfPend_[s] = 0;
            const bool live = eng_.streamAudible(s) && eng_.sectionAllows(s);
            const uint8_t col = fired ? kW332 : (live ? kD332 : 0);
            const uint32_t k0 = x.k, scan = eng_.scanOffset(s);
            uint8_t* b = row + s * 50 + 1;
            for (int i = 0; i < 48; ++i) {
                const int64_t kk = int64_t(k0) + i - 24;
                if (kk < 0) continue;
                uint8_t v, a;
                readValue(sl, x.p[P_UNIT], readIndex(x, sl, uint32_t(kk), scan), v, a);
                if (wouldHit(s, v)) b[i] = col;
            }
            if (fired) b[24] = kR332;   // the read head
            else if (!live) for (int i = 0; i < 48; i += 6) b[i] = kD332;   // out: a dotted trace
        }
    }
    wfHead_ = (wfHead_ + 1) % kWfH;
    if (wfDirtyCount_ < kWfH) ++wfDirtyCount_;
    ++wfRows_;
}

void Datamatics::rebuildPreview(int slot) {
    std::memset(pv_, 0, kPvW * kPvH);
    const Slot& sl = eng_.slot[slot];
    const uint32_t n = sl.n;
    if (!n) return;
    if (sl.mode == kImage && sl.width > 1) {
        const uint32_t w = sl.width, h = n / w;
        if (!h) return;
        // fit, keep the aspect, centre
        uint32_t ow = kPvW, oh = uint32_t((uint64_t(h) * kPvW) / w);
        if (oh > uint32_t(kPvH)) {
            oh = kPvH;
            ow = uint32_t((uint64_t(w) * kPvH) / h);
            if (!ow) ow = 1;
        }
        const uint32_t x0 = (kPvW - ow) / 2, y0 = (kPvH - oh) / 2;
        for (uint32_t y = 0; y < oh; ++y)
            for (uint32_t x = 0; x < ow; ++x) {
                const uint32_t sx = x * w / ow, sy = y * h / oh;
                pv_[(y0 + y) * kPvW + x0 + x] = grey332(sl.v[sy * w + sx]);
            }
        return;
    }
    // other data: the whole slot as a raster, one value per cell (sampled when long)
    const uint32_t cells = kPvW * kPvH;
    for (uint32_t i = 0; i < cells; ++i) {
        const uint32_t j = n <= cells ? i : uint32_t((uint64_t(i) * n) / cells);
        if (j >= n) break;
        const uint8_t v = sl.v[j];
        pv_[i] = sl.mode == kBits ? (v ? kW332 : 0) : grey332(v);
    }
}

bool Datamatics::previewXY(int slot, uint32_t idx, int& x, int& y) const {
    const Slot& sl = eng_.slot[slot];
    const uint32_t n = sl.n;
    if (!n || idx >= n) return false;
    const uint32_t cells = kPvW * kPvH;
    if (sl.mode == kImage && sl.width > 1) {
        const uint32_t w = sl.width, h = n / w;
        if (!h) return false;
        uint32_t ow = kPvW, oh = uint32_t((uint64_t(h) * kPvW) / w);
        if (oh > uint32_t(kPvH)) {
            oh = kPvH;
            ow = uint32_t((uint64_t(w) * kPvH) / h);
        }
        x = int((kPvW - ow) / 2 + (idx % w) * ow / w);
        y = int((kPvH - oh) / 2 + (idx / w) * oh / h);
        return true;
    }
    const uint32_t c = n <= cells ? idx : uint32_t((uint64_t(idx) * cells) / n);
    x = int(c % kPvW);
    y = int(c / kPvW);
    return true;
}

void Datamatics::drainEvents(uint32_t ms) {
    EventRec e;
    while (eng_.events.pop(e)) {
        const int s = e.stream & 7;
        lastVal_[s] = e.val;
        lastIdx_[s] = e.idx;
        lastHz_[s] = e.hz;
        flashMs_[s] = ms;
        wfPend_[s] = uint32_t(e.val) | (uint32_t(e.sound) << 8) | (1u << 16);
        LogLine& l = log_[logHead_];
        l.idx = e.idx;
        l.hz = e.hz;
        l.s = uint8_t(s);
        l.val = e.val;
        l.sound = e.sound;
        logHead_ = (logHead_ + 1) % 10;
        ++evCount_;
    }
    if (eng_.curSec != lastSec_) {
        if (lastSec_ >= 0 && ctx_->playing()) wfSecMark_ = true;
        lastSec_ = eng_.curSec;
        secFlashMs_ = ms;
    }
    static const uint32_t kSlice[3] = {40, 20, 10};
    const uint32_t slice = kSlice[setSpeed_ < 0 || setSpeed_ > 2 ? 1 : setSpeed_];
    if (!ctx_->playing()) {
        wfLastMs_ = ms;
        return;
    }
    if (ms - wfLastMs_ > 1000) wfLastMs_ = ms - slice;
    while (ms - wfLastMs_ >= slice) {
        composeRow();
        wfLastMs_ += slice;
    }
}

void Datamatics::uploadVisuals() {
    if (!disp_) return;
    if (page_ == PG_VIEW || page_ == PG_MATRIX) {
        if (wfAllDirty_) {
            if (disp_->writeRamG(kRamWf, wf_, kWfW * kWfH)) {
                wfAllDirty_ = false;
                wfDirtyCount_ = 0;
            }
        } else if (wfDirtyCount_) {
            int start = wfHead_ - wfDirtyCount_;
            if (start < 0) start += kWfH;
            int count = wfDirtyCount_;
            while (count > 0) {
                const int run = start + count > kWfH ? kWfH - start : count;
                disp_->writeRamG(kRamWf + uint32_t(start) * kWfW, wf_ + start * kWfW, uint32_t(run) * kWfW);
                count -= run;
                start = 0;
            }
            wfDirtyCount_ = 0;
        }
    } else if (wfDirtyCount_ >= kWfH - 8) {
        wfAllDirty_ = true;
    }
    if (page_ == PG_MATRIX || page_ == PG_STREAM) {
        const uint32_t every = ctx_->playing() ? 80 : 160;
        if (!mxUploaded_ || nowMs_ - mxLastMs_ >= every) {
            mxLastMs_ = nowMs_;
            composeMatrix();
            mxUploaded_ = disp_->writeRamG(kRamMx, mx_, kMxW * kMxH);
        }
    }
    if (page_ == PG_DATA) {
        if (pvSlot_ != target_ || pvGen_ != slotGen_[target_] || !pvUploaded_) {
            pvSlot_ = target_;
            pvGen_ = slotGen_[target_];
            rebuildPreview(target_);
            pvUploaded_ = disp_->writeRamG(kRamPv, pv_, kPvW * kPvH);
        }
    }
}

void Datamatics::drawWaterfall(Canvas& g, int x, int y, int scale) {
    if (wfAllDirty_) return;
    g.color(kWhite);
    const int a = kWfH - wfHead_;
    if (a > 0) g.bitmap332(x, y, kRamWf + uint32_t(wfHead_) * kWfW, kWfW, a, scale);
    if (wfHead_ > 0) g.bitmap332(x, y + a * scale, kRamWf, kWfW, wfHead_, scale);
}

// ---- pages ---------------------------------------------------------------------------------------
void Datamatics::draw(Canvas& g) {
    if (!disp_) disp_ = g.display();
    switch (page_) {
        case PG_MATRIX: drawMatrix(g, false); break;
        case PG_STREAM: drawStream(g); break;
        case PG_DATA: drawData(g); break;
        case PG_SECTIONS: drawSections(g); break;
        case PG_MIXER: drawMixer(g); break;
        case PG_PIECE: drawPiece(g); break;
        default: drawMatrix(g, true); break;
    }
}

void Datamatics::drawMatrix(Canvas& g, bool full) {
    char buf[96];
    if (full) {   // VIEW: the waterfall over the whole screen + a thin overlay
        drawWaterfall(g, 0, 0, 2);
        g.color(kBlack);
        g.fill(0, 0, 330, 20);
        g.fill(560, 460, 240, 20);
        g.color(kWhite);
        const int cs = eng_.curSec;
        fmtTime(buf, 32, ctx_->playing() ? eng_.clock : 0);
        char line[64];
        std::snprintf(line, sizeof line, "DATAMATICS  %02d %-7s %s", cs + 1,
                      eng_.arrange ? secTypeName(eng_.sec[cs < eng_.secCount ? cs : 0].type) : "FREE", buf);
        g.text(6, 2, kMono, line);
        std::snprintf(line, sizeof line, "%010lu EV", static_cast<unsigned long>(evCount_));
        g.text(794, 462, kMono, line, Canvas::kRight);
        if (!ctx_->playing()) {
            g.color(kGrey);
            g.text(400, 240, kMono, "PLAY TO WRITE THE WATERFALL", Canvas::kCenter);
        }
        return;
    }
    drawTop(g, "MATRIX");
    constexpr int bx = 100, by = 34, sc = 4;
    // bands
    if (mxUploaded_) {
        g.color(kWhite);
        g.bitmap332(bx, by, kRamMx, kMxW, kMxH, sc);
    }
    // labels left, numbers right
    for (int s = 0; s < kStreams; ++s) {
        const int y = by + s * 32;
        const int* p = eng_.st[s].p;
        const bool aud = eng_.streamAudible(s);
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 60;
        g.color(s == sel_ ? kWhite : (aud ? kGrey : kDark));
        std::snprintf(buf, sizeof buf, "%d %s S%d", s + 1, kSoundShort[p[P_SOUND] & 3], (p[P_SLOT] & 3) + 1);
        g.text(6, y + 4, kMono, buf);
        if (p[P_SYNC]) std::snprintf(buf, sizeof buf, "%s %3d", rateDivName(p[P_RATE_DIV]), eng_.thrFor(s));
        else std::snprintf(buf, sizeof buf, "%dMS %3d", p[P_RATE_MS], eng_.thrFor(s));
        g.text(6, y + 20, kMonoS, buf);
        if (p[P_MUTE]) {
            g.color(kRed);
            g.text(84, y + 20, kMonoS, "M");
        }
        g.color(flash ? kWhite : kDark);
        std::snprintf(buf, sizeof buf, "%02X", lastVal_[s]);
        g.text(748, y + 4, kMono, buf);
        if (p[P_SOUND] == S_SINE) std::snprintf(buf, sizeof buf, "%5d", int(lastHz_[s]));
        else bits8(buf, lastVal_[s]);
        g.text(748, y + 20, kMonoS, buf);
    }
    // selection and playhead
    g.color(kRed);
    g.fill(bx - 4, by + sel_ * 32 + 4, 2, 24);
    g.beginRects();
    const float px = float(bx + kMxPast * sc);
    g.vtx(px - 1, float(by));
    g.vtx(px - 1, float(by + kMxH * sc - 1));
    g.vtx(px + sc, float(by));
    g.vtx(px + sc, float(by + kMxH * sc - 1));
    g.endBatch();

    // section strip
    const int sy = 300, sx = bx, sw = kMxW * sc;
    const int total = eng_.totalBars();
    int acc = 0;
    for (int i = 0; i < eng_.secCount; ++i) {
        const Section& c = eng_.sec[i];
        const int x0 = sx + acc * sw / (total ? total : 1);
        acc += c.bars;
        const int x1 = sx + acc * sw / (total ? total : 1);
        const bool cur = eng_.arrange && i == eng_.curSec;
        if (cur) {
            g.color(kWhite);
            g.fill(x0, sy, x1 - x0 - 1, 18);
        } else {
            g.color(c.type == T_SILENCE ? kFaint : kDark);
            g.frame(x0, sy, x1 - x0 - 1, 18);
        }
        if (x1 - x0 > 30) {
            g.color(cur ? kBlack : kGrey);
            g.text(x0 + 4, sy + 5, kMonoS, secTypeShort(c.type));
        }
        if (cur && ctx_->playing()) {
            g.color(kRed);
            g.fill(x0 + int(eng_.secProgress * float(x1 - x0 - 1)), sy - 3, 2, 24);
        }
    }
    if (!eng_.arrange) {
        g.color(kRed);
        g.text(sx + sw / 2, sy + 9, kMonoS, "FREE RUN - SECTIONS OFF", Canvas::kCenter);
    }

    // scrolling numbers: the newest events
    for (int i = 0; i < 6; ++i) {
        const LogLine& l = log_[(logHead_ + 9 - i) % 10];
        const int y = 330 + i * 18;
        if (!evCount_ || uint32_t(i) >= evCount_) break;
        char b8[9];
        bits8(b8, l.val);
        if (l.sound == S_SINE || l.sound == S_NOISE)
            std::snprintf(buf, sizeof buf, "%d %s %08lX %s %3u %7d HZ", l.s + 1, kSoundShort[l.sound & 3],
                          static_cast<unsigned long>(l.idx), b8, unsigned(l.val), int(l.hz + 0.5f));
        else
            std::snprintf(buf, sizeof buf, "%d %s %08lX %s %3u %s", l.s + 1, kSoundShort[l.sound & 3],
                          static_cast<unsigned long>(l.idx), b8, unsigned(l.val), l.sound == S_IMPULSE ? "22.7 US" : "CLICK");
        g.color(i == 0 ? kWhite : (i < 3 ? kGrey : kDark));
        g.text(bx, y, kMono, buf);
    }
    // right: the waterfall's newest rows as a small live strip
    if (!wfAllDirty_) {
        g.clip(560, 330, 200, 108);
        drawWaterfall(g, 560, 330 - kWfH + 108, 1);
        g.noClip();
    }
    g.color(kDark);
    g.frame(560, 330, 200, 108);
    if (loadBusy()) {
        g.color(kRed);
        g.text(560, 314, kMonoS, "LOADING DATA");
    }
    drawKeys(g, eng_.st[sel_].p[P_MUTE] ? "UNMUTE" : "MUTE", "VIEW");
}

void Datamatics::drawStream(Canvas& g) {
    char buf[64], val[32];
    drawTop(g, "STREAM");
    const int s = sel_;
    const int* p = eng_.st[s].p;
    g.color(kWhite);
    std::snprintf(buf, sizeof buf, "STREAM %d", s + 1);
    g.text(20, 36, kMono, buf);
    const Slot& sl = eng_.slot[p[P_SLOT] & 3];
    g.color(kGrey);
    std::snprintf(buf, sizeof buf, "%.14s %s", sl.path[0] ? sl.path : "(empty)", modeName(sl.mode));
    g.text(20, 54, kMonoS, buf);
    // this stream's band from the matrix bitmap
    if (mxUploaded_) {
        g.clip(200, 34, 600, 32);
        g.color(kWhite);
        g.bitmap332(200, 34 - s * 32, kRamMx, kMxW, kMxH, 4);
        g.noClip();
        g.color(kRed);
        g.fill(200 + kMxPast * 4, 34, 4, 2);
    }
    const int rows = streamParamRows();
    for (int i = 0; i < rows; ++i) {
        const int pp = streamParamAt(i);
        const int col = i < 12 ? 0 : 1;
        const int r = i < 12 ? i : i - 12;
        const int x = 20 + col * 400, y = 78 + r * 28;
        const bool on = i == streamParam_;
        if (on) {
            g.color(kWhite);
            g.fill(x - 6, y - 4, 372, 24);
        }
        // greyed rows: not used by this configuration
        bool used = true;
        if (pp == P_RATE_MS && p[P_SYNC]) used = false;
        if (pp == P_RATE_DIV && !p[P_SYNC]) used = false;
        if ((pp == P_LO || pp == P_HI || pp == P_SCALE) && p[P_SOUND] != S_SINE && p[P_SOUND] != S_NOISE) used = false;
        if ((pp == P_DUR || pp == P_DURMOD || pp == P_SHAPE) && p[P_SOUND] == S_IMPULSE) used = false;
        if (pp == P_PAN && p[P_PANMODE] == PM_FIX) used = true;
        g.color(on ? kBlack : (used ? kGrey : kDark));
        g.text(x, y, kMono, paramInfo(pp).name);
        paramText(val, sizeof val, pp, p[pp]);
        g.color(on ? kBlack : (used ? kWhite : kDark));
        g.text(x + 356, y, kMono, val, Canvas::kRight);
    }
    // the formula of this stream
    g.color(kGrey);
    const char* thr = "v >= thr";
    switch (p[P_SOUND]) {
        case S_SINE:
            std::snprintf(buf, sizeof buf, "f = %d * (%d/%d)^(v/255)  %s  dur %d.%d ms", p[P_LO], p[P_HI], p[P_LO], thr,
                          p[P_DUR] / 10, p[P_DUR] % 10);
            break;
        case S_NOISE:
            std::snprintf(buf, sizeof buf, "noise, lp = %d * (%d/%d)^(v/255)  %s", p[P_LO], p[P_HI], p[P_LO], thr);
            break;
        case S_CLICK:
            std::snprintf(buf, sizeof buf, "click = +-1 * %d.%d ms, sign = v & 1  %s", p[P_DUR] / 10, p[P_DUR] % 10, thr);
            break;
        default: std::snprintf(buf, sizeof buf, "impulse = +-1 for 1 sample (22.7 us)  %s", thr); break;
    }
    g.text(420, 78 + 11 * 28, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "events %lu", static_cast<unsigned long>(eng_.st[s].events));
    g.text(420, 78 + 11 * 28 + 12, kMonoS, buf);
    drawKeys(g, "< STREAM", "STREAM >");
}

void Datamatics::drawData(Canvas& g) {
    char buf[96];
    drawTop(g, "DATA");
    // browser
    g.color(kGrey);
    std::snprintf(buf, sizeof buf, "/%.50s", dir_);
    g.text(14, 34, kMonoS, buf);
    if (!ctx_->fs) {
        g.color(kRed);
        g.text(14, 400, kMono, "NO CARD - BUILT-IN DATA ONLY");
    }
    const int vis = 12;
    for (int i = 0; i < vis && entTop_ + i < entCount_; ++i) {
        const int idx = entTop_ + i;
        const Entry& e = ent_[idx];
        const int y = 50 + i * 28;
        const bool on = idx == entSel_;
        if (on) {
            g.color(kWhite);
            g.fill(8, y - 4, 432, 24);
        }
        g.color(on ? kBlack : (e.dir ? kWhite : (e.name[0] == '@' ? kGrey : kWhite)));
        std::snprintf(buf, sizeof buf, "%.44s%s", e.name, e.dir && std::strcmp(e.name, "..") ? "/" : "");
        g.text(14, y, kMono, buf);
        if (!e.dir && e.name[0] != '@') {
            fmtSize(buf, sizeof buf, e.size);
            g.text(434, y, kMono, buf, Canvas::kRight);
        } else if (e.name[0] == '@') {
            g.text(434, y, kMono, "BUILT-IN", Canvas::kRight);
        }
    }
    if (entCount_ > vis) {   // scroll bar
        g.color(kDark);
        g.vline(446, 46, 336);
        g.color(kWhite);
        const int h = 336 * vis / entCount_;
        g.fill(445, 46 + (336 - h) * entTop_ / (entCount_ - vis), 3, h);
    }
    // slots
    for (int i = 0; i < kSlots; ++i) {
        const Slot& sl = eng_.slot[i];
        const int y = 40 + i * 26;
        const bool on = i == target_;
        if (on) {
            g.color(kWhite);
            g.fill(460, y - 4, 332, 24);
        }
        const char* nm = sl.path;
        for (const char* q = sl.path; *q; ++q)
            if (*q == '/') nm = q + 1;
        g.color(on ? kBlack : kWhite);
        std::snprintf(buf, sizeof buf, "S%d %-5s %.22s", i + 1, sl.n ? modeName(sl.mode) : "-", nm[0] ? nm : "(empty)");
        g.text(466, y, kMono, buf);
        // which streams read this slot
        for (int s = 0; s < kStreams; ++s)
            if ((eng_.st[s].p[P_SLOT] & 3) == i) {
                g.color(on ? kBlack : kRed);
                g.fill(774 + (s & 3) * 4, y + (s >> 2) * 8 + 2, 3, 6);
            }
    }
    // preview of the target slot + the read heads on it
    const int px = 470, py = 152;
    if (pvUploaded_ && pvSlot_ == target_) {
        g.color(kWhite);
        g.bitmap332(px, py, kRamPv, kPvW, kPvH, 2);
    }
    g.color(kDark);
    g.frame(px - 1, py - 1, kPvW * 2 + 2, kPvH * 2 + 2);
    for (int s = 0; s < kStreams; ++s) {
        if ((eng_.st[s].p[P_SLOT] & 3) != target_) continue;
        uint32_t idx = eng_.st[s].lastIdx;
        if (eng_.st[s].p[P_UNIT] == U_BIT) idx >>= 3;
        int x, y;
        if (previewXY(target_, idx, x, y)) {
            g.color(kRed);
            g.fill(px + x * 2 - 1, py + y * 2 - 1, 4, 4);
        }
    }
    const Slot& sl = eng_.slot[target_];
    g.color(kGrey);
    if (sl.mode == kImage && sl.srcW)
        std::snprintf(buf, sizeof buf, "IMAGE %dx%d -> %ux%lu", sl.srcW, sl.srcH, unsigned(sl.width),
                      static_cast<unsigned long>(sl.width ? sl.n / sl.width : 0));
    else
        std::snprintf(buf, sizeof buf, "%s  %lu VALUES", modeName(sl.mode), static_cast<unsigned long>(sl.n));
    g.text(466, 262, kMonoS, buf);
    g.color(kWhite);
    std::snprintf(buf, sizeof buf, "READ MODE  %s", modeName(loadMode_));
    g.text(466, 284, kMono, buf);
    g.color(kDark);
    static const char* const kHelp[kModeCount] = {
        "by file type: image / wav / midi / text / raw",
        "every byte is a value 0..255",
        "every bit is a step: 1 = 255, 0 = rest",
        "BMP / PGM / PPM / PBM pixels, luminance",
        "envelope per 1.45 ms frame, aux = brightness",
        "note-ons in time, rests on a 1/16 grid",
        "characters; white space = rest",
    };
    g.text(466, 304, kMonoS, kHelp[loadMode_ % kModeCount]);
    // status
    if (loadBusy()) {
        g.color(kRed);
        std::snprintf(buf, sizeof buf, "LOADING %lu%%",
                      static_cast<unsigned long>(job_.limit ? uint64_t(job_.pos) * 100u / job_.limit : 0));
        g.text(466, 330, kMono, buf);
    } else if (status_[0] && int32_t(statusUntil_ - nowMs_) > 0) {
        g.color(kWhite);
        g.text(466, 330, kMonoS, status_);
    }
    g.color(kDark);
    g.text(466, 360, kMonoS, "LEFT/RIGHT target slot   DELETE re-read");
    g.text(466, 374, kMonoS, "ENCODER / UP DOWN browse   PUSH load");
    std::snprintf(buf, sizeof buf, "MODE %s", modeName(loadMode_));
    drawKeys(g, buf, "LOAD");
}

void Datamatics::drawSections(Canvas& g) {
    char buf[96];
    drawTop(g, "SECTIONS");
    const int x0 = 60, w = 720, ty = 40, th = 64;
    const int total = eng_.totalBars();
    int acc = 0;
    // timeline blocks with a pattern for each type
    for (int i = 0; i < eng_.secCount; ++i) {
        const Section& c = eng_.sec[i];
        const int a = x0 + acc * w / (total ? total : 1);
        acc += c.bars;
        const int b = x0 + acc * w / (total ? total : 1) - 2;
        const int bw = b - a;
        const bool cur = eng_.arrange && i == eng_.curSec && ctx_->playing();
        g.color(cur ? kWhite : kGrey);
        g.lineWidth(1.0f);
        switch (c.type) {
            case T_PLAY: g.color(cur ? kWhite : kDark); g.fill(a, ty, bw, th); break;
            case T_SCAN:
                g.beginLines();
                for (int k = 0; k < bw; k += 8) {
                    g.vtx(float(a + k), float(ty + th));
                    g.vtx(float(a + k + (bw - k < 16 ? bw - k : 16)), float(ty));
                }
                g.endBatch();
                break;
            case T_DENSIFY:
            case T_THIN:
                g.beginRects();
                for (int k = 0; k < bw; k += 4) {
                    const float f = float(k) / float(bw > 0 ? bw : 1);
                    const int h = int(float(th) * (c.type == T_DENSIFY ? f : 1.0f - f));
                    if (h > 0) {
                        g.vtx(float(a + k), float(ty + th - h));
                        g.vtx(float(a + k + 1), float(ty + th - 1));
                    }
                }
                g.endBatch();
                break;
            case T_SILENCE: break;
            case T_CUT: {
                uint32_t r = 0x1234567u + uint32_t(i) * 77u;
                g.beginRects();
                for (int k = 0; k < bw; k += 6) {
                    r = r * 1664525u + 1013904223u;
                    if ((r >> 24) & 1) {
                        g.vtx(float(a + k), float(ty));
                        g.vtx(float(a + k + 3), float(ty + th - 1));
                    }
                }
                g.endBatch();
                break;
            }
            default:   // freeze: a short window repeated
                g.beginRects();
                for (int k = 0; k < bw; k += 12) {
                    g.vtx(float(a + k), float(ty + 20));
                    g.vtx(float(a + k + 1), float(ty + th - 20));
                    g.vtx(float(a + k + 4), float(ty + 20));
                    g.vtx(float(a + k + 4), float(ty + th - 20));
                }
                g.endBatch();
                break;
        }
        g.color(i == secSel_ ? kRed : kDark);
        g.frame(a, ty, bw, th);
        if (i == secSel_) g.frame(a - 1, ty - 1, bw + 2, th + 2);
        if (bw > 34) {
            g.color(kBlack);
            g.fill(a + 1, ty + th + 2, 30, 10);
            g.color(i == secSel_ ? kWhite : kGrey);
            std::snprintf(buf, sizeof buf, "%02d", i + 1);
            g.text(a + 2, ty + th + 3, kMonoS, buf);
        }
        if (cur) {
            g.color(kRed);
            g.fill(a + int(eng_.secProgress * float(bw)), ty - 4, 2, th + 8);
        }
        if (i == eng_.startSec && !ctx_->playing()) {
            g.color(kWhite);
            g.fill(a, ty - 6, 6, 4);
        }
    }
    // stream x section matrix
    const int my = 128, mh = 18;
    g.color(kGrey);
    for (int s = 0; s < kStreams; ++s) {
        std::snprintf(buf, sizeof buf, "%d", s + 1);
        g.text(36, my + s * mh + 4, kMonoS, buf);
    }
    acc = 0;
    g.color(kWhite);
    g.beginRects();
    for (int i = 0; i < eng_.secCount; ++i) {
        const Section& c = eng_.sec[i];
        const int a = x0 + acc * w / (total ? total : 1);
        acc += c.bars;
        const int b = x0 + acc * w / (total ? total : 1) - 3;
        if (c.type == T_SILENCE) continue;
        for (int s = 0; s < kStreams; ++s)
            if ((c.mask >> s) & 1) {
                g.vtx(float(a + 1), float(my + s * mh + 2));
                g.vtx(float(b), float(my + s * mh + mh - 3));
            }
    }
    g.endBatch();
    g.color(kFaint);
    g.beginLines();
    for (int s = 0; s <= kStreams; ++s) {
        g.vtx(float(x0), float(my + s * mh));
        g.vtx(float(x0 + w), float(my + s * mh));
    }
    g.endBatch();
    // details of the selected section
    const Section& c = eng_.sec[secSel_ < eng_.secCount ? secSel_ : 0];
    const int dy = 290;
    static const char* const kField[4] = {"TYPE", "BARS", "MASK", "DEPTH"};
    for (int f = 0; f < 4; ++f) {
        const int x = 20 + f * 190;
        const bool on = f == secField_;
        if (on) {
            g.color(kWhite);
            g.fill(x - 4, dy - 4, 182, 48);
        }
        g.color(on ? kBlack : kGrey);
        g.text(x, dy, kMonoS, kField[f]);
        g.color(on ? kBlack : kWhite);
        if (f == 0) g.text(x, dy + 16, kMono, secTypeName(c.type));
        else if (f == 1) {
            std::snprintf(buf, sizeof buf, "%d", c.bars);
            g.text(x, dy + 16, kMono, buf);
        } else if (f == 2) {
            for (int s = 0; s < kStreams; ++s) {
                const int bx = x + s * 20;
                if ((c.mask >> s) & 1) g.fill(bx, dy + 16, 16, 16);
                else g.frame(bx, dy + 16, 16, 16);
            }
            if (on) {
                g.color(kRed);
                g.fill(x + maskBit_ * 20, dy + 34, 16, 3);
                g.color(on ? kBlack : kWhite);
            }
        } else {
            std::snprintf(buf, sizeof buf, "%d%%", c.depth);
            g.text(x, dy + 16, kMono, buf);
        }
    }
    g.color(kWhite);
    g.text(20, 352, kMono, kSecDesc[c.type % T_COUNT]);
    g.color(kGrey);
    const float secs = float(total) * 4.0f * 60.0f / (ctx_->bpm() > 1.0f ? ctx_->bpm() : 120.0f);
    std::snprintf(buf, sizeof buf, "%d SECTIONS  %d BARS  %d:%02d AT %d BPM  %s", eng_.secCount, total,
                  int(secs) / 60, int(secs) % 60, int(ctx_->bpm() + 0.5f), eng_.arrange ? "" : "(SECTIONS OFF)");
    g.text(20, 376, kMono, buf);
    g.color(kDark);
    g.text(20, 400, kMonoS, "LEFT/RIGHT section  UP/DOWN field  ENCODER value  SHIFT+ENC toggle mask bit");
    g.text(20, 414, kMonoS, "INSERT duplicate  DELETE remove  PUSH/F7 cue (stopped) or jump (playing)");
    drawKeys(g, ctx_->playing() ? "JUMP" : "CUE", eng_.arrange ? "ARR ON" : "ARR OFF");
}

void Datamatics::drawMixer(Canvas& g) {
    char buf[32];
    drawTop(g, "MIXER");
    static const char* const kFieldNames[7] = {"LEVEL", "PAN", "PANMODE", "STEM", "MUTE", "SOLO", "MIDI"};
    static const uint8_t kFields[7] = {P_LEVEL, P_PAN, P_PANMODE, P_STEM, P_MUTE, P_SOLO, P_MIDI};
    for (int s = 0; s < kStreams; ++s) {
        const int x = 14 + s * 98, w = 86;
        const int* p = eng_.st[s].p;
        const bool on = s == sel_;
        const bool aud = eng_.streamAudible(s);
        // header: number + activity
        g.color(on ? kWhite : kGrey);
        std::snprintf(buf, sizeof buf, "%d %s", s + 1, kSoundShort[p[P_SOUND] & 3]);
        g.text(x, 36, kMono, buf);
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 50;
        g.color(flash ? kWhite : kFaint);
        g.fill(x + w - 12, 38, 10, 10);
        // level: fine segmented column
        const int fy = 64, fh = 220;
        const int segs = 44;
        const int lit = p[P_LEVEL] * segs / 100;
        g.color(aud ? kWhite : kDark);
        g.beginRects();
        for (int i = 0; i < lit; ++i) {
            const float y = float(fy + fh - (i + 1) * 5);
            g.vtx(float(x + 20), y);
            g.vtx(float(x + w - 21), y + 2);
        }
        g.endBatch();
        g.color(kFaint);
        g.frame(x + 18, fy - 1, w - 36, fh + 2);
        // pan: a marker on a line
        const int py = fy + fh + 14;
        g.color(kDark);
        g.hline(x + 4, py, w - 8);
        g.vline(x + w / 2, py - 3, 7);
        g.color(kWhite);
        g.fill(x + w / 2 + p[P_PAN] * (w / 2 - 6) / 100 - 2, py - 4, 4, 9);
        // fields
        for (int f = 1; f < 7; ++f) {
            const int y = fy + fh + 26 + (f - 1) * 22;
            const bool fs = on && f == mixField_;
            if (fs) {
                g.color(kWhite);
                g.fill(x, y - 3, w, 20);
            }
            const int v = p[kFields[f]];
            if (kFields[f] == P_PAN) std::snprintf(buf, sizeof buf, "%+d", v);
            else if (kFields[f] == P_MUTE) copyStr(buf, sizeof buf, v ? "MUTED" : "-");
            else if (kFields[f] == P_SOLO) copyStr(buf, sizeof buf, v ? "SOLO" : "-");
            else if (kFields[f] == P_MIDI) copyStr(buf, sizeof buf, v ? "MIDI" : "-");
            else paramText(buf, sizeof buf, kFields[f], v);
            const bool red = (kFields[f] == P_MUTE || kFields[f] == P_SOLO) && v;
            g.color(fs ? kBlack : (red ? kRed : kGrey));
            g.text(x + 4, y, kMono, buf);
        }
        std::snprintf(buf, sizeof buf, "%d", p[P_LEVEL]);
        g.color(on && mixField_ == 0 ? kRed : kGrey);
        g.text(x + w / 2, fy + fh + 2, kMonoS, buf, Canvas::kCenterX);
        if (on) {
            g.color(kWhite);
            g.frame(x - 4, 32, w + 8, 418);
        }
    }
    g.color(kDark);
    g.text(400, 440, kMonoS, kFieldNames[mixField_], Canvas::kCenterX);
    drawKeys(g, "MUTE", "SOLO");
}

void Datamatics::drawPiece(Canvas& g) {
    char buf[96];
    drawTop(g, "PIECE");
    g.color(kGrey);
    g.text(20, 36, kMonoS, "PIECES ON THE CARD");
    if (!ctx_->fs) {
        g.color(kRed);
        g.text(20, 60, kMono, "NO CARD");
    } else if (!pieceCount_) {
        g.color(kDark);
        g.text(20, 60, kMono, "none yet - F7 saves one");
    }
    const int vis = 13;
    const int top = pieceSel_ >= vis ? pieceSel_ - vis + 1 : 0;
    for (int i = 0; i < vis && top + i < pieceCount_; ++i) {
        const int idx = top + i;
        const int y = 56 + i * 28;
        const bool on = idx == pieceSel_;
        if (on) {
            g.color(delArm_ ? kRed : kWhite);
            g.fill(14, y - 4, 280, 24);
        }
        g.color(on ? kBlack : kWhite);
        g.text(20, y, kMono, pieces_[idx]);
    }
    // the piece in memory
    const int x = 340;
    g.color(kWhite);
    g.text(x, 36, font::kHuge, pieceName_);
    g.color(kRed);
    g.fill(x, 76, 8, 8);
    const int total = eng_.totalBars();
    const float secs = float(total) * 4.0f * 60.0f / (ctx_->bpm() > 1.0f ? ctx_->bpm() : 120.0f);
    g.color(kGrey);
    std::snprintf(buf, sizeof buf, "%d SECTIONS  %d BARS  %d:%02d  %d BPM", eng_.secCount, total, int(secs) / 60,
                  int(secs) % 60, int(ctx_->bpm() + 0.5f));
    g.text(x, 100, kMono, buf);
    for (int i = 0; i < kSlots; ++i) {
        const Slot& sl = eng_.slot[i];
        std::snprintf(buf, sizeof buf, "S%d %-5s %.40s", i + 1, modeName(sl.wantMode), sl.path[0] ? sl.path : "-");
        g.color(sl.n ? kWhite : kDark);
        g.text(x, 130 + i * 20, kMono, buf);
    }
    g.color(kDark);
    std::snprintf(buf, sizeof buf, "/%s/Pieces/*.dmx   text, version %d", ctx_->home, kPieceVersion);
    g.text(x, 230, kMonoS, buf);
    g.text(x, 250, kMonoS, "F7 save as new    F8 / PUSH load selected");
    g.text(x, 264, kMonoS, "COPY overwrite selected    INSERT new (demo)");
    g.text(x, 278, kMonoS, "DELETE twice: remove the file");
    g.text(x, 292, kMonoS, "the piece is also kept in last.dmx (Config > Save)");
    // a small barcode of the piece: section lengths as bars
    g.color(kWhite);
    g.beginRects();
    int acc = 0;
    for (int i = 0; i < eng_.secCount; ++i) {
        const int a = x + acc * 440 / (total ? total : 1);
        acc += eng_.sec[i].bars;
        const int b = x + acc * 440 / (total ? total : 1);
        if (eng_.sec[i].type == T_SILENCE) continue;
        const int bw = (b - a) * (eng_.sec[i].type == T_THIN ? 1 : (eng_.sec[i].type == T_DENSIFY ? 3 : 2)) / 4;
        g.vtx(float(a), 330.0f);
        g.vtx(float(a + (bw > 1 ? bw : 1)), 400.0f);
    }
    g.endBatch();
    drawKeys(g, "SAVE NEW", "LOAD");
}

// ---- pads --------------------------------------------------------------------------------------
void Datamatics::leds(Rgb* pads) {
    const bool playing = ctx_->playing();
    for (int row = 0; row < 4; ++row) {
        const int s = bank_ * 4 + row;
        const Stream& x = eng_.st[s];
        const Slot& sl = eng_.slot[x.p[P_SLOT] & 3];
        const bool aud = eng_.streamAudible(s);
        const bool flash = playing && nowMs_ - flashMs_[s] < 45;
        const uint32_t scan = eng_.scanOffset(s);
        for (int c = 0; c < 8; ++c) {
            Rgb col;
            if (sl.n) {
                const uint32_t idx = readIndex(x, sl, x.k + uint32_t(c), scan);
                uint8_t v, a;
                readValue(sl, x.p[P_UNIT], idx, v, a);
                if (wouldHit(s, v)) {
                    const uint8_t b = uint8_t(aud ? 40 + (v * 215) / 255 : 18);
                    col = Rgb(b, b, b);
                }
            }
            if (c == 0 && flash) col = Rgb(0xFFFFFF);
            pads[padAt(c, row)] = col;
        }
        pads[padAt(8, row)] = x.p[P_MUTE] ? Rgb(0xFF2000) : Rgb(0x000000);
        pads[padAt(9, row)] = x.p[P_SOLO] ? Rgb(0xFFFFFF) : Rgb(0x000000);
        pads[padAt(10, row)] = s == sel_ ? Rgb(0xFFFFFF) : Rgb(0x101010);
        const int sec = (secSel_ / 4) * 4 + row;
        Rgb sc;
        if (sec < eng_.secCount) {
            if (playing && eng_.arrange && sec == eng_.curSec) sc = Rgb(0xFF2000);
            else if (!playing && sec == eng_.startSec) sc = Rgb(0x808080);
            else sc = Rgb(0x141414);
            if (eng_.jumpReq == sec && (nowMs_ / 80) & 1) sc = Rgb(0xFFFFFF);
        }
        pads[padAt(11, row)] = sc;
    }
}

}  // namespace dm
