// MSVC keywords and intrinsics the engine uses, for targets built with GCC.
#pragma once

#if defined(KISAK_VITA) || !defined(_MSC_VER)

#include <stdint.h>

#define __cdecl
#define __stdcall
#define __fastcall
#define __forceinline inline __attribute__((always_inline))

// every use in the tree is align(N)
#define __declspec(x) __declspec_##x
#define __declspec_align(n) __attribute__((aligned(n)))

#define __debugbreak() __builtin_trap()

#include <stdio.h>
#include <strings.h>
#include <time.h>
#define _iobuf __FILE

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

#define _vsnprintf vsnprintf
#define _snprintf  snprintf
#define _stricmp   strcasecmp
#define _time64    time
#define __time64_t time_t
#define _strnicmp  strncasecmp

#define _itoa(value, buffer, radix) \
    ((void)(radix), sprintf((buffer), "%d", (int)(value)), (buffer))

// the secure variants return an error code and take the handle by pointer
#define fopen_s(handle, name, mode) \
    ((*(handle) = fopen((name), (mode))) ? 0 : 1)

// macros rather than typedefs, since the tree writes 'unsigned __int64'
#define __int8  char
#define __int16 short
#define __int32 int
#define __int64 long long

// Win32 returns the new value for increment and decrement, as __sync_*_and_fetch does
#define InterlockedIncrement(p)              __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p)              __sync_sub_and_fetch((p), 1)
#define InterlockedExchangeAdd(p, v)         __sync_fetch_and_add((p), (v))
#define InterlockedExchange(p, v)            __sync_lock_test_and_set((p), (v))
#define InterlockedExchangePointer(p, v)     __sync_lock_test_and_set((p), (v))
#define InterlockedCompareExchange(p, x, c)  __sync_val_compare_and_swap((p), (c), (x))

#endif
