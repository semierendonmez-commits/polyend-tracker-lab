# Third-party material in GRAF

GRAF's workstation (`src/ws/`) contains **no copied third-party source code**.
Every module was written for GRAF. Published algorithms it follows:

| Where | Algorithm | Source | Licence / status |
| --- | --- | --- | --- |
| `graf_mod_fx.cpp` VERB | Plate reverb topology, delay lengths and output taps | J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES 45(9), 1997 | Published paper; the structure is used, no code |
| `graf_mod_fx.cpp` SVF, LPG; `graf_mod_src.cpp` RETE | Topology-preserving-transform state-variable filter | V. Zavalishin, "The Art of VA Filter Design" (2012) | Published book; no code |
| `graf_mod_fx.cpp` LADR | Four one-pole TPT stages with zero-delay feedback estimate | Same book, chapter on ladder filters | Published book; no code |
| `graf_mod_src.cpp` NOIS pink | "Economy" pink-noise filter coefficients | Paul Kellet, music-dsp mailing list archive (1999) | Public domain |
| `graf_mod_src.cpp` VCO | PolyBLEP band-limiting | Valimaki & Huovilainen (2007); same as `src/lab/dsp.h` | Published method; no code |
| `graf_mod_fx.cpp` VERB addressing | One ring, one decrementing pointer for all delay lines | The idea of Mutable Instruments' `FxEngine` (github.com/pichenettes/eurorack, MIT) | Idea only, no code copied |

Code GRAF uses from this tree (OpenTracker base, same repository):
`src/core/formats/wav.*` (WAV import with resampling, for the sampler),
`src/core/fastrun.h` (SRAM placement of hot functions), `src/lab/*` (the Shell).
