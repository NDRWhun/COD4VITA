// The Sys_* platform layer: the boot log, fatal reporting, the event queue and localization.
#pragma once

#define VITA_LOG_PATH "ux0:data/kisakcod/kisakcod.log"

// truncates the log, so each run leaves only its own evidence
bool VitaSys_LogOpen(const char *path);
void VitaSys_LogClose(void);

void VitaSys_LogPrint(const char *text);
void VitaSys_LogPrintf(const char *format, ...);
void VitaSys_LogFlush(void);

// every line is flushed while this is set, which costs a write per print
void VitaSys_LogSetLineFlush(bool enabled);

// overwrites a one-line marker file, opened and closed per call so a hard death cannot lose it
void VitaSys_Breadcrumb(const char *format, ...);

// fills sys_info from the hardware, in place of win_configure's cpuid probing
void VitaSys_FindInfo(void);

// reports to the log and the screen, then ends the process; never returns
void VitaSys_Fatal(const char *title, const char *message);
