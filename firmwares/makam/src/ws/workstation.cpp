// MAKAM-JUNGLE -- workstation logic: pages, rows, pads, keys, MIDI, files,
// LEDs, bench scenarios (drawing lives in ui.cpp).
#include <cstdio>
#include <cstring>

#include "ws/gen.h"
#include "ws/ws.h"

namespace makam {

using namespace lab;

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char* gUsulNames[kUsulCount];
const char* gMakamNames[kMakamCount];
const char* const kTuneNames[] = {"AEU 24", "53-TET"};
const char* const kUnitNames[] = {"1/16", "1/8"};
const char* const kOffOn[] = {"Off", "On"};
const char* const kVarNames[] = {"A", "B", "C", "D"};
const char* const kVarOff[] = {"Off", "A", "B", "C", "D"};
const char* const kStrokeNames[] = {"Rest", "Dum", "Tek", "Ke"};
const char* const kSliceNames[] = {"Auto", "1", "2", "3", "4", "5", "6", "7", "8",
                                   "9", "10", "11", "12", "13", "14", "15", "16"};
const char* const kBassModels[] = {"Reese", "Sub", "Growl"};
const char* const kLeadModels[] = {"Ney", "Kanun", "Saz", "Zurna"};
const char* const kStartNames[] = {"Seyir", "Karar", "Guclu", "Tiz durak"};
const char* const kSongMode[] = {"Loop section", "Arrangement"};
const char* const kResample[] = {"Cut only", "Follow tempo"};
const char* const kMidiOut[] = {"Off", "Bass+Lead"};

// Performance pads (Break page, bottom row).
enum Perf { kPfRoll2, kPfRoll3, kPfRoll4, kPfRoll6, kPfRev, kPfStut, kPfHalf, kPfMute, kPfChaos, kPfDum, kPfTek, kPfKe };
}  // namespace

// ---------------------------------------------------------------------------
bool MakamWs::init(Context& c) {
    ctx_ = &c;
    theme_.bg = col::kNight;
    theme_.panel = col::kPanel;
    theme_.fg = col::kCream;
    theme_.dim = col::kDim;
    theme_.accent = col::kTurq;
    theme_.accent2 = col::kGold;
    theme_.warn = col::kRed;
    theme_.ok = col::kEmerald;
    for (int i = 0; i < kUsulCount; ++i) gUsulNames[i] = kUsuls[i].name;
    for (int i = 0; i < kMakamCount; ++i) gMakamNames[i] = kMakams[i].name;

    Arena& a = *c.arena;
    song_ = a.make<Song>();
    scratch_ = static_cast<float*>(a.alloc(kKitFrames * sizeof(float)));
    fileBuf_ = static_cast<uint8_t*>(a.alloc(kFileMax));
    if (!song_ || !scratch_ || !fileBuf_) return false;
    for (int k = 0; k <= kKits; ++k) {
        const uint32_t cap = k == kKits ? kWavFrames : kKitFrames;
        kits_[k].data = static_cast<int16_t*>(a.alloc(cap * sizeof(int16_t)));
        kits_[k].cap = cap;
        if (!kits_[k].data) return false;
        if (k < kKits) synthKit(kits_[k], k, scratch_);
    }
    if (!eng_.init(a, ctx_)) return false;
    songDefaults(*song_);
    eng_.song = song_;
    eng_.kits = kits_;

    if (c.fs) {
        char p[96];
        std::snprintf(p, sizeof p, "%s/Projects", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/Breaks", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/project.mkm", c.home);
        loadProject(p);
        scanFiles();
    }
    if (c.shell) c.setBpm(float(song_->bpm));
    lastBpm_ = song_->bpm;
    return true;
}

// ---- files -------------------------------------------------------------------
void MakamWs::slotPath(int slot, char* buf, int cap) const {
    std::snprintf(buf, size_t(cap), "%s/Projects/SONG%d.mkm", ctx_->home, slot + 1);
}

bool MakamWs::saveProject(const char* path) {
    if (!ctx_ || !ctx_->fs) return false;
    const size_t n = songSave(*song_, fileBuf_, kFileMax);
    if (!n) return false;
    const IStorage::Handle h = ctx_->fs->open(path, IStorage::kWrite);
    if (h < 0) return false;
    const bool ok = ctx_->fs->write(h, fileBuf_, uint32_t(n)) == int32_t(n);
    ctx_->fs->close(h);
    return ok;
}

bool MakamWs::loadProject(const char* path) {
    if (!ctx_ || !ctx_->fs) return false;
    const IStorage::Handle h = ctx_->fs->open(path, IStorage::kRead);
    if (h < 0) return false;
    uint32_t got = 0;
    for (;;) {
        const int32_t r = ctx_->fs->read(h, fileBuf_ + got, kFileMax - got);
        if (r <= 0) break;
        got += uint32_t(r);
        if (got >= kFileMax) break;
    }
    ctx_->fs->close(h);
    eng_.reset();
    if (!songLoad(*song_, fileBuf_, got)) return false;
    if (song_->kit == kKits) {
        if (!song_->wav[0] || !loadWav(song_->wav)) song_->kit = 0;
    }
    if (ctx_->shell) ctx_->setBpm(float(song_->bpm));
    lastBpm_ = song_->bpm;
    return true;
}

bool MakamWs::loadWav(const char* name) {
    if (!ctx_ || !ctx_->fs || !name || !name[0]) return false;
    char p[128];
    std::snprintf(p, sizeof p, "%s/Breaks/%s", ctx_->home, name);
    eng_.wavReady = false;
    const bool ok = loadWavKit(kits_[kKits], *ctx_->fs, p);
    if (ok) {
        copyStr(song_->wav, sizeof song_->wav, name);
        eng_.wavReady = true;
    }
    return ok;
}

void MakamWs::scanFiles() {
    wavCount_ = 0;
    for (int i = 0; i < 8; ++i) slotUsed_[i] = false;
    if (!ctx_->fs) return;
    char p[96];
    for (int i = 0; i < 8; ++i) {
        slotPath(i, p, sizeof p);
        slotUsed_[i] = ctx_->fs->exists(p);
    }
    std::snprintf(p, sizeof p, "%s/Breaks", ctx_->home);
    ctx_->fs->listDir(
        p,
        [](const IStorage::DirEntry& e, void* x) {
            MakamWs* w = static_cast<MakamWs*>(x);
            if (e.isDir || !endsWithCi(e.name, ".wav") || e.name[0] == '.') return true;
            if (w->wavCount_ >= kMaxWavs) return false;
            copyStr(w->wavs_[w->wavCount_++], sizeof w->wavs_[0], e.name);
            return true;
        },
        this);
}

void MakamWs::save() {
    if (!ctx_ || !ctx_->fs || benchMode_) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/project.mkm", ctx_->home);
    saveProject(p);
}

void MakamWs::selectKit(int k) {
    if (k == kKits && !eng_.wavReady) {
        if (wavCount_ > 0 && loadWav(wavs_[0])) {
            song_->kit = kKits;
            ctx_->toast("Card break loaded");
        } else {
            song_->kit = 0;
            ctx_->toast("No WAV in /Lab/MAKAM/Breaks");
        }
        return;
    }
    song_->kit = clampi(k, 0, kKits);
}

// ---- settings rows (Config page) ---------------------------------------------------
int MakamWs::settings(SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, const char* unit) {
        if (n >= cap) return;
        SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.unit = unit;
    };
    add("MAKAM tuning", &song_->tuning, 0, 1, kTuneNames, "");
    add("MAKAM ahenk (transpose)", &song_->transpose, -12, 12, nullptr, " st");
    add("MAKAM usul unit", &song_->unit, 0, 1, kUnitNames, "");
    add("MAKAM MIDI notes out", &song_->midiOut, 0, 1, kMidiOut, "");
    add("MAKAM MIDI bend range", &song_->midiBend, 1, 24, nullptr, " st");
    add("MAKAM break tempo", &song_->breakResample, 0, 1, kResample, "");
    return n;
}

// ---- page rows -----------------------------------------------------------------------
int MakamWs::buildRows(int page, PRow* rows, int cap) {
    int n = 0;
    Song& s = *song_;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, uint8_t id = kRidNone) {
        if (n >= cap) return;
        rows[n++] = PRow{label, v, mn, mx, names, id};
    };
    switch (page) {
        case kPgUsul:
            add("Usul", &s.usul, 0, kUsulCount - 1, gUsulNames, kRidUsul);
            add("Tempo", &s.bpm, 60, 220, nullptr, kRidBpm);
            add("Unit", &s.unit, 0, 1, kUnitNames);
            add("Swing", &s.swing, 0, 50, nullptr);
            add("Break kit", &s.kit, 0, kKits, kKitNames, kRidKit);
            add("Edit variation", &s.editVar, 0, kVars - 1, kVarNames, kRidVar);
            add("Play", &s.songMode, 0, 1, kSongMode, kRidSongMode);
            break;
        case kPgMakam:
            add("Makam", &s.makam, 0, kMakamCount - 1, gMakamNames, kRidMakam);
            add("Tuning", &s.tuning, 0, 1, kTuneNames, kRidTuning);
            add("Ahenk", &s.transpose, -12, 12, nullptr, kRidTuning);
            break;
        case kPgBreak: {
            BreakStep& b = s.brk[s.editVar].s[clampi(stepSel_, 0, kSteps - 1)];
            add("Kit", &s.kit, 0, kKits, kKitNames, kRidKit);
            add("Variation", &s.editVar, 0, kVars - 1, kVarNames, kRidVar);
            add("Stroke", &b.stroke, 0, 3, kStrokeNames, kRidBreakEdit);
            add("Slice", &b.slice, -1, kSlices - 1, kSliceNames, kRidBreakEdit);
            add("Roll", &b.roll, 1, 4, nullptr, kRidBreakEdit);
            add("Pitch", &b.pitch, -12, 12, nullptr, kRidBreakEdit);
            add("Reverse", &b.rev, 0, 1, kOffOn, kRidBreakEdit);
            add("Stutter", &b.stutter, 0, 1, kOffOn, kRidBreakEdit);
            add("Choke", &b.choke, 0, 1, kOffOn, kRidBreakEdit);
            add("Edit density", &s.editDensity, 0, 100, nullptr);
            add("Drive", &s.drive, 0, 100, nullptr);
            break;
        }
        case kPgMelody: {
            Phrase& p = s.phr[s.editPhrase];
            add("Phrase", &s.editPhrase, 0, kPhrases - 1, kVarNames, kRidPhrase);
            add("Cycles", &p.cycles, 1, 4, nullptr, kRidPhraseCycles);
            add("Density", &s.gen.density, 0, 100, nullptr, kRidGen);
            add("Range", &s.gen.range, 4, 14, nullptr, kRidGen);
            add("Ornament", &s.gen.ornament, 0, 100, nullptr, kRidGen);
            add("Start", &s.gen.start, 0, 3, kStartNames, kRidGen);
            add("Seed", &s.gen.seed, 1, 999, nullptr, kRidGen);
            add("Live", &s.gen.live, 0, 1, kOffOn);
            break;
        }
        case kPgSynth:
            add("Bass", &s.bp.model, 0, 2, kBassModels);
            add("Octave", &s.bp.octave, -4, -1, nullptr);
            add("Detune", &s.bp.detune, 0, 100, nullptr);
            add("Cutoff", &s.bp.cutoff, 0, 100, nullptr);
            add("Reso", &s.bp.reso, 0, 100, nullptr);
            add("Env", &s.bp.env, 0, 100, nullptr);
            add("Decay", &s.bp.decay, 0, 100, nullptr);
            add("Drive", &s.bp.drive, 0, 100, nullptr);
            add("Glide", &s.bp.glide, 0, 100, nullptr);
            add("Lead", &s.lp.model, 0, 3, kLeadModels);
            add("Octave", &s.lp.octave, -2, 1, nullptr);
            add("Tone", &s.lp.tone, 0, 100, nullptr);
            add("Decay", &s.lp.decay, 0, 100, nullptr);
            add("Vibrato", &s.lp.vibrato, 0, 100, nullptr);
            add("Glide", &s.lp.glide, 0, 100, nullptr);
            break;
        case kPgArrange: {
            Section& x = s.sec[clampi(secSel_, 0, kSections - 1)];
            add("Play", &s.songMode, 0, 1, kSongMode, kRidSongMode);
            add("Section", &secSel_, 0, s.sectionCount - 1, kSliceNames + 1, kRidSectionSel);
            add("Type", &x.type, 0, kSecTypes - 1, kSectionNames);
            add("Cycles", &x.cycles, 1, 64, nullptr);
            add("Break", &x.brk, 0, 4, kVarOff);
            add("Bass", &x.bass, 0, 4, kVarOff);
            add("Lead", &x.lead, 0, 4, kVarOff);
            add("Chaos", &x.chaos, 0, 100, nullptr);
            add("Filter", &x.filter, -100, 100, nullptr);
            break;
        }
        case kPgMixer:
            if (mixCol_ < 4) {
                Channel& c = s.ch[mixCol_];
                add("Level", &c.level, 0, 100, nullptr);
                add("Pan", &c.pan, -100, 100, nullptr);
                add("Delay", &c.dly, 0, 100, nullptr);
                add("Reverb", &c.rvb, 0, 100, nullptr);
                add("Mute", &c.mute, 0, 1, kOffOn);
            } else {
                add("Delay time", &s.dlyTime, 0, kDelaySyncCount - 1, kDelaySyncNames);
                add("Feedback", &s.dlyFb, 0, 95, nullptr);
                add("Reverb size", &s.rvbSize, 0, 100, nullptr);
                add("Damping", &s.rvbDamp, 0, 100, nullptr);
                add("Dum duck", &s.duck, 0, 100, nullptr);
                add("DJ filter", &s.djFilter, -100, 100, nullptr);
            }
            break;
        default: break;
    }
    return n;
}

void MakamWs::rowChanged(const PRow& r) {
    Song& s = *song_;
    switch (r.id) {
        case kRidBpm: ctx_->setBpm(float(s.bpm)); lastBpm_ = s.bpm; break;
        case kRidUsul:
            songApplyUsul(s);
            songApplyMakam(s);
            stepSel_ = clampi(stepSel_, 0, usulLength(s.usul) - 1);
            phraseCur_ = 0;
            break;
        case kRidMakam:
            for (int v = 0; v < kVars; ++v) genBass(s, v);
            songApplyMakam(s);
            break;
        case kRidKit: selectKit(s.kit); break;
        case kRidGen:
        case kRidPhraseCycles:
            genPhrase(kMakams[s.makam], s.gen, s.usul, s.phr[s.editPhrase], uint32_t(s.gen.seed * 31 + s.editPhrase * 977));
            break;
        case kRidPhrase: phraseCur_ = 0; break;
        case kRidSectionSel:
            if (!s.songMode) s.curSection = secSel_;
            break;
        case kRidSongMode:
            if (!s.songMode) s.curSection = clampi(secSel_, 0, s.sectionCount - 1);
            eng_.reset();
            break;
        default: break;
    }
}

void MakamWs::setPage(int p) {
    page_ = clampi(p, 0, kPages - 1);
    if (page_ == kPgFile) scanFiles();
}

void MakamWs::encoder(int delta, bool shift) {
    if (page_ == kPgFile) {
        const int cnt = fileCol_ == 0 ? 8 : (wavCount_ ? wavCount_ : 1);
        fileSel_[fileCol_] = clampi(fileSel_[fileCol_] + delta, 0, cnt - 1);
        return;
    }
    rowCount_ = buildRows(page_, rows_, kMaxRows);
    if (!rowCount_) return;
    int& sel = row_[page_];
    sel = clampi(sel, 0, rowCount_ - 1);
    PRow& r = rows_[sel];
    if (!r.v) return;
    const int range = r.max - r.min;
    const int step = (shift && !r.names && range > 20) ? 10 : 1;
    const int v = clampi(*r.v + delta * step, r.min, r.max);
    if (v == *r.v) return;
    *r.v = v;
    rowChanged(r);
}

void MakamWs::action(bool shift) {
    Song& s = *song_;
    switch (page_) {
        case kPgUsul:
        case kPgBreak:
            genBreakVar(s, s.editVar, ctx_->millis() | 1u);
            ctx_->toast("Break variation re-cut");
            break;
        case kPgMelody:
            s.gen.seed = s.gen.seed % 999 + 1;
            genPhrase(kMakams[s.makam], s.gen, s.usul, s.phr[s.editPhrase], uint32_t(s.gen.seed * 31 + s.editPhrase * 977));
            ctx_->toast("Seyir phrase generated");
            break;
        case kPgMakam:
            for (int v = 0; v < kVars; ++v) genBass(s, v);
            songApplyMakam(s);
            ctx_->toast("Bass and phrases follow the makam");
            break;
        case kPgArrange:
            if (!s.songMode) s.curSection = secSel_;
            break;
        case kPgFile: {
            char p[96];
            if (fileCol_ == 0) {
                slotPath(fileSel_[0], p, sizeof p);
                if (shift) {
                    ctx_->toast(saveProject(p) ? "Song saved" : "Cannot save (card?)");
                    scanFiles();
                } else {
                    ctx_->toast(loadProject(p) ? "Song loaded" : "Empty slot or bad file");
                }
            } else if (wavCount_ > 0) {
                if (loadWav(wavs_[fileSel_[1]])) {
                    s.kit = kKits;
                    ctx_->toast("Break loaded and sliced");
                } else {
                    ctx_->toast("Cannot read that WAV");
                }
            }
            break;
        }
        default: break;
    }
}

// ---- pads ------------------------------------------------------------------------------
int MakamWs::padDegree(int pad) const {
    const int r = padRow(pad), c = padCol(pad);
    static const int kRowShift[4] = {14, 7, 0, 0};
    return c - 1 + kRowShift[r];
}
bool MakamWs::padIsLead(int pad) const { return padRow(pad) < 3; }

void MakamWs::breakStepPad(int step, bool shift) {
    Song& s = *song_;
    if (step >= usulLength(s.usul)) return;
    BreakStep& b = s.brk[s.editVar].s[step];
    if (page_ == kPgBreak && !shift) {
        stepSel_ = step;
        return;
    }
    b.stroke = (b.stroke + 1) & 3;
    stepSel_ = step;
}

void MakamWs::melodyWrite(int deg) {
    Song& s = *song_;
    Phrase& p = s.phr[s.editPhrase];
    const int n = phraseSteps(p, s.usul);
    phraseCur_ = clampi(phraseCur_, 0, n - 1);
    p.s[phraseCur_] = NoteStep{deg, kNoteOn};
    phraseCur_ = (phraseCur_ + 1) % n;
}

void MakamWs::padDown(int pad, int vel, bool shift) {
    Song& s = *song_;
    const int r = padRow(pad), c = padCol(pad);
    padHeld_[pad] = true;
    bool play = false;
    switch (page_) {
        case kPgUsul:
            if (r == 0) breakStepPad(c, true);
            else play = true;
            break;
        case kPgBreak:
            if (r == 0) {
                breakStepPad(c, shift);
            } else if (r == 1 || (r == 2 && c < 4)) {
                const int slice = r == 1 ? c : 12 + c;
                if (shift && stepSel_ < usulLength(s.usul)) {
                    BreakStep& b = s.brk[s.editVar].s[stepSel_];
                    b.slice = slice;
                    if (!b.stroke) b.stroke = kTek;
                }
                LiveEv e;
                e.type = LiveEv::kSlice;
                e.deg = int8_t(slice);
                e.vel = uint8_t(vel);
                eng_.live.push(e);
            } else if (r == 2 && c < 8) {
                s.editVar = c - 4;
            } else if (r == 2) {
                selectKit(c - 8);
            } else {   // performance row
                switch (c) {
                    case kPfRoll2: eng_.perfRoll = 2; break;
                    case kPfRoll3: eng_.perfRoll = 3; break;
                    case kPfRoll4: eng_.perfRoll = 4; break;
                    case kPfRoll6: eng_.perfRoll = 6; break;
                    case kPfRev: eng_.perfRev = true; break;
                    case kPfStut: eng_.perfStutter = true; break;
                    case kPfHalf: eng_.perfHalf = true; break;
                    case kPfMute: eng_.perfMute = true; break;
                    case kPfChaos: eng_.perfChaos = true; break;
                    default: {
                        const Kit& k = kits_[s.kit == kKits && eng_.wavReady ? kKits : (s.kit < kKits ? s.kit : 0)];
                        const int cls = c == kPfDum ? kClsKick : (c == kPfTek ? kClsSnare : kClsGhost);
                        LiveEv e;
                        e.type = LiveEv::kSlice;
                        e.deg = int8_t(k.list[cls][0]);
                        e.vel = uint8_t(vel);
                        eng_.live.push(e);
                        break;
                    }
                }
            }
            break;
        case kPgMelody:
            if (s.gen.live == 0 && !shift) {
                const int len = usulLength(s.usul);
                if (c < len) {
                    const int idx = r * len + c;
                    if (idx < phraseSteps(s.phr[s.editPhrase], s.usul)) phraseCur_ = idx;
                }
            } else {
                play = true;
            }
            break;
        case kPgArrange:
            if (r == 0 || (r == 1 && c < 4)) {
                const int i = r == 0 ? c : 12 + c;
                if (i < s.sectionCount) {
                    secSel_ = i;
                    if (!s.songMode) s.curSection = i;
                }
            } else {
                play = true;
            }
            break;
        case kPgMixer:
            if (r == 0 && c < 4) s.ch[c].mute ^= 1;
            else if (r == 0 && c == 4) mixCol_ = 4;
            else play = true;
            break;
        default: play = true; break;
    }
    if (!play) return;
    if (page_ == kPgUsul && r == 0) return;
    const int deg = padDegree(pad);
    padNote_[pad] = int8_t(deg);
    LiveEv e;
    e.type = padIsLead(pad) ? LiveEv::kLeadOn : LiveEv::kBassOn;
    e.deg = int8_t(deg);
    e.vel = uint8_t(vel);
    e.id = uint8_t(pad);
    eng_.live.push(e);
    if (page_ == kPgMelody && shift && padIsLead(pad)) melodyWrite(deg);
}

void MakamWs::padUp(int pad) {
    Song& s = *song_;
    const int r = padRow(pad), c = padCol(pad);
    const bool wasHeld = padHeld_[pad];
    padHeld_[pad] = false;
    if (!wasHeld) return;
    if (page_ == kPgBreak && r == 3) {
        switch (c) {
            case kPfRoll2: case kPfRoll3: case kPfRoll4: case kPfRoll6: eng_.perfRoll = 0; break;
            case kPfRev: eng_.perfRev = false; break;
            case kPfStut: eng_.perfStutter = false; break;
            case kPfHalf: eng_.perfHalf = false; break;
            case kPfMute: eng_.perfMute = false; break;
            case kPfChaos: eng_.perfChaos = false; break;
            default: break;
        }
        (void)s;
        return;
    }
    LiveEv e;
    e.type = padIsLead(pad) ? LiveEv::kLeadOff : LiveEv::kBassOff;
    e.id = uint8_t(pad);
    eng_.live.push(e);
}

// ---- events ------------------------------------------------------------------------------
void MakamWs::onEvent(const Event& e) {
    Song& s = *song_;
    switch (e.type) {
        case Event::kPadDown: padDown(e.id, e.velocity, e.shift); return;
        case Event::kPadUp: padUp(e.id); return;
        case Event::kEncoder: encoder(e.delta, e.shift); return;
        case Event::kPress: break;
        default: return;
    }
    const uint8_t id = e.id;
    if (id <= btn::F8) {
        setPage(id);
        return;
    }
    rowCount_ = buildRows(page_, rows_, kMaxRows);
    int& sel = row_[page_];
    switch (id) {
        case btn::Up:
            if (page_ == kPgFile) encoder(-1, false);
            else if (page_ == kPgMelody && e.shift) {
                Phrase& p = s.phr[s.editPhrase];
                NoteStep& n = p.s[clampi(phraseCur_, 0, kPhraseMax - 1)];
                n.deg = clampi(n.deg + 1, -1, 21);
                n.flags |= kNoteOn;
            } else sel = sel > 0 ? sel - 1 : 0;
            break;
        case btn::Down:
            if (page_ == kPgFile) encoder(1, false);
            else if (page_ == kPgMelody && e.shift) {
                Phrase& p = s.phr[s.editPhrase];
                NoteStep& n = p.s[clampi(phraseCur_, 0, kPhraseMax - 1)];
                n.deg = clampi(n.deg - 1, -1, 21);
                n.flags |= kNoteOn;
            } else sel = sel + 1 < rowCount_ ? sel + 1 : sel;
            break;
        case btn::Left:
        case btn::Right: {
            const int d = id == btn::Left ? -1 : 1;
            if (page_ == kPgBreak) stepSel_ = clampi(stepSel_ + d, 0, usulLength(s.usul) - 1);
            else if (page_ == kPgMelody) {
                const int n = phraseSteps(s.phr[s.editPhrase], s.usul);
                phraseCur_ = (phraseCur_ + d + n) % n;
            } else if (page_ == kPgMixer) mixCol_ = clampi(mixCol_ + d, 0, 4);
            else if (page_ == kPgFile) fileCol_ = clampi(fileCol_ + d, 0, 1);
            else if (page_ == kPgArrange) {
                secSel_ = clampi(secSel_ + d, 0, s.sectionCount - 1);
                if (!s.songMode) s.curSection = secSel_;
            }
            else encoder(d, e.shift);
            break;
        }
        case btn::EncPush: action(e.shift); break;
        case btn::Rec: if (page_ == kPgFile) action(true); break;
        case btn::Song:
            s.songMode ^= 1;
            // loop mode keeps the selected section (Arrange page) or the one playing
            if (!s.songMode) s.curSection = clampi(page_ == kPgArrange ? secSel_ : int(eng_.st.section), 0, s.sectionCount - 1);
            secSel_ = s.curSection;
            eng_.reset();
            ctx_->toast(s.songMode ? "Arrangement" : "Loop section");
            break;
        case btn::Insert:
            if (page_ == kPgArrange) sectionInsert();
            else if (page_ == kPgMelody) {
                NoteStep& n = s.phr[s.editPhrase].s[clampi(phraseCur_, 0, kPhraseMax - 1)];
                n.flags = (n.flags & kNoteOn) ? (n.flags ^ kNoteGlide) : kNoteTie;
            }
            break;
        case btn::Delete:
            if (page_ == kPgArrange) sectionDelete();
            else if (page_ == kPgMelody) s.phr[s.editPhrase].s[clampi(phraseCur_, 0, kPhraseMax - 1)] = NoteStep();
            else if (page_ == kPgBreak) s.brk[s.editVar].s[stepSel_] = BreakStep();
            else if (page_ == kPgFile && e.shift) {
                eng_.reset();
                songDefaults(s);
                ctx_->setBpm(float(s.bpm));
                ctx_->toast("New song");
            }
            break;
        case btn::Copy:
            if (page_ == kPgArrange) sectionCopy();
            else if (page_ == kPgBreak) {
                const int to = (s.editVar + 1) % kVars;
                s.brk[to] = s.brk[s.editVar];
                s.editVar = to;
                ctx_->toast("Variation copied to the next");
            } else if (page_ == kPgMelody) {
                const int to = (s.editPhrase + 1) % kPhrases;
                s.phr[to] = s.phr[s.editPhrase];
                s.editPhrase = to;
                ctx_->toast("Phrase copied to the next");
            }
            break;
        default: break;
    }
}

void MakamWs::sectionInsert() {
    Song& s = *song_;
    if (s.sectionCount >= kSections) return;
    const int at = clampi(secSel_ + 1, 0, s.sectionCount);
    for (int i = s.sectionCount; i > at; --i) s.sec[i] = s.sec[i - 1];
    s.sec[at] = Section();
    ++s.sectionCount;
    secSel_ = at;
}
void MakamWs::sectionDelete() {
    Song& s = *song_;
    if (s.sectionCount <= 1) return;
    for (int i = secSel_; i + 1 < s.sectionCount; ++i) s.sec[i] = s.sec[i + 1];
    --s.sectionCount;
    secSel_ = clampi(secSel_, 0, s.sectionCount - 1);
    s.curSection = clampi(s.curSection, 0, s.sectionCount - 1);
}
void MakamWs::sectionCopy() {
    Song& s = *song_;
    if (s.sectionCount >= kSections) return;
    const Section x = s.sec[secSel_];
    sectionInsert();
    s.sec[secSel_] = x;
}

void MakamWs::onMidi(const MidiMsg& m) {
    const uint8_t t = m.type();
    if (t != 0x90 && t != 0x80) return;
    const bool on = t == 0x90 && m.d2 > 0;
    const Makam& mk = kMakams[song_->makam];
    // nearest makam degree to the incoming 12-TET note (A4 = 69 = Dugah)
    const int comma = kDugah + int((float(int(m.d1) - 69) * 100.0f / (1200.0f / 53.0f)) + (m.d1 >= 69 ? 0.5f : -0.5f));
    const bool bass = m.d1 < 48;
    int deg = nearestDegree(mk, comma + (bass ? 53 * 3 : 0));
    LiveEv e;
    e.type = bass ? (on ? LiveEv::kBassOn : LiveEv::kBassOff) : (on ? LiveEv::kLeadOn : LiveEv::kLeadOff);
    e.deg = int8_t(clampi(deg, -7, 21));
    e.vel = on ? m.d2 : 0;
    e.id = uint8_t(200 + (m.d1 & 31));
    eng_.live.push(e);
}

void MakamWs::update(uint32_t ms) {
    lastMs_ = ms;
    Song& s = *song_;
    // the Config page tempo row edits the shell tempo
    if (ctx_->shell) {
        const int b = int(ctx_->bpm() + 0.5f);
        if (b != lastBpm_) {
            lastBpm_ = b;
            s.bpm = clampi(b, 60, 220);
        }
    }
    // Live seyir: a new phrase every time the playing one wraps
    const uint32_t w = eng_.st.phraseWraps;
    if (w != lastPhraseWraps_) {
        lastPhraseWraps_ = w;
        if (s.gen.live) {
            const int ph = s.songMode ? s.sec[eng_.st.section].lead - 1 : s.editPhrase;
            if (ph >= 0) genPhrase(kMakams[s.makam], s.gen, s.usul, s.phr[ph], uint32_t(s.gen.seed * 131 + w * 7919u));
        }
    }
}

void MakamWs::onTransport(bool playing) {
    if (!playing) {
        for (int i = 0; i < kPads; ++i) padHeld_[i] = false;
    }
}

void MakamWs::render(const Transport& t, const AudioIn&, float* l, float* r) { eng_.render(t, l, r); }

// ---- LEDs -------------------------------------------------------------------------------------
void MakamWs::leds(Rgb* pads) {
    Song& s = *song_;
    const Makam& m = kMakams[s.makam];
    const int len = usulLength(s.usul);
    const int step = eng_.st.unit >= 0 ? eng_.st.step : -1;
    auto degreeColour = [&](int deg) -> Rgb {
        int d = deg % 7;
        if (d < 0) d += 7;
        if (deg == -1) return scale(col::kViolet, 0.5f);
        if (d == 0) return scale(col::kRed, 0.55f);
        if (d == m.guclu % 7) return scale(col::kGold, 0.55f);
        if (m.asma[0] >= 0 && d == m.asma[0] % 7) return scale(col::kTurq, 0.35f);
        return scale(col::kCobalt, 0.28f);
    };
    auto strokeColour = [&](int stroke, bool cur) -> Rgb {
        Rgb c = stroke == kDum ? col::kRed : stroke == kTek ? col::kGold : stroke == kKe ? col::kTurq : Rgb(0x0A1020);
        return cur ? col::kCream : (stroke ? scale(c, 0.6f) : c);
    };
    const int sounding = eng_.st.leadDeg, bassDeg = eng_.st.bassDeg;
    for (int p = 0; p < kPads; ++p) {
        const int r = padRow(p), c = padCol(p);
        Rgb out = degreeColour(padDegree(p));
        const int deg = padDegree(p);
        if (padIsLead(p) && deg == sounding) out = col::kCream;
        if (!padIsLead(p) && deg == bassDeg) out = col::kCream;
        if (padHeld_[p]) out = Rgb(0xFFFFFF);
        switch (page_) {
            case kPgUsul:
                if (r == 0) out = c < len ? strokeColour(s.brk[s.editVar].s[c].stroke, c == step) : Rgb();
                break;
            case kPgBreak: {
                const Kit& k = kits_[s.kit == kKits && eng_.wavReady ? kKits : (s.kit < kKits ? s.kit : 0)];
                if (r == 0) {
                    out = c < len ? strokeColour(s.brk[s.editVar].s[c].stroke, c == step) : Rgb();
                    if (c == stepSel_ && c != step) out = mix(out, col::kCream, 0.5f);
                } else if (r == 1 || (r == 2 && c < 4)) {
                    const int sl = r == 1 ? c : 12 + c;
                    const int cl = k.cls[sl];
                    out = scale(cl == kClsKick ? col::kRed : cl == kClsSnare ? col::kGold : col::kTurq, 0.35f);
                    if (sl == eng_.st.slice) out = col::kCream;
                } else if (r == 2 && c < 8) {
                    out = (c - 4) == s.editVar ? col::kEmerald : scale(col::kEmerald, 0.15f);
                } else if (r == 2) {
                    out = (c - 8) == s.kit ? col::kSaffron : scale(col::kSaffron, 0.15f);
                } else {
                    static const uint32_t kPerfCol[12] = {0x2F5FD8, 0x2F5FD8, 0x2F5FD8, 0x2F5FD8, 0x9B6BE0, 0x9B6BE0,
                                                          0x2A9D6F, 0x5D6C8F, 0xE5484D, 0xE5484D, 0xE9B949, 0x2FD0C0};
                    out = padHeld_[p] ? Rgb(0xFFFFFF) : scale(Rgb(kPerfCol[c]), 0.4f);
                }
                break;
            }
            case kPgMelody:
                if (!s.gen.live) {
                    const Phrase& ph = s.phr[s.editPhrase];
                    const int n = phraseSteps(ph, s.usul);
                    const int idx = r * len + c;
                    if (c >= len || idx >= n) {
                        out = Rgb();
                    } else {
                        const NoteStep& x = ph.s[idx];
                        out = (x.flags & kNoteOn) ? degreeColour(x.deg) : (x.flags & kNoteTie) ? Rgb(0x0C1430) : Rgb(0x030308);
                        if (idx == eng_.st.phraseStep && eng_.st.unit >= 0) out = col::kCream;
                        if (idx == phraseCur_) out = mix(out, Rgb(0xFFFFFF), 0.6f);
                    }
                }
                break;
            case kPgArrange:
                if (r == 0 || (r == 1 && c < 4)) {
                    const int i = r == 0 ? c : 12 + c;
                    if (i >= s.sectionCount) out = Rgb();
                    else {
                        static const uint32_t kSecCol[kSecTypes] = {0x2F5FD8, 0x2FD0C0, 0xE5484D, 0x9B6BE0, 0xE9B949, 0xF4A261, 0x2A9D6F};
                        out = scale(Rgb(kSecCol[s.sec[i].type]), i == secSel_ ? 0.9f : 0.3f);
                        if (i == eng_.st.section && eng_.st.unit >= 0) out = col::kCream;
                    }
                } else if (r == 1) {
                    out = Rgb();
                }
                break;
            case kPgMixer:
                if (r == 0) out = c < 4 ? (s.ch[c].mute ? col::kRed : scale(col::kEmerald, 0.5f)) : Rgb();
                break;
            default: break;
        }
        pads[p] = out;
    }
}

// ---- bench ------------------------------------------------------------------------------------
const char* MakamWs::benchName(int i) const {
    static const char* const kN[] = {"idle", "breaks", "bass-lead", "full", "full-kanun"};
    return i >= 0 && i < 5 ? kN[i] : "?";
}

void MakamWs::benchSetup(int i) {
    benchMode_ = true;
    Song& s = *song_;
    songDefaults(s);
    s.songMode = 0;
    s.curSection = 0;
    s.sec[0] = Section();
    Section& x = s.sec[0];
    x.chaos = 0;
    x.brk = x.bass = x.lead = 0;
    for (auto& c : s.ch) { c.dly = 0; c.rvb = 0; }
    s.editVar = 3;
    s.editPhrase = 2;
    eng_.reset();
    eng_.perfRoll = 0;
    eng_.perfChaos = false;
    s.sec[0].brk = s.sec[0].bass = s.sec[0].lead = 1;
    switch (i) {
        case 0:   // idle: transport running, every part off
            s.sec[0].brk = s.sec[0].bass = s.sec[0].lead = 0;
            s.editVar = 0;
            break;
        case 1:   // breaks: aksak, variation D, chaos 100, rolls, 6-rolls held, drive, filter
            s.usul = 0;
            songApplyUsul(s);
            eng_.perfRoll = 6;
            eng_.perfChaos = true;
            s.djFilter = -40;
            s.drive = 80;
            s.sec[0].bass = s.sec[0].lead = 0;
            s.ch[0].dly = 50;
            s.ch[0].rvb = 40;
            break;
        default: {   // 2 bass + lead, 3 full, 4 full with the kanun
            s.usul = 1;   // curcuna: densest strokes
            s.makam = 3;  // hicaz
            songApplyUsul(s);
            songApplyMakam(s);
            s.bp.model = 2;      // growl: three oscillators + fold
            s.lp.model = i == 4 ? 1 : 2;   // saz: two KS courses
            s.gen.density = 100;
            for (int k = 0; k < kPhraseMax; ++k) s.phr[2].s[k] = NoteStep{k % 9, kNoteOn | (k & 1 ? kNoteGlide : kNoteGrace)};
            if (i == 2) s.sec[0].brk = 0;
            if (i >= 3) {
                eng_.perfRoll = 4;
                eng_.perfChaos = true;
                s.djFilter = 35;
                s.drive = 70;
                for (auto& c : s.ch) { c.dly = 60; c.rvb = 60; }
                s.dlyFb = 80;
            }
            // two pad voices held on top of the phrase
            LiveEv e;
            e.type = LiveEv::kLeadOn;
            e.vel = 110;
            e.deg = 4;
            e.id = 30;
            eng_.live.push(e);
            e.deg = 7;
            e.id = 31;
            eng_.live.push(e);
            break;
        }
    }
}

}  // namespace makam

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<makam::MakamWs>(); }
}  // namespace lab
