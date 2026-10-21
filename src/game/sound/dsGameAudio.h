// The game's audio state (main 0x0208bcfc..0x0208c9d4, 0x30 bytes, ctor 0x0208c8ec): which sequence archives are
// loaded for the current screen, the music, two volume faders and the car audio of the race. It drives
// dsSoundManager by the names of two string tables (0x0208ab50: the SEQARC_* names, 0x0208c9d4: the sounds).
//  - +0x00 state: 0 none, 1 front end (0x0208c4b8), 3 editor (0x0208c3cc), 4 race (0x0208c04c);
//  - +0x04 the music playing (0x0208bd74: 1 front end, 2 Coast, 3 Snow, 4 Island, 5 Stadium), +0x08 its handle;
//  - +0x10 music on, +0x11 ambience on, +0x12 sound on (options; all on by default);
//  - +0x14 / +0x20 the sound-effect and music faders {u8 fade in, s32 frames left, s32 frames}, applied by the
//    update (0x0208c730) to dsSoundManager's SE / music volumes; +0x2c stops the music at the end of a fade-out;
//  - +0x2d set outside the race proper (front end, editor, the editor's test drive): the music is not faded with
//    the sound effects and the end of the race stops everything.
#pragma once

#include <string>

#include "nitro.h"

struct dsCarParams;
class dsCarDynamics;

class dsGameAudio {
public:
    enum State { NONE = 0, FRONT_END = 1, EDITOR = 3, RACE = 4 };
    enum Music { MUSIC_NONE = 0, MUSIC_FRONT_END = 1, MUSIC_COAST = 2, MUSIC_SNOW = 3, MUSIC_ISLAND = 4,
                 MUSIC_STADIUM = 5 };

    static dsGameAudio *Get();      // the ctor (0x0208c8ec) on first use
    static void Destroy();

    // 0x0208c04c: environment 0..3 (stadium, island, snow, coast; +4: the editor's test drive, with the
    // environment's ambience instead of its music), horn 0..7, output mode (settings +0x2c + 1). The car's name
    // picks SEQARC_CAR_<car> and its config/caraudio entry; its parameters give the gear thresholds.
    void StartRace(int env, int horn, int outputMode, const std::string &car, const dsCarParams &params);
    void EndRace();                 // 0x0208bf50
    void StartFrontEnd();           // 0x0208c4b8
    void StartEditor();             // 0x0208c3cc: the menu sounds and SEQARC_EDITOR (stamp, delete, rotate)
    void PlayMusic(int music, fx32 fadeSeconds); // 0x0208bd74
    u32 PlaySe(const char *name);   // 0x0208bd5c
    void FadeIn(int fader, fx32 seconds);   // 0x0208c5ec (fader 0: sound effects, 1: music)
    void FadeOut(int fader, fx32 seconds);  // 0x0208c588
    void FadeOutAll(fx32 seconds);          // 0x0208c674
    // 0x0208c730, once per game frame: the faders, then in the race the car audio (dsCarAudio::Update) heard from
    // the scene camera's matrix.
    void Update(const MtxFx43 *camera, const dsCarDynamics *car);

    int state = NONE;
    int music = MUSIC_NONE;
    u32 musicHandle = 0;
    bool musicOn = true, ambienceOn = true, soundOn = true;
    bool outsideRace = false;       // +0x2d

private:
    struct Fader {
        bool fadeIn = false;
        s32 left = 0, frames = 0;
    };
    void ResetFaders();

    Fader m_fader[2];
    bool m_stopMusic = false;       // +0x2c
};
