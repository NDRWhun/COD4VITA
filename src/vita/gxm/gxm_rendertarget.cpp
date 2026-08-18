#include "gxm_rendertarget.h"
#include "gxm_device.h"
#include "gxm_pipeline.h"
#include <psp2/kernel/sysmem.h>
#include <vita/platform/vita_memory.h>
#include <vita/platform/vita_system.h>

#include <string.h>

#define GXM_MAX_RENDER_TARGETS 32

static GxmRenderTarget *s_registry[GXM_MAX_RENDER_TARGETS];
static uint32_t s_registryCount;

static GxmRenderTarget s_display;
static GxmDepthStencil s_displayDepth;
static bool s_displayDepthReady;

static GxmRenderTarget *s_current;
static uint32_t s_overflowedScenes;

static SceGxmDepthStencilSurface s_noDepth;
static bool s_noDepthReady;

static const SceGxmDepthStencilSurface *GxmRenderTarget_NoDepth(void)
{
    if (!s_noDepthReady)
    {
        sceGxmDepthStencilSurfaceInitDisabled(&s_noDepth);
        s_noDepthReady = true;
    }
    return &s_noDepth;
}

static void GxmRenderTarget_Register(GxmRenderTarget *rt)
{
    if (s_registryCount < GXM_MAX_RENDER_TARGETS)
        s_registry[s_registryCount++] = rt;
}

static void GxmRenderTarget_Unregister(GxmRenderTarget *rt)
{
    for (uint32_t i = 0; i < s_registryCount; ++i)
    {
        if (s_registry[i] != rt)
            continue;
        s_registry[i] = s_registry[--s_registryCount];
        return;
    }
}

// one SceGxmRenderTarget per size; the colour surface is a BeginScene argument
struct GxmSharedTarget
{
    SceGxmRenderTarget *target;
    uint32_t width;
    uint32_t height;
};

static GxmSharedTarget s_shared[GXM_MAX_RENDER_TARGETS];
static uint32_t s_sharedCount;
static uint32_t s_sharedDriverBytes;

static SceGxmRenderTarget *GxmRenderTarget_Shared(uint32_t width, uint32_t height)
{
    for (uint32_t i = 0; i < s_sharedCount; ++i)
    {
        if (s_shared[i].width == width && s_shared[i].height == height)
            return s_shared[i].target;
    }
    if (s_sharedCount >= GXM_MAX_RENDER_TARGETS)
        return NULL;

    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)width;
    params.height = (uint16_t)height;
    params.scenesPerFrame = GXM_SCENES_PER_FRAME;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;

    uint32_t driverBytes = 0;
    sceGxmGetRenderTargetMemSize(&params, &driverBytes);

    // the driver allocates this itself, so what is free beforehand is what decides the call
    VitaSys_LogPrintf("gxm target %ux%u: wants %u KB, free main %u KB cdram %u KB\n",
                      width, height, driverBytes / 1024, GxmMem_FreeMain() / 1024,
                      GxmMem_FreeCdram() / 1024);
    VitaSys_LogFlush();

    VitaMem_GpuLock();
    SceGxmRenderTarget *target = NULL;
    const int created = sceGxmCreateRenderTarget(&params, &target);
    VitaMem_GpuUnlock();

    s_sharedDriverBytes += driverBytes;
    VitaSys_LogPrintf("gxm target %ux%u: result 0x%08x, %u KB over %u targets, free main %u KB\n",
                      width, height, (unsigned)created, s_sharedDriverBytes / 1024,
                      s_sharedCount + 1, GxmMem_FreeMain() / 1024);
    VitaSys_LogFlush();

    if (created < 0)
        return NULL;

    s_shared[s_sharedCount].target = target;
    s_shared[s_sharedCount].width = width;
    s_shared[s_sharedCount].height = height;
    ++s_sharedCount;
    return target;
}

void GxmRenderTarget_ShutdownShared(void)
{
    VitaMem_GpuLock();
    for (uint32_t i = 0; i < s_sharedCount; ++i)
        sceGxmDestroyRenderTarget(s_shared[i].target);
    VitaMem_GpuUnlock();
    s_sharedCount = 0;
    s_sharedDriverBytes = 0;

    // a restart builds a new device, so nothing cached from the old one may be reused
    s_current = NULL;
    s_registryCount = 0;
    memset(s_registry, 0, sizeof(s_registry));
    memset(&s_display, 0, sizeof(s_display));
    s_displayDepthReady = false;
}

bool GxmDepthStencil_Create(GxmDepthStencil *depth, uint32_t width, uint32_t height)
{
    memset(depth, 0, sizeof(*depth));

    const uint32_t alignedWidth = (width + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const uint32_t alignedHeight = (height + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);

    VitaSys_LogPrintf("depth %ux%u: wants %u KB, free main %u KB cdram %u KB\n", width, height,
                      (alignedWidth * alignedHeight * 4) / 1024, GxmMem_FreeMain() / 1024,
                      GxmMem_FreeCdram() / 1024);
    VitaSys_LogFlush();

    if (!GxmMem_AllocPooled(&depth->memory, alignedWidth * alignedHeight * 4, GXM_MEM_CDRAM,
                            SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT))
        return false;

    if (sceGxmDepthStencilSurfaceInit(&depth->surface,
                                      SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                      alignedWidth, depth->memory.base, NULL) < 0)
    {
        GxmMem_Free(&depth->memory);
        return false;
    }

    // without these GXM discards depth at scene end, and the engine switches targets mid-frame
    sceGxmDepthStencilSurfaceSetForceLoadMode(&depth->surface,
                                              SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(&depth->surface,
                                               SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);

    depth->width = width;
    depth->height = height;
    return true;
}

void GxmDepthStencil_Free(GxmDepthStencil *depth)
{
    GxmMem_Free(&depth->memory);
    memset(depth, 0, sizeof(*depth));
}

bool GxmRenderTarget_Create(GxmRenderTarget *rt, uint32_t width, uint32_t height,
                            SceGxmColorFormat colorFormat, SceGxmTextureFormat textureFormat)
{
    memset(rt, 0, sizeof(*rt));

    // colour surfaces stride in multiples of 8 pixels
    const uint32_t stride = (width + 7) & ~7u;


    // pooled: a memblock and a GPU mapping per surface exhausts the kernel during target creation
    if (!GxmMem_AllocPooled(&rt->colorMem, stride * height * 4, GXM_MEM_CDRAM,
                            SCE_GXM_TEXTURE_ALIGNMENT))
    {
        return false;
    }

    if (sceGxmColorSurfaceInit(&rt->color, colorFormat,
                               SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                               width, height, stride, rt->colorMem.base) < 0)
    {
        GxmMem_Free(&rt->colorMem);
        return false;
    }

    if (sceGxmTextureInitLinearStrided(&rt->texture, rt->colorMem.base, textureFormat,
                                       width, height, stride * 4) < 0)
    {
        GxmMem_Free(&rt->colorMem);
        return false;
    }


    rt->target = GxmRenderTarget_Shared(width, height);
    if (!rt->target)
    {
        GxmMem_Free(&rt->colorMem);
        return false;
    }

    rt->width = width;
    rt->height = height;
    rt->strideInPixels = stride;
    rt->sceneBudget = GXM_SCENES_PER_FRAME;
    GxmRenderTarget_Register(rt);
    return true;
}

void GxmRenderTarget_Free(GxmRenderTarget *rt)
{
    if (s_current == rt)
        GxmRenderTarget_End();

    GxmRenderTarget_Unregister(rt);

    // the shared target outlives any one surface
    GxmMem_Free(&rt->colorMem);
    memset(rt, 0, sizeof(*rt));
}

void GxmRenderTarget_SetDepth(GxmRenderTarget *rt, const GxmDepthStencil *depth)
{
    rt->depth = depth ? &depth->surface : NULL;
}

const SceGxmTexture *GxmRenderTarget_Texture(const GxmRenderTarget *rt)
{
    return &rt->texture;
}

GxmDepthStencil *GxmRenderTarget_DisplayDepth(void)
{
    if (!s_displayDepthReady)
    {
        // the device owns the memory; this copy carries the load and store modes
        s_displayDepth.surface = *GxmDevice_DisplayDepthSurface();
        s_displayDepth.width = GXM_SCREEN_WIDTH;
        s_displayDepth.height = GXM_SCREEN_HEIGHT;
        sceGxmDepthStencilSurfaceSetForceLoadMode(&s_displayDepth.surface,
                                                  SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&s_displayDepth.surface,
                                                   SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        s_displayDepthReady = true;
    }
    return &s_displayDepth;
}

// every other target gets its texture at creation, but the display's colour is a back buffer
// that rotates each frame, so its view is rebuilt whenever the surface is
static void GxmRenderTarget_ViewDisplay(GxmRenderTarget *rt)
{
    void *base = sceGxmColorSurfaceGetData(&rt->color);
    if (!base)
        return;
    sceGxmTextureInitLinearStrided(&rt->texture, base, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
                                   rt->width, rt->height, rt->strideInPixels * 4);
}

GxmRenderTarget *GxmRenderTarget_Display(void)
{
    if (!s_display.isDisplay)
    {
        s_display.isDisplay = true;
        s_display.width = GXM_SCREEN_WIDTH;
        s_display.height = GXM_SCREEN_HEIGHT;
        s_display.strideInPixels = GXM_SCREEN_WIDTH;
        s_display.depth = &GxmRenderTarget_DisplayDepth()->surface;
        s_display.color = *GxmDevice_BackBufferSurface();
        GxmRenderTarget_ViewDisplay(&s_display);
        GxmRenderTarget_Register(&s_display);
    }
    return &s_display;
}

void GxmRenderTarget_End(void)
{
    if (!s_current)
        return;

    // the counter must only advance for a scene the GPU will actually retire
    const SceGxmNotification *notification = GxmDevice_SceneNotification();
    const int ended = sceGxmEndScene(GxmDevice_Context(), NULL, notification);
    if (ended < 0)
    {
        GxmDevice_CancelScene();
        VitaSys_LogPrintf("sceGxmEndScene failed 0x%08x\n", (unsigned)ended);
    }
    s_current = NULL;
}

bool GxmRenderTarget_Begin(GxmRenderTarget *rt)
{
    if (s_current == rt)
        return true;

    GxmRenderTarget_End();

    // the back buffer rotates every frame, so the display view is rebuilt on entry
    if (rt->isDisplay)
    {
        rt->target = GxmDevice_DisplayTarget();
        rt->color = *GxmDevice_BackBufferSurface();
        if (!rt->depth)
            rt->depth = &GxmRenderTarget_DisplayDepth()->surface;
        GxmRenderTarget_ViewDisplay(rt);
    }

    if (!rt->target)
        return false;

    if (++rt->sceneCount > (rt->sceneBudget ? rt->sceneBudget : GXM_SCENES_PER_FRAME))
        ++s_overflowedScenes;

    SceGxmSyncObject *sync = rt->isDisplay ? GxmDevice_BackBufferSync() : NULL;

    if (sceGxmBeginScene(GxmDevice_Context(), 0, rt->target, NULL, NULL, sync, &rt->color,
                         rt->depth ? rt->depth : GxmRenderTarget_NoDepth()) < 0)
        return false;

    GxmPipeline_SceneChanged();
    s_current = rt;
    return true;
}

const GxmRenderTarget *GxmRenderTarget_Current(void)
{
    return s_current;
}

bool GxmRenderTarget_SceneOpen(void)
{
    return s_current != NULL;
}

uint32_t GxmRenderTarget_OverflowedScenes(void)
{
    return s_overflowedScenes;
}

void GxmRenderTarget_ResetCounters(void)
{
    for (uint32_t i = 0; i < s_registryCount; ++i)
        s_registry[i]->sceneCount = 0;
    s_overflowedScenes = 0;
}
