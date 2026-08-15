// GXM library init, rendering context, and the display buffer rotation.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#define GXM_SCREEN_WIDTH    960
#define GXM_SCREEN_HEIGHT   544
#define GXM_DISPLAY_BUFFERS 3

bool GxmDevice_Init(void);
void GxmDevice_Shutdown(void);

// scene bracket; every draw belongs to one of these
void GxmDevice_BeginFrame(void);
void GxmDevice_EndFrame(void);

SceGxmContext *GxmDevice_Context(void);
SceGxmShaderPatcher *GxmDevice_ShaderPatcher(void);

// blocks until the GPU has retired everything submitted so far
void GxmDevice_Finish(void);

uint32_t GxmDevice_FrameIndex(void);
