// GRAF -- module DSP: process / reset functions (graf_mod_*.cpp) and the
// helpers they share. The definition table is in graf_defs.cpp.
#pragma once

#include "lab/dsp.h"
#include "ws/graf_engine.h"

namespace graf {

// Shared zero buffer and per-input scratch (SRAM on the device).
extern float gZero[kBlock];

// Clock-division names / lengths in beats (sequencer "Rate", LFO / delay sync).
constexpr int kRates = 8;
extern const char* const kRateNames[kRates];
extern const float kRateBeats[kRates];
extern const char* const kNoteNames[12];
extern const char* const kOffOn[2];
extern const char* const kMidiChNames[17];

// Wavetables: 4 tables x 8 frames x (256 + 1) samples, built by initWavetables().
constexpr int kWtTables = 4, kWtFrames = 8, kWtLen = 256;
extern float* gWt;
bool initWavetables(lab::Arena& a);

// Built-in sampler sound (bell / pluck), n frames mono.
void makeBuiltinSample(int16_t* dst, uint32_t n, uint32_t* frames);

// Tick source for clocked modules: the Clock input when patched, else the
// transport at `rateBeats` per step (only while playing).
struct TickIn {
    Schmitt sch;
    int32_t lastStep;
    uint32_t sinceTick;   // samples since the last tick (gate length)
    uint32_t period;      // samples between the last two ticks
    void reset() {
        sch = Schmitt();
        lastStep = -1;
        sinceTick = 0;
        period = 11025;
    }
    // true on a tick at sample i; invRate = 1 / (beats per step)
    bool tick(const Proc& p, const float* clk, uint32_t i, float invRate) {
        ++sinceTick;
        bool t = false;
        if (clk) {
            t = sch.rise(clk[i]);
            if (t) period = sinceTick;
        } else if (p.playing) {
            const int32_t st = int32_t((p.beat + float(i) * p.bps) * invRate);
            if (st != lastStep) {
                lastStep = st;
                t = true;
                period = uint32_t(1.0f / (invRate * (p.bps > 1e-9f ? p.bps : 1e-9f)));
            }
        }
        if (t) sinceTick = 0;
        return t;
    }
};
// Gate length in samples for a step period and a 0..1 width.
inline uint32_t gateSamples(uint32_t period, float width) {
    float s = float(period) * width;
    if (s < 64.0f) s = 64.0f;
    return uint32_t(s);
}

// MIDI out events from the audio side (sequencers): note on / off.
void midiOutNote(Engine* e, int ch1, uint8_t note, uint8_t vel, bool on);

// sources
void pVco(Module&, Proc&);
void pCplx(Module&, Proc&);
void pWtbl(Module&, Proc&);
void pNoise(Module&, Proc&);
void pSmpl(Module&, Proc&);
void pAudIn(Module&, Proc&);
void pMidi(Module&, Proc&);
void pRete(Module&, Proc&);
void rRete(Module&);
void pPerc(Module&, Proc&);
// control
void pPinb(Module&, Proc&);
void rPinb(Module&);
void pClk(Module&, Proc&);
void pDiv(Module&, Proc&);
void pSeq(Module&, Proc&);
void rSeq(Module&);
void pEucl(Module&, Proc&);
void pTuring(Module&, Proc&);
void rTuring(Module&);
void pMarkov(Module&, Proc&);
void rMarkov(Module&);
void pSnh(Module&, Proc&);
void pQuant(Module&, Proc&);
void pLfo(Module&, Proc&);
void pAdsr(Module&, Proc&);
void pAd(Module&, Proc&);
void pFunc(Module&, Proc&);
void pSlew(Module&, Proc&);
void pLogic(Module&, Proc&);
void pCvmx(Module&, Proc&);
void rClocked(Module&);
// shapers / fx / io
void pLadder(Module&, Proc&);
void pSvf(Module&, Proc&);
void pLpg(Module&, Proc&);
void pFold(Module&, Proc&);
void pVca(Module&, Proc&);
void pMix(Module&, Proc&);
void pRing(Module&, Proc&);
void pCrush(Module&, Proc&);
void pDelay(Module&, Proc&);
void pVerb(Module&, Proc&);
void pGrain(Module&, Proc&);
void pOut(Module&, Proc&);
// round 2
void pPlts(Module&, Proc&);
void pReso(Module&, Proc&);
void pExct(Module&, Proc&);
void pNet(Module&, Proc&);
void rNet(Module&);
void pRval(Module&, Proc&);
void pMg(Module&, Proc&);
void pMgate(Module&, Proc&);
void pChaos(Module&, Proc&);
void pBern(Module&, Proc&);
void pAcid(Module&, Proc&);
void pFltr(Module&, Proc&);
void pPart(Module&, Proc&);
float netNode(const Module& m, int i);   // 0..1 activity of rete node i

// Shared tables of the round-2 modules (particle windows...), from the arena.
bool initModuleTables(lab::Arena& a);
// Plaits voices (graf_mod_plaits.cpp): one pooled voice per PLTS module.
void* plaitsAlloc(lab::Arena& a);
void plaitsReset(void* voice);
int32_t plaitsCost(int model);   // M4 instructions per block of one Plaits model
extern const char* const kPlaitsModels[24];
// Which output of a sequencing module is its LABKIT lane.
int laneOutput(int type);

// Markov matrix from a seed (MARKOV ext bytes).
void markovSeed(Module& m, int seed);
// Pinball table layout (PINB ext bytes).
void pinbLayout(Module& m, uint32_t seed);
// SEQ: a default pattern.
void seqDefault(Module& m);

// Sequencer step data in Module::ext (SEQ).
inline int8_t& seqNote(Module& m, int i) { return reinterpret_cast<int8_t&>(m.ext[i]); }
inline bool seqGate(const Module& m, int i) { return (m.ext[16 + (i >> 3)] >> (i & 7)) & 1; }
inline void seqSetGate(Module& m, int i, bool on) {
    if (on) m.ext[16 + (i >> 3)] |= uint8_t(1u << (i & 7));
    else m.ext[16 + (i >> 3)] &= uint8_t(~(1u << (i & 7)));
}
inline uint8_t& seqCv(Module& m, int i) { return m.ext[18 + i]; }

// Live views for the UI (read from the module state, main loop).
int seqPosition(const Module& m);
int pinbBall(const Module& m, int i);   // cell of ball i, -1 if none
int markovState(const Module& m);
uint32_t turingRegister(const Module& m);
int euclStep(const Module& m);
float reteNode(const Module& m, int i);   // 0..1 activity

}  // namespace graf
