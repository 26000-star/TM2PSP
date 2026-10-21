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
- At model load, main 0x02055970 rewrites each material. The model loader main 0x0205600c sets flag +0x70 when the
  resource name (lower-cased by main 0x02045904) contains "cars", +0x71 when it contains "tracks" or "misc": **car
  models take set 0** as ambient / diffuse / specular with the shininess table on, polygon ID 2 (the ID of their
  shadow volume, which therefore never darkens them), "underside" dark grey and "lights"/"light" translucent (alpha
  4); **track and misc models take set 1** on lights 2 and 3 for every material ("snowsnow" without specular);
  other models keep their colours, with "shadowback" / "shadowfront" turned into shadow volumes (ID 3). Emission
  stays the material's; "sky0" materials have no fog. (Run 1 read the two flags the other way round, lighting the
  track with set 0 and the car with set 1; fixed in step 5b.) Track pieces and the car carry normals and no vertex colours: **they are lit by the hardware**, which
  the port drew flat until now.
- Renderer init (main 0x0204f528): shininess table `(2i+1)^9 / 256^8`, fog table 0, 4, ... 124 over 32 entries,
  `SWAP_BUFFERS = 2` (W-buffering); every frame (itcm 0x01ff9628) `G3X_SetFog(on, mode 0, 1, 0x60)`.
- The sky is a model, `Sky/<env>/sky_<rise|day|set>` (ov003 0x0211de20), scaled (7987, 6349, 7987) / 4096 on Coast;
  the same function creates the animated sea (ported in step 5d).

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
  four wheels with those matrices, lit with colour set 0 (relit every frame on the PSP, 56 vertices per wheel). Hubs
  check: +-0.8 across, +-1.4 along, 0.35 up in car space (wheel radius 0.33), inside lod1's wheel arches.
- The shadow is `Cars/<env>/shadow.n`, a closed car-shaped volume drawn as DS shadow polygons (the stencil-like
  shadow volume of the geometry engine): not reproduced then (done in step 5a, with the stencil buffer).
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
colour where it is set and the depth test passes, once per pixel (stencil zeroed). Colour: both passes clear the
light enables of the material's POLYGON_ATTR and every vertex of the shapes follows a NORMAL command, so the DS sets
the vertex colour to the material's emission (0: black), not its diffuse 0x318c (the first port reading, a grey
shadow on the PSP-3000); alpha 14/31. From the chase camera most of it lies under the car,
as on the DS. The alpha test made the mask pass depend on the last colour set: the mask pass now sets an opaque
colour first.

### Step 5b: black shadow, car and track lighting sets (done; console test pending)

Reported on the PSP-3000: the shadow was grey. Two readings of main 0x02055970 were wrong:

- **Colour**: see step 5a; the shadow volume is drawn black (the material's emission) at alpha 14/31.
- **Model flags**: the model loader main 0x0205600c sets +0x70 when the resource name contains "cars" and +0x71 for
  "tracks" / "misc" (`strstr`, main 0x02019710, on a name lower-cased by main 0x02045904); run 1 had them the other
  way round. So the car takes colour set 0 (lights 0 and 1, shininess table) and polygon ID 2, the track set 1
  (lights 2 and 3): `RaceSession::CarLight` / `PieceLight` swapped. The walls that faced away from lights 0 and 1
  were black on the host and the PSP; they are now lit, and the car takes the time-of-day tint of set 0.
- **Polygon ID**: a DS shadow polygon is drawn only where the attribute buffer holds another polygon ID (GBATEK
  "Shadow Polygons"); the car's materials and the shadow both have ID 2, so the shadow darkens the track and never
  the car. Port: both drivers draw the volume after the track and before the car and wheels (which then cover it
  where they are in front, as the ID rule does); before, the volume greyed the lower half of the body.

### Step 5c: the HUD messages (done; console test pending)

The missing piece of run 3 was in the scripts: **`Scripts/config/ui` defines the fonts** (`font20_White` ...
`font40_Orange`: `resource`, `height`, `spaceWidth`, `upperCaseOnly`, `glyphs = {{character, width}, ...}`), and
glyph i is cell i (148 glyphs: '!'..'~', then Latin-1 ¡ © ® ° ¿ À..ü). The font manager (main 0x02085528, `*0x020d35cc`)
loads twelve slots from the names at 0x020bfa08 (font20/28/32/40_White, three times) with Fonts/Cell/Font as the
palette resource; the font object (0x0207f01c) keeps per byte the advance (width + 1 pixel) and the cell (0xff:
none), and the space width.

- **Text layout** (main 0x0207e9f8, called by the text object 0x020840e4 / set-text 0x02083f38): one glyph per
  character with a cell, advance `d2i(i2d(w << 12) + 0.5)`; a space only advances; '\n' breaks the line; another
  character without a cell is drawn as '1' (font +0x145). When a glyph passes the wrap width, the line breaks at its
  last space (the word moves down) or before that glyph; lines are aligned left / centred / right
  (`-w / 2` truncated), then the block top / centred / bottom (`height - h / 2` for centred).
- **Messages** (HUD +0x3c, init 0x0210982c): three text objects at (128, 102), centred both ways, 252 wide: +0x0c
  font slot 0 palette 7 (always "" in the solo race), +0x10 slot 1 (Font28_White) palette 0, +0x14 slot 1 palette 1
  moved by the vector at ov003 0x0213784c = (1, 1) (`__sinit` 0x02135280), drawn under the white text. Palettes
  of Font.NCLR: 0 white, 1 dark grey. Shown for 0x78 frames by 0x02109658 (the network modes move them first),
  hidden by 0x02109610 and by the HUD's display switch (0x02102a60, pause / finish / restart).
- **Triggers**: the HUD update shows `FreeWheeling` on entering a free-wheel zone (the car's +0x4e8, set by surface
  8 and cleared by a respawn) and hides it on leaving; the race mode's finish (0x02115704) calls 0x02104620 with
  the number of checkpoints missed: `CheckpointMissed` (1) or `CheckpointsMissed`, formatted by vsnprintf.
- **Language**: `*0x020d35a4` picks gb, us, fr, it, de, es (0x020bf7d0). Port: the PSP's system language (English
  -> gb), `tm_race --lang xx` on the host (default gb).
- The backing's panel +0x14 is not the messages' (run 3 note): 0x02101e28 is called by another HUD part
  (0x02109b3c / 0x02109c8c), not the solo race.

Port: `dsStringTable` (`src/game/text`, main 0x0207e4a0..0x0207e908), `dsFont` / `dsFontManager` / `dsTextLayout`,
`dsHud::ShowMessage` / `HideMessage`: the glyphs become HUD sprites of two new banks (Font28_White's 148 cells in
palette 0 and 1: `G2CellBank::Load` takes a palette that replaces the OAM entries', as the text sprites' +0x45
does). The host's red stand-in text for missed checkpoints is gone. Checks: `tm_race --missed-at F` /
`--free-wheel-at F` (host) and `race_shot.ps1 -Screen missed|freewheel` (PPSSPP) show "Vous avez manqué 2
checkpoints." on two lines (289 pixels > 252) and "Roue libre !"; the PSP heap grows by 0.6 MB (two 256 x 512
16-bit atlases and the strings).

### Step 5d: the sea (done; console test pending)

The sea is not a model. `dsRaceEnv_LoadSky` (ov003 0x0211de20), after the sky dome, builds the name `misc/%s/%s`
from `dsRaceSetup_EnvironmentLongName` (main 0x020a11dc, table 0x020c1218: stadiumGrass, islandSea, snowSnow,
coastSea) and creates:

- a **drawer** (`dsSeaDrawer_ctor`, ov003 0x0213079c, 0x3c bytes, vtable 0x021373b8, kept at race environment
  +0x88): +0x14 speed 0x11f on Island and Coast (0 elsewhere), +0x18 phase 0, +0x1c height (Coast 0x2800 = 2.5,
  Island -16219 = -3.96: the car's sea levels), +0x20 scale 2 on Island and Coast (else 1), +0x24..+0x30 the race
  environment's +0x44..+0x50, i.e. **colour 4 of `Scripts/config/tracks`** (the one step 3a found unused: coa set
  (3686, 3317, 3276) -> COLOR 0x633b, coa rise 0x775f, nearly white);
- a **custom scene node** (`dsCustomNode_ctor`, main 0x0206e814, 0x40 bytes, node type 4) with the drawer at +0x18,
  the resource handle of `misc/coastSea/coastSea` at +0x14 (renderer vtable +0x88 / +0xec load its texture and
  palette), position 0, scale 1, draw order +0x08 = 4 (the sky 0xb, models 0x10; `dsScene_SortNodes` main
  0x02058740 sorts the nodes by it) and +0x34 = 1: `dsRenderer_Frame` draws it at once through
  `dsScene_DrawCustomNodes` (itcm 0x01ffcf3c) instead of queueing it.

`dsScene_DrawCustomNodes` resets the NNS base matrix (identity rotation, no translation, unit scale) and loads the
camera, sets the texture matrix to identity, then per node: DIF_AMB = SPE_EMI = 0, MTX_PUSH, the node's translation
and scale, the texture (`dsTexture_Bind` itcm 0x01ffddd4: TEXIMAGE_PARAM with repeat S and T, texture coordinates
through the texture matrix), the drawer's vtable +0x0c, MTX_POP. **The drawer (`dsSeaDrawer_Draw`, itcm 0x01fff8cc)**
pops that matrix (back to the camera's: world space) and sends straight to the geometry engine:

- POLYGON_ATTR 0x001f80c0 (no light, modulation, both faces, fog, alpha 31, ID 0), COLOR = each colour channel / 132;
- for i, j = 0..4: MTX_PUSH, MTX_SCALE(2 x 0x10002 = 32.001, 1, 32.001), MTX_TRANS(-12 + 16 i, ty, -12 + 16 j)
  (steps 0xfffc; the translation is scaled), one quad of VTX_16 (+-7.9995, -0.9497, +-7.9995), MTX_POP; a final
  MTX_PUSH balances the node's pop. So **5 x 5 tiles of 512 units from -640 to 1920 in x and z**, fixed in the world
  (the Coast track's cells span 0..352): the sea reaches past the far plane from anywhere on the track (the
  default camera, `dsCamera_ctor` main 0x02054a28, has near 0.1 and far 400).
- **Height:** ty = 0.975 + height - clamp((camera y - 2.4) / 4, 0, 2) (camera y: the scene camera's matrix
  translation, +0x28 -> +0x50), so the surface lies at 2.525 on Coast while the camera is below 2.4 and **goes down
  as the camera rises**, by a quarter of its height, at most 2 units (0.525 from 10.4 up).
- **Texture coordinates:** a = (phase + 0x900000) >> 8, b = phase >> 8, kept in 16 bits: (a, b) at (-x, +z),
  (a, a) at (-x, -z), (b, a) at (+x, -z), (b, b) at (+x, +z). b - a = 28672 (1/16 texel) = 14 repeats of the
  128-texel texture across a tile, continuous from tile to tile, the same function for s along x and t along z; then
  phase += speed: a **diagonal scroll** of 287 / 4096 texel per draw (one texture every 61 s at 30 draws per
  second). After about 29,000 draws b wraps alone and the tiles take 18 repeats the other way until a wraps too
  (about 16 to 30 minutes into a race; kept).
- **Texture:** `islandsea` of Graphics/Misc/CoastSea/CoastSea.n, 128 x 128 **A5I3** (8 light blue-grey colours,
  alpha 15..27 / 31). An A5I3 texture makes the DS draw the quads as **translucent polygons** (after the opaque
  ones, without depth update: POLYGON_ATTR bit 11 clear): the sky dome below the horizon and the terrain under the
  water (cliff bases, beaches) show through. The file's own model (one tilted quad, material "cowboy1") is never
  drawn: the "tilted quad whose placement is not understood" of run 1 is not the sea surface.

Port: `dsSeaBackdrop` (`src/game/gfx`, common code: `Init` = the constructor and the fields set by LoadSky, `Quads`
= the draw's 100 vertices and 16-bit texture coordinates, `Color`, `Advance`), `dsRaceSetup::EnvironmentLongName`,
`RaceSession::seaModel` (texture forced to repeat, as TEXIMAGE_PARAM) and `SeaDraw`, and `G3Model::DrawQuads` on
both back-ends: the host draws the 25 GL quads; the PSP cuts each tile into a quadtree (one draw call, 16-byte
vertices with the DS's s16 texture coordinates and float positions, a static buffer of 8,192 vertices): cells whose
corners are all outside one frustum plane are left out (clip coordinates averaged along with the positions), cells
larger than `guardEdge` (3) are split while their 3D distance to the eye is under 4 times their size, so a visible
triangle keeps its vertices within about 5 times its depth sideways (the guard band is +-2048 pixels, 8.5 times the
depth at the PSP's FOV; a factor 3 would allow 13). The phase advances once per `RaceSession::Frame` (the game: at
each draw, once per 30 Hz frame), so the image does not depend on how often a driver draws. Both drivers draw the
sea after the track and the car's shadow volume and before the car (deviations: the track's own translucent polygons
are drawn before it instead of being sorted with it, and a sinking car is drawn over it). The host's flat blue quad
at 2.5 and the PSP's untextured rings (bug 0a of run 3) are gone.

Checks: host and PPSSPP shots (easy/0 150 frames, hard/0 600) show the same textured sea; car and camera identical
to the host; 114 checks, 0 errors; reload test (32 loads) constant (heap 891,144 bytes after every unload from the
3rd on, textures and meshes 0); `check_culling.py` 0 differences (26 tracks x 3). PPSSPP, easy/0: 1,644 sea vertices (26 KB of list; hard/0 1,710), GE list build
5.3 -> 6.1 ms per frame (the earlier rings: 3,136 untextured vertices); heap unchanged.

### Step 6: sound (done; console test pending)

The game does not use the NNS sound player directly: its own layers sit on top of it. Names in
`config/names/phase5_audio.txt`.

- **6a, the DS sound driver on the CPU** (`src/shim/common/snd_sdat.*`, `snd_driver.*`, library code). The SDAT
  reader takes `audio/sound_data.sdat` (5 music sequences, 24 sequence archives of sound effects, 29 banks and wave
  archives, 1.7 MB, kept in memory: there is no sound heap). The driver is the NitroSDK ARM7 sequencer (SSEQ players
  and tracks, the 16 extended channels with envelopes, sweeps, LFOs and the allocation order) and the 16 hardware
  channels (PCM8 / PCM16 / IMA-ADPCM, no interpolation, as the DS mixer) mixed to 32 kHz stereo, with the driver
  frame every 64 x 2728 ARM7 cycles between the samples; above it the NNS player layer the game calls (players with
  their sequence count and channel mask, faders, archive volumes). Its tables come from `arm7.bin`. Integer only:
  the output depends only on the calls and the number of samples rendered. `tm_sdat --list` lists the archive,
  `tm_sdat --seq|--arc ... --wav` renders a sequence (host, release build: 2 ms of CPU per second of the Coast music).
- **6b, the game's sound layers** (`src/game/sound`): `dsSoundManager` (*0x020c8e88: handles type << 28 | slot << 20
  | serial, sounds by name, sound-effect and music volumes, heap levels of the loaded archives), `dsSound3D`
  (*0x020c8e8c: 64 sources heard from the camera, volume by distance between a minimum and a maximum, pan and pan-law
  attenuation from the DTCM tables, at most 8 playing, sorted by heard volume; the angle now comes from
  `FX_Atan2Idx`, ported with the ROM's table, instead of a float atan2) and `dsCarAudio` (ov003 *0x02137740: the
  engine as three looping layers CAR_IDLE / CAR_LOW / CAR_MED whose volume and pitch follow a "sound RPM", skids,
  collisions, horn; envelopes from `config/caraudio`).
- **6c, the game's audio state** (`dsGameAudio`, main 0x0208bcfc..0x0208c9d4): the race start (0x0208c04c, from the
  race loader) loads SEQARC_MENU_BASIC, SEQARC_DRIVING, the environment's archive (CHECKPOINT, LAP, SPEEDBOOST), a
  horn (SEQARC_HORN_<n>, the player's number modulo 8) and SEQARC_CAR_<car>, starts the environment's music
  (SEQARC_CoastMusic; the editor's test drive plays the environment's AMBIENT instead) and the car audio; the end of
  a race (0x0208bf50) only drops the car audio, so the next race keeps the music; the front end (0x0208c4b8) stops
  everything and loads the menu archives, its music switcher (0x0208bd74: 1 front end, 2 Coast, 3 Snow, 4 Island, 5
  Stadium) changes the music. Two faders (sound effects, music: 0x0208c5ec in, 0x0208c588 out, 0x0208c674 both with
  the music stopped at the end) run in the update (0x0208c730), which then updates the car audio from the camera.
- **What plays in the race** (found in ov003): the countdown (0x02114ee4: COUNTDOWN_1 when the clock is above 2 s,
  above 1 s and above 0, COUNTDOWN_2 at 0, the race mode's +0x18 = -1 after a restart), CHECKPOINT at each
  checkpoint (0x0210e9fc, also at each lap since a lap passes the start piece's checkpoint) and at the finish
  (0x0210ecf0), the car audio reset at every restart and respawn (0x020f7e6c), skids and collisions in the player's
  update (0x0210ed9c), the horn on a button (dsRace_Update), the car's fade-out / fade-in over 30 frames when the
  pause opens / closes (0x0210dd64 / 0x0210dbec). LAP and SPEEDBOOST are loaded but never played by ov003. The UI
  plays MENU_SELECT / MENU_BACK / MENU_MOVE (main 0x0208c8ec hands them to *0x020d35cc +0x40..+0x48).
- **The sound RPM** (0x020f799c) is the car speed smoothed by 41 / 4096 per frame, placed in a gear by the car's
  TopSpeed thresholds, mapped to 7000..9000 across the gear, followed at 0.5 (0.2 when falling in gear 2 and up) and
  lowered by 800 every frame, so it settles 1,600 (4,000) below its target: 6,200 at a standstill, about 5,400 to
  7,400 in a gear. With the Coast car's envelopes (CAR_MED full from about 4,000 to 6,591 RPM, silent above 7,031)
  the engine is mostly the CAR_MED layer, fading at the top of each gear. Checked against the decompilation twice
  (the speed is the car's +0x384, the envelopes' unit is MaxRPM / 2048); it still deserves a listen against a DS.

Port: `RaceSession::Load` starts the race sounds (Coast, horn 0, output mode 1, car "coa"), `RaceSession::Frame`
plays the countdown and checkpoint sounds from the race events, resets the car audio and runs the player's sound
update, `RaceSession::AudioUpdate` is the game audio update with the current chase camera; `RaceApp` calls the game
audio, the 3D sources and the manager (NNS_SndMain) on every frame, also in the menus and while paused, plays the
menu sounds, starts the front end and its music on the track list, stops it before a race (the front end's exit is
not ported) and fades the car out under the result menu (port choice). Horn: H on the host, L on the PSP (the DS
button was not identified).

Output (`src/shim/common/snd_stream.*`): the main thread renders the driver after each game frame into a ring of
16,384 frames, so the driver and every game sound call stay on one thread; the device takes from the ring (host: an
SDL audio device at 32 kHz, the callback; PSP: a thread at priority 0x12 on a 32 kHz SRC channel, 512-frame blocks,
`sceAudioSRCOutputBlocking`). Interactive runs render kRate / 30 samples per frame corrected by an eighth of the
distance to a target fill (two frames plus a block, about 80 ms), which follows the device's clock; scripted runs
render exactly kRate / 30 with the remainder carried, so their sound is the same on every platform. An empty
`mute.txt` next to the EBOOT turns the sound off (host: `--mute`, `--no-music`).

Checks: `tm_race --frames N --auto --wav out.wav` prints the FNV-1a hash of the rendered sound, the PSP auto run
prints it in `race_log.txt` (`race_shot.ps1 -Wav` also copies the PSP's WAV); **car, camera and sound identical to
the host** after easy/0 150 frames and hard/0 600 frames in PPSSPP. `race_shot.ps1 -Screen audio` runs the auto drive
with the audio thread playing: hard/0 600 frames, 0 underruns, 0 dropped frames. The car and camera after 300 frames
of easy/0 are those of the build before the sound (the sound code does not touch the race). 114 checks, 0 errors;
reload test (32 loads) constant (heap 2,657,704 bytes after every unload from the 4th on: the archive is loaded once).
PPSSPP: sound rendering 2.7 ms per 30 Hz frame on easy/0, 2.9 ms on hard/0 (8 % of the frame at 222 MHz); the
console's own figure is in the R overlay (second number) and `frametimes.txt` (`snd`).

### Step 7: the four environments (done; console test pending)

The ported loaders already took the environment from the race setup; only the application was Coast-only.
`RaceSession::Load` now takes the scene path (`isl/easy/0`): the environment's piece catalogue, car (`isl`, `sno`,
`sta`: parameters, lod1, wheels, shadow), cameras, texture batch, lighting, sky (Island's own scale, the others
Coast's: ov003 0x0211de20 only tests Island) and ground drawer (`misc/<islandSea|snowSnow|stadiumGrass>`: the sea's
drawer, scrolling only on Island and Coast) and the race sounds (SEQARC_<env>, its music, SEQARC_CAR_<env>). The
menu lists the 26 tracks of each environment in the game's order (Stadium, Island, Snow, Coast): left / right change
the environment, up / down the track; "next track" stays in the environment. Track ids are now `coa/easy/0`; ids
without an environment (old save files, `--track easy/0`, `auto.txt`) are Coast tracks.

Checks extended to the 104 tracks (`run_checks.py --env all`: 426 checks, 0 errors; the default stays Coast, the
114 checks of the PSP suite):
- `tm_trackdump --env` and `tm_physics --env` (new) produce the dumps; the piece catalogues of Island, Snow and
  Stadium were decompiled with unluac like Coast's.
- `check_track.py`: the footprint used Coast's 16-unit cells (Island 64, Snow and Stadium 32); the Lua evaluator
  missed unluac's `t[var] = value` keys (ColumnNotches, DisableNotch, Cullable...): the 6 catalogue differences it
  reported were checker errors, the port was right. Stadium's zeppelin overhangs its cells by 2.43 (tolerance 2.5).
- `check_terrain.py` covers Island (its terrain names from the ROM table, ground level 0, 64-unit cells): the 26
  Island terrains match the re-implementation; Snow and Stadium must have none.
- `check_triggers.py` takes the environment from the dump (Island's start point offsets of ov003 0x02114a6c).
- `check_progress.py`: the respawn puts the car on the first surface of a segment from 2 units above the respawn
  point to 8 below; Stadium's tilted checkpoints lie up to 3 units below their InitPos (the check expected above).
- PPSSPP: car, camera and sound identical to the host after 150 frames on isl/easy/0, sno/easy/0 and sta/easy/0.

**Detail levels for the GE** (PSP only; `G3SetDrawDetail`, `RaceSession::detail`). Splitting every triangle to 3
units for the guard band made Island's pieces (64-unit cells) 15 MB of vertices and 1.07 million vertices per frame
(Coast hard/0: 146,000), far beyond what the GE can fetch at 30 Hz. Each piece is now converted at three levels:
edges up to 3 units (`guardEdge`), 8 and 24, and drawn with the coarsest level whose edge is at most 2.5 times
smaller than the distance from the eye to its bounding sphere (20 and 60 units). A triangle with edges E whose
vertices are all 2.5 E away stays within about 70 degrees of the view axis when part of it is visible, inside the
guard band (about 82 degrees at 480 x 272 with the 70 degree FOV). Vertices per frame (PPSSPP, 150 frames of auto
drive): Coast hard/0 146,000 -> 43,500, Island easy/0 1,067,000 -> 162,000, Snow 390,000 -> 37,000, Stadium 371,000
-> 104,000; mesh memory +20 % (Island 18.6 MB, heap 23.5 MB of 52); images unchanged but for the interpolation of
lit colours and fog over the longer far edges (at most 1,942 pixels of 130,560 differ by more than 24 levels).
The largest Island pieces stay fine-split while the camera is over them: the next step if the console is slow there.

Reload test: `reload_test.ps1` now loads the tracks in menu order (Stadium first) and compares the states per
environment (the piece catalogue kept between races has a size per environment); `-Count 208` makes two rounds of the
104 tracks: in the second round every environment comes back to one state (no leak; a few containers keep the
capacity of the largest track seen, +640 bytes for Stadium once Island and the others have been loaded).

### Step 8: platform mode, retries, Stadium rings (done; console test pending)

- **Retries** (`dsRaceProgress::RespawnRequest` / `Restart`, dsRace_Update ov003 0x02113048): every respawn (the
  button or the player's own request: sinking, stuck) counts one retry (race +0x14c), or resets the count to 0 when
  no checkpoint was passed (back to the start); the restart button sets it to 0. On Stadium a respawn whose only
  checkpoint is a ring (pieces 0x10 cpringh / 0x11 cpringv, which give no respawn point) restarts the race.
  `lastCheckpoint` = race +0x138. Checked with `tm_race --respawn-at F` (repeatable): 1, 2, 3 after coa/hard/0's
  first checkpoint, 0 before it.
- **Stadium's horizontal ring** (piece 0x10): its trigger matrix is turned a quarter (angle 6432) about its scaled
  row 0 (ov003 0x021114e4, MTX_RotAxis + MTX_Concat33), now in `dsTrigger::PieceMatrix`; `check_triggers.py`
  models it (sta/practice/2 has two).
- **Platform mode** (race mode 8, `Scenes/pla/<difficulty>/<n>`, 17 tracks, environment n = 0..3 read from the
  scene header): `RaceSession::Load("pla/easy/1")` sets mode 8; the HUD shows "Retries N" (string `Retries`, French
  "Tentatives") under the clock (`dsHud::SetRetries`, port stand-in for the bottom screen panel +0x32c, 0x0210b53c,
  whose other lines are `TargetText` and `BestTriesText`). Menu section "PLATE-FORME"; best = fewest retries then
  time (`retries <track> N` lines in save.txt); medals from `config/PlatformProgression` (main 0x020b1f70:
  {difficulty, number 1..17, medals to unlock, gold, silver, bronze retries at most, coppers}): gold 0, silver
  7/6/5/4/3 by difficulty, bronze 10, shown on the result panel. Unlocking is not ported (all tracks open).
- Not done: **puzzle mode** (mode 7, `Scenes/puz/*` with `-Layout` scenes: the player places pieces; needs the
  editor's piece placement and UI), the platform "Target" line, the bottom screen panel itself.

## Reprise 2 (2026-10-20 soir, pause demandée en cours de travail)

Retour console du PSP-3000 : ralentissements (Coast, pire sur Stadium), textures qui clignotent (start gate, drapeaux,
panneaux fléchés), objets qui se superposent, son des voitures qui part ou se coupe au hasard, Snow très bugué.
Skill `psp-developer` (SandstoneDev) installé dans `~/.claude/skills`. **PPSSPP en rendu logiciel** (`SoftwareRenderer =
True` dans ppsspp.ini) reproduit les artefacts du GE ; le rendu Vulkan ne les montre pas.
Corrigé dans l'arbre de travail (non commité, hôte et PSP compilent sans avertissement, voiture/caméra/son identiques
à l'hôte sur coa, sno, sta, isl ; checks complets pas encore relancés) :
- **Son** : `dsSound3D::Compute` oubliait le `m_playing--` du DS (0x02060118) après la fin d'un son ponctuel ; après 8
  dérapages/chocs plus aucune source ne démarrait (moteur compris). Vérifié sur l'hôte (compteur = sons joués).
- **Textures** : le GE lit les coordonnées de texture 16 bits en non signé ; 5 à 13 % des triangles chevauchaient 0
  (rayures, damiers en moiré). `RebaseTexCoords` / `TexPeriod` (g3d_gu.cpp) décalent chaque triangle et chaque case
  de la mer d'un nombre entier de répétitions ; sommets en u16. 0 coordonnée tronquée.
- **Ordre de dessin** : tous les opaques de l'image puis tous les translucides, comme le DS (`G3Model::Draw(batch,
  pass)`, les deux pilotes) ; avant, une pièce dessinée plus tard recouvrait le feuillage/filet d'une pièce plus proche.
- **Caméra relative** (PSP) : vue sans translation, l'œil retiré des matrices et des sommets de la mer (float24).
Écartés par essai : plan lointain (MinZ 1) et précision de profondeur (near 2.0), sans effet.
Vérifié (suite de la reprise) : `run_checks.py` 114 et `--env all` 426 à 0 erreur, `check_culling.py` 0 différence
(26 pistes x 3 images), `run_checks_psp.ps1` 114 dumps identiques, `reload_test.ps1` 32 sans fuite.
Performances (fait, à mesurer sur la console) :
- CPU à 333 MHz / bus 166 (`scePowerSetClockFrequency` en tête de `main`, psppower) ; `race_log.txt` et l'en-tête de
  `frametimes.txt` donnent l'horloge lue après coup.
- Recouvrement CPU/GE : la boucle fait mise à jour + son pendant que le GE dessine l'image précédente, puis
  `sceGuSync`, rythme 30 Hz, échange des tampons, construction de la liste. « ge » = attente restante du GE après la
  mise à jour. `~G3Model` attend le GE s'il a des données côté GE (le modèle `hud/backing`, lu sur le CPU, est
  détruit dans une liste ouverte : un sync inconditionnel bloquait). Sync avant la capture des runs scriptés.
- Son produit pour la durée réelle de l'image (`FrameSamples(target, frameUs)`, plafonnée à 100 ms) : une image en
  retard (50 ms) vidait l'anneau, d'où des coupures. Runs scriptés inchangés (kRate / 30).
PPSSPP : coa/hard/0 300 images, isl, sno, sta easy/0 et pla/easy/1 150 images identiques à l'hôte ; thread audio
600 images sans trou ; 32 rechargements sans fuite. Paquet `package.ps1 -Perf` (ajoute `perf.txt`).

## Reprise 3 (2026-10-21) : retour console du paquet « perf », décors, sol, son moteur

**Console (PSP-3000, `frametimes.txt`)** : plus de ralentissement ni de texture qui clignote. CPU lu à 333/166. Sur
Coast hard/0, Stadium practice/0 et easy/0, Island easy/0 et Snow easy/0 : `late 0` à chaque seconde, sauf la
seconde du chargement ; `ge 0.0` (le GE finit avant la fin de la mise à jour). Pire seconde en travail CPU : Island,
mise à jour 9-11 + son 2-3 + liste 18,7 ms = environ 32 ms sur 33. Stadium : mise à jour 15 ms (practice/0). Signalé
encore : Coast, dessous des routes « sans texture » ; Snow, impression de rouler au-dessus du vide, éléments
disparus, route transparente avant l'arrivée d'easy/0 ; son moteur toujours « à fond ».

Corrigé (non commité ; hôte et PSP compilent sans avertissement) :
- **Commandes de rendu NNS** (`G3Model::Load`, commun) : la table du nombre de paramètres avait deux entrées de trop
  avant 0x40, donc 0x44 (MAT) se lisait comme inconnue et arrêtait le bytecode : 37 modèles de Snow (48 dessins,
  dont le dessus de `WoodRoadStraightAir`, la route avant l'arrivée d'easy/0) et les îles d'Island perdaient leurs
  derniers dessins. Remplacée par un `switch` (valeurs de nitro3d.py).
- **Piliers de Snow** (`dsTrackPiece::pillars`, ov003 0x0211fe08 fin, 0x0211ef00, 0x0211ee60) : une pièce en l'air
  (case y au-dessus du niveau du sol) porte un pilier `<nom>pillar1/2/4/8` par bit de sa hauteur en cases (1 à 15),
  empilés depuis le sol (8 unités par case, fixé dans le code, + 0,35) ; ce sont des pièces avec modèle et collision,
  hors de la liste de la piste. sno/easy/0 : 9 piliers.
- **Sol** (ov003 0x0211de20, monde de collision vtable +0x10 = 0x0205b4bc) : un plan horizontal, y = 0 matériau 1
  (rugueux) sur Stadium et Snow, fond de mer y = -16 (Island) et -19 (Coast) matériau 3. `RaceSession::groundPlane`.
  Avant, une voiture sortie de la piste tombait sans fin sur Snow et Stadium.
- **Décors** (`RaceSession::scenery`, ov003 0x0211d37c / 0x0211d02c / 0x0211ccec / 0x0211ca88) : Stadium 2 tribunes
  `Tracks/sta/StadiumSide` et 2 écrans `StadiumScreen`, Island 2 `misc/island/large` + 2 `small`, Snow 2
  `misc/snow/lg_snow` + 2 `sm_snow`, Coast `misc/coast/coastbackground` ; positions de la table autour du centre de
  la grille (320 ou 640), nœuds 1 et 3 (et Coast) tournés d'un demi-tour. Ils sont à 71 unités au moins de la zone
  de jeu (Coast 225) ; les pilotes les dessinent après le ciel avec son plan lointain (4000 ; celui de la course,
  600, les coupait), testés en profondeur entre eux, puis effacent la profondeur avant la piste (choix du portage ;
  ordre 0x15 du DS respecté). PSP : découpe `guardEdge` 28 / 36 / 28 / 90 (2,5 fois moins que leur distance).
  Sommets par image (PPSSPP, 300 images) : Coast hard/0 43 500 → 77 300, Stadium easy/0 104 000 → 146 000, Island
  162 000 → 184 000, Snow 37 000 → 68 000.
- **Son moteur** (`dsCarAudio::SoundRpm`, ov003 0x020f799c) : le 3e argument (octet +0x0c de l'audio voiture nul,
  remis à 0 par `dsHumanPlayer_Reset`) prend le régime et le rapport de l'entité voiture, que la mise à jour du joueur
  (0x0210ed9c) copie du moteur : rapport = moteur +0x20, régime lissé vers moteur +0x2c (0,3 de l'écart par image en
  descendant, 0,1 en montant : 0x0210f300 / 0x0210f304). Le portage prenait toujours le « régime sonore » tiré de la
  vitesse (7000 à 9000 dans chaque rapport), d'où un moteur toujours à fond.
- Outils : `tm_race --view ex,ey,ez,ax,ay,az` et `view.txt` à côté de l'EBOOT (PSP) : caméra fixe d'inspection.
  `RaceSession` signale un modèle de pièce introuvable (`no model ...`).
Coast, dessous des routes : texturé sur l'hôte et dans PPSSPP (Vulkan et rendu logiciel) aux mêmes points de vue ;
pas reproduit, photo demandée.
Vérifié : `run_checks.py` 114 et `--env all` 426 à 0 erreur, `run_checks_psp.ps1` 114 dumps identiques, PPSSPP
coa/hard/0, isl, sno, sta easy/0 et pla/easy/1 300 images identiques à l'hôte, thread audio 600 images sans trou,
`check_culling.py` 0 différence, `reload_test.ps1 -Count 242` (deux tours des 121 pistes du menu) sans fuite : le
script supposait encore 104 pistes de 26 par environnement et lisait les pistes de plate-forme comme une fuite ; il
prend maintenant l'environnement de chaque piste dans le journal. Paquet `package.ps1 -Perf` prêt.

## Reprise 4 (2026-10-21) : retour console de la reprise 3

**Console** : `late` 2 à 5 en une à deux minutes par piste ; `ge` à 0 sauf Stadium (pointe 10,2 ms) et Island
(1,3) ; pire seconde mise à jour + son + liste : Island 33,9 ms. Photos : les dessous des routes en l'air sont
transparents sur Coast et Snow (on voit le ciel, de dessus tout est normal) ; feux de départ gris fixes ; son moteur
« toujours à fond ». Le ciel et les logos du stade seraient animés sur DS (pas fait, priorité aux bugs).

- **Son moteur** (`ConvRpm`, ov003 0x020f93e0) : 0x0201201c avec 0x3fe00000 est `_dadd` 0,5 (l'arrondi des doubles,
  `dsRoundD`), pas un produit : seuils = valeur / 1024 x MaxRPM. Le portage les divisait par deux : RevMed se coupait
  à 7031 tr/min alors que le moteur tourne de 6000 à 9000 en course, donc moteur muet ou couche moyenne au maximum.
  Trace hôte coa/hard/0 : ralenti 2000 tr/min = Idle 89 / RevLow 26 ; en course RevLow 110 et RevMed 85 à 127 (volume
  et hauteur) selon le régime.
- **Feux de départ** (main 0x0206aaf4, ov003 0x02114ee4) : les matériaux `StartRed` / `StartAmber` / `StartGreen`
  (StartDonut du Stadium) ont des contrôleurs de lumière 0 / 1 / 2, couleurs 0x1f / 0x3ef / 0x3e0 ; le décompte met
  rouge 13 au-dessus d'1 s, orange 13 jusqu'à 0, vert 13 à 0 (niveau = alpha ; niveau 0 = forme cachée). Les
  surcharges de matériau (`G3Model::overrides`) s'appliquent maintenant aussi aux matériaux du lot (indice du lot) ;
  `RaceSession::startLights`, `ApplyStartLights`. Coast et Island n'ont pas ces matériaux.
- **Dessous transparents** : pas reproduits (hôte, PPSSPP Vulkan et rendu logiciel). Communs aux dessous : normale
  vers le bas, matériaux « faces avant » opaques, textures surtout 4 bits (underside1, SnowRoadWood2/3, GWunder,
  underside), mais des toits 4 bits s'affichent. Instrument : `diag.txt` (`package.ps1 -Diag`), au premier
  lancement 3 vues fixes sous des ponts (sno/easy/0, coa/easy/0 x 2), 7 variantes chacune (normal, sans culling, sans
  brouillard, sans texture, sans test alpha, texture seule, tout) en `diag_<vue>_<variante>.bmp`, puis le jeu
  continue (`RunDiag`, `G3GuSetDebug`). La variante qui fait apparaître les dessous désigne la cause.
Vérifié : 114 et 426 checks, 114 dumps PSP identiques, PPSSPP coa/hard/0, isl, sno, sta easy/0, pla/easy/1 300 images
identiques à l'hôte, thread audio sans trou, 32 rechargements sans fuite. Paquet `package.ps1 -Perf -Diag`.

## Reprise 5 (2026-10-21) : retour du diagnostic console

Console : son moteur et feux de départ validés par l'utilisateur. `frametimes.txt` : `late` 1 à 2 par piste (au
chargement), `ge` 0 sauf deux secondes de Stadium (9-11 ms).
- **Diagnostic** : sur console, les dessous des ponts sont **visibles** dans les 21 captures, variante normale
  comprise. Le diagnostic dessine toutes les pièces au niveau de détail 0 (le plus fin) ; en course, les pièces à
  plus de 20 / 60 unités prennent les niveaux 1 / 2 (arêtes de 8 / 24 unités). PPSSPP montre les dessous aux trois
  niveaux. Le diagnostic capture maintenant les niveaux 0, 1, 2 puis, au niveau 2, sans culling, sans texture, sans
  test alpha et tout coupé (variantes 00 20 40 41 44 48 5f, bits 5-6 = niveau) sur 4 vues (Snow près / loin, Coast
  près / loin). Prochaine étape : les captures console.
- **Terrain selon l'ordre des pistes** (`dsPieceDataTable::Load`) : le niveau du sol (3 sur Coast, 0 ailleurs)
  n'était fixé qu'à la construction du singleton, donc par le premier environnement chargé. Coast après un autre
  environnement n'avait plus de terrain (0 pièce au lieu de 44 sur easy/0 : ni îles ni falaises), Snow après Coast
  perdait ses piliers (109 pièces au lieu de 113), Island après Coast une partie de son terrain (29 au lieu de 33).
  Vérifié dans PPSSPP : coa → sno → coa → isl → sta → coa donne 44 / 113 / 44 / 33 / 0 / 44.
- `view.txt` (PSP) prend un 7e nombre facultatif, le niveau de détail.
- **Dessous transparents : cause trouvée pour Snow** (console : « plus de problème » avec ce paquet). Le diagnostic
  console montre les dessous aux niveaux 0, 1 et 2 dans toutes les variantes : le rendu n'y est pour rien. C'est le
  niveau du sol ci-dessus : une pièce n'utilise son modèle « air » que au-dessus du niveau du sol ; sur Snow (ou
  Island) joué après Coast, le niveau valait 3 au lieu de 0 et les pièces à 1-3 cases de haut prenaient leur modèle
  « ground », qui n'a pas de dessous (19 paires sur 46 à Snow, 26 sur 42 à Coast, 29 sur 80 à Island : le modèle air
  a des faces vers le bas, le ground aucune). Reproduit sur l'hôte en forçant le niveau 3 sur sno/easy/0 : le bout
  de `roadwood_bislope_end` (case y = 2) n'a plus que son contour vu d'en dessous, comme sur la photo. Les photos de
  Coast venaient de la première course de leur session (bon niveau) : non expliquées ; à surveiller.
Vérifié : 114 et 426 checks, 114 dumps PSP identiques, coa/hard/0, sno et sta easy/0 identiques à l'hôte,
32 rechargements sans fuite. Paquet `package.ps1 -Perf -Diag` (4 vues x 7 variantes, environ 11 Mo de BMP).

## Reprise (état au 2026-10-20, fin de session)

Tout le travail des étapes 6 (son), 7 (quatre environnements, niveaux de détail GE) et 8 (plate-forme) est **dans
l'arbre de travail, non commité** (l'utilisateur valide les commits lui-même). Pour reprendre :
1. `cmake --build build/host` et `bash tools/psp_build.sh` (0 avertissement attendu).
2. Vérifications : `python -P tools/host/run_checks.py` (114, Coast) et `--env all` (426) à 0 erreur ;
   `tools/psp/race_shot.ps1 -Track coa/easy/0` puis `pla/easy/1` : voiture, caméra et son identiques à l'hôte ;
   `tools/psp/reload_test.ps1` (32) sans fuite (`-Count 208` : deux tours complets, à relancer une fois avec le
   critère final, environ 8 min) ; `tools/psp/run_checks_psp.ps1` (114 dumps identiques, vérifié avant l'étape 7).
3. Proposer les commits par étape (6c son, 7 environnements + LOD, 8 plate-forme), puis `tools/psp/package.ps1`
   pour le test console (son, vitesse du GE sur Island, médailles plate-forme).
Pistes suivantes : mode puzzle (placement de pièces : à partir de l'éditeur), front end du jeu (ov004), écran du bas
(temps cible / meilleur, retries) dans un coin, LOD par région pour les grandes pièces d'Island si la console rame.
Pièges de l'environnement : les heredocs bash de l'outil transforment `\\n` en vrai saut de ligne (écrire les
scripts d'édition avec l'outil Write) ; Python sous Windows écrit en CRLF sans `newline=''`.

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

(The HUD messages are done in step 5c, which corrects two points below: the character map is in config/ui, and
the backing's panel +0x14 belongs to another HUD part. The sea is done in step 5d.)

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

## Bilan de la phase 5, run 4 (rapport, 2026-10-20)

**Ombre noire, jeux d'éclairage, messages du HUD, mer animée et son : la phase 5 a fait le tour de ce qui manquait à
la course Coast.** Tout est vérifié sur l'hôte et dans PPSSPP ; l'effet sur la console reste à confirmer.

- Ombre de la voiture noire (émission du matériau, alpha 14/31) et jeux d'éclairage remis dans le bon sens (voiture =
  jeu 0, piste = jeu 1) ; messages du HUD (« Roue libre ! », checkpoints manqués) avec les polices, la mise en page
  et les chaînes localisées du jeu ; la mer telle que le jeu la dessine (5 x 5 quads translucides texturés qui
  défilent, abaissés quand la caméra monte).
- Son (étape 6) : le pilote son du DS sur le CPU (séquenceur SSEQ, 16 canaux, PCM/ADPCM, 32 kHz) sous les couches du
  jeu (gestionnaire de sons, sources 3D, audio de la voiture, état audio) ; musique Coast et musique du menu, moteur
  à trois couches, dérapages, chocs, klaxon (L), compte à rebours, checkpoints, sons des menus ; fondu du moteur à la
  pause. Sur PSP un thread audio joue le son sur un canal SRC de sceAudio.

Vérifications : hôte et PSP sans avertissement, 114 vérifications à 0 erreur, voiture, caméra **et son identiques à
l'hôte échantillon pour échantillon** dans PPSSPP (easy/0 150 images, hard/0 600 images), thread audio sans trou
(600 images), 32 rechargements à tas constant. Coût du son dans PPSSPP : 2,7 à 2,9 ms par image (8 %).

**Prochaine étape = test console** (copier `build\psp\package\PSP`, 28,8 Mo) : écouter le son (moteur surtout : sa
courbe de régime suit le code mais mérite une écoute comparée au DS), regarder le temps du son dans l'overlay R
(deuxième nombre). Ensuite : les trois autres environnements (Island, Snow, Stadium), puis le front end du jeu.

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

## Phase 6 : le front end (en cours, 2026-10-21)

Demande : interface fidèle au DS en contenu, présentée comme le menu de TrackMania Wii adapté au 16:9 (maquette de
l'utilisateur), fond `assets/fe/background.png` (fourni par l'utilisateur, pas dans la ROM). Ordre : front end, puis
fantômes, puis éditeur ; multijoueur en bonus à la fin.

Inventaire du DS (ov004, 1308 fonctions, 219 Ko, rien de nommé encore) : chaque écran est une classe dont deux
petites méthodes rendent ses fonds `FrontEnd/Screen/<nom>TopBG` / `...BottomBG` (105 fonds, 256 x 192, NSCR + NCGR +
NCLR), une son titre et une sa liste de boutons (clés de `strings/main/frontend`, 904 chaînes). Menu principal :
SoloButton, EditorButton, NetworkButton, ShopButton, ProfileButton, OptionsButton ; Solo : Race, Puzzle, Platform,
QuickRace, CustomRace ; puis choix d'environnement, de difficulté, de piste (ChallengeBottomBG, sprites lettres,
chiffres, médailles, cuivres), résultats, profil (nom, véhicule en 3D, statistiques), options (langue, commandes,
affichage, son, effacement, crédits), boutique (pistes, blocs, skins). Données : `Scripts/config/RaceProgression`,
`PlatformProgression`, `PuzzleProgression`, `TrackShop`, `BlockShop`, `SkinShop`, `rewardShop`, `challenges`, `ui`.
Ressources 2D : sprites `FE/Cell/*` (icônes d'environnement, médailles, cuivres, difficultés, casque), polices
`Fonts/Cell/Font20/24/28/32/40_White` (sans-serif arrondie proche de celle de la Wii).

Maquette : `tools/fe/mockup_main.py` (Python, ressources du jeu + fond) : titre en onglet, trois cartes (illustrations
du haut du DS : SinglePlayerTopBG, EditorTopBG, MultiplayerTopBG), trois boutons larges (Shop, Profile, Options),
sol blanc courbe, sélection au pad (carte agrandie, contour bleu), rappel des touches.

### Étape 1 : le moteur de menu et les écrans (fait ; test console à faire)

Le menu provisoire de `RaceApp` est remplacé par le front end, commun à l'hôte et à la PSP (même API : `Start`,
`FindTrack`, `NextTrack`, `Unload`, `RaceTracks`, `screen`, `race`, `menus`, auto.txt / reload.txt / diag.txt,
`--frames/--screen/--finish-at` inchangés ; les 114 / 426 vérifications, la PSP et `race_shot` sont identiques).

- **Dessin** `src/app/fe_canvas.*` : `FeCanvas` dessine en coordonnées 480 x 272 (1:1 sur PSP, x1,6 centré sur la
  fenêtre 768 x 576 de l'hôte) par trois rappels du pilote : image (`FeImage`, modulée en couleur), cellule DS
  (modulée), rectangle. Panneaux arrondis en 9 morceaux d'un disque anticrénelé généré (32 x 32, 8888), texte avec
  les polices DS `Font20/28/32/40_White` (métriques de `dsFontManager`, `dsTextLayout`, glyphes rendus blancs puis
  teintés ; le haut des capitales vient des pixels, les boîtes des cellules étant celles des OAM), icônes DS
  `FE/Cell/Coppers` (médailles bronze / argent / or, pièce), `EnvIcons`, `DifficultyIcons` (palette `FrontEnd.NCLR`).
- **Images** : `tools/fe/convert.py` convertit hors ligne le fond et les cartes de l'utilisateur (`assets/fe`) et 33
  fonds hauts du DS (partie au-dessus du bandeau titre) en fichiers `.fet` (5650 opaque, 8888 pour les icônes,
  largeur puissance de 2, 1,2 Mo en tout) dans `build/fe/data` ; la PSP les lit dans `fe/` à côté de l'EBOOT
  (copiés par `race_shot.ps1`, `reload_test.ps1`, `package.ps1`, qui relancent la conversion si besoin), en RAM
  (rien en VRAM). Hôte : `--fe <dir>` (défaut `build/fe/data`). Tas PSP au menu : 3,5 Mo.
- **Écrans** (`src/app/fe_menu.cpp`, textes des clés DS `strings/main/frontend` + `backend`, langue de la console
  ou de l'option) : menu principal comme la maquette validée (cartes Solo / Éditeur « Bientôt » / Multijoueur
  « Bientôt », boutons Boutique / Profil / Options, sélection agrandie au contour bleu, rappels X / O) ; Solo (Course,
  Puzzle et Mes Circuits verrouillés avec les messages DS `lockedpuzzlemode` / `lockedcustommode`, Plate-forme
  verrouillée tant qu'il manque une médaille dans un des 4 environnements `lockedplatformmode`, Course rapide) ;
  environnement (4 cartes, médailles de l'environnement) ; difficulté (médailles par difficulté, verrou
  « Remportez N Médailles en course pour déverrouiller cette difficulté ») ; liste des circuits (meilleur temps,
  médaille, panneau : temps or / argent / bronze des fantômes fournis, meilleur temps ; piste 5 des difficultés
  entraînement..difficile à acheter) ; heure du jour (course rapide : matinée / journée / soirée, appliquée à la
  session) ; peinture (10 skins par environnement = `Cars/<env>/0..9`, appliquée) ; profil (nom = pseudo de la
  console, véhicules, statistiques) ; options (langue, commandes, affichage = caméra basse / haute par
  environnement, son = musique / effets du menu, données = effacer les temps ou tout, crédits) ; boutique (circuits
  et skins par environnement avec prix en coppers, confirmation `shopconfirmpurchase`, `shopinsufficentfunds` ;
  blocs verrouillés = éditeur). Pause (Reprendre / Recommencer / Passer circuit / Quitter avec confirmation) et
  résultats (temps, meilleur temps, médailles remportées, coppers reçus, essais en plate-forme ; Réessayer / Course
  suivante / Changer circuit / Menu principal) dans le même style. Transition simple (glissement + fondu, 6 images).
- **Règles** (lecture des tables Scripts/config ; à confirmer dans ov004, voir plus bas) : une médaille par fantôme
  battu (or = 3 médailles) ; les temps sont lus dans l'en-tête des fantômes comme le DS (`ov004 0x020fa3ec`,
  porté : +8 si la version +4 vaut 6, appelé par l'écran de choix des défis `0x020fa49c` pour or, argent, bronze) ;
  difficultés débloquées par le total de médailles course (RaceProgression : 0 / 40 / 90 / 140 / 190 / 200) ou
  plate-forme (0 / 8 / 20 / 32 / 48) ; coppers = montant du défi (50..300) par médaille gagnée, rien en course
  rapide ; TrackShop « StadiumA5 » = 5e piste de la difficulté de rang 0..3. La sauvegarde DS garde un u32 par défi
  (104, `0x020a9678`), sans doute le meilleur temps : les médailles s'en déduisent, comme ici.
- **Sauvegarde** `save.txt` : lignes existantes + `coppers`, `owntrack`, `ownskin`, `skin`, `camera`, `lang`,
  `music`, `sfx`, `races`, `finishes` (les anciennes sauvegardes se relisent).
- **Captures** : hôte `tm_race --frames 10 --screen menu --keys <touches>` (U D L R, O = X, B = retour) ; PSP
  `race_shot.ps1 -Track menu -Frames 20 -Screen <touches>`.

### Étape 2 : les fantômes (fait ; test console à faire)

- **Format** (lu dans le DS) : en-tête `main 0x02095ad8` = u8 1, u8, u16 60 (ticks par seconde), u32 6 (version),
  u32 temps d'arrivée, u32 taille du flux, u32 tours, u32 checkpoints, tours x checkpoints temps de passage, puis le
  flux (certains fichiers ont des octets en plus après, inutilisés). Enregistrements `main 0x02095c5c` : octet de
  drapeaux puis 1 : axe haut (ligne 1) += 3 x s16 ; 2 : axe avant (ligne 2) += 3 x s16 ; 8 : position x, z += 2 x
  s16, sinon 0x10 : position = 3 x s32 ; 0x80 : durée en ticks (u8, sinon 1) ; 0x20 / 4 : un s8 chacun ; 0x40 : 4
  octets. Lecture `0x02095f70` : un enregistrement par tick de 1/60 s, interpolé sur sa durée, ligne 0 = haut x avant.
  Vérifié : le flux de chaque fichier testé est consommé exactement ; le premier enregistrement de coa/easy/0 est à
  (131026, 90015, 589824), notre voiture au départ à (131030, 90029, 589824) ; le fantôme bouge au tick 8 (départ).
- **Port** `src/game/race/dsGhost.*` (indépendant de la plate-forme) : `Load` (liste d'échantillons), `Matrix(tick)`
  (position interpolée dans un enregistrement long), enregistrement (`BeginRecord` / `Record` une fois par image =
  2 ticks : drapeaux 0x80 | 0x10 | 1 / 2) et `Save` au même format que le jeu.
- **Course** : `RaceSession` enregistre la course du « go » à l'arrivée et dessine jusqu'à 3 fantômes (la caisse
  seule, à leur matrice pour le tick de l'horloge de course) avec le modèle de la voiture rendu translucide
  (`GhostLook` : alpha 12 sur tous les matériaux, feux cachés, surcharges remises après). `RaceApp::LoadGhosts` :
  l'adversaire de la prochaine médaille à gagner (or une fois gagné ; défis course seulement : « Battez les Ghosts
  des adversaires pour gagner des médailles ») et le meilleur passage du joueur (`ghost_<piste>.bin` à côté de
  save.txt, écrit à chaque record). Options Affichage : « Ghosts concurrents » et « Ghosts joueur » Oui / Non.
- **PSP** : la voiture éclairée à chaque dessin gardait l'alpha du maillage en cache ; `g3d_gu.cpp` prend
  maintenant l'alpha du dessin (même arrondi que `GuSink::Color8888` : la voiture du joueur ne change pas). Coût
  mesuré dans PPSSPP sur coa/hard/0 : dessin 4,68 -> 5,24 ms par image (+1 500 sommets), tas +370 Ko, `late 0`.

## Bilan de la phase 6, run 2 (2026-10-21)

Fait : moteur de menu commun, tous les écrans hors réseau et éditeur (dont pause et résultats), fantômes (temps des
médailles, lecture, enregistrement, affichage). Vérifications : `cmake --build build/host` et
`bash tools/psp_build.sh` sans avertissement ; `run_checks.py` 114 / 0 erreur et `--env all` 426 / 0 erreur ;
`run_checks_psp.ps1` 114 / 0 différence ; `race_shot.ps1 -Track coa/hard/0 -Frames 300` voiture, caméra et son
identiques à l'hôte, `late 0` ; `reload_test.ps1` 32 rechargements sans fuite ; captures des menus regardées (hôte et
PPSSPP) ; `package.ps1 -Perf` fait (30,1 Mo).

À tester sur la PSP-3000 :
1. Démarrage sur le menu principal (fond, cartes, polices nettes à 1:1), navigation croix / X / O, sons du menu,
   musique ; O sur le menu principal quitte le programme.
2. Solo > Course > environnement > difficulté > circuit > peinture : la course démarre avec la peinture choisie ;
   Course rapide > environnement > circuit > heure du jour (matinée / journée / soirée visibles en course).
3. En course : le fantôme translucide de la médaille visée (bronze au début) part avec vous au « go » ; perf
   (R : `late` doit rester à 0 avec le fantôme) ; START : menu pause dans le nouveau style, Quitter demande
   confirmation.
4. Arrivée : écran de résultats (temps, meilleur temps, médailles et coppers gagnés) ; refaire la piste : votre
   fantôme (meilleur passage) apparaît aussi ; `ghost_<piste>.bin` est créé à côté de save.txt.
5. Boutique : acheter une piste « 5 » et une skin avec les coppers gagnés ; Profil : nom = pseudo de la console,
   statistiques ; Options : langue (le changement s'applique tout de suite), caméra haute / basse par
   environnement, musique / effets, Ghosts concurrents / joueur, effacement des données.
6. Verrous : Puzzle, Mes Circuits, Blocs (éditeur) et Plate-forme (une médaille par environnement) affichent les
   messages du DS ; Éditeur et Multijoueur « Bientôt ».

Reste (lectures à confirmer dans ov004, sans test console nécessaire) : le compte exact des médailles pour les
déverrouillages, l'attribution des coppers, la correspondance TrackShop A..D = difficultés ; l'avatar et la saisie du
nom (le pseudo de la console sert de nom) ; un aperçu 3D de la voiture au choix de la peinture ; le choix de
l'adversaire (`selectghosttitle`) et la sauvegarde manuelle des fantômes (`saveghostbutton`).

## Phase 7 : l'éditeur de circuits et le mode puzzle (2026-10-21)

Demande : « passe à la suite » après la phase 6 (front end, fantômes) ; ordre convenu : éditeur, puis le reste du DS,
le multijoueur en bonus à la fin.

### Ce que fait le DS (lu dans ov002 et main)

L'éditeur est ov002 (393 fonctions, chargé avec ov003, la course) : `build/decomp/ov002.c`, annoté par
`tools/analysis/annotate_decomp.py` (valeur et chaîne de chaque littéral) en `ov002a.c`. Noms :
`config/names/phase7_editor.txt`.
- **Menu des blocs** (ov002 0x020e79d0 / 0x020e763c) : `Scripts/screens/<Env>/TrackPieceLayout`, une table Lua par
  dossier, `{ FOLDER (4096) ou PIECE (0), nom, "0x<icône>" }`, racine `TopFolder` ; icônes des cellules de
  `Editor/Cell/<Env>Folders` et `<Env>Blocks` (palette `Editor<Env>.NCLR`, 32 x 32, environ 190 par environnement).
  Les puzzles ont leur propre menu `Scenes/puz/<diff>/<n>-Layout` : une seule liste, 4e champ = nombre de blocs.
- **Grille** (main, singleton `*0x020d6628`, constructeur 0x0208da98) : un octet par case (indice de pièce),
  tailles de `dsRaceSetup_GridSize` (20 x 20 x 20, Island et Coast 22 x 20 x 22 avec une bordure d'une case, +0x414),
  au plus 250 / 127 / 240 / 127 pièces (main 0x020b8f5c). Une pièce occupe les cases de son `NotchData` tourné par
  0x0209e0a8 (H couches, D lignes vers +z, W colonnes vers -x, W et D échangés aux rotations impaires) ; une pièce
  de Snow sur piliers occupe aussi la colonne en dessous (0x0208d1d4 avec la pièce +0x94). Limites : 0x0208cf94.
- **Poser** (ov002 0x020ddf98) : les pièces sous le bloc sont supprimées, sauf une pièce verrouillée du puzzle
  (+0x77, 0x0208d098 : refus) ; puis le terrain de Coast / Island est reconstruit (ov003 0x02100b2c).
- **Budget** (ov002 0x020f091c ; 0x020ebe14 ajout, 0x020ebfb4 retrait, 0x020ebe90 plein) : polygones = somme des
  CostPoly, plein à 190 ; mémoire = une unité par pièce + CostMem au premier exemplaire de chaque id (le terrain ne
  compte que son unité et ses polygones), plein à 612 / 630 / 612 / 642 ; pilier de Snow : table 0x020ee814 selon la
  hauteur (x 2 si CostMem < 6). Jamais plein en puzzle (mode 7). Les pistes officielles dépassent parfois ces limites
  (jusqu'à 211 polygones, 707 de mémoire sur coa/bonus/4) : elles n'ont pas été faites dans l'éditeur.
- **Validation** (ov002 0x020e5808) : pas de départ ni multitours → `validate1startpiece` ; plus d'un →
  `validateonly1start` ; départ simple sans arrivée → `validate1finishpiece` ; multitours avec un départ ou une
  arrivée en plus → `validate1multilapnostartfinish` (le code 6 `validatecheckpoint` n'est jamais produit). Un
  multitours passe le nombre de tours à 2 au moins. Puis course de validation : le temps devient le temps de
  l'auteur, médailles + 2 / + 3 / + 5 s (0x0209fa5c).
- **Objectifs** (ov002 0x020e3d68) : type de défi (course / plate-forme), difficulté, tours (2 à 30, multitours).
- **Format compacté** des circuits perso (lecture ov003 0x02119bec + 0x0211a1f0, écriture 0x0211a7e0, bits de
  poids faible d'abord) : 12 x (nom[i] 8 bits, auteur[i] 8 bits), tours 8, type 1, +0x02 8, environnement 3, heure du
  jour 2, difficulté 4, date (année 7, mois 4, jour 5, heure 5, minute 6, puis 6 bits que l'écriture remplit avec
  l'heure), temps auteur / or / argent / bronze en u32 (octet fort d'abord), 3 octets, nombre de pièces 7 (126 au
  plus) ; par pièce id 8 + 2, rotation 2, case x, y, z 5 chacune. Le terrain n'est pas écrit.
- **Son** (main 0x0208c3cc, `dsGameAudio::StartEditor`) : les archives des menus et `SEQARC_EDITOR` ; l'essai et la
  validation jouent l'ambiance de l'environnement (environnement + 4 de 0x0208c04c).
- **Déblocages** : Puzzle et Mes Circuits s'ouvrent quand un circuit validé est sauvegardé (`lockedpuzzlemode`) ;
  puzzles : `config/PuzzleProgression` = { difficulté, numéro, médailles puzzle requises (0 / 8 / 20 / 32 / 48),
  or, argent, bronze, coppers par médaille } ; le puzzle k d'une difficulté est `Scenes/puz/<diff>/<k>`,
  environnement k.

### Le portage

- **Modèle** `src/game/editor/dsEditorTrack.*` (commun) : menu des blocs, grille d'occupation avec les masques,
  `Place` / `CanPlace` / `Remove` / `Clear`, terrain reconstruit à chaque changement, budget, `Validate`, format
  compacté (`SavePacked`, `LoadPacked`, `ReadHeader`), annulation (instantanés, ajout du portage).
  `tm_trackdump --editor` charge les 104 pistes et les 17 puzzles dans ce modèle : toutes dans les limites, 0 erreur
  de validation, aller-retour du format compacté identique (pièces, rotations, positions), toutes les entrées des
  4 menus trouvées dans PieceData (75 / 57 / 55 / 53 dossiers, 172 / 144 / 130 / 123 blocs).
- **Session** (`RaceSession`) : `Load` découpé en `Build` commun ; `LoadView` (décor, ciel, éclairage, lot de
  textures sans voiture ni course) pour la vue de l'éditeur, `SyncPieces` après chaque modification (modèles chargés
  une fois), `LoadTrack` (course sur une copie des pièces de l'éditeur, mode 6 = essai sans départ possible).
  Dessins de l'éditeur : `editorDraws` (le bloc porté, translucide, teinté de rouge s'il ne peut pas être posé :
  `GhostLook` généralisé) et `lines` (lignes en coordonnées monde, dessinées par les deux pilotes après les opaques).
  `dsRaceProgress::Drop` place la voiture de l'essai.
- **Écran** `src/app/editor.cpp` (+ `editor.h`, état `RaceApp::Editor`) : curseur sur la grille à la croix (selon
  l'orientation de la caméra), L / R hauteur, stick = caméra orbitale (rotation, zoom), X poser (sans bloc : prendre
  le bloc sous le curseur, le « Select » du DS), carré tourner, O effacer, triangle menu des blocs (gauche / droite,
  X ouvrir, haut = dossier parent comme « Press Up to backtrack »), START menu (Reprendre, Tester, Valider,
  Objectifs, Sauvegarder, Charger, Annuler, Tout effacer, Aide, Quitter ; puzzle : sans objectifs ni fichiers),
  SELECT aide. Repères : bord de la zone de construction et grille locale à la hauteur du curseur, boîte du bloc
  (bleue / rouge, jaune sur un bloc existant, orange s'il est verrouillé), colonne jusqu'au sol, jauge mémoire.
  Saisie du nom par clavier à l'écran (12 caractères), écrasement confirmé, fichiers `tracks/<nom>.trk` à côté de
  save.txt (mêmes octets que le DS). Les textes viennent des chaînes DS (`help*`, `customdata*`, `validate*`,
  `exitconfirm`, `queryclearall`...) ; l'aide des commandes est un texte du portage.
- **Front end** : carte Éditeur active → choix de l'environnement ; Solo > Puzzle (liste des 17, verrous par
  médailles de puzzle, temps des médailles) ; Solo > Mes Circuits (circuits sauvegardés, non validés verrouillés avec
  `customdatasavevalidatemsg`). Menus pause / résultats selon la course (`RaceApp::RaceMenu`) : essai et validation
  (Reprendre, Recommencer, Editer le circuit), puzzle (… Modifier le puzzle, Puzzle suivant), Mes Circuits.
  Sauvegarde : `editor 1`, `puzzle <n> <temps>`, `custom <temps> <nom>`.
- **Pilotes** : manette de l'éditeur (`RaceApp::EditorPad`) ; hôte : flèches, Z X A S = croix rond carré triangle,
  Q / W = L / R, Entrée = START, Tab = SELECT, J L / I K = stick ; `tm_race --editor <sta|isl|sno|coa|puz<n>>
  [--pad <touches>]` ; PSP : `auto.txt` « editor:<...> <images> <touches> » pour les captures PPSSPP.
- `G2CellBank` : l'atlas passe à 512 de large au-delà de 512 lignes (limite du GE ; les icônes de blocs faisaient
  256 x 1024 et ne s'affichaient pas sur PSP).

### Vérifié (2026-10-21)

`cmake --build build/host` et `bash tools/psp_build.sh` sans avertissement ; `tm_trackdump --editor` 0 erreur ;
`run_checks.py` 114 / 0 erreur et `--env all` 426 / 0 erreur (le découpage de `RaceSession::Load` ne change rien) ;
`run_checks_psp.ps1` 114 dumps / 0 différence ; `race_shot.ps1` coa/hard/0 (300 images), sno/easy/0 et pla/easy/1
(150) : voiture, caméra et son identiques à l'hôte, `late 0` ; `reload_test.ps1` 32 rechargements sans fuite.
Éditeur, hôte et PPSSPP (scripts de touches) : palette et icônes, pose, rotation, suppression, terrain de Coast,
validation refusée (message DS), course de validation (4,81 s sur l'hôte et la PSP) puis retour à l'éditeur avec
or / argent / bronze, essai libre depuis le curseur, saisie du nom et sauvegarde (69 octets pour 5 blocs), « editor 1 »
dans save.txt, Mes Circuits (liste, course, record « custom »), puzzles (Facile 1, Extrême 1 : blocs verrouillés,
quantités, temps des médailles, menu pause du puzzle). PSP : tas hors capture du son identique après 1, 2 et 3
allers-retours éditeur → validation → éditeur (6 348 600 octets).

À tester sur la PSP-3000 :
1. Menu principal > Éditeur de circuit > un environnement : vue, grille, curseur à la croix, caméra au stick,
   L / R hauteur ; triangle : menu des blocs avec les icônes DS, X pour ouvrir / prendre, haut pour remonter.
2. Poser, tourner (carré), effacer (O), prendre un bloc posé (X sans bloc) ; Coast / Island : falaises et plage
   autour des routes au sol ; Snow : piliers sous les blocs en hauteur ; jauge mémoire.
3. START > Tester (la voiture part du curseur ; START en course > Editer le circuit), Valider (messages DS si pas
   de départ / d'arrivée ; à l'arrivée retour à l'éditeur avec les temps), Objectifs, Sauvegarder (clavier à
   l'écran, START = OK), Charger, Annuler, Tout effacer, Aide (SELECT), Quitter (sauvegarde proposée).
4. Après une sauvegarde validée : Solo > Puzzle et Solo > Mes Circuits s'ouvrent ; un puzzle (blocs comptés,
   pièces du puzzle intouchables, Valider = course, résultats : Modifier le puzzle / Puzzle suivant) ; Mes Circuits :
   course sur le circuit sauvegardé, médailles de sa validation. Fichiers : `tracks/<nom>.trk` à côté de l'EBOOT.
5. Perf de la vue de l'éditeur (R n'affiche plus le chronométrage dans l'éditeur : c'est la touche hauteur).

Reste (éditeur) :
- ~~Boutique des blocs~~ : faite (Reprise 6).
- ~~Dessin libre, placement du véhicule~~ : faits (Reprise 7). Reste la sauvegarde d'un fantôme par circuit
  (`CustomDataSaveGhost`).
- La musique de l'éditeur (le DS n'en lance pas dans `StartEditor` ; à confirmer), les budgets des pistes
  officielles (au-dessus des limites : non utilisées dans l'éditeur).

## Reprise 6 (2026-10-21) : retour console de la phase 7

- **Musique hachée sur la page « Éditeur de circuit »** du front end : les pages sont reconstruites à chaque image
  (`Items`), et celle-ci relisait le dossier `tracks/` et l'en-tête de chaque circuit sauvegardé, quatre fois par
  image (Mes Circuits aussi) : accès carte mémoire en continu, images en retard, son coupé. La liste est lue une
  fois (`RaceApp::SavedList`) et relue seulement après une sauvegarde ou une suppression.
- **Flèches de sens des départs, arrivées, checkpoints et multitours** dans l'éditeur : le DS crée ces modèles
  seulement dans les modes éditeur (drapeau `*0x021378dc`, mis par ov002, remis à 0 par la course ; main 0x0208cc74
  les montre ou cache tous). `dsTrackPiece_CreateArch` (ov003 0x0211eac0) : le modèle `Misc/helpers/<env>/<Helper>`
  de la pièce (Island, Snow, Coast), tourné comme la pièce, à la position de la pièce (pas le coin de l'empreinte),
  décalé de 16 en x pour quelques portes de Coast larges de 2 cases tournées de 2 ou 3 quarts ; sans Helper
  (Stadium) l'arche `Tracks/sta/Arch<type>` décalée de l'InitPos, puis `dsTrackPiece_CreateHelper`
  (0x0211e74c) : la flèche `Tracks/sta/Arrow<type>` orientée par `dsMtx43_FromAxes` sur l'InitDir tourné par la
  table 0x02134fbc (pi, 3pi/2, 0, pi/2), la pièce 0x10 de Stadium ayant sa flèche debout (quart de tour autour de
  -X, 8 unités en z). Portage : `dsTrackPiece::EditorMarkers`, dessinés translucides (alpha 20, le +0x08 = 0x14 du
  nœud) par l'éditeur (`editorDraws`, ghost 4, jusqu'à 64 dessins).
- **Choix des fantômes adversaires** (course, hors course rapide et plate-forme) : le menu pause du DS a un
  sous-menu « Concurrents » (ov003 0x02130a54 : Tous, Or, Argent, Bronze, Aucun, avec le temps de chaque fantôme) ;
  porté dans le menu pause (le choix s'applique tout de suite) et dans Options > Affichage (« Ghosts concurrents »,
  X fait défiler les cinq valeurs). Sauvegarde : `ghostchoice <n>` (l'ancienne ligne `ghosts 0 x` = Aucun).
  Défaut : Or. Jusqu'à 4 fantômes dessinés (3 adversaires + le meilleur passage du joueur).
Vérifié : hôte et PSP compilent sans avertissement ; `tm_race --track coa/easy/0` charge 1 / 1 / 1 / 3 / 0
fantômes pour Or / Argent / Bronze / Tous / Aucun ; sous-menu capturé ; PPSSPP coa/hard/0 300 images identiques à
l'hôte (`late 0`) ; marqueurs visibles dans l'éditeur (hôte : Stadium, Island, Coast ; PPSSPP : Stadium).
- **Boutique des blocs** (suite de la phase 7) : `config/BlockShop` (main 0x020b1b4c : 147 blocs, `Name` =
  `<STA|ISL|SNO|COA>_<nom PieceData>`, `Env`, `Price` en coppers de 5 à 50, `ViewScale` de l'aperçu 3D du DS) ;
  main 0x020b21e4 dit si un bloc est vendu et pas encore acheté (comparaison du nom sans le préfixe, sans la
  casse) ; ov002 grise alors son icône dans le menu des blocs et refuse de le prendre, sauf en puzzle. Portage :
  `RaceApp::BlockLocked`, onglet Blocs de la boutique (environnement, liste avec l'icône de l'éditeur et le prix,
  panneau avec l'icône en grand ; achat confirmé comme les pistes et les skins), dans l'éditeur icône grisée
  marquée d'une pièce et message `lockedrequirementpurchase`. Sauvegarde : `ownblock <n>` ; « tout effacer » les
  remet en vente. L'aperçu 3D tournant du DS (ViewScale) n'est pas porté : l'icône de l'éditeur le remplace.
  Vérifié : achat (100 → 75 coppers, `ownblock 0`), Multilap grisé puis disponible dans l'éditeur, boutique
  capturée sur l'hôte et dans PPSSPP.

## Reprise 7 (2026-10-21) : dessin libre et placement du véhicule

- **Dessin libre** (ov002 0x020e62cc / 0x020e5b48 / 0x020e5f58) : quatre blocs de route par environnement
  (ov002 0x020eff0c : `straight corner tshaped cross`, Island `highway*`, Snow `woodroad*` dont `woodroadx`, Coast
  `coastroad*`) et leurs côtés ouverts par rotation (ov002 0x020ee7a0, `[rotation][bloc]` = 5 12 13 15 / 10 6 14 15 /
  5 3 7 15 / 10 9 11 15 ; bits 1 +z, 2 -x, 4 -z, 8 +x). Un bloc est créé avec la rotation de la table puis tourné
  deux fois en arrière (ov003 0x0211fbd8), si bien que l'entrée de table d'un voisin à sa propre rotation donne les
  côtés qui font face à la nouvelle case. À chaque pas du tracé l'ancienne case prend le bit du côté du pas plus
  ceux de ses voisins et devient le bloc dont les côtés correspondent exactement (sinon elle ne change pas) ; la
  nouvelle case est ajustée à ses voisins seuls, et devient une ligne droite dans l'axe du pas si rien ne
  correspond (un cul-de-sac). La ligne droite du tout premier point est tournée dans l'axe dès que le tracé part
  selon z (ov002 +0x44). Un bloc identique déjà en place est gardé (0x020e5a38 : une ligne droite retournée ou un
  croisement tourné comptent pour le même) ; sinon le bloc remplace ce qui est dessous comme une pose normale. Le
  tracé ne descend pas sous le niveau du sol (0x020df178). Portage : `dsEditorTrack::FreeDrawBegin / FreeDrawStep`,
  mode « Dessin libre » du menu START de l'éditeur (X maintenu en déplaçant le curseur ; O pour finir ; une
  annulation par tracé). `tm_trackdump --editor` trace une boucle, une traversée et un T dans les 4 environnements :
  4 virages, 2 croisements, 1 T, aucun côté ouvert sans voisin.
- **Placement du véhicule** (« Placing Vehicle... ») : le DS pose une pièce d'id -1, le modèle
  `Tracks/<env>/TestArrow`, déplacée, montée et tournée avant l'essai ; la course part de sa position, direction -z
  tournée de sa rotation. Portage : START > Tester ouvre ce mode (croix / L / R / stick comme le curseur, carré
  tourne la flèche, X lance l'essai, O revient) ; la flèche garde sa place d'un essai à l'autre.
- Script de touches de l'éditeur : `[` appuie sur X et le garde jusqu'à `]` (tracés dans les captures).
Vérifié : `tm_trackdump --editor` 0 erreur ; captures hôte (boucle Stadium, flèche puis essai : voiture dans le sens
de la flèche) et PPSSPP (boucle sur Coast avec le terrain automatique, flèche) ; aide de l'éditeur mise à jour.

## Dépôt public et builder (2026-10-21)

- **Une seule version du code** : le dépôt public est ce dépôt-ci. Rien du jeu n'est suivi par git : ROM,
  `extract/`, données converties, `assets/` (dont les images de menu personnelles `assets/fe/*.png`) et les
  captures `docs/*.png` sont ignorés. `tools/check_public.py` vérifie chaque commit poussé (chemins, fichiers
  binaires, en-tête de ROM DS, signatures Nitro et zip en tête de fichier) depuis le hook `.githooks/pre-push`
  (`git config core.hooksPath .githooks`) et dans la CI (`.github/workflows/build.yml`, qui compile aussi l'EBOOT
  dans l'image `pspdev/pspdev`).
- **Images du menu** (`tools/fe/fe_pictures.py`) : celles de `assets/fe` si elles existent, sinon celles de la ROM :
  fond = haut de `MainMenuTopBG` (au-dessus du bandeau, éclairci à 50 % vers le blanc), cartes = haut de
  `SinglePlayerTopBG`, `EditorTopBG`, `MultiplayerTopBG`, icônes = sprites du front end (`Coppers` 2 : pièce d'or,
  `PlayerIcon` 0 : casque, `Wireless` 0 : console DS), agrandis ×4 au plus proche avant la réduction lissée.
  `TM_FE_ROM_PICTURES=1` ignore `assets/fe` (aperçu de la version publique :
  `tools/fe/mockup_main.py`). Avec `assets/fe`, les 40 `.fet` sont identiques octet pour octet à ceux d'avant.
- **Builder** (`builder.py`) : lit la ROM (`tools/nitrofs.py`, FNT + FAT ; les 13 fichiers sont identiques à
  `dsd rom extract`), vérifie le code jeu `BT2P` et le SHA-1 des 4 fichiers lus par l'EBOOT (`vfs.zip`, `vfs.bin`,
  `vfs.dat`, `audio/sound_data.sdat` ; une ROM rognée passe donc), convertit les images (`convert.py`, avec
  `TM_ROM_FILES`), compile `tm_race` en Release (psp-cmake natif, sinon une distribution WSL qui l'a, sinon Docker ;
  ou `--eboot`) et écrit `PSP/GAME/TM2PSP`. `tools/psp/package.ps1` passe par lui. Vérifié : depuis la ROM, via WSL,
  le paquet (EBOOT compris) est identique octet pour octet à celui de `psp_build.sh` + l'ancien `package.ps1`.
