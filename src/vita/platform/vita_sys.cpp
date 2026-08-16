#include "vita_sys.h"

#include <psp2/kernel/processmgr.h>

#define VITA_TICKS_PER_SECOND 1000000ull

extern "C" {

unsigned long long __rdtsc(void)
{
    return sceKernelGetProcessTimeWide();
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *counter)
{
    if (!counter)
        return 0;
    counter->QuadPart = (long long)sceKernelGetProcessTimeWide();
    return 1;
}

// one tick is a microsecond, so the engine's calibration collapses to a constant
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
    if (!frequency)
        return 0;
    frequency->QuadPart = (long long)VITA_TICKS_PER_SECOND;
    return 1;
}

}

unsigned int VitaSys_Milliseconds(void)
{
    static unsigned long long base;
    const unsigned long long now = sceKernelGetProcessTimeWide();
    if (!base)
        base = now;
    return (unsigned int)((now - base) / 1000ull);
}
