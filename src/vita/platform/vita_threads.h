// Threads, events and thread-local slots on the Vita kernel.
#pragma once

#include <psp2/types.h>
#include <stdint.h>

#include <qcommon/sys_types.h>

#define WAIT_OBJECT_0       0x00000000
#define WAIT_TIMEOUT        0x00000102
#define WAIT_FAILED         0xFFFFFFFF
#define INFINITE            0xFFFFFFFF
#define CREATE_SUSPENDED    0x00000004

#define THREAD_PRIORITY_LOWEST          (-2)
#define THREAD_PRIORITY_BELOW_NORMAL    (-1)
#define THREAD_PRIORITY_NORMAL          0
#define THREAD_PRIORITY_ABOVE_NORMAL    1
#define THREAD_PRIORITY_HIGHEST         2

// the engine casts its thread entry to this before handing it to CreateThread
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);

bool VitaThreads_Init(void);
void VitaThreads_Shutdown(void);

// the engine's own names, so qcommon/threads.cpp keeps its logic
extern "C" {

HANDLE CreateEventA(void *attributes, BOOL manualReset, BOOL initialState, const char *name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds);
BOOL CloseHandle(HANDLE object);

void VitaThreads_SetNextName(const char *name);
HANDLE CreateThread(void *attributes, SIZE_T stackSize, DWORD (*start)(void *),
                    void *parameter, DWORD flags, DWORD *threadId);
DWORD SuspendThread(HANDLE thread);
DWORD ResumeThread(HANDLE thread);
BOOL SetThreadPriority(HANDLE thread, int priority);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
DWORD GetCurrentThreadId(void);
void Sleep(DWORD milliseconds);

}

// suspension is cooperative: it takes effect where the thread next waits
void VitaThreads_CheckSuspend(void);

// thread-local slots, keyed by thread rather than by compiler TLS
void **VitaThreads_LocalSlots(void);

// the registered handle for the calling thread
HANDLE VitaThreads_CurrentHandle(void);

uint32_t VitaThreads_CpuCount(void);
