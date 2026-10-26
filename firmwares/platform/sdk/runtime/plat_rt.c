/* PLATFORM native app runtime: the few C / C++ ABI symbols a freestanding app
 * may need besides libgcc / libm / libc_nano's pure functions. There is no
 * heap (no malloc, no operator new): allocate with host->alloc in init(). */
#include <stddef.h>

void* __dso_handle = 0;

/* static destructors never run (the app is dropped as a whole) */
int atexit(void (*fn)(void)) { (void)fn; return 0; }
int __aeabi_atexit(void* obj, void (*fn)(void*), void* dso) { (void)obj; (void)fn; (void)dso; return 0; }
int __cxa_atexit(void (*fn)(void*), void* obj, void* dso) { (void)fn; (void)obj; (void)dso; return 0; }
void __cxa_pure_virtual(void) { for (;;) {} }

/* newlib's float libm sets errno through this; one shared int is enough. */
static int platErrno;
int* __errno(void) { return &platErrno; }
