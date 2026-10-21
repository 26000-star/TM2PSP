// Ghost cars: the recorded runs of Ghosts/<Env>/<difficulty>/<n>/{bronze,silver,gold}.bin (the medal opponents)
// and the player's best run, played back as a car body matrix per race tick (1/60 s).
//
// File (main 0x02095ad8): u8 1, u8, u16 60 (ticks per second), u32 6 (version), u32 finish time (fx32), u32 stream
// bytes, u32 laps, u32 checkpoints, laps x checkpoints u32 split times (passing order), then the stream; some files
// carry bytes after it (unused). Stream records (main 0x02095c5c): a flags byte, then
//   1: up axis (matrix row 1) += 3 x s16;      2: forward axis (row 2) += 3 x s16;
//   8: position x, z += 2 x s16, else 0x10: position = 3 x s32;
//   0x80: u8 ticks this record lasts (else 1);  0x20: s8 (<< 6, +0x30);  4: s8 (<< 3, +0x38);  0x40: 4 bytes;
// a record lasting n ticks is reached by interpolation from the previous one (0x02095f70 with 0x02097070); row 0 is
// up x forward, normalised. The first record is the start (the race clock's 0).
#pragma once

#include <vector>

#include "nitro.h"

class dsGhost {
public:
    enum { TICKS_PER_SECOND = 60 };

    // @port main:02095ad8 partial (into the port's sample list instead of the game's stream reader)
    // @port main:02095c5c done (each record decoded into a sample)
    bool Load(const std::vector<u8> &file);
    bool Loaded() const { return !m_samples.empty(); }
    // The body matrix at race tick `tick` (clamped to the run); false when nothing is loaded.
    // @port main:02095f70 partial (positions interpolated within a record, axes taken from the record)
    bool Matrix(int tick, MtxFx43 *out) const;
    int Ticks() const { return m_samples.empty() ? 0 : m_samples.back().tick + m_samples.back().length; }

    fx32 finishTime = 0;
    int laps = 0, checkpoints = 0;
    std::vector<fx32> splits;

    // Recording, one call per 30 Hz frame from the start (two ticks): records with flags 0x80 (2 ticks), 0x10 and
    // 1 / 2 when the axes changed, decoded by Load.
    void BeginRecord();
    void Record(const MtxFx43 &body);
    bool Recording() const { return m_recording; }
    // The recorded run as a ghost file (same format as the game's).
    std::vector<u8> Save(fx32 finish, const std::vector<fx32> &splitTimes, int lapCount, int checkpointCount) const;

private:
    struct Sample {
        int tick, length;
        fx32 up[3], fwd[3], pos[3];
    };
    std::vector<Sample> m_samples;
    std::vector<u8> m_stream;
    s32 m_recUp[3] = {}, m_recFwd[3] = {};
    bool m_recording = false;
};
