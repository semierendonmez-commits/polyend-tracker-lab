// PLATFORM -- the session pool: a lab::Arena-like bump allocator over the
// memory of the running script / app, with mark / rewind for temporary
// buffers (the loader's relocation words, a .so file copy). Reset as a whole
// when the session closes. Main loop only.
#pragma once

#include <cstdint>
#include <cstring>

#include "lab/lab.h"

namespace plat {

class Pool {
public:
    void attach(uint8_t* base, uint32_t size) {
        base_ = base;
        size_ = size;
        used_ = 0;
    }
    void reset() { used_ = 0; }
    void* alloc(uint32_t bytes, uint32_t align = 16) {
        if (align < 4) align = 4;
        const uint32_t at = (used_ + (align - 1)) & ~(align - 1);
        if (!base_ || at < used_ || at + bytes < at || at + bytes > size_) return nullptr;
        used_ = at + bytes;
        std::memset(base_ + at, 0, bytes);
        return base_ + at;
    }
    uint32_t used() const { return used_; }
    uint32_t free() const { return size_ - used_; }
    uint32_t size() const { return size_; }
    uint8_t* base() const { return base_; }
    void rewind(uint32_t mark) {
        if (mark <= used_) used_ = mark;
    }
    // A lab::Arena over the rest of the pool (for code written against lab::Arena,
    // e.g. a station's createWorkstation): it takes all remaining memory.
    void giveRest(lab::Arena& a) {
        a.attach(base_ + used_, size_ - used_);
        used_ = size_;
    }

private:
    uint8_t* base_ = nullptr;
    uint32_t size_ = 0, used_ = 0;
};

}  // namespace plat
