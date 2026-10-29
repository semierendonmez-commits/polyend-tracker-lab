# MAKAM — third-party code and borrowed ideas

MAKAM's own code is `src/ws/`. Everything else in this tree is the
OpenTracker base and the lab Shell (LABKIT v2) as synced from
`polyend-tracker-lab/labkit`.

## Code used as it is

| What | Where | Licence / owner |
|---|---|---|
| OpenTracker base (HAL, FT81x, SD card, MIDI, audio I/O) | everything outside `src/ws`, `src/lab` | sandlbn/polyend-tracker-re (no licence file upstream; the lab's base, see LAB.md) |
| Reverb (Clouds-style FxEngine topology, written from the spec) | `src/core/audio/effects.cpp` (`ot::audio::Reverb`) | OpenTracker; topology after Mutable Instruments Clouds (MIT, Emilie Gillet) |
| WAV import (any rate / depth, cubic resampler) | `src/core/formats/wav.cpp` | OpenTracker |
| CRC-32 of the song file | `src/core/formats/crc32.cpp` | OpenTracker |
| LABKIT v2: registry, macros, LFOs, routes, MAP, PARAM pads, browser, keyboard | `src/lab/` | polyend-tracker-lab (the user's lab) |

## Rewritten for MAKAM from the user's own projects (GPL-3.0 approved by the user)

| MAKAM part | From | What was taken |
|---|---|---|
| Onset slicing of card breaks (`kit.cpp`, `onsetSlices`) | OpenTracker semi fork, `src/core/app/auto_slice.cpp` ("Breaks" auto-slice) | the spectral-flux algorithm: 256-point Hann frames, hop 128, square-rooted magnitudes, high-bin weighting, peak over +-2 frames and 1.5 x the local mean, 46 ms spacing, 32-sample energy refinement. Re-implemented with its own radix-2 FFT. |
| BBCut cut mode (`engine.cpp`, `planCut`, kCutClassic) | semi fork `src/core/gen/gen_bbcut.cpp`, after Nick Collins' BBCut (SuperCollider, GPL) via bencodec/BBenCut | BBCutProc11's block choice (odd unit sizes, 1-2 repeats, the closing roll) on one usul cycle |
| Warp cut mode (kCutWarp) | same file, WarpCut | straight blocks vs accelerating / decelerating repeat rolls |
| Phrase-end fills (`stepEvent`, FillStyle) | semi fork `gen_vary.cpp` `genTurnaround`, after the user's TURNAROUND plug-in | phrase / half / quarter tails with Rolls, Stutter, Reverse, Drop, Build styles |
| DNA cut mode (kCutDna) | semi fork `gen_vary.cpp` Genotype, after the user's GENOTYPE plug-in | learn the break, play a variation made only of its own material (here a class Markov chain over the slices) |
| Call + response phrase form (`gen.cpp`) | the user's Call & Response plug-in / semi fork `gen_callresp.cpp` (Nordl3, MIT) | the idea: a call and an answer sharing a rhythm; written for the seyir (the call stops on the guclu) |
| Talk, FM and Hoover bass models, the clean sine sub | the user's NEUROSTACK (`Source/engine/Neuro.cpp`) | the articulations' structure (formant frequencies "o" -> "i", ratio-2 FM with a falling index, diving detuned saws), the sub under the layers |
| Scenes + morph | the user's SETBOOK idea / semi fork perform scenes | snapshots of every continuous parameter and a timed morph |

No code was copied verbatim from these sources. The JUCE parts of the
plug-ins (AGPL / commercial) were not used.

## Theory sources (data, not code)

- İsmail Hakkı Özkan et al., TDV İslâm Ansiklopedisi: articles Rast, Uşşak,
  Hüzzam, Hicaz, Hümâyun, Uzzâl, Sabâ, Nihâvend, Kürdî, Segâh, Karcığar,
  Hüseynî, Muhayyer, Hicazkâr, Sûzinak, Nevâ, Kürdîlihicazkâr, Acemaşîran,
  Usul — scales, karar, güçlü, yeden, seyir, practice intonation notes.
- Arel-Ezgi-Uzdilek system: perde positions in commas (Wikipedia, "Turkish
  makam"), dörtlü / beşli definitions.
- Usul stroke orders: Varlık Ergen, "Türk Musikîsinde Usûller" (2025);
  group structures from Wikipedia "Aksak" and chromatone.center.
