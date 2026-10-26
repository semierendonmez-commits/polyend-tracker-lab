// PLATFORM -- Lua build switches (before any Lua header).
#pragma once
#ifndef LUA_32BITS
#define LUA_32BITS 1
#endif
#define LUA_USE_LONGJMP 1
#define LUA_USER_H "../../src/ws/lua_user.h"
#define lua_getlocaledecpoint() ('.')
#define LUAI_MAXCCALLS 80
#define LUA_COMPAT_MATHLIB 0
