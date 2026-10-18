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

## PSP build environment

```bash
wsl -d Ubuntu-24.04 -u root -- bash -lc "psp-gcc --version"
```
