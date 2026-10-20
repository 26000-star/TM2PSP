// The DS sound driver on the CPU (see snd_driver.h). The structure follows the SDK: SND_seq (players, tracks, the
// sequence commands), SND_exChannel (channel allocation, envelope, sweep, LFO, the per-frame volume / pitch / pan),
// the hardware channels, then the NNS player layer on top.
#include "snd_driver.h"
#include "snd_sdat.h"

#include <math.h>
#include <string.h>

// arm7.bin 0xf268: SNDi_DecibelSquareTable (0.1 dB of (v / 127)^2)
static const s16 kDecibelSquare[128] = {
    -32768, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360, -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239, -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169, -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119, -116, -114, -111, -109, -106, -103, -101, -99, -96, -94, -91, -89, -87, -85, -82,
    -80, -78, -76, -74, -72, -70, -68, -66, -64, -62, -60, -58, -56, -54, -52, -50,
    -49, -47, -45, -43, -42, -40, -38, -36, -35, -33, -31, -30, -28, -27, -25, -23,
    -22, -20, -19, -17, -16, -14, -13, -11, -10, -8, -7, -6, -4, -3, -1, 0
};
// arm7.bin 0xf368 (= main 0x020b8500): SNDi_DecibelTable (0.1 dB of v / 127)
static const s16 kDecibel[128] = {
    -32768, -421, -361, -325, -300, -281, -265, -252, -240, -230, -221, -212, -205, -198, -192, -186,
    -180, -175, -170, -165, -161, -156, -152, -148, -145, -141, -138, -134, -131, -128, -125, -122,
    -120, -117, -114, -112, -110, -107, -105, -103, -100, -98, -96, -94, -92, -90, -88, -86,
    -85, -83, -81, -79, -78, -76, -74, -73, -71, -70, -68, -67, -65, -64, -62, -61,
    -60, -58, -57, -56, -54, -53, -52, -51, -49, -48, -47, -46, -45, -43, -42, -41,
    -40, -39, -38, -37, -36, -35, -34, -33, -32, -31, -30, -29, -28, -27, -26, -25,
    -24, -23, -23, -22, -21, -20, -19, -18, -17, -17, -16, -15, -14, -13, -12, -12,
    -11, -10, -9, -9, -8, -7, -6, -6, -5, -4, -3, -3, -2, -1, -1, 0
};

// arm7.bin 0xf47c: attack coefficients for attack 109..127.
static const u8 kAttackCoeff[19] = {0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143};
// arm7.bin 0xf46c: the channel allocation order.
static const u8 kAllocOrder[16] = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
// arm7.bin 0xf468: the volume divider shifts.
static const u8 kDivShift[4] = {0, 1, 2, 4};
// arm7.bin 0xf244: a quarter of the LFO sine.
static const s8 kLfoSin[33] = {0, 6, 12, 19, 25, 31, 37, 43, 49, 54, 60, 65, 71, 76, 81, 85, 90,
                               94, 98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127, 127};
static const s8 kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const u16 kAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

enum { kVolDbMin = -723, kTimerRate = 240, kMaxCall = 3 };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { LFO_PITCH, LFO_VOLUME, LFO_PAN };
enum { INST_INVALID, INST_PCM, INST_PSG, INST_NOISE, INST_DIRECTPCM, INST_DUMMY, INST_DRUM = 16, INST_KEYSPLIT = 17 };

namespace {

struct LfoParam {
    u8 target = LFO_PITCH, speed = 16, depth = 0, range = 1;
    u16 delay = 0;
};

struct Lfo {
    LfoParam param;
    u16 delayCounter = 0, counter = 0;
};

struct WaveParam {
    u8 format = 0, loop = 0;
    u16 rate = 0, timer = 0, loopStart = 0;
    u32 loopLen = 0;
};

struct InstParam {
    u16 wave[2];
    u8 rootKey, attack, decay, sustain, release, pan;
};

struct InstData {
    u8 type;
    InstParam param;
};

// SNDExChannel. The track's channel list is the next indices; track >= 0 stands for the SDK's callback.
struct ExChannel {
    u8 id = 0;
    bool active = false, start = false, autoSweep = true;
    u8 envStatus = 0, envAttack = 0, envSustain = 0;
    u16 envDecay = 0, envRelease = 0;
    s32 envAttenuation = 0;
    u8 priority = 0, midiKey = 60, rootKey = 60, velocity = 127, panRange = 127, pan = 64;
    s8 initPan = 0, userPan = 0;
    s16 userDecay = 0, userDecay2 = 0, userPitch = 0, sweepPitch = 0;
    s32 sweepLength = 0, sweepCounter = 0, length = 0;
    u16 volume = 0, timer = 0;
    Lfo lfo;
    WaveParam wave;
    const u8 *data = nullptr;
    int next = -1, track = -1;
    bool syncStart = false, syncStop = false;
};

struct Track {
    bool active = false, noteWait = true, muted = false, tie = false, noteFinishWait = false, portamento = false;
    bool cmp = true, hasChannelMask = false;
    u8 panRange = 127, volume = 127, expression = 127, bendRange = 2, priority = 64, portamentoKey = 60;
    u8 portamentoTime = 0, envAttack = 255, envDecay = 255, envSustain = 255, envRelease = 255;
    u16 program = 0, channelMask = 0xffff;
    s8 pitchBend = 0, pan = 0, extPan = 0, transpose = 0;
    s16 extFader = 0, extPitch = 0, sweepPitch = 0;
    LfoParam mod;
    s32 wait = 0;
    const u8 *base = nullptr, *cur = nullptr;
    const u8 *callStack[kMaxCall];
    u8 loopCount[kMaxCall];
    u8 callDepth = 0;
    int channels = -1;
};

struct Player {
    bool active = false, prepared = false, paused = false;
    u8 id = 0, prio = 64, volume = 127;
    s16 extFader = 0;
    u8 tracks[16];
    u16 tempo = 120, tempoRatio = 256, tempoCounter = kTimerRate;
    int bank = -1;
    s16 localVars[16];
};

// A hardware channel (SOUNDxCNT and the sample fetch).
struct HwChannel {
    bool active = false;
    u8 format = 0, repeat = 0, vol = 0, div = 0, pan = 64;
    const u8 *data = nullptr;
    u32 loopStart = 0, total = 0;   // in samples
    u32 pos = 0, frac = 0, step = 0;
    s32 sample = 0;                  // current output sample
    // ADPCM: the state after decoding sample `decoded`, and the state saved at the loop start.
    s32 pred = 0, index = 0, loopPred = 0, loopIndex = 0;
    u32 decoded = 0;
};

// NNS sequence player (NNSSndSeqPlayer) and sound player (NNSSndPlayer).
struct NnsFader {
    s32 origin = 0, target = 0, frames = 0, counter = 0;
    void Set(s32 t, s32 f)
    {
        origin = Get();
        target = t;
        frames = f;
        counter = 0;
    }
    void Update()
    {
        if (counter < frames)
            counter++;
    }
    s32 Get() const { return frames == 0 ? target : origin + (target - origin) * counter / frames; }
    bool Done() const { return counter >= frames; }
};

struct NnsSeq {
    bool active = false, stopping = false;
    u8 nnsPlayer = 0, prio = 0, snd = 0, infoVolume = 127, volume = 127;
    u16 gen = 0;
    s16 lastVol = 0x7fff;
    NnsFader fader;
};

struct NnsPlayer {
    u8 maxSeq = 16, volume = 127;
    u16 channelMask = 0;
    int count = 0;
};

const SndSdat *s_sdat;
ExChannel s_chn[SndDriver::kChannels];
HwChannel s_hw[SndDriver::kChannels];
Track s_trk[SndDriver::kTracks];
Player s_ply[SndDriver::kPlayers];
NnsSeq s_seq[SndDriver::kPlayers];
NnsPlayer s_nns[16];
s16 s_globalVars[16];
u32 s_lockedMask, s_weakLockedMask;
u32 s_random = 0x12345678;
u64 s_cycleAcc;
int s_toFrame;
u16 s_pitchTable[768];
u8 s_volumeTable[724];

// ---- SND_util ----

u16 CalcRandom()
{
    s_random = s_random * 1664525u + 1013904223u;
    return (u16)(s_random >> 16);
}

// SND_CalcTimer; the BIOS pitch table is 65536 (2^(i / 768) - 1).
u16 CalcTimer(int timer, int pitch)
{
    int octave = 0, p = -pitch;
    while (p < 0) {
        octave--;
        p += 768;
    }
    while (p >= 768) {
        octave++;
        p -= 768;
    }
    u64 r = (u64)(s_pitchTable[p] + 0x10000) * (u32)timer;
    int shift = octave - 16;
    if (shift <= 0)
        r >>= -shift;
    else if (shift < 32) {
        if (r & (~0ull << (32 - shift)))
            return 0xffff;
        r <<= shift;
    } else
        return 0xffff;
    if (r < 0x10)
        r = 0x10;
    else if (r > 0xffff)
        r = 0xffff;
    return (u16)r;
}

// SND_CalcChannelVolume; the BIOS volume table maps -72.3..0 dB onto the 7-bit volume of the divider band.
u16 CalcChannelVolume(int v)
{
    if (v < kVolDbMin)
        v = kVolDbMin;
    else if (v > 0)
        v = 0;
    int div = v < -240 ? 3 : v < -120 ? 2 : v < -60 ? 1 : 0;
    return (u16)(s_volumeTable[v - kVolDbMin] | div << 8);
}

s8 SinIdx(int x)
{
    if (x < 0x20)
        return kLfoSin[x];
    if (x < 0x40)
        return kLfoSin[0x40 - x];
    if (x < 0x60)
        return (s8)-kLfoSin[x - 0x40];
    return (s8)-kLfoSin[0x20 - (x - 0x60)];
}

// ---- hardware channels ----

u32 Rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

void HwSetTimer(HwChannel &h, u16 timer)
{
    // The channel plays at 16756991 / timer Hz; the step is in 1/65536 samples per output sample.
    h.step = (u32)(((u64)16756991 << 16) / ((u64)(timer ? timer : 1) * SndDriver::kRate));
}

void HwSetupPcm(int i, const u8 *data, int format, int repeat, int loopStart, int loopLen, int vol, int div,
                int timer, int pan)
{
    HwChannel &h = s_hw[i];
    h.format = (u8)format;
    h.repeat = (u8)repeat;
    h.vol = (u8)vol;
    h.div = (u8)div;
    h.pan = (u8)pan;
    h.data = data;
    int perWord = format == 0 ? 4 : format == 1 ? 2 : 8;
    if (format == 2) {   // the first word is the ADPCM header
        h.loopStart = loopStart > 0 ? (u32)(loopStart - 1) * 8 : 0;
        h.total = (u32)(loopStart + loopLen - 1) * 8;
        h.pred = (s16)(data[0] | data[1] << 8);
        h.index = data[2] > 88 ? 88 : data[2];
        h.loopPred = h.pred;
        h.loopIndex = h.index;
    } else {
        h.loopStart = (u32)loopStart * perWord;
        h.total = (u32)(loopStart + loopLen) * perWord;
    }
    h.pos = 0;
    h.frac = 0;
    h.decoded = 0;
    h.sample = 0;
    HwSetTimer(h, (u16)timer);
    h.active = h.total > 0;
}

void AdpcmStep(HwChannel &h, int nibble)
{
    int step = kAdpcmStep[h.index];
    int diff = step >> 3;
    if (nibble & 1)
        diff += step >> 2;
    if (nibble & 2)
        diff += step >> 1;
    if (nibble & 4)
        diff += step;
    if (nibble & 8) {
        h.pred -= diff;
        if (h.pred < -0x7fff)
            h.pred = -0x7fff;
    } else {
        h.pred += diff;
        if (h.pred > 0x7fff)
            h.pred = 0x7fff;
    }
    h.index += kAdpcmIndex[nibble & 7];
    if (h.index < 0)
        h.index = 0;
    else if (h.index > 88)
        h.index = 88;
}

// The sample at h.pos (decoding the ADPCM stream forward up to it).
inline s32 HwFetch(HwChannel &h)
{
    switch (h.format) {
    case 0:
        return (s8)h.data[h.pos] << 8;
    case 1:
        return (s16)(h.data[2 * h.pos] | h.data[2 * h.pos + 1] << 8);
    default: {
        const u8 *d = h.data + 4;
        while (h.decoded <= h.pos) {
            if (h.decoded == h.loopStart) {
                h.loopPred = h.pred;
                h.loopIndex = h.index;
            }
            u8 b = d[h.decoded >> 1];
            AdpcmStep(h, h.decoded & 1 ? b >> 4 : b & 15);
            h.decoded++;
        }
        return h.pred;
    }
    }
}

// Mixes n samples of one channel into the 32-bit stereo accumulators.
void HwMix(HwChannel &h, s32 *acc, int n)
{
    int shift = 14 + kDivShift[h.div & 3];
    s32 gl = h.vol * (128 - h.pan), gr = h.vol * h.pan;
    for (int i = 0; i < n; i++) {
        s32 s = HwFetch(h);
        acc[2 * i] += (s * gl) >> shift;
        acc[2 * i + 1] += (s * gr) >> shift;
        h.frac += h.step;
        h.pos += h.frac >> 16;
        h.frac &= 0xffff;
        if (h.pos >= h.total) {
            if (h.repeat != 1 || h.total <= h.loopStart) {
                h.active = false;
                return;
            }
            u32 len = h.total - h.loopStart;
            h.pos = h.loopStart + (h.pos - h.total) % len;
            if (h.format == 2) {   // back to the state saved at the loop start
                h.pred = h.loopPred;
                h.index = h.loopIndex;
                h.decoded = h.loopStart;
            }
        }
    }
}

// ---- SND_exChannel ----

u16 CalcDecayCoeff(int v)
{
    if (v == 127)
        return 0xffff;
    if (v == 126)
        return 0x3c00;
    if (v < 50)
        return (u16)(v * 2 + 1);
    return (u16)(0x1e00 / (126 - v));
}

void SetAttack(ExChannel &c, int a) { c.envAttack = a < 109 ? (u8)(255 - a) : kAttackCoeff[127 - a]; }
void SetDecay(ExChannel &c, int d) { c.envDecay = CalcDecayCoeff(d); }
void SetSustain(ExChannel &c, int s) { c.envSustain = (u8)s; }
void SetRelease(ExChannel &c, int r) { c.envRelease = CalcDecayCoeff(r); }

void InitLfoParam(LfoParam &p) { p = LfoParam(); }

void ChannelCallback(ExChannel &c, int status);

void FreeChannel(ExChannel &c) { c.track = -1; }

void ExChannelSetup(ExChannel &c, int track, int priority)
{
    c.next = -1;
    c.track = track;
    c.length = 0;
    c.priority = (u8)priority;
    c.volume = 127;
    c.start = false;
    c.autoSweep = true;
    c.midiKey = 60;
    c.rootKey = 60;
    c.velocity = 127;
    c.initPan = 0;
    c.userDecay = 0;
    c.userDecay2 = 0;
    c.userPitch = 0;
    c.userPan = 0;
    c.panRange = 127;
    c.sweepPitch = 0;
    c.sweepLength = 0;
    c.sweepCounter = 0;
    SetAttack(c, 127);
    SetDecay(c, 127);
    SetSustain(c, 127);
    SetRelease(c, 127);
    InitLfoParam(c.lfo.param);
}

int VolumeCmp(const ExChannel &a, const ExChannel &b)
{
    int va = ((a.volume & 0xff) << 4) >> kDivShift[a.volume >> 8 & 3];
    int vb = ((b.volume & 0xff) << 4) >> kDivShift[b.volume >> 8 & 3];
    if (va != vb)
        return va < vb ? 1 : -1;
    return 0;
}

ExChannel *AllocChannel(u32 mask, int priority, bool strong, int track)
{
    mask &= ~s_lockedMask;
    if (!strong)
        mask &= ~s_weakLockedMask;
    ExChannel *best = nullptr;
    for (int i = 0; i < SndDriver::kChannels; i++) {
        int cand = kAllocOrder[i];
        if (!(mask & 1u << cand))
            continue;
        ExChannel *c = &s_chn[cand];
        if (!best) {
            best = c;
            continue;
        }
        if (c->priority > best->priority)
            continue;
        if (c->priority != best->priority || VolumeCmp(*best, *c) < 0)
            best = c;
    }
    if (!best || priority < best->priority)
        return nullptr;
    if (best->track >= 0)
        ChannelCallback(*best, 0);
    best->syncStop = true;
    best->syncStart = false;
    best->active = false;
    ExChannelSetup(*best, track, priority);
    return best;
}

void ExChannelStart(ExChannel &c, int length)
{
    c.envAttenuation = -92544;
    c.envStatus = ENV_ATTACK;
    c.length = length;
    c.lfo.counter = 0;
    c.lfo.delayCounter = 0;
    c.start = true;
    c.active = true;
}

s32 UpdateEnvelope(ExChannel &c, bool step)
{
    if (step) {
        switch (c.envStatus) {
        case ENV_ATTACK:
            c.envAttenuation = -((-c.envAttenuation * c.envAttack) >> 8);
            if (c.envAttenuation == 0)
                c.envStatus = ENV_DECAY;
            break;
        case ENV_DECAY: {
            s32 sustain = kDecibelSquare[c.envSustain] << 7;
            c.envAttenuation -= c.envDecay;
            if (c.envAttenuation <= sustain) {
                c.envAttenuation = sustain;
                c.envStatus = ENV_SUSTAIN;
            }
            break;
        }
        case ENV_RELEASE:
            c.envAttenuation -= c.envRelease;
            break;
        }
    }
    return c.envAttenuation >> 7;
}

int SweepUpdate(ExChannel &c, bool step)
{
    if (c.sweepPitch == 0 || c.sweepCounter >= c.sweepLength)
        return 0;
    s64 r = (s64)c.sweepPitch * (c.sweepLength - c.sweepCounter) / c.sweepLength;
    if (step && c.autoSweep)
        c.sweepCounter++;
    return (int)r;
}

int LfoValue(const Lfo &l)
{
    if (l.param.depth == 0 || l.delayCounter < l.param.delay)
        return 0;
    return SinIdx(l.counter >> 8) * l.param.depth * l.param.range;
}

void UpdateLfo(Lfo &l)
{
    if (l.delayCounter < l.param.delay) {
        l.delayCounter++;
        return;
    }
    u32 tmp = (l.counter + (l.param.speed << 6)) >> 8;
    while (tmp >= 0x80)
        tmp -= 0x80;
    l.counter = (u16)(((l.counter + (l.param.speed << 6)) & 0xff) | tmp << 8);
}

int LfoUpdate(ExChannel &c, bool step)
{
    s64 r = LfoValue(c.lfo);
    if (r != 0) {
        r = c.lfo.param.target == LFO_VOLUME ? r * 60 : r << 6;
        r >>= 14;
    }
    if (step)
        UpdateLfo(c.lfo);
    return (int)r;
}

void StopHw(int i) { s_hw[i].active = false; }

// SND_UpdateExChannel: the channels' new state goes to the hardware.
void UpdateExChannels()
{
    for (int i = 0; i < SndDriver::kChannels; i++) {
        ExChannel &c = s_chn[i];
        if (c.syncStop)
            StopHw(i);
        if (c.syncStart && c.data)
            HwSetupPcm(i, c.data, c.wave.format, c.wave.loop ? 1 : 2, c.wave.loopStart, (int)c.wave.loopLen,
                       c.volume & 0xff, c.volume >> 8, c.timer, c.pan);
        else if (!c.syncStop || c.syncStart) {
            HwChannel &h = s_hw[i];
            if (h.active) {
                HwSetTimer(h, c.timer);
                h.vol = (u8)(c.volume & 0xff);
                h.div = (u8)(c.volume >> 8);
                h.pan = c.pan;
            }
        }
        c.syncStart = c.syncStop = false;
    }
}

void ExChannelMain(bool step)
{
    for (int i = 0; i < SndDriver::kChannels; i++) {
        ExChannel &c = s_chn[i];
        if (!c.active)
            continue;
        if (c.start) {
            c.syncStart = true;
            c.start = false;
        } else if (!s_hw[i].active) {
            if (c.track >= 0)
                ChannelCallback(c, 1);
            else
                c.priority = 0;
            c.volume = 0;
            c.active = false;
            continue;
        }
        s32 vol = kDecibelSquare[c.velocity];
        s32 pitch = (c.midiKey - c.rootKey) * 0x40;
        s32 pan = 0;
        vol += UpdateEnvelope(c, step);
        pitch += SweepUpdate(c, step);
        vol += c.userDecay;
        vol += c.userDecay2;
        pitch += c.userPitch;
        s32 lfo = LfoUpdate(c, step);
        switch (c.lfo.param.target) {
        case LFO_VOLUME:
            if (vol > -0x8000)
                vol += lfo;
            break;
        case LFO_PAN:
            pan += lfo;
            break;
        case LFO_PITCH:
            pitch += lfo;
            break;
        }
        pan += c.initPan;
        if (c.panRange != 127)
            pan = (pan * c.panRange + 0x40) >> 7;
        pan += c.userPan;
        if (c.envStatus == ENV_RELEASE && vol <= -723) {
            c.syncStop = true;
            c.syncStart = false;
            if (c.track >= 0)
                ChannelCallback(c, 1);
            else
                c.priority = 0;
            c.volume = 0;
            c.active = false;
        } else {
            c.volume = CalcChannelVolume(vol);
            c.timer = CalcTimer(c.wave.timer, pitch);
            pan += 0x40;
            c.pan = (u8)(pan < 0 ? 0 : pan > 127 ? 127 : pan);
        }
    }
}

// ---- SND_bank ----

bool ReadInstData(int bank, int program, int key, InstData *inst)
{
    const u8 *b = program >= 0 ? s_sdat->Bank(bank) : nullptr;
    if (!b || program >= (int)Rd32(b + 0x38))
        return false;
    u32 off = Rd32(b + 0x3c + 4 * program);
    const u8 *p;
    inst->type = (u8)(off & 0xff);
    switch (inst->type) {
    case INST_PCM:
    case INST_PSG:
    case INST_NOISE:
    case INST_DIRECTPCM:
    case INST_DUMMY:
        p = b + (off >> 8);
        break;
    case INST_DRUM: {
        const u8 *d = b + (off >> 8);
        if (key < d[0] || key > d[1])
            return false;
        p = d + 2 + 12 * (key - d[0]);
        inst->type = p[0];
        p += 2;
        break;
    }
    case INST_KEYSPLIT: {
        const u8 *k = b + (off >> 8);
        int i = 0;
        while (key > k[i])
            if (++i >= 8)
                return false;
        p = k + 8 + 12 * i;
        inst->type = p[0];
        p += 2;
        break;
    }
    default:
        return false;
    }
    inst->param.wave[0] = (u16)(p[0] | p[1] << 8);
    inst->param.wave[1] = (u16)(p[2] | p[3] << 8);
    inst->param.rootKey = p[4];
    inst->param.attack = p[5];
    inst->param.decay = p[6];
    inst->param.sustain = p[7];
    inst->param.release = p[8];
    inst->param.pan = p[9];
    return true;
}

bool NoteOn(ExChannel &c, int key, int velocity, int length, int bank, const InstData &inst)
{
    u8 release = inst.param.release;
    if (release == 0xff) {
        length = -1;
        release = 0;
    }
    if (inst.type != INST_PCM)   // PSG, noise and direct PCM: not used by this game's banks
        return false;
    int arcLink = inst.param.wave[1];
    if (arcLink > 3)
        return false;
    int arc = s_sdat->bankWaveArcs[arcLink][bank];
    const SndWave *w = arc != 0xffff ? s_sdat->Wave(arc, inst.param.wave[0]) : nullptr;
    if (!w)
        return false;
    const u8 *wp = (const u8 *)w;
    c.wave.format = wp[0];
    c.wave.loop = wp[1];
    c.wave.rate = (u16)(wp[2] | wp[3] << 8);
    c.wave.timer = (u16)(wp[4] | wp[5] << 8);
    c.wave.loopStart = (u16)(wp[6] | wp[7] << 8);
    c.wave.loopLen = Rd32(wp + 8);
    c.data = wp + 12;
    ExChannelStart(c, length);
    c.midiKey = (u8)key;
    c.rootKey = inst.param.rootKey;
    c.velocity = (u8)velocity;
    SetAttack(c, inst.param.attack);
    SetDecay(c, inst.param.decay);
    SetSustain(c, inst.param.sustain);
    SetRelease(c, release);
    c.initPan = (s8)(inst.param.pan - 0x40);
    return true;
}

// ---- SND_seq ----

Track *PlayerTrack(Player &p, int t)
{
    if (t > 15 || p.tracks[t] == 0xff)
        return nullptr;
    return &s_trk[p.tracks[t]];
}

void ChannelCallback(ExChannel &c, int status)
{
    Track &t = s_trk[c.track];
    int self = c.id;
    if (status == 1) {
        c.priority = 0;
        FreeChannel(c);
    }
    if (t.channels == self) {
        t.channels = c.next;
        return;
    }
    for (int cur = t.channels; cur >= 0 && s_chn[cur].next >= 0; cur = s_chn[cur].next)
        if (s_chn[cur].next == self) {
            s_chn[cur].next = c.next;
            return;
        }
}

void TrackUpdateChannel(Track &t, Player &p, bool release)
{
    int vol = kDecibelSquare[t.volume] + kDecibelSquare[t.expression] + kDecibelSquare[p.volume];
    int fader = t.extFader + p.extFader;
    int pitch = (t.pitchBend * (t.bendRange << 6)) >> 7;
    pitch += t.extPitch;
    int pan = t.pan;
    if (t.panRange != 127)
        pan = (pan * t.panRange + 0x40) >> 7;
    pan += t.extPan;
    if (vol < -0x8000)
        vol = -0x8000;
    if (fader < -0x8000)
        fader = -0x8000;
    pan = pan < -128 ? -128 : pan > 127 ? 127 : pan;
    for (int i = t.channels; i >= 0; i = s_chn[i].next) {
        ExChannel &c = s_chn[i];
        c.userDecay2 = (s16)fader;
        if (c.envStatus == ENV_RELEASE)
            continue;
        c.userDecay = (s16)vol;
        c.userPitch = (s16)pitch;
        c.userPan = (s8)pan;
        c.panRange = t.panRange;
        c.lfo.param = t.mod;
        if (c.length == 0 && release) {
            c.priority = 1;
            c.envStatus = ENV_RELEASE;
        }
    }
}

void TrackReleaseChannels(Track &t, Player &p, int release)
{
    TrackUpdateChannel(t, p, false);
    for (int i = t.channels; i >= 0; i = s_chn[i].next) {
        ExChannel &c = s_chn[i];
        if (!c.active)
            continue;
        if (release >= 0)
            SetRelease(c, release & 0xff);
        c.priority = 1;
        c.envStatus = ENV_RELEASE;
    }
}

void TrackFreeChannels(Track &t)
{
    for (int i = t.channels; i >= 0; i = s_chn[i].next)
        FreeChannel(s_chn[i]);
    t.channels = -1;
}

void TrackInit(Track &t)
{
    bool active = t.active;
    t = Track();
    t.active = active;
}

void TrackStop(Track &t, Player &p)
{
    TrackReleaseChannels(t, p, -1);
    TrackFreeChannels(t);
}

void PlayerStopTrack(Player &p, int i)
{
    Track *t = PlayerTrack(p, i);
    if (!t)
        return;
    TrackStop(*t, p);
    t->active = false;
    p.tracks[i] = 0xff;
}

void PlayerStop(Player &p)
{
    for (int i = 0; i < 16; i++)
        PlayerStopTrack(p, i);
    p.active = false;
}

int AllocateTrack()
{
    for (int i = 0; i < SndDriver::kTracks; i++)
        if (!s_trk[i].active) {
            s_trk[i].active = true;
            return i;
        }
    return -1;
}

u8 ReadU8(Track &t) { return *t.cur++; }

u16 ReadU16(Track &t)
{
    int v = ReadU8(t);
    return (u16)(v | ReadU8(t) << 8);
}

u32 ReadU24(Track &t)
{
    u32 v = ReadU8(t);
    v |= ReadU8(t) << 8;
    return v | ReadU8(t) << 16;
}

int ReadVlv(Track &t)
{
    int v = 0, b;
    do {
        b = ReadU8(t);
        v = (v << 7) | (b & 0x7f);
    } while (b & 0x80);
    return v;
}

s16 *Var(Player &p, int v) { return v < 16 ? &p.localVars[v] : v < 32 ? &s_globalVars[v - 16] : nullptr; }

enum { VAL_U8, VAL_U16, VAL_VLV, VAL_RAN, VAL_VAR };

int ParseValue(Track &t, Player &p, int type)
{
    switch (type) {
    case VAL_U8:
        return ReadU8(t);
    case VAL_U16:
        return ReadU16(t);
    case VAL_VLV:
        return ReadVlv(t);
    case VAL_VAR: {
        s16 *v = Var(p, ReadU8(t));
        return v ? *v : 0;
    }
    default: {   // VAL_RAN
        int lo = (s16)ReadU16(t);
        int hi = (s16)ReadU16(t);
        int ran = CalcRandom();
        return ((ran * (hi - lo + 1)) >> 16) + lo;
    }
    }
}

void TrackMute(Track &t, Player &p, int mode)
{
    switch (mode) {
    case 0:
        t.muted = false;
        break;
    case 1:
        t.muted = true;
        break;
    case 2:
        t.muted = true;
        TrackReleaseChannels(t, p, -1);
        break;
    case 3:
        t.muted = true;
        TrackReleaseChannels(t, p, 127);
        TrackFreeChannels(t);
        break;
    }
}

void TrackPlayNote(Track &t, Player &p, int ti, int key, int velocity, int length)
{
    ExChannel *c = nullptr;
    if (t.tie && t.channels >= 0) {
        c = &s_chn[t.channels];
        c->midiKey = (u8)key;
        c->velocity = (u8)velocity;
    }
    if (!c) {
        InstData inst;
        if (!ReadInstData(p.bank, t.program, key, &inst))
            return;
        u32 allowed;
        switch (inst.type) {
        case INST_PCM:
        case INST_DIRECTPCM:
            allowed = 0xffff;
            break;
        case INST_PSG:
            allowed = 0x3f00;
            break;
        case INST_NOISE:
            allowed = 0xc000;
            break;
        default:
            return;
        }
        allowed &= t.channelMask;
        c = AllocChannel(allowed, p.prio + t.priority, t.hasChannelMask, ti);
        if (!c)
            return;
        if (!NoteOn(*c, key, velocity, t.tie ? -1 : length, p.bank, inst)) {
            c->priority = 0;
            FreeChannel(*c);
            return;
        }
        c->next = t.channels;
        t.channels = c->id;
    }
    if (t.envAttack != 0xff)
        SetAttack(*c, t.envAttack);
    if (t.envDecay != 0xff)
        SetDecay(*c, t.envDecay);
    if (t.envSustain != 0xff)
        SetSustain(*c, t.envSustain);
    if (t.envRelease != 0xff)
        SetRelease(*c, t.envRelease);
    c->sweepPitch = t.sweepPitch;
    if (t.portamento)
        c->sweepPitch = (s16)(c->sweepPitch + ((t.portamentoKey - key) << 6));
    if (t.portamentoTime == 0) {
        c->sweepLength = length;
        c->autoSweep = false;
    } else {
        int swp = t.portamentoTime * t.portamentoTime;
        swp *= c->sweepPitch < 0 ? -c->sweepPitch : c->sweepPitch;
        c->sweepLength = swp >> 11;
    }
    c->sweepCounter = 0;
}

void TrackStart(Track &t, const u8 *base, u32 off)
{
    t.base = base;
    t.cur = base + off;
}

// TrackStepTicks: one tick of a track; -1 when the track ends.
int TrackStepTicks(Track &t, Player &p, int ti, bool playNotes)
{
    for (int i = t.channels; i >= 0; i = s_chn[i].next) {
        ExChannel &c = s_chn[i];
        if (c.length > 0)
            c.length--;
        if (!c.autoSweep && c.sweepCounter < c.sweepLength)
            c.sweepCounter++;
    }
    if (t.noteFinishWait) {
        if (t.channels >= 0)
            return 0;
        t.noteFinishWait = false;
    }
    if (t.wait > 0 && --t.wait > 0)
        return 0;
    while (t.wait == 0 && !t.noteFinishWait) {
        bool special = false, run = true;
        int vtype = VAL_U8;
        u8 cmd = ReadU8(t);
        if (cmd == 0xa2) {
            cmd = ReadU8(t);
            run = t.cmp;
        }
        if (cmd == 0xa0) {
            cmd = ReadU8(t);
            vtype = VAL_RAN;
            special = true;
        }
        if (cmd == 0xa1) {
            cmd = ReadU8(t);
            vtype = VAL_VAR;
            special = true;
        }
        if (!(cmd & 0x80)) {
            int vel = ReadU8(t);
            int len = ParseValue(t, p, special ? vtype : VAL_VLV);
            int key = cmd + t.transpose;
            if (!run)
                continue;
            key = key < 0 ? 0 : key > 127 ? 127 : key;
            if (!t.muted && playNotes)
                TrackPlayNote(t, p, ti, key, vel, len > 0 ? len : -1);
            t.portamentoKey = (u8)key;
            if (t.noteWait) {
                t.wait = len;
                if (len == 0)
                    t.noteFinishWait = true;
            }
            continue;
        }
        switch (cmd & 0xf0) {
        case 0x80: {
            int par = ParseValue(t, p, special ? vtype : VAL_VLV);
            if (!run)
                break;
            if (cmd == 0x80)
                t.wait = par;
            else if (cmd == 0x81 && par < 0x10000)
                t.program = (u16)par;
            break;
        }
        case 0x90:
            if (cmd == 0x93) {
                int tr = ReadU8(t);
                u32 off = ReadU24(t);
                Track *nt = run ? PlayerTrack(p, tr) : nullptr;
                if (nt && nt != &t) {
                    TrackStop(*nt, p);
                    TrackStart(*nt, t.base, off);
                }
            } else if (cmd == 0x94) {
                u32 off = ReadU24(t);
                if (run)
                    t.cur = t.base + off;
            } else if (cmd == 0x95) {
                u32 off = ReadU24(t);
                if (run && t.callDepth < kMaxCall) {
                    t.callStack[t.callDepth++] = t.cur;
                    t.cur = t.base + off;
                }
            }
            break;
        case 0xc0:
        case 0xd0: {
            u8 par = (u8)ParseValue(t, p, special ? vtype : VAL_U8);
            if (!run)
                break;
            switch (cmd) {
            case 0xc1: t.volume = par; break;
            case 0xd5: t.expression = par; break;
            case 0xc2: p.volume = par; break;
            case 0xc5: t.bendRange = par; break;
            case 0xc6: t.priority = par; break;
            case 0xc7: t.noteWait = par != 0; break;
            case 0xcf: t.portamentoTime = par; break;
            case 0xca: t.mod.depth = par; break;
            case 0xcb: t.mod.speed = par; break;
            case 0xcc: t.mod.target = par; break;
            case 0xcd: t.mod.range = par; break;
            case 0xd0: t.envAttack = par; break;
            case 0xd1: t.envDecay = par; break;
            case 0xd2: t.envSustain = par; break;
            case 0xd3: t.envRelease = par; break;
            case 0xd4:
                if (t.callDepth < kMaxCall) {
                    t.callStack[t.callDepth] = t.cur;
                    t.loopCount[t.callDepth] = par;
                    t.callDepth++;
                }
                break;
            case 0xc8:
                t.tie = par != 0;
                TrackReleaseChannels(t, p, -1);
                TrackFreeChannels(t);
                break;
            case 0xd7: TrackMute(t, p, par); break;
            case 0xc9:
                t.portamentoKey = (u8)(par + t.transpose);
                t.portamento = true;
                break;
            case 0xce: t.portamento = par != 0; break;
            case 0xc3: t.transpose = (s8)par; break;
            case 0xc4: t.pitchBend = (s8)par; break;
            case 0xc0: t.pan = (s8)(par - 0x40); break;
            }
            break;
        }
        case 0xe0: {
            s16 par = (s16)ParseValue(t, p, special ? vtype : VAL_U16);
            if (!run)
                break;
            if (cmd == 0xe3)
                t.sweepPitch = par;
            else if (cmd == 0xe1)
                p.tempo = (u16)par;
            else if (cmd == 0xe0)
                t.mod.delay = (u16)par;
            break;
        }
        case 0xb0: {
            int vn = ReadU8(t);
            s16 par = (s16)ParseValue(t, p, special ? vtype : VAL_U16);
            s16 *v = Var(p, vn);
            if (!run || !v)
                break;
            switch (cmd) {
            case 0xb0: *v = par; break;
            case 0xb1: *v = (s16)(*v + par); break;
            case 0xb2: *v = (s16)(*v - par); break;
            case 0xb3: *v = (s16)(*v * par); break;
            case 0xb4:
                if (par)
                    *v = (s16)(*v / par);
                break;
            case 0xb5: *v = (s16)(par >= 0 ? *v << par : *v >> -par); break;
            case 0xb6: {
                bool neg = par < 0;
                if (neg)
                    par = (s16)-par;
                s32 r = (CalcRandom() * (par + 1)) >> 16;
                *v = (s16)(neg ? -r : r);
                break;
            }
            case 0xb8: t.cmp = *v == par; break;
            case 0xb9: t.cmp = *v >= par; break;
            case 0xba: t.cmp = *v > par; break;
            case 0xbb: t.cmp = *v <= par; break;
            case 0xbc: t.cmp = *v < par; break;
            case 0xbd: t.cmp = *v != par; break;
            }
            break;
        }
        case 0xf0:
            if (!run)
                break;
            if (cmd == 0xfd) {
                if (t.callDepth)
                    t.cur = t.callStack[--t.callDepth];
            } else if (cmd == 0xfc) {
                if (!t.callDepth)
                    break;
                u8 n = t.loopCount[t.callDepth - 1];
                if (n && --n == 0) {
                    t.callDepth--;
                    break;
                }
                t.loopCount[t.callDepth - 1] = n;
                t.cur = t.callStack[t.callDepth - 1];
            } else if (cmd == 0xff)
                return -1;
            break;
        }
    }
    return 0;
}

bool PlayerStepTicks(Player &p, bool playNotes)
{
    bool playing = false;
    for (int i = 0; i < 16; i++) {
        Track *t = PlayerTrack(p, i);
        if (t && t->cur) {
            if (TrackStepTicks(*t, p, p.tracks[i], playNotes))
                PlayerStopTrack(p, i);
            else
                playing = true;
        }
    }
    return !playing;
}

void PlayerSeqMain(Player &p)
{
    int ticks = 0;
    while (p.tempoCounter >= kTimerRate) {
        p.tempoCounter -= kTimerRate;
        ticks++;
    }
    for (int i = 0; i < ticks; i++)
        if (PlayerStepTicks(p, true)) {
            PlayerStop(p);
            break;
        }
    p.tempoCounter = (u16)(p.tempoCounter + ((p.tempo * p.tempoRatio) >> 8));
}

void SeqMain(bool step)
{
    for (int i = 0; i < SndDriver::kPlayers; i++) {
        Player &p = s_ply[i];
        if (!p.active || !p.prepared)
            continue;
        if (step && !p.paused)
            PlayerSeqMain(p);
        for (int t = 0; t < 16; t++)
            if (Track *tr = PlayerTrack(p, t))
                TrackUpdateChannel(*tr, p, true);
    }
}

void PrepareSeq(int pi, const u8 *seq, u32 offset, int bank)
{
    Player &p = s_ply[pi];
    if (p.active)
        PlayerStop(p);
    p.paused = false;
    p.bank = bank;
    p.tempo = 120;
    p.tempoRatio = 256;
    p.tempoCounter = kTimerRate;
    p.volume = 127;
    p.extFader = 0;
    p.prio = 64;
    memset(p.tracks, 0xff, sizeof(p.tracks));
    for (int i = 0; i < 16; i++)
        p.localVars[i] = -1;
    int ti = AllocateTrack();
    if (ti < 0)
        return;
    Track &t = s_trk[ti];
    TrackInit(t);
    TrackStart(t, seq, offset);
    p.tracks[0] = (u8)ti;
    if (*t.cur == 0xfe) {   // multitrack: the mask of the tracks the sequence opens
        t.cur++;
        u16 mask = (u16)(ReadU16(t) >> 1);
        for (int tr = 1; mask; tr++, mask >>= 1)
            if (mask & 1) {
                int n = AllocateTrack();
                if (n < 0)
                    break;
                TrackInit(s_trk[n]);
                p.tracks[tr] = (u8)n;
            }
    }
    p.active = true;
    p.prepared = true;   // SND_StartPreparedSeq follows at once (the NNS layer does not wait for a command tag)
}

void SetTrackParam(int pi, u16 mask, void (*set)(Track &, int), int value)
{
    Player &p = s_ply[pi];
    for (int i = 0; i < 16 && mask; i++, mask >>= 1)
        if (mask & 1)
            if (Track *t = PlayerTrack(p, i))
                set(*t, value);
}

// One ARM7 sound frame (SndThread's alarm): hardware update, sequencer, channels.
void DriverFrame()
{
    UpdateExChannels();
    SeqMain(true);
    ExChannelMain(true);
    CalcRandom();
}

// ---- NNS player layer ----

NnsSeq *Seq(const SndHandle *h)
{
    if (!h || h->id < 0)
        return nullptr;
    NnsSeq *s = &s_seq[h->id & 0xff];
    return s->active && s->gen == (h->id >> 8) ? s : nullptr;
}

void SeqFinish(NnsSeq &s)
{
    if (!s.active)
        return;
    if (s_ply[s.snd].active)
        PlayerStop(s_ply[s.snd]);
    s.active = false;
    s_nns[s.nnsPlayer].count--;
}

bool Start(SndHandle *h, const u8 *data, u32 offset, const SndSeqInfo &info)
{
    h->id = -1;   // a handle still attached lets go of its sequence, which plays on (main 0x02087340)
    if (!data || info.player >= 16)
        return false;
    NnsPlayer &np = s_nns[info.player];
    // The player's sequence count: the lowest-priority sequence of that player gives way (main 0x02087340).
    if (np.count >= np.maxSeq) {
        NnsSeq *low = nullptr;
        for (auto &s : s_seq)
            if (s.active && s.nnsPlayer == info.player && (!low || s.prio < low->prio))
                low = &s;
        if (!low || info.playerPrio < low->prio)
            return false;
        SeqFinish(*low);
    }
    NnsSeq *slot = nullptr;
    for (auto &s : s_seq)
        if (!s.active) {
            slot = &s;
            break;
        }
    if (!slot) {
        for (auto &s : s_seq)
            if (!slot || s.prio < slot->prio)
                slot = &s;
        if (info.playerPrio < slot->prio)
            return false;
        SeqFinish(*slot);
    }
    int si = (int)(slot - s_seq);
    slot->active = true;
    slot->stopping = false;
    slot->nnsPlayer = info.player;
    slot->prio = info.playerPrio;
    slot->snd = (u8)si;
    slot->infoVolume = info.volume;
    slot->volume = 127;
    slot->lastVol = 0x7fff;
    slot->gen = (u16)((slot->gen + 1) & 0x7fffff);
    slot->fader = NnsFader();
    slot->fader.target = 127 << 8;
    np.count++;
    PrepareSeq(si, data, offset, info.bank);
    s_ply[si].prio = info.channelPrio;
    if (np.channelMask)
        SetTrackParam(si, 0xffff, [](Track &t, int m) { t.channelMask = (u16)m; t.hasChannelMask = true; },
                      np.channelMask);
    h->id = si | slot->gen << 8;
    return true;
}

}

namespace SndDriver {

void Init(const SndSdat *sdat)
{
    for (int i = 0; i < 768; i++)
        s_pitchTable[i] = (u16)lround(65536.0 * (pow(2.0, i / 768.0) - 1.0));
    // The 7-bit volume of each 0.1 dB step, before the divider of its band (-6, -12, -24 dB).
    for (int i = 0; i < 724; i++) {
        int v = i + kVolDbMin;
        int div = v < -240 ? 4 : v < -120 ? 2 : v < -60 ? 1 : 0;
        double a = 127.0 * pow(10.0, v / 200.0) * (div == 4 ? 16 : div == 2 ? 4 : div == 1 ? 2 : 1);
        s_volumeTable[i] = (u8)(a > 127 ? 127 : lround(a));
    }
    s_sdat = sdat;
    for (int i = 0; i < kChannels; i++) {
        s_chn[i] = ExChannel();
        s_chn[i].id = (u8)i;
        s_hw[i] = HwChannel();
    }
    for (int i = 0; i < kTracks; i++)
        s_trk[i] = Track();
    for (int i = 0; i < kPlayers; i++) {
        s_ply[i] = Player();
        s_ply[i].id = (u8)i;
        u16 gen = s_seq[i].gen;
        s_seq[i] = NnsSeq();
        s_seq[i].gen = gen;
    }
    for (int i = 0; i < 16; i++) {
        s_nns[i] = NnsPlayer();
        s_globalVars[i] = -1;
        if (sdat && i < (int)sdat->players.size()) {
            s_nns[i].maxSeq = sdat->players[i].maxSeq;
            s_nns[i].channelMask = sdat->players[i].channelMask;
        }
    }
    s_lockedMask = s_weakLockedMask = 0;
    s_random = 0x12345678;
    s_cycleAcc = 0;
    s_toFrame = 0;
}

void Shutdown() { Init(nullptr); }

bool StartSeq(SndHandle *h, int seq)
{
    if (!s_sdat || seq < 0 || seq >= (int)s_sdat->seqs.size())
        return false;
    return Start(h, s_sdat->SeqData(seq), 0, s_sdat->seqs[seq]);
}

bool StartSeqArc(SndHandle *h, int arc, int index)
{
    SndSeqInfo info;
    if (!s_sdat || !s_sdat->SeqArcEntry(arc, index, &info))
        return false;
    return Start(h, s_sdat->SeqArcData(arc), info.offset, info);
}

void Stop(SndHandle *h, int fadeFrames)
{
    NnsSeq *s = Seq(h);
    if (!s)
        return;
    if (fadeFrames <= 0) {
        SeqFinish(*s);
        return;
    }
    s->fader.Set(0, fadeFrames);
    s->stopping = true;
}

void StopAll(int fadeFrames)
{
    for (int i = 0; i < kPlayers; i++) {
        SndHandle h;
        h.id = i | s_seq[i].gen << 8;
        Stop(&h, fadeFrames);
    }
}

bool IsActive(const SndHandle *h) { return Seq(h) != nullptr; }

void MoveVolume(SndHandle *h, int target, int frames)
{
    if (NnsSeq *s = Seq(h))
        if (!s->stopping)
            s->fader.Set(target << 8, frames);
}

void SetVolume(SndHandle *h, int volume)
{
    if (NnsSeq *s = Seq(h))
        s->volume = (u8)(volume < 0 ? 0 : volume > 127 ? 127 : volume);
}

void SetTrackVolume(SndHandle *h, u16 mask, int volume)
{
    if (NnsSeq *s = Seq(h))
        SetTrackParam(s->snd, mask, [](Track &t, int v) { t.extFader = (s16)v; },
                      kDecibel[volume < 0 ? 0 : volume > 127 ? 127 : volume]);
}

void SetTrackPitch(SndHandle *h, u16 mask, int pitch)
{
    if (NnsSeq *s = Seq(h))
        SetTrackParam(s->snd, mask, [](Track &t, int v) { t.extPitch = (s16)v; }, pitch);
}

void SetTrackPan(SndHandle *h, u16 mask, int pan)
{
    if (NnsSeq *s = Seq(h))
        SetTrackParam(s->snd, mask, [](Track &t, int v) { t.extPan = (s8)v; }, pan);
}

void SetTempoRatio(SndHandle *h, int ratio)
{
    if (NnsSeq *s = Seq(h))
        s_ply[s->snd].tempoRatio = (u16)ratio;
}

// SND_PauseSeq: pausing releases and frees the notes.
void Pause(SndHandle *h, bool pause)
{
    NnsSeq *s = Seq(h);
    if (!s)
        return;
    Player &p = s_ply[s->snd];
    p.paused = pause;
    if (pause)
        for (int i = 0; i < 16; i++)
            if (Track *t = PlayerTrack(p, i)) {
                TrackReleaseChannels(*t, p, 127);
                TrackFreeChannels(*t);
            }
}

void SetPlayerVolume(int nnsPlayer, int volume)
{
    if (nnsPlayer >= 0 && nnsPlayer < 16)
        s_nns[nnsPlayer].volume = (u8)(volume < 0 ? 0 : volume > 127 ? 127 : volume);
}

// NNS_SndMain's player update (main 0x02087498): finished sequences free their slot, the fader moves, the player
// volume is the sum of the fader, the archive's volume, the handle's and the player's (0.1 dB, SNDi_DecibelTable).
void Main()
{
    for (auto &s : s_seq) {
        if (!s.active)
            continue;
        if (!s_ply[s.snd].active) {
            s.active = false;
            s_nns[s.nnsPlayer].count--;
            continue;
        }
        s.fader.Update();
        int vol = kDecibel[s.fader.Get() >> 8] + kDecibel[s.infoVolume] + kDecibel[s.volume] +
                  kDecibel[s_nns[s.nnsPlayer].volume];
        vol = vol < -0x8000 ? -0x8000 : vol > 0x7fff ? 0x7fff : vol;
        if (vol != s.lastVol) {
            s_ply[s.snd].extFader = (s16)vol;
            s.lastVol = (s16)vol;
        }
        if (s.stopping && s.fader.Done())
            SeqFinish(s);
    }
}

void Render(s16 *out, int frames)
{
    static s32 acc[2 * 256];
    while (frames > 0) {
        if (s_toFrame == 0) {
            DriverFrame();
            // 64 x 2728 cycles of the 33.513982 MHz clock per frame.
            s_cycleAcc += 174592ull * kRate;
            s_toFrame = (int)(s_cycleAcc / 33513982u);
            s_cycleAcc %= 33513982u;
        }
        int n = frames < s_toFrame ? frames : s_toFrame;
        if (n > 256)
            n = 256;
        memset(acc, 0, sizeof(s32) * 2 * n);
        for (int i = 0; i < kChannels; i++)
            if (s_hw[i].active)
                HwMix(s_hw[i], acc, n);
        for (int i = 0; i < 2 * n; i++) {   // master volume 127
            s32 v = (acc[i] * 127) >> 7;
            out[i] = (s16)(v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v);
        }
        out += 2 * n;
        frames -= n;
        s_toFrame -= n;
    }
}

int ActiveChannels()
{
    int n = 0;
    for (auto &h : s_hw)
        n += h.active;
    return n;
}

int ActivePlayers()
{
    int n = 0;
    for (auto &p : s_ply)
        n += p.active;
    return n;
}

}
