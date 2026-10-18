// Per environment and time of day lighting, from Scripts/config/tracks.luac: the global "tracks" maps
// "<env>_<tod>" (sta_rise, coa_day, ...) to 26 numbers: eight RGB colours (fx32, 0..1) then two scalars.
// ov003 0x0211c5d4 loads the entry of the current race; main 0x02098aac lists the keys.
#pragma once

#include <set>
#include <string>

#include "nitro.h"

struct dsTrackLighting {
    VecFx32 colors[8] = {};     // +0x04, 16-byte stride (+0x04 +0x14 ... +0x74); meaning not identified yet
    fx32 value25 = 0;           // +0xdc: entry 25 as is
    fx32 value24 = 0;           // +0xe0: entry 24 * 41 / 4096 (about / 100), rounded
    u32 randomPick = 0;         // +0xe4: race and platform modes only, random % 3

    bool Load(const char *env, const char *timeOfDay); // ov003 0x0211c5d4 (port: names passed in)
};

// main 0x02098aac: the set of keys of "tracks" (stored at +0x0c of its owner).
void dsTrackLighting_ListKeys(std::set<std::string> &out);
