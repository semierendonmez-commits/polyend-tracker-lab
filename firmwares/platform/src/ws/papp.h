// PLATFORM -- the .papp native app image (MANUAL.md "Native apps", tools/mkpapp.py).
//
// A .papp is a statically linked Cortex-M4F program, linked at address 0 with
// --emit-relocs and converted by tools/mkpapp.py into:
//
//   PappHeader (incl. PlatInfo)
//   fast segment   fastSize bytes   code for the fast RAM (SRAM_L), may be 0
//   main segment   imageSize bytes  .text .rodata .init_array .data
//   relocations    relocCount x u32
//
// The main segment is followed by bssSize zero bytes at run time. Every
// absolute address in either segment is listed in the relocations: the word
// holds an offset from the start of its target segment, and the loader adds
// that segment's load address. PC-relative references never cross segments
// (mkpapp.py refuses images where they do), so each segment can be placed
// anywhere. Relocation entry: bits 0..29 byte offset of the word in its
// segment, bit 30 target segment (1 = fast), bit 31 segment of the word
// (1 = fast).
//
// Everything here is plain parsing / copying (no execution), so the host
// tests, the QEMU loader test and the device share it.
#pragma once

#include <cstdint>

#include "platform_app.h"

namespace plat {

constexpr uint32_t kPappVersion = 1;

struct PappHeader {
    char magic[4];          // "PAPP"
    uint16_t version;       // kPappVersion
    uint16_t headerSize;    // sizeof(PappHeader)
    uint32_t abi;           // PLAT_ABI_VERSION
    uint32_t flags;
    uint32_t fastSize;
    uint32_t imageSize;
    uint32_t bssSize;
    uint32_t relocCount;
    uint32_t entry;         // plat_app_entry, offset in the main segment (Thumb bit set)
    uint32_t initArray;     // offset of .init_array in the main segment
    uint32_t initCount;     // entries
    uint32_t imageAlign;    // load alignment of the main segment (power of two, >= 8)
    uint32_t crc;           // CRC-32 (zlib) of every byte after the header
    uint32_t reserved[3];
    PlatInfo info;
};
static_assert(sizeof(PlatInfo) == 176, "PlatInfo layout");
static_assert(sizeof(PappHeader) == 64 + 176, "PappHeader layout");

enum class PappStatus : uint8_t {
    Ok = 0,
    TooShort,
    BadMagic,
    BadVersion,
    BadAbi,
    BadLayout,     // sizes / offsets out of range
    BadCrc,
    BadReloc,      // a relocation points outside its segment
    NoMemory,
    IoError,
};
const char* pappStatusText(PappStatus s);

constexpr uint32_t kRelocFast = 1u << 31, kRelocToFast = 1u << 30, kRelocOffsetMask = (1u << 30) - 1u;

// Checks the header against `fileSize` (no CRC). Fills `h`.
PappStatus pappParseHeader(const uint8_t* file, uint32_t fileSize, PappHeader& h);

// Bytes the loader needs in the SDRAM: the main segment + bss (+ the fast
// segment when it has to live there too), with alignment slack.
uint32_t pappMemoryNeeded(const PappHeader& h, bool fastInSdram);

// Places a parsed image: copies the segments to `main` (imageSize + bssSize
// bytes, bss zeroed) and `fast` (fastSize bytes; may equal any address) and
// applies the relocations using the run-time addresses `mainAddr` / `fastAddr`
// (normally the same as the pointers; the host tests relocate for another
// address). Checks the CRC first when `checkCrc`.
PappStatus pappPlace(const uint8_t* file, uint32_t fileSize, const PappHeader& h, uint8_t* main, uint8_t* fast,
                     uint32_t mainAddr, uint32_t fastAddr, bool checkCrc = true);

// In-place placement for the device loader: `main` already holds the main
// segment (read straight from the file into its final place), `rel` the
// relocation words, `fast` the fast segment. `crc` is the CRC-32 the caller
// computed over the file body while reading. Applies the relocations.
PappStatus pappRelocate(const PappHeader& h, uint8_t* main, uint8_t* fast, const uint32_t* rel, uint32_t mainAddr,
                        uint32_t fastAddr, uint32_t crc);

// Run-time address of the entry point / i-th constructor after pappPlace().
inline uint32_t pappEntryAddr(const PappHeader& h, uint32_t mainAddr) { return mainAddr + h.entry; }

// CRC-32 (zlib) -- the loader's own copy so the QEMU test needs no core sources.
uint32_t pappCrc32(const uint8_t* p, uint32_t n, uint32_t crc = 0);

}  // namespace plat
