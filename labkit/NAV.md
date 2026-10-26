# LABKIT v2 -- one navigation for every lab firmware

You asked for a common key layout ("ortak tuş düzeni") and whether every
firmware can really fit one. The honest answer: **not all of it, but the part
that matters can.** Each firmware is a different instrument (a clip launcher,
a modular patcher, a granular cloud machine, a pinball table, a script host),
so its pages and edit verbs stay its own. What every firmware shares from now
on is a **common core**: the same keys open the same views -- the parameter
pads, the MAP, the macros, the modulation page and the help -- and inside
those views every key does the same thing everywhere. Each firmware then
*extends* the core with its own pages and keys; the table at the end lists
what each one gives up and how it moves.

## 0. What the original Tracker actually has

The original Tracker's key matrix (`src/hal/input.cpp`, `buttonFromCode`)
delivers **F1..F8** and **25 keys**: I.Param, Perform, File, Config, Master,
S.Load, S.Play, S.Edit, S.Rec, Song, Note, Instr, FX1, FX2, Pattern, Play,
Copy, Insert, Up, Delete, Rec, Shift, Left, Down, Right -- plus the encoder
(turn only) and the 48 pads.

**Aux, Help and the encoder push are not on that matrix.** The simulator draws
them (they exist on other Tracker models), but on your Tracker they never fire.
So LABKIT uses none of them, and the table below flags every firmware feature
that today is reachable only through them.

## 1. The common core (every firmware)

| Key | Does |
| --- | --- |
| **Play** | transport start / stop (Shell) |
| **Shift + Rec** | master (+ stems) recorder (Shell) |
| **Config** | Settings, Firmware update, About (Shell) |
| **Shift + Config** | **HELP**: the page's help, its F-keys and this key card |
| **I.Param** | **PARAM pads**: tap = on / off, hold = only while held |
| **Shift + I.Param** | **MACRO** page |
| **Master** | **MAP**: hold = look and jump (release goes there), tap = stays open |
| **Shift + Master** | **MOD** page (LFOs, random, all routes) |
| MIDI CC | learned CCs move their parameter everywhere (see below) |

Why these keys: Config is the Shell's already, so Shift + Config cannot collide
with any workstation. I.Param ("instrument parameters") is the natural name for
the parameter pads and, in the seven firmwares, only ever opens a page that
another key or F-key opens too. Master is the overview; it too only duplicates
a page key (a mixer reachable by an F-key) where it is used at all. No
firmware loses a function -- only a second way to reach a page, and the MAP
gives every page back in one gesture anyway.

### Inside the LABKIT views (the same everywhere)

| Control | PARAM pads | MAP | MACRO | MOD |
| --- | --- | --- | --- | --- |
| pads | rows 1-3: this page's params, row 4: sources | row 1: pages, rows 2-4: the page's params | 8 faders (cols 1-8), targets (cols 9-12) | routes (rows 1-3), sources (row 4) |
| tap a pad | focus | page / focus | set the fader | select |
| hold pad + encoder | edit that param | edit | fine-turn the macro | -- |
| encoder | the focused param (Shift: fine) | same | macro / target depth | the field / depth |
| Up / Down | previous / next page | param on the page | target row | row |
| Left / Right | previous / next param | page column | field (depth / mode / min / max) | field |
| Delete | focused param to default; with a source held: remove the route | default | remove the target | remove the route |
| Shift + Delete | remove all modulation of the focused param | -- | remove all targets | all routes of a source |
| Insert | -- | -- | rename the macro (keyboard) | -- |
| F-keys | go to the workstation (its pages) | F8: go there and close | F1-F8: pick a macro | F8: close |

**Routes**: on the PARAM pads, hold a **source pad** in the bottom row (LFO1-4,
RND1-2, the macros; Shift + a bottom pad shows the second bank: M7, M8 and the
firmware's own sources such as an envelope or the pinball) and tap parameter
pads. Each tap adds a route; the encoder sets its depth while you hold the
source (and the param, or the last one you tapped). The pads routed from the
held source blink in its colour. Delete removes a route.

**Macros**: the same gesture with a macro pad adds a *target* (8 per macro).
On the MACRO page a target can be an *add* (base + depth x macro) or a
*window* (the param sweeps from MIN to MAX as the macro goes 0 -> 100 %).

**MIDI learn**: on the PARAM pads hold **Shift + a parameter pad** and move a
CC. Delete while still holding forgets it. Learned CCs are saved with the
patch (the registry chunk).

**Value HUD**: any change -- encoder, pad, macro, CC -- shows
`PAGE > Param  value` for a second at the top of the screen.

## 2. Recommended conventions for the firmwares' own pages

Not enforced, but the closer a firmware sticks to them, the less there is to learn:

- **F1..F7 = pages, F8 = the page's action** (Save, Add, Launch). GRAF,
  BULUT, DATAMATICS, FLIPPER and MAKAM already work like this; KONSOL uses
  Shift + F1..F8 for pages.
- **Encoder = value, Shift + encoder = fine**; arrows move the cursor.
- **Delete = default / remove, Insert = add / confirm** (Insert also stands
  in for the encoder push, which the original Tracker lacks).
- Put every user-facing value in the **registry** (`ctx.params()`): it then
  appears on the PARAM pads, in the MAP, in MIDI learn, in macros and routes,
  and in the save chunk without further work.
- Describe the pages with `navPages()` (name, one help line, a line per
  F-key): that is what HELP shows.

## 3. Per-firmware conflicts and migration

"Today" is the firmware as it is now (its `src/ws` was not changed by LABKIT).
The navigation stays **off** until a firmware registers a parameter or
describes its pages, so all seven behave exactly as before until they adopt it.

| Firmware | I.Param today | Master today | Other LABKIT keys | Needs Aux / Help / Push today (not on the hardware) | Migration |
| --- | --- | --- | --- | --- | --- |
| **KONSOL** | Sound page (also on Instr) | Mixer page (also Shift + F6) | free | Help = control map; Aux = keys overlay (Perform does it too); Push = the page's main action | Sound stays on Instr, Mixer on Shift + F6 / the MAP. Move the help map into `navPages()` (Shift + Config). Give each page's Push action an F-key or Insert. Register engine / mixer params per track; export the generators' lanes as sources. Use the browser for the Samples page and kits. |
| **GRAF** | unused | unused | free | Push: Add page, cable polarity, reset a knob, song on / off, mute, load | No key conflict. Register the selected module's knobs (one page per module, re-registered on selection, or a page per module type); the modulation matrix stays GRAF's own, LABKIT routes add on top. Move each Push action to Insert / an F-key. |
| **BULUT** | GRAIN page (also S.Play, F2) | MIX page (also F6) | free | Push: the page's main action (reset, load, recall, mute) | GRAIN on S.Play / F2, MIX on F6. Shift + pads (clouds, scenes, freezes, mutes) are untouched: they only act outside the LABKIT views. Replace the SOURCE page's sample picking with the browser (the complaint). Push actions to Insert / F-keys. |
| **DATAMATICS** | STREAM page (also F2) | MIXER page (also F5) | free | Push: open STREAM, audition, page actions | STREAM on F2, MIXER on F5. The DATA page (any-file data) can use the browser with `exts = ""`. Push actions to F7 / F8 / Insert. |
| **FLIPPER** | unused | unused | free | Help = page help card; Push: launch, audition (Down / F6 also do them) | No key conflict. Move the help cards into `navPages()`. The Note / Instr / FX1 / FX2 / Pattern hold effects stay. Export the ball position and flipper hits as sources (a pinball LFO). |
| **MAKAM** | unused | unused | free | Push: the page action (Rec does it on FILE only) | No key conflict. Move the page action to F8 or Insert. Register the synth / mixer / seyir params; usul / makam pickers stay on their pages. |
| **PLATFORM** | delivered to the script (`key("INSTRPARAMS")`) | delivered to the script | free (Config is the Shell's) | **leaving a script** (Aux held, Shift + Help) and the params page (Shift + Aux) | Keep LABKIT off for scripts by default (`navKeys`: `param = map = kNoKey`), or map the Lua `params` into the registry so scripts get PARAM pads / MAP / MIDI learn for free. Needs a device-reachable exit, e.g. hold Shift + F8. **Stations must be rebuilt** (the `lab.h` layout changed; the station loader refuses old ones). |
| **TILT** | (template) | (template) | free | -- | Start from the LABKIT demo workstation. |

### Moving a key out of the way

A firmware that really needs I.Param or Master for itself moves the LABKIT
view instead of the other way round:

```cpp
void navKeys(kit::NavKeys& k) override {
    k.map = btn::SampleEditor;    // MAP on S.Edit in this firmware
    // k.param = kit::kNoKey;     // or no PARAM pads key at all
}
```

The HELP card always shows the firmware's actual keys, so a moved key is never
a hidden one -- but please keep the defaults wherever possible: the point is
that your hands find the same views on every firmware.

## 4. Pad maps

```
PARAM pads (I.Param)                       MAP (hold Master)
+----------------------------------+       +----------------------------------+
| row 1  params 1-12 of the page   |       | row 1  pages (+ MACRO, MOD)      |
| row 2  params 13-24              |       | row 2  params 1-12 of the page   |
| row 3  params 25-36              |       | row 3  params 13-24              |
| row 4  LFO1-4 RND1-2 M1-M6       |       | row 4  params 25-36              |
|        (Shift: M7 M8 + firmware  |       +----------------------------------+
|         sources)                 |       LED colour = group, brightness =
+----------------------------------+       value (the modulated one), white =
                                           focus.
MACRO page (Shift + I.Param)               Browser (a firmware opens it)
cols 1-8: one fader per macro (top =       rows 1-3: A..Z 0..9 (jump), row 4:
100 %), cols 9-12 rows 1-2: its targets    up dn pgup pgdn back open play fav
                                           sort view cancel load; Shift + pad:
                                           audition at that pitch
```
