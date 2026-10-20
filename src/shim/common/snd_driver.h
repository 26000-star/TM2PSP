/* The DS sound driver on the CPU: the NitroSDK ARM7 sequencer (SSEQ players and tracks, the 16 extended channels
 * with their envelopes, sweeps and LFOs), the 16 hardware channels (PCM8 / PCM16 / IMA-ADPCM, no interpolation, as
 * the DS mixer) mixed to 32 kHz stereo, and the NitroSystem sound-player layer the game calls (handles, players
 * with a sequence count and channel mask, faders, the seq volume from the archive).
 *
 * Library code, rewritten from the SDK's behaviour (the ARM7 driver as decompiled by pret/pokediamond, the ARM9 NNS
 * player as called by main 0x02086e30..0x02088b64); its tables are the ROM's (arm7.bin 0xf268 / 0xf368, the
 * allocation order 0xf46c) except the two BIOS tables, computed. Deterministic: the output depends only on the
 * calls and the number of samples rendered. Not thread-safe: the back-ends lock around Render and the game's calls. */
#ifndef TM_SND_DRIVER_H
#define TM_SND_DRIVER_H

#include "nitro.h"

struct SndSdat;

struct SndHandle {
    int id = -1;   // NNS sequence player slot | generation << 8; -1: none
};

namespace SndDriver {

enum { kRate = 32000, kChannels = 16, kPlayers = 16, kTracks = 32 };

void Init(const SndSdat *sdat);   // stops everything; nullptr leaves the driver silent
void Shutdown();

// NitroSystem player layer (NNS_SndArcPlayerStartSeq / StartSeqArc and the NNS_SndPlayer calls).
bool StartSeq(SndHandle *h, int seq);
bool StartSeqArc(SndHandle *h, int arc, int index);
void Stop(SndHandle *h, int fadeFrames);   // 0: at once (the channels still release)
void StopAll(int fadeFrames);
bool IsActive(const SndHandle *h);
void MoveVolume(SndHandle *h, int target, int frames);   // fader, 0..127
void SetVolume(SndHandle *h, int volume);                // the handle's own volume, 0..127
void SetTrackVolume(SndHandle *h, u16 trackMask, int volume);   // 0..127
void SetTrackPitch(SndHandle *h, u16 trackMask, int pitch);     // 1/64 semitone
void SetTrackPan(SndHandle *h, u16 trackMask, int pan);         // -128..127
void SetTempoRatio(SndHandle *h, int ratio);                    // 256 = 1
void Pause(SndHandle *h, bool pause);
void SetPlayerVolume(int nnsPlayer, int volume);
void Main();   // NNS_SndMain: faders and player volumes, once per game frame

// Output: interleaved stereo s16, kRate Hz. Runs the driver frames (every 64 x 2728 ARM7 cycles, 192 Hz) between
// the samples.
void Render(s16 *out, int frames);

// Statistics for the reports: active hardware channels and sequence players.
int ActiveChannels();
int ActivePlayers();

}

#endif
