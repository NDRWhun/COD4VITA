// Timing, in place of the TSC and performance counter the engine calibrates against.
#pragma once

#include <qcommon/sys_types.h>

extern "C" {

// the kernel's microsecond process clock stands in for the cycle counter
unsigned long long __rdtsc(void);

BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);

}

unsigned int VitaSys_Milliseconds(void);
