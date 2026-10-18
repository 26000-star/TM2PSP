/* NitroSDK OS services on the host: debug output and panics go to stderr. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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
    abort();
}
