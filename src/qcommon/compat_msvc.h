// MSVC keywords and intrinsics the engine uses, for targets built with GCC.
#pragma once

#if defined(KISAK_VITA) || !defined(_MSC_VER)

#include <stdint.h>

#define __cdecl
#define __stdcall
#define __fastcall
#define __thiscall
#define __forceinline inline __attribute__((always_inline))

// every use in the tree is align(N)
#define __declspec(x) __declspec_##x
#define __declspec_align(n) __attribute__((aligned(n)))

#define __debugbreak() __builtin_trap()

// deps/ode/common.h routes alloca through _alloca
#define _alloca __builtin_alloca

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#define _iobuf __FILE

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

#define _vsnprintf vsnprintf
#define _snprintf  snprintf
#define _stricmp   strcasecmp
#define __time64_t time_t
#define _strnicmp  strncasecmp

// time_t is 32 bits here, so these narrow the tree's __int64 seconds
static inline long long _time64(long long *t) { const time_t v = time(0); if (t) *t = v; return v; }
static inline struct tm *_localtime64(const long long *t) { time_t v = (time_t)*t; return localtime(&v); }

// newlib hides strdup under -std=c++20
static inline char *_strdup(const char *s) { const size_t n = strlen(s) + 1; char *p = (char *)malloc(n); if (p) memcpy(p, s, n); return p; }

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
