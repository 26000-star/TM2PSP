// Host entry of the check suite (the PSP one is src/psp/checks_psp.cpp).
//   tm_checksuite --root extract/files --list <checks.txt> --out <dir>
// Writes <dir>/<name>.txt per check and <dir>/suite.log; tools/psp/compare_checks.py compares them with the PSP.
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <string>

#include "suite.h"

static unsigned MsNow()
{
    using namespace std::chrono;
    return (unsigned)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    const char *root = "extract/files", *list = "build/host/checks.txt", *out = "build/host/suite";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--root"))
            root = argv[i + 1];
        else if (!strcmp(argv[i], "--list"))
            list = argv[i + 1];
        else if (!strcmp(argv[i], "--out"))
            out = argv[i + 1];
    }
    std::string log = std::string(out) + "/suite.log";
    int failed = RunSuite(root, list, out, log.c_str(), MsNow, nullptr);
    fprintf(stderr, "suite: %d failed (log %s)\n", failed, log.c_str());
    return failed ? 1 : 0;
}
