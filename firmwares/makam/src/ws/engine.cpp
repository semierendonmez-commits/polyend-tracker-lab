// MAKAM -- audio engine (engine.h).
#include "ws/engine.h"

#include <cmath>
#include <cstring>

#include "audio/audio_tables.h"
#include "fastrun.h"
#include "ws/gen.h"
#include "ws/usul.h"

namespace makam {

using lab::kBlock;
namespace dsp = lab::dsp;

namespace {
constexpr int kSub = 12;   // ticks per usul unit (rolls of 2, 3, 4, 6)
constexpr float kSr = 44100.0f;

// sin(2 pi p) for p in [0, 1): a triangle through the cubic x (1.5 - 0.5 x^2)
// (|error| < 0.02, smooth at the peaks). Oscillator use only.
inline float fastSin(float p) {
    float q = p + 0.25f;
    if (q >= 1.0f) q -= 1.0f;
    const float x = q < 0.5f ? q * 4.0f - 1.0f : 3.0f - q * 4.0f;
    return x * (1.5f - 0.5f * x * x);
}
// Sine wavefolder without sinf: sin(pi/2 x) = fastSin(x / 4).
inline float fastFold(float x) {
    float p = x * 0.25f;
    p -= floorf(p);
    return fastSin(p);
}
// tan(pi f / sr) for the SVF from a rational approximation (f < 0.45 sr).
inline void svfSet(lab::dsp::Svf& f, float hz, float q) {
    hz = hz < 20.0f ? 20.0f : (hz > 16000.0f ? 16000.0f : hz);
    const float x = 3.14159265f * hz / 44100.0f;
    const float x2 = x * x;
    f.g = x * (1.0f + x2 * (0.33333f + x2 * 0.13333f)) / (1.0f - x2 * 0.04f);
    f.k = 1.0f / q;
}
inline float tri(float p) { return p < 0.5f ? p * 4.0f - 1.0f : 3.0f - p * 4.0f; }
inline float levelGain(int v) {
    const float g = float(v) * 0.01f;
    return g * g * 1.4f;
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

bool Engine::init(lab::Arena& a, lab::Context* ctx) {
    ctx_ = ctx;
    int16_t* dmem = static_cast<int16_t*>(a.alloc(ot::audio::Delay::kMemorySamples * sizeof(int16_t)));
    int16_t* rmem = static_cast<int16_t*>(a.alloc(ot::audio::Reverb::kMemorySamples * sizeof(int16_t)));
    if (!dmem || !rmem) return false;
    delay_.init(dmem);
    reverb_.init(rmem);
    delay_.setFilter(5200.0f);
    reverb_.setDiffusion(0.7f);
    for (auto& v : lead_) {
        v.ks = static_cast<float*>(a.alloc(kKsLen * sizeof(float)));
        v.ks2 = static_cast<float*>(a.alloc(kKsLen * sizeof(float)));
        if (!v.ks || !v.ks2) return false;
    }
    bass_.amp.a = 0.003f;
    bass_.amp.s = 0.8f;
    bass_.amp.r = 0.05f;
    return true;
}

const Kit& Engine::kit() const {
    const int k = song ? song->kit : 0;
    if (k == kKits && wavReady) return kits[kKits];
    return kits[k >= 0 && k < kKits ? k : 0];
}

int Engine::degreeToComma(int deg, int octaveShift) const {
    const Makam& m = kMakams[song ? song->makam : 0];
    return degreeComma(m, deg) + 53 * octaveShift;
}

float Engine::commaToHz(int comma) const {
    return freqHz(comma, song && song->tuning ? kTune53 : kTuneAEU, 440.0f, song ? song->transpose : 0);
}

// ---- MIDI out: nearest 12-TET note + pitch bend for the comma offset ------------
void Engine::sendNote(bool on, int chOff, int comma, uint8_t vel, uint8_t* lastNote) {
    if (!song || !song->midiOut || !ctx_ || !ctx_->shell) {
        *lastNote = 255;
        return;
    }
    if (*lastNote != 255) {
        ctx_->midiNote(false, *lastNote, 0, uint8_t(chOff));
        *lastNote = 255;
    }
    if (!on) return;
    const Tuning tu = song && song->tuning ? kTune53 : kTuneAEU;
    const float semis = 69.0f + (cents(comma, tu) - cents(kDugah, tu)) * 0.01f + float(song ? song->transpose : 0);
    int n = int(floorf(semis + 0.5f));
    n = clampi(n, 0, 127);
    const int range = song->midiBend < 1 ? 1 : song->midiBend;
    int bend = 8192 + int((semis - float(n)) / float(range) * 8192.0f);
    bend = clampi(bend, 0, 16383);
    lab::MidiMsg pb;
    pb.status = uint8_t(0xE0 | ((ctx_->midiChannel() + chOff) & 15));
    pb.d1 = uint8_t(bend & 127);
    pb.d2 = uint8_t(bend >> 7);
    ctx_->midiOut(pb);
    ctx_->midiNote(true, uint8_t(n), vel, uint8_t(chOff));
    *lastNote = uint8_t(n);
}

// ---- break --------------------------------------------------------------------------
void Engine::triggerSlice(int slice, int pitch, bool rev, bool stutter, float vel) {
    const Kit& k = kit();
    if (!k.data || k.frames < 64) return;
    slice = clampi(slice, 0, kSlices - 1);
    SliceVoice& old = sv_[svNext_ ^ 1];
    if (old.active) old.fadeStep = -1.0f / 96.0f;   // ~2 ms crossfade
    SliceVoice& v = sv_[svNext_];
    svNext_ ^= 1;
    const float a = float(k.start[slice]), b = float(k.start[slice + 1]);
    v.data = k.data;
    v.frames = k.frames;
    float rate = tempoRate_ * (perfHalf ? 0.5f : 1.0f);
    if (pitch) rate *= exp2f(float(pitch) * (1.0f / 12.0f));
    v.rate = rate;
    v.rev = rev;
    v.stutter = stutter;
    v.start = a;
    v.loopLen = (b - a) * 0.25f;
    v.end = rev ? a : float(k.frames - 2);
    v.pos = rev ? b - 1.0f : a;
    v.gain = vel;
    v.fade = 0.0f;
    v.fadeStep = 1.0f / 24.0f;
    v.active = true;
    st.slice = slice;
    st.sliceHit = st.sliceHit + 1;
}

// ---- bass ----------------------------------------------------------------------------
void Engine::bassNote(int deg, int flags, float vel) {
    const int comma = degreeToComma(deg, song->bp.octave);
    const float f = commaToHz(comma);
    bass_.target = f;
    const bool glide = (flags & kNoteGlide) && bass_.gate;
    if (!glide) bass_.freq = f;
    const float gt = 0.01f + float(song->bp.glide) * 0.002f;   // 10..210 ms
    bass_.glide = expf(-1.0f / (gt * kSr));
    bass_.glide16 = expf(-16.0f / (gt * kSr));
    bass_.vel = (flags & kNoteAccent) ? 1.0f : 0.85f * vel;
    if (!glide) {
        bass_.amp.gate(true);
        bass_.fenv = 1.0f;
    }
    bass_.gate = true;
    st.bassDeg = deg;
    sendNote(true, 0, comma, uint8_t(bass_.vel * 120.0f), &midiBass_);
}

void Engine::bassOff() {
    if (!bass_.gate) return;
    bass_.gate = false;
    bass_.amp.gate(false);
    st.bassDeg = -100;
    sendNote(false, 0, 0, 0, &midiBass_);
}

// ---- lead ------------------------------------------------------------------------------
void Engine::leadNote(int deg, int flags, float vel, uint8_t padId) {
    // Voice 0: the sequenced phrase (mono, legato). Pads / MIDI: voices 1..2.
    int vi = 0;
    if (padId != 255) {
        vi = 1 + leadNext_;
        leadNext_ = (leadNext_ + 1) % (kLeadVoices - 1);
    }
    LeadVoice& v = lead_[vi];
    const int comma = degreeToComma(deg, song->lp.octave);
    const float f = commaToHz(comma);
    const int model = song->lp.model;
    const bool legato = v.gate && (flags & kNoteGlide);
    v.target = f;
    const float gt = 0.015f + float(song->lp.glide) * 0.0025f;   // 15..265 ms
    v.glideK = legato ? 1.0f - expf(-1.0f / (gt * kSr)) : 1.0f;
    v.glide16 = legato ? 1.0f - expf(-16.0f / (gt * kSr)) : 1.0f;
    if (!legato) v.freq = f;
    v.vel = ((flags & kNoteAccent) ? 1.0f : 0.8f) * vel;
    v.padId = padId;
    v.deg = deg;
    v.graceLeft = 0;
    if (flags & kNoteGrace) {   // grace note from the degree above (cesni ornament)
        v.graceFreq = commaToHz(degreeToComma(deg + 1, song->lp.octave));
        v.graceLeft = int32_t(0.045f * kSr);
    }
    if (!legato) {
        v.age = 0.0f;
        const bool plucked = model == 1 || model == 2;
        v.amp.a = model == 0 ? 0.05f : (model == 3 ? 0.012f : 0.001f);
        v.amp.d = 0.3f;
        v.amp.s = plucked ? 1.0f : 0.85f;
        v.amp.r = plucked ? 0.08f : 0.12f;
        v.amp.gate(true);
        if (plucked) {   // Karplus-Strong: a filtered noise burst one period long, fed per sample
            const float pf = v.graceLeft ? v.graceFreq : f;
            uint32_t len = uint32_t(kSr / pf) + 2;
            if (len > kKsLen - 4) len = kKsLen - 4;
            v.burst = int32_t(len);
            v.burstLp = 0.0f;
            v.burstK = 0.25f + float(song->lp.tone) * 0.0075f;
        }
    }
    v.gate = true;
    v.active = true;
    if (vi == 0 || padId != 255) {
        st.leadDeg = deg;
        st.leadComma = comma;
    }
    sendNote(true, 1, comma, uint8_t(v.vel * 120.0f), &midiLead_);
}

void Engine::leadOff(uint8_t padId) {
    for (int i = 0; i < kLeadVoices; ++i) {
        LeadVoice& v = lead_[i];
        if (!v.gate || v.padId != padId) continue;
        if (padId == 255 && i != 0) continue;
        v.gate = false;
        v.amp.gate(false);
    }
    st.leadDeg = -100;
    sendNote(false, 1, 0, 0, &midiLead_);
}

void Engine::allOff() {
    bassOff();
    for (auto& v : lead_)
        if (v.gate) {
            v.gate = false;
            v.amp.gate(false);
        }
    sendNote(false, 1, 0, 0, &midiLead_);
    st.leadDeg = -100;
}

// ---- sequencer -------------------------------------------------------------------------
void Engine::stepEvent(int32_t unit, int len) {
    Song& s = *song;
    const int step = int(unit % len);
    const int32_t cycle = unit / len;
    if (step == 0 || unit == 0) {
        if (s.songMode) {
            uint32_t start = 0;
            section_ = songSectionAt(s, uint32_t(cycle), &start);
            secStart_ = int(start % songCycles(s));
            // cycles inside the section
            const uint32_t total = songCycles(s);
            st.secCycle = int32_t((uint32_t(cycle) % total) - start);
        } else {
            section_ = clampi(s.curSection, 0, s.sectionCount - 1);
            st.secCycle = cycle;
        }
        st.section = section_;
        st.cycle = cycle;
    }
    st.step = step;
    st.unit = unit;
    const Section& sec = s.sec[section_];
    // Song mode plays the section's variations; loop mode plays the ones
    // being edited, for the parts the section has switched on.
    const int brkVar = s.songMode ? sec.brk - 1 : (sec.brk ? s.editVar : -1);
    const int bassVar = s.songMode ? sec.bass - 1 : (sec.bass ? s.editVar : -1);
    const int phr = s.songMode ? sec.lead - 1 : (sec.lead ? s.editPhrase : -1);
    const int chaos = perfChaos ? 100 : sec.chaos;

    // break
    curOn_ = false;
    if (brkVar >= 0 && !perfMute) {
        const BreakStep& b = s.brk[brkVar].s[step];
        int stroke = b.stroke;
        int roll = b.roll, pitch = b.pitch;
        bool rev = b.rev, stut = b.stutter;
        int slice = b.slice;
        // live jungle edits: more at the end of every 4th cycle (turnaround)
        const bool turn = (st.secCycle % 4) == 3 && step >= len - 3;
        const int p = chaos / 2 + (turn ? chaos / 2 : 0);
        if (p > 0 && rng_.range(100) < p) {
            switch (rng_.range(5)) {
                case 0: roll = rng_.chance(0.5f) ? 2 : 3; if (!stroke) stroke = kKe; break;
                case 1: rev = true; break;
                case 2: pitch = rng_.chance(0.5f) ? -12 : 5; break;
                case 3: stut = true; if (!stroke) stroke = kTek; break;
                default: slice = rng_.range(kSlices); if (!stroke) stroke = kKe; break;
            }
        }
        if (perfRoll) {
            roll = perfRoll;
            if (!stroke) stroke = kTek;
        }
        if (perfRev) rev = true;
        if (perfStutter) stut = true;
        if (stroke) {
            const Kit& k = kit();
            if (slice < 0) {
                const int c = stroke == kDum ? kClsKick : (stroke == kTek ? kClsSnare : kClsGhost);
                const int n = k.count[c] ? k.count[c] : 1;
                // the n-th stroke of this kind in the cycle takes the n-th slice of the class
                int nth = 0;
                const char* str = kUsuls[s.usul].strokes;
                for (int i = 0; i < step; ++i)
                    if (strokeOf(str[i]) == stroke) ++nth;
                slice = k.list[c][(nth + (cycle & 1) * (stroke == kKe ? 1 : 0)) % n];
            }
            const float vel = stroke == kKe ? 0.62f : (stroke == kDum ? 1.0f : 0.92f);
            curRoll_ = roll < 1 ? 1 : roll;
            curSlice_ = slice;
            curPitch_ = pitch;
            curRev_ = rev;
            curStutter_ = stut;
            curOn_ = true;
            triggerSlice(slice, pitch, rev, stut, vel);
            if (stroke == kDum) duck_ = float(s.duck) * 0.01f * 0.7f;
        } else if (b.choke) {
            for (auto& v : sv_)
                if (v.active) v.fadeStep = -1.0f / 64.0f;
        }
    }

    // bass
    if (bassVar >= 0) {
        const NoteStep& n = s.bass[bassVar].s[step];
        if (n.flags & kNoteOn) bassNote(n.deg, n.flags, 1.0f);
        else if (!(n.flags & kNoteTie)) bassOff();
    } else {
        bassOff();
    }

    // lead phrase
    if (phr >= 0) {
        const Phrase& ph = s.phr[phr];
        const int n = phraseSteps(ph, s.usul);
        const int idx = int((st.secCycle * len + step) % n);
        st.phraseStep = idx;
        if (idx == 0 && unit > 0) st.phraseWraps = st.phraseWraps + 1;
        const NoteStep& x = ph.s[idx];
        if (x.flags & kNoteOn) leadNote(x.deg, x.flags, 1.0f, 255);
        else if (!(x.flags & kNoteTie)) leadOff(255);
    } else if (lead_[0].gate) {
        leadOff(255);
    }
}

void Engine::subTick(int32_t unit, int sub, int swingTicks) {
    // roll repeats inside the unit
    if (!curOn_ || curRoll_ <= 1) return;
    const int every = kSub / curRoll_;
    const int rel = sub - swingTicks;
    if (rel <= 0 || rel % every) return;
    const int hitN = rel / every;
    if (hitN >= curRoll_) return;
    (void)unit;
    triggerSlice(curSlice_, curPitch_ + (curRoll_ >= 4 ? hitN : 0), curRev_, curStutter_, 0.55f + 0.12f * float(hitN));
}

// ---- per-sample rendering --------------------------------------------------------------
OT_FASTRUN_FN void Engine::renderSegment(uint32_t i0, uint32_t i1) {
    if (i1 <= i0) return;
    const Song& s = *song;
    // ---- break voices
    for (uint32_t i = i0; i < i1; ++i) brk_[i] = 0.0f;
    for (auto& v : sv_) {
        if (!v.active) continue;
        for (uint32_t i = i0; i < i1; ++i) {
            const int32_t ip = int32_t(v.pos);
            if (ip < 0 || uint32_t(ip) + 1 >= v.frames) {
                v.active = false;
                break;
            }
            const float fr = v.pos - float(ip);
            const float a = float(v.data[ip]), b = float(v.data[ip + 1]);
            v.fade += v.fadeStep;
            if (v.fade >= 1.0f) {
                v.fade = 1.0f;
                v.fadeStep = 0.0f;
            }
            if (v.fade <= 0.0f && v.fadeStep < 0.0f) {
                v.active = false;
                break;
            }
            brk_[i] += (a + (b - a) * fr) * (1.0f / 32768.0f) * v.gain * v.fade;
            if (v.rev) {
                v.pos -= v.rate;
                if (v.pos <= v.end) v.fadeStep = -1.0f / 32.0f;
            } else {
                v.pos += v.rate;
                if (v.stutter && v.pos >= v.start + v.loopLen) v.pos -= v.loopLen;
            }
        }
    }
    float pos = 0.0f;
    for (auto& v : sv_)
        if (v.active && v.frames) pos = v.pos / float(v.frames);
    st.slicePos = pos;

    // ---- bass
    BassVoice& b = bass_;
    const int model = s.bp.model;
    const float det = 0.002f + float(s.bp.detune) * 0.00012f;
    const float envAmt = float(s.bp.env) * 0.01f;
    const float fdec = expf(-1.0f / ((0.03f + float(s.bp.decay) * 0.004f) * kSr));
    const float drive = 1.0f + float(s.bp.drive) * 0.05f;
    for (uint32_t i = i0; i < i1; ++i) bas_[i] = 0.0f;
    if (b.amp.active()) {
        for (uint32_t i = i0; i < i1; ++i) {
            if (((i - i0) & 15) == 0) {
                b.freq = b.target + (b.freq - b.target) * b.glide16;
                b.o1.setHz(b.freq * (1.0f + det));
                b.o2.setHz(b.freq * (1.0f - det));
                b.o3.setHz(model == 1 ? b.freq : b.freq * 0.5f);
                float cut = bassCut_ * (1.0f + envAmt * 6.0f * b.fenv);
                if (model == 2) cut *= 1.0f + 3.0f * (0.5f + 0.5f * tri(b.lfo));
                svfSet(b.f, cut, bassQ_);
            }
            b.fenv *= fdec;
            const float e = b.amp.process();
            float x;
            if (model == 1) {   // Sub: sine + a touch of the octave
                const float p = b.o3.step();
                float p2 = p * 2.0f;
                if (p2 >= 1.0f) p2 -= 1.0f;
                x = fastSin(p) + 0.18f * fastSin(p2) * envAmt;
                x = dsp::softclip(x * drive) * 0.9f;
            } else {
                float raw = b.o1.saw() + b.o2.saw();
                if (model == 2) {   // Growl: square on top, folded
                    raw += b.o3.square(0.3f) * 0.7f;
                    b.lfo += (s.bpm / 60.0f * 4.0f) * (1.0f / kSr);   // 16th-note wobble
                    if (b.lfo >= 1.0f) b.lfo -= 1.0f;
                } else {
                    raw += fastSin(b.o3.step()) * 0.9f;   // sub an octave down
                }
                b.f.process(raw * 0.5f);
                x = b.f.lp;
                if (model == 2) x = fastFold(x * drive * 0.8f);
                else x = dsp::softclip(x * drive);
            }
            bas_[i] = x * e * b.vel * 0.55f;
        }
    }

    // ---- lead
    for (uint32_t i = i0; i < i1; ++i) led_[i] = 0.0f;
    const int lm = s.lp.model;
    const float tone = float(s.lp.tone) * 0.01f;
    const float vibDepth = float(s.lp.vibrato) * 0.0001f;   // up to 1 %
    const float vibInc = 5.3f / kSr;
    const float ksDecay = 0.9935f + float(s.lp.decay) * 0.000062f;
    for (auto& v : lead_) {
        if (!v.active) continue;
        if (!v.amp.active()) {
            v.active = false;
            continue;
        }
        if (lm == 0) svfSet(v.f, v.freq * 2.0f, 1.2f);
        else if (lm == 3) svfSet(v.f, 1100.0f + tone * 1400.0f, 2.2f);
        float f = v.freq, inc = f / kSr, d = kSr / f;
        for (uint32_t i = i0; i < i1; ++i) {
            if (((i - i0) & 15) == 0) {   // control rate: glide, vibrato, grace, period
                v.freq += (v.target - v.freq) * v.glide16;
                v.age += 16.0f / kSr;
                v.vibPh += vibInc * 16.0f;
                if (v.vibPh >= 1.0f) v.vibPh -= 1.0f;
                const float vibOn = v.age > 0.25f ? 1.0f : v.age * 4.0f;
                f = v.freq * (1.0f + vibDepth * vibOn * tri(v.vibPh));
                if (v.graceLeft > 0) {
                    v.graceLeft -= 16;
                    f = v.graceFreq;
                    if (v.graceLeft <= 0) {
                        v.graceLeft = 0;
                        v.glide16 = 1.0f - expf(-16.0f / (0.012f * kSr));
                    }
                }
                inc = f * (1.0f / kSr);
                d = kSr / f;
                if (d > float(kKsLen - 4)) d = float(kKsLen - 4);
            }
            const float e = v.amp.process();
            float x = 0.0f;
            switch (lm) {
                case 0: {   // Ney: breathy, few harmonics, chiff of noise
                    v.ph += inc;
                    if (v.ph >= 1.0f) v.ph -= 1.0f;
                    float p2 = v.ph * 2.0f;
                    if (p2 >= 1.0f) p2 -= 1.0f;
                    v.f.process(rng_.bi());
                    const float chiff = v.age < 0.08f ? (0.08f - v.age) * 6.0f : 0.0f;
                    x = fastSin(v.ph) * 0.75f + fastSin(p2) * 0.18f * tone + v.f.bp * (0.22f + chiff) * (1.2f - tone * 0.6f);
                    break;
                }
                case 3: {   // Zurna: bright saw through a nasal formant, driven
                    v.ph += inc;
                    if (v.ph >= 1.0f) v.ph -= 1.0f;
                    const float saw = 2.0f * v.ph - 1.0f;
                    v.f.process(saw);
                    x = dsp::softclip((v.f.bp * 1.8f + saw * 0.25f) * 1.6f) * 0.7f;
                    break;
                }
                default: {  // Kanun (1) / Saz (2): Karplus-Strong strings
                    float rp = float(v.ksW) - d;
                    if (rp < 0.0f) rp += float(kKsLen);
                    const uint32_t j = uint32_t(rp);
                    const float fr = rp - float(j);
                    const float a0 = v.ks[j & (kKsLen - 1)], a1 = v.ks[(j + 1) & (kKsLen - 1)];
                    const float y = a0 + (a1 - a0) * fr;
                    const float damp = v.gate ? ksDecay : 0.97f;
                    const float bright = lm == 1 ? 0.55f + tone * 0.4f : 0.7f + tone * 0.28f;
                    float in = y;
                    if (v.burst > 0) {   // excitation: the noise burst replaces the loop for one period
                        --v.burst;
                        v.burstLp += v.burstK * (rng_.bi() - v.burstLp);
                        in = v.burstLp * 0.9f;
                    }
                    v.ksLp += bright * (in - v.ksLp);
                    v.ks[v.ksW] = v.burst > 0 ? in : v.ksLp * damp;
                    x = v.burst > 0 ? in : y;
                    if (lm == 2) {   // Saz: second course an octave up, slightly off
                        float rp2 = float(v.ksW) - d * 0.4985f;
                        if (rp2 < 0.0f) rp2 += float(kKsLen);
                        const uint32_t j2 = uint32_t(rp2);
                        const float f2 = rp2 - float(j2);
                        const float b0 = v.ks2[j2 & (kKsLen - 1)], b1 = v.ks2[(j2 + 1) & (kKsLen - 1)];
                        const float y2 = b0 + (b1 - b0) * f2;
                        const float in2 = v.burst > 0 ? in * 0.75f : y2;
                        v.ksLp2 += 0.8f * (in2 - v.ksLp2);
                        v.ks2[v.ksW] = v.burst > 0 ? in2 : v.ksLp2 * damp;
                        x = dsp::softclip((y + y2 * 0.6f) * 1.5f) * 0.8f;
                    }
                    v.ksW = (v.ksW + 1) & (kKsLen - 1);
                    x *= 1.3f;
                    break;
                }
            }
            led_[i] += x * e * v.vel * 0.45f;
        }
    }
}

void Engine::updateFx(float bpm) {
    const Song& s = *song;
    const int bx = int(bpm * 10.0f);
    if (s.dlyTime != lastFxTime_ || bx != lastBpmX10_) {
        lastFxTime_ = s.dlyTime;
        lastBpmX10_ = bx;
        const float beats = kDelaySyncBeats[clampi(s.dlyTime, 0, kDelaySyncCount - 1)];
        delay_.setTimeSamples(uint32_t(beats * 60.0f / (bpm > 20.0f ? bpm : 120.0f) * kSr));
    }
    if (s.dlyFb != lastFb_) {
        lastFb_ = s.dlyFb;
        delay_.setFeedbackK(ot::audio::delayFeedbackK(clampi(s.dlyFb, 0, 95)));
    }
    const float size = 0.35f + float(s.rvbSize) * 0.006f;
    if (size != lastSize_) {
        lastSize_ = size;
        reverb_.setSize(size);
    }
    const float damp = float(s.rvbDamp) * 0.01f;
    if (damp != lastDamp_) {
        lastDamp_ = damp;
        reverb_.setDamping(damp);
    }
}

OT_FASTRUN_FN void Engine::render(const lab::Transport& t, float* l, float* r) {
    if (!song || !kits) return;
    Song& s = *song;
    bpm_ = t.bpm;
    const Kit& k = kit();
    tempoRate_ = s.breakResample && k.bpm > 1.0f ? t.bpm / k.bpm : 1.0f;
    updateFx(t.bpm);

    if (resetReq) {
        resetReq = false;
        allOff();
        lastTick_ = -1;
        for (auto& v : sv_) v.active = false;
    }
    // live events (pads, MIDI in)
    LiveEv ev;
    while (live.pop(ev)) {
        switch (ev.type) {
            case LiveEv::kLeadOn: leadNote(ev.deg, kNoteOn | kNoteAccent, float(ev.vel) / 127.0f, ev.id); break;
            case LiveEv::kLeadOff: leadOff(ev.id); break;
            case LiveEv::kBassOn: bassNote(ev.deg, kNoteOn | kNoteAccent, float(ev.vel) / 127.0f); break;
            case LiveEv::kBassOff: bassOff(); break;
            case LiveEv::kSlice: triggerSlice(ev.deg, 0, false, false, float(ev.vel) / 127.0f); break;
            default: allOff(); break;
        }
    }
    // bass filter base (per block)
    bassCut_ = 60.0f + float(s.bp.cutoff) * float(s.bp.cutoff) * 0.35f;
    bassQ_ = 0.6f + float(s.bp.reso) * 0.09f;

    // sequencer ticks inside the block
    uint32_t done = 0;
    if (t.stoppedThisBlock) {
        allOff();
        st.unit = -1;
        lastTick_ = -1;
    }
    if (t.playing && t.beatsPerSample > 0.0) {
        const int len = usulLength(s.usul);
        const double unitBeats = s.unit ? 0.5 : 0.25;
        const double tickBeats = unitBeats / kSub;
        const int swingTicks = (s.swing * kSub) / 100;
        if (lastTick_ >= 0 && double(lastTick_) * tickBeats > t.beat + 1.0) lastTick_ = -1;   // beat jumped back
        if (t.startedThisBlock || lastTick_ < 0) {
            const double first = t.beat / tickBeats;
            lastTick_ = int32_t(first) - (first == double(int32_t(first)) ? 1 : 0);
            if (t.beat <= 0.0) lastTick_ = -1;
        }
        const double endBeat = t.beat + t.beatsPerSample * double(kBlock);
        for (int32_t tk = lastTick_ + 1;; ++tk) {
            const double tb = double(tk) * tickBeats;
            if (tb >= endBeat) break;
            int at = tb <= t.beat ? 0 : int((tb - t.beat) / t.beatsPerSample);
            if (at >= int(kBlock)) break;
            renderSegment(done, uint32_t(at));
            done = uint32_t(at);
            lastTick_ = tk;
            const int32_t unit = tk / kSub;
            const int sub = int(tk % kSub);
            const int swing = (unit & 1) ? swingTicks : 0;
            if (sub == swing) stepEvent(unit, len);
            else subTick(unit, sub, swing);
        }
    }
    renderSegment(done, kBlock);

    // ---- mixer, sends, ducking, stems
    const Section& sec = s.sec[clampi(section_, 0, kSections - 1)];
    int filt = s.djFilter + (s.songMode ? sec.filter : sec.filter);
    filt = clampi(filt, -100, 100);
    if (filt != 0) {
        if (filt < 0) brkF_.set(18000.0f * exp2f(float(filt) * 0.075f), 1.1f);   // LP down to ~100 Hz
        else brkF_.set(30.0f * exp2f(float(filt) * 0.07f), 1.1f);                 // HP up to ~3.8 kHz
    }
    const float bDrive = 1.0f + float(s.drive) * 0.04f;
    const Channel* ch = s.ch;
    float g[3], pl[3], pr[3], ds[3], rs[3];
    for (int c = 0; c < 3; ++c) {
        g[c] = ch[c].mute ? 0.0f : levelGain(ch[c].level);
        const float pan = float(clampi(ch[c].pan, -100, 100)) * 0.01f;
        pl[c] = pan > 0.0f ? 1.0f - pan : 1.0f;
        pr[c] = pan < 0.0f ? 1.0f + pan : 1.0f;
        ds[c] = float(ch[c].dly) * 0.01f;
        rs[c] = float(ch[c].rvb) * 0.01f;
    }
    const float duckK = expf(-1.0f / (0.09f * kSr));
    float pk[3] = {};
    for (uint32_t i = 0; i < kBlock; ++i) {
        float x = brk_[i];
        if (filt != 0) {
            brkF_.process(x);
            x = filt < 0 ? brkF_.lp : brkF_.hp;
        }
        x = dsp::softclip(x * bDrive) * (1.0f / (bDrive > 1.5f ? 1.2f : 1.0f));
        const float duck = 1.0f - duck_;
        duck_ *= duckK;
        const float xb = bas_[i] * duck, xl = led_[i] * (0.4f + 0.6f * duck);
        const float v0 = x * g[0], v1 = xb * g[1], v2 = xl * g[2];
        bL_[i] = v0 * pl[0];
        bR_[i] = v0 * pr[0];
        sL_[i] = v1 * pl[1];
        sR_[i] = v1 * pr[1];
        lL_[i] = v2 * pl[2];
        lR_[i] = v2 * pr[2];
        dIn_[i] = v0 * ds[0] + v1 * ds[1] + v2 * ds[2];
        rIn_[i] = v0 * rs[0] + v1 * rs[1] + v2 * rs[2];
        const float a0 = fabsf(v0), a1 = fabsf(v1), a2 = fabsf(v2);
        if (a0 > pk[0]) pk[0] = a0;
        if (a1 > pk[1]) pk[1] = a1;
        if (a2 > pk[2]) pk[2] = a2;
    }
    // sends: delay feeds the reverb a little (dub tail)
    const bool dOn = delay_.process(dIn_, fL_, fR_);
    if (!dOn) {
        for (uint32_t i = 0; i < kBlock; ++i) fL_[i] = fR_[i] = 0.0f;
    }
    for (uint32_t i = 0; i < kBlock; ++i) rIn_[i] += (fL_[i] + fR_[i]) * 0.15f;
    float rvL[kBlock], rvR[kBlock];
    const bool rOn = reverb_.process(rIn_, rvL, rvR);
    const float gf = ch[3].mute ? 0.0f : levelGain(ch[3].level);
    float pf = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        float a = fL_[i], c = fR_[i];
        if (rOn) {
            a += rvL[i];
            c += rvR[i];
        }
        fL_[i] = a * gf;
        fR_[i] = c * gf;
        const float m = fabsf(fL_[i]) > fabsf(fR_[i]) ? fabsf(fL_[i]) : fabsf(fR_[i]);
        if (m > pf) pf = m;
        l[i] += bL_[i] + sL_[i] + lL_[i] + fL_[i];
        r[i] += bR_[i] + sR_[i] + lR_[i] + fR_[i];
    }
    if (ctx_ && ctx_->shell) {
        ctx_->stem(0, bL_, bR_);
        ctx_->stem(1, sL_, sR_);
        ctx_->stem(2, lL_, lR_);
        ctx_->stem(3, fL_, fR_);
    }
    for (int c = 0; c < 3; ++c) pk_[c] = pk[c] > pk_[c] ? pk[c] : pk_[c] * 0.9f;
    pk_[3] = pf > pk_[3] ? pf : pk_[3] * 0.9f;
    for (int c = 0; c < 4; ++c) st.meter[c] = pk_[c];
}

}  // namespace makam
