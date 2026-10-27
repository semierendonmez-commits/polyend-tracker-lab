// BULUT profiling (QEMU bench only: build lab-bench-m4 with -DBULUT_PROF).
// Instructions per render section, read from the MPS2 timer the bench uses.
#pragma once
#include <cstdint>
#ifdef BULUT_PROF
namespace bulut {
extern double gProf[20];
extern double gProfMax[20];
extern int gProfBlocks;
extern uint32_t gProfLast;
extern double gProfBlock[20];
inline volatile uint32_t* profTimer() { return reinterpret_cast<volatile uint32_t*>(0x40000000u); }
}  // namespace bulut
#define PROF_BEGIN() do { gProfLast = profTimer()[1]; for (int q_ = 0; q_ < 20; ++q_) gProfBlock[q_] = 0.0; } while (0)
#define PROF_MARK(k) do { const uint32_t n_ = profTimer()[1]; gProfBlock[k] += double(gProfLast - n_) * 40.0; gProfLast = n_; } while (0)
#define PROF_END() do { for (int q_ = 0; q_ < 20; ++q_) { gProf[q_] += gProfBlock[q_]; if (gProfBlock[q_] > gProfMax[q_]) gProfMax[q_] = gProfBlock[q_]; } ++gProfBlocks; } while (0)
#else
#define PROF_BEGIN() do {} while (0)
#define PROF_MARK(k) do {} while (0)
#define PROF_END() do {} while (0)
#endif
