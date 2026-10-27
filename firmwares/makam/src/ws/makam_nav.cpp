// MAKAM x LABKIT v2: every song-wide value in the registry (PARAM pads, MAP,
// macros, LFO / random routes, MIDI learn, HUD), the pages for MAP / HELP,
// the modulation sources, the scenes (snapshots of the registry, morphed),
// and the project file's tagged blocks (registry chunk, scenes).
//
// The song keeps plain ints; the registry binds them by pointer. Values that
// rebuild the song (usul, makam, tuning, generator settings) are noMod():
// modulating them would regenerate the break or the phrase every block.
// Per-step / per-section values stay on their pages (they follow a cursor).
#include <cstdio>
#include <cstring>

#include "ws/gen.h"
#include "ws/ws.h"

namespace makam {

using namespace lab;
namespace kit = lab::kit;

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char* gUsulN[kUsulCount];
const char* gMakamN[kMakamCount];
const char* const kTuneN[] = {"AEU 24", "53-TET"};
const char* const kIntonN[] = {"Written", "Practice"};
const char* const kUnitN[] = {"1/16", "1/8"};
const char* const kOffOnN[] = {"Off", "On"};
const char* const kBassN[kBassModels] = {"Reese", "Sub", "Growl", "Talk", "FM", "Hoover"};
const char* const kLeadN[] = {"Ney", "Kanun", "Saz", "Zurna"};
const char* const kStartN[] = {"Seyir", "Karar", "Guclu", "Tiz durak"};
const char* const kFormN[] = {"Seyir", "Call+Resp"};
const char* const kSliceN[] = {"Equal", "Onsets"};
const char* const kResampleN[] = {"Cut only", "Follow"};
const char* const kDlyN[kDlyTypes] = {"Clean", "Tape", "Lo-fi", "Ping-pong"};
const char* const kRvbN[kRvbTypes] = {"Plate", "Hall", "Gated"};
const char* const kChN[4] = {"BREAK", "BASS", "LEAD", "FX"};

const Rgb kPageCol[kPages] = {Rgb(0x2FD0C0), Rgb(0xE9B949), Rgb(0xE5484D), Rgb(0x9B6BE0),
                              Rgb(0x2F5FD8), Rgb(0xF4A261), Rgb(0x2A9D6F), Rgb(0x8A97B8)};

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

// ---- registration ------------------------------------------------------------------------
kit::ParamId MakamWs::paramOf(const void* field) const {
    for (int i = 0; i < regCount_; ++i)
        if (regPtr_[i] == field) return regId_[i];
    return kit::kNoParam;
}

void MakamWs::registerParams() {
    for (int i = 0; i < kUsulCount; ++i) gUsulN[i] = kUsuls[i].name;
    for (int i = 0; i < kMakamCount; ++i) gMakamN[i] = kMakams[i].name;
    kit::Registry& P = ctx_->params();
    Song& s = *song_;
    auto add = [&](const char* key, const char* name, int* v) -> kit::Param& {
        kit::Param& p = P.add(key, name, v);
        if (regCount_ < kMaxReg) {
            regPtr_[regCount_] = v;
            regId_[regCount_++] = p.id();
        }
        return p;
    };
    P.setPage(kPgUsul);
    P.setGroup("USUL", col::kTurq);
    add("u.usul", "Usul", &s.usul).names(gUsulN, kUsulCount).noMod();
    add("u.swing", "Swing", &s.swing).range(0, 50).unit("%");
    add("u.unit", "Unit", &s.unit).names(kUnitN, 2).noMod();
    add("u.kit", "Kit", &s.kit).names(kKitNames, kKits + 1).noMod();

    P.setPage(kPgMakam);
    P.setGroup("MAKAM", col::kGold);
    add("m.makam", "Makam", &s.makam).names(gMakamN, kMakamCount).noMod();
    add("m.tuning", "Tuning", &s.tuning).names(kTuneN, 2).noMod();
    add("m.ahenk", "Ahenk", &s.transpose).range(-12, 12).bipolar().unit("st").noMod();
    add("m.inton", "Intonation", &s.intonation).names(kIntonN, 2).noMod();

    P.setPage(kPgBreak);
    P.setGroup("BREAK", col::kRed);
    add("b.pitch", "Pitch", &s.brkPitch).range(-12, 12).bipolar().unit("st");
    add("b.drive", "Drive", &s.drive).range(0, 100).unit("%");
    add("b.dens", "Edit dens", &s.editDensity).range(0, 100).unit("%").noMod();
    add("b.offs", "Cut offset", &s.brkStart).range(0, 15);
    add("b.slice", "Auto slice", &s.autoSlice).names(kSliceN, 2).noMod();
    add("b.tempo", "Tempo", &s.breakResample).names(kResampleN, 2).noMod();

    P.setPage(kPgMelody);
    P.setGroup("SEYIR", col::kViolet);
    add("g.dens", "Density", &s.gen.density).range(0, 100).unit("%").noMod();
    add("g.range", "Range", &s.gen.range).range(4, 14).noMod();
    add("g.orn", "Ornament", &s.gen.ornament).range(0, 100).unit("%").noMod();
    add("g.start", "Start", &s.gen.start).names(kStartN, 4).noMod();
    add("g.form", "Form", &s.gen.form).names(kFormN, 2).noMod();
    add("g.seed", "Seed", &s.gen.seed).range(1, 999).noMod();
    add("g.live", "Live", &s.gen.live).names(kOffOnN, 2).noMod();

    P.setPage(kPgSynth);
    P.setGroup("BASS", col::kRed);
    add("s.model", "Bass", &s.bp.model).names(kBassN, kBassModels).noMod();
    add("s.oct", "Octave", &s.bp.octave).range(-4, -1).noMod();
    add("s.det", "Detune", &s.bp.detune).range(0, 100);
    add("s.cut", "Cutoff", &s.bp.cutoff).range(0, 100);
    add("s.res", "Reso", &s.bp.reso).range(0, 100);
    add("s.env", "Env", &s.bp.env).range(0, 100);
    add("s.dec", "Decay", &s.bp.decay).range(0, 100);
    add("s.drv", "Drive", &s.bp.drive).range(0, 100);
    add("s.gli", "Glide", &s.bp.glide).range(0, 100);
    add("s.sub", "Sub", &s.bp.sub).range(0, 100);
    add("s.mot", "Motion", &s.bp.motion).range(0, 100);
    P.setGroup("LEAD", col::kTurq);
    add("l.model", "Lead", &s.lp.model).names(kLeadN, 4).noMod();
    add("l.oct", "Octave", &s.lp.octave).range(-2, 1).noMod();
    add("l.tone", "Tone", &s.lp.tone).range(0, 100);
    add("l.dec", "Decay", &s.lp.decay).range(0, 100);
    add("l.vib", "Vibrato", &s.lp.vibrato).range(0, 100);
    add("l.gli", "Glide", &s.lp.glide).range(0, 100);

    P.setPage(kPgMixer);
    static const uint32_t kChCol[4] = {0xE9B949, 0xE5484D, 0x2FD0C0, 0x9B6BE0};
    static char keys[4][5][8];
    static const char* const kField[5] = {"lvl", "pan", "dly", "rvb", "mute"};
    static const char* const kFieldN[5] = {"Level", "Pan", "Delay", "Reverb", "Mute"};
    for (int c = 0; c < 4; ++c) {
        P.setGroup(kChN[c], Rgb(kChCol[c]));
        int* f[5] = {&s.ch[c].level, &s.ch[c].pan, &s.ch[c].dly, &s.ch[c].rvb, &s.ch[c].mute};
        for (int k = 0; k < 5; ++k) {
            if (c == 3 && (k == 2 || k == 3)) continue;   // the FX return has no sends
            std::snprintf(keys[c][k], sizeof keys[c][k], "x%d%s", c, kField[k]);
            kit::Param& p = add(keys[c][k], kFieldN[k], f[k]);
            if (k == 1) p.range(-100, 100).bipolar();
            else if (k == 4) p.names(kOffOnN, 2).noMod();
            else p.range(0, 100);
        }
    }
    P.setGroup("SENDS", col::kCobalt);
    add("f.dtype", "Delay", &s.dlyType).names(kDlyN, kDlyTypes).noMod();
    add("f.dtime", "Time", &s.dlyTime).names(kDelaySyncNames, kDelaySyncCount).noMod();
    add("f.dfb", "Feedback", &s.dlyFb).range(0, 95).unit("%");
    add("f.rtype", "Reverb", &s.rvbType).names(kRvbN, kRvbTypes).noMod();
    add("f.rsize", "Size", &s.rvbSize).range(0, 100);
    add("f.rdamp", "Damping", &s.rvbDamp).range(0, 100);
    add("f.duck", "Dum duck", &s.duck).range(0, 100);
    add("f.filt", "DJ filter", &s.djFilter).range(-100, 100).bipolar();
    add("f.morph", "Morph", &s.morphCycles).range(0, 16).unit("cyc").noMod();

    // the scene set: every continuous (modulatable) parameter
    sceneCount_ = 0;
    for (int i = 0; i < regCount_ && sceneCount_ < kSceneParams; ++i)
        if (P.modulatable(regId_[i])) sceneIds_[sceneCount_++] = regId_[i];

    // modulation sources (Shift + a bottom pad on the PARAM pads)
    kit::Mod& M = ctx_->mod();
    M.addSource("USUL", &eng_.srcUsul);
    M.addSource("DUM", &eng_.srcDum);
    M.addSource("TEK", &eng_.srcTek);
    M.addSource("SLICE", &eng_.srcSlice);
    M.addSource("ENERGY", &eng_.srcEnergy);
    M.addSource("SEYIR", &eng_.srcSeyir);
    M.addSource("SECTION", &eng_.srcSection);
    M.addSource("FILL", &eng_.srcFill);
}

// The song's fields were written directly (load, new song, generators, the
// bench): make them the registry's base values. A modulated param's field
// holds base + modulation, so it is skipped (its base comes from the
// registry chunk of a project, or stays).
void MakamWs::pullFields() {
    kit::Registry& P = ctx_->params();
    for (int i = 0; i < regCount_; ++i) {
        if (P.meta(regId_[i]).mods) continue;
        const int v = *static_cast<const int*>(regPtr_[i]);
        if (int(P.get(regId_[i]) + (P.get(regId_[i]) < 0 ? -0.5f : 0.5f)) != v) P.set(regId_[i], float(v), false);
    }
}

// First-boot macros (a loaded project brings its own).
void MakamWs::defaultMacros() {
    kit::Mod& M = ctx_->mod();
    Song& s = *song_;
    M.setMacroName(0, "DUB");
    M.route(kit::kSrcMacro + 0, paramOf(&s.ch[0].dly), 0.6f);
    M.route(kit::kSrcMacro + 0, paramOf(&s.ch[2].dly), 0.4f);
    M.route(kit::kSrcMacro + 0, paramOf(&s.dlyFb), 0.35f);
    M.setMacroName(1, "SWEEP");
    const int i = M.route(kit::kSrcMacro + 1, paramOf(&s.djFilter), 1.0f);
    if (i >= 0) M.setWindow(i, true, 0.5f, 0.08f);   // centre -> deep low-pass
    M.setMacroName(2, "NEURO");
    M.route(kit::kSrcMacro + 2, paramOf(&s.bp.motion), 0.6f);
    M.route(kit::kSrcMacro + 2, paramOf(&s.bp.cutoff), 0.4f);
    M.route(kit::kSrcMacro + 2, paramOf(&s.bp.drive), 0.3f);
    M.setMacroName(3, "SPACE");
    M.route(kit::kSrcMacro + 3, paramOf(&s.ch[2].rvb), 0.6f);
    M.route(kit::kSrcMacro + 3, paramOf(&s.rvbSize), 0.4f);
}

// ---- pages -----------------------------------------------------------------------------------
int MakamWs::navPages(kit::PageInfo* out, int cap) {
    static const char* const kNames[kPages] = {"USUL", "MAKAM", "BREAK", "SEYIR", "SYNTH", "ARRANGE", "MIXER", "FILE"};
    static const char* const kHelp[kPages] = {
        "The usul dial: cycle, tempo, swing, kit. Pads row 1: the strokes.",
        "The makam ring: makam, tuning (AEU / 53), ahenk, practice intonation.",
        "Break slicer: steps, slices, rolls; bottom row held = performance edits.",
        "Seyir melody generator (Insert = new phrase); pads edit the phrase.",
        "Bass (Reese Sub Growl Talk FM Hoover) and lead (Ney Kanun Saz Zurna).",
        "Sections on a ring: cut mode, phrase-end fills, scene, chaos, filter.",
        "Four channels, dub delay (Clean Tape Lo-fi Ping), reverb (Plate Hall Gated).",
        "Songs on the card, breaks (the browser previews them).",
    };
    static const char* const kAct[kPages] = {
        "Insert: re-cut variation", "Insert: bass + phrases follow", "Insert: re-cut variation",
        "Insert: generate phrase",  "Insert: -",                     "Insert: play the section",
        "Insert: -",                "Insert: load  Rec: save",
    };
    int n = 0;
    for (int i = 0; i < kPages && n < cap; ++i, ++n) {
        kit::PageInfo& p = out[n];
        p.name = kNames[i];
        p.help = kHelp[i];
        p.color = kPageCol[i];
        for (int k = 0; k < 8; ++k) p.fkeys[k] = nullptr;
        p.fkeys[0] = "F1-F8: the eight pages";
        p.fkeys[1] = kAct[i];
        p.fkeys[2] = "Song: arrangement / loop the section";
        p.fkeys[3] = "Perform + pad 1-8 (row 1): scene, + Shift: store";
        p.fkeys[4] = "Shift + Insert: add (section, glissando)";
    }
    return n;
}

void MakamWs::navFocus(kit::ParamId id) {
    if (!ctx_->params().valid(id)) return;
    const void* ptr = ctx_->params().binding(id).ptr;
    for (int c = 0; c < 4; ++c) {   // a channel param: show that channel on the mixer
        const uint8_t* a = reinterpret_cast<const uint8_t*>(&song_->ch[c]);
        if (ptr >= a && ptr < a + sizeof(Channel)) mixCol_ = c;
    }
    const int pg = ctx_->params().page(id);
    if (pg >= 0 && pg < kPages) page_ = pg;
    if (pg == kPgMixer && (ptr == &song_->dlyType || ptr == &song_->dlyTime || ptr == &song_->dlyFb ||
                           ptr == &song_->rvbType || ptr == &song_->rvbSize || ptr == &song_->rvbDamp ||
                           ptr == &song_->duck || ptr == &song_->djFilter || ptr == &song_->morphCycles))
        mixCol_ = 4;
    rowCount_ = buildRows(page_, rows_, kMaxRows);
    for (int i = 0; i < rowCount_; ++i)
        if (rows_[i].v == ptr) row_[page_] = i;
}

uint8_t MakamWs::ridOf(const void* f) const {
    const Song& s = *song_;
    if (f == &s.usul) return kRidUsul;
    if (f == &s.makam) return kRidMakam;
    if (f == &s.tuning || f == &s.transpose || f == &s.intonation) return kRidTuning;
    if (f == &s.kit) return kRidKit;
    if (f == &s.songMode) return kRidSongMode;
    if (f == &s.gen.density || f == &s.gen.range || f == &s.gen.ornament || f == &s.gen.start || f == &s.gen.seed ||
        f == &s.gen.form)
        return kRidGen;
    if (f == &s.autoSlice) return kRidAutoSlice;
    return kRidNone;
}

void MakamWs::navParamChanged(kit::ParamId id) {
    if (!ctx_->params().valid(id)) return;
    applyRid(ridOf(ctx_->params().binding(id).ptr));
}

// ---- scenes ----------------------------------------------------------------------------------
void MakamWs::storeScene(int i) {
    if (i < 0 || i >= kScenes || !scene_) return;
    kit::Registry& P = ctx_->params();
    for (int k = 0; k < sceneCount_; ++k) scene_[i * kSceneParams + k] = P.norm(sceneIds_[k]);
    sceneUsed_[i] = true;
}

void MakamWs::recallScene(int i, bool morph) {
    if (!sceneUsed(i)) return;
    kit::Registry& P = ctx_->params();
    for (int k = 0; k < sceneCount_; ++k) morphFrom_[k] = P.norm(sceneIds_[k]);
    morphTo_ = i;
    const float cyc = float(song_->morphCycles);
    const float unitBeats = song_->unit ? 0.5f : 0.25f;
    const float bpm = ctx_->shell ? ctx_->bpm() : float(song_->bpm);
    morphMs_ = morph ? uint32_t(cyc * float(usulLength(song_->usul)) * unitBeats * 60000.0f / (bpm > 20.0f ? bpm : 120.0f)) : 0;
    morphStart_ = ctx_->millis();
    morphOn_ = true;
    lastMorphMs_ = 0;
    updateMorph(morphStart_);
}

void MakamWs::updateMorph(uint32_t ms) {
    if (!morphOn_ || morphTo_ < 0) return;
    if (ms - lastMorphMs_ < 20 && morphMs_ && ms - morphStart_ < morphMs_) return;
    lastMorphMs_ = ms;
    float f = morphMs_ ? float(ms - morphStart_) / float(morphMs_) : 1.0f;
    if (f >= 1.0f) {
        f = 1.0f;
        morphOn_ = false;
    }
    kit::Registry& P = ctx_->params();
    const float* to = scene_ + morphTo_ * kSceneParams;
    for (int k = 0; k < sceneCount_; ++k) {
        const float v = morphFrom_[k] + (to[k] - morphFrom_[k]) * f;
        if (v != P.norm(sceneIds_[k])) P.setNorm(sceneIds_[k], v, false);
    }
}

// ---- project file blocks ---------------------------------------------------------------------
size_t MakamWs::saveTrailers(uint8_t* out, size_t cap) const {
    size_t n = 0;
    const kit::Registry& P = ctx_->params();
    const uint32_t need = P.chunkSize();
    if (n + 8 + need <= cap) {
        const uint32_t w = P.saveChunk(out + n + 8, uint32_t(cap - n - 8));
        if (w) {
            std::memcpy(out + n, "LKIT", 4);
            put32(out + n + 4, w);
            n += 8 + w;
        }
    }
    // scenes: [count u8][per scene: used u8, n u16, n x (hash u32, norm f32)]
    size_t sz = 1;
    for (int i = 0; i < kScenes; ++i) sz += 3 + (sceneUsed_[i] ? size_t(sceneCount_) * 8 : 0);
    if (scene_ && n + 8 + sz <= cap) {
        std::memcpy(out + n, "SCEN", 4);
        put32(out + n + 4, uint32_t(sz));
        uint8_t* p = out + n + 8;
        *p++ = kScenes;
        for (int i = 0; i < kScenes; ++i) {
            *p++ = sceneUsed_[i] ? 1 : 0;
            const int cnt = sceneUsed_[i] ? sceneCount_ : 0;
            *p++ = uint8_t(cnt & 0xFF);
            *p++ = uint8_t(cnt >> 8);
            for (int k = 0; k < cnt; ++k) {
                put32(p, P.meta(sceneIds_[k]).key);
                std::memcpy(p + 4, &scene_[i * kSceneParams + k], 4);
                p += 8;
            }
        }
        n += 8 + sz;
    }
    return n;
}

void MakamWs::loadTrailers(const uint8_t* data, size_t len) {
    kit::Registry& P = ctx_->params();
    for (int i = 0; i < kScenes; ++i) sceneUsed_[i] = false;
    size_t at = 0;
    while (at + 8 <= len) {
        const uint32_t sz = get32(data + at + 4);
        if (at + 8 + sz > len) break;
        const uint8_t* b = data + at + 8;
        if (std::memcmp(data + at, "LKIT", 4) == 0) {
            P.loadChunk(b, sz);
        } else if (std::memcmp(data + at, "SCEN", 4) == 0 && sz >= 1 && scene_) {
            const uint8_t* p = b;
            const uint8_t* end = b + sz;
            const int cnt = *p++;
            for (int i = 0; i < cnt && i < kScenes && p + 3 <= end; ++i) {
                const bool used = *p++ != 0;
                const int n = p[0] | (p[1] << 8);
                p += 2;
                for (int k = 0; k < sceneCount_; ++k) scene_[i * kSceneParams + k] = P.norm(sceneIds_[k]);
                for (int k = 0; k < n && p + 8 <= end; ++k, p += 8) {
                    const kit::ParamId id = P.findHash(get32(p));
                    float v;
                    std::memcpy(&v, p + 4, 4);
                    for (int j = 0; j < sceneCount_; ++j)
                        if (sceneIds_[j] == id) scene_[i * kSceneParams + j] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                }
                sceneUsed_[i] = used;
            }
        }
        at += 8 + sz;
    }
}

// ---- break browser -----------------------------------------------------------------------------
void MakamWs::openBreakBrowser() {
    if (!ctx_->fs) {
        ctx_->toast("No SD card");
        return;
    }
    kit::BrowserRequest q;
    q.title = "Break WAV";
    q.folder = "Lab/MAKAM/Breaks";
    q.exts = ".wav";
    q.ctx = this;
    q.onPick = [](void* c, const char* path) {
        MakamWs* w = static_cast<MakamWs*>(c);
        copyStr(w->pendingWav_, sizeof w->pendingWav_, path);
        w->pendingWavReady_ = true;   // loaded in update() (main loop)
    };
    ctx_->browser().open(q);
}

}  // namespace makam
