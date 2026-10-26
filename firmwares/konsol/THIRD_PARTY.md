# Third-party material in KONSOL

KONSOL's workstation (`src/ws/`) contains **no copied third-party source code**:
every file in it was written for KONSOL. It reuses code of this tree and follows
published algorithms and ideas:

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

Also used from this tree: `src/core/formats/wav.*` (WAV import with resampling),
`src/core/formats/patch_pb.*` (synth parameter tables and init patches),
`src/lab/*` (the Shell). Fonts are the FT81x ROM fonts.
