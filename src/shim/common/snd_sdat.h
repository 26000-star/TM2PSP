/* Nitro sound archive (SDAT) reader: the symbol, info and file tables, and the SSEQ / SSAR / SBNK / SWAR files the
 * sound driver plays (format notes in docs/ASSETS.md). The whole archive is kept in memory; the driver reads the
 * sequences, banks and samples in place, as the DS does once a group is loaded. */
#ifndef TM_SND_SDAT_H
#define TM_SND_SDAT_H

#include "nitro.h"

#include <string>
#include <vector>

enum SndSdatKind { SDAT_SEQ = 0, SDAT_SEQARC, SDAT_BANK, SDAT_WAVEARC, SDAT_PLAYER, SDAT_GROUP, SDAT_PLAYER2, SDAT_STRM };

struct SndSeqInfo {   // INFO record of a sequence, and of a sequence archive entry
    u32 offset = 0;   // SSAR entries: from the archive's data; SSEQ: from the file's data
    u16 file = 0xffff, bank = 0;
    u8 volume = 127, channelPrio = 64, playerPrio = 64, player = 0;
};

struct SndPlayerInfo {
    u8 maxSeq = 0;
    u16 channelMask = 0;   // 0: all channels
    u32 heapSize = 0;
};

// One wave of a SWAR (SNDWaveData): its parameter block, then the samples.
struct SndWave {
    u8 format;      // 0 PCM8, 1 PCM16, 2 IMA-ADPCM
    u8 loop;        // 1: loops from loopStart
    u16 rate;
    u16 timer;      // 16756991 / rate
    u16 loopStart;  // in 32-bit words (the ADPCM header word included)
    u32 loopLen;    // in 32-bit words
    u8 data[1];
};

struct SndSdat {
    std::vector<u8> image;
    std::vector<std::string> names[8];           // SYMB names per kind ("" when absent)
    std::vector<std::vector<std::string>> seqArcNames;   // SYMB names of each sequence archive's sequences
    std::vector<SndSeqInfo> seqs;
    std::vector<u16> seqArcFiles;
    std::vector<u16> bankFiles;
    std::vector<u16> bankWaveArcs[4];            // the 4 wave archive links of each bank (0xffff: none)
    std::vector<u16> waveArcFiles;
    std::vector<SndPlayerInfo> players;

    bool Load(const char *path);                 // through the FS shim, from the ROM root
    bool LoadImage(std::vector<u8> &&bytes);
    void Clear();

    int Find(SndSdatKind kind, const char *name) const;   // -1 when absent
    const u8 *File(u16 id, u32 *size = nullptr) const;

    // Sequence archives: the entry count, an entry and the start of the archive's sequence data.
    int SeqArcCount(int arc) const;
    bool SeqArcEntry(int arc, int index, SndSeqInfo *info) const;
    const u8 *SeqArcData(int arc) const;
    const u8 *SeqData(int seq) const;            // the SSEQ's sequence data (the event stream)

    const u8 *Bank(int bank) const;              // the SBNK file (SNDBankData), offsets count from its start
    int WaveCount(int waveArc) const;
    const SndWave *Wave(int waveArc, int wave) const;

  private:
    u32 U32(u32 off) const;
    u16 U16(u32 off) const;
    std::string Str(u32 off) const;
};

#endif
