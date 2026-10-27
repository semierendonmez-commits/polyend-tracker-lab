// LABKIT v2 -- navigation implementation: Kit hooks, MAP, PARAM pads, MACRO,
// MOD, HELP, value HUD, MIDI learn (compiled inside shell.cpp).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/lab.h"

namespace lab {
namespace kit {

namespace {

// FT81x ROM font advances, chars 32..126 (the same values as core/ui/gfx.cpp
// and the simulator's look-alike fonts), for layout and cutting long names.
const uint8_t kW26[95] = {4,5,6,10,8,11,9,3,5,5,6,10,4,4,4,4,8,8,8,8,8,8,8,8,8,8,4,4,10,10,10,6,12,8,8,8,9,8,7,9,9,4,4,8,7,10,9,9,7,9,8,8,7,9,8,12,8,7,8,5,4,5,10,6,6,7,8,7,8,7,4,8,8,3,3,7,3,12,8,7,8,8,5,6,5,8,7,10,7,7,6,8,4,8,10};
const uint8_t kW27[95] = {5,6,7,13,10,14,12,4,6,6,8,13,5,5,5,5,10,10,10,10,10,10,10,10,10,10,5,5,13,13,13,8,15,10,10,10,12,9,9,12,11,4,4,10,8,13,11,12,9,12,10,10,9,11,10,15,10,9,10,6,5,6,13,8,8,9,10,8,10,9,5,10,10,4,4,9,4,15,10,9,10,10,6,8,6,10,9,12,9,9,8,10,5,10,13};
const uint8_t kW28[95] = {6,8,9,16,12,18,15,5,7,7,10,16,6,7,6,6,12,12,12,12,12,12,12,12,12,12,6,6,16,16,16,10,19,13,13,13,15,12,11,15,14,6,6,12,11,16,14,15,11,15,13,12,12,14,13,19,13,12,13,7,6,7,16,10,10,12,12,10,12,12,7,12,12,5,5,11,5,19,12,12,12,12,8,10,7,12,11,16,11,11,10,12,6,12,16};
const uint8_t kW29[95] = {7,9,10,18,14,21,17,6,9,9,11,18,7,8,7,7,14,14,14,14,14,14,14,14,14,14,7,7,18,18,18,12,22,15,15,15,17,14,13,17,17,6,6,14,12,19,16,17,13,17,15,14,13,16,15,22,15,13,15,9,7,9,18,11,11,13,14,12,14,14,8,14,14,6,6,13,6,21,14,13,14,14,9,11,9,14,13,18,13,13,12,14,7,14,18};

const uint8_t kW30[95] = {9,11,13,23,18,27,22,8,11,11,14,23,9,10,9,9,18,18,18,18,18,18,18,18,18,18,9,9,23,23,23,15,28,19,19,20,22,18,16,22,21,8,8,18,16,24,21,22,17,22,19,18,17,20,19,28,19,17,19,11,9,11,23,14,14,17,18,15,18,17,10,18,18,8,8,16,8,27,18,17,18,18,12,15,11,18,17,23,17,17,15,18,9,18,23};
const uint8_t kW31[95] = {12,15,17,32,24,36,30,10,15,15,19,32,12,14,12,13,24,24,24,24,24,24,24,24,24,24,13,13,32,32,32,20,38,26,26,27,29,24,22,29,29,11,11,25,21,33,28,30,23,30,26,24,23,28,26,38,26,23,26,15,13,15,32,19,19,23,24,21,24,23,13,24,24,11,11,22,11,37,24,23,24,24,16,20,15,24,22,31,22,22,20,24,13,24,32};

const Rgb kWhite(0xFFFFFF);

inline int px(int c, int cols, int width) { return c * width / cols; }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char* const kAutoPageNames[kMaxPages] = {"PAGE 1", "PAGE 2", "PAGE 3", "PAGE 4", "PAGE 5", "PAGE 6",
                                               "PAGE 7", "PAGE 8", "PAGE 9", "PAGE 10", "PAGE 11", "PAGE 12",
                                               "PAGE 13", "PAGE 14", "PAGE 15", "PAGE 16"};
// Front-panel names of the keys (ids 0..0x20), for the HELP card.
const char* keyName(uint8_t id) {
    static const char* const names[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "I.Param", "Perform", "File",
                                        "Config", "Master", "S.Load", "S.Play", "S.Edit", "S.Rec", "Song", "Note",
                                        "Instr", "FX1", "FX2", "Pattern", "Play", "Copy", "Insert", "Up", "Delete",
                                        "Rec", "Shift", "Left", "Down", "Right"};
    return id < sizeof names / sizeof names[0] ? names[id] : "-";
}

const char* const kLayerNames[kLfos + kRands] = {"LFO1", "LFO2", "LFO3", "LFO4", "RND1", "RND2"};

// Display lists: a cell = colour + rect in a batch (3 words) twice, + text.
constexpr uint32_t kHudWords = 110;

}  // namespace

int textWidth(uint8_t fnt, const char* s) {
    if (!s) return 0;
    const uint8_t* t = fnt <= 26 ? kW26
                       : fnt == 27 ? kW27
                       : fnt == 28 ? kW28
                       : fnt == 29 ? kW29
                       : fnt == 30 ? kW30
                                   : kW31;
    int w = 0;
    for (const char* p = s; *p; ++p) {
        const uint8_t c = uint8_t(*p);
        w += (c >= 32 && c < 127) ? t[c - 32] : 8;
    }
    return w;
}

void fitText(char* buf, int cap, uint8_t fnt, const char* s, int width) {
    copyStr(buf, uint32_t(cap), s ? s : "");
    int n = int(std::strlen(buf));
    if (textWidth(fnt, buf) <= width) return;
    while (n > 1) {
        buf[--n] = 0;
        if (n + 2 < cap) {
            char tmp[64];
            std::snprintf(tmp, sizeof tmp, "%s..", buf);
            if (textWidth(fnt, tmp) <= width) {
                copyStr(buf, uint32_t(cap), tmp);
                return;
            }
        }
    }
}

// ---- drawing helpers for workstations -----------------------------------------------
void drawParamKnob(Canvas& g, const Theme& t, const Registry& r, ParamId id, int cx, int cy, int radius,
                   bool focused) {
    if (!r.valid(id)) return;
    const Rgb col = r.color(id);
    const float a0 = 2.3561945f, span = 4.712389f;   // 135 deg .. 405 deg
    g.lineWidth(radius > 20 ? 3.0f : 2.0f);
    g.color(scale(t.dim, 0.6f));
    g.arc(float(cx), float(cy), float(radius), a0, a0 + span, 18);
    const float nb = r.norm(id), ne = r.effectiveNorm(id);
    if (r.isBipolar(id)) {
        g.color(col);
        const float c = a0 + span * 0.5f, e = a0 + span * nb;
        if (fabsf(e - c) > 0.02f) g.arc(float(cx), float(cy), float(radius), c < e ? c : e, c < e ? e : c, 10);
    } else if (nb > 0.005f) {
        g.color(col);
        g.arc(float(cx), float(cy), float(radius), a0, a0 + span * nb, int(4 + 14 * nb));
    }
    if (fabsf(ne - nb) > 0.004f) {   // the modulated value: a dot on the ring
        g.color(kWhite);
        const float a = a0 + span * ne;
        g.point(float(cx) + float(radius) * cosf(a), float(cy) + float(radius) * sinf(a), radius > 20 ? 4.0f : 3.0f);
    }
    g.lineWidth(1.0f);
    char nm[24], val[24];
    fitText(nm, sizeof nm, font::kTiny, r.name(id), radius * 2 + 20);
    r.format(id, val, sizeof val);
    g.color(focused ? t.accent : t.fg);
    g.text(cx, cy + radius + 12, font::kTiny, nm, Canvas::kCenter);
    // the value inside the ring: the bigger font when it fits
    const int inner = radius * 2 - 10;
    uint8_t vf = radius >= 26 ? font::kSmall : font::kTiny;
    if (textWidth(vf, val) > inner) vf = font::kTiny;
    char fit[24];
    fitText(fit, sizeof fit, vf, val, inner + 4);
    g.color(focused ? t.fg : t.dim);
    g.text(cx, cy, vf, fit, Canvas::kCenter);
    if (focused) {
        g.color(t.accent);
        g.ring(float(cx), float(cy), float(radius + 6), 20);
    }
}

void drawParamBar(Canvas& g, const Theme& t, const Registry& r, ParamId id, int x, int y, int w, bool focused) {
    if (!r.valid(id)) return;
    const Rgb col = r.color(id);
    char nm[24], val[24];
    fitText(nm, sizeof nm, font::kSmall, r.name(id), w / 2 - 4);
    r.format(id, val, sizeof val);
    if (focused) {
        g.color(scale(col, 0.25f));
        g.fill(x - 4, y - 2, w + 8, 30);
    }
    g.color(focused ? t.fg : t.dim);
    g.text(x, y + 8, font::kSmall, nm, Canvas::kCenterY);
    g.color(focused ? t.accent : t.fg);
    g.text(x + w, y + 8, font::kSmall, val, Canvas::kCenterY | Canvas::kRight);
    g.color(scale(t.dim, 0.5f));
    g.fill(x, y + 20, w, 4);
    const float nb = r.norm(id), ne = r.effectiveNorm(id);
    g.color(col);
    if (r.isBipolar(id)) {
        const int c = x + w / 2, e = x + int(float(w) * nb);
        g.fill(c < e ? c : e, y + 20, (c < e ? e - c : c - e) + 1, 4);
    } else {
        g.fill(x, y + 20, int(float(w) * nb), 4);
    }
    if (fabsf(ne - nb) > 0.004f) {
        g.color(kWhite);
        g.fill(x + int(float(w - 2) * ne), y + 17, 3, 10);
    }
}

// ---- Kit -----------------------------------------------------------------------------------------
const Theme& Kit::theme() const {
    static const Theme kDefault;
    return (ctx && ctx->theme) ? *ctx->theme : kDefault;
}

bool Kit::init(Shell& sh, Context& c, Arena& arena, bool audio) {
    shell = &sh;
    ctx = &c;
    audioRunning = audio;
    reg.attach(this, &arena);
    mod.attach(this, &reg);
    nav.attach(this);
    browser = arena.make<Browser>();
    keyboard = arena.make<Keyboard>();
    if (!browser || !keyboard) return false;
    browser->attach(this, &arena);
    keyboard->attach(this);
    return true;
}

void Kit::afterWorkstationInit(Workstation* w) {
    ws = w;
    NavKeys k;
    if (ws) ws->navKeys(k);
    nav.setKeys(k);
    PageInfo probe[kMaxPages];
    const int pages = ws ? ws->navPages(probe, kMaxPages) : 0;
    loadPrefs();
    if (wsParams == 0 && pages <= 0) return;   // nothing registered: the Shell stays as it was
    active = true;
    registeringShell = true;
    mod.registerShellParams();
    registeringShell = false;
    nav.refreshPages();
    mod.update(false);
}

bool Kit::event(const Event& e) {
    if (browser && browser->isOpen()) {
        browser->event(e);
        return true;
    }
    if (keyboard && keyboard->isOpen()) {
        keyboard->event(e);
        return true;
    }
    if (!active) return false;
    inShellUi = true;
    const bool used = nav.event(e);
    inShellUi = false;
    return used;
}

bool Kit::midi(const MidiMsg& m) {
    if (!active) return false;
    inShellUi = true;
    const bool used = nav.midi(m);
    inShellUi = false;
    return used;
}

void Kit::update(uint32_t now) {
    if (browser && browser->isOpen()) browser->update(now);
    if (active) {
        mod.update(audioRunning);
        nav.update(now);
        if (now - lastSyncMs >= 50) {
            lastSyncMs = now;
            reg.sync();
        }
        if (!audioRunning) reg.applyNow();
    }
    if (prefsDirty && now - prefsMs > 2000) {
        prefsMs = now;
        savePrefs();
    }
}

bool Kit::fullScreen() const {
    if (browser && browser->isOpen()) return true;
    if (keyboard && keyboard->isOpen()) return true;
    return active && nav.fullScreen();
}

uint32_t Kit::reserveWords() const {
    if (!active) return 0;
    return nav.reserveWords();
}

void Kit::draw(Canvas& g) {
    if (browser && browser->isOpen()) browser->draw(g);
    else if (keyboard && keyboard->isOpen()) keyboard->draw(g);
    else if (active) nav.draw(g);
}

bool Kit::leds(Rgb* pads) {
    if (browser && browser->isOpen()) {
        browser->leds(pads);
        return true;
    }
    if (keyboard && keyboard->isOpen()) {
        keyboard->leds(pads);
        return true;
    }
    return active && nav.leds(pads);
}

LABKIT_HOT void Kit::audioBlock(const Transport& t) {
    if (!active) return;
    mod.tick(t);
    reg.applyBlock();
    mod.applyRoutes();
}

void Kit::mix(float* l, float* r) {
    if (browser) browser->mix(l, r);
}

void Kit::powerOff() {
    if (prefsDirty) savePrefs();
}

namespace {
constexpr uint32_t kPrefsMagic = 0x50464B4Cu;   // "LKFP"
}

void Kit::loadPrefs() {
    if (!ctx || !ctx->fs || !browser || !keyboard) return;
    char path[64];
    std::snprintf(path, sizeof path, "%s/labkit.bin", ctx->home);
    if (!ctx->fs->exists(path)) return;
    const IStorage::Handle h = ctx->fs->open(path, IStorage::kRead);
    if (h < 0) return;
    uint8_t buf[6144];
    const int32_t n = ctx->fs->read(h, buf, sizeof buf);
    ctx->fs->close(h);
    if (n < 16) return;
    uint32_t magic, bsz, ksz;
    std::memcpy(&magic, buf, 4);
    std::memcpy(&bsz, buf + 4, 4);
    std::memcpy(&ksz, buf + 8, 4);
    int32_t hud;
    std::memcpy(&hud, buf + 12, 4);
    if (magic != kPrefsMagic || 16 + bsz + ksz > uint32_t(n)) return;
    hudOn = hud ? 1 : 0;
    browser->loadPrefs(buf + 16, bsz);
    keyboard->loadPrefs(buf + 16 + bsz, ksz);
}

void Kit::savePrefs() {
    prefsDirty = false;
    if (!ctx || !ctx->fs || !browser || !keyboard) return;
    if (shell && shell->recording()) {
        prefsDirty = true;   // never write while the recorder streams
        return;
    }
    const uint32_t bsz = browser->prefsSize(), ksz = keyboard->prefsSize();
    uint8_t buf[6144];
    if (16 + bsz + ksz > sizeof buf) return;
    const int32_t hud = hudOn;
    std::memcpy(buf, &kPrefsMagic, 4);
    std::memcpy(buf + 4, &bsz, 4);
    std::memcpy(buf + 8, &ksz, 4);
    std::memcpy(buf + 12, &hud, 4);
    browser->savePrefs(buf + 16);
    keyboard->savePrefs(buf + 16 + bsz);
    char path[64];
    std::snprintf(path, sizeof path, "%s/labkit.bin", ctx->home);
    const IStorage::Handle h = ctx->fs->open(path, IStorage::kWrite);
    if (h < 0) return;
    ctx->fs->write(h, buf, 16 + bsz + ksz);
    ctx->fs->close(h);
}

// ---- Nav: pages ------------------------------------------------------------------------------------
const Theme& Nav::theme() const { return kit_->theme(); }

void Nav::refreshPages() {
    Kit& k = *kit_;
    int n = k.ws ? k.ws->navPages(pages_, kMaxPages) : 0;
    if (n < 0) n = 0;
    if (n > kMaxPages) n = kMaxPages;
    int maxPage = -1;
    for (int i = 0; i < k.reg.count(); ++i) {
        const int p = k.reg.page(ParamId(i));
        if (p >= 0 && p < kMaxPages && p > maxPage) maxPage = p;
    }
    for (int i = n; i <= maxPage; ++i) {
        pages_[i] = PageInfo();
        pages_[i].name = kAutoPageNames[i];
    }
    if (maxPage + 1 > n) n = maxPage + 1;
    wsPages_ = n;
    pageCount_ = 0;
    for (int i = 0; i < n; ++i) pageIds_[pageCount_++] = i;
    pageIds_[pageCount_++] = kPageMacro;
    pageIds_[pageCount_++] = kPageMod;
    if (k.ws && k.ws->navPage() >= 0) layerPage_ = k.ws->navPage();
    layoutPage_ = -999;
}

int Nav::pageIndexOf(int page) const {
    for (int i = 0; i < pageCount_; ++i)
        if (pageIds_[i] == page) return i;
    return 0;
}
int Nav::pageAt(int col) const { return (col >= 0 && col < pageCount_) ? pageIds_[col] : 0; }

const char* Nav::pageName(int page) const {
    if (page == kPageMacro) return "MACRO";
    if (page == kPageMod) return "MOD";
    if (page >= 0 && page < wsPages_) return pages_[page].name && pages_[page].name[0] ? pages_[page].name : kAutoPageNames[page];
    return "";
}

static Rgb pageColorOf(const Nav& n, const PageInfo* pages, int wsPages, int page, const Theme& t) {
    (void)n;
    if (page == kPageMacro) return Rgb(0xFFB020);
    if (page == kPageMod) return Rgb(0x37D6FF);
    if (page >= 0 && page < wsPages) {
        const Rgb c = pages[page].color;
        if (c.r | c.g | c.b) return c;
    }
    return t.accent;
}

void Nav::loadLayout(int page) {
    if (layoutPage_ == page) return;
    kit_->reg.padLayout(page, layout_);
    layoutPage_ = page;
}

ParamId Nav::padParam(int pad, bool inMap) const {
    const int idx = inMap ? pad - kPadCols : pad;
    if (idx < 0 || idx >= kPadParams) return kNoParam;
    return layout_[idx];
}

int Nav::padSource(int pad) const {
    const int c = pad - 3 * kPadCols;
    if (c < 0 || c >= kPadCols) return -1;
    if (layerBank_ == 0) {
        if (c < kLfos + kRands) return c;            // LFO1-4, RND1-2
        return kSrcMacro + (c - (kLfos + kRands));    // M1..M6
    }
    if (c < 2) return kSrcMacro + 6 + c;             // M7, M8
    const int s = kSrcWs + (c - 2);
    return s < kit_->mod.sourceCount() ? s : -1;
}

void Nav::setFocus(ParamId id) {
    if (!kit_->reg.valid(id)) return;
    focus_ = id;
    if (kit_->ws && id < kit_->reg.count()) kit_->ws->navFocus(id);
}

void Nav::paramLabel(ParamId id, char* buf, int cap, bool withPage) const {
    const Registry& r = kit_->reg;
    if (!r.valid(id)) {
        buf[0] = 0;
        return;
    }
    const int pg = r.page(id);
    if (pg == kPageMod) std::snprintf(buf, size_t(cap), "%s %s", r.groupName(r.groupOf(id)), r.name(id));   // "LFO 2 Rate"
    else if (withPage) std::snprintf(buf, size_t(cap), "%s > %s", pageName(pg), r.name(id));
    else copyStr(buf, uint32_t(cap), r.name(id));
}

void Nav::hud(ParamId id) {
    hudParam_ = id;
    hudUntil_ = now_ + 1100;
    lastTouched_ = id;
}

const char* Nav::overlayName() const {
    if (kit_->browser && kit_->browser->isOpen()) return "browser";
    if (kit_->keyboard && kit_->keyboard->isOpen()) return "keyboard";
    switch (ov_) {
        case kParam: return "param";
        case kMap: return "map";
        case kMacro: return "macro";
        case kMod: return "mod";
        case kHelp: return "help";
        default: return "none";
    }
}

void Nav::open(Overlay o) {
    if (o == kMap) {
        refreshPages();
        returnPage_ = layerPage_;
        mapCol_ = pageIndexOf(layerPage_);
        loadLayout(pageAt(mapCol_));
    }
    if (o == kParam) {
        layoutPage_ = -999;
        loadLayout(layerPage_);
        // the param last turned on this page (the workstation's own cursor, usually)
        if (kit_->reg.valid(lastTouched_) && kit_->reg.page(lastTouched_) == layerPage_) focus_ = lastTouched_;
        if (!kit_->reg.valid(focus_) || kit_->reg.page(focus_) != layerPage_) {
            for (int i = 0; i < kPadParams; ++i)
                if (layout_[i] != kNoParam) {
                    focus_ = layout_[i];
                    break;
                }
        }
    }
    if (o == kMacro) macroRow_ = 0;
    ov_ = o;
    heldParamPad_ = heldSourcePad_ = -1;
    lastRoute_ = -1;
    learnArmed_ = false;
}

void Nav::close() {
    ov_ = kNone;
    latched_ = false;
    heldParamPad_ = heldSourcePad_ = -1;
    learnArmed_ = false;
}

void Nav::jumpToPage(int page) {
    if (page == kPageMacro) {
        open(kMacro);
        latched_ = true;
        return;
    }
    if (page == kPageMod) {
        open(kMod);
        latched_ = true;
        return;
    }
    layerPage_ = page;
    if (kit_->ws) {
        kit_->ws->navGoto(page);
        lastWsPage_ = kit_->ws->navPage();
    }
}

void Nav::toggleHelp() {
    if (ov_ == kHelp) {
        close();
        return;
    }
    refreshPages();
    open(kHelp);
    latched_ = true;
}

bool Nav::fullScreen() const { return ov_ == kMap || ov_ == kMacro || ov_ == kMod || ov_ == kHelp; }

uint32_t Nav::reserveWords() const {
    if (ov_ == kParam) {
        int filled = 0;
        for (int i = 0; i < kPadParams; ++i) filled += layout_[i] != kNoParam;
        return 480u + uint32_t(filled) * 16u;   // panel, strip, sources ~430 + ~14 per used cell (measured)
    }
    if (ov_ == kNone && kit_->hudOn && hudParam_ != kNoParam && int32_t(hudUntil_ - now_) > 0) return kHudWords;
    return 0;
}

void Nav::update(uint32_t now) {
    now_ = now;
    Kit& k = *kit_;
    // Follow the workstation when *it* changes page (F-keys), not merely
    // because the PARAM pads show another page (MACRO / MOD, Up / Down).
    const int wp = k.ws ? k.ws->navPage() : -1;
    if (wp >= 0 && wp != lastWsPage_) {
        lastWsPage_ = wp;
        if ((ov_ == kNone || ov_ == kParam) && heldParamPad_ < 0 && heldSourcePad_ < 0 && wp != layerPage_) {
            layerPage_ = wp;
            if (ov_ == kParam) {
                loadLayout(layerPage_);
                if (!k.reg.valid(focus_) || k.reg.page(focus_) != layerPage_)
                    for (int i = 0; i < kPadParams; ++i)
                        if (layout_[i] != kNoParam) {
                            focus_ = layout_[i];
                            break;
                        }
            }
        }
    }
    if (ov_ == kParam) loadLayout(layerPage_);
}

int Nav::learnCount() const {
    int n = 0;
    for (const Learn& l : learn_) n += l.status != 0;
    return n;
}

bool Nav::learnAssign(uint8_t status, uint8_t cc, ParamId p) {
    for (Learn& l : learn_)
        if (l.status == status && l.cc == cc) {
            l.param = p;
            return true;
        }
    for (Learn& l : learn_)
        if (!l.status) {
            l.status = status;
            l.cc = cc;
            l.param = p;
            return true;
        }
    return false;
}

bool Nav::midi(const MidiMsg& m) {
    if (m.type() != 0xB0) return false;
    Kit& k = *kit_;
    if (learnArmed_ && heldParamPad_ >= 0) {
        const ParamId p = padParam(heldParamPad_, ov_ == kMap);
        if (k.reg.valid(p) && learnAssign(m.status, m.d1, p)) {
            char msg[64];
            std::snprintf(msg, sizeof msg, "CC %d ch %d -> %s", m.d1, m.channel() + 1, k.reg.name(p));
            k.ctx->toast(msg);
            learnArmed_ = false;
        }
        return true;
    }
    for (const Learn& l : learn_)
        if (l.status && l.status == m.status && l.cc == m.d1 && k.reg.valid(l.param)) {
            k.reg.setNormFromShell(l.param, float(m.d2) * (1.0f / 127.0f));
            return true;
        }
    return false;
}

// ---- Nav: events --------------------------------------------------------------------------------------
bool Nav::event(const Event& e) {
    Kit& k = *kit_;
    const bool shift = e.shift;
    // the LABKIT keys
    if ((e.type == Event::kPress || e.type == Event::kRelease || e.type == Event::kHold) &&
        (e.id == keys_.param || e.id == keys_.map) && e.id != kNoKey) {
        const bool isParam = e.id == keys_.param;
        if (e.type == Event::kHold) return true;
        if (e.type == Event::kPress) {
            if (shift) {   // Shift + key: the Shell pages
                const Overlay want = isParam ? kMacro : kMod;
                if (ov_ == want) close();
                else {
                    open(want);
                    latched_ = true;
                }
                keyDown_ = kNoKey;
                return true;
            }
            const Overlay want = isParam ? kParam : kMap;
            if (ov_ == want && latched_) {   // second tap closes a latched view
                if (want == kMap) {
                    const int page = pageAt(mapCol_);
                    close();
                    if (page != returnPage_) jumpToPage(page);
                } else {
                    close();
                }
                keyDown_ = kNoKey;
                return true;
            }
            open(want);
            latched_ = false;
            keyDown_ = e.id;
            keyDownMs_ = e.ms;
            keyUsed_ = false;
            return true;
        }
        // release
        if (keyDown_ == e.id) {
            keyDown_ = kNoKey;
            const Overlay want = isParam ? kParam : kMap;
            if (ov_ == want && !latched_) {
                if (e.ms - keyDownMs_ < 400 && !keyUsed_) {
                    latched_ = true;   // a tap: stays open
                } else if (want == kMap) {
                    const int page = pageAt(mapCol_);
                    close();
                    if (page != returnPage_) jumpToPage(page);
                } else {
                    close();
                }
            }
        }
        return true;
    }
    switch (ov_) {
        case kParam: return layerEvent(e, false);
        case kMap: return layerEvent(e, true);
        case kMacro: return macroEvent(e);
        case kMod: return modEvent(e);
        case kHelp: return helpEvent(e);
        default: break;
    }
    // Shift + Config = HELP is handled by the Shell (it owns Config).
    (void)k;
    return false;
}

void Nav::editFocused(int delta, bool fine) {
    if (kit_->reg.valid(focus_)) kit_->reg.nudge(focus_, delta, fine);
}

void Nav::sourceEncoder(int src, int delta, bool fine) {
    Kit& k = *kit_;
    if (lastRoute_ >= 0 && lastRoute_ < k.mod.routeCount() && k.mod.routeAt(lastRoute_).src == src) {
        const Route& r = k.mod.routeAt(lastRoute_);
        k.mod.setDepth(lastRoute_, r.depth + float(delta) * (fine ? 0.002f : 0.01f));
        hudParam_ = r.param;
        hudUntil_ = now_ + 1100;
        return;
    }
    if (Mod::isMacro(src)) {
        k.reg.nudge(k.mod.macroParam(src - kSrcMacro), delta, fine);
    } else if (src >= kSrcLfo && src < kSrcLfo + kLfos) {
        k.reg.nudge(k.mod.lfoParam(src - kSrcLfo, 1), delta, fine);
    } else if (src >= kSrcRnd && src < kSrcRnd + kRands) {
        k.reg.nudge(k.mod.rndParam(src - kSrcRnd, 1), delta, fine);
    }
}

void Nav::layerPad(int pad, bool down, bool inMap) {
    Kit& k = *kit_;
    const int row = padRow(pad);
    if (inMap && row == 0) {   // MAP: page tiles
        if (down && pad < pageCount_) {
            mapCol_ = pad;
            loadLayout(pageAt(mapCol_));
            keyUsed_ = true;
            for (int i = 0; i < kPadParams; ++i)
                if (layout_[i] != kNoParam) {
                    focus_ = layout_[i];
                    break;
                }
        }
        return;
    }
    if (!inMap && row == 3) {   // PARAM layer: sources
        if (down) {
            if (k.shell->shiftHeld()) {   // Shift + source pad: the other source bank
                layerBank_ ^= 1;
                return;
            }
            const int src = padSource(pad);
            if (src < 0) return;
            heldSourcePad_ = int8_t(pad);
            padMs_ = now_;
            lastRoute_ = -1;
            keyUsed_ = true;
        } else if (heldSourcePad_ == pad) {
            const int src = padSource(pad);
            // a tap on a macro / LFO without routing: focus its main knob
            if (lastRoute_ < 0 && now_ - padMs_ < 350 && src >= 0) {
                if (Mod::isMacro(src)) focus_ = k.mod.macroParam(src - kSrcMacro);
                else if (src < kSrcRnd) focus_ = k.mod.lfoParam(src, 1);
                else if (src < kSrcMacro) focus_ = k.mod.rndParam(src - kSrcRnd, 0);
            }
            heldSourcePad_ = -1;
            lastRoute_ = -1;
        }
        return;
    }
    const ParamId p = padParam(pad, inMap);
    if (down) {
        if (!k.reg.valid(p)) return;
        keyUsed_ = true;
        if (!inMap && heldSourcePad_ >= 0) {   // source held + param pad = a route
            const int src = padSource(heldSourcePad_);
            int r = k.mod.findRoute(src, p);
            if (r < 0) {
                r = k.mod.route(src, p, Mod::isMacro(src) ? 0.5f : 0.25f);
                if (r < 0) {
                    k.ctx->toast(!k.reg.modulatable(p) ? "This parameter takes no modulation"
                                                      : (Mod::isMacro(src) ? "A macro takes 8 targets" : "64 routes in use"));
                    return;
                }
            }
            lastRoute_ = r;
            heldParamPad_ = int8_t(pad);
            hud(p);
            return;
        }
        heldParamPad_ = int8_t(pad);
        setFocus(p);
        learnArmed_ = k.shell->shiftHeld();
        if (learnArmed_) {
            char msg[64];
            std::snprintf(msg, sizeof msg, "MIDI learn: move a CC for %s", k.reg.name(p));
            k.ctx->toast(msg);
        }
    } else if (heldParamPad_ == pad) {
        heldParamPad_ = -1;
        learnArmed_ = false;
    }
}

bool Nav::layerEvent(const Event& e, bool inMap) {
    Kit& k = *kit_;
    switch (e.type) {
        case Event::kPadDown:
        case Event::kPadUp:
            layerPad(e.id, e.type == Event::kPadDown, inMap);
            padMine_[e.id] = e.type == Event::kPadDown;
            return true;
        case Event::kEncoder: {
            keyUsed_ = true;
            const bool fine = e.shift;
            if (!inMap && heldSourcePad_ >= 0) {
                const int src = padSource(heldSourcePad_);
                if (heldParamPad_ >= 0) {
                    const ParamId p = padParam(heldParamPad_, false);
                    const int r = k.mod.findRoute(src, p);
                    if (r >= 0) {
                        k.mod.setDepth(r, k.mod.routeAt(r).depth + float(e.delta) * (fine ? 0.002f : 0.01f));
                        lastRoute_ = r;
                        hud(p);
                    }
                } else {
                    sourceEncoder(src, e.delta, fine);
                }
                return true;
            }
            if (heldParamPad_ >= 0) {
                const ParamId p = padParam(heldParamPad_, inMap);
                if (k.reg.valid(p)) k.reg.nudge(p, e.delta, fine);
                return true;
            }
            editFocused(e.delta, fine);
            return true;
        }
        case Event::kPress:
            if (e.id == btn::Up || e.id == btn::Down) {
                const int d = e.id == btn::Down ? 1 : -1;
                if (inMap) {   // the focused param of the selected page
                    ParamId list[kMaxParams > 64 ? 64 : kMaxParams];
                    const int n = k.reg.onPage(pageAt(mapCol_), list, 64);
                    int at = 0;
                    for (int i = 0; i < n; ++i)
                        if (list[i] == focus_) at = i;
                    if (n) focus_ = list[clampi(at + d, 0, n - 1)];
                } else {
                    int col = pageIndexOf(layerPage_) + d;
                    if (col < 0) col = pageCount_ - 1;
                    if (col >= pageCount_) col = 0;
                    const int page = pageAt(col);
                    layerPage_ = page;
                    loadLayout(page);
                    if (page < kPageMacro && k.ws) {
                        k.ws->navGoto(page);
                        lastWsPage_ = k.ws->navPage();
                    }
                    for (int i = 0; i < kPadParams; ++i)
                        if (layout_[i] != kNoParam) {
                            focus_ = layout_[i];
                            break;
                        }
                }
                keyUsed_ = true;
                return true;
            }
            if (e.id == btn::Left || e.id == btn::Right) {
                const int d = e.id == btn::Right ? 1 : -1;
                if (inMap) {
                    mapCol_ = clampi(mapCol_ + d, 0, pageCount_ - 1);
                    loadLayout(pageAt(mapCol_));
                    for (int i = 0; i < kPadParams; ++i)
                        if (layout_[i] != kNoParam) {
                            focus_ = layout_[i];
                            break;
                        }
                } else {
                    ParamId list[64];
                    const int n = k.reg.onPage(layerPage_, list, 64);
                    int at = -1;
                    for (int i = 0; i < n; ++i)
                        if (list[i] == focus_) at = i;
                    if (n) setFocus(list[clampi(at + d, 0, n - 1)]);
                }
                keyUsed_ = true;
                return true;
            }
            if (e.id == btn::Delete) {
                keyUsed_ = true;
                if (!inMap && heldSourcePad_ >= 0) {
                    const int src = padSource(heldSourcePad_);
                    if (heldParamPad_ >= 0) k.mod.unroute(src, padParam(heldParamPad_, false));
                    else if (lastRoute_ >= 0) k.mod.removeRoute(lastRoute_);
                    else if (e.shift) {
                        int idx[kMaxRoutes];
                        for (int n = k.mod.routesFrom(src, idx, kMaxRoutes); n > 0; --n) k.mod.removeRoute(idx[n - 1]);
                        k.ctx->toast("Routes of this source removed");
                    }
                    lastRoute_ = -1;
                    return true;
                }
                if (learnArmed_ && heldParamPad_ >= 0) {
                    const ParamId p = padParam(heldParamPad_, inMap);
                    for (Learn& l : learn_)
                        if (l.param == p) l = Learn();
                    k.ctx->toast("MIDI learn removed");
                    return true;
                }
                if (e.shift && k.reg.valid(focus_)) {
                    int idx[kMaxRoutes];
                    for (int n = k.mod.routesTo(focus_, idx, kMaxRoutes); n > 0; --n) k.mod.removeRoute(idx[n - 1]);
                    k.ctx->toast("Modulation removed");
                    return true;
                }
                if (k.reg.valid(focus_)) k.reg.reset(focus_);
                return true;
            }
            if (inMap) {
                if (e.id == btn::F8) {
                    const int page = pageAt(mapCol_);
                    close();
                    if (page != returnPage_) jumpToPage(page);
                }
                return e.id != btn::Shift;   // the MAP is full screen: keys stay with it
            }
            return false;   // PARAM pads: F-keys and the rest reach the workstation
        case Event::kRelease:
        case Event::kHold:
            if (e.id == btn::Up || e.id == btn::Down || e.id == btn::Left || e.id == btn::Right || e.id == btn::Delete)
                return true;
            return inMap && e.id != btn::Shift;
        default: return false;
    }
}

bool Nav::macroEvent(const Event& e) {
    Kit& k = *kit_;
    int targets[kMacroTargets];
    const int src = kSrcMacro + macroSel_;
    const int nt = k.mod.routesFrom(src, targets, kMacroTargets);
    switch (e.type) {
        case Event::kPadDown: {
            padMine_[e.id] = true;
            const int col = padCol(e.id), row = padRow(e.id);
            if (col < kMacros) {   // 8 macro faders, top = 100 %
                macroSel_ = col;
                heldParamPad_ = int8_t(e.id);
                k.reg.setNormFromShell(k.mod.macroParam(col), float(3 - row) / 3.0f);
            } else if (row < 2) {   // the target slots of the selected macro
                const int t = row * 4 + (col - kMacros);
                if (t < nt) macroRow_ = t + 1;
            }
            return true;
        }
        case Event::kPadUp:
            padMine_[e.id] = false;
            if (heldParamPad_ == e.id) heldParamPad_ = -1;
            return true;
        case Event::kEncoder: {
            const bool fine = e.shift;
            if (heldParamPad_ >= 0) {
                k.reg.nudge(k.mod.macroParam(padCol(heldParamPad_)), e.delta, fine);
                return true;
            }
            if (macroRow_ == 0 || macroRow_ > nt) {
                k.reg.nudge(k.mod.macroParam(macroSel_), e.delta, fine);
                return true;
            }
            const int r = targets[macroRow_ - 1];
            const Route& rt = k.mod.routeAt(r);
            const float d = float(e.delta) * (fine ? 0.002f : 0.01f);
            if (macroField_ == 0) k.mod.setDepth(r, rt.depth + d);
            else if (macroField_ == 2) k.mod.setWindow(r, true, rt.lo + d, rt.hi);
            else if (macroField_ == 3) k.mod.setWindow(r, true, rt.lo, rt.hi + d);
            else k.mod.setWindow(r, e.delta > 0, rt.lo, rt.hi);
            return true;
        }
        case Event::kPress:
            if (e.id <= btn::F8) {
                macroSel_ = e.id;
                macroRow_ = 0;
                return true;
            }
            if (e.id == btn::Up) macroRow_ = macroRow_ > 0 ? macroRow_ - 1 : 0;
            else if (e.id == btn::Down) macroRow_ = macroRow_ < nt ? macroRow_ + 1 : nt;
            else if (e.id == btn::Left) macroField_ = macroField_ > 0 ? macroField_ - 1 : 0;
            else if (e.id == btn::Right) macroField_ = macroField_ < 3 ? macroField_ + 1 : 3;
            else if (e.id == btn::Delete) {
                if (e.shift) {
                    for (int i = nt; i > 0; --i) k.mod.removeRoute(targets[i - 1]);
                    macroRow_ = 0;
                } else if (macroRow_ > 0 && macroRow_ <= nt) {
                    k.mod.removeRoute(targets[macroRow_ - 1]);
                    if (macroRow_ > nt - 1) macroRow_ = nt - 1;
                } else {
                    k.reg.reset(k.mod.macroParam(macroSel_));
                }
            } else if (e.id == btn::Insert && k.keyboard) {   // rename the macro
                KeyboardRequest q;
                q.title = "Macro name";
                q.initial = k.mod.macroName(macroSel_);
                q.maxLen = 10;
                q.ctx = kit_;
                q.onDone = [](void* c, const char* text) {
                    Kit* kk = static_cast<Kit*>(c);
                    kk->mod.setMacroName(kk->nav.macroSelected(), text);
                };
                k.keyboard->open(q);
            }
            if (macroField_ != 0 && macroRow_ > 0 && macroRow_ <= nt && macroField_ == 1) {
                // field 1 = mode: Left / Right on it toggles add / window
            }
            return e.id != btn::Shift;
        case Event::kRelease:
        case Event::kHold: return e.id != btn::Shift;
        default: return false;
    }
}

bool Nav::modEvent(const Event& e) {
    Kit& k = *kit_;
    const int rows = kLfos + kRands + k.mod.routeCount();
    switch (e.type) {
        case Event::kPadDown: {
            padMine_[e.id] = true;
            if (padRow(e.id) == 3) {
                const int c = padCol(e.id);
                if (c < kLfos + kRands) {
                    modRow_ = c;
                    modField_ = 0;
                }
            } else if (e.id < k.mod.routeCount()) {
                modRow_ = kLfos + kRands + e.id;
            }
            return true;
        }
        case Event::kPadUp: padMine_[e.id] = false; return true;
        case Event::kEncoder: {
            const bool fine = e.shift;
            if (modRow_ < kLfos) k.reg.nudge(k.mod.lfoParam(modRow_, clampi(modField_, 0, 4)), e.delta, fine);
            else if (modRow_ < kLfos + kRands) k.reg.nudge(k.mod.rndParam(modRow_ - kLfos, clampi(modField_, 0, 2)), e.delta, fine);
            else {
                const int r = modRow_ - kLfos - kRands;
                if (r < k.mod.routeCount()) k.mod.setDepth(r, k.mod.routeAt(r).depth + float(e.delta) * (fine ? 0.002f : 0.01f));
            }
            return true;
        }
        case Event::kPress:
            if (e.id == btn::Up) modRow_ = modRow_ > 0 ? modRow_ - 1 : 0;
            else if (e.id == btn::Down) modRow_ = modRow_ + 1 < rows ? modRow_ + 1 : modRow_;
            else if (e.id == btn::Left) modField_ = modField_ > 0 ? modField_ - 1 : 0;
            else if (e.id == btn::Right) modField_ = modField_ < 4 ? modField_ + 1 : 4;
            else if (e.id == btn::Delete) {
                if (modRow_ >= kLfos + kRands) {
                    k.mod.removeRoute(modRow_ - kLfos - kRands);
                    if (modRow_ >= kLfos + kRands + k.mod.routeCount() && modRow_ > 0) --modRow_;
                } else if (e.shift) {
                    const int src = modRow_ < kLfos ? kSrcLfo + modRow_ : kSrcRnd + modRow_ - kLfos;
                    int idx[kMaxRoutes];
                    for (int n = k.mod.routesFrom(src, idx, kMaxRoutes); n > 0; --n) k.mod.removeRoute(idx[n - 1]);
                }
            } else if (e.id == btn::F8) close();
            if (modRow_ < kLfos) modField_ = clampi(modField_, 0, 4);
            else if (modRow_ < kLfos + kRands) modField_ = clampi(modField_, 0, 2);
            return e.id != btn::Shift;
        case Event::kRelease:
        case Event::kHold: return e.id != btn::Shift;
        default: return false;
    }
}

bool Nav::helpEvent(const Event& e) {
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        padMine_[e.id] = e.type == Event::kPadDown;
        return true;
    }
    if (e.type == Event::kPress && e.id != btn::Shift) close();
    return e.id != btn::Shift;
}

// ---- Nav: drawing --------------------------------------------------------------------------------------
namespace {
// The 12 x 3 grid of a page's param pads (PARAM layer rows 1-3, MAP rows 2-4).
void drawParamCells(Canvas& g, const Theme& t, const Kit& k, const ParamId* layout, ParamId focus, int heldSrc,
                    int x0, int y0, int cellH, bool values) {
    const Registry& r = k.reg;
    const int w = kScreenW;
    // one background for the grid, the cell lines, then the used cells (one batch)
    g.color(scale(t.panel, 1.4f));
    g.fill(x0, y0, w, 3 * cellH);
    g.color(t.bg);
    g.beginRects();
    for (int c = 1; c < kPadCols; ++c) {
        const float x = float(x0 + px(c, kPadCols, w));
        g.vtx(x - 1.0f, float(y0));
        g.vtx(x, float(y0 + 3 * cellH - 1));
    }
    for (int row = 1; row < 3; ++row) {
        g.vtx(float(x0), float(y0 + row * cellH - 2));
        g.vtx(float(x0 + w - 1), float(y0 + row * cellH - 1));
    }
    for (int i = 0; i < kPadParams; ++i) {
        const ParamId p = layout[i];
        if (!r.valid(p)) continue;
        const int c = i % kPadCols, row = i / kPadCols;
        const int x = x0 + px(c, kPadCols, w) + 1, y = y0 + row * cellH + 1;
        const int cw = px(c + 1, kPadCols, w) - px(c, kPadCols, w) - 2;
        Rgb bg = scale(r.color(p), p == focus ? 0.62f : 0.22f);
        if (heldSrc >= 0 && k.mod.findRoute(heldSrc, p) >= 0) bg = scale(k.mod.sourceColor(heldSrc), 0.55f);
        g.color(bg);
        g.vtx(float(x), float(y));
        g.vtx(float(x + cw - 1), float(y + cellH - 3));
        const float n = r.effectiveNorm(p);
        g.color(p == focus ? kWhite : r.color(p));
        const int bw = cw - 8, by = y + cellH - 9;
        if (r.isBipolar(p)) {
            const int cx = x + 4 + bw / 2, ex = x + 4 + int(float(bw) * n);
            g.vtx(float(cx < ex ? cx : ex), float(by));
            g.vtx(float((cx < ex ? ex : cx) + 1), float(by + 3));
        } else {
            g.vtx(float(x + 4), float(by));
            g.vtx(float(x + 4 + int(float(bw) * n) + 1), float(by + 3));
        }
    }
    g.endBatch();
    char nm[24], val[24];
    for (int i = 0; i < kPadParams; ++i) {
        const ParamId p = layout[i];
        if (!r.valid(p)) continue;
        const int c = i % kPadCols, row = i / kPadCols;
        const int x = x0 + px(c, kPadCols, w), y = y0 + row * cellH;
        const int cw = px(c + 1, kPadCols, w) - x;
        fitText(nm, sizeof nm, font::kTiny, r.name(p), cw - 8);
        g.color(p == focus ? kWhite : t.fg);
        g.text(x + 5, y + (values ? 10 : cellH / 2 - 3), font::kTiny, nm, Canvas::kCenterY);
        if (values) {
            r.format(p, val, sizeof val, true);
            fitText(nm, sizeof nm, font::kTiny, val, cw - 8);
            g.color(p == focus ? kWhite : t.dim);
            g.text(x + 5, y + 25, font::kTiny, nm, Canvas::kCenterY);
        }
        if (r.meta(p).mods && g.room(8)) {
            int idx[1];
            k.mod.routesTo(p, idx, 1);
            g.color(k.mod.sourceColor(k.mod.routeAt(idx[0]).src));
            g.point(float(x + cw - 8), float(y + 8), 3.0f);
        }
    }
}
}  // namespace

void Nav::drawFocusStrip(Canvas& g, int y) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    const Registry& r = k.reg;
    char buf[64], val[32];
    // a source pad is held: the strip is about that source and the route being set
    const int src = heldSourcePad_ >= 0 ? padSource(heldSourcePad_) : -1;
    if (src >= 0) {
        const Rgb sc = k.mod.sourceColor(src);
        fitText(buf, sizeof buf, font::kTiny, k.mod.sourceName(src), 64);
        g.color(sc);
        g.fillRound(8, y + 4, 72, 20, 4);
        g.color(t.bg);
        g.text(44, y + 14, font::kTiny, buf, Canvas::kCenter);
        const int ri = (lastRoute_ >= 0 && lastRoute_ < k.mod.routeCount()) ? lastRoute_ : -1;
        if (ri < 0) {
            const int n = k.mod.routesFrom(src, nullptr, 0);
            g.color(t.fg);
            g.textf(88, y + 14, font::kBody, Canvas::kCenterY, n == 1 ? "1 route - tap params to add" : "%d routes - tap params to add", n);
            return;
        }
        const Route& rt = k.mod.routeAt(ri);
        paramLabel(rt.param, buf + 2, int(sizeof buf) - 2, true);
        buf[0] = '>';
        buf[1] = ' ';
        fitText(val, sizeof val, font::kBody, buf, 300);
        g.color(t.fg);
        g.text(88, y + 14, font::kBody, val, Canvas::kCenterY);
        if (rt.window) std::snprintf(buf, sizeof buf, "%d..%d%%", int(rt.lo * 100.0f + 0.5f), int(rt.hi * 100.0f + 0.5f));
        else std::snprintf(buf, sizeof buf, "%+d%%", int(rt.depth * 100.0f + (rt.depth < 0 ? -0.5f : 0.5f)));
        g.color(sc);
        g.text(400, y + 14, font::kLarge, buf, Canvas::kCenterY);
        const int bx = 500, bw = 200, by = y + 10;
        g.color(scale(t.dim, 0.45f));
        g.fill(bx, by, bw, 8);
        g.color(sc);
        if (rt.window) {
            g.fill(bx + int(float(bw) * rt.lo), by, int(float(bw) * fabsf(rt.hi - rt.lo)) + 1, 8);
        } else {
            const int c = bx + bw / 2, e = c + int(float(bw / 2) * rt.depth);
            g.fill(c < e ? c : e, by, (c < e ? e - c : c - e) + 1, 8);
            g.color(t.fg);
            g.fill(c, by - 3, 1, 14);
        }
        r.format(rt.param, val, sizeof val, true);
        g.color(t.dim);
        g.text(792, y + 14, font::kTiny, val, Canvas::kCenterY | Canvas::kRight);
        return;
    }
    if (!r.valid(focus_)) return;
    const int pg = r.page(focus_);
    const Rgb pc = pageColorOf(*this, pages_, wsPages_, pg, t);
    // page tag
    fitText(buf, sizeof buf, font::kTiny, pageName(pg), 64);
    g.color(pc);
    g.fillRound(8, y + 4, 72, 20, 4);
    g.color(t.bg);
    g.text(44, y + 14, font::kTiny, buf, Canvas::kCenter);
    // name + value
    char lbl[48];
    paramLabel(focus_, lbl, int(sizeof lbl), false);
    fitText(buf, sizeof buf, font::kBody, lbl, 190);
    g.color(t.fg);
    g.text(88, y + 14, font::kBody, buf, Canvas::kCenterY);
    r.format(focus_, val, sizeof val);
    fitText(buf, sizeof buf, font::kLarge, val, 160);
    g.color(t.accent);
    g.text(286, y + 14, font::kLarge, buf, Canvas::kCenterY);
    // range bar: base + effective
    const int bx = 456, bw = 110, by = y + 10;
    g.color(scale(t.dim, 0.45f));
    g.fill(bx, by, bw, 8);
    const float nb = r.norm(focus_), ne = r.effectiveNorm(focus_);
    g.color(r.color(focus_));
    g.fill(bx, by, int(float(bw) * nb) + 1, 8);
    if (fabsf(ne - nb) > 0.004f) {
        g.color(kWhite);
        g.fill(bx + int(float(bw - 3) * ne), by - 3, 3, 14);
    }
    // routes into this param (and a learned CC)
    int idx[4];
    const int n = k.mod.routesTo(focus_, idx, 4);
    int x = 578;
    for (int i = 0; i < n && i < 2; ++i) {
        const Route& rt = k.mod.routeAt(idx[i]);
        if (rt.window) std::snprintf(buf, sizeof buf, "%s %d-%d", k.mod.sourceName(rt.src), int(rt.lo * 100.0f + 0.5f), int(rt.hi * 100.0f + 0.5f));
        else std::snprintf(buf, sizeof buf, "%s %+d", k.mod.sourceName(rt.src), int(rt.depth * 100.0f + (rt.depth >= 0 ? 0.5f : -0.5f)));
        fitText(val, sizeof val, font::kTiny, buf, 90);
        g.color(scale(k.mod.sourceColor(rt.src), 0.3f));
        g.fillRound(x, y + 4, 96, 20, 4);
        g.color(k.mod.sourceColor(rt.src));
        g.text(x + 48, y + 14, font::kTiny, val, Canvas::kCenter);
        x += 100;
    }
    if (n > 2) {
        g.color(t.dim);
        g.textf(794, y + 14, font::kTiny, Canvas::kCenterY | Canvas::kRight, "+%d", n - 2);
    }
    for (const Learn& l : learn_)
        if (l.status && l.param == focus_ && n < 2) {
            g.color(t.dim);
            g.textf(x + 4, y + 14, font::kTiny, Canvas::kCenterY, "CC%d", l.cc);
            break;
        }
}

void Nav::drawLayer(Canvas& g) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    const int y0 = 258;
    g.alpha(255);        // the page may have left a wide line or a fade set
    g.lineWidth(1.0f);
    g.color(t.bg);
    g.fill(0, y0, kScreenW, kScreenH - 30 - y0);
    g.color(t.accent);
    g.fill(0, y0, kScreenW, 2);
    // title chip
    const char* chip = latched_ ? "PARAM" : "PARAM (held)";
    const int cw = textWidth(font::kTiny, chip) + 24;
    g.color(t.accent);
    g.fillRound(8, y0 - 22, cw, 22, 5);
    g.color(t.bg);
    g.text(8 + cw / 2, y0 - 11, font::kTiny, chip, Canvas::kCenter);
    drawFocusStrip(g, y0 + 4);
    // hint line
    const char* hint;
    if (heldSourcePad_ >= 0) hint = "tap params: route   encoder: depth   Delete: remove";
    else if (learnArmed_) hint = "move a MIDI CC to learn it   Delete: forget";
    else hint = "hold pad + encoder: edit   hold source + pad: route";
    if (g.room(64)) {
        g.color(t.dim);
        g.text(400, y0 + 40, font::kTiny, hint, Canvas::kCenter);
    }
    // the grid: rows 1-3 params, row 4 sources
    const int gy = y0 + 50, ch = 35;
    const int heldSrc = heldSourcePad_ >= 0 ? padSource(heldSourcePad_) : -1;
    drawParamCells(g, t, k, layout_, focus_, heldSrc, 0, gy, ch, false);
    // sources row
    const int sy = gy + 3 * ch;
    g.beginRects();
    for (int c = 0; c < kPadCols; ++c) {
        const int s = padSource(3 * kPadCols + c);
        const int x = px(c, kPadCols, kScreenW) + 1, cw = px(c + 1, kPadCols, kScreenW) - px(c, kPadCols, kScreenW) - 2;
        if (s < 0) {
            g.color(scale(t.panel, 1.2f));
            g.vtx(float(x), float(sy + 1));
            g.vtx(float(x + cw - 1), float(sy + ch - 3));
            continue;
        }
        const Rgb sc = k.mod.sourceColor(s);
        const float v = k.mod.sourceValue(s);
        const float u = k.mod.sourceBipolar(s) ? 0.5f * (v + 1.0f) : v;
        g.color(heldSourcePad_ == 3 * kPadCols + c ? scale(sc, 0.8f) : scale(sc, 0.18f + 0.22f * u));
        g.vtx(float(x), float(sy + 1));
        g.vtx(float(x + cw - 1), float(sy + ch - 3));
        g.color(sc);
        g.vtx(float(x + 4), float(sy + ch - 9));
        g.vtx(float(x + 4 + int(float(cw - 8) * u) + 1), float(sy + ch - 6));
    }
    g.endBatch();
    char nm[16];
    for (int c = 0; c < kPadCols; ++c) {
        const int s = padSource(3 * kPadCols + c);
        if (s < 0) continue;
        const int x = px(c, kPadCols, kScreenW), cw = px(c + 1, kPadCols, kScreenW) - x;
        fitText(nm, sizeof nm, font::kTiny, k.mod.sourceName(s), cw - 8);
        g.color(heldSourcePad_ == 3 * kPadCols + c ? t.bg : t.fg);
        g.text(x + 5, sy + 13, font::kTiny, nm, Canvas::kCenterY);
    }
}

void Nav::drawMap(Canvas& g) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    const Registry& r = k.reg;
    drawHeader(g, t, *k.shell, "MAP");
    g.color(t.dim);
    g.text(790, 15, font::kTiny, "", Canvas::kRight);
    const int sel = pageAt(mapCol_);
    // 1) the selected page as pads (rows 2-4 of the pad grid) -- first, it matters most
    const int gy = 312, ch = 44;
    g.color(t.dim);
    g.textf(8, gy - 10, font::kTiny, Canvas::kCenterY, "%s on the pads (rows 2-4)", pageName(sel));
    drawParamCells(g, t, k, layout_, focus_, -1, 0, gy, ch, true);
    // 2) focused param line
    if (r.valid(focus_)) {
        char val[32], buf[64];
        r.format(focus_, val, sizeof val);
        paramLabel(focus_, buf, int(sizeof buf), true);
        g.color(t.panel);
        g.fill(0, kScreenH - 30, kScreenW, 30);
        g.color(t.fg);
        g.text(10, kScreenH - 15, font::kBody, buf, Canvas::kCenterY);
        g.color(t.accent);
        g.text(330, kScreenH - 15, font::kLarge, val, Canvas::kCenterY);
        if (fabsf(r.effectiveNorm(focus_) - r.norm(focus_)) > 0.004f) {
            r.format(focus_, val, sizeof val, true);
            std::snprintf(buf, sizeof buf, "mod %s", val);
            g.color(t.dim);
            g.text(470, kScreenH - 15, font::kTiny, buf, Canvas::kCenterY);
        }
        g.color(t.dim);
        g.text(790, kScreenH - 15, font::kTiny, latched_ ? "Master / F8: go there" : "release Master: go there",
               Canvas::kCenterY | Canvas::kRight);
    }
    // 3) every page as a column of its params
    const int n = pageCount_;
    const int colW = kScreenW / (n < 6 ? 6 : n);
    const int listY = 70, rowH = 15, maxRows = (gy - 24 - listY) / rowH;
    ParamId list[64];
    char nm[24];
    // headers first, so every page stays reachable however big the registry is
    for (int c = 0; c < n; ++c) {
        const int page = pageAt(c);
        const int x = c * colW;
        const Rgb pc = pageColorOf(*this, pages_, wsPages_, page, t);
        const bool on = c == mapCol_;
        g.color(on ? pc : scale(pc, 0.28f));
        g.fillRound(x + 2, 36, colW - 4, 26, 4);
        fitText(nm, sizeof nm, colW >= 90 ? font::kSmall : font::kTiny, pageName(page), colW - 10);
        g.color(on ? t.bg : t.fg);
        g.text(x + colW / 2, 49, colW >= 90 ? font::kSmall : font::kTiny, nm, Canvas::kCenter);
        if (on) {
            g.color(scale(pc, 0.12f));
            g.fill(x + 2, listY - 4, colW - 4, gy - 22 - listY + 4);
        }
    }
    // then the lists: the selected column in full, the others share what is left
    constexpr uint32_t kRowWords = 18u;   // a tiny name (~10 chars + 4) + colour changes
    for (int o = 0; o < n; ++o) {
        const int c = o == 0 ? mapCol_ : (o <= mapCol_ ? o - 1 : o);
        const int page = pageAt(c);
        const int x = c * colW;
        const bool on = c == mapCol_;
        if (page == kPageMod) {   // the MOD page: its sources and the routes, not 26 fields
            for (int s = 0; s < kLfos + kRands && g.room(24); ++s) {
                const int nr = k.mod.routesFrom(s, nullptr, 0);
                g.color(k.mod.sourceColor(s));
                g.text(x + 9, listY + s * rowH + 6, font::kTiny, kLayerNames[s], Canvas::kCenterY);
                if (nr) {
                    g.color(t.dim);
                    g.textf(x + colW - 8, listY + s * rowH + 6, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d", nr);
                }
            }
            if (g.room(16)) {
                g.color(on ? t.fg : t.dim);
                g.textf(x + 9, listY + 7 * rowH + 6, font::kTiny, Canvas::kCenterY, "%d routes", k.mod.routeCount());
            }
            continue;
        }
        const int cnt = r.onPage(page, list, 64);
        int rows = maxRows;
        if (!on) {   // fair share of the words left between the columns still to draw
            const int share = int(g.left() / (uint32_t(n - o) * kRowWords));
            if (share < 3) {   // too little room for names: just how many there are
                if (g.room(16)) {
                    g.color(t.dim);
                    g.textf(x + 9, listY + 6, font::kTiny, Canvas::kCenterY, colW >= 80 ? "%d params" : "%d", cnt);
                }
                continue;
            }
            if (share < rows) rows = share;
        }
        const int show = cnt > rows ? rows - 1 : cnt;
        for (int i = 0; i < show; ++i) {
            if (!g.room(kRowWords)) break;
            fitText(nm, sizeof nm, font::kTiny, r.name(list[i]), colW - 12);
            if (list[i] == focus_) {
                g.color(t.accent);
                g.fill(x + 3, listY + i * rowH - 1, 3, rowH - 2);
            }
            g.color(list[i] == focus_ ? t.accent : (on ? t.fg : t.dim));
            g.text(x + 9, listY + i * rowH + 6, font::kTiny, nm, Canvas::kCenterY);
        }
        if (cnt > show && g.room(16)) {
            g.color(t.dim);
            g.textf(x + 9, listY + show * rowH + 6, font::kTiny, Canvas::kCenterY, "+%d more", cnt - show);
        }
    }
}

void Nav::drawMacro(Canvas& g) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    const Registry& r = k.reg;
    drawHeader(g, t, *k.shell, "MACROS");
    const Rgb amber(0xFFB020);
    const float a0 = 2.3561945f, span = 4.712389f;
    char buf[64], val[32];
    for (int m = 0; m < kMacros; ++m) {
        const int cx = 50 + m * 100, cy = 104;
        const bool on = m == macroSel_;
        const float v = k.mod.macro(m);
        if (on) {
            g.color(scale(amber, 0.16f));
            g.fillRound(cx - 47, 40, 94, 160, 8);
        }
        g.lineWidth(4.0f);
        g.color(scale(t.dim, 0.5f));
        g.arc(float(cx), float(cy), 32.0f, a0, a0 + span, 16);
        if (v > 0.005f) {
            g.color(on ? amber : scale(amber, 0.7f));
            g.arc(float(cx), float(cy), 32.0f, a0, a0 + span * v, int(3 + 13 * v));
        }
        g.lineWidth(1.0f);
        g.color(on ? t.fg : t.dim);
        g.textf(cx, cy, font::kSmall, Canvas::kCenter, "%d%%", int(v * 100.0f + 0.5f));
        fitText(buf, sizeof buf, font::kSmall, k.mod.macroName(m), 90);
        g.color(on ? amber : t.fg);
        g.text(cx, 158, font::kSmall, buf, Canvas::kCenter);
        const int nt = k.mod.routesFrom(kSrcMacro + m, nullptr, 0);
        g.color(t.dim);
        g.textf(cx, 180, font::kTiny, Canvas::kCenter, nt == 1 ? "1 target" : "%d targets", nt);
    }
    // the selected macro's targets
    int targets[kMacroTargets];
    const int nt = k.mod.routesFrom(kSrcMacro + macroSel_, targets, kMacroTargets);
    const int ty = 214;
    g.color(amber);
    std::snprintf(buf, sizeof buf, "%s  -  targets", k.mod.macroName(macroSel_));
    g.text(20, ty, font::kBody, buf, Canvas::kCenterY);
    g.color(t.dim);
    g.text(780, ty, font::kTiny,
           "PARAM pads: hold this macro's pad + tap params to add",
           Canvas::kCenterY | Canvas::kRight);
    const char* fields[4] = {"DEPTH", "MODE", "MIN", "MAX"};
    for (int f = 0; f < 4; ++f) {
        g.color(macroField_ == f && macroRow_ > 0 ? t.accent : t.dim);
        g.text(420 + f * 90, ty + 22, font::kTiny, fields[f], Canvas::kCenterY);
    }
    if (!nt) {
        g.color(t.dim);
        g.text(400, ty + 80, font::kSmall, "No targets yet: open the PARAM pads (I.Param), hold a macro pad", Canvas::kCenter);
        g.text(400, ty + 102, font::kSmall, "(bottom row) and tap the parameters it should move.", Canvas::kCenter);
    }
    for (int i = 0; i < nt; ++i) {
        const Route& rt = k.mod.routeAt(targets[i]);
        const int y = ty + 40 + i * 24;
        const bool selRow = macroRow_ == i + 1;
        if (selRow) {
            g.color(scale(amber, 0.18f));
            g.fill(14, y - 2, 772, 22);
            g.color(amber);
            g.fill(14, y - 2, 3, 22);
        }
        paramLabel(rt.param, buf, int(sizeof buf), true);
        fitText(val, sizeof val, font::kSmall, buf, 380);
        g.color(t.fg);
        g.text(26, y + 9, font::kSmall, val, Canvas::kCenterY);
        // depth bar (centre = 0)
        const int bx = 420, bw = 80;
        g.color(scale(t.dim, 0.4f));
        g.fill(bx, y + 6, bw, 6);
        g.color(r.color(rt.param));
        if (rt.window) {
            g.fill(bx + int(float(bw) * (rt.lo < rt.hi ? rt.lo : rt.hi)), y + 6, int(float(bw) * fabsf(rt.hi - rt.lo)) + 1, 6);
        } else {
            const int c = bx + bw / 2, e = c + int(float(bw / 2) * rt.depth);
            g.fill(c < e ? c : e, y + 6, (c < e ? e - c : c - e) + 1, 6);
        }
        g.color(t.fg);
        if (rt.window) {
            g.text(510, y + 9, font::kTiny, "WINDOW", Canvas::kCenterY);
            g.textf(600, y + 9, font::kTiny, Canvas::kCenterY, "%d%%", int(rt.lo * 100.0f + 0.5f));
            g.textf(690, y + 9, font::kTiny, Canvas::kCenterY, "%d%%", int(rt.hi * 100.0f + 0.5f));
        } else {
            g.textf(510, y + 9, font::kTiny, Canvas::kCenterY, "ADD %+d%%", int(rt.depth * 100.0f + (rt.depth < 0 ? -0.5f : 0.5f)));
            g.color(t.dim);
            g.text(600, y + 9, font::kTiny, "-", Canvas::kCenterY);
            g.text(690, y + 9, font::kTiny, "-", Canvas::kCenterY);
        }
        r.format(rt.param, val, sizeof val, true);
        g.color(t.dim);
        g.text(780, y + 9, font::kTiny, val, Canvas::kCenterY | Canvas::kRight);
    }
    g.color(t.dim);
    g.text(400, 436, font::kTiny,
           "pads: 8 faders  |  F1-F8 macro  |  Up/Down target  |  Left/Right field  |  encoder value  |  Insert name  |  Delete",
           Canvas::kCenter);
    const char* keys[8];
    char names[8][12];
    for (int m = 0; m < kMacros; ++m) {
        fitText(names[m], sizeof names[m], font::kSmall, k.mod.macroName(m), 92);
        keys[m] = names[m];
    }
    drawFKeyBar(g, t, keys, macroSel_);
}

void Nav::drawMod(Canvas& g) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    const Registry& r = k.reg;
    drawHeader(g, t, *k.shell, "MOD");
    char buf[64], val[32];
    const int cardW = kScreenW / (kLfos + kRands);
    for (int s = 0; s < kLfos + kRands; ++s) {
        const bool isLfo = s < kLfos;
        const int x = s * cardW, y = 36;
        const Rgb sc = k.mod.sourceColor(s);
        const bool on = modRow_ == s;
        g.color(on ? scale(sc, 0.2f) : scale(t.panel, 1.2f));
        g.fillRound(x + 3, y, cardW - 6, 206, 6);
        g.color(sc);
        g.text(x + 10, y + 13, font::kSmall, isLfo ? kLayerNames[s] : kLayerNames[s], Canvas::kCenterY);
        // live value
        const float v = k.mod.sourceValue(s);
        g.color(scale(t.dim, 0.5f));
        g.fill(x + 10, y + 30, cardW - 20, 2);
        g.color(sc);
        const int mid = x + cardW / 2, ex = mid + int(float(cardW / 2 - 10) * v);
        g.fill(mid < ex ? mid : ex, y + 28, (mid < ex ? ex - mid : mid - ex) + 2, 6);
        // the shape (LFO): 24 points
        if (isLfo) {
            const Mod::Lfo& L = k.mod.lfo(s);
            const int shape = int(L.shape + 0.5f);
            g.color(scale(sc, 0.8f));
            g.beginStrip();
            for (int i = 0; i <= 24; ++i) {
                const float p = float(i) / 24.0f;
                float yv;
                switch (shape) {
                    case 0: yv = sinf(6.2831853f * p); break;
                    case 1: yv = 1.0f - 4.0f * fabsf(p - 0.5f); break;
                    case 2: yv = 2.0f * p - 1.0f; break;
                    case 3: yv = p < 0.5f ? 1.0f : -1.0f; break;
                    default: yv = (i % 6 < 3) ? 0.6f * float((i * 7) % 5 - 2) / 2.0f : 0.6f * float((i * 3) % 5 - 2) / 2.0f; break;
                }
                g.vtx(float(x + 12 + i * (cardW - 24) / 24), float(y + 62 - int(yv * 18.0f)));
            }
            g.endBatch();
            g.color(kWhite);
            g.point(float(x + 12 + int(L.ph * float(cardW - 24))), float(y + 62 - int(v * 18.0f)), 3.0f);
        } else {
            g.color(t.dim);
            g.text(x + cardW / 2, y + 62, font::kTiny, "random", Canvas::kCenter);
        }
        const int fields = isLfo ? 5 : 3;
        for (int f = 0; f < fields; ++f) {
            const ParamId p = isLfo ? k.mod.lfoParam(s, f) : k.mod.rndParam(s - kLfos, f);
            const int fy = y + 96 + f * 21;
            const bool fsel = on && modField_ == f;
            if (fsel) {
                g.color(scale(sc, 0.45f));
                g.fill(x + 6, fy - 9, cardW - 12, 19);
            }
            g.color(fsel ? t.fg : t.dim);
            g.text(x + 10, fy, font::kTiny, r.name(p), Canvas::kCenterY);
            r.format(p, val, sizeof val);
            fitText(buf, sizeof buf, font::kTiny, val, cardW / 2 + 4);
            g.color(fsel ? kWhite : t.fg);
            g.text(x + cardW - 10, fy, font::kTiny, buf, Canvas::kCenterY | Canvas::kRight);
        }
        const int nr = k.mod.routesFrom(s, nullptr, 0);
        g.color(nr ? sc : t.dim);
        g.textf(x + cardW / 2, y + 196, font::kTiny, Canvas::kCenter, nr == 1 ? "1 route" : "%d routes", nr);
    }
    // routes
    const int ry = 256, rowH = 20, vis = 7;
    g.color(t.fg);
    g.textf(14, ry, font::kSmall, Canvas::kCenterY, "ROUTES  %d / %d", k.mod.routeCount(), kMaxRoutes);
    g.color(t.dim);
    g.text(786, ry, font::kTiny, "hold LFO / RND / macro pad + tap a param on the PARAM pads to add",
           Canvas::kCenterY | Canvas::kRight);
    const int first = modRow_ >= kLfos + kRands ? clampi(modRow_ - kLfos - kRands - vis + 1, 0, 1000) : 0;
    for (int i = 0; i < vis; ++i) {
        const int ri = first + i;
        if (ri >= k.mod.routeCount()) break;
        const Route& rt = k.mod.routeAt(ri);
        const int y = ry + 16 + i * rowH;
        const bool sel = modRow_ == kLfos + kRands + ri;
        if (sel) {
            g.color(scale(t.accent, 0.2f));
            g.fill(10, y, 780, rowH - 2);
        }
        g.color(k.mod.sourceColor(rt.src));
        g.text(20, y + 9, font::kTiny, k.mod.sourceName(rt.src), Canvas::kCenterY);
        paramLabel(rt.param, buf + 4, int(sizeof buf) - 4, true);
        std::memcpy(buf, "->  ", 4);
        g.color(sel ? t.fg : t.dim);
        g.text(90, y + 9, font::kTiny, buf, Canvas::kCenterY);
        const int bx = 480, bw = 160;
        g.color(scale(t.dim, 0.4f));
        g.fill(bx, y + 6, bw, 6);
        g.color(r.color(rt.param));
        if (rt.window) g.fill(bx + int(float(bw) * rt.lo), y + 6, int(float(bw) * fabsf(rt.hi - rt.lo)) + 1, 6);
        else {
            const int c = bx + bw / 2, e = c + int(float(bw / 2) * rt.depth);
            g.fill(c < e ? c : e, y + 6, (c < e ? e - c : c - e) + 1, 6);
        }
        g.color(t.fg);
        if (rt.window) g.textf(780, y + 9, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d..%d%%", int(rt.lo * 100.0f), int(rt.hi * 100.0f));
        else g.textf(780, y + 9, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%+d%%", int(rt.depth * 100.0f + (rt.depth < 0 ? -0.5f : 0.5f)));
    }
    const int below = k.mod.routeCount() - first - vis;
    if (first > 0 || below > 0) {
        g.color(t.dim);
        g.textf(786, ry + 16 + vis * rowH + 6, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d above, %d below",
                first, below > 0 ? below : 0);
    }
    if (!k.mod.routeCount()) {
        g.color(t.dim);
        g.text(400, ry + 70, font::kSmall, "No routes. On the PARAM pads (I.Param) hold a source pad", Canvas::kCenter);
        g.text(400, ry + 92, font::kSmall, "in the bottom row and tap a parameter; the encoder sets the depth.", Canvas::kCenter);
    }
    const char* keys[8] = {"", "", "", "", "", "", "", "Close"};
    drawFKeyBar(g, t, keys);
    g.color(t.dim);
    g.text(400, 436, font::kTiny, "Up/Down: source or route  |  Left/Right: field  |  encoder: value / depth  |  Delete: remove route",
           Canvas::kCenter);
}

void Nav::drawHelp(Canvas& g) {
    const Kit& k = *kit_;
    const Theme& t = theme();
    drawHeader(g, t, *k.shell, "HELP");
    const int page = layerPage_;
    const PageInfo* pi = (page >= 0 && page < wsPages_) ? &pages_[page] : nullptr;
    g.color(pageColorOf(*this, pages_, wsPages_, page, t));
    g.text(20, 54, font::kTitle, pageName(page), Canvas::kCenterY);
    g.color(t.fg);
    g.text(20, 86, font::kSmall, (pi && pi->help && *pi->help) ? pi->help : "No help text for this page.", Canvas::kCenterY);
    for (int f = 0; f < 8; ++f) {
        const char* s = pi ? pi->fkeys[f] : nullptr;
        const int x = f < 4 ? 20 : 410, y = 116 + (f % 4) * 22;
        g.color(s && *s ? t.accent : scale(t.dim, 0.6f));
        g.textf(x, y, font::kSmall, Canvas::kCenterY, "F%d", f + 1);
        g.color(s && *s ? t.fg : scale(t.dim, 0.6f));
        g.text(x + 34, y, font::kSmall, s && *s ? s : "-", Canvas::kCenterY);
    }
    g.color(t.accent);
    g.fill(20, 212, 760, 1);
    g.text(20, 228, font::kSmall, "LABKIT keys (every lab firmware)", Canvas::kCenterY);
    char kp[24], kps[32], km[24], kms[32];
    copyStr(kp, sizeof kp, keys_.param != kNoKey ? keyName(keys_.param) : "-");
    copyStr(km, sizeof km, keys_.map != kNoKey ? keyName(keys_.map) : "-");
    std::snprintf(kps, sizeof kps, "Shift + %s", kp);
    std::snprintf(kms, sizeof kms, "Shift + %s", km);
    const char* rows[][2] = {
        {kp, "tap: PARAM pads on/off   hold: PARAM pads while held"},
        {kps, "MACRO page: 8 macros, their targets, names"},
        {km, "hold: MAP of all pages (release = go there)   tap: MAP stays open"},
        {kms, "MOD page: 4 LFOs, 2 random sources, all routes"},
        {"PARAM pads", "tap = focus, hold + encoder = edit, Delete = default, Shift+Delete = no mod"},
        {"route", "hold an LFO / RND / macro pad (bottom row) + tap params; encoder = depth"},
        {"MIDI learn", "Shift + hold a param pad, move a CC; Delete while held forgets it"},
        {"Shift + Config", "this help        Config: settings        Play: transport"},
    };
    for (int i = 0; i < 8; ++i) {
        const int y = 252 + i * 22;
        g.color(t.accent);
        g.text(20, y, font::kSmall, rows[i][0], Canvas::kCenterY);
        g.color(t.fg);
        g.text(180, y, font::kSmall, rows[i][1], Canvas::kCenterY);
    }
    const char* keys[8] = {"", "", "", "", "", "", "", "Close"};
    drawFKeyBar(g, t, keys);
}

void Nav::drawHud(Canvas& g) {
    const Kit& k = *kit_;
    if (!k.hudOn || hudParam_ == kNoParam || int32_t(hudUntil_ - now_) <= 0 || !k.reg.valid(hudParam_)) return;
    const Theme& t = theme();
    const Registry& r = k.reg;
    char buf[64], name[48], val[32];
    paramLabel(hudParam_, buf, int(sizeof buf), true);
    fitText(name, sizeof name, font::kSmall, buf, 210);
    r.format(hudParam_, val, sizeof val);
    const int x = 210, y = 36, w = 380, h = 34;
    g.color(t.panel);
    g.fillRound(x, y, w, h, 7);
    g.color(r.color(hudParam_));
    g.fill(x + 6, y + 6, 4, h - 12);
    g.color(t.fg);
    g.text(x + 18, y + h / 2, font::kSmall, name, Canvas::kCenterY);
    g.color(t.accent);
    const bool modded = r.meta(hudParam_).mods && fabsf(r.effectiveNorm(hudParam_) - r.norm(hudParam_)) > 0.004f;
    g.text(x + w - 12, y + h / 2 - (modded ? 5 : 0), font::kBody, val, Canvas::kCenterY | Canvas::kRight);
    if (modded) {
        char eff[32];
        r.format(hudParam_, eff, sizeof eff, true);
        std::snprintf(buf, sizeof buf, "mod: %s", eff);
        g.color(t.dim);
        g.text(x + w - 12, y + h - 7, font::kTiny, buf, Canvas::kCenterY | Canvas::kRight);
    }
}

void Nav::draw(Canvas& g) {
    switch (ov_) {
        case kParam: drawLayer(g); break;
        case kMap: drawMap(g); break;
        case kMacro: drawMacro(g); break;
        case kMod: drawMod(g); break;
        case kHelp: drawHelp(g); break;
        default: drawHud(g); break;
    }
}

// ---- Nav: LEDs ----------------------------------------------------------------------------------------------
void Nav::layerLeds(Rgb* pads, bool inMap) {
    const Kit& k = *kit_;
    const Registry& r = k.reg;
    const int heldSrc = (!inMap && heldSourcePad_ >= 0) ? padSource(heldSourcePad_) : -1;
    const bool blink = (now_ / 250) & 1;
    for (int i = 0; i < kPadParams; ++i) {
        const ParamId p = layout_[i];
        const int pad = inMap ? i + kPadCols : i;
        if (!r.valid(p)) {
            pads[pad] = Rgb();
            continue;
        }
        Rgb c = scale(r.color(p), 0.12f + 0.88f * r.effectiveNorm(p));
        if (heldSrc >= 0 && k.mod.findRoute(heldSrc, p) >= 0) c = blink ? k.mod.sourceColor(heldSrc) : scale(k.mod.sourceColor(heldSrc), 0.3f);
        if (p == focus_) c = kWhite;
        pads[pad] = c;
    }
    if (inMap) {
        for (int c = 0; c < kPadCols; ++c) {
            if (c >= pageCount_) {
                pads[c] = Rgb();
                continue;
            }
            const Rgb pc = pageColorOf(*this, pages_, wsPages_, pageAt(c), theme());
            pads[c] = c == mapCol_ ? kWhite : scale(pc, 0.35f);
        }
        return;
    }
    for (int c = 0; c < kPadCols; ++c) {
        const int pad = 3 * kPadCols + c;
        const int s = padSource(pad);
        if (s < 0) {
            pads[pad] = Rgb();
            continue;
        }
        const float v = k.mod.sourceValue(s);
        const float u = k.mod.sourceBipolar(s) ? 0.5f * (v + 1.0f) : v;
        pads[pad] = heldSourcePad_ == pad ? kWhite : scale(k.mod.sourceColor(s), 0.1f + 0.9f * u);
    }
}

bool Nav::leds(Rgb* pads) {
    const Kit& k = *kit_;
    switch (ov_) {
        case kParam: layerLeds(pads, false); return true;
        case kMap: layerLeds(pads, true); return true;
        case kMacro: {
            const Rgb amber(0xFFB020);
            for (int i = 0; i < kPads; ++i) pads[i] = Rgb();
            for (int m = 0; m < kMacros; ++m) {
                const float v = k.mod.macro(m);
                for (int row = 0; row < 4; ++row) {
                    const float level = float(3 - row) / 3.0f;
                    const bool lit = v + 0.01f >= level;
                    pads[padAt(m, row)] = lit ? (m == macroSel_ ? amber : scale(amber, 0.45f)) : scale(amber, 0.05f);
                }
            }
            int targets[kMacroTargets];
            const int nt = k.mod.routesFrom(kSrcMacro + macroSel_, targets, kMacroTargets);
            for (int t = 0; t < nt; ++t) {
                const int pad = padAt(kMacros + (t % 4), t / 4);
                pads[pad] = macroRow_ == t + 1 ? kWhite : scale(k.reg.color(k.mod.routeAt(targets[t]).param), 0.6f);
            }
            return true;
        }
        case kMod: {
            for (int i = 0; i < kPads; ++i) pads[i] = Rgb();
            for (int i = 0; i < k.mod.routeCount() && i < 36; ++i)
                pads[i] = modRow_ == kLfos + kRands + i ? kWhite : scale(k.mod.sourceColor(k.mod.routeAt(i).src), 0.5f);
            for (int s = 0; s < kLfos + kRands; ++s) {
                const float u = 0.5f * (k.mod.sourceValue(s) + 1.0f);
                pads[3 * kPadCols + s] = modRow_ == s ? kWhite : scale(k.mod.sourceColor(s), 0.1f + 0.9f * u);
            }
            return true;
        }
        case kHelp:
            for (int i = 0; i < kPads; ++i) pads[i] = scale(theme().accent, 0.06f);
            return true;
        default: return false;
    }
}

}  // namespace kit
}  // namespace lab
