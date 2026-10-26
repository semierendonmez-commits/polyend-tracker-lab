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
**Shift + Rec** (recorder) -- and, once the workstation uses LABKIT v2, the
LABKIT keys (I.Param, Master, Shift + Config; NAV.md). Ids are in `ot::btn`
(`src/core/ui/buttons.h`): F1..F8 under the screen, the module keys
(InstrParams, Performance, File, Master, SampleLoader, SamplePlayback,
SampleEditor, SampleRecorder, Song, Pattern), Note / Instr / Fx1 / Fx2, Copy,
Insert, Delete, Rec, Shift, arrows. `Event::kHold` fires once after 500 ms.
Pads: 48, `pad = row * 12 + col`, row 0 at the top, with velocity.

**EncPush, Aux and Help are not on the original Tracker's key matrix**
(`src/hal/input.cpp`): the simulator has them, the hardware never sends them.
Never make an action reachable only through them -- use Insert or an F-key.
A release (or hold) always goes to whoever received the press, so a note or
modifier the workstation saw always ends there even if a LABKIT view opened
in between; Shift press / release always reach the workstation.

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

## LABKIT v2

LABKIT is the shared navigation of every lab firmware (`src/lab/params.h`,
`mod.h`, `nav.h`, `browser.h`, `keyboard.h`; NAV.md is the user's side). It
turns on as soon as the workstation registers a parameter or describes its
pages; a workstation that does neither runs exactly as before.

### Parameters (`ctx.params()`, in `init()`)

```cpp
kit::Registry& P = ctx.params();
P.setPage(kPgFilter);                         // defaults for the following add()s
P.setGroup("FILTER", Rgb(0xFF8A3D));          // group = colour on the pads
cutoff_ = P.add("flt.cut", "Cutoff", &cut_).range(40, 16000).exp().unit("Hz");
P.add("flt.res", "Reso", &res_).percent();                 // 0..1 shown as %
P.add("osc.wave", "Wave", &wave_).names(kWaveNames, 5);    // int or float, enum
P.add("osc.oct", "Octave", &oct_).range(-2, 2).bipolar();  // int binding = integer
P.add("amp.level", "Level", [](void* c, float db) { static_cast<Me*>(c)->gain_ = dsp::dbToGain(db); },
      this, -6.0f).range(-40, 6).step(0.5f).unit("dB");   // setter binding
P.add("arp.swing", "Swing", &swing_).range(0, 0.5f).percent().pad(12);   // fixed pad
```

- **Key** (first argument): stable, it is what saves store (hashed). Never
  reuse a key for a different meaning; renaming the display name is free.
- **Binding**: `float*` or `int*` into your own state, or a setter. The
  registry writes the *effective* value (base + modulation) there once per
  audio block, before `render()` -- your DSP keeps reading its own fields.
  Pointer bindings are also written at once from the main loop when the
  param is not modulated. **Setters run in the audio interrupt**: keep them
  short (no I/O, no allocation).
- **Curves**: linear (default, `range`), `exp()` (lo > 0), `integer()`,
  `names()` (enum). `step(s)` = encoder step in units (default 1 % of the
  range, Shift = fine), `def(v)` = Delete / reset value (default: the bound
  value at `add()`), `unit()`, `percent()`, `bipolar()`, `noMod()` (never a
  route target), `pad(n)` (0..35 on its page).
- **Edits** from your own UI go through the registry so the HUD, MIDI learn
  and the save chunk see them: `P.nudge(id, e.delta, e.shift)`,
  `P.set(id, value)`, `P.reset(id)`. If you still write a bound field
  directly (preset code, generators), the registry follows it within 50 ms
  (`sync()`); a modulated field is overwritten every block, so modulated
  params must be changed through the registry.
- **Reading**: `get()` / `norm()` (base), `effective()` / `effectiveNorm()`
  (after modulation, for drawing), `format(id, buf, n)`.
- Up to 512 params (`reserve(n)` before the first `add()` to change it).
  Everything lives in the arena; nothing is allocated after `init()`.

### Pages (new `Workstation` virtuals, all optional)

```cpp
int navPages(kit::PageInfo* out, int cap) override;  // name, help line, F-key lines, colour
int navPage() const override { return page_; }       // the page on screen
void navGoto(int page) override { page_ = page; }    // the MAP jumped here
void navFocus(kit::ParamId id) override;             // PARAM pads / MAP focused a param
void navParamChanged(kit::ParamId id) override;      // a Shell view changed a value
void navKeys(kit::NavKeys& k) override;              // move I.Param / Master if you must
```

Params name their page with `setPage()` / `.page()`; without `navPages()` the
pages are called PAGE 1.. . The Shell adds two pages of its own after yours:
MACRO and MOD.

### Modulation and macros (`ctx.mod()`)

- Export your own sources in `init()`: `ctx.mod().addSource("ENV", &env_)`
  (a float you update, 0..1; `bipolar = true` for -1..1). They appear on the
  PARAM pads' second source bank (Shift + a bottom pad) and in the MOD page.
- First-boot patches can be set from code: `mod.setMacroName(0, "BRIGHT")`,
  `mod.route(kit::kSrcMacro + 0, cutoff_, 0.45f)`, `mod.setWindow(i, true, lo, hi)`,
  `mod.route(kit::kSrcLfo + 1, res_, 0.2f)`.
- Cost: the LABKIT block (4 LFOs, 2 random, 8 macros, the routes and the
  registry writes) runs before `render()`; 64 routes on 32 params measure
  ~2,500 M4 instructions per block (`lab.labkit-mod64` in the bench, limit 3,000).

### Saving

One versioned chunk holds the base values, the macro names, the routes and the
MIDI learn table. Put it in your own project file, or use the file helpers:

```cpp
uint32_t n = ctx.params().saveChunk(buf, cap);   // 0 = no room (chunkSize() tells)
ctx.params().loadChunk(buf, n);                 // unknown keys are skipped
ctx.params().saveFile(ctx.fs, path);  ctx.params().loadFile(ctx.fs, path);
```

Load **after** registering your params and sources (in `init()` is fine: the
Shell's own MACRO / MOD params are created on demand). A load marks every
param; the audio side writes them 32 per block.

### The browser (`ctx.browser()`)

```cpp
ctx.browser().reserve(256, 4.0f);   // init(): listing + 4 s preview buffer (0 = no preview)
kit::BrowserRequest q;
q.title = "Load sample"; q.folder = "Samples"; q.exts = ".wav";   // ";"-separated, "" = all
q.ctx = this; q.onPick = [](void* c, const char* path) { /* main loop */ };
ctx.browser().open(q);
```

Full-screen modal; folders, size, WAV length / rate / channels, a thumbnail of
the whole file, auto-preview (mixed into the output by the Shell), favourites,
recent files, sort by name / size / type, pads as letters, Shift + pad =
audition at that pitch. The last folder, favourites and recent files are kept
in `/Lab/<FW>/labkit.bin`. Load the picked file yourself in the main loop
(`update()`), never in `render()`. (IStorage gives no file dates, so there is
no date sort.)

### The keyboard (`ctx.keyboard()`)

```cpp
kit::KeyboardRequest q;
q.title = "Patch name"; q.initial = name_; q.maxLen = 16;
q.ctx = this; q.onDone = [](void* c, const char* text) { /* copy it */ };
ctx.keyboard().open(q);
```

### Drawing helpers

`kit::drawParamKnob(g, theme, P, id, cx, cy, r, focused)` and
`kit::drawParamBar(...)` draw a registered param (base arc, modulated dot,
name, value); `kit::fitText()` / `kit::textWidth()` cut text to a width in a
ROM font. While the PARAM pads are open the Shell keeps ~600..900 display
words for its panel: your page is cut there (not counted as over budget).

### Rules

1. Register in `init()`; edit from the main loop only.
2. A registered field may be *read* in `render()`; write it there only if it
   is not registered (or not modulatable: `noMod()`).
3. Never depend on EncPush / Aux / Help (not on the hardware). Prefer F1..F7
   for pages and F8 for the page's action, Insert for "confirm / add".
4. Keep I.Param and Master free; if you cannot, move LABKIT's keys with
   `navKeys()` and say so in your MANUAL.
5. After syncing a new `src/lab` (`labkit/tools/sync_labkit.sh <fw>`), rebuild
   from clean simulator objects (the sync script removes them: the simulator
   makefile does not track header dependencies for src/lab and src/ws) and
   rebuild PLATFORM's stations (`make stations`): the `lab.h` layout changed.

### Testing

Simulator script commands: `expect-overlay none|param|map|macro|mod|help|browser|keyboard`,
`expect-param KEY VALUE [EPS]`, `expect-effective KEY VALUE [EPS]`,
`expect-routes N`, `expect-learn N`, `cc CH CC VALUE`, `mkdir PATH`,
`mkwav PATH SECONDS [HZ] [CHANNELS]` (see `labkit/sim/lab/check.txt`).
Host tests: `tests/lab/labkit_test.cpp` (registry, routes, macros, save /
load, nav keys, MIDI learn, browser on a fake card, keyboard).
