#!/usr/bin/env python3
"""PLATFORM: convert a native app ELF (linked with sdk/ld/papp.ld and
--emit-relocs) into a .papp image (src/ws/papp.h).

usage: mkpapp.py app.elf out.papp

The image holds the fast segment, the main segment (.text: code, rodata,
init_array, data), and one relocation word per absolute address in either
segment. Only R_ARM_ABS32 / R_ARM_TARGET1 need run-time fixing; PC-relative
branches stay valid because each segment moves as a whole. A PC-relative
reference from one segment into the other is refused (the linker uses
absolute veneers for long calls, which carry R_ARM_ABS32 entries the tool
collects from the veneer symbols). Pure Python, no third-party modules.
"""
import struct
import sys
import zlib

R_ARM_ABS32, R_ARM_REL32, R_ARM_TARGET1 = 2, 3, 38
R_ARM_THM_CALL, R_ARM_THM_JUMP24 = 10, 30
# absolute MOVW / MOVT pairs cannot be patched with one word: refuse them
ABS_MOV = {43, 44, 47, 48}
FAST_BASE = 0x40000000
PLAT_ABI_VERSION = 1
PAPP_VERSION = 2
INFO_SIZE = 180


def die(msg):
    sys.exit("mkpapp: " + msg)


class Elf:
    def __init__(self, data):
        self.d = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            die("not a 32-bit little-endian ELF")
        (self.type, self.machine, _, self.entry, _, self.shoff, _, _, _, _, self.shentsize, self.shnum,
         self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        if self.machine != 40:
            die("not an ARM ELF")
        self.sh = []
        for i in range(self.shnum):
            self.sh.append(struct.unpack_from("<IIIIIIIIII", data, self.shoff + i * self.shentsize))
        strtab = self.sh[self.shstrndx]
        self.names = [self.cstr(strtab[4] + s[0]) for s in self.sh]

    def cstr(self, off):
        e = self.d.index(b"\0", off)
        return self.d[off:e].decode()

    def section(self, name):
        for i, n in enumerate(self.names):
            if n == name:
                return i
        return None

    def body(self, i):
        s = self.sh[i]
        return self.d[s[4]:s[4] + s[5]] if s[1] != 8 else b"\0" * s[5]

    def symbols(self):
        i = next((k for k, s in enumerate(self.sh) if s[1] == 2), None)
        if i is None:
            die("no symbol table")
        s = self.sh[i]
        strs = self.sh[s[6]][4]
        out = []
        for k in range(s[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.d, s[4] + k * 16)
            out.append((self.cstr(strs + name), value, size, info, shndx))
        return out


def main(src, dst):
    data = open(src, "rb").read()
    elf = Elf(data)
    ti, bi, fi = elf.section(".text"), elf.section(".bss"), elf.section(".fast")
    if ti is None:
        die("no .text section (link with sdk/ld/papp.ld)")
    text = bytearray(elf.body(ti))
    taddr = elf.sh[ti][3]
    if taddr != 0:
        die(".text must be linked at 0")
    bss = elf.sh[bi][5] if bi is not None else 0
    if bi is not None and bss:
        gap = elf.sh[bi][3] - len(text)
        if gap < 0 or gap > 8:
            die(".bss must follow .text directly")
        text += b"\0" * gap          # bss starts where the linker put it
    text += b"\0" * ((-len(text)) % 8)
    fast = bytearray(elf.body(fi)) if fi is not None else bytearray()
    if fi is not None and fast and elf.sh[fi][3] != FAST_BASE:
        die(".fast must be linked at 0x40000000")
    fast += b"\0" * ((-len(fast)) % 4)
    main_end = len(text) + bss
    syms = elf.symbols()
    sym = {n: v for (n, v, _, _, _) in syms}
    for need in ("plat_app_entry", "plat_app_info"):
        if need not in sym:
            die(f"symbol {need} missing (PLAT_APP_INFO / plat_app_entry)")

    def seg_of(addr):
        if FAST_BASE <= addr < FAST_BASE + max(len(fast), 1) and fast:
            return 1
        if 0 <= addr <= main_end:
            return 0
        return None

    relocs = []
    # relocation sections that apply to .text / .fast
    for k, s in enumerate(elf.sh):
        if s[1] != 9:   # SHT_REL
            continue
        target = s[7]
        if target not in (ti, fi):
            continue
        in_fast = target == fi
        seg = fast if in_fast else text
        base = FAST_BASE if in_fast else 0
        symtab = elf.sh[s[6]]
        for e in range(s[5] // 8):
            off, info = struct.unpack_from("<II", data, s[4] + e * 8)
            rtype, sidx = info & 0xFF, info >> 8
            if rtype in (R_ARM_ABS32, R_ARM_TARGET1):
                where = off - base
                val = struct.unpack_from("<I", seg, where)[0]
                # thumb bit / addend: find the segment from the value
                tgt = seg_of(val & ~1)
                if tgt is None:
                    # absolute address outside the image (e.g. a weak undefined = 0)
                    name = syms[sidx][0] if sidx < len(syms) else "?"
                    if val == 0:
                        continue
                    die(f"absolute reference to 0x{val:08x} ({name}) outside the app")
                newval = (val - (FAST_BASE if tgt else 0)) & 0xFFFFFFFF
                struct.pack_into("<I", seg, where, newval)
                relocs.append(where | (0x40000000 if tgt else 0) | (0x80000000 if in_fast else 0))
            elif rtype == R_ARM_REL32:
                die("R_ARM_REL32 relocation: build with -fno-pic and no exception tables")
            elif rtype in ABS_MOV:
                die("MOVW/MOVT absolute relocation: do not build apps with -mslow-flash-data / -mpure-code")
            else:
                shndx = syms[sidx][4] if sidx < len(syms) else 0
                if sidx and shndx == 0:
                    die(f"undefined symbol {syms[sidx][0]} (apps may only call the PlatHost table)")
                if rtype in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                    # decode the linked branch: it must stay in its segment (a veneer when crossing)
                    hw1, hw2 = struct.unpack_from("<HH", seg, off - base)
                    sbit = (hw1 >> 10) & 1
                    j1, j2 = (hw2 >> 13) & 1, (hw2 >> 11) & 1
                    i1, i2 = 1 - (j1 ^ sbit), 1 - (j2 ^ sbit)
                    imm = (sbit << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
                    if sbit:
                        imm -= 1 << 25
                    pc = off + 4
                    if rtype == R_ARM_THM_CALL and not (hw2 & 0x1000):   # BLX: ARM target, word aligned
                        pc &= ~3
                    tgt_addr = (pc + imm) & 0xFFFFFFFF
                    tgt = seg_of(tgt_addr)
                    if tgt != (1 if in_fast else 0):
                        die(f"branch at 0x{off:08x} leaves its segment (to 0x{tgt_addr:08x}, {syms[sidx][0]})")
    # the veneers the linker made for cross-segment calls: ldr.w pc, [pc] + .word addr
    for (name, value, size, info, shndx) in syms:
        if not name.endswith("_veneer"):
            continue
        in_fast = FAST_BASE <= value < FAST_BASE + len(fast)
        seg = fast if in_fast else text
        base = FAST_BASE if in_fast else 0
        where = (value & ~1) - base + 4
        if where + 4 > len(seg):
            continue
        val = struct.unpack_from("<I", seg, where)[0]
        tgt = seg_of(val & ~1)
        if tgt is None:
            die(f"veneer {name} to 0x{val:08x} outside the app")
        if any((r & 0x3FFFFFFF) == where and bool(r & 0x80000000) == in_fast for r in relocs):
            continue
        struct.pack_into("<I", seg, where, (val - (FAST_BASE if tgt else 0)) & 0xFFFFFFFF)
        relocs.append(where | (0x40000000 if tgt else 0) | (0x80000000 if in_fast else 0))

    init_start, init_end = sym.get("__plat_init_start", 0), sym.get("__plat_init_end", 0)
    info_off = sym["plat_app_info"]
    info = bytes(text[info_off:info_off + INFO_SIZE])
    if info[:8] != b"PLATINF\0":
        die("plat_app_info is not a PlatInfo record")
    entry = sym["plat_app_entry"]
    if not entry & 1:
        die("plat_app_entry is not Thumb code")
    body = bytes(fast) + bytes(text) + b"".join(struct.pack("<I", r) for r in sorted(set(relocs)))
    flags = struct.unpack_from("<I", info, 172)[0]
    hdr = struct.pack("<4sHHIIIIIIIIIII3I", b"PAPP", PAPP_VERSION, 64 + INFO_SIZE, PLAT_ABI_VERSION, flags, len(fast),
                      len(text), (bss + 7) // 8 * 8, len(set(relocs)), entry, init_start,
                      (init_end - init_start) // 4, 8, zlib.crc32(body) & 0xFFFFFFFF, 0, 0, 0)
    assert len(hdr) == 64
    open(dst, "wb").write(hdr + info + body)
    print(f"mkpapp: {dst}: main {len(text)} B + bss {bss} B, fast {len(fast)} B, {len(set(relocs))} relocations")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
