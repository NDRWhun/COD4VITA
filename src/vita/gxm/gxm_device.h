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

// the display surfaces for the frame in flight, for the scene switcher
SceGxmRenderTarget *GxmDevice_DisplayTarget(void);
const SceGxmColorSurface *GxmDevice_BackBufferSurface(void);
const SceGxmDepthStencilSurface *GxmDevice_DisplayDepthSurface(void);
SceGxmSyncObject *GxmDevice_BackBufferSync(void);

// every scene end stamps this rising value into notification memory
const SceGxmNotification *GxmDevice_SceneNotification(void);

// GPU fence: the issued value is reached once the GPU retires the scenes submitted before it
void GxmDevice_IssueFence(void);
bool GxmDevice_FenceReached(void);

// the same counters the fence compares, for the pool of fences the backend keeps in flight
uint32_t GxmDevice_ScenesSubmitted(void);
uint32_t GxmDevice_ScenesRetired(void);

// blocks until the GPU has retired everything submitted so far
void GxmDevice_Finish(void);

// the display buffer holding the last presented frame, R,G,B,A bytes; finishes the GPU first
const void *GxmDevice_FrontBuffer(uint32_t *width, uint32_t *height, uint32_t *pitchInPixels);

uint32_t GxmDevice_FrameIndex(void);
