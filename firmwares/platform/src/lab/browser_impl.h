// LABKIT v2 -- Browser implementation (compiled inside shell.cpp).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "formats/wav.h"
#include "lab/lab.h"

namespace lab {
namespace kit {

namespace {

char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
int cmpCi(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = lowerc(*a), y = lowerc(*b);
        if (x != y || !x) return int(uint8_t(x)) - int(uint8_t(y));
    }
}
const char* extOf(const char* name) {
    const char* dot = std::strrchr(name, '.');
    return dot ? dot : "";
}
const char* baseName(const char* path) {
    const char* s = std::strrchr(path, '/');
    return s ? s + 1 : path;
}
// Pads rows 1-3: A..Z then 0..9.
char padLetter(int pad) {
    if (pad < 26) return char('A' + pad);
    if (pad < 36) return char('0' + (pad - 26));
    return 0;
}
const char* const kViewNames[3] = {"Folder", "Favourites", "Recent"};
const char* const kSortNames[3] = {"Name", "Size", "Type"};
enum PadFn { kPUp, kPDown, kPPgUp, kPPgDn, kPParent, kPOpen, kPPlay, kPFav, kPSort, kPView, kPCancel, kPLoad };
const Rgb kFnColor[12] = {Rgb(0x3070FF), Rgb(0x3070FF), Rgb(0x2050B0), Rgb(0x2050B0), Rgb(0xA0A0A0), Rgb(0x30FF70),
                          Rgb(0x30FFE0), Rgb(0xFFD020), Rgb(0xB07CFF), Rgb(0xFF8A3D), Rgb(0xFF3030), Rgb(0x30FF70)};

}  // namespace

const Theme& Browser::theme() const { return kit_->theme(); }

bool Browser::reserve(int maxEntries, float previewSeconds) {
    if (entries_ || !arena_) return entries_ != nullptr;
    if (maxEntries < 16) maxEntries = 16;
    entries_ = arena_->make<Entry>(uint32_t(maxEntries));
    if (!entries_) return false;
    cap_ = maxEntries;
    if (previewSeconds > 0.0f) {
        pvCap_ = uint32_t(previewSeconds * float(kSampleRate));
        pv_ = static_cast<int16_t*>(arena_->alloc(pvCap_ * 4u));
        if (!pv_) pvCap_ = 0;
    }
    return true;
}

bool Browser::matches(const char* name) const {
    if (!exts_[0]) return true;
    const char* p = exts_;
    while (*p) {
        char one[12];
        int n = 0;
        while (*p && *p != ';' && n < 11) one[n++] = *p++;
        one[n] = 0;
        if (*p == ';') ++p;
        if (n && endsWithCi(name, one)) return true;
    }
    return false;
}

bool Browser::open(const BrowserRequest& req) {
    if (!kit_) return false;
    if (!entries_) {
        if (kit_->ctx) kit_->ctx->toast("Browser: call browser().reserve() in init()");
        return false;
    }
    if (!kit_->ctx || !kit_->ctx->fs) {
        if (kit_->ctx) kit_->ctx->toast("No card");
        return false;
    }
    req_ = req;
    copyStr(exts_, sizeof exts_, req.exts ? req.exts : "");
    const char* start = req.folder ? req.folder : "";
    // the last folder, when it lies inside the requested one
    if (lastFolder_[0] && std::strncmp(lastFolder_, start, std::strlen(start)) == 0) start = lastFolder_;
    copyStr(folder_, sizeof folder_, start);
    if (!kit_->ctx->fs->exists(folder_[0] ? folder_ : "/")) folder_[0] = 0;
    view_ = 0;
    open_ = true;
    status_[0] = 0;
    list();
    sel_ = count_ > 1 && entries_[0].isDir && std::strcmp(entries_[0].name, "..") == 0 ? 1 : 0;
    // land on the first file
    for (int i = 0; i < count_; ++i)
        if (!entries_[i].isDir) {
            sel_ = i;
            break;
        }
    top_ = 0;
    selectionChanged();
    return true;
}

void Browser::close(bool picked) {
    stopPreview();
    if (pvFile_ >= 0 && kit_->ctx && kit_->ctx->fs) kit_->ctx->fs->close(pvFile_);
    pvFile_ = -1;
    open_ = false;
    copyStr(lastFolder_, sizeof lastFolder_, folder_);
    kit_->prefsDirty = true;
    if (!picked && req_.onCancel) req_.onCancel(req_.ctx);
}

void Browser::list() {
    count_ = 0;
    IStorage* fs = kit_->ctx ? kit_->ctx->fs : nullptr;
    if (view_ == 0) {
        if (folder_[0]) {
            Entry& e = entries_[count_++];
            copyStr(e.name, sizeof e.name, "..");
            e.size = 0;
            e.isDir = true;
            e.ref = -1;
        }
        if (fs) {
            struct Ctx {
                Browser* b;
            } c{this};
            fs->listDir(
                folder_[0] ? folder_ : "/",
                [](const IStorage::DirEntry& de, void* p) {
                    Browser* b = static_cast<Ctx*>(p)->b;
                    if (!de.name || de.name[0] == '.') return true;
                    if (!de.isDir && !b->matches(de.name)) return true;
                    if (std::strlen(de.name) >= sizeof(Entry::name)) return true;   // too long to open again
                    if (b->count_ >= b->cap_) return false;
                    Entry& e = b->entries_[b->count_++];
                    copyStr(e.name, sizeof e.name, de.name);
                    e.size = de.size;
                    e.isDir = de.isDir;
                    e.ref = -1;
                    return true;
                },
                &c);
        }
        sort();
    } else {
        char (*src)[kPathLen] = view_ == 1 ? favs_ : recent_;
        const int n = view_ == 1 ? kFavs : kRecent;
        for (int i = 0; i < n && count_ < cap_; ++i) {
            if (!src[i][0]) continue;
            Entry& e = entries_[count_++];
            copyStr(e.name, sizeof e.name, baseName(src[i]));
            e.size = 0;
            e.isDir = false;
            e.ref = int16_t(i);
        }
    }
    if (sel_ >= count_) sel_ = count_ ? count_ - 1 : 0;
}

void Browser::sort() {
    const int first = (count_ && std::strcmp(entries_[0].name, "..") == 0) ? 1 : 0;
    auto less = [this](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        if (sortMode_ == 1 && a.size != b.size) return a.size > b.size;
        if (sortMode_ == 2) {
            const int c = cmpCi(extOf(a.name), extOf(b.name));
            if (c) return c < 0;
        }
        return cmpCi(a.name, b.name) < 0;
    };
    for (int i = first + 1; i < count_; ++i) {   // insertion sort: a few hundred entries
        Entry v = entries_[i];
        int j = i - 1;
        while (j >= first && less(v, entries_[j])) {
            entries_[j + 1] = entries_[j];
            --j;
        }
        entries_[j + 1] = v;
    }
}

void Browser::fullPath(int i, char* out, int cap) const {
    out[0] = 0;
    if (i < 0 || i >= count_) return;
    const Entry& e = entries_[i];
    if (e.ref >= 0) {
        copyStr(out, uint32_t(cap), view_ == 1 ? favs_[e.ref] : recent_[e.ref]);
        return;
    }
    if (folder_[0]) std::snprintf(out, size_t(cap), "%s/%s", folder_, e.name);
    else copyStr(out, uint32_t(cap), e.name);
}

void Browser::select(int i) {
    if (!count_) return;
    i = i < 0 ? 0 : (i >= count_ ? count_ - 1 : i);
    if (i == sel_) return;
    sel_ = i;
    selectionChanged();
}

void Browser::selectionChanged() {
    stopPreview();
    if (pvFile_ >= 0 && kit_->ctx->fs) kit_->ctx->fs->close(pvFile_);
    pvFile_ = -1;
    pvFrames_ = 0;
    wavOk_ = false;
    thumbDone_ = 0;
    loadPending_ = false;
    for (float& v : thumb_) v = 0.0f;
    if (sel_ < 0 || sel_ >= count_ || entries_[sel_].isDir) return;
    IStorage* fs = kit_->ctx->fs;
    char path[kPathLen + kNameLen];
    fullPath(sel_, path, sizeof path);
    if (!endsWithCi(path, ".wav") || !fs) return;
    const IStorage::Handle h = fs->open(path, IStorage::kRead);
    if (h < 0) return;
    if (entries_[sel_].ref >= 0) entries_[sel_].size = uint32_t(fs->size(h) > 0 ? fs->size(h) : 0);
    ot::StorageReader rd(*fs, h);
    ot::WavInfo info;
    if (ot::wavParseHeader(rd, info) && ot::wavIsSupported(info)) {
        wavOk_ = true;
        wavCh_ = info.channels;
        wavBits_ = info.bitsPerSample;
        wavRate_ = info.sampleRate;
        wavFormat_ = info.format;
        wavBpf_ = ot::wavBytesPerFrame(info);
        wavDataOff_ = info.dataOffset;
        wavFrames_ = ot::wavSourceFrames(info);
        const int32_t fsize = fs->size(h);
        if (fsize > 0 && wavBpf_ && wavDataOff_ + wavFrames_ * wavBpf_ > uint32_t(fsize))
            wavFrames_ = (uint32_t(fsize) - wavDataOff_) / wavBpf_;   // a cut file: what is there
    }
    fs->close(h);
    selChangedMs_ = now_;
    loadPending_ = wavOk_ && pv_ && (req_.preview || true);
}

void Browser::startLoad() {
    loadPending_ = false;
    IStorage* fs = kit_->ctx->fs;
    if (!fs || !wavOk_ || !pv_) return;
    char path[kPathLen + kNameLen];
    fullPath(sel_, path, sizeof path);
    pvFile_ = fs->open(path, IStorage::kRead);
    if (pvFile_ < 0) return;
    fs->seek(pvFile_, wavDataOff_);
    pvLeft_ = wavFrames_ < pvCap_ ? wavFrames_ : pvCap_;
    pvFrames_ = 0;
    pvPos_ = 0.0f;
    pvGain_ = 0.0f;
    pvRate_ = float(wavRate_) / float(kSampleRate);
    pvPlay_ = autoPreview_ && req_.preview;
}

void Browser::loadChunk() {
    IStorage* fs = kit_->ctx->fs;
    if (pvFile_ < 0 || !fs) return;
    uint8_t raw[4096];
    const uint32_t frames = pvLeft_ < sizeof raw / wavBpf_ ? pvLeft_ : uint32_t(sizeof raw / wavBpf_);
    const int32_t got = frames ? fs->read(pvFile_, raw, frames * wavBpf_) : 0;
    const uint32_t n = got > 0 ? uint32_t(got) / wavBpf_ : 0;
    const uint32_t bps = wavBits_ / 8u;
    int16_t* d = pv_ + pvFrames_ * 2;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* p = raw + i * wavBpf_;
        const int16_t a = ot::wavDecodeSample(p, uint16_t(wavFormat_), wavBits_);
        const int16_t b = wavCh_ > 1 ? ot::wavDecodeSample(p + bps, uint16_t(wavFormat_), wavBits_) : a;
        d[2 * i] = a;
        d[2 * i + 1] = b;
    }
    __atomic_store_n(&pvFrames_, pvFrames_ + n, __ATOMIC_RELEASE);
    pvLeft_ -= n;
    if (!n || !pvLeft_) {
        fs->close(pvFile_);
        pvFile_ = -1;
    }
}

void Browser::thumbStep() {
    if (!wavOk_ || thumbDone_ >= kThumb || !wavFrames_) return;
    IStorage* fs = kit_->ctx->fs;
    const uint32_t span = wavFrames_ / kThumb ? wavFrames_ / kThumb : 1;
    IStorage::Handle h = -1;
    for (int k = 0; k < 16 && thumbDone_ < kThumb; ++k, ++thumbDone_) {
        const uint32_t f0 = uint32_t(uint64_t(thumbDone_) * wavFrames_ / kThumb);
        float peak = 0.0f;
        if (f0 + span <= pvFrames_) {   // in the preview buffer: the whole column
            const uint32_t step = span > 256 ? span / 256 : 1;
            for (uint32_t f = f0; f < f0 + span; f += step) {
                const float v = float(abs(pv_[2 * f]) > abs(pv_[2 * f + 1]) ? abs(pv_[2 * f]) : abs(pv_[2 * f + 1]));
                if (v > peak) peak = v;
            }
        } else if (fs) {   // beyond it: 64 frames from the card
            if (h < 0) {
                char path[kPathLen + kNameLen];
                fullPath(sel_, path, sizeof path);
                h = fs->open(path, IStorage::kRead);
                if (h < 0) return;
            }
            uint8_t raw[512];
            uint32_t n = span < 64 ? span : 64;
            if (n * wavBpf_ > sizeof raw) n = sizeof raw / wavBpf_;
            fs->seek(h, wavDataOff_ + f0 * wavBpf_);
            const int32_t got = fs->read(h, raw, n * wavBpf_);
            for (int32_t i = 0; got > 0 && i < got / int32_t(wavBpf_); ++i) {
                const float v = fabsf(float(ot::wavDecodeSample(raw + i * wavBpf_, uint16_t(wavFormat_), wavBits_)));
                if (v > peak) peak = v;
            }
        }
        thumb_[thumbDone_] = peak * (1.0f / 32768.0f);
    }
    if (h >= 0) fs->close(h);
}

void Browser::update(uint32_t now) {
    now_ = now;
    if (!open_) return;
    if (loadPending_ && now - selChangedMs_ >= 120) startLoad();
    if (pvFile_ >= 0) loadChunk();
    else if (wavOk_ && thumbDone_ < kThumb && !loadPending_) thumbStep();
}

void Browser::stopPreview() {
    pvPlay_ = false;
}

void Browser::audition(int semitones) {
    if (!pvFrames_) return;
    pvPlay_ = false;
    pvPos_ = 0.0f;
    pvGain_ = 0.0f;
    pvRate_ = float(wavRate_) / float(kSampleRate) * exp2f(float(semitones) * (1.0f / 12.0f));
    pvPlay_ = true;
}

LABKIT_HOT void Browser::mix(float* l, float* r) {
    if (!pvPlay_ || !pv_) return;
    const uint32_t n = __atomic_load_n(&pvFrames_, __ATOMIC_ACQUIRE);
    const bool loading = pvLeft_ > 0;
    float pos = pvPos_, g = pvGain_;
    const float rate = pvRate_;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const uint32_t idx = uint32_t(pos);
        if (idx + 1 >= n) {
            if (!loading) pvPlay_ = false;
            break;
        }
        const float fr = pos - float(idx);
        const int16_t* s = pv_ + idx * 2;
        const float a = float(s[0]) + (float(s[2]) - float(s[0])) * fr;
        const float b = float(s[1]) + (float(s[3]) - float(s[1])) * fr;
        g += (0.8f - g) * 0.01f;
        l[i] += a * (1.0f / 32768.0f) * g;
        r[i] += b * (1.0f / 32768.0f) * g;
        pos += rate;
    }
    pvPos_ = pos;
    pvGain_ = g;
}

void Browser::parent() {
    if (view_ != 0 || !folder_[0]) return;
    char* s = std::strrchr(folder_, '/');
    char prev[kNameLen];
    copyStr(prev, sizeof prev, s ? s + 1 : folder_);
    if (s) *s = 0;
    else folder_[0] = 0;
    list();
    sel_ = 0;
    for (int i = 0; i < count_; ++i)
        if (std::strcmp(entries_[i].name, prev) == 0) sel_ = i;
    top_ = 0;
    selectionChanged();
}

void Browser::enterOrPick() {
    if (sel_ < 0 || sel_ >= count_) return;
    const Entry& e = entries_[sel_];
    if (e.isDir) {
        if (std::strcmp(e.name, "..") == 0) {
            parent();
            return;
        }
        char next[kPathLen + kNameLen + 2];
        if (folder_[0]) std::snprintf(next, sizeof next, "%s/%s", folder_, e.name);
        else copyStr(next, sizeof next, e.name);
        if (std::strlen(next) >= sizeof folder_) {
            kit_->ctx->toast("Folder path too long");
            return;
        }
        copyStr(folder_, sizeof folder_, next);
        list();
        sel_ = count_ > 1 ? 1 : 0;
        top_ = 0;
        selectionChanged();
        return;
    }
    char path[kPathLen + kNameLen];
    fullPath(sel_, path, sizeof path);
    addRecent(path);
    auto cb = req_.onPick;
    void* ctx = req_.ctx;
    close(true);
    if (cb) cb(ctx, path);
}

void Browser::jumpLetter(char c) {
    for (int i = 0; i < count_; ++i) {
        const char f = entries_[i].name[0];
        if (lowerc(f) == lowerc(c)) {
            select(i);
            return;
        }
    }
}

void Browser::setSort(int mode) {
    sortMode_ = (mode % 3 + 3) % 3;
    if (view_ != 0) return;
    char cur[kNameLen] = {};
    if (sel_ < count_) copyStr(cur, sizeof cur, entries_[sel_].name);
    sort();
    for (int i = 0; i < count_; ++i)
        if (std::strcmp(entries_[i].name, cur) == 0) sel_ = i;
}

void Browser::setView(int view) {
    view_ = (view % 3 + 3) % 3;
    sel_ = 0;
    top_ = 0;
    list();
    selectionChanged();
}

bool Browser::isFavourite(const char* path) const {
    for (const auto& f : favs_)
        if (f[0] && std::strcmp(f, path) == 0) return true;
    return false;
}

void Browser::toggleFavourite() {
    if (sel_ < 0 || sel_ >= count_ || entries_[sel_].isDir) return;
    char path[kPathLen + kNameLen];
    fullPath(sel_, path, sizeof path);
    for (auto& f : favs_)
        if (f[0] && std::strcmp(f, path) == 0) {
            f[0] = 0;
            kit_->prefsDirty = true;
            if (view_ == 1) list();
            return;
        }
    for (auto& f : favs_)
        if (!f[0]) {
            copyStr(f, kPathLen, path);
            kit_->prefsDirty = true;
            return;
        }
    kit_->ctx->toast("Favourites full (24)");
}

void Browser::addRecent(const char* path) {
    int at = kRecent - 1;
    for (int i = 0; i < kRecent; ++i)
        if (std::strcmp(recent_[i], path) == 0) at = i;
    for (int i = at; i > 0; --i) copyStr(recent_[i], kPathLen, recent_[i - 1]);
    copyStr(recent_[0], kPathLen, path);
    kit_->prefsDirty = true;
}

uint32_t Browser::prefsSize() const { return sizeof favs_ + sizeof recent_ + sizeof lastFolder_ + 4; }
void Browser::savePrefs(uint8_t* buf) const {
    std::memcpy(buf, favs_, sizeof favs_);
    std::memcpy(buf + sizeof favs_, recent_, sizeof recent_);
    std::memcpy(buf + sizeof favs_ + sizeof recent_, lastFolder_, sizeof lastFolder_);
    const int32_t flags = (autoPreview_ ? 1 : 0) | (sortMode_ << 1);
    std::memcpy(buf + sizeof favs_ + sizeof recent_ + sizeof lastFolder_, &flags, 4);
}
void Browser::loadPrefs(const uint8_t* buf, uint32_t len) {
    if (len != prefsSize()) return;
    std::memcpy(favs_, buf, sizeof favs_);
    std::memcpy(recent_, buf + sizeof favs_, sizeof recent_);
    std::memcpy(lastFolder_, buf + sizeof favs_ + sizeof recent_, sizeof lastFolder_);
    int32_t flags;
    std::memcpy(&flags, buf + sizeof favs_ + sizeof recent_ + sizeof lastFolder_, 4);
    autoPreview_ = flags & 1;
    sortMode_ = (flags >> 1) % 3;
    for (auto& f : favs_) f[kPathLen - 1] = 0;
    for (auto& f : recent_) f[kPathLen - 1] = 0;
    lastFolder_[kPathLen - 1] = 0;
}

// ---- events ----------------------------------------------------------------------------------
bool Browser::event(const Event& e) {
    const int page = 15;
    switch (e.type) {
        case Event::kEncoder: select(sel_ + e.delta * (e.shift ? page : 1)); return true;
        case Event::kPadDown: {
            if (e.shift) {   // audition at the pad's pitch: row 3 = root, a row up = +12
                if (!pvFrames_ && wavOk_ && pvFile_ < 0) startLoad();
                audition(padCol(e.id) + (2 - padRow(e.id)) * 12);
                return true;
            }
            const int row = padRow(e.id);
            if (row < 3) {
                const char c = padLetter(e.id);
                if (c) jumpLetter(c);
                return true;
            }
            switch (padCol(e.id)) {
                case kPUp: select(sel_ - 1); break;
                case kPDown: select(sel_ + 1); break;
                case kPPgUp: select(sel_ - page); break;
                case kPPgDn: select(sel_ + page); break;
                case kPParent: parent(); break;
                case kPOpen: enterOrPick(); break;
                case kPPlay:
                    if (pvPlay_) stopPreview();
                    else if (pvFrames_) audition(0);
                    else if (wavOk_) startLoad(), pvPlay_ = true;
                    break;
                case kPFav: toggleFavourite(); break;
                case kPSort: setSort(sortMode_ + 1); break;
                case kPView: setView(view_ + 1); break;
                case kPCancel: close(false); break;
                case kPLoad: enterOrPick(); break;
                default: break;
            }
            return true;
        }
        case Event::kPress:
            switch (e.id) {
                case btn::Up: select(sel_ - (e.shift ? page : 1)); break;
                case btn::Down: select(sel_ + (e.shift ? page : 1)); break;
                case btn::Left: parent(); break;
                case btn::Right:
                case btn::Insert:
                case btn::F8: enterOrPick(); break;
                case btn::F1: setView(0); break;
                case btn::F2: setView(1); break;
                case btn::F3: setView(2); break;
                case btn::F4: setSort(sortMode_ + 1); break;
                case btn::F5: toggleFavourite(); break;
                case btn::F6:
                    autoPreview_ = !autoPreview_;
                    if (!autoPreview_) stopPreview();
                    kit_->prefsDirty = true;
                    break;
                case btn::F7: close(false); break;
                case btn::Delete:
                    if (view_ != 0 && sel_ < count_ && entries_[sel_].ref >= 0) {
                        (view_ == 1 ? favs_ : recent_)[entries_[sel_].ref][0] = 0;
                        kit_->prefsDirty = true;
                        list();
                        selectionChanged();
                    }
                    break;
                default: break;
            }
            return true;
        default: return true;
    }
}

// ---- drawing ------------------------------------------------------------------------------------
void Browser::draw(Canvas& g) {
    const Theme& t = theme();
    drawHeader(g, t, *kit_->shell, req_.title);
    char buf[160], nm[64];
    // path + view + sort
    if (view_ == 0) std::snprintf(buf, sizeof buf, "/%s", folder_);
    else copyStr(buf, sizeof buf, kViewNames[view_]);
    fitText(nm, sizeof nm, font::kSmall, buf, 440);
    g.color(t.accent);
    g.text(12, 46, font::kSmall, nm, Canvas::kCenterY);
    g.color(t.dim);
    g.textf(468, 46, font::kTiny, Canvas::kCenterY | Canvas::kRight, "%d  |  sort: %s", count_, kSortNames[sortMode_]);
    // the list
    const int ly = 60, rowH = 22, rows = 16;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
    g.color(scale(t.panel, 1.3f));
    g.fill(6, ly, 466, rows * rowH + 4);
    for (int i = 0; i < rows && top_ + i < count_; ++i) {
        const int idx = top_ + i;
        const Entry& e = entries_[idx];
        const int y = ly + 2 + i * rowH;
        if (idx == sel_) {
            g.color(scale(t.accent, 0.3f));
            g.fill(8, y, 462, rowH - 1);
            g.color(t.accent);
            g.fill(8, y, 3, rowH - 1);
        }
        if (e.isDir) {
            g.color(Rgb(0xFFD060));
            g.fill(18, y + 6, 12, 9);
        }
        char path[kPathLen + kNameLen];
        bool fav = false;
        if (!e.isDir) {
            fullPath(idx, path, sizeof path);
            fav = isFavourite(path);
        }
        fitText(nm, sizeof nm, font::kSmall, e.name, e.isDir ? 400 : 330);
        g.color(idx == sel_ ? t.fg : (e.isDir ? Rgb(0xFFD060) : t.fg));
        g.text(e.isDir ? 38 : 20, y + rowH / 2, font::kSmall, nm, Canvas::kCenterY);
        if (fav) {
            g.color(Rgb(0xFFD020));
            g.text(462, y + rowH / 2, font::kSmall, "*", Canvas::kCenterY | Canvas::kRight);
        }
        if (!e.isDir && e.size) {
            if (e.size >= 1048576) std::snprintf(buf, sizeof buf, "%.1f MB", double(e.size) / 1048576.0);
            else std::snprintf(buf, sizeof buf, "%u KB", unsigned((e.size + 1023) / 1024));
            g.color(t.dim);
            g.text(448, y + rowH / 2, font::kTiny, buf, Canvas::kCenterY | Canvas::kRight);
        }
    }
    if (!count_) {
        g.color(t.dim);
        g.text(238, ly + 60, font::kSmall, view_ == 0 ? "Nothing here" : "Empty", Canvas::kCenter);
    }
    if (count_ > rows) {   // scroll bar
        const int h = rows * rowH;
        g.color(t.dim);
        g.fill(470, ly + 2 + h * top_ / count_, 3, h * rows / count_ > 6 ? h * rows / count_ : 6);
    }
    // info panel
    const int ix = 486;
    if (sel_ < count_ && !entries_[sel_].isDir) {
        fitText(nm, sizeof nm, font::kBody, entries_[sel_].name, 300);
        g.color(t.fg);
        g.text(ix, 72, font::kBody, nm, Canvas::kCenterY);
        if (wavOk_) {
            const float secs = wavRate_ ? float(wavFrames_) / float(wavRate_) : 0.0f;
            g.color(t.dim);
            std::snprintf(buf, sizeof buf, "WAV  %.1f kHz  %s  %u-bit", double(wavRate_) / 1000.0, wavCh_ > 1 ? "stereo" : "mono",
                          unsigned(wavBits_));
            g.text(ix, 98, font::kTiny, buf, Canvas::kCenterY);
            std::snprintf(buf, sizeof buf, "%.2f s   %u frames", double(secs), unsigned(wavFrames_));
            g.text(ix, 116, font::kTiny, buf, Canvas::kCenterY);
            // thumbnail
            const int tx = ix, ty = 136, tw = kThumb + 106, th = 110;
            g.color(scale(t.panel, 1.5f));
            g.fill(tx, ty, tw, th);
            g.color(scale(t.accent, 0.8f));
            g.beginLines();
            const int mid = ty + th / 2;
            for (int c = 0; c < thumbDone_; ++c) {
                const float x = float(tx + 2) + float(c) * float(tw - 4) / float(kThumb);
                const float h = thumb_[c] * float(th / 2 - 3) + 0.5f;
                g.vtx(x, float(mid) - h);
                g.vtx(x, float(mid) + h + 1.0f);
            }
            g.endBatch();
            // preview position
            if (pvPlay_ && wavFrames_) {
                const float x = float(tx) + float(tw) * (pvPos_ / float(wavFrames_));
                g.color(Rgb(0xFFFFFF));
                g.fill(int(x), ty, 2, th);
            }
            if (pvCap_ && wavFrames_ > pvCap_) {   // the part the preview holds
                g.color(scale(t.dim, 0.7f));
                g.fill(tx, ty + th + 2, int(float(tw) * float(pvCap_) / float(wavFrames_)), 3);
            }
            g.color(t.dim);
            if (pvFile_ >= 0) g.textf(ix, ty + th + 16, font::kTiny, Canvas::kCenterY, "loading %u%%",
                                      unsigned(100u * pvFrames_ / (pvFrames_ + pvLeft_ + 1)));
            else if (pvFrames_) g.text(ix, ty + th + 16, font::kTiny, pvPlay_ ? "preview playing" : "preview ready", Canvas::kCenterY);
        } else {
            g.color(t.dim);
            g.textf(ix, 98, font::kTiny, Canvas::kCenterY, "%u bytes", unsigned(entries_[sel_].size));
        }
    } else if (sel_ < count_) {
        g.color(Rgb(0xFFD060));
        g.text(ix, 72, font::kBody, std::strcmp(entries_[sel_].name, "..") == 0 ? "Parent folder" : "Folder", Canvas::kCenterY);
        g.color(t.dim);
        g.text(ix, 98, font::kTiny, "Right / Insert / F8: open", Canvas::kCenterY);
    }
    // pad legend
    g.color(t.dim);
    g.text(ix, 300, font::kTiny, "PADS  rows 1-3: jump to A..Z, 0..9", Canvas::kCenterY);
    g.text(ix, 318, font::kTiny, "row 4: up dn pgup pgdn  back open", Canvas::kCenterY);
    g.text(ix, 336, font::kTiny, "       play fav sort view cancel load", Canvas::kCenterY);
    g.text(ix, 354, font::kTiny, "Shift + pad: audition at that pitch", Canvas::kCenterY);
    g.text(ix, 380, font::kTiny, "Encoder / Up Down: select   Left: back", Canvas::kCenterY);
    g.text(ix, 398, font::kTiny, "Delete: remove from favourites / recent", Canvas::kCenterY);
    const char* keys[8] = {"Folder", "Favs", "Recent", kSortNames[sortMode_], "Fav +/-", autoPreview_ ? "Auto on" : "Auto off",
                           "Cancel", "Load"};
    drawFKeyBar(g, t, keys, view_);
}

void Browser::leds(Rgb* pads) {
    const Theme& t = theme();
    if (kit_->shell && kit_->shell->shiftHeld()) {
        for (int i = 0; i < kPads; ++i) pads[i] = padCol(i) == 0 ? t.accent : scale(t.accent, 0.25f);
        return;
    }
    bool has[36] = {};
    for (int i = 0; i < count_; ++i) {
        const char c = lowerc(entries_[i].name[0]);
        if (c >= 'a' && c <= 'z') has[c - 'a'] = true;
        if (c >= '0' && c <= '9') has[26 + c - '0'] = true;
    }
    const char cur = sel_ < count_ ? lowerc(entries_[sel_].name[0]) : 0;
    for (int i = 0; i < 36; ++i) {
        const char c = lowerc(padLetter(i));
        pads[i] = c == cur ? Rgb(0xFFFFFF) : (has[i] ? scale(t.accent, 0.5f) : Rgb(0x040404));
    }
    for (int c = 0; c < kPadCols; ++c) {
        Rgb col = kFnColor[c];
        if (c == kPPlay && pvPlay_) col = Rgb(0xFFFFFF);
        pads[36 + c] = scale(col, 0.6f);
    }
}

}  // namespace kit
}  // namespace lab
