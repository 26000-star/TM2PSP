// Firebrand's fixed-point "math" library for Lua (luaL_reg table at 0x020b8ebc). Numbers are fx32.
// Note: math.pi is 3.0 (0x3000), and the angle helpers use that value.
#include "nitro.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

static fx32 s_degToRad; // data_020c7c04
static fx32 s_radToDeg; // data_020c7c00

static const fx32 FXMATH_PI = 0x3000;

// @port main:02041fc4 done
static int fxmath_abs(lua_State *L)
{
    int v = (int)luaL_checknumber(L, 1);
    if (v < 0)
        v = -v;
    lua_pushnumber(L, v);
    return 1;
}

// @port main:02041f90 done
static int fxmath_sin(lua_State *L)
{
    fx32 a = (fx32)luaL_checknumber(L, 1);
    int i = FX_ModS32(a, 2 * FXMATH_PI);
    i = (i << 16) / (2 * FXMATH_PI);
    lua_pushnumber(L, FX_SinCosTable_[(i >> 4) * 2]);
    return 1;
}

// @port main:02041f58 done
static int fxmath_cos(lua_State *L)
{
    fx32 a = (fx32)luaL_checknumber(L, 1);
    int i = FX_ModS32(a, 2 * FXMATH_PI);
    i = (i << 16) / (2 * FXMATH_PI);
    lua_pushnumber(L, FX_SinCosTable_[(i >> 4) * 2 + 1]);
    return 1;
}

// @port main:02041f2c done
static int fxmath_mod(lua_State *L)
{
    int b = (int)luaL_checknumber(L, 2);
    int a = (int)luaL_checknumber(L, 1);
    lua_pushnumber(L, FX_ModS32(a, b));
    return 1;
}

// @port main:02041f10 done
static int fxmath_sqrt(lua_State *L)
{
    lua_pushnumber(L, FX_Sqrt((fx32)luaL_checknumber(L, 1)));
    return 1;
}

// @port main:02041ed0 done
static int fxmath_deg(lua_State *L)
{
    fx32 k = s_radToDeg;
    fx32 v = (fx32)luaL_checknumber(L, 1);
    lua_pushnumber(L, FX_MUL(v, k));
    return 1;
}

// @port main:02041e90 done
static int fxmath_rad(lua_State *L)
{
    fx32 k = s_degToRad;
    fx32 v = (fx32)luaL_checknumber(L, 1);
    lua_pushnumber(L, FX_MUL(v, k));
    return 1;
}

static const luaL_reg fxmathlib[] = {
    {"abs", fxmath_abs}, {"sin", fxmath_sin}, {"cos", fxmath_cos}, {"mod", fxmath_mod},
    {"sqrt", fxmath_sqrt}, {"deg", fxmath_deg}, {"rad", fxmath_rad}, {NULL, NULL},
};

// @port main:02041e30 done
extern "C" int luaopen_fxmath(lua_State *L)
{
    luaL_openlib(L, "math", fxmathlib, 0);
    lua_pushlstring(L, "pi", 2);
    lua_pushnumber(L, FXMATH_PI);
    lua_settable(L, -3);
    s_degToRad = FX_Div(FXMATH_PI, 180 * FX32_ONE);
    s_radToDeg = FX_Inv(s_degToRad); // 0x0202c4f0 reuses the divider result of the line above
    return 1;
}
