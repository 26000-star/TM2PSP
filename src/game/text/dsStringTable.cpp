// Localised strings (see dsStringTable.h).
#include "text/dsStringTable.h"

#include <stdio.h>
#include <string.h>

#include "fs/dsFile.h"
#include "fs/dsFileSystem.h"

namespace {

const char *const kRoot = "strings/Main";                                  // main 0x020bf7c0
const char *const kFiles[dsStringTable::FILE_COUNT] = {"Common", "FrontEnd", "BackEnd"}; // 0x020bf7c4
const char *const kLanguages[dsStringTable::LANG_COUNT] = {"gb", "us", "fr", "it", "de", "es"}; // 0x020bf7d0

u32 Hash(const char *s)
{
    u32 h = 5381;
    for (; *s; s++)
        h = h * 33 + ((u8)*s & 0xdf);
    return h;
}

bool SameKey(const char *a, const char *b)
{
    for (; ((u8)*a & 0xdf) == ((u8)*b & 0xdf); a++, b++)
        if (!*a)
            return true;
    return false;
}

bool ReadAll(const char *path, std::vector<u8> *out)
{
    dsFile *f = dsFileSystem::Get()->Open(path);
    if (!f)
        return false;
    bool ok = f->IsOpen();
    if (ok) {
        out->resize((size_t)f->GetSize());
        ok = f->Read(out->data(), (u32)out->size()) == (int)out->size();
    }
    delete f;
    return ok;
}

u16 Rd16(const std::vector<u8> &d, size_t p) { return (u16)(d[p] | d[p + 1] << 8); }

} // namespace

dsStringTable *dsStringTable::Get()
{
    static dsStringTable table;
    return &table;
}

// @port main:0207e7bc done (the reads through the file object, as one buffer) / main:0207e724 done
bool dsStringTable::Load(int language, int file)
{
    m_names.clear();
    m_text.clear();
    m_chars.clear();
    m_language = language >= 0 && language < LANG_COUNT ? language : LANG_GB;
    char path[0x80];
    snprintf(path, sizeof path, "%s/tokens.dat", kRoot);
    std::vector<u8> d;
    if (!ReadAll(path, &d) || d.size() < 6 * FILE_COUNT + 0x200)
        return false;
    size_t p = 0;
    u32 total = 0;
    for (int k = 0; k < FILE_COUNT; k++, p += 6) {
        m_count[k] = Rd16(d, p);
        total += m_count[k];
    }
    u16 sizes[256];
    for (int b = 0; b < 256; b++, p += 2)
        sizes[b] = Rd16(d, p);
    m_names.assign(d.begin() + (long)p, d.end());
    m_names.push_back('\0');
    m_text.assign(total, -1);
    // Bucket starts and the index of each bucket's first key.
    u32 start = 0, index = 0;
    for (int b = 0; b < 256; b++) {
        m_bucketStart[b] = (u16)start;
        m_bucketIndex[b] = (u16)index;
        u32 end = start + sizes[b];
        for (u32 q = start + 1; q < end && q < m_names.size(); q += (u32)strlen(&m_names[q]) + 2)
            index++;
        start = end;
    }
    LoadStrings(FILE_COMMON);
    if (file != FILE_COMMON)
        LoadStrings(file);
    return true;
}

// @port main:0207e5a0 done
bool dsStringTable::LoadStrings(int file)
{
    if (file < 0 || file >= FILE_COUNT)
        return false;
    char path[0x80];
    snprintf(path, sizeof path, "%s/%s/%s.str", kRoot, kFiles[file], kLanguages[m_language]);
    std::vector<u8> d;
    if (!ReadAll(path, &d))
        return false;
    size_t p = 0, q = 0;
    int index = 0;
    for (u32 n = 0; n < m_count[file] && p + 2 <= d.size(); n++) {
        u16 len = Rd16(d, p);
        p += 2;
        if (p + len > d.size())
            break;
        while (q + 1 < m_names.size() && (u8)m_names[q] != file) {
            q += strlen(&m_names[q + 1]) + 2;
            index++;
        }
        if (index >= (int)m_text.size())
            break;
        m_text[index] = (int)m_chars.size();
        m_chars.insert(m_chars.end(), d.begin() + (long)p, d.begin() + (long)(p + len));
        m_chars.push_back('\0');
        p += len;
        q += strlen(&m_names[q + 1]) + 2;
        index++;
    }
    return true;
}

// @port main:0207e4c4 done (the bucket is searched by its byte range)
int dsStringTable::Find(const char *key) const
{
    if (!key || !*key || m_names.empty())
        return -1;
    int b = Hash(key) & 0xff;
    u32 q = m_bucketStart[b], end = b < 255 ? m_bucketStart[b + 1] : (u32)m_names.size() - 1;
    for (int index = m_bucketIndex[b]; q + 1 < end; index++) {
        const char *name = &m_names[q + 1];
        if (SameKey(name, key))
            return index;
        q += (u32)strlen(name) + 2;
    }
    return -1;
}

// @port main:0207e4a0 done
const char *dsStringTable::Text(int index) const
{
    if (index < 0 || index >= (int)m_text.size() || m_text[index] < 0)
        return "";
    return &m_chars[(size_t)m_text[index]];
}
