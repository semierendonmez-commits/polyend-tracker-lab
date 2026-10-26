/* PLATFORM native app runtime: the newlib-nano system calls. Apps have no
 * stdio, so the file calls are stubs. snprintf("%f") (newlib's dtoa) mallocs
 * small bignum buffers that it keeps: _sbrk serves them from a 32 KB static
 * area in the app's .bss. Apps never call malloc themselves (use host->alloc). */
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>

int _write(int fd, const void* p, size_t n) { (void)fd; (void)p; return (int)n; }
int _read(int fd, void* p, size_t n) { (void)fd; (void)p; (void)n; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int w) { (void)fd; (void)off; (void)w; return -1; }
int _fstat(int fd, struct stat* st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
#define PLAT_LIBC_HEAP 32768
static unsigned char platLibcHeap[PLAT_LIBC_HEAP] __attribute__((aligned(8)));
static unsigned platLibcUsed;
void* _sbrk(int n) {
    if (n < 0 || platLibcUsed + (unsigned)n > PLAT_LIBC_HEAP) {
        errno = ENOMEM;
        return (void*)-1;
    }
    void* p = platLibcHeap + platLibcUsed;
    platLibcUsed += (unsigned)n;
    return p;
}
void _exit(int c) { (void)c; for (;;) {} }
int _kill(int p, int s) { (void)p; (void)s; return -1; }
int _getpid(void) { return 1; }
