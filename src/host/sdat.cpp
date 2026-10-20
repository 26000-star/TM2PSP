// Sound archive tool: lists audio/sound_data.sdat and renders its sequences through the ported sound driver.
//   tm_sdat --root <dir> --list
//   tm_sdat --root <dir> --seq <name|n> | --arc <name|n> --index <n|name>  [--seconds S] [--wav out.wav] [--pitch P]
// Without --wav it prints the render's statistics (peak, RMS, active channels, CPU time per second of sound).
#include "snd_driver.h"
#include "snd_sdat.h"
#include "wav_writer.h"

#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Index(const SndSdat &sd, SndSdatKind kind, const char *s)
{
    char *end;
    long n = strtol(s, &end, 0);
    return *end ? sd.Find(kind, s) : (int)n;
}

static void List(const SndSdat &sd)
{
    static const char *const fmt[] = {"pcm8", "pcm16", "adpcm"};
    for (size_t i = 0; i < sd.seqs.size(); i++) {
        const SndSeqInfo &s = sd.seqs[i];
        u32 size = 0;
        sd.File(s.file, &size);
        printf("seq %zu %s file %u bank %u (%s) vol %u cpr %u ppr %u player %u size %u\n", i,
               sd.names[SDAT_SEQ][i].c_str(), s.file, s.bank, sd.names[SDAT_BANK][s.bank].c_str(), s.volume,
               s.channelPrio, s.playerPrio, s.player, size);
    }
    for (size_t a = 0; a < sd.seqArcFiles.size(); a++) {
        printf("seqarc %zu %s file %u entries %d\n", a, sd.names[SDAT_SEQARC][a].c_str(), sd.seqArcFiles[a],
               sd.SeqArcCount((int)a));
        for (int i = 0; i < sd.SeqArcCount((int)a); i++) {
            SndSeqInfo s;
            if (!sd.SeqArcEntry((int)a, i, &s))
                continue;
            const char *nm = i < (int)sd.seqArcNames[a].size() ? sd.seqArcNames[a][i].c_str() : "";
            printf("  %d %s offset %u bank %u (%s) vol %u cpr %u ppr %u player %u\n", i, nm, s.offset, s.bank,
                   sd.names[SDAT_BANK][s.bank].c_str(), s.volume, s.channelPrio, s.playerPrio, s.player);
        }
    }
    for (size_t b = 0; b < sd.bankFiles.size(); b++) {
        const u8 *f = sd.Bank((int)b);
        unsigned n = f ? f[0x38] | f[0x39] << 8 : 0;
        printf("bank %zu %s file %u instruments %u wavearc %u\n", b, sd.names[SDAT_BANK][b].c_str(), sd.bankFiles[b],
               n, sd.bankWaveArcs[0][b]);
    }
    for (size_t w = 0; w < sd.waveArcFiles.size(); w++) {
        int n = sd.WaveCount((int)w);
        printf("wavearc %zu %s file %u waves %d:", w, sd.names[SDAT_WAVEARC][w].c_str(), sd.waveArcFiles[w], n);
        for (int i = 0; i < n; i++)
            if (const SndWave *v = sd.Wave((int)w, i)) {
                const u8 *p = (const u8 *)v;
                unsigned rate = p[2] | p[3] << 8, ls = p[6] | p[7] << 8;
                unsigned ll = p[8] | p[9] << 8 | p[10] << 16 | (unsigned)p[11] << 24;
                printf(" [%s %u Hz %s %u+%u]", fmt[p[0] % 3], rate, p[1] ? "loop" : "once", ls, ll);
            }
        printf("\n");
    }
    for (size_t p = 0; p < sd.players.size(); p++)
        printf("player %zu %s maxseq %u channels 0x%04x heap %u\n", p, sd.names[SDAT_PLAYER][p].c_str(),
               sd.players[p].maxSeq, sd.players[p].channelMask, sd.players[p].heapSize);
}

int main(int argc, char **argv)
{
    const char *root = ".", *seq = nullptr, *arc = nullptr, *index = "0", *wav = nullptr;
    double seconds = 10;
    int pitch = 0;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--list"))
            list = true;
        else if (!strcmp(argv[i], "--seq") && i + 1 < argc)
            seq = argv[++i];
        else if (!strcmp(argv[i], "--arc") && i + 1 < argc)
            arc = argv[++i];
        else if (!strcmp(argv[i], "--index") && i + 1 < argc)
            index = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc)
            pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
        else {
            fprintf(stderr, "usage: tm_sdat --root <dir> [--list] [--seq <name|n> | --arc <name|n> --index <n|name>]"
                            " [--seconds S] [--pitch P] [--wav out.wav]\n");
            return 2;
        }
    }
    FSh_SetRoot(root);
    FS_Init(3);
    SndSdat sd;
    if (!sd.Load("audio/sound_data.sdat")) {
        fprintf(stderr, "cannot read audio/sound_data.sdat under %s\n", root);
        return 1;
    }
    if (list)
        List(sd);
    if (!seq && !arc)
        return 0;
    SndDriver::Init(&sd);
    SndHandle h;
    bool ok;
    if (seq)
        ok = SndDriver::StartSeq(&h, Index(sd, SDAT_SEQ, seq));
    else {
        int a = Index(sd, SDAT_SEQARC, arc);
        char *end;
        int n = (int)strtol(index, &end, 0);
        if (*end && a >= 0)
            for (n = 0; n < (int)sd.seqArcNames[a].size() && sd.seqArcNames[a][n] != index; n++) {
            }
        ok = SndDriver::StartSeqArc(&h, a, n);
    }
    if (!ok) {
        fprintf(stderr, "cannot start the sequence\n");
        return 1;
    }
    if (pitch)
        SndDriver::SetTrackPitch(&h, 0xffff, pitch);
    // 30 game frames per second, as the race calls NNS_SndMain.
    std::vector<s16> out;
    int total = (int)(seconds * SndDriver::kRate), done = 0, maxChannels = 0, frame = 0;
    double cpu = 0;
    while (done < total) {
        SndDriver::Main();
        int n = (int)((long long)(frame + 1) * SndDriver::kRate / 30 - (long long)frame * SndDriver::kRate / 30);
        if (n > total - done)
            n = total - done;
        size_t at = out.size();
        out.resize(at + 2 * n);
        auto t0 = std::chrono::steady_clock::now();
        SndDriver::Render(&out[at], n);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int c = SndDriver::ActiveChannels();
        maxChannels = c > maxChannels ? c : maxChannels;
        done += n;
        frame++;
    }
    int peak = 0, clipped = 0;
    double sum = 0;
    unsigned hash = 2166136261u;
    for (s16 v : out) {
        int a = v < 0 ? -v : v;
        peak = a > peak ? a : peak;
        clipped += a >= 32767;
        sum += (double)v * v;
        hash = (hash ^ (u16)v) * 16777619u;
    }
    printf("samples %d peak %d rms %.1f clipped %d max_channels %d playing %d hash %08x cpu %.2f ms per second\n",
           total, peak, sqrt(sum / (out.size() ? out.size() : 1)), clipped, maxChannels,
           SndDriver::IsActive(&h) ? 1 : 0, hash, 1000 * cpu / seconds);
    if (wav && !WriteWav(wav, out, SndDriver::kRate)) {
        fprintf(stderr, "cannot write %s\n", wav);
        return 1;
    }
    return 0;
}
