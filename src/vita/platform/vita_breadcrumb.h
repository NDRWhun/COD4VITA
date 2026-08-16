// A one-line marker file, rewritten per call, that survives an abnormal exit.
#pragma once

void VitaSys_Breadcrumb(const char *format, ...);

// truncates the trail, so each run leaves only its own
void VitaSys_BreadcrumbReset(void);

// writes whatever has accumulated; call before ending the process
void VitaSys_BreadcrumbFlush(void);
