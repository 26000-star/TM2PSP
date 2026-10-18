# Lua 5.0.2

Unmodified Lua 5.0.2 sources (core, `lauxlib`, `lbaselib`, `ltablib`) from https://www.lua.org/ftp/lua-5.0.2.tar.gz
(md5 `dea74646b7e5c621fef7174df83c34b1`). MIT licence: see the notice at the end of `include/lua.h`.

Patches to the original sources (marked `TM2PSP`), needed for 64-bit hosts; no effect on the PSP:
- `src/llimits.h`: `Instruction` is `unsigned int` instead of `unsigned long` (8 bytes on LP64).
- `src/lundump.c`: `LoadSize` reads the 32-bit `size_t` the game's chunks were dumped with.

Port-specific files:
- `tm_luaconf.h`: integer `lua_Number`, force-included by the build.
- `tm_noparser.c`: stubs for `luaX_init` / `luaY_parser`, because the game links no parser (bytecode only).

The game also registers Firebrand's fixed-point `math` library instead of `lmathlib`; it is game code and lives in
`src/game/script/fxmath.cpp`.
