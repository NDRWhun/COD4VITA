#include "vita_breadcrumb.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VITA_BREADCRUMB_PATH "ux0:data/kisakcod/step.txt"
#define VITA_BREADCRUMB_BYTES (64 * 1024)

// the card is busy streaming fastfiles during boot, so a write per call blocks long enough
// for the system to kill the app; the trail accumulates here and reaches the card in batches
static char s_trail[VITA_BREADCRUMB_BYTES];
static uint32_t s_used;
static uint64_t s_lastWrite;

void VitaSys_BreadcrumbFlush(void)
{
    if (!s_used)
        return;

    const SceUID file = sceIoOpen(VITA_BREADCRUMB_PATH,
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (file < 0)
        return;
    sceIoWrite(file, s_trail, (SceSize)s_used);
    sceIoClose(file);
    s_used = 0;
}

void VitaSys_BreadcrumbReset(void)
{
    s_used = 0;
    s_lastWrite = 0;

    const SceUID file = sceIoOpen(VITA_BREADCRUMB_PATH,
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (file >= 0)
        sceIoClose(file);
}

void VitaSys_Breadcrumb(const char *format, ...)
{
    char text[512];
    va_list arguments;

    const uint64_t now = sceKernelGetProcessTimeWide();
    const int stamp = snprintf(text, sizeof(text), "[%8u] ", (unsigned)(now / 1000));

    va_start(arguments, format);
    int length = vsnprintf(text + stamp, sizeof(text) - stamp - 1, format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    length += stamp;
    text[length++] = '\n';

    if (s_used + (uint32_t)length <= sizeof(s_trail))
    {
        memcpy(s_trail + s_used, text, (size_t)length);
        s_used += (uint32_t)length;
    }

    // two seconds of trail is the most a kill can cost, for four writes across a boot
    if (now - s_lastWrite >= 2000000ull)
    {
        s_lastWrite = now;
        VitaSys_BreadcrumbFlush();
    }
}
