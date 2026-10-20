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

Targets: `tm_lua`, `tm_shim`, `tm_unzip`, `tm_game` (static libraries), `tm_cfgdump` and `tm_trackdump` (test
drivers).
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
- Cells: **Coast 16 × 4 units** (Island 64 × 8, others 32 × 8). Grid **22 × 20 × 22** cells on Coast and Island,
  20³ elsewhere. Cell sizes go through software doubles everywhere: `d2i(i2d(n << 12) + 0.5)` (MSL `_dadd`
  0x0201201c with 0.5 = `0x3fe00000`), which `dsRoundD` reproduces.

### Piece catalogue (main 0x0209e674, singleton `*0x020d6644`)

- `PieceData/<env>/PieceData` (global `PieceData`, 149 pieces on Coast) → `dsPieceInfo` (0x58 bytes): Name, ID,
  Type (normal/start/finish/checkpoint/multilap), Width/Height/Depth in cells, CostMem/CostPoly, HasGround/HasAir,
  BelowGnd+Cullable, IsPillar+HasPillars, Helper, DesignerTPV, DisableNotch, NotchData, IsLooseTPV, ColumnNotches,
  InitDir, InitPosAir/Ground, TriggerSize (default per environment: Coast 16 × 14 × 7).
- Model names: `<name>ground` / `<name>air`; a piece with neither flag uses its name for both (for the air one only
  when not BelowGnd, or on Island/Coast). Pieces without ID get the first free id in 1..349. Snow links
  `<name>pillar1/2/4/8`. One Coast piece has an empty name (id 149).
- Ground level `+0x38` = 3 on Coast: a piece placed above cell y = 3 uses its air model (pillars down to the sea).

### Track scenes (ov003 0x0211b404; front-end header reader main 0x0209fd2c)

- `Scenes/<env>/<difficulty>/<n>.dcsc`, global `Track`: `{"Environment", e}`, `{"TimeOfDay", t}`,
  `{"AuthorTime", s}`, `{"AuthorName", "…"}`, `{"LapCount", n}`, then `{name, rotation, x, y, z}` per piece, in cells
  (fx32 integers). Medal times: gold = author + 2 s, silver = gold + 3 s, bronze = silver + 5 s (0x0209fa5c).
- Position = cell × cell size (rounded fx multiply with the fraction dropped, then `dsRoundD`); y gets + 0.35
  (0x59a) + a per-environment offset that is zero everywhere.
- **Coast and Island skip their terrain pieces** (`wcliff*`/`dcliff*` and `beach*`, six ids, ov003 0x02100d7c /
  0x02100438): the game rebuilds the terrain itself (`dsTerrain_Build`, ov003 0x02100b2c, not ported yet).
- Placement (ov003 0x0211f088 + 0x0211f2f0): model matrix = `MTX_RotY43_` of rotation × π/2 (sin/cos table index
  `65535·a/2π >> 4`, so a quarter turn gives cos = 6/4096, as on the DS), translated to the origin cell. A W × D piece
  grows towards **−x and +z** from its origin cell; rotations 1/2/3 move the origin by (D−1)/(W−1) cells so the
  footprint stays on the same cells. Cell index of a position = `FX_Div(pos, cell) >> 12` (0x0211fe08).
- Bit-packed tracks (editor, download play; ov003 0x0211a1f0) store per piece 8+2 bits id, 2 bits rotation and three
  5-bit cell coordinates; they use the same placement code (not ported: race tracks are Lua).

### Collision meshes (main 0x02065c04, 0x02065804, 0x02073500, 0x02065b2c; format in ASSETS.md)

- The collision world (`*0x020c8e78`, vtable 0x020bd390) creates shapes: +0x10 plane (type 3), +0x14/+0x18 mesh
  (type 2) bound to a shared `dsCollisionMesh` resource from the resource cache (0x0205bcb0). Pair tests dispatch
  through a type × type table (0x0205b3c0); segment batches run in ITCM (0x01ffb28c).
- ITCM holds Firebrand's own math (0x01ffa6ec..0x01ffab20: matrix copy/identity, vector × 3×3 truncating, magnitude,
  normalise with the divider and sqrt units started together), ported in `src/game/math/dsMath.cpp`.

### Lighting (ov003 0x0211c5d4, main 0x02098aac)

- `Scripts/config/tracks` global `tracks["<env>_<tod>"]`: eight RGB colours (fx32 0..1) at a 16-byte stride, then
  two scalars (entry 24 × 41/4096 → +0xe0, entry 25 → +0xdc). Meaning of each colour to be found with the renderer.

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
- Ray/triangle (itcm 0x01ffc3c4): front faces only (`n·d < -16`, raw 64-bit dot products), barycentric bounds
  without division, `t = (n·s · (2^56 / -n·d)) >> 44` using the 64/64 divider started at the top. The local probe
  direction is truncated to 1/4096, so grazing hits on walls move by up to 0.05 unit, as on the DS.
- Plane shape (type 3, 0x0205b510/0x0205b19c): `n·p = d`, used for the Coast sea level; hit t from `FX_Div`.
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
  top · ratio · (1 - start/top rpm) / time, shift-down rpm = start - 500. Coast car: top speed 69.6 m/s.
- Torque falls off below a gear's start rpm as `(1 - t)^3` where the original subtracts t from the **integer 1**
  (`rsb r3, r12, #1`), not 1.0: kept. Above the shift-up rpm it fades to 0 at RPMMax.
- Automatic box: up-shift above the shift-up rpm, down-shift below the gear's shift-down rpm when the lower gear
  would stay under the shift-up rpm; GearChangeGap blocks the drive force after a shift; after 0.25 s nearly
  stopped, throttle/brake switch between first and reverse.
- Wheels (0x5c bytes): hub and probe in car space, compression target clamped to ±WheelTravelPhys/2, compression
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
settles level on four wheels (height 21.98 ± 0.008), reaches 15 m/s in 1.4 s, shifts to 2nd, stops when braking
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

**Objectif atteint : une course Coast se joue sur l'hôte Windows.** `tm_race --root extract/files --track easy/0`
(n'importe laquelle des 26 pistes Coast) charge la piste et son terrain, place la voiture sur le départ, lance le
compte à rebours de 3 s, puis on pilote au clavier (flèches) la vraie physique du jeu (pas de 1/60 s, deux par image
à 30 Hz) vue par la caméra poursuite du jeu (`config/cameras`, C pour Chase1/Chase2) ; les checkpoints, les tours,
l'arrivée (« N CP manqués » si on en saute), le respawn au dernier checkpoint (Retour arrière, ou automatiquement
quand la voiture coule ou reste bloquée) et le redémarrage (Entrée) suivent le code du jeu ; un HUD de substitution
affiche chrono, checkpoints, tours, compte à rebours et messages.

Chaque tranche est vérifiée contre une référence indépendante (lecture Python des scripts unluac, des `.h`, des
tables de la ROM) : carParams (320 valeurs), pistes/collisions/éclairage (26 pistes), terrain (26), requêtes de
collision (16 722 sondes), moteur (4 voitures), chute/accélération/freinage, triggers (26 pistes, 49 sondes par
porte), progression de course (26 pistes), caméras (8 + 238 recherches) et comportement caméra : 0 erreur partout.

Reste pour une fidélité complète (non bloquant pour jouer) : le HUD du jeu (sprites, feux, temps intermédiaires),
l'éclairage DS (`config/tracks`), le ciel et la mer, les roues/ombre de la voiture, le gestionnaire de caméras
(départ, arrivée, replay), la boucle `main()` et les menus, le son, et une trace melonDS pour comparer la physique
au bit près. La phase 4 (back-ends PSP du shim) peut démarrer sur cette base.

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

**Objectif atteint : la course Coast se joue dans PPSSPP depuis un EBOOT prêt à copier sur la PSP-3000.**
`build/psp/tm_race/EBOOT.PBP` (paquet complet : `tools/psp/package.ps1` -> `build/psp/package/PSP/GAME/TM2PSP`)
charge la piste et son terrain, lance le compte à rebours et fait tourner la vraie physique du jeu (2 pas de 1/60 s
par image à 30 Hz), la progression de course et la caméra poursuite, dessinées par sceGu en 480 x 272, avec le HUD
de substitution ; commandes sceCtrl (croix, carré, croix directionnelle ou stick, triangle, START, SELECT).

Vérifications : 
- le code du jeu n'a pas changé pour la PSP (seuls `src/shim/psp`, `src/psp`, `src/app`, CMake et les outils sont
  nouveaux) et compile sans avertissement ;
- les 114 vérifications de l'hôte tournent sur la PSP (PPSSPP) : 2,1 Mo de sorties **identiques octet pour octet** à
  l'hôte, 0 erreur des vérificateurs ;
- la course complète (boucle, entrée, caméra) donne après 150 images (easy/0) et 600 images (hard/0, 3 tours)
  **le même état de voiture et de caméra** que l'hôte ; l'image PSP montre la même scène (`docs/psp_race.png`) ;
- l'hôte reste à 0 erreur (`tools/host/run_checks.py`) et son rendu est identique au pixel près après la
  séparation G3D commun / back-ends.

Mesures (PPSSPP, vitesse de CPU PSP émulée ; **vitesse réelle sur console non vérifiée**) : ~7 ms de CPU par image
sur 33 ms disponibles, chargement 0,6 s, tas 2,9 Mo sur 52 Mo (le PSP-1000 32 Mo devrait suffire aussi). Le temps du
GE n'est pas mesurable dans PPSSPP.

Reste à faire (étape suivante pour l'utilisateur : copier `build\psp\package\PSP` sur la Memory Stick et
essayer sur la console, ce que l'automatisation n'a pas le droit de faire) :
- mesurer sur la vraie PSP (touche R : temps de mise à jour / construction de liste / attente GE) ; si le GE est
  trop lent : sommets 16 bits, textures en VRAM, culling des pièces hors champ ;
- fidélité (inchangé depuis la phase 3) : HUD du jeu, éclairage DS (`config/tracks`), ciel, roues/ombre, menus,
  son (sceAudio), gestionnaire de caméras ; sur PSP en plus : brouillard et éventuelle profondeur 16 bits à affiner.

### Feedback from the real PSP-3000 (2026-10-19, user test)

- **Steering was inverted** (d-pad and stick): the game's `steer` is positive to the **left**; both drivers chose
  +1 = right. Fixed in `race_psp.cpp` and the host `race.cpp` (same mapping, same game code).
- **The track vanished behind the car while driving**: the GE drops a whole triangle when a vertex projects out of
  its guard band, which large track triangles near the camera do; PPSSPP does not emulate this, so the screenshot
  checks could not see it. The sceGu back-end now splits triangles along their longest edge until every edge is at
  most 3 world units (`G3_GU_MAX_EDGE`): vertex arrays 0.26 -> 1.5 MB, heap 4.2 MB, PPSSPP image and car/camera
  state unchanged. **Confirmed on the PSP-3000 by the user: steering correct, the track no longer vanishes.**
- The package now holds only `vfs.zip`, `vfs.bin` and `vfs.dat` (26.9 MB): the race reads nothing else (checked
  in PPSSPP from the package folder).

## Phase 5: polish on the real PSP and fidelity

### Step 1: track menu, best times, leak-free reloads (done)

`src/app/race_app.*` (`RaceApp`, shared by both drivers, a port stand-in: the game's own front end is not ported)
puts a track menu in front of the race: the 26 Coast tracks in the game's order (practice, easy, medium, hard,
extreme, bonus), each with its best time; up/down move, left/right jump 5 tracks, OK starts. During the race,
START (PSP) / Enter or Esc (host) opens the pause menu: continue, restart, next track, track menu. 3 s after a
finish the result menu shows the time, the record (or "NOUVEAU RECORD!") and restart / next track / track menu.
The 5 x 7 HUD font gained the missing letters (B D F H J L W X Y Z) and `- < >`.

- Save file: `save.txt` next to the EBOOT (host: `tm_race_save.txt` next to the program, or `--save`), text lines
  `last <track>` and `best <track> <fx32 time>`, written when a track starts and on a new record. `track.txt` only
  places the cursor when there is no save yet. Checked on the host with `--finish-at N` (a forced finish at frame N):
  a better time replaces the record, a worse one does not, records are per track, the cursor comes back on the last
  track.
- Scripted runs keep their behaviour: `auto.txt` / `--frames N` start the track directly, write no save and open no
  menu, so `race_shot.ps1` still compares the car and camera with the host (hard/0 600 frames: identical). New:
  `race_shot.ps1 -Screen menu|pause|result` (host `--screen`) captures a menu for a layout check.
- **Leak found and fixed:** the texture batch was uploaded before `material.bin` replaced its texture list, so the
  first list's back-end textures (668 KB) were never freed; it is now uploaded once, after `material.bin` (textures
  in use on a track: 1.0 MB instead of 1.7 MB). `G3Model` now frees its back-end textures and vertex arrays (PSP)
  / GL textures (host) in its destructor; `RaceSession` owns its models; `RaceUnloadWorld()` clears the collision
  world's mesh cache after a session is deleted.
- **Check:** `powershell -ExecutionPolicy Bypass -File tools/psp/reload_test.ps1 [-Count 32]` (PPSSPP,
  `reload.txt`) loads the tracks in menu order, 20 drawn frames each, and logs after each unload the heap, the
  sceGu back-end's texture and vertex bytes and `FSh_OpenFiles()`. Result over 32 loads (every track, then 6 more):
  textures 0, vertices 0, 1 open file (the VFS archive, as before the first load), heap 203,832 bytes from the 3rd
  unload on (one container of the game singletons grows by 512 bytes once at the 3rd load, then nothing moves).

### Step 2: real-hardware performance safety (done; console speed to be measured by the user)

Only PPSSPP numbers are available here (it runs the CPU at 222 MHz but does not time the GE): **the console's real
frame times need the R overlay or the frame-time log on the PSP-3000.**

- **Piece culling** (`RaceSession::Cull`, common code): each piece model gets a bounding sphere at load
  (`G3Model::ComputeBounds`: the model's ops replayed with the node matrices and scales, every decoded vertex
  transformed); a piece is skipped when its sphere (plus a 2 % + 0.5 unit margin for the fx32 camera basis) is
  outside the camera's frustum (FOV from `config/cameras`, the driver's aspect, near and far planes). Both drivers
  use it. **Check:** `python -P tools/host/check_culling.py` renders each of the 26 tracks at 3 points of the auto
  drive with and without culling (`tm_race --no-cull`): **the 78 image pairs are identical pixel for pixel**, with
  50 % of the pieces drawn on average (3,843 of 7,716).
- **Textures: palettes and VRAM.** Textures with at most 16 / 256 distinct colours (after the mirror expansion)
  become T4 / T8 with an 8888 CLUT holding their exact decoded colours (lossless, and closer to the host than the
  previous 5551, which rounded each channel to 5 bits); the others stay 5551 / 8888. They go to VRAM while it lasts
  (`G3GuSetVram`: the 688 KB after the frame and depth buffers, a bump allocator reset when the last VRAM texture
  of a track is freed). Coast hard/0: 1,016 KB of textures -> **580 KB, all in VRAM**.
- **16-bit vertices** (16 bytes instead of 24): DS positions are 4.12 fixed point within +-8, so x 4096 they fit
  an s16 exactly, and texture coordinates are the DS s16 values (1/16 texel); the GE reads 16-bit values as
  s16 / 32768, so the model matrix carries a x8 scale and the texture scale is 32768 / (16 x size). Midpoints of
  split triangles round to 1/4096 unit, identically on both sides of a shared edge. 0 values clamped on the tracks
  tried. Vertex arrays on hard/0: 2.38 MB -> **1.59 MB**.
- **Meshes converted at load** (`G3Model::Prepare`): with culling, the lazy conversion on first draw would have
  hit mid-race as pieces come into view; the first frame went from 237 ms to 56 ms (the rest of that first frame is
  the race update).
- **Frame-time log:** an empty `perf.txt` next to the EBOOT makes it append one line per second of racing to
  `frametimes.txt`: average/max ms of the update, the GE list build and the GE wait, late frames and pieces drawn
  (kept in memory, written only outside the race).
- Results (PPSSPP, hard/0, 600 frames, auto drive): car and camera **identical to the host**; GE list build
  3.1 -> 1.9 ms per frame, update unchanged (4.3 ms), heap 4.4 -> 2.6 MB. The image differs from the previous one by
  at most 4 levels (exact palette colours) plus 405 edge pixels (16-bit positions). Reload test: still constant.

### Step 3a: DS lighting, fog and sky (done)

Found in the code (names in `config/names/phase5_render.txt`):

- `Scripts/config/tracks` colours (`dsTrackLighting`, ov003 0x0211c5d4) live in the race environment object
  (`*0x027e0e58`, 0xec bytes, constructor ov003 0x0211e50c, created by the race loader main 0x02099e6c) as eight
  RGBA vectors at +0x04 + 16k. `dsRaceEnv_SetupLights` (ov003 0x0211dc68) gives them to the renderer singleton
  (`*0x020c8dcc`, vtable 0x020bc468): lights 0 and 1 along (-4095, -5119, 0), lights 2 and 3 along
  (2048, -6757, 2048), all white (`+0x28`, itcm 0x01ff90dc); **colour set 0 = colours 1, 2, 3 with light mask 3,
  set 1 = colours 5, 6, 7 with mask 0xc** (`+0xdc`, main 0x0204ca64, renderer +0x7b8/+0x7f8/+0x838); **colour 0 =
  fog colour** (`+0xe0`, main 0x0204c964: FOG_COLOR, fog on, shift 1, offset 0x60). Colour 4 is not used there.
- At model load, main 0x02055970 rewrites each material: track models (flag +0x70) take set 0 as ambient /
  diffuse / specular with the shininess table on, the car (+0x71) set 1 on lights 2 and 3; emission stays the
  material's; "underside" materials become dark grey, "lights"/"light" translucent (alpha 4), "sky0" materials
  have no fog. Track pieces and the car carry normals and no vertex colours: **they are lit by the hardware**, which
  the port drew flat until now.
- Renderer init (main 0x0204f528): shininess table `(2i+1)^9 / 256^8`, fog table 0, 4, ... 124 over 32 entries,
  `SWAP_BUFFERS = 2` (W-buffering); every frame (itcm 0x01ff9628) `G3X_SetFog(on, mode 0, 1, 0x60)`.
- The sky is a model, `Sky/<env>/sky_<rise|day|set>` (ov003 0x0211de20), scaled (7987, 6349, 7987) / 4096 on Coast;
  the same function creates `Misc/CoastSea/CoastSea` (the animated sea, not ported yet).

Port (common code, identical on both back-ends): `G3LightEnv` / `G3ResolveMaterial` / `G3LightVertex`
(`g3d_model.*`) implement the DS equation (GBATEK: emission + sum of light x (specular x shininess + diffuse x
max(0, -L.N) + ambient), 5-bit channels clamped to 31); `RaceBuildLighting` builds it from the track's time of day;
`RaceSession::PieceLight` / `CarLight` select it per draw. The PSP bakes the lit colours of each piece rotation into
its vertex arrays at load, and keeps the car's geometry with its distinct normals, relighting them every frame into
the display list (0.65 ms in PPSSPP, against 4.7 ms for a full reconversion). The sky is drawn first around the eye
(own far plane, no depth); the PSP splits it to 150-unit edges instead of 3 (`G3Model::guardEdge`). Fog: linear in
view depth from 12 to 2,044 units, from the W-buffer reading (15-bit fog depth = W x 8) — a port reading, not checked
on hardware; the fog alpha of mode 0 (far geometry fading into the 2D backdrop) is not reproduced. Track specular is
left out (its shininess table is 0 up to index 72 of 127, i.e. it only shows when the light, the surface and the line of
sight line up); the car's specular is computed. `tm_race --unlit` draws the previous way.

Checks: host and PSP images show the same lit scene (`docs/host_race.png`, `docs/psp_race.png`); car and camera
still identical to the host (easy/0 150 frames, hard/0 600 frames); `check_culling.py` 0 differences; reload test
constant; PPSSPP list build hard/0 2.0 -> 3.0 ms per frame (sky, relit car, lit colours), easy/0 3.1 -> 3.8 ms.

### Step 3b: car wheels (done); shadow not ported

- The car visual (loader ov003 0x020fd914) gives each wheel its own scene node with `Cars/<env>/lwheel` (wheels 0
  and 2) or `Cars/<env>/rwheel` (1 and 3); rwheel has no textures and shows lwheel's (`G3Model::BorrowTextures`).
  Every frame the visual update ov003 0x02126d10 (`dsCarDynamics::UpdateVisual`, already ported) computes the wheel
  matrices (spin, steering on the front pair, hub position with the suspension) and ov003 0x020fbe48 stores them in
  the visual, whose draw copies them into the wheel nodes. `RaceSession::CarDraws` returns the body (lod1) and the
  four wheels with those matrices, lit with colour set 1 (relit every frame on the PSP, 56 vertices per wheel). Hubs
  check: +-0.8 across, +-1.4 along, 0.35 up in car space (wheel radius 0.33), inside lod1's wheel arches.
- The shadow is `Cars/<env>/shadow.n`, a closed car-shaped volume drawn as DS shadow polygons (the stencil-like
  shadow volume of the geometry engine): not reproduced (it needs a stencil pass on both back-ends).
- `lod0` holds the same wheels at fixed places plus light cones and a flat shadow: it is the close-up model of the
  shop / replays, not used here.

### Not done in this run: HUD, sound, shadow, sea backdrop (findings for the next one)

- **HUD.** The race HUD object (`*0x02137830`, update ov003 0x02102e9c) is a composite of about ten sub-objects
  (clock at +0x130: update ov003 0x02105408 over 8 sprite objects with blinking; others at +0x04, +0x2c, +0x3c,
  +0x58, +0x1f4, +0x32c, +0x360). Its sprites are `Graphics/2dSprites/common/HUD/Cell/*` (SplitTimeSprites,
  TimerIcon, HUD_Small, UpDownSprites, AB, Coppers; palette HUD_UI_palette) and the fonts
  `.../Fonts/Cell/Font{20..40}_White`. `tools/assets/nitro2d.py` renders these cells with scrambled tiles (its
  1D mapping for `mapping` 16 / 0x300010 is wrong), so the next step is to fix the cell renderer, then port the
  sub-objects' set-up (sprite positions) and an OAM stand-in on both back-ends.
- **Sound:** not started (`audio/sound_data.sdat`; sceAudio on the PSP).
- **Shadow:** `Cars/<env>/shadow.n` is a DS shadow volume (stencil pass needed).
- **Sea backdrop:** `Misc/CoastSea/CoastSea` (4 vertices, texture-animated by ov003 0x0213079c, created by ov003
  0x0211de20) is a tilted quad whose placement is not understood yet; the port keeps its flat sea.

### Step 4a: the game's HUD (done; console test pending)

- **2D cells** (`src/shim/common/g2d_cells.*`, `tools/assets/nitro2d.py`): the NCGR / NCLR data offsets count from
  the block payload (the old renderer used `offset - 8`, shifting tiles and palettes by 8 bytes: magenta colours and
  scrambled tiles). NCER: 24-byte CEBK header {u16 cells, u16 attr (1 = 16-byte cells with a bounding box), u32
  cells offset, u32 mapping (1D, 32 KB << mapping), u32 VRAM transfer table offset, ...}; the transfer table ({u32
  max, u32 offset}, then per cell {u32 source, u32 size}) makes each cell's OAM tile numbers count from its own
  source: address = source + tile x (32 << mapping). The HUD sprites and fonts (`Fonts/Cell/Font{20..40}_White`,
  cell = character - 0x21) render exactly. `G2CellBank` builds an atlas per bank; both drivers draw sprites (GL
  quads on the host, 5551 GE sprites with bilinear filtering on the PSP).
- **HUD object** (`*0x02137830`, constructor ov003 0x02104f1c, init 0x02103958, update 0x02102e9c; game code
  `src/game/hud/dsHud.*`), DS coordinates 256 x 192, sprites created by main 0x02069584 / 0x020696b0 (+0x10
  visible, +0x28/+0x2c position fx32 = cell origin, +0x3e cell, +0x44 screen). Ported parts of the solo race:
  countdown +0x58 (HUD_Small at (128, 75): "3" while the clock > 2 s, "2" > 1 s, "1" > 0, "GO!" hidden 60 ticks
  later); speed +0x2c (three right-aligned HUD_Small digits ending at x = 252, y = 179, value = (speed + 0x120) x
  3.6 rounded, widths table 0x02134f14, leading zeros hidden); laps +0x04 ("n/N" from x = 50 leftwards, widths
  0x02134f3c, '/' = cell 0xe, only with 2 laps or more); split times +0x6c / +0xc4 (nine SplitTimeSprites at x =
  88, y = 12 and 31: sign, minutes, ':', seconds, ':', hundredths; white, slower red 0x0210bca0 / faster blue
  0x0210bcdc against the record's split (0x0209558c), shown 120 frames); clock +0x130 (0x021057cc / 0x02105094 /
  0x02105408; base cell 0xf = '0', ':' 0x19, "--" 0xc).
- **Deviations:** the clock lives on the DS bottom screen in Font40; the PSP has one screen, so it is drawn on the
  race view top right in the game's small variant (Font28, pitch 10/5, as the bottom screen's best/target times).
  On wide screens each part follows its screen edge (`dsHudSprite::anchor`). The record's split times come from the
  save file (`split <track> t1 t2 ...` lines in `save.txt`, written with the best time). Not ported: the text
  messages (+0x3c, localised strings through main 0x0208547c at (128, 102)), the solo bottom screen (four blue panels: clock, target, best, retries),
  network and ghost modes. The HUD banks are loaded once and kept (reload_test stays flat).
- **Backing panels (run 3):** the HUD's backing object (`*(hud + 0x128)`, ctor ov003 0x02131068, draw 0x02130e1c)
  draws translucent quads through itcm 0x01fffd4c (x / 128 - 1, (y - 96) x 43 / 4096 with y from the bottom
  edge, texture coordinates per corner; POLYGON_ATTR 0x021f00c0, colour 0x3def) with the texture "speedobg" of
  `Graphics/hud/backing.n` (64 x 32, black, alpha 49 / 255 with a softer edge): the speed's panel at (197, 6,
  64 x 32) always; the lap counter's, mirrored, at (-21 below 10 laps else -6, 6, 79 x 32) while it is shown
  (flag +0x15, x set by 0x02130e04); the split times' at (48, 132, 132 x 60), or (48, 154, 132 x 35) for one line
  (+0x17), while they are shown (+0x16); a message panel (+0x14) goes with the unported messages. The model
  `hud/backing` is also loaded, hidden, by the car visual (0x020fd914, +0x28) and placed with the shadow; it is
  never shown in the race. Port: `dsHud::BackingQuads` (game code) and `RaceHudBacking`, which draws the texture
  exactly as filled rectangles of equal alpha (merged rows), so both drivers reuse their rectangle callback.

### Step 5a: the car's shadow volume (done; console test pending)

`Cars/<env>/shadow` (loaded by the car visual ov003 0x020fd914 as +0x10, model flag +0x86 bit 1) is a closed
volume of the car's footprint, 1.31 high. The visual update (0x020fc6b4) places it at the car's display matrix
(car +0x1c0), moved by the visual's vector +0x88 x 0x11f (never written in the race: zero) and lowered by 0x666
(0.4 units: the volume spans -0.4..0.91 around the ground at the car's origin); it is shown while the car's LOD is
below 3 (0x020fbf20 area). Its draw (itcm 0x01ffc7a4) sends the shapes twice: shadow-mask polygons
(POLYGON_ATTR | 0x000e0070: shadow mode, back faces, polygon ID 0, alpha 14), then shadow polygons (| 0x020000c0:
both faces, ID 2). Port: `G3Model::DrawShadowVolume` with the stencil buffer on both back-ends (host: an 8-bit
stencil requested from SDL; PSP: the stencil in the alpha bits of the 8888 frame buffer, RGB masked in the mask
pass, cleared with `sceGuClear(GU_STENCIL_BUFFER_BIT)`): stencil set where back faces fail the depth test, then
colour where it is set and the depth test passes, once per pixel (stencil zeroed). Colour: the material's diffuse
0x318c unlit (port reading; the DS lights it), alpha 14/31. From the chase camera most of it lies under the car,
as on the DS. The alpha test made the mask pass depend on the last colour set: the mask pass now sets an opaque
colour first.

### Bugs reported on the PSP-3000 after run 1 (fixed in run 3; console test pending)

- **0c, boosters pushing sideways** (`dsTrack::PieceRotationAt`, game logic): main 0x0208cb80 adds half a cell to x
  and z before dividing by the cell size (grid +0x40c = (cellWidth >> 1) << 12, set by main 0x0208da98); the port
  did not, so the boost direction (material 7, from the piece rotation) came from the neighbouring piece depending
  on where the car touched the pad. Overlapping footprints (a loop over a booster beside it), which the game settles
  with its per-cell grid bytes, give the piece with booster surfaces. Checked on the 152 booster surface points of
  the 26 tracks (90 wrong before, 0 after; throw-away test). The 114 checks are unchanged (no check run crosses a
  booster at a wrong spot); the auto-drive trajectories of check_culling changed on some tracks.
- **0b, rear lights** (car `lod1` materials `blight` / `rlight` / `hlight`, a5i3 glow texture): the game draws these
  shapes through material controllers (created by main 0x0206b0xx from the names; itcm 0x01fff814 draws a shape
  only while its light level is not 0, 0x01fff858 sets the polygon alpha to the level and DIF_AMB = SPE_EMI = the
  light colour: brake red 0x1f, reverse and head white). The levels are three bytes of the car model's light
  struct (`*(model + 0x18)` +4/+5/+6, zeroed by ov003 0x020fd914): the brake light ramps by 3 per frame between the
  visual's +0x104 = 0 and +0x105 = 0x10 (ov003 0x020fcdf0), the reverse light between 0 and 0x18 (0x020fcda4); the
  human player's update (0x0210ed9c) turns the brake light on while braking forwards (pad +0x99) or accelerating in
  reverse (+0x98, engine gear < 0) and the reverse light while the gear is negative; the head lights stay off in a
  race. The port drew the three glows always, opaque-first order mixed with depth writes. Now: `dsCarLights`
  (game code) + `G3Model::overrides` / `ApplyOverride` (both back-ends), applied by `RaceSession::ApplyCarLights`
  after every frame; and every model is drawn in two passes as the DS orders polygons, opaque then translucent
  (polygon alpha < 31 or a texture with partial alpha) without depth writes (POLYGON_ATTR bit 11 is clear in these
  materials). `tm_race --auto --brake-from F` shows the brake lights.
- **0a, sky showing below the scenery**: the PSP sea was 24 x 24 tiles of 24 units around the eye: no splitting near
  the camera (the GE drops a triangle whose vertex leaves its guard band, as the track did before
  `G3_GU_MAX_EDGE`) and only +-288 units of sea while the far plane is at 600, so the sky dome showed below the
  horizon and in the sea inlets between cliffs. Now 7 nested rings of 16 x 16 tiles (3 units within at least 18
  units of the eye, doubling per ring, +-1536 units), rows as triangle strips (3,136 vertices, 50 KB of display
  list). The terrain itself matches the game (`tm_trackdump --terrain` also prints the ground-level cell map now:
  the enclosed empty cells are sea inlets, as on the DS).
- **0d, arrow signs**: the chevrons of the Sign* pieces are a quad strip with skewed texture coordinates over a 64 x 8
  strip of four colour blocks (`signArrow`, red to green); the game scrolls it with a texture SRT animation
  (`<piece>.na`, format in ASSETS.md) over the material's own texture matrix (material flag bit 0, translation S
  0.975 / 0.025), so the colours run along the arrows. The port ignored both: static colours with black block
  borders at odd places. Now `G3Model::LoadTexAnim` / `TexSrt` and both back-ends apply s * scale - translation
  (the Maya convention; with this sign the chevrons run in their arrow's direction on SignLeft and SignRight alike,
  checked from the meshes; the opposite sign would run them backwards). The animation advances one frame per 30 Hz
  frame (the game's own rate was not found; port choice). PPSSPP showed no other difference from the host: whatever
  the console shows beyond the missing animation needs the user's eyes.
- **Host fix found by check_culling**: shapes without TEXCOORD used GL's current texture coordinate left by the
  last piece drawn (a black part of the car changed colour with culling); each shape now starts at (0, 0) as the
  PSP arrays do.

### Not done in run 3: HUD messages, sound, sea backdrop (findings for the next run)

- **HUD messages** (+0x3c, update 0x021097d4, show 0x02109658 / by key 0x02109778): three text objects of the
  game's text engine (main 0x0208547c(font manager *0x020d35cc, text, x 128, y 102, ..., width 252): +0x0c font 0
  colour 7, +0x10 font 1 colour 0, +0x14 font 1 offset by the vector at 0x0213784c with +0x3a = 1 (shadow), colour 1
  when shown), 120 frames. Keys: `FreeWheeling` while the car's +0x4e8 flag is set (HUD update, read by ov003 0x0210e9f0),
  `CheckpointMissed` / `CheckpointsMissed` formatted with the count at the finish (0x02104620, from
  dsRaceMode_OnFinish 0x02115704). French: "Roue libre !", "Vous avez manqué %d checkpoint(s).".
  Strings: `strings/Main/tokens.dat` = 3 x {u16 count, u32 size} (Common 49, FrontEnd 904, BackEnd 185), 256 u16
  bucket sizes, then the names block of {u8 file, lower-case name, 0}; a key's index is its place in the block
  (lookup main 0x0207e4c4: bucket = (h = h x 33 + (c & 0xdf), seed 5381) & 0xff, case-insensitive compare), and
  file k's `.str` strings ({u16 length, chars}) go in order to the names of file k (0x0207e5a0); checked in Python.
  Missing piece: the fonts' character -> cell map for non-ASCII letters (Font20 has 148 cells: 0x21..0x7e then 54
  Latin-1 glyphs ¡ © ® ° ¿ À Á Â ... ü in code-point order); it is in the text engine, not found yet.
- **Sound** (`audio/sound_data.sdat`, SSEQ/SBNK/SWAR, sceAudio) and the **animated sea backdrop**
  (`Misc/CoastSea`, ov003 0x0211de20 / 0x0213079c) are not started.

## Bilan de la phase 5, run 3 (rapport, 2026-10-20)

**Nettoyage, les 4 bugs signalés sur PSP-3000 et le HUD terminés ; ombre et panneaux du HUD en plus.** Tout est
vérifié sur l'hôte et dans PPSSPP ; l'effet réel sur la console reste à confirmer par l'utilisateur.

- Branche wip/parallel-bugfix relue contre le code DS et fusionnée (case de piste + ½ case, passe translucide).
- 0c boosters : bonne pièce donc bonne direction (152 points de booster, 0 erreur). 0b feux arrière : dessinés
  seulement quand le jeu les allume (frein, marche arrière), comme les contrôleurs de matériau du DS. 0a ciel sous
  le décor : mer PSP en anneaux découpés (tuiles de 3 unités près de la caméra, jusqu'à ±1536). 0d panneaux
  flèches : matrice de texture et animation `.na` portées (couleurs qui défilent dans le sens de la flèche).
- HUD : documentation (format des cellules 2D, objets du HUD), noms, plus les panneaux translucides sous la
  vitesse, les tours et les temps intermédiaires (exacts au texel).
- Ombre de la voiture : volume d'ombre au stencil sur les deux back-ends, comme le DS (masque puis ombre).
- Correctif hôte trouvé par check_culling (coordonnée de texture qui fuyait entre modèles).

Vérifications à chaque étape : hôte et PSP sans avertissement, 114 vérifications à 0 erreur, 114 dumps PSP
identiques, culling 0 différence (26 pistes x 3), race_shot hard/0 600 et easy/0 150 identiques à l'hôte,
32 rechargements à tas constant.

**Prochaine étape = test console** (copier `build\psp\package\PSP`) : voir la note phase5-run3.md. Ensuite :
messages du HUD (table caractères -> cellules des polices), son, fond de mer animé.

## Bilan de la phase 5, run 1 (rapport, 2026-10-19)

**Priorités 1 et 2 terminées, priorité 3 en partie.** Tout est vérifié dans PPSSPP et sur l'hôte ; la vitesse et
l'aspect sur la vraie PSP-3000 restent à confirmer par l'utilisateur.

1. **Menu des pistes** (hôte et PSP, `RaceApp`) : 26 pistes Coast avec leur meilleur temps, pause (START),
   écran d'arrivée, piste suivante, sauvegarde `save.txt` à côté de l'EBOOT. Une fuite de 668 Ko de textures par
   chargement a été trouvée et corrigée ; 32 rechargements dans PPSSPP gardent tas, textures, sommets et fichiers
   ouverts constants (`tools/psp/reload_test.ps1`).
2. **Sécurité de performance** : culling des pièces hors champ (images identiques au pixel près, 50 % des pièces
   dessinées en moyenne), textures palettisées sans perte et en VRAM (1,0 Mo -> 580 Ko), sommets 16 bits exacts
   (2,4 -> 1,6 Mo), conversion au chargement, journal `frametimes.txt` (fichier `perf.txt`). PPSSPP, hard/0 :
   construction de liste 3,1 -> 1,9 ms par image avant l'éclairage, 3,3 ms avec ciel, éclairage et roues ; mise à
   jour du jeu 4,3 ms ; tas 3,2 Mo. **Le temps GE réel n'est mesurable que sur la console.**
3. **Fidélité** : l'éclairage du jeu retrouvé dans le code (lumières, jeux de couleurs de `config/tracks`, règles
   des matériaux, table de brillance) appliqué à l'identique sur les deux back-ends — les pièces et la voiture
   étaient dessinées à plat ; brouillard (lecture du W-buffer à confirmer) ; ciel de l'heure de la piste ; roues
   de la voiture. Restent : HUD du jeu, son, ombre (volume d'ombre), fond de mer animé.

Vérifications à chaque étape : hôte et PSP sans avertissement, 114 vérifications hôte à 0 erreur, 114 dumps PSP
identiques à l'hôte, état voiture/caméra PSP = hôte (easy/0 150 images, hard/0 600 images).

**Prochaine étape = test console par l'utilisateur** (copier `build\psp\package\PSP`) : menu et records, vitesse
(touche R ou `perf.txt` -> `frametimes.txt`), aspect (ciel, ombrage, roues, pas de triangles manquants). Selon les
temps mesurés : optimisations GE supplémentaires ou suite de la fidélité (HUD, son).
