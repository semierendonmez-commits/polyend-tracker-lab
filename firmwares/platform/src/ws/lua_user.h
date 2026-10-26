// PLATFORM -- Lua 5.4 configuration (included by lua.h through LUA_USER_H,
// set in lua_all.cpp). 32-bit integers and float numbers (LUA_32BITS) so the
// image never links the double-precision libm; no time(), clock(), locale or
// stdio: printing goes to the PLATFORM console (lua_host.cpp).
#ifndef PLATFORM_LUA_USER_H
#define PLATFORM_LUA_USER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void platLuaWrite(const char* s, size_t n);
void platLuaWriteLine(void);
unsigned int platLuaSeed(void);
#ifdef __cplusplus
}
#endif

#define lua_writestring(s, l) platLuaWrite((s), (l))
#define lua_writeline() platLuaWriteLine()
#define lua_writestringerror(s, p) platLuaWrite((p), 0)
#define luai_makeseed(L) (platLuaSeed() ^ (unsigned int)(size_t)(L))
#define l_randomizePivot() (platLuaSeed())

#endif
