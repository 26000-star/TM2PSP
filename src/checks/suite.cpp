#include "suite.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "collision/dsCollisionWorld.h"
#include "fs/dsFileSystem.h"
#include "nitro.h"

int cfgdump_main(int argc, char **argv);
int trackdump_main(int argc, char **argv);
int physicstest_main(int argc, char **argv);

// Each driver starts from the state a new process would have: the singletons that hold loaded data are reset.
// (dsRaceSetup and dsPieceDataTable are re-initialised by the drivers themselves.)
static void ResetState()
{
    dsCollisionWorld::Get()->Clear();
    delete dsFileSystem::Get();
}

int RunSuite(const char *root, const char *list, const char *outdir, const char *log, unsigned (*msNow)(),
             unsigned (*heapUsed)())
{
    FILE *in = fopen(list, "rb");
    FILE *lf = fopen(log, "wb");
    if (!in || !lf) {
        fprintf(stderr, "cannot open %s or %s\n", list, log);
        return -1;
    }
    char line[512];
    int failed = 0, count = 0;
    unsigned start = msNow();
    while (fgets(line, sizeof line, in)) {
        std::vector<std::string> words;
        for (char *w = strtok(line, " \t\r\n"); w; w = strtok(nullptr, " \t\r\n"))
            words.push_back(w);
        if (words.size() < 2)
            continue;
        const std::string &prog = words[1];
        int (*entry)(int, char **) = prog == "tm_cfgdump" ? cfgdump_main
                                     : prog == "tm_trackdump" ? trackdump_main
                                     : prog == "tm_physics" ? physicstest_main
                                     : nullptr;
        std::vector<std::string> args = {prog, "--root", root};
        args.insert(args.end(), words.begin() + 2, words.end());
        std::vector<char *> argv;
        for (std::string &a : args)
            argv.push_back(&a[0]);
        argv.push_back(nullptr);
        std::string out = std::string(outdir) + "/" + words[0] + ".txt";
        fflush(stdout);
#ifdef PSP
        // newlib's stdout is assignable; freopen leaks one sceIo descriptor per call on the PSP (59 at most).
        FILE *saved = stdout, *f = fopen(out.c_str(), "wb");
        if (f)
            stdout = f;
#else
        FILE *f = freopen(out.c_str(), "wb", stdout);
#endif
        if (!f) {
            fprintf(lf, "%s cannot-open-output\n", words[0].c_str());
            failed++;
            continue;
        }
        unsigned t0 = msNow();
        int rc = entry ? entry((int)args.size(), argv.data()) : 127;
        fflush(stdout);
#ifdef PSP
        stdout = saved;
        fclose(f);
#endif
        ResetState();
        fprintf(lf, "%s %d %u %u %d\n", words[0].c_str(), rc, msNow() - t0, heapUsed ? heapUsed() : 0,
                FSh_OpenFiles());
        fflush(lf);
        count++;
        if (rc != 0)
            failed++;
    }
    fprintf(lf, "done %d checks, %d failed, %u ms\n", count, failed, msNow() - start);
    fclose(lf);
    fclose(in);
    return failed;
}
