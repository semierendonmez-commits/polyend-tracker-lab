// polyend-tracker-lab host tests: a tiny registry. Every tests/lab/*.cpp is
// compiled into build/lab_test (make lab-test, with ASan + UBSan).
//   LAB_TEST(name) { CHECK(cond); CHECK_NEAR(a, b, eps); }
#pragma once

#include <cmath>
#include <cstdio>

namespace labtest {
struct Case {
    const char* name;
    void (*fn)();
    Case* next;
};
Case*& head();
int& failures();
struct Reg {
    Case c;
    Reg(const char* n, void (*f)()) : c{n, f, nullptr} {
        c.next = head();
        head() = &c;
    }
};
}  // namespace labtest

#define LAB_TEST(name)                                         \
    static void lab_test_##name();                             \
    static labtest::Reg lab_reg_##name(#name, &lab_test_##name); \
    static void lab_test_##name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++labtest::failures();                                                       \
        }                                                                                \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                        \
    do {                                                                                             \
        const double va_ = double(a), vb_ = double(b);                                              \
        if (!(std::fabs(va_ - vb_) <= double(eps))) {                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, va_, vb_); \
            ++labtest::failures();                                                                   \
        }                                                                                            \
    } while (0)
