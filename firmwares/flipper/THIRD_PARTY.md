# Third-party material in FLIPPER

FLIPPER's workstation (`src/ws/`) contains **no copied third-party source code**.
Every file was written for FLIPPER. Published algorithms it follows:

| Where | Algorithm | Source | Licence / status |
| --- | --- | --- | --- |
| `flip_audio.cpp` pluck | Karplus-Strong plucked string (noise burst into a delay with a two-point average) | K. Karplus, A. Strong, "Digital Synthesis of Plucked-String and Drum Timbres", CMJ 7(2), 1983 | Published paper; no code |
| `flip_audio.cpp` reverb | Four-line feedback delay network with a Hadamard mixing matrix and input all-passes | J.-M. Jot, A. Chaigne (1991); J. O. Smith, "Physical Audio Signal Processing" (online book) | Published method; no code |
| `flip_audio.cpp` lead, filter sweep | TPT state-variable filter; PolyBLEP saw (via `src/lab/dsp.h`) | V. Zavalishin, "The Art of VA Filter Design" (2012); Valimaki & Huovilainen (2007) | Published methods; no code |
| `flip_audio.cpp` bell / zap | Two-operator phase modulation | J. Chowning, "The Synthesis of Complex Audio Spectra by Means of Frequency Modulation", JAES 21(7), 1973 | Published paper; no code |
| `flip_audio.cpp` `fsin` | Parabolic sine approximation with one refinement step | Folklore (devmaster.net forum post by "Nick", 2006) | Public knowledge; no code copied |
| `flip_game.cpp` rails | Euclidean rhythms (via `lab::dsp::euclid`) | G. Toussaint, "The Euclidean Algorithm Generates Traditional Musical Rhythms" (2005) | Published paper |
| `flip_game.cpp` | Markov-chain step sequencing with per-cell transition weights, flippers, bumpers, ramps, multiball, tilt, ball save, entropy and next-step preview | Concepts of the "Pinball 2" Max for Live device (the user's own renewed device) and of pinball machines in general | Ideas only; no code |

Code FLIPPER uses from this tree (OpenTracker base, same repository):
`src/core/formats/wav.*` (WAV import with resampling, for the sampler),
`src/core/fastrun.h` (SRAM placement of hot functions), `src/lab/*` (the Shell).
