// SDAT reader (see snd_sdat.h). Layouts: NitroSDK sound archive, checked on audio/sound_data.sdat (tm_sdat --list).
#include "snd_sdat.h"

#include <string.h>

u32 SndSdat::U32(u32 off) const
{
    if (off + 4 > image.size())
        return 0;
    const u8 *p = &image[off];
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

u16 SndSdat::U16(u32 off) const
{
    if (off + 2 > image.size())
        return 0;
    return (u16)(image[off] | image[off + 1] << 8);
}

std::string SndSdat::Str(u32 off) const
{
    std::string s;
    while (off < image.size() && image[off])
        s += (char)image[off++];
    return s;
}

void SndSdat::Clear()
{
    *this = SndSdat();
}

bool SndSdat::Load(const char *path)
{
    FSFile f;
    FS_InitFile(&f);
    if (!FS_OpenFile(&f, path))
        return false;
    std::vector<u8> bytes(FS_GetLength(&f));
    s32 n = FS_ReadFile(&f, bytes.data(), (s32)bytes.size());
    FS_CloseFile(&f);
    if (n != (s32)bytes.size())
        return false;
    return LoadImage(std::move(bytes));
}

bool SndSdat::LoadImage(std::vector<u8> &&bytes)
{
    Clear();
    image = std::move(bytes);
    if (image.size() < 0x40 || memcmp(image.data(), "SDAT", 4))
        return false;
    u32 symb = U32(0x10), info = U32(0x18);
    // SYMB: 8 record offsets; each record is a count and name offsets, the sequence archive record pairs a name
    // with a sub-record of the archive's sequence names. All offsets count from the SYMB block.
    if (symb) {
        for (int k = 0; k < 8; k++) {
            u32 rec = U32(symb + 8 + 4 * k);
            if (!rec)
                continue;
            u32 n = U32(symb + rec);
            for (u32 i = 0; i < n; i++) {
                if (k == SDAT_SEQARC) {
                    u32 name = U32(symb + rec + 4 + 8 * i), sub = U32(symb + rec + 8 + 8 * i);
                    names[k].push_back(name ? Str(symb + name) : "");
                    std::vector<std::string> subs;
                    for (u32 j = 0, m = sub ? U32(symb + sub) : 0; j < m; j++) {
                        u32 s = U32(symb + sub + 4 + 4 * j);
                        subs.push_back(s ? Str(symb + s) : "");
                    }
                    seqArcNames.push_back(subs);
                } else {
                    u32 name = U32(symb + rec + 4 + 4 * i);
                    names[k].push_back(name ? Str(symb + name) : "");
                }
            }
        }
    }
    // INFO: 8 record offsets (from the INFO block); each record is a count and entry offsets.
    for (int k = 0; k < 8; k++) {
        u32 rec = U32(info + 8 + 4 * k);
        u32 n = rec ? U32(info + rec) : 0;
        for (u32 i = 0; i < n; i++) {
            u32 e = U32(info + rec + 4 + 4 * i);
            u32 p = info + e;
            switch (k) {
            case SDAT_SEQ: {
                SndSeqInfo s;
                if (e) {
                    s.file = U16(p);
                    s.bank = U16(p + 4);
                    s.volume = image[p + 6];
                    s.channelPrio = image[p + 7];
                    s.playerPrio = image[p + 8];
                    s.player = image[p + 9];
                }
                seqs.push_back(s);
                break;
            }
            case SDAT_SEQARC:
                seqArcFiles.push_back(e ? U16(p) : 0xffff);
                break;
            case SDAT_BANK:
                bankFiles.push_back(e ? U16(p) : 0xffff);
                for (int w = 0; w < 4; w++)
                    bankWaveArcs[w].push_back(e ? U16(p + 4 + 2 * w) : 0xffff);
                break;
            case SDAT_WAVEARC:
                waveArcFiles.push_back(e ? U16(p) : 0xffff);
                break;
            case SDAT_PLAYER: {
                SndPlayerInfo pl;
                if (e) {
                    pl.maxSeq = image[p];
                    pl.channelMask = U16(p + 2);
                    pl.heapSize = U32(p + 4);
                }
                players.push_back(pl);
                break;
            }
            default:
                break;
            }
        }
    }
    for (int k = 0; k < 8; k++)
        names[k].resize(k == SDAT_SEQ ? seqs.size() : k == SDAT_SEQARC ? seqArcFiles.size()
                        : k == SDAT_BANK ? bankFiles.size() : k == SDAT_WAVEARC ? waveArcFiles.size()
                        : k == SDAT_PLAYER ? players.size() : names[k].size());
    seqArcNames.resize(seqArcFiles.size());
    return true;
}

int SndSdat::Find(SndSdatKind kind, const char *name) const
{
    for (size_t i = 0; i < names[kind].size(); i++)
        if (names[kind][i] == name)
            return (int)i;
    return -1;
}

// FAT: count, then {u32 offset (from the archive start), u32 size, 8 reserved bytes} per file.
const u8 *SndSdat::File(u16 id, u32 *size) const
{
    u32 fat = U32(0x20);
    if (!fat || id >= U32(fat + 8))
        return nullptr;
    u32 off = U32(fat + 12 + 16 * id), len = U32(fat + 16 + 16 * id);
    if (off + len > image.size())
        return nullptr;
    if (size)
        *size = len;
    return &image[off];
}

// SSAR: header (16), "DATA", size, u32 data offset (from the file start), u32 count,
// count x {u32 sequence offset (from the data), u16 bank, u8 volume, channel priority, player priority, player, 2 pad}.
int SndSdat::SeqArcCount(int arc) const
{
    const u8 *f = arc >= 0 && arc < (int)seqArcFiles.size() ? File(seqArcFiles[arc]) : nullptr;
    return f ? (int)(f[0x1c] | f[0x1d] << 8 | f[0x1e] << 16 | f[0x1f] << 24) : 0;
}

bool SndSdat::SeqArcEntry(int arc, int index, SndSeqInfo *info) const
{
    if (index < 0 || index >= SeqArcCount(arc))
        return false;
    const u8 *e = File(seqArcFiles[arc]) + 0x20 + 12 * index;
    info->offset = e[0] | e[1] << 8 | e[2] << 16 | (u32)e[3] << 24;
    info->file = seqArcFiles[arc];
    info->bank = (u16)(e[4] | e[5] << 8);
    info->volume = e[6];
    info->channelPrio = e[7];
    info->playerPrio = e[8];
    info->player = e[9];
    return info->offset != 0xffffffff;
}

const u8 *SndSdat::SeqArcData(int arc) const
{
    const u8 *f = arc >= 0 && arc < (int)seqArcFiles.size() ? File(seqArcFiles[arc]) : nullptr;
    return f ? f + (f[0x18] | f[0x19] << 8 | f[0x1a] << 16 | f[0x1b] << 24) : nullptr;
}

// SSEQ: header (16), "DATA", size, u32 data offset (from the file start).
const u8 *SndSdat::SeqData(int seq) const
{
    const u8 *f = seq >= 0 && seq < (int)seqs.size() ? File(seqs[seq].file) : nullptr;
    return f ? f + (f[0x18] | f[0x19] << 8 | f[0x1a] << 16 | f[0x1b] << 24) : nullptr;
}

const u8 *SndSdat::Bank(int bank) const
{
    return bank >= 0 && bank < (int)bankFiles.size() ? File(bankFiles[bank]) : nullptr;
}

// SWAR: header (16), "DATA", size, 32 reserved bytes, u32 wave count, u32 offsets (from the file start).
int SndSdat::WaveCount(int waveArc) const
{
    u32 size = 0;
    const u8 *f = waveArc >= 0 && waveArc < (int)waveArcFiles.size() ? File(waveArcFiles[waveArc], &size) : nullptr;
    return f && size >= 0x3c ? (int)(f[0x38] | f[0x39] << 8 | f[0x3a] << 16 | f[0x3b] << 24) : 0;
}

const SndWave *SndSdat::Wave(int waveArc, int wave) const
{
    if (wave < 0 || wave >= WaveCount(waveArc))
        return nullptr;
    const u8 *f = File(waveArcFiles[waveArc]);
    const u8 *o = f + 0x3c + 4 * wave;
    u32 off = o[0] | o[1] << 8 | o[2] << 16 | (u32)o[3] << 24;
    return off ? (const SndWave *)(f + off) : nullptr;
}
