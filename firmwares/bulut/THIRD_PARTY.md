# Third-party code and ideas in BULUT

Since round 2 BULUT compiles third-party DSP. Files in `src/clouds/`,
`src/beads/` and `src/stmlib/` are copied **unchanged** (their licence texts sit
next to them); BULUT's own code is in `src/ws/`. Code *derived* from GPL-3.0
sources (artifex, rete) makes the BULUT firmware image a GPL-3.0 combined work:
the `.ptf` is distributed under the GNU GPL v3, with the MIT notices kept.

## Copied unchanged

| Files in BULUT | Origin | Licence | Used for |
|----------------|--------|---------|----------|
| `src/clouds/dsp/fx/diffuser.h`, `pitch_shifter.h`, `fx_engine.h`, `src/clouds/dsp/frame.h` | Mutable Instruments Clouds, Emilie Gillet, `pichenettes/eurorack` commit f4cfa7697095a28d292bff758beb51deb49b5a86 | MIT (`src/clouds/LICENSE`, the header notice; eurorack has no root licence file) | TEXTURE (the diffuser) and SHIFTER (the pitch shifter) FX modes, run at half rate (`src/ws/fx.cpp`) |
| `src/beads/dsp/fx/wow_and_flutter.h`, `dsp/mu_law.h`, `dsp/mu_law.cc`, `dsp/frame.h`, `dsp/sine_oscillator.h`, `resources.h`; the `lut_sine` table in `src/ws/tp_tables.inc` (extracted from `beads/resources.cc`) | Mutable Instruments Beads, Emilie Gillet (via `/home/claude/reticulum/ThirdParty/beads`) | MIT (`src/beads/LICENSE`) | DIRTY TAPE media: wow and flutter on the channel output (half rate) and the mu-law recording format of the buffer (tabulated at init on 12 bits) |
| `src/stmlib/stmlib.h`, `dsp/dsp.h`, `dsp/delay_line.h`, `dsp/parameter_interpolator.h`, `dsp/cosine_oscillator.h`, `utils/random.h`, `utils/random.cc` | stmlib, Emilie Gillet (the copy in `/home/claude/reticulum/ThirdParty/stmlib`; eurorack's is a submodule) | MIT (`src/stmlib/LICENSE`) | support code of the files above. On the host (tests, simulator) `TEST` is defined so stmlib takes its portable C path instead of ARM `ssat`/`usat` |

`src/ws/tp.h` / `src/ws/tp_build.cpp` include these files (with the compiler
warnings they raise under BULUT's flags silenced) and build their `.cc` parts.

## Derived (rewritten for BULUT)

| What | Where | Origin | Licence |
|------|-------|--------|---------|
| FLANGER, PANNER, CRUSHER, SLICER, FREEZER FX modes: ranges, laws, the 32-entry rhythm table (copied), the freezer's catch / roll behaviour (simplified: recording stops while frozen) | `src/ws/fx.cpp` | forsitan modulare **artifex** (`artifex/core.hpp`), upstream c642f12512a9484ec9bf92aec5ae843ba62b5163, via RETICULUM's copy | GPL-3.0 |
| RETE generator: eight leaky integrators, a seeded 8 x 8 matrix, DC block, clip, fed back; excite = a slow random walk. Run at 11.025 kHz with linear interpolation | `src/ws/fx.cpp` (`Generator::rete`) | forsitan modulare **rete** (`src/rete.cpp`) as ported in RETICULUM `Source/engine/Modules.h` | GPL-3.0 |
| BEADS engine: grain duration curve (ComputeGrainDuration), grain envelope (GrainEnvelope: attack / sustain / release + smoothness), trigger law (regular left of noon, random right, seeds at noon), amplitude 1/sqrt(active + 1), pan spread, the four media | `src/ws/engine.cpp` (`spawnBeads`, `runEmitter`), `writeBuffer`, `fx.cpp` (`media`) | Mutable Instruments Beads (`beads/dsp/granular_processor.cc`, `grain.h`, `grain_envelope.h`) | MIT |
| QUANTA engine: particle types (glisson, Gabor grain, grainlet, pulsar, trainlet), stochastic onset laws, MASK | `src/ws/engine.cpp` (`spawnQuanta`) | the user's QUANTA plug-in (`/home/claude/quanta`, concepts) and Curtis Roads, *Microsound* (MIT Press, 2001); Iannis Xenakis, *Formalized Music* | concepts; written for BULUT |
| GRANULAR engine parameter set (position, size, density, pitch, spread, texture, feedback) | `src/ws/engine.cpp` | concepts after Mutable Instruments Clouds | concepts; written for BULUT |

## From the OpenTracker base

| What | Where | Note |
|------|-------|------|
| Delay and reverb send effects (`ot::audio::Delay`, `ot::audio::Reverb`) | `src/core/audio/effects.cpp` | OpenTracker's own code (its reverb follows the topology of the Clouds reverb; no Mutable Instruments code is copied there) |
| HAL, display, WAV import / export, the LABKIT Shell (`src/lab`) | `src/core`, `src/hal`, `src/lab` | see `OPENTRACKER.md` and the repository README |
