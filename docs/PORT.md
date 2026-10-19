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
- The frame time is a global (*0x020d6a78, .bss) written by the main loop; host drivers set it (1/30 s).

### Slice 3c: car dynamics step (done)

`src/game/car/dsCarDynamics.*` holds the 0x554-byte simulation object (it starts with its own copy of
`dsCarParams`), its construction (sea level 2.5 on Coast, speed limit 180.6), wheel and engine setup, reset and
placement, and the whole step `dsCarDynamics_Step` (0x021222fc) split by topic: `dsCarCollide.cpp` (0x02124fa0 and
its probes/contacts/impulse), `dsCarForces.cpp` (0x02127f1c), `dsCarIntegrate.cpp` (0x02129098, 0x02129ce4),
`dsCarGrip.cpp` (0x02129f04, 0x0212a848, 0x0212a9c0), `dsCarSurfaces.cpp` (0x02122b54), `dsCarStep.cpp`
(0x021222fc, 0x02127374). Shared fixed-point idioms are in `car/dsCarMath.h`.

`tm_physics --root extract/files --drop` places the Coast car one unit above the start platform of coa/easy/0,
lets it settle for 90 steps (1/30 s), drives at full throttle for 150 steps and brakes for 60.
`tools/host/check_drop.py` checks the behaviour (no DS trace exists yet to compare with): **0 errors**, the car
settles level on four wheels (height 21.98 � 0.008), reaches 15 m/s in 1.4 s, shifts to 2nd, stops when braking
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
  (main 0x0208d404): the port takes it as a callback (`pieceRotationAt`), not wired in the test driver yet.

## Slice 4a: host race window (first pass)

`tm_race --root extract/files [--track easy/0]` opens an SDL2 window (768 x 576) and runs the ported code at 30 Hz:
track scene and piece placement, collision world, car parameters, engine and the full car step, driven from the
keyboard (arrows, R = restart). `--frames N --shot out.bmp [--auto]` renders N steps off-screen and saves the
last frame, which is how the renderer was checked (see `docs/host_race.png`: easy/0 after 4 s at full throttle).

- **SDL2** 2.30.12 is built as a static library from the official source release, whose GPG signature was checked
  against Sam Lantinga's key (fingerprint 1528 635D 8053 A57F 77D1 E086 30A5 9377 A776 3BE6), into
  `build/ext/sdl2` (not in git). The MinGW development archive has no signature, hence the source build. CMake
  skips `tm_race` when SDL2 is missing.
- **G3D stand-in** (`src/shim/host/g3d_gl.*`): MDL0 render commands (node, material, shape, scale up/down) and GX
  display lists replayed as OpenGL 1.x primitives; all DS texture formats decoded (tex4x4 included); polygon
  attributes give culling (DS front faces are counter-clockwise in GL terms) and alpha. Track pieces take their
  materials from `material.bin` + `texturebatch.n` (format in ASSETS.md).
- Car: `lod1` is drawn (body at the physics scale). `lod0` holds one wheel and tread at the origin, the shadow
  quads and a large "cone" shape that the game positions or hides itself in the visual update (0x02126d10).
- Not ported yet (stand-ins in tm_race): main loop/race manager (`main()` 0x02003000), cameras (`config/cameras`,
  a fixed chase view here), DS lighting/fog/toon (flat GL colours), the Coast terrain builder (cliffs and beaches
  are missing, the sea is a flat quad at 2.5), wheels, HUD, checkpoints/laps, sound.

## Next slices

1. **Cameras** (`config/cameras`), the race grid lookup for boosters (main 0x0208d404 / 0x0208cb80), car/car
   contacts (the +0x248 sums), and a dynamic trace of `dsCarDynamics_Step` in melonDS to compare the port bit for
   bit (the drop test only checks behaviour).
2. Terrain builder (ov003 0x02100b2c and callees) so Coast cliffs and beaches exist for collision and display.
3. Renderer, faithful pass: Firebrand's model drawing (dsModel, visual update 0x02126d10 for wheels/shadow),
   DS lighting from `config/tracks` (the eight colours), sky and sea from the environment loader (main
   0x02099e6c), then the same G3 path on the PSP (TMPM from phase 2).
4. Main loop from `main()` (0x02003000) and the race manager (countdown, checkpoints, laps, finish) replacing the
   tm_race stand-ins; HUD.
5. Dynamic coverage trace in melonDS to rank the 4,200 race functions.
