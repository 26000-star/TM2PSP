// File objects returned by dsFileSystem::Open. Vtable layout of the original (CodeWarrior, slot = byte offset):
//   +0x00 ~dsFile (complete)   +0x04 ~dsFile (deleting)   +0x08 IsOpen   +0x0c Read   +0x10 Write
//   +0x14 Close                +0x18 GetSize
// Three implementations: dsPackedFile (entry of vfs.zip), dsRawFile (Nitro FS file), dsMemoryFile (RAM buffer).
// Inline members: @port main:0206e0d0 done  @port main:0206e0d4 done  @port main:0206e398 done
// @port main:0206e39c done  @port main:0207e134 done  @port main:0207e19c done  @port main:0207e138 done
#pragma once

#include "nitro.h"
#include "unzip.h"

class dsFile {
public:
    virtual ~dsFile() {}
    virtual bool IsOpen() = 0;
    virtual int Read(void *dst, u32 len) = 0;
    virtual int Write(const void *src, u32 len) = 0;
    virtual void Close() = 0;
    virtual int GetSize() = 0;

protected:
    // +0x04, +0x08 of the base class; the constructors clear them and nothing in the ported code reads them yet.
    u32 m_unk04 = 0;
    u8 m_unk08 = 0;
};

// vtable 0x020beab8, RTTI name "dsPackedFile". 0x20 bytes, allocated from a fixed pool.
class dsPackedFile : public dsFile {
public:
    dsPackedFile(const char *path, bool yieldToLoader);
    ~dsPackedFile() override;
    bool IsOpen() override { return m_open; }
    int Read(void *dst, u32 len) override;
    int Write(const void *, u32) override { return 0; }
    void Close() override;
    int GetSize() override;

    static int SizeOf(const char *path);
    static bool Exists(const char *path);

private:
    unzFile m_unz = nullptr;    // +0x0c: private copy of the archive state (unzDup)
    u32 m_cdOffset = 0;         // +0x10: entry offset relative to the central directory
    bool m_open = false;        // +0x14
    u32 m_pos = 0;              // +0x18
    bool m_yield = false;       // +0x1c: read in 1 KiB chunks and yield between them (loader thread)
};

// vtable 0x020beaec, RTTI name "dsRawFile". 0x54 bytes, allocated from a fixed pool.
class dsRawFile : public dsFile {
public:
    explicit dsRawFile(const char *path);
    ~dsRawFile() override;
    bool IsOpen() override { return m_open; }
    int Read(void *dst, u32 len) override;
    int Write(const void *, u32) override { return -1; }
    void Close() override;
    int GetSize() override;

    static int SizeOf(const char *path);
    static bool Exists(const char *path);

private:
    bool m_open = false;        // +0x09
    FSFile m_file;              // +0x0c
};

// vtable 0x020bf7a4. Wraps a buffer registered with dsFileSystem::SetMemoryFile.
class dsMemoryFile : public dsFile {
public:
    dsMemoryFile(u8 *data, u32 size, bool owns);
    ~dsMemoryFile() override;
    bool IsOpen() override { return true; }
    int Read(void *dst, u32 len) override;
    int Write(const void *src, u32 len) override;
    void Close() override { m_pos = 0; }
    int GetSize() override { return (int)m_size; }

private:
    u8 *m_data;                 // +0x0c
    u32 m_size;                 // +0x10
    u32 m_pos = 0;              // +0x14
    bool m_owns;                // +0x18
};
