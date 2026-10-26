// polyend-tracker-lab host test runner (tests/lab/lab_test.h).
#include <cstring>

#include "lab_test.h"

namespace labtest {
Case*& head() {
    static Case* h = nullptr;
    return h;
}
int& failures() {
    static int f = 0;
    return f;
}
}  // namespace labtest

int main(int argc, char** argv) {
    int ran = 0;
    for (labtest::Case* c = labtest::head(); c; c = c->next) {
        if (argc > 1 && std::strstr(c->name, argv[1]) == nullptr) continue;
        const int before = labtest::failures();
        c->fn();
        std::fprintf(stderr, "%s %s\n", labtest::failures() == before ? "ok  " : "FAIL", c->name);
        ++ran;
    }
    std::fprintf(stderr, "%d tests, %d failures\n", ran, labtest::failures());
    return labtest::failures() ? 1 : 0;
}
