// BULUT workstation: state, controls, pages, files (bulut_ws.h).
// Drawing lives in draw.cpp, the LABKIT registration in params.cpp, the audio
// engine in engine.cpp / fx.cpp.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/dsp.h"
#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

namespace bulut {

using lab::Event;
namespace btn = lab::btn;
namespace kit = lab::kit;

namespace {
const char* const kOffOn[] = {"Off", "On"};
const char* const kMidiCh[] = {"Selected", "1", "2", "3", "4"};
const char* const kLedLevels[] = {"Low", "Medium", "High"};

struct WsSettings {
    uint32_t magic = 0x42554C54u;   // "BULT"
    uint32_t version = 2;
    int grains = kDefaultGrains, cloudGrains = kDefaultCloudGrains, velocity = 1, midiCh = 0, leds = 2, restore = 1;
};

// The channel params in the order the FX page lists them (cursor 0..9).
const uint8_t kFxRows[10] = {CP::Drive, CP::Flt, CP::Cutoff, CP::Reso, CP::FxMode,
                             CP::FxTime, CP::FxAmt, CP::FxFb, CP::LfoRate, CP::LfoShape};
// The SOURCE page's rows (cursor 0..8; 8 = the radio, not a param).
const uint8_t kSrcRows[8] = {CP::Source, CP::Gain, CP::Start, CP::End, CP::Bed, CP::GenType, CP::GenA, CP::GenB};
}  // namespace

// ---- init ---------------------------------------------------------------------
void Bulut::firstBoot() {
    for (int c = 0; c < kCh; ++c) {
        ChParams& p = par_[c];
        p = ChParams();
        ChState& s = st_[c];
        std::memset(&s, 0, sizeof s);
        s.freezeSlot = 0xFF;
        s.stem = uint8_t(c);
        s.gestOn = 1;
        for (int e = 0; e < kEngines; ++e) defaultEngineKnobs(e, s.engMem[e], c);
        std::memset(&src_[c], 0, sizeof src_[c]);
    }
    // a first state that sounds: an input cloud, a feedback cloud on BULUT's own
    // output, a QUANTA cloud, a TAPE channel on the rete generator
    par_[0].engine = kGranular;
    par_[0].source = kSrcInput;
    par_[1].engine = kBeads;
    par_[1].source = kSrcOutput;
    par_[1].gain = 0.8f;
    st_[1].engMem[kBeads][K::Mode] = enumValue(kMediaCleanTape, kMedias);
    par_[2].engine = kQuanta;
    par_[2].source = kSrcInput;
    par_[2].level = 0.6f;
    par_[3].engine = kTape;
    par_[3].source = kSrcGen;
    par_[3].genType = kGenRete;
    par_[3].level = 0.55f;
    for (int c = 0; c < kCh; ++c) {
        for (int i = 0; i < kKnobs; ++i) par_[c].k[i] = st_[c].engMem[par_[c].engine][i];
        par_[c].pan = c == 0 ? -0.2f : (c == 1 ? 0.25f : (c == 2 ? -0.35f : 0.35f));
    }
    fx_ = FxState();
    tools_ = SpecTools();
}

bool Bulut::init(lab::Context& c) {
    ctx_ = &c;
    theme_.bg = lab::Rgb(0x080C18);
    theme_.panel = lab::Rgb(0x141A2E);
    theme_.fg = lab::Rgb(0xE6EAF4);
    theme_.dim = lab::Rgb(0x6E7894);
    theme_.accent = lab::Rgb(0x9CD6F2);
    theme_.accent2 = lab::Rgb(0xF2B6C8);
    theme_.warn = lab::Rgb(0xFF6B6B);
    theme_.ok = lab::Rgb(0x92E2B4);

    lab::Arena& a = *c.arena;
    eng_ = a.make<Engine>();
    rec_ = a.make<GestureRecorder>();
    gest_ = a.make<Gesture>(kCh * kGestBufs);
    scenes_ = a.make<Scene>(kScenes);
    frz_ = a.make<FreezeSlot>(kFreezeSlots);
    morphFrom_ = static_cast<float*>(a.alloc((kCh * kChParams + kMasterParams) * sizeof(float)));
    morphTo_ = static_cast<float*>(a.alloc((kCh * kChParams + kMasterParams) * sizeof(float)));
    chunkBuf_ = static_cast<uint8_t*>(a.alloc(kChunkCap));
    if (!eng_ || !rec_ || !gest_ || !scenes_ || !frz_ || !morphFrom_ || !morphTo_ || !chunkBuf_) return false;
    firstBoot();
    if (!eng_->init(a, par_, st_, src_, gest_, &fx_)) return false;
    if (!spec_.init(a)) return false;
    for (int i = 0; i < kParticles; ++i) part_[i].life = 0.0f;
    registerParams();
    // BULUT's own modulation sources (LABKIT routes / macros can use them)
    kit::Mod& M = c.mod();
    static const char* const kSrcName[kCh][4] = {{"C1 LFO", "C1 BUF", "C1 DENS", "C1 GEST"},
                                                 {"C2 LFO", "C2 BUF", "C2 DENS", "C2 GEST"},
                                                 {"C3 LFO", "C3 BUF", "C3 DENS", "C3 GEST"},
                                                 {"C4 LFO", "C4 BUF", "C4 DENS", "C4 GEST"}};
    for (int ch = 0; ch < kCh; ++ch) {
        M.addSource(kSrcName[ch][0], &eng_->rt[ch].lfo, true);
        M.addSource(kSrcName[ch][1], &eng_->rt[ch].bufLevel, false);
        M.addSource(kSrcName[ch][2], &eng_->rt[ch].density, false);
        M.addSource(kSrcName[ch][3], &eng_->rt[ch].gestPos, false);
    }
    M.addSource("MIX LVL", &eng_->masterLevel, false);
    c.browser().reserve(256, 4.0f);

    if (c.fs) {
        char p[96];
        std::snprintf(p, sizeof p, "%s/Projects", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/Resample", c.home);
        c.fs->mkdir(p);
        std::snprintf(p, sizeof p, "%s/bulut.cfg", c.home);
        const ot::IStorage::Handle h = c.fs->open(p, ot::IStorage::kRead);
        if (h >= 0) {
            WsSettings s;
            if (c.fs->read(h, &s, sizeof s) == int32_t(sizeof s) && s.magic == WsSettings().magic && s.version == 2) {
                setGrains_ = s.grains < 8 ? 8 : (s.grains > kMaxGrains ? kMaxGrains : s.grains);
                setCloudGrains_ = s.cloudGrains < 2 ? 2 : (s.cloudGrains > kMaxCloudGrains ? kMaxCloudGrains : s.cloudGrains);
                setVelocity_ = s.velocity;
                setMidiCh_ = s.midiCh;
                setLeds_ = s.leds;
                setAutosave_ = s.restore;
            }
            c.fs->close(h);
        }
        if (setAutosave_) loadAutosave();
    }
    eng_->grainBudget = setGrains_;
    eng_->cloudBudget = setCloudGrains_;
    radioMHz_ = c.shell ? c.shell->radioMHz() : 100.0f;
    scanProjects();
    return true;
}

void Bulut::loadAutosave() {
    char p[96];
    std::snprintf(p, sizeof p, "%s/autosave.blt", ctx_->home);
    if (ctx_->fs && ctx_->fs->exists(p)) loadProjectFrom(p);
}

#ifdef BULUT_PROF
extern double gProf[20], gProfMax[20];
extern int gProfBlocks;
#endif

const char* Bulut::benchName(int i) const {
#ifdef BULUT_PROF
    if (gProfBlocks) {
        static const char* const kSec[20] = {"bufwr", "gen", "emit", "grains", "media", "mix", "sndset", "master", "dly", "rev", "drive", "filter",
                                             "m0", "tex", "shift", "flang", "pan", "crush", "slice", "frz"};
        std::printf("prof:");
        for (int q = 0; q < 20; ++q) {
            std::printf(" %s %.0f/%.0f", kSec[q], gProf[q] / gProfBlocks, gProfMax[q]);
            gProf[q] = gProfMax[q] = 0.0;
        }
        std::printf("\n");
        gProfBlocks = 0;
    }
#endif
    static const char* const k[] = {"idle-live",  "granular-max", "quanta-max", "beads-tape", "tape-max",   "fx-texture",
                                    "fx-modes",   "gen-rete",     "freeze-max", "everything", "fx-gen-mix"};
    return k[i < 0 ? 0 : (i > 10 ? 10 : i)];
}

// Bench: every channel busy, the grain budget full, the heavy paths on.
void Bulut::benchSetup(int i) {
    kit::Registry& P = ctx_->params();
    kit::Mod& M = ctx_->mod();
    M.clearRoutes();
    lab::dsp::Rng r(77u + uint32_t(i));
    auto set = [&](int c, int idx, float v) { P.set(pid_[c][idx], v, false); };
    auto setN = [&](int c, int idx, float v) { P.setNorm(pid_[c][idx], v, false); };
    for (int c = 0; c < kCh; ++c) {
        eng_->releaseAll(c);
        st_[c].gestOn = 0;
        st_[c].drone = 0;
        st_[c].freeze = 0;
        set(c, CP::Source, float(c == 1 ? kSrcOutput : kSrcInput));
        set(c, CP::FxMode, 0.0f);
        set(c, CP::Flt, 0.0f);
        set(c, CP::Drive, 0.0f);
        set(c, CP::Engine, float(kGranular));
        // fill the buffers so grains read real data
        for (uint32_t k = 0; k < kBufLen; ++k) eng_->rt[c].buf[k] = int16_t(8000.0f * sinf(float(k) * 0.01f * float(c + 1)));
    }
    syncEngines();
    // the worst a user can set: both budgets at their maximum
    eng_->grainBudget = kMaxGrains;
    eng_->cloudBudget = kMaxCloudGrains;
    if (i == 0) {   // idle: the first-boot setup, a generator running
        set(3, CP::Source, float(kSrcGen));
        set(3, CP::GenType, float(kGenRete));
        return;
    }
    static const int kEng[11] = {kGranular, kGranular, kQuanta, kBeads, kTape, kGranular, kGranular, kGranular, kGranular, -1, -1};
    for (int c = 0; c < kCh; ++c) {
        const int e = kEng[i] >= 0 ? kEng[i] : c;   // everything: one of each engine
        set(c, CP::Engine, float(e));
    }
    syncEngines();
    for (int c = 0; c < kCh; ++c) {
        const int e = par_[c].engine;
        st_[c].drone = 1;
        if (e == kGranular) {
            setN(c, K::Dens, 1.0f);
            setN(c, K::Size, 0.9f);
            setN(c, K::Pitch, 0.85f);
            setN(c, K::Rand, 0.5f);
            setN(c, K::Mode, 0.3f);
            setN(c, K::Feedback, 0.6f);
        } else if (e == kQuanta) {
            setN(c, K::Dens, 1.0f);
            setN(c, K::Size, 1.0f);
            setN(c, K::Mode, enumValue(1 + c % (kQTypes - 1), kQTypes));
            setN(c, K::Rand, enumValue(c % kLaws, kLaws));
            setN(c, K::Shape, 0.9f);
        } else if (e == kBeads) {
            setN(c, K::Dens, 1.0f);
            setN(c, K::Size, 0.95f);
            setN(c, K::Mode, enumValue(kMediaDirtyTape, kMedias));
            setN(c, K::Rand, 0.4f);
            setN(c, K::Feedback, 0.6f);
        } else {
            setN(c, K::Size, 1.0f);
            setN(c, K::Dens, 0.95f);
            setN(c, K::Extra, 1.0f);
            setN(c, K::Feedback, 0.6f);
        }
        for (int p = 0; p < 4; ++p) eng_->padDown(c, p * 13 % 48, 120);
        set(c, CP::Dly, 1.0f);
        set(c, CP::Rev, 1.0f);
    }
    fx_.dlyFb = 1.0f;
    fx_.dlyDiv = 0;
    fx_.revSize = 1.0f;
    P.sync();
    auto freezeAll = [&]() {
        for (int c = 0; c < kCh; ++c) {
            spec_.analyse([&](int k) { return float(eng_->rt[c].buf[k]) / 32768.0f; }, frz_[c]);
            spec_.synth(frz_[c], eng_->rt[c].frz, 9u + uint32_t(c));
            eng_->rt[c].frzReady = 1;
            st_[c].freeze = 1;
            set(c, CP::Bed, 1.0f);
        }
    };
    if (i == 5) {
        for (int c = 0; c < kCh; ++c) {
            set(c, CP::FxMode, float(kFxTexture));
            set(c, CP::FxAmt, 1.0f);
            set(c, CP::FxFb, 1.0f);
            set(c, CP::Flt, float(kFltLp));
            set(c, CP::Drive, 1.0f);
        }
    }
    if (i == 6) {
        static const int kModes[4] = {kFxShifter, kFxFlanger, kFxCrusher, kFxFreezer};
        for (int c = 0; c < kCh; ++c) {
            set(c, CP::FxMode, float(kModes[c]));
            set(c, CP::FxAmt, 0.8f);
            set(c, CP::FxFb, 0.9f);
            set(c, CP::FxTime, 0.8f);
            set(c, CP::Flt, float(kFltBp));
            set(c, CP::Drive, 1.0f);
        }
    }
    if (i == 7) {
        for (int c = 0; c < kCh; ++c) {
            set(c, CP::Source, float(kSrcGen));
            set(c, CP::GenType, float(kGenRete));
        }
    }
    if (i == 8) {
        freezeAll();
        set(0, CP::FxMode, float(kFxTexture));
        set(1, CP::FxMode, float(kFxTexture));
        set(0, CP::FxAmt, 1.0f);
        set(1, CP::FxAmt, 1.0f);
    }
    if (i == 10) {
        // the lighter FX modes and generators: PANNER, SLICER, CRUSHER, FLANGER; TONE, NOISE
        static const int kModes[4] = {kFxPanner, kFxSlicer, kFxCrusher, kFxFlanger};
        for (int c = 0; c < kCh; ++c) {
            set(c, CP::FxMode, float(kModes[c]));
            set(c, CP::FxAmt, 0.9f);
            set(c, CP::FxFb, 0.8f);
            set(c, CP::FxTime, 0.6f);
            set(c, CP::Flt, float(kFltHp));
        }
        set(0, CP::Source, float(kSrcGen));
        set(0, CP::GenType, float(kGenTone));
        set(2, CP::Source, float(kSrcGen));
        set(2, CP::GenType, float(kGenNoise));
    }
    if (i == 9) {
        // one of each engine, a different FX on each, a generator, a freeze,
        // gestures at 4 x with parameter moves, capture, 32 LABKIT routes
        static const int kModes[4] = {kFxTexture, kFxShifter, kFxFreezer, kFxFlanger};
        for (int c = 0; c < kCh; ++c) {
            set(c, CP::FxMode, float(kModes[c]));
            set(c, CP::FxAmt, 0.9f);
            set(c, CP::FxFb, 0.8f);
            set(c, CP::Flt, float(kFltLp));
            set(c, CP::Drive, 0.8f);
            set(c, CP::GestSpeed, 4.0f);
        }
        set(3, CP::Source, float(kSrcGen));
        set(3, CP::GenType, float(kGenRete));
        spec_.analyse([&](int k) { return float(eng_->rt[0].buf[k]) / 32768.0f; }, frz_[0]);
        spec_.synth(frz_[0], eng_->rt[0].frz, 9u);
        eng_->rt[0].frzReady = 1;
        st_[0].freeze = 1;
        set(0, CP::Bed, 1.0f);
        for (int c = 0; c < kCh; ++c) {
            Gesture& g = eng_->gestIdle(c);
            g.len = 22050;
            g.quant = 0;
            int n = 0;
            for (int k = 0; k < 200 && n < kGestEvents - 2; ++k) {
                GestEvent& ev = g.ev[n++];
                ev.t = uint32_t(k * 110);
                if (k % 3 == 0) {
                    ev.type = kGPadDown;
                    ev.a = uint8_t(r.range(48));
                    ev.v = 110;
                } else if (k % 3 == 1) {
                    ev.type = kGParam;
                    ev.a = uint8_t(K::Pos);
                    ev.v = uint16_t(r.next());
                } else {
                    ev.type = kGPadUp;
                    ev.a = g.ev[n - 3].a;
                    ev.v = 0;
                }
            }
            g.count = uint16_t(n);
            eng_->postGesture(c);
            st_[c].gestOn = 1;
        }
        for (int c = 0; c < kCh; ++c)
            for (int k = 0; k < 8; ++k) M.route(kit::kSrcLfo + (k & 3), pid_[c][k == 7 ? CP::Cutoff : k], 0.2f);
        M.update(false);
        eng_->startCapture(-1);
    }
}

// ---- audio ----------------------------------------------------------------------
void Bulut::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    eng_->render(t, in, l, r, ctx_);
}

void Bulut::onTransport(bool playing) {
    (void)playing;
    tlPlaying_ = -1;
}

// ---- pages, channels, switches --------------------------------------------------
void Bulut::setPage(int p) {
    if (p < 0 || p >= kPages) return;
    page_ = p;
    regionMode_ = false;
    regionFirst_ = -1;
    if (p == kPgFile) scanProjects();
}

void Bulut::selectChannel(int c) {
    if (c < 0) c = kCh - 1;
    if (c >= kCh) c = 0;
    if (c == sel_) return;
    // pads held on the old channel are released (their grains finish)
    for (int i = 0; i < kLiveEmitters; ++i)
        if (eng_->rt[sel_].em[i].pad != 0xFF) eng_->rt[sel_].em[i].on = 0;
    held_ = 0;
    if (rec_->state() != GestureRecorder::kIdle && rec_->cloud() == sel_) finishGesture();
    sel_ = c;
    regionFirst_ = -1;
}

void Bulut::setEngine(int c, int e) {
    ctx_->params().set(pid_[c][CP::Engine], float((e % kEngines + kEngines) % kEngines), false);
    syncEngines();
}

void Bulut::setSource(int c, int s) {
    s = (s % kSrcCount + kSrcCount) % kSrcCount;
    ctx_->params().set(pid_[c][CP::Source], float(s), false);
    if (s == kSrcSample && eng_->rt[c].sampleLen == 0) {
        if (src_[c].path[0]) loadSample(c, src_[c].path);
        else openBrowser(c);
    }
    srcNow_[c] = s;
}

// ---- gestures -------------------------------------------------------------------
void Bulut::waitGesture(int c) {
    // first movement after Rec: the recording starts now
    const bool od = rec_->overdub();
    const Gesture& live = eng_->gestLive(c);
    if (!od) {
        Gesture& idle = eng_->gestIdle(c);
        idle.len = 0;
        idle.count = 0;
        idle.quant = live.quant;
        eng_->postGesture(c);   // the old loop stops while a new one is recorded
    }
    rec_->start(eng_->sampleClock, od ? &live : nullptr, uint32_t(eng_->rt[c].gphase),
                speedMul(int(par_[c].gestSpeed + 0.5f)));
}

void Bulut::gestureRecButton(int c) {
    if (rec_->state() == GestureRecorder::kIdle) {
        if (c != sel_) selectChannel(c);
        const bool od = gestMode_ == 1 && eng_->gestLive(c).len > 0;
        rec_->arm(c, od);
        ctx_->toast(od ? "Overdub armed: move to record" : "Gesture armed: move to record");
    } else {
        finishGesture();
    }
}

void Bulut::finishGesture() {
    const int c = rec_->cloud();
    if (rec_->state() == GestureRecorder::kArmed) {
        rec_->cancel();
        ctx_->toast("Gesture cancelled");
        return;
    }
    const Gesture& live = eng_->gestLive(c);
    const int q = live.quant;
    const float bpm = ctx_->bpm() > 20.0f ? ctx_->bpm() : 120.0f;
    const uint32_t beat = uint32_t(60.0f / bpm * kSr);
    const uint32_t quant = q == kQuantBeat ? beat : (q == kQuantBar ? beat * 4 : 0);
    Gesture& out = eng_->gestIdle(c);
    out.quant = live.quant;
    out.speed = 2;
    if (rec_->finish(eng_->sampleClock, quant, out, &live)) {
        eng_->postGesture(c);
        st_[c].gestOn = 1;
        char m[64];
        std::snprintf(m, sizeof m, "Gesture %d: %u moves, %.2f s", c + 1, unsigned(out.count), double(float(out.len) / kSr));
        ctx_->toast(m);
    } else {
        ctx_->toast("Nothing recorded");
    }
}

// ---- freezes ----------------------------------------------------------------------
void Bulut::toggleFreeze(int c) {
    ChState& s = st_[c];
    if (s.freeze) {
        s.freeze = 0;
        ctx_->toast("Freeze off");
        return;
    }
    if (!eng_->rt[c].frzReady) {
        // nothing frozen yet: take this moment of the channel's source into a free slot
        int slot = -1;
        for (int i = 0; i < kFreezeSlots && slot < 0; ++i)
            if (!frz_[i].used) slot = i;
        if (slot < 0) slot = frzCur_;
        const int keep = sel_;
        sel_ = c;
        if (captureFreeze(slot, kFromSource)) recallFreeze(slot);
        sel_ = keep;
        return;
    }
    s.freeze = 1;
    ctx_->toast("Freeze on");
}

bool Bulut::captureFreeze(int slot, int from) {
    if (slot < 0 || slot >= kFreezeSlots) return false;
    const int c = sel_;
    ChRt& r = eng_->rt[c];
    bool got = false;
    if (from == kFromMix) {
        const uint32_t w = eng_->mixW;
        got = spec_.analyse(
            [&](int k) { return float(eng_->mixHist[(w - Spectral::kAnaSpan + uint32_t(k)) & (kMixHist - 1)]) / 32768.0f; },
            frz_[slot]);
    } else if (par_[c].source == kSrcSample && r.sampleLen > 0) {
        const uint32_t len = r.sampleLen;
        const float pos = 0.5f * (par_[c].regStart + par_[c].regEnd);
        uint32_t s0 = uint32_t(pos * float(len));
        s0 = s0 > uint32_t(Spectral::kAnaSpan / 2) ? s0 - Spectral::kAnaSpan / 2 : 0;
        got = spec_.analyse(
            [&](int k) {
                const uint32_t i = s0 + uint32_t(k);
                return i < len ? float(r.buf[i]) / 32768.0f : 0.0f;
            },
            frz_[slot]);
    } else {
        const uint32_t w = r.w;
        got = spec_.analyse([&](int k) { return float(r.buf[(w - Spectral::kAnaSpan + uint32_t(k)) & kBufMask]) / 32768.0f; },
                            frz_[slot]);
    }
    if (!got) {
        ctx_->toast("Nothing to freeze: the source is silent");
        return false;
    }
    frz_[slot].cloud = uint8_t(c);
    frz_[slot].from = uint8_t(from);
    frzCur_ = slot;
    specViewSlot_ = -1;
    char m[48];
    std::snprintf(m, sizeof m, "Frozen moment -> slot %d", slot + 1);
    ctx_->toast(m);
    return true;
}

void Bulut::recallFreeze(int slot) {
    if (slot < 0 || slot >= kFreezeSlots || !frz_[slot].used) {
        ctx_->toast("Empty freeze slot");
        return;
    }
    ChRt& r = eng_->rt[sel_];
    r.frzReady = 0;
    const int ms = tools_.morphSlot;
    const FreezeSlot* morph = (ms >= 0 && ms < kFreezeSlots && frz_[ms].used && ms != slot) ? &frz_[ms] : nullptr;
    spec_.synth(frz_[slot], r.frz, 0x5EEDu + uint32_t(slot) * 977u + uint32_t(sel_), morph, &tools_);
    r.frzReady = 1;
    st_[sel_].freeze = 1;
    st_[sel_].freezeSlot = uint8_t(slot);
    char m[48];
    std::snprintf(m, sizeof m, "CH %d frozen on slot %d", sel_ + 1, slot + 1);
    ctx_->toast(m);
}

// ---- scenes ---------------------------------------------------------------------------
void Bulut::storeScene(int s) {
    if (s < 0 || s >= kScenes) return;
    kit::Registry& P = ctx_->params();
    Scene& sc = scenes_[s];
    for (int c = 0; c < kCh; ++c) {
        for (int i = 0; i < kChParams; ++i) sc.norm[c][i] = P.norm(pid_[c][i]);
        for (int i = 0; i < kKnobs; ++i) st_[c].engMem[par_[c].engine][i] = P.norm(pid_[c][i]);
        sc.st[c] = st_[c];
        const Gesture& g = eng_->gestLive(c);
        sc.gest[c].len = g.len;
        sc.gest[c].count = g.count;
        sc.gest[c].quant = g.quant;
        sc.gest[c].speed = g.speed;
        std::memcpy(sc.gest[c].ev, g.ev, sizeof(GestEvent) * g.count);
    }
    for (int i = 0; i < kMasterParams; ++i) sc.master[i] = P.norm(mid_[i]);
    sc.used = 1;
    curScene_ = s;
    char m[32];
    std::snprintf(m, sizeof m, "Scene %d stored", s + 1);
    ctx_->toast(m);
}

void Bulut::recallScene(int s, bool morph) {
    if (s < 0 || s >= kScenes || !scenes_[s].used) return;
    kit::Registry& P = ctx_->params();
    const Scene& sc = scenes_[s];
    if (rec_->state() != GestureRecorder::kIdle) rec_->cancel();
    static const uint8_t kSwitches[6] = {CP::Engine, CP::Source, CP::GenType, CP::Flt, CP::FxMode, CP::LfoShape};
    for (int c = 0; c < kCh; ++c) {
        // switches change at once, the engine first (its knobs follow below)
        const bool engChanged = enumIndex(sc.norm[c][CP::Engine], kEngines) != par_[c].engine;
        for (int k = 0; k < 6; ++k) P.setNorm(pid_[c][kSwitches[k]], sc.norm[c][kSwitches[k]], false);
        st_[c] = sc.st[c];
        engNow_[c] = par_[c].engine;
        renameKnobs(c);
        if (par_[c].source != srcNow_[c]) {
            srcNow_[c] = par_[c].source;
            if (par_[c].source == kSrcSample && eng_->rt[c].sampleLen == 0 && src_[c].path[0]) loadSample(c, src_[c].path);
        }
        Gesture& g = eng_->gestIdle(c);
        g.len = sc.gest[c].len;
        g.count = sc.gest[c].count;
        g.quant = sc.gest[c].quant;
        g.speed = sc.gest[c].speed;
        std::memcpy(g.ev, sc.gest[c].ev, sizeof(GestEvent) * sc.gest[c].count);
        eng_->postGesture(c);
        for (int i = 0; i < kChParams; ++i) {
            morphFrom_[c * kChParams + i] = P.norm(pid_[c][i]);
            morphTo_[c * kChParams + i] = sc.norm[c][i];
            if (engChanged && i < kKnobs) morphFrom_[c * kChParams + i] = sc.norm[c][i];   // another engine: no glide
        }
    }
    for (int i = 0; i < kMasterParams; ++i) {
        morphFrom_[kCh * kChParams + i] = P.norm(mid_[i]);
        morphTo_[kCh * kChParams + i] = sc.master[i];
    }
    const float dur = fx_.morphSec;
    morphT_ = 0.0f;
    morphDur_ = dur;
    morphOn_ = true;
    if (!morph || dur <= 0.01f) {
        morphT_ = 1.0f;
        morphDur_ = 0.0f;
        pumpMorph(0.0f);
    }
    // frozen channels get their library moment back
    for (int c = 0; c < kCh; ++c) {
        const uint8_t fs = sc.st[c].freezeSlot;
        if (sc.st[c].freeze && fs < kFreezeSlots && frz_[fs].used) {
            ChRt& r = eng_->rt[c];
            r.frzReady = 0;
            spec_.synth(frz_[fs], r.frz, 0x5EEDu + uint32_t(fs) * 977u + uint32_t(c));
            r.frzReady = 1;
        }
    }
    curScene_ = s;
}

void Bulut::pumpMorph(float dt) {
    if (!morphOn_) return;
    kit::Registry& P = ctx_->params();
    morphT_ += dt;
    const float k = (morphDur_ <= 0.0f || morphT_ >= morphDur_) ? 1.0f : morphT_ / morphDur_;
    const float e = k * k * (3.0f - 2.0f * k);   // smoothstep
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kChParams; ++i) {
            if (i == CP::Engine || i == CP::Source || i == CP::GenType || i == CP::Flt || i == CP::FxMode ||
                i == CP::LfoShape)
                continue;
            const float a = morphFrom_[c * kChParams + i], b = morphTo_[c * kChParams + i];
            const kit::ParamId id = pid_[c][i];
            const bool stepped = P.curve(id) == kit::Curve::kEnum || P.curve(id) == kit::Curve::kInt ||
                                 (i < kKnobs && knobSteps(par_[c].engine, i) > 1);
            P.setNorm(id, stepped ? (e < 0.5f ? a : b) : a + (b - a) * e, false);
        }
    for (int i = 0; i < kMasterParams; ++i) {
        const float a = morphFrom_[kCh * kChParams + i], b = morphTo_[kCh * kChParams + i];
        const bool stepped = P.curve(mid_[i]) == kit::Curve::kEnum;
        P.setNorm(mid_[i], stepped ? (e < 0.5f ? a : b) : a + (b - a) * e, false);
    }
    if (k >= 1.0f) morphOn_ = false;
}

void Bulut::pumpTimeline() {
    if (!songMode_ || tlLen_ <= 0 || !ctx_->playing()) {
        if (!ctx_->playing()) tlPlaying_ = -1;
        return;
    }
    const double b = eng_->beat;
    double total = 0.0;
    for (int i = 0; i < tlLen_; ++i) total += timeline_[i].bars * 4.0;
    if (total <= 0.0) return;
    const double pos = b - total * double(int64_t(b / total));   // the song loops
    double start = 0.0;
    int idx = -1;
    for (int i = 0; i < tlLen_; ++i) {
        const double len = timeline_[i].bars * 4.0;
        if (pos < start + len) {
            idx = i;
            break;
        }
        start += len;
    }
    if (idx >= 0 && idx != tlPlaying_) {
        tlPlaying_ = idx;
        recallScene(timeline_[idx].scene, true);
    }
}

// ---- resample loop ---------------------------------------------------------------------
void Bulut::startResample() {
    if (rsState_) return;
    rsTarget_ = rsDst_;
    rsWritten_ = 0;
    rsPath_[0] = 0;
    char tag[16];
    if (rsSrc_ < 0) std::snprintf(tag, sizeof tag, "MIX");
    else std::snprintf(tag, sizeof tag, "CH%d", rsSrc_ + 1);
    int n = 1;
    if (ctx_->fs) {
        char p[96];
        for (; n < 1000; ++n) {
            std::snprintf(p, sizeof p, "%s/Resample/%s-%03d.wav", ctx_->home, tag, n);
            if (!ctx_->fs->exists(p)) break;
        }
        std::snprintf(rsPath_, sizeof rsPath_, "%s/Resample/%s-%03d.wav", ctx_->home, tag, n);
        if (!rsWriter_.open(*ctx_->fs, rsPath_, 1)) rsPath_[0] = 0;
    }
    std::snprintf(rsLabel_, sizeof rsLabel_, "RESAMPLE %s %03d", tag, n);
    eng_->startCapture(rsSrc_);
    rsState_ = 1;
    char m[48];
    std::snprintf(m, sizeof m, "Resampling %s -> CH %d", rsSrc_ < 0 ? "the mix" : tag, rsTarget_ + 1);
    ctx_->toast(m);
}

void Bulut::stopResample() {
    if (rsState_ != 1) return;
    eng_->stopCapture();
    rsState_ = 2;
}

void Bulut::pumpResample() {
    if (!rsState_) return;
    const uint32_t have = eng_->capLen;
    if (rsPath_[0] && have > rsWritten_) {
        uint32_t n = have - rsWritten_;
        if (n > 16384) n = 16384;
        rsWriter_.write(eng_->capBuf + rsWritten_, n);
        rsWritten_ += n;
    } else if (!rsPath_[0]) {
        rsWritten_ = have;
    }
    if (rsState_ == 1 && !eng_->capOn) rsState_ = 2;   // the capture buffer is full
    if (rsState_ == 2 && rsWritten_ >= eng_->capLen) {
        rsState_ = 0;
        const int t = rsTarget_;
        const uint32_t len = eng_->capLen;
        if (len < 2048) {
            if (rsPath_[0]) rsWriter_.close();
            ctx_->toast("Resample too short");
            return;
        }
        if (rsPath_[0]) {
            rsWriter_.close();
            if (loadSample(t, rsPath_)) {
                lab::copyStr(src_[t].label, sizeof src_[t].label, rsLabel_);
                char m[64];
                std::snprintf(m, sizeof m, "%s -> CH %d", rsLabel_, t + 1);
                ctx_->toast(m);
                return;
            }
        }
        // no card: hand the capture straight to the target channel
        ChRt& r = eng_->rt[t];
        r.busy = 1;
        std::memcpy(r.buf, eng_->capBuf, len * 2);
        std::memset(r.buf + len, 0, (kBufLen - len) * sizeof(int16_t));
        r.sampleLen = len;
        src_[t].path[0] = 0;
        lab::copyStr(src_[t].label, sizeof src_[t].label, rsLabel_);
        r.busy = 0;
        ctx_->params().set(pid_[t][CP::Source], float(kSrcSample), false);
        srcNow_[t] = kSrcSample;
        ctx_->toast("Resample -> channel (not saved: no card)");
    }
}

// ---- samples ---------------------------------------------------------------------------
bool Bulut::loadSample(int c, const char* path) {
    if (!ctx_->fs || c < 0 || c >= kCh || !path || !path[0]) return false;
    ot::IStorage& fs = *ctx_->fs;
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return false;
    ot::WavInfo info;
    ot::StorageReader rd(fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    fs.close(h);
    if (!ok) return false;
    const uint32_t out = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
    ot::WavImportOptions opt;
    if (out > kBufLen - 16) opt.endMarker = uint16_t(65535.0f * float(kBufLen - 4096) / float(out));
    ChRt& r = eng_->rt[c];
    r.busy = 1;
    r.sampleLen = 0;
    ot::WavImportResult res;
    const ot::FmtStatus st = ot::wavImportFile(fs, path, opt, r.buf, kBufLen, res);
    if (st != ot::FmtStatus::Ok || res.frames < 64) {
        r.busy = 0;
        return false;
    }
    // silence after the sample: grains reading past its end hear nothing old
    std::memset(r.buf + res.frames, 0, (kBufLen - res.frames) * sizeof(int16_t));
    r.sampleLen = res.frames;
    lab::copyStr(src_[c].path, sizeof src_[c].path, path);
    const char* b = std::strrchr(path, '/');
    lab::copyStr(src_[c].label, sizeof src_[c].label, b ? b + 1 : path);
    r.busy = 0;
    ctx_->params().set(pid_[c][CP::Source], float(kSrcSample), false);
    srcNow_[c] = kSrcSample;
    return true;
}

// The next / previous WAV in the folder of the channel's sample (Copy / Shift + Copy).
bool Bulut::swapSample(int c, int dir) {
    if (!ctx_->fs || !src_[c].path[0]) {
        openBrowser(c);
        return false;
    }
    char folder[128];
    lab::copyStr(folder, sizeof folder, src_[c].path);
    char* slash = std::strrchr(folder, '/');
    const char* cur = slash ? slash + 1 : src_[c].path;
    if (slash) *slash = 0;
    else folder[0] = 0;
    struct List {
        char names[96][48];
        int n = 0;
    };
    static List list;
    list.n = 0;
    ctx_->fs->listDir(folder[0] ? folder : "/",
                      [](const ot::IStorage::DirEntry& e, void* p) {
                          List* l = static_cast<List*>(p);
                          if (e.isDir || e.name[0] == '.' || !lab::endsWithCi(e.name, ".wav")) return true;
                          if (std::strlen(e.name) >= sizeof l->names[0] || l->n >= 96) return l->n < 96;
                          lab::copyStr(l->names[l->n++], sizeof l->names[0], e.name);
                          return true;
                      },
                      &list);
    if (list.n == 0) return false;
    // sort by name (insertion: small lists)
    for (int i = 1; i < list.n; ++i)
        for (int j = i; j > 0 && std::strcmp(list.names[j], list.names[j - 1]) < 0; --j) {
            char t[48];
            std::memcpy(t, list.names[j], 48);
            std::memcpy(list.names[j], list.names[j - 1], 48);
            std::memcpy(list.names[j - 1], t, 48);
        }
    int idx = -1;
    for (int i = 0; i < list.n; ++i)
        if (std::strcmp(list.names[i], cur) == 0) idx = i;
    const int next = idx < 0 ? 0 : ((idx + dir) % list.n + list.n) % list.n;
    char p[176];
    if (folder[0]) std::snprintf(p, sizeof p, "%s/%s", folder, list.names[next]);
    else lab::copyStr(p, sizeof p, list.names[next]);
    if (std::strlen(p) >= sizeof src_[c].path) return false;
    if (!loadSample(c, p)) return false;
    char m[96];
    std::snprintf(m, sizeof m, "CH %d: %.36s  (%d/%d)", c + 1, list.names[next], next + 1, list.n);
    ctx_->toast(m);
    return true;
}

void Bulut::openBrowser(int c) {
    if (!ctx_->fs) {
        ctx_->toast("No SD card");
        return;
    }
    kit::BrowserRequest q;
    static char title[48];
    std::snprintf(title, sizeof title, "Load a sample into CH %d", c + 1);
    q.title = title;
    static char folder[128];
    lab::copyStr(folder, sizeof folder, src_[c].path[0] ? src_[c].path : "Samples");
    char* s = std::strrchr(folder, '/');
    if (src_[c].path[0]) {
        if (s) *s = 0;
        else folder[0] = 0;
    }
    q.folder = folder;
    q.exts = ".wav";
    q.ctx = this;
    pendingCh_ = c;
    q.onPick = [](void* cx, const char* path) {
        Bulut* b = static_cast<Bulut*>(cx);
        lab::copyStr(b->pendingPath_, sizeof b->pendingPath_, path);   // loaded in update()
    };
    ctx_->browser().open(q);
}

void Bulut::reloadSources() {
    for (int c = 0; c < kCh; ++c) {
        ChRt& r = eng_->rt[c];
        if (par_[c].source == kSrcSample) {
            if (!src_[c].path[0] || !loadSample(c, src_[c].path)) {
                r.sampleLen = 0;
                if (src_[c].path[0]) {
                    char m[64];
                    std::snprintf(m, sizeof m, "CH %d: sample missing", c + 1);
                    ctx_->toast(m);
                }
            }
        } else {
            r.busy = 1;
            std::memset(r.buf, 0, kBufLen * 2);
            r.sampleLen = 0;
            r.busy = 0;
        }
        r.frzReady = 0;
        const uint8_t fs = st_[c].freezeSlot;
        if (fs < kFreezeSlots && frz_[fs].used) {
            spec_.synth(frz_[fs], r.frz, 0x5EEDu + uint32_t(fs) * 977u + uint32_t(c));
            r.frzReady = 1;
        } else {
            st_[c].freeze = 0;
        }
        srcNow_[c] = par_[c].source;
    }
}

// ---- projects -----------------------------------------------------------------------------
void Bulut::projectPath(int slot, char* out, uint32_t cap) const {
    std::snprintf(out, cap, "%s/Projects/P%02d.blt", ctx_->home, slot + 1);
}

void Bulut::scanProjects() {
    for (int i = 0; i < kProjects; ++i) {
        char p[96];
        projectPath(i, p, sizeof p);
        projUsed_[i] = ctx_ && ctx_->fs && ctx_->fs->exists(p) ? 1 : 0;
    }
}

bool Bulut::saveProjectTo(const char* path) {
    kit::Registry& P = ctx_->params();
    for (int c = 0; c < kCh; ++c)
        for (int i = 0; i < kKnobs; ++i) st_[c].engMem[par_[c].engine][i] = P.norm(pid_[c][i]);
    ProjectRefs r;
    r.states = st_;
    r.sources = src_;
    for (int c = 0; c < kCh; ++c) r.gestures[c] = &eng_->gestLive(c);
    r.reg = &P;
    r.scratch = chunkBuf_;
    r.scratchCap = kChunkCap;
    r.scenes = scenes_;
    r.timeline = timeline_;
    r.timelineLen = &tlLen_;
    r.freezes = frz_;
    bpmSave_ = ctx_->bpm();
    r.bpm = &bpmSave_;
    r.songMode = &songMode_;
    return saveProject(ctx_->fs, path, r) == ProjResult::kOk;
}

bool Bulut::loadProjectFrom(const char* path) {
    if (rec_->state() != GestureRecorder::kIdle) rec_->cancel();
    if (rsState_) {
        eng_->stopCapture();
        rsState_ = 0;
        if (rsPath_[0]) rsWriter_.close();
    }
    for (int c = 0; c < kCh; ++c) eng_->releaseAll(c);
    ProjectRefs r;
    r.states = st_;
    r.sources = src_;
    for (int c = 0; c < kCh; ++c) r.gestures[c] = &eng_->gestIdle(c);
    r.reg = &ctx_->params();
    r.scratch = chunkBuf_;
    r.scratchCap = kChunkCap;
    r.scenes = scenes_;
    r.timeline = timeline_;
    r.timelineLen = &tlLen_;
    r.freezes = frz_;
    r.bpm = &bpmSave_;
    r.songMode = &songMode_;
    // the registry chunk sets the engine params: park nothing on the way (the
    // file's engMem holds every engine's knobs, the chunk the active ones)
    const ProjResult res = loadProject(ctx_->fs, path, r);
    if (res != ProjResult::kOk) {
        char m[64];
        std::snprintf(m, sizeof m, "Load failed: %s", projResultText(res));
        ctx_->toast(m);
        if (res != ProjResult::kPartial) return false;
    }
    ctx_->params().applyNow();
    for (int c = 0; c < kCh; ++c) {
        engNow_[c] = par_[c].engine;
        renameKnobs(c);
        eng_->postGesture(c);
    }
    morphOn_ = false;
    curScene_ = -1;
    ctx_->setBpm(bpmSave_);
    ctx_->mod().update(false);
    reloadSources();
    return res == ProjResult::kOk;
}

void Bulut::newProject() {
    kit::Registry& P = ctx_->params();
    for (int c = 0; c < kCh; ++c) eng_->releaseAll(c);
    firstBoot();
    for (int c = 0; c < kCh; ++c) {
        for (int i = 0; i < kChParams; ++i) P.reset(pid_[c][i], false);
        P.set(pid_[c][CP::Engine], float(par_[c].engine), false);
        P.set(pid_[c][CP::Source], float(par_[c].source), false);
        engNow_[c] = par_[c].engine;
        for (int i = 0; i < kKnobs; ++i) P.setNorm(pid_[c][i], st_[c].engMem[par_[c].engine][i], false);
        renameKnobs(c);
        Gesture& g = eng_->gestIdle(c);
        g.len = 0;
        g.count = 0;
        g.quant = 0;
        eng_->postGesture(c);
    }
    for (int i = 0; i < kMasterParams; ++i) P.reset(mid_[i], false);
    ctx_->mod().clearRoutes();
    for (int s = 0; s < kScenes; ++s) scenes_[s].used = 0;
    for (int f = 0; f < kFreezeSlots; ++f) frz_[f].used = 0;
    tlLen_ = 0;
    songMode_ = 0;
    reloadSources();
    ctx_->toast("New project");
}

void Bulut::save() {
    if (!ctx_->fs) return;
    char p[96];
    std::snprintf(p, sizeof p, "%s/autosave.blt", ctx_->home);
    saveProjectTo(p);
    WsSettings s;
    s.grains = setGrains_;
    s.cloudGrains = setCloudGrains_;
    s.velocity = setVelocity_;
    s.midiCh = setMidiCh_;
    s.leds = setLeds_;
    s.restore = setAutosave_;
    std::snprintf(p, sizeof p, "%s/bulut.cfg", ctx_->home);
    const ot::IStorage::Handle h = ctx_->fs->open(p, ot::IStorage::kWrite);
    if (h >= 0) {
        ctx_->fs->write(h, &s, sizeof s);
        ctx_->fs->close(h);
    }
}

// ---- settings rows -------------------------------------------------------------------------
int Bulut::settings(lab::SettingRow* rows, int cap) {
    int n = 0;
    auto add = [&](const char* label, int* v, int mn, int mx, const char* const* names, const char* unit) {
        if (n >= cap) return;
        lab::SettingRow& r = rows[n++];
        r.label = label;
        r.value = v;
        r.min = mn;
        r.max = mx;
        r.names = names;
        r.unit = unit;
        r.ctx = this;
        r.onChange = [](void* c, int) {
            Bulut* b = static_cast<Bulut*>(c);
            b->eng_->grainBudget = b->setGrains_;
            b->eng_->cloudBudget = b->setCloudGrains_;
        };
    };
    add("BULUT grain budget", &setGrains_, 8, kMaxGrains, nullptr, " grains");
    add("BULUT grains per channel", &setCloudGrains_, 2, kMaxCloudGrains, nullptr, " grains");
    add("BULUT pad velocity", &setVelocity_, 0, 1, kOffOn, "");
    add("BULUT MIDI notes play", &setMidiCh_, 0, 4, kMidiCh, "");
    add("BULUT pad LEDs", &setLeds_, 0, 2, kLedLevels, "");
    add("BULUT restore last session", &setAutosave_, 0, 1, kOffOn, "");
    return n;
}

// ---- MIDI ------------------------------------------------------------------------------------
void Bulut::onMidi(const lab::MidiMsg& m) {
    const int c = setMidiCh_ > 0 ? setMidiCh_ - 1 : sel_;
    const uint8_t t = m.type();
    if (t == 0x90 && m.d2) {
        eng_->noteOn(c, m.d1, setVelocity_ ? m.d2 : 100);
    } else if (t == 0x80 || (t == 0x90 && m.d2 == 0)) {
        eng_->noteOff(c, m.d1);
    } else if (t == 0xB0) {
        const float v = float(m.d2) / 127.0f;
        if (m.d1 == 1) setNorm(c, K::Pos, v, true);
        else if (m.d1 >= 20 && m.d1 < 20 + kChParams && m.d1 - 20 != CP::Engine && m.d1 - 20 != CP::Source)
            setNorm(c, m.d1 - 20, v, true);
        else if (m.d1 == 123) eng_->releaseAll(c);
    }
}

// ---- events ------------------------------------------------------------------------------------
// Shift + pads: row 1 cols 1-4 select a channel, 5-8 mute, 9-12 solo; row 2
// cols 1-8 scenes (empty = store, used = morph), 9-12 gesture REC of channel
// 1-4; row 3 freeze slots 1-12 (empty = capture, used = recall); row 4 cols
// 1-4 freeze on/off, 5-8 drone on/off, 9-12 gesture play on/off.
void Bulut::shiftPad(int pad) {
    const int row = pad / 12, col = pad % 12;
    if (row == 0) {
        if (col < 4) selectChannel(col);
        else if (col < 8) st_[col - 4].mute ^= 1;
        else eng_->soloMask = uint8_t(eng_->soloMask ^ (1u << (col - 8)));
    } else if (row == 1) {
        if (col < kScenes) {
            if (scenes_[col].used) recallScene(col, true);
            else storeScene(col);
        } else {
            gestureRecButton(col - 8);
        }
    } else if (row == 2) {
        if (frz_[col].used) recallFreeze(col);
        else captureFreeze(col, frzFrom_);
    } else {
        if (col < 4) toggleFreeze(col);
        else if (col < 8) st_[col - 4].drone ^= 1;
        else st_[col - 8].gestOn ^= 1;
    }
}

// REGION pads (SOURCE page, F8): the 48 pads are the whole source in 48 steps;
// the first tap sets one end, the second the other.
void Bulut::regionPad(int pad) {
    if (regionFirst_ < 0) {
        regionFirst_ = pad;
        setNorm(sel_, CP::Start, float(pad) / 48.0f, true);
        setNorm(sel_, CP::End, float(pad + 1) / 48.0f, true);
        return;
    }
    const int a = regionFirst_ < pad ? regionFirst_ : pad, b = regionFirst_ < pad ? pad : regionFirst_;
    setNorm(sel_, CP::Start, float(a) / 48.0f, true);
    setNorm(sel_, CP::End, float(b + 1) / 48.0f, true);
    regionFirst_ = -1;
    char m[48];
    std::snprintf(m, sizeof m, "Region %d%% .. %d%%", a * 100 / 48, (b + 1) * 100 / 48);
    ctx_->toast(m);
}

void Bulut::onEvent(const Event& e) {
    if (e.type == Event::kPress && e.id == btn::Shift) shift_ = true;
    if (e.type == Event::kRelease && e.id == btn::Shift) shift_ = false;
    if (e.type == Event::kPadDown) {
        if (e.shift || shift_) {
            shiftPad(e.id);
            return;
        }
        if (page_ == kPgSource && regionMode_) {
            regionPad(e.id);
            return;
        }
        const int vel = setVelocity_ ? e.velocity : 100;
        held_ |= 1ull << e.id;
        if (rec_->state() == GestureRecorder::kArmed && rec_->cloud() == sel_) waitGesture(sel_);
        eng_->padDown(sel_, e.id, vel);
        rec_->pad(true, e.id, vel, eng_->sampleClock);
        return;
    }
    if (e.type == Event::kPadUp) {
        if (!((held_ >> e.id) & 1)) return;
        held_ &= ~(1ull << e.id);
        eng_->padUp(sel_, e.id);
        rec_->pad(false, e.id, 0, eng_->sampleClock);
        return;
    }
    if (e.type == Event::kEncoder) {
        encoder(e.delta, e.shift);
        return;
    }
    if (e.type != Event::kPress) return;
    const int id = e.id;
    if (id <= btn::F8) {
        if (e.shift) action(id, true);
        else if (id == btn::F8) action(7, false);
        else setPage(id);
        return;
    }
    switch (id) {
        case btn::Up:
        case btn::Down:
        case btn::Left:
        case btn::Right: arrows(id); return;
        case btn::Insert:
        case btn::EncPush: insertKey(); return;   // EncPush: the simulator only (not on the Tracker)
        case btn::Delete: deleteKey(e.shift); return;
        case btn::Rec: gestureRecButton(sel_); return;
        case btn::Fx1: toggleFreeze(sel_); return;
        case btn::Fx2:
            st_[sel_].drone ^= 1;
            ctx_->toast(st_[sel_].drone ? "Drone on: the cloud breathes" : "Drone off");
            return;
        case btn::Instr: {
            setEngine(sel_, par_[sel_].engine + (e.shift ? -1 : 1));
            char m[32];
            std::snprintf(m, sizeof m, "CH %d: %s", sel_ + 1, engineName(par_[sel_].engine));
            ctx_->toast(m);
            return;
        }
        case btn::Note: selectChannel(sel_ + (e.shift ? -1 : 1)); return;
        case btn::Copy: swapSample(sel_, e.shift ? -1 : 1); return;
        case btn::SampleRecorder:
            if (rsState_ == 1) stopResample();
            else if (!rsState_) startResample();
            return;
        case btn::SampleLoader: openBrowser(sel_); return;
        case btn::Performance: setPage(kPgClouds); return;
        case btn::SamplePlayback: setPage(kPgSound); return;
        case btn::SampleEditor: setPage(kPgSource); return;
        case btn::Pattern: setPage(kPgMotion); return;
        case btn::Song:
            if (e.shift) {
                songMode_ ^= 1;
                ctx_->toast(songMode_ ? "Song mode on: Play runs the timeline" : "Song mode off");
            } else {
                setPage(kPgMotion);
                motionFocus_ = 1;
            }
            return;
        case btn::File: setPage(kPgFile); return;
        default: return;
    }
}

// F8 (shift = false) or Shift + F1..F8 (k = 0..7): the page's actions.
void Bulut::action(int k, bool shift) {
    const int c = sel_;
    kit::Registry& P = ctx_->params();
    switch (page_) {
        case kPgClouds:
            if (!shift) {
                gestureRecButton(c);
                return;
            }
            if (k == 0) st_[c].mute ^= 1;
            if (k == 1) eng_->soloMask = eng_->soloMask == (1u << c) ? 0 : uint8_t(1u << c);
            if (k == 2) toggleFreeze(c);
            if (k == 3) st_[c].drone ^= 1;
            if (k == 4) st_[c].hold ^= 1;
            if (k == 5) st_[c].stem = uint8_t((st_[c].stem + 1) & 3);
            if (k == 6) st_[c].gestOn ^= 1;
            if (k == 7) storeScene(curScene_ >= 0 ? curScene_ : 0);
            return;
        case kPgSound:
            if (!shift) {
                setEngine(c, par_[c].engine + 1);
                return;
            }
            if (k < 4) setEngine(c, k);
            if (k == 4) {
                float d[kKnobs];
                defaultEngineKnobs(par_[c].engine, d, c);
                for (int i = 0; i < kKnobs; ++i) setNorm(c, i, d[i], false);
                ctx_->toast("Knobs reset");
            }
            if (k == 5) {
                lab::dsp::Rng r(eng_->sampleClock * 2654435761u + 1u);
                for (int i = 0; i < kKnobs; ++i)
                    if (i != K::Mode || par_[c].engine != kBeads) setNorm(c, i, r.uni(), false);
                ctx_->toast("Knobs randomised");
            }
            if (k == 6) st_[c].drone ^= 1;
            if (k == 7) toggleFreeze(c);
            return;
        case kPgSource:
            if (!shift) {
                regionMode_ = !regionMode_;
                regionFirst_ = -1;
                ctx_->toast(regionMode_ ? "REGION pads: tap start, tap end (F8: done)" : "Pads play again");
                return;
            }
            if (k < 4) setSource(c, k);
            if (k == 4) openBrowser(c);
            if (k == 5) swapSample(c, -1);
            if (k == 6) swapSample(c, 1);
            if (k == 7) {
                st_[c].hold ^= 1;
                ctx_->toast(st_[c].hold ? "Buffer held" : "Buffer recording");
            }
            return;
        case kPgFx:
            if (!shift) {
                addRoute(c);
                return;
            }
            if (k == 0) P.set(pid_[c][CP::FxMode], 0.0f, false);
            if (k == 1) P.set(pid_[c][CP::FxMode], float((par_[c].fxMode + 1) % kFxModes), false);
            if (k == 2) P.set(pid_[c][CP::Flt], float((par_[c].fltMode + 1) % kFltModes), false);
            if (k == 3) P.set(pid_[c][CP::Drive], par_[c].drive > 0.01f ? 0.0f : 0.5f, false);
            if (k == 7) {
                int idx[kit::kMaxRoutes];
                const int n = ctx_->mod().routesTo(pid_[c][cursorParam() >= 0 ? cursorParam() : CP::Cutoff], idx, kit::kMaxRoutes);
                if (n > 0) ctx_->mod().removeRoute(idx[0]);
            }
            return;
        case kPgMotion: {
            if (!shift) {
                gestureRecButton(c);
                return;
            }
            if (k == 0) {
                gestMode_ ^= 1;
                ctx_->toast(gestMode_ ? "Rec mode: overdub" : "Rec mode: new");
            }
            if (k == 1) {
                Gesture& g = eng_->gestIdle(c);
                g.len = 0;
                g.count = 0;
                eng_->postGesture(c);
                ctx_->toast("Gesture cleared");
            }
            if (k == 2) st_[c].gestOn ^= 1;
            if (k == 3)
                for (int i = 0; i < kCh; ++i) st_[i].gestOn = 1;
            if (k == 4)
                for (int i = 0; i < kCh; ++i) st_[i].gestOn = 0;
            if (k == 5) {
                songMode_ ^= 1;
                tlPlaying_ = -1;
                ctx_->toast(songMode_ ? "Song mode on: Play runs the timeline" : "Song mode off");
            }
            if (k == 6 && tlLen_ < kTimeline && scenes_[scnCur_].used) {
                timeline_[tlLen_].scene = uint8_t(scnCur_);
                timeline_[tlLen_].bars = 4;
                tlCur_ = tlLen_++;
            }
            if (k == 7) storeScene(scnCur_);
            return;
        }
        case kPgFreeze:
            if (!shift) {
                captureFreeze(frzCur_, frzFrom_);
                return;
            }
            if (k == 0) recallFreeze(frzCur_);
            if (k == 1) frz_[frzCur_].used = 0;
            if (k == 2) frzFrom_ = frzFrom_ == kFromSource ? kFromMix : kFromSource;
            if (k == 3) st_[c].freeze = 0;
            if (k == 4) P.set(tid_[5], float(frzCur_), false);
            if (k == 5)
                for (int i = 0; i < 5; ++i) P.reset(tid_[i], false);
            return;
        case kPgFile: {
            char p[96];
            projectPath(fileCur_, p, sizeof p);
            if (!shift || k == 7) {
                ctx_->toast(saveProjectTo(p) ? "Project saved" : "Save failed (card?)");
                scanProjects();
                return;
            }
            if (k == 0) {
                if (projUsed_[fileCur_]) {
                    if (loadProjectFrom(p)) ctx_->toast("Project loaded");
                } else {
                    ctx_->toast("Empty slot");
                }
            }
            if (k == 1) newProject();
            if (k == 2) {
                if (rsState_ == 1) stopResample();
                else if (!rsState_) startResample();
            }
            return;
        }
        default: return;
    }
}

// LABKIT route from the page: the channel's LFO onto the focused param (depth 25 %).
void Bulut::addRoute(int c) {
    int idx = cursorParam();
    if (idx < 0) idx = CP::Cutoff;
    const int src = ctx_->mod().findSource(c == 0 ? "C1 LFO" : (c == 1 ? "C2 LFO" : (c == 2 ? "C3 LFO" : "C4 LFO")));
    if (src < 0) return;
    if (!ctx_->params().modulatable(pid_[c][idx])) {
        ctx_->toast("That one is a switch: no modulation");
        return;
    }
    const int r = ctx_->mod().route(src, pid_[c][idx], 0.25f);
    char m[48];
    std::snprintf(m, sizeof m, r >= 0 ? "C%d LFO -> %s" : "No room for a route", c + 1, paramName(c, idx));
    ctx_->toast(m);
}

// The channel param under the page's cursor (-1 = none).
int Bulut::cursorParam() const {
    switch (page_) {
        case kPgSound: return cur_[kPgSound] < kKnobs ? cur_[kPgSound] : -1;
        case kPgSource: return cur_[kPgSource] < 8 ? kSrcRows[cur_[kPgSource]] : -1;
        case kPgFx: return cur_[kPgFx] < 10 ? kFxRows[cur_[kPgFx]] : -1;
        case kPgClouds: {
            static const uint8_t kCol[4] = {CP::Level, CP::Pan, CP::Dly, CP::Rev};
            return cloudsRow_ < kCh ? kCol[cur_[kPgClouds] & 3] : -1;
        }
        default: return -1;
    }
}

void Bulut::arrows(int id) {
    const int dx = id == btn::Left ? -1 : (id == btn::Right ? 1 : 0);
    const int dy = id == btn::Up ? -1 : (id == btn::Down ? 1 : 0);
    int& cur = cur_[page_];
    switch (page_) {
        case kPgClouds:
            if (dy) {
                cloudsRow_ = (cloudsRow_ + dy + kCh + 1) % (kCh + 1);   // 4 = the SEND FX row
                if (cloudsRow_ < kCh) selectChannel(cloudsRow_);
            }
            if (dx) cur = (cur + dx + (cloudsRow_ < kCh ? 4 : 7) * 4) % (cloudsRow_ < kCh ? 4 : 7);
            break;
        case kPgSound:
            if (dx) cur = (cur + dx + kKnobs) % kKnobs;
            if (dy) cur = (cur + dy * 4 + kKnobs) % kKnobs;
            break;
        case kPgSource:
            if (dy) cur = (cur + dy + 9) % 9;
            if (dx) encoder(dx, false);
            break;
        case kPgFx: {
            const int n = 10 + ctx_->mod().routeCount();
            if (dy) cur = (cur + dy + n) % n;
            if (dx && cur < 10) cur = (cur + dx * (cur < 8 ? 4 : 1) + 10) % 10;
            break;
        }
        case kPgMotion:
            if (dy) motionFocus_ = (motionFocus_ + dy + 3) % 3;
            if (dx) {
                if (motionFocus_ == 0) cur = (cur + dx + 4) % 4;
                if (motionFocus_ == 1) scnCur_ = (scnCur_ + dx + kScenes) % kScenes;
                if (motionFocus_ == 2 && tlLen_ > 0) tlCur_ = (tlCur_ + dx + tlLen_) % tlLen_;
            }
            break;
        case kPgFreeze:
            // Left / Right walk the 16 slots, Down: the tools row, Up: back
            if (frzFocus_ == 0) {
                if (dx) frzCur_ = (frzCur_ + dx + kFreezeSlots) % kFreezeSlots;
                if (dy > 0) frzFocus_ = 1;
            } else {
                if (dx) frzTool_ = (frzTool_ + dx + 7) % 7;
                if (dy < 0) frzFocus_ = 0;
            }
            break;
        case kPgFile:
            if (dy) {
                // two columns of 8: Down past the bottom of a column reaches the resample panel
                if (fileFocus_ == 0) {
                    const int row = fileCur_ % 8 + dy;
                    if (row > 7) fileFocus_ = 1;
                    else if (row >= 0) fileCur_ += dy;
                } else if (dy < 0) {
                    fileFocus_ = 0;
                }
            }
            if (dx && fileFocus_ == 0) fileCur_ = (fileCur_ + dx * 8 + kProjects) % kProjects;
            if (dx && fileFocus_ == 1) encoder(dx, false);
            break;
        default: break;
    }
}

void Bulut::encoder(int d, bool fine) {
    kit::Registry& P = ctx_->params();
    const int c = sel_;
    auto nudge = [&](int idx) {
        const kit::ParamId id = pid_[c][idx];
        if (idx < kKnobs && knobSteps(par_[c].engine, idx) > 1) {
            const int n = knobSteps(par_[c].engine, idx);
            setNorm(c, idx, enumValue(enumIndex(P.norm(id), n) + d, n), true);
            return;
        }
        P.nudge(id, d, fine, false);
        if (idx == CP::Engine) syncEngines();
        if (rec_->state() == GestureRecorder::kArmed && rec_->cloud() == c) waitGesture(c);
        if (rec_->state() == GestureRecorder::kRecording && rec_->cloud() == c && idx != CP::Engine && idx != CP::Source)
            rec_->param(idx, P.norm(id), eng_->sampleClock);
    };
    switch (page_) {
        case kPgClouds:
            if (cloudsRow_ < kCh) nudge(cursorParam());
            else P.nudge(mid_[cur_[kPgClouds] % 7], d, fine, false);
            break;
        case kPgSound: nudge(cur_[kPgSound] % kKnobs); break;
        case kPgSource:
            if (cur_[kPgSource] < 8) {
                const int idx = kSrcRows[cur_[kPgSource]];
                if (idx == CP::Source) setSource(c, par_[c].source + d);
                else nudge(idx);
            } else {
                radioMHz_ = lab::dsp::clamp(radioMHz_ + float(d) * 0.1f, 87.5f, 108.0f);
                radioMHz_ = floorf(radioMHz_ * 10.0f + 0.5f) * 0.1f;
                if (ctx_->radio) ctx_->radio->tune(radioMHz_);
            }
            break;
        case kPgFx:
            if (cur_[kPgFx] < 10) {
                nudge(kFxRows[cur_[kPgFx]]);
            } else {
                const int ri = cur_[kPgFx] - 10;
                if (ri < ctx_->mod().routeCount()) {
                    const kit::Route& r = ctx_->mod().routeAt(ri);
                    ctx_->mod().setDepth(ri, lab::dsp::clamp(r.depth + float(d) * (fine ? 0.005f : 0.02f), -1.0f, 1.0f));
                }
            }
            break;
        case kPgMotion:
            if (motionFocus_ == 0) {
                Gesture& g = eng_->gestLive(c);
                switch (cur_[kPgMotion] & 3) {
                    case 0: st_[c].gestOn = d > 0 ? 1 : 0; break;
                    case 1: nudge(CP::GestSpeed); break;
                    case 2: {
                        const int v = int(g.quant) + d;
                        g.quant = uint8_t(v < 0 ? 0 : (v >= kQuants ? kQuants - 1 : v));
                        break;
                    }
                    default: gestMode_ = d > 0 ? 1 : 0; break;
                }
            } else if (motionFocus_ == 1) {
                if (fine) P.nudge(mid_[kMMorph], d, false, true);
                else scnCur_ = (scnCur_ + d + kScenes * 8) % kScenes;
            } else if (tlLen_ > 0) {
                TimeEntry& te = timeline_[tlCur_];
                if (fine) te.scene = uint8_t((te.scene + d + kScenes * 8) % kScenes);
                else {
                    const int b = te.bars + d;
                    te.bars = uint8_t(b < 1 ? 1 : (b > 64 ? 64 : b));
                }
            }
            break;
        case kPgFreeze:
            if (frzFocus_ == 0) nudge(CP::Bed);
            else if (frzTool_ == 0) nudge(CP::Bed);
            else P.nudge(tid_[frzTool_ - 1], d, fine, false);
            break;
        case kPgFile:
            if (fileFocus_ == 0) {
                fileCur_ = (fileCur_ + d + kProjects * 8) % kProjects;
            } else if (fine) {
                rsDst_ = (rsDst_ + d + kCh * 8) % kCh;
            } else {
                rsSrc_ += d;
                if (rsSrc_ < -1) rsSrc_ = kCh - 1;
                if (rsSrc_ >= kCh) rsSrc_ = -1;
            }
            break;
        default: break;
    }
}

// Insert: confirm / use (stands in for the encoder push the Tracker does not have).
void Bulut::insertKey() {
    switch (page_) {
        case kPgSource: openBrowser(sel_); break;
        case kPgFx: addRoute(sel_); break;
        case kPgMotion:
            if (motionFocus_ == 2) {
                if (tlLen_ < kTimeline) {
                    for (int i = tlLen_; i > tlCur_ + 1 && i > 0; --i) timeline_[i] = timeline_[i - 1];
                    const int at = tlLen_ ? tlCur_ + 1 : 0;
                    timeline_[at].scene = uint8_t(curScene_ >= 0 ? curScene_ : scnCur_);
                    timeline_[at].bars = 4;
                    ++tlLen_;
                    tlCur_ = at;
                }
            } else if (motionFocus_ == 1) {
                if (scenes_[scnCur_].used) recallScene(scnCur_, true);
                else storeScene(scnCur_);
            } else {
                gestureRecButton(sel_);
            }
            break;
        case kPgFreeze:
            if (frz_[frzCur_].used) recallFreeze(frzCur_);
            else captureFreeze(frzCur_, frzFrom_);
            break;
        case kPgFile:
            if (fileFocus_ == 1) {
                if (rsState_ == 1) stopResample();
                else if (!rsState_) startResample();
            } else {
                action(0, true);   // load
            }
            break;
        default: {
            // CLOUDS / SOUND: store the scene being played (or the first one)
            int s = curScene_;
            if (s < 0)
                for (int i = 0; i < kScenes && s < 0; ++i)
                    if (!scenes_[i].used) s = i;
            storeScene(s < 0 ? 0 : s);
            break;
        }
    }
}

void Bulut::deleteKey(bool shift) {
    kit::Registry& P = ctx_->params();
    const int c = sel_;
    switch (page_) {
        case kPgMotion:
            if (motionFocus_ == 2 && tlLen_ > 0) {
                for (int i = tlCur_; i + 1 < tlLen_; ++i) timeline_[i] = timeline_[i + 1];
                --tlLen_;
                if (tlCur_ >= tlLen_ && tlCur_ > 0) --tlCur_;
            } else if (motionFocus_ == 1) {
                scenes_[scnCur_].used = 0;
            } else {
                Gesture& g = eng_->gestIdle(c);
                g.len = 0;
                g.count = 0;
                eng_->postGesture(c);
                ctx_->toast("Gesture cleared");
            }
            return;
        case kPgFreeze:
            if (frzFocus_ == 0) frz_[frzCur_].used = 0;
            else if (frzTool_ > 0) P.reset(tid_[frzTool_ - 1], false);
            return;
        case kPgFx:
            if (cur_[kPgFx] >= 10) {
                const int ri = cur_[kPgFx] - 10;
                if (ri < ctx_->mod().routeCount()) ctx_->mod().removeRoute(ri);
                return;
            }
            break;
        case kPgFile: return;
        default: break;
    }
    // the param under the cursor back to its default; Shift: also no modulation on it
    const int idx = cursorParam();
    if (idx < 0) {
        if (page_ == kPgClouds && cloudsRow_ == kCh) P.reset(mid_[cur_[kPgClouds] % 7], false);
        return;
    }
    if (idx < kKnobs) {
        float d[kKnobs];
        defaultEngineKnobs(par_[c].engine, d, c);
        setNorm(c, idx, d[idx], true);
    } else if (idx != CP::Engine && idx != CP::Source) {
        P.reset(pid_[c][idx], false);
    }
    if (shift) {
        int ri[kit::kMaxRoutes];
        const int n = ctx_->mod().routesTo(pid_[c][idx], ri, kit::kMaxRoutes);
        for (int i = n - 1; i >= 0; --i) ctx_->mod().removeRoute(ri[i]);
    }
}

// ---- main loop -------------------------------------------------------------------------------
void Bulut::pumpParamEvents() {
    ParamEvent pe;
    kit::Registry& P = ctx_->params();
    int n = 0;
    while (eng_->paramEvents.pop(pe) && n++ < 64) {
        if (pe.ch >= kCh || pe.idx >= kChParams || pe.idx == CP::Engine || pe.idx == CP::Source) continue;
        P.setNorm(pid_[pe.ch][pe.idx], float(pe.v) * (1.0f / 65535.0f), false);
    }
}

void Bulut::update(uint32_t ms) {
    const float dt = lastMs_ ? float(ms - lastMs_) * 0.001f : 0.0f;
    lastMs_ = ms;
    syncEngines();
    for (int c = 0; c < kCh; ++c)
        if (par_[c].source != srcNow_[c]) {
            srcNow_[c] = par_[c].source;
            if (par_[c].source == kSrcSample && eng_->rt[c].sampleLen == 0 && src_[c].path[0]) loadSample(c, src_[c].path);
        }
    if (pendingPath_[0] && pendingCh_ >= 0) {
        const int c = pendingCh_;
        char p[160];
        lab::copyStr(p, sizeof p, pendingPath_);
        pendingPath_[0] = 0;
        pendingCh_ = -1;
        char m[64];
        const char* b = std::strrchr(p, '/');
        if (std::strlen(p) < sizeof src_[c].path && loadSample(c, p))
            std::snprintf(m, sizeof m, "%.40s -> CH %d", b ? b + 1 : p, c + 1);
        else
            std::snprintf(m, sizeof m, "Cannot load %.40s", b ? b + 1 : p);
        ctx_->toast(m);
    }
    pumpParamEvents();
    pumpMorph(dt > 2.0f ? 2.0f : dt);   // wall clock (frames are ~16 ms)
    pumpTimeline();
    pumpResample();
    pumpParticles(dt > 0.1f ? 0.1f : dt);
    if (ms - lastPeakMs_ >= 100 || ms < lastPeakMs_) {
        lastPeakMs_ = ms;
        updatePeaks();
    }
    for (int c = 0; c < kCh; ++c) {
        if (eng_->rt[c].gWrapped) {
            eng_->rt[c].gWrapped = 0;
            gestFlash_[c] = 1.0f;
        }
        gestFlash_[c] *= 0.9f;
    }
    // Config > Radio is the saved station: follow it when it changes there
    if (ctx_->shell) {
        const float sh = ctx_->shell->radioMHz();
        if (sh != shellRadio_) {
            shellRadio_ = sh;
            radioMHz_ = sh;
        }
    }
}

void Bulut::updatePeaks() {
    for (int c = 0; c < kCh; ++c) {
        const ChRt& r = eng_->rt[c];
        const bool sample = par_[c].source == kSrcSample;
        const uint32_t len = sample ? r.sampleLen : kLiveSpan;
        const uint32_t base = sample ? 0 : (r.w - kLiveSpan) & kBufMask;
        float colMax[12] = {};
        for (int s = 0; s < 96; ++s) {
            int m = 0;
            if (len) {
                const uint32_t a = uint32_t(uint64_t(len) * uint32_t(s) / 96u);
                const uint32_t step = len / 96u / 16u + 1u;
                for (uint32_t k = 0; k < 16; ++k) {
                    const int v = r.buf[(base + a + k * step) & kBufMask];
                    const int av = v < 0 ? -v : v;
                    m = av > m ? av : m;
                }
            }
            const int h = m >> 7;
            wave_[c][s] = uint8_t(h > 255 ? 255 : h);
            // the pads read the region: their columns follow it
            const float x = (float(s) + 0.5f) / 96.0f;
            const float a0 = par_[c].regStart < par_[c].regEnd ? par_[c].regStart : par_[c].regEnd;
            const float a1 = par_[c].regStart < par_[c].regEnd ? par_[c].regEnd : par_[c].regStart;
            if (x >= a0 && x <= a1 && a1 > a0) {
                const int col = int((x - a0) / (a1 - a0) * 12.0f);
                const float f = float(m) / 32768.0f;
                if (col >= 0 && col < 12) colMax[col] = f > colMax[col] ? f : colMax[col];
            }
        }
        for (int k = 0; k < 12; ++k) colPeak_[c][k] = uint8_t(sqrtf(colMax[k]) * 255.0f);
        // the region itself, 64 columns (what the pads and the lanes show)
        float a0 = par_[c].regStart < par_[c].regEnd ? par_[c].regStart : par_[c].regEnd;
        float a1 = par_[c].regStart < par_[c].regEnd ? par_[c].regEnd : par_[c].regStart;
        if (a1 - a0 < 0.002f) a1 = a0 + 0.002f;
        for (int s = 0; s < 64; ++s) {
            int m = 0;
            if (len) {
                const float f0 = a0 + (a1 - a0) * float(s) / 64.0f;
                const uint32_t a = uint32_t(f0 * float(len));
                const uint32_t span = uint32_t((a1 - a0) * float(len) / 64.0f) + 1u;
                const uint32_t step = span / 12u + 1u;
                for (uint32_t k = 0; k < 12; ++k) {
                    const int v = r.buf[(base + a + k * step) & kBufMask];
                    const int av = v < 0 ? -v : v;
                    m = av > m ? av : m;
                }
            }
            const int h = m >> 7;
            regWave_[c][s] = uint8_t(h > 255 ? 255 : h);
        }
    }
}

void Bulut::pumpParticles(float dt) {
    Spark s;
    static lab::dsp::Rng vr(0xC10D);
    int n = 0;
    while (eng_->sparks.pop(s)) {
        if (++n > 120) continue;   // drain everything, draw at most 120 new grains per pass
        if (s.cloud >= kCh) continue;
        const float a = float(s.amp) / 255.0f;
        const float life = float(s.life) * 0.01f;   // grain length, seconds
        const int row = s.live & 7;
        const float x0 = float(s.pos) / 65535.0f;
        const float y0 = row < 4 ? (float(row) + 0.5f) / 4.0f : 0.5f - float(s.semis) * 0.008f;
        // a grain is a small puff: one bright core and a few drifting motes
        const int motes = s.kind >= 3 && s.kind != 6 ? 1 : (life > 0.2f ? 4 : (life > 0.05f ? 3 : 2));
        for (int k = 0; k < motes; ++k) {
            Particle& p = part_[partHead_];
            partHead_ = (partHead_ + 1) % kParticles;
            p.cloud = s.cloud;
            p.kind = s.kind;
            p.live = s.live & 0x80 ? 1 : 0;
            const float spread = k == 0 ? 0.25f : 1.0f;
            p.x = x0 + (vr.uni() - 0.5f) * (0.035f + 0.05f * life) * spread;
            p.y = y0 + (vr.uni() - 0.5f) * 0.20f * spread - float(s.semis) * 0.004f;
            p.vx = (vr.uni() - 0.5f) * 0.04f;
            p.vy = -(0.02f + 0.07f * vr.uni()) * (k == 0 ? 0.5f : 1.0f);
            const float l = 0.5f + life * 3.0f + 0.4f * vr.uni();
            p.life = l > 2.8f ? 2.8f : l;
            p.age = 0.0f;
            if (k == 0) p.size = uint8_t(s.kind >= 3 && s.kind != 6 ? (a > 0.3f ? 1 : 0) : (life > 0.15f ? 2 : 1));
            else p.size = 0;
        }
        const int col = int(x0 * 12.0f);
        if (col >= 0 && col < 12) colGlow_[s.cloud][col] = lab::dsp::clamp(colGlow_[s.cloud][col] + 0.3f * (0.4f + a), 0.0f, 1.0f);
    }
    for (int i = 0; i < kParticles; ++i) {
        Particle& p = part_[i];
        if (p.life <= 0.0f) continue;
        p.age += dt;
        if (p.age >= p.life) {
            p.life = 0.0f;
            continue;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
    }
    const float k = expf(-dt * 3.0f);
    for (int c = 0; c < kCh; ++c)
        for (int col = 0; col < 12; ++col) colGlow_[c][col] *= k;
}

}  // namespace bulut

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<bulut::Bulut>(); }
}  // namespace lab
