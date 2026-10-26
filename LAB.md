# polyend-tracker-lab

An experimental, separate firmware for the original Polyend Tracker, built on
upstream OpenTracker (sandlbn/polyend-tracker-re, branch `v2`).

## Kept apart from polyend-tracker-re-semi

This repo is deliberately unrelated to `semierendonmez-commits/polyend-tracker-re-semi`:

- It is not a GitHub fork, so no pull requests can be opened between the two.
- It has no remote pointing at the semi fork, and no semi-fork code or branches.
- The only other remote is `upstream` (sandlbn/polyend-tracker-re), read-only.
- Its firmware image is named `PolyendTrackerOTL<ver>.ptf` (`L` = lab), so on
  the SD card it never collides with upstream's `OTv` or the semi fork's `OTs`.

If an idea from here is ever wanted in the semi fork, it is ported by hand in
that project, never merged from here.

## Base

- Upstream `v2` at `4505834` (UI screens in SDRAM, #66).
- Upstream has no LICENSE file; ask the author before sharing built images.

## Back to a known-good firmware

Put a stock, upstream or semi `.ptf` in `/Firmware` on the SD card and flash it
the usual way.
