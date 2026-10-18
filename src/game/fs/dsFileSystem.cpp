#include "dsFileSystem.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

dsFileSystem *dsFileSystem::s_instance = nullptr;

// @port main:0206d904 partial (creates the singleton; the loader thread, its message queue and the file pools are not ported)
void dsFileSystem::Create()
{
    if (!s_instance)
        s_instance = new dsFileSystem();
}

// @port main:0206dd80 partial (the two request queues of the loader thread are not ported)
dsFileSystem::dsFileSystem() {}

// @port main:0206d994 partial
dsFileSystem::~dsFileSystem()
{
    if (m_unz)
        unzClose(m_unz);
    delete m_index;
    if (s_instance == this)
        s_instance = nullptr;
}

// Inlined at the start of Open, Exists and GetSize (0x0206d790, 0x0206d67c, 0x0206d560): strips "raw:", copies at
// most 128 characters, turns '/' into '\' and lower-cases ASCII. Returns whether the VFS should be searched.
bool dsFileSystem::NormalizePath(char *out, const char *in, bool vfsEnabled)
{
    bool useVfs = true;
    if (strncmp(in, "raw:", 4) == 0) {
        in += 4;
        useVfs = false;
    } else if (!vfsEnabled) {
        useVfs = false;
    }
    char *o = out;
    for (int n = 0x80; *in && n; n--)
        *o++ = *in++;
    *o = '\0';
    for (size_t i = 0, len = strlen(out); i < len; i++) {
        if (out[i] == '/')
            out[i] = '\\';
        int c = out[i];
        if (c >= 0 && c < 0x80 && isupper(c))
            out[i] = (char)tolower(c);
    }
    return useVfs;
}

// @port main:0206d790 done
dsFile *dsFileSystem::Open(const char *path)
{
    char norm[128 + 1];
    bool useVfs = NormalizePath(norm, path, m_vfsEnabled);
    if (m_memData && strcmp(norm, m_memName) == 0)
        return new dsMemoryFile(m_memData, m_memSize, false);
    if (!useVfs)
        return new dsRawFile(norm);
    if (!m_vfsOk)
        return nullptr;
    // The original yields between reads when called from the loader thread (OSi_ThreadInfo.current == loader).
    return new dsPackedFile(norm, false);
}

// @port main:0206d67c done
bool dsFileSystem::Exists(const char *path)
{
    char norm[128 + 1];
    bool useVfs = NormalizePath(norm, path, m_vfsEnabled);
    if (m_memData && strcmp(norm, m_memName) == 0)
        return true;
    if (!useVfs)
        return dsRawFile::Exists(norm);
    if (m_vfsOk)
        return dsPackedFile::Exists(norm);
    return false;
}

// @port main:0206d560 done
int dsFileSystem::GetSize(const char *path)
{
    char norm[128 + 1];
    bool useVfs = NormalizePath(norm, path, m_vfsEnabled);
    if (m_memData && strcmp(norm, m_memName) == 0)
        return (int)m_memSize;
    if (!useVfs)
        return dsRawFile::SizeOf(norm);
    if (m_vfsOk)
        return dsPackedFile::SizeOf(norm);
    return -1;
}

// @port main:0206d41c done
void dsFileSystem::EnableVfs(bool enable)
{
    m_vfsEnabled = enable;
    if (m_vfsEnabled)
        LoadVfs("vfs.zip", "vfs.dat", "vfs.bin");
}

// @port main:0206d2b4 done
void dsFileSystem::LoadVfs(const char *zipPath, const char *datPath, const char *binPath)
{
    bool ok = false;
    bool zipFound;
    {
        dsRawFile zip(zipPath);
        zipFound = zip.IsOpen();
    }
    if (zipFound) {
        int numNodes = 0, numFiles = 0, stringBytes = 0;
        dsRawFile dat(datPath);
        if (dat.IsOpen()) {
            char text[128];
            text[0] = '\0';
            int n = dat.Read(text, sizeof text - 1);
            text[n > 0 ? n : 0] = '\0'; // the original reads 0x80 bytes into a zeroed buffer
            sscanf(text, "%d, %d, %d\n", &numNodes, &numFiles, &stringBytes);
            m_index = new dsVfsIndex(numNodes, numFiles, stringBytes + 2);
            dat.Close();
            dsRawFile bin(binPath);
            if (bin.IsOpen()) {
                bin.Read(m_index->Root(), dsVfsIndex::NodeSize());
                bin.Read(m_index->Nodes(), dsVfsIndex::NodeSize() * numNodes);
                bin.Read(m_index->Offsets(), dsVfsIndex::OffsetSize() * numFiles);
                bin.Read(m_index->Strings(), stringBytes);
                bin.Close();
                m_index->SetNumNodes(numNodes);
                m_index->SetNumFiles(numFiles);
                m_index->SetStringBytes(stringBytes);
            }
        }
        ok = m_index != nullptr; // original: vfs.dat's open flag, kept in a register from before Close()
    }
    if (ok) {
        // data_020ce8e4 is set around the call (tells the loader thread that the archive is being opened)
        m_unz = unzOpen(zipPath);
        m_vfsOk = m_unz != nullptr;
    }
}

// @port main:0206d4bc done
void *dsFileSystem::SetMemoryFile(const char *path, u32 size)
{
    u8 *data = new u8[size];
    m_memData = data;
    snprintf(m_memName, sizeof m_memName, "%s", path);
    for (size_t i = 0, len = strlen(m_memName); i < len; i++) {
        if (m_memName[i] == '/')
            m_memName[i] = '\\';
        int c = m_memName[i];
        if (c >= 0 && c < 0x80 && isupper(c))
            m_memName[i] = (char)tolower(c);
    }
    m_memSize = size;
    return data;
}

// @port main:0206d49c done
void dsFileSystem::ClearMemoryFile(void *data)
{
    delete[] (u8 *)data;
    m_memData = nullptr;
    m_memSize = 0;
}
