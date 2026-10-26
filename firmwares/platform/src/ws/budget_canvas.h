// PLATFORM -- lab::Canvas behind a word limit for app / script drawing.
//
// The Shell draws the toast and the REC badge after the workstation, so an app
// gets `limit` words of the 1,800-word frame (Canvas::kBudgetWords) and every
// primitive is checked against it before it reaches the Canvas. Costs match
// src/lab/canvas.cpp. Text is restricted to ASCII 32..126 here.
#pragma once

#include <cstring>

#include "lab/lab.h"

namespace plat {

class BudgetCanvas {
public:
    BudgetCanvas(lab::Canvas& g, uint32_t limit) : g_(g), limit_(limit) {}
    lab::Canvas& canvas() { return g_; }
    uint32_t used() const { return g_.words(); }
    uint32_t limit() const { return limit_; }
    uint32_t dropped() const { return dropped_; }
    bool fits(uint32_t w) {
        if (g_.words() + w > limit_) {
            ++dropped_;
            return false;
        }
        return true;
    }

    void color(uint32_t rgb) { if (fits(1)) g_.color(lab::Rgb(rgb)); }
    void alpha(uint8_t a) { if (fits(1)) g_.alpha(a); }
    void lineWidth(float px) { if (fits(1)) g_.lineWidth(px); }
    void pointSize(float px) { if (fits(1)) g_.pointSize(px); }
    void clip(int x, int y, int w, int h) { if (fits(2)) g_.clip(x, y, w, h); }
    void noClip() { if (fits(2)) g_.noClip(); }
    void fill(int x, int y, int w, int h) { if (fits(4)) g_.fill(x, y, w, h); }
    void fillRound(int x, int y, int w, int h, float r) { if (fits(6)) g_.fillRound(x, y, w, h, r); }
    void frame(int x, int y, int w, int h) { if (fits(7)) g_.frame(x, y, w, h); }
    void line(float x0, float y0, float x1, float y1) { if (fits(4)) g_.line(x0, y0, x1, y1); }
    void point(float x, float y, float r) { if (fits(4)) g_.point(x, y, r); }
    void ring(float x, float y, float r, int seg) {
        if (seg < 3) seg = 3;
        if (seg > 96) seg = 96;
        if (fits(uint32_t(seg) + 3)) g_.ring(x, y, r, seg);
    }
    void arc(float x, float y, float r, float a0, float a1, int seg) {
        if (seg < 1) seg = 1;
        if (seg > 96) seg = 96;
        if (fits(uint32_t(seg) + 3)) g_.arc(x, y, r, a0, a1, seg);
    }
    // Batches: reserve begin + all vertices + end up front.
    bool beginBatch(int kind, uint32_t vertices) {
        if (inBatch_ || !fits(vertices + 3)) return false;
        reserved_ = vertices;
        switch (kind) {
            case 0: g_.beginLines(); break;
            case 1: g_.beginPoints(); break;
            case 2: g_.beginRects(); break;
            default: g_.beginStrip(); break;
        }
        inBatch_ = true;
        return true;
    }
    // Open-ended batch for native apps: each vertex is checked (keeps 1 word for END).
    void beginOpen(int kind) {
        if (inBatch_ || !fits(3)) return;
        reserved_ = 0xFFFFFFFFu;
        switch (kind) {
            case 0: g_.beginLines(); break;
            case 1: g_.beginPoints(); break;
            case 2: g_.beginRects(); break;
            default: g_.beginStrip(); break;
        }
        inBatch_ = true;
    }
    void vtx(float x, float y) {
        if (!inBatch_) return;
        if (reserved_ == 0xFFFFFFFFu && g_.words() + 2 > limit_) {
            ++dropped_;
            return;
        }
        g_.vtx(x, y);
    }
    void endBatch() {
        if (!inBatch_) return;
        inBatch_ = false;
        g_.endBatch();
    }
    void text(int x, int y, uint8_t font, const char* s, uint16_t opt) {
        if (!s) return;
        char buf[128];
        uint32_t n = 0;
        for (; s[n] && n < sizeof buf - 1; ++n) {
            const unsigned char c = static_cast<unsigned char>(s[n]);
            buf[n] = (c >= 32 && c <= 126) ? char(c) : '?';
        }
        buf[n] = 0;
        if (!n) return;
        if (font < 16 || font > 31) font = lab::font::kBody;
        if (fits(n + 4)) g_.text(x, y, font, buf, opt);
    }
    void number(int x, int y, uint8_t font, int32_t v, uint16_t opt) {
        if (font < 16 || font > 31) font = lab::font::kBody;
        if (fits(16)) g_.number(x, y, font, v, opt);
    }
    void bitmap332(int x, int y, uint32_t ramg, int w, int h, int scale) {
        if (fits(14)) g_.bitmap332(x, y, ramg, w, h, scale);
    }

private:
    lab::Canvas& g_;
    uint32_t limit_;
    uint32_t dropped_ = 0;
    uint32_t reserved_ = 0;
    bool inBatch_ = false;
};

}  // namespace plat
