// DATAMATICS -- VIEW: full-screen datamatics visual modes.
//
//   STREAM   the waterfall: every stream's read head over time (RGB332 ring)
//   BARCODE  eight rows of bits under the read heads, digit strips, solid rows on events
//   TRANSIT  the bytes around the head on a rail, a fan to the same bytes sorted
//   GRID     the byte-pair network (b[i], b[i+1]) with a crosshair on the head
//   SPATIAL  byte triplets as a rotating point cloud, the streams as red crosses
//
// The modes follow the design of the user's IKEDAT / PINDATA visual engine
// (Ikeda datamatics), redrawn for the FT81x display list. A held CUT throw
// inverts the frame (white ground, black data).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ws/dm_ws.h"

namespace dm {

using namespace lab;

namespace {
constexpr uint8_t kMono = 18, kMonoS = 16;
const Rgb kWhite(0xFFFFFF), kBlack(0x000000), kGrey(0x8A8A8A), kDark(0x3A3A3A), kRed(0xFF2A00);
const char* const kModeShort[kModeCount] = {"AUTO", "RAW", "BITS", "IMG", "WAV", "MIDI", "TXT"};

bool gInv = false;   // CUT held: inverted frame
inline Rgb ink(Rgb c) { return gInv ? (c.r == c.g && c.g == c.b ? Rgb(uint8_t(255 - c.r), uint8_t(255 - c.g), uint8_t(255 - c.b)) : c) : c; }

const char* baseName(const char* path) {
    const char* nm = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/') nm = q + 1;
    return nm;
}
}  // namespace

// The 1024 bytes around the followed stream's read head (win_[512] = the head).
void Datamatics::fillWindow() {
    const int s = (follow_ - 1) & 7;
    const Slot& sl = eng_.slot[eng_.st[s].p[P_SLOT] & 3];
    const uint32_t n = sl.n;
    if (!n) {
        for (int i = 0; i < 1024; ++i) win_[i] = uint8_t((i * 37) ^ (i >> 3));
        return;
    }
    uint32_t idx = eng_.st[s].lastIdx;
    if (eng_.st[s].p[P_UNIT] == U_BIT) idx >>= 3;
    const uint32_t start = uint32_t((uint64_t(idx % n) + uint64_t(n) * 4u - 512u) % n);
    for (int i = 0; i < 1024; ++i) win_[i] = sl.v[(start + uint32_t(i)) % n];
}

void Datamatics::drawView(Canvas& g) {
    gInv = (eng_.throws & TH_CUT) != 0;
    if (gInv) {
        g.color(kWhite);
        g.fill(0, 0, 800, 480);
    }
    fillWindow();
    yaw_ += 0.012f + 0.05f * level_;
    if (yaw_ > 6.2831853f) yaw_ -= 6.2831853f;
    switch (visMode_) {
        case VM_BARCODE: visBarcode(g); break;
        case VM_TRANSIT: visTransit(g); break;
        case VM_GRID: visGrid(g); break;
        case VM_SPATIAL: visSpatial(g); break;
        default:
            drawWaterfall(g, 0, 0, 2);
            if (!ctx_->playing()) {
                g.color(kGrey);
                g.text(400, 240, kMono, "PLAY TO WRITE THE WATERFALL", Canvas::kCenter);
            }
            break;
    }
    visOverlay(g);
    gInv = false;
}

void Datamatics::visBarcode(Canvas& g) {
    // eight rows: the bits of the units around each read head, 4 px cells
    char buf[128];
    const int top = 26, rowH = 52, barH = 36, cells = 196, x0 = 8;
    for (int s = 0; s < kStreams; ++s) {
        const Stream& x = eng_.st[s];
        const int si = x.p[P_SLOT] & 3;
        const Slot& sl = eng_.slot[si];
        const int y = top + s * rowH;
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 50;
        const bool aud = eng_.streamAudible(s);
        if (!sl.n) continue;
        uint32_t idx = x.lastIdx;
        if (x.p[P_UNIT] == U_BIT) idx >>= 3;
        const uint32_t n = sl.n;
        const uint32_t b0 = uint32_t((uint64_t(idx % n) + uint64_t(n) * 8u - 12u) % n);   // 24.5 bytes: 196 bits
        auto bit = [&](int i) -> bool {
            const uint8_t v = sl.v[(b0 + uint32_t(i >> 3)) % n];
            return (v >> (7 - (i & 7))) & 1u;
        };
        const Rgb col = aud ? slotColor(si) : scale(slotColor(si), 0.3f);
        if (flash) {   // a solid row on an event, bits cut out of it
            g.color(col);
            g.fill(x0, y, cells * 4, barH);
            g.color(gInv ? kWhite : kBlack);
        } else {
            g.color(col);
        }
        g.beginRects();
        int i = 0;
        while (i < cells) {
            if (!bit(i)) {
                ++i;
                continue;
            }
            int j = i;
            while (j < cells && bit(j)) ++j;
            g.vtx(float(x0 + i * 4), float(y));
            g.vtx(float(x0 + j * 4 - 2), float(y + barH - 1));
            i = j;
        }
        g.endBatch();
        // the head: unit 96..103 (byte 12)
        g.color(kRed);
        g.fill(x0 + 96 * 4, y - 2, 2, barH + 4);
        // digit strip: the byte values under the bars
        int k = 0;
        for (int c = 0; c < 24 && k < 120; ++c) k += std::snprintf(buf + k, sizeof buf - size_t(k), "%03u ", unsigned(sl.v[(b0 + uint32_t(c)) % n]));
        g.color(ink(kGrey));
        g.text(x0, y + barH + 3, kMonoS, buf);
    }
}

void Datamatics::visTransit(Canvas& g) {
    // 128 bytes around the head: a rail, the same bytes sorted on a second rail,
    // a fan of lines from each byte to its sorted place; histograms above / below
    const int n = 128, m = 80;
    const float cw = 640.0f / float(n);
    const uint8_t* w = win_ + 512 - 64;
    // counting sort: sortedPos[i] = place of byte i in the sorted order
    uint16_t cnt[257] = {};
    for (int i = 0; i < n; ++i) ++cnt[w[i] + 1];
    for (int v = 1; v < 257; ++v) cnt[v] = uint16_t(cnt[v] + cnt[v - 1]);
    uint8_t sortedPos[128], order[128];
    uint16_t fill[256];
    for (int v = 0; v < 256; ++v) fill[v] = cnt[v];
    for (int i = 0; i < n; ++i) {
        const int p = fill[w[i]]++;
        sortedPos[i] = uint8_t(p);
        order[p] = uint8_t(i);
    }
    const int si = eng_.st[(follow_ - 1) & 7].p[P_SLOT] & 3;
    const Rgb col = slotColor(si);
    const float yT = 196.0f, yB = 300.0f;
    // bit lines at the top
    g.color(ink(kWhite));
    for (int L = 0; L < 2; ++L) {
        g.beginRects();
        int i = 0;
        const int cells = 160;
        while (i < cells) {
            auto b = [&](int k) { return (win_[256 + L * 32 + (k >> 3)] >> (7 - (k & 7))) & 1; };
            if (!b(i)) {
                ++i;
                continue;
            }
            int j = i;
            while (j < cells && b(j)) ++j;
            g.vtx(float(m + i * 4), float(40 + L * 8));
            g.vtx(float(m + j * 4 - 2), float(44 + L * 8));
            i = j;
        }
        g.endBatch();
    }
    // histogram above the top rail
    g.color(col);
    g.beginRects();
    for (int i = 0; i < n; i += 2) {
        const float h = float(w[i]) * (110.0f / 255.0f) * (0.6f + 0.6f * level_);
        g.vtx(float(m) + float(i) * cw, yT - 8.0f - h);
        g.vtx(float(m) + float(i) * cw + cw * 2.0f - 2.0f, yT - 8.0f);
    }
    g.endBatch();
    // rails
    g.color(ink(kWhite));
    g.beginRects();
    for (int i = 0; i < n; ++i) {
        if (w[i] >= 128) {
            g.vtx(float(m) + float(i) * cw, yT);
            g.vtx(float(m) + float(i) * cw + cw - 2.0f, yT + 7.0f);
        }
        if (w[order[i]] >= 128) {
            g.vtx(float(m) + float(i) * cw, yB);
            g.vtx(float(m) + float(i) * cw + cw - 2.0f, yB + 7.0f);
        }
    }
    g.endBatch();
    g.color(ink(kDark));
    g.beginRects();
    for (int i = 0; i < n; ++i) {
        if (w[i] < 128) {
            g.vtx(float(m) + float(i) * cw, yT);
            g.vtx(float(m) + float(i) * cw + cw - 2.0f, yT + 7.0f);
        }
        if (w[order[i]] < 128) {
            g.vtx(float(m) + float(i) * cw, yB);
            g.vtx(float(m) + float(i) * cw + cw - 2.0f, yB + 7.0f);
        }
    }
    g.endBatch();
    // the fan
    g.color(ink(kGrey));
    g.beginLines();
    const float jit = level_ * 3.0f;
    for (int i = 0; i < n; ++i) {
        g.vtx(float(m) + (float(i) + 0.5f) * cw, yT + 8.0f);
        g.vtx(float(m) + (float(sortedPos[i]) + 0.5f) * cw + ((i & 1) ? jit : -jit), yB - 1.0f);
    }
    g.endBatch();
    // the head: red vertical above, at its sorted place below
    g.color(kRed);
    g.fill(m + int(64.0f * cw), 32, 2, int(yT) - 32);
    g.fill(m + int(float(sortedPos[64]) * cw), int(yB) + 10, 2, 120);
    g.beginLines();
    g.vtx(float(m) + 64.5f * cw, yT + 8.0f);
    g.vtx(float(m) + (float(sortedPos[64]) + 0.5f) * cw, yB - 1.0f);
    g.endBatch();
    // sorted histogram hanging below
    g.color(scale(col, 0.7f));
    g.beginRects();
    for (int i = 0; i < n; i += 2) {
        const float h = float(w[order[i]]) * (100.0f / 255.0f);
        g.vtx(float(m) + float(i) * cw, yB + 14.0f);
        g.vtx(float(m) + float(i) * cw + cw * 2.0f - 2.0f, yB + 14.0f + h);
    }
    g.endBatch();
}

void Datamatics::visGrid(Canvas& g) {
    // byte-pair network: (b[i], b[i+1]) for 320 bytes before the head
    char buf[48];
    const int ax = 90, ay = 44, aw = 640, ah = 384;
    g.color(ink(Rgb(0x1C1C1C)));
    g.beginLines();
    for (int x = 0; x <= aw; x += 40) {
        g.vtx(float(ax + x), float(ay));
        g.vtx(float(ax + x), float(ay + ah));
    }
    for (int y = 0; y <= ah; y += 48) {
        g.vtx(float(ax), float(ay + y));
        g.vtx(float(ax + aw), float(ay + y));
    }
    g.endBatch();
    g.color(ink(kGrey));
    for (int k = 0; k <= 8; k += 2) {
        std::snprintf(buf, sizeof buf, "%02X", k * 32 > 255 ? 255 : k * 32);
        g.text(ax - 24, ay + ah - k * ah / 8 - 4, kMonoS, buf);
    }
    auto px = [&](int i) { return float(ax) + float(win_[i]) / 255.0f * float(aw); };
    auto py = [&](int i) { return float(ay + ah) - float(win_[i + 1]) / 255.0f * float(ah); };
    const int si = eng_.st[(follow_ - 1) & 7].p[P_SLOT] & 3;
    g.color(gInv ? kBlack : scale(slotColor(si), 0.55f + 0.45f * level_));
    g.beginStrip();
    for (int i = 512 - 320; i <= 512; ++i) g.vtx(px(i), py(i));
    g.endBatch();
    g.color(kRed);
    g.lineWidth(2.0f);
    g.beginStrip();
    for (int i = 512 - 12; i <= 512; ++i) g.vtx(px(i), py(i));
    g.endBatch();
    g.lineWidth(1.0f);
    const float hx = px(512), hy = py(512);
    g.fill(ax, int(hy), aw, 1);
    g.fill(int(hx), ay, 1, ah);
    g.frame(int(hx) - 5, int(hy) - 5, 11, 11);
    g.color(gInv ? kWhite : kBlack);
    g.fill(int(hx) + 8, int(hy) + 6, 150, 24);
    g.color(ink(kWhite));
    const int s = (follow_ - 1) & 7;
    std::snprintf(buf, sizeof buf, "OFFSET 0x%06lX", static_cast<unsigned long>(eng_.st[s].lastIdx));
    g.text(int(hx) + 10, int(hy) + 7, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "VALUE %3u NEXT %3u", unsigned(win_[512]), unsigned(win_[513]));
    g.text(int(hx) + 10, int(hy) + 19, kMonoS, buf);
}

void Datamatics::visSpatial(Canvas& g) {
    // byte triplets as points in a rotating cube; the streams as red crosses
    const float ca = cosf(yaw_), sa = sinf(yaw_);
    const float pt = -0.42f, cb = cosf(pt), sb = sinf(pt);
    auto proj = [&](float x, float y, float z, float& ox, float& oy) {
        const float rx = x * ca - z * sa;
        float rz = x * sa + z * ca;
        const float ry = y * cb - rz * sb;
        rz = y * sb + rz * cb;
        const float s = 330.0f / (rz + 3.4f);
        ox = 400.0f + rx * s * 1.2f;
        oy = 226.0f - ry * s;
    };
    float ox, oy;
    // floor dots
    g.color(ink(kDark));
    g.pointSize(1.0f);
    g.beginPoints();
    for (int a = -6; a <= 6; ++a)
        for (int b = -6; b <= 6; ++b) {
            proj(float(a) * 0.28f, -1.05f, float(b) * 0.28f, ox, oy);
            g.vtx(ox, oy);
        }
    g.endBatch();
    // cube
    static const int8_t c8[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    static const uint8_t ed[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    g.color(ink(kGrey));
    g.beginLines();
    for (const auto& e : ed) {
        proj(c8[e[0]][0], c8[e[0]][1], c8[e[0]][2], ox, oy);
        g.vtx(ox, oy);
        proj(c8[e[1]][0], c8[e[1]][1], c8[e[1]][2], ox, oy);
        g.vtx(ox, oy);
    }
    g.endBatch();
    // the cloud
    const int si = eng_.st[(follow_ - 1) & 7].p[P_SLOT] & 3;
    g.color(gInv ? kBlack : slotColor(si));
    g.pointSize(1.2f + level_ * 1.2f);
    g.beginPoints();
    for (int i = 0; i < 340; ++i) {
        const int o = 2 + i * 3;
        proj(float(win_[o]) / 127.5f - 1.0f, float(win_[o + 1]) / 127.5f - 1.0f, float(win_[o + 2]) / 127.5f - 1.0f, ox, oy);
        g.vtx(ox, oy);
    }
    g.endBatch();
    // streams: value, aux and step position as coordinates
    char buf[32];
    for (int s = 0; s < kStreams; ++s) {
        const Stream& x = eng_.st[s];
        const float vx = float(lastVal_[s]) / 127.5f - 1.0f;
        const float vy = float(x.lastAux) / 127.5f - 1.0f;
        const float vz = float(x.k & 63u) / 31.5f - 1.0f;
        proj(vx, vy, vz, ox, oy);
        const bool flash = ctx_->playing() && nowMs_ - flashMs_[s] < 80;
        const float r = flash ? 9.0f : 4.0f;
        g.color(kRed);
        g.beginLines();
        g.vtx(ox - r, oy);
        g.vtx(ox + r, oy);
        g.vtx(ox, oy - r);
        g.vtx(ox, oy + r);
        g.endBatch();
        if (flash) {
            g.color(ink(kWhite));
            std::snprintf(buf, sizeof buf, "D%d %u %dHZ", s + 1, unsigned(lastVal_[s]), int(lastHz_[s]));
            g.text(int(ox) + 10, int(oy) - 6, kMonoS, buf);
        }
    }
}

void Datamatics::visOverlay(Canvas& g) {
    char buf[80];
    // mode tabs, top left
    g.color(gInv ? kWhite : kBlack);
    g.fill(0, 0, 800, 18);
    for (int m = 0; m < VM_COUNT; ++m) {
        const bool on = m == visMode_;
        if (on) {
            g.color(gInv ? kBlack : kWhite);
            g.fill(4 + m * 84, 1, 80, 16);
        }
        g.color(on ? (gInv ? kWhite : kBlack) : ink(kGrey));
        g.text(44 + m * 84, 9, kMonoS, visName(m), Canvas::kCenter);
    }
    const int cs = eng_.curSec;
    char t[24];
    const uint32_t ms = uint32_t((uint64_t(ctx_->playing() ? eng_.clock : 0) * 1000u) / 44100u);
    std::snprintf(t, sizeof t, "%02lu:%02lu.%03lu", static_cast<unsigned long>(ms / 60000u),
                  static_cast<unsigned long>((ms / 1000u) % 60u), static_cast<unsigned long>(ms % 1000u));
    std::snprintf(buf, sizeof buf, "%02d %s  %s%s", cs + 1, eng_.arrange ? secTypeName(eng_.sec[cs < eng_.secCount ? cs : 0].type) : "FREE", t,
                  visAuto_ ? "  AUTO" : "");
    g.color(ink(kWhite));
    g.text(796, 5, kMonoS, buf, Canvas::kRight);
    if (visMode_ == VM_STREAM || visMode_ == VM_BARCODE) {
        // stream legend for the column / row modes
        if (visMode_ == VM_STREAM)
            for (int s = 0; s < kStreams; ++s) {
                g.color(slotColor(eng_.st[s].p[P_SLOT]));
                std::snprintf(buf, sizeof buf, "%d S%d", s + 1, (eng_.st[s].p[P_SLOT] & 3) + 1);
                g.text(s * 100 + 4, 22, kMonoS, buf);
            }
        return;
    }
    // meta block, bottom left: what the followed stream reads
    const int s = (follow_ - 1) & 7;
    const int si = eng_.st[s].p[P_SLOT] & 3;
    const Slot& sl = eng_.slot[si];
    const SlotMeta& m = meta_[si];
    const int y0 = 386;
    g.color(gInv ? kWhite : kBlack);
    g.fill(4, y0 - 4, 312, 92);
    g.color(slotColor(si));
    g.fill(4, y0 - 4, 4, 92);
    std::snprintf(buf, sizeof buf, "STREAM %d  S%d %.20s", s + 1, si + 1, sl.path[0] ? baseName(sl.path) : "-");
    g.text(14, y0, kMonoS, buf);
    g.color(ink(kWhite));
    std::snprintf(buf, sizeof buf, "TYPE    %s  %lu VALUES", sl.n ? modeName(sl.mode) : "-", static_cast<unsigned long>(sl.n));
    g.text(14, y0 + 12, kMonoS, buf);
    if (sl.path[0] == '@') std::snprintf(buf, sizeof buf, "SIZE    BUILT-IN DATA SET");
    else std::snprintf(buf, sizeof buf, "SIZE    %lu B", static_cast<unsigned long>(m.fileSize));
    g.text(14, y0 + 24, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "ENTROPY %.3f BITS  ONES %.1f%%", double(m.entropy), double(m.ones * 100.0f));
    g.text(14, y0 + 36, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "FNV1A   %08lX", static_cast<unsigned long>(m.hash));
    g.text(14, y0 + 48, kMonoS, buf);
    const uint32_t n = sl.n ? sl.n : 1;
    uint32_t idx = eng_.st[s].lastIdx;
    if (eng_.st[s].p[P_UNIT] == U_BIT) idx >>= 3;
    std::snprintf(buf, sizeof buf, "OFFSET  0x%06lX  %.1f%%", static_cast<unsigned long>(idx), double(float(idx % n) * 100.0f / float(n)));
    g.text(14, y0 + 60, kMonoS, buf);
    std::snprintf(buf, sizeof buf, "EVENTS  %010lu", static_cast<unsigned long>(evCount_));
    g.text(14, y0 + 72, kMonoS, buf);
    (void)kModeShort;
}

}  // namespace dm
