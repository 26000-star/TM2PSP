#include "dsVfsIndex.h"

#include <string.h>

// @port main:0207a30c done
dsVfsIndex::dsVfsIndex(int numNodes, int numFiles, int stringBytes)
    : m_numNodes(numNodes), m_numFiles(numFiles), m_stringBytes(stringBytes)
{
    // The original takes the nodes from a block allocator (0x020216a4) and the other arrays from the heap.
    m_nodes = new dsVfsNode[numNodes];
    m_offsets = new u32[numFiles];
    m_strings = new char[stringBytes];
    m_root.name = 0;
    m_root.next = m_root.child = m_root.info = 0xffff;
}

dsVfsIndex::~dsVfsIndex()
{
    delete[] m_nodes;
    delete[] m_offsets;
    delete[] m_strings;
}

// Shared walk of Find (0x0207a24c) and List (0x0207a0b8), which CodeWarrior inlined into both.
const dsVfsNode *dsVfsIndex::FindNode(const char *path) const
{
    char parts[512];
    u32 len = (u32)strlen(path);
    u32 depth = 0;
    strcpy(parts, path);
    for (u32 i = 0; i < len; i++) {
        if (parts[i] == '\\') {
            parts[i] = '\0';
            depth++;
        }
    }
    const dsVfsNode *dir = &m_root;
    const char *part = parts;
    for (u32 level = 0;; level++) {
        const dsVfsNode *found = nullptr;
        for (u16 c = dir->child; c != 0xffff; c = m_nodes[c].next) {
            if (strcmp(Name(&m_nodes[c]), part) == 0) {
                found = &m_nodes[c];
                break;
            }
        }
        if (!found)
            return nullptr;
        if (level >= depth)
            return found;
        part += strlen(part) + 1;
        dir = found;
    }
}

// @port main:0207a24c done
const u32 *dsVfsIndex::Find(const char *path) const
{
    const dsVfsNode *n = FindNode(path);
    return n ? &m_offsets[n->info & 0x7fff] : nullptr;
}

// @port main:0207a0b8 done
void dsVfsIndex::List(const char *path, std::vector<const char *> &out) const
{
    const dsVfsNode *n = FindNode(path);
    if (!n || n->child == 0xffff)
        return;
    for (u16 c = n->child; c != 0xffff; c = m_nodes[c].next)
        out.push_back(Name(&m_nodes[c]));
}
