# Third-party material in PLATFORM

## Copied source code

| Where | What | Source | Licence |
| --- | --- | --- | --- |
| `third_party/lua/` | Lua 5.4.7 (core + base, coroutine, table, string, math, utf8 libraries; not io, os, debug, package, the interpreter or luac) | https://github.com/lua/lua, tag `v5.4.7` (commit in `third_party/lua/VERSION.txt`) | MIT, `third_party/lua/LICENSE` |

The only change to the Lua sources: `luaconf.h` guards `#define LUA_32BITS 0` with
`#ifndef LUA_32BITS`, so PLATFORM's build (`src/ws/lua_config.h`) selects 32-bit
integers and `float` numbers. Everything else (no stdio, no `time()`, no locale, the
print sink) is configured from outside the Lua files, in `src/ws/lua_user.h`,
`src/ws/lua_config.h` and `src/ws/lua_all.cpp`.

## Algorithms (no code copied)

| Where | Algorithm | Source |
| --- | --- | --- |
| `src/ws/heap.cpp` | Two-level segregated fit allocator (the Lua heap) | M. Masmano et al., "TLSF: a New Dynamic Memory Allocator for Real-Time Systems" (ECRTS 2004); written from the paper |
| `src/ws/engine.cpp` filter | TPT state-variable filter | V. Zavalishin, "The Art of VA Filter Design" (2012) |
| `src/ws/engine.cpp` saw / square | PolyBLEP | V. Valimaki, A. Huovilainen (2007) |
| `src/ws/engine.cpp` reverb | Schroeder / Moorer comb + all-pass reverberator (Freeverb's tuning, at half rate) | M. R. Schroeder (1962); J. Moorer (1979); Jezar's Freeverb (public domain) tuning values |
| `src/ws/engine.cpp` FM | Two-operator phase modulation | J. Chowning (JAES 1973) |
| `src/ws/engine.cpp` `ftan` | Pade approximant of tan(x) | textbook |
| `src/ws/papp.cpp`, `tools/mkpapp.py` | CRC-32 (zlib polynomial) | ISO-HDLC / IEEE 802.3 |
| `examples/scripts/*.lua` | Euclidean rhythms | G. Toussaint (2005) |
| Lua API design | `init` / `redraw` / `key` / `enc` / `grid` / `clock.sync` / `params` | the monome norns script API (concepts and names only; no norns code) |

## From this repository (OpenTracker base)

`src/lab/*` (the Shell), `src/core/formats/wav.*` (WAV import for `engine.load`),
`src/core/fastrun.h`, `src/hal/sdram_cache.*` (cache maintenance after loading code).

## Station apps (release/Apps, built by `make stations`)

Station `.papp` files contain the sibling workstations' own code (read from `../<fw>/src`,
under those firmwares' licences). The TILT station (`tilt.papp`) also contains TILT's
`src/ws/mi/` -- Mutable Instruments Plaits drum models and stmlib by Emilie Gillet, MIT
(`../tilt/src/ws/mi/plaits/LICENSE`, `../tilt/src/ws/mi/stmlib/LICENSE`).
