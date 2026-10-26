# LABKIT v2 — the shared navigation, parameter, macro and modulation layer

Round 2 of polyend-tracker-lab. The user tried all firmwares (in the simulator / on paper) and
said: navigation is hard in **every** firmware — finding a page, finding a parameter, changing
it, knowing which channel / slot something is on. They chose, for all firmwares:

1. **A common key layout** ("ortak tuş düzeni") — but they asked whether every firmware can
   really fit one layout. Answer that honestly in NAV.md: a common core of verbs that every
   firmware keeps, plus per-firmware extensions, with a conflict table.
2. **A MAP page** ("harita"): hold a key and see every page and every parameter on one screen;
   jump to any of them with a pad.
3. **Pad + encoder parameters**: pads show the current page's parameters; hold a pad and turn
   the encoder to change it; what is selected is always visible. "Use the pads much more."
4. **Modulation by two pads**: hold an LFO (or any mod source) pad + press a parameter pad =
   a modulation route; the encoder sets the depth while both are held.
5. **An open macro system in every firmware**: macros the user builds by assigning any
   parameters to them (with depth / range), reachable from pads and the encoder.
6. Also needed by several firmwares (the user complained about sample loading/selection in
   BULUT; KONSOL needs kits): a **shared sample / file browser** with preview.

Everything lives in `src/lab/` (the Shell), so all eight firmwares (KONSOL, GRAF, BULUT,
DATAMATICS, FLIPPER, MAKAM, PLATFORM, and the new TILT) get the same behaviour.

## Pieces

### Param registry (`lab/params.h/.cpp`)
- A workstation registers its parameters in `init()`: stable id (for saves), short name,
  group, page index, min / max / default, step, curve (linear, exp, int, enum with names),
  unit, and a binding (pointer to float or int in the workstation's own state, or get/set
  callbacks). Up to ~512 params.
- The registry keeps the **base** value (what the user set) and computes the **effective**
  value = base + modulation, clamped; it writes the effective value to the binding at block
  rate (in the audio callback, before `render()`), so DSP code keeps reading its own fields.
  UI edits go through the registry (`set(id, v)` / `nudge(id, delta)`), main loop only.
- Lookup by page for the pad layer and the MAP; formatting helper (`format(id, buf)`).
- Save / load helpers: the registry's base values, the macros and the mod routes as one
  versioned chunk a workstation can put into its own project file (unknown ids skipped).
- Workstations that do not register anything keep working exactly as now.

### Modulation (`lab/mod.h/.cpp`)
- Shell sources: **8 macros**, **4 LFOs** (sine / tri / saw / square / S&H / smooth random;
  free Hz or tempo-synced; phase; retrigger on Play), **2 random** (stepped + drunk walk,
  synced), and **sources a workstation exports** (envelopes, sequencer lanes, pinball ball
  position, input level...) through `registerSource(name, const float* value)`.
- Up to 64 routes (source, param, depth -1..+1, optional range); applied once per audio block;
  cheap: measure the overhead under QEMU (target: < 3,000 M4 instructions per block with 64
  routes) and add it to the bench.
- LFO / random settings are registry params themselves (page "MOD" of the Shell).

### Macros
- 8 macros, each with up to 8 targets (param + depth + optional min/max window) and a name.
- Assign: hold a macro pad + press param pads (or turn param knobs) to add targets; the
  encoder sets depth while held; Delete removes.
- A MACRO page (Shell page) with the 8 macros as big knobs + their targets; the macros also
  appear on the pad layer's bottom row.

### Navigation (`lab/nav.h/.cpp`)
- The workstation describes its pages (`pages()`: names, the param groups on each, a one-line
  help text per page and per F-key) through new virtuals with safe defaults.
- **MAP overlay** (hold the MAP key): all pages as columns / tiles with their params; pads
  mirror the map (row 0 pages, rows 1-3 params of the selected page); pad = jump / focus;
  encoder edits the focused param inside the overlay; releasing the key returns (or tap = latch).
- **PARAM PAD layer** (tap the PARAM key = latch, hold = momentary): pads 0..35 = the
  current page's params (lit by group colour, brightness = value), row 3 = mod sources
  (LFO1-4, RND1-2) and macros 1-6 (exact split is yours); tap = focus; hold + encoder = edit;
  hold source + tap param = route (encoder = depth); the screen shows the focused param, its
  value, its mod depth ring and its routes.
- **Value HUD**: whenever a registered param changes (encoder, pad, MIDI CC learn, macro),
  a small shell-drawn HUD shows `PAGE · Param  value  (+mod)` for ~1 s.
- **HELP**: Help key = overlay with the page's help text and the current key layout.
- **MIDI learn** for any registered param (Shift + hold param pad + move a CC), saved with
  the routes.
- Survey the 7 existing workstations' `src/ws` (read only) for the keys they use today
  (Aux, Help, Instr, Note, Fx1, Fx2, Pattern, Song, Master, Performance, the module keys...)
  and choose the MAP / PARAM / MACRO / HELP keys to collide least. Write the result into
  `NAV.md`: the common core, the chosen keys, and a per-firmware conflict + migration table.

### Sample / file browser (`lab/browser.h/.cpp`)
- A shell modal any workstation opens with a filter (extensions), a start folder and a
  callback: folders, files with size and WAV length / channels / rate, waveform thumbnail,
  **auto-preview** (the Shell mixes a preview voice into the output; reads the first few
  seconds in main-loop chunks into a preview buffer), favourites (stored per firmware),
  recent files, sort by name / date, jump by first letter (pads as letters), Shift = audition
  at the selected pad's pitch. Pads: scroll / select / preview without the screen.
- Main loop only; never blocks the audio; a slow card only slows the list.

### Text entry (`lab/keyboard.h/.cpp`)
- A shell modal for naming things: on-screen keyboard mirrored on the 48 pads, encoder moves,
  Insert / Delete, Shift for case, recent names. Returns via callback.

## Constraints
- Backwards compatible: every existing workstation must still build and pass its own
  `make lab-gate` with the new `src/lab` and **no change to its src/ws** (new virtuals have
  defaults; nothing existing is renamed). PLATFORM's station ABI compares `lab.h` layouts:
  note in your report that stations need a rebuild (they will be rebuilt later).
- Display: overlays must fit the 1,800-word budget on their own (the Shell skips the
  workstation's draw while the MAP is open; HUD and pad-layer overlays are small).
- CPU: mod engine + registry writes under QEMU, as above. No allocation after init;
  everything from the arena.
- Device first: fonts 26..31, ASCII only, 800x480, 48 pads.

## Deliverables (in /home/claude/polyend-tracker-lab/labkit/)
- `src/lab/` — the new shell (all files), `sim/src/lab_main.cpp` if you changed it,
  `tests/lab/labkit_test.cpp` (registry, mod routes, macros, save/load, browser listing on a
  fake card), the template workstation `src/ws/workstation.cpp` rewritten as a small demo
  that uses every feature (it is the starting point of new firmwares).
- `NAV.md` (user-facing common layout + per-firmware table), updated `WORKSTATION_GUIDE.md`
  section "LABKIT v2" (how to register params, pages, sources; how to use the browser and
  the keyboard; rules).
- `tools/sync_labkit.sh <firmware-dir>`: copies the labkit files into one firmware tree.
- Proof: sync into scratch copies of all 7 firmwares (copy each `firmwares/<fw>` to a scratch
  dir first; do NOT touch the real firmware folders) and run each one's `make lab-gate`;
  all must pass. Plus a scratch firmware built from the demo workstation with screenshots of
  MAP, PARAM pads, macro page, a mod route being assigned, the browser and the keyboard.
