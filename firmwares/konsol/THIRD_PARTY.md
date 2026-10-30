# Third-party material in KONSOL

KONSOL's workstation (`src/ws/`) contains one copied third-party library,
**Mutable Instruments Plaits + stmlib** (`src/ws/mi/`, MIT, see below); every
other file in `src/ws/` was written for KONSOL. It reuses code of this tree,
ports the user's own plug-in DSP (approved as GPL-3 material) and follows
published algorithms and ideas.

## Copied code

| Where | What | Source | Licence |
| --- | --- | --- | --- |
| `src/ws/mi/plaits/`, `src/ws/mi/stmlib/` | Plaits macro-oscillator (24 models: VA-VCF, phase distortion, 3 x six-op FM, wave terrain, string machine, chiptune, VA, waveshaper, FM, formant, additive, wavetable, chords, speech, swarm, noise, particle, string, modal, bass drum, snare, hi-hat) and the stmlib helpers it uses | Emilie Gillet, Mutable Instruments (via /home/claude/floatdat/ThirdParty, the Move Anything port by charlesvestal) | MIT; `src/ws/mi/plaits/LICENSE` |

KONSOL changes to that copy (each marked `KONSOL` in the source): `.cc`
renamed `.cpp`, includes prefixed `ws/mi/`, `stmlib/fft` removed,
`kSampleRate` / `kCorrectedSampleRate` 44100, `kMaxBlockSize` and `kBlockSize`
64 (two renders per 128-frame block), `kTriggerDelay` 1 (no trigger delay),
no note smoothing on the first block of a pitch, `IN_RAM` empty, the
`stmlib/dsp/dsp.h` ARM path only on ARM, `user_data.h` a stub (no user
wavetables), `dx_units.h` `Pow2Fast` shifts an unsigned value (UBSan:
left shift of a negative value), and `speech_engine.cpp` hands the LPC word
bank no banks unless `KONSOL_PLAITS_WORDS=1` (KONSOL never selects them; the
11 KB word data is then not linked).

## Ported (rewritten in KONSOL's own code)

| Where | What | Source | Licence / status |
| --- | --- | --- | --- |
| `src/ws/engines_semi.cpp` TB303 | Open303 signal chain: blend oscillator, TeeBee 303 ladder (TB_303 mode coefficients by mystran and kunn), feedback high-pass, accent RC, amp / main envelopes, slide; the parameter mappings of schwung-303 | Robin Schmidt's Open303 (`/home/claude/acid/third_party/schwung-303/dsp/open303`); mappings from the user's schwung-303 / Schwung Acid plug-in | Open303: MIT. schwung-303 mappings: the user's code (GPL-3 approved) |
| `src/ws/engines_semi.cpp` FLOATER | Kick (2x oversampled sine / triangle with pitch env, FM transient, click, overdrive or wavefolder), snare (3 modal sines + white / pink noise through a 12 / 24 dB high-pass, snap), perc (impulse into two high-Q band-passes, scale quantiser, bit / rate crush) | The user's FLOATER plug-in (`/home/claude/floater/Source/engine/Voices.cpp`) | The user's code (GPL-3 approved) |
| `src/ws/engines_semi.cpp` NEURO | 12 neuro-bass models (Reese, Wobble, Growl, FM Stab, Hoover, Talk, Screech, Snarl, 808 Drop, Solid FM, Hollow, Semi Sines) with Tone / Motion / Grit, LFO, sub | The user's NEUROSTACK plug-in (`/home/claude/neurostack/Source/engine/Neuro.cpp`) | The user's code (GPL-3 approved) |
| `src/ws/engines_semi.cpp` QUANTA | Microsound particles: grain, glisson, grainlet, trainlet, pulsar; Gauss / Hann / expodec / rexpodec / trapezoid / rect windows | The user's QUANTA plug-in (`/home/claude/quanta/Source/engine/Particles.cpp`), after C. Roads, *Microsound* (2001) | The user's code (GPL-3 approved) |
| `src/ws/samples.cpp` onsets | Spectral-flux onset detector (N 256, hop 128, compressed magnitudes, high-bin weighting, +-2-frame peak picking, 46 ms spacing, energy refinement) | The user's OpenTracker semi fork, `auto_slice.cpp` ("Breaks") | The user's code (GPL-3 approved) |
| `src/ws/stretch.cpp` | WSOLA (W. Verhelst, M. Roelands 1993), phase vocoder with identity phase locking (J. Laroche, M. Dolson 1999), transient handling by onset segmentation / phase reset | Published methods | No code copied |
| `src/ws/fx.cpp` | FDN reverb (J.-M. Jot / Stautner-Puckette: 8 lines, Walsh-Hadamard feedback), octave-up delay-line pitch shifter (shimmer), tape / lo-fi delay | Published methods | No code copied |
| `src/ws/dspx.cpp` | Radix-2 FFT, polyBLEP (V. Valimaki et al.), TPT SVF (V. Zavalishin) | Published methods | No code copied |

## Reused from this tree and ideas


| Where | What | Source | Licence / status |
| --- | --- | --- | --- |
| `src/core/gen/gen_callresp.cpp` (used by the Call+Resp player) | Phrase logic of the Call / Response Generator: 46 style presets, 38 answer strategies | Nordl3, github.com/Nordl3/Call-Response-Generator, ported to C++ in the user's polyend-tracker-re-semi fork | MIT |
| `src/core/gen/gen_bbcut.cpp` (BBCut player) | The BBCut cut procedures (10) | Nick Collins' BBCut library for SuperCollider, via bencodec/BBenCut, rewritten in the user's fork | Algorithms re-implemented; see that file's header |
| `src/core/gen/gen_acid.cpp` (Acid player) | 303 line with density / accent / slide and live Mutate | Idea of the TB-3PO generator (the user's schwung-tb3po work); own implementation | Ideas only |
| `src/core/gen/gen_vary.cpp`, `gen_station.cpp` | Turnaround, Genotype, Station | The user's own plug-in designs (TURNAROUND, GENOTYPE / TILT, CIPHER) | The user's code |
| `src/core/audio/engines/*` (ACD, VAP, FAT, WTFM, PERC engines) | OpenTracker's clean-room synth engines | This repository's OpenTracker base | See `OPENTRACKER.md` |
| `src/core/audio/effects.cpp` (delay and reverb sends) | OpenTracker's delay; reverb topology after Mutable Instruments Clouds' FxEngine reverb | OpenTracker, written from its spec | MIT idea (stmlib); no Mutable Instruments code copied |
| `src/core/ui/screen_synth_tables.cpp` | Parameter names, ranges and value formats of the synth engines (shown on the Sound page) | OpenTracker | See `OPENTRACKER.md` |
| `src/ws/engines_native.cpp` Makam | Makam scales in Holdrian commas (53-TET) | Arel-Ezgi-Uzdilek theory of Turkish makam music, as published | Public music theory |
| `src/ws/engines_native.cpp` Makam (Kanun / Saz) | Karplus-Strong plucked string | K. Karplus, A. Strong, CMJ 7(2), 1983 | Published paper; no code |
| `src/ws/engines_native.cpp` filters | TPT state-variable filter | V. Zavalishin, "The Art of VA Filter Design" (2012) | Published method; no code |
| `src/ws/engines_native.cpp` `fsin` | Parabolic sine approximation with one refinement step | Folklore (devmaster.net, 2006) | Public knowledge |
| `src/ws/gen_glue.cpp` Euclid | Euclidean rhythms (`lab::dsp::euclid`) | G. Toussaint (2005) | Published paper |
| `src/ws/gen_glue.cpp` Usul | Turkish usul rhythm cycles (Aksak, Curcuna, Devr-i Hindi, Turk Aksagi, Aksak Semai, Duyek, Sofyan) as dum / tek strokes | Traditional rhythmic cycles | Public domain music theory |
| `src/ws/gen_glue.cpp` Pinball | A ball bouncing over a scale grid, bumpers kick it | Concept of the user's Pinball 2 / TILT Max for Live device | Idea only |
| `src/ws/ui_draw.cpp` | Session-view (clip grid + scene launch) workflow | Common clip-launcher convention (Ableton Live session view, Push / Launchpad style) | Concept only, own layout |

SD streaming (`src/ws/stream.*`) is KONSOL's own ring / loop / fade code
after upstream OpenTracker's `audio/stream_source.*` design (head resident,
per-voice rings filled by the main loop, underruns fade); it reads through
upstream's `ot::audio::IStreamReader` (`hal/sd_stream_reader.*` on the
device, `app/storage_stream_reader.*` in the simulator, both unchanged) and
converts files with upstream's `WavChunkConverter` (`formats/wav.*`).

Device image only: `src/core/ui/fonts/fonts.cpp` (OpenTracker's packed UI
font image) is filtered out by the Makefile and replaced by
`src/ws/device_fonts.cpp`, a 12-byte stand-in that makes the display driver
keep the FT81x ROM fonts (MANUAL.md "Building"). No upstream file is edited.

Also used from this tree: `src/core/formats/wav.*` (WAV import with resampling),
`src/core/formats/patch_pb.*` (synth parameter tables and init patches),
`src/lab/*` (the Shell). Fonts are the FT81x ROM fonts.
