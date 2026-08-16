#include "vita_breadcrumb.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>

#define VITA_BREADCRUMB_PATH "ux0:data/kisakcod/step.txt"

void VitaSys_Breadcrumb(const char *format, ...)
{
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

    // open/write/close per call; a buffered stream loses its tail on an abnormal exit
    const SceUID file = sceIoOpen(VITA_BREADCRUMB_PATH,
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (file < 0)
        return;
    sceIoWrite(file, text, (SceSize)length);
    sceIoClose(file);
}

void VitaSys_BreadcrumbReset(void)
{
    const SceUID file = sceIoOpen(VITA_BREADCRUMB_PATH,
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (file >= 0)
        sceIoClose(file);
}
