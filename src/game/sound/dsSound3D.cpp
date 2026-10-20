#include "sound/dsSound3D.h"

#include <math.h>

#include "math/dsMath.h"
#include "sound/dsSoundManager.h"

static dsSound3D *s_sound3d;

// DTCM 0x027e0130: volume of the pan law by |cos| x 63 (1.0 at the front, 0.707 at the side).
static const u16 kPanVolume[65] = {
    4096, 4082, 4069, 4054, 4040, 4025, 4010, 3996, 3979, 3964, 3948, 3934, 3919, 3903, 3886, 3870, 3853,
    3837, 3820, 3803, 3786, 3769, 3751, 3733, 3715, 3698, 3679, 3661, 3643, 3624, 3606, 3587, 3568, 3549,
    3530, 3511, 3490, 3471, 3452, 3431, 3411, 3391, 3371, 3349, 3330, 3309, 3289, 3267, 3247, 3226, 3205,
    3183, 3161, 3140, 3118, 3096, 3074, 3053, 3031, 3008, 2986, 2963, 2942, 2919, 2896};
// DTCM 0x027e00ec: pan / 2 by |cos| x 63.
static const s8 kPan[65] = {0,  1,  2,  2,  3,  4,  5,  6,  6,  7,  8,  9,  9,  10, 11, 12, 13,
                            14, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 26, 27,
                            28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 42, 43, 44, 45,
                            46, 47, 49, 50, 51, 53, 54, 55, 57, 58, 59, 61, 62, 63};

// MTX_MultVec43 (row vector times a 4 x 3 matrix).
static VecFx32 MulMtx43(const VecFx32 &v, const MtxFx43 &m)
{
    VecFx32 r;
    r.x = (fx32)(((s64)v.x * m.m[0][0] + (s64)v.y * m.m[1][0] + (s64)v.z * m.m[2][0]) >> 12) + m.m[3][0];
    r.y = (fx32)(((s64)v.x * m.m[0][1] + (s64)v.y * m.m[1][1] + (s64)v.z * m.m[2][1]) >> 12) + m.m[3][1];
    r.z = (fx32)(((s64)v.x * m.m[0][2] + (s64)v.y * m.m[1][2] + (s64)v.z * m.m[2][2]) >> 12) + m.m[3][2];
    return r;
}

// FX_Atan2Idx: the angle of (x, y) as a 16-bit index (port approximation through atan2: only its top 12 bits are
// used, through the sine table).
static u16 Atan2Idx(fx32 y, fx32 x)
{
    if (x == 0 && y == 0)
        return 0;
    return (u16)lround(atan2((double)y, (double)x) * (32768.0 / 3.14159265358979323846));
}

dsSound3D *dsSound3D::Get()
{
    if (!s_sound3d)
        s_sound3d = new dsSound3D;
    return s_sound3d;
}

void dsSound3D::Destroy()
{
    if (s_sound3d)
        s_sound3d->ReleaseAll();
    delete s_sound3d;
    s_sound3d = nullptr;
}

// @port main:0205fe40 done: the listener matrix is the camera matrix's inverse (transposed rotation, -t R^T).
void dsSound3D::SetListener(const MtxFx43 &c)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m_listener.m[i][j] = c.m[j][i];
    fx32 tx = -c.m[3][0], ty = -c.m[3][1], tz = -c.m[3][2];
    for (int j = 0; j < 3; j++)
        m_listener.m[3][j] =
            (fx32)(((s64)tz * m_listener.m[2][j] + (s64)tx * m_listener.m[0][j] + (s64)ty * m_listener.m[1][j]) >> 12);
}

u32 dsSound3D::InvalidHandle() const { return dsSoundManager::Get()->InvalidHandle(); }

dsSound3D::Source *dsSound3D::Find(u32 h)
{
    if (h >> 29 != 3)
        return nullptr;
    u32 slot = (h >> 21) & 0xff;
    return slot < SOURCES && m_src[slot].used && m_src[slot].handle == h ? &m_src[slot] : nullptr;
}

// @port main:0205fa38 done
bool dsSound3D::IsValid(u32 h) const
{
    if (h >> 29 != 3)
        return dsSoundManager::Get()->IsValid(h);
    u32 slot = (h >> 21) & 0xff;
    return slot < SOURCES && m_src[slot].used && m_src[slot].handle == h;
}

// @port main:0205fc9c done (also +0x18 0x0205fc54, the position by pointer)
// A one-shot source already out of range is not created.
u32 dsSound3D::Create(const char *name, const VecFx32 &pos, int minDist, int maxDist, bool loop)
{
    Source *s = nullptr;
    int slot = 0;
    for (; slot < SOURCES; slot++)
        if (!m_src[slot].used) {
            s = &m_src[slot];
            break;
        }
    if (!s)
        return InvalidHandle();
    VecFx32 rel = MulMtx43(pos, m_listener);
    if (!loop && maxDist <= dsVec_Mag(&rel) >> 12)
        return InvalidHandle();
    *s = Source();
    s->used = true;
    s->handle = 3u << 29 | (u32)slot << 21 | (m_serial & 0x1fffff);
    m_serial = (m_serial + 1) & 0x1fffff;
    if (m_serial == 0)
        m_serial = 1;
    s->pos = pos;
    s->name = name;
    s->minDist = minDist;
    s->maxDist = maxDist;
    s->sound = InvalidHandle();
    s->loop = loop;
    s->volume = 0x7f;
    s->fresh = true;
    s->heardVolume = 0;
    s->invRange = maxDist == minDist ? 0x40000000 : (s32)(0x40000000 / (maxDist - minDist));
    return s->handle;
}

// @port main:0205f994 done
u32 dsSound3D::Release(u32 h, fx32 fadeSeconds)
{
    Source *s = Find(h);
    if (!s)
        return InvalidHandle();
    dsSoundManager *sm = dsSoundManager::Get();
    if (sm->IsValid(s->sound)) {
        s->sound = sm->Stop(s->sound, fadeSeconds);
        m_playing--;
    }
    s->used = false;
    return InvalidHandle();
}

// @port main:0205f870 done
void dsSound3D::ReleaseAll()
{
    dsSoundManager *sm = dsSoundManager::Get();
    for (Source &s : m_src)
        if (s.used) {
            if (sm->IsValid(s.sound))
                s.sound = sm->Stop(s.sound, 0);
            s.used = false;
        }
    m_playing = 0;
}

// @port main:0205fab8 done / main:0205fa88 done
void dsSound3D::SetPosition(u32 h, const VecFx32 &pos)
{
    u32 slot = (h >> 21) & 0xff;
    if (slot < SOURCES)
        m_src[slot].pos = pos;
}

// @port main:0205f944 done
void dsSound3D::SetVolume(u32 h, int volume)
{
    if (h >> 29 != 3) {
        dsSoundManager::Get()->SetVolume(h, volume);
        return;
    }
    u32 slot = (h >> 21) & 0xff;
    if (slot < SOURCES)
        m_src[slot].volume = (s8)(volume < 0 ? 0 : volume > 0x7f ? 0x7f : volume);
}

// @port main:0205f914 done
void dsSound3D::SetPitch(u32 h, int pitch)
{
    u32 slot = (h >> 21) & 0xff;
    if (h >> 29 == 3 && slot < SOURCES)
        dsSoundManager::Get()->SetPitch(m_src[slot].sound, pitch);
}

// @port main:02060118 done
void dsSound3D::Compute()
{
    dsSoundManager *sm = dsSoundManager::Get();
    for (Source &s : m_src) {
        if (!s.used)
            continue;
        if (!s.loop && !s.fresh && !sm->IsPlaying(s.sound)) {   // a one-shot sound that has ended
            Release(s.handle, 0);
            continue;
        }
        VecFx32 rel = MulMtx43(s.pos, m_listener);
        int d = dsVec_Mag(&rel) >> 12;
        int vol = s.volume;
        if (s.minDist < d) {
            if (d < s.maxDist) {
                s64 k = (s64)vol * (d - s.minDist) * s.invRange;
                vol -= (int)(k >> 30);
            } else
                vol = 0;
        }
        bool release = false, stop = false, play = false;
        if (!s.loop) {
            if (d < s.maxDist)
                play = true;
            else
                release = true;
        } else if (!sm->IsValid(s.sound)) {
            play = d < s.maxDist;
        } else if (!sm->IsPlaying(s.sound) || s.maxDist <= d) {
            stop = true;
        } else
            play = true;
        if (release)
            Release(s.handle, 0);
        else if (stop) {
            s.sound = sm->Stop(s.sound, 0);
            s.heardVolume = 0;
            s.fresh = false;
            m_playing--;
        } else if (play) {
            fx32 az = rel.z < 0 ? -rel.z : rel.z;
            u16 a = Atan2Idx(az, -rel.x);
            int c = FX_SinCosTable_[(a >> 4) * 2 + 1] * 0x7f;   // cos x 127, fx32
            int ci = c >> 12, idx = ci < 0 ? -ci >> 1 : c >> 13;
            s64 v = (s64)(vol << 12) * kPanVolume[idx] + 0x800;
            s.heardVolume = (s8)((s32)(v >> 12) >> 12);
            s.heardPan = (s8)((ci < 0 ? -kPan[idx] : kPan[idx]) << 1);
            s.fresh = false;
        }
    }
}

// @port main:0205ffb4 partial (when 8 sources play, a new one waits instead of replacing the quietest)
void dsSound3D::StartStop()
{
    dsSoundManager *sm = dsSoundManager::Get();
    // 0x0206054c: the sources in decreasing heard volume (a stable merge sort).
    int order[SOURCES], n = 0;
    for (int i = 0; i < SOURCES; i++)
        if (m_src[i].used)
            order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && m_src[order[j]].heardVolume > m_src[order[j - 1]].heardVolume; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int k = 0; k < n; k++) {
        Source &s = m_src[order[k]];
        if (!s.used)
            continue;
        u32 old = s.sound;
        if (s.heardVolume > 0 && !sm->IsPlaying(s.sound) && m_playing < MAX_PLAYING) {
            s.sound = sm->PlaySe(s.name.c_str());
            if (sm->IsValid(s.sound) && old == InvalidHandle())
                m_playing++;
        }
        if (sm->IsPlaying(s.sound)) {
            sm->SetPan(s.sound, s.heardPan);
            sm->SetVolume(s.sound, s.heardVolume);
        }
    }
}

// @port main:0206032c done
void dsSound3D::Update()
{
    if (m_paused)
        return;
    Compute();
    StartStop();
}
