// Game file system: resolves paths in vfs.zip (through dsVfsIndex) or on the Nitro FS, plus one RAM file.
// Singleton at *0x020c8de4 (0x1de4 bytes, constructed by 0x0206d904 from main()). Vtable 0x020be9bc:
//   +0x00 Open  +0x04 Exists  +0x08 GetSize  +0x0c EnableVfs  +0x10 IsVfsEnabled  +0x14 MountArchive
//   +0x18 GetArchiveFile  +0x1c QueueLoad  +0x20 0x0206ced4  +0x24 0x0206cc20  +0x28 0x0206cc14
//   +0x2c SetMemoryFile  +0x30 ClearMemoryFile  +0x34 0x0206d24c (List)  ...
// Paths: "raw:" forces the Nitro FS; otherwise the VFS is used when enabled. Paths are lower-cased and use '\'.
// Inline members: @port main:0206c884 done  @port main:0206d294 done  @port main:0206d274 done
#pragma once

#include "dsFile.h"
#include "dsVfsIndex.h"

class dsFileSystem {
public:
    static dsFileSystem *Get() { return s_instance; }
    static void Create();   // part of 0x0206d904

    dsFileSystem();          // 0x0206dd80 (partial: the loader-thread queues are not ported)
    virtual ~dsFileSystem();
    virtual dsFile *Open(const char *path);
    virtual bool Exists(const char *path);
    virtual int GetSize(const char *path);
    virtual void EnableVfs(bool enable);
    virtual bool IsVfsEnabled() { return m_vfsEnabled; }
    virtual void *SetMemoryFile(const char *path, u32 size);
    virtual void ClearMemoryFile(void *data);

    // Used by dsPackedFile (0x0206d294, 0x0206d274).
    const u32 *FindVfsEntry(const char *path) const { return m_index ? m_index->Find(path) : nullptr; }
    const u32 *VfsOffsets() const { return m_index ? m_index->Offsets() : nullptr; }
    unzFile Archive() const { return m_unz; }

private:
    void LoadVfs(const char *zipPath, const char *datPath, const char *binPath);
    static bool NormalizePath(char *out, const char *in, bool vfsEnabled);

    static dsFileSystem *s_instance;

    bool m_vfsEnabled = false;      // +0x1d48
    bool m_vfsOk = false;           // +0x1d49: vfs.zip opened
    unzFile m_unz = nullptr;        // +0x1d4c
    dsVfsIndex *m_index = nullptr;  // +0x1d50
    u8 *m_memData = nullptr;        // +0x1d58
    char m_memName[128] = {};       // +0x1d5c
    u32 m_memSize = 0;              // +0x1ddc
};
