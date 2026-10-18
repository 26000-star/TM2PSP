/* Build configuration of the game's Lua 5.0.2: lua_Number is a 32-bit int (DS fx32 values in the scripts),
 * so the 32-bit little-endian bytecode in vfs.zip loads unchanged. Force-included in every Lua source file. */
#ifndef TM_LUACONF_H
#define TM_LUACONF_H

#include <stdlib.h>

#define LUA_NUMBER int
#define LUA_UACNUMBER int
#define LUA_NUMBER_SCAN "%d"
#define LUA_NUMBER_FMT "%d"
#define lua_str2number(s, p) ((int)strtol((s), (p), 10))

#endif
