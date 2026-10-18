#include "dsFile.h"

#include <string.h>

#include "dsFileSystem.h"

// ---- dsPackedFile ----

// @port main:0206e2d4 done
dsPackedFile::dsPackedFile(const char *path, bool yieldToLoader) : m_yield(yieldToLoader)
{
    dsFileSystem *fs = dsFileSystem::Get();
    const u32 *entry = fs->FindVfsEntry(path);
    // (yield to the loader thread here when m_yield)
    m_open = entry != nullptr;
    if (m_open) {
        m_cdOffset = *entry;
        u32 cd = unzGetOffsetCentralDir(fs->Archive());
        u32 index = (u32)(entry - fs->VfsOffsets());
        m_unz = unzDup(fs->Archive());
        if (unzGoToFilePos(m_unz, m_cdOffset + cd, index) != UNZ_OK) {
            m_open = false;
            unzFreeDup(m_unz); // original: unzClose (0x0202364c) on the copy, which also FS_CloseFile()s it
            m_unz = nullptr;
            return;
        }
        if (unzOpenCurrentFile(m_unz) != UNZ_OK) {
            m_open = false;
            unzFreeDup(m_unz);
            m_unz = nullptr;
        }
    }
}

// @port main:0206e2b0 done
dsPackedFile::~dsPackedFile()
{
    if (m_open)
        Close();
}

// @port main:0206e12c done
int dsPackedFile::Read(void *dst, u32 len)
{
    if (!m_open)
        return -1;
    u8 *out = (u8 *)dst;
    int total = 0;
    while (len != 0 && total != -1) {
        u32 chunk = len;
        if (m_yield && len > 0x400)
            chunk = 0x400;
        int n = unzReadCurrentFile(m_unz, out, chunk);
        int next = -1;
        if (n != -1) {
            m_pos += n;
            out += chunk;
            len -= chunk;
            next = total + n;
        }
        total = next;
        // (yield to the loader thread here when m_yield)
    }
    return total;
}

// @port main:0206e114 done
void dsPackedFile::Close()
{
    if (m_open)
        unzFreeDup(m_unz);
    m_unz = nullptr;
    m_open = false;
}

// @port main:0206e0d8 done
int dsPackedFile::GetSize()
{
    unz_file_info info;
    if (!m_open)
        return -1;
    if (unzGetCurrentFileInfo(m_unz, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
        return -1;
    return (int)info.uncompressed_size;
}

// @port main:0206e1b4 done
int dsPackedFile::SizeOf(const char *path)
{
    dsPackedFile f(path, false);
    return f.IsOpen() ? f.GetSize() : -1;
}

// @port main:0206e23c done
bool dsPackedFile::Exists(const char *path)
{
    dsPackedFile f(path, false);
    return f.IsOpen();
}

// ---- dsRawFile ----

// @port main:0206e538 done
dsRawFile::dsRawFile(const char *path)
{
    FS_InitFile(&m_file);
    m_open = FS_OpenFile(&m_file, path) > 0;
}

// @port main:0206e514 done
dsRawFile::~dsRawFile()
{
    if (m_open)
        Close();
}

// @port main:0206e3b8 done
int dsRawFile::Read(void *dst, u32 len)
{
    if (!m_open)
        return -1;
    return FS_ReadFile(&m_file, dst, (s32)len);
}

// @port main:0206e3d4 done
void dsRawFile::Close()
{
    if (m_open) {
        FS_CloseFile(&m_file);
        m_open = false;
    }
}

// @port main:0206e3a4 done
int dsRawFile::GetSize()
{
    return m_open ? (int)FS_GetLength(&m_file) : -1;
}

// @port main:0206e3ec done
int dsRawFile::SizeOf(const char *path)
{
    FSFile f;
    FS_InitFile(&f);
    if (FS_OpenFile(&f, path) < 1)
        return -1;
    int size = (int)FS_GetLength(&f);
    FS_CloseFile(&f);
    return size;
}

// @port main:0206e480 done
bool dsRawFile::Exists(const char *path)
{
    FSFile f;
    FS_InitFile(&f);
    BOOL ok = FS_OpenFile(&f, path);
    if (ok > 0)
        FS_CloseFile(&f);
    return ok > 0;
}

// ---- dsMemoryFile ----

// @port main:0207e1fc done
dsMemoryFile::dsMemoryFile(u8 *data, u32 size, bool owns) : m_data(data), m_size(size), m_owns(owns) {}

// @port main:0207e1d4 done
dsMemoryFile::~dsMemoryFile()
{
    if (m_owns)
        delete[] m_data;
    m_data = nullptr;
}

// @port main:0207e13c done
int dsMemoryFile::Read(void *dst, u32 len)
{
    if (m_pos + len > m_size)
        len = m_size - m_pos;
    if (len)
        memcpy(dst, m_data + m_pos, len);
    m_pos += len;
    return (int)len;
}

// @port main:0207e16c done
int dsMemoryFile::Write(const void *src, u32 len)
{
    if (m_pos + len > m_size)
        len = m_size - m_pos;
    if ((int)len > 0)
        memcpy(m_data + m_pos, src, len);
    m_pos += len;
    return (int)len;
}
