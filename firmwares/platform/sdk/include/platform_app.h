/* PLATFORM native app ABI (polyend-tracker-lab, firmwares/platform).
 *
 * A native app is C or C++ compiled for the Cortex-M4F (hard float) and loaded
 * from the SD card into the SDRAM by PLATFORM. It never links against the
 * firmware: everything it may call is in the PlatHost table it receives, and
 * everything PLATFORM calls is in the PlatApp table it returns from
 * plat_app_entry(). The same source builds as a host shared library (.so) for
 * the desktop simulator.
 *
 * The callbacks mirror lab::Workstation (src/lab/lab.h), so a workstation can
 * be wrapped as an app:
 *   init      main loop, once: allocate everything with host->alloc
 *   event     buttons / pads / encoder (main loop)
 *   midi      parsed MIDI in (main loop)
 *   update    main loop, every pass
 *   draw      ~40 frames a second (main loop), only with the host draw calls
 *   leds      48 pad colours, ~30 times a second (main loop)
 *   render    AUDIO INTERRUPT: add into l / r (kBlock = 128 frames, zeroed);
 *             no file I/O, no drawing, no host calls except stem(), never block
 *   transport play / stop changes (main loop)
 *   settings  rows for the Config > Settings page (optional)
 *   save      write state to the card (main loop)
 *   close     the app is about to be unloaded (main loop): stop, save
 * Any callback pointer may be null.
 *
 * All structs are plain C with fixed layout; never reorder fields. New fields
 * are appended and the `size` fields tell each side what the other has.
 */
#ifndef PLATFORM_APP_H
#define PLATFORM_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAT_ABI_VERSION 1u
#define PLAT_BLOCK 128u
#define PLAT_SAMPLE_RATE 44100u
#define PLAT_SCREEN_W 800
#define PLAT_SCREEN_H 480
#define PLAT_PADS 48

/* Event types: same values as lab::Event::Type. */
enum {
    PLAT_EV_NONE = 0,
    PLAT_EV_PRESS = 1,
    PLAT_EV_RELEASE = 2,
    PLAT_EV_HOLD = 3,
    PLAT_EV_PAD_DOWN = 4,
    PLAT_EV_PAD_UP = 5,
    PLAT_EV_ENCODER = 6
};

/* Button ids (same as ot::btn in src/core/ui/buttons.h). */
enum {
    PLAT_BTN_F1 = 0x00, PLAT_BTN_F2, PLAT_BTN_F3, PLAT_BTN_F4, PLAT_BTN_F5, PLAT_BTN_F6, PLAT_BTN_F7, PLAT_BTN_F8,
    PLAT_BTN_INSTRPARAMS = 0x08, PLAT_BTN_PERFORMANCE = 0x09, PLAT_BTN_FILE = 0x0a, PLAT_BTN_CONFIG = 0x0b,
    PLAT_BTN_MASTER = 0x0c, PLAT_BTN_SAMPLELOADER = 0x0d, PLAT_BTN_SAMPLEPLAYBACK = 0x0e,
    PLAT_BTN_SAMPLEEDITOR = 0x0f, PLAT_BTN_SAMPLERECORDER = 0x10, PLAT_BTN_SONG = 0x11,
    PLAT_BTN_NOTE = 0x12, PLAT_BTN_INSTR = 0x13, PLAT_BTN_FX1 = 0x14, PLAT_BTN_FX2 = 0x15,
    PLAT_BTN_PATTERN = 0x16, PLAT_BTN_PLAY = 0x17, PLAT_BTN_COPY = 0x18, PLAT_BTN_INSERT = 0x19,
    PLAT_BTN_UP = 0x1a, PLAT_BTN_DELETE = 0x1b, PLAT_BTN_REC = 0x1c, PLAT_BTN_SHIFT = 0x1d,
    PLAT_BTN_LEFT = 0x1e, PLAT_BTN_DOWN = 0x1f, PLAT_BTN_RIGHT = 0x20, PLAT_BTN_ENCPUSH = 0x21,
    PLAT_BTN_AUX = 0x22, PLAT_BTN_HELP = 0x2b
};

/* FT81x ROM fonts (lab::font): 26 tiny .. 31 huge. */
enum { PLAT_FONT_TINY = 26, PLAT_FONT_SMALL = 27, PLAT_FONT_BODY = 28, PLAT_FONT_LARGE = 29,
       PLAT_FONT_TITLE = 30, PLAT_FONT_HUGE = 31 };
/* Text options (FT81x OPT_*). */
enum { PLAT_TEXT_CENTERX = 512, PLAT_TEXT_CENTERY = 1024, PLAT_TEXT_CENTER = 1536, PLAT_TEXT_RIGHT = 2048 };
/* Batch kinds for beginBatch(). */
enum { PLAT_BATCH_LINES = 0, PLAT_BATCH_POINTS = 1, PLAT_BATCH_RECTS = 2, PLAT_BATCH_STRIP = 3 };
/* File open modes (ot::IStorage::OpenMode). */
enum { PLAT_FILE_READ = 1, PLAT_FILE_WRITE = 2, PLAT_FILE_READWRITE = 3 };

typedef struct PlatEvent {
    uint8_t type;      /* PLAT_EV_* */
    uint8_t id;        /* button id or pad 0..47 (row * 12 + col, row 0 at the top) */
    uint8_t velocity;  /* pads, 1..127 */
    int8_t delta;      /* encoder detents */
    uint8_t shift;     /* Shift held */
    uint8_t reserved[3];
    uint32_t ms;
} PlatEvent;

typedef struct PlatTransport {
    int32_t playing;
    float bpm;
    double beat;            /* quarter notes since Play, at the block start */
    double beatsPerSample;
    int32_t startedThisBlock;
    int32_t stoppedThisBlock;
} PlatTransport;

/* Same layout as lab::SettingRow. */
typedef struct PlatSetting {
    const char* label;
    int* value;
    int min, max;
    const char* const* names; /* max - min + 1 labels, or null = number */
    void (*onChange)(void* ctx, int v);
    void* ctx;
    const char* unit;
} PlatSetting;

typedef bool (*PlatDirFn)(const char* name, bool isDir, uint32_t size, void* user);

/* Services of the host. Every call takes `ctx` first. Drawing calls are valid
 * only inside draw() / drawIntro(); they go through PLATFORM's budgeted canvas
 * (1,800 display-list words a frame, minus a small reserve). */
typedef struct PlatHost {
    uint32_t abi;    /* PLAT_ABI_VERSION */
    uint32_t size;   /* sizeof(PlatHost) of the host */
    void* ctx;
    const char* home;    /* the app's data folder on the card, e.g. "Lab/PLATFORM/Data/drone" */
    const char* fwName;  /* "PLATFORM" */

    /* memory: bump allocation from the app's pool (init only; never freed
     * until the app closes). Returns zeroed memory or null. */
    void* (*alloc)(void* ctx, uint32_t bytes, uint32_t align);
    uint32_t (*memFree)(void* ctx);

    /* drawing (lab::Canvas) -- colours are 0xRRGGBB */
    void (*color)(void* ctx, uint32_t rgb);
    void (*alpha)(void* ctx, uint8_t a);
    void (*lineWidth)(void* ctx, float px);
    void (*pointSize)(void* ctx, float px);
    void (*clip)(void* ctx, int x, int y, int w, int h);
    void (*noClip)(void* ctx);
    void (*fill)(void* ctx, int x, int y, int w, int h);
    void (*fillRound)(void* ctx, int x, int y, int w, int h, float radius);
    void (*frame)(void* ctx, int x, int y, int w, int h);
    void (*line)(void* ctx, float x0, float y0, float x1, float y1);
    void (*point)(void* ctx, float x, float y, float radius);
    void (*ring)(void* ctx, float x, float y, float radius, int segments);
    void (*arc)(void* ctx, float x, float y, float radius, float a0, float a1, int segments);
    void (*beginBatch)(void* ctx, int kind);
    void (*vtx)(void* ctx, float x, float y);
    void (*endBatch)(void* ctx);
    void (*text)(void* ctx, int x, int y, uint8_t font, const char* s, uint16_t opt); /* ASCII 32..126 */
    void (*number)(void* ctx, int x, int y, uint8_t font, int32_t n, uint16_t opt);
    void (*bitmap332)(void* ctx, int x, int y, uint32_t ramg, int w, int h, int scale);
    /* RAM_G upload (main loop, outside draw): the app's own RAM_G band. */
    bool (*writeRamG)(void* ctx, uint32_t addr, const void* src, uint32_t len);
    uint32_t ramgBase, ramgSize;
    uint32_t (*canvasWords)(void* ctx);  /* words used so far in this frame */
    uint32_t canvasBudget;               /* the app's share of the frame */

    /* files: paths are relative to the card root ("Lab/PLATFORM/Data/x/y.bin") */
    int32_t (*fileOpen)(void* ctx, const char* path, uint8_t mode); /* handle, < 0 error */
    int32_t (*fileRead)(void* ctx, int32_t h, void* dst, uint32_t len);
    int32_t (*fileWrite)(void* ctx, int32_t h, const void* src, uint32_t len);
    bool (*fileSeek)(void* ctx, int32_t h, uint32_t pos);
    int32_t (*fileSize)(void* ctx, int32_t h);
    void (*fileClose)(void* ctx, int32_t h);
    bool (*fileExists)(void* ctx, const char* path);
    bool (*mkdir)(void* ctx, const char* path);
    bool (*fileRemove)(void* ctx, const char* path);
    bool (*fileRename)(void* ctx, const char* from, const char* to);
    int32_t (*listDir)(void* ctx, const char* path, PlatDirFn fn, void* user);
    bool hasCard;

    /* time, transport */
    uint32_t (*millis)(void* ctx);
    bool (*playing)(void* ctx);
    float (*bpm)(void* ctx);
    void (*setBpm)(void* ctx, float bpm);
    void (*setPlaying)(void* ctx, bool on);

    /* MIDI out (queued, any context) */
    void (*midiOut)(void* ctx, uint8_t status, uint8_t d1, uint8_t d2);
    uint8_t (*midiChannel)(void* ctx);

    /* ui */
    void (*toast)(void* ctx, const char* msg);
    void (*requestRedraw)(void* ctx);
    void (*log)(void* ctx, const char* msg);

    /* audio side: the recorder's stems (inside render only) */
    void (*stem)(void* ctx, int index, const float* l, const float* r);

    /* shell state */
    bool (*recording)(void* ctx);
    uint32_t (*cpuPermille)(void* ctx);
    int (*inputSource)(void* ctx); /* 0 line, 1 radio, 2 mic, 3 off */
    float (*radioMHz)(void* ctx);
    bool hasRadio;
    void (*radioSetBand)(void* ctx, float minMHz, float maxMHz, float stepMHz);
    bool (*radioTune)(void* ctx, float mhz);
    bool (*radioSeek)(void* ctx, bool up);
    bool (*radioBusy)(void* ctx);
    float (*radioFrequency)(void* ctx);

    /* Station apps (a lab::Workstation built as an app, sdk/station/): the
     * host's own objects, used through the lab.h / hal_api.h class layouts
     * (virtual calls and inline members only, never linked symbols).
     * labLayout must equal PLAT_LAB_LAYOUT computed by the app from the same
     * headers, or the app refuses to start. Null / 0 for plain apps' needs. */
    void* labShell;     /* lab::Shell* */
    void* labCanvas;    /* lab::Canvas*, valid inside draw() / drawIntro() */
    void* labStorage;   /* ot::IStorage* (null without a card) */
    void* labRadio;     /* ot::IRadio* (may be null) */
    uint32_t labLayout;
    void (*midiNote)(void* ctx, bool on, uint8_t note, uint8_t vel, uint8_t chOffset);
} PlatHost;

/* What the app gives PLATFORM. */
typedef struct PlatApp {
    uint32_t abi;    /* PLAT_ABI_VERSION */
    uint32_t size;   /* sizeof(PlatApp) of the app */
    const char* name;
    const char* tagline;
    uint32_t theme[8]; /* bg, panel, fg, dim, accent, accent2, warn, ok (0xRRGGBB) */
    uint32_t introMs;  /* 0 = no intro */
    void* self;

    bool (*init)(void* self, const PlatHost* host);
    void (*drawIntro)(void* self, uint32_t ms);
    void (*event)(void* self, const PlatEvent* e);
    void (*midi)(void* self, uint8_t status, uint8_t d1, uint8_t d2);
    void (*update)(void* self, uint32_t ms);
    void (*draw)(void* self);
    void (*leds)(void* self, uint8_t* rgb); /* 48 x {r, g, b} */
    void (*render)(void* self, const PlatTransport* t, const int16_t* in, float* l, float* r);
    void (*transport)(void* self, bool playing);
    int (*settings)(void* self, PlatSetting* rows, int cap);
    void (*save)(void* self);
    void (*close)(void* self);
} PlatApp;

/* Metadata the launcher reads without running the app. Put exactly one in each
 * app with PLAT_APP_INFO(...); tools/mkpapp.py copies it into the .papp header
 * and the simulator reads the symbol from the .so. */
typedef struct PlatInfo {
    char magic[8];   /* "PLATINF" */
    char name[24];
    char author[24];
    char desc[96];
    char icon[16];   /* launcher glyph: drone, seq, grid, wave, dots, euclid, arp, spark, station, app */
    uint32_t memKB;  /* memory the app needs from host->alloc; the loader refuses to start it with less */
    uint32_t flags;  /* PLAT_INFO_WORKSTATION: listed under Stations */
} PlatInfo;
#define PLAT_INFO_WORKSTATION 1u

#if defined(__GNUC__)
#define PLAT_USED __attribute__((used, visibility("default")))
#else
#define PLAT_USED
#endif
#ifdef __cplusplus
#define PLAT_EXTERN_C extern "C"
#else
#define PLAT_EXTERN_C
#endif
#define PLAT_APP_INFO(NAME, AUTHOR, DESC, ICON, MEMKB, FLAGS) \
    PLAT_EXTERN_C PLAT_USED const PlatInfo plat_app_info = {"PLATINF", NAME, AUTHOR, DESC, ICON, MEMKB, FLAGS}

/* Hot code for the fast RAM (SRAM_L, on the Cortex-M4 code bus): the loader
 * copies the .fast segment there when it fits (up to PLAT_FAST_MAX bytes),
 * otherwise it runs from the SDRAM with the rest of the app. */
#if defined(__arm__) && defined(PLAT_DEVICE)
#define PLAT_FAST __attribute__((section(".fast"), noinline, noclone))
#else
#define PLAT_FAST
#endif
#define PLAT_FAST_MAX 16384

/* The one symbol PLATFORM looks for. */
typedef const PlatApp* (*PlatEntryFn)(void);
const PlatApp* plat_app_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_APP_H */
