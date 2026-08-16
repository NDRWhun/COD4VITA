// Timing, in place of the TSC and performance counter the engine calibrates against.
#pragma once

#include <qcommon/sys_types.h>

extern "C" {

// the kernel's microsecond process clock stands in for the cycle counter
unsigned long long __rdtsc(void);

BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);

// the hunk reserves then commits; the kernel has no reserve, so a reserve allocates
void *VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void *address, SIZE_T size, DWORD type);

// there is one window and it is always active
HWND GetActiveWindow(void);
BOOL MessageBoxA(HWND owner, const char *text, const char *caption, unsigned int type);

}

unsigned int VitaSys_Milliseconds(void);
