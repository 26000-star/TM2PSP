/* NitroSDK FS on the PSP, on top of sceIo. The ROM file system root is a directory on the memory stick (or UMD,
 * host0: under PSPLink), set by the program with FSh_SetRoot, normally "<EBOOT dir>/files" as an absolute path.
 * Same path rules as the host back-end: relative to the root, '\' and '/' both separators; FAT is case-insensitive.
 * An FSFile struct copy is an independent cursor (the game's unzip relies on this), so the handle is shared and
 * every read seeks first unless the OS position already matches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspiofilemgr.h>

#include "nitro.h"

typedef struct PspFile {
    SceUID fd;
    u32 ospos; /* position of the OS file pointer, to skip redundant seeks */
} PspFile;

static char g_root[512] = "ms0:/PSP/GAME/TM2PSP/files";
static char g_cwd[256] = "";
static int g_open; /* files open now (FSh_OpenFiles) */

void FSh_SetRoot(const char *dir)
{
    snprintf(g_root, sizeof g_root, "%s", dir);
}

int FSh_OpenFiles(void)
{
    return g_open;
}

void FS_Init(u32 default_dma_no)
{
    (void)default_dma_no;
}

static void psp_path(char *out, size_t n, const char *path)
{
    char rel[512];
    size_t i, o = 0;
    while (*path == '/' || *path == '\\')
        path++;
    if (g_cwd[0] && !strchr(path, ':'))
        o = (size_t)snprintf(rel, sizeof rel, "%s/", g_cwd);
    for (i = 0; path[i] && o + 1 < sizeof rel; i++)
        rel[o++] = path[i] == '\\' ? '/' : path[i];
    rel[o] = 0;
    snprintf(out, n, "%s/%s", g_root, rel);
}

BOOL FS_ChangeDir(const char *path)
{
    /* Archive mounts ("HRZ:/") are not backed by directories; keep the root. */
    if (strchr(path, ':'))
        return TRUE;
    snprintf(g_cwd, sizeof g_cwd, "%s", path);
    return TRUE;
}

void FS_InitFile(FSFile *file)
{
    memset(file, 0, sizeof *file);
}

BOOL FS_OpenFile(FSFile *file, const char *path)
{
    char pp[1024];
    SceUID fd;
    SceOff size;
    PspFile *f;
    psp_path(pp, sizeof pp, path);
    fd = sceIoOpen(pp, PSP_O_RDONLY, 0);
    if (fd < 0)
        return FALSE;
    size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    f = (PspFile *)malloc(sizeof *f);
    f->fd = fd;
    f->ospos = 0;
    file->handle = f;
    g_open++;
    file->top = 0;
    file->bottom = (u32)size;
    file->pos = 0;
    return TRUE;
}

BOOL FS_CloseFile(FSFile *file)
{
    PspFile *f = (PspFile *)file->handle;
    if (f) {
        sceIoClose(f->fd);
        free(f);
        g_open--;
    }
    file->handle = NULL;
    return TRUE;
}

s32 FS_ReadFile(FSFile *file, void *dst, s32 len)
{
    PspFile *f = (PspFile *)file->handle;
    int got;
    if (!f || len < 0)
        return -1;
    if ((u32)len > file->bottom - file->pos)
        len = (s32)(file->bottom - file->pos);
    if (len == 0)
        return 0;
    if (f->ospos != file->pos) {
        if (sceIoLseek(f->fd, file->pos, PSP_SEEK_SET) < 0)
            return -1;
        f->ospos = file->pos;
    }
    got = sceIoRead(f->fd, dst, (SceSize)len);
    if (got < 0)
        return -1;
    f->ospos += (u32)got;
    file->pos += (u32)got;
    return got;
}

BOOL FS_SeekFile(FSFile *file, s32 offset, FSSeekFileMode origin)
{
    s64 base = origin == FS_SEEK_SET ? file->top : origin == FS_SEEK_CUR ? file->pos : file->bottom;
    s64 p = base + offset;
    if (p < file->top)
        p = file->top;
    if (p > file->bottom)
        p = file->bottom;
    file->pos = (u32)p;
    return TRUE;
}
