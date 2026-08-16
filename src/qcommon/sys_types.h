// The Win32 scalar types the engine names outside src/win32, for other targets.
#pragma once

#if defined(KISAK_VITA) || !defined(_WIN32)

#include <stdint.h>

#if !defined(_WINDEF_) && !defined(MSS_H)
typedef int BOOL;
typedef int INT;
typedef float FLOAT;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned short WORD;
typedef char CHAR;
typedef short SHORT;
typedef CHAR *LPSTR, *PSTR;
typedef const CHAR *LPCSTR, *PCSTR;
typedef void *LPVOID;
typedef unsigned char BYTE, UCHAR, *LPBYTE;
typedef unsigned short USHORT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
// pointer-sized as unsigned long, as windef.h has it; uintptr_t is a distinct type on arm
typedef unsigned long ULONG_PTR, UINT_PTR, DWORD_PTR, SIZE_T;
typedef long LONG_PTR, INT_PTR;
typedef const void *LPCVOID;
typedef int INT32;
typedef unsigned int UINT32;
#endif

typedef unsigned long DWORD;    // as windef.h has it, not uint32_t
typedef void *HANDLE;

typedef struct tagPOINT { LONG x; LONG y; } POINT;

typedef void (*LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, struct _OVERLAPPED *);

typedef struct _OVERLAPPED
{
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _OSVERSIONINFOA
{
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFO;

typedef void *PVOID, *LPVOID_ALIAS, *HGLOBAL, *HLOCAL;
typedef unsigned int WPARAM;
typedef long LPARAM;
typedef long LRESULT;
#define CALLBACK
#define WINAPI
#define CONST const

// distinct incomplete types, matching windef.h and what snd_public.h already declares
#if !defined(_WINDEF_) && !defined(MSS_H)
typedef struct HWND__ *HWND;
typedef struct HINSTANCE__ *HINSTANCE;
typedef struct HDC__ *HDC;
#endif

typedef union _LARGE_INTEGER
{
    struct { DWORD LowPart; long HighPart; };
    long long QuadPart;
} LARGE_INTEGER;

#endif
