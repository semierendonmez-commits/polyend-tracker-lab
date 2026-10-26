// GRAF -- control modules: clock, divider, step sequencer, euclidean, Turing
// machine, Markov, Pinball, sample & hold, quantizer, LFO, envelopes,
// function generator, slew, logic, CV mixer.
#include "ws/graf_modules.h"

namespace graf {

void midiOutNote(Engine* e, int ch1, uint8_t note, uint8_t vel, bool on) {
    if (ch1 <= 0 || !e) return;
    Engine::MidiOutEv ev;
    ev.ch = uint8_t(ch1 - 1);
    ev.note = note;
    ev.vel = vel;
    ev.on = on;
    e->midiQ.push(ev);
}

namespace {
inline const float* clkIn(const Proc& p, int i) { return p.has(i) ? p.in[i] : nullptr; }
inline float pitchOf(int semis) { return float(semis) * (1.0f / 12.0f); }
inline uint8_t midiNoteOf(float pitch) {
    int n = 60 + int(floorf(pitch * 12.0f + 0.5f));
    return uint8_t(n < 0 ? 0 : (n > 127 ? 127 : n));
}
// A gate + note-off tracker for sequencers with MIDI out.
struct GateOut {
    uint32_t left;
    uint8_t note;
    bool on;
    void start(Engine* e, int ch, float pitch, uint32_t len, uint8_t vel = 100) {
        if (on) midiOutNote(e, ch, note, 0, false);
        left = len;
        note = midiNoteOf(pitch);
        on = true;
        midiOutNote(e, ch, note, vel, true);
    }
    float step(Engine* e, int ch) {
        if (left > 0) {
            --left;
            return 1.0f;
        }
        if (on) {
            on = false;
            midiOutNote(e, ch, note, 0, false);
        }
        return 0.0f;
    }
};
}  // namespace

void rClocked(Module& m) { reinterpret_cast<TickIn*>(m.st.bytes)->reset(); }

// ---------------------------------------------------------------- CLK
struct ClkS {
    TickIn t;   // unused: CLK computes its own phase
    int32_t last, lastBeat, lastBar;
    float own;  // free-running beat when Run = Always and stopped
    Pulse pc, pb, pr, pbar;
    uint32_t glen;
};
void pClk(Module& m, Proc& p) {
    auto& s = m.s<ClkS>();
    const float rate = kRateBeats[int(m.v[0]) & 7];
    const float inv = 1.0f / rate;
    const float sw = m.v[1];
    const bool always = m.v[3] > 0.5f;
    const bool run = p.playing || always;
    if (p.started) {
        s.last = s.lastBeat = s.lastBar = -1;
        s.pr.fire(220);
    }
    const float bps = p.bps > 1e-9f ? p.bps : 1e-9f;
    const int32_t glen = int32_t(gateSamples(uint32_t(rate / bps), m.v[2]));
    const int32_t blen = int32_t(gateSamples(uint32_t(1.0f / bps), 0.25f));
    float pos = p.playing ? p.beat : s.own;
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (run) {
            const float q = pos * inv;
            const int32_t pair = int32_t(q * 0.5f);
            const int32_t id = pair * 2 + (q - float(pair * 2) >= 1.0f + sw ? 1 : 0);
            if (id != s.last) {
                s.last = id;
                s.pc.fire(glen);
            }
            const int32_t b = int32_t(pos);
            if (b != s.lastBeat) {
                s.lastBeat = b;
                s.pb.fire(blen);
                if ((b & 3) == 0 || s.lastBar < 0) {
                    s.lastBar = b;
                    s.pbar.fire(4000);
                }
            }
            pos += bps;
        }
        o0[i] = s.pc.step();
        o1[i] = s.pb.step();
        o2[i] = s.pbar.step();
        o3[i] = s.pr.step();
    }
    if (!p.playing && always) {
        s.own = pos >= 1024.0f ? pos - 1024.0f : pos;
    }
}

// ---------------------------------------------------------------- DIV
struct DivS {
    TickIn t;
    uint32_t count;
    Schmitt rst;
    Pulse p2, p4, pn, pm;
};
void pDiv(Module& m, Proc& p) {
    auto& s = m.s<DivS>();
    const uint32_t n = uint32_t(m.v[0]), mm = uint32_t(m.v[1]);
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    if (p.started) s.count = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) s.count = 0;
        if (s.t.tick(p, clkIn(p, 0), i, 4.0f)) {
            const uint32_t c = s.count++;
            const int32_t len = int32_t(gateSamples(s.t.period, 0.5f));
            if (c % 2 == 0) s.p2.fire(len * 2);
            if (c % 4 == 0) s.p4.fire(len * 4);
            if (n && c % n == 0) s.pn.fire(len * int32_t(n));
            if (mm && c % mm == 0) s.pm.fire(len * int32_t(mm));
        }
        o0[i] = s.p2.step();
        o1[i] = s.p4.step();
        o2[i] = s.pn.step();
        o3[i] = s.pm.step();
    }
}

// ---------------------------------------------------------------- SEQ
// ext: notes[16] (scale degrees, int8), gates (2 bytes), cv[16] (0..255).
struct SeqS {
    TickIn t;
    int32_t pos;
    int32_t dir;
    float pitch, target;
    GateOut g;
    Pulse eoc;
    Schmitt rst;
    float cv;
};
void seqDefault(Module& m) {
    static const int8_t notes[16] = {0, 0, 4, 2, 0, 7, 4, 2, 0, 5, 4, 2, 7, 4, 9, 2};
    for (int i = 0; i < 16; ++i) {
        seqNote(m, i) = notes[i];
        seqSetGate(m, i, (i % 8) != 3 && i != 13);
        seqCv(m, i) = uint8_t((i * 53) & 255);
    }
}
void rSeq(Module& m) {
    auto& s = m.s<SeqS>();
    s.t.reset();
    s.pos = -1;
    s.dir = 1;
    seqDefault(m);
}
int seqPosition(const Module& m) { return reinterpret_cast<const SeqS*>(m.st.bytes)->pos; }
void pSeq(Module& m, Proc& p) {
    auto& s = m.s<SeqS>();
    const int steps = int(m.v[0]);
    const float rate = kRateBeats[int(m.v[1]) & 7];
    const float invRate = 1.0f / rate;
    const int sc = int(m.v[3]), root = int(m.v[4]);
    const float oct = m.v[5];
    const int dir = int(m.v[6]);
    const float glide = coefSec(m.v[7]);
    const int ch = int(m.v[8]);
    if (p.started) s.pos = -1;
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) s.pos = -1;
        if (s.t.tick(p, clkIn(p, 0), i, invRate)) {
            int np = s.pos;
            switch (dir) {
                case 0: np = s.pos + 1; break;
                case 1: np = s.pos <= 0 ? steps - 1 : s.pos - 1; break;
                case 2:
                    if (s.dir == 0) s.dir = 1;
                    np = s.pos + s.dir;
                    if (np >= steps) {
                        s.dir = -1;
                        np = steps > 1 ? steps - 2 : 0;
                    } else if (np < 0) {
                        s.dir = 1;
                        np = steps > 1 ? 1 : 0;
                    }
                    break;
                default: np = p.eng->rng.range(steps); break;
            }
            if (np >= steps || np < 0) {
                np = 0;
            }
            if (dir == 0 && np == 0 && s.pos >= 0) s.eoc.fire(220);
            s.pos = np;
            s.target = pitchOf(scaleDegree(sc, seqNote(m, np)) + root) + oct;
            s.cv = float(seqCv(m, np)) * (1.0f / 255.0f);
            if (seqGate(m, np)) {
                s.g.start(p.eng, ch, s.target, gateSamples(s.t.period, m.v[2]));
            }
            if (m.v[7] < 0.001f) s.pitch = s.target;
        }
        s.pitch += (s.target - s.pitch) * glide;
        o0[i] = s.pitch;
        o1[i] = s.g.step(p.eng, ch);
        o2[i] = s.cv;
        o3[i] = s.eoc.step();
    }
}

// ---------------------------------------------------------------- EUCL
struct EuclS {
    TickIn t;
    int32_t pos;
    Schmitt rst;
    Pulse g, inv, acc;
};
int euclStep(const Module& m) { return reinterpret_cast<const EuclS*>(m.st.bytes)->pos; }
void pEucl(Module& m, Proc& p) {
    auto& s = m.s<EuclS>();
    const int steps = int(m.v[0]);
    int hits = int(m.v[1]);
    if (hits > steps) hits = steps;
    const int rot = int(m.v[2]);
    const float rate = kRateBeats[int(m.v[3]) & 7];
    const float invRate = 1.0f / rate;
    if (p.started) s.pos = -1;
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) s.pos = -1;
        if (s.t.tick(p, clkIn(p, 0), i, invRate)) {
            s.pos = (s.pos + 1) % (steps > 0 ? steps : 1);
            const int32_t len = int32_t(gateSamples(s.t.period, m.v[4]));
            const bool hit = lab::dsp::euclid(s.pos, steps, hits, rot);
            if (hit) {
                s.g.fire(len);
                // accent on the first hit of the cycle
                bool first = true;
                for (int k = 0; k < s.pos; ++k)
                    if (lab::dsp::euclid(k, steps, hits, rot)) {
                        first = false;
                        break;
                    }
                if (first) s.acc.fire(len);
            } else {
                s.inv.fire(len);
            }
        }
        o0[i] = s.g.step();
        o1[i] = s.inv.step();
        o2[i] = s.acc.step();
    }
}

// ---------------------------------------------------------------- TURG
struct TurS {
    TickIn t;
    uint16_t reg;
    float cv, pitch;
    GateOut g;
    Pulse pulse;
};
void rTuring(Module& m) {
    auto& s = m.s<TurS>();
    s.t.reset();
    s.reg = uint16_t(0x9A5Du ^ (m.cell * 977u));
}
uint32_t turingRegister(const Module& m) { return reinterpret_cast<const TurS*>(m.st.bytes)->reg; }
void pTuring(Module& m, Proc& p) {
    auto& s = m.s<TurS>();
    const int len = int(m.v[1]);
    const int sc = int(m.v[2]), root = int(m.v[4]);
    const float range = m.v[3];
    const float rate = kRateBeats[int(m.v[5]) & 7];
    const float invRate = 1.0f / rate;
    const int ch = int(m.v[7]);
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.t.tick(p, clkIn(p, 0), i, invRate)) {
            const float prob = clampf(m.v[0] + p.in[1][i], 0.0f, 1.0f);
            uint16_t bit = (s.reg >> (len - 1)) & 1;
            if (p.eng->rng.uni() < prob) bit ^= 1;
            s.reg = uint16_t((s.reg << 1) | bit);
            s.cv = float(s.reg & 0xFF) * (1.0f / 255.0f);
            const float semi = quantizeSemi(s.cv * range * 12.0f, sc);
            s.pitch = (semi + float(root)) * (1.0f / 12.0f);
            const int32_t gl = int32_t(gateSamples(s.t.period, m.v[6]));
            if (s.reg & 1) s.g.start(p.eng, ch, s.pitch, uint32_t(gl));
            s.pulse.fire(220);
        }
        o0[i] = s.pitch;
        o1[i] = s.cv;
        o2[i] = s.g.step(p.eng, ch);
        o3[i] = s.pulse.step();
    }
}

// ---------------------------------------------------------------- MRKV
// ext[0..63]: 8x8 transition weights (0..255).
struct MrkS {
    TickIn t;
    int32_t state;
    int32_t seed;
    float pitch;
    GateOut g;
    Schmitt rst;
};
void markovSeed(Module& m, int seed) {
    Rand r;
    r.seed(uint32_t(seed) * 2654435761u + 17u);
    for (int a = 0; a < 8; ++a)
        for (int b = 0; b < 8; ++b) {
            int w = int(r.uni() * r.uni() * 255.0f);
            const int d = a > b ? a - b : b - a;
            if (d == 1) w += 90;   // melodic steps are likelier
            if (d == 0) w /= 3;
            m.ext[a * 8 + b] = uint8_t(w > 255 ? 255 : w);
        }
}
void rMarkov(Module& m) {
    auto& s = m.s<MrkS>();
    s.t.reset();
    s.seed = -1;
}
int markovState(const Module& m) { return reinterpret_cast<const MrkS*>(m.st.bytes)->state; }
void pMarkov(Module& m, Proc& p) {
    auto& s = m.s<MrkS>();
    const int n = int(m.v[0]);
    const float ent = m.v[1];
    const int sc = int(m.v[2]), root = int(m.v[3]);
    const float oct = m.v[4];
    const float rate = kRateBeats[int(m.v[5]) & 7];
    const float invRate = 1.0f / rate;
    const float rest = m.v[6];
    const int ch = int(m.v[8]);
    if (int(m.v[7]) != s.seed) {   // seed changed: new matrix (cheap, rare)
        s.seed = int(m.v[7]);
        markovSeed(m, s.seed);
    }
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) s.state = 0;
        if (s.t.tick(p, clkIn(p, 0), i, invRate)) {
            const int a = s.state < n ? s.state : 0;
            float tot = 0.0f, w[8];
            for (int b = 0; b < n; ++b) {
                w[b] = float(m.ext[a * 8 + b]) * (1.0f - ent) + 128.0f * ent + 1.0f;
                tot += w[b];
            }
            float r = p.eng->rng.uni() * tot;
            int nb = n - 1;
            for (int b = 0; b < n; ++b) {
                r -= w[b];
                if (r <= 0.0f) {
                    nb = b;
                    break;
                }
            }
            s.state = nb;
            s.pitch = pitchOf(scaleDegree(sc, nb) + root) + oct;
            if (p.eng->rng.uni() >= rest) s.g.start(p.eng, ch, s.pitch, gateSamples(s.t.period, 0.6f));
        }
        o0[i] = s.pitch;
        o1[i] = s.g.step(p.eng, ch);
        o2[i] = float(s.state) / float(n > 1 ? n - 1 : 1);
    }
}

// ---------------------------------------------------------------- PINB
// ext[0..47]: table cells (bit 0: note cell, bit 1: bumper). Balls move one
// cell per clock: walls reflect, bumpers send the ball off in a direction
// picked by Chaos, Gravity pulls it down. A note cell fires a gate with the
// cell's pitch (column = scale degree, row = octave).
struct Ball {
    int8_t x, y, dx, dy;
};
struct PinS {
    TickIn t;
    Ball b[3];
    float pitch, x, y;
    GateOut g;
    Schmitt rst;
    int8_t nb;
};
void pinbLayout(Module& m, uint32_t seed) {
    Rand r;
    r.seed(seed * 7919u + 3u);
    for (int c = 0; c < kCells; ++c) {
        uint8_t v = 0;
        if (r.uni() < 0.45f) v |= 1;
        if (r.uni() < 0.14f) v |= 2;
        m.ext[c] = v;
    }
}
static void pinbStart(PinS& s) {
    for (int i = 0; i < 3; ++i) {
        s.b[i].x = int8_t(1 + i * 4);
        s.b[i].y = int8_t(i & 1 ? 3 : 0);
        s.b[i].dx = int8_t(i & 1 ? -1 : 1);
        s.b[i].dy = 1;
    }
}
void rPinb(Module& m) {
    auto& s = m.s<PinS>();
    s.t.reset();
    pinbStart(s);
    pinbLayout(m, m.cell + 1u);
}
int pinbBall(const Module& m, int i) {
    const PinS& s = *reinterpret_cast<const PinS*>(m.st.bytes);
    if (i < 0 || i >= int(m.v[6])) return -1;
    return s.b[i].y * 12 + s.b[i].x;
}
void pPinb(Module& m, Proc& p) {
    auto& s = m.s<PinS>();
    const float rate = kRateBeats[int(m.v[0]) & 7];
    const float invRate = 1.0f / rate;
    const float chaos = m.v[1], grav = m.v[2];
    const int sc = int(m.v[3]), root = int(m.v[4]);
    const int balls = int(m.v[6]);
    const int ch = int(m.v[7]);
    if (p.started) pinbStart(s);
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    Rand& r = p.eng->rng;
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) pinbStart(s);
        if (s.t.tick(p, clkIn(p, 0), i, invRate)) {
            bool hit = false;
            for (int k = 0; k < balls && k < 3; ++k) {
                Ball& b = s.b[k];
                if (r.uni() < grav * 0.5f) b.dy = 1;
                int nx = b.x + b.dx, ny = b.y + b.dy;
                if (nx < 0 || nx > 11) {
                    b.dx = int8_t(-b.dx);
                    nx = b.x + b.dx;
                }
                if (ny < 0 || ny > 3) {
                    b.dy = int8_t(-b.dy);
                    ny = b.y + b.dy;
                    if (r.uni() < chaos) b.dx = int8_t(r.range(3) - 1);
                }
                b.x = int8_t(nx < 0 ? 0 : (nx > 11 ? 11 : nx));
                b.y = int8_t(ny < 0 ? 0 : (ny > 3 ? 3 : ny));
                const uint8_t cell = m.ext[b.y * 12 + b.x];
                if (cell & 2) {   // bumper: Markov-ish deflection
                    b.dx = int8_t(r.uni() < 0.5f + chaos * 0.5f ? r.range(3) - 1 : -b.dx);
                    b.dy = int8_t(r.uni() < chaos ? r.range(3) - 1 : -b.dy);
                    if (b.dx == 0 && b.dy == 0) b.dx = 1;
                }
                if ((cell & 1) && !hit) {
                    hit = true;
                    s.pitch = pitchOf(scaleDegree(sc, b.x) + root) + float(1 - b.y);
                    s.x = float(b.x) / 11.0f;
                    s.y = float(3 - b.y) / 3.0f;
                    s.g.start(p.eng, ch, s.pitch, gateSamples(s.t.period, m.v[5]));
                }
            }
        }
        o0[i] = s.pitch;
        o1[i] = s.g.step(p.eng, ch);
        o2[i] = s.x;
        o3[i] = s.y;
    }
}

// ---------------------------------------------------------------- S&H
struct SnhS {
    Schmitt t;
    float sh, th, rnd, osh, oth, ornd;
};
void pSnh(Module& m, Proc& p) {
    auto& s = m.s<SnhS>();
    const float sl = coefSec(m.v[0]);
    const float *in = p.in[0], *tr = p.in[1];
    const bool hasIn = p.has(0);
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = hasIn ? in[i] : p.eng->rng.bi();
        if (s.t.rise(tr[i])) {
            s.sh = x;
            s.rnd = p.eng->rng.bi();
        }
        if (tr[i] > 0.25f) s.th = x;
        s.osh += (s.sh - s.osh) * sl;
        s.oth += (s.th - s.oth) * sl;
        s.ornd += (s.rnd - s.ornd) * sl;
        o0[i] = s.osh;
        o1[i] = s.oth;
        o2[i] = s.ornd;
    }
}

// ---------------------------------------------------------------- QNT
struct QntS {
    float lastIn, out;
    Schmitt t;
    int32_t pulse;
    bool init;
};
void pQuant(Module& m, Proc& p) {
    auto& s = m.s<QntS>();
    const int sc = int(m.v[0]), root = int(m.v[1]);
    const float oct = m.v[2];
    const bool hasTrig = p.has(1);
    const float *in = p.in[0], *tr = p.in[1], *tp = p.in[2];
    float *o0 = m.out[0], *o1 = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = in[i] + tp[i];
        const bool sample = hasTrig ? s.t.rise(tr[i]) : (x != s.lastIn || !s.init);
        if (sample) {
            s.init = true;
            s.lastIn = x;
            const float q = (quantizeSemi(x * 12.0f - float(root), sc) + float(root)) * (1.0f / 12.0f) + oct;
            if (q != s.out) s.pulse = 220;
            s.out = q;
        }
        o0[i] = s.out;
        o1[i] = s.pulse > 0 ? 1.0f : 0.0f;
        if (s.pulse > 0) --s.pulse;
    }
}

// ---------------------------------------------------------------- LFO
struct LfoS {
    float ph;
    Schmitt rst;
};
void pLfo(Module& m, Proc& p) {
    auto& s = m.s<LfoS>();
    static const float kSyncBeats[8] = {0, 16, 8, 4, 2, 1, 0.5f, 0.25f};
    const int sync = int(m.v[1]);
    const bool uni = m.v[2] > 0.5f;
    const float ofs = m.v[3], lvl = m.v[4];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    float inc;
    bool lock = false;
    if (sync > 0) {
        inc = p.bps / kSyncBeats[sync];
        lock = p.playing;
    } else {
        inc = m.v[0] * fexp2(p.in[0][0] * 4.0f) * kInvSr;
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (p.has(1) && s.rst.rise(p.in[1][i])) s.ph = 0.0f;
        float ph;
        if (lock) {   // phase from the transport: stays in time
            const float pos = (p.beat + float(i) * p.bps) / kSyncBeats[sync];
            ph = pos - float(int32_t(pos));
            s.ph = ph;
        } else {
            s.ph += inc;
            if (s.ph >= 1.0f) s.ph -= 1.0f;
            ph = s.ph;
        }
        ph += ofs;
        if (ph >= 1.0f) ph -= 1.0f;
        float a = sinP(ph), b = 4.0f * (ph < 0.5f ? ph : 1.0f - ph) - 1.0f, c = 2.0f * ph - 1.0f,
              d = ph < 0.5f ? 1.0f : -1.0f;
        if (uni) {
            a = a * 0.5f + 0.5f;
            b = b * 0.5f + 0.5f;
            c = c * 0.5f + 0.5f;
            d = d * 0.5f + 0.5f;
        }
        o0[i] = a * lvl;
        o1[i] = b * lvl;
        o2[i] = c * lvl;
        o3[i] = d * lvl;
    }
}

// ---------------------------------------------------------------- ADSR
struct AdsrS {
    float v;
    uint8_t st;   // 0 idle 1 attack 2 decay 3 release
    bool gate;
    Schmitt rt;
};
void pAdsr(Module& m, Proc& p) {
    auto& s = m.s<AdsrS>();
    const float ca = coefSec(m.v[0] * 0.6f), cd = coefSec(m.v[1] * 0.3f), sus = m.v[2],
                cr = coefSec(m.v[3] * 0.3f), lvl = m.v[4];
    const float *g = p.in[0], *rt = p.in[1];
    float *o0 = m.out[0], *o1 = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const bool on = g[i] > 0.4f;
        if (on && !s.gate) s.st = 1;
        if (!on && s.gate) s.st = 3;
        s.gate = on;
        if (p.has(1) && s.rt.rise(rt[i]) && on) s.st = 1;
        switch (s.st) {
            case 1:
                s.v += (1.25f - s.v) * ca;
                if (s.v >= 1.0f) {
                    s.v = 1.0f;
                    s.st = 2;
                }
                break;
            case 2: s.v += (sus - s.v) * cd; break;
            case 3:
                s.v += (0.0f - s.v) * cr;
                if (s.v < 1e-5f) {
                    s.v = 0.0f;
                    s.st = 0;
                }
                break;
            default: break;
        }
        o0[i] = s.v * lvl;
        o1[i] = (1.0f - s.v) * lvl;
    }
}

// ---------------------------------------------------------------- AD
struct AdS {
    float x;
    uint8_t st;
    Schmitt t;
    Pulse eoc;
};
void pAd(Module& m, Proc& p) {
    auto& s = m.s<AdS>();
    const float ia = kInvSr / m.v[0], id = kInvSr / m.v[1];
    const float cv = m.v[2];
    const bool loop = m.v[3] > 0.5f;
    float *o0 = m.out[0], *o1 = m.out[1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.t.rise(p.in[0][i])) s.st = 1;
        if (s.st == 0 && loop) s.st = 1;
        if (s.st == 1) {
            s.x += ia;
            if (s.x >= 1.0f) {
                s.x = 1.0f;
                s.st = 2;
            }
        } else if (s.st == 2) {
            s.x -= id;
            if (s.x <= 0.0f) {
                s.x = 0.0f;
                s.st = 0;
                s.eoc.fire(220);
            }
        }
        const float x = s.x;
        o0[i] = cv >= 0.0f ? x + cv * (x * x * x - x) : x + cv * (x - (2.0f * x - x * x));
        o1[i] = s.eoc.step();
    }
}

// ---------------------------------------------------------------- FUNC
struct FuncS {
    float y;
    uint8_t st;   // 0 follow, 1 rising (env), 2 falling (env)
    Schmitt t;
    Pulse eoc;
    float env;
};
void pFunc(Module& m, Proc& p) {
    auto& s = m.s<FuncS>();
    const float rise = m.v[0] * fexp2(p.in[2][0] * 4.0f), fall = m.v[1] * fexp2(p.in[3][0] * 4.0f);
    const float ir = kInvSr / rise, ifl = kInvSr / fall;
    const float cv = m.v[2];
    const bool cyc = m.v[3] > 0.5f;
    const bool hasIn = p.has(1);
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (s.t.rise(p.in[0][i]) || (cyc && s.st == 0)) s.st = 1;
        if (s.st == 1) {
            s.env += ir;
            if (s.env >= 1.0f) {
                s.env = 1.0f;
                s.st = 2;
            }
        } else if (s.st == 2) {
            s.env -= ifl;
            if (s.env <= 0.0f) {
                s.env = 0.0f;
                s.st = 0;
                s.eoc.fire(220);
            }
        }
        // slew the input with the same rise / fall rates (per unit)
        if (hasIn) {
            const float x = p.in[1][i];
            if (x > s.y) s.y = x < s.y + ir * 2.0f ? x : s.y + ir * 2.0f;
            else s.y = x > s.y - ifl * 2.0f ? x : s.y - ifl * 2.0f;
        } else {
            s.y = 0.0f;
        }
        const float e = s.env;
        const float shaped = cv >= 0.0f ? e + cv * (e * e - e) : e + cv * (e - (2.0f * e - e * e));
        const float out = s.y + shaped;
        o0[i] = out;
        o1[i] = -out;
        o2[i] = s.st == 2 || s.st == 0 ? 1.0f : 0.0f;
        o3[i] = s.eoc.step();
    }
}

// ---------------------------------------------------------------- SLEW
struct SlewS {
    float y;
};
void pSlew(Module& m, Proc& p) {
    auto& s = m.s<SlewS>();
    const bool ex = m.v[2] > 0.5f;
    const float ir = kInvSr / m.v[0] * 2.0f, ifl = kInvSr / m.v[1] * 2.0f;
    const float cr = coefSec(m.v[0] * 0.3f), cf = coefSec(m.v[1] * 0.3f);
    float* o = m.out[0];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = p.in[0][i];
        if (ex) {
            s.y += (x - s.y) * (x > s.y ? cr : cf);
        } else if (x > s.y) {
            s.y = x < s.y + ir ? x : s.y + ir;
        } else {
            s.y = x > s.y - ifl ? x : s.y - ifl;
        }
        o[i] = s.y;
    }
}

// ---------------------------------------------------------------- LOGC
void pLogic(Module& m, Proc& p) {
    const float th = m.v[0];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float a = p.in[0][i], b = p.in[1][i];
        const bool A = a > th, B = b > th;
        o0[i] = (A && B) ? 1.0f : 0.0f;
        o1[i] = (A || B) ? 1.0f : 0.0f;
        o2[i] = (A != B) ? 1.0f : 0.0f;
        o3[i] = a > b ? 1.0f : 0.0f;
    }
}

// ---------------------------------------------------------------- CVMX
void pCvmx(Module& m, Proc& p) {
    const float a = m.v[0], b = m.v[1], c = m.v[2], o = m.v[3];
    float *o0 = m.out[0], *o1 = m.out[1], *o2 = m.out[2], *o3 = m.out[3];
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = p.in[0][i] * a, y = p.in[1][i] * b, z = p.in[2][i] * c;
        const float sum = x + y + z + o;
        o0[i] = sum;
        o1[i] = -sum;
        o2[i] = fabsf(sum);
        float mx = x > y ? x : y;
        mx = mx > z ? mx : z;
        o3[i] = mx + o;
    }
}

}  // namespace graf
