// LABKIT v2 -- the shared sample / file browser (lab::kit::Browser).
//
// A full-screen Shell modal: folders and files with their size, WAV length /
// channels / rate, a waveform thumbnail of the whole file, auto-preview (the
// Shell mixes a preview voice into the output; the first seconds are read in
// main-loop chunks), favourites and recent files (kept per firmware in
// /Lab/<FW>/labkit.bin), sort by name / size / type, jump by first letter.
//
//   init():   ctx.browser().reserve();              // listing + preview buffer
//   later:    kit::BrowserRequest q;
//             q.title = "Load sample"; q.folder = "Samples"; q.exts = ".wav";
//             q.onPick = [](void* c, const char* path) { ... }; q.ctx = this;
//             ctx.browser().open(q);
//
// Pads (no Shift): rows 1-3 = letters A..Z and digits 0..9 (jump), row 4 =
// up, down, page up, page down, parent, open, play, fav, sort, view, cancel,
// load. Shift + pad = audition the selected file at that pad's pitch.
// Main loop only; the card is never read from the audio interrupt.
#pragma once

#include "lab/lab.h"

namespace lab {
namespace kit {

struct BrowserRequest {
    const char* title = "Browse";
    const char* folder = "";       // start folder (card root = ""); the last one is remembered
    const char* exts = ".wav";     // ';'-separated suffixes, "" = every file
    void (*onPick)(void* ctx, const char* path) = nullptr;   // path from the card root
    void (*onCancel)(void* ctx) = nullptr;
    void* ctx = nullptr;
    bool preview = true;           // auto-preview WAV files
};

class Browser {
public:
    static constexpr int kNameLen = 48, kPathLen = 128, kFavs = 24, kRecent = 12, kThumb = 192;
    // In init(): the listing (maxEntries) and the preview buffer (seconds of
    // 44.1 kHz stereo int16, 0 = no preview). false = the arena is full.
    bool reserve(int maxEntries = 256, float previewSeconds = 4.0f);
    bool reserved() const { return entries_ != nullptr; }
    bool open(const BrowserRequest& req);
    bool isOpen() const { return open_; }
    void close(bool picked);

    // Shell hooks
    void attach(Kit* kit, Arena* arena) { kit_ = kit; arena_ = arena; }
    bool event(const Event& e);
    void update(uint32_t now);
    void draw(Canvas& g);
    void leds(Rgb* pads);
    void mix(float* l, float* r);   // audio interrupt: the preview voice

    // Tests and the simulator.
    int entryCount() const { return count_; }
    const char* entryName(int i) const { return (i >= 0 && i < count_) ? entries_[i].name : ""; }
    bool entryIsDir(int i) const { return i >= 0 && i < count_ && entries_[i].isDir; }
    int selected() const { return sel_; }
    const char* folder() const { return folder_; }
    void select(int i);
    void enterOrPick();
    void parent();
    void jumpLetter(char c);
    void setSort(int mode);           // 0 name, 1 size, 2 type
    void setView(int view);           // 0 folder, 1 favourites, 2 recent
    void toggleFavourite();
    bool isFavourite(const char* path) const;
    void addRecent(const char* path);
    bool previewLoaded() const { return pvFrames_ > 0; }
    uint32_t previewFrames() const { return pvFrames_; }
    bool wavValid() const { return wavOk_; }
    uint32_t wavFrames() const { return wavFrames_; }
    void audition(int semitones);
    void stopPreview();
    // prefs (labkit.bin)
    uint32_t prefsSize() const;
    void savePrefs(uint8_t* buf) const;
    void loadPrefs(const uint8_t* buf, uint32_t len);

private:
    struct Entry {
        char name[kNameLen];
        uint32_t size;
        bool isDir;
        int16_t ref;     // favourites / recent view: index into favs_ / recent_ (-1 = folder entry)
    };
    void list();
    void sort();
    void selectionChanged();
    void startLoad();
    void loadChunk();
    void thumbStep();
    void fullPath(int i, char* out, int cap) const;
    bool matches(const char* name) const;
    const Theme& theme() const;

    Kit* kit_ = nullptr;
    Arena* arena_ = nullptr;
    BrowserRequest req_;
    bool open_ = false;
    Entry* entries_ = nullptr;
    int cap_ = 0, count_ = 0, sel_ = 0, top_ = 0;
    int sortMode_ = 0, view_ = 0;
    char folder_[kPathLen] = {};
    char lastFolder_[kPathLen] = {};
    char exts_[48] = {};
    char favs_[kFavs][kPathLen] = {};
    char recent_[kRecent][kPathLen] = {};
    // selection info
    bool wavOk_ = false;
    uint16_t wavCh_ = 0, wavBits_ = 0;
    uint32_t wavRate_ = 0, wavFrames_ = 0, wavDataOff_ = 0, wavBpf_ = 0, wavFormat_ = 0;
    float thumb_[kThumb] = {};
    int thumbDone_ = 0;
    uint32_t selChangedMs_ = 0;
    bool loadPending_ = false;
    // preview: interleaved stereo int16 at the file's rate
    int16_t* pv_ = nullptr;
    uint32_t pvCap_ = 0;                 // frames
    volatile uint32_t pvFrames_ = 0;     // loaded so far (main writes, audio reads)
    IStorage::Handle pvFile_ = -1;
    uint32_t pvLeft_ = 0;                // source frames still to read
    volatile bool pvPlay_ = false;
    volatile float pvRate_ = 1.0f;
    float pvPos_ = 0.0f, pvGain_ = 0.0f;
    bool autoPreview_ = true;
    uint32_t now_ = 0;
    char status_[48] = {};
};

}  // namespace kit
}  // namespace lab
