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

#include "vita_memory.h"
#include "vita_selftest.h"
#include "vita_system.h"
#include "vita_threads.h"

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

int main(void)
{
    VitaSys_LogOpen(VITA_LOG_PATH);
    VitaSys_LogPrintf("KisakCOD single player, Vita, built %s %s\n", __DATE__, __TIME__);

    if (!VitaMem_Init())
        VitaSys_Fatal("Boot failed", "The memory arenas could not be created.");

    // must precede Sys_InitMainThread, which registers the calling thread through this table
    if (!VitaThreads_Init())
        VitaSys_Fatal("Boot failed", "The thread table could not be created.");

    VitaMain_RunSelfTests();

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    track_init();
    Win_InitLocalization();

    Com_InitParse();
    Dvar_Init();
    InitTiming();
    VitaSys_FindInfo();
    VitaSys_LogPrintf("hardware: %i cpus, %.2f GHz measured, %.2f GHz configure, %i MB, gpu \"%s\"\n",
                      sys_info.logicalCpuCount, (double)sys_info.cpuGHz,
                      (double)sys_info.configureGHz, sys_info.sysMB, sys_info.gpuDescription);

    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);

    VitaMain_ReadCommandLine();
    VitaSys_LogPrint("--- Com_Init ---\n");
    Com_Init(sys_cmdline);

    Com_Printf(16, "Working directory: %s\n", Sys_Cwd());
    VitaSys_LogPrint("--- entering the frame loop ---\n");

    // boot is over, so the log stops paying a write per line and buffers like a file
    VitaSys_LogSetLineFlush(false);

    for (;;)
        Com_Frame();
}
