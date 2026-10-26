# Third-party code and ideas in BULUT

BULUT's own code (`src/ws/`) contains no third-party source files.

| What | Where | Licence / note |
|------|-------|----------------|
| Reverb topology after Mutable Instruments Clouds' FxEngine reverb (Emilie Gillet) | `src/core/audio/effects.cpp` (OpenTracker's `ot::audio::Reverb`, used as BULUT's reverb send) | MIT (stmlib). OpenTracker wrote it from its spec; no Mutable Instruments code is copied. |
| Granular parameter set (position, size, density, pitch, spread, texture, feedback, reverb, freeze) | `src/ws/engine.cpp` | Concept only, after Mutable Instruments Clouds / Beads. Written from scratch; no code taken from `pichenettes/eurorack`. |
| Particle types (pulsar, glisson, trainlet, Gabor grain) and stochastic onset laws | `src/ws/engine.cpp` | Concepts from Curtis Roads, *Microsound* (MIT Press, 2001) and Iannis Xenakis, *Formalized Music*. Written from scratch. |
| OpenTracker base (HAL, display, WAV import / export, delay, reverb) | `src/core`, `src/hal`, `src/lab` | See `OPENTRACKER.md` and the repository README. |
