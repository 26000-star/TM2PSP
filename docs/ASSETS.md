# Asset pipeline (phase 2)

All game data lives in `vfs.zip` (4,258 files), extracted to `assets/vfs/` (git-ignored).

**Principle:** only data tied to DS hardware gets converted. Everything else is read unchanged by the ported code, since the PSP is also 32-bit little-endian with natural alignment.

| Data | Files | DS-specific? | Plan |
|---|---|---|---|
| 3D models `.n` (BMD0) | 2,160 | yes: GPU display lists, DS texture formats | **convert offline to TMPM** (`tools/assets/psp_model.py`) |
| Material animations `.na` (BTA0/SRT0) | 70 | no (keyframes) | read as is; drives `sceGuTexOffset`/`TexScale` |
| Track piece collision `.h` | 964 | no | **read as is** by the ported collision code (format below) |
| 2D screens/sprites (NCGR/NCLR/NCER/NANR, `.nsc` NCSC) | ~530 | yes: tiles, OAM | read as is by the PSP "DS 2D engine" shim (BG tilemaps + OBJ as GE quads) |
| Lua bytecode `.luac`/`.dcsc` | 182 | no | read as is (embedded Lua 5.0.2 with `int` numbers) |
| Strings `.str` + `tokens.dat` | 38 | no | read as is |
| Ghosts `Ghosts/**.bin`, `material.bin` | 313+ | no | read as is |
| Sound `audio/*.sdat` | 2 | yes: ARM7 sequencer | needs an SSEQ/SBNK/SWAR player on PSP (sound phase) |
| Intro `movies/intro.mob` (Mobiclip) | 1 | yes | FFmpeg decode → JPEG sequence for `sceJpeg` (or skip) |

## 3D models: findings

- **All 2,230 Nitro 3D files parse** (`python -P tools/assets/bmd_stats.py`): 2,160 models, 815k vertices in total, at most 7,592 per model (car LOD0).
- **Verified against apicula:** triangle counts and position bounds match on 25/25 models (`validate_apicula.py`), and decoded textures match pixel for pixel on 36/36 (`validate_textures.py`, covering tex4x4, pal16, pal4 and a3i5).
- No skinning and no matrix-restore commands: rigid meshes. Display lists use only vertex/normal/texcoord/colour/begin/end commands.
- 97% of vertices carry normals (DS hardware lighting is used). Vertex colours are rare (8%).
- Positions all fit in ±8.0, i.e. the DS 1.3.12 int16 range, so they map directly onto `GU_VERTEX_16BIT`.
- Textures go up to 512×512. Formats: tex4x4 344 (9.5 MB of 13 MB), pal16 224, pal256 105, a3i5 94, pal4 67, a5i3 38.
- **Firebrand extension:** track piece models have the materials offset with bit 31 set. In that case the block is a count byte followed by indices into the environment's shared `Graphics/Tracks/<env>/texturebatch.n` (84 textures for Coast). All pieces of an environment share one texture set, which suits the PSP VRAM budget well.

## TMPM (PSP model) format

Written by `tools/assets/psp_model.py`, drawn by `psp/viewer/main.c`. Little endian, 16-byte aligned sections.

| Record | Layout |
|---|---|
| Header | `"TMPM"` u16 version u16 flags · u16 n_tex n_mat n_mesh n_draw · f32 up_scale · f32 node[12] (3×4) · u32 off_tex off_mat off_mesh off_draw |
| Texture (24 B) | u16 w h · u8 psm swizzled clut_psm pad · u16 clut_count pad · u32 data_off data_size clut_off |
| Material (28 B) | s16 tex · u8 alpha(0–31) flags · u32 diffuse ambient specular emission (8888) · f32 su sv |
| Mesh (24 B) | u32 vtype · u16 vsize pad · u32 n_verts n_idx vtx_off idx_off |
| Draw (4 B) | u16 material mesh |

- Geometry: one indexed `GU_TRIANGLES` mesh per DS piece. Quads are split, strips unrolled (a DS quad strip has triangle-strip winding) and vertices deduplicated.
- Attributes: UV int16 = DS texel×16, with `sceGuTexScale(2048/w, 2048/h)`. Colour is 5650 and normals 8-bit, each only when present. Positions are the DS int16 values.
- **GU normalizes 16-bit positions and UVs by 1/32768**, so the model matrix scale is `up_scale × 8` (32768 / 4096).
- Textures: pal4/pal16 → T4, pal256 → T8, a3i5/a5i3 → T8 with their alpha folded into an 8888 CLUT, direct/tex4x4 → 5551. DS colours are 5-bit, so this is near lossless. All textures are swizzled.
- TODO: transcode tex4x4 to DXT1 (the GE decodes it natively, at half the size of 5551). Track pieces with batch materials need the environment's texturebatch.

Viewer check: `bash tools/psp_build.sh psp/viewer`, then `powershell -File tools/ppsspp_shot.ps1 build/psp/viewer/EBOOT.PBP TM2PSPVIEW build/preview/x.png "<model.tmpm>=model.tmpm"`. The viewer writes `shot.bmp` after 45 frames. **Car `Cars/Sta/0/lod0.n` renders textured and lit in PPSSPP.**

## Track piece collision `.h`

Confirmed from the loader (main 0x02065c04, `dsCollisionMesh::Load`) and checked on all 964 files (every byte accounted for):

```
u16 version = 0x010E
u16 n_verts, n_tris
s16 has_materials                    // 0 in 89 files, 1..6 elsewhere: only "non-zero" matters
n_verts × { s32 x, y, z }            // 20.12 fixed; pieces span ±8.0 per cell around the origin cell
n_tris  × { u16 a, b, c }
u32 n_cells                          // 0 = no grid (then only the materials follow)
n_cells × u16 head                   // first node of the cell's list, 0xFFFF = empty; cell = x + gx*y + gx*gy*z
u32 gx, gy, gz                       // grid dimensions (e.g. 6, 4, 6)
s32 min[3]; s32 size[3]              // grid origin and extent (20.12) = the mesh bounds
u32 n_nodes; n_nodes × { u16 tri, u16 next }   // singly linked triangle lists, next 0xFFFF ends
if has_materials: n_tris × u8        // surface per triangle: 0, 1, 3, 5..11, 255
```

- The per-cell lists are **linked lists**, not `{count, first}` ranges: a triangle appears in every cell it touches.
- Face normals are not stored: the loader computes `(b-a)×(c-a)` (`VEC_CrossProduct`, rounded), scales it by 2^-7 when a component exceeds 100.0, and normalises it to fx16 with the ITCM hardware-divider routine (0x01ffaac0).
- The grid (0x02073500) also keeps `cellSize = size / dim` (fx32) and `dim / size` as a 32.32 value. Segment queries (0x0206614c) map points into grid space with it and walk the cells with a 3D DDA (0x01ffe3ec), gathering each cell's triangles once.
- Paths: `graphics/` + `Tracks/<env>/<model>` + `.h`, where the model is the piece's `<name>ground` or `<name>air` (lower case).
- `_dup.h` is only used by the wireless (download play) path, which renames shared assets.

## Other formats

- `.str`: a sequence of `{u16 length, char[length]}`. There are 904 strings per language in `strings/main/frontend` (gb, us, fr, de, es, it).
- `.nsc` screens are **NCSC** (NITRO-CHARACTER editor format, 7 blocks, tags not reversed). `SCRN` holds u32 width and height in tiles, two u32 settings, then u16 map entries. `tools/assets/nitro2d.py` renders screens for inspection. Known issue: some 8bpp backgrounds still come out scrambled in the preview, so the tile layout needs another look when the 2D shim is written.
- SDAT: `sound_data.sdat` has 5 SSEQ (music), 24 SSAR (SFX, including `SEQARC_CAR_*` engine sounds), 29 SBNK and 29 SWAR (1.6 MB of samples), no STRM. The network SDAT is dropped.
- `intro.mob`: Mobiclip, 256×192, 30 fps, 26 s, no audio. FFmpeg decodes it, but the colour space is **YCgCo** (plane order Y, Cg, Co): convert with `R = Y−Cg+Co, G = Y+Cg, B = Y−Cg−Co`.
