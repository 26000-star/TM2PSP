// Ghost cars (see dsGhost.h).
#include "dsGhost.h"

#include <math.h>
#include <string.h>

namespace {

u32 Rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
s16 Rd16(const u8 *p) { return (s16)(p[0] | p[1] << 8); }
void Put32(std::vector<u8> &v, u32 x)
{
    for (int i = 0; i < 4; i++)
        v.push_back((u8)(x >> (8 * i)));
}
void Put16(std::vector<u8> &v, s32 x)
{
    v.push_back((u8)x);
    v.push_back((u8)(x >> 8));
}

void Normalize(fx32 *v)
{
    float x = (float)v[0], y = (float)v[1], z = (float)v[2], l = sqrtf(x * x + y * y + z * z);
    if (l <= 0) {
        v[0] = v[1] = 0;
        v[2] = FX32_ONE;
        return;
    }
    for (int i = 0; i < 3; i++)
        v[i] = (fx32)lrintf(v[i] * 4096.0f / l);
}

} // namespace

bool dsGhost::Load(const std::vector<u8> &d)
{
    m_samples.clear();
    splits.clear();
    if (d.size() < 0x18 || d[0] != 1 || Rd32(&d[4]) != 6)
        return false;
    finishTime = (fx32)Rd32(&d[8]);
    u32 size = Rd32(&d[0xc]);
    laps = (int)Rd32(&d[0x10]);
    checkpoints = (int)Rd32(&d[0x14]);
    size_t p = 0x18;
    if (laps < 0 || checkpoints < 0 || laps * checkpoints > 256 || p + 4u * laps * checkpoints > d.size())
        return false;
    for (int i = 0; i < laps * checkpoints; i++, p += 4)
        splits.push_back((fx32)Rd32(&d[p]));
    if (p + size > d.size())
        return false;
    const u8 *s = &d[p], *end = s + size;
    Sample cur = {};
    int tick = 0;
    while (s < end) {
        u8 f = *s++;
        size_t need = (f & 1 ? 6 : 0) + (f & 2 ? 6 : 0) + (f & 8 ? 4 : f & 0x10 ? 12 : 0) + (f & 0x80 ? 1 : 0) +
                      (f & 0x20 ? 1 : 0) + (f & 4 ? 1 : 0) + (f & 0x40 ? 4 : 0);
        if ((size_t)(end - s) < need)
            break;
        if (f & 1) {
            for (int k = 0; k < 3; k++)
                cur.up[k] += Rd16(s + 2 * k);
            s += 6;
        }
        if (f & 2) {
            for (int k = 0; k < 3; k++)
                cur.fwd[k] += Rd16(s + 2 * k);
            s += 6;
        }
        if (f & 8) {
            cur.pos[0] += Rd16(s);
            cur.pos[2] += Rd16(s + 2);
            s += 4;
        } else if (f & 0x10) {
            for (int k = 0; k < 3; k++)
                cur.pos[k] = (fx32)Rd32(s + 4 * k);
            s += 12;
        }
        cur.length = f & 0x80 ? *s++ : 1;
        s += (f & 0x20 ? 1 : 0) + (f & 4 ? 1 : 0) + (f & 0x40 ? 4 : 0); // steering, other and flag bytes: unused
        cur.tick = tick;
        tick += cur.length;
        m_samples.push_back(cur);
    }
    return !m_samples.empty();
}

bool dsGhost::Matrix(int tick, MtxFx43 *out) const
{
    if (m_samples.empty())
        return false;
    // The record whose ticks hold `tick`: reached from the previous one over its length.
    size_t lo = 0, hi = m_samples.size() - 1;
    if (tick < 0)
        tick = 0;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (m_samples[mid].tick <= tick)
            lo = mid;
        else
            hi = mid - 1;
    }
    const Sample &b = m_samples[lo];
    const Sample &a = lo ? m_samples[lo - 1] : b;
    fx32 t = b.length > 1 ? (fx32)(((tick - b.tick + 1) << 12) / b.length) : FX32_ONE;
    if (t > FX32_ONE)
        t = FX32_ONE;
    fx32 up[3], fwd[3];
    for (int k = 0; k < 3; k++) {
        up[k] = b.up[k];
        fwd[k] = b.fwd[k];
        out->m[3][k] = a.pos[k] + (fx32)(((s64)(b.pos[k] - a.pos[k]) * t) >> 12);
    }
    Normalize(up);
    Normalize(fwd);
    fx32 side[3] = {(fx32)(((s64)up[1] * fwd[2] - (s64)up[2] * fwd[1]) >> 12),
                    (fx32)(((s64)up[2] * fwd[0] - (s64)up[0] * fwd[2]) >> 12),
                    (fx32)(((s64)up[0] * fwd[1] - (s64)up[1] * fwd[0]) >> 12)};
    Normalize(side);
    for (int k = 0; k < 3; k++) {
        out->m[0][k] = side[k];
        out->m[1][k] = up[k];
        out->m[2][k] = fwd[k];
    }
    return true;
}

void dsGhost::BeginRecord()
{
    m_stream.clear();
    memset(m_recUp, 0, sizeof m_recUp);
    memset(m_recFwd, 0, sizeof m_recFwd);
    m_recording = true;
}

void dsGhost::Record(const MtxFx43 &m)
{
    if (!m_recording)
        return;
    u8 f = 0x80 | 0x10;
    s32 du[3], df[3];
    for (int k = 0; k < 3; k++) {
        du[k] = m.m[1][k] - m_recUp[k];
        df[k] = m.m[2][k] - m_recFwd[k];
        if (du[k])
            f |= 1;
        if (df[k])
            f |= 2;
    }
    m_stream.push_back(f);
    for (int k = 0; f & 1 && k < 3; k++) {
        Put16(m_stream, du[k]);
        m_recUp[k] = m.m[1][k];
    }
    for (int k = 0; f & 2 && k < 3; k++) {
        Put16(m_stream, df[k]);
        m_recFwd[k] = m.m[2][k];
    }
    for (int k = 0; k < 3; k++)
        Put32(m_stream, (u32)m.m[3][k]);
    m_stream.push_back(2); // two ticks per 30 Hz frame
}

std::vector<u8> dsGhost::Save(fx32 finish, const std::vector<fx32> &splitTimes, int lapCount,
                              int checkpointCount) const
{
    std::vector<u8> v = {1, 0, TICKS_PER_SECOND, 0};
    Put32(v, 6);
    Put32(v, (u32)finish);
    Put32(v, (u32)m_stream.size());
    int n = (int)splitTimes.size();
    if (lapCount * checkpointCount != n) {
        lapCount = 1;
        checkpointCount = n;
    }
    Put32(v, (u32)lapCount);
    Put32(v, (u32)checkpointCount);
    for (fx32 t : splitTimes)
        Put32(v, (u32)t);
    v.insert(v.end(), m_stream.begin(), m_stream.end());
    return v;
}
