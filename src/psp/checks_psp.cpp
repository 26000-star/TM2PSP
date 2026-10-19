// PSP test EBOOT: runs the check suite (src/checks/suite.cpp) on the PSP. Data next to the EBOOT:
//   files/       the extracted ROM file system (extract/files)
//   checks.txt   the check list (python -P tools/host/run_checks.py --list)
// Output: out/<name>.txt per check, out/suite.log (exit code and milliseconds per check, "done" line last).
// tools/psp/run_checks_psp.ps1 installs it in PPSSPP's memstick, waits for the log and compares with the host.
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>

#include "nitro.h"
#include "suite.h"

PSP_MODULE_INFO("TM2PSP checks", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-1024);

static unsigned MsNow()
{
    return (unsigned)(sceKernelGetSystemTimeWide() / 1000);
}

static unsigned HeapUsed()
{
    return (unsigned)mallinfo().uordblks;
}

int main(int argc, char **argv)
{
    std::string dir = argc > 0 ? argv[0] : "ms0:/PSP/GAME/TM2PSPCHK/EBOOT.PBP";
    dir = dir.substr(0, dir.find_last_of('/'));
    pspDebugScreenInit();
    pspDebugScreenPrintf("TM2PSP check suite\n%s\n", dir.c_str());
    std::string root = dir + "/files", list = dir + "/checks.txt", out = dir + "/out";
    sceIoMkdir(out.c_str(), 0777);
    // The log is written under a temporary name and renamed at the end, so a reader sees it only when complete.
    std::string tmp = out + "/suite.tmp", log = out + "/suite.log";
    sceIoRemove(log.c_str());
    int failed = RunSuite(root.c_str(), list.c_str(), out.c_str(), tmp.c_str(), MsNow, HeapUsed);
    sceIoRename(tmp.c_str(), log.c_str());
    pspDebugScreenPrintf("done: %d failed\n", failed);
    sceKernelExitGame();
    return 0;
}
