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

DWORD timeGetTime(void);
DWORD SleepEx(DWORD milliseconds, BOOL alertable);
unsigned char _BitScanReverse(unsigned long *index, unsigned long mask);

// the file calls are real, over descriptors; a HANDLE carries the descriptor plus one
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

DWORD GetFileAttributesA(const char *path);
BOOL SetFileAttributesA(const char *path, DWORD attributes);
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security,
                   DWORD creation, DWORD flags, HANDLE templateFile);
BOOL ReadFile(HANDLE file, void *buffer, DWORD count, DWORD *read, OVERLAPPED *overlapped);
BOOL WriteFile(HANDLE file, const void *buffer, DWORD count, DWORD *written,
               OVERLAPPED *overlapped);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER move, LARGE_INTEGER *newPosition, DWORD from);
BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size);

DWORD GetLastError(void);
HANDLE GetCurrentProcess(void);
HANDLE GetCurrentThread(void);
HWND GetDesktopWindow(void);

// there is no clipboard, so the engine's copy paths do nothing
BOOL OpenClipboard(HWND owner);
BOOL CloseClipboard(void);
BOOL EmptyClipboard(void);
HANDLE SetClipboardData(unsigned int format, HANDLE data);
HANDLE GetClipboardData(unsigned int format);

}

unsigned int VitaSys_Milliseconds(void);
