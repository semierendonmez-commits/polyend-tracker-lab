// PLATFORM -- .papp parsing and relocation (papp.h).
#include "ws/papp.h"

#include <cstring>

namespace plat {

const char* pappStatusText(PappStatus s) {
    switch (s) {
        case PappStatus::Ok: return "ok";
        case PappStatus::TooShort: return "file too short";
        case PappStatus::BadMagic: return "not a .papp file";
        case PappStatus::BadVersion: return "unknown .papp version";
        case PappStatus::BadAbi: return "built for another PLATFORM ABI";
        case PappStatus::BadLayout: return "corrupt image layout";
        case PappStatus::BadCrc: return "checksum mismatch (damaged file)";
        case PappStatus::BadReloc: return "bad relocation";
        case PappStatus::NoMemory: return "not enough memory";
        case PappStatus::IoError: return "card read error";
    }
    return "?";
}

uint32_t pappCrc32(const uint8_t* p, uint32_t n, uint32_t crc) {
    crc = ~crc;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

PappStatus pappParseHeader(const uint8_t* file, uint32_t fileSize, PappHeader& h) {
    if (!file || fileSize < sizeof(PappHeader)) return PappStatus::TooShort;
    std::memcpy(&h, file, sizeof h);
    if (std::memcmp(h.magic, "PAPP", 4) != 0) return PappStatus::BadMagic;
    if (h.version != kPappVersion || h.headerSize != sizeof(PappHeader)) return PappStatus::BadVersion;
    if (h.abi != PLAT_ABI_VERSION) return PappStatus::BadAbi;
    const uint64_t body = uint64_t(h.fastSize) + h.imageSize + uint64_t(h.relocCount) * 4u;
    if (uint64_t(h.headerSize) + body != fileSize) return PappStatus::BadLayout;
    if (h.imageSize == 0 || (h.imageSize & 3u) || (h.fastSize & 3u)) return PappStatus::BadLayout;
    if (h.imageAlign < 8 || (h.imageAlign & (h.imageAlign - 1)) || h.imageAlign > 4096) return PappStatus::BadLayout;
    if (h.bssSize > 64u * 1024u * 1024u) return PappStatus::BadLayout;
    if (!(h.entry & 1u) || (h.entry & ~1u) >= h.imageSize) return PappStatus::BadLayout;
    if (h.initCount && (h.initArray & 3u || uint64_t(h.initArray) + uint64_t(h.initCount) * 4u > h.imageSize))
        return PappStatus::BadLayout;
    return PappStatus::Ok;
}

uint32_t pappMemoryNeeded(const PappHeader& h, bool fastInSdram) {
    uint32_t n = h.imageSize + ((h.bssSize + 7u) & ~7u) + h.imageAlign;
    if (fastInSdram) n += h.fastSize + 16u;
    return n;
}

PappStatus pappPlace(const uint8_t* file, uint32_t fileSize, const PappHeader& h, uint8_t* main, uint8_t* fast,
                     uint32_t mainAddr, uint32_t fastAddr, bool checkCrc) {
    if (fileSize != h.headerSize + h.fastSize + h.imageSize + h.relocCount * 4u) return PappStatus::BadLayout;
    const uint8_t* body = file + h.headerSize;
    if (checkCrc && pappCrc32(body, fileSize - h.headerSize) != h.crc) return PappStatus::BadCrc;
    const uint8_t* fastSrc = body;
    const uint8_t* mainSrc = body + h.fastSize;
    const uint8_t* rel = mainSrc + h.imageSize;
    // Check every relocation before touching memory.
    for (uint32_t i = 0; i < h.relocCount; ++i) {
        uint32_t r;
        std::memcpy(&r, rel + i * 4u, 4);
        const uint32_t off = r & kRelocOffsetMask;
        const uint32_t segSize = (r & kRelocFast) ? h.fastSize : h.imageSize;
        if (uint64_t(off) + 4u > segSize) return PappStatus::BadReloc;
        if ((r & kRelocToFast) && !h.fastSize) return PappStatus::BadReloc;
    }
    if (h.fastSize) std::memcpy(fast, fastSrc, h.fastSize);
    std::memcpy(main, mainSrc, h.imageSize);
    std::memset(main + h.imageSize, 0, h.bssSize);
    for (uint32_t i = 0; i < h.relocCount; ++i) {
        uint32_t r;
        std::memcpy(&r, rel + i * 4u, 4);
        uint8_t* at = ((r & kRelocFast) ? fast : main) + (r & kRelocOffsetMask);
        uint32_t w;
        std::memcpy(&w, at, 4);
        w += (r & kRelocToFast) ? fastAddr : mainAddr;
        std::memcpy(at, &w, 4);
    }
    return PappStatus::Ok;
}

PappStatus pappRelocate(const PappHeader& h, uint8_t* main, uint8_t* fast, const uint32_t* rel, uint32_t mainAddr,
                        uint32_t fastAddr, uint32_t crc) {
    if (crc != h.crc) return PappStatus::BadCrc;
    for (uint32_t i = 0; i < h.relocCount; ++i) {
        const uint32_t r = rel[i];
        const uint32_t off = r & kRelocOffsetMask;
        const uint32_t segSize = (r & kRelocFast) ? h.fastSize : h.imageSize;
        if (uint64_t(off) + 4u > segSize || ((r & kRelocToFast) && !h.fastSize)) return PappStatus::BadReloc;
    }
    std::memset(main + h.imageSize, 0, h.bssSize);
    for (uint32_t i = 0; i < h.relocCount; ++i) {
        const uint32_t r = rel[i];
        uint8_t* at = ((r & kRelocFast) ? fast : main) + (r & kRelocOffsetMask);
        uint32_t w;
        std::memcpy(&w, at, 4);
        w += (r & kRelocToFast) ? fastAddr : mainAddr;
        std::memcpy(at, &w, 4);
    }
    return PappStatus::Ok;
}

}  // namespace plat
