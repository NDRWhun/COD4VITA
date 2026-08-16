#include "vita_breadcrumb.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>

#define VITA_BREADCRUMB_PATH "ux0:data/kisakcod/step.txt"

// held open for the run; opening per call fails once the engine exhausts the descriptor table
static SceUID s_file = -1;

void VitaSys_BreadcrumbReset(void)
{
    if (s_file >= 0)
        sceIoClose(s_file);
    s_file = sceIoOpen(VITA_BREADCRUMB_PATH,
                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

void VitaSys_Breadcrumb(const char *format, ...)
{
    if (s_file < 0)
        return;

    char text[512];
    va_list arguments;

    const int stamp = snprintf(text, sizeof(text), "[%8u] ",
                               (unsigned)(sceKernelGetProcessTimeWide() / 1000));
    va_start(arguments, format);
    int length = vsnprintf(text + stamp, sizeof(text) - stamp - 1, format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    length += stamp;
    text[length++] = '\n';

    sceIoWrite(s_file, text, (SceSize)length);
    sceIoSyncByFd(s_file, 0);
}
