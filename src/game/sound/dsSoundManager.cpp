#include "sound/dsSoundManager.h"

#include <ctype.h>
#include <string.h>

static dsSoundManager *s_manager;

// The game compares string ids, which ignore case.
static bool SameName(const std::string &a, const char *b)
{
    size_t n = strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return false;
    return true;
}

static int FindName(const std::vector<std::string> &names, const char *name)
{
    for (size_t i = 0; i < names.size(); i++)
        if (SameName(names[i], name))
            return (int)i;
    return -1;
}

// fx32 seconds -> 60 Hz frames: (s * 60 << 12 + 0x800) >> 24 (main 0x0205d898, 0x0205da38).
static int FadeFrames(fx32 seconds) { return (int)(((s64)seconds * 0x3c000 + 0x800) >> 24); }

// @port main:0205ea98 done (the archive is loaded by LoadArchive)
dsSoundManager *dsSoundManager::Get()
{
    if (!s_manager)
        s_manager = new dsSoundManager;
    return s_manager;
}

// @port main:0205e9c0 done
void dsSoundManager::Destroy()
{
    if (s_manager)
        s_manager->Unload();
    delete s_manager;
    s_manager = nullptr;
}

// @port main:0205e42c partial (the whole archive is read into memory; no sound heap)
bool dsSoundManager::LoadArchive(const char *path)
{
    if (m_loaded)
        return true;
    m_loaded = m_sdat.Load(path);
    SndDriver::Init(m_loaded ? &m_sdat : nullptr);
    m_level = 0;
    m_se.clear();
    return m_loaded;
}

// @port main:0205e124 done
void dsSoundManager::Unload()
{
    StopAll(0);
    SndDriver::Init(nullptr);
    m_se.clear();
    m_level = 0;
    m_sdat.Clear();
    m_loaded = false;
}

// @port main:0205de70 partial / main:0205df58 partial (no sound heap: a sequence archive's sequence names become
// playable at a new heap level; music and wave archives need no list)
void dsSoundManager::LoadByName(const char *name)
{
    if (!m_loaded)
        return;
    int arc = FindName(m_sdat.names[SDAT_SEQARC], name);
    if (arc < 0)
        return;
    m_level++;   // NNS_SndHeapSaveState
    const std::vector<std::string> &subs = m_sdat.seqArcNames[arc];
    for (int i = 0; i < m_sdat.SeqArcCount(arc); i++)
        if (i < (int)subs.size() && !subs[i].empty())
            m_se.push_back({subs[i], arc, i, m_level});
}

// @port main:0205ddd0 done
void dsSoundManager::PopState()
{
    if (m_level == 0)
        return;
    for (size_t i = 0; i < m_se.size();)
        if (m_se[i].level == m_level)
            m_se.erase(m_se.begin() + (long)i);
        else
            i++;
    m_level--;
}

// @port main:0205ddb0 done
void dsSoundManager::PopAllStates()
{
    while (m_level > 0)
        PopState();
}

// @port main:0205d31c done
dsSoundManager::Slot *dsSoundManager::AllocSlot(int type)
{
    for (int i = 0; i < SLOTS; i++) {
        Slot &s = m_slots[i];
        if (s.used)
            continue;
        s = Slot();
        s.used = true;
        s.handle = (u32)type << 28 | (u32)i << 20 | (m_serial & 0xfffff);
        m_serial = (m_serial + 1) & 0xfffff;
        if (m_serial == 0)
            m_serial = 1;
        return &s;
    }
    return nullptr;
}

void dsSoundManager::FreeSlot(Slot &s)
{
    s.used = false;
    s.handle = 0;
    s.nns = SndHandle();
}

dsSoundManager::Slot *dsSoundManager::Find(u32 h)
{
    return IsValid(h) ? &m_slots[(h >> 20) & 0xff] : nullptr;
}

// @port main:0205d3fc done
bool dsSoundManager::IsValid(u32 h) const
{
    u32 type = h >> 28, slot = (h >> 20) & 0xff;
    if (!(type == 1 || type == 2 || type == 3 || type == 5) || slot >= SLOTS)
        return false;
    return m_slots[slot].used && m_slots[slot].handle == h;
}

// @port main:0205d45c done
bool dsSoundManager::IsPlaying(u32 h)
{
    Slot *s = Find(h);
    return s && SndDriver::IsActive(&s->nns);
}

// @port main:0205dba0 done
u32 dsSoundManager::PlaySe(const char *name)
{
    const SeEntry *e = nullptr;
    for (const SeEntry &x : m_se)
        if (SameName(x.name, name)) {
            e = &x;
            break;
        }
    if (!e)
        return m_invalid;
    Slot *s = AllocSlot(TYPE_SE);
    if (!s)
        return m_invalid;
    if (!SndDriver::StartSeqArc(&s->nns, e->arc, e->index)) {
        FreeSlot(*s);
        return m_invalid;
    }
    SetVolume(s->handle, 0x7f);
    s->startFrame = m_frame;
    return s->handle;
}

// @port main:0205da38 partial (the fade-in moves the fader towards 127, as the game; the player priority is not
// kept: the music player has one sequence)
u32 dsSoundManager::PlayMusic(const char *name, fx32 fadeIn)
{
    int seq = m_loaded ? FindName(m_sdat.names[SDAT_SEQ], name) : -1;
    if (seq < 0)
        return m_invalid;
    Slot *s = AllocSlot(TYPE_MUSIC);
    if (!s)
        return m_invalid;
    if (!SndDriver::StartSeq(&s->nns, seq)) {
        FreeSlot(*s);
        return m_invalid;
    }
    SetVolume(s->handle, 0x7f);
    if (fadeIn > 0)
        SndDriver::MoveVolume(&s->nns, 0x7f, FadeFrames(fadeIn));
    s->startFrame = m_frame;
    return s->handle;
}

// @port main:0205d898 done
// A sound younger than 8 frames is stopped later by Update (stop pending); otherwise its channels are cut at once
// (SND_StopUnlockedChannel) and the sequence stops, with a fade when one is given.
u32 dsSoundManager::Stop(u32 h, fx32 fadeSeconds)
{
    Slot *s = Find(h);
    if (!s)
        return m_invalid;
    if (m_frame - s->startFrame < 8) {
        s->stopPending = true;
        return m_invalid;
    }
    s->stopPending = false;
    s->startFrame = 0;
    SndDriver::CutChannels(&s->nns);
    SndDriver::Stop(&s->nns, FadeFrames(fadeSeconds));
    if (fadeSeconds == 0)
        FreeSlot(*s);
    return m_invalid;
}

// @port main:0205d7c4 done
void dsSoundManager::StopAll(fx32 fadeSeconds)
{
    for (Slot &s : m_slots)
        if (s.used) {
            SndDriver::Stop(&s.nns, FadeFrames(fadeSeconds));
            FreeSlot(s);
        }
}

// @port main:0205d76c done
void dsSoundManager::PauseAll()
{
    for (Slot &s : m_slots)
        if (s.used && !s.paused) {
            s.paused = true;
            SndDriver::Pause(&s.nns, true);
        }
    m_paused = true;
}

// @port main:0205d70c done
void dsSoundManager::ResumeAll()
{
    if (!m_paused)
        return;
    for (Slot &s : m_slots)
        if (s.used && s.paused) {
            s.paused = false;
            SndDriver::Pause(&s.nns, false);
        }
    m_paused = false;
}

// @port main:0205d4c0 done (type 5 not used)
void dsSoundManager::ApplyVolume(Slot &s)
{
    u32 type = s.handle >> 28;
    int master = type == TYPE_MUSIC ? m_musicVolume : m_seVolume;
    s8 v = (s8)(((master * s.volume + 0x40) * 0x20000) >> 24);
    if (v != s.trackVolume) {
        SndDriver::SetTrackVolume(&s.nns, 0xffff, v);
        s.trackVolume = v;
    }
}

// @port main:0205d620 done
void dsSoundManager::SetVolume(u32 h, int volume)
{
    Slot *s = Find(h);
    if (!s)
        return;
    s->volume = (s8)(volume < 0 ? 0 : volume > 0x7f ? 0x7f : volume);
    ApplyVolume(*s);
}

// @port main:0205d66c done (type 5 not used)
void dsSoundManager::SetPan(u32 h, int pan)
{
    Slot *s = Find(h);
    if (!s)
        return;
    s->pan = (s8)(pan < -0x80 ? -0x80 : pan > 0x7f ? 0x7f : pan);
    if (s->pan != s->trackPan) {
        SndDriver::SetTrackPan(&s->nns, 0xffff, s->pan);
        s->trackPan = s->pan;
    }
}

// @port main:0205d5b8 done (type 5 not used): music pitch in semitones / 64 x 64, sound effects in 1/64 semitone
void dsSoundManager::SetPitch(u32 h, int pitch)
{
    Slot *s = Find(h);
    if (!s)
        return;
    u32 type = h >> 28;
    if (type == TYPE_MUSIC)
        SndDriver::SetTrackPitch(&s->nns, 0xffff, pitch << 6);
    else if (type == TYPE_SE)
        SndDriver::SetTrackPitch(&s->nns, 0xffff, pitch);
}

// @port main:0205d564 done
void dsSoundManager::SetTempo(u32 h, int percent)
{
    Slot *s = Find(h);
    if (!s || (h >> 28) != TYPE_MUSIC)
        return;
    percent = percent < 1 ? 1 : percent > 500 ? 500 : percent;
    SndDriver::SetTempoRatio(&s->nns, (0xa3d8 * percent) >> 14);
}

// @port main:0205e570 done
void dsSoundManager::SetSeVolume(int v)
{
    m_seVolume = v < 0 ? 0 : v > 0x7f ? 0x7f : v;
    for (Slot &s : m_slots)
        if (s.used)
            ApplyVolume(s);
}

// @port main:0205e518 done
void dsSoundManager::SetMusicVolume(int v)
{
    m_musicVolume = v < 0 ? 0 : v > 0x7f ? 0x7f : v;
    for (Slot &s : m_slots)
        if (s.used)
            ApplyVolume(s);
}

// @port main:0205d1f8 done
// NNS_SndMain, then: pending stops of sounds 8 frames old, and the slots of finished sounds are freed.
void dsSoundManager::Update()
{
    SndDriver::Main();
    if (!m_loaded)
        return;
    m_frame++;
    for (Slot &s : m_slots) {
        if (!s.used)
            continue;
        if (s.stopPending && m_frame - s.startFrame > 7)
            Stop(s.handle, 0);
        else if (!s.paused && !SndDriver::IsActive(&s.nns))
            FreeSlot(s);
    }
}
