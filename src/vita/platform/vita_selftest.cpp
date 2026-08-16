// On-device checks for the allocator and the threading layer.

#include "vita_selftest.h"
#include "vita_files.h"
#include "vita_memory.h"
#include "vita_sys.h"
#include "vita_threads.h"

#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#define TEST_ALLOCATIONS 192

struct TestBlock
{
    uint8_t *pointer;
    uint32_t size;
    uint8_t pattern;
};

static uint32_t s_seed = 12345;

static uint32_t TestRandom(uint32_t limit)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (s_seed >> 16) % limit;
}

// every byte carries the block's pattern, so an overlap shows up as a mismatch
static bool TestVerify(const TestBlock *block)
{
    for (uint32_t i = 0; i < block->size; ++i)
    {
        if (block->pointer[i] != block->pattern)
            return false;
    }
    return true;
}

bool VitaSelfTest_Memory(char *report, uint32_t reportSize)
{
    TestBlock blocks[TEST_ALLOCATIONS];
    memset(blocks, 0, sizeof(blocks));

    VitaMemStats before;
    VitaMem_GetStats(VITA_MEM_MAIN, &before);

    uint32_t live = 0;
    uint32_t corrupted = 0;
    uint32_t misaligned = 0;
    uint32_t failed = 0;

    for (uint32_t round = 0; round < 4; ++round)
    {
        for (uint32_t i = 0; i < TEST_ALLOCATIONS; ++i)
        {
            if (blocks[i].pointer)
                continue;

            const uint32_t size = 16 + TestRandom(4096);
            const uint32_t alignment = 1u << (4 + TestRandom(4));   // 16 to 128
            uint8_t *pointer = (uint8_t *)VitaMem_Alloc(VITA_MEM_MAIN, size, alignment);
            if (!pointer)
            {
                ++failed;
                continue;
            }
            if ((uintptr_t)pointer & (alignment - 1))
                ++misaligned;

            blocks[i].pointer = pointer;
            blocks[i].size = size;
            blocks[i].pattern = (uint8_t)(i + round * 7 + 1);
            memset(pointer, blocks[i].pattern, size);
            ++live;
        }

        // free a scattered half, so the free list has to coalesce rather than stack up
        for (uint32_t i = 0; i < TEST_ALLOCATIONS; ++i)
        {
            if (!blocks[i].pointer || TestRandom(2))
                continue;
            if (!TestVerify(&blocks[i]))
                ++corrupted;
            VitaMem_Free(blocks[i].pointer);
            blocks[i].pointer = NULL;
            --live;
        }
    }

    for (uint32_t i = 0; i < TEST_ALLOCATIONS; ++i)
    {
        if (!blocks[i].pointer)
            continue;
        if (!TestVerify(&blocks[i]))
            ++corrupted;
        VitaMem_Free(blocks[i].pointer);
        blocks[i].pointer = NULL;
        --live;
    }

    VitaMemStats after;
    VitaMem_GetStats(VITA_MEM_MAIN, &after);

    const bool balanced = after.used == before.used &&
                          after.liveAllocations == before.liveAllocations;

    // freeing everything should leave one run per block, so a short one means a merge was missed
    const bool merged = after.blocks == 0 || after.largestFreeRun + 64 * 1024 >= after.reserved;
    const bool pass = !corrupted && !misaligned && !failed && balanced && merged;

    snprintf(report, reportSize,
             "memory: %s corrupt %u, misaligned %u, failed %u, live %u, "
             "used %u->%u, reserved %u KB, largest free %u KB, blocks %u\n",
             pass ? "PASS" : "FAIL", corrupted, misaligned, failed, live,
             before.used, after.used, after.reserved / 1024,
             after.largestFreeRun / 1024, after.blocks);

    return pass;
}

static HANDLE s_ping;
static HANDLE s_pong;
static volatile uint32_t s_counter;
static volatile bool s_workerRan;
static volatile bool s_slotsDistinct;
static void *s_mainSlots;

static DWORD TestWorker(void *parameter)
{
    (void)parameter;
    s_workerRan = true;
    s_slotsDistinct = VitaThreads_LocalSlots() != s_mainSlots;

    // ping-pong proves both directions of the event path, not just one
    for (uint32_t i = 0; i < 32; ++i)
    {
        if (WaitForSingleObject(s_ping, 2000) != WAIT_OBJECT_0)
            break;
        s_counter = s_counter + 1;
        SetEvent(s_pong);
    }
    return 0;
}

bool VitaSelfTest_Threads(char *report, uint32_t reportSize)
{
    s_counter = 0;
    s_workerRan = false;
    s_slotsDistinct = false;
    s_mainSlots = VitaThreads_LocalSlots();

    s_ping = CreateEventA(NULL, 0, 0, NULL);        // auto reset
    s_pong = CreateEventA(NULL, 0, 0, NULL);
    if (!s_ping || !s_pong)
    {
        snprintf(report, reportSize, "threads: FAIL could not create events\n");
        return false;
    }

    // created suspended, so nothing should run until it is resumed
    HANDLE worker = CreateThread(NULL, 0, TestWorker, NULL, CREATE_SUSPENDED, NULL);
    if (!worker)
    {
        snprintf(report, reportSize, "threads: FAIL could not create the thread\n");
        return false;
    }

    sceKernelDelayThread(20000);
    const bool stayedDormant = !s_workerRan;

    ResumeThread(worker);

    uint32_t exchanges = 0;
    for (uint32_t i = 0; i < 32; ++i)
    {
        SetEvent(s_ping);
        if (WaitForSingleObject(s_pong, 2000) != WAIT_OBJECT_0)
            break;
        ++exchanges;
    }

    // a manual-reset event stays signalled, so a second wait must return immediately
    HANDLE manual = CreateEventA(NULL, 1, 1, NULL);
    const bool manualFirst = WaitForSingleObject(manual, 100) == WAIT_OBJECT_0;
    const bool manualSecond = WaitForSingleObject(manual, 100) == WAIT_OBJECT_0;
    ResetEvent(manual);
    const bool manualCleared = WaitForSingleObject(manual, 50) == WAIT_TIMEOUT;

    // an auto-reset event consumes its signal, so the second wait must time out
    HANDLE automatic = CreateEventA(NULL, 0, 1, NULL);
    const bool autoFirst = WaitForSingleObject(automatic, 100) == WAIT_OBJECT_0;
    const bool autoCleared = WaitForSingleObject(automatic, 50) == WAIT_TIMEOUT;

    const bool pass = stayedDormant && s_workerRan && s_slotsDistinct &&
                      exchanges == 32 && s_counter == 32 &&
                      manualFirst && manualSecond && manualCleared &&
                      autoFirst && autoCleared;

    snprintf(report, reportSize,
             "threads: %s dormant %d, ran %d, own slots %d, exchanges %u/32, "
             "manual %d%d%d, auto %d%d, cpus %u\n",
             pass ? "PASS" : "FAIL", stayedDormant, s_workerRan, s_slotsDistinct, exchanges,
             manualFirst, manualSecond, manualCleared, autoFirst, autoCleared,
             VitaThreads_CpuCount());

    CloseHandle(manual);
    CloseHandle(automatic);
    CloseHandle(s_ping);
    CloseHandle(s_pong);
    return pass;
}

#define TEST_DIRECTORY "ux0:data/kisakcod/selftest"

static void TestWriteFile(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), TEST_DIRECTORY "/%s", name);
    FILE *file = fopen(path, "wb");
    if (file)
    {
        fwrite("kcod", 1, 4, file);
        fclose(file);
    }
}

bool VitaSelfTest_Files(char *report, uint32_t reportSize)
{
    _mkdir(TEST_DIRECTORY);

    TestWriteFile("alpha.ff");
    TestWriteFile("beta.ff");
    TestWriteFile("gamma.iwd");
    _mkdir(TEST_DIRECTORY "/subdir");

    // the engine writes specifications with backslashes, which must still open
    uint32_t fastfiles = 0;
    uint32_t everything = 0;
    uint32_t directories = 0;

    _finddata64i32_t entry;
    intptr_t handle = _findfirst64i32(TEST_DIRECTORY "\\*.ff", &entry);
    if (handle != -1)
    {
        do
        {
            ++fastfiles;
        } while (_findnext64i32(handle, &entry) == 0);
        _findclose(handle);
    }

    char seen[160];
    seen[0] = 0;

    handle = _findfirst64i32(TEST_DIRECTORY "/*", &entry);
    if (handle != -1)
    {
        do
        {
            ++everything;
            if (entry.attrib & _A_SUBDIR)
                ++directories;
            if (strlen(seen) + strlen(entry.name) + 2 < sizeof(seen))
            {
                strcat(seen, entry.name);
                strcat(seen, " ");
            }
        } while (_findnext64i32(handle, &entry) == 0);
        _findclose(handle);
    }

    char path[256];
    static const char *const written[] = { "alpha.ff", "beta.ff", "gamma.iwd" };
    for (uint32_t i = 0; i < 3; ++i)
    {
        snprintf(path, sizeof(path), TEST_DIRECTORY "/%s", written[i]);
        remove(path);
    }
    _rmdir(TEST_DIRECTORY "/subdir");
    _rmdir(TEST_DIRECTORY);

    // '.' and '..' come back from readdir, so the wildcard sees five entries
    const bool pass = fastfiles == 2 && everything >= 4 && directories >= 1;

    const unsigned int start = VitaSys_Milliseconds();
    sceKernelDelayThread(50000);
    const unsigned int elapsed = VitaSys_Milliseconds() - start;
    const bool clockSane = elapsed >= 45 && elapsed <= 80;

    snprintf(report, reportSize,
             "files: %s matched %u .ff, %u entries, %u dirs [%s]; clock %s (%u ms for 50)\n",
             (pass && clockSane) ? "PASS" : "FAIL", fastfiles, everything, directories,
             seen, clockSane ? "ok" : "BAD", elapsed);
    return pass && clockSane;
}
