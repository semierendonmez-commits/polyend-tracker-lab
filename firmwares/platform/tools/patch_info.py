#!/usr/bin/env python3
"""patch_info.py FILE [--desc TEXT] [--color 0xRRGGBB] -- rewrite the PlatInfo record
(sdk/include/platform_app.h) of a built app in place: a .papp (the record is in
its header, outside the CRC) or a simulator .so (the record in .rodata).
Used by sdk/station/station.mk to put a station's own tagline and theme accent
(read from the workstation by tools/station_info) into its launcher entry."""
import argparse
import struct

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--desc")
ap.add_argument("--color")
a = ap.parse_args()
d = bytearray(open(a.file, "rb").read())
at = d.find(b"PLATINF\0")
if at < 0:
    raise SystemExit(f"patch_info: no PlatInfo record in {a.file}")
if a.desc is not None:
    t = "".join(c if 32 <= ord(c) <= 126 else "?" for c in a.desc).encode()[:95]
    d[at + 56:at + 56 + 96] = t + b"\0" * (96 - len(t))          # magic 8 + name 24 + author 24
if a.color is not None:
    struct.pack_into("<I", d, at + 176, int(a.color, 0) & 0xFFFFFF)  # after icon 16, memKB 4, flags 4
open(a.file, "wb").write(d)
