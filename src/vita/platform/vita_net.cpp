#include <universal/q_shared.h>

#include <win32/win_net.h>
#include <win32/win_net_debug.h>

// the engine's spin loops and its frame limiter both yield through this, so it must really sleep
void NET_Sleep(int msec)
{
    Sleep(msec > 0 ? (DWORD)msec : 0);
}

// the script debugger's TCP link to a PC dev kit; no socket is ever opened on this target
void __cdecl NET_ShutdownDebug()
{
}

void NET_RestartDebug()
{
}
