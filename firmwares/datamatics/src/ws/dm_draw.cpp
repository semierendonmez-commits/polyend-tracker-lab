// DATAMATICS -- drawing: intro, pages, RGB332 visuals in RAM_G, pad LEDs.
//
// RAM_G (free lab range 0x000000..0x06CBFF):
//   0x000000  waterfall ring, 400 x 240 RGB332 (96,000 B), drawn x2 full screen
//   0x020000  slot barcodes, 4 x 200 x 12 (9,600 B), drawn x2 on DATA
//   0x028000  matrix bands, 160 x 64 (10,240 B), x4 on MATRIX, x2 on PERFORM
// Uploads happen in update() (main loop, outside a display list): only new
// waterfall rows; the matrix ~12 times a second while it is visible.
//
// Colour: black and white with one accent (red = action, playhead, CUT).
// The four slot colours are the only other colours: they say which data a
// stream reads, on every page, pad and visual.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/dm_ws.h"

namespace dm {

using namespace lab;
using kit::ParamId;

namespace {
constexpr uint8_t kMono = 18;    // FT81x ROM 8 x 16 monospace
constexpr uint8_t kMonoS = 16;   // FT81x ROM 8 x 8 monospace
const Rgb kWhite(0xFFFFFF), kBlack(0x000000), kGrey(0x8A8A8A), kDark(0x3A3A3A), kFaint(0x1C1C1C), kRed(0xFF2A00);
constexpr uint8_t kW332 = 0xFF, kG332 = 0xB6, kD332 = 0x49, kR332 = 0xE0;

const char* const kSoundShort[] = {"SIN", "CLK", "NOI", "IMP"};
const char* const kModeShort[kModeCount] = {"AUTO", "RAW", "BITS", "IMG", "WAV", "MIDI", "TXT"};

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

void bits8(char* out, uint8_t v) {
    for (int i = 0; i < 8; ++i) out[i] = ((v >> (7 - i)) & 1) ? '1' : '0';
    out[8] = 0;
}

const char* baseName(const char* path) {
    const char* nm = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/') nm = q + 1;
    return nm;
}
}  // namespace

bool Datamatics::wouldHit(int s, uint8_t val) const {
    const int thr = eng_.thrFor(s);
    bool hit = !(int(val) < thr || (thr > 0 && val == 0));
    if (eng_.throws & TH_INVERT) hit = !hit;
    return hit;
}

// "S2 circle.bmp IMG" in the slot's colour (at most maxChars characters).
void Datamatics::drawSlotTag(Canvas& g, int x, int y, int s, int maxChars) {
    const int sl = eng_.st[s].p[P_SLOT] & 3;
    const Slot& slot = eng_.slot[sl];
    char nm[64];
    const char* b = slot.path[0] ? baseName(slot.path) : "(empty)";
    const int room = maxChars - 8;
    std::snprintf(nm, sizeof nm, "S%d %-.*s %s", sl + 1, room > 4 ? room : 4, b, slot.n ? kModeShort[slot.mode % kModeCount] : "-");
    g.color(slotColor(sl));
    g.text(x, y, kMonoS, nm);
}

// ---- intro ---------------------------------------------------------------------------
void Datamatics::drawIntroBars(Canvas& g, float reveal, float height, int cy) {
    uint32_t s = 0x9E3779B9u;   // deterministic barcode: widths 1..9 px
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
    if (t < 1000) {
        const float u = float(t) / 1000.0f;
        drawIntroBars(g, u * (2.0f - u), 360.0f, 240);
        g.color(kRed);
        g.fill(int(u * (2.0f - u) * 800.0f), 60, 2, 361);
    } else if (t < 1800) {
        const float u = float(t - 1000) / 800.0f;
        const float e = u * u * (3.0f - 2.0f * u);
        drawIntroBars(g, 1.0f, 360.0f - 352.0f * e, 240);
    } else {
        const float u = float(t - 1800) / 1000.0f;
        const float k = u > 1.0f ? 1.0f : u;
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
    const int cs = eng_.curSec;
    if (eng_.arrange)
        std::snprintf(buf, sizeof buf, "%02d %s %s", cs + 1, secTypeShort(eng_.sec[cs < eng_.secCount ? cs : 0].type),
                      eng_.songMode ? "CH" : "LV");
    else
        copyStr(buf, sizeof buf, "FREE");
    g.color(kWhite);
    g.text(330, 4, kMono, buf);
    fmtTime(buf, sizeof buf, ctx_->playing() ? eng_.clock : 0);
    g.text(450, 4, kMono, buf);
    const int b10 = int(ctx_->bpm() * 10.0f + 0.5f);
    std::snprintf(buf, sizeof buf, "%d.%d", b10 / 10, b10 % 10);
    g.text(556, 4, kMono, buf);
    if (perfRec_ || perfArm_ || perfReplaying_) {
        g.color(kRed);
        g.text(620, 4, kMono, perfRec_ ? "REC" : (perfArm_ ? "ARM" : "RPL"));
    }
    std::snprintf(buf, sizeof buf, "%lu%%", static_cast<unsigned long>(ctx_->shell->cpuPermille() / 10));
    g.color(kGrey);
    g.text(712, 4, kMono, buf, Canvas::kRight);
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

void Datamatics::drawKeys(Canvas& g, const char* f8) {
    static const char* const kNames[7] = {"PERFORM", "MATRIX", "STREAM", "DATA", "MIXER", "SONG", "VIEW"};
    int active = -1;
    switch (screenPage()) {
        case PG_PERFORM: active = 0; break;
        case PG_MATRIX: active = 1; break;
        case PG_ST0: active = 2; break;
        case PG_DATA: active = 3; break;
        case PG_MIXER: active = 4; break;
        case PG_SONG: active = 5; break;
        case PG_VIEW: active = 6; break;
        default: break;
    }
    g.color(kWhite);
    g.hline(0, 452, 800);
    for (int i = 0; i < 8; ++i) {
        const char* s = i < 7 ? kNames[i] : f8;
        const bool on = i == active;
        if (on) {
            g.color(kWhite);
            g.fill(i * 100 + 2, 455, 96, 23);
        }
        if (s && *s) {
            g.color(on ? kBlack : (i < 7 ? kGrey : kRed));
            g.text(i * 100 + 50, 467, kMono, s, Canvas::kCenter);
        }
    }
    g.color(kDark);
    g.vline(700, 456, 22);
}

// ---- RAM_G visuals ---------------------------------------------------------------------
void Datamatics::composeMatrix() {
    for (int s = 0; s < kStreams; ++s) {
        const Stream& x = eng_.st[s];
        const int sl = x.p[P_SLOT] & 3;
        const Slot& slot = eng_.slot[sl];
        const bool live = eng_.streamAudible(s) && (!ctx_->playing() || eng_.sectionAllows(s));
        const uint8_t on = live ? slotColor332(sl) : kD332, past = slotColor332(sl, true);
        const uint32_t k0 = x.k;
        const uint32_t scan = eng_.scanOffset(s);
        uint8_t* band = mx_ + s * 8 * kMxW;
        std::memset(band, 0, 8 * kMxW);
        if (!slot.n) {
            for (int c = 0; c < kMxW; c += 4) band[4 * kMxW + c] = kD332;
            continue;
        }
        for (int c = 0; c < kMxW; ++c) {
            const int64_t kk = int64_t(k0) + c - kMxPast;
            if (kk < 0) continue;
            const uint32_t idx = readIndex(x, slot, uint32_t(kk), scan);
            uint8_t v, a;
            readValue(slot, x.p[P_UNIT], idx, v, a);
            if (wouldHit(s, v) && !(fill_ & (1u << s) && false)) {
                const uint8_t col = c < kMxPast ? past : on;
                for (int r = 1; r <= 6; ++r) band[r * kMxW + c] = col;
            } else {
                const int h = (v * 5) >> 8;   // the value under the threshold, as a dark stub
                for (int r = 6 - h; r <= 6; ++r) band[r * kMxW + c] = kD332;
            }
        }
    }
}

void Datamatics::composeBarcodes() {
    // DATA page: the whole of each slot as a barcode, 200 columns x 12 rows.
    for (int i = 0; i < kSlots; ++i) {
        if (bcGen_[i] == slotGen_[i]) continue;
        bcGen_[i] = slotGen_[i];
        uint8_t* b = bc_ + i * kBcW * kBcH;
        std::memset(b, 0, kBcW * kBcH);
        const Slot& sl = eng_.slot[i];
        const uint32_t n = sl.n;
        if (!n) continue;
        const uint8_t col = slotColor332(i), dim = slotColor332(i, true);
        for (int c = 0; c < kBcW; ++c) {
            const uint32_t j = uint32_t((uint64_t(c) * n) / kBcW);
            const uint8_t v = sl.v[j < n ? j : n - 1];
            // top: a bar where the value is >= 128; bottom: the value as height
            if (v >= 128)
                for (int r = 0; r < 7; ++r) b[r * kBcW + c] = col;
            const int h = (v * 4) >> 8;
            for (int r = 11 - h; r <= 11; ++r) b[r * kBcW + c] = dim;
        }
    }
}

void Datamatics::composeRow() {
    // VIEW / STREAM mode: one row per time slice; each stream owns a 50 px column
    // band: the 48 units around its read head. Slot colour = the stream sounded
    // in this slice, its dim colour = read but silent, dots = out of the section
    // or muted. A section change writes a red row.
    uint8_t* row = wf_ + wfHead_ * kWfW;
    std::memset(row, 0, kWfW);
    if (wfSecMark_) {
        std::memset(row, kR332, kWfW);
        wfSecMark_ = false;
    } else {
        for (int s = 0; s < kStreams; ++s) {
            const Stream& x = eng_.st[s];
            const int sl = x.p[P_SLOT] & 3;
            const Slot& slot = eng_.slot[sl];
            if (!slot.n) continue;
            const bool fired = (wfPend_[s] >> 16) != 0;
            wfPend_[s] = 0;
            const bool live = eng_.streamAudible(s) && eng_.sectionAllows(s);
            const uint8_t col = fired ? slotColor332(sl) : (live ? slotColor332(sl, true) : 0);
            const uint32_t k0 = x.k, scan = eng_.scanOffset(s);
            uint8_t* b = row + s * 50 + 1;
            if (col)
                for (int i = 0; i < 48; ++i) {
                    const int64_t kk = int64_t(k0) + i - 24;
                    if (kk < 0) continue;
                    uint8_t v, a;
                    readValue(slot, x.p[P_UNIT], readIndex(x, slot, uint32_t(kk), scan), v, a);
                    if (wouldHit(s, v)) b[i] = col;
                }
            if (fired) b[24] = kW332;   // the read head
            else if (!live) for (int i = 0; i < 48; i += 6) b[i] = kD332;
        }
    }
    wfHead_ = (wfHead_ + 1) % kWfH;
    if (wfDirtyCount_ < kWfH) ++wfDirtyCount_;
    ++wfRows_;
}

void Datamatics::drainEvents(uint32_t ms) {
    EventRec e;
    int n = 0;
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
        ++n;
    }
    level_ += ((n > 8 ? 1.0f : float(n) * 0.125f) - level_) * 0.2f;
    if (eng_.curSec != lastSec_) {
        if (lastSec_ >= 0 && ctx_->playing()) wfSecMark_ = true;
        lastSec_ = eng_.curSec;
    }
    if (visAuto_ && page_ == PG_VIEW && ms - visAutoMs_ > 8000) {   // auto cycle every 8 s
        visAutoMs_ = ms;
        visMode_ = (visMode_ + 1) % VM_COUNT;
        ctx_->params().sync();
    }
    static const uint32_t kSlice[3] = {40, 20, 10};
    const uint32_t slice = kSlice[wfSpeed_ < 0 || wfSpeed_ > 2 ? 1 : wfSpeed_];
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
    const bool wfVisible = (page_ == PG_VIEW && visMode_ == VM_STREAM) || page_ == PG_MATRIX;
    if (wfVisible) {
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
    if (page_ == PG_MATRIX || page_ == PG_PERFORM || isStreamPage(page_)) {
        const uint32_t every = ctx_->playing() ? 80 : 160;
        if (!mxUploaded_ || nowMs_ - mxLastMs_ >= every) {
            mxLastMs_ = nowMs_;
            composeMatrix();
            mxUploaded_ = disp_->writeRamG(kRamMx, mx_, kMxW * kMxH);
        }
    }
    if (page_ == PG_DATA) {
        bool changed = !bcUploaded_;
        for (int i = 0; i < kSlots; ++i) changed = changed || bcGen_[i] != slotGen_[i];
        if (changed) {
            composeBarcodes();
            bcUploaded_ = disp_->writeRamG(kRamBc, bc_, kBcW * kBcH * kSlots);
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
    switch (screenPage()) {
        case PG_PERFORM: drawPerform(g); break;
        case PG_MATRIX: drawMatrix(g); break;
        case PG_ST0: drawStream(g); break;
        case PG_DATA: drawData(g); break;
        case PG_SONG: drawSong(g); break;
        case PG_MIXER: drawMixer(g); break;
        case PG_PIECE: drawPiece(g); break;
        default: drawView(g); break;
    }
}

void Datamatics::drawPerform(Canvas& g) {
    char buf[64];
    drawTop(g, "PERFORM");
    kit::Registry& P = ctx_->params();
    const bool playing = ctx_->playing();
    // eight stream strips: slot colour bar, name, slot tag, the units ahead, state
    for (int s = 0; s < kStreams; ++s) {
        const int y = 32 + s * 34;
        const int* p = eng_.st[s].p;
        const int sl = p[P_SLOT] & 3;
        const bool aud = eng_.streamAudible(s);
        const bool flash = playing && nowMs_ - flashMs_[s] < 60;
        g.color(aud ? slotColor(sl) : scale(slotColor(sl), 0.25f));
        g.fill(4, y + 2, 5, 28);
        g.color(s == sel_ ? kWhite : (aud ? kGrey : kDark));
        std::snprintf(buf, sizeof buf, "%d %s", s + 1, kSoundShort[p[P_SOUND] & 3]);
        g.text(16, y + 1, kMono, buf);
        drawSlotTag(g, 16, y + 20, s, 26);
        if (mxUploaded_) {
            g.clip(232, y + 8, 320, 16);
            g.color(kWhite);
            g.bitmap332(232, y + 8 - s * 16, kRamMx, kMxW, kMxH, 2);
            g.noClip();
        }
        g.color(kRed);
        g.fill(232 + kMxPast * 2, y + 6, 2, 20);
        // state letters: M(ute) S(olo) F(ill)
        if (p[P_MUTE]) {
            g.color(kRed);
            g.text(564, y + 9, kMono, "M");
        }
        if (p[P_SOLO]) {
            g.color(kWhite);
            g.text(580, y + 9, kMono, "S");
        }
        if (fill_ & (1u << s)) {
            g.color(kWhite);
            g.fill(594, y + 10, 12, 14);
            g.color(kBlack);
            g.text(596, y + 9, kMono, "F");
        }
        g.color(flash ? kWhite : kDark);
        std::snprintf(buf, sizeof buf, "%02X", lastVal_[s]);
        g.text(620, y + 9, kMono, buf);
        if (p[P_SOUND] == S_SINE) std::snprintf(buf, sizeof buf, "%5dHZ", int(lastHz_[s]));
        else bits8(buf, lastVal_[s]);
        g.text(648, y + 13, kMonoS, buf);
        if (flash) {
            g.color(slotColor(sl));
            g.fill(782, y + 8, 12, 16);
        }
    }
    // sections / scenes / throws
    auto label = [&](int y, const char* t) {
        g.color(kGrey);
        g.text(4, y + 8, kMonoS, t);
    };
    label(312, "SECT");
    for (int i = 0; i < 8; ++i) {
        const int si = secBank_ * 8 + i;
        const int x = 44 + i * 94, y = 312;
        if (si >= eng_.secCount) {
            g.color(kFaint);
            g.frame(x, y, 90, 24);
            continue;
        }
        const Section& c = eng_.sec[si];
        const bool cur = eng_.arrange && si == eng_.curSec && playing;
        const bool cue = !playing && si == eng_.startSec;
        if (cur) {
            g.color(kWhite);
            g.fill(x, y, 90, 24);
            g.color(kRed);
            g.fill(x, y + 21, int(eng_.secProgress * 90.0f), 3);
        } else {
            g.color(eng_.jumpReq == si && ((nowMs_ / 120) & 1) ? kRed : (cue ? kWhite : kDark));
            g.frame(x, y, 90, 24);
        }
        g.color(cur ? kBlack : kGrey);
        std::snprintf(buf, sizeof buf, "%02d %s %d", si + 1, secTypeShort(c.type), c.bars);
        g.text(x + 6, y + 8, kMonoS, buf);
    }
    label(342, "SCENE");
    for (int i = 0; i < kScenes; ++i) {
        const int x = 44 + i * 94, y = 342;
        const bool used = scene_[i].used;
        const bool tgt = (morphing_ && morphTo_ == i) || morphPending_ == i;
        if (tgt) {
            g.color(kRed);
            g.fill(x, y, int(90.0f * (morphPending_ == i ? 0.0f : morphProg_)), 24);
        }
        g.color(i == sceneSel_ ? kWhite : (used ? kGrey : kFaint));
        g.frame(x, y, 90, 24);
        static const char* const kSceneNames[4] = {"BASE", "SPARSE", "DENSE", "ULTRA"};
        if (used) std::snprintf(buf, sizeof buf, "%d %s", i + 1, i < 4 ? kSceneNames[i] : "SCENE");
        else std::snprintf(buf, sizeof buf, "%d -", i + 1);
        g.text(x + 6, y + 8, kMonoS, buf);
    }
    label(372, "THROW");
    const uint32_t th = eng_.throws;
    for (int i = 0; i < kThrows; ++i) {
        const int x = 44 + i * 94, y = 372;
        const bool held = (thHeld_ >> i) & 1u, latched = (thLatch_ >> i) & 1u;
        if (held || latched) {
            g.color(latched && !held ? kRed : kWhite);
            g.fill(x, y, 90, 24);
        } else {
            g.color(kDark);
            g.frame(x, y, 90, 24);
        }
        g.color((th >> i) & 1u ? kBlack : kGrey);
        g.text(x + 45, y + 12, kMonoS, throwName(i), Canvas::kCenter);
    }
    // field strip
    static const char* const kFields[6] = {"QUANTISE", "MORPH", "STUTTER", "FREEZE", "DEPTH", "SCRUB"};
    const ParamId ids[6] = {quantId_, morphId_, stutId_, freezeId_, depthId_, scrubId_[scrubSlot_]};
    for (int f = 0; f < 6; ++f) {
        const int x = 4 + f * 116, y = 404;
        const bool on = f == perfField_;
        if (on) {
            g.color(kWhite);
            g.fill(x, y, 112, 42);
        }
        g.color(on ? kBlack : kGrey);
        if (f == 5) {
            std::snprintf(buf, sizeof buf, "S%d SCRUB", scrubSlot_ + 1);
            g.text(x + 6, y + 4, kMonoS, buf);
        } else {
            g.text(x + 6, y + 4, kMonoS, kFields[f]);
        }
        P.format(ids[f], buf, sizeof buf, true);
        g.color(on ? kBlack : (f == 5 ? slotColor(scrubSlot_) : kWhite));
        g.text(x + 6, y + 20, kMono, buf);
    }
    // recorder
    const int rx = 704;
    g.color(perfRec_ ? kRed : (perfArm_ ? kRed : kDark));
    g.frame(rx, 404, 92, 42);
    g.color(perfRec_ || perfArm_ ? kRed : kGrey);
    g.text(rx + 6, 408, kMonoS, perfRec_ ? "REC" : (perfArm_ ? "ARMED" : "TAKE"));
    std::snprintf(buf, sizeof buf, "%d EV", perfN_);
    g.color(kWhite);
    g.text(rx + 6, 424, kMonoS, buf);
    g.color(perfReplay_ ? kWhite : kDark);
    g.text(rx + 6, 434, kMonoS, perfReplaying_ ? "REPLAYING" : (perfReplay_ ? "REPLAY ON" : "REPLAY OFF"));
    drawKeys(g, perfReplay_ ? "REPLAY*" : "REPLAY");
}

void Datamatics::drawMatrix(Canvas& g) {
    char buf[96];
    drawTop(g, "MATRIX");
    constexpr int bx = 184, by = 34, sc = 4, bw = 512;
    if (mxUploaded_) {
        g.clip(bx, by, bw, kMxH * sc);
        g.color(kWhite);
        g.bitmap332(bx, by, kRamMx, kMxW, kMxH, sc);
        g.noClip();
    }
    for (int s = 0; s < kStreams; ++s) {
        const int y = by + s * 32;
        const int* p = eng_.st[s].p;
        const int sl = p[P_SLOT] & 3;
        const bool aud = eng_.streamAudible(s);
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 60;
        g.color(aud ? slotColor(sl) : scale(slotColor(sl), 0.25f));
        g.fill(2, y + 3, 4, 26);
        g.color(s == sel_ ? kWhite : (aud ? kGrey : kDark));
        if (p[P_SYNC]) std::snprintf(buf, sizeof buf, "%d %s %s %d", s + 1, kSoundShort[p[P_SOUND] & 3], rateDivName(p[P_RATE_DIV]), eng_.thrFor(s));
        else std::snprintf(buf, sizeof buf, "%d %s %dMS %d", s + 1, kSoundShort[p[P_SOUND] & 3], p[P_RATE_MS], eng_.thrFor(s));
        g.text(10, y + 2, kMono, buf);
        drawSlotTag(g, 10, y + 21, s, 21);
        if (p[P_MUTE]) {
            g.color(kRed);
            g.text(168, y + 2, kMono, "M");
        }
        g.color(flash ? kWhite : kDark);
        std::snprintf(buf, sizeof buf, "%02X", lastVal_[s]);
        g.text(704, y + 2, kMono, buf);
        if (p[P_SOUND] == S_SINE) std::snprintf(buf, sizeof buf, "%5d", int(lastHz_[s]));
        else bits8(buf, lastVal_[s]);
        g.text(704, y + 20, kMonoS, buf);
    }
    g.color(kRed);
    g.fill(bx - 6, by + sel_ * 32 + 4, 2, 24);
    g.beginRects();
    const float px = float(bx + kMxPast * sc);
    g.vtx(px - 1, float(by));
    g.vtx(px - 1, float(by + kMxH * sc - 1));
    g.vtx(px + sc, float(by));
    g.vtx(px + sc, float(by + kMxH * sc - 1));
    g.endBatch();

    // section strip
    const int sy = 300, sx = bx, sw = bw;
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
    // scrolling numbers: the newest events
    for (int i = 0; i < 6; ++i) {
        const LogLine& l = log_[(logHead_ + 9 - i) % 10];
        const int y = 330 + i * 18;
        if (!evCount_ || uint32_t(i) >= evCount_) break;
        char b8[9];
        bits8(b8, l.val);
        if (l.sound == S_SINE || l.sound == S_NOISE)
            std::snprintf(buf, sizeof buf, "%d %s %08lX %s %3u %6dHZ", l.s + 1, kSoundShort[l.sound & 3],
                          static_cast<unsigned long>(l.idx), b8, unsigned(l.val), int(l.hz + 0.5f));
        else
            std::snprintf(buf, sizeof buf, "%d %s %08lX %s %3u %s", l.s + 1, kSoundShort[l.sound & 3],
                          static_cast<unsigned long>(l.idx), b8, unsigned(l.val), l.sound == S_IMPULSE ? "22.7US" : "CLICK");
        g.color(i == 0 ? slotColor(eng_.st[l.s].p[P_SLOT]) : (i < 3 ? kGrey : kDark));
        g.text(10, y, kMono, buf);
    }
    if (!wfAllDirty_) {
        g.clip(560, 330, 200, 108);
        drawWaterfall(g, 560, 330 - kWfH + 108, 1);
        g.noClip();
    }
    g.color(kDark);
    g.frame(560, 330, 200, 108);
    drawKeys(g, eng_.st[sel_].p[P_MUTE] ? "UNMUTE" : "MUTE");
}

void Datamatics::drawStream(Canvas& g) {
    char buf[64], val[32];
    drawTop(g, "STREAM");
    kit::Registry& P = ctx_->params();
    const int s = sel_;
    const int sl = eng_.st[s].p[P_SLOT] & 3;
    const Slot& slot = eng_.slot[sl];
    // header: the stream and the data it reads, in the slot's colour
    g.color(slotColor(sl));
    g.fill(0, 30, 6, 36);
    g.color(kWhite);
    std::snprintf(buf, sizeof buf, "STREAM %d", s + 1);
    g.text(14, 32, kMono, buf);
    g.color(slotColor(sl));
    std::snprintf(buf, sizeof buf, "S%d: %.28s (%s)", sl + 1, slot.path[0] ? baseName(slot.path) : "empty",
                  slot.n ? modeName(slot.mode) : "-");
    g.text(14, 50, kMonoS, buf);
    if (mxUploaded_) {
        g.clip(320, 34, 470, 32);
        g.color(kWhite);
        g.bitmap332(320, 34 - s * 32, kRamMx, kMxW, kMxH, 4);
        g.noClip();
        g.color(kRed);
        g.fill(320 + kMxPast * 4, 32, 4, 2);
    }
    const int rows = streamParamRows();
    for (int i = 0; i < rows; ++i) {
        const int pp = streamParamAt(i);
        const ParamId id = pid_[s][pp];
        const int col = i < 13 ? 0 : 1;
        const int r = i < 13 ? i : i - 13;
        const int x = 14 + col * 396, y = 74 + r * 28;
        const bool on = i == streamRow_;
        if (on) {
            g.color(pp == P_SLOT ? slotColor(sl) : kWhite);
            g.fill(x - 6, y - 4, 382, 24);
        }
        const int* p = eng_.st[s].p;
        bool used = true;
        if (pp == P_RATE_MS && p[P_SYNC]) used = false;
        if (pp == P_RATE_DIV && !p[P_SYNC]) used = false;
        if ((pp == P_LO || pp == P_HI || pp == P_SCALE) && p[P_SOUND] != S_SINE && p[P_SOUND] != S_NOISE) used = false;
        if ((pp == P_DUR || pp == P_DURMOD || pp == P_SHAPE) && p[P_SOUND] == S_IMPULSE) used = false;
        g.color(on ? kBlack : (used ? kGrey : kDark));
        g.text(x, y, kMono, paramInfo(pp).name);
        P.format(id, val, sizeof val, true);
        const bool mod = P.valid(id) && P.meta(id).mods;
        if (pp == P_SLOT) std::snprintf(val, sizeof val, "S%d %.18s", p[P_SLOT] + 1, baseName(eng_.slot[p[P_SLOT] & 3].path));
        g.color(on ? kBlack : (pp == P_SLOT ? slotColor(sl) : (used ? kWhite : kDark)));
        g.text(x + 366, y, kMono, val, Canvas::kRight);
        if (mod) {
            g.color(kRed);
            g.fill(x + 370, y + 4, 4, 8);
        }
    }
    g.color(kDark);
    std::snprintf(buf, sizeof buf, "EVENTS %lu   LEFT/RIGHT STREAM   F8 AUDITION   COPY / INSERT PASTE",
                  static_cast<unsigned long>(eng_.st[s].events));
    g.text(14, 438, kMonoS, buf);
    drawKeys(g, "AUDITION");
}

void Datamatics::drawData(Canvas& g) {
    char buf[96];
    drawTop(g, "DATA");
    // left: the four slots with their barcode and what they are
    for (int i = 0; i < kSlots; ++i) {
        const Slot& sl = eng_.slot[i];
        const SlotMeta& m = meta_[i];
        const int y = 32 + i * 104;
        const bool on = i == dataSlot_;
        g.color(slotColor(i));
        g.fill(4, y + 2, 6, 96);
        if (on) {
            g.color(kWhite);
            g.frame(0, y, 474, 100);
        }
        g.color(slotColor(i));
        std::snprintf(buf, sizeof buf, "S%d %.34s", i + 1, sl.path[0] ? baseName(sl.path) : "(empty)");
        g.text(16, y + 4, kMono, buf);
        g.color(kWhite);
        g.text(466, y + 4, kMono, sl.n ? modeName(sl.mode) : "-", Canvas::kRight);
        g.color(kGrey);
        if (sl.mode == kImage && sl.srcW)
            std::snprintf(buf, sizeof buf, "%dx%d PX  %lu VALUES  WANT %s", sl.srcW, sl.srcH, static_cast<unsigned long>(sl.n),
                          modeName(sl.wantMode));
        else
            std::snprintf(buf, sizeof buf, "%lu VALUES  %lu B  WANT %s", static_cast<unsigned long>(sl.n),
                          static_cast<unsigned long>(m.fileSize), modeName(sl.wantMode));
        g.text(16, y + 24, kMonoS, buf);
        std::snprintf(buf, sizeof buf, "ENTROPY %.2f  ONES %.1f%%  FNV %08lX", double(m.entropy), double(m.ones * 100.0f),
                      static_cast<unsigned long>(m.hash));
        g.text(16, y + 36, kMonoS, buf);
        if (bcUploaded_) {
            g.color(kWhite);
            g.bitmap332(16, y + 58, kRamBc + uint32_t(i) * kBcW * kBcH, kBcW, kBcH, 2);
        }
        g.color(kFaint);
        g.frame(15, y + 57, 402, 26);
        // read heads of the streams on this slot
        for (int s = 0; s < kStreams; ++s) {
            if ((eng_.st[s].p[P_SLOT] & 3) != i || !sl.n) continue;
            uint32_t idx = eng_.st[s].lastIdx;
            if (eng_.st[s].p[P_UNIT] == U_BIT) idx >>= 3;
            const int x = 16 + int((uint64_t(idx % sl.n) * 400u) / sl.n);
            g.color(kWhite);
            g.fill(x, y + 50, 2, 34);
            std::snprintf(buf, sizeof buf, "%d", s + 1);
            g.text(x + 3, y + 48, kMonoS, buf);
        }
        // the streams reading it, as numbers
        g.color(kGrey);
        g.text(424, y + 62, kMonoS, "READ BY");
        int k = 0;
        for (int s = 0; s < kStreams; ++s)
            if ((eng_.st[s].p[P_SLOT] & 3) == i) {
                buf[k++] = char('1' + s);
            }
        buf[k] = 0;
        g.color(slotColor(i));
        g.text(424, y + 74, kMonoS, k ? buf : "-");
    }
    // right: the patch, slots x streams
    const int gx = 520, gy = 60, cw = 33, chh = 34;
    g.color(kGrey);
    g.text(gx - 30, 34, kMonoS, "PATCH  STREAM -> SLOT");
    for (int s = 0; s < kStreams; ++s) {
        g.color(s == dataCol_ ? kWhite : kGrey);
        std::snprintf(buf, sizeof buf, "%d", s + 1);
        g.text(gx + s * cw + 12, gy - 12, kMonoS, buf);
        g.color(kDark);
        g.text(gx + s * cw + 4, gy + 4 * chh + 4, kMonoS, kSoundShort[eng_.st[s].p[P_SOUND] & 3]);
    }
    for (int i = 0; i < kSlots; ++i) {
        g.color(slotColor(i));
        std::snprintf(buf, sizeof buf, "S%d", i + 1);
        g.text(gx - 26, gy + i * chh + 12, kMonoS, buf);
    }
    g.beginRects();
    for (int s = 0; s < kStreams; ++s) {   // filled cells, per slot colour below
        (void)s;
    }
    g.endBatch();
    for (int i = 0; i < kSlots; ++i) {
        g.color(slotColor(i));
        g.beginRects();
        for (int s = 0; s < kStreams; ++s)
            if ((eng_.st[s].p[P_SLOT] & 3) == i) {
                g.vtx(float(gx + s * cw + 2), float(gy + i * chh + 2));
                g.vtx(float(gx + s * cw + cw - 3), float(gy + i * chh + chh - 3));
            }
        g.endBatch();
    }
    g.color(kFaint);
    g.beginLines();
    for (int s = 0; s <= kStreams; ++s) {
        g.vtx(float(gx + s * cw), float(gy));
        g.vtx(float(gx + s * cw), float(gy + kSlots * chh));
    }
    for (int i = 0; i <= kSlots; ++i) {
        g.vtx(float(gx), float(gy + i * chh));
        g.vtx(float(gx + kStreams * cw), float(gy + i * chh));
    }
    g.endBatch();
    g.color(kRed);
    g.frame(gx + dataCol_ * cw, gy + dataSlot_ * chh, cw + 1, chh + 1);
    // selected slot: how to change it
    const Slot& cur = eng_.slot[dataSlot_];
    const int ty = 232;
    g.color(slotColor(dataSlot_));
    std::snprintf(buf, sizeof buf, "S%d  READ MODE %s", dataSlot_ + 1, modeName(cur.wantMode));
    g.text(490, ty, kMono, buf);
    g.color(kGrey);
    g.text(490, ty + 24, kMonoS, "ENCODER        reading mode");
    g.text(490, ty + 36, kMonoS, "SHIFT+ENCODER  built-in data");
    g.text(490, ty + 48, kMonoS, "UP/DOWN slot  LEFT/RIGHT stream");
    g.text(490, ty + 60, kMonoS, "INSERT  patch stream -> slot");
    g.text(490, ty + 72, kMonoS, "F8 LOAD  DELETE clear slot");
    g.text(490, ty + 84, kMonoS, "PADS: row = slot, col 1-8 patch");
    g.text(490, ty + 96, kMonoS, "9 load 10 mode 11 solo 12 built-in");
    if (loadBusy()) {
        g.color(kRed);
        std::snprintf(buf, sizeof buf, "LOADING %lu%%",
                      static_cast<unsigned long>(job_.limit ? uint64_t(job_.pos) * 100u / job_.limit : 0));
        g.text(490, ty + 120, kMono, buf);
    } else if (status_[0] && int32_t(statusUntil_ - nowMs_) > 0) {
        g.color(kWhite);
        g.text(490, ty + 120, kMonoS, status_);
    }
    if (!ctx_->fs) {
        g.color(kRed);
        g.text(490, ty + 140, kMonoS, "NO CARD - BUILT-IN DATA ONLY");
    }
    drawKeys(g, "LOAD");
}

void Datamatics::drawSong(Canvas& g) {
    char buf[96];
    drawTop(g, "SONG");
    const int x0 = 60, w = 720, ty = 40, th = 64;
    const int total = eng_.totalBars();
    int acc = 0;
    for (int i = 0; i < eng_.secCount; ++i) {
        const Section& c = eng_.sec[i];
        const int a = x0 + acc * w / (total ? total : 1);
        acc += c.bars;
        const int b = x0 + acc * w / (total ? total : 1) - 2;
        const int bw = b - a;
        const bool cur = eng_.arrange && i == eng_.curSec && ctx_->playing();
        g.color(cur ? kWhite : kGrey);
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
            default:
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
        if (eng_.jumpReq == i && ((nowMs_ / 120) & 1)) {
            g.color(kRed);
            g.fill(a, ty + th - 4, bw, 4);
        }
        if (i == eng_.startSec && !ctx_->playing()) {
            g.color(kWhite);
            g.fill(a, ty - 6, 6, 4);
        }
    }
    // stream x section matrix, rows in the streams' slot colours
    const int my = 128, mh = 18;
    for (int s = 0; s < kStreams; ++s) {
        g.color(slotColor(eng_.st[s].p[P_SLOT]));
        std::snprintf(buf, sizeof buf, "%d", s + 1);
        g.text(36, my + s * mh + 4, kMonoS, buf);
    }
    for (int s = 0; s < kStreams; ++s) {
        g.color(scale(slotColor(eng_.st[s].p[P_SLOT]), 0.8f));
        g.beginRects();
        acc = 0;
        for (int i = 0; i < eng_.secCount; ++i) {
            const Section& c = eng_.sec[i];
            const int a = x0 + acc * w / (total ? total : 1);
            acc += c.bars;
            const int b = x0 + acc * w / (total ? total : 1) - 3;
            if (c.type == T_SILENCE || !((c.mask >> s) & 1)) continue;
            g.vtx(float(a + 1), float(my + s * mh + 2));
            g.vtx(float(b), float(my + s * mh + mh - 3));
        }
        g.endBatch();
    }
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
            }
        } else {
            std::snprintf(buf, sizeof buf, "%d%%", c.depth);
            g.text(x, dy + 16, kMono, buf);
        }
    }
    g.color(kWhite);
    g.text(20, 346, kMono, kSecDesc[c.type % T_COUNT]);
    g.color(kGrey);
    const float secs = float(total) * 4.0f * 60.0f / (ctx_->bpm() > 1.0f ? ctx_->bpm() : 120.0f);
    std::snprintf(buf, sizeof buf, "%s  %d SECTIONS  %d BARS  %d:%02d  LAUNCH %s%s",
                  eng_.songMode ? "CHAIN" : "LIVE", eng_.secCount, total, int(secs) / 60, int(secs) % 60,
                  quantName(eng_.quant), eng_.arrange ? "" : "  (ARRANGEMENT OFF)");
    g.text(20, 368, kMono, buf);
    g.color(kDark);
    g.text(20, 392, kMonoS, "LIVE: a section repeats until you launch another   CHAIN: the sections play in order");
    g.text(20, 406, kMonoS, "LEFT/RIGHT section  UP/DOWN field  ENCODER value  SHIFT+ENC / NOTE toggle mask bit");
    g.text(20, 420, kMonoS, "INSERT duplicate  DELETE remove  F8 launch (stopped: start here)  SHIFT+F8 LIVE/CHAIN");
    // the recorded take as a strip of event ticks
    if (perfN_ && perfLen_) {
        g.color(kDark);
        g.frame(20, 434, 760, 12);
        g.color(kRed);
        g.beginRects();
        const int step = perfN_ > 380 ? perfN_ / 380 : 1;
        for (int i = 0; i < perfN_; i += step) {
            const int x = 20 + int((uint64_t(perf_[i].t) * 758u) / perfLen_);
            g.vtx(float(x), 436.0f);
            g.vtx(float(x), 444.0f);
        }
        g.endBatch();
    }
    drawKeys(g, ctx_->playing() ? "LAUNCH" : "CUE");
}

void Datamatics::drawMixer(Canvas& g) {
    char buf[32];
    drawTop(g, "MIXER");
    kit::Registry& P = ctx_->params();
    static const char* const kFieldNames[5] = {"LEVEL", "PAN", "PANMODE", "STEM", "MIDI"};
    static const uint8_t kFields[5] = {P_LEVEL, P_PAN, P_PANMODE, P_STEM, P_MIDI};
    for (int s = 0; s < kStreams; ++s) {
        const int x = 14 + s * 98, w = 86;
        const int* p = eng_.st[s].p;
        const int sl = p[P_SLOT] & 3;
        const bool on = s == sel_;
        const bool aud = eng_.streamAudible(s);
        g.color(slotColor(sl));
        g.fill(x, 30, w, 4);
        g.color(on ? kWhite : kGrey);
        std::snprintf(buf, sizeof buf, "%d %s", s + 1, kSoundShort[p[P_SOUND] & 3]);
        g.text(x, 38, kMono, buf);
        g.color(slotColor(sl));
        std::snprintf(buf, sizeof buf, "S%d %.6s", sl + 1, eng_.slot[sl].path[0] ? baseName(eng_.slot[sl].path) + (eng_.slot[sl].path[0] == '@' ? 1 : 0) : "-");
        g.text(x, 56, kMonoS, buf);
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 50;
        g.color(flash ? slotColor(sl) : kFaint);
        g.fill(x + w - 12, 40, 10, 10);
        const int fy = 72, fh = 200, segs = 40;
        const int lvl = int(P.effective(pid_[s][P_LEVEL]) + 0.5f);
        const int lit = lvl * segs / 100;
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
        const int py = fy + fh + 16;
        g.color(kDark);
        g.hline(x + 4, py, w - 8);
        g.vline(x + w / 2, py - 3, 7);
        g.color(kWhite);
        g.fill(x + w / 2 + p[P_PAN] * (w / 2 - 6) / 100 - 2, py - 4, 4, 9);
        for (int f = 1; f < 5; ++f) {
            const int y = fy + fh + 28 + (f - 1) * 22;
            const bool fs = on && f == mixField_;
            if (fs) {
                g.color(kWhite);
                g.fill(x, y - 3, w, 20);
            }
            P.format(pid_[s][kFields[f]], buf, sizeof buf, true);
            g.color(fs ? kBlack : kGrey);
            g.text(x + 4, y, kMono, buf);
        }
        if (p[P_MUTE] || p[P_SOLO]) {
            g.color(p[P_MUTE] ? kRed : kWhite);
            g.text(x + 4, fy + fh + 28 + 4 * 22, kMono, p[P_MUTE] ? "MUTED" : "SOLO");
        }
        std::snprintf(buf, sizeof buf, "%d", lvl);
        g.color(on && mixField_ == 0 ? kRed : kGrey);
        g.text(x + w / 2, fy + fh + 2, kMonoS, buf, Canvas::kCenterX);
        if (on) {
            g.color(kWhite);
            g.frame(x - 4, 28, w + 8, 420);
        }
    }
    g.color(kDark);
    g.text(400, 440, kMonoS, kFieldNames[mixField_], Canvas::kCenterX);
    drawKeys(g, "MUTE");
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
        g.text(20, 60, kMono, "none yet - INSERT saves one");
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
    const int x = 340;
    g.color(kWhite);
    g.text(x, 36, font::kHuge, pieceName_);
    g.color(kRed);
    g.fill(x, 76, 8, 8);
    const int total = eng_.totalBars();
    int scenes = 0;
    for (int i = 0; i < kScenes; ++i) scenes += scene_[i].used;
    g.color(kGrey);
    std::snprintf(buf, sizeof buf, "%d SECTIONS %d BARS  %d SCENES  TAKE %d EV  %d BPM", eng_.secCount, total, scenes,
                  perfN_, int(ctx_->bpm() + 0.5f));
    g.text(x, 100, kMonoS, buf);
    for (int i = 0; i < kSlots; ++i) {
        const Slot& sl = eng_.slot[i];
        std::snprintf(buf, sizeof buf, "S%d %-5s %.40s", i + 1, modeName(sl.wantMode), sl.path[0] ? sl.path : "-");
        g.color(sl.n ? slotColor(i) : kDark);
        g.text(x, 124 + i * 20, kMono, buf);
    }
    g.color(kDark);
    std::snprintf(buf, sizeof buf, "/%s/Pieces/*.dmx   text, version %d", ctx_->home, kPieceVersion);
    g.text(x, 220, kMonoS, buf);
    g.text(x, 240, kMonoS, "INSERT save as new     F8 load selected");
    g.text(x, 254, kMonoS, "COPY overwrite selected  SHIFT+INSERT new (demo)");
    g.text(x, 268, kMonoS, "DELETE twice: remove the file");
    g.text(x, 282, kMonoS, "saved: streams, sections, slots, scenes, the take,");
    g.text(x, 296, kMonoS, "macros, LFO routes and MIDI learn (LABKIT chunk)");
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
    drawKeys(g, "LOAD");
}

// ---- pads --------------------------------------------------------------------------------------
void Datamatics::leds(Rgb* pads) {
    const bool playing = ctx_->playing();
    if (page_ == PG_MATRIX) {
        for (int row = 0; row < 4; ++row) {
            const int s = bank_ * 4 + row;
            const Stream& x = eng_.st[s];
            const int si = x.p[P_SLOT] & 3;
            const Slot& sl = eng_.slot[si];
            const bool aud = eng_.streamAudible(s);
            const bool flash = playing && nowMs_ - flashMs_[s] < 45;
            const uint32_t scan = eng_.scanOffset(s);
            for (int c = 0; c < 8; ++c) {
                Rgb col;
                if (sl.n) {
                    uint8_t v, a;
                    readValue(sl, x.p[P_UNIT], readIndex(x, sl, x.k + uint32_t(c), scan), v, a);
                    if (wouldHit(s, v)) col = scale(slotColor(si), aud ? 0.15f + 0.85f * float(v) / 255.0f : 0.06f);
                }
                if (c == 0 && flash) col = Rgb(0xFFFFFF);
                pads[padAt(c, row)] = col;
            }
            pads[padAt(8, row)] = x.p[P_MUTE] ? Rgb(0xFF2000) : Rgb(0x000000);
            pads[padAt(9, row)] = x.p[P_SOLO] ? Rgb(0xFFFFFF) : Rgb(0x000000);
            pads[padAt(10, row)] = s == sel_ ? Rgb(0xFFFFFF) : scale(slotColor(si), 0.1f);
            pads[padAt(11, row)] = (fill_ >> s) & 1u ? Rgb(0xFFFFFF) : scale(slotColor(si), 0.08f);
        }
        return;
    }
    if (page_ == PG_DATA) {
        for (int row = 0; row < kSlots; ++row) {
            for (int c = 0; c < 8; ++c) {
                const bool on = (eng_.st[c].p[P_SLOT] & 3) == row;
                Rgb col = on ? slotColor(row) : Rgb(0x060606);
                if (on && playing && nowMs_ - flashMs_[c] < 45) col = Rgb(0xFFFFFF);
                pads[padAt(c, row)] = col;
            }
            pads[padAt(8, row)] = scale(slotColor(row), 0.3f);
            pads[padAt(9, row)] = scale(slotColor(row), 0.12f);
            pads[padAt(10, row)] = (slotSoloMask_ >> row) & 1u ? Rgb(0xFFFFFF) : scale(slotColor(row), 0.06f);
            pads[padAt(11, row)] = Rgb(0x101010);
        }
        return;
    }
    // PERFORM layout (every other page)
    const uint32_t th = eng_.throws;
    for (int s = 0; s < kStreams; ++s) {
        const int si = eng_.st[s].p[P_SLOT] & 3;
        const bool muted = eng_.st[s].p[P_MUTE] != 0;
        const bool flash = playing && nowMs_ - flashMs_[s] < 45;
        Rgb c = muted ? scale(slotColor(si), 0.05f) : scale(slotColor(si), 0.55f);
        if (eng_.st[s].p[P_SOLO]) c = slotColor(si);
        if (flash && !muted) c = Rgb(0xFFFFFF);
        if (slotHeld_ >= 0 && si == slotHeld_) c = slotColor(si);
        pads[padAt(s, 0)] = c;
        pads[padAt(s, 1)] = (fill_ >> s) & 1u ? Rgb(0xFFFFFF) : scale(slotColor(si), 0.1f);
        const int sec = secBank_ * 8 + s;
        Rgb sc;
        if (sec < eng_.secCount) {
            if (playing && eng_.arrange && sec == eng_.curSec) sc = Rgb(0xFF2000);
            else if (!playing && sec == eng_.startSec) sc = Rgb(0x707070);
            else sc = Rgb(0x141414);
            if (eng_.jumpReq == sec && (nowMs_ / 80) & 1) sc = Rgb(0xFFFFFF);
        }
        pads[padAt(s, 2)] = sc;
        Rgb scn;
        if (scene_[s].used) scn = Rgb(0x303030);
        if (s == sceneSel_) scn = scene_[s].used ? Rgb(0x909090) : Rgb(0x101010);
        if ((morphing_ && morphTo_ == s) || (morphPending_ == s && (nowMs_ / 80) & 1)) scn = Rgb(0xFF2000);
        pads[padAt(s, 3)] = scn;
    }
    for (int i = 0; i < kSlots; ++i) {
        Rgb c = scale(slotColor(i), 0.35f);
        if ((slotSoloMask_ >> i) & 1u) c = slotColor(i);
        if (slotHeld_ == i) c = Rgb(0xFFFFFF);
        pads[padAt(8 + i, 0)] = c;
    }
    for (int t = 0; t < kThrows; ++t) {
        const int row = 1 + t / 4, col = 8 + t % 4;
        Rgb c = Rgb(0x1A0400);
        if ((thLatch_ >> t) & 1u) c = Rgb(0xFF2000);
        if ((thHeld_ >> t) & 1u) c = Rgb(0xFFFFFF);
        (void)th;
        pads[padAt(col, row)] = c;
    }
    pads[padAt(8, 3)] = perfRec_ ? Rgb(0xFF2000) : (perfArm_ ? ((nowMs_ / 200) & 1 ? Rgb(0xFF2000) : Rgb(0x200000)) : Rgb(0x200000));
    pads[padAt(9, 3)] = perfReplaying_ ? ((nowMs_ / 200) & 1 ? Rgb(0xFFFFFF) : Rgb(0x404040)) : (perfReplay_ ? Rgb(0xFFFFFF) : Rgb(0x101010));
    pads[padAt(10, 3)] = Rgb(0x404040);
    pads[padAt(11, 3)] = Rgb(0x300800);
}

}  // namespace dm
