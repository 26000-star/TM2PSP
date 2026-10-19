/* Nitro-on-host/PSP shim: the subset of the NitroSDK 4.2 API that the ported game code calls.
 * Signatures follow the SDK so decompiled code compiles unchanged; implementations live in src/shim/<platform>/.
 * Only what the ported code needs is declared here; add functions as decompiled code starts using them. */
#ifndef TM_NITRO_H
#define TM_NITRO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef int BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* ---- FX: fixed point (fx32 = 20.12, fx16 = 4.12) ---- */
typedef s32 fx32;
typedef s16 fx16;
typedef s64 fx64;
#define FX32_SHIFT 12
#define FX32_ONE 4096
#define FX32_HALF 2048
#define FX_MUL(a, b) ((fx32)(((s64)(a) * (b) + FX32_HALF) >> FX32_SHIFT))

/* 4096 (sin, cos) pairs, fx16. The SDK ships it as data; the shim generates it (FX_Init). */
extern s16 FX_SinCosTable_[4096 * 2];

typedef s64 fx64c; /* 32.32 fixed point (hardware divider results) */

void FX_Init(void);
fx32 FX_Div(fx32 numer, fx32 denom);
fx32 FX_Inv(fx32 denom);
fx64c FX_DivFx64c(fx32 numer, fx32 denom); /* (numer << 32) / denom, 64/32 divider mode */
fx64c FX_InvFx64c(fx32 denom);             /* FX_DivFx64c(FX32_ONE, denom) */
s32 FX_ModS32(s32 a, s32 b);
fx32 FX_Sqrt(fx32 x);

/* ---- VEC / MTX ---- */
typedef struct { fx32 x, y, z; } VecFx32;
typedef struct { fx16 x, y, z; } VecFx16;
/* 4x3 matrix, row vectors: rows 0..2 are the rotation, row 3 the translation (m[3] = _30 _31 _32). */
typedef struct { fx32 m[4][3]; } MtxFx43;

fx32 VEC_DotProduct(const VecFx32 *a, const VecFx32 *b);
void VEC_CrossProduct(const VecFx32 *a, const VecFx32 *b, VecFx32 *out);
void MTX_RotY43_(MtxFx43 *mtx, fx32 sinVal, fx32 cosVal);
void MTX_RotX43_(MtxFx43 *mtx, fx32 sinVal, fx32 cosVal);
int MTX_Inverse43(const MtxFx43 *src, MtxFx43 *dst); /* 0 on success, -1 when singular */
void MTX_Concat43(const MtxFx43 *a, const MtxFx43 *b, MtxFx43 *ab);
u16 FX_AcosIdx(fx32 x);

/* ---- FS: ROM file system (backed by a host directory) ---- */
typedef enum { FS_SEEK_SET = 0, FS_SEEK_CUR = 1, FS_SEEK_END = 2 } FSSeekFileMode;

typedef struct FSFile {
    void *handle; /* host file; NULL when closed */
    u32 top;      /* start offset (always 0 on host) */
    u32 bottom;   /* end offset = file size */
    u32 pos;      /* current offset; each FSFile keeps its own, so a struct copy is an independent cursor (the game's unzip relies on this) */
} FSFile;

void FS_Init(u32 default_dma_no);
void FS_InitFile(FSFile *file);
BOOL FS_OpenFile(FSFile *file, const char *path);
BOOL FS_CloseFile(FSFile *file);
s32 FS_ReadFile(FSFile *file, void *dst, s32 len);
BOOL FS_SeekFile(FSFile *file, s32 offset, FSSeekFileMode origin);
BOOL FS_ChangeDir(const char *path);
static inline u32 FS_GetLength(const FSFile *file) { return file->bottom - file->top; }
static inline u32 FS_GetPosition(const FSFile *file) { return file->pos - file->top; }

/* Host only: directory that stands for the ROM's file system root (holds vfs.zip, vfs.dat, vfs.bin). */
void FSh_SetRoot(const char *dir);

/* ---- OS ---- */
void OS_Printf(const char *fmt, ...);
void OS_Panic(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
