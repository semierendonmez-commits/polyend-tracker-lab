# Third-party material in GRAF

GRAF's workstation (`src/ws/`) is GRAF's own code except for **Mutable
Instruments Plaits + stmlib** (copied, MIT) and the modules **ported** from
open code listed below (rewritten for GRAF's module interface; GPL-3.0 code is
approved for this firmware). Every borrowed file or port is listed here.

## Copied source code

| Where | What | Source | Licence |
| --- | --- | --- | --- |
| `src/ws/mi/plaits/`, `src/ws/mi/stmlib/` | Plaits macro-oscillator (24 models) and the stmlib helpers it uses -- the PLTS module | Emilie Gillet, Mutable Instruments (github.com/pichenettes/eurorack). Copied from this repository's KONSOL firmware (`firmwares/konsol/src/ws/mi/`), which took it from `/home/claude/floatdat/ThirdParty` (the Move Anything port by charlesvestal) | MIT; `src/ws/mi/plaits/LICENSE`, `src/ws/mi/stmlib/LICENSE` |

The copy is KONSOL's, unchanged (KONSOL's changes are marked `KONSOL` in the
source: 44.1 kHz, 64-sample blocks, two renders per 128-frame block, no
trigger delay, no user wavetables, an unsigned shift in `dx_units.h`).
GRAF's wrapper (`src/ws/graf_mod_plaits.cpp`, GRAF's own code) adds a pool of
four voices, per-model CPU costs measured in GRAF, and keeps the Speech model
below its LPC word banks (decoding a bank costs more than one audio block).

## Ported (rewritten in GRAF's own code)

| Where | What | Source | Licence / status |
| --- | --- | --- | --- |
| `graf_mod_new.cpp` NET | forsitan rete: eight leaky integrators, random 8 x 8 matrix from a seed, DC-blocking high-pass, hard clip, one-sample feedback; RND = new matrix | forsitan modulare by gosub (github.com/gosub/forsitan-modulare, `src/rete.cpp`), via the user's RETICULUM plug-in port (`/home/claude/reticulum/Source/engine/Modules.h`) | GPL-3.0 |
| `graf_mod_new.cpp` NET gains, RVAL | Random Values: new random values on a trigger | VCV Fundamental (github.com/VCVRack/Fundamental, `src/RandomValues.cpp`), via RETICULUM | GPL-3.0 |
| `graf_mod_new.cpp` MG | Modulation Generator MG-16: each trigger makes every output a random LFO (random wave and rate) or a held value | TinyTricks by Thomas Rene Sidor (github.com/thomassidor/tinytricks, `src/modulation-generator.cpp`, `src/oscillators/lfo.cpp`), via RETICULUM | GPL-3.0 |
| `graf_mod_new.cpp` MGAT | Manual Gate: gate, trigger, latch and extended outputs from a button | Count Modula by Adam Verspaget (github.com/countmodula/VCVRackPlugins, `src/modules/ManualGate.cpp`), via RETICULUM | GPL-3.0 |
| `graf_mod_new.cpp` ACID | Open303 signal chain: blend oscillator, TeeBee 303 ladder (TB_303 coefficients by mystran and kunn), feedback high-pass, accent, envelopes, slide | Robin Schmidt's Open303, as ported in this repository's KONSOL firmware (`firmwares/konsol/src/ws/engines_semi.cpp`, from `/home/claude/acid/third_party/schwung-303/dsp/open303`); parameter mappings of the user's schwung-303 | Open303: MIT. schwung-303 mappings: the user's code |
| `graf_mod_new.cpp` FLTR | FLOATER kick (sine / triangle, pitch envelope, click, overdrive), snare (three modal sines, noise through a high-pass, snap), perc (impulse into two high-Q band-passes, bit / rate crush) | The user's FLOATER plug-in (`/home/claude/floater/Source/engine/Voices.cpp`), via KONSOL's port | The user's code |
| `graf_mod_new.cpp` PART | QUANTA microsound particles: grain, glisson, grainlet, trainlet, pulsar; Gauss / Hann / expodec / rexpodec / trapezoid / rect windows | The user's QUANTA plug-in (`/home/claude/quanta/Source/engine/Particles.cpp`), via KONSOL's port; after C. Roads, *Microsound* (2001) | The user's code |
| `graf_mod_src.cpp` RETE | GRAF's own resonant-node network after the user's RETICULUM patch idea (round 1) | Idea of the user's patch | Own code |

## Algorithms and ideas (no code copied)

| Where | Algorithm | Source | Licence / status |
| --- | --- | --- | --- |
| `graf_mod_new.cpp` RESO | Modal resonator with structure / brightness / damping / position, odd / even outputs | The idea of Mutable Instruments Rings (Emilie Gillet) | Idea only; GRAF's own code |
| `graf_mod_new.cpp` EXCT | Bow, blow and strike exciter with a contour envelope | The idea of Mutable Instruments Elements | Idea only; GRAF's own code |
| `graf_mod_new.cpp` BERN | Bernoulli gate with trigger / gate / latch modes | The idea of Mutable Instruments Branches | Idea only |
| `graf_mod_new.cpp` CHAO | Lorenz attractor, logistic map | E. Lorenz (1963); R. May (1976) | Published mathematics |
| `graf_mod_fx.cpp` VERB | Plate reverb topology, delay lengths and output taps | J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES 45(9), 1997 | Published paper; the structure is used, no code |
| `graf_mod_fx.cpp` SVF, LPG; `graf_mod_src.cpp` RETE | Topology-preserving-transform state-variable filter | V. Zavalishin, "The Art of VA Filter Design" (2012) | Published book; no code |
| `graf_mod_fx.cpp` LADR | Four one-pole TPT stages with zero-delay feedback estimate | Same book, chapter on ladder filters | Published book; no code |
| `graf_mod_fx.cpp` GRN | Granular processor with freeze and feedback | The idea of Mutable Instruments Beads / Clouds | Idea only; GRAF's own code |
| `graf_mod_src.cpp` NOIS pink | "Economy" pink-noise filter coefficients | Paul Kellet, music-dsp mailing list archive (1999) | Public domain |
| `graf_mod_src.cpp` VCO | PolyBLEP band-limiting | Valimaki & Huovilainen (2007); same as `src/lab/dsp.h` | Published method; no code |
| `graf_mod_fx.cpp` VERB addressing | One ring, one decrementing pointer for all delay lines | The idea of Mutable Instruments' `FxEngine` (MIT) | Idea only, no code copied |

## Code from this repository

`src/core/formats/wav.*` (WAV import with resampling, for the sampler),
`src/core/fastrun.h` (SRAM placement of hot functions), `src/lab/*` (the Shell
and LABKIT, synced from `labkit/` with `labkit/tools/sync_labkit.sh`; do not
edit in place).
