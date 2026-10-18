/* The game only loads precompiled chunks: the Lua parser (llex.c, lparser.c, lcode.c) is not linked in the ROM.
 * These stubs stand in for the two entry points the core still references. */
#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "llex.h"
#include "lparser.h"

void luaX_init(lua_State *L)
{
    (void)L;
}

Proto *luaY_parser(lua_State *L, ZIO *z, Mbuffer *buff)
{
    (void)z;
    (void)buff;
    luaG_runerror(L, "source chunks are not supported (parser not linked)");
    return NULL;
}
