/* NitroSDK FS on top of stdio. The ROM file system root is a host directory (FSh_SetRoot).
 * Paths are relative to the root; '\' and '/' are both separators, as in the SDK. Host file systems used by the
 * port (Windows, PSP FAT) are case-insensitive, which matches the game's lower-cased paths against mixed-case files. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nitro.h"

static char g_root[512] = ".";
static char g_cwd[256] = "";

void FSh_SetRoot(const char *dir)
{
    snprintf(g_root, sizeof g_root, "%s", dir);
}

void FS_Init(u32 default_dma_no)
{
    (void)default_dma_no;
}

static void host_path(char *out, size_t n, const char *path)
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
    /* Archive mounts ("HRZ:/") are not backed by host directories; keep the root. */
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
    char hp[1024];
    FILE *fp;
    long size;
    host_path(hp, sizeof hp, path);
    fp = fopen(hp, "rb");
    if (!fp)
        return FALSE;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    file->handle = fp;
    file->top = 0;
    file->bottom = (u32)size;
    file->pos = 0;
    return TRUE;
}

BOOL FS_CloseFile(FSFile *file)
{
    if (file->handle)
        fclose((FILE *)file->handle);
    file->handle = NULL;
    return TRUE;
}

s32 FS_ReadFile(FSFile *file, void *dst, s32 len)
{
    size_t got;
    if (!file->handle || len < 0)
        return -1;
    if ((u32)len > file->bottom - file->pos)
        len = (s32)(file->bottom - file->pos);
    if (fseek((FILE *)file->handle, (long)file->pos, SEEK_SET) != 0)
        return -1;
    got = fread(dst, 1, (size_t)len, (FILE *)file->handle);
    file->pos += (u32)got;
    return (s32)got;
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
