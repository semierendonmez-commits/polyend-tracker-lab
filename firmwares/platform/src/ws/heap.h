// PLATFORM -- a small two-level segregated-fit heap (TLSF-style) over a fixed
// memory region. Used as the Lua allocator: O(1) malloc / free, immediate
// coalescing, a hard cap (the region size), 8-byte aligned payloads.
// Main loop only (never from render()).
#pragma once

#include <cstdint>

namespace plat {

class Heap {
public:
    // `size` is rounded down to 8 bytes; returns false when the region is too small.
    bool init(void* mem, uint32_t size);
    void* alloc(uint32_t bytes);
    void free(void* p);
    // Like C realloc (p may be null; bytes 0 frees and returns null).
    void* realloc(void* p, uint32_t bytes);

    uint32_t capacity() const { return size_; }
    uint32_t used() const { return used_; }        // block bytes in use (headers included)
    uint32_t peak() const { return peak_; }
    uint32_t allocs() const { return count_; }
    uint32_t largestFree() const;
    // Walks every block and checks the links (tests).
    bool check() const;

    static uint32_t blockSizeFor(uint32_t bytes);

private:
    static constexpr int kSlLog = 3, kSl = 1 << kSlLog, kFl = 28;
    static constexpr uint32_t kHdr = 8, kMinBlock = 16, kUsed = 1u;
    // Block layout at offset o: u32 sizeFlags, u32 prevPhysSize, then (free
    // blocks) u32 nextFree, u32 prevFree (offset + 1, 0 = none).
    uint32_t& w(uint32_t off, int i) const { return reinterpret_cast<uint32_t*>(base_ + off)[i]; }
    uint32_t sizeOf(uint32_t off) const { return w(off, 0) & ~7u; }
    bool usedAt(uint32_t off) const { return (w(off, 0) & kUsed) != 0; }
    static void mapping(uint32_t s, int& fl, int& sl);
    void insertFree(uint32_t off);
    void removeFree(uint32_t off);
    uint32_t findFree(uint32_t s);   // offset + 1, 0 = none
    void setSize(uint32_t off, uint32_t s, bool used);

    uint8_t* base_ = nullptr;
    uint32_t size_ = 0, used_ = 0, peak_ = 0, count_ = 0;
    uint32_t flMap_ = 0;
    uint32_t slMap_[kFl] = {};
    uint32_t heads_[kFl][kSl] = {};
};

}  // namespace plat
