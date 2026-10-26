// PLATFORM native app runtime, C++ part: deleting destructors reference
// operator delete even though apps never use the heap. They trap if called.
#include <stddef.h>

void operator delete(void*) noexcept { for (;;) {} }
void operator delete(void*, size_t) noexcept { for (;;) {} }
void operator delete[](void*) noexcept { for (;;) {} }
void operator delete[](void*, size_t) noexcept { for (;;) {} }
