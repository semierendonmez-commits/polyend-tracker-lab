// LABKIT v2 -- text entry (lab::kit::Keyboard).
//
// A full-screen Shell modal for naming things: the keyboard is mirrored on
// the 48 pads (row 1 digits, rows 2-3 letters, row 4 punctuation, space,
// delete and OK). Encoder or Left / Right move the cursor, Up / Down recall
// recent names, Insert inserts a space, Delete deletes the character before
// the cursor, Shift switches the case of the next letter, F7 cancels, F8
// (or the OK pad) confirms. Recent names are kept in /Lab/<FW>/labkit.bin.
//
//   kit::KeyboardRequest q;
//   q.title = "Patch name"; q.initial = name_; q.maxLen = 16;
//   q.onDone = [](void* c, const char* text) { ... }; q.ctx = this;
//   ctx.keyboard().open(q);
#pragma once

#include "lab/lab.h"

namespace lab {
namespace kit {

struct KeyboardRequest {
    const char* title = "Name";
    const char* initial = "";
    int maxLen = 24;                    // 1..31
    void (*onDone)(void* ctx, const char* text) = nullptr;
    void (*onCancel)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

class Keyboard {
public:
    static constexpr int kMax = 31, kRecent = 8;
    bool open(const KeyboardRequest& req);
    bool isOpen() const { return open_; }
    void close(bool ok);
    const char* text() const { return text_; }
    int cursor() const { return cur_; }

    // Shell hooks
    void attach(Kit* kit) { kit_ = kit; }
    bool event(const Event& e);
    void draw(Canvas& g);
    void leds(Rgb* pads);
    // The character on a pad (0 = a function pad), lower case unless upper.
    static char padChar(int pad, bool upper);
    void type(char c);
    void backspace();
    void move(int delta);
    void addRecent(const char* name);
    uint32_t prefsSize() const { return sizeof recent_; }
    void savePrefs(uint8_t* buf) const;
    void loadPrefs(const uint8_t* buf, uint32_t len);

private:
    const Theme& theme() const;
    Kit* kit_ = nullptr;
    KeyboardRequest req_;
    bool open_ = false;
    char text_[kMax + 1] = {};
    int len_ = 0, cur_ = 0;
    bool upper_ = true;          // case lock (Shift + pad flips one letter)
    int recentSel_ = -1;
    int8_t padHeld_ = -1;
    uint32_t padMs_ = 0;
    char recent_[kRecent][kMax + 1] = {};
};

}  // namespace kit
}  // namespace lab
