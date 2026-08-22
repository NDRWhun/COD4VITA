#include <stdio.h>
#include <string.h>

#include <universal/q_shared.h>

#include <win32/win_local.h>
#include <win32/win_localize.h>

#include <qcommon/mem_track.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <universal/profile.h>
#include <universal/q_parse.h>
#include <universal/timing.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "vita_memory.h"
#include "vita_selftest.h"
#include "vita_system.h"
#include "vita_threads.h"

extern "C" unsigned int _newlib_heap_size_user;

#define COMMAND_LINE_PATH "ux0:data/kisakcod/cmdline.txt"

static char sys_cmdline[1024];

// the console has no command line, so one is read from the data root when present
static void VitaMain_ReadCommandLine(void)
{
    FILE *file = fopen(COMMAND_LINE_PATH, "r");
    if (!file)
        return;

    const size_t read = fread(sys_cmdline, 1, sizeof(sys_cmdline) - 1, file);
    fclose(file);
    sys_cmdline[read] = 0;

    for (size_t i = 0; i < read; ++i)
    {
        if (sys_cmdline[i] == '\r' || sys_cmdline[i] == '\n')
            sys_cmdline[i] = ' ';
    }
    VitaSys_LogPrintf("command line: %s\n", sys_cmdline);
}

static void VitaMain_RunSelfTests(void)
{
    char report[256];

    VitaSelfTest_Memory(report, sizeof(report));
    VitaSys_LogPrintf("memory:  %s", report);
    VitaSelfTest_Threads(report, sizeof(report));
    VitaSys_LogPrintf("threads: %s", report);
    VitaSelfTest_Files(report, sizeof(report));
    VitaSys_LogPrintf("files:   %s", report);
    VitaSelfTest_Clocks(report, sizeof(report));
    VitaSys_LogPrintf("clocks:  %s", report);
}

extern int com_frameTime;

// a hang tells the log whether the main thread is spinning or blocked
static int VitaMain_Watchdog(SceSize args, void *argp)
{
    (void)args; (void)argp;
    const SceUID mainUid = *(SceUID *)argp;
    int last = 0;
    unsigned stalled = 0;
    SceUInt64 lastClocks = 0;
    while (1)
    {
        sceKernelDelayThread(2 * 1000 * 1000);
        if (com_frameTime != last)
        {
            last = com_frameTime;
            stalled = 0;
            continue;
        }
        if (!last)
            continue;
        ++stalled;
        SceKernelThreadInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.size = sizeof(ti);
        sceKernelGetThreadInfo(mainUid, &ti);
        VitaSys_LogPrintf("watchdog: main stalled %us at frame time %i, status %i, "
                          "ranClocks +%llu\n",
                          stalled * 2, last, ti.status,
                          (unsigned long long)(ti.runClocks - lastClocks));
        lastClocks = ti.runClocks;
    }
    return 0;
}

int main(void)
{
    VitaSys_LogOpen(VITA_LOG_PATH);
    // rate-limited in VitaSys_LogFlushLine, so boot pays a handful of writes rather than hundreds
    VitaSys_LogSetLineFlush(true);
    VitaSys_LogPrintf("KisakCOD single player, Vita, built %s %s\n", __DATE__, __TIME__);
    // the module moves every run, so every %p logged below resolves against this one
    VitaSys_LogPrintf("module: main at %p\n", (const void *)&main);

    if (!VitaMem_Init())
        VitaSys_Fatal("Boot failed", "The memory arenas could not be created.");

    // must precede Sys_InitMainThread, which registers the calling thread through this table
    if (!VitaThreads_Init())
        VitaSys_Fatal("Boot failed", "The thread table could not be created.");

    VitaMain_RunSelfTests();

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();

    SceUID watchdogMain = sceKernelGetThreadId();
    SceUID watchdog = sceKernelCreateThread("kcod_watchdog", VitaMain_Watchdog, 191,
                                            16 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (watchdog >= 0)
        sceKernelStartThread(watchdog, sizeof(watchdogMain), &watchdogMain);
    track_init();
    Win_InitLocalization();

    Com_InitParse();
    Dvar_Init();
    InitTiming();
    VitaSys_FindInfo();
    // configureGHz is upstream's benchmark score, not a clock, so neither figure is printed here
    VitaSys_LogPrintf("hardware: %i cpus, %i MB, gpu \"%s\"\n",
                      sys_info.logicalCpuCount, sys_info.sysMB, sys_info.gpuDescription);

    // every partition the process is allowed, so the split is read off a run rather than guessed
    SceKernelFreeMemorySizeInfo budget;
    memset(&budget, 0, sizeof(budget));
    budget.size = sizeof(budget);
    if (sceKernelGetFreeMemorySize(&budget) >= 0)
        VitaSys_LogPrintf("budget: user %u KB free, cdram %u KB, phycont %u KB, heap %u KB taken\n",
                          (unsigned)(budget.size_user / 1024), (unsigned)(budget.size_cdram / 1024),
                          (unsigned)(budget.size_phycont / 1024),
                          (unsigned)(_newlib_heap_size_user / 1024));

    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);

    VitaMain_ReadCommandLine();
    VitaSys_LogPrint("--- Com_Init ---\n");
    Com_Init(sys_cmdline);

    Com_Printf(16, "Working directory: %s\n", Sys_Cwd());
    VitaSys_LogPrint("--- entering the frame loop ---\n");

    // the card is free again once boot is done, so the whole trail reaches it here
    VitaSys_LogFlush();

    for (;;)
        Com_Frame();
}
