// Players / generators of a clip.
//
// The six generators of src/core/gen (the user's code, unchanged) write an
// ot::Pattern track; KONSOL runs them on a scratch pattern and converts the
// steps into clip notes (patternToNotes): note + FX become pitch, velocity
// (Volume), length (Gate / next note / Off), slide (Glide), ratchets (Roll),
// reverse, micro timing and slices. Genotype and Turnaround read the clip's
// own notes first (notesToPattern).
//
// Four KONSOL players write notes directly: Euclid, Usul (aksak cycles),
// Pinball (a ball over the scale) and Markov (a melodic chain).
//
// A clip's generator runs either once (WRITE: the notes land in the piano roll
// to edit) or LIVE: after every `every` loops of the playing clip the main loop
// writes the next variation into the clip's back buffer, and the audio swaps
// it in exactly at the loop point (Acid mutates its phrase, BBCut cuts anew,
// Genotype walks its learnt chain, the others re-roll their seed).
#include <cstdio>
#include <cstring>

#include "gen/gen.h"
#include "gen/gen_internal.h"
#include "model/pattern.h"
#include "ws/gen_glue.h"
#include "ws/konsol.h"

namespace lab {
namespace konsol {

namespace {

using ot::gen::ParamDesc;

const char* const kUsulNames[7] = {"Aksak 9/8", "Curcuna 10/8", "Devr-i Hindi 7/8", "Turk Aksagi 5/8",
                                   "Aksak Semai 10/8", "Duyek 8/8", "Sofyan 4/4"};
// 'D' dum (low, accented), 'T' tek (high), 'k' ke (light), '.' rest; 1 char = 1/8.
const char* const kUsul[7] = {"D.T.D.TT.", "D.kT.D.T..", "D.kT.T.", "D.T.k", "D..T.D.T.k", "DT.TD.T.", "D...T.T."};

const ParamDesc kEuclidP[kGenParams] = {
    {"Hits", 1, 32, 5, nullptr},  {"Rotate", 0, 31, 0, nullptr}, {"Note", -12, 24, 0, nullptr},
    {"Spread", 0, 7, 2, nullptr}, {"Accent", 0, 100, 40, nullptr}, {"Seed", 0, 999, 0, nullptr}};
const ParamDesc kUsulP[kGenParams] = {
    {"Usul", 0, 6, 0, kUsulNames}, {"Repeats", 1, 4, 2, nullptr}, {"Dum note", 0, 127, 0, nullptr},
    {"Tek note", 0, 127, 0, nullptr}, {"Fills", 0, 100, 30, nullptr}, {"Seed", 0, 999, 0, nullptr}};
const ParamDesc kPinballP[kGenParams] = {
    {"Speed", 0, 100, 60, nullptr}, {"Gravity", 0, 100, 30, nullptr}, {"Bumpers", 0, 100, 45, nullptr},
    {"Range", 1, 3, 2, nullptr},    {"Gate", 5, 100, 60, nullptr},    {"Seed", 0, 999, 0, nullptr}};
const ParamDesc kMarkovP[kGenParams] = {
    {"Density", 0, 100, 65, nullptr}, {"Stepwise", 0, 100, 60, nullptr}, {"Leaps", 0, 100, 15, nullptr},
    {"Range", 1, 3, 2, nullptr},      {"Gate", 5, 100, 70, nullptr},     {"Seed", 0, 999, 0, nullptr}};
const ParamDesc kNone = {nullptr, 0, 0, 0, nullptr};

constexpr int kRates[8] = {1, 2, 3, 4, 6, 8, 12, 16};

bool drumLike(int engine) { return engine == kEngDrums || engine == kEngPerc; }

}  // namespace

const ParamDesc& genParam(int kind, int i, const int16_t* v) {
    if (i < 0 || i >= kGenParams) return kNone;
    if (kind >= kGenAcid && kind <= kGenStation) return ot::gen::param(kind - kGenAcid, i, v);
    switch (kind) {
        case kGenEuclid: return kEuclidP[i];
        case kGenUsul: return kUsulP[i];
        case kGenPinball: return kPinballP[i];
        case kGenMarkov: return kMarkovP[i];
        default: return kNone;
    }
}

int usulSteps(int usul, int repeats) {
    usul = usul < 0 ? 0 : (usul > 6 ? 6 : usul);
    const int n = int(std::strlen(kUsul[usul])) * 2 * (repeats < 1 ? 1 : repeats);
    return n > kMaxSteps ? kMaxSteps : n;
}

// ---------------------------------------------------------------------------
// ot::Pattern <-> notes
// ---------------------------------------------------------------------------
void Konsol::patternToNotes(const ot::Pattern& p, int track, int steps, NoteBuf& out, bool sliceNotes) {
    using namespace ot::gen;
    out.clear();
    const ot::Track& tr = p.tracks[track];
    for (int s = 0; s < steps; ++s) {
        const ot::Step& st = tr.steps[s];
        if (st.note < 0) continue;
        Note n;
        n.pitch = uint8_t(st.note);
        n.vel = 96;
        int gate = -1, micro = 0;
        for (const ot::StepFx& f : st.fx) {
            switch (f.type) {
                case kFxVolume: n.vel = uint8_t(f.value >= 100 ? 127 : 20 + f.value * 107 / 100); break;
                case kFxGate: gate = f.value; break;
                case kFxGlide: n.flags |= kFlagSlide; break;
                case kFxReverse: n.flags |= kFlagReverse; break;
                case kFxMicroMove: micro = f.value; break;
                case kFxRoll: {
                    const int idx = (f.value - 1) % 9;
                    n.roll = uint8_t(idx >= 0 && idx < 8 ? kRates[idx] : 0);
                    if (n.roll == 1) n.roll = 0;
                    break;
                }
                case kFxSliceId: n.pitch = uint8_t(36 + (f.value & 63)); break;
                case kFxChance: n.chance = f.value > 100 ? 100 : f.value; break;
                default: break;
            }
        }
        if (sliceNotes && st.note >= 24 && st.note < 24 + 64) n.pitch = uint8_t(36 + st.note - 24);
        n.start = uint16_t(s * kTps + micro * kTps / 100);
        // length: the gate, else until the next note / Off on this track
        int next = steps;
        for (int k = s + 1; k < steps; ++k)
            if (tr.steps[k].note >= 0 || tr.steps[k].note == ot::kNoteOff || tr.steps[k].note == ot::kNoteCut) {
                next = k;
                break;
            }
        int len = (next - s) * kTps;
        if (gate >= 0) {
            int g = gate * kTps / 100;
            len = g < 2 ? 2 : g;
        } else if (!(n.flags & kFlagSlide) && next < steps && tr.steps[next].note >= 0) {
            // a plain note into the next one: leave a tiny gap (retrigger)
            len -= 1;
        }
        if (len < 1) len = 1;
        n.len = uint16_t(len);
        if (n.flags & kFlagSlide) n.len = uint16_t(len + 2);   // overlap: the engine glides
        out.add(n);
    }
    // A slide flag marks the note that is slid INTO (Glide FX); the previous
    // note must overlap it, which len + 2 above does for the slider. Move the
    // overlap to the note before a slid-into note.
    for (int i = 0; i + 1 < out.count; ++i)
        if ((out.n[i + 1].flags & kFlagSlide) && out.n[i].start + out.n[i].len <= out.n[i + 1].start)
            out.n[i].len = uint16_t(out.n[i + 1].start - out.n[i].start + 2);
}

void Konsol::notesToPattern(const NoteBuf& in, int steps, ot::Pattern& p, int track, uint8_t instr) {
    using namespace ot::gen;
    ot::Track& tr = p.tracks[track];
    for (int s = 0; s < ot::kStepCount; ++s) tr.steps[s].clear();
    tr.lastStep = uint8_t(steps - 1);
    for (int i = 0; i < in.count; ++i) {
        const Note& n = in.n[i];
        const int s = n.start / kTps;
        if (s >= steps) continue;
        ot::Step& st = tr.steps[s];
        if (st.note >= 0) continue;   // one note per step (the lowest start wins)
        st.note = int8_t(n.pitch);
        st.instrument = instr;
        if (n.vel < 120) putFx(st, kFxVolume, uint8_t((n.vel < 20 ? 0 : (n.vel - 20) * 100 / 107)));
        if (n.len < kTps) putFx(st, kFxGate, uint8_t(n.len * 100 / kTps));
        if (n.flags & kFlagSlide) putFx(st, kFxGlide, 30);
    }
}

// ---------------------------------------------------------------------------
// native players
// ---------------------------------------------------------------------------
namespace {

struct NRng {
    uint32_t s;
    explicit NRng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return n <= 1 ? 0 : int(next() % uint32_t(n)); }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
};

void addNote(NoteBuf& b, int step, int pitch, int vel, int lenTicks, int flags = 0) {
    Note n;
    n.start = uint16_t(step * kTps);
    n.len = uint16_t(lenTicks < 1 ? 1 : lenTicks);
    n.pitch = uint8_t(pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch));
    n.vel = uint8_t(vel < 1 ? 1 : (vel > 127 ? 127 : vel));
    n.flags = uint8_t(flags);
    b.add(n);
}

int degNote(int root, const uint8_t* deg, int nd, int d) { return ot::gen::degreeNote(root, deg, nd, d); }

void genEuclid(const int16_t* v, int steps, int root, uint16_t mask, bool drum, NRng& r, NoteBuf& out) {
    uint8_t deg[12];
    const int nd = ot::gen::scaleDegrees(mask, deg);
    const int hits = v[0];
    for (int s = 0; s < steps; ++s) {
        if (!dsp::euclid(s, steps, hits > steps ? steps : hits, v[1])) continue;
        int pitch = root + v[2];
        if (!drum && v[3] > 0) pitch = degNote(root + v[2], deg, nd, r.below(v[3] + 1));
        const bool acc = r.below(100) < v[4] || (s % 4) == 0;
        addNote(out, s, pitch, acc ? 120 : 82, drum ? kTps / 2 : kTps - 2);
    }
}

void genUsul(const int16_t* v, int steps, int root, int engine, NRng& r, NoteBuf& out) {
    const char* u = kUsul[v[0] < 0 ? 0 : (v[0] > 6 ? 6 : v[0])];
    const int units = int(std::strlen(u));
    int dum = v[2], tek = v[3], ke;
    if (engine == kEngDrums) {
        if (!dum) dum = 36;
        if (!tek) tek = 37;
        ke = 39;
    } else if (engine == kEngPerc) {
        if (!dum) dum = 60;
        if (!tek) tek = 62;
        ke = 64;
    } else {
        if (!dum) dum = root;
        if (!tek) tek = root + 7;
        ke = root + 12;
    }
    for (int s = 0; s < steps; s += 2) {
        const char c = u[(s / 2) % units];
        if (c == 'D') addNote(out, s, dum, 120, kTps * 2 - 2);
        else if (c == 'T') addNote(out, s, tek, 100, kTps - 2);
        else if (c == 'k') addNote(out, s, ke, 70, kTps - 2);
        // fills: ghost 16ths between the strokes
        if (c == '.' || r.below(100) < v[4] / 3) {
            if (r.below(100) < v[4]) addNote(out, s + 1 < steps ? s + 1 : s, ke, 45 + r.below(20), kTps / 2);
        }
    }
}

void genPinball(const int16_t* v, int steps, int root, uint16_t mask, bool drum, NRng& r, NoteBuf& out) {
    uint8_t deg[12];
    const int nd = ot::gen::scaleDegrees(mask, deg);
    const int top = nd * v[3];
    float y = float(top) * 0.5f, vy = 0.0f;
    const float g = float(v[1]) * 0.012f;
    // bumpers: steps that kick the ball
    bool bump[kMaxSteps] = {};
    for (int s = 0; s < steps; ++s) bump[s] = r.below(100) < v[2] / 2 || (s % 8) == 0;
    for (int s = 0; s < steps; ++s) {
        vy -= g;
        y += vy;
        bool hit = false;
        if (y < 0) {
            y = -y;
            vy = -vy * 0.85f;
            hit = true;
        }
        if (y > float(top)) {
            y = 2.0f * float(top) - y;
            vy = -vy * 0.85f;
            hit = true;
        }
        if (bump[s]) {
            vy += (r.uni() * 2.0f - 0.6f) * (0.5f + float(v[0]) * 0.03f);
            hit = true;
        }
        if (hit || r.below(100) < v[0] / 3) {
            const int d = int(y + 0.5f);
            const int pitch = drum ? root + (d % 8) : degNote(root, deg, nd, d);
            addNote(out, s, pitch, hit ? 112 : 80, v[4] * kTps / 100);
        }
    }
}

void genMarkov(const int16_t* v, int steps, int root, uint16_t mask, bool drum, NRng& r, NoteBuf& out) {
    uint8_t deg[12];
    const int nd = ot::gen::scaleDegrees(mask, deg);
    const int top = nd * v[3];
    int d = 0;
    for (int s = 0; s < steps; ++s) {
        // on the beat more likely; density elsewhere
        const int dens = (s % 4) == 0 ? v[0] + 25 : v[0] - ((s % 2) ? 20 : 0);
        if (r.below(100) >= dens) continue;
        const int roll = r.below(100);
        if (roll < v[1]) d += r.below(2) ? 1 : -1;                         // stepwise
        else if (roll < v[1] + v[2]) d += (r.below(2) ? 1 : -1) * (2 + r.below(4));   // leap
        else if (r.below(3) == 0) d = r.below(2) ? 0 : nd;                 // back home / octave
        if (d < 0) d = -d;
        if (d > top) d = 2 * top - d;
        const int pitch = drum ? root + (d % 8) : degNote(root, deg, nd, d);
        // phrase endings resolve to the root
        const bool end = s == steps - 2 || (s % 16) == 14;
        addNote(out, s, end && !drum ? root : pitch, (s % 4) == 0 ? 110 : 88, v[4] * kTps * ((s % 8) == 0 ? 2 : 1) / 100);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// run a clip's generator
// ---------------------------------------------------------------------------
bool Konsol::runGenerator(int t, int s, uint32_t seed, char* msg, int cap) {
    Clip& c = P_->clip[t][s];
    if (c.gen == kGenNotes) {
        if (msg) std::snprintf(msg, size_t(cap), "Pick a generator first");
        return false;
    }
    if (!c.exists()) c.lenSteps = 16;
    const int engine = P_->track[t].engine;
    const bool drum = drumLike(engine);
    int root = rootNote(t);
    if (engine == kEngDrums) root = 36;
    if (engine == kEngPerc) root = 60;
    const uint16_t mask = scaleMask(t);
    if (c.gen == kGenUsul) c.lenSteps = uint8_t(usulSteps(c.gp[0], c.gp[1]));
    const int steps = c.lenSteps;
    if (!seed) seed = ctx_ ? (ctx_->millis() * 2654435761u) ^ 0x5bd1e995u : 12345u;
    // (main loop: the edit copy is built here, then published)
    const bool live = c.live && rt_[t].play == s;
    NoteBuf* out;
    NoteBuf tmp;
    if (live) {
        if (c.pending) return true;   // the next loop is ready already
        out = &c.buf[c.cur ^ 1];
    } else {
        out = &tmp;
    }
    out->clear();
    bool ok = true;
    if (c.gen >= kGenAcid && c.gen <= kGenStation) {
        const int k = c.gen - kGenAcid;
        ot::Pattern& p = *scratch_;
        p.setDefault();
        for (auto& tr : p.tracks) tr.lastStep = uint8_t(steps - 1);
        // Genotype / Turnaround vary existing material: the clip's notes (live:
        // the notes learnt when Live was switched on).
        if (k == ot::gen::kGenotype || k == ot::gen::kTurnaround) {
            // live: learn the clip once (when it starts live), then vary that
            if (live && srcScene_[t] != s) {
                std::memcpy(&src_[t], &c.notes(), sizeof(NoteBuf));
                srcScene_[t] = int8_t(s);
            }
            const NoteBuf& srcNotes = live ? src_[t] : c.notes();
            notesToPattern(srcNotes, steps, p, 0, 0);
        }
        ot::gen::Target tg;
        tg.pat = &p;
        tg.t0 = tg.t1 = 0;
        if (k == ot::gen::kCallResponse && c.gp[3] == 2) tg.t1 = 1;
        tg.s0 = 0;
        tg.s1 = steps - 1;
        tg.instrument = 0;
        tg.rootNote = root;
        tg.scaleMask = drum ? 0x0FFF : mask;
        tg.stepsPerBeat = 4;
        tg.entropy = seed | 1u;
        uint16_t sl[64];
        int nsl = 16;
        if (engine == kEngSampler) {
            const TrackSample& tsm = ts_[t];
            const bool trans = this->engine(t)->get(3) > 0.5f && pool_.valid(tsm.slot) && pool_.slot(tsm.slot).nOn > 1;
            nsl = trans ? pool_.slot(tsm.slot).nOn : int(this->engine(t)->get(4) + 0.5f);
            if (nsl > 64) nsl = 64;
        }
        if (engine == kEngKit) nsl = 16;
        if (engine == kEngDrums) nsl = 8;
        if (nsl < 2) nsl = 2;
        for (int i = 0; i < nsl; ++i) sl[i] = uint16_t(uint32_t(i) * 65536u / uint32_t(nsl));
        tg.slices.count = nsl;
        tg.slices.start = sl;
        tg.slices.beatSlice = true;
        float beats = 4.0f;
        if (engine == kEngSampler || engine == kEngGrain) {
            const uint32_t len = pool_.valid(ts_[t].slot) ? pool_.slot(ts_[t].slot).frames : 0;
            const float sel = this->engine(t)->get(2) - this->engine(t)->get(1);
            if (len) beats = float(len) / 44100.0f * (bpm_ > 20.0f ? bpm_ : 120.0f) / 60.0f * (sel > 0.05f ? sel : 1.0f);
        } else {
            beats = float(steps) / 4.0f;
        }
        tg.slices.sampleBeats = beats;
        tg.freq10 = uint16_t(ctx_ && ctx_->radio ? ctx_->radio->frequency() * 10.0f : 1000.0f);
        tg.text = P_->name;   // the HAL radio has no RDS text: Station reads the project name
        int16_t v[kGenParams];
        for (int i = 0; i < kGenParams; ++i) v[i] = c.gp[i];
        if (live && v[5] > 0) v[5] = int16_t(1 + (v[5] + int(liveCount_[t])) % 999);   // a live seed walks
        if (k == ot::gen::kAcid && live && acidScene_[t] != s) {   // phrase of another clip: rebuild
            ot::gen::MonoStep* ph = static_cast<ot::gen::MonoStep*>(acidPh_) + t * kMaxSteps;
            ot::gen::Rng rng(c.gp[5] > 0 ? uint32_t(c.gp[5]) * 2654435761u : (seed | 1u));
            ot::gen::acidPhrase(v, tg, rng, ph, steps);
            acidScene_[t] = int8_t(s);
        }
        if (k == ot::gen::kAcid && live) {
            // Acid live: mutate the kept phrase (TB-3PO Mutate)
            ot::gen::MonoStep* ph = static_cast<ot::gen::MonoStep*>(acidPh_) + t * kMaxSteps;
            ot::gen::Rng rng(seed | 1u);
            const float amt = float(v[4] > 0 ? v[4] : 15) / 100.0f;
            ot::gen::acidMutate(v, tg, rng, ph, steps, amt);
            ot::gen::writeMono(p, 0, 0, ph, steps, 0);
            if (msg) std::snprintf(msg, size_t(cap), "Acid: mutated %d%%", int(amt * 100.0f));
        } else {
            ot::gen::Result res = ot::gen::generate(k, v, tg);
            ok = res.ok;
            if (msg) std::snprintf(msg, size_t(cap), "%s", res.msg);
            if (k == ot::gen::kAcid) {
                // keep the phrase for live mutation
                ot::gen::MonoStep* ph = static_cast<ot::gen::MonoStep*>(acidPh_) + t * kMaxSteps;
                ot::gen::Rng rng(res.rngSeed);
                ot::gen::acidPhrase(v, tg, rng, ph, steps);
                acidScene_[t] = int8_t(s);
            }
        }
        if (!ok) return false;
        // slice hits (Beat Slice notes 24 + n) only from BBCut and Station's Slices mode
        const bool slices = k == ot::gen::kBBCut || (k == ot::gen::kStation && v[1] == 1);
        patternToNotes(p, 0, steps, *out, slices);
        if (tg.t1 == 1) {   // Call & Response answer on a second track: merge
            NoteBuf ans;
            patternToNotes(p, 1, steps, ans, false);
            for (int i = 0; i < ans.count; ++i) out->add(ans.n[i]);
        }
    } else {
        // Seed parameter > 0: the same notes every time (live: the seed walks)
        uint32_t ns = seed;
        if (c.gp[kGenParams - 1] > 0)
            ns = (uint32_t(c.gp[kGenParams - 1]) + (live ? liveCount_[t] : 0u)) * 2654435761u;
        NRng r(ns);
        switch (c.gen) {
            case kGenEuclid: genEuclid(c.gp, steps, root, mask, drum, r, *out); break;
            case kGenUsul: genUsul(c.gp, steps, root, engine, r, *out); break;
            case kGenPinball: genPinball(c.gp, steps, root, mask, drum, r, *out); break;
            case kGenMarkov: genMarkov(c.gp, steps, root, mask, drum, r, *out); break;
            default: break;
        }
        if (msg) std::snprintf(msg, size_t(cap), "%s: %d notes in %d steps", genName(c.gen), out->count, steps);
    }
    if (live) {
        c.pending = 1;   // the audio swaps it in at the loop point
    } else {
        NoteBuf& b = c.beginEdit();
        std::memcpy(&b, &tmp, sizeof(NoteBuf));
        c.commit();
    }
    dirty_ = true;
    return true;
}

void Konsol::liveStep(int t) {
    const int s = rt_[t].play;
    if (s < 0) return;
    Clip& c = P_->clip[t][s];
    if (!c.live || c.gen == kGenNotes) return;
    ++liveCount_[t];
    runGenerator(t, s, (ctx_ ? ctx_->millis() : 1u) * 747796405u + liveCount_[t] * 2891336453u + uint32_t(t));
}

}  // namespace konsol
}  // namespace lab
