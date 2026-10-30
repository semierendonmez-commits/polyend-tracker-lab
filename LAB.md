# polyend-tracker-lab

Experimental, separate firmwares for the original Polyend Tracker, built on
upstream OpenTracker (sandlbn/polyend-tracker-re, branch `v2`).

## Kept apart from polyend-tracker-re-semi

This repo is deliberately unrelated to `semierendonmez-commits/polyend-tracker-re-semi`:

- It is not a GitHub fork, so no pull requests can be opened between the two.
- It has no remote pointing at the semi fork, and no semi-fork branches.
- Every image here has its own 6-character id (`PolyendTracker<ID>.ptf`), so on
  the SD card it never collides with upstream's `OTv` or the semi fork's `OTs`.
- One exception, by request: KONSOL carries a copy of the semi fork's pattern
  generators (`firmwares/konsol/src/core/gen/`, Acid, Call & Response, BBCut,
  Turnaround, Genotype, Station) and their test. It is a plain file copy: no
  shared history, and nothing here is ever merged back.

## Layout

```
firmwares/<name>/     a complete, independent OpenTracker tree per firmware
  src/lab/            the lab Shell (intro, settings, firmware update, recorder, MIDI)
  src/ws/             the firmware's workstation
  MANUAL.md           its manual
  release/            its .ptf image
WORKSTATION_GUIDE.md  how a workstation plugs into the Shell
```

The copies are intentionally separate (no shared code between firmwares): a
change in one never breaks another.

## Base

- Upstream `v2` at `1c8a529` (2026-10-30, after 2.2.3: stack guard, SD
  streaming, stereo synths, no-card boot fix, DIN clock timing). The trees
  were first built on `4505834` (UI screens in SDRAM, #66).
- Built with Ubuntu `arm-none-eabi-gcc` 13.2.1; GitHub CI builds with Arm GNU
  14.2.Rel1 like upstream.
- Upstream is licensed under **Apache-2.0** (its `LICENSE`, added in
  `f977f2f`); every firmware tree carries that file. Lab code that borrows GPL-3.0
  sources (see each firmware's THIRD_PARTY.md) makes that image GPL-3.0.

## Updating to a newer upstream

```
git clone https://github.com/sandlbn/polyend-tracker-re /path/to/upstream
tools/upstream_merge.py /path/to/upstream <current base> origin/v2
```

It applies the upstream diff to every firmware tree with a three-way merge and
resolves the conflicts all lab trees share (Makefile, `src/app_main.cpp`,
`tests/perf_baseline.txt`). Then, per firmware: delete `build/sim`, run
`make lab-gate`, copy the image to `release/`; rebuild PLATFORM's stations
(`make stations` in firmwares/platform). Update the base commit above.

## Back to a known-good firmware

Every lab firmware has **Config > Firmware**: it lists the `.ptf` files in
`/Firmware` and installs one. The upstream escape hatches work too: put a
file name into `/Firmware/ot_restore.txt` and power-cycle. Keep only one lab
image plus stock in `/Firmware`: the rear reset button installs a random `.ptf`
from that folder.
