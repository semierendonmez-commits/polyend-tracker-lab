# Third-party material in TILT

TILT's workstation (`src/ws/`) uses the drum models of **Mutable Instruments Plaits** and
the small part of **stmlib** they need, both MIT-licensed. They were copied from the copy
in the user's FLOATDAT repository (`ThirdParty/plaits`, `ThirdParty/stmlib`) into
`src/ws/mi/`, with their licence files. Everything else in `src/ws/` was written for TILT.
No GPL code is used.

## Copied files (MIT, Copyright Emilie Gillet / Mutable Instruments)

| File in `src/ws/mi/` | From | Licence | Changes for TILT |
| --- | --- | --- | --- |
| `plaits/LICENSE`, `stmlib/LICENSE` | Plaits, stmlib | MIT | none |
| `plaits/dsp/drums/analog_bass_drum.h` | Plaits | MIT | include paths; `__restrict` on the output pointer |
| `plaits/dsp/drums/synthetic_bass_drum.h` | Plaits | MIT | include paths; `__restrict` on the output pointer |
| `plaits/dsp/drums/analog_snare_drum.h` | Plaits | MIT | include paths; `__restrict`; modes with zero gain are skipped; an unrolled two-mode loop for the 808 case (tone below 2/3, no sustain), same maths |
| `plaits/dsp/drums/synthetic_snare_drum.h` | Plaits | MIT | include paths; `__restrict` on the output pointer |
| `plaits/dsp/drums/hi_hat.h` | Plaits | MIT | include paths; `__restrict` on the output pointer |
| `plaits/dsp/dsp.h` | Plaits | MIT | include path; `kSampleRate` and `kCorrectedSampleRate` set to 44100 (the Tracker's rate) |
| `plaits/dsp/oscillator/oscillator.h`, `sine_oscillator.h` | Plaits | MIT | include paths |
| `plaits/resources.h`, `plaits/resources.cpp` | Plaits `resources.h` / `resources.cc` | MIT | trimmed to the one table the drums use (`lut_sine`); `.cc` renamed `.cpp` for the build |
| `stmlib/stmlib.h` | stmlib | MIT | `IN_RAM` always empty (the Tracker image has no `.ramtext` section) |
| `stmlib/dsp/dsp.h` | stmlib | MIT | include path; the portable C versions are used off the ARM target (host tests, simulator) |
| `stmlib/dsp/filter.h`, `parameter_interpolator.h`, `units.h`, `polyblep.h`, `rsqrt.h` | stmlib | MIT | include paths |
| `stmlib/dsp/units.cpp` | stmlib `units.cc` | MIT | include path; renamed `.cpp` |
| `stmlib/utils/random.h`, `random.cpp` | stmlib (`random.cc`) | MIT | include paths; renamed `.cpp` |

Where they are used: `src/ws/tilt_audio.cpp` (the KICK, SNARE and HAT engines, the built-in
BREAK sample, and Plaits' sine table for the FM engine).

## Ideas and published methods (no code copied)

| Where | What | Source | Status |
| --- | --- | --- | --- |
| `tilt_engine.*`, `tilt_model.*` | The Markov ball: 16 steps, a 17 x 17 weight matrix (state 0 = S before step 1, column 0 = drain), trig / trig % / delay / delay % / velocity / length per step, trig lanes with a mute step, choke, swing, Time Multiplier, Pattern Length reset, Randomize Trigs, clear functions | "Pinball 2" Max for Live device by nouserid, as decoded in the user's FLOATDAT port (`Source/engine/Pinball.h`) | Behaviour only; the engine is new code |
| `tilt_engine.*` | Flippers, bumpers, ramps, combos, multiball, tilt, ball save, gravity, edge properties, memory, mutation, meta pages, melody mode, rules, heat / trail, next-move odds and entropy, capture, randomizer with locks | The user's TILT design (the renewed Pinball 2) and SEEDWALK / GENOTYPE (matrix shapes) | The user's own designs |
| `tilt_audio.cpp` ACID | 303-style voice: polyBLEP saw / square into a four-pole low-pass with resonance feedback, filter envelope, accent, slide | Classic TB-303 structure; ladder as a cascade of one-poles with feedback (Moog ladder, R. Moog 1969; Stilson & Smith 1996) | Published methods |
| `tilt_audio.cpp` FM | Two-operator phase modulation with feedback | J. Chowning, JAES 21(7), 1973 | Published method |
| `tilt_audio.cpp` reverb | Four-line feedback delay network with a Hadamard matrix, damping, two input all-passes | J. Stautner, M. Puckette (1982); J.-M. Jot (1991) | Published methods |
| `tilt_audio.cpp` delay | Tempo delay with ping-pong cross-feedback and a tone filter | Common technique | - |
| `tilt_engine.cpp`, `tilt_audio.cpp` | Fast log2 (exponent + quadratic) and fast exp2 (exponent + cubic) approximations | Folklore | - |
| `tilt_io.cpp` | Standard MIDI File, format 0 | MIDI Manufacturers Association, SMF 1.0 | Public specification |
| `tilt_json.*` | JSON (RFC 8259) reader / writer for the pattern files: a flat token array (type, start, end, child count) in a caller-provided buffer, in the manner of S. Zaitsev's jsmn (MIT); no jsmn code was copied, the tokenizer is new | RFC 8259; the jsmn token idea | Public specification / idea only |
| `tilt_json.*` | The pattern JSON's model (steps, four 17 x 17 pages with edge properties, flippers, combos, tilt, ball, memory, mutation, meta, rules, melody, randomizer, lanes) | The TILT plug-in readme (the user's own design); the plug-in's `FORMAT.md` / `Json.h` could not be reached, so the field names are TILT-for-Tracker's | The user's own design |
| `lab/dsp.h` (lab Shell) | PolyBLEP, TPT state-variable filter | Valimaki & Huovilainen (2007); V. Zavalishin (2012) | Published methods |

## Code of this tree (OpenTracker base and the lab Shell)

`src/core/formats/wav.*` (WAV import with resampling, for the SAMPLE channels),
`src/core/fastrun.h` (SRAM placement of the audio render), `src/lab/*` (the Shell:
intro, settings, transport, MIDI, recorder, display canvas, and **LABKIT v2**: the
parameter registry, modulation engine, navigation views, browser and keyboard, synced
unchanged from `labkit/` with `labkit/tools/sync_labkit.sh`; also `sim/src/lab_main.cpp`,
`tests/lab/lab_bench.cpp`, `tests/lab/labkit_test.cpp` and `WORKSTATION_GUIDE.md`).
Fonts are the FT81x ROM fonts. No new third-party code was added for LABKIT or JSON.
