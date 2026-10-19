// The race application shared by the race drivers (host and PSP): the track menu in front of the race, the pause
// and result menus, track loading and unloading, and the best time per track kept in a small save file.
// This is a port stand-in: the game's own front end (main() 0x02003000 and its menus) is not ported. Drawing goes
// through two callbacks (text in the 5 x 7 font of race_session.h, flat rectangles); the 3D scene is drawn by the
// driver from `race` when it is not null.
#pragma once

#include <memory>
#include <string>

#include "race_session.h"

struct RaceTrackEntry {
    const char *id;    // scene under coa/: "easy/0"
    const char *label; // shown in the menu
};
const RaceTrackEntry *RaceTracks(int *count); // the 26 Coast tracks, menu order

typedef void (*RaceRectFn)(float x, float y, float w, float h, float r, float g, float b, float a, void *user);

class RaceApp {
public:
    enum Key { UP, DOWN, LEFT, RIGHT, OK, BACK, PAUSE };
    enum Screen { TRACKS, PLAYING, PAUSED, RESULT };

    // savePath: best times and the last track ("" = none). okName / backName: button names shown in the hints.
    RaceApp(const std::string &savePath, const char *okName, const char *backName);
    ~RaceApp();

    // Loads track `index` (any session is deleted first, with the collision meshes) and starts playing.
    bool Start(int index);
    int FindTrack(const char *id) const; // -1 when unknown
    void Unload();
    // A menu key; returns false when the player leaves the program (BACK on the track list).
    bool Press(Key k);
    // One 30 Hz frame: the race advances while playing; a finish records the best time and opens the result menu
    // after 3 s (unless menus are off: the drivers' auto tests).
    void Frame(const dsCarInput &in);
    // HUD while playing, the menus otherwise, for a w x h screen (scale as RaceHudLayout).
    void Overlay(int w, int h, float scale, RaceTextFn text, RaceRectFn rect, void *user) const;

    Screen screen = TRACKS;
    std::unique_ptr<RaceSession> race;
    int track = -1;       // loaded track
    int selected = 0;     // cursor on the track list
    int item = 0;         // cursor in the pause / result menu
    bool menus = true;    // false: no result menu (auto tests)
    bool lighting = true; // false: sessions draw unlit (tm_race --unlit)

private:
    void LoadSave();
    void WriteSave() const;
    int ItemCount() const;

    std::string m_savePath, m_ok, m_back;
    std::unique_ptr<fx32[]> m_best; // best finish time per track, 0 = none
    int m_count = 0;
    int m_finishFrames = -1;  // frames since the finish (-1: not finished)
    bool m_newRecord = false;
};
