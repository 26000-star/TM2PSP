/* NitroSDK OS services on the PSP: debug output goes to stderr (PSPLink / PPSSPP log), a panic prints and exits
 * the program back to the XMB. */
#include <stdarg.h>
#include <stdio.h>

#include <pspkernel.h>

#include "nitro.h"

void OS_Printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void OS_Panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "OS_Panic: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    sceKernelExitGame();
    for (;;)
        sceKernelDelayThread(1000000);
}
