#include "vita_threads.h"

#include "vita_sys.h"
#include "vita_system.h"

#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define VITA_MAX_THREADS        16
#define VITA_MAX_EVENTS         64
#define VITA_THREAD_LOCAL_SLOTS 8
#define VITA_THREAD_STACK       (256 * 1024)

#define VITA_EVENT_SIGNAL       1u

// three user CPUs, and priority 128-191 is the queue they share
#define VITA_USER_CPUS          3
#define VITA_PRIORITY_NORMAL    160
#define VITA_PRIORITY_STEP      8

struct VitaEvent
{
    SceUID flag;
    bool manualReset;
    bool inUse;
};

struct VitaThread
{
    SceUID uid;
    SceUID resumeFlag;          // the gate a cooperative suspend parks the thread on
    DWORD (*start)(void *);
    void *parameter;
    void *localSlots[VITA_THREAD_LOCAL_SLOTS];
    bool suspendRequested;
    bool started;
    bool inUse;
};

static VitaEvent s_events[VITA_MAX_EVENTS];
static VitaThread s_threads[VITA_MAX_THREADS];
static SceUID s_lock = -1;

static void VitaThreads_Enter(void)
{
    if (s_lock >= 0)
        sceKernelLockMutex(s_lock, 1, NULL);
}

static void VitaThreads_Leave(void)
{
    if (s_lock >= 0)
        sceKernelUnlockMutex(s_lock, 1);
}

static VitaThread *VitaThreads_Current(void)
{
    const SceUID self = sceKernelGetThreadId();
    for (uint32_t i = 0; i < VITA_MAX_THREADS; ++i)
    {
        if (s_threads[i].inUse && s_threads[i].uid == self)
            return &s_threads[i];
    }
    return NULL;
}

bool VitaThreads_Init(void)
{
    memset(s_events, 0, sizeof(s_events));
    memset(s_threads, 0, sizeof(s_threads));

    s_lock = sceKernelCreateMutex("kcod_threads", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    if (s_lock < 0)
        return false;

    // the calling thread gets an entry so it has local slots like the rest
    s_threads[0].uid = sceKernelGetThreadId();
    s_threads[0].inUse = true;
    s_threads[0].started = true;
    s_threads[0].resumeFlag = -1;
    return true;
}

void VitaThreads_CheckSuspend(void)
{
    VitaThread *thread = VitaThreads_Current();
    if (!thread || !thread->suspendRequested || thread->resumeFlag < 0)
        return;

    // park until resumed; the flag is cleared by the waiter so it re-arms
    unsigned int pattern = 0;
    sceKernelWaitEventFlag(thread->resumeFlag, VITA_EVENT_SIGNAL,
                           SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                           &pattern, NULL);
    thread->suspendRequested = false;
}

void **VitaThreads_LocalSlots(void)
{
    VitaThread *thread = VitaThreads_Current();
    if (thread)
        return thread->localSlots;

    // a thread the engine never registered still needs somewhere to look
    static void *orphan[VITA_THREAD_LOCAL_SLOTS];
    return orphan;
}

HANDLE VitaThreads_CurrentHandle(void)
{
    return (HANDLE)VitaThreads_Current();
}

uint32_t VitaThreads_CpuCount(void)
{
    return VITA_USER_CPUS;
}

extern "C" {

HANDLE CreateEventA(void *attributes, BOOL manualReset, BOOL initialState, const char *name)
{
    (void)attributes;
    (void)name;

    VitaThreads_Enter();
    for (uint32_t i = 0; i < VITA_MAX_EVENTS; ++i)
    {
        if (s_events[i].inUse)
            continue;

        const int attr = SCE_EVENT_WAITMULTIPLE;
        s_events[i].flag = sceKernelCreateEventFlag("kcod_event", attr,
                                                    initialState ? VITA_EVENT_SIGNAL : 0, NULL);
        if (s_events[i].flag < 0)
        {
            VitaThreads_Leave();
            return NULL;
        }
        s_events[i].manualReset = manualReset != 0;
        s_events[i].inUse = true;
        VitaThreads_Leave();
        return (HANDLE)&s_events[i];
    }
    VitaThreads_Leave();
    return NULL;
}

BOOL SetEvent(HANDLE event)
{
    VitaEvent *self = (VitaEvent *)event;
    if (!self || !self->inUse)
        return 0;
    return sceKernelSetEventFlag(self->flag, VITA_EVENT_SIGNAL) >= 0;
}

BOOL ResetEvent(HANDLE event)
{
    VitaEvent *self = (VitaEvent *)event;
    if (!self || !self->inUse)
        return 0;
    return sceKernelClearEventFlag(self->flag, ~VITA_EVENT_SIGNAL) >= 0;
}

DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds)
{
    VitaEvent *self = (VitaEvent *)object;
    if (!self || !self->inUse)
        return WAIT_FAILED;

    VitaThreads_CheckSuspend();

    // an auto-reset event consumes its signal, a manual-reset one leaves it set
    unsigned int wait = SCE_EVENT_WAITOR;
    if (!self->manualReset)
        wait |= SCE_EVENT_WAITCLEAR;

    unsigned int pattern = 0;
    SceUInt timeout = milliseconds * 1000;
    const int result = sceKernelWaitEventFlag(self->flag, VITA_EVENT_SIGNAL, wait, &pattern,
                                              milliseconds == INFINITE ? NULL : &timeout);
    if (result < 0)
        return milliseconds == INFINITE ? WAIT_FAILED : WAIT_TIMEOUT;
    return WAIT_OBJECT_0;
}

BOOL CloseHandle(HANDLE object)
{
    if (VitaSys_IsFileHandle(object))
        return VitaSys_CloseFile(object);

    VitaThreads_Enter();
    for (uint32_t i = 0; i < VITA_MAX_EVENTS; ++i)
    {
        if ((HANDLE)&s_events[i] == object && s_events[i].inUse)
        {
            sceKernelDeleteEventFlag(s_events[i].flag);
            memset(&s_events[i], 0, sizeof(s_events[i]));
            VitaThreads_Leave();
            return 1;
        }
    }
    VitaThreads_Leave();
    return 0;
}

static int VitaThreads_Entry(SceSize argSize, void *argBlock)
{
    (void)argSize;
    VitaThread *thread = *(VitaThread **)argBlock;
    thread->start(thread->parameter);
    return 0;
}

HANDLE CreateThread(void *attributes, SIZE_T stackSize, DWORD (*start)(void *),
                    void *parameter, DWORD flags, DWORD *threadId)
{
    (void)attributes;

    VitaThreads_Enter();
    for (uint32_t i = 1; i < VITA_MAX_THREADS; ++i)
    {
        if (s_threads[i].inUse)
            continue;

        VitaThread *thread = &s_threads[i];
        memset(thread, 0, sizeof(*thread));
        thread->start = start;
        thread->parameter = parameter;

        thread->resumeFlag = sceKernelCreateEventFlag("kcod_resume", SCE_EVENT_WAITMULTIPLE,
                                                      0, NULL);
        thread->uid = sceKernelCreateThread("kcod_thread", VitaThreads_Entry,
                                            VITA_PRIORITY_NORMAL,
                                            stackSize ? (SceSize)stackSize : VITA_THREAD_STACK,
                                            0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
        if (thread->uid < 0 || thread->resumeFlag < 0)
        {
            if (thread->resumeFlag >= 0)
                sceKernelDeleteEventFlag(thread->resumeFlag);
            VitaThreads_Leave();
            return NULL;
        }

        thread->inUse = true;
        if (threadId)
            *threadId = (DWORD)thread->uid;

        // a thread is dormant until started, which is what CREATE_SUSPENDED means here
        if (!(flags & CREATE_SUSPENDED))
        {
            VitaThread *argument = thread;
            const int started = sceKernelStartThread(thread->uid, sizeof(argument), &argument);
            if (started < 0)
                VitaSys_LogPrintf("CreateThread: sceKernelStartThread failed 0x%08x\n",
                                  (unsigned)started);
            thread->started = started >= 0;
        }

        VitaThreads_Leave();
        return (HANDLE)thread;
    }
    VitaThreads_Leave();
    return NULL;
}

DWORD ResumeThread(HANDLE handle)
{
    VitaThread *thread = (VitaThread *)handle;
    if (!thread || !thread->inUse)
        return (DWORD)-1;

    VitaThreads_Enter();
    if (!thread->started)
    {
        VitaThread *argument = thread;
        const int started = sceKernelStartThread(thread->uid, sizeof(argument), &argument);
        if (started < 0)
            VitaSys_LogPrintf("ResumeThread: sceKernelStartThread(0x%08x) failed 0x%08x\n",
                              (unsigned)thread->uid, (unsigned)started);
        thread->started = started >= 0;
    }
    else
    {
        thread->suspendRequested = false;
        sceKernelSetEventFlag(thread->resumeFlag, VITA_EVENT_SIGNAL);
    }
    VitaThreads_Leave();
    return 0;
}

DWORD SuspendThread(HANDLE handle)
{
    VitaThread *thread = (VitaThread *)handle;
    if (!thread || !thread->inUse)
        return (DWORD)-1;

    // the kernel has no arbitrary suspend, so the thread parks at its next wait
    thread->suspendRequested = true;
    sceKernelClearEventFlag(thread->resumeFlag, ~VITA_EVENT_SIGNAL);
    return 0;
}

BOOL SetThreadPriority(HANDLE handle, int priority)
{
    VitaThread *thread = (VitaThread *)handle;
    if (!thread || !thread->inUse)
        return 0;

    // lower numbers run first, and staying in 128-191 keeps threads on the shared queue
    int value = VITA_PRIORITY_NORMAL - priority * VITA_PRIORITY_STEP;
    if (value < 128)
        value = 128;
    if (value > 191)
        value = 191;
    return sceKernelChangeThreadPriority(thread->uid, value) >= 0;
}

DWORD_PTR SetThreadAffinityMask(HANDLE handle, DWORD_PTR mask)
{
    VitaThread *thread = (VitaThread *)handle;
    if (!thread || !thread->inUse)
        return 0;

    int affinity = 0;
    if (mask & 1)
        affinity |= SCE_KERNEL_CPU_MASK_USER_0;
    if (mask & 2)
        affinity |= SCE_KERNEL_CPU_MASK_USER_1;
    if (mask & 4)
        affinity |= SCE_KERNEL_CPU_MASK_USER_2;
    if (!affinity)
        affinity = SCE_KERNEL_CPU_MASK_USER_ALL;

    return sceKernelChangeThreadCpuAffinityMask(thread->uid, affinity) >= 0 ? mask : 0;
}

DWORD GetCurrentThreadId(void)
{
    return (DWORD)sceKernelGetThreadId();
}

void Sleep(DWORD milliseconds)
{
    // the scheduler does not time-slice equal priorities, so a zero sleep must still yield
    sceKernelDelayThread(milliseconds ? milliseconds * 1000 : 100);
}

}

void VitaThreads_Shutdown(void)
{
    for (uint32_t i = 0; i < VITA_MAX_EVENTS; ++i)
    {
        if (s_events[i].inUse)
            sceKernelDeleteEventFlag(s_events[i].flag);
    }
    for (uint32_t i = 1; i < VITA_MAX_THREADS; ++i)
    {
        if (!s_threads[i].inUse)
            continue;
        if (s_threads[i].resumeFlag >= 0)
            sceKernelDeleteEventFlag(s_threads[i].resumeFlag);
        sceKernelDeleteThread(s_threads[i].uid);
    }
    memset(s_events, 0, sizeof(s_events));
    memset(s_threads, 0, sizeof(s_threads));

    if (s_lock >= 0)
    {
        sceKernelDeleteMutex(s_lock);
        s_lock = -1;
    }
}
