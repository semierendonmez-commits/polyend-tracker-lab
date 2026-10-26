// PLATFORM -- two-level segregated-fit heap (heap.h).
#include "ws/heap.h"

#include <cstring>

namespace plat {

namespace {
inline int fls32(uint32_t v) { return 31 - __builtin_clz(v); }
inline int ffs32(uint32_t v) { return __builtin_ctz(v); }
}  // namespace

uint32_t Heap::blockSizeFor(uint32_t bytes) {
    uint32_t s = (bytes + kHdr + 7u) & ~7u;
    return s < kMinBlock ? kMinBlock : s;
}

void Heap::mapping(uint32_t s, int& fl, int& sl) {
    if (s < 64) {
        fl = 0;
        sl = int(s >> 3);
    } else {
        const int f = fls32(s);
        sl = int((s >> (f - kSlLog)) & (kSl - 1));
        fl = f - 5;
    }
}

bool Heap::init(void* mem, uint32_t size) {
    base_ = static_cast<uint8_t*>(mem);
    size_ = size & ~7u;
    used_ = peak_ = count_ = 0;
    flMap_ = 0;
    std::memset(slMap_, 0, sizeof slMap_);
    std::memset(heads_, 0, sizeof heads_);
    if (!base_ || size_ < 64 || (reinterpret_cast<uintptr_t>(base_) & 7u)) return false;
    setSize(0, size_, false);
    w(0, 1) = 0;
    insertFree(0);
    return true;
}

void Heap::setSize(uint32_t off, uint32_t s, bool used) {
    w(off, 0) = s | (used ? kUsed : 0u);
    const uint32_t next = off + s;
    if (next < size_) w(next, 1) = s;
}

void Heap::insertFree(uint32_t off) {
    int fl, sl;
    mapping(sizeOf(off), fl, sl);
    const uint32_t head = heads_[fl][sl];
    w(off, 2) = head;
    w(off, 3) = 0;
    if (head) w(head - 1, 3) = off + 1;
    heads_[fl][sl] = off + 1;
    flMap_ |= 1u << fl;
    slMap_[fl] |= 1u << sl;
}

void Heap::removeFree(uint32_t off) {
    int fl, sl;
    mapping(sizeOf(off), fl, sl);
    const uint32_t next = w(off, 2), prev = w(off, 3);
    if (next) w(next - 1, 3) = prev;
    if (prev) w(prev - 1, 2) = next;
    else heads_[fl][sl] = next;
    if (!heads_[fl][sl]) {
        slMap_[fl] &= ~(1u << sl);
        if (!slMap_[fl]) flMap_ &= ~(1u << fl);
    }
}

uint32_t Heap::findFree(uint32_t s) {
    // Round up to the next class boundary so any block of the class fits.
    uint32_t r = s;
    if (r >= 64) r += (1u << (fls32(r) - kSlLog)) - 1u;
    int fl, sl;
    mapping(r, fl, sl);
    if (fl >= kFl) return 0;
    uint32_t slm = slMap_[fl] & (~0u << sl);
    if (!slm) {
        const uint32_t flm = fl + 1 < 32 ? flMap_ & (~0u << (fl + 1)) : 0;
        if (!flm) {
            // Last chance: the exact class may still hold a big-enough block.
            mapping(s, fl, sl);
            for (uint32_t b = heads_[fl][sl]; b; b = w(b - 1, 2))
                if (sizeOf(b - 1) >= s) return b;
            return 0;
        }
        fl = ffs32(flm);
        slm = slMap_[fl];
    }
    sl = ffs32(slm);
    return heads_[fl][sl];
}

void* Heap::alloc(uint32_t bytes) {
    if (!base_ || bytes > size_) return nullptr;
    const uint32_t s = blockSizeFor(bytes);
    const uint32_t b = findFree(s);
    if (!b) return nullptr;
    const uint32_t off = b - 1;
    removeFree(off);
    const uint32_t have = sizeOf(off);
    if (have - s >= kMinBlock) {
        setSize(off, s, true);
        setSize(off + s, have - s, false);
        w(off + s, 1) = s;
        insertFree(off + s);
    } else {
        setSize(off, have, true);
    }
    used_ += sizeOf(off);
    if (used_ > peak_) peak_ = used_;
    ++count_;
    return base_ + off + kHdr;
}

void Heap::free(void* p) {
    if (!p) return;
    uint32_t off = uint32_t(static_cast<uint8_t*>(p) - base_) - kHdr;
    uint32_t s = sizeOf(off);
    used_ -= s;
    --count_;
    // merge with the next block
    const uint32_t next = off + s;
    if (next < size_ && !usedAt(next)) {
        removeFree(next);
        s += sizeOf(next);
    }
    // merge with the previous block
    if (off > 0) {
        const uint32_t prev = off - w(off, 1);
        if (!usedAt(prev)) {
            removeFree(prev);
            s += sizeOf(prev);
            off = prev;
        }
    }
    setSize(off, s, false);
    insertFree(off);
}

void* Heap::realloc(void* p, uint32_t bytes) {
    if (!p) return alloc(bytes);
    if (!bytes) {
        free(p);
        return nullptr;
    }
    const uint32_t off = uint32_t(static_cast<uint8_t*>(p) - base_) - kHdr;
    const uint32_t have = sizeOf(off);
    const uint32_t s = blockSizeFor(bytes);
    if (s <= have) {   // shrink in place
        if (have - s >= kMinBlock) {
            setSize(off, s, true);
            uint32_t rest = have - s, roff = off + s;
            const uint32_t next = off + have;
            if (next < size_ && !usedAt(next)) {
                removeFree(next);
                rest += sizeOf(next);
            }
            setSize(roff, rest, false);
            w(roff, 1) = s;
            insertFree(roff);
            used_ -= have - s;
        }
        return p;
    }
    // grow in place into a free next block
    const uint32_t next = off + have;
    if (next < size_ && !usedAt(next) && have + sizeOf(next) >= s) {
        const uint32_t total = have + sizeOf(next);
        removeFree(next);
        if (total - s >= kMinBlock) {
            setSize(off, s, true);
            setSize(off + s, total - s, false);
            w(off + s, 1) = s;
            insertFree(off + s);
            used_ += s - have;
        } else {
            setSize(off, total, true);
            used_ += total - have;
        }
        if (used_ > peak_) peak_ = used_;
        return p;
    }
    void* q = alloc(bytes);
    if (!q) return nullptr;
    std::memcpy(q, p, have - kHdr);
    free(p);
    return q;
}

uint32_t Heap::largestFree() const {
    uint32_t best = 0;
    for (uint32_t off = 0; off < size_; off += sizeOf(off))
        if (!usedAt(off) && sizeOf(off) > best) best = sizeOf(off);
    return best > kHdr ? best - kHdr : 0;
}

bool Heap::check() const {
    uint32_t off = 0, prevSize = 0, used = 0;
    bool prevFree = false;
    while (off < size_) {
        const uint32_t s = sizeOf(off);
        if (s < kMinBlock || off + s > size_) return false;
        if (w(off, 1) != prevSize) return false;
        const bool f = !usedAt(off);
        if (f && prevFree) return false;   // two free neighbours = missed merge
        if (!f) used += s;
        prevFree = f;
        prevSize = s;
        off += s;
    }
    return off == size_ && used == used_;
}

}  // namespace plat
