// PLATFORM -- Lua 5.4.7 (third_party/lua, MIT) compiled as one C++ unit.
//
// One translation unit keeps every build (device image, simulator, host tests,
// QEMU bench) free of extra Makefile rules. Errors use setjmp/longjmp
// (LUA_USE_LONGJMP), never C++ exceptions. Libraries: base, coroutine, table,
// string, math, utf8 (no io, os, debug, package: the card is reached through
// PLATFORM's own `file` API). Numbers are 32-bit (LUA_32BITS: int32 + float).
#include "ws/lua_config.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "ws/lua_user.h"

// No C stdio on the device: Lua's loadfile/dofile (removed from the globals by
// lua_host.cpp anyway) see a file system with no files.
namespace {
FILE* platNoOpen(const char*, const char*) { errno = ENOENT; return nullptr; }
FILE* platNoReopen(const char*, const char*, FILE*) { errno = ENOENT; return nullptr; }
int platNoGetc(FILE*) { return EOF; }
size_t platNoRead(void*, size_t, size_t, FILE*) { return 0; }
int platNoClose(FILE*) { return 0; }
int platNoErr(FILE*) { return 1; }
}  // namespace
#undef getc
#undef ferror
#undef feof
#define fopen platNoOpen
#define freopen platNoReopen
#define getc platNoGetc
#define fread platNoRead
#define fclose platNoClose
#define ferror platNoErr
#define feof platNoErr

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wdouble-promotion"

#include "../../third_party/lua/lapi.c"
#include "../../third_party/lua/lcode.c"
#include "../../third_party/lua/lctype.c"
#include "../../third_party/lua/ldebug.c"
#include "../../third_party/lua/ldo.c"
#include "../../third_party/lua/ldump.c"
#include "../../third_party/lua/lfunc.c"
#include "../../third_party/lua/lgc.c"
#include "../../third_party/lua/llex.c"
#include "../../third_party/lua/lmem.c"
#include "../../third_party/lua/lobject.c"
#include "../../third_party/lua/lopcodes.c"
#include "../../third_party/lua/lparser.c"
#include "../../third_party/lua/lstate.c"
#include "../../third_party/lua/lstring.c"
#include "../../third_party/lua/ltable.c"
#include "../../third_party/lua/ltm.c"
#include "../../third_party/lua/lundump.c"
#include "../../third_party/lua/lvm.c"
#include "../../third_party/lua/lzio.c"
#include "../../third_party/lua/lauxlib.c"
#include "../../third_party/lua/lbaselib.c"
#include "../../third_party/lua/lcorolib.c"
// math.randomseed() without arguments: no time() on the device.
#define time(p) ((time_t)platLuaSeed())
#include "../../third_party/lua/lmathlib.c"
#undef time
#include "../../third_party/lua/lstrlib.c"
#include "../../third_party/lua/ltablib.c"
#include "../../third_party/lua/lutf8lib.c"

#pragma GCC diagnostic pop
