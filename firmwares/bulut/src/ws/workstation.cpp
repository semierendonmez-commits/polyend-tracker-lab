// BULUT workstation: state, controls, pages, files (bulut_ws.h).
// Drawing lives in draw.cpp, the audio engine in engine.cpp.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lab/dsp.h"
#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

namespace bulut {

using lab::Event;
namespace btn = lab::btn;

namespace {
const char* const kOffOn[] = {"Off", "On"};
const char* const kMidiCloud[] = {"Selected", "1", "2", "3", "4", "5", "6"};
const char* const kMorph[] = {"Cut", "0.5 s", "1 s", "2 s", "4 s", "8 s"};
const float kMorphSec[] = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
const char* const kLedLevels[] = {"Low", "Medium", "High"};

struct WsSettings {
    uint32_t magic = 0x42554C54u;   // "BULT"
    uint32_t version = 1;
    int grains = kDefaultGrains, cloudGrains = kDefaultCloudGrains, velocity = 1, midiCloud = 0, morph = 2, leds = 2, restore = 1;
};
}  // namespace

// ---- init ---------------------------------------------------------------------
bool Bulut::init(lab::Context& c) {
    ctx_ = &c;
    theme_.bg = lab::Rgb(0x090D1A);
    theme_.panel = lab::Rgb(0x151B30);
    theme_.fg = lab::Rgb(0xE6EAF4);
    theme_.dim = lab::Rgb(0x6E7894);
    theme_.accent = lab::Rgb(0x9CD6F2);
    theme_.accent2 = lab::Rgb(0xF2B6C8);
    theme_.warn = lab::Rgb(0xFF6B6B);
    theme_.ok = lab::Rgb(0x92E2B4);

    lab::Arena& a = *c.arena;
    eng_ = a.make<Engine>();
    rec_ = a.make<GestureRecorder>();
    gest_ = a.make<Gesture>(kClouds * kGestBufs);
    scenes_ = a.make<Scene>(kScenes);
    frz_ = a.make<FreezeSlot>(kFreezeSlots);
    morphFrom_ = a.make<CloudState>(kClouds);
    morphTo_ = a.make<CloudState>(kClouds);
    if (!eng_ || !rec_ || !gest_ || !scenes_ || !frz_ || !morphFrom_ || !morphTo_) return false;
    for (int i = 0; i < kClouds; ++i) {
        defaultCloud(clouds_[i], i);
        std::memset(&sources_[i], 0, sizeof sources_[i]);
        sources_[i].type = kSrcLiveIn;
        sources_[i].inGain = 1.0f;
        for (int k = 0; k < kGestBufs; ++k) gest_[i * kGestBufs + k].speed = 2;
    }
    // A pleasant first state: cloud 2 listens to BULUT's own output, 3 is QUANTA.
    sources_[1].type = kSrcLiveOut;
    sources_[1].inGain = 0.8f;
    clouds_[2].set(0, P::Engine, 1.0f);
    clouds_[2].p[0][P::Level] = clouds_[2].p[1][P::Level] = 0.6f;
    defaultFx(fx_);
    if (!eng_->init(a, clouds_, sources_, gest_, &fx_)) return false;
    if (!spec_.init(a)) return false;
    for (int i = 0; i < kParticles; ++i) part_[i].life = 0.0f;

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
            if (c.fs->read(h, &s, sizeof s) == int32_t(sizeof s) && s.magic == WsSettings().magic && s.version == 1) {
                setGrains_ = s.grains < 8 ? 8 : (s.grains > kMaxGrains ? kMaxGrains : s.grains);
                setCloudGrains_ = s.cloudGrains < 2 ? 2 : (s.cloudGrains > kMaxCloudGrains ? kMaxCloudGrains : s.cloudGrains);
                setVelocity_ = s.velocity;
                setMidiCloud_ = s.midiCloud;
                setMorph_ = s.morph;
                setLeds_ = s.leds;
                setAutosave_ = s.restore;
            }
            c.fs->close(h);
        }
        if (!c.fs->exists("Samples")) std::strcpy(browseDir_, "");
        if (setAutosave_) loadAutosave();
    }
    eng_->grainBudget = setGrains_;
    eng_->cloudBudget = setCloudGrains_;
    radioMHz_ = c.shell ? c.shell->radioMHz() : 100.0f;
    scanBrowse();
    scanProjects();
    return true;
}

void Bulut::loadAutosave() {
    char p[96];
    std::snprintf(p, sizeof p, "%s/autosave.blt", ctx_->home);
    if (ctx_->fs && ctx_->fs->exists(p)) loadProjectFrom(p);
}

const char* Bulut::benchName(int i) const {
    static const char* const k[] = {"idle-live", "granular-max", "quanta-max", "freeze-max", "fx-feedback", "gesture-all"};
    return k[i < 0 ? 0 : (i > 5 ? 5 : i)];
}

// Bench: every cloud busy, the grain budget full, all effects on.
void Bulut::benchSetup(int i) {
    lab::dsp::Rng r(77u + uint32_t(i));
    for (int c = 0; c < kClouds; ++c) {
        defaultCloud(clouds_[c], c);
        sources_[c].type = c == 1 ? kSrcLiveOut : kSrcLiveIn;
        sources_[c].inGain = 1.0f;
        eng_->releaseAll(c);
        clouds_[c].gestOn = 0;
        // fill the buffers so grains read real data
        for (uint32_t k = 0; k < kBufLen; ++k) eng_->rt[c].buf[k] = int16_t(8000.0f * sinf(float(k) * 0.01f * float(c + 1)));
    }
    // the worst case a user can set: both budgets at their maximum
    eng_->grainBudget = kMaxGrains;
    eng_->cloudBudget = kMaxCloudGrains;
    if (i == 0) return;
    for (int c = 0; c < kClouds; ++c) {
        CloudState& s = clouds_[c];
        s.drone = 1;
        s.p[0][P::Dens] = 1.0f;
        s.p[0][P::Size] = 0.9f;
        s.p[0][P::Pitch] = 0.85f;
        s.p[0][P::PRand] = 0.5f;
        s.p[0][P::Reverse] = 0.3f;
        s.p[1][P::QRate] = 1.0f;
        s.p[1][P::QDur] = 0.9f;
        s.p[1][P::QType] = enumValue(c % kQTypes, kQTypes);
        s.p[1][P::QLaw] = enumValue(c % kLaws, kLaws);
        s.p[1][P::QChar] = 0.9f;
        s.set(0, P::Engine, (i == 2 || (i >= 4 && (c & 1))) ? 1.0f : 0.0f);
        for (int p = 0; p < 4; ++p) eng_->padDown(c, p * 13 % 48, 120);
        if (i == 3) {
            spec_.analyse([&](int k) { return float(eng_->rt[c].buf[k]) / 32768.0f; }, frz_[c]);
            spec_.synth(frz_[c], eng_->rt[c].frz, 9u + uint32_t(c));
            eng_->rt[c].frzReady = 1;
            s.freeze = 1;
            s.bed = 1.0f;
        }
        if (i >= 4) {
            s.set(0, P::Delay, 1.0f);
            s.set(0, P::Reverb, 1.0f);
            s.set(0, P::Tone, c & 1 ? 0.1f : 0.9f);
            s.p[0][P::Feedback] = 0.8f;
            fx_.dlyFb = 1.0f;
            fx_.dlyDiv = 0.0f;
            fx_.revSize = 1.0f;
            s.freeze = 1;
            s.bed = 1.0f;
            spec_.analyse([&](int k) { return float(eng_->rt[c].buf[k]) / 32768.0f; }, frz_[c]);
            spec_.synth(frz_[c], eng_->rt[c].frz, 9u + uint32_t(c));
            eng_->rt[c].frzReady = 1;
        }
        if (i == 5) {
            Gesture& g = eng_->gestIdle(c);
            g.len = 22050;
            g.speed = 4;
            g.quant = 0;
            int n = 0;
            for (int k = 0; k < 200 && n < kGestEvents - 2; ++k) {
                GestEvent& e = g.ev[n++];
                e.t = uint32_t(k * 110);
                if (k % 3 == 0) {
                    e.type = kGPadDown;
                    e.a = uint8_t(r.range(48));
                    e.v = 110;
                } else if (k % 3 == 1) {
                    e.type = kGParam;
                    e.a = uint8_t(P::Pos | (s.engine() << 4));
                    e.v = uint16_t(r.next());
                } else {
                    e.type = kGPadUp;
                    e.a = g.ev[n - 3].a;
                    e.v = 0;
                }
            }
            g.count = uint16_t(n);
            eng_->postGesture(c);
            s.gestOn = 1;
        }
    }
    if (i == 5) eng_->startCapture(-1);
}

// ---- audio ----------------------------------------------------------------------
void Bulut::render(const lab::Transport& t, const lab::AudioIn& in, float* l, float* r) {
    eng_->render(t, in, l, r, ctx_);
}

void Bulut::onTransport(bool playing) {
    (void)playing;
    tlPlaying_ = -1;
}

// ---- parameters ---------------------------------------------------------------------
void Bulut::setParam(int c, int slot, float v, bool record) {
    CloudState& s = clouds_[c];
    const int eng = s.engine();
    s.set(eng, slot, v);
    if (!record) return;
    if (rec_->state() == GestureRecorder::kArmed && rec_->cloud() == c) waitGesture(c);
    if (rec_->state() == GestureRecorder::kRecording && rec_->cloud() == c && slot != P::Engine)
        rec_->param(eng, slot, s.p[eng][slot], eng_->sampleClock);
}

void Bulut::nudgeParam(int c, int slot, int delta, bool fine) {
    CloudState& s = clouds_[c];
    const int eng = s.engine();
    const int n = paramSteps(eng, slot);
    float v = s.p[eng][slot];
    if (n > 1) v = enumValue(enumIndex(v, n) + delta, n);
    else v += float(delta) * (fine ? 0.002f : 0.01f);
    setParam(c, slot, clamp01(v), true);
}

int Bulut::stripParam(int idx) const {
    static const int8_t kGran[8] = {P::Pos, P::Size, P::Dens, P::Pitch, P::Spread, P::Texture, P::Feedback, P::Reverb};
    static const int8_t kQua[8] = {P::QType, P::QRate, P::QDur, P::QFreq, P::QBand, P::QLaw, P::QChar, P::Reverb};
    return clouds_[sel_].engine() == kGranular ? kGran[idx & 7] : kQua[idx & 7];
}
int Bulut::pageParam(int idx) const { return idx & 15; }

// ---- pages and buttons ------------------------------------------------------------------
void Bulut::setPage(int p) {
    page_ = p;
    if (p == kPgSource) scanBrowse();
    if (p == kPgFile) scanProjects();
}

void Bulut::selectCloud(int c) {
    if (c < 0) c = kClouds - 1;
    if (c >= kClouds) c = 0;
    if (c == sel_) return;
    // pads held on the old cloud are released (their grains finish)
    for (int i = 0; i < kLiveEmitters; ++i)
        if (eng_->rt[sel_].em[i].pad != 0xFF) eng_->rt[sel_].em[i].on = 0;
    held_ = 0;
    if (rec_->state() != GestureRecorder::kIdle) finishGesture();
    sel_ = c;
}

void Bulut::waitGesture(int c) {
    // first movement after Rec: the recording starts now
    const bool od = rec_->overdub();
    const Gesture& live = eng_->gestLive(c);
    if (!od) {
        Gesture& idle = eng_->gestIdle(c);
        idle.len = 0;
        idle.count = 0;
        idle.speed = live.speed < kSpeeds ? live.speed : 2;
        idle.quant = live.quant;
        eng_->postGesture(c);   // the old loop stops while a new one is recorded
    }
    rec_->start(eng_->sampleClock, od ? &live : nullptr, uint32_t(eng_->rt[c].gphase));
}

void Bulut::gestureRecButton() {
    if (rec_->state() == GestureRecorder::kIdle) {
        const bool od = gestMode_ == 1 && eng_->gestLive(sel_).len > 0;
        rec_->arm(sel_, od);
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
    out.speed = live.speed < kSpeeds ? live.speed : 2;
    out.quant = live.quant;
    if (rec_->finish(eng_->sampleClock, quant, out, &live)) {
        eng_->postGesture(c);
        clouds_[c].gestOn = 1;
        char m[64];
        std::snprintf(m, sizeof m, "Gesture %d: %u moves, %.2f s", c + 1, unsigned(out.count),
                      double(float(out.len) / kSr));
        ctx_->toast(m);
    } else {
        ctx_->toast("Nothing recorded");
    }
}

void Bulut::toggleFreeze() {
    CloudState& s = clouds_[sel_];
    if (s.freeze) {
        s.freeze = 0;
        ctx_->toast("Freeze off");
        return;
    }
    if (!eng_->rt[sel_].frzReady) {
        // nothing frozen yet: take this moment of the cloud's source into a free slot
        int slot = -1;
        for (int i = 0; i < kFreezeSlots && slot < 0; ++i)
            if (!frz_[i].used) slot = i;
        if (slot < 0) slot = frzCur_;
        if (captureFreeze(slot, kFromSource)) recallFreeze(slot);
        return;
    }
    s.freeze = 1;
    ctx_->toast("Freeze on");
}

bool Bulut::captureFreeze(int slot, int from) {
    if (slot < 0 || slot >= kFreezeSlots) return false;
    bool got = false;
    const int c = sel_;
    CloudRt& r = eng_->rt[c];
    if (from == kFromMix) {
        const uint32_t w = eng_->mixW;
        got = spec_.analyse([&](int k) {
            return float(eng_->mixHist[(w - Spectral::kAnaSpan + uint32_t(k)) & (kMixHist - 1)]) / 32768.0f;
        }, frz_[slot]);
    } else if (sources_[c].type == kSrcSample && r.sampleLen > 0) {
        const uint32_t len = r.sampleLen;
        const float pos = clouds_[c].get(clouds_[c].engine() == kGranular ? int(P::Pos) : int(P::QPos));
        uint32_t s0 = uint32_t(pos * float(len));
        s0 = s0 > uint32_t(Spectral::kAnaSpan / 2) ? s0 - Spectral::kAnaSpan / 2 : 0;
        got = spec_.analyse([&](int k) {
            const uint32_t i = s0 + uint32_t(k);
            return i < len ? float(r.buf[i]) / 32768.0f : 0.0f;
        }, frz_[slot]);
    } else {
        const uint32_t w = r.w;
        got = spec_.analyse([&](int k) {
            return float(r.buf[(w - Spectral::kAnaSpan + uint32_t(k)) & kBufMask]) / 32768.0f;
        }, frz_[slot]);
    }
    if (!got) {
        ctx_->toast("Nothing to freeze: the source is silent");
        return false;
    }
    frz_[slot].cloud = uint8_t(c);
    frz_[slot].from = uint8_t(from);
    frzCur_ = slot;
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
    CloudRt& r = eng_->rt[sel_];
    r.frzReady = 0;
    spec_.synth(frz_[slot], r.frz, 0x5EEDu + uint32_t(slot) * 977u + uint32_t(sel_));
    r.frzReady = 1;
    clouds_[sel_].freeze = 1;
    clouds_[sel_].freezeSlot = uint8_t(slot);
    char m[48];
    std::snprintf(m, sizeof m, "Cloud %d frozen on slot %d", sel_ + 1, slot + 1);
    ctx_->toast(m);
}

// ---- scenes ---------------------------------------------------------------------------
void Bulut::storeScene(int s) {
    if (s < 0 || s >= kScenes) return;
    Scene& sc = scenes_[s];
    std::memcpy(sc.cloud, clouds_, sizeof clouds_);
    for (int c = 0; c < kClouds; ++c) {
        const Gesture& g = eng_->gestLive(c);
        sc.gest[c].len = g.len;
        sc.gest[c].count = g.count;
        sc.gest[c].quant = g.quant;
        sc.gest[c].speed = g.speed;
        std::memcpy(sc.gest[c].ev, g.ev, sizeof(GestEvent) * g.count);
    }
    sc.used = 1;
    curScene_ = s;
    char m[32];
    std::snprintf(m, sizeof m, "Scene %d stored", s + 1);
    ctx_->toast(m);
}

void Bulut::recallScene(int s, bool morph) {
    if (s < 0 || s >= kScenes || !scenes_[s].used) return;
    const Scene& sc = scenes_[s];
    if (rec_->state() != GestureRecorder::kIdle) rec_->cancel();
    for (int c = 0; c < kClouds; ++c) {
        Gesture& g = eng_->gestIdle(c);
        g.len = sc.gest[c].len;
        g.count = sc.gest[c].count;
        g.quant = sc.gest[c].quant;
        g.speed = sc.gest[c].speed;
        std::memcpy(g.ev, sc.gest[c].ev, sizeof(GestEvent) * sc.gest[c].count);
        eng_->postGesture(c);
    }
    const float dur = kMorphSec[setMorph_ < 0 ? 0 : (setMorph_ > 5 ? 5 : setMorph_)];
    if (!morph || dur <= 0.0f) {
        std::memcpy(clouds_, sc.cloud, sizeof clouds_);
        morphOn_ = false;
    } else {
        std::memcpy(morphFrom_, clouds_, sizeof clouds_);
        std::memcpy(morphTo_, sc.cloud, sizeof clouds_);
        // switches change at once, continuous values glide
        for (int c = 0; c < kClouds; ++c) {
            CloudState& d = clouds_[c];
            const CloudState& t = sc.cloud[c];
            d.p[0][P::Engine] = d.p[1][P::Engine] = t.p[0][P::Engine];
            d.drone = t.drone;
            d.mute = t.mute;
            d.stem = t.stem;
            d.gestOn = t.gestOn;
            d.freeze = t.freeze;
            d.freezeSlot = t.freezeSlot;
            morphFrom_[c].p[0][P::Engine] = morphFrom_[c].p[1][P::Engine] = t.p[0][P::Engine];
        }
        morphT_ = 0.0f;
        morphDur_ = dur;
        morphOn_ = true;
    }
    // frozen clouds get their library moment back
    for (int c = 0; c < kClouds; ++c) {
        const uint8_t fs = sc.cloud[c].freezeSlot;
        if (sc.cloud[c].freeze && fs < kFreezeSlots && frz_[fs].used) {
            CloudRt& r = eng_->rt[c];
            r.frzReady = 0;
            spec_.synth(frz_[fs], r.frz, 0x5EEDu + uint32_t(fs) * 977u + uint32_t(c));
            r.frzReady = 1;
        }
    }
    curScene_ = s;
}

void Bulut::pumpMorph(uint32_t ms) {
    (void)ms;
    if (!morphOn_) return;
    const float k = morphT_ >= morphDur_ ? 1.0f : morphT_ / morphDur_;
    const float e = k * k * (3.0f - 2.0f * k);   // smoothstep
    for (int c = 0; c < kClouds; ++c) {
        CloudState& d = clouds_[c];
        const CloudState& a = morphFrom_[c];
        const CloudState& b = morphTo_[c];
        for (int en = 0; en < 2; ++en)
            for (int i = 0; i < kParams; ++i) {
                if (i == P::Engine) continue;
                const int steps = paramSteps(en, i);
                d.p[en][i] = steps > 1 ? (e < 0.5f ? a.p[en][i] : b.p[en][i]) : a.p[en][i] + (b.p[en][i] - a.p[en][i]) * e;
            }
        d.pan = a.pan + (b.pan - a.pan) * e;
        d.bed = a.bed + (b.bed - a.bed) * e;
    }
    if (k >= 1.0f) morphOn_ = false;
}

void Bulut::pumpTimeline() {
    if (!songMode_ || tlLen_ <= 0 || !ctx_->playing()) {
        if (!ctx_->playing()) tlPlaying_ = -1;
        return;
    }
    const double b = eng_->beat;
    double start = 0.0;
    int idx = -1;
    double total = 0.0;
    for (int i = 0; i < tlLen_; ++i) total += timeline_[i].bars * 4.0;
    if (total <= 0.0) return;
    double pos = b - total * double(int64_t(b / total));   // the song loops
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
    if (ctx_->fs) {
        char p[96];
        int n = 1;
        for (; n < 10000; ++n) {
            std::snprintf(p, sizeof p, "%s/Resample/RS-%04d.wav", ctx_->home, n);
            if (!ctx_->fs->exists(p)) break;
        }
        std::snprintf(rsPath_, sizeof rsPath_, "%s/Resample/RS-%04d.wav", ctx_->home, n);
        if (!rsWriter_.open(*ctx_->fs, rsPath_, 1)) rsPath_[0] = 0;
    }
    eng_->startCapture(rsSrc_);
    rsState_ = 1;
    ctx_->toast(rsSrc_ < 0 ? "Resampling the mix..." : "Resampling the cloud...");
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
                char m[64];
                std::snprintf(m, sizeof m, "%s -> cloud %d", rsPath_ + std::strlen(ctx_->home) + 10, t + 1);
                ctx_->toast(m);
                return;
            }
        }
        // no card: hand the capture straight to the target cloud
        CloudRt& r = eng_->rt[t];
        r.busy = 1;
        std::memcpy(r.buf, eng_->capBuf, len * 2);
        std::memset(r.buf + len, 0, (kBufLen - len) * sizeof(int16_t));
        r.sampleLen = len;
        sources_[t].type = kSrcSample;
        sources_[t].path[0] = 0;
        r.busy = 0;
        ctx_->toast("Resample -> cloud (not saved: no card)");
    }
}

// ---- samples ---------------------------------------------------------------------------
bool Bulut::loadSample(int c, const char* path) {
    if (!ctx_->fs || c < 0 || c >= kClouds) return false;
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
    CloudRt& r = eng_->rt[c];
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
    sources_[c].type = kSrcSample;
    lab::copyStr(sources_[c].path, sizeof sources_[c].path, path);
    r.busy = 0;
    return true;
}

void Bulut::reloadSources() {
    for (int c = 0; c < kClouds; ++c) {
        CloudRt& r = eng_->rt[c];
        if (sources_[c].type == kSrcSample) {
            if (!sources_[c].path[0] || !loadSample(c, sources_[c].path)) {
                r.sampleLen = 0;
                if (sources_[c].path[0]) {
                    char m[64];
                    std::snprintf(m, sizeof m, "Cloud %d: sample missing", c + 1);
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
        const uint8_t fs = clouds_[c].freezeSlot;
        if (fs < kFreezeSlots && frz_[fs].used) {
            spec_.synth(frz_[fs], r.frz, 0x5EEDu + uint32_t(fs) * 977u + uint32_t(c));
            r.frzReady = 1;
        } else {
            clouds_[c].freeze = 0;
        }
    }
}

void Bulut::scanBrowse() {
    browseN_ = 0;
    if (!ctx_ || !ctx_->fs) return;
    struct Ctx {
        Bulut* b;
    } cx{this};
    if (browseDir_[0]) {
        BrowseEntry& up = browse_[browseN_++];
        lab::copyStr(up.name, sizeof up.name, "..");
        up.dir = 1;
    }
    ctx_->fs->listDir(browseDir_[0] ? browseDir_ : "/",
                      [](const ot::IStorage::DirEntry& e, void* p) {
                          Bulut* b = static_cast<Ctx*>(p)->b;
                          if (b->browseN_ >= kBrowseMax) return false;
                          if (e.name[0] == '.') return true;
                          if (!e.isDir && !lab::endsWithCi(e.name, ".wav")) return true;
                          if (std::strlen(e.name) >= sizeof b->browse_[0].name) return true;
                          BrowseEntry& be = b->browse_[b->browseN_++];
                          lab::copyStr(be.name, sizeof be.name, e.name);
                          be.dir = e.isDir ? 1 : 0;
                          return true;
                      },
                      &cx);
    // folders first, then files, by name
    for (int i = 1; i < browseN_; ++i) {
        BrowseEntry x = browse_[i];
        int j = i - 1;
        auto before = [](const BrowseEntry& a, const BrowseEntry& b) {
            if (a.name[0] == '.' && a.name[1] == '.') return true;
            if (b.name[0] == '.' && b.name[1] == '.') return false;
            if (a.dir != b.dir) return a.dir > b.dir;
            return std::strcmp(a.name, b.name) < 0;
        };
        while (j >= 0 && before(x, browse_[j])) {
            browse_[j + 1] = browse_[j];
            --j;
        }
        browse_[j + 1] = x;
    }
    if (browseCur_ >= browseN_) browseCur_ = browseN_ ? browseN_ - 1 : 0;
}

void Bulut::browseEnter() {
    if (browseCur_ < 0 || browseCur_ >= browseN_) return;
    const BrowseEntry& e = browse_[browseCur_];
    if (e.dir) {
        if (e.name[0] == '.') {
            char* s = std::strrchr(browseDir_, '/');
            if (s) *s = 0;
            else browseDir_[0] = 0;
        } else {
            char nd[96];
            if (browseDir_[0]) std::snprintf(nd, sizeof nd, "%s/%s", browseDir_, e.name);
            else lab::copyStr(nd, sizeof nd, e.name);
            lab::copyStr(browseDir_, sizeof browseDir_, nd);
        }
        browseCur_ = 0;
        browseTop_ = 0;
        scanBrowse();
        return;
    }
    char p[160];
    if (browseDir_[0]) std::snprintf(p, sizeof p, "%s/%s", browseDir_, e.name);
    else lab::copyStr(p, sizeof p, e.name);
    char m[64];
    if (std::strlen(p) < sizeof sources_[0].path && loadSample(sel_, p)) {
        std::snprintf(m, sizeof m, "%.36s -> cloud %d", e.name, sel_ + 1);
    } else {
        std::snprintf(m, sizeof m, "Cannot load %.40s", e.name);
    }
    ctx_->toast(m);
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
    ProjectRefs r;
    r.clouds = clouds_;
    r.sources = sources_;
    for (int c = 0; c < kClouds; ++c) r.gestures[c] = &eng_->gestLive(c);
    r.fx = &fx_;
    r.scenes = scenes_;
    r.timeline = timeline_;
    r.timelineLen = &tlLen_;
    r.freezes = frz_;
    bpmSave_ = ctx_->bpm();
    r.bpm = &bpmSave_;
    r.songMode = &songMode_;
    const ProjResult res = saveProject(ctx_->fs, path, r);
    return res == ProjResult::kOk;
}

bool Bulut::loadProjectFrom(const char* path) {
    if (rec_->state() != GestureRecorder::kIdle) rec_->cancel();
    if (rsState_) {
        eng_->stopCapture();
        rsState_ = 0;
        if (rsPath_[0]) rsWriter_.close();
    }
    for (int c = 0; c < kClouds; ++c) eng_->releaseAll(c);
    ProjectRefs r;
    r.clouds = clouds_;
    r.sources = sources_;
    for (int c = 0; c < kClouds; ++c) r.gestures[c] = &eng_->gestIdle(c);
    r.fx = &fx_;
    r.scenes = scenes_;
    r.timeline = timeline_;
    r.timelineLen = &tlLen_;
    r.freezes = frz_;
    r.bpm = &bpmSave_;
    r.songMode = &songMode_;
    const ProjResult res = loadProject(ctx_->fs, path, r);
    if (res != ProjResult::kOk) {
        char m[64];
        std::snprintf(m, sizeof m, "Load failed: %s", projResultText(res));
        ctx_->toast(m);
        // nothing was changed unless the file was cut short (kPartial: keep what is whole)
        if (res != ProjResult::kPartial) return false;
    }
    for (int c = 0; c < kClouds; ++c) {
        Gesture& g = eng_->gestFilling(c);
        if (g.speed >= kSpeeds) g.speed = 2;
        eng_->postGesture(c);
    }
    morphOn_ = false;
    curScene_ = -1;
    ctx_->setBpm(bpmSave_);
    reloadSources();
    return res == ProjResult::kOk;
}

void Bulut::newProject() {
    for (int c = 0; c < kClouds; ++c) {
        eng_->releaseAll(c);
        defaultCloud(clouds_[c], c);
        std::memset(&sources_[c], 0, sizeof sources_[c]);
        sources_[c].inGain = 1.0f;
        Gesture& g = eng_->gestIdle(c);
        g.len = 0;
        g.count = 0;
        g.speed = 2;
        g.quant = 0;
        eng_->postGesture(c);
    }
    for (int s = 0; s < kScenes; ++s) scenes_[s].used = 0;
    for (int f = 0; f < kFreezeSlots; ++f) frz_[f].used = 0;
    tlLen_ = 0;
    songMode_ = 0;
    defaultFx(fx_);
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
    s.midiCloud = setMidiCloud_;
    s.morph = setMorph_;
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
    add("BULUT grains per cloud", &setCloudGrains_, 2, kMaxCloudGrains, nullptr, " grains");
    add("BULUT pad velocity", &setVelocity_, 0, 1, kOffOn, "");
    add("BULUT MIDI notes play", &setMidiCloud_, 0, 6, kMidiCloud, "");
    add("BULUT scene morph", &setMorph_, 0, 5, kMorph, "");
    add("BULUT pad LEDs", &setLeds_, 0, 2, kLedLevels, "");
    add("BULUT restore last session", &setAutosave_, 0, 1, kOffOn, "");
    return n;
}

// ---- MIDI ------------------------------------------------------------------------------------
void Bulut::onMidi(const lab::MidiMsg& m) {
    const int c = setMidiCloud_ > 0 ? setMidiCloud_ - 1 : sel_;
    const uint8_t t = m.type();
    if (t == 0x90 && m.d2) {
        eng_->noteOn(c, m.d1, setVelocity_ ? m.d2 : 100);
    } else if (t == 0x80 || (t == 0x90 && m.d2 == 0)) {
        eng_->noteOff(c, m.d1);
    } else if (t == 0xB0) {
        const float v = float(m.d2) / 127.0f;
        if (m.d1 == 1) setParam(c, clouds_[c].engine() == kGranular ? int(P::Pos) : int(P::QPos), v, true);
        else if (m.d1 >= 20 && m.d1 < 36) setParam(c, m.d1 - 20, v, true);
        else if (m.d1 == 123) eng_->releaseAll(c);
    }
}

// ---- events ------------------------------------------------------------------------------------
void Bulut::shiftPad(int pad) {
    const int row = pad / 12, col = pad % 12;
    if (row == 0 && col < kClouds) selectCloud(col);
    else if (row == 1 && col < kScenes) {
        if (scenes_[col].used) recallScene(col, true);
        else storeScene(col);
    } else if (row == 2) {
        if (frz_[col].used) recallFreeze(col);
        else captureFreeze(col, frzFrom_);
    } else if (row == 3 && col < kClouds) {
        clouds_[col].mute ^= 1;
    } else if (row == 0 && col == 11) {
        toggleFreeze();
    }
}

void Bulut::onEvent(const Event& e) {
    if (e.type == Event::kPress && e.id == btn::Shift) shift_ = true;
    if (e.type == Event::kRelease && e.id == btn::Shift) shift_ = false;
    if (e.type == Event::kPadDown) {
        if (e.shift || shift_) {
            shiftPad(e.id);
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
        if (e.shift) action(id);
        else setPage(id);
        return;
    }
    switch (id) {
        case btn::Up:
        case btn::Down:
        case btn::Left:
        case btn::Right: arrows(id); return;
        case btn::EncPush: push(); return;
        case btn::Rec: gestureRecButton(); return;
        case btn::Fx1: toggleFreeze(); return;
        case btn::Fx2:
            clouds_[sel_].drone ^= 1;
            ctx_->toast(clouds_[sel_].drone ? "Drone on: the cloud breathes" : "Drone off");
            return;
        case btn::Instr: {
            CloudState& s = clouds_[sel_];
            s.set(0, P::Engine, s.engine() == kGranular ? 1.0f : 0.0f);
            ctx_->toast(s.engine() == kGranular ? "Engine: GRANULAR" : "Engine: QUANTA");
            return;
        }
        case btn::Note: selectCloud(sel_ + (e.shift ? -1 : 1)); return;
        case btn::SampleRecorder:
            if (rsState_ == 1) stopResample();
            else if (!rsState_) startResample();
            return;
        case btn::Performance: setPage(kPgCloud); return;
        case btn::InstrParams:
        case btn::SamplePlayback: setPage(kPgGrain); return;
        case btn::SampleLoader: setPage(kPgSource); return;
        case btn::Pattern: setPage(kPgGesture); return;
        case btn::SampleEditor: setPage(kPgFreeze); return;
        case btn::Master: setPage(kPgMix); return;
        case btn::Song:
            if (e.shift) {
                songMode_ ^= 1;
                ctx_->toast(songMode_ ? "Song mode on: Play runs the timeline" : "Song mode off");
            } else {
                setPage(kPgScene);
            }
            return;
        case btn::File: setPage(kPgFile); return;
        case btn::Insert:
            if (page_ == kPgScene && scnFocus_ == 1) {
                if (tlLen_ < kTimeline) {
                    for (int i = tlLen_; i > tlCur_ + 1 && i > 0; --i) timeline_[i] = timeline_[i - 1];
                    const int at = tlLen_ ? tlCur_ + 1 : 0;
                    timeline_[at].scene = uint8_t(curScene_ >= 0 ? curScene_ : scnCur_);
                    timeline_[at].bars = 4;
                    ++tlLen_;
                    tlCur_ = at;
                }
            } else {
                storeScene(page_ == kPgScene ? scnCur_ : (curScene_ >= 0 ? curScene_ : 0));
            }
            return;
        case btn::Delete:
            if (page_ == kPgGesture || page_ == kPgCloud) {
                Gesture& g = eng_->gestIdle(sel_);
                g.len = 0;
                g.count = 0;
                eng_->postGesture(sel_);
                ctx_->toast("Gesture cleared");
            } else if (page_ == kPgFreeze) {
                frz_[frzCur_].used = 0;
            } else if (page_ == kPgScene) {
                if (scnFocus_ == 1 && tlLen_ > 0) {
                    for (int i = tlCur_; i + 1 < tlLen_; ++i) timeline_[i] = timeline_[i + 1];
                    --tlLen_;
                    if (tlCur_ >= tlLen_ && tlCur_ > 0) --tlCur_;
                } else {
                    scenes_[scnCur_].used = 0;
                }
            }
            return;
        default: return;
    }
}

// Shift + F1..F8: the actions of the current page (shown in the bar while Shift is held).
void Bulut::action(int k) {
    CloudState& s = clouds_[sel_];
    switch (page_) {
        case kPgCloud:
        case kPgGrain:
            if (k == 0) s.set(0, P::Engine, s.engine() == kGranular ? 1.0f : 0.0f);
            if (k == 1) s.drone ^= 1;
            if (k == 2) toggleFreeze();
            if (k == 3) gestureRecButton();
            if (k == 4) s.mute ^= 1;
            if (k == 5) eng_->soloMask = eng_->soloMask == (1u << sel_) ? 0 : uint8_t(1u << sel_);
            if (k == 6) {
                const int en = s.engine();
                CloudState d;
                defaultCloud(d, sel_);
                for (int i = 0; i < kParams; ++i)
                    if (i != P::Engine) s.set(en, i, d.p[en][i]);
                ctx_->toast("Parameters reset");
            }
            if (k == 7) {
                lab::dsp::Rng r(eng_->sampleClock * 2654435761u + 1u);
                const int en = s.engine();
                for (int i = 0; i < kParams; ++i)
                    if (i != P::Engine && i != P::Level && i != P::Delay && i != P::Reverb) s.set(en, i, r.uni());
                ctx_->toast("Parameters randomised");
            }
            break;
        case kPgSource:
            if (k <= 2) {
                if (k == 2) {
                    if (eng_->rt[sel_].sampleLen == 0 && sources_[sel_].path[0]) loadSample(sel_, sources_[sel_].path);
                    sources_[sel_].type = kSrcSample;
                } else {
                    sources_[sel_].type = uint8_t(k);
                }
            }
            if (k == 3) sources_[sel_].hold ^= 1;
            if ((k == 4 || k == 5) && ctx_->radio) {
                ctx_->radio->seek(k == 5);
                radioMHz_ = ctx_->radio->frequency();
            }
            if (k == 6) {
                eng_->rt[sel_].busy = 1;
                std::memset(eng_->rt[sel_].buf, 0, kBufLen * 2);
                eng_->rt[sel_].busy = 0;
                ctx_->toast("Buffer cleared");
            }
            if (k == 7) browseEnter();
            break;
        case kPgGesture: {
            Gesture& g = eng_->gestLive(sel_);
            if (k == 0) gestureRecButton();
            if (k == 1) {
                gestMode_ ^= 1;
                ctx_->toast(gestMode_ ? "Rec mode: overdub" : "Rec mode: new");
            }
            if (k == 2) {
                Gesture& id = eng_->gestIdle(sel_);
                id.len = 0;
                id.count = 0;
                eng_->postGesture(sel_);
            }
            if (k == 3) s.gestOn ^= 1;
            if (k == 4 && g.speed > 0) --g.speed;
            if (k == 5 && g.speed + 1 < kSpeeds) ++g.speed;
            if (k == 6)
                for (int c = 0; c < kClouds; ++c) clouds_[c].gestOn = 1;
            if (k == 7)
                for (int c = 0; c < kClouds; ++c) clouds_[c].gestOn = 0;
            break;
        }
        case kPgFreeze:
            if (k == 0) captureFreeze(frzCur_, frzFrom_);
            if (k == 1) recallFreeze(frzCur_);
            if (k == 2) toggleFreeze();
            if (k == 3) frz_[frzCur_].used = 0;
            if (k == 4) frzFrom_ = frzFrom_ == kFromSource ? kFromMix : kFromSource;
            if (k == 5) s.freeze = 0;
            break;
        case kPgMix:
            if (k == 0 && mixCol_ < kClouds) clouds_[mixCol_].mute ^= 1;
            if (k == 1 && mixCol_ < kClouds)
                eng_->soloMask = uint8_t(eng_->soloMask ^ (1u << mixCol_));
            if (k == 2) eng_->soloMask = 0;
            break;
        case kPgScene:
            if (k == 0) storeScene(scnCur_);
            if (k == 1) recallScene(scnCur_, true);
            if (k == 2) scenes_[scnCur_].used = 0;
            if (k == 3 && tlLen_ < kTimeline) {
                timeline_[tlLen_].scene = uint8_t(scnCur_);
                timeline_[tlLen_].bars = 4;
                tlCur_ = tlLen_++;
            }
            if (k == 4 && tlLen_ > 0) {
                for (int i = tlCur_; i + 1 < tlLen_; ++i) timeline_[i] = timeline_[i + 1];
                --tlLen_;
                if (tlCur_ >= tlLen_ && tlCur_ > 0) --tlCur_;
            }
            if (k == 5) {
                songMode_ ^= 1;
                tlPlaying_ = -1;
            }
            if (k == 6) recallScene(scnCur_, false);
            break;
        case kPgFile: {
            char p[96];
            projectPath(fileCur_, p, sizeof p);
            if (k == 0) {
                ctx_->toast(saveProjectTo(p) ? "Project saved" : "Save failed (card?)");
                scanProjects();
            }
            if (k == 1) {
                if (projUsed_[fileCur_]) {
                    if (loadProjectFrom(p)) ctx_->toast("Project loaded");
                } else {
                    ctx_->toast("Empty slot");
                }
            }
            if (k == 2) newProject();
            if (k == 3) {
                if (rsState_ == 1) stopResample();
                else if (!rsState_) startResample();
            }
            break;
        }
        default: break;
    }
}

void Bulut::arrows(int id) {
    const int dx = id == btn::Left ? -1 : (id == btn::Right ? 1 : 0);
    const int dy = id == btn::Up ? -1 : (id == btn::Down ? 1 : 0);
    switch (page_) {
        case kPgCloud:
            if (dy) selectCloud(sel_ + dy);
            stripCur_ = (stripCur_ + dx + 8) % 8;
            break;
        case kPgGrain:
            if (dx) grainCur_ = (grainCur_ & 12) | (((grainCur_ & 3) + dx + 4) & 3);
            if (dy) grainCur_ = (grainCur_ + dy * 4 + 16) & 15;
            break;
        case kPgSource:
            if (srcCur_ == 4 && browseN_ > 0) {
                if (dy < 0 && browseCur_ == 0) srcCur_ = 3;
                else if (dy) browseCur_ = browseCur_ + dy < 0 ? 0 : (browseCur_ + dy >= browseN_ ? browseN_ - 1 : browseCur_ + dy);
                if (dx < 0 && browseDir_[0]) {
                    browseCur_ = 0;
                    browseEnter();
                }
                if (dx > 0) browseEnter();
            } else {
                if (dy) srcCur_ = srcCur_ + dy < 0 ? 0 : (srcCur_ + dy > 4 ? 4 : srcCur_ + dy);
                if (dx) encoder(dx, false);
            }
            break;
        case kPgGesture:
            if (dy) selectCloud(sel_ + dy);
            if (dx) gestCur_ = (gestCur_ + dx + 4) % 4;
            break;
        case kPgFreeze:
            if (dx) frzCur_ = (frzCur_ + dx + kFreezeSlots) % kFreezeSlots;
            if (dy) frzCur_ = (frzCur_ + dy * 8 + kFreezeSlots) % kFreezeSlots;
            break;
        case kPgMix:
            if (dx) mixCol_ = (mixCol_ + dx + kClouds + 1) % (kClouds + 1);
            if (dy) mixRow_ = (mixRow_ + dy + 7) % 7;
            if (mixCol_ < kClouds && dx) sel_ = mixCol_;
            break;
        case kPgScene:
            if (dy) scnFocus_ = dy > 0 ? 1 : 0;
            if (dx && scnFocus_ == 0) scnCur_ = (scnCur_ + dx + kScenes) % kScenes;
            if (dx && scnFocus_ == 1 && tlLen_ > 0) tlCur_ = (tlCur_ + dx + tlLen_) % tlLen_;
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
    CloudState& s = clouds_[sel_];
    switch (page_) {
        case kPgCloud: nudgeParam(sel_, stripParam(stripCur_), d, fine); break;
        case kPgGrain: nudgeParam(sel_, pageParam(grainCur_), d, fine); break;
        case kPgSource: {
            CloudSource& so = sources_[sel_];
            if (srcCur_ == 0) {
                const int t = (int(so.type) + d + kSrcCount * 4) % kSrcCount;
                if (t == kSrcSample && eng_->rt[sel_].sampleLen == 0 && so.path[0]) loadSample(sel_, so.path);
                so.type = uint8_t(t);
            }
            if (srcCur_ == 1) so.inGain = lab::dsp::clamp(so.inGain + float(d) * (fine ? 0.005f : 0.02f), 0.0f, 2.0f);
            if (srcCur_ == 2) so.hold = so.hold ? 0 : 1;
            if (srcCur_ == 3) {
                radioMHz_ = lab::dsp::clamp(radioMHz_ + float(d) * (fine ? 0.1f : 0.1f), 87.5f, 108.0f);
                radioMHz_ = floorf(radioMHz_ * 10.0f + 0.5f) * 0.1f;
                if (ctx_->radio) ctx_->radio->tune(radioMHz_);
            }
            if (srcCur_ == 4 && browseN_) {
                browseCur_ += d;
                if (browseCur_ < 0) browseCur_ = 0;
                if (browseCur_ >= browseN_) browseCur_ = browseN_ - 1;
            }
            break;
        }
        case kPgGesture: {
            Gesture& g = eng_->gestLive(sel_);
            if (gestCur_ == 0) s.gestOn = d > 0 ? 1 : 0;
            if (gestCur_ == 1) {
                const int v = int(g.speed) + d;
                g.speed = uint8_t(v < 0 ? 0 : (v >= kSpeeds ? kSpeeds - 1 : v));
            }
            if (gestCur_ == 2) {
                const int v = int(g.quant) + d;
                g.quant = uint8_t(v < 0 ? 0 : (v >= kQuants ? kQuants - 1 : v));
            }
            if (gestCur_ == 3) gestMode_ = d > 0 ? 1 : 0;
            break;
        }
        case kPgFreeze: s.bed = clamp01(s.bed + float(d) * (fine ? 0.002f : 0.01f)); break;
        case kPgMix: {
            const float k = float(d) * (fine ? 0.002f : 0.01f);
            if (mixCol_ < kClouds) {
                CloudState& m = clouds_[mixCol_];
                switch (mixRow_) {
                    case 0: m.set(0, P::Level, m.p[0][P::Level] + k); break;
                    case 1: m.pan = clamp01(m.pan + k); break;
                    case 2: m.set(0, P::Delay, m.p[0][P::Delay] + k); break;
                    case 3: m.set(0, P::Reverb, m.p[0][P::Reverb] + k); break;
                    case 4: m.set(0, P::Tone, m.p[0][P::Tone] + k); break;
                    case 5: m.stem = uint8_t((m.stem + d + 16) & 3); break;
                    default: m.mute = d > 0 ? 1 : 0; break;
                }
            } else {
                switch (mixRow_) {
                    case 0: fx_.dlyDiv = enumValue(enumIndex(fx_.dlyDiv, kDlyDivs) + d, kDlyDivs); break;
                    case 1: fx_.dlyFb = clamp01(fx_.dlyFb + k); break;
                    case 2: fx_.dlyTone = clamp01(fx_.dlyTone + k); break;
                    case 3: fx_.dlyRet = clamp01(fx_.dlyRet + k); break;
                    case 4: fx_.revSize = clamp01(fx_.revSize + k); break;
                    case 5: fx_.revDamp = clamp01(fx_.revDamp + k); break;
                    default: fx_.revRet = clamp01(fx_.revRet + k); break;
                }
            }
            break;
        }
        case kPgScene:
            if (scnFocus_ == 0) scnCur_ = (scnCur_ + d + kScenes * 8) % kScenes;
            else if (tlLen_ > 0) {
                TimeEntry& te = timeline_[tlCur_];
                if (fine) te.scene = uint8_t((te.scene + d + kScenes * 8) % kScenes);
                else {
                    const int b = te.bars + d;
                    te.bars = uint8_t(b < 1 ? 1 : (b > 64 ? 64 : b));
                }
            }
            break;
        case kPgFile:
            if (fileFocus_ == 0) {
                fileCur_ = (fileCur_ + d + kProjects * 8) % kProjects;
            } else {
                // resample row: encoder picks source, Shift + encoder the target cloud
                if (fine) rsDst_ = (rsDst_ + d + kClouds * 8) % kClouds;
                else {
                    rsSrc_ += d;
                    if (rsSrc_ < -1) rsSrc_ = kClouds - 1;
                    if (rsSrc_ >= kClouds) rsSrc_ = -1;
                }
            }
            break;
        default: break;
    }
}

void Bulut::push() {
    switch (page_) {
        case kPgCloud:
        case kPgGrain: {
            const int slot = page_ == kPgCloud ? stripParam(stripCur_) : pageParam(grainCur_);
            CloudState d;
            defaultCloud(d, sel_);
            const int en = clouds_[sel_].engine();
            if (slot != P::Engine) setParam(sel_, slot, d.p[en][slot], true);
            else clouds_[sel_].set(0, P::Engine, clouds_[sel_].engine() == kGranular ? 1.0f : 0.0f);
            break;
        }
        case kPgSource:
            if (srcCur_ == 4) browseEnter();
            else if (srcCur_ == 2) sources_[sel_].hold ^= 1;
            break;
        case kPgGesture: gestureRecButton(); break;
        case kPgFreeze:
            if (frz_[frzCur_].used) recallFreeze(frzCur_);
            else captureFreeze(frzCur_, frzFrom_);
            break;
        case kPgMix:
            if (mixCol_ < kClouds) clouds_[mixCol_].mute ^= 1;
            break;
        case kPgScene:
            if (scnFocus_ == 0) {
                if (scenes_[scnCur_].used) recallScene(scnCur_, true);
                else storeScene(scnCur_);
            } else if (tlLen_ > 0) {
                recallScene(timeline_[tlCur_].scene, true);
            }
            break;
        case kPgFile:
            if (fileFocus_ == 1) {
                if (rsState_ == 1) stopResample();
                else if (!rsState_) startResample();
            } else {
                action(projUsed_[fileCur_] ? 1 : 0);
            }
            break;
        default: break;
    }
}

// ---- main loop -------------------------------------------------------------------------------
void Bulut::update(uint32_t ms) {
    // Config > Radio is the saved station: follow it when it changes there
    if (ctx_->shell) {
        const float sh = ctx_->shell->radioMHz();
        if (sh != shellRadio_) {
            shellRadio_ = sh;
            radioMHz_ = sh;
        }
    }
    const float dt = lastMs_ ? float(ms - lastMs_) * 0.001f : 0.0f;
    lastMs_ = ms;
    if (morphOn_) morphT_ += dt > 0.1f ? 0.1f : dt;
    pumpMorph(ms);
    pumpTimeline();
    pumpResample();
    pumpParticles(dt > 0.1f ? 0.1f : dt);
    if (ms - lastPeakMs_ >= 100 || ms < lastPeakMs_) {
        lastPeakMs_ = ms;
        updatePeaks();
    }
    for (int c = 0; c < kClouds; ++c) {
        if (eng_->rt[c].gWrapped) {
            eng_->rt[c].gWrapped = 0;
            gestFlash_[c] = 1.0f;
        }
        gestFlash_[c] *= 0.9f;
    }
}

void Bulut::updatePeaks() {
    for (int c = 0; c < kClouds; ++c) {
        const CloudRt& r = eng_->rt[c];
        const bool sample = sources_[c].type == kSrcSample;
        const uint32_t len = sample ? r.sampleLen : kLiveSpan;
        const uint32_t base = sample ? 0 : (r.w - kLiveSpan) & kBufMask;
        float colMax[12] = {};
        for (int s = 0; s < 64; ++s) {
            int m = 0;
            if (len) {
                const uint32_t a = uint32_t(uint64_t(len) * uint32_t(s) / 64u);
                const uint32_t step = len / 64u / 24u + 1u;
                for (uint32_t k = 0; k < 24; ++k) {
                    const int v = r.buf[(base + a + k * step) & kBufMask];
                    const int av = v < 0 ? -v : v;
                    m = av > m ? av : m;
                }
            }
            const int h = m >> 7;
            wave_[c][s] = uint8_t(h > 255 ? 255 : h);
            const int col = s * 12 / 64;
            const float f = float(m) / 32768.0f;
            colMax[col] = f > colMax[col] ? f : colMax[col];
        }
        for (int k = 0; k < 12; ++k) colPeak_[c][k] = uint8_t(sqrtf(colMax[k]) * 255.0f);
    }
}

void Bulut::pumpParticles(float dt) {
    Spark s;
    static lab::dsp::Rng vr(0xC10D);
    int n = 0;
    while (eng_->sparks.pop(s)) {
        if (++n > 120) continue;   // drain everything, draw at most 120 new grains per pass
        if (s.cloud >= kClouds) continue;
        const float a = float(s.amp) / 255.0f;
        const float life = float(s.life) * 0.01f;   // grain length, seconds
        const int row = s.live & 7;
        const float x0 = float(s.pos) / 65535.0f;
        const float y0 = row < 4 ? (float(row) + 0.5f) / 4.0f : 0.5f - float(s.semis) * 0.008f;
        // a grain is a small puff: one bright core and a few drifting motes
        const int motes = s.kind >= 3 ? 1 : (life > 0.2f ? 4 : (life > 0.05f ? 3 : 2));
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
            float l = 0.5f + life * 3.0f + 0.4f * vr.uni();
            p.life = l > 2.8f ? 2.8f : l;
            p.age = 0.0f;
            if (k == 0) p.size = uint8_t(s.kind >= 3 ? (a > 0.3f ? 1 : 0) : (life > 0.15f ? 2 : 1));
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
    for (int c = 0; c < kClouds; ++c)
        for (int col = 0; col < 12; ++col) colGlow_[c][col] *= k;
}

}  // namespace bulut

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<bulut::Bulut>(); }
}  // namespace lab
