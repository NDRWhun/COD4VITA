// Offscreen render targets, and the scene bracket that switches between them.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_memory.h"

// GXM sizes a render target's driver memory for this many scenes per frame
#define GXM_SCENES_PER_FRAME 8

// an offscreen target is rendered into once or twice, so it must not reserve the display's budget
#define GXM_SCENES_PER_TARGET 2

struct GxmDepthStencil
{
    SceGxmDepthStencilSurface surface;
    GxmAlloc memory;
    uint32_t width;
    uint32_t height;
};

struct GxmRenderTarget
{
    SceGxmRenderTarget *target;
    SceGxmColorSurface color;
    GxmAlloc colorMem;
    SceGxmTexture texture;
    const SceGxmDepthStencilSurface *depth;     // may belong to another target
    uint32_t width;
    uint32_t height;
    uint32_t strideInPixels;
    uint32_t sceneCount;                        // scenes entered so far this frame
    uint32_t sceneBudget;                       // what its driver memory was sized for
    bool isDisplay;
};

// depth survives a target switch, so the surface is created with forced load and store
bool GxmDepthStencil_Create(GxmDepthStencil *depth, uint32_t width, uint32_t height);
void GxmDepthStencil_Free(GxmDepthStencil *depth);

bool GxmRenderTarget_Create(GxmRenderTarget *rt, uint32_t width, uint32_t height,
                            SceGxmColorFormat colorFormat, SceGxmTextureFormat textureFormat);
void GxmRenderTarget_Free(GxmRenderTarget *rt);

void GxmRenderTarget_SetDepth(GxmRenderTarget *rt, const GxmDepthStencil *depth);
const SceGxmTexture *GxmRenderTarget_Texture(const GxmRenderTarget *rt);

// the display buffer of the frame in flight, as R_RENDERTARGET_FRAME_BUFFER's surface
GxmRenderTarget *GxmRenderTarget_Display(void);
GxmDepthStencil *GxmRenderTarget_DisplayDepth(void);

// one scene at a time: entering a target ends the open scene and begins the target's
bool GxmRenderTarget_Begin(GxmRenderTarget *rt);
void GxmRenderTarget_End(void);

const GxmRenderTarget *GxmRenderTarget_Current(void);
bool GxmRenderTarget_SceneOpen(void);

// scenes begun after the per-frame budget ran out; non-zero means GXM_SCENES_PER_FRAME is low
uint32_t GxmRenderTarget_OverflowedScenes(void);
void GxmRenderTarget_ResetCounters(void);
