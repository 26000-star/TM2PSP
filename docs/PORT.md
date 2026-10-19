# Port architecture (phases 3 and 4)

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
| `src/game/` | Decompiled and cleaned game code, one directory per subsystem (`core`, `fs`, `script`, `car`, ‚Ä¶) | port |
| `src/shim/include/nitro.h` | The NitroSDK/NitroSystem API surface the game code calls: types (`u32`, `fx32`, ‚Ä¶), FS, FX, OS, later GX/G3/SND | shim |
| `src/shim/common/` | Platform-independent SDK replacements (FX math, sin/cos table) | shim |
| `src/shim/host/`, `src/shim/psp/` | Back-ends: stdio/SDL2/OpenGL on the host, `sceIo`/`sceGu`/‚Ä¶ on the PSP (phase 4) | shim |
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

Targets: `tm_lua`, `tm_shim`, `tm_unzip`, `tm_game` (static libraries), `tm_cfgdump` and `tm_trackdump` (test
drivers).
`tm_game` collects `src/game/**/*.cpp` automatically. The PSP build will use the same `CMakeLists.txt` through
`psp-cmake` (which defines `PSP`, selecting `src/shim/psp/`).

## Tracking the work

| Step | Tool |
|---|---|
| Find what a feature needs | `python -P tools/analysis/callgraph.py callers <sel>` / `callees <sel>` (sel: name, `0xADDR`, `ov003:0xADDR`) |
| Decompile | `powershell -File tools\ghidra\decompile.ps1 out.c <sel> ‚Ä¶` |
| Read literal-pool constants (`DAT_‚Ä¶`) | `python -P tools/analysis/peek.py [ov003] 0206d440 ‚Ä¶` (shows strings and symbols) |
| Rename | add `addr Class_Method # note` to `config/names/<topic>.txt`, run `python -P tools/analysis/apply_names.py config/names/*.txt`, then `tools\ghidra\analyze.ps1` + `export.ps1` |
| Port | write C++ in `src/game/‚Ä¶`, tag each function `// @port main:0206d790 done` |
| Report | `python -P tools/analysis/port_status.py [--race-only] [--list]` ‚Üí `build/analysis/port_status.txt` |

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
  macros that read structure fields directly (`FS_GetLength`, ‚Ä¶) become inline functions on the shim's own structs.
- **FS**: the ROM file system root is a host directory (`FSh_SetRoot`). `'\'` and `'/'` are both separators.
  An `FSFile` keeps its own position, so a struct copy is an independent cursor, which the game's unzip relies on.
- **FX**: `FX_Div`, `FX_Inv`, `FX_ModS32`, `FX_Sqrt` reproduce the hardware divider/sqrt results with integer C.
  `FX_SinCosTable_` (4096 sin/cos pairs, fx16) is generated by `FX_Init()` with `round(sin¬∑4096)`.
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
- Paths: `raw:` prefix ‚Üí Nitro FS; else VFS when enabled. Copied to 128 chars, `/` ‚Üí `\`, lower-cased.
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
- `ComputeDerived` converts speeds from **mph to m/s** (√ó1831/4096), degrees to radians (√ó12868/737280, i.e. œÄ/180
  with the real œÄ), `cos(FlatAng)` via the sin/cos table, `Mass √ó RotInertia`, and percentages (/100).
  Quirks reproduced: `GripOversteer` is overwritten with `Grip`, and `FakePitchFactor` with an unloaded field / 5.

## Vertical slice 2: track loading (done)

`tm_trackdump --root extract/files [--track easy/0] --pieces --collision --lighting` loads the Coast piece
catalogue, one race scene, the collision mesh of every model it uses and the lighting entries.
`tools/host/check_track.py` re-reads the unluac decompilations and the `.h` files independently: **all 26 Coast
tracks (practice, easy, medium, hard, extreme, bonus) give 0 errors**, and VFS and raw modes give identical output.
Besides field-by-field comparisons, the check transforms every collision vertex with the dumped piece matrix and
requires it to fall inside the piece's footprint cells, which validates the rotation and pivot rules geometrically.

### Race setup and grid (main 0x0209fc8c, 0x0209fcd0, 0x020a1170..0x020a1200)

- `dsRaceSetup` (`*0x020d6a30`): mode +0x10, environment +0x20 (0 sta, 1 isl, 2 sno, **3 coa**), time of day +0x24
  (rise/day/set), difficulty +0x28 (practice/easy/medium/hard/extreme), track number +0x2c, cell size +0xa4/+0xa8.
- Cells: **Coast 16 √ó 4 units** (Island 64 √ó 8, others 32 √ó 8). Grid **22 √ó 20 √ó 22** cells on Coast and Island,
  20¬≥ elsewhere. Cell sizes go through software doubles everywhere: `d2i(i2d(n << 12) + 0.5)` (MSL `_dadd`
  0x0201201c with 0.5 = `0x3fe00000`), which `dsRoundD` reproduces.

### Piece catalogue (main 0x0209e674, singleton `*0x020d6644`)

- `PieceData/<env>/PieceData` (global `PieceData`, 149 pieces on Coast) ‚Üí `dsPieceInfo` (0x58 bytes): Name, ID,
  Type (normal/start/finish/checkpoint/multilap), Width/Height/Depth in cells, CostMem/CostPoly, HasGround/HasAir,
  BelowGnd+Cullable, IsPillar+HasPillars, Helper, DesignerTPV, DisableNotch, NotchData, IsLooseTPV, ColumnNotches,
  InitDir, InitPosAir/Ground, TriggerSize (default per environment: Coast 16 √ó 14 √ó 7).
- Model names: `<name>ground` / `<name>air`; a piece with neither flag uses its name for both (for the air one only
  when not BelowGnd, or on Island/Coast). Pieces without ID get the first free id in 1..349. Snow links
  `<name>pillar1/2/4/8`. One Coast piece has an empty name (id 149).
- Ground level `+0x38` = 3 on Coast: a piece placed above cell y = 3 uses its air model (pillars down to the sea).

### Track scenes (ov003 0x0211b404; front-end header reader main 0x0209fd2c)

- `Scenes/<env>/<difficulty>/<n>.dcsc`, global `Track`: `{"Environment", e}`, `{"TimeOfDay", t}`,
  `{"AuthorTime", s}`, `{"AuthorName", "‚Ä¶"}`, `{"LapCount", n}`, then `{name, rotation, x, y, z}` per piece, in cells
  (fx32 integers). Medal times: gold = author + 2 s, silver = gold + 3 s, bronze = silver + 5 s (0x0209fa5c).
- Position = cell √ó cell size (rounded fx multiply with the fraction dropped, then `dsRoundD`); y gets + 0.35
  (0x59a) + a per-environment offset that is zero everywhere.
- **Coast and Island skip their terrain pieces** (`wcliff*`/`dcliff*` and `beach*`, six ids, ov003 0x02100d7c /
  0x02100438): the game rebuilds the terrain itself (`dsTerrain_Build`, ov003 0x02100b2c, not ported yet).
- Placement (ov003 0x0211f088 + 0x0211f2f0): model matrix = `MTX_RotY43_` of rotation √ó œÄ/2 (sin/cos table index
  `65535¬∑a/2œÄ >> 4`, so a quarter turn gives cos = 6/4096, as on the DS), translated to the origin cell. A W √ó D piece
  grows towards **‚àíx and +z** from its origin cell; rotations 1/2/3 move the origin by (D‚àí1)/(W‚àí1) cells so the
  footprint stays on the same cells. Cell index of a position = `FX_Div(pos, cell) >> 12` (0x0211fe08).
- Bit-packed tracks (editor, download play; ov003 0x0211a1f0) store per piece 8+2 bits id, 2 bits rotation and three
  5-bit cell coordinates; they use the same placement code (not ported: race tracks are Lua).

### Collision meshes (main 0x02065c04, 0x02065804, 0x02073500, 0x02065b2c; format in ASSETS.md)

- The collision world (`*0x020c8e78`, vtable 0x020bd390) creates shapes: +0x10 plane (type 3), +0x14/+0x18 mesh
  (type 2) bound to a shared `dsCollisionMesh` resource from the resource cache (0x0205bcb0). Pair tests dispatch
  through a type √ó type table (0x0205b3c0); segment batches run in ITCM (0x01ffb28c).
- ITCM holds Firebrand's own math (0x01ffa6ec..0x01ffab20: matrix copy/identity, vector √ó 3√ó3 truncating, magnitude,
  normalise with the divider and sqrt units started together), ported in `src/game/math/dsMath.cpp`.

### Lighting (ov003 0x0211c5d4, main 0x02098aac)

- `Scripts/config/tracks` global `tracks["<env>_<tod>"]`: eight RGB colours (fx32 0..1) at a 16-byte stride, then
  two scalars (entry 24 √ó 41/4096 ‚Üí +0xe0, entry 25 ‚Üí +0xdc). Meaning of each colour to be found with the renderer.

## Vertical slice 3a: collision queries (done)

`tm_physics --root extract/files --track easy/0 --raycast` builds the collision world of a Coast track (one mesh shape
per placed piece, placed with the piece matrix) and casts batches of vertical and slanted probes over every piece,
the way the car casts its wheel and body probes. `tools/host/check_raycast.py` reads the `.h` files itself and
intersects every probe with every triangle: **26 Coast tracks, 16,722 probes, 15,292 hits, 0 errors** (11 probes
graze a triangle edge and are not counted). Hits match on the set of pieces, material, normal (8/4096) and distance
(1/64 unit against the game's own plane and local ray, max(0.1, 0.5 %) against the exact world-space plane).

- **SDK rounding fixed in the shim**: `FX_Div`/`FX_Inv` round the 32.32 quotient, `(q + 2^19) >> 20`
  (`FX_GetDivResult` 0x0202c584); `FX_Sqrt` is the 64-bit root of `x << 32` rounded `(r + 0x200) >> 10`. Slice 1 and
  2 checks are unchanged (they do not depend on the last bit). `MTX_Inverse43` reproduces the SDK's mix of rounded
  first-row cofactors and truncated products.
- World (`*0x020c8e78`): the car sets the masks (vtable +0x24: include 1, exclude 0), adds up to 10 segments
  (`{origin, unit dir, length, hit list}`, +0x28 keeps their bounding box) and tests them (+0x2c): shapes are visited
  from the last added; mesh shapes take the whole batch (+0x1c), other shapes one segment at a time (+0x00).
- Mesh batch test (itcm 0x01ffbb3c): the batch box is moved to the mesh's space (inverse matrix), the triangles of
  the grid cells it overlaps are gathered once (itcm 0x01ffb834, y/x/z order, all triangles without a grid), each
  probe is moved to local space too (12 scratch slots in the shape, so at most 12 probes per batch), and each pair
  goes through the ray/triangle test; the nearest hit per probe is appended with its world point, the triangle's
  stored normal rotated back, its material and the shape.
- Ray/triangle (itcm 0x01ffc3c4): front faces only (`n¬∑d < -16`, raw 64-bit dot products), barycentric bounds
  without division, `t = (n¬∑s ¬∑ (2^56 / -n¬∑d)) >> 44` using the 64/64 divider started at the top. The local probe
  direction is truncated to 1/4096, so grazing hits on walls move by up to 0.05 unit, as on the DS.
- Plane shape (type 3, 0x0205b510/0x0205b19c): `n¬∑p = d`, used for the Coast sea level; hit t from `FX_Div`.
- Not ported yet: the single-segment mesh query 0x020673c4 (grid DDA 0x01ffe3ec, used by cameras), pair tests
  between shapes (0x0205b3c0).

## Vertical slice 3b: engine, gearbox, wheels, body box (done)

`tm_physics --root extract/files --engine` builds the engine of the four cars and prints their gear tables, their
torque curves in every gear and the body box of each skin; `tools/host/check_engine.py` recomputes them from the
unluac decompilation of `vehicles.luac` and reads the boxes with the phase-2 BMD0 parser: **4 cars, 96 lines,
0 errors**.

- Engine (ov003 0x0212ee38..0x0212f248, 0x64 bytes): RPMMin/RPMMax, shift-up at RPMMax - 100. `NumGears` (6) counts
  reverse: gear 0 = reverse (TopSpeedRev, TimeRev), 1..5 forward (TopSpeed1..5, Time1..5; TopSpeed6/Time6 unused
  by the Coast car). Per gear: ratio = top speed / top rpm, start rpm = previous gear's top speed / ratio, torque =
  top ¬∑ ratio ¬∑ (1 - start/top rpm) / time, shift-down rpm = start - 500. Coast car: top speed 69.6 m/s.
- Torque falls off below a gear's start rpm as `(1 - t)^3` where the original subtracts t from the **integer 1**
  (`rsb r3, r12, #1`), not 1.0: kept. Above the shift-up rpm it fades to 0 at RPMMax.
- Automatic box: up-shift above the shift-up rpm, down-shift below the gear's shift-down rpm when the lower gear
  would stay under the shift-up rpm; GearChangeGap blocks the drive force after a shift; after 0.25 s nearly
  stopped, throttle/brake switch between first and reverse.
- Wheels (0x5c bytes): hub and probe in car space, compression target clamped to ¬±WheelTravelPhys/2, compression
  follows instantly when it grows and relaxes at 2.0/s, clamped to [WheelTravelBot, WheelTravelTop]; spin angle.
  The car model takes the wheel positions from FrontWheel/RearWheel; wheel radius = hub height.
- Body box (main 0x0204c1a4): the first model's box in `Cars/<car>/<skin>/lod1` scaled by posScale (Coast:
  1.97 x 1.18 x 4.45 m), centred on (0, H/2, 0); the snow car's box is shortened.
- The tick time is a global (*0x020d6a78) set once to FX_Inv(60.0) = 0x44 by a static initialiser (main
  0x020b32a0); see the race progress section for how ticks relate to frames.

### Slice 3c: car dynamics step (done)

`src/game/car/dsCarDynamics.*` holds the 0x554-byte simulation object (it starts with its own copy of
`dsCarParams`), its construction (sea level 2.5 on Coast, speed limit 180.6), wheel and engine setup, reset and
placement, and the whole step `dsCarDynamics_Step` (0x021222fc) split by topic: `dsCarCollide.cpp` (0x02124fa0 and
its probes/contacts/impulse), `dsCarForces.cpp` (0x02127f1c), `dsCarIntegrate.cpp` (0x02129098, 0x02129ce4),
`dsCarGrip.cpp` (0x02129f04, 0x0212a848, 0x0212a9c0), `dsCarSurfaces.cpp` (0x02122b54), `dsCarStep.cpp`
(0x021222fc, 0x02127374). Shared fixed-point idioms are in `car/dsCarMath.h`.

`tm_physics --root extract/files --drop` places the Coast car one unit above the start platform of coa/easy/0,
lets it settle for 180 ticks (1/60 s, the game's tick), drives at full throttle for 300 ticks and brakes for 120
(until the race progress slice the host ran 1/30 s steps; the behaviour and timings are unchanged).
`tools/host/check_drop.py` checks the behaviour (no DS trace exists yet to compare with): **0 errors**, the car
settles level on four wheels (height 21.98 ¬± 0.008), reaches 15 m/s in 1.4 s, shifts to 2nd, stops when braking
and then reverses. Two runs give identical output.

- Step order: donut blend, then for player/AI input (modes 0, 3) `UpdateInputs` (0x02127af8), `Drive`
  (0x0212a9c0: engine update, drive direction along the front wheels' ground), `Grip` (0x02129f04: gravity,
  lateral grip, skid drag, brakes, rolling resistance), `Integrate` (0x02129098: steering yaw, air control, tilt,
  two rotations about the body centre, the yaw one about the rear axle when grounded, then position); then
  `Collide` (except replay mode 2), `Surfaces`, `Forces`, one row re-orthonormalised per step, `UpdateWheels`.
- Collision pass: 4 wheel probes (downwards through each wheel, length 0.75..1.5 growing with fall speed and spin)
  and up to 6 body probes (centre to the four corners, a roof probe, one along the velocity from the previous
  centre), one batch. Ground plane = mean of the grounded wheels' contacts, minus the most deviating normal when
  3-4 wheels touch. Body hits are averaged into one impulse `-(1+e) vn / (1/m + |r x n|^2 / I)`, penetration
  first resolved as a position push; against walls the normal is pulled into the ground plane (WallEnforcer), only
  the yaw part of the angular impulse stays, and WallTurnFactor/WallTurnMax/WallSlowdown apply.
- Suspension (Forces): per grounded wheel, force = contact normal x compression (vertical on ground flatter than
  FlatAng), torque = force x lever (lever x scaled by SuspAngForce, z by SuspAngForce raised towards 30 on slopes);
  damping along the ground normal; self-righting; angular damping per car axis between AngDampAir and the ground
  values (AngDampRelease when relaxing).
- Exactness: products are 64-bit rounded (`+0x800 >> 12`) except where the original multiplies on 32 bits
  (lerps by WallEnforcer/SlopeClimb, booster gravity), vectors are scaled by `FX_InvFx64c` 32.32 reciprocals
  (bits 32..63 of `v * k + 2^31`), `MTX_Concat43`, `dsVec_MulMtx33/43` and the axis-angle matrix truncate.
  `FX_AcosIdx` is a binary search of the cosine table (added to the shim); the random generator is MT19937
  seeded with 0 at reset, 12-bit outputs.
- Known quirks kept: the ramp-pitch test checks wheel 2 twice; the wall test `|fwd . n| > -0.707` is always true;
  the stuck test uses `_dcmpgt(up.y, 0.0)`; `cos(pi)` makes the steep-front-ground branch of Drive dead code.
- Not ported: replay modes 1/2 targets (+0x424/+0x448), the visual update 0x02126d10 except the wheel surfaces,
  sparks and debug lines, the owner vehicle's booster flag. Boosters (material 7) need the race grid lookup
  (main 0x0208d404): the port takes it as a callback (`pieceRotationAt`), served by `dsTrack::PieceRotationAt`,
  which walks the piece footprints instead of keeping the game's per-cell byte grid (partial).

## Coast/Island terrain (ov003 0x02100998, 0x02100480, 0x02100750, 0x02100554)

`dsTrack::BuildTerrain` recreates the cliffs and beaches the scene loader skips. For every grid cell of the
ground level (3) that is empty and has a "solid" piece in its 3 x 3 neighbourhood (a placed, non-terrain piece,
not BelowGnd, with a ground model), the 3 x 3 solid mask is matched against 15 patterns (ov003 0x02134e8c: 0/1 or
0xff = any, centre byte = terrain kind) under the four quarter turns (index tables 0x02134e68); the first match
gives the piece (Coast: wcliffcorner, wcliffdeadend02, wcliffbase1a (base rotation 3), wcliffbase1a,
dcliffcrossa, wcliffbase5; table 0x02135bcc) and rotation = base + turn. Pieces stand at (x * 16, 0.35, z * 16).
`tm_trackdump --terrain` + `tools/host/check_terrain.py` (patterns and names read from the ROM, scene and
catalogue from the unluac output): **26 Coast tracks, 0 errors** (44 to 100 terrain pieces each). The editor
path (piece budget, replacing earlier terrain pieces) is not ported. Grid lookups walk piece footprints
(`dsTrack::PieceAtCell`, stand-in for main 0x0208d3c4).

## Slice 4a: host race window (first pass)

`tm_race --root extract/files [--track easy/0]` opens an SDL2 window (768 x 576) and runs the ported code at 30 Hz
(two 1/60 s ticks per frame): track scene and piece placement, collision world, car parameters, engine, the full
car step and the race progress, driven from the keyboard (arrows; Backspace = last checkpoint, Enter = restart). `--frames N --shot out.bmp [--auto]` renders N steps off-screen and saves the
last frame, which is how the renderer was checked (see `docs/host_race.png`: easy/0, 2 s after "go" at full
throttle, from the game's Chase1 camera, with the host HUD).

- **SDL2** 2.30.12 is built as a static library from the official source release, whose GPG signature was checked
  against Sam Lantinga's key (fingerprint 1528 635D 8053 A57F 77D1 E086 30A5 9377 A776 3BE6), into
  `build/ext/sdl2` (not in git). The MinGW development archive has no signature, hence the source build. CMake
  skips `tm_race` when SDL2 is missing.
- **G3D stand-in** (`src/shim/host/g3d_gl.*`): MDL0 render commands (node, material, shape, scale up/down) and GX
  display lists replayed as OpenGL 1.x primitives; all DS texture formats decoded (tex4x4 included); polygon
  attributes give culling (DS front faces are counter-clockwise in GL terms) and alpha. Track pieces take their
  materials from `material.bin` + `texturebatch.n` (format in ASSETS.md).
- Start, respawn, countdown, checkpoints, laps and finish: dsRaceProgress (next section); the race state is drawn
  by a host HUD stand-in (5 x 7 bitmap font: clock, checkpoints, laps, countdown, messages), also in the window
  title, and the events go to stdout.
- Car: `lod1` is drawn (body at the physics scale). `lod0` holds one wheel and tread, the shadow quads and a
  large "cone" shape. The visual update (0x02126d10: steering angle WheelTurnVisual x steer x the steer scale,
  wheel spin, wheel surfaces, per-wheel matrices `wheelMatrix`/`wheelSideMatrix` from the display matrix +0x1f0)
  is ported. 0x020fbe48 only stores these matrices in the car model (0xa8 bytes per wheel, matrices at +0xc and
  +0x3c); the drawing code that uses them is not found yet, and lod0's shapes do not match the lod1 scale
  (body 2.6 high at up_scale 2), so tm_race does not draw wheels yet.
- Not ported yet (stand-ins in tm_race): main loop (`main()` 0x02003000) and the game state machine, the camera
  manager (start/finish/replay cameras), DS lighting/fog/toon (flat GL colours), sky and sea (a flat quad at
  2.5), wheels, HUD, sound. tm_race builds the terrain before the collision world.

## Race progress: triggers, checkpoints, laps, finish, respawn

`src/game/race/dsTrigger.*` and `dsRaceProgress.*` port what the race manager dsRace (*0x02137854), the race game
mode (vtable 0x02136828) and the HUD clock do for the local player. `tm_trackdump --triggers` +
`tools/host/check_triggers.py` (catalogue from unluac, gate geometry and 49 crossing probes per gate recomputed in
Python): **26 Coast tracks, 0 errors**. `tm_physics --progress` + `tools/host/check_progress.py` (countdown with the
real car, then gates crossed by moving the car: finish too early, checkpoints, repeated checkpoint, respawn, laps,
finish, a car left under the sea): **26 tracks, 0 errors**.

- **Tick**: `*0x020d6a78` = FX_Inv(60.0) = 0x44, constant. The race update (0x02113048) steps every car `ticks`
  times (0x0212bd68 -> dsCarDynamics_Step) and passes `ticks` to the players, the clock and the timers, so a frame
  drawn every second VBlank is two 1/60 s ticks. The host now does the same (it ran one 1/30 s step before).
- **Triggers** (0x02111280, 0x021114e4): every finish and multilap piece gets a "finish" trigger, every checkpoint a
  "cp<n>" trigger numbered in piece order; the start piece of a lap track (first start or multilap piece, main
  0x0208cdcc) takes the next number, so the count +0x134 includes it. Trigger matrix = piece matrix with row 0
  scaled by TriggerSize.x, row 1 by TriggerSize.y, translation + InitPos (turned by the piece) + TriggerSize.z on
  y: **TriggerSize is the width, height and height offset of a gate**, not a box (Coast default 16 x 14 raised 7;
  the Coast finish declares 14 x 3). Stadium piece 0x10 is also turned about its forward axis (not ported).
- **Gate** (0x02130358, 0x021304dc): plane through the centre with the unit z row as normal; per player the sign of
  the last test point (-4095 / 0 / 4096). A test hits when the sign changed, the point is within half the diagonal
  of rows 0/1 of the centre (dsVec_Distance, itcm 0x01ffa9d4) and, moved by the inverse matrix (main 0x02057db4:
  inverse of a scaled 4x3 matrix), lies in (-0.55, 0.55) on x and y. Crossing either way counts; the corners of the
  rectangle are cut by the sphere. Triggers keep an "inside" bit per player so two hits in a row count once.
- **Test point** (0x02121738): car position + 3 x unit velocity, local player only. The sides are reset to the car's
  position at every player reset (0x0210e124 -> 0x02121ac8), so a respawn never crosses a gate. The original reads
  the player's car entity (+0x58 matrix, +0x10c velocity copied from the dynamics); the port reads the dynamics.
- **Checkpoint** (listener 0x0213018c -> dsRace 0x02112f8c -> mode 0x0211527c): only while its pending flag +0xf8
  is set; the mode counts it (+0x44) and moves the respawn point to the piece's start point (Coast: pieces 0x3c and
  0xe8 4 units higher; 2-cell wide pieces turned 2 or 3 quarters 16 units along x).
- **Finish** (0x021222dc -> 0x02112f50 -> mode 0x02115704): a finish track needs all checkpoints (else "N
  checkpoints missed", HUD 0x02104620), then the clock stops (0x02104670). A lap track needs the car to have left
  the start piece (+0x23; piece under the car 0x02112360: cell = FX_Div rounded to the nearest, then above/below,
  then 7 units ahead) and every "cp<n>" passed; each pass counts a lap, acts as a checkpoint (respawn point) and
  re-arms the checkpoints; the last lap finishes.
- **Reset** (mode vtable +0x20, 0x02115af4): full = triggers recreated, start point (0x02114a6c: InitPos/InitDir of
  the start piece), 3 s countdown (0x3000; 0.1 s when *0x020d6a70 is set), controls released at 0. Respawn = car at
  the last respawn point, clock running; with no checkpoint passed, or after the finish, it is a full reset. The car
  matrix comes from 0x0211c184: ground probe from 2 units above (10 long), direction snapped to the axes,
  dsMtx43_FromAxes.
- **Sea** (0x02115940): more than 2 units under the sea level -> 180 ticks later a respawn; the car's stuck flag
  (owner +0xd5) also requests one. The race update handles the request at the start of the next frame.
- Not ported: HUD drawing (lights, clock, split times 0x0210e9fc, messages), sounds, ghosts/replays, the pause
  button, network modes, medals/records. The trigger list's per-camera update (vtable +0x1c) is visual only.

## Chase camera (config/cameras)

`src/game/race/dsChaseCamera.*` ports the race chase camera (vtable 0x020c00b8, constructor main 0x0208fe10):
`tm_cfgdump --cameras` + `tools/host/check_cameras.py` (fields from unluac, spring table from the ROM): **8 cameras,
238 damping lookups, 0 errors**; `tm_physics --camera` + `tools/host/check_camera.py` (Coast Chase1 behind the real
car: countdown, full throttle, braking into reverse): **0 errors** on easy/0, practice/0, medium/2, hard/0,
extreme/0. tm_race draws through it (C switches Chase1/Chase2; restart and respawn reset it).

- **Loader** (0x0208fa18): `cameras[<env>]`, keys `Chase<kind>` + Height (+0x6c), Distance (+0x74), LookAtHeight
  (+0x78), TiltTightness (+0x7c), FOV (+0x2c, degrees; 70 everywhere), DirTightness (+0x9c), DistInc/DistDec
  (+0xa0/+0xa4), RollMode (+0xdc, integer: 2 on Coast Chase1, 1 on Chase2) and DistTightness (stiffness of the
  distance spring +0xa8). The Bumper* keys belong to another camera.
- **Spring** (0x020abb68/0x020abb78/0x020abc38): `vel += ((target - pos) k - vel d) dt`, `pos += vel dt`, with the
  damping d looked up in a table of 30 (stiffness, damping) pairs (0x020c1d08, about 2 sqrt(k)), extended linearly
  below the first pair and from the middle pair above the last one (0x020abc68).
- **Update** (0x0208e694, once per frame, tick time dt = 1/60 even when a frame is two ticks): the distance is
  Distance + spring x DistInc (target 1 while the car accelerates along its forward axis by more than 0.5 with the
  throttle on, or flies or boosts) or x DistDec (-1 when braking); a reverse blend goes to +1 (2/s) when the car
  moves backwards faster than 0.3, which swings the camera round the car through a smoothstep, on the side it is
  already on. The desired point (Height + 2 x smoothstep of the airborne blend above the car) is followed with
  DirTightness x dt (raised to 6 in the air while boosting). The up vector follows the car's up (RollMode 1: the
  ground-projected right axis crossed with forward) with TiltTightness x dt, blends back to the world up while the
  car is upside down, and is rolled by the 20-frame average of the ground normal's change along the car (pitch
  bumps, clamped at -0.3 rad). The final position keeps the distance in the plane across that up, Height above the
  car, at least 0.5 above the sea when the car is under it; the camera looks at the car + LookAtHeight x up
  (RollMode 0/1 re-derive the up from the look direction; 0 also levels it), frame built by itcm 0x01ffabfc.
- The car side comes from the player: display matrix (dynamics +0x1f0, copied to the car entity +0x58 by
  0x0210e278), velocity +0x378, acceleration +0x514, throttle +0x468, turbo timer +0x4ec, ground normal +0x220, and
  "airborne" = player vtable +0x2c (0x0210eac8 -> 0x021226f8: no wheel down and no body hit with a normal above
  0.8).
- Assumptions: RollMode 0 divides |up.y| by 0.1 (operands not visible in the decompilation); tm_race takes FOV as
  the full vertical angle. The base camera's fade and FOV ramp (0x0208e524) and the camera manager that picks and
  blends cameras (ov003 0x020fb0f0 / 0x020fb3a4 / 0x020fb450: start, finish, replay cameras) are not ported.

## Bilan de la phase 3 (rapport, 2026-10-19)

**Objectif atteint : une course Coast se joue sur l'h√¥te Windows.** `tm_race --root extract/files --track easy/0`
(n'importe laquelle des 26 pistes Coast) charge la piste et son terrain, place la voiture sur le d√©part, lance le
compte √† rebours de 3 s, puis on pilote au clavier (fl√®ches) la vraie physique du jeu (pas de 1/60 s, deux par image
√† 30 Hz) vue par la cam√©ra poursuite du jeu (`config/cameras`, C pour Chase1/Chase2) ; les checkpoints, les tours,
l'arriv√©e (¬´ N CP manqu√©s ¬ª si on en saute), le respawn au dernier checkpoint (Retour arri√®re, ou automatiquement
quand la voiture coule ou reste bloqu√©e) et le red√©marrage (Entr√©e) suivent le code du jeu ; un HUD de substitution
affiche chrono, checkpoints, tours, compte √† rebours et messages.

Chaque tranche est v√©rifi√©e contre une r√©f√©rence ind√©pendante (lecture Python des scripts unluac, des `.h`, des
tables de la ROM) : carParams (320 valeurs), pistes/collisions/√©clairage (26 pistes), terrain (26), requ√™tes de
collision (16 722 sondes), moteur (4 voitures), chute/acc√©l√©ration/freinage, triggers (26 pistes, 49 sondes par
porte), progression de course (26 pistes), cam√©ras (8 + 238 recherches) et comportement cam√©ra : 0 erreur partout.

Reste pour une fid√©lit√© compl√®te (non bloquant pour jouer) : le HUD du jeu (sprites, feux, temps interm√©diaires),
l'√©clairage DS (`config/tracks`), le ciel et la mer, les roues/ombre de la voiture, le gestionnaire de cam√©ras
(d√©part, arriv√©e, replay), la boucle `main()` et les menus, le son, et une trace melonDS pour comparer la physique
au bit pr√®s. La phase 4 (back-ends PSP du shim) peut d√©marrer sur cette base.

## Next slices

1. The camera manager (ov003 0x020fb450: start/finish/replay cameras, BumperCam), the race grid itself (main 0x0208cb80 + the per-cell bytes), car/car
   contacts (the +0x248 sums), and a dynamic trace of `dsCarDynamics_Step` in melonDS to compare the port bit for
   bit (the drop test only checks behaviour).
2. Renderer, faithful pass: Firebrand's model drawing (dsModel, visual update 0x02126d10 for wheels/shadow),
   DS lighting from `config/tracks` (the eight colours), sky and sea from the environment loader (main
   0x02099e6c), then the same G3 path on the PSP (TMPM from phase 2).
3. Main loop from `main()` (0x02003000) and the game state machine around the race mode (menus, results);
   the game's HUD (lights, clock, checkpoint times from the HUD object *0x02137830) in place of tm_race's
   bitmap-font stand-in.
4. Dynamic coverage trace in melonDS to rank the 4,200 race functions.

## Phase 4: PSP back-ends

Goal: the same game code (`src/game` unchanged) running on the PSP (PSP-3000 target, 64 MB, extended partition),
first proven bit-exact against the host, then drawn with `sceGu` and driven with `sceCtrl`.

### Build (step 1, done)

`bash tools/psp_build.sh` runs psp-cmake (pspdev, gcc 15.2, in the WSL distro Ubuntu-24.04) on the root
`CMakeLists.txt` in `~/tm2psp-build/game` inside the distro (much faster than `/mnt/c`), then copies each EBOOT to
`build/psp/<program>/EBOOT.PBP`. Release build (`-O2` from the toolchain file's defaults), 0 warnings. `PSP` selects
`src/shim/psp`; zlib is pspdev's `libz`. EBOOTs are made with `MEMSIZE 1` (PARAM.SFO: the extended 52 MB user
partition on a PSP-2000/3000) and `PSP_HEAP_SIZE_KB(-1024)` (the whole partition minus 1 MB as heap).

- `fs_psp.c`: the FS shim on sceIo. Same path rules as the host (root + relative path, `\` or `/`; FAT is
  case-insensitive like Windows). The handle is shared between `FSFile` copies (the game's unzip copies the struct
  as a second cursor), so it remembers the OS file position and seeks only when the cursor differs.
- `os_psp.c`: `OS_Printf` to stderr (PPSSPP log / PSPLink), `OS_Panic` prints and returns to the XMB.
- Portability fixes needed in the game code: none. newlib's `int32_t` is `long` on the Allegrex, 32 bits like
  `int`, so the drivers' `%d` formats on `s32`/`fx32` print the same (gcc's -Wformat warns about them in the PSP
  race driver, where it is silenced); the code already avoided `sizeof(void *) == 4` assumptions for the 64-bit host.

### Bit-exact check on the PSP (step 2, done)

`src/checks/suite.cpp` runs the three host test drivers (`cfgdump.cpp`, `trackdump.cpp`, `physicstest.cpp`, each
compiled once more with `main` renamed, see `src/checks/*_entry.cpp`) in one process over a list of checks
(`python -P tools/host/run_checks.py --list`), writing each check's stdout to `<out>/<name>.txt`; between checks the
collision world is cleared and the file system singleton deleted, so each check starts as a new process would. The
host entry is `tm_checksuite`, the PSP one the `tm_checks` EBOOT (`src/psp/checks_psp.cpp`: data next to the EBOOT,
`files/` = `extract/files`, `checks.txt`, output in `out/`, `out/suite.log` written last).

`powershell -ExecutionPolicy Bypass -File tools/psp/run_checks_psp.ps1` runs the host suite, installs the EBOOT,
the list and the ROM files in PPSSPP's memstick (`PSP/GAME/TM2PSPCHK`, never a real PSP), waits for the log,
compares the dumps byte for byte (`tools/psp/compare_checks.py`) and runs the host checkers on the PSP dumps.
`tools/host/run_checks.py` runs every host check (114: car parameters, cameras, engine, ray casts, drop test, and
for the 26 Coast tracks pieces/collision/lighting, terrain, triggers and race progress, plus 5 camera runs).

Result (2026-10-19, PPSSPP 1.20.4): **114 dumps, 2,137,856 bytes, 0 differences; host checkers: 0 errors on the PSP
dumps.** The suite's in-process outputs are also identical to the separate host runs. Fixed point is exact on the
Allegrex as expected (no floats in the game logic). Time: 4.3 s on the host, 104 s of emulated PSP time (PPSSPP
runs the CPU at the PSP's 222 MHz by default; real-hardware speed unverified): a progress run (~1,000 ticks of car
physics plus loading) takes 2.2-3.5 s, a track load with collision meshes ~0.45 s. Heap in use between checks:
72 KB (no leak across 114 checks).

Finding: on the PSP, `freopen` on `stdout` leaks one sceIo descriptor per call (newlib/cglue); after 59 calls opens
fail, and the game's loaders then hang. The suite assigns newlib's `stdout` to an `fopen`ed file instead (host:
`freopen`). `FSh_OpenFiles()` (both back-ends) counts the files open through the FS shim; `suite.log` records it
with the heap in use after each check: 0 everywhere.

### Race on the PSP (step 3, first pass done)

`build/psp/tm_race/EBOOT.PBP` (`src/psp/race_psp.cpp`) plays the Coast race: the same `RaceSession`
(`src/app/race_session.*`, taken out of the host `tm_race`: loading, the 30 Hz frame of two 1/60 s ticks, events,
camera reset/update, HUD stand-in layout and font) drawn by the G3D stand-in on sceGu (`src/shim/psp/g3d_gu.cpp`)
at 480 x 272, 8888 colour, 16-bit depth.

- Controls: d-pad (full lock, as on the DS) or analog stick (proportional beyond a dead zone of 24/128: a PSP
  addition), X accelerate, square brake/reverse, triangle last checkpoint, start restart, select other chase
  camera, R timing overlay (update / list build / GE wait in ms). 30 Hz: each frame is shown at the second VBlank.
- Data: `files/` next to the EBOOT (= `extract/files`), optional `track.txt` ("easy/0"). Auto test: `auto.txt`
  "<track> <frames>" drives at full throttle, saves the last frame to `shot.bmp` and writes `race_log.txt`.
- **Check**: `powershell -ExecutionPolicy Bypass -File tools/psp/race_shot.ps1` runs it in PPSSPP and compares the
  car and camera state after 150 frames of coa/easy/0 with `tm_race --frames 150 --auto` on the host:
  **identical**; the frame (`docs/psp_race.png`) shows the same scene as the host (wider: 16:9 instead of 4:3).
- G3D on sceGu: display lists are converted on first draw into triangle lists per (material, shape) with the
  material colour and alpha baked in; textures to swizzled 5551 (lossless for DS colours) or 8888 (partial alpha);
  the GE has no mirrored repeat, so flipped axes are stored mirrored at twice the size; textures above 512 are
  halved. Culling: GU front faces are counter-clockwise as GL_CCW on the host. The sea is a 24 x 24 grid of
  24-unit tiles around the camera (a single huge quad leaves the GE's guard band and is dropped). Near plane 0.5
  (host 0.25) for the 16-bit depth buffer.
- Screenshots under PPSSPP: the frame must be copied out of VRAM with `sceDmacMemcpy` (PPSSPP renders on the host
  GPU and writes VRAM back on DMA/GE copies, not on CPU reads; a CPU read returned a frame several seconds old).
- `powershell -ExecutionPolicy Bypass -File tools/psp/package.ps1` fills `build/psp/package/PSP/GAME/TM2PSP`
  (EBOOT, `files/`, `track.txt`, `LISEZMOI.txt`; 33 MB, not in git) for the user to copy to the memory stick.
- Also checked: coa/hard/0 (3 laps), 600 frames: car and camera identical to the host, 4.3 ms update + 3.0 ms list
  build per frame, heap 2.9 MB.
- Measured on PPSSPP (coa/easy/0, 150 frames, auto): load 0.58 s; per frame 3.6 ms race update (2 ticks), 2.6 ms
  to build the GE list; the GE wait is not meaningful in PPSSPP (it does not time the GE). 1 late frame of 150 (the
  first, which converts the display lists). Memory: heap 2.9 MB in use (textures 1.7 MB, vertex arrays 0.26 MB),
  far below the 52 MB extended partition: the 32 MB PSP-1000 (about 24 MB free) fits too. **Real-hardware speed is
  not verified.**

## Bilan de la phase 4 (rapport, 2026-10-19)

**Objectif atteint : la course Coast se joue dans PPSSPP depuis un EBOOT prÍt ‡ copier sur la PSP-3000.**
`build/psp/tm_race/EBOOT.PBP` (paquet complet : `tools/psp/package.ps1` -> `build/psp/package/PSP/GAME/TM2PSP`)
charge la piste et son terrain, lance le compte ‡ rebours et fait tourner la vraie physique du jeu (2 pas de 1/60 s
par image ‡ 30 Hz), la progression de course et la camÈra poursuite, dessinÈes par sceGu en 480 x 272, avec le HUD
de substitution ; commandes sceCtrl (croix, carrÈ, croix directionnelle ou stick, triangle, START, SELECT).

VÈrifications : 
- le code du jeu n'a pas changÈ pour la PSP (seuls `src/shim/psp`, `src/psp`, `src/app`, CMake et les outils sont
  nouveaux) et compile sans avertissement ;
- les 114 vÈrifications de l'hÙte tournent sur la PSP (PPSSPP) : 2,1 Mo de sorties **identiques octet pour octet** ‡
  l'hÙte, 0 erreur des vÈrificateurs ;
- la course complËte (boucle, entrÈe, camÈra) donne aprËs 150 images (easy/0) et 600 images (hard/0, 3 tours)
  **le mÍme Ètat de voiture et de camÈra** que l'hÙte ; l'image PSP montre la mÍme scËne (`docs/psp_race.png`) ;
- l'hÙte reste ‡ 0 erreur (`tools/host/run_checks.py`) et son rendu est identique au pixel prËs aprËs la
  sÈparation G3D commun / back-ends.

Mesures (PPSSPP, vitesse de CPU PSP ÈmulÈe ; **vitesse rÈelle sur console non vÈrifiÈe**) : ~7 ms de CPU par image
sur 33 ms disponibles, chargement 0,6 s, tas 2,9 Mo sur 52 Mo (le PSP-1000 32 Mo devrait suffire aussi). Le temps du
GE n'est pas mesurable dans PPSSPP.

Reste ‡ faire (Ètape suivante pour l'utilisateur : copier `build\psp\package\PSP` sur la Memory Stick et
essayer sur la console, ce que l'automatisation n'a pas le droit de faire) :
- mesurer sur la vraie PSP (touche R : temps de mise ‡ jour / construction de liste / attente GE) ; si le GE est
  trop lent : sommets 16 bits, textures en VRAM, culling des piËces hors champ ;
- fidÈlitÈ (inchangÈ depuis la phase 3) : HUD du jeu, Èclairage DS (`config/tracks`), ciel, roues/ombre, menus,
  son (sceAudio), gestionnaire de camÈras ; sur PSP en plus : brouillard et Èventuelle profondeur 16 bits ‡ affiner.

### Feedback from the real PSP-3000 (2026-10-19, user test)

- **Steering was inverted** (d-pad and stick): the game's `steer` is positive to the **left**; both drivers chose
  +1 = right. Fixed in `race_psp.cpp` and the host `race.cpp` (same mapping, same game code).
- **The track vanished behind the car while driving**: the GE drops a whole triangle when a vertex projects out of
  its guard band, which large track triangles near the camera do; PPSSPP does not emulate this, so the screenshot
  checks could not see it. The sceGu back-end now splits triangles along their longest edge until every edge is at
  most 3 world units (`G3_GU_MAX_EDGE`): vertex arrays 0.26 -> 1.5 MB, heap 4.2 MB, PPSSPP image and car/camera
  state unchanged. To confirm on the console; if pieces still drop, lower the limit or clip near triangles on the
  CPU.
- The package now holds only `vfs.zip`, `vfs.bin` and `vfs.dat` (26.9 MB): the race reads nothing else (checked
  in PPSSPP from the package folder).
