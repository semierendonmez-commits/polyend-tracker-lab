// BULUT TAPE FILE, main loop side: choosing a file, converting it, binding it
// to its channel's tape, play / stop / jump / FROM / LOOP, the read-ahead pump
// and the project glue (stream.h has the design; the audio side is
// Tapes::render, called from Engine::render for a TAPE FILE channel).
//
// The tape model is a tape deck: Stop pauses where it is, Play continues
// from there; FROM is where a run starts after the end and where LOOP wraps
// back to; changing FROM while stopped cues the tape to it. A jump moves the
// playhead now (playing) or the cue (stopped). A TAPE FILE channel's tape
// only runs while its source is TAPE FILE.
#pragma GCC optimize("Os")
#include <cstdio>
#include <cstring>

#include "ws/bulut_ws.h"
#include "ws/ui_text.h"

namespace bulut {

namespace kit = lab::kit;

namespace {
bool streamable16(const ot::WavInfo& i) {
    return i.format == ot::wav::kFormatPcm && i.bitsPerSample == 16 && i.sampleRate == 44100 &&
           (i.channels == 1 || i.channels == 2);
}
bool probeWav(ot::IStorage& fs, const char* path, ot::WavInfo& info) {
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return false;
    ot::StorageReader rd(fs, h);
    const bool ok = ot::wavParseHeader(rd, info) && ot::wavIsSupported(info);
    fs.close(h);
    return ok;
}
uint32_t ctxMillis(void* c) { return static_cast<lab::Context*>(c)->millis(); }
const char* baseName(const char* p) {
    const char* b = std::strrchr(p, '/');
    return b ? b + 1 : p;
}
}  // namespace

// ---- init ---------------------------------------------------------------------------
void Bulut::initTapes(lab::Context& c) {
    for (int ch = 0; ch < kCh; ++ch) {
        tape_[ch] = TapeSlot();
        tape_[ch].headFrom = 0xFFFFFFFFu;
        tape_[ch].jumpTo = -1.0f;
        tape_[ch].loopNow = 1;
    }
    lab::Arena& a = *c.arena;
    tapeSave_ = a.make<TapeSave>(kCh);
    ot::audio::IStreamReader* reader = nullptr;
#if defined(LAB_HAS_STREAM_READER)
    if (c.fs) reader = c.streamReader;
#endif
    bool want = reader != nullptr;
#if defined(OT_BENCH_M4) || !defined(__arm__)
    want = want || !c.fs;   // the CPU bench (no card): tape-file4 brings a synthetic reader
#endif
    if (want) {
        tapes_ = a.make<Tapes>();
        if (tapes_ && !tapes_->init(a)) tapes_ = nullptr;
        if (tapes_) tapes_->setReader(reader);
        if (reader) {
            conv_ = a.make<StreamConv>();
            if (conv_ && !conv_->init(a)) conv_ = nullptr;
        }
    }
    eng_->tapes = tapes_;
}

void Bulut::setTapeReader(ot::audio::IStreamReader* r) {
    if (tapes_) tapes_->setReader(r);
}

bool Bulut::tapeAvailable() const { return tapes_ && tapes_->ready() && tapes_->reader(); }

uint32_t Bulut::tapeFromFrame(int c) const {
    const TapeSlot& s = tape_[c];
    if (s.frames < 2u * 44100u) return 0;
    const float f = clamp01(ctx_->params().get(tapePid_[c][kTpFrom]));
    uint32_t v = uint32_t(f * float(s.frames));
    const uint32_t lim = s.frames - 44100u;   // at least a second to play
    return v > lim ? lim : v;
}

// ---- files ---------------------------------------------------------------------------
void Bulut::tapeConvPath(const char* src, uint32_t frames, char* out, int cap) const {
    // <home>/Stream/<file stem>_<FNV-1a of the path and the length>.wav
    uint32_t h = 2166136261u ^ frames;
    for (const char* p = src; *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
    const char* b = baseName(src);
    char stem[25];
    int n = 0;
    for (const char* p = b; *p && *p != '.' && n < 24; ++p) {
        const char c = *p;
        const bool okc = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem[n++] = okc ? c : '_';
    }
    stem[n] = 0;
    std::snprintf(out, size_t(cap), "%s/Stream/%s_%08lX.wav", ctx_->home, stem, static_cast<unsigned long>(h));
}

bool Bulut::loadTape(int c, const char* path) {
    if (c < 0 || c >= kCh || !path || !path[0]) return false;
    if (!tapeAvailable() || !ctx_->fs) {
        ctx_->toast("TAPE FILE: no card / not available here");
        return false;
    }
    ot::WavInfo info;
    if (std::strlen(path) >= sizeof tape_[c].path || !probeWav(*ctx_->fs, path, info)) {
        ctx_->toast("Cannot read that WAV");
        return false;
    }
    const uint32_t frames = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
    if (frames < kTapeMinFrames) {
        ctx_->toast("Under 4 s: load it as a SAMPLE (S.Load)");
        return false;
    }
    // the channel's old tape stops (its buffer holds) and a conversion for it is dropped
    tapes_->unbind(c);
    if (convPend_ & (1u << c)) convPend_ = uint8_t(convPend_ & ~(1u << c));
    if (convCh_ == c && conv_) {
        conv_->cancel();
        convCh_ = -1;
    }
    TapeSlot& s = tape_[c];
    lab::copyStr(s.path, sizeof s.path, path);
    lab::copyStr(s.label, sizeof s.label, baseName(path));
    s.file[0] = 0;
    s.frames = frames;
    s.cue = tapeFromFrame(c);
    s.ended = 0;
    s.jumpTo = -1.0f;
    s.headFrom = 0xFFFFFFFFu;
    s.wantPlay = 1;   // a new tape runs at once, like an input
    s.state = kTapeWant;
    ctx_->params().set(pid_[c][CP::Source], float(kSrcTape), false);
    srcNow_[c] = kSrcTape;
    return tapeOpen(c) || s.state == kTapeConv;
}

// Resolve the file (as it is, its 16-bit copy, or a conversion first), bind
// it, read the head, play if wanted. False: not open (yet).
bool Bulut::tapeOpen(int c) {
    TapeSlot& s = tape_[c];
    if (!tapeAvailable() || !ctx_->fs || !s.path[0]) return false;
    ot::IStorage& fs = *ctx_->fs;
    ot::WavInfo info;
    if (!s.file[0]) {
        if (!probeWav(fs, s.path, info)) {
            s.state = kTapeFail;
            char m[64];
            std::snprintf(m, sizeof m, "CH %d: tape file missing", c + 1);
            ctx_->toast(m);
            return false;
        }
        const uint32_t frames = ot::wavResampledFrames(ot::wavSourceFrames(info), info.sampleRate);
        if (streamable16(info)) {
            lab::copyStr(s.file, sizeof s.file, s.path);
        } else {
            char copy[128];
            tapeConvPath(s.path, frames, copy, sizeof copy);
            ot::WavInfo ci;
            if (fs.exists(copy) && probeWav(fs, copy, ci) && streamable16(ci)) {
                lab::copyStr(s.file, sizeof s.file, copy);
            } else {
                if (!conv_) {
                    s.state = kTapeFail;
                    return false;
                }
                lab::copyStr(convSrc_[c], sizeof convSrc_[c], s.path);
                convFrames_[c] = frames;
                convPend_ = uint8_t(convPend_ | (1u << c));
                s.state = kTapeConv;
                startConv();
                ctx_->toast("Preparing the tape (a 16-bit copy)...");
                return false;
            }
        }
    }
    if (!probeWav(fs, s.file, info) || !streamable16(info)) {
        s.state = kTapeFail;
        s.file[0] = 0;
        return false;
    }
    const uint32_t frames = ot::wavSourceFrames(info);
    const char* why = "";
    if (frames < kTapeMinFrames || !tapes_->bind(c, s.file, info.dataOffset, uint8_t(info.channels), frames, &why)) {
        s.state = kTapeFail;
        char m[64];
        if (!std::strcmp(why, "fragmented")) std::snprintf(m, sizeof m, "Card too fragmented: copy the file again");
        else std::snprintf(m, sizeof m, "CH %d: cannot stream the file (%s)", c + 1, why[0] ? why : "short");
        ctx_->toast(m);
        return false;
    }
    s.frames = frames;
    s.state = kTapeOpen;
    s.fromFrame = tapeFromFrame(c);
    s.seenFrom = s.fromFrame;
    s.loopNow = ctx_->params().get(tapePid_[c][kTpLoop]) > 0.5f ? 1 : 0;
    s.headFrom = tapes_->loadHead(c, s.fromFrame) ? s.fromFrame : 0xFFFFFFFFu;
    if (s.cue >= frames) s.cue = s.fromFrame;
    if (s.wantPlay) {
        s.wantPlay = 0;
        s.ended = 0;
        tapes_->play(c, s.cue, s.fromFrame, s.loopNow != 0);
    } else {
        tapes_->stop(c);
    }
    return true;
}

void Bulut::startConv() {
    if (!conv_ || conv_->busy() || !ctx_->fs) return;
    for (int c = 0; c < kCh; ++c) {
        if (!(convPend_ & (1u << c))) continue;
        char dst[128];
        tapeConvPath(convSrc_[c], convFrames_[c], dst, sizeof dst);
        char dir[96];
        std::snprintf(dir, sizeof dir, "%s/Stream", ctx_->home);
        ctx_->fs->mkdir(dir);
        if (conv_->start(*ctx_->fs, convSrc_[c], dst)) {
            convCh_ = c;
            return;
        }
        convPend_ = uint8_t(convPend_ & ~(1u << c));
        tape_[c].state = kTapeFail;
        ctx_->toast("Cannot convert the tape file (card full?)");
    }
}

float Bulut::tapeConvProgress(int c) const {
    if (convCh_ == c && conv_ && conv_->busy()) return conv_->progress();
    return (convPend_ >> c) & 1u ? 0.0f : -1.0f;
}

void Bulut::openTapeBrowser(int c) {
    if (!tapeAvailable() || !ctx_->fs) {
        ctx_->toast("TAPE FILE: no card / not available here");
        return;
    }
    kit::BrowserRequest q;
    static char title[48];
    std::snprintf(title, sizeof title, "TAPE FILE for CH %d (a long WAV)", c + 1);
    q.title = title;
    static char folder[128];
    const char* from = tape_[c].path[0] ? tape_[c].path : (src_[c].path[0] ? src_[c].path : "");
    lab::copyStr(folder, sizeof folder, from[0] ? from : "Samples");
    if (from[0]) {
        char* sl = std::strrchr(folder, '/');
        if (sl) *sl = 0;
        else folder[0] = 0;
    }
    q.folder = folder;
    q.exts = ".wav";
    q.ctx = this;
    pendingCh_ = c;
    pendingTape_ = true;
    q.onPick = [](void* cx, const char* path) {
        Bulut* b = static_cast<Bulut*>(cx);
        lab::copyStr(b->pendingPath_, sizeof b->pendingPath_, path);   // loaded in update()
    };
    ctx_->browser().open(q);
}

bool Bulut::swapTape(int c, int dir) {
    if (!ctx_->fs || !tape_[c].path[0]) {
        openTapeBrowser(c);
        return false;
    }
    char p[160];
    int idx = 0, n = 0;
    if (!neighbourWav(*ctx_->fs, tape_[c].path, dir, p, sizeof p, &idx, &n)) return false;
    if (!loadTape(c, p)) return false;
    char m[96];
    std::snprintf(m, sizeof m, "CH %d tape: %.32s  (%d/%d)", c + 1, baseName(p), idx + 1, n);
    ctx_->toast(m);
    return true;
}

// ---- transport -------------------------------------------------------------------------
bool Bulut::tapePlaying(int c) const {
    if (!tapes_) return false;
    const TapeSlot& s = tape_[c];
    if (s.state != kTapeOpen) return s.wantPlay != 0 && (s.state == kTapeWant || s.state == kTapeConv);
    const Tape& t = tapes_->tape(c);
    const uint32_t seq = t.reqSeq;
    if (seq != t.seen) return t.req[seq & 1u].play != 0;
    return t.playing != 0;
}

void Bulut::tapePlay(int c, bool on) {
    if (!tapes_) return;
    TapeSlot& s = tape_[c];
    if (s.state != kTapeOpen) {
        if (s.state == kTapeWant || s.state == kTapeConv) s.wantPlay = on ? 1 : 0;
        else if (on) ctx_->toast(tapeAvailable() ? "No tape file: Shift+F5 picks one" : "TAPE FILE: no card / not available here");
        return;
    }
    if (on == tapePlaying(c)) return;
    if (on) {
        if (s.ended || s.cue + 64u >= s.frames) s.cue = s.fromFrame;
        s.ended = 0;
        tapes_->play(c, s.cue, s.fromFrame, s.loopNow != 0);
    } else {
        s.cue = tapes_->requestPending(c) ? s.cue : tapes_->position(c);
        tapes_->stop(c);
    }
}

float Bulut::tapePos(int c) const {
    const TapeSlot& s = tape_[c];
    if (!tapes_ || s.frames == 0) return 0.0f;
    uint32_t f = s.cue;
    const Tape& t = tapes_->tape(c);
    if (s.state == kTapeOpen) {
        const uint32_t seq = t.reqSeq;
        if (seq != t.seen) f = t.req[seq & 1u].play ? t.req[seq & 1u].v0 : s.cue;
        else if (t.playing) f = tapes_->position(c);
    }
    return clamp01(float(f) / float(s.frames));
}

void Bulut::tapeJump(int c, float pos, bool now) {
    TapeSlot& s = tape_[c];
    if (s.frames == 0) return;
    s.jumpTo = clamp01(pos);
    s.jumpMs = lastMs_;
    if (!now || s.state != kTapeOpen) return;
    uint32_t f = uint32_t(s.jumpTo * float(s.frames));
    if (f + 64u >= s.frames) f = s.frames > 64u ? s.frames - 64u : 0;
    s.jumpTo = -1.0f;
    s.ended = 0;
    if (tapePlaying(c)) tapes_->play(c, f, s.fromFrame, s.loopNow != 0);
    else s.cue = f;
}

// ---- the main-loop pump: read-ahead, conversion, FROM / LOOP, scrub, end -------------
void Bulut::pumpTapes(uint32_t ms) {
    if (!tapes_) return;
    kit::Registry& P = ctx_->params();
    for (int c = 0; c < kCh; ++c) {
        TapeSlot& s = tape_[c];
        if (s.state == kTapeWant && par_[c].source == kSrcTape && ctx_->fs) tapeOpen(c);
        if (s.state != kTapeOpen) continue;
        Tape& t = tapes_->tape(c);
        const bool pending = tapes_->requestPending(c);
        if (t.ended && !pending) {   // a LOOP-off run reached the end: Play starts again at FROM
            t.ended = 0;
            s.ended = 1;
            s.cue = s.frames;
            if (par_[c].source == kSrcTape) {
                char m[40];
                std::snprintf(m, sizeof m, "CH %d: the tape reached its end", c + 1);
                ctx_->toast(m);
            }
        }
        const bool playing = tapePlaying(c);
        if (playing && !pending && t.playing) s.cue = tapes_->position(c);
        // the scrub: a jump applies once the encoder rests
        if (s.jumpTo >= 0.0f && ms - s.jumpMs >= 120u) tapeJump(c, s.jumpTo, true);
        // FROM (after a short rest) and LOOP: a new loop point, a cue while stopped
        const uint32_t from = tapeFromFrame(c);
        const uint8_t loop = P.get(tapePid_[c][kTpLoop]) > 0.5f ? 1 : 0;
        if (from != s.seenFrom) {
            s.seenFrom = from;
            s.fromMs = ms;
        }
        const bool fromDue = from != s.fromFrame && ms - s.fromMs >= 200u;
        if (fromDue || loop != s.loopNow) {
            if (fromDue) s.fromFrame = from;
            s.loopNow = loop;
            if (playing) tapes_->play(c, pending ? s.cue : tapes_->position(c), s.fromFrame, loop != 0);
            else if (fromDue) {
                s.cue = s.fromFrame;
                s.ended = 0;
            }
        }
        // the head follows FROM, but never while the run is inside the old one
        if (s.headFrom != s.fromFrame) {
            const uint32_t pos = tapes_->position(c);
            const bool insideOld = t.headOk && t.playing && pos >= t.headBase && pos < t.headBase + t.headLen;
            if (!insideOld) s.headFrom = tapes_->loadHead(c, s.fromFrame) ? s.fromFrame : 0xFFFFFFFFu;
        }
        // the virtual position stays far from the uint32 range (a re-anchor every ~6.7 h at 2 x)
        if (t.playing && !pending && t.need > kTapeRebase) tapes_->play(c, tapes_->position(c), s.fromFrame, loop != 0);
    }
    // read ahead: up to 12 ms of card reads a pass (a chunk is ~2-6 ms)
    tapes_->service(ctxMillis, ctx_, 12, 24);
    // the conversion job: ~10 ms a pass
    if (!conv_) return;
    if (conv_->busy()) {
        const uint32_t t0 = ctx_->millis();
        bool more = true;
        for (int i = 0; i < 64 && more; ++i) {   // 64 x 4 KB, or ~10 ms
            more = conv_->step(4096);
            if (ctx_->millis() - t0 >= 10) break;
        }
        if (more) return;
        const int c = convCh_;
        convCh_ = -1;
        if (c >= 0 && (convPend_ & (1u << c))) {
            convPend_ = uint8_t(convPend_ & ~(1u << c));
            TapeSlot& s = tape_[c];
            if (conv_->ok()) {
                lab::copyStr(s.file, sizeof s.file, conv_->dst());
                s.state = kTapeWant;
                if (par_[c].source == kSrcTape && tapeOpen(c)) {
                    char m[48];
                    std::snprintf(m, sizeof m, "CH %d: tape ready", c + 1);
                    ctx_->toast(m);
                }
            } else {
                s.state = kTapeFail;
                ctx_->toast("Cannot convert the tape file (card full?)");
            }
        }
    }
    if (convPend_ && !conv_->busy()) startConv();
}

// ---- projects ---------------------------------------------------------------------------
void Bulut::tapeSaveState(TapeSave* out) {
    for (int c = 0; c < kCh; ++c) {
        TapeSave& o = out[c];
        std::memset(&o, 0, sizeof o);
        const TapeSlot& s = tape_[c];
        lab::copyStr(o.path, sizeof o.path, s.path);
        lab::copyStr(o.label, sizeof o.label, s.label);
        o.playing = tapePlaying(c) ? 1 : 0;
        o.cue = s.frames ? uint32_t(tapePos(c) * float(s.frames)) : s.cue;
    }
}

void Bulut::tapeLoadState(const TapeSave* in) {
    if (conv_) conv_->cancel();
    convPend_ = 0;
    convCh_ = -1;
    for (int c = 0; c < kCh; ++c) {
        if (tapes_) tapes_->unbind(c);
        TapeSlot& s = tape_[c];
        s = TapeSlot();
        s.headFrom = 0xFFFFFFFFu;
        s.jumpTo = -1.0f;
        s.loopNow = 1;
        if (!in || !in[c].path[0]) continue;
        lab::copyStr(s.path, sizeof s.path, in[c].path);
        lab::copyStr(s.label, sizeof s.label, in[c].label[0] ? in[c].label : baseName(in[c].path));
        s.cue = in[c].cue;
        s.frames = 0;
        s.wantPlay = in[c].playing;
        s.state = kTapeWant;
        if (par_[c].source == kSrcTape) tapeOpen(c);   // the others open when they become TAPE FILE
    }
}

// ---- SOURCE page rows 5..9 on a TAPE FILE channel: PLAY, SPEED, FROM, LOOP, POSITION -----------
void Bulut::tapeEncoder(int row, int d, bool fine) {
    const int c = sel_;
    kit::Registry& P = ctx_->params();
    switch (row) {
        case 5: tapePlay(c, d > 0); break;
        case 6: P.nudge(tapePid_[c][kTpSpeed], d, fine, true); break;
        case 7: P.nudge(tapePid_[c][kTpFrom], d, fine, true); break;
        case 8: P.set(tapePid_[c][kTpLoop], d > 0 ? 1.0f : 0.0f, true); break;
        case 9: {
            const float at = tape_[c].jumpTo >= 0.0f ? tape_[c].jumpTo : tapePos(c);
            tapeJump(c, at + float(d) * (fine ? 0.001f : 0.01f), false);
            break;
        }
        default: break;
    }
}

void Bulut::tapeDelete(int row) {
    const int c = sel_;
    kit::Registry& P = ctx_->params();
    switch (row) {
        case 5: tapePlay(c, false); break;
        case 6: P.reset(tapePid_[c][kTpSpeed], true); break;
        case 7: P.reset(tapePid_[c][kTpFrom], true); break;
        case 8: P.reset(tapePid_[c][kTpLoop], true); break;
        case 9:   // back to FROM
            if (tape_[c].frames) tapeJump(c, float(tape_[c].fromFrame) / float(tape_[c].frames), true);
            break;
        default: break;
    }
}

void Bulut::tapeText(int c, char* out, int cap) const {
    const TapeSlot& s = tape_[c];
    if (!tapeAvailable()) {
        std::snprintf(out, size_t(cap), "TAPE: not available");
        return;
    }
    if (!s.path[0]) {
        std::snprintf(out, size_t(cap), "TAPE: no file");
        return;
    }
    if (s.state == kTapeConv) {
        const float p = tapeConvProgress(c);
        std::snprintf(out, size_t(cap), "TAPE %.14s %d%%", s.label, int((p < 0.0f ? 0.0f : p) * 100.0f));
        return;
    }
    if (s.state == kTapeFail) {
        std::snprintf(out, size_t(cap), "TAPE %.14s: error", s.label);
        return;
    }
    std::snprintf(out, size_t(cap), "TAPE %.14s %d%%%s", s.label, int(tapePos(c) * 100.0f), tapePlaying(c) ? "" : " ||");
}

// ---- bench: tape-file4 / tape-gran4 ------------------------------------------------------
// Every channel on TAPE FILE (a 10-minute stereo file on FakeStreamReader: the
// bench has no card), at 2 / 1.5 / 2 / 0.75 x from four FROM points, LOOP on;
// the main loop (update(), every 8 blocks in the bench) reads ahead. mixed:
// one of each engine with TEXTURE / SHIFTER / FREEZER / FLANGER, low-pass and
// drive on every channel (the "everything" chain); else GRANULAR at its maximum.
void Bulut::benchTapes(bool mixed) {
#if defined(OT_BENCH_M4) || !defined(__arm__)
    static FakeStreamReader fake;
    if (!tapes_) return;
    tapes_->setReader(&fake);
    kit::Registry& P = ctx_->params();
    static const float kSpeed[kCh] = {2.0f, 1.5f, 2.0f, 0.75f};
    for (int c = 0; c < kCh; ++c) {
        TapeSlot& s = tape_[c];
        s = TapeSlot();
        std::snprintf(s.path, sizeof s.path, "bench/tape%d.wav", c + 1);
        lab::copyStr(s.label, sizeof s.label, baseName(s.path));
        lab::copyStr(s.file, sizeof s.file, s.path);
        const uint32_t frames = 44100u * 600u;
        if (!tapes_->bind(c, s.file, 44, 2, frames, nullptr)) continue;
        s.frames = frames;
        s.state = kTapeOpen;
        s.loopNow = 1;
        s.jumpTo = -1.0f;
        P.set(tapePid_[c][kTpSpeed], kSpeed[c], false);
        P.set(tapePid_[c][kTpFrom], 0.1f * float(c), false);
        P.set(tapePid_[c][kTpLoop], 1.0f, false);
        s.fromFrame = s.seenFrom = s.cue = tapeFromFrame(c);
        s.headFrom = tapes_->loadHead(c, s.fromFrame) ? s.fromFrame : 0xFFFFFFFFu;
        P.set(pid_[c][CP::Source], float(kSrcTape), false);
        srcNow_[c] = kSrcTape;
        tapes_->play(c, s.fromFrame, s.fromFrame, true);
        if (mixed) {
            static const int kModes[kCh] = {kFxTexture, kFxShifter, kFxFreezer, kFxFlanger};
            P.set(pid_[c][CP::FxMode], float(kModes[c]), false);
            P.set(pid_[c][CP::FxAmt], 0.9f, false);
            P.set(pid_[c][CP::FxFb], 0.8f, false);
            P.set(pid_[c][CP::Flt], float(kFltLp), false);
            P.set(pid_[c][CP::Drive], 0.8f, false);
        }
    }
    P.sync();
#else
    (void)mixed;
#endif
}

}  // namespace bulut
