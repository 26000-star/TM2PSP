#include "dsTrackLighting.h"

#include <stdio.h>
#include <string.h>

#include "script/dsLua.h"

static dsScriptManager s_scriptManager;

// @port ov003:0211c5d4 partial (randomPick: the random generator 0x0205ad60 is not ported, stays 0)
bool dsTrackLighting::Load(const char *env, const char *timeOfDay)
{
    char key[0x80];
    snprintf(key, sizeof key, "%s_%s", env, timeOfDay);
    dsLuaStateRef state = s_scriptManager.Run("config/tracks");
    if (!state.get())
        return false;
    dsLuaObject tracks = state->Global("tracks");
    bool found = false;
    for (dsLuaObject k = tracks.Begin(); !k.IsNil(); k = tracks.Next(k)) {
        dsLuaObject v = tracks.Value(k);
        const char *name = k.ToString();
        if (!name || strcmp(name, key) != 0)
            continue;
        for (int c = 0; c < 8; c++) {
            colors[c].x = v.IndexNumber(c * 3);
            colors[c].y = v.IndexNumber(c * 3 + 1);
            colors[c].z = v.IndexNumber(c * 3 + 2);
        }
        value24 = (fx32)(((s64)v.IndexNumber(24) * 0x29 + 0x800) >> 12);
        value25 = v.IndexNumber(25);
        found = true;
    }
    return found;
}

// @port main:02098aac done
void dsTrackLighting_ListKeys(std::set<std::string> &out)
{
    dsLuaStateRef state = s_scriptManager.Run("config/tracks");
    if (!state.get())
        return;
    dsLuaObject tracks = state->Global("tracks");
    for (dsLuaObject k = tracks.Begin(); !k.IsNil(); k = tracks.Next(k)) {
        const char *name = k.ToString();
        out.insert(name ? name : "");
    }
}
