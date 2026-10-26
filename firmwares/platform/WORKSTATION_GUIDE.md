# Writing a lab workstation

Every firmware in `polyend-tracker-lab` is a full OpenTracker tree whose tracker
App is replaced by the **lab Shell** (`src/lab/`) running one **Workstation**
(`src/ws/`). This file is the contract between the two.

## What the Shell already does

- **Boot intro** — calls `drawIntro(g, ms)` for `introMs()`; any key skips it;
  Settings > Boot intro turns it off.
- **Config button** — opens the Settings page (general rows, then the
  workstation's own rows from `settings()`), **Firmware** (installs any `.ptf`
  in `/Firmware`, so the user can always flash back) and **About**.
- **Play** — transport start/stop. Tempo, MIDI clock out (24 ppqn) or follow
  MIDI clock are Settings rows. `render()` gets a `Transport` with the beat
  position at the block start and beats per sample.
- **Shift + Rec** — master recorder to `/Lab/<FW>/Rec/REC-nnnn.wav`, plus up to
  4 stereo stems when Settings > Record stems is on (`ctx.stem(i, l, r)` inside
  `render()`).
- **Input** — line in / FM radio / mic / off, gain and monitor; the selected
  input arrives in `render()` as `AudioIn`.
- **Output** — master level and limiter after `render()`.
- **MIDI** — DIN + USB in (channel filter, running status) to `onMidi()`;
  `ctx.midiOut()` / `ctx.midiNote()` to both outputs.
- **Pad LEDs** — `leds(pads)` ~30 times a second (48 RGB colours).
- **Files** — `/Lab/<FW>/` exists on the card (`ctx.home`); the Shell's own
  `settings.bin` lives there.

Everything else -- the instrument, pages, sequencing, saving projects -- is the
workstation's.

## Buttons the workstation receives

All buttons except **Config** (settings), **Play** (transport) and
**Shift + Rec** (recorder). Ids are in `ot::btn` (`src/core/ui/buttons.h`):
F1..F8 under the screen, the module keys (InstrParams, Performance, File,
Master, SampleLoader, SamplePlayback, SampleEditor, SampleRecorder, Song,
Pattern), Note / Instr / Fx1 / Fx2, Copy, Insert, Delete, Rec, Shift,
arrows, EncPush, Aux, Help. `Event::kHold` fires once after 500 ms.
Pads: 48, `pad = row * 12 + col`, row 0 at the top, with velocity.

## Rules

1. **No heap after init.** Allocate everything from `ctx.arena` in `init()`
   (about 13 MB is free on the device). No exceptions, no RTTI.
2. **`render()` is the audio interrupt.** No file access, no drawing, no
   printf, no locks, never wait. Main loop <-> audio: plain fields written by
   one side, `volatile` flags, or `SpscRing`.
3. **Float only in per-sample code.** The M4F has a single-precision FPU:
   `double` is software. Use `sinf`, `expf`, `tanf`... (the image check fails
   on the double libm).
4. **CPU:** the worst block of every bench scenario stays under **180,000 M4
   instructions** (`make lab-gate`). Add a `benchSetup()` scenario for each
   heavy mode (all voices, all effects, the busiest sequence).
5. **Display:** a frame stays under `Canvas::kBudgetWords` (1,800) display-list
   words; Canvas counts and drops beyond it. Text costs about its length in
   words; batch many points / lines / rects with `beginPoints()` .. `vtx()` ..
   `endBatch()` (1 word per vertex). For dense pixel graphics write an RGB332
   buffer into RAM_G (`IDisplay::writeRamG`) and draw it with `bitmap332()`.
   Free RAM_G for lab bitmaps: `0x000000..0x06CBFF` (445,440 B) and
   `0x0A3930..0x0FFFFF` (378,576 B); the rest holds the driver's fonts and
   fragments. Upload only what changed: SPI moves ~1 MB/s, so a 61 KB frame
   costs ~60 ms -- keep full-bitmap refreshes to ~10 a second or use small tiles.
6. **Flash:** the image stays under 720,000 B (`.text + .data`); the lab shell
   itself is ~240 KB. Fonts: FT81x ROM fonts 16..31 (`lab::font`).
7. **The card may be missing** (`ctx.fs == nullptr`): every file path must
   handle it. `update()` and `save()` run in the main loop.
8. Hot DSP that runs every block can be tagged `OT_FASTRUN_FN` (`core/fastrun.h`)
   to run from SRAM_L; keep the stack headroom >= 90 KiB (the gate checks it).

## Files of a firmware

```
src/ws/            the workstation (createWorkstation() lives here)
tests/lab/*.cpp    host tests (LAB_TEST), run by make lab-test
sim/lab/check.txt  the simulator check of make lab-gate
sim/lab/shots.txt  screenshots for README / MANUAL (make lab-shots)
MANUAL.md          the user manual
release/           the released .ptf
```

## Commands

```
make LAB_FW=<NAME> LAB_ID=<ID6>          # firmware image build/PolyendTracker<ID6>.ptf
make lab-sim LAB_FW=<NAME>               # simulator: build/sim/lab-sim
make lab-gate LAB_FW=<NAME> LAB_ID=<ID6> # everything a release must pass
make lab-shots LAB_FW=<NAME>             # docs/screenshots/*.png
```

Each firmware's `Makefile` already sets its `LAB_FW` / `LAB_ID`.
