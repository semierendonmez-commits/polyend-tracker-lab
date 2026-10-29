// LABKIT v2 -- Keyboard implementation (compiled inside shell.cpp).
#include <cstdio>
#include <cstring>

#include "lab/lab.h"

namespace lab {
namespace kit {

namespace {
// 12 x 4, mirrored on the pads. '\1' case, '\2' delete, '\3' OK.
const char kRows[4][13] = {"1234567890-_", "qwertyuiop()", "asdfghjkl!&+", "zxcvbnm. \1\2\3"};
}  // namespace

const Theme& Keyboard::theme() const { return kit_->theme(); }

char Keyboard::padChar(int pad, bool upper) {
    if (pad < 0 || pad >= kPads) return 0;
    const char c = kRows[padRow(pad)][padCol(pad)];
    if (c < ' ') return 0;
    if (upper && c >= 'a' && c <= 'z') return char(c - 'a' + 'A');
    return c;
}

bool Keyboard::open(const KeyboardRequest& req) {
    req_ = req;
    if (req_.maxLen < 1) req_.maxLen = 1;
    if (req_.maxLen > kMax) req_.maxLen = kMax;
    copyStr(text_, uint32_t(req_.maxLen + 1), req.initial ? req.initial : "");
    len_ = int(std::strlen(text_));
    cur_ = len_;
    recentSel_ = -1;
    open_ = true;
    return true;
}

void Keyboard::close(bool ok) {
    open_ = false;
    if (ok) {
        addRecent(text_);
        if (req_.onDone) req_.onDone(req_.ctx, text_);
    } else if (req_.onCancel) {
        req_.onCancel(req_.ctx);
    }
}

void Keyboard::type(char c) {
    if (len_ >= req_.maxLen || c < 32 || c > 126) return;
    for (int i = len_; i > cur_; --i) text_[i] = text_[i - 1];
    text_[cur_++] = c;
    text_[++len_] = 0;
}

void Keyboard::backspace() {
    if (cur_ <= 0) return;
    for (int i = cur_ - 1; i < len_; ++i) text_[i] = text_[i + 1];
    --cur_;
    --len_;
}

void Keyboard::move(int d) {
    cur_ += d;
    if (cur_ < 0) cur_ = 0;
    if (cur_ > len_) cur_ = len_;
}

void Keyboard::addRecent(const char* name) {
    if (!name || !*name) return;
    int at = kRecent - 1;
    for (int i = 0; i < kRecent; ++i)
        if (std::strcmp(recent_[i], name) == 0) at = i;
    for (int i = at; i > 0; --i) copyStr(recent_[i], kMax + 1, recent_[i - 1]);
    copyStr(recent_[0], kMax + 1, name);
    if (kit_) kit_->prefsDirty = true;
}

void Keyboard::savePrefs(uint8_t* buf) const { std::memcpy(buf, recent_, sizeof recent_); }
void Keyboard::loadPrefs(const uint8_t* buf, uint32_t len) {
    if (len != sizeof recent_) return;
    std::memcpy(recent_, buf, sizeof recent_);
    for (auto& r : recent_) r[kMax] = 0;
}

bool Keyboard::event(const Event& e) {
    switch (e.type) {
        case Event::kPadDown: {
            padHeld_ = int8_t(e.id);
            padMs_ = e.ms;
            const char raw = kRows[padRow(e.id)][padCol(e.id)];
            if (raw == '\1') upper_ = !upper_;
            else if (raw == '\2') backspace();
            else if (raw == '\3') close(true);
            else type(padChar(e.id, upper_ != e.shift));
            return true;
        }
        case Event::kPadUp:
            if (padHeld_ == e.id) padHeld_ = -1;
            return true;
        case Event::kEncoder: move(e.delta); return true;
        case Event::kPress:
            switch (e.id) {
                case btn::Left: move(-1); break;
                case btn::Right: move(1); break;
                case btn::Insert: type(' '); break;
                case btn::Delete: backspace(); break;
                case btn::Up:
                case btn::Down: {
                    int n = 0;
                    while (n < kRecent && recent_[n][0]) ++n;
                    if (!n) break;
                    recentSel_ += e.id == btn::Down ? 1 : -1;
                    if (recentSel_ < 0) recentSel_ = n - 1;
                    if (recentSel_ >= n) recentSel_ = 0;
                    copyStr(text_, uint32_t(req_.maxLen + 1), recent_[recentSel_]);
                    len_ = int(std::strlen(text_));
                    cur_ = len_;
                    break;
                }
                case btn::F7: close(false); break;
                case btn::F8: close(true); break;
                case btn::F1: upper_ = !upper_; break;
                case btn::F2:
                    text_[0] = 0;
                    len_ = cur_ = 0;
                    break;
                default: break;
            }
            return true;
        default: return true;
    }
}

void Keyboard::draw(Canvas& g) {
    const Theme& t = theme();
    drawHeader(g, t, *kit_->shell, req_.title);
    // the text
    g.color(scale(t.panel, 1.5f));
    g.fillRound(60, 46, 680, 64, 10);
    g.color(t.fg);
    g.text(80, 78, font::kHuge, len_ ? text_ : "", Canvas::kCenterY);
    // cursor
    char head[kMax + 1];
    copyStr(head, uint32_t(cur_ + 1), text_);
    const int cx = 80 + textWidth(font::kHuge, head);
    if ((kit_->shell->millis() / 400) & 1) {
        g.color(t.accent);
        g.fill(cx, 60, 3, 36);
    }
    g.color(t.dim);
    g.textf(730, 100, font::kTiny, Canvas::kRight, "%d / %d", len_, req_.maxLen);
    // recent names
    int n = 0;
    while (n < kRecent && recent_[n][0]) ++n;
    if (n) {
        g.color(t.dim);
        g.text(60, 126, font::kTiny, "Up / Down: recent", Canvas::kCenterY);
        int x = 180;
        for (int i = 0; i < n && x < 740; ++i) {
            char nm[24];
            fitText(nm, sizeof nm, font::kTiny, recent_[i], 110);
            const int w = textWidth(font::kTiny, nm) + 14;
            if (x + w > 740) break;
            g.color(i == recentSel_ ? t.accent : scale(t.panel, 1.8f));
            g.fillRound(x, 117, w, 18, 4);
            g.color(i == recentSel_ ? t.bg : t.fg);
            g.text(x + 7, 126, font::kTiny, nm, Canvas::kCenterY);
            x += w + 6;
        }
    }
    // the keyboard = the pads
    const int ky = 146, kh = 62, kw = 66;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < kPadCols; ++c) {
            const int x = c * kw + 4, y = ky + r * (kh + 6);
            const char raw = kRows[r][c];
            const bool held = padHeld_ == r * kPadCols + c;
            Rgb bg = scale(t.panel, 2.0f);
            const char* label = nullptr;
            char s[2] = {0, 0};
            if (raw == '\1') { bg = scale(Rgb(0xFFB020), upper_ ? 0.7f : 0.3f); label = upper_ ? "ABC" : "abc"; }
            else if (raw == '\2') { bg = scale(Rgb(0xFF3030), 0.45f); label = "DEL"; }
            else if (raw == '\3') { bg = scale(Rgb(0x30FF70), 0.45f); label = "OK"; }
            else if (raw == ' ') { label = "SPACE"; }
            else {
                s[0] = (upper_ && raw >= 'a' && raw <= 'z') ? char(raw - 'a' + 'A') : raw;
                label = s;
            }
            g.color(held ? t.accent : bg);
            g.fillRound(x, y, kw - 8, kh, 6);
            g.color(held ? t.bg : t.fg);
            g.text(x + (kw - 8) / 2, y + kh / 2, label[1] ? font::kSmall : font::kLarge, label, Canvas::kCenter);
        }
    const char* keys[8] = {upper_ ? "abc" : "ABC", "Clear", "", "", "", "", "Cancel", "OK"};
    drawFKeyBar(g, t, keys);
}

void Keyboard::leds(Rgb* pads) {
    const Theme& t = theme();
    for (int i = 0; i < kPads; ++i) {
        const char raw = kRows[padRow(i)][padCol(i)];
        Rgb c = (raw >= '0' && raw <= '9') ? scale(t.accent2, 0.35f) : scale(t.accent, 0.35f);
        if (raw == '\1') c = upper_ ? Rgb(0xFFB020) : scale(Rgb(0xFFB020), 0.3f);
        if (raw == '\2') c = Rgb(0xC02020);
        if (raw == '\3') c = Rgb(0x20C050);
        if (raw == ' ') c = Rgb(0x707070);
        if (padHeld_ == i) c = Rgb(0xFFFFFF);
        pads[i] = c;
    }
}

}  // namespace kit
}  // namespace lab
