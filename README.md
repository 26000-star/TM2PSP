# TM2PSP

Native PSP port of **TrackMania Turbo (DS, Europe, `BT2P`)** through decompilation. See [STUDY.md](STUDY.md) for the feasibility study and the toolchain.

No game data lives in this repository. You need your own dump of the ROM.

## Layout

| Path | Content | In git |
|---|---|---|
| `baserom.nds` | Your ROM (hardlink or copy). SHA-1 `857b8f5f5ce9b91eaa04e35b99a591243672e51c` | no |
| `extract/` | `dsd rom extract` output: ARM9/ARM7 binaries, ITCM/DTCM, overlays, files | no |
| `config/` | `dsd init` output: per-module `symbols.txt`, `relocs.txt`, `delinks.txt` | yes |
| `tools/ghidra/` | Headless Ghidra scripts (dsd import, stats) | yes |
| `config/names/` | Function names recovered by hand, applied to `symbols.txt` | yes |
| `src/` | The port: `game/` (decompiled code), `shim/` (NitroSDK replacement), `lib/` (Lua 5.0.2, unzip), `host/` | yes |
| `docs/` | `PORT.md` (port architecture), `ASSETS.md` (data formats) | yes |
| `ghidra/` | Ghidra project `tmturbo` | no |
| `build/` | Logs and generated reports | no |

## Phase 0, reproducing it

Tools are installed as described in STUDY.md §6 (`C:\Tools`, plus pspdev in WSL `Ubuntu-24.04`).

```bash
dsd rom extract --rom baserom.nds --output-path extract
```

```bash
dsd init --rom-config extract/config.yaml --output-path config --build-path build --allow-unknown-function-calls
```

`--allow-unknown-function-calls` is a hidden dsd flag. Without it, `init` aborts on the Thumb function at `0x0209F79C`. Its function discovery misses that function, which is only reached by a `bl` from `0x0209F734`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\ghidra\analyze.ps1
```

This imports `extract/arm9/arm9.bin` at `0x02000000` (ARMv5T). `DsdImport.java` then adds ITCM, DTCM, BSS, the hardware regions and the 8 overlays (one Ghidra overlay space each). It applies dsd's function starts with their ARM/Thumb mode, plus all data labels, and attaches NTRGhidra's NitroSDK FID database. Full auto-analysis runs next, and `DsdStats.java` writes `build/ghidra_stats.txt`.
Open the result with `ghidraRun.bat` → File → Open Project → `ghidra/tmturbo.gpr`.

## Phase 0 results (2026-10-18)

- `dsd rom extract`: ARM9 main (`0x02000000`, 796 KB + BSS up to `0x020D6B00`), ITCM `0x01FF8000` (32 KB), DTCM `0x027E0000`, 8 overlays, ARM7. Nothing is compressed or encrypted. The `sdk_version` field is `0x04027539`, i.e. **NitroSDK 4.2**. The ARM9 has a `.ctor` section and the overlays have constructor tables, so the game is C++ (CodeWarrior). Its `std::map` code is visible at `0x0209F79C`.
- `dsd init`: **10,634 functions** (7,390 Thumb, 3,243 ARM, 1 unknown), plus about 5,400 data/BSS symbols. 149 warnings, all "call into the middle of a function" (shared tails), which dsd handles as external labels.

  | Module | Functions | Notes |
  |---|---|---|
  | main | 5,526 | game engine + SDK + Lua 5.0.2 + unzip |
  | itcm | 95 | hot code (math, interrupts) |
  | ov000 | 1,108 | |
  | ov001 | 766 | WFC/HTTP (drop) |
  | ov002 | 400 | |
  | ov003 | 986 | |
  | ov004 | 1,314 | |
  | ov005 | 153 | all ARM |
  | ov006 | 328 | GameSpy stats (drop) |
- Ghidra: all 10,634 entries were created with the correct mode (0 failures), and Ghidra found 42 more. Auto-analysis takes about 60 s. The decompiler handled all of a 202-function sample.
- **NitroSDK FID: 1 match (`init_cp15`).** NTRGhidra's FIDB covers only SDK 3.0 ARM-mode filesystem code, and this game uses SDK 4.2, mostly in Thumb. SDK identification needs better signatures. Options: build FID/dsd signatures from a public NitroSDK-4.x decomp (pokeplatinum, pokeheartgold), or use string and SDK-banner anchors. That is the first job of phase 1.

## Phase 1: code map (in progress)

[config/code_map.txt](config/code_map.txt) assigns every ARM9 address range to a library and an action:

| Action | Functions | Bytes | What |
|---|---|---|---|
| **port** | 6,193 | 973 KB | Firebrand engine + game: `main()`, main `0x020435CC..0x020B33BC`, ov002 (custom data UI), ov003 (race), ov004 (front end) |
| replace | 1,601 | 223 KB | NitroSDK 4.2 + NitroSystem → PSP shim; MSL → newlib/libstdc++; zlib 1.2.2 + unzip 0.15 and Lua 5.0.2 → original sources |
| drop | 2,882 | 312 KB | network stack (CPS/SSL/WiFi/DWC), ov000 WFC utility, ov001/ov006 GameSpy, ov005 Download Play |

Findings:
- Layout of main: BIOS stubs → crt0 → `main()` → network libs → MSL → zlib/unzip → NitroSDK/NitroSystem (ARM) → Lua (Thumb) → SDK CARD/BACKUP → game code → static constructors.
- **The Lua scripts are pure data** (182 chunks, no function calls). The engine reads globals such as `carParams`, `Track`, `PieceData`, `tracks`, `credits` and `rewardShop` through about 60 call sites in total (`lua_rawgeti`, `lua_gettable`, `lua_next`, `luaL_ref`...). The Lua parser is not even linked, since the game only loads bytecode. Decompiled sources go to `assets/lua_src/` (git-ignored).
- **Lua 5.0.2: 305 of 320 functions named**, plus Firebrand's fixed-point math library (`fxmath_*`). Consistency check: 799 matched call edges agree with the source call graph, 6 do not (self-recursion and one Firebrand-modified function). 21 of the 22 Lua functions the game calls are named.
- CodeWarrior detail used for the matching: within each source file, **functions are emitted in reverse definition order**.
- **NitroSDK / NitroSystem: 735 of 938 library functions named**, plus 110 data symbols. Method: `ntrtwl/NitroSDK` (4.2.30001; the game has 4.2.30009) and `ntrtwl/NitroSystem` (071126) are built with the original CodeWarrior compiler (`mid-kid/metroskrew` 0.1.3, `-wrap:sdk 2.0/sp2p2`, same setup as the pret decomps; used locally for analysis only). Then 684 functions match **byte for byte** with relocated words masked, and 178 more names come from the matched functions' relocations. A source-only attempt (call graph + register sets, no compiler) found 26.
  - **Game → SDK boundary: the game calls 319 SDK/ITCM functions, 252 of them named (79%)**, which covers 895 of 1,173 call sites. This is the surface the PSP shim must implement. The main families are SND 38, OS 36, NNS (G2D/Fnd/Gfd) 21, GX 19, MTX 12, FS 11, TP 10 and FX 10. WM 37 and VIB 5 belong to local wireless and the rumble pak, so they get stubbed.
  - **NitroSystem G3D is not used** (3 helper matches only) and neither is the NNS sound player. Firebrand has its own 3D engine: it parses BMD0 itself and drives the GPU through G3 commands. It is part of the code to port, and its G3 output will map onto `sceGu`.
  - The most-called unnamed functions are Firebrand math routines in ITCM (`0x01FFA6EC` copies a 4×3 matrix, `0x01FFA708` loads the 4×3 identity, `0x01FFAAC0` normalizes a vector with the hardware divider/sqrt).
- Game subsystem map (heuristic, from SDK-call families + strings, smoothed by address): `python -P tools/analysis/game_modules.py` → `build/analysis/game_modules.txt`. For example, ov003 holds car audio (`config/caraudio`), cameras (`config/cameras`), car loading (`Cars/%s/%d/lod%d`, wheels), particles, track triggers (`ChallengeFinishLine`, `CarCamTrigger`), HUD, ghosts and hot seat.

Analysis pipeline (after `tools/ghidra/analyze.ps1`):

```powershell
powershell -ExecutionPolicy Bypass -File tools\ghidra\export.ps1
```

```bash
python -P tools/analysis/classify.py arm9_main
```

```bash
python -P tools/analysis/code_map.py
```

```bash
bash tools/analysis/lua_names.sh
```

```bash
bash tools/analysis/sdk_names.sh
```

| Script | Role |
|---|---|
| `tools/ghidra/ExportAnalysis.java` | per-function JSON Lines: callees, strings, I/O registers, function pointers |
| `tools/analysis/romimage.py` | address-space view of `extract/` + dsd symbols |
| `tools/analysis/find_reg_tables.py` | `{name, function}` tables (luaL_reg) |
| `tools/analysis/classify.py`, `window.py` | evidence tags and address-window listings used to place library boundaries |
| `tools/analysis/code_map.py` | applies `config/code_map.txt`, per-library totals |
| `tools/analysis/align_lib.py`, `match_lib.py` | names a library from its source: anchors + order alignment + call-graph propagation |
| `tools/analysis/sig_match.py` | names library functions by masked byte signatures from ELF objects, plus their relocation targets |
| `tools/analysis/sdk_names.sh` | builds NitroSDK + NitroSystem with metroskrew, then runs `sig_match.py` and `apply_names.py` |
| `tools/analysis/game_modules.py` | heuristic subsystem map of the ported game code |
| `tools/analysis/apply_names.py` | writes names into dsd `symbols.txt` (placeholders only) |

After renaming, rerun `analyze.ps1` so Ghidra picks up the names.

## Phase 2: asset pipeline

See [docs/ASSETS.md](docs/ASSETS.md). In short:
- Only DS-hardware data is converted: **3D models → TMPM** (PSP-native vertices and swizzled GU textures), sound (SSEQ player needed) and the intro movie. Collision `.h`, ghosts, strings, Lua bytecode and 2D files are read unchanged by the ported code.
- The BMD0 reader (`tools/assets/nitro3d.py`) parses all 2,230 files. It matches apicula on geometry (25/25 models) and textures (36/36).
- **First PSP rendering:** `psp/viewer` draws a converted car with sceGu in PPSSPP, and checks itself by writing a screenshot (`tools/ppsspp_shot.ps1`).
- Ghidra import fix: Thumb `BL` used as a long jump inside a function is now a branch (82 cases), so functions are no longer split. Use `tools/ghidra/decompile.ps1` for per-function C output.

## PSP build environment

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc "psp-gcc --version"
```

## Phase 3: host port (in progress)

See [docs/PORT.md](docs/PORT.md) for the architecture, the build, the function tracking and the findings.
In short:
- C++ port built with CMake: `src/game` (decompiled code, each function tagged `@port <module>:<addr> <status>`),
  `src/shim` (NitroSDK API on stdio now, SDL2/OpenGL next, PSP in phase 4), `src/lib` (Lua 5.0.2 with integer
  numbers, unzip API on zlib).
- **Race-mode inventory** (`tools/analysis/callgraph.py race`, from dsd relocations including vtables): 4,200 game
  functions, 816 SDK, 314 Lua, 161 MSL, 48 zlib/unzip, 230 network to stub. Static reachability keeps almost all of
  main + ov003, so a dynamic trace is the next step to rank them.
- **First slice runs on the host:** the VFS (`vfs.zip` + the `vfs.bin` directory tree), Firebrand's Lua layer and the
  ov003 car parameter loader. All 4,258 VFS files read back identical, and the 4 cars' 320 values match the unluac
  decompilation.
- **Second slice: track loading.** The Coast piece catalogue, the race scenes, piece placement and the `.h` collision
  meshes (format now confirmed from the code). All 26 Coast tracks match an independent reading of the scripts and
  meshes, including a geometric check of every placed piece.
- **Third slice: car physics.** Collision queries (16,722 probes over the 26 Coast tracks match an independent
  ray/triangle test), engine and gearbox, and the whole car step of ov003 (wheel and body probes, suspension,
  grip, drive, integration, surfaces). The Coast car dropped on the start platform settles, accelerates through
  the gears, brakes and reverses (`tm_physics --drop` + `tools/host/check_drop.py`).
- **Host race window** (`tm_race`, SDL2 + OpenGL): the Coast car drives on a Coast track with the game's own models
  and textures, the terrain, and the race rules: countdown, checkpoints, laps, finish and respawn
  (`tm_trackdump --triggers` + `check_triggers.py`, `tm_physics --progress` + `check_progress.py`), seen from the
  game's chase camera (`config/cameras`, `check_cameras.py` + `check_camera.py`), with a host HUD stand-in: **a Coast race is playable on the host**
  (the game's own HUD, lighting, sky and menus are still to port).
  ![tm_race on coa/easy/0](docs/host_race.png)

```bash
cmake -S . -B build/host -G Ninja
```

```bash
cmake --build build/host
```

```bash
build/host/tm_cfgdump --root extract/files --carparams > build/host/carparams.txt
```

```bash
python -P tools/host/check_carparams.py build/host/carparams.txt
```

```bash
build/host/tm_trackdump --root extract/files --track easy/0 --pieces --collision --lighting > build/host/track.txt
```

```bash
python -P tools/host/check_track.py build/host/track.txt --track easy/0
```

```bash
python -P tools/host/run_checks.py
```

```bash
python -P tools/analysis/port_status.py --race-only
```

## Phase 4: PSP back-ends (done: the race runs in PPSSPP)

- **The game libraries build for the PSP** (`bash tools/psp_build.sh`: psp-cmake + pspdev in WSL, 0 warnings) with
  the shim's PSP back-ends (`src/shim/psp`: sceIo file system, OS services).
- **Bit-exact on the PSP:** the check suite (`tm_checksuite` on the host, `tm_checks` EBOOT on the PSP) runs the
  114 host checks' producers in PPSSPP; the 2.1 MB of dumps are identical byte for byte to the host's and the host
  checkers find 0 errors in them (`tools/psp/run_checks_psp.ps1`).
- **The Coast race runs on the PSP** (`build/psp/tm_race/EBOOT.PBP`, sceGu 480 x 272, sceCtrl: X accelerate,
  square brake, d-pad/analog steer, triangle checkpoint, start pause menu, select camera). In PPSSPP, after 150 frames
  the car and camera are identical to the host's (`tools/psp/race_shot.ps1`).
  ![tm_race on the PSP (PPSSPP), coa/easy/0](docs/psp_race.png)

```bash
bash tools/psp_build.sh
```

```bash
powershell -ExecutionPolicy Bypass -File tools/psp/run_checks_psp.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File tools/psp/race_shot.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File tools/psp/package.ps1
```

The last one fills `build/psp/package/PSP/GAME/TM2PSP` (EBOOT + the game files from your ROM): copy its `PSP`
folder to the root of a memory stick (custom firmware needed). Real-hardware speed is not verified yet.

## Phase 5: polish on the real PSP and fidelity (in progress)

- **Track menu** in front of the race (both drivers, `src/app/race_app.*`): the 26 Coast tracks with their best
  time, pause and result menus (continue, restart, next track, track menu), best times in `save.txt` next to the
  EBOOT. Track reloads are leak-free (`tools/psp/reload_test.ps1`: 32 loads in PPSSPP, constant heap).
- **Performance safety for the real PSP:** pieces outside the view frustum are skipped (pixel-identical images,
  `tools/host/check_culling.py`), textures are paletted (T4/T8, lossless) and live in VRAM, vertices are 16-bit
  (exact DS positions), meshes are converted at load; an empty `perf.txt` next to the EBOOT logs frame times to
  `frametimes.txt`. Console speed still has to be measured on the PSP-3000 (R overlay or the log).
- **Fidelity:** the DS lighting found in the code (lights, colour sets from `config/tracks`, material rules,
  shininess table) in common code for both back-ends, the fog, the sky dome of the track's time of day, and the car
  wheels from the game's wheel models and matrices; the game's HUD as sprites (countdown, speed, laps, split times
  against the record, clock), the car's brake and reverse lights, the arrow signs' scrolling texture animation.
  The HUD's backing panels and the car's shadow volume (stencil) too. Still to do: the HUD's text messages, the
  animated sea backdrop and the sound.

```bash
powershell -ExecutionPolicy Bypass -File tools/psp/reload_test.ps1
```

```bash
python -P tools/host/check_culling.py
```
