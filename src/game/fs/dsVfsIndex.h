// Directory tree of vfs.zip, loaded from vfs.bin (sizes in vfs.dat). Main 0x0207a098..0x0207a364.
//
// vfs.dat: "<nodes>, <files>, <string bytes>\n"
// vfs.bin: root node (8 bytes) | nodes[nodes] (8 bytes each) | u32 offsets[files] | lower-case names (NUL separated)
// node:    u16 name_lo, u16 next_sibling, u16 first_child, u16 info
//          name offset = name_lo | (info & 0x8000) << 1, file index = info & 0x7fff, 0xffff = none
// offsets: position of each file's central directory record, relative to the start of the central directory;
//          the file index is also the entry number in the ZIP.
// Inline accessors: @port main:0207a098 done  @port main:0207a09c done  @port main:0207a0a0 done
// @port main:0207a0a4 done  @port main:0207a0a8 done  @port main:0207a0ac done  @port main:0207a0b0 done
// @port main:0207a0b4 done
#pragma once

#include <vector>

#include "nitro.h"

struct dsVfsNode {
    u16 name;
    u16 next;
    u16 child;
    u16 info;
};

class dsVfsIndex {
public:
    dsVfsIndex(int numNodes, int numFiles, int stringBytes);
    ~dsVfsIndex();

    // Looks up a lower-case path with '\' separators. Returns the file's entry in the offsets table, or NULL.
    const u32 *Find(const char *path) const;
    // Appends the names of the children of a directory to out.
    void List(const char *path, std::vector<const char *> &out) const;

    dsVfsNode *Root() { return &m_root; }
    dsVfsNode *Nodes() { return m_nodes; }
    u32 *Offsets() { return m_offsets; }
    char *Strings() { return m_strings; }
    static int NodeSize() { return 8; }
    static int OffsetSize() { return 4; }
    void SetNumNodes(int n) { m_numNodes = n; }
    void SetNumFiles(int n) { m_numFiles = n; }
    void SetStringBytes(int n) { m_stringBytes = n; }

private:
    const dsVfsNode *FindNode(const char *path) const;
    const char *Name(const dsVfsNode *n) const { return m_strings + (n->name | (n->info & 0x8000) << 1); }

    dsVfsNode *m_nodes;         // +0x00
    int m_numNodes;             // +0x08
    u32 *m_offsets;             // +0x0c
    int m_numFiles;             // +0x14 (capacity), +0x10 count
    char *m_strings;            // +0x18
    int m_stringBytes;          // +0x20 (capacity), +0x1c count
    dsVfsNode m_root;           // +0x24
};
