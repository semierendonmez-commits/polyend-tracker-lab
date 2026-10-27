// GRAF -- demo patches, templates (patch blocks added to a canvas) and the
// CPU bench scenarios.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "ws/graf_ws.h"

namespace graf {

// ------------------------------------------------------------------ demos
namespace {
struct Builder {
    Engine& e;
    int patch;
    int add(int type, int cell, std::initializer_list<float> vals = {}) {
        const int s = e.add(type, patch, cell);
        if (s < 0) return -1;
        int p = 0;
        for (float v : vals) {
            if (p < def(type).nParam && v == v) e.setParamK(s, p, Engine::unmapParam(def(type).p[p], v));
            ++p;
        }
        return s;
    }
    void c(int a, int ao, int b, int bi, float amt = 1.0f) {
        if (a >= 0 && b >= 0) e.connect(a, ao, b, bi, amt);
    }
};
constexpr float kNaN = __builtin_nanf("");
const char* const kDemoNames[] = {"Demo: First light", "Demo: Reticulum", "Demo: West coast", "Demo: Turing drums",
                                  "Demo: Poly keys", "Demo: 8 patches",  "Demo: Resonators", "Demo: Acid net"};
constexpr int kDemos = 8;
}  // namespace

int Graf::demoCount() { return kDemos; }
const char* Graf::demoName(int i) { return kDemoNames[(i >= 0 && i < kDemos) ? i : 0]; }

void Graf::loadDemo(int i) {
    clearAllRoutes();
    eng_.clear();
    eng_.quiesce();
    eng_.allNotesOff();
    for (int k = 0; k < kSnaps; ++k) eng_.clearSnap(k);
    eng_.rack = i != 5;
    prefModel_ = eng_.rack ? 0 : 1;
    for (int p = 0; p < kPatches; ++p) eng_.mix[p] = MixStrip();
    Builder b{eng_, 0};
    int varyA = -1, varyB = -1;
    switch (i) {
        default:
        case 0: {   // SEQ -> VCO -> LADR -> VCA <- ADSR -> DLY -> OUT
            const int seq = b.add(kSeq, 0, {8, 4, 0.45f, 2, 0, -1});
            const int vco = b.add(kVco, 2, {0, 0, 0.2f, 0.5f});
            const int lfo = b.add(kLfo, 12, {0.2f});
            const int lad = b.add(kLadder, 3, {500, 0.55f, 4, 1.6f});
            const int env = b.add(kAdsr, 14, {0.002f, 0.18f, 0.2f, 0.25f});
            const int vca = b.add(kVca, 4);
            const int dly = b.add(kDelay, 5, {0.35f, 5, 0.45f, 0.6f, 0.3f});
            const int out = b.add(kOut, 6, {0.8f});
            const int mi = b.add(kMidi, 24);   // keys / MIDI transpose the sequence and retrigger
            b.c(seq, 0, vco, 0);
            b.c(seq, 1, env, 0);
            b.c(mi, 0, vco, 0);
            b.c(mi, 1, env, 0);
            b.c(vco, 0, lad, 0);
            b.c(vco, 1, lad, 0, 0.5f);
            b.c(env, 0, lad, 2, 0.6f);
            b.c(lfo, 0, lad, 2, 0.15f);
            b.c(lad, 0, vca, 0);
            b.c(env, 0, vca, 1);
            b.c(vca, 0, dly, 0);
            b.c(dly, 0, out, 0);
            b.c(dly, 1, out, 1);
            varyA = lad;
            varyB = vco;
            break;
        }
        case 1: {   // RETICULUM: rete + two random sources + a manual-gate clock into granular + reverb
            const int clk = b.add(kClk, 0, {0, 0, 0.1f});
            const int eucl = b.add(kEucl, 12, {16, 3, 0, 3});
            const int snh = b.add(kSnh, 24, {0.2f});
            const int lfo = b.add(kLfo, 25, {0.07f});
            const int rete = b.add(kRete, 2, {0.8f, 0.82f, 180, 0.6f, 0.25f, 1, 0.3f, 0.45f, 0.8f});
            const int noi = b.add(kNoise, 36, {0.2f, 0.4f});
            const int grn = b.add(kGrain, 4, {0.18f, 8, 0.3f, 0, 0.5f, 0.2f, 0, 0.45f, 0.8f});
            const int vrb = b.add(kVerb, 5, {0.8f, 0.45f, 0.02f, 0.4f});
            const int out = b.add(kOut, 6, {0.8f});
            b.c(clk, 2, eucl, 1);
            b.c(eucl, 0, rete, 0);
            b.c(eucl, 0, snh, 1);
            b.c(snh, 0, rete, 2, 0.4f);
            b.c(lfo, 0, rete, 3, 0.4f);
            b.c(noi, 3, rete, 1, 0.5f);
            b.c(rete, 0, grn, 0);
            b.c(rete, 1, grn, 1);
            b.c(grn, 0, vrb, 0);
            b.c(grn, 1, vrb, 1);
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyA = rete;
            varyB = grn;
            break;
        }
        case 2: {   // PINB -> CPLX -> LPG (pinged) -> VERB; FUNC cycling into timbre
            const int pin = b.add(kPinb, 0, {4, 0.3f, 0.2f, 5, 2});
            const int cpx = b.add(kCplx, 2, {-1, 0, 1.5f, 0.8f, 0.2f, 0.3f, 0.1f});
            const int fn = b.add(kFunc, 13, {0.8f, 1.6f, 0.2f, 1});
            const int lpg = b.add(kLpg, 3, {2, 0, 0.5f, 0.3f});
            const int vrb = b.add(kVerb, 4, {0.85f, 0.35f, 0.03f, 0.35f});
            const int out = b.add(kOut, 5, {0.85f});
            b.c(pin, 0, cpx, 0);
            b.c(pin, 1, lpg, 2);
            b.c(fn, 0, cpx, 2, 0.5f);
            b.c(fn, 0, cpx, 3, 0.3f);
            b.c(cpx, 0, lpg, 0);
            b.c(lpg, 0, vrb, 0);
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyA = cpx;
            varyB = pin;
            break;
        }
        case 3: {   // clock, three euclideans into drums, Turing -> wavetable -> SVF
            const int k1 = b.add(kEucl, 0, {16, 4, 0, 4});
            const int k2 = b.add(kEucl, 12, {16, 3, 4, 4});
            const int k3 = b.add(kEucl, 24, {16, 9, 1, 4, 0.2f});
            const int kick = b.add(kPerc, 1, {0, 0, 0.35f, 0.4f, 0.9f});
            const int snr = b.add(kPerc, 13, {1, 0, 0.2f, 0.5f, 0.6f});
            const int hat = b.add(kPerc, 25, {2, 0, 0.06f, 0.6f, 0.4f});
            const int tur = b.add(kTuring, 36, {0.1f, 8, 6, 1.5f, 0, 4});
            const int wt = b.add(kWtbl, 37, {-1, 0, 1, 0.3f});
            const int lfo = b.add(kLfo, 38, {0.13f});
            const int svf = b.add(kSvf, 39, {900, 0.5f, 3});
            const int ad = b.add(kAd, 40, {0.003f, 0.25f});
            const int vca = b.add(kVca, 41);
            const int mix = b.add(kMix, 3, {0.9f, 0.7f, 0.5f, 0.6f, 1.3f});
            const int out = b.add(kOut, 4, {0.8f});
            b.c(k1, 0, kick, 0);
            b.c(k2, 0, snr, 0);
            b.c(k3, 0, hat, 0);
            b.c(k3, 2, hat, 2);
            b.c(tur, 0, wt, 0);
            b.c(lfo, 1, wt, 1, 0.5f);
            b.c(wt, 0, svf, 0);
            b.c(tur, 2, ad, 0);
            b.c(ad, 0, svf, 2, 0.5f);
            b.c(svf, 0, vca, 0);
            b.c(ad, 0, vca, 1);
            b.c(kick, 0, mix, 0);
            b.c(snr, 0, mix, 1);
            b.c(hat, 0, mix, 2);
            b.c(vca, 0, mix, 3);
            b.c(mix, 0, out, 0);
            varyA = tur;
            varyB = svf;
            break;
        }
        case 4: {   // 4-voice poly from MIDI / the pads
            const int vrb = b.add(kVerb, 10, {0.75f, 0.5f, 0.01f, 0.3f});
            const int out = b.add(kOut, 11, {0.8f});
            for (int v = 0; v < 4; ++v) {
                const int mi = b.add(kMidi, v * 12, {float(v + 1)});
                const int vco = b.add(kVco, v * 12 + 1, {0, v * 0.04f - 0.06f, 0, 0.4f});
                const int svf = b.add(kSvf, v * 12 + 2, {700, 0.35f, 4});
                const int env = b.add(kAdsr, v * 12 + 3, {0.004f, 0.4f, 0.5f, 0.6f});
                const int vca = b.add(kVca, v * 12 + 4);
                b.c(mi, 0, vco, 0);
                b.c(mi, 1, env, 0);
                b.c(vco, 0, svf, 0);
                b.c(env, 0, svf, 2, 0.5f);
                b.c(svf, 0, vca, 0);
                b.c(env, 0, vca, 1, 0.6f);
                b.c(vca, 0, vrb, 0, 0.7f);
                if (v == 0) varyA = svf;
            }
            b.c(vrb, 0, out, 0);
            b.c(vrb, 1, out, 1);
            varyB = vrb;
            break;
        }
        case 5: {   // 8 patches: a bass on 1, a pluck on 2, rete on 3, drums on 4
            {
                b.patch = 0;
                const int seq = b.add(kSeq, 0, {8, 3, 0.4f, 6, 0, -2});
                const int vco = b.add(kVco, 1, {-1});
                const int svf = b.add(kSvf, 2, {300, 0.6f, 5, 2});
                const int env = b.add(kAdsr, 13, {0.002f, 0.2f, 0.1f, 0.1f});
                const int vca = b.add(kVca, 3);
                const int out = b.add(kOut, 4, {0.8f});
                b.c(seq, 0, vco, 0);
                b.c(seq, 1, env, 0);
                b.c(vco, 0, svf, 0);
                b.c(env, 0, svf, 2, 0.7f);
                b.c(svf, 0, vca, 0);
                b.c(env, 0, vca, 1);
                b.c(vca, 0, out, 0);
                varyA = svf;
            }
            {
                b.patch = 1;
                const int mrk = b.add(kMarkov, 0, {6, 0.2f, 5, 0, 0, 4});
                const int cpx = b.add(kCplx, 1, {0, 0, 2, 1.2f, 0.1f, 0.2f});
                const int lpg = b.add(kLpg, 2, {2, 0, 0.4f, 0.3f});
                const int out = b.add(kOut, 3, {0.7f, -0.3f});
                b.c(mrk, 0, cpx, 0);
                b.c(mrk, 1, lpg, 2);
                b.c(cpx, 0, lpg, 0);
                b.c(lpg, 0, out, 0);
                varyB = cpx;
            }
            {
                b.patch = 2;
                const int rete = b.add(kRete, 0, {0.7f, 0.7f, 300, 0.4f});
                const int out = b.add(kOut, 1, {0.5f, 0.3f});
                b.c(rete, 0, out, 0);
                b.c(rete, 1, out, 1);
            }
            {
                b.patch = 3;
                const int k1 = b.add(kEucl, 0, {16, 5, 0, 4});
                const int kick = b.add(kPerc, 1, {0, 0, 0.3f});
                const int hat = b.add(kPerc, 13, {2, 0, 0.05f, 0.7f, 0.4f});
                const int out = b.add(kOut, 2, {0.8f});
                b.c(k1, 0, kick, 0);
                b.c(k1, 1, hat, 0);   // the off-steps
                b.c(kick, 0, out, 0);
                b.c(hat, 0, out, 0);
            }
            break;
        }
        case 6: {   // resonators: Euclidean strikes an exciter into a Rings-style resonator; Plaits chords pad
            const int clk = b.add(kClk, 0, {4});
            const int eu = b.add(kEucl, 12, {16, 7, 0, 4, 0.3f});
            const int tur = b.add(kTuring, 24, {0.12f, 8, 6, 1.5f, 0, 4});
            const int ex = b.add(kExct, 1, {0.0f, 0.5f, 0.15f, 0.4f, 0.9f, 0.6f, 0.05f, 0.8f});
            const int rs = b.add(kReso, 2, {0, -1, 0, 0.25f, 0.6f, 0.6f, 0.3f, 0.4f, 0.8f});
            const int lfo = b.add(kLfo, 37, {0.05f});
            const int pl = b.add(kPlts, 36, {14, -1, 0, 0.4f, 0.5f, 0.5f, 0.7f, 0.5f, 0.45f});
            const int mx = b.add(kMix, 3, {0.9f, 0.6f, 0.0f, 0.0f, 1.0f});
            const int vb = b.add(kVerb, 4, {0.85f, 0.4f, 0.02f, 0.4f});
            const int out = b.add(kOut, 5, {0.8f});
            b.c(clk, 0, eu, 0);
            b.c(eu, 0, ex, 0);
            b.c(eu, 0, tur, 0);
            b.c(tur, 0, rs, 2);
            b.c(ex, 0, rs, 0);
            b.c(lfo, 0, rs, 3, 0.3f);
            b.c(lfo, 1, pl, 2, 0.3f);
            b.c(rs, 2, mx, 0);
            b.c(pl, 0, mx, 1);
            b.c(mx, 0, vb, 0);
            b.c(vb, 0, out, 0);
            b.c(vb, 1, out, 1);
            varyA = rs;
            varyB = ex;
            break;
        }
        case 7: {   // acid net: a 303 line, FLOATER drums and the real rete patch (Manual Gate = Perform)
            const int seq = b.add(kSeq, 0, {16, 4, 0.5f, 2, 9, -1});
            const int acid = b.add(kAcid, 1, {0.0f, 0.35f, 0.75f, 0.6f, 0.3f, 0.7f, 60, 0.3f, 0.7f, -12});
            const int bern = b.add(kBern, 12, {0.3f, 1});
            const int eu = b.add(kEucl, 24, {16, 4, 0, 4});
            const int ki = b.add(kFltr, 25, {0, 0.25f, 0.3f, 0.5f, 0.4f, 0.4f, 0.9f});
            const int eu2 = b.add(kEucl, 36, {16, 5, 2, 4});
            const int pc = b.add(kFltr, 37, {2, 0.5f, 0.2f, 0.85f, 0.2f, 0.2f, 0.5f});
            const int mg = b.add(kMgate, 13);
            const int mgen = b.add(kMg, 14, {0.4f, -0.4f, 1, 0, 0.6f});
            const int net = b.add(kNet, 15, {0.5f, 0.6f, 0.55f, 0.45f, 0.5f, 3, 0.35f});
            const int dly = b.add(kDelay, 2, {0.3f, 3, 0.4f, 0.5f, 0.3f});
            const int mx = b.add(kMix, 3, {0.8f, 0.8f, 0.5f, 0.45f, 1.2f});
            const int out = b.add(kOut, 4, {0.8f});
            b.c(seq, 0, acid, 1);
            b.c(seq, 1, acid, 2);
            b.c(seq, 1, bern, 0);
            b.c(bern, 0, acid, 3);
            b.c(eu, 0, ki, 0);
            b.c(eu2, 0, pc, 0);
            b.c(mg, 1, net, 0);
            b.c(mg, 1, mgen, 0);
            b.c(mgen, 0, net, 3, 0.3f);
            b.c(mgen, 1, net, 2, 0.4f);
            b.c(acid, 0, dly, 0);
            b.c(dly, 0, mx, 0);
            b.c(ki, 0, mx, 1);
            b.c(pc, 0, mx, 2);
            b.c(net, 2, mx, 3);
            b.c(mx, 0, out, 0);
            varyA = acid;
            varyB = net;
            break;
        }
    }
    eng_.rebuild();
    // snapshot 1 = as built, 2 = a variation, 3 = sparser; song 1 2 1 3
    eng_.storeSnap(0);
    Rand r;
    r.seed(0x5EED + uint32_t(i));
    if (varyA >= 0) eng_.randomize(varyA, r);
    if (varyB >= 0) eng_.randomize(varyB, r);
    eng_.storeSnap(1);
    if (varyA >= 0) eng_.randomize(varyA, r);
    eng_.storeSnap(2);
    eng_.recallSnap(0, 0, now_);
    static const uint8_t order[4] = {0, 1, 0, 2};
    for (int k = 0; k < 4; ++k) {
        eng_.song[k].snap = order[k];
        eng_.song[k].morphBars = 2;
        eng_.song[k].holdBars = 2;
    }
    eng_.songLen = 4;
    eng_.songOn = false;
    patch_ = 0;
    sel_ = -1;
    selCable_ = -1;
    mRow_ = mCol_ = 0;
    for (int c = 0; c < kCells; ++c) {
        const int s = eng_.slotAtCell(0, c);
        if (s >= 0) {
            select(s);
            break;
        }
    }
    // LABKIT: macro 1 opens the brightest knob of the demo, LFO 1 breathes on it
    if (varyA >= 0 && knobBase_ >= 0) {
        kit::Mod& M = ctx_->mod();
        static const uint8_t kBright[] = {0, 1, 4, 0, 0, 0, 3, 1};   // a knob of varyA per demo
        const int p = kBright[i < 8 ? i : 0] < def(eng_.mod(varyA).type).nParam ? kBright[i < 8 ? i : 0] : 0;
        M.setMacroName(0, "BRIGHT");
        const int r = M.route(kit::kSrcMacro + 0, knobId(varyA, p), 0.4f);
        if (r >= 0) M.setWindow(r, true, eng_.mod(varyA).k[p], 1.0f);
        M.route(kit::kSrcLfo + 0, knobId(varyA, p), 0.08f);
        M.update(false);
    }
    rebuildJacks();
    pagesDirty_ = true;
}

// ------------------------------------------------------------------ templates
// Small patches added to the current canvas next to a cell: the layout is a
// block of cells (dx, dy) placed where it fits. An OUT in a template is
// replaced by the canvas's own OUT when it has one; "FX chain" takes its
// input from the selected module.
namespace {
struct TplMod {
    uint8_t type;
    int8_t dx, dy;
    float k[4];   // first knobs (NaN = default)
};
struct TplCab {
    int8_t a, ao, b, bi;
    float amt;
};
struct Tpl {
    const char* name;
    const char* about;
    int nMod, nCab;
    TplMod m[8];
    TplCab c[12];
};
constexpr float N_ = __builtin_nanf("");
const Tpl kTpl[kTemplates] = {
    {"East voice", "MIDI > VCO > ladder > VCA, ADSR on both", 6, 7,
     {{kMidi, 0, 0, {1, N_, N_, N_}}, {kVco, 1, 0, {0, 0, N_, N_}}, {kLadder, 2, 0, {700, 0.45f, 4, 1.4f}},
      {kVca, 3, 0, {N_, N_, N_, N_}}, {kAdsr, 1, 1, {0.004f, 0.3f, 0.5f, 0.4f}}, {kOut, 4, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 1, 0, 1}, {0, 1, 4, 0, 1}, {1, 0, 2, 0, 1}, {2, 0, 3, 0, 1}, {4, 0, 3, 1, 1}, {4, 0, 2, 2, 0.5f},
      {3, 0, 5, 0, 1}}},
    {"West bongo", "Euclid pings a low-pass gate on a complex osc", 4, 4,
     {{kEucl, 0, 0, {16, 5, 0, 4}}, {kCplx, 1, 0, {-1, 7, 1.5f, 1.2f}}, {kLpg, 2, 0, {2, 0, 0.18f, 0.5f}},
      {kOut, 3, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 2, 2, 1}, {1, 0, 2, 0, 1}, {2, 0, 3, 0, 1}, {0, 0, 1, 4, 0.4f}}},
    {"Reticulum noise", "Manual gate re-rolls the rete net, MG-16 moves it, granular after", 5, 7,
     {{kMgate, 0, 0, {0.5f, N_, N_, N_}}, {kMg, 0, 1, {0.4f, -0.3f, 1, 0}}, {kNet, 1, 0, {0.55f, 0.6f, 0.55f, 0.45f}},
      {kGrain, 2, 0, {0.15f, 8, 0.3f, 0}}, {kOut, 3, 0, {0.7f, N_, N_, N_}}},
     {{0, 1, 2, 0, 1}, {0, 1, 1, 0, 1}, {1, 0, 2, 3, 0.3f}, {1, 1, 2, 2, 0.3f}, {2, 0, 3, 0, 1}, {2, 1, 3, 1, 1},
      {3, 0, 4, 0, 1}}},
    {"Plaits voice", "MIDI plays Plaits (gate pings its LPG)", 3, 3,
     {{kMidi, 0, 0, {1, N_, N_, N_}}, {kPlts, 1, 0, {8, 0, 0, 0.5f}}, {kOut, 2, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 1, 0, 1}, {0, 1, 1, 1, 1}, {1, 0, 2, 0, 1}}},
    {"Rings pluck", "Sequencer strums an exciter into the resonator", 4, 5,
     {{kSeq, 0, 0, {8, 4, 0.3f, 6}}, {kExct, 1, 1, {0, 0.5f, 0.1f, 0.5f}}, {kReso, 1, 0, {1, -1, 0, 0.3f}},
      {kOut, 2, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 2, 2, 1}, {0, 1, 1, 0, 1}, {1, 0, 2, 0, 1}, {2, 0, 3, 0, 1}, {2, 1, 3, 1, 1}}},
    {"Acid line", "Sequencer into the 303, Bernoulli accents, delay", 5, 6,
     {{kSeq, 0, 0, {16, 4, 0.5f, 2}}, {kBern, 0, 1, {0.3f, 1, N_, N_}}, {kAcid, 1, 0, {0, 0.35f, 0.75f, 0.6f}},
      {kDelay, 2, 0, {0.3f, 3, 0.35f, 0.5f}}, {kOut, 3, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 2, 1, 1}, {0, 1, 2, 2, 1}, {0, 1, 1, 0, 1}, {1, 0, 2, 3, 1}, {2, 0, 3, 0, 1}, {3, 0, 4, 0, 1}}},
    {"Drum kit", "Three Euclideans on FLOATER kick, snare, perc", 7, 7,
     {{kEucl, 0, 0, {16, 4, 0, 4}}, {kEucl, 0, 1, {16, 2, 4, 4}}, {kEucl, 0, 2, {16, 7, 1, 4}},
      {kFltr, 1, 0, {0, 0.3f, 0.3f, 0.5f}}, {kFltr, 1, 1, {1, 0.4f, 0.3f, 0.4f}}, {kFltr, 1, 2, {2, 0.6f, 0.2f, 0.8f}},
      {kMix, 2, 1, {0.9f, 0.7f, 0.5f, 0.0f}}},
     {{0, 0, 3, 0, 1}, {1, 0, 4, 0, 1}, {2, 0, 5, 0, 1}, {3, 0, 6, 0, 1}, {4, 0, 6, 1, 1}, {5, 0, 6, 2, 1},
      {6, 0, -1, 0, 1}}},
    {"Particle cloud", "LFO moves the density of a QUANTA particle cloud into reverb", 4, 5,
     {{kLfo, 0, 0, {0.1f, N_, N_, N_}}, {kPart, 1, 0, {0, 0, 0, 12}}, {kVerb, 2, 0, {0.8f, 0.4f, 0.02f, 0.4f}},
      {kOut, 3, 0, {0.7f, N_, N_, N_}}},
     {{0, 0, 1, 2, 0.5f}, {1, 0, 2, 0, 1}, {1, 1, 2, 1, 1}, {2, 0, 3, 0, 1}, {2, 1, 3, 1, 1}}},
    {"FX chain", "Delay into reverb, fed from the selected module", 3, 4,
     {{kDelay, 0, 0, {0.35f, 5, 0.4f, 0.6f}}, {kVerb, 1, 0, {0.75f, 0.5f, 0.02f, 0.35f}},
      {kOut, 2, 0, {0.8f, N_, N_, N_}}},
     {{0, 0, 1, 0, 1}, {0, 1, 1, 1, 1}, {1, 0, 2, 0, 1}, {1, 1, 2, 1, 1}}},
};
}  // namespace

const char* Graf::templateName(int i) { return kTpl[(i >= 0 && i < kTemplates) ? i : 0].name; }
const char* templateAbout(int i) { return kTpl[(i >= 0 && i < kTemplates) ? i : 0].about; }

int Graf::addTemplate(int i, int patch, int nearCell) {
    if (i < 0 || i >= kTemplates) return 0;
    const Tpl& t = kTpl[i];
    // an OUT the canvas already has
    int existingOut = -1;
    for (int c = 0; c < kCells && existingOut < 0; ++c) {
        const int s = eng_.slotAtCell(patch, c);
        if (s >= 0 && eng_.mod(s).type == kOut) existingOut = s;
    }
    int w = 1, h = 1;
    for (int k = 0; k < t.nMod; ++k) {
        if (t.m[k].type == kOut && existingOut >= 0) continue;
        w = t.m[k].dx + 1 > w ? t.m[k].dx + 1 : w;
        h = t.m[k].dy + 1 > h ? t.m[k].dy + 1 : h;
    }
    // an origin where the whole block is free, scanning from nearCell
    int origin = -1;
    for (int k = 0; k < kCells && origin < 0; ++k) {
        const int o = (nearCell + k) % kCells;
        const int ox = o % lab::kPadCols, oy = o / lab::kPadCols;
        if (ox + w > lab::kPadCols || oy + h > lab::kPadRows) continue;
        bool ok = true;
        for (int m = 0; m < t.nMod && ok; ++m) {
            if (t.m[m].type == kOut && existingOut >= 0) continue;
            ok = eng_.slotAtCell(patch, (oy + t.m[m].dy) * lab::kPadCols + ox + t.m[m].dx) < 0;
        }
        if (ok) origin = o;
    }
    int slots[8];
    int made = 0;
    for (int m = 0; m < t.nMod; ++m) {
        const TplMod& tm = t.m[m];
        if (tm.type == kOut && existingOut >= 0) {
            slots[m] = existingOut;
            continue;
        }
        const int cell = origin >= 0 ? (origin / lab::kPadCols + tm.dy) * lab::kPadCols + origin % lab::kPadCols + tm.dx : -1;
        const int s = eng_.add(tm.type, patch, cell);
        slots[m] = s;
        if (s < 0) continue;
        ++made;
        for (int p = 0; p < 4 && p < def(tm.type).nParam; ++p)
            if (tm.k[p] == tm.k[p]) eng_.setParamK(s, p, Engine::unmapParam(def(tm.type).p[p], tm.k[p]));
    }
    const int from = (sel_ >= 0 && eng_.mod(sel_).used) ? sel_ : -1;
    for (int c = 0; c < t.nCab; ++c) {
        const TplCab& k = t.c[c];
        const int a = slots[k.a];
        const int b = k.b >= 0 ? slots[k.b] : (existingOut >= 0 ? existingOut : -1);
        if (a >= 0 && b >= 0) eng_.connect(a, k.ao, b, k.bi, k.amt);
    }
    if (i == 8 && from >= 0 && slots[0] >= 0 && def(eng_.mod(from).type).nOut) eng_.connect(from, 0, slots[0], 0, 1.0f);
    eng_.rebuild();
    if (made && slots[0] >= 0) select(slots[0]);
    pagesDirty_ = true;
    return made;
}

// ------------------------------------------------------------------ bench
namespace {
const char* const kBenchNames[] = {"idle",      "first-light", "reticulum",  "west-coast", "turing-drums",
                                   "poly-keys", "8-patches",   "fill-synth", "full-fx",    "fill-mixed",
                                   "fill-ctl",  "grain-max",   "rete-max",   "resonators", "acid-net",
                                   "plaits-max", "fill-new",   "net-max",    "mods-64"};
constexpr int kBench = 19;
// Fills the rack up to the CPU budget with the given module types (cycled),
// every input patched from the module before it -- the busiest patch the
// budget admits for that mix.
void fillRack(Engine& e, const int* types, int nTypes) {
    e.clear();
    e.quiesce();
    e.rack = true;
    const int out = e.add(kOut, 7, 47);
    const int clk = e.add(kClk, 7, 46);
    int prev = clk, cell = 0, misses = 0;
    for (int k = 0; misses < nTypes && cell < kMaxSlots - 3; ++k) {
        const int t = types[k % nTypes];
        const int s = e.add(t, cell / kCells, cell % kCells);
        if (s < 0) {
            ++misses;
            continue;
        }
        misses = 0;
        ++cell;
        const ModDef& d = def(t);
        for (int in = 0; in < d.nIn; ++in) {
            const ModDef& ps = def(e.mod(prev).type);
            if (e.connect(prev, in % (ps.nOut ? ps.nOut : 1), s, in, 0.6f) < 0) break;
        }
        if (d.nOut) prev = s;
    }
    if (out >= 0) e.connect(prev, 0, out, 0, 0.5f);
    e.rebuild();
}
}  // namespace

int Graf::benchScenarios() const {
#ifdef GRAF_CAL
    return 2 + kTypeCount + 3;
#else
    return kBench;
#endif
}

const char* Graf::benchName(int i) const {
#ifdef GRAF_CAL
    static char buf[24];
    if (i == 0) return "cal-base";
    if (i == 1) return "cal-cables";
    if (i == 2 + kTypeCount) return "cal-mix4";
    if (i == 3 + kTypeCount) return "cal-vco-saw";
    if (i == 4 + kTypeCount) return "cal-vco-4out";
    std::snprintf(buf, sizeof buf, "cal-%s", def(i - 2).tag);
    return buf;
#else
    return (i >= 0 && i < kBench) ? kBenchNames[i] : "?";
#endif
}

void Graf::benchSetup(int i) {
    benchScenario_ = i;
    eng_.setConcurrent(false);
#ifdef GRAF_CAL
    // base: one VCO. cables: a VCO into 4 CVMX x 3 inputs (12 cables, amount 0.5).
    // module T: a VCO feeding 4 copies of T on all their inputs (amount 0.5).
    eng_.clear();
    eng_.quiesce();
    eng_.rack = true;
    eng_.allOutputs = true;
    const int v = eng_.add(kVco, 0, 0);
    eng_.setParamK(v, 0, 0.5f);
    if (i >= 2 + kTypeCount) {   // VCO variants: pitch patched, no FM, into one MIX (4 cables)
        eng_.allOutputs = false;
        eng_.scopeSlot = -1;
        const int mx = eng_.add(kMix, 0, 9);
        eng_.connect(v, 0, mx, 0, 0.5f);
        eng_.connect(v, 0, mx, 1, 0.5f);
        eng_.connect(v, 0, mx, 2, 0.5f);
        eng_.connect(v, 0, mx, 3, 0.5f);
        if (i > 2 + kTypeCount) {
            eng_.disconnect(eng_.cableFind(v, 0, mx, 1));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 2));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 3));
            eng_.disconnect(eng_.cableFind(v, 0, mx, 0));
            for (int k = 0; k < 4; ++k) {
                const int s = eng_.add(kVco, 0, k + 1);
                eng_.connect(v, 3, s, 0, 0.1f);
                const int no = i == 3 + kTypeCount ? 1 : 4;
                for (int o = 0; o < no; ++o) eng_.connect(s, o, mx, k, 0.25f);
            }
        }
        eng_.rebuild();
        return;
    }
    if (i >= 1) {
        const int t = i == 1 ? kCvmx : i - 2;
        for (int k = 0; k < 4; ++k) {
            const int s = eng_.add(t, 0, k + 1);
            if (s < 0) break;
            const int nIn = def(t).nIn;
            for (int in = 0; in < nIn; ++in) eng_.connect(v, 0, s, in, 0.5f);
        }
    }
    eng_.rebuild();
    for (int k = 0; k < 4; ++k) eng_.noteOn(0, uint8_t(48 + k * 7), 100);
    return;
#endif
    switch (i) {
        case 0:
            eng_.clear();
            eng_.quiesce();
            break;
        case 1: loadDemo(0); break;
        case 2: loadDemo(1); break;
        case 3: loadDemo(2); break;
        case 4: loadDemo(3); break;
        case 5:
            loadDemo(4);
            for (int k = 0; k < 4; ++k) eng_.noteOn(0, uint8_t(48 + k * 4), 100);
            break;
        case 6: {
            loadDemo(5);
            for (int p = 0; p < kPatches; ++p) eng_.noteOn(p, 48, 100);
            break;
        }
        case 7: {
            const int t[] = {kSeq, kVco, kCplx, kLadder, kSvf, kAdsr, kVca, kLfo, kFold, kWtbl, kLpg};
            fillRack(eng_, t, int(sizeof t / sizeof t[0]));
            break;
        }
        case 8: {   // effects: granular, reverbs, delays fed by noise
            eng_.clear();
            eng_.quiesce();
            eng_.rack = true;
            const int n = eng_.add(kNoise, 0, 0);
            const int clk = eng_.add(kClk, 0, 1);
            int prev = n;
            int cell = 2;
            const int fx[] = {kGrain, kVerb, kDelay, kVerb, kDelay, kGrain};
            for (int t : fx) {
                const int s = eng_.add(t, 0, cell++);
                if (s < 0) continue;
                eng_.connect(prev, 0, s, 0, 0.8f);
                eng_.connect(prev, prev == n ? 1 : 1, s, 1, 0.8f);
                if (t == kGrain) eng_.connect(clk, 0, s, 2, 1.0f);
                prev = s;
            }
            const int out = eng_.add(kOut, 0, cell++);
            eng_.connect(prev, 0, out, 0, 0.5f);
            eng_.connect(prev, 1, out, 1, 0.5f);
            eng_.rebuild();
            break;
        }
        case 9: {
            int t[kTypeCount], n = 0;
            for (int k = 0; k < kTypeCount; ++k)
                if (k != kOut) t[n++] = (k * 7) % kTypeCount == kOut ? kVco : (k * 7) % kTypeCount;
            fillRack(eng_, t, n);
            break;
        }
        case 10: {
            const int t[] = {kClk, kDiv, kSeq, kEucl, kTuring, kMarkov, kPinb, kSnh, kQuant, kLfo, kAdsr, kAd,
                             kFunc, kSlew, kLogic, kCvmx, kMidi, kPerc};
            fillRack(eng_, t, int(sizeof t / sizeof t[0]));
            break;
        }
        case 11: {   // granulars at full density and size, pitched up two octaves
            const int t[] = {kGrain};
            fillRack(eng_, t, 1);
            for (int s = 0; s < kMaxSlots; ++s)
                if (eng_.mod(s).used && eng_.mod(s).type == kGrain) {
                    eng_.setParamK(s, 0, 1.0f);
                    eng_.setParamK(s, 1, 1.0f);
                    eng_.setParamK(s, 3, 1.0f);
                    eng_.setParamK(s, 4, 1.0f);
                }
            const int n = eng_.add(kNoise, 6, 0);
            for (int s = 0; s < kMaxSlots; ++s)
                if (eng_.mod(s).used && eng_.mod(s).type == kGrain) eng_.connect(n, 0, s, 0, 1.0f);
            eng_.rebuild();
            break;
        }
        case 12: {
            const int t[] = {kRete};
            fillRack(eng_, t, 1);
            break;
        }
        case 13: loadDemo(6); break;
        case 14:
            loadDemo(7);
            eng_.manualGate = true;
            break;
        case 15: {   // Plaits voices at the dearest models the budget lets through
            const int t[] = {kPlts};
            fillRack(eng_, t, 1);
            static const int kDear[] = {20, 12, 19, 18, 10, 22, 6};
            for (int s = 0; s < kMaxSlots; ++s) {
                if (!eng_.mod(s).used || eng_.mod(s).type != kPlts) continue;
                for (int m : kDear) {
                    const float k = Engine::unmapParam(def(kPlts).p[0], float(m));
                    eng_.setParamK(s, 0, k);
                    if (eng_.mod(s).k[0] == k) break;
                }
                eng_.setParamK(s, 7, 0.9f);   // long LPG
            }
            break;
        }
        case 16: {
            const int t[] = {kReso, kExct, kNet, kAcid, kFltr, kPart, kChaos, kBern, kRval, kMg, kMgate};
            fillRack(eng_, t, int(sizeof t / sizeof t[0]));
            for (int s = 0; s < kMaxSlots; ++s) {
                Module& m = eng_.mod(s);
                if (!m.used) continue;
                if (m.type == kPart) {   // the densest cloud: trainlets, many particles
                    eng_.setParamK(s, 0, Engine::unmapParam(def(kPart).p[0], 3));
                    eng_.setParamK(s, 3, 1.0f);
                    eng_.setParamK(s, 4, 1.0f);
                }
                if (m.type == kReso) eng_.setParamK(s, 1, Engine::unmapParam(def(kReso).p[1], -3));   // all partials audible
            }
            eng_.manualGate = true;
            break;
        }
        case 17: {
            const int t[] = {kNet};
            fillRack(eng_, t, 1);
            break;
        }
        case 18: {   // a full patch with 64 LABKIT routes on its knobs
            loadDemo(0);
            kit::Mod& M = ctx_->mod();
            M.clearRoutes();
            int n = 0;
            for (int src = 0; src < kit::kSrcWs + 8 && n < kit::kMaxRoutes; ++src)
                for (int s = 0; s < kMaxSlots && n < kit::kMaxRoutes; ++s) {
                    if (!eng_.mod(s).used) continue;
                    const int np = def(eng_.mod(s).type).nParam;
                    for (int p = (src % 3); p < np && n < kit::kMaxRoutes; p += 3)
                        if (M.route(src, knobId(s, p), 0.05f + 0.01f * float(p)) >= 0) ++n;
                }
            for (int i = 0; i < kit::kLfos; ++i) ctx_->params().set(M.lfoParam(i, 0), float(i));
            M.update(false);
            break;
        }
        default: break;
    }
    eng_.quiesce();
}

}  // namespace graf
