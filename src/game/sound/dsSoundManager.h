// The game's sound manager (*0x020c8e88, main 0x0205d1f8..0x0205ea98, vtable 0x020bd5a8): sounds by name over the
// NitroSystem sound player (here SndDriver), with handles the game keeps.
//  - a handle is type << 28 | slot << 20 | serial (20 bits, never 0); types 1 sound effect (a sequence of a loaded
//    sequence archive, found by its name), 2 music (a sequence by name), 5 wave (not used by the race);
//  - 32 slots of 0x1c bytes (+0x08 handle, +0x0c NNS handle, +0x10 the track volume set, +0x11 the handle's volume,
//    +0x12 the pan set, +0x13 the handle's pan, +0x14 bit 0 paused / bit 1 stop pending, +0x18 the start frame);
//  - the sound-effect and music volumes (+0xb0, +0xb4) scale the handles' volumes: track volume =
//    (master x volume + 64) >> 7;
//  - loading a sequence archive by name (+0x14) makes its sequences playable by their names, until the matching
//    heap state is popped (+0x18, +0x1c). On the DS that also loads the data into the sound heap; the port keeps the
//    whole archive in memory (SndSdat) and keeps only the name lists.
#pragma once

#include <string>
#include <vector>

#include "nitro.h"
#include "snd_driver.h"
#include "snd_sdat.h"

class dsSoundManager {
public:
    enum { TYPE_SE = 1, TYPE_MUSIC = 2, TYPE_WAVE = 5, SLOTS = 32 };

    static dsSoundManager *Get();       // *0x020c8e88, created with the archive on first use
    static void Destroy();

    bool LoadArchive(const char *path); // +0x08: audio/sound_data.sdat (false: the manager stays silent)
    void Unload();                      // +0x10
    void LoadByName(const char *name);  // +0x14: a sequence archive or a sequence
    void PopState();                    // +0x18
    void PopAllStates();                // +0x1c
    u32 PlaySe(const char *name);       // +0x20
    u32 PlayMusic(const char *name, fx32 fadeIn); // +0x28
    u32 Stop(u32 h, fx32 fadeSeconds);  // +0x30, returns InvalidHandle()
    void StopAll(fx32 fadeSeconds);     // +0x34
    void PauseAll();                    // +0x38
    void ResumeAll();                   // +0x3c
    void SetPan(u32 h, int pan);        // +0x40, -128..127
    void SetVolume(u32 h, int volume);  // +0x44, 0..127
    void SetPitch(u32 h, int pitch);    // +0x48
    void SetTempo(u32 h, int percent);  // +0x4c
    bool IsPlaying(u32 h);              // +0x50
    bool IsValid(u32 h) const;          // +0x54
    u32 InvalidHandle() const { return m_invalid; } // +0x58
    void SetOutputMode(int mode) { m_outputMode = mode; } // +0x5c (mono / stereo / surround: stereo only here)
    void SetSeVolume(int v);            // +0x64
    int SeVolume() const { return m_seVolume; }       // +0x68
    void SetMusicVolume(int v);         // +0x6c
    int MusicVolume() const { return m_musicVolume; } // +0x70
    void Update();                      // +0x74, once per game frame

    const SndSdat &Sdat() const { return m_sdat; }
    bool Loaded() const { return m_loaded; }

private:
    struct Slot {
        u32 handle = 0;
        SndHandle nns;
        s8 trackVolume = (s8)0x80, volume = 0x7f, trackPan = (s8)0x80, pan = 0;
        bool used = false, paused = false, stopPending = false;
        u32 startFrame = 0;
    };
    struct SeEntry {
        std::string name;
        int arc, index, level;
    };

    Slot *AllocSlot(int type);          // 0x0205d31c
    void FreeSlot(Slot &s);
    Slot *Find(u32 h);
    void ApplyVolume(Slot &s);          // 0x0205d4c0

    SndSdat m_sdat;
    bool m_loaded = false;
    Slot m_slots[SLOTS];
    std::vector<SeEntry> m_se;          // +0xe8
    int m_level = 0;                    // +0xa8 sound heap state
    u32 m_serial = 1;                   // +0xac
    int m_seVolume = 0x7f, m_musicVolume = 0x7f; // +0xb0, +0xb4
    u32 m_invalid = 0;                  // +0xc0
    u32 m_frame = 0;
    bool m_paused = false;
    int m_outputMode = 1;
};
