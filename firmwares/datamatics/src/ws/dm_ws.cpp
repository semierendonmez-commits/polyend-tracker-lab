// DATAMATICS -- workstation logic: registry, input, pages, data loader, pieces.
// Drawing: dm_draw.cpp (pages) and dm_vis.cpp (VIEW modes). Performance
// (scenes, morph, recorder, PERFORM pads): dm_perf.cpp.
#include "ws/dm_ws.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dm {

using namespace lab;
using kit::ParamId;

namespace {
const char* const kOffOn[] = {"OFF", "ON"};
const char* const kEndNames[] = {"LOOP", "STOP"};
const char* const kMidiModes[] = {"OFF", "ONE CH", "CH/STREAM"};
const char* const kArrNames[] = {"FREE", "SECTIONS"};
const char* const kSongNames[] = {"LIVE", "CHAIN"};
const char* const kQuantN[] = {"NOW", "BEAT", "BAR", "2 BARS", "4 BARS"};
const char* const kVisNames[VM_COUNT] = {"STREAM", "BARCODE", "TRANSIT", "GRID", "SPATIAL"};
const char* const kSpeedNames[] = {"SLOW", "NORMAL", "FAST"};
const char* const kSetEnd[] = {"Loop", "Stop"};
const char* const kSetMidi[] = {"Off", "One channel", "Channel per stream"};
const char* const kSetArr[] = {"Free run", "Sections"};
const char* const kSetOffOn[] = {"Off", "On"};

// STREAM page rows: every parameter of one stream, data first.
const uint8_t kStreamOrder[] = {P_SLOT,  P_UNIT, P_ORDER,  P_START,  P_LEN,   P_STRIDE, P_THR,     P_SYNC,
                                P_RATE_MS, P_RATE_DIV, P_SOUND, P_LO, P_HI, P_SCALE, P_DUR,   P_DURMOD,
                                P_SHAPE, P_VEL, P_LEVEL, P_PANMODE, P_PAN, P_STEM, P_MIDI, P_MUTE, P_SOLO};
constexpr int kStreamRows = int(sizeof kStreamOrder);

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int enumCount(int p) { return paramInfo(p).max - paramInfo(p).min + 1; }

const Rgb kSlotRgb[kSlots] = {Rgb(0x3CC8FF), Rgb(0xFFB020), Rgb(0x7CFF5A), Rgb(0xD27CFF)};
const uint8_t kSlot332[kSlots] = {0x3B, 0xF4, 0x7D, 0xCF};
const uint8_t kSlot332Dim[kSlots] = {0x0A, 0x48, 0x2C, 0x46};
}  // namespace

const char* visName(int m) { return (m >= 0 && m < VM_COUNT) ? kVisNames[m] : "?"; }
Rgb slotColor(int slot) { return kSlotRgb[slot & 3]; }
uint8_t slotColor332(int slot, bool dim) { return dim ? kSlot332Dim[slot & 3] : kSlot332[slot & 3]; }
int streamParamAt(int row) { return kStreamOrder[clampi(row, 0, kStreamRows - 1)]; }
int streamParamRows() { return kStreamRows; }

// ---------------------------------------------------------------------------------
bool Datamatics::init(Context& c) {
    ctx_ = &c;
    theme_.bg = Rgb(0x000000);
    theme_.panel = Rgb(0x0C0C0C);
    theme_.fg = Rgb(0xFFFFFF);
    theme_.dim = Rgb(0x5A5A5A);
    theme_.accent = Rgb(0xFF2A00);    // the single accent: signal red
    theme_.accent2 = Rgb(0xB0B0B0);
    theme_.warn = Rgb(0xFF2A00);
    theme_.ok = Rgb(0xFFFFFF);
    Arena& a = *c.arena;
    if (!eng_.init(a)) return false;
    eng_.bindMidi(&c);
    pieces_ = static_cast<char(*)[24]>(a.alloc(kMaxPieces * 24));
    stage_ = static_cast<uint8_t*>(a.alloc(kStageBytes, 32));
    midiScratch_ = static_cast<MidiEv*>(a.alloc(sizeof(MidiEv) * kMaxMidiEvents));
    wf_ = static_cast<uint8_t*>(a.alloc(kWfW * kWfH));
    bc_ = static_cast<uint8_t*>(a.alloc(kBcW * kBcH * kSlots));
    mx_ = static_cast<uint8_t*>(a.alloc(kMxW * kMxH));
    builtinBuf_ = static_cast<uint8_t*>(a.alloc(65536));
    scene_ = a.make<Scene>(kScenes + 1);
    perf_ = static_cast<PerfEv*>(a.alloc(sizeof(PerfEv) * kPerfMax));
    pieceBuf_ = static_cast<char*>(a.alloc(kPieceBuf));
    if (!pieces_ || !stage_ || !midiScratch_ || !wf_ || !bc_ || !mx_ || !builtinBuf_ || !scene_ || !perf_ || !pieceBuf_)
        return false;
    // built-in data in every slot: the demo piece plays without a card
    loadBuiltinNow(0, 1, kAuto);
    loadBuiltinNow(1, 0, kAuto);
    loadBuiltinNow(2, 2, kAuto);
    loadBuiltinNow(3, 3, kAuto);
    copyStr(pieceName_, sizeof pieceName_, "DEMO");

    registerParams();
    kit::Mod& M = c.mod();
    static const char* const kSrc[kStreams] = {"D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"};
    for (int s = 0; s < kStreams; ++s) M.addSource(kSrc[s], &eng_.srcData[s]);
    M.addSource("DENS", &eng_.srcDens);
    M.addSource("SEC", &eng_.srcSec);
    M.addSource("HIT", &eng_.srcHit);
    M.addSource("MORPH", &srcMorph_);
    c.browser().reserve(256, 4.0f);
    firstBootScenes();

    if (c.fs) {
        char p[96];
        std::snprintf(p, sizeof p, "%s/Pieces", c.home);
        c.fs->mkdir(p);
        c.fs->mkdir("Data");
        // UI settings: "DMUI <waterfall speed> <autoload>"
        std::snprintf(p, sizeof p, "%s/ui.cfg", c.home);
        const int32_t h = c.fs->open(p, IStorage::kRead);
        if (h >= 0) {
            char b[32] = {};
            const int32_t n = c.fs->read(h, b, sizeof b - 1);
            c.fs->close(h);
            if (n >= 8 && !std::strncmp(b, "DMUI ", 5) && b[5] >= '0' && b[5] <= '2' && (b[7] == '0' || b[7] == '1')) {
                wfSpeed_ = b[5] - '0';
                setAutoload_ = b[7] - '0';
            }
        }
        std::snprintf(p, sizeof p, "%s/last.dmx", c.home);
        if (setAutoload_ && c.fs->exists(p)) loadPiece(p);
    }
    pushRegistry();
    return true;
}

// ---- registry ------------------------------------------------------------------------
void Datamatics::registerParams() {
    kit::Registry& P = ctx_->params();
    // per-stream display names ("3 Thresh") live in the arena
    char(*names)[16] = static_cast<char(*)[16]>(ctx_->arena->alloc(16 * 100));
    int nn = 0;
    auto nm = [&](int s, const char* what) -> const char* {
        if (!names || nn >= 100) return what;
        std::snprintf(names[nn], 16, "%d %s", s + 1, what);
        return names[nn++];
    };
    static const char* const kGrp[kStreams] = {"ST1", "ST2", "ST3", "ST4", "ST5", "ST6", "ST7", "ST8"};
    char key[24];
    auto K = [&](int s, const char* k) -> const char* {
        std::snprintf(key, sizeof key, "s%d.%s", s + 1, k);
        return key;
    };
    for (int s = 0; s < kStreams; ++s)
        for (int p = 0; p < P_COUNT; ++p) pid_[s][p] = kit::kNoParam;

    // PERFORM: mutes, solos, the throw / morph settings, the slot scrubs
    P.setPage(PG_PERFORM);
    for (int s = 0; s < kStreams; ++s) {
        P.setGroup(kGrp[s], slotColor(eng_.st[s].p[P_SLOT]));
        pid_[s][P_MUTE] = P.add(K(s, "mute"), nm(s, "Mute"), &eng_.st[s].p[P_MUTE]).names(kOffOn, 2).pad(s);
        pid_[s][P_SOLO] = P.add(K(s, "solo"), nm(s, "Solo"), &eng_.st[s].p[P_SOLO]).names(kOffOn, 2).pad(12 + s);
    }
    P.setGroup("PERFORM", Rgb(0xFF2A00));
    quantId_ = P.add("pf.quant", "Quantise", &eng_.quant).names(kQuantN, Q_COUNT).pad(8);
    morphId_ = P.add("pf.morph", "Morph", &morphBars_).range(0, 16).unit("bars").pad(9);
    stutId_ = P.add("pf.stutter", "Stutter", &eng_.stutterDiv).names(paramInfo(P_RATE_DIV).names, kRateDivs).pad(10);
    freezeId_ = P.add("pf.freeze", "Freeze", &eng_.freezeWin).range(1, 32).unit("units").pad(11);
    depthId_ = P.add("pf.depth", "Throw depth", &eng_.throwDepth).range(0, 100).unit("%").pad(20);
    for (int i = 0; i < kSlots; ++i) {
        static const char* const kScrubN[kSlots] = {"S1 Scrub", "S2 Scrub", "S3 Scrub", "S4 Scrub"};
        static const char* const kScrubK[kSlots] = {"pf.scrub1", "pf.scrub2", "pf.scrub3", "pf.scrub4"};
        static const char* const kSG[kSlots] = {"SLOT1", "SLOT2", "SLOT3", "SLOT4"};
        P.setGroup(kSG[i], slotColor(i));
        scrubId_[i] = P.add(kScrubK[i], kScrubN[i], &eng_.scrub[i]).percent().pad(24 + i);
    }
    // MATRIX: how each stream reads (density, start, length, stride)
    P.setPage(PG_MATRIX);
    for (int s = 0; s < kStreams; ++s) {
        P.setGroup(kGrp[s]);
        pid_[s][P_THR] = P.add(K(s, "thr"), nm(s, "Thresh"), &eng_.st[s].p[P_THR]).range(0, 255).pad(s);
        pid_[s][P_START] = P.add(K(s, "start"), nm(s, "Start"),
                                 [](void* c, float v) { *static_cast<int*>(c) = int(v * 10.0f + 0.5f); },
                                 &eng_.st[s].p[P_START], float(eng_.st[s].p[P_START]) * 0.1f)
                               .range(0.0f, 99.9f)
                               .step(0.1f)
                               .unit("%")
                               .def(0.0f)
                               .pad(12 + s);
        pid_[s][P_LEN] = P.add(K(s, "len"), nm(s, "Length"), &eng_.st[s].p[P_LEN])
                             .names(paramInfo(P_LEN).names, enumCount(P_LEN))
                             .pad(24 + s);
    }
    for (int s = 0; s < 4; ++s) {   // strides of 1-4 on the free pads, 5-8 after
        P.setGroup(kGrp[s]);
        pid_[s][P_STRIDE] = P.add(K(s, "stride"), nm(s, "Stride"), &eng_.st[s].p[P_STRIDE]).range(1, 64).pad(8 + s);
    }
    for (int s = 4; s < kStreams; ++s) {
        P.setGroup(kGrp[s]);
        pid_[s][P_STRIDE] = P.add(K(s, "stride"), nm(s, "Stride"), &eng_.st[s].p[P_STRIDE]).range(1, 64).pad(16 + s);
    }
    // MIXER
    P.setPage(PG_MIXER);
    for (int s = 0; s < kStreams; ++s) {
        P.setGroup(kGrp[s]);
        pid_[s][P_LEVEL] = P.add(K(s, "level"), nm(s, "Level"), &eng_.st[s].p[P_LEVEL]).range(0, 100).unit("%").pad(s);
        pid_[s][P_PAN] = P.add(K(s, "pan"), nm(s, "Pan"), &eng_.st[s].p[P_PAN]).range(-100, 100).bipolar().pad(12 + s);
        pid_[s][P_PANMODE] = P.add(K(s, "panmode"), nm(s, "Pan mode"), &eng_.st[s].p[P_PANMODE])
                                 .names(paramInfo(P_PANMODE).names, enumCount(P_PANMODE))
                                 .pad(24 + s);
    }
    for (int s = 0; s < kStreams; ++s) {
        P.setGroup(kGrp[s]);
        pid_[s][P_STEM] = P.add(K(s, "stem"), nm(s, "Stem"), &eng_.st[s].p[P_STEM]).names(paramInfo(P_STEM).names, 4).noMod();
    }
    // SONG
    P.setPage(PG_SONG);
    P.setGroup("SONG", Rgb(0xB0B0B0));
    songId_ = P.add("song.mode", "Song mode", &eng_.songMode).names(kSongNames, 2).noMod();
    P.add("song.arrange", "Arrangement", &eng_.arrange).names(kArrNames, 2).noMod();
    P.add("song.end", "End", &eng_.endMode).names(kEndNames, 2).noMod();
    P.add("song.midi", "MIDI out", &eng_.midiMode).names(kMidiModes, 3).noMod();
    // STREAM pages: one per stream
    for (int s = 0; s < kStreams; ++s) {
        P.setPage(PG_ST0 + s);
        P.setGroup(kGrp[s]);
        auto en = [&](int p, const char* k, const char* n) {
            pid_[s][p] = P.add(K(s, k), n, &eng_.st[s].p[p]).names(paramInfo(p).names, enumCount(p));
        };
        en(P_SLOT, "slot", "Slot");
        en(P_UNIT, "unit", "Unit");
        en(P_ORDER, "order", "Order");
        en(P_SYNC, "grid", "Grid");
        pid_[s][P_RATE_MS] = P.add(K(s, "rate"), "Rate", &eng_.st[s].p[P_RATE_MS]).range(1, 2000).exp().unit("ms");
        en(P_RATE_DIV, "div", "Division");
        en(P_SOUND, "sound", "Sound");
        pid_[s][P_LO] = P.add(K(s, "lo"), "Low", &eng_.st[s].p[P_LO]).range(1, 21000).exp().unit("Hz");
        pid_[s][P_HI] = P.add(K(s, "hi"), "High", &eng_.st[s].p[P_HI]).range(1, 21000).exp().unit("Hz");
        en(P_SCALE, "scale", "Scale");
        pid_[s][P_DUR] = P.add(K(s, "dur"), "Duration",
                               [](void* c, float v) {
                                   const int x = int(v * 10.0f + 0.5f);
                                   *static_cast<int*>(c) = x < 1 ? 1 : x;
                               },
                               &eng_.st[s].p[P_DUR], float(eng_.st[s].p[P_DUR]) * 0.1f)
                             .range(0.1f, 2000.0f)
                             .exp()
                             .unit("ms")
                             .def(10.0f);
        pid_[s][P_DURMOD] = P.add(K(s, "durmod"), "Dur mod", &eng_.st[s].p[P_DURMOD]).range(0, 100).unit("%");
        en(P_SHAPE, "shape", "Shape");
        pid_[s][P_VEL] = P.add(K(s, "vel"), "Velocity", &eng_.st[s].p[P_VEL]).range(0, 100).unit("%");
        en(P_MIDI, "midi", "MIDI");
    }
    // VIEW
    P.setPage(PG_VIEW);
    P.setGroup("VIEW", Rgb(0xFFFFFF));
    visId_ = P.add("view.mode", "Visual", &visMode_).names(kVisNames, VM_COUNT).noMod();
    followId_ = P.add("view.follow", "Follow", &follow_).range(1, 8).noMod();
    autoId_ = P.add("view.auto", "Auto cycle", &visAuto_).names(kOffOn, 2).noMod();
    speedId_ = P.add("view.speed", "Waterfall", &wfSpeed_).names(kSpeedNames, 3).noMod();
    P.setPage(0);

    // the params a scene captures: everything about the streams + the scrubs
    sceneN_ = 0;
    for (int s = 0; s < kStreams; ++s)
        for (int p = 0; p < P_COUNT; ++p)
            if (pid_[s][p] != kit::kNoParam && p != P_STEM && sceneN_ < kSceneMax) sceneIds_[sceneN_++] = pid_[s][p];
    for (int i = 0; i < kSlots; ++i)
        if (sceneN_ < kSceneMax) sceneIds_[sceneN_++] = scrubId_[i];
}

void Datamatics::pushRegistry() {
    // Fields written directly (defaults, an older piece, presets) -> registry base.
    kit::Registry& P = ctx_->params();
    for (int s = 0; s < kStreams; ++s)
        for (int p = 0; p < P_COUNT; ++p) {
            const ParamId id = pid_[s][p];
            if (id == kit::kNoParam) continue;
            const int v = eng_.st[s].p[p];
            P.set(id, (p == P_DUR || p == P_START) ? float(v) * 0.1f : float(v), false);
        }
    for (int i = 0; i < kSlots; ++i) P.set(scrubId_[i], eng_.scrub[i], false);
    // Without audio (tests, before the first block) nothing clears the dirty
    // marks, and sync() skips dirty params: write them now.
    if (!ctx_->shell || !ctx_->shell->labkit().audioRunning) P.applyNow();
}

void Datamatics::nudge(ParamId id, int delta, bool fine) {
    kit::Registry& P = ctx_->params();
    if (!P.valid(id)) return;
    P.nudge(id, delta, fine);
    perfLog(PE_PARAM, P.meta(id).key, uint16_t(P.norm(id) * 65535.0f + 0.5f));
}

void Datamatics::setParam(ParamId id, float v) {
    kit::Registry& P = ctx_->params();
    if (!P.valid(id)) return;
    P.set(id, v);
    perfLog(PE_PARAM, P.meta(id).key, uint16_t(P.norm(id) * 65535.0f + 0.5f));
}

void Datamatics::assign(int stream, int slot) {
    if (stream < 0 || stream >= kStreams || slot < 0 || slot >= kSlots) return;
    setParam(pid_[stream][P_SLOT], float(slot));
    ctx_->params().applyNow();
}

// ---- LABKIT navigation ----------------------------------------------------------------------
int Datamatics::navPages(kit::PageInfo* out, int cap) {
    static const char* const kNames[PG_COUNT] = {"PERFORM", "MATRIX", "MIXER", "SONG", "ST1", "ST2", "ST3", "ST4",
                                                 "ST5",     "ST6",    "ST7",   "ST8",  "VIEW", "DATA", "PIECE"};
    static const char* const kStHelp = "One stream: what it reads (slot, unit, order), its grid, its sound and its mix.";
    static const char* const kHelp[PG_COUNT] = {
        "Live: mute / fill streams, launch sections, recall scenes, hold throws, scrub the data.",
        "The barcode sequencer: what every stream will read next. Encoder = density.",
        "Level, pan, pan mode and stem of the eight streams.",
        "The section chain: LIVE repeats a section until you launch another, CHAIN plays them in order.",
        kStHelp, kStHelp, kStHelp, kStHelp, kStHelp, kStHelp, kStHelp, kStHelp,
        "Full-screen datamatics: waterfall, barcode, transit, grid, spatial. Arrows: mode / stream.",
        "The four data slots and the patch: which stream reads which slot. F8 opens the browser.",
        "Save and load pieces (.dmx). Insert = save as new, F8 = load."};
    static const char* const kF8[PG_COUNT] = {
        "F8: replay the recorded performance on / off (Rec arms recording)",
        "F8: mute the selected stream",
        "F8: mute (Shift: solo) the selected stream",
        "F8: launch the selected section (Shift: LIVE / CHAIN)",
        "F8: audition the stream", "F8: audition the stream", "F8: audition the stream", "F8: audition the stream",
        "F8: audition the stream", "F8: audition the stream", "F8: audition the stream", "F8: audition the stream",
        "F8: auto-cycle the visual modes",
        "F8: load a file into the selected slot (browser)",
        "F8: load the selected piece"};
    int n = 0;
    for (; n < PG_COUNT && n < cap; ++n) {
        out[n].name = kNames[n];
        out[n].help = kHelp[n];
        out[n].color = isStreamPage(n) ? slotColor(eng_.st[n - PG_ST0].p[P_SLOT]) : Rgb(0xFFFFFF);
        out[n].fkeys[0] = "F1: PERFORM";
        out[n].fkeys[1] = "F2: MATRIX";
        out[n].fkeys[2] = "F3: STREAM (the selected stream)";
        out[n].fkeys[3] = "F4: DATA (slots and patch)";
        out[n].fkeys[4] = "F5: MIXER";
        out[n].fkeys[5] = "F6: SONG (sections)";
        out[n].fkeys[6] = "F7: VIEW (again: next visual)";
        out[n].fkeys[7] = kF8[n];
    }
    return n;
}

void Datamatics::navGoto(int page) {
    if (page < 0 || page >= PG_COUNT) return;
    if (isStreamPage(page)) sel_ = page - PG_ST0;
    setPage(page);
}

void Datamatics::navFocus(ParamId id) {
    for (int s = 0; s < kStreams; ++s)
        for (int r = 0; r < kStreamRows; ++r)
            if (pid_[s][kStreamOrder[r]] == id && isStreamPage(page_)) streamRow_ = r;
}

void Datamatics::navParamChanged(ParamId id) {
    kit::Registry& P = ctx_->params();
    if (P.valid(id)) perfLog(PE_PARAM, P.meta(id).key, uint16_t(P.norm(id) * 65535.0f + 0.5f));
}

// ---- data slots ----------------------------------------------------------------------------
void Datamatics::loadBuiltinNow(int slot, int b, int mode) {
    // A built-in set read in AUTO keeps its own form; a forced mode reads its
    // values as a byte file through that mode (e.g. @LFSR as BITS).
    Slot& sl = eng_.slot[slot];
    __atomic_store_n(&sl.n, 0u, __ATOMIC_RELEASE);
    DecodeOut o;
    o.cap = kMaxValues;
    if (mode == kAuto || !builtinBuf_) {
        o.v = sl.v;
        o.aux = sl.aux;
        makeBuiltin(b, o);
    } else {
        o.v = builtinBuf_;
        o.aux = nullptr;
        o.cap = 65536;
        const uint32_t n = makeBuiltin(b, o);
        DecodeOut d;
        d.v = sl.v;
        d.aux = sl.aux;
        d.cap = kMaxValues;
        decode(builtinName(b), builtinBuf_, n, Mode(mode), d, midiScratch_);
        o = d;
    }
    sl.width = o.width ? o.width : 256;
    sl.mode = o.mode;
    sl.srcW = o.srcW;
    sl.srcH = o.srcH;
    sl.wantMode = uint8_t(mode);
    copyStr(sl.path, sizeof sl.path, builtinName(b));
    __atomic_store_n(&sl.n, o.n, __ATOMIC_RELEASE);
    ++slotGen_[slot];
    computeMeta(slot, 0);
}

void Datamatics::computeMeta(int slot, uint32_t fileSize) {
    const Slot& sl = eng_.slot[slot];
    SlotMeta& m = meta_[slot];
    m.fileSize = fileSize;
    uint32_t hist[256] = {};
    uint32_t h = 2166136261u, ones = 0;
    const uint32_t n = sl.n;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t v = sl.v[i];
        ++hist[v];
        h = (h ^ v) * 16777619u;
        ones += uint32_t(__builtin_popcount(v));
    }
    m.hash = h;
    m.ones = n ? float(ones) / float(n * 8u) : 0.0f;
    float e = 0.0f;
    for (int i = 0; i < 256; ++i)
        if (hist[i]) {
            const float p = float(hist[i]) / float(n);
            e -= p * log2f(p);
        }
    m.entropy = e;
}

void Datamatics::requestLoad(int slot, const char* path, int mode) {
    if (slot < 0 || slot >= kSlots || !path || !path[0]) return;
    const int b = builtinIndex(path);
    if (b >= 0) {
        loadBuiltinNow(slot, b, mode);
        return;
    }
    for (int i = 0; i < jobCount_; ++i)   // a newer request for the same slot replaces the queued one
        if (jobs_[i].slot == slot) {
            copyStr(jobs_[i].path, sizeof jobs_[i].path, path);
            jobs_[i].mode = mode;
            return;
        }
    if (jobCount_ >= kJobQ) return;
    LoadJob& j = jobs_[jobCount_++];
    copyStr(j.path, sizeof j.path, path);
    j.slot = slot;
    j.mode = mode;
}

void Datamatics::reloadSlot(int slot, int mode) {
    Slot& sl = eng_.slot[slot & 3];
    sl.wantMode = uint8_t(mode);
    if (sl.path[0]) requestLoad(slot, sl.path, mode);
}

void Datamatics::pumpLoader() {
    if (!job_.active) {
        if (!jobCount_) return;
        const LoadJob j = jobs_[0];
        for (int i = 1; i < jobCount_; ++i) jobs_[i - 1] = jobs_[i];
        --jobCount_;
        Slot& sl = eng_.slot[j.slot];
        copyStr(sl.path, sizeof sl.path, j.path);
        sl.wantMode = uint8_t(j.mode);
        if (!ctx_->fs) {
            std::snprintf(status_, sizeof status_, "NO CARD: %.40s", j.path);
            statusUntil_ = nowMs_ + 3000;
            return;
        }
        const int32_t h = ctx_->fs->open(j.path, IStorage::kRead);
        if (h < 0) {
            std::snprintf(status_, sizeof status_, "MISSING %.44s", j.path);
            statusUntil_ = nowMs_ + 3000;
            return;
        }
        job_.active = true;
        copyStr(job_.path, sizeof job_.path, j.path);
        job_.slot = j.slot;
        job_.mode = j.mode;
        job_.handle = h;
        const int32_t sz = ctx_->fs->size(h);
        job_.size = sz > 0 ? uint32_t(sz) : 0;
        job_.limit = job_.size < kStageBytes ? job_.size : kStageBytes;
        job_.pos = 0;
    }
    uint32_t budget = 32768;   // ~32 KB per pass keeps the UI and MIDI moving
    while (job_.pos < job_.limit && budget) {
        uint32_t want = job_.limit - job_.pos;
        if (want > budget) want = budget;
        const int32_t got = ctx_->fs->read(job_.handle, stage_ + job_.pos, want);
        if (got <= 0) {
            job_.limit = job_.pos;
            break;
        }
        job_.pos += uint32_t(got);
        budget -= uint32_t(got) < budget ? uint32_t(got) : budget;
    }
    if (job_.pos >= job_.limit) finishJob();
}

void Datamatics::finishJob() {
    ctx_->fs->close(job_.handle);
    job_.active = false;
    Slot& sl = eng_.slot[job_.slot];
    __atomic_store_n(&sl.n, 0u, __ATOMIC_RELEASE);   // audio reads nothing while we rewrite
    DecodeOut o;
    o.v = sl.v;
    o.aux = sl.aux;
    o.cap = kMaxValues;
    const char* name = job_.path;
    for (const char* p = job_.path; *p; ++p)
        if (*p == '/') name = p + 1;
    decode(name, stage_, job_.pos, Mode(job_.mode), o, midiScratch_);
    sl.width = o.width ? o.width : 256;
    sl.mode = o.mode;
    sl.srcW = o.srcW;
    sl.srcH = o.srcH;
    __atomic_store_n(&sl.n, o.n, __ATOMIC_RELEASE);
    ++slotGen_[job_.slot];
    computeMeta(job_.slot, job_.size);
    if (job_.size > kStageBytes)
        std::snprintf(status_, sizeof status_, "S%d < %.24s first 4 MB", job_.slot + 1, name);
    else
        std::snprintf(status_, sizeof status_, "S%d < %.30s  %s", job_.slot + 1, name, modeName(o.mode));
    statusUntil_ = nowMs_ + 3000;
}

void Datamatics::finishLoadsNow() {
    for (int guard = 0; guard < 100000 && loadBusy(); ++guard) pumpLoader();
}

void Datamatics::openBrowser(int slot) {
    kit::BrowserRequest q;
    static char title[32];
    std::snprintf(title, sizeof title, "Load data into S%d", slot + 1);
    q.title = title;
    q.folder = "Data";
    q.exts = "";   // any file is data
    q.preview = true;
    q.ctx = this;
    pickSlot_ = slot;
    q.onPick = [](void* c, const char* path) {
        Datamatics* w = static_cast<Datamatics*>(c);
        copyStr(w->pendingPick_, sizeof w->pendingPick_, path);   // loaded in update()
    };
    ctx_->browser().open(q);
}

// ---- pieces ------------------------------------------------------------------------
void Datamatics::scanPieces() {
    pieceCount_ = 0;
    if (!ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/Pieces", ctx_->home);
    struct C {
        Datamatics* w;
    } c{this};
    ctx_->fs->listDir(p,
                      [](const IStorage::DirEntry& d, void* q) {
                          Datamatics* w = static_cast<C*>(q)->w;
                          if (d.isDir || !endsWithCi(d.name, ".dmx")) return true;
                          if (w->pieceCount_ >= kMaxPieces) return false;
                          char* n = w->pieces_[w->pieceCount_++];
                          copyStr(n, 24, d.name);
                          const size_t l = std::strlen(n);
                          if (l >= 4) n[l - 4] = 0;
                          return true;
                      },
                      &c);
    if (pieceSel_ >= pieceCount_) pieceSel_ = pieceCount_ ? pieceCount_ - 1 : 0;
}

namespace {
struct Writer {
    char* p;
    uint32_t cap, n;
    bool ok = true;
    void put(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
void Writer::put(const char* fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(p + n, cap - n, fmt, ap);
    va_end(ap);
    if (w < 0 || uint32_t(w) >= cap - n) {
        ok = false;
        return;
    }
    n += uint32_t(w);
}
int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
uint32_t readHex(const char*& s, const char* e) {
    while (s < e && *s == ' ') ++s;
    uint32_t v = 0;
    int d;
    while (s < e && (d = hexv(*s)) >= 0) {
        v = (v << 4) | uint32_t(d);
        ++s;
    }
    return v;
}
}  // namespace

bool Datamatics::savePiece(const char* path) {
    if (!ctx_->fs) return false;
    ctx_->params().sync();   // fields written directly follow into the registry chunk
    uint32_t n = writePiece(eng_, ctx_->bpm(), pieceBuf_, kPieceBuf);
    if (!n) return false;
    Writer w{pieceBuf_, kPieceBuf, n};
    kit::Registry& P = ctx_->params();
    // scenes: key hash : normalized value x 65535, hex
    for (int i = 0; i < kScenes; ++i) {
        if (!scene_[i].used) continue;
        w.put("scene %d", i);
        for (int k = 0; k < sceneN_; ++k)
            w.put(" %lx:%x", static_cast<unsigned long>(P.meta(sceneIds_[k]).key),
                  unsigned(scene_[i].v[k] * 65535.0f + 0.5f));
        w.put("\n");
    }
    // the recorded performance and its start snapshot
    if (perfN_) {
        w.put("perf %lu %d", static_cast<unsigned long>(perfLen_), perfStartSec_);
        for (int k = 0; k < sceneN_; ++k)
            w.put(" %lx:%x", static_cast<unsigned long>(P.meta(sceneIds_[k]).key),
                  unsigned(scene_[kScenes].v[k] * 65535.0f + 0.5f));
        w.put("\n");
        for (int i = 0; i < perfN_; ++i)
            w.put("pe %lx %d %lx %x %d\n", static_cast<unsigned long>(perf_[i].t), perf_[i].type,
                  static_cast<unsigned long>(perf_[i].key), unsigned(perf_[i].val), perf_[i].a);
    }
    // LABKIT registry chunk (base values, macros, routes, MIDI learn), hex in 64-byte lines
    const uint32_t cs = P.chunkSize();
    if (cs && w.ok && w.n + cs * 2 + cs / 32 + 64 < kPieceBuf) {
        uint8_t* chunk = stage_ + (kStageBytes - cs - 16);   // the loader is idle while we save
        if (job_.active) chunk = nullptr;
        const uint32_t m = chunk ? P.saveChunk(chunk, cs + 16) : 0;
        for (uint32_t i = 0; i < m; i += 64) {
            w.put("kit");
            w.put(" ");
            for (uint32_t j = i; j < m && j < i + 64; ++j) w.put("%02x", chunk[j]);
            w.put("\n");
        }
    }
    if (!w.ok) return false;
    const int32_t h = ctx_->fs->open(path, IStorage::kWrite);
    if (h < 0) return false;
    const bool ok = ctx_->fs->write(h, pieceBuf_, w.n) == int32_t(w.n);
    ctx_->fs->close(h);
    return ok;
}

bool Datamatics::loadPiece(const char* path) {
    if (!ctx_->fs) return false;
    const int32_t h = ctx_->fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    uint32_t n = 0;
    for (;;) {
        const int32_t got = ctx_->fs->read(h, pieceBuf_ + n, uint32_t(kPieceBuf - 1 - n));
        if (got <= 0) break;
        n += uint32_t(got);
        if (n >= kPieceBuf - 1) break;
    }
    ctx_->fs->close(h);
    pieceBuf_[n] = 0;
    float bpm = ctx_->bpm();
    char oldPaths[kSlots][128];
    uint8_t oldModes[kSlots];
    for (int i = 0; i < kSlots; ++i) {
        copyStr(oldPaths[i], sizeof oldPaths[i], eng_.slot[i].path);
        oldModes[i] = eng_.slot[i].wantMode;
        eng_.slot[i].path[0] = 0;
    }
    if (!readPiece(eng_, pieceBuf_, n, &bpm)) {
        for (int i = 0; i < kSlots; ++i) copyStr(eng_.slot[i].path, sizeof eng_.slot[i].path, oldPaths[i]);
        return false;
    }
    if (bpm >= 20.0f && bpm <= 400.0f) ctx_->setBpm(bpm);
    for (int i = 0; i < kSlots; ++i) {
        char p[128];
        copyStr(p, sizeof p, eng_.slot[i].path);
        const int m = eng_.slot[i].wantMode;
        if (!p[0]) {
            eng_.slot[i].n = 0;
            ++slotGen_[i];
            computeMeta(i, 0);
            continue;
        }
        if (!std::strcmp(p, oldPaths[i]) && m == oldModes[i] && eng_.slot[i].n) continue;   // already in memory
        requestLoad(i, p, m);
    }
    pushRegistry();
    // scenes, performance, registry chunk
    kit::Registry& P = ctx_->params();
    for (int i = 0; i <= kScenes; ++i) scene_[i].used = false;
    perfN_ = 0;
    perfLen_ = 0;
    uint8_t* chunk = stage_ + (kStageBytes - 65536);
    uint32_t chunkN = 0;
    const char* s = pieceBuf_;
    const char* end = pieceBuf_ + n;
    auto readPairs = [&](const char*& q, const char* le, Scene& sc) {
        captureScene(sc);   // keys missing in the file keep the current value
        while (q < le) {
            const uint32_t key = readHex(q, le);
            if (q >= le || *q != ':') break;
            ++q;
            const uint32_t v = readHex(q, le);
            const ParamId id = P.findHash(key);
            for (int k = 0; k < sceneN_; ++k)
                if (sceneIds_[k] == id) sc.v[k] = float(v > 65535 ? 65535 : v) * (1.0f / 65535.0f);
        }
        sc.used = true;
    };
    while (s < end) {
        const char* eol = s;
        while (eol < end && *eol != '\n') ++eol;
        const char* le = eol;
        if (le > s && le[-1] == '\r') --le;
        if (le - s > 6 && !std::strncmp(s, "scene ", 6)) {
            const char* q = s + 6;
            const uint32_t i = readHex(q, le);
            if (i < uint32_t(kScenes)) readPairs(q, le, scene_[i]);
        } else if (le - s > 5 && !std::strncmp(s, "perf ", 5)) {
            const char* q = s + 5;
            int len = 0, sec = 0;
            while (q < le && *q == ' ') ++q;
            while (q < le && *q >= '0' && *q <= '9') len = len * 10 + (*q++ - '0');
            while (q < le && *q == ' ') ++q;
            while (q < le && *q >= '0' && *q <= '9') sec = sec * 10 + (*q++ - '0');
            perfLen_ = uint32_t(len);
            perfStartSec_ = sec;
            readPairs(q, le, scene_[kScenes]);
        } else if (le - s > 3 && !std::strncmp(s, "pe ", 3) && perfN_ < kPerfMax) {
            const char* q = s + 3;
            PerfEv& e = perf_[perfN_];
            e.t = readHex(q, le);
            e.type = uint8_t(readHex(q, le));
            e.key = readHex(q, le);
            e.val = uint16_t(readHex(q, le));
            e.a = uint8_t(readHex(q, le));
            if (e.type <= PE_SLOTSOLO) ++perfN_;
        } else if (le - s > 4 && !std::strncmp(s, "kit ", 4)) {
            for (const char* q = s + 4; q + 1 < le && chunkN < 65536; q += 2) {
                const int a = hexv(q[0]), b = hexv(q[1]);
                if (a < 0 || b < 0) break;
                chunk[chunkN++] = uint8_t((a << 4) | b);
            }
        }
        s = eol < end ? eol + 1 : end;
    }
    if (chunkN && !job_.active) P.loadChunk(chunk, chunkN);
    P.applyNow();
    eng_.reset();
    return true;
}

void Datamatics::newPiece() {
    eng_.defaults();
    loadBuiltinNow(0, 1, kAuto);
    loadBuiltinNow(1, 0, kAuto);
    loadBuiltinNow(2, 2, kAuto);
    loadBuiltinNow(3, 3, kAuto);
    copyStr(pieceName_, sizeof pieceName_, "DEMO");
    pushRegistry();
    ctx_->params().applyNow();
    perfN_ = 0;
    firstBootScenes();
}

void Datamatics::save() {
    if (!ctx_ || !ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/last.dmx", ctx_->home);
    savePiece(p);
    std::snprintf(p, sizeof p, "%s/ui.cfg", ctx_->home);
    const int32_t h = ctx_->fs->open(p, IStorage::kWrite);
    if (h >= 0) {
        char b[16];
        const int n = std::snprintf(b, sizeof b, "DMUI %d %d\n", wfSpeed_ < 0 || wfSpeed_ > 2 ? 1 : wfSpeed_, setAutoload_ ? 1 : 0);
        ctx_->fs->write(h, b, uint32_t(n));
        ctx_->fs->close(h);
    }
}

// ---- settings rows ----------------------------------------------------------------------
int Datamatics::settings(SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names) {
        if (n >= cap) return;
        SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.onChange = [](void* c, int) { static_cast<Datamatics*>(c)->ctx_->params().sync(); };
        r.ctx = this;
    };
    add("Arrangement", &eng_.arrange, 0, 1, kSetArr);
    add("At the end of the chain", &eng_.endMode, 0, 1, kSetEnd);
    add("MIDI out of the streams", &eng_.midiMode, 0, 2, kSetMidi);
    add("Reload last piece at boot", &setAutoload_, 0, 1, kSetOffOn);
    return n;
}

// ---- transport / audio --------------------------------------------------------------------
void Datamatics::onTransport(bool playing) {
    if (!playing) {
        eng_.wantStop = false;
        if (perfRec_) perfStop();
        perfReplaying_ = false;
        morphPending_ = -1;
    }
}

void Datamatics::render(const Transport& t, const AudioIn&, float* l, float* r) {
    eng_.render(t, l, r, stemL_, stemR_);
    for (int i = 0; i < 4; ++i) ctx_->stem(i, stemL_[i], stemR_[i]);
}

void Datamatics::onMidi(const MidiMsg& m) {
    // Notes 36..43 audition streams 1..8; 48.. launch sections; CC 1..8 = stream levels.
    if (m.type() == 0x90 && m.d2) {
        if (m.d1 >= 36 && m.d1 < 36 + kStreams) eng_.st[m.d1 - 36].audition = m.d2;
        else if (m.d1 >= 48 && m.d1 < 48 + kMaxSections && m.d1 - 48 < eng_.secCount) launchSection(m.d1 - 48);
    } else if (m.type() == 0xB0 && m.d1 >= 1 && m.d1 <= kStreams) {
        setParam(pid_[m.d1 - 1][P_LEVEL], float(m.d2 * 100 / 127));
    }
}

void Datamatics::update(uint32_t ms) {
    nowMs_ = ms;
    pumpLoader();
    if (pendingPick_[0]) {
        requestLoad(pickSlot_, pendingPick_, eng_.slot[pickSlot_].wantMode);
        pendingPick_[0] = 0;
    }
    if (eng_.wantStop && ctx_->playing()) ctx_->setPlaying(false);
    // stream groups follow their slot's colour (PARAM pads / MAP)
    static int lastSlot[kStreams] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (int s = 0; s < kStreams; ++s) {
        const int sl = eng_.st[s].p[P_SLOT] & 3;
        if (sl != lastSlot[s]) {
            static const char* const kGrp[kStreams] = {"ST1", "ST2", "ST3", "ST4", "ST5", "ST6", "ST7", "ST8"};
            ctx_->params().group(kGrp[s], slotColor(sl));
            lastSlot[s] = sl;
        }
    }
    stepMorph();
    perfReplayStep();
    drainEvents(ms);
    uploadVisuals();
}

// ---- input --------------------------------------------------------------------------------
void Datamatics::setPage(int p) {
    if (p != page_) lastPage_ = page_;
    page_ = p;
    if (isStreamPage(p)) sel_ = p - PG_ST0;
    delArm_ = false;
    if (p == PG_PIECE) scanPieces();
    wfAllDirty_ = true;
    mxUploaded_ = false;
    bcUploaded_ = false;
    for (auto& g : bcGen_) g = 0xFFFFFFFFu;
}

void Datamatics::onEvent(const Event& e) {
    if (e.type == Event::kPadDown || e.type == Event::kPadUp) {
        padEvent(e);
        return;
    }
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::F1: setPage(PG_PERFORM); return;
            case btn::F2: setPage(PG_MATRIX); return;
            case btn::F3: setPage(PG_ST0 + sel_); return;
            case btn::F4: setPage(PG_DATA); return;
            case btn::F5: setPage(PG_MIXER); return;
            case btn::F6: setPage(PG_SONG); return;
            case btn::F7:
                if (page_ == PG_VIEW) setParam(visId_, float((visMode_ + 1) % VM_COUNT));
                else setPage(PG_VIEW);
                return;
            case btn::Performance: setPage(PG_PERFORM); return;
            case btn::Pattern: setPage(PG_MATRIX); return;
            case btn::SampleEditor: setPage(PG_ST0 + sel_); return;
            case btn::SampleLoader: setPage(PG_DATA); return;
            case btn::Song: setPage(PG_SONG); return;
            case btn::File: setPage(PG_PIECE); return;
            case btn::SamplePlayback: setPage(PG_VIEW); return;
            case btn::Rec:   // Shift + Rec is the Shell's recorder
                if (perfRec_) {
                    perfStop();
                } else {
                    perfArm_ = !perfArm_;
                    ctx_->toast(perfArm_ ? "Performance armed: Play records" : "Performance recording off");
                }
                return;
            default: break;
        }
    }
    switch (screenPage()) {
        case PG_PERFORM: performEvent(e); break;
        case PG_MATRIX: matrixEvent(e); break;
        case PG_ST0: streamEvent(e); break;
        case PG_DATA: dataEvent(e); break;
        case PG_SONG: songEvent(e); break;
        case PG_MIXER: mixerEvent(e); break;
        case PG_PIECE: pieceEvent(e); break;
        default: viewEvent(e); break;
    }
}

void Datamatics::padEvent(const Event& e) {
    const int row = padRow(e.id), col = padCol(e.id);
    const bool down = e.type == Event::kPadDown;
    switch (page_) {
        case PG_MATRIX: matrixPad(row, col, down, e.velocity, e.shift); break;
        case PG_DATA: dataPad(row, col, down, e.shift); break;
        default: performPad(row, col, down, e.velocity, e.shift); break;
    }
}

void Datamatics::matrixPad(int row, int col, bool down, uint8_t vel, bool shift) {
    // rows = the 4 streams of the bank; cols 1-8: the next 8 units (audition),
    // 9 mute, 10 solo, 11 select (Shift: STREAM page), 12 fill (Shift: bank)
    const int s = bank_ * 4 + row;
    if (col == 11) {
        if (shift && down) bank_ ^= 1;
        else if (!shift) setFill(down ? (fill_ | (1u << s)) : (fill_ & ~(1u << s)));
        return;
    }
    if (!down) return;
    if (col < 8) {
        eng_.st[s].audOff = uint16_t(col);
        eng_.st[s].audition = uint8_t(vel ? vel : 100);
        sel_ = s;
        return;
    }
    if (col == 8) setParam(pid_[s][P_MUTE], float(eng_.st[s].p[P_MUTE] ^ 1));
    else if (col == 9) setParam(pid_[s][P_SOLO], float(eng_.st[s].p[P_SOLO] ^ 1));
    else if (col == 10) {
        sel_ = s;
        if (shift) setPage(PG_ST0 + s);
    }
}

void Datamatics::dataPad(int row, int col, bool down, bool shift) {
    // rows = slots S1..S4; cols 1-8: patch the stream to this slot; 9 browse,
    // 10 next reading mode, 11 solo the slot (held), 12 next built-in data set
    const int sl = row;
    if (col == 10) {
        setSlotSolo(down ? (slotSoloMask_ | (1u << sl)) : (slotSoloMask_ & ~(1u << sl)));
        return;
    }
    if (!down) return;
    dataSlot_ = sl;
    if (col < 8) {
        dataCol_ = col;
        assign(col, sl);
    } else if (col == 8) {
        openBrowser(sl);
    } else if (col == 9) {
        reloadSlot(sl, (eng_.slot[sl].wantMode + (shift ? kModeCount - 1 : 1)) % kModeCount);
    } else if (col == 11) {
        const int b = builtinIndex(eng_.slot[sl].path);
        loadBuiltinNow(sl, (b < 0 ? 0 : b + 1) % kBuiltinCount, eng_.slot[sl].wantMode);
    }
}

void Datamatics::matrixEvent(const Event& e) {
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: sel_ = (sel_ + kStreams - 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Down: sel_ = (sel_ + 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Left: nudge(pid_[sel_][P_START], e.shift ? -1 : -10, false); break;   // 1 % (Shift: 0.1 %)
            case btn::Right: nudge(pid_[sel_][P_START], e.shift ? 1 : 10, false); break;
            case btn::Insert: setPage(PG_ST0 + sel_); break;
            case btn::F8: setParam(pid_[sel_][P_MUTE], float(eng_.st[sel_].p[P_MUTE] ^ 1)); break;
            case btn::Delete: eng_.st[sel_].cue = 0; break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        // encoder: density (threshold down = more events); Shift: fine
        nudge(pid_[sel_][P_THR], -e.delta * (e.shift ? 1 : 4), false);
    }
}

void Datamatics::streamEvent(const Event& e) {
    const int s = sel_;
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: streamRow_ = (streamRow_ + kStreamRows - 1) % kStreamRows; break;
            case btn::Down: streamRow_ = (streamRow_ + 1) % kStreamRows; break;
            case btn::Left: setPage(PG_ST0 + (s + kStreams - 1) % kStreams); break;
            case btn::Right: setPage(PG_ST0 + (s + 1) % kStreams); break;
            case btn::Copy:
                for (int p = 0; p < P_COUNT; ++p) clip_[p] = eng_.st[s].p[p];
                clipValid_ = true;
                ctx_->toast("Stream copied");
                break;
            case btn::Insert:
                if (clipValid_) {
                    for (int p = 0; p < P_COUNT; ++p)
                        if (pid_[s][p] != kit::kNoParam)
                            setParam(pid_[s][p], (p == P_DUR || p == P_START) ? float(clip_[p]) * 0.1f : float(clip_[p]));
                    ctx_->toast("Stream pasted");
                }
                break;
            case btn::Delete: {
                const ParamId id = pid_[s][kStreamOrder[streamRow_]];
                ctx_->params().reset(id);
                break;
            }
            case btn::F8: eng_.st[s].audition = 110; break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        nudge(pid_[s][kStreamOrder[streamRow_]], e.delta, e.shift);
    }
}

void Datamatics::dataEvent(const Event& e) {
    Slot& sl = eng_.slot[dataSlot_];
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: dataSlot_ = (dataSlot_ + kSlots - 1) % kSlots; break;
            case btn::Down: dataSlot_ = (dataSlot_ + 1) % kSlots; break;
            case btn::Left: dataCol_ = (dataCol_ + kStreams - 1) % kStreams; break;
            case btn::Right: dataCol_ = (dataCol_ + 1) % kStreams; break;
            case btn::Insert:
            case btn::Note: assign(dataCol_, dataSlot_); break;
            case btn::F8: openBrowser(dataSlot_); break;
            case btn::Delete:
                __atomic_store_n(&sl.n, 0u, __ATOMIC_RELEASE);
                sl.path[0] = 0;
                ++slotGen_[dataSlot_];
                computeMeta(dataSlot_, 0);
                break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        if (e.shift) {   // built-in data sets
            const int b = builtinIndex(sl.path);
            const int nb = ((b < 0 ? (e.delta > 0 ? -1 : 0) : b) + e.delta + kBuiltinCount * 4) % kBuiltinCount;
            loadBuiltinNow(dataSlot_, nb, sl.wantMode);
        } else {         // reading mode of the slot (the file is read again)
            reloadSlot(dataSlot_, (sl.wantMode + e.delta + kModeCount * 4) % kModeCount);
        }
    }
}

void Datamatics::songEvent(const Event& e) {
    Engine& g = eng_;
    Section& c = g.sec[secSel_ < g.secCount ? secSel_ : 0];
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: secSel_ = secSel_ > 0 ? secSel_ - 1 : 0; break;
            case btn::Right: secSel_ = secSel_ + 1 < g.secCount ? secSel_ + 1 : secSel_; break;
            case btn::Up: secField_ = (secField_ + 3) % 4; break;
            case btn::Down: secField_ = (secField_ + 1) % 4; break;
            case btn::Insert:
                if (g.secCount < kMaxSections) {
                    const int at = g.secCount ? secSel_ + 1 : 0;
                    for (int i = g.secCount; i > at; --i) g.sec[i] = g.sec[i - 1];
                    g.sec[at] = g.secCount ? g.sec[secSel_] : Section();
                    g.secCount = g.secCount + 1;
                    secSel_ = at;
                }
                break;
            case btn::Delete:
                if (g.secCount > 1) {
                    for (int i = secSel_; i + 1 < g.secCount; ++i) g.sec[i] = g.sec[i + 1];
                    g.secCount = g.secCount - 1;
                    if (secSel_ >= g.secCount) secSel_ = g.secCount - 1;
                    if (g.curSec >= g.secCount) g.jumpReq = 0;
                }
                break;
            case btn::F8:
                if (e.shift) setParam(songId_, float(g.songMode ^ 1));
                else launchSection(secSel_);
                break;
            case btn::Note:
                if (secField_ == 2) c.mask ^= 1 << maskBit_;
                break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        const int d = e.delta;
        switch (secField_) {
            case 0: c.type = (c.type + T_COUNT + d) % T_COUNT; break;
            case 1: c.bars = clampi(c.bars + d * (e.shift ? 4 : 1), 1, 64); break;
            case 2:   // mask: encoder walks the bit, Shift + encoder toggles it
                if (e.shift) c.mask ^= 1 << maskBit_;
                else maskBit_ = clampi(maskBit_ + d, 0, 7);
                break;
            default: c.depth = clampi(c.depth + d * (e.shift ? 10 : 1), 0, 100); break;
        }
    }
}

void Datamatics::mixerEvent(const Event& e) {
    static const uint8_t fields[] = {P_LEVEL, P_PAN, P_PANMODE, P_STEM, P_MIDI};
    constexpr int nf = int(sizeof fields);
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: sel_ = (sel_ + kStreams - 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Right: sel_ = (sel_ + 1) % kStreams; bank_ = sel_ / 4; break;
            case btn::Up: mixField_ = (mixField_ + nf - 1) % nf; break;
            case btn::Down: mixField_ = (mixField_ + 1) % nf; break;
            case btn::F8:
                if (e.shift) setParam(pid_[sel_][P_SOLO], float(eng_.st[sel_].p[P_SOLO] ^ 1));
                else setParam(pid_[sel_][P_MUTE], float(eng_.st[sel_].p[P_MUTE] ^ 1));
                break;
            case btn::Insert: eng_.st[sel_].audition = 110; break;
            case btn::Delete: ctx_->params().reset(pid_[sel_][fields[mixField_]]); break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        nudge(pid_[sel_][fields[mixField_]], e.delta, e.shift);
    }
}

void Datamatics::viewEvent(const Event& e) {
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Left: setParam(visId_, float((visMode_ + VM_COUNT - 1) % VM_COUNT)); break;
            case btn::Right: setParam(visId_, float((visMode_ + 1) % VM_COUNT)); break;
            case btn::Up: setParam(followId_, float(follow_ > 1 ? follow_ - 1 : 8)); break;
            case btn::Down: setParam(followId_, float(follow_ < 8 ? follow_ + 1 : 1)); break;
            case btn::F8: setParam(autoId_, float(visAuto_ ^ 1)); break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        setParam(visId_, float((visMode_ + e.delta + VM_COUNT * 8) % VM_COUNT));
    }
}

void Datamatics::pieceEvent(const Event& e) {
    char path[128];
    if (e.type == Event::kPress) {
        switch (e.id) {
            case btn::Up: pieceSel_ = pieceSel_ > 0 ? pieceSel_ - 1 : 0; delArm_ = false; break;
            case btn::Down: pieceSel_ = pieceSel_ + 1 < pieceCount_ ? pieceSel_ + 1 : pieceSel_; delArm_ = false; break;
            case btn::Insert: {
                if (e.shift) {
                    newPiece();
                    ctx_->toast("New piece (demo data)");
                    break;
                }
                if (!ctx_->fs) { ctx_->toast("No card"); break; }
                int n = 1;
                for (; n < 1000; ++n) {
                    std::snprintf(path, sizeof path, "%s/Pieces/DM-%03d.dmx", ctx_->home, n);
                    if (!ctx_->fs->exists(path)) break;
                }
                if (savePiece(path)) {
                    std::snprintf(pieceName_, sizeof pieceName_, "DM-%03d", n);
                    char m[48];
                    std::snprintf(m, sizeof m, "Saved %s", pieceName_);
                    ctx_->toast(m);
                } else {
                    ctx_->toast("Cannot write to the card");
                }
                scanPieces();
                break;
            }
            case btn::F8:
                if (pieceSel_ < pieceCount_) {
                    std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                    if (loadPiece(path)) {
                        copyStr(pieceName_, sizeof pieceName_, pieces_[pieceSel_]);
                        ctx_->toast("Piece loaded");
                    } else {
                        ctx_->toast("Not a DATAMATICS piece");
                    }
                }
                break;
            case btn::Copy:   // overwrite the selected piece
                if (pieceSel_ < pieceCount_) {
                    std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                    if (savePiece(path)) {
                        copyStr(pieceName_, sizeof pieceName_, pieces_[pieceSel_]);
                        ctx_->toast("Overwritten");
                    }
                }
                break;
            case btn::Delete:
                if (pieceSel_ < pieceCount_ && ctx_->fs) {
                    if (!delArm_) {
                        delArm_ = true;
                        ctx_->toast("Delete again to remove the file");
                    } else {
                        std::snprintf(path, sizeof path, "%s/Pieces/%s.dmx", ctx_->home, pieces_[pieceSel_]);
                        ctx_->fs->remove(path);
                        delArm_ = false;
                        scanPieces();
                    }
                }
                break;
            default: break;
        }
    } else if (e.type == Event::kEncoder) {
        pieceSel_ = clampi(pieceSel_ + e.delta, 0, pieceCount_ ? pieceCount_ - 1 : 0);
        delArm_ = false;
    }
}

// ---- bench ------------------------------------------------------------------------------
const char* Datamatics::benchName(int i) const {
    static const char* const n[] = {"demo", "dense_sine", "ms_clicks", "noise_wall", "max_events", "perform_throws"};
    return (i >= 0 && i < 6) ? n[i] : "?";
}

void Datamatics::benchSetup(int i) {
    eng_.defaults();
    eng_.reset();
    eng_.midiMode = 0;
    if (i == 0) return;
    eng_.arrange = 0;   // free run: every stream sounds
    for (int s = 0; s < kStreams; ++s) {
        int* p = eng_.st[s].p;
        p[P_SLOT] = 2;   // LFSR: dense random bytes
        p[P_UNIT] = U_BYTE;
        p[P_THR] = 0;
        p[P_MUTE] = p[P_SOLO] = 0;
        p[P_SHAPE] = SH_DECAY;
        p[P_PANMODE] = PM_DATA;
        p[P_PAN] = 100;
        p[P_LEVEL] = 30;
        p[P_SYNC] = 0;
        p[P_STEM] = s & 3;
        switch (i) {
            case 1:   // 8 streams x 4 sine voices always sounding
                p[P_SOUND] = S_SINE; p[P_RATE_MS] = 2; p[P_DUR] = 20000; p[P_LO] = 40; p[P_HI] = 18000;
                break;
            case 2:   // 1 ms clicks on every stream
                p[P_SOUND] = S_CLICK; p[P_RATE_MS] = 1; p[P_DUR] = 30; p[P_SHAPE] = SH_FADE;
                break;
            case 3:   // noise everywhere, long
                p[P_SOUND] = S_NOISE; p[P_RATE_MS] = 3; p[P_DUR] = 20000; p[P_LO] = 200; p[P_HI] = 20000;
                break;
            default:   // maximum event density: 1 ms grid, mixed sounds
                p[P_SOUND] = s & 3; p[P_RATE_MS] = 1; p[P_DUR] = 20000; p[P_DURMOD] = 100;
                break;
        }
    }
    if (i == 4) {
        eng_.arrange = 1;
        eng_.secCount = 1;
        eng_.sec[0] = Section{T_CUT, 64, 0xFF, 10};
    }
    if (i == 5) {   // the densest case with throws held: 1 ms grid, long voices, reverse + freeze + octave + dense, fills, scrub
        eng_.freezeWin = 7;
        eng_.throws = TH_REVERSE | TH_FREEZE | TH_OCT | TH_DENSE;
        eng_.fillMask = 0xFF;
        for (int s = 0; s < kSlots; ++s) eng_.scrub[s] = 0.37f;
    }
}

}  // namespace dm
