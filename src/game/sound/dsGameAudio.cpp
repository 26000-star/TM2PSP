#include "sound/dsGameAudio.h"

#include <stdio.h>

#include "car/dsCarDynamics.h"
#include "sound/dsCarAudio.h"
#include "sound/dsSound3D.h"
#include "sound/dsSoundManager.h"

static dsGameAudio *s_gameAudio;

// The sequence archives of the string table 0x0208ab50 (+0x10 .. +0x1c, +0x30 .. +0x4c, +0x50 .. +0x5c).
static const char *const kEnvArchive[4] = {"SEQARC_STADIUM", "SEQARC_ISLAND", "SEQARC_SNOW", "SEQARC_COAST"};
static const char *const kAmbience[4] = {"SEQARC_AMB_STADIUM", "SEQARC_AMB_ISLAND", "SEQARC_AMB_SNOW",
                                         "SEQARC_AMB_COAST"};
static const char *const kRaceMusic[4] = {"SEQARC_StadiumMusic", "SEQARC_IslandMusic", "SEQARC_SnowMusic",
                                          "SEQARC_CoastMusic"};
// 0x0208bd74's names by music number (1..5).
static const char *const kMusic[6] = {nullptr, "SEQARC_FrontEndMusic", "SEQARC_CoastMusic", "SEQARC_SnowMusic",
                                      "SEQARC_IslandMusic", "SEQARC_StadiumMusic"};

// fx32 seconds -> 60 Hz frames: (s * 0x3c000 + 0x800) >> 24, as dsSoundManager.
static s32 FadeFrames(fx32 seconds) { return (s32)(((s64)seconds * 0x3c000 + 0x800) >> 24); }

// @port main:0208c8ec partial (the menu sounds it gives the UI, *0x020d35cc +0x40..+0x48 = MENU_SELECT, MENU_BACK,
// MENU_MOVE, are played by the port's menus through PlaySe)
dsGameAudio *dsGameAudio::Get()
{
    if (!s_gameAudio)
        s_gameAudio = new dsGameAudio;
    return s_gameAudio;
}

void dsGameAudio::Destroy()
{
    delete s_gameAudio;
    s_gameAudio = nullptr;
}

void dsGameAudio::ResetFaders()
{
    m_fader[0] = Fader();
    m_fader[1] = Fader();
    m_stopMusic = false;
}

// @port main:0208c04c partial (the car's name and parameters are passed in: the game takes them from the local
// player, *0x027e0e54 +0x20)
void dsGameAudio::StartRace(int env, int horn, int outputMode, const std::string &car, const dsCarParams &params)
{
    dsCarAudio *ca = dsCarAudio::Get();
    if (state == RACE) {   // already in a race: the archives and the music stay
        ca->Init(car, params);
        return;
    }
    dsSoundManager *sm = dsSoundManager::Get();
    sm->SetOutputMode(outputMode);
    sm->PopAllStates();
    sm->LoadByName("SEQARC_MENU_BASIC");
    sm->LoadByName("SEQARC_DRIVING");
    sm->LoadByName(kEnvArchive[env & 3]);
    char name[256];
    snprintf(name, sizeof name, "SEQARC_HORN_%02d", horn & 7);
    sm->LoadByName(name);
    musicHandle = sm->InvalidHandle();
    if ((unsigned)(env - 4) < 4) {   // the editor's test drive: ambience
        sm->LoadByName("SEQARC_EDITOR");
        sm->LoadByName("SEQARC_MENU");
        if (ambienceOn) {
            sm->LoadByName(kAmbience[env & 3]);
            musicHandle = sm->PlaySe("AMBIENT");
        }
    } else if (musicOn) {
        sm->LoadByName(kRaceMusic[env & 3]);
        musicHandle = sm->PlayMusic(kRaceMusic[env & 3], 0);
    }
    ResetFaders();
    sm->SetSeVolume(0x7f);
    sm->SetMusicVolume(0x7f);
    snprintf(name, sizeof name, "SEQARC_CAR_%s", car.c_str());
    sm->LoadByName(name);
    outsideRace = (unsigned)(env - 4) < 4;
    ca->Init(car, params);
    state = RACE;
}

// @port main:0208bf50 done
// After a race proper only the car audio goes: the archives and the music stay for the next race (StartRace keeps
// them) until the front end (StartFrontEnd) stops everything.
void dsGameAudio::EndRace()
{
    if (!outsideRace) {
        dsCarAudio::Destroy();
        return;
    }
    dsSoundManager *sm = dsSoundManager::Get();
    sm->ResumeAll();
    if (state == RACE)
        dsCarAudio::Destroy();
    if (sm->IsValid(musicHandle))
        musicHandle = sm->Stop(musicHandle, 0);
    dsSound3D::Get()->ReleaseAll();
    sm->StopAll(0);
    sm->PopAllStates();
    state = NONE;
    music = MUSIC_NONE;
}

// @port main:0208c4b8 done
void dsGameAudio::StartFrontEnd()
{
    if (state == FRONT_END)
        return;
    dsSoundManager *sm = dsSoundManager::Get();
    sm->StopAll(0);
    sm->PopAllStates();
    sm->LoadByName("SEQARC_MENU_BASIC");
    sm->LoadByName("SEQARC_MENU");
    ResetFaders();
    sm->SetSeVolume(0x7f);
    sm->SetMusicVolume(0x7f);
    music = MUSIC_NONE;
    musicHandle = sm->InvalidHandle();
    outsideRace = true;
    state = FRONT_END;
}

// @port main:0208c3cc done
void dsGameAudio::StartEditor()
{
    if (state == EDITOR)
        return;
    dsSoundManager *sm = dsSoundManager::Get();
    sm->StopAll(0);
    sm->PopAllStates();
    sm->LoadByName("SEQARC_MENU_BASIC");
    sm->LoadByName("SEQARC_MENU");
    sm->LoadByName("SEQARC_EDITOR");
    ResetFaders();
    sm->SetSeVolume(0x7f);
    sm->SetMusicVolume(0x7f);
    music = MUSIC_NONE;
    musicHandle = sm->InvalidHandle();
    outsideRace = true;
    state = EDITOR;
}

// @port main:0208bd74 done
void dsGameAudio::PlayMusic(int index, fx32 fadeSeconds)
{
    dsSoundManager *sm = dsSoundManager::Get();
    if (music == index && sm->IsValid(musicHandle)) {
        if (sm->MusicVolume() != 0x7f) {
            FadeIn(0, fadeSeconds);
            FadeIn(1, fadeSeconds);
        }
        return;
    }
    if (sm->IsValid(musicHandle)) {
        musicHandle = sm->Stop(musicHandle, 0);
        sm->PopState();
        music = MUSIC_NONE;
    }
    if (!musicOn)
        return;
    if (index >= 1 && index <= 5) {
        sm->LoadByName(kMusic[index]);
        musicHandle = sm->PlayMusic(kMusic[index], 0);
    }
    FadeIn(0, fadeSeconds);
    FadeIn(1, fadeSeconds);
    music = index;
    m_stopMusic = false;
}

// @port main:0208bd5c done
u32 dsGameAudio::PlaySe(const char *name) { return dsSoundManager::Get()->PlaySe(name); }

// @port main:0208c5ec done
// A fade-in starts only when the volume is not already full.
void dsGameAudio::FadeIn(int fader, fx32 seconds)
{
    dsSoundManager *sm = dsSoundManager::Get();
    Fader &f = m_fader[fader & 1];
    if (seconds == 0) {
        if (fader == 0)
            sm->SetSeVolume(0x7f);
        else
            sm->SetMusicVolume(0x7f);
        f.left = f.frames = 0;
        return;
    }
    int v = fader == 0 ? sm->SeVolume() : sm->MusicVolume();
    if (v == 0x7f)
        return;
    f.frames = f.left = FadeFrames(seconds);
    f.fadeIn = true;
}

// @port main:0208c588 done
void dsGameAudio::FadeOut(int fader, fx32 seconds)
{
    Fader &f = m_fader[fader & 1];
    if (seconds == 0) {
        dsSoundManager *sm = dsSoundManager::Get();
        if (fader == 0)
            sm->SetSeVolume(0);
        else
            sm->SetMusicVolume(0);
        f.left = f.frames = 0;
        return;
    }
    f.frames = f.left = FadeFrames(seconds);
    f.fadeIn = false;
}

// @port main:0208c674 done
// The sound effects fade out; in the race proper the music too, and it stops at the end.
void dsGameAudio::FadeOutAll(fx32 seconds)
{
    FadeOut(0, seconds);
    if (!outsideRace) {
        FadeOut(1, seconds);
        m_stopMusic = true;
    }
}

// @port main:0208c730 partial (the camera and the car are passed in: the game reads the scene's camera,
// *0x020d6630 +4, and the car list)
void dsGameAudio::Update(const MtxFx43 *camera, const dsCarDynamics *car)
{
    dsSoundManager *sm = dsSoundManager::Get();
    for (int i = 0; i < 2; i++) {
        Fader &f = m_fader[i];
        if (f.frames == 0)
            continue;
        int v;
        if (!f.fadeIn) {
            v = f.left * 0x7f / f.frames;
            if (--f.left == 0) {
                f.frames = 0;
                v = 0;
                if (m_stopMusic && i == 1) {
                    if (sm->IsValid(musicHandle)) {
                        musicHandle = sm->Stop(musicHandle, 0);
                        sm->PopState();
                        music = MUSIC_NONE;
                    }
                    m_stopMusic = false;
                }
            }
        } else {
            v = (f.frames - f.left) * 0x7f / f.frames;
            if (--f.left == 0) {
                f.frames = 0;
                v = 0x7f;
            }
        }
        if (i == 0)
            sm->SetSeVolume(v);
        else
            sm->SetMusicVolume(v);
    }
    if (state == RACE && camera && car)
        dsCarAudio::Get()->Update(*camera, *car);
}
