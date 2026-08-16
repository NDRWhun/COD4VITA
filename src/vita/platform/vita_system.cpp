#include "vita_system.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// newlib hides fileno under -std=c++20, but the symbol is in the library
extern "C" int fileno(FILE *);
#include <sys/stat.h>

#include <universal/q_shared.h>

#include <win32/win_local.h>
#include <win32/win_localize.h>
#include <win32/win_net_debug.h>

#include <gfx_d3d/r_init.h>
#include <qcommon/cmd.h>
#include <qcommon/com_fileaccess.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <stringed/stringed_hooks.h>
#include <universal/com_memory.h>
#include <universal/profile.h>
#include <universal/q_parse.h>
#include <universal/timing.h>

#include "vita_errorscreen.h"
#include "vita_memory.h"
#include "../input/vita_input.h"

// --- the boot log, which is the only console this target has ---

static FILE *s_log;
static bool s_logLineFlush = true;

static void VitaSys_MakeParentDirs(const char *path)
{
    char partial[256];
    uint32_t length = 0;

    for (; path[length] && length + 1 < sizeof(partial); ++length)
    {
        partial[length] = path[length];
        if (path[length] != '/' || length == 0)
            continue;
        partial[length] = 0;
        mkdir(partial, 0777);
        partial[length] = '/';
    }
}

bool VitaSys_LogOpen(const char *path)
{
    VitaSys_MakeParentDirs(path);
    s_log = fopen(path, "w");
    return s_log != NULL;
}

void VitaSys_LogClose(void)
{
    // cleared first, so a print racing the shutdown finds no stream rather than a closed one
    FILE *file = s_log;
    if (!file)
        return;
    s_log = NULL;
    fclose(file);
}

static void VitaSys_LogFlushLine(void);

void VitaSys_LogPrint(const char *text)
{
    if (!s_log || !text)
        return;

    // a timestamp per line is what tells a stall apart from a crash after the fact
    static bool atLineStart = true;
    if (atLineStart)
        fprintf(s_log, "[%8u] ", (unsigned)(sceKernelGetProcessTimeWide() / 1000));
    const size_t length = strlen(text);
    atLineStart = length && text[length - 1] == '\n';

    fputs(text, s_log);
    if (s_logLineFlush)
        VitaSys_LogFlushLine();
}

void VitaSys_LogPrintf(const char *format, ...)
{
    char text[2048];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    VitaSys_LogPrint(text);
}

void VitaSys_Breadcrumb(const char *format, ...)
{
    char text[512];
    va_list arguments;

    va_start(arguments, format);
    const int length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;

    // a fresh open/write/close per call, because a buffered stream loses its tail when the app dies
    const SceUID file = sceIoOpen("ux0:data/kisakcod/step.txt",
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (file < 0)
        return;
    sceIoWrite(file, text, (SceSize)length);
    sceIoClose(file);
}

void VitaSys_LogFlush(void)
{
    if (!s_log)
        return;
    fflush(s_log);
    // fflush only leaves libc; the filesystem cache still loses the tail on an abnormal exit
    const int descriptor = fileno(s_log);
    if (descriptor >= 0)
        sceIoSyncByFd(descriptor, 0);
}

// the per-line path: a card sync costs milliseconds, so it runs at most ten times a second
static void VitaSys_LogFlushLine(void)
{
    if (!s_log)
        return;
    fflush(s_log);

    static uint64_t lastSync;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (lastSync && now - lastSync < 100000)
        return;
    lastSync = now;

    const int descriptor = fileno(s_log);
    if (descriptor >= 0)
        sceIoSyncByFd(descriptor, 0);
}

void VitaSys_LogSetLineFlush(bool enabled)
{
    s_logLineFlush = enabled;
    if (enabled)
        VitaSys_LogFlush();
}

// --- fatal reporting ---

static volatile uint32_t s_fatalCount;

// CRITSECT_COM_ERROR is a plain mutex on this target, so a fatal path must not wait on it
static void VitaSys_ClaimFatal(void)
{
    if (InterlockedIncrement(&s_fatalCount) == 1)
        return;

    // a second thread reporting would interleave with the first; the process is ending either way
    for (;;)
        Sleep(1000);
}

void VitaSys_Fatal(const char *title, const char *message)
{
    VitaSys_LogSetLineFlush(true);
    VitaSys_LogPrintf("\n======== %s ========\n%s\n", title, message);

    if (!VitaErrorScreen_Show(title, message, "Log: " VITA_LOG_PATH "  -  press X to quit"))
        VitaSys_LogPrint("the error screen could not take a framebuffer\n");

    VitaSys_LogClose();
    sceKernelExitProcess(0);
    for (;;)
        Sleep(1000);
}

// --- system information, in place of win_configure's cpuid probing ---

SysInfo sys_info;

static volatile float s_benchmarkSink;

// upstream's chaotic-map loop, timed against the same counter its constant was fitted to
static long double VitaSys_BenchmarkGHz(void)
{
    const float k = 2.5999999f;
    unsigned long long minTime = ~0ull;

    for (uint32_t attempt = 0; attempt < 1000; ++attempt)
    {
        Sleep(0);
        const unsigned long long start = __rdtsc();
        int holdrand = 0;
        float x = 0.25f;
        float y = 0.75f;
        for (uint32_t i = 0; i < 1000; ++i)
        {
            const float xa = (1.0f - x) * x * k + x;
            const float ya = (1.0f - y) * y * k + y;
            x = (1.0f - xa) * xa * k + xa;
            y = (1.0f - ya) * ya * k + ya;
            if (i & 1)
                holdrand = 0x343FD * (0x343FD * (0x343FD * holdrand + 0x269EC3) + 0x269EC3)
                         + 0x269EC3;
        }
        const unsigned long long elapsed = __rdtsc() - start;

        // the results are dead after the loop, and without a sink the loop is dead with them
        s_benchmarkSink = x + y + (float)holdrand;
        if (elapsed < minTime)
            minTime = elapsed;
    }

    if (!minTime)
        return 0.0;
    return 0.1010328 / ((double)minTime * (double)msecPerRawTimerTick);
}

static int VitaSys_SystemMemoryMB(void)
{
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    if (sceKernelGetFreeMemorySize(&info) < 0)
        return 0;

    // the configure table tops out at 1 GB, as the win32 path also caps
    const int megabytes = info.size_user / (1024 * 1024);
    return megabytes > 1024 ? 1024 : megabytes;
}

void VitaSys_FindInfo(void)
{
    memset(&sys_info, 0, sizeof(sys_info));

    sys_info.logicalCpuCount = Sys_GetCpuCount();
    sys_info.physicalCpuCount = sys_info.logicalCpuCount;       // three cores, none shared

    const int armMHz = scePowerGetArmClockFrequency();
    sys_info.cpuGHz = armMHz > 0 ? armMHz / 1000.0 : 0.0;

    const double multiCpuFactor = sys_info.physicalCpuCount >= 3 ? 2.0
                                : sys_info.physicalCpuCount == 2 ? 1.75
                                : 1.0;
    sys_info.configureGHz = VitaSys_BenchmarkGHz() * multiCpuFactor;

    sys_info.sysMB = VitaSys_SystemMemoryMB();
    sys_info.SSE = false;                                       // ARM has NEON, not SSE

    I_strncpyz(sys_info.cpuVendor, "ARM", sizeof(sys_info.cpuVendor));
    Com_sprintf(sys_info.cpuName, sizeof(sys_info.cpuName), "Cortex-A9 MPCore %i MHz", armMHz);
    I_strncpyz(sys_info.gpuDescription, "PowerVR SGX543MP4+", sizeof(sys_info.gpuDescription));
}

// --- localization, which picks the zone subdirectory the fastfiles load from ---

static LocalizationData s_localization;
static char s_languageBuffer[0x1000];

char *__cdecl Win_CopyLocalizationString(const char *string)
{
    return va("%s", string);
}

char *__cdecl Win_GetLanguage()
{
    return s_localization.language ? s_localization.language : (char *)"english";
}

// localization.txt is one line of language name followed by the reference/value pairs
int __cdecl Win_InitLocalization()
{
    char path[256];
    int language = 0;

    s_localization.language = NULL;
    s_localization.strings = NULL;

    Com_sprintf(path, sizeof(path), "%s/localization.txt", Sys_Cwd());
    FILE *file = FS_FileOpenReadText(path);
    if (!file)
    {
        VitaSys_LogPrintf("localization: %s is absent, falling back to english\n", path);
        return 0;
    }

    const int size = FS_FileGetFileSize(file);
    if (size <= 0 || size >= (int)sizeof(s_languageBuffer))
    {
        VitaSys_LogPrintf("localization: %s is %i bytes, which does not fit\n", path, size);
        FS_FileClose(file);
        return 0;
    }

    const int read = (int)FS_FileRead(s_languageBuffer, size, file);
    FS_FileClose(file);
    if (read <= 0)
    {
        VitaSys_LogPrintf("localization: %s read back empty\n", path);
        return 0;
    }

    s_languageBuffer[read] = 0;
    s_localization.language = s_languageBuffer;
    for (int i = 0; s_languageBuffer[i]; ++i)
    {
        if (s_languageBuffer[i] != '\n')
            continue;
        s_languageBuffer[i] = 0;
        s_localization.strings = &s_languageBuffer[i + 1];
        SEH_GetLanguageIndexForName(s_localization.language, &language);
        break;
    }

    VitaSys_LogPrintf("localization: language \"%s\", index %i\n", s_localization.language,
                      language);
    return language;
}

char *__cdecl Win_LocalizeRef(const char *ref)
{
    if (!s_localization.strings)
        return Win_CopyLocalizationString(ref);

    Com_BeginParseSession("localization");
    const char *strings = s_localization.strings;
    for (;;)
    {
        const char *token = Com_Parse(&strings)->token;
        if (!*token)
            break;
        const bool matched = strcmp(token, ref) == 0;

        token = Com_Parse(&strings)->token;
        if (!*token)
            break;
        if (matched)
        {
            Com_EndParseSession();
            return Win_CopyLocalizationString(token);
        }
    }

    Com_EndParseSession();
    VitaSys_LogPrintf("localization: unlocalized reference \"%s\"\n", ref);
    return Win_CopyLocalizationString(ref);
}

void __cdecl Win_ShutdownLocalization()
{
    s_localization.language = NULL;
    s_localization.strings = NULL;
}

// --- the event queue, fed by the pad rather than by a window message pump ---

static sysEvent_t s_eventQueue[MAX_QUED_EVENTS];
static int s_eventHead;
static int s_eventTail;

void __cdecl Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2,
                          int ptrLength, void *ptr)
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    sysEvent_t *ev = &s_eventQueue[(unsigned char)s_eventHead];
    if (s_eventHead - s_eventTail >= MAX_QUED_EVENTS)
    {
        Com_Printf(16, "Sys_QueEvent: overflow\n");
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
        ++s_eventTail;
    }
    ++s_eventHead;

    ev->evTime = time ? time : Sys_Milliseconds();
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;

    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_ShutdownEvents()
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    while (s_eventHead > s_eventTail)
    {
        sysEvent_t *ev = &s_eventQueue[(unsigned char)s_eventTail++];
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

static void VitaSys_KeyEvent(int key, bool down)
{
    Sys_QueEvent(0, SE_KEY, key, down, 0, NULL);
}

static void VitaSys_CharEvent(int character)
{
    Sys_QueEvent(0, SE_CHAR, character, 0, 0, NULL);
}

sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result)
{
    PROF_SCOPED("Sys_GetEvent");

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    const bool empty = s_eventHead <= s_eventTail;
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    // polled outside the section, since the input callbacks queue through it
    if (empty)
        VitaInput_Frame();

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    if (s_eventHead > s_eventTail)
    {
        *result = s_eventQueue[(unsigned char)s_eventTail++];
    }
    else
    {
        memset(result, 0, sizeof(*result));
        result->evTime = Sys_Milliseconds();
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);

    return result;
}

void __cdecl Sys_LoadingKeepAlive()
{
    sysEvent_t result;

    do
    {
        Sys_GetEvent(&result);
    } while (result.evType);

    R_CheckLostDevice();
}

// --- bring-up and teardown ---

cmd_function_s Sys_In_Restart_f_VAR;

void Sys_In_Restart_f()
{
    VitaInput_Shutdown();
    VitaInput_Init(VitaSys_KeyEvent, VitaSys_CharEvent);
}

void __cdecl Sys_Init()
{
    Cmd_AddCommandInternal("in_restart", Sys_In_Restart_f, &Sys_In_Restart_f_VAR);

    Com_Printf(16, "CPU vendor is \"%s\"\n", sys_info.cpuVendor);
    Com_Printf(16, "CPU name is \"%s\"\n", sys_info.cpuName);
    Com_Printf(16, "%i logical CPU%s reported\n", sys_info.logicalCpuCount,
               sys_info.logicalCpuCount == 1 ? "" : "s");
    Com_Printf(16, "%i physical CPU%s detected\n", sys_info.physicalCpuCount,
               sys_info.physicalCpuCount == 1 ? "" : "s");
    Com_Printf(16, "Measured CPU speed is %.2lf GHz\n", (double)sys_info.cpuGHz);
    Com_Printf(16, "Total CPU performance is estimated as %.2lf GHz\n",
               (double)sys_info.configureGHz);
    Com_Printf(16, "System memory is %i MB (capped at 1 GB)\n", sys_info.sysMB);
    Com_Printf(16, "Video card is \"%s\"\n", sys_info.gpuDescription);
    Com_Printf(16, "Streaming SIMD Extensions (SSE) not supported\n");
    Com_Printf(16, "\n");

    if (!VitaInput_Init(VitaSys_KeyEvent, VitaSys_CharEvent))
        Com_Error(ERR_FATAL, "Couldn't initialize the pad");
}

void __cdecl Sys_NormalExit()
{
    // upstream drops its crash semaphore here; this target records the clean exit instead
    VitaSys_LogPrint("--- normal exit ---\n");
    VitaSys_LogFlush();
}

// Com_Quit_f still holds CRITSECT_COM_ERROR here, and this target's sections do not nest
void __cdecl Sys_Quit()
{
    VitaSys_LogSetLineFlush(true);
    VitaInput_Shutdown();
    Key_Shutdown();
    Sys_NormalExit();
    Win_ShutdownLocalization();
    RefreshQuitOnErrorCondition();
    Dvar_Shutdown();
    Cmd_Shutdown();
    Sys_ShutdownEvents();
    SL_Shutdown();
    if (!com_errorEntered)
        track_shutdown(0);
    Con_ShutdownChannels();

    VitaSys_LogClose();
    sceKernelExitProcess(0);
}

void Sys_Error(const char *error, ...)
{
    char string[4096];
    va_list arguments;

    va_start(arguments, error);
    vsnprintf(string, sizeof(string), error, arguments);
    va_end(arguments);

    VitaSys_ClaimFatal();
    com_errorEntered = 1;
    Com_PrintStackTrace();
    Sys_SuspendOtherThreads();
    VitaSys_Fatal("Fatal error", string);
}

void __cdecl Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    char string[512];

    VitaSys_ClaimFatal();
    com_errorEntered = 1;
    Sys_SuspendOtherThreads();

    VitaMemStats mainArena;
    VitaMemStats cdramArena;
    VitaMem_GetStats(VITA_MEM_MAIN, &mainArena);
    VitaMem_GetStats(VITA_MEM_CDRAM, &cdramArena);

    Com_sprintf(string, sizeof(string),
                "Out of memory at %s line %i.  Main arena %u KB reserved, %u KB used, largest free "
                "run %u KB; CDRAM %u KB reserved, %u KB used.",
                filename, line, mainArena.reserved / 1024, mainArena.used / 1024,
                mainArena.largestFreeRun / 1024, cdramArena.reserved / 1024,
                cdramArena.used / 1024);

    VitaSys_Fatal("Out of memory", string);
}

void __cdecl Sys_Print(const char *msg)
{
    VitaSys_LogPrint(msg);
}

void __cdecl Sys_OpenURL(const char *url, int doexit)
{
    // there is no browser handoff from a homebrew process, so the address is only recorded
    Com_Printf(16, "shellExecute: %s\n", url ? url : "");
    if (doexit)
        Cbuf_AddText(0, "quit\n");
}

// --- clipboard, which this target does not have ---

char *__cdecl Sys_GetClipboardData()
{
    return NULL;
}

int __cdecl Sys_SetClipboardData(const char *text)
{
    (void)text;
    return 0;
}

// --- the script debugger's socket to a PC dev kit, which is not reachable from here ---

int __cdecl Sys_IsRemoteDebugClient()
{
    return 0;
}

int __cdecl Sys_UpdateDebugSocket()
{
    return 0;
}

void __cdecl Sys_AckDebugSocket()
{
}

void __cdecl Sys_FlushDebugSocketData()
{
}

int __cdecl Sys_ReadDebugSocketData(char *buffer, int len, int blocking)
{
    (void)blocking;
    if (buffer && len > 0)
        memset(buffer, 0, len);
    return 0;
}

int __cdecl Sys_ReadDebugSocketInt()
{
    return 0;
}

char *__cdecl Sys_ReadDebugSocketString()
{
    static char empty[1];
    empty[0] = 0;
    return empty;
}

void __cdecl Sys_ReadDebugSocketStringBuffer(char *buffer, int len)
{
    if (buffer && len > 0)
        buffer[0] = 0;
}

void __cdecl Sys_WriteDebugSocketInt(int value)
{
    (void)value;
}

void __cdecl Sys_WriteDebugSocketMessageType(unsigned __int8 type)
{
    (void)type;
}

void __cdecl Sys_WriteDebugSocketString(char *text)
{
    (void)text;
}

void __cdecl Sys_EndWriteDebugSocket()
{
}
