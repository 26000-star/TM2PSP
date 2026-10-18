# TM2PSP — feasibility & toolchain study

Target: `TrackMania Turbo (Europe) (En,Fr,De,Es,It).nds` (64 MB, game code `BT2P`, build `TM_01_DS_2009_05_22_16_12`, Firebrand Games / Nadeo).
Goal: a native PSP build (EBOOT.PBP homebrew, optionally ISO), testable on PPSSPP and real hardware.

## 1. Verdict

**Possible, but only as a source port, not a conversion.** No tool can turn a DS ROM into a PSP program automatically.
The two CPUs (ARM9/ARM7 vs MIPS Allegrex) and especially the hardware (DS 2D/3D engines, two screens, touch, ARM7 sound) are completely different.
Emulating the DS on the PSP is not an option either: the PSP is far too slow for DS 3D emulation.

The realistic path is the one used by the PC/console ports of SM64, Zelda, etc.:

1. Decompile the game code (ARM9 + overlays) into C/C++ that compiles. It does **not** need to be byte-matching.
2. Throw away the Nintendo layers (NitroSDK, NitroSystem, ARM7, WiFi/DWC) and replace them with a thin "Nitro-on-PSP" layer: 3D → `sceGu`, 2D → textured sprites, files → `vfs.zip` on the Memory Stick, buttons/touch → PSP pad + cursor, sound → an SSEQ player or pre-converted audio.
3. Build it with the PSP SDK, then package it as EBOOT.PBP.

It is a big project: months of work, not days. But this ROM is a *favourable* case (see §2).

## 2. What is in the ROM (observed)

| Part | Size | Notes |
|---|---|---|
| ARM9 main binary | 830 KB @ `0x02000000`, entry `0x02000800` | **Not compressed** (strings readable). ~1.1k ARM + ~2.6k Thumb prologues → several thousand functions |
| ARM9 overlays 0–7 | 933 KB total | ov0/1/2 share `0x020D6B00`, ov3/4 share `0x020F68E0`, ov5/6/7 share `0x02139280` (mode-specific code that swaps in and out) |
| ov1, ov6 | — | Nintendo WFC / GameSpy online code (dead service) → **drop** |
| ARM7 binary | 160 KB | Nintendo SDK sound/WiFi driver → **drop**, not ported |
| `vfs.zip` | 26.7 MB, 4258 entries | All game data, standard ZIP (deflate). Engine reads it with *unzip 0.15* (Gilles Vollant), which is easy to replace with zlib/minizip |
| `audio/*.sdat` | 2.1 MB | NitroSDK sound archives (SSEQ/SBNK/SWAR) |
| `movies/intro.mob` | 815 KB | Mobiclip video (`MODS`) |
| `download*.srl`, `dwc/utility.bin` | 3.8 MB | DS Download Play child ROMs + WFC utility → **drop** |

Contents of `vfs.zip`:

| Type | Count | Format | Plan |
|---|---|---|---|
| `.n` | 2160 | **BMD0** = NitroSystem NSBMD 3D models (cars, track pieces, sky) | Parse at runtime on PSP, or convert offline |
| `.h` | 964 | Custom Firebrand track-piece data (collision/geometry, `0x010E` header) | Reverse with the code, write a parser |
| `.na` | 70 | BTA0 = NSBTA texture animations | Same as `.n` |
| `.NCGR/.NCLR/.NCER/.NANR/.nsc` | ~530 | Nitro 2D tiles, palettes, cells, animations, screens | Convert offline to PSP textures |
| `.luac` / `.dcsc` | 27 / 155 | **Lua 5.0.2 bytecode**, 32-bit little-endian, 4-byte *integer* `lua_Number` | **Run as-is**: embed Lua 5.0.2 built with `int` numbers. PSP is also 32-bit little-endian, so the bytecode loads unchanged |
| `Ghosts/*.bin` | 313 | Medal ghost replays | Keep; format comes out of the RE |
| `strings/*.str`, `tokens.dat` | 38 | Length-prefixed localized strings (5 languages) | Keep |

Libraries linked into ARM9: Lua 5.0.2, unzip 0.15, DWC 3.1 (WFC), NitroWiFi 2.1, Ubiquitous SSL/CPS, NitroSDK backup.
→ Lua and unzip exist as public source, so we recompile them instead of decompiling them.
→ All SDK and WiFi code can be identified by signatures and skipped.

**Why this is a favourable case:** the code is not compressed, the data is in a plain ZIP, the gameplay config and level scripts are Lua (data-driven), and the network code is cleanly isolated in overlays we drop.
The work left is the Firebrand engine and gameplay code (physics, cameras, track builder, UI/menus, the editor) plus the rendering and sound back-ends.

## 3. Hardware gap, DS → PSP

| | DS | PSP | Impact |
|---|---|---|---|
| CPU | ARM946E-S 67 MHz (+ARM7 33 MHz) | MIPS Allegrex 333 MHz + FPU + VFPU | Plenty of headroom. DS fixed-point math ports as-is |
| RAM | 4 MB | 32 MB (PSP-1000) / 64 MB (2000+) | Fine. Overlay swapping becomes unnecessary: link everything |
| 3D | ~2048 polys/frame, fixed-function, 256×192 | GE, far stronger, 480×272 | Re-implement the G3 command stream / NSBMD rendering on `sceGu` |
| Screens | 2× 256×192, bottom one is touch | 1× 480×272, no touch | Main design choice: race view full screen + HUD overlay; menus/editor use a cursor driven by the analog stick |
| Sound | ARM7 SSEQ sequencer | `sceAudio` PCM channels | Port an SSEQ player, or pre-render the music to streams |

The **track editor** is built around the touchscreen, so it is the hardest part to adapt. A sensible plan is to ship the racing modes first and the editor later.

## 4. Toolchain

### 4.1 ROM extraction & code analysis

| Tool | Version | Role | Host |
|---|---|---|---|
| **ds-decomp (`dsd`)** — AetiasHax | v0.12.1 | `rom extract`, auto-analysis (`init`), symbol/relocation configs, `delink` into relocatable **ARM ELF** objects, `dis`, signatures (`sig`), `rom build` | Windows exe |
| **Ghidra** | 12.1.4 (REA wants 12.1.x) | Main decompiler (ARM/Thumb → C pseudocode) | Win, needs JDK 21 ✅ |
| **NTRGhidra** — pedro-javierf | v1.5.1, built for **Ghidra 12.0.4** | Loads `.nds` directly, overlay switching | Rebuild against 12.1.4, or import `dsd` ELFs instead |
| **REA** (this MCP) | 6.5.0 | Evidence ledger, archive/asset inventory, bookmarks/annotations | ⚠ see below |
| objdiff | v3.8.2 | Function diffing. Only needed for byte-matching, which we skip. Optional | Win |
| Python 3.11 | ✅ installed | Custom parsers (`.h`, ghosts, strings, SDAT glue) | — |

⚠ **REA limitation (observed in `binary_session`):** on Windows, REA's Ghidra provider only accepts x86/x86-64 PE files and raw 6502 images.
**It cannot open ARM DS code from this Windows host.** Options:
- (a) Run REA + Ghidra inside the existing WSL Ubuntu (Linux provider: ARM support still to be tested),
- (b) Use Ghidra directly for the ARM code and keep REA for assets and evidence tracking,
- (c) Use IDA, if you have a licence (REA has an IDA provider).

My recommendation: (b) now, try (a) as a quick test.

### 4.2 DS-side runtime reference (to compare behaviour)

| Tool | Version | Role |
|---|---|---|
| **melonDS** | 1.1 | Accurate emulator with a GDB stub → breakpoints and RAM watch while we RE physics and formats |
| no$gba (debug build) | latest | Very good DS debugger (I/O registers, VRAM/3D viewers) |
| DeSmuME | 0.9.13 | 3D/VRAM viewers, Lua scripting for logging |

### 4.3 Asset tools

| Asset | Tool |
|---|---|
| NSBMD/NSBTA (`.n`, `.na`) | **apicula** (Windows zip) → glTF/DAE + PNG textures, for inspection and offline conversion. Its Rust source is the reference for a runtime loader |
| NCGR/NCLR/NCER/NANR/NSCR | NitroPaint or Tinke (view); a Python converter to PSP textures (ours) |
| Lua 5.0 bytecode | **unluac** (Java; supports 5.0). Test it on the int-number build. Fallback: luadec 5.0 rebuilt with `int` numbers. Decompiling is only needed to *understand* the scripts; the bytecode runs as-is |
| SDAT | VGMTrans / SDATtool (extract, MIDI+SF2), or an SSEQ player library ported to PSP |
| Mobiclip `.mob` | **FFmpeg** (it has a Mobiclip decoder and a MODS demuxer) → re-encode for PSP playback, or show frames from our own code |
| ZIP | Python `zipfile` / 7-Zip ✅ (7-Zip installed) |

### 4.4 PSP development

| Tool | Version | Role | Host |
|---|---|---|---|
| **pspdev** (psptoolchain + pspsdk + psp-packages) | v20261001, `pspdev-ubuntu-latest-x86_64.tar.gz` 173 MB | `psp-gcc`/`psp-g++`, newlib, `sceGu`/`sceGum`, `psp-cmake`, `mksfo`, `pack-pbp`, `psp-prxgen`, `psp-pacman` (zlib, libpng, SDL2…) | **WSL Ubuntu** (already present) |
| psplinkusb | in pspdev | USB shell/debug/`usbhostfs` on a real PSP (Windows needs a libusb driver) | — |
| ebootsigner | in pspdev | Signs the EBOOT so it also runs from ISO / OFW-style launchers | — |
| CMake 3.29 / Ninja / Make | ✅ installed | Build system (also a host-side Windows/SDL build for fast debugging) | — |

### 4.5 Testing

| Target | Tool |
|---|---|
| Emulator | **PPSSPP** 1.20.4 (Windows x64, 20 MB): CPU debugger, GE debugger (frame dump), memory view |
| Real hardware | PSP on CFW (ARK-4, or PRO-C/LME on 6.61). Copy `PSP/GAME/TM2PSP/EBOOT.PBP` to the Memory Stick, debug over PSPLink |
| ISO (optional) | Lay out a UMD tree (`PSP_GAME/SYSDIR/EBOOT.BIN`, `PARAM.SFO`, `USRDIR/`), sign it, build with `mkisofs`/UMDGen. CFW runs it. EBOOT.PBP comes first |

## 5. Proposed phases (not started)

0. **Setup**: install the tools (§6), `dsd rom extract` + `init`, and a git repo for the project.
1. **Map the code**: identify SDK/NitroSystem/Lua/unzip/DWC functions by signatures, then label the game's subsystems (FS, renderer, physics, Lua bindings, UI).
2. **Asset pipeline**: converters for 2D graphics, a model viewer/loader (apicula as reference), and the `.h`, ghost and string formats.
3. **Host-side port first** (Windows + SDL2 + OpenGL): decompiled code + Nitro shim. This is much faster to debug than the PSP.
4. **PSP back-end**: swap the shim implementation (`sceGu`, `sceCtrl`, `sceAudio`, `sceIo`), plus memory and perf tuning.
5. **Packaging & hardware tests**: EBOOT.PBP → PPSSPP → real PSP → optional ISO.

Order of value: race mode (one environment) → all 4 environments + menus → platform/puzzle modes → sound/music → track editor.

## 6. What I suggest installing (awaiting your OK)

| # | What | Source | Size | Where |
|---|---|---|---|---|
| 1 | Ghidra 12.1.4 | github.com/NationalSecurityAgency/ghidra releases | 570 MB | `C:\Tools\ghidra_12.1.4` + set `GHIDRA_INSTALL_DIR` (also enables REA's Ghidra provider) |
| 2 | NTRGhidra v1.5.1 | github.com/pedro-javierf/NTRGhidra | 0.1 MB | Try it against 12.1.4; rebuild if the version check refuses |
| 3 | dsd v0.12.1 | github.com/AetiasHax/ds-decomp | 11.5 MB | `C:\Tools\bin` |
| 4 | apicula | github.com/scurest/apicula | 0.7 MB | `C:\Tools\bin` |
| 5 | PPSSPP 1.20.4 Win x64 | github.com/hrydgard/ppsspp | 20 MB | `C:\Tools\PPSSPP` |
| 6 | melonDS 1.1 Win x64 | github.com/melonDS-emu/melonDS | 19.5 MB | `C:\Tools\melonDS` |
| 7 | pspdev v20261001 (ubuntu x86_64) | github.com/pspdev/pspdev | 173 MB | WSL `~/pspdev` + PATH |
| 8 | unluac | sourceforge.net/projects/unluac | <1 MB | `C:\Tools\bin` (runs on your JDK 21) |
| 9 | FFmpeg (gyan.dev essentials build) | gyan.dev | ~90 MB | `C:\Tools\ffmpeg` |

### 6.1 Install status (2026-10-18)

| Tool | Status | Location / check |
|---|---|---|
| Ghidra 12.1.4 | ✅ installed, sha256 verified | `C:\Tools\ghidra_12.1.4_PUBLIC`; user env `GHIDRA_INSTALL_DIR` set |
| NTRGhidra | ✅ **rebuilt from v1.5.1 source for 12.1.4** (the release zip only targets 12.0.4) and installed in `Ghidra\Extensions\NTRGhidra` | Headless import of the ROM works: loader "Nintendo DS (NTR) and DSi (TWL)", `ARM:LE:32:v5t`. Ships `fidb/nitrosdk3.0.fidb` (NitroSDK function IDs) |
| dsd 0.12.1 | ✅ sha256 verified | `C:\Tools\bin\dsd.exe` |
| apicula 0.1.1-dev (3d4e91e) | ✅ no checksum published | `C:\Tools\bin\apicula.exe` |
| unluac 2025-12-23 | ✅ no checksum published | `C:\Tools\bin\unluac.jar` + `unluac.cmd`. **Tested: decompiles the game's int-number Lua 5.0 bytecode** (`vehicles.luac`, `Scenes/coa/bonus/0.dcsc`). Numbers are DS fx32 (4096 = 1.0) |
| FFmpeg 9.0.2 essentials | ✅ sha256 verified | `C:\Tools\ffmpeg\bin`. Has the `mobiclip` decoder and the `mods` demuxer |
| PPSSPP 1.20.4 | ✅ sha256 verified | `C:\Tools\PPSSPP\PPSSPPWindows64.exe` |
| melonDS 1.1 | ✅ sha256 verified | `C:\Tools\melonDS\melonDS.exe` |
| pspdev v20261001 | ✅ sha256 verified, in a **dedicated WSL distro `Ubuntu-24.04`** (root, `~/pspdev`, PATH set in `.bashrc` and `.profile`) plus cmake 3.28 and make. The release needs glibc ≥ 2.38, so it cannot run in the old `Ubuntu` 22.04 distro, which was left untouched | psp-gcc 15.2.0. A hello-world `psp-cmake` project builds EBOOT.PBP, and PPSSPP boots it and exits normally |

Build command, from Windows: `wsl -d Ubuntu-24.04 -u root -- bash -lc "cd /mnt/c/... && psp-cmake -B build && cmake --build build"`.

User PATH now includes `C:\Tools\bin`, `C:\Tools\ffmpeg\bin`, `C:\Tools\PPSSPP` and `C:\Tools\melonDS`. Open a new terminal (and restart Claude/REA) to pick them up.
REA will only see `GHIDRA_INSTALL_DIR` after a restart, and on Windows it still refuses ARM targets. Use Ghidra directly for the DS code.
Tip: `analyzeHeadless.bat` breaks on paths containing `(`, so work on a copy of the ROM with a plain name.

Legal note: keep the ROM and the extracted assets out of any public repo. A distributable port would ship code plus an extractor that reads the user's own ROM.
