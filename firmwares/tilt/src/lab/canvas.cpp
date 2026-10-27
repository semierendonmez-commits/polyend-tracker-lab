// lab::Canvas -- budgeted drawing on the FT81x display list (lab.h).
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "lab/lab.h"

namespace lab {

Rgb mix(Rgb a, Rgb b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return Rgb(uint8_t(a.r + (float(b.r) - float(a.r)) * t), uint8_t(a.g + (float(b.g) - float(a.g)) * t),
               uint8_t(a.b + (float(b.b) - float(a.b)) * t));
}

Rgb scale(Rgb a, float k) {
    auto c = [k](uint8_t v) {
        const float f = float(v) * k;
        return uint8_t(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
    };
    return Rgb(c(a.r), c(a.g), c(a.b));
}

namespace {
inline int32_t px16(float v) { return int32_t(v * 16.0f); }
}  // namespace

bool Canvas::spend(uint32_t w) {
    if (words_ + w > kBudgetWords) {
        ++dropped_;
        return false;
    }
    if (words_ + w > limit_) {   // words kept for a Shell overlay (setLimit)
        ++clipped_;
        return false;
    }
    words_ += w;
    return true;
}

void Canvas::begin(Rgb c) {
    words_ = 0;
    dropped_ = 0;
    clipped_ = 0;
    limit_ = kBudgetWords;
    inBatch_ = false;
    if (!d_) return;
    d_->dlStart();
    d_->clearColorRGB(c.r, c.g, c.b);
    d_->clear(true, true, true);
    d_->lineWidth(16);
    words_ = 8;   // DLSTART, clear colour, clear, line width, driver font setup
}

void Canvas::end() {
    if (!d_) return;
    if (inBatch_) endBatch();
    d_->swap();
}

void Canvas::color(Rgb c) {
    if (d_ && spend(1)) d_->colorRGB(c.r, c.g, c.b);
}
void Canvas::alpha(uint8_t a) {
    if (d_ && spend(1)) d_->colorA(a);
}
void Canvas::lineWidth(float px) {
    if (d_ && spend(1)) d_->lineWidth(uint16_t(px * 16.0f < 1.0f ? 1.0f : px * 16.0f));
}
void Canvas::pointSize(float px) {
    if (d_ && spend(1)) d_->pointSize(uint16_t(px * 16.0f < 1.0f ? 1.0f : px * 16.0f));
}
void Canvas::clip(int x, int y, int w, int h) {
    if (!d_ || !spend(2)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    d_->scissorXY(uint16_t(x), uint16_t(y));
    d_->scissorSize(uint16_t(w), uint16_t(h));
}
void Canvas::noClip() { clip(0, 0, 2048, 2048); }

void Canvas::fill(int x, int y, int w, int h) {
    if (!d_ || w <= 0 || h <= 0 || !spend(4)) return;
    d_->begin(IDisplay::kRects);
    d_->vertex2f(int32_t(x) * 16 + 8, int32_t(y) * 16 + 8);
    d_->vertex2f(int32_t(x + w - 1) * 16 + 8, int32_t(y + h - 1) * 16 + 8);
    d_->end();
}

void Canvas::fillRound(int x, int y, int w, int h, float radius) {
    if (!d_ || w <= 0 || h <= 0 || !spend(6)) return;
    const float r = radius < 1.0f ? 1.0f : radius;
    d_->lineWidth(uint16_t(r * 16.0f));
    d_->begin(IDisplay::kRects);
    d_->vertex2f(px16(float(x) + r), px16(float(y) + r));
    d_->vertex2f(px16(float(x + w) - r), px16(float(y + h) - r));
    d_->end();
    d_->lineWidth(16);
}

void Canvas::frame(int x, int y, int w, int h) {
    if (!d_ || w <= 0 || h <= 0 || !spend(7)) return;
    d_->begin(IDisplay::kLineStrip);
    const int32_t x0 = int32_t(x) * 16 + 8, y0 = int32_t(y) * 16 + 8;
    const int32_t x1 = int32_t(x + w - 1) * 16 + 8, y1 = int32_t(y + h - 1) * 16 + 8;
    d_->vertex2f(x0, y0);
    d_->vertex2f(x1, y0);
    d_->vertex2f(x1, y1);
    d_->vertex2f(x0, y1);
    d_->vertex2f(x0, y0);
    d_->end();
}

void Canvas::line(float x0, float y0, float x1, float y1) {
    if (!d_ || !spend(4)) return;
    d_->begin(IDisplay::kLines);
    d_->vertex2f(px16(x0 + 0.5f), px16(y0 + 0.5f));
    d_->vertex2f(px16(x1 + 0.5f), px16(y1 + 0.5f));
    d_->end();
}

void Canvas::point(float x, float y, float radius) {
    if (!d_ || !spend(4)) return;
    d_->pointSize(uint16_t(radius * 16.0f < 1.0f ? 1.0f : radius * 16.0f));
    d_->begin(IDisplay::kPoints);
    d_->vertex2f(px16(x), px16(y));
    d_->end();
}

void Canvas::ring(float x, float y, float radius, int segments) {
    if (segments < 3) segments = 3;
    if (!d_ || !spend(uint32_t(segments) + 3)) return;
    d_->begin(IDisplay::kLineStrip);
    for (int i = 0; i <= segments; ++i) {
        const float a = 6.2831853f * float(i) / float(segments);
        d_->vertex2f(px16(x + radius * cosf(a)), px16(y + radius * sinf(a)));
    }
    d_->end();
}

void Canvas::arc(float x, float y, float radius, float a0, float a1, int segments) {
    if (segments < 1) segments = 1;
    if (!d_ || !spend(uint32_t(segments) + 3)) return;
    d_->begin(IDisplay::kLineStrip);
    for (int i = 0; i <= segments; ++i) {
        const float a = a0 + (a1 - a0) * float(i) / float(segments);
        d_->vertex2f(px16(x + radius * cosf(a)), px16(y + radius * sinf(a)));
    }
    d_->end();
}

void Canvas::beginLines() {
    if (!d_ || inBatch_ || !spend(2)) return;
    d_->begin(IDisplay::kLines);
    inBatch_ = true;
}
void Canvas::beginPoints() {
    if (!d_ || inBatch_ || !spend(2)) return;
    d_->begin(IDisplay::kPoints);
    inBatch_ = true;
}
void Canvas::beginRects() {
    if (!d_ || inBatch_ || !spend(2)) return;
    d_->begin(IDisplay::kRects);
    inBatch_ = true;
}
void Canvas::beginStrip() {
    if (!d_ || inBatch_ || !spend(2)) return;
    d_->begin(IDisplay::kLineStrip);
    inBatch_ = true;
}
void Canvas::vtx(float x, float y) {
    if (!d_ || !inBatch_) return;
    // Keep one word for END: an unfinished batch still closes cleanly.
    if (words_ + 1 > kBudgetWords - 1) {
        ++dropped_;
        return;
    }
    if (words_ + 1 > limit_ - 1) {
        ++clipped_;
        return;
    }
    ++words_;
    d_->vertex2f(px16(x), px16(y));
}
void Canvas::endBatch() {
    if (!d_ || !inBatch_) return;
    inBatch_ = false;
    d_->end();
}

void Canvas::text(int x, int y, uint8_t fnt, const char* s, uint16_t opt) {
    if (!d_ || !s) return;
    const uint32_t n = uint32_t(std::strlen(s));
    if (!spend(n + 4)) return;
    d_->text(int16_t(x), int16_t(y), fnt, opt, s);
}

void Canvas::textf(int x, int y, uint8_t fnt, uint16_t opt, const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text(x, y, fnt, buf, opt);
}

void Canvas::number(int x, int y, uint8_t fnt, int32_t n, uint16_t opt) {
    if (!d_ || !spend(16)) return;
    d_->number(int16_t(x), int16_t(y), fnt, opt, n);
}

void Canvas::bitmap332(int x, int y, uint32_t ramg, int w, int h, int scale) {
    if (!d_ || !spend(14)) return;
    d_->drawRgb332(int16_t(x), int16_t(y), ramg, uint16_t(w), uint16_t(h), uint8_t(scale < 1 ? 1 : scale));
}

void copyStr(char* dst, uint32_t cap, const char* src) {
    if (!cap) return;
    uint32_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool endsWithCi(const char* s, const char* suffix) {
    const size_t a = std::strlen(s), b = std::strlen(suffix);
    if (b > a) return false;
    for (size_t i = 0; i < b; ++i) {
        char x = s[a - b + i], y = suffix[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace lab
