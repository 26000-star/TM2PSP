// The game's localised strings (global *0x020d35a4, main 0x0207e4a0..0x0207e908), from strings/Main:
//  - tokens.dat: for each of the three string files (Common, FrontEnd, BackEnd) {u16 string count, u32 bytes},
//    then 256 u16 bucket sizes in bytes, then the names block: {u8 file, name, 0} sorted by bucket. A key's index
//    is its place in the block; its bucket is (h = h x 33 + (c & 0xdf), seed 5381) & 0xff, compared without case.
//  - <File>/<lang>.str: {u16 length, chars} per string, given in order to the names of that file.
// Common is always loaded with one of the two others (the race: BackEnd). Strings are Latin-1.
#pragma once

#include <vector>

#include "nitro.h"

class dsStringTable {
public:
    // Languages in the game's order (main 0x020bf7d0, file names gb, us, fr, it, de, es).
    enum Language { LANG_GB, LANG_US, LANG_FR, LANG_IT, LANG_DE, LANG_ES, LANG_COUNT };
    enum File { FILE_COMMON, FILE_FRONTEND, FILE_BACKEND, FILE_COUNT };

    static dsStringTable *Get(); // the game's instance (*0x020d35a4), created on first use

    // tokens.dat (0x0207e7bc) then Common and `file` in `language` (0x0207e724 / 0x0207e5a0). False when
    // tokens.dat is missing; missing strings read as "".
    bool Load(int language, int file);
    // A port addition: one more string file after Load (the port's front end shows the race's pause and results
    // menus, so it keeps FrontEnd and BackEnd together).
    bool LoadAlso(int file) { return LoadStrings(file); }
    bool IsLoaded() const { return !m_names.empty(); }
    int Language() const { return m_language; }

    int Find(const char *key) const;         // 0x0207e4c4: the key's index, -1 if unknown
    const char *Text(int index) const;       // 0x0207e4a0: "" for -1 or a string not loaded
    const char *Text(const char *key) const { return Text(Find(key)); }

private:
    bool LoadStrings(int file);              // 0x0207e5a0

    int m_language = LANG_GB;                // +0x00
    u32 m_count[FILE_COUNT] = {};            // +0x24: strings per file
    std::vector<char> m_names;               // +0x30: the names block
    u16 m_bucketStart[256] = {};             // +0x34: byte offset of each bucket in the names block
    u16 m_bucketIndex[256] = {};             // +0x238: index of each bucket's first key
    std::vector<int> m_text;                 // +0x234: per key, offset in m_chars or -1
    std::vector<char> m_chars;               // +0x10: the loaded strings, NUL-terminated
};
