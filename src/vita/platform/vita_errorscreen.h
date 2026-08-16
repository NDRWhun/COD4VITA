// A framebuffer text report, for failures that happen before or instead of the renderer.
#pragma once

// takes over the display with its own buffer, so it works whether or not GXM came up
bool VitaErrorScreen_Show(const char *title, const char *body, const char *footer);
