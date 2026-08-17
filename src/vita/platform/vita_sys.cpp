#include "vita_sys.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "vita_memory.h"
#include "vita_system.h"
#include "vita_threads.h"

extern "C" unsigned int _newlib_heap_size_user;

#include <sys/stat.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

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

    // Win32 hands back page-aligned, zero-filled pages and the engine relies on both
    void *memory = memalign(4096, (size_t)size);
    if (memory)
        memset(memory, 0, (size_t)size);
    else
    {
        const struct mallinfo heap = mallinfo();
        VitaSys_LogPrintf("VirtualAlloc failed: wanted %u KB, heap %u KB used of %u KB\n",
                          (unsigned)((size_t)size / 1024), (unsigned)(heap.uordblks / 1024),
                          (unsigned)(_newlib_heap_size_user / 1024));
        VitaSys_LogFlush();
    }
    return memory;
}

BOOL VirtualFree(void *address, SIZE_T size, DWORD type)
{
    (void)size;

    // decommit leaves the reservation in place, so only a release hands memory back
    if (!(type & MEM_RELEASE))
        return 1;

    free(address);
    return 1;
}

}

extern "C" {

HWND GetActiveWindow(void)
{
    return (HWND)1;         // non-null, since the engine only tests it for focus
}

DWORD timeGetTime(void)
{
    return VitaSys_Milliseconds();
}

DWORD SleepEx(DWORD milliseconds, BOOL alertable)
{
    (void)alertable;        // nothing queues APCs on this target
    Sleep(milliseconds);
    return 0;
}

unsigned char _BitScanReverse(unsigned long *index, unsigned long mask)
{
    if (!mask)
        return 0;
    *index = 31 - (unsigned long)__builtin_clzl(mask);
    return 1;
}

DWORD GetFileAttributesA(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0)
        return 0xFFFFFFFFu;                 // INVALID_FILE_ATTRIBUTES
    return S_ISDIR(info.st_mode) ? 0x10u : 0x80u;
}

BOOL SetFileAttributesA(const char *path, DWORD attributes)
{
    (void)path;
    (void)attributes;       // there is no read-only bit to set here
    return 1;
}

// a tag keeps file handles disjoint from the event handles CloseHandle also takes
#define VITA_FILE_HANDLE_TAG 0x40000000u

static int VitaSys_Descriptor(HANDLE file)
{
    return (int)((uintptr_t)file & (uintptr_t)0xFFFF);
}

HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security,
                   DWORD creation, DWORD flags, HANDLE templateFile)
{
    (void)share;
    (void)security;
    (void)flags;
    (void)templateFile;

    const bool wants_write = (access & 0x40000000u) != 0;       // GENERIC_WRITE
    const bool wants_read = (access & 0x80000000u) != 0;        // GENERIC_READ

    int mode = wants_write ? (wants_read ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (creation == 2 || creation == 1)                         // CREATE_ALWAYS, CREATE_NEW
        mode |= O_CREAT | O_TRUNC;
    else if (creation == 4)                                     // OPEN_ALWAYS
        mode |= O_CREAT;

    char normalized[512];
    uint32_t i = 0;
    for (; path[i] && i + 1 < sizeof(normalized); ++i)
        normalized[i] = path[i] == '\\' ? '/' : path[i];
    normalized[i] = 0;

    const int descriptor = open(normalized, mode, 0777);
    if (descriptor < 0)
        return INVALID_HANDLE_VALUE;
    return (HANDLE)(uintptr_t)((unsigned int)descriptor | VITA_FILE_HANDLE_TAG);
}

BOOL ReadFile(HANDLE file, void *buffer, DWORD count, DWORD *read, OVERLAPPED *overlapped)
{
    (void)overlapped;
    const ssize_t got = ::read(VitaSys_Descriptor(file), buffer, count);
    if (read)
        *read = got < 0 ? 0 : (DWORD)got;
    return got >= 0;
}

BOOL WriteFile(HANDLE file, const void *buffer, DWORD count, DWORD *written,
               OVERLAPPED *overlapped)
{
    (void)overlapped;
    const ssize_t put = ::write(VitaSys_Descriptor(file), buffer, count);
    if (written)
        *written = put < 0 ? 0 : (DWORD)put;
    return put >= 0;
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER move, LARGE_INTEGER *newPosition, DWORD from)
{
    const int whence = from == FILE_CURRENT ? SEEK_CUR : from == FILE_END ? SEEK_END : SEEK_SET;
    const off_t position = lseek(VitaSys_Descriptor(file), (off_t)move.QuadPart, whence);
    if (position < 0)
        return 0;
    if (newPosition)
        newPosition->QuadPart = position;
    return 1;
}

BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER *size)
{
    struct stat info;
    if (fstat(VitaSys_Descriptor(file), &info) != 0)
        return 0;
    if (size)
        size->QuadPart = info.st_size;
    return 1;
}

BOOL DeleteFileA(const char *path)
{
    return unlink(path) == 0;
}

DWORD GetFileSize(HANDLE file, DWORD *sizeHigh)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size))
        return 0xFFFFFFFFu;                 // INVALID_FILE_SIZE
    if (sizeHigh)
        *sizeHigh = (DWORD)((unsigned long long)size.QuadPart >> 32);
    return (DWORD)size.QuadPart;
}

BOOL GetProcessAffinityMask(HANDLE process, DWORD_PTR *processMask, DWORD_PTR *systemMask)
{
    (void)process;
    // one bit per user CPU, which is what the caller walks to build its per-thread masks
    const DWORD_PTR mask = (DWORD_PTR)((1u << VitaThreads_CpuCount()) - 1u);
    if (processMask)
        *processMask = mask;
    if (systemMask)
        *systemMask = mask;
    return 1;
}

DWORD GetLastError(void)
{
    return (DWORD)errno;
}

HANDLE GetCurrentProcess(void)
{
    return (HANDLE)1;
}

HANDLE GetCurrentThread(void)
{
    // a thread handle here is the registered VitaThread, not a kernel uid
    return VitaThreads_CurrentHandle();
}

BOOL OpenClipboard(HWND owner)
{
    (void)owner;
    return 0;
}

BOOL CloseClipboard(void)
{
    return 0;
}

BOOL EmptyClipboard(void)
{
    return 0;
}

HANDLE SetClipboardData(unsigned int format, HANDLE data)
{
    (void)format;
    (void)data;
    return NULL;
}

HANDLE GetClipboardData(unsigned int format)
{
    (void)format;
    return NULL;
}

HWND GetDesktopWindow(void)
{
    return (HWND)1;
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

BOOL VitaSys_ReadAt(HANDLE file, void *buffer, DWORD count, unsigned long long offset, DWORD *read)
{
    const int descriptor = VitaSys_Descriptor(file);
    if (lseek(descriptor, (off_t)offset, SEEK_SET) < 0)
        return 0;
    const ssize_t got = ::read(descriptor, buffer, count);
    if (read)
        *read = got < 0 ? 0 : (DWORD)got;
    return got >= 0;
}

BOOL VitaSys_IsFileHandle(HANDLE object)
{
    return ((uintptr_t)object & ~(uintptr_t)0xFFFF) == VITA_FILE_HANDLE_TAG;
}

BOOL VitaSys_CloseFile(HANDLE object)
{
    return close(VitaSys_Descriptor(object)) == 0;
}

unsigned int VitaSys_Milliseconds(void)
{
    static unsigned long long base;
    const unsigned long long now = sceKernelGetProcessTimeWide();
    if (!base)
        base = now;
    return (unsigned int)((now - base) / 1000ull);
}
