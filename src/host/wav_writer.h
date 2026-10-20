// Minimal 16-bit stereo WAV writer for the host tools (tm_sdat, tm_race --wav).
#ifndef TM_WAV_WRITER_H
#define TM_WAV_WRITER_H

#include <stdint.h>
#include <stdio.h>
#include <vector>

static inline bool WriteWav(const char *path, const std::vector<int16_t> &stereo, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)(stereo.size() * 2);
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32((uint32_t)rate);
    u32((uint32_t)rate * 4);
    u16(4);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(bytes);
    fwrite(stereo.data(), 2, stereo.size(), f);
    return fclose(f) == 0;
}

#endif
