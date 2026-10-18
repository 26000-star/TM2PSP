# Port architecture (phase 3)

Goal of phase 3: run the race mode of **one environment** on a Windows host, built from decompiled game code plus a
"Nitro-on-host" shim. Phase 4 swaps the shim's back-ends for the PSP (`sceGu`, `sceCtrl`, `sceIo`, `sceAudio`); the
game code does not change.

## Principles

- **A source port, not a matching decompilation.** Decompiled functions are rewritten as readable C++ (the game is
  CodeWarrior C++) with the same behaviour. Byte-matching is not a goal.
- **Behaviour first, layout second.** Class fields keep the original order and their offsets are written in
  comments, so decompiler output (`*(param_1 + 0x1d4c)`) maps onto names. Code that depends on DS memory layout
  (pools, overlays, DMA, caches) is replaced, not emulated.
- **Every ported function is keyed by its dsd address.** A `@port <module>:<addr> <status>` comment marks it in the
  source, and the name goes into `config/arm9/**/symbols.txt`. Ghidra, the call graph and the status report all use
  the same keys.
- **Deviations are explicit.** When the port changes behaviour (bug fix, simplification), the comment next to the tag
  says so and the status is `partial`, or the fix is called out ("port fix").
- The DS is 32-bit little-endian, like the PSP. Data files (Lua bytecode, `.h` collision, ghosts, strings) are read
  unchanged. The host build is 64-bit, so code must not assume `sizeof(void*) == 4` or rely on DS structure offsets
  in memory; offsets stay documentation.

## Source layout

| Path | Content | Origin |
|---|---|---|
| `src/game/` | Decompiled and cleaned game code, one directory per subsystem (`core`, `fs`, `script`, `car`, …) | port |
| `src/shim/include/nitro.h` | The NitroSDK/NitroSystem API surface the game code calls: types (`u32`, `fx32`, …), FS, FX, OS, later GX/G3/SND | shim |
| `src/shim/common/` | Platform-independent SDK replacements (FX math, sin/cos table) | shim |
| `src/shim/host/`, `src/shim/psp/` | Back-ends: stdio/SDL2/OpenGL on the host, `sceIo`/`sceGu`/… on the PSP (phase 4) | shim |
| `src/lib/lua-5.0.2/` | Lua 5.0.2, original source with an integer `lua_Number` (`tm_luaconf.h`), no parser, two 64-bit patches | third party |
| `src/lib/unzip/` | unzip 0.15 API subset re-implemented on zlib + the FS shim, with Firebrand's additions | re-implementation |
| `src/host/` | Host programs (test drivers now, the SDL2 game executable later) | port |
| `tools/host/` | Checks that compare host output with independent references | tooling |

zlib is not vendored: the host uses the system/MinGW `libz`, the PSP uses the pspdev package.

## Build

Host (Windows, Strawberry MinGW-w64 gcc 13 + CMake 3.29 + Ninja, all on PATH):

```bash
cmake -S . -B build/host -G Ninja
```

```bash
cmake --build build/host
```

Targets: `tm_lua`, `tm_shim`, `tm_unzip`, `tm_game` (static libraries) and `tm_cfgdump` (test driver).
`tm_game` collects `src/game/**/*.cpp` automatically. The PSP build will use the same `CMakeLists.txt` through
`psp-cmake` (which defines `PSP`, selecting `src/shim/psp/`).

## Tracking the work

| Step | Tool |
|---|---|
| Find what a feature needs | `python -P tools/analysis/callgraph.py callers <sel>` / `callees <sel>` (sel: name, `0xADDR`, `ov003:0xADDR`) |
| Decompile | `powershell -File tools\ghidra\decompile.ps1 out.c <sel> …` |
| Read literal-pool constants (`DAT_…`) | `python -P tools/analysis/peek.py [ov003] 0206d440 …` (shows strings and symbols) |
| Rename | add `addr Class_Method # note` to `config/names/<topic>.txt`, run `python -P tools/analysis/apply_names.py config/names/*.txt`, then `tools\ghidra\analyze.ps1` + `export.ps1` |
| Port | write C++ in `src/game/…`, tag each function `// @port main:0206d790 done` |
| Report | `python -P tools/analysis/port_status.py [--race-only] [--list]` → `build/analysis/port_status.txt` |

Statuses: `done`, `partial`, `stub`, `lib` (tagged); untagged functions default to `shim` (implemented in
`nitro.h`), `lib` (Lua, MSL), `drop` (code map) or `todo`.

Names in `symbols.txt` use `Class_Method` (dsd symbols cannot contain `::`), with `_ctor`, `_dtor`,
`_dtor_deleting` for special members. CodeWarrior emits two destructors per class with a vtable: the complete one
(vtable slot 0) and the deleting one (slot 1).

## Race-mode inventory

`python -P tools/analysis/callgraph.py race` builds a reference graph from **dsd relocations** (not just Ghidra's
direct calls): every call, every function-pointer load and every data run of relocated words (vtables, `luaL_reg`
tables) is an edge. A constructor that installs a vtable therefore pulls in every virtual method. Roots: `main()`,
`Entry` and the `.ctor`/`.init` tables of main and ov003. Only main, ITCM, DTCM and ov003 (the race overlay) are
followed. Output: `build/analysis/race_inventory.txt` (module, address, mode, action, library, subsystem, size, name;
`[race]` = only reachable once ov003 is loaded).

Result (2026-10-18):

| Action | Library | Functions | Bytes |
|---|---|---|---|
| port | game (3,234 main + 966 ov003) | 4,200 | 682 KB |
| replace | NitroSDK/NitroSystem (177 still unnamed) | 816 | 114 KB |
| replace | Lua 5.0.2 | 314 | 26 KB |
| replace | MSL | 161 | 50 KB |
| replace | zlib/unzip | 48 | 10 KB |
| drop | network (stubs needed at the call sites) | 230 | 28 KB |

So **static reachability barely narrows the game code**: 4,200 of the 4,474 main + ov003 game functions. `main()`
constructs every manager (front end, save, sound, network), and their vtables reach most of the engine. The
references into absent overlays (ov004 front end 160, ov006 GameSpy 80, ov005 26, ov002 23) are the calls the port
must route or stub. To order the work inside these 4,200 functions, the next tool is a **dynamic coverage trace**:
log executed function starts in melonDS (GDB stub breakpoints, or a Lua/DeSmuME script) while loading and driving
one Coast track, then intersect with the inventory.

## Shim design

- `nitro.h` declares the SDK functions with their SDK signatures, so decompiled calls compile unchanged. Inline SDK
  macros that read structure fields directly (`FS_GetLength`, …) become inline functions on the shim's own structs.
- **FS**: the ROM file system root is a host directory (`FSh_SetRoot`). `'\'` and `'/'` are both separators.
  An `FSFile` keeps its own position, so a struct copy is an independent cursor, which the game's unzip relies on.
- **FX**: `FX_Div`, `FX_Inv`, `FX_ModS32`, `FX_Sqrt` reproduce the hardware divider/sqrt results with integer C.
  `FX_SinCosTable_` (4096 sin/cos pairs, fx16) is generated by `FX_Init()` with `round(sin·4096)`.
- **OS**: `OS_Printf`/`OS_Panic` go to stderr. Threads, the loader thread's message queue and the fixed-size object
  pools are not ported: the host and the PSP load synchronously at first.
- Next: GX/G3 (the Firebrand renderer emits G3 commands; host back-end = OpenGL immediate mode, PSP = `sceGu`
  display lists), G2/OAM for 2D, PAD/TP input, SND.

## Vertical slice 1: VFS and Lua configuration (done)

`tm_cfgdump --root extract/files --carparams` loads `Scripts/config/vehicles.luac` exactly like ov003 does and
prints all four cars. `tools/host/check_carparams.py` evaluates the unluac decompilation independently and compares:
**4 cars, 320 values, 0 differences**, identical in VFS mode (`vfs.zip`) and raw mode (`--root assets/vfs --raw`).
All **4,258 files of vfs.zip** read through the ported VFS are byte-identical to Python's `zipfile`.

### File system (main 0x0206c800..0x0206e600, 0x0207a098.., 0x0207e134..)

- `dsFileSystem` singleton (`*0x020c8de4`, 0x1de4 bytes). `EnableVfs(true)` loads `vfs.dat` (`"4556, 4258, 79974"`:
  nodes, files, name bytes) and `vfs.bin`, then opens `vfs.zip` with unzip.
- Paths: `raw:` prefix → Nitro FS; else VFS when enabled. Copied to 128 chars, `/` → `\`, lower-cased.
- `vfs.bin` = root node + 4,556 nodes `{u16 name, next, child, info}` (name offset bit 16 = `info & 0x8000`,
  file index = `info & 0x7fff`) + 4,258 `u32` offsets of central-directory records relative to the central directory
  + lower-case names. The file index is the ZIP entry number. Lookup walks one directory level per `\` component.
- `dsPackedFile` (vtable 0x020beab8, RTTI "dsPackedFile") copies the global unzip state (`unzDup`), seeks with
  `unzGoToFilePos(cd_offset + entry, index)` and opens the entry. `dsRawFile` wraps `FSFile`; `dsMemoryFile` serves
  one RAM buffer registered with `SetMemoryFile`.
- The game's unzip is unzip 0.15 ("Copyright 1998 Gilles Vollant") reading through `FSFile`, plus
  `unzGoToFilePos` (as in later minizip) and Firebrand's `unzDup`/`unzFreeDup`.

### Lua layer (main 0x02054ba4..0x02054e58, 0x020572b0, 0x0205c838, 0x02068f28..0x020694e8)

- `dsScriptManager::Run(name)`: loads `scripts/<name>.lua`, else `.luac`, else `scenes/<name>.dcs[c]`, runs the
  chunk in a **new** `lua_State` (base + table + Firebrand `math` libraries) and returns it; the caller reads globals
  and drops the state. The cache in front of it (0x0205ce70) frees the script data as soon as it has run.
- `dsLuaObject` = `{type, lua_State*, registry ref}`; field/index/iteration helpers push the object from the registry,
  act, and pop. Assignment reuses the copy constructor and leaks the old reference (kept; the state is closed after).
  Port fix: at the end of an iteration the original copies the stack slot *below* the table; the port pushes nil.
- Firebrand `math`: `abs sin cos mod sqrt deg rad` in fx32, with **`math.pi = 3.0`** (0x3000); `sin`/`cos` wrap at
  6.0 and index the SDK table.

### Car parameters (ov003 0x0212bed0..0x0212de00)

- `dsCarParamsTable::Load` reads global `carParams` (cars `coa`, `isl`, `sno`, `sta`: one per environment) and
  `SaveTime` (`"Firebrand19Feb"`), into a `std::map<std::string, dsCarParams>`.
- `dsCarParams` (0x1c0 bytes) registers **78 field descriptors** `{type, int*, loaded value, name, min, max,
  editable}`; `INT` fields (`RPMMin`, `RPMMax`, `NumGears`) store `fx32 >> 12`, the rest raw fx32. `FrontWheel` and
  `RearWheel` are 3-element arrays. The script's `WiimoteMode` key is never read.
- `ComputeDerived` converts speeds from **mph to m/s** (×1831/4096), degrees to radians (×12868/737280, i.e. π/180
  with the real π), `cos(FlatAng)` via the sin/cos table, `Mass × RotInertia`, and percentages (/100).
  Quirks reproduced: `GripOversteer` is overwritten with `Grip`, and `FakePitchFactor` with an unloaded field / 5.

## Next slices

1. **Track loading**: `PieceData` reader (main 0x0209e674, `PieceData/%s/PieceData`, pillars), `Track` reader
   (0x0209fd2c; ov003 0x0211b11c/0x0211b404), `config/tracks` (0x02098aac, ov003 0x0211c5d4), environment assets
   (0x02099e6c: `graphics/tracks/%s/texturebatch.n`, `material.bin`, sky), `.h` collision (`_dup.h` at 0x0209947c);
   confirm the collision grid semantics from that code.
2. **Car physics update** (ov003, consumer of `dsCarParams`), cameras (`config/cameras`), car audio parameters.
3. Renderer: Firebrand's BMD0/G3 path onto an OpenGL host back-end (TMPM from phase 2 for the PSP).
4. Host executable with SDL2 (window, pad), main loop from `main()` (0x02003000).
5. Dynamic coverage trace in melonDS to rank the 4,200 race functions.
