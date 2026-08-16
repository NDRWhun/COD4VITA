#include "vita_sys.h"

#include <psp2/kernel/processmgr.h>

#include "vita_memory.h"

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

#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE  0x8000

extern "C" {

void *VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    (void)protect;

    // a commit of already-reserved space is a no-op, since the reserve allocated it
    if (address && (type & MEM_COMMIT))
        return address;
    if (!(type & (MEM_RESERVE | MEM_COMMIT)))
        return NULL;

    return VitaMem_Alloc(VITA_MEM_MAIN, (uint32_t)size, 16);
}

BOOL VirtualFree(void *address, SIZE_T size, DWORD type)
{
    (void)size;

    // decommit leaves the reservation in place, so only a release hands memory back
    if (!(type & MEM_RELEASE))
        return 1;

    VitaMem_Free(address);
    return 1;
}

}

extern "C" {

HWND GetActiveWindow(void)
{
    return (HWND)1;         // non-null, since the engine only tests it for focus
}

BOOL MessageBoxA(HWND owner, const char *text, const char *caption, unsigned int type)
{
    (void)owner;
    (void)text;
    (void)caption;
    (void)type;
    return 0;
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
