#include "gxm_device.h"
#include "gxm_memory.h"
#include "gxm_rendertarget.h"
#include "../platform/vita_errorscreen.h"
#include "../platform/vita_memory.h"

#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <stdlib.h>
#include <string.h>

// what the display queue callback receives; kept small, the queue copies it
struct GxmDisplayData
{
    void *address;
};

struct GxmDisplayBuffer
{
    GxmAlloc mem;
    SceGxmColorSurface surface;
    SceGxmSyncObject *sync;
};

struct GxmDevice
{
    SceGxmContext *context;
    GxmAlloc contextHostMem;        // plain malloc'd, not GPU-mapped
    GxmAlloc vdmRing;
    GxmAlloc vertexRing;
    GxmAlloc fragmentRing;
    GxmAlloc fragmentUsseRing;

    SceGxmRenderTarget *renderTarget;

    GxmDisplayBuffer display[GXM_DISPLAY_BUFFERS];
    uint32_t backBufferIndex;
    uint32_t frontBufferIndex;

    GxmAlloc depthMem;
    SceGxmDepthStencilSurface depthSurface;

    SceGxmShaderPatcher *shaderPatcher;
    GxmAlloc patcherBuffer;
    GxmAlloc patcherVertexUsse;
    GxmAlloc patcherFragmentUsse;

    SceGxmNotification sceneNotification;
    uint32_t fenceValue;

    uint32_t frameIndex;
    bool initialized;
};

static GxmDevice gxmDev;

// runs on the display queue thread once the GPU has finished the frame
static void GxmDevice_DisplayCallback(const void *callbackData)
{
    const GxmDisplayData *data = (const GxmDisplayData *)callbackData;

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = data->address;
    fb.pitch = GXM_SCREEN_WIDTH;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = GXM_SCREEN_WIDTH;
    fb.height = GXM_SCREEN_HEIGHT;

    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();
}

static void *GxmDevice_PatcherHostAlloc(void *userData, uint32_t size)
{
    (void)userData;
    return malloc(size);
}

static void GxmDevice_PatcherHostFree(void *userData, void *mem)
{
    (void)userData;
    free(mem);
}

static bool GxmDevice_InitLibrary(void)
{
    SceGxmInitializeParams params;
    memset(&params, 0, sizeof(params));
    params.flags = 0;
    params.displayQueueMaxPendingCount = GXM_DISPLAY_BUFFERS - 1;
    params.displayQueueCallback = GxmDevice_DisplayCallback;
    params.displayQueueCallbackDataSize = sizeof(GxmDisplayData);
    params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;

    return sceGxmInitialize(&params) >= 0;
}

static bool GxmDevice_CreateContext(void)
{
    SceGxmContextParams params;
    memset(&params, 0, sizeof(params));

    params.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    if (!params.hostMem)
        return false;
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    gxmDev.contextHostMem.base = params.hostMem;

    if (!GxmMem_Alloc(&gxmDev.vdmRing, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
                      GXM_MEM_MAIN, SCE_GXM_MEMORY_ATTRIB_READ))
        return false;
    if (!GxmMem_Alloc(&gxmDev.vertexRing, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
                      GXM_MEM_MAIN, SCE_GXM_MEMORY_ATTRIB_READ))
        return false;
    if (!GxmMem_Alloc(&gxmDev.fragmentRing, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
                      GXM_MEM_MAIN, SCE_GXM_MEMORY_ATTRIB_READ))
        return false;
    if (!GxmMem_AllocFragmentUsse(&gxmDev.fragmentUsseRing,
                                  SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE))
        return false;

    params.vdmRingBufferMem = gxmDev.vdmRing.base;
    params.vdmRingBufferMemSize = gxmDev.vdmRing.size;
    params.vertexRingBufferMem = gxmDev.vertexRing.base;
    params.vertexRingBufferMemSize = gxmDev.vertexRing.size;
    params.fragmentRingBufferMem = gxmDev.fragmentRing.base;
    params.fragmentRingBufferMemSize = gxmDev.fragmentRing.size;
    params.fragmentUsseRingBufferMem = gxmDev.fragmentUsseRing.base;
    params.fragmentUsseRingBufferMemSize = gxmDev.fragmentUsseRing.size;
    params.fragmentUsseRingBufferOffset = gxmDev.fragmentUsseRing.usseOffset;

    return sceGxmCreateContext(&params, &gxmDev.context) >= 0;
}

static bool GxmDevice_CreateRenderTarget(void)
{
    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = GXM_SCREEN_WIDTH;
    params.height = GXM_SCREEN_HEIGHT;
    params.scenesPerFrame = GXM_SCENES_PER_FRAME;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;

    return sceGxmCreateRenderTarget(&params, &gxmDev.renderTarget) >= 0;
}

static bool GxmDevice_CreateDisplayBuffers(void)
{
    const uint32_t size = GXM_SCREEN_WIDTH * GXM_SCREEN_HEIGHT * 4;

    for (uint32_t i = 0; i < GXM_DISPLAY_BUFFERS; ++i)
    {
        GxmDisplayBuffer *buf = &gxmDev.display[i];

        if (!GxmMem_Alloc(&buf->mem, size, GXM_MEM_CDRAM,
                          SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE))
            return false;

        memset(buf->mem.base, 0, size);

        if (sceGxmColorSurfaceInit(&buf->surface,
                                   SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                   SCE_GXM_COLOR_SURFACE_LINEAR,
                                   SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                   SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                   GXM_SCREEN_WIDTH, GXM_SCREEN_HEIGHT,
                                   GXM_SCREEN_WIDTH, buf->mem.base) < 0)
            return false;

        if (sceGxmSyncObjectCreate(&buf->sync) < 0)
            return false;
    }
    return true;
}

static bool GxmDevice_CreateDepthBuffer(void)
{
    // depth is sampled per tile-aligned sample, so both axes round up to the 32x32 tile grid
    const uint32_t alignedWidth = (GXM_SCREEN_WIDTH + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const uint32_t alignedHeight = (GXM_SCREEN_HEIGHT + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    const uint32_t samples = alignedWidth * alignedHeight;

    if (!GxmMem_Alloc(&gxmDev.depthMem, samples * 4, GXM_MEM_MAIN,
                      SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE))
        return false;

    return sceGxmDepthStencilSurfaceInit(&gxmDev.depthSurface,
                                         SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                         SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                         alignedWidth, gxmDev.depthMem.base, NULL) >= 0;
}

static bool GxmDevice_CreateShaderPatcher(void)
{
    const uint32_t bufferSize = 1024 * 1024;
    const uint32_t vertexUsseSize = 512 * 1024;
    const uint32_t fragmentUsseSize = 512 * 1024;

    if (!GxmMem_Alloc(&gxmDev.patcherBuffer, bufferSize, GXM_MEM_MAIN,
                      SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE))
        return false;
    if (!GxmMem_AllocVertexUsse(&gxmDev.patcherVertexUsse, vertexUsseSize))
        return false;
    if (!GxmMem_AllocFragmentUsse(&gxmDev.patcherFragmentUsse, fragmentUsseSize))
        return false;

    SceGxmShaderPatcherParams params;
    memset(&params, 0, sizeof(params));
    params.hostAllocCallback = GxmDevice_PatcherHostAlloc;
    params.hostFreeCallback = GxmDevice_PatcherHostFree;
    params.bufferMem = gxmDev.patcherBuffer.base;
    params.bufferMemSize = gxmDev.patcherBuffer.size;
    params.vertexUsseMem = gxmDev.patcherVertexUsse.base;
    params.vertexUsseMemSize = gxmDev.patcherVertexUsse.size;
    params.vertexUsseOffset = gxmDev.patcherVertexUsse.usseOffset;
    params.fragmentUsseMem = gxmDev.patcherFragmentUsse.base;
    params.fragmentUsseMemSize = gxmDev.patcherFragmentUsse.size;
    params.fragmentUsseOffset = gxmDev.patcherFragmentUsse.usseOffset;

    return sceGxmShaderPatcherCreate(&params, &gxmDev.shaderPatcher) >= 0;
}

bool GxmDevice_Init(void)
{
    memset(&gxmDev, 0, sizeof(gxmDev));

    if (!GxmDevice_InitLibrary())
        return false;

    // pooled arenas map each block once, so every texture inside one is GPU-visible
    // render targets are written by the GPU, so a read-only mapping would not carry them
    VitaMem_SetGpuMapping(VITA_MEM_CDRAM, true,
                          SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    VitaMem_SetGpuMapping(VITA_MEM_MAIN_UNCACHED, true,
                          SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!GxmDevice_CreateContext())
        return false;
    if (!GxmDevice_CreateRenderTarget())
        return false;
    if (!GxmDevice_CreateDisplayBuffers())
        return false;
    if (!GxmDevice_CreateDepthBuffer())
        return false;
    if (!GxmDevice_CreateShaderPatcher())
        return false;

    gxmDev.backBufferIndex = 0;
    gxmDev.frontBufferIndex = GXM_DISPLAY_BUFFERS - 1;
    gxmDev.sceneNotification.address = sceGxmGetNotificationRegion();
    gxmDev.sceneNotification.value = 0;
    // the region keeps whatever the last process left there, and both fence tests read it
    if (gxmDev.sceneNotification.address)
        *gxmDev.sceneNotification.address = 0;
    gxmDev.initialized = true;
    return true;
}

void GxmDevice_BeginFrame(void)
{
    GxmRenderTarget_ResetCounters();
    GxmRenderTarget_Begin(GxmRenderTarget_Display());
}

void GxmDevice_EndFrame(void)
{
    GxmDisplayBuffer *back = &gxmDev.display[gxmDev.backBufferIndex];

    // the frame must land on the back buffer however many targets the scene switcher visited
    GxmRenderTarget_Begin(GxmRenderTarget_Display());
    GxmRenderTarget_End();

    // the system composites its dialogs (the IME among them) into the buffer being handed over
    SceCommonDialogUpdateParam dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    dialog.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    dialog.renderTarget.width = GXM_SCREEN_WIDTH;
    dialog.renderTarget.height = GXM_SCREEN_HEIGHT;
    dialog.renderTarget.strideInPixels = GXM_SCREEN_WIDTH;
    dialog.renderTarget.colorSurfaceData = back->mem.base;
    dialog.displaySyncObject = back->sync;
    sceCommonDialogUpdate(&dialog);

    sceGxmPadHeartbeat(&back->surface, back->sync);

    GxmDisplayData data;
    data.address = back->mem.base;

    // the boot screen keeps the display until the first real frame is queued here
    VitaBootScreen_Disable();

    // the queue waits on the new buffer's fence and releases the one leaving the screen
    sceGxmDisplayQueueAddEntry(gxmDev.display[gxmDev.frontBufferIndex].sync, back->sync, &data);

    gxmDev.frontBufferIndex = gxmDev.backBufferIndex;
    gxmDev.backBufferIndex = (gxmDev.backBufferIndex + 1) % GXM_DISPLAY_BUFFERS;
    ++gxmDev.frameIndex;
}

void GxmDevice_Finish(void)
{
    if (gxmDev.context)
        sceGxmFinish(gxmDev.context);
}

const void *GxmDevice_FrontBuffer(uint32_t *width, uint32_t *height, uint32_t *pitchInPixels)
{
    if (!gxmDev.initialized)
        return NULL;

    GxmDevice_Finish();

    if (width)
        *width = GXM_SCREEN_WIDTH;
    if (height)
        *height = GXM_SCREEN_HEIGHT;
    if (pitchInPixels)
        *pitchInPixels = GXM_SCREEN_WIDTH;

    return gxmDev.display[gxmDev.frontBufferIndex].mem.base;
}

void GxmDevice_Shutdown(void)
{
    if (!gxmDev.initialized)
        return;

    GxmDevice_Finish();
    sceGxmDisplayQueueFinish();

    sceGxmShaderPatcherDestroy(gxmDev.shaderPatcher);
    GxmMem_Free(&gxmDev.patcherFragmentUsse);
    GxmMem_Free(&gxmDev.patcherVertexUsse);
    GxmMem_Free(&gxmDev.patcherBuffer);

    GxmMem_Free(&gxmDev.depthMem);

    for (uint32_t i = 0; i < GXM_DISPLAY_BUFFERS; ++i)
    {
        sceGxmSyncObjectDestroy(gxmDev.display[i].sync);
        GxmMem_Free(&gxmDev.display[i].mem);
    }

    sceGxmDestroyRenderTarget(gxmDev.renderTarget);
    sceGxmDestroyContext(gxmDev.context);

    GxmMem_Free(&gxmDev.fragmentUsseRing);
    GxmMem_Free(&gxmDev.fragmentRing);
    GxmMem_Free(&gxmDev.vertexRing);
    GxmMem_Free(&gxmDev.vdmRing);
    free(gxmDev.contextHostMem.base);

    sceGxmTerminate();
    memset(&gxmDev, 0, sizeof(gxmDev));
}

SceGxmContext *GxmDevice_Context(void)
{
    return gxmDev.context;
}

SceGxmShaderPatcher *GxmDevice_ShaderPatcher(void)
{
    return gxmDev.shaderPatcher;
}

SceGxmRenderTarget *GxmDevice_DisplayTarget(void)
{
    return gxmDev.renderTarget;
}

const SceGxmColorSurface *GxmDevice_BackBufferSurface(void)
{
    return &gxmDev.display[gxmDev.backBufferIndex].surface;
}

const SceGxmDepthStencilSurface *GxmDevice_DisplayDepthSurface(void)
{
    return &gxmDev.depthSurface;
}

SceGxmSyncObject *GxmDevice_BackBufferSync(void)
{
    return gxmDev.display[gxmDev.backBufferIndex].sync;
}

const SceGxmNotification *GxmDevice_SceneNotification(void)
{
    ++gxmDev.sceneNotification.value;
    return &gxmDev.sceneNotification;
}

void GxmDevice_IssueFence(void)
{
    // everything queued so far; one more would wait on the next frame
    gxmDev.fenceValue = gxmDev.sceneNotification.value;
}

bool GxmDevice_FenceReached(void)
{
    if (!gxmDev.sceneNotification.address)
        return true;

    // the value rises by one per scene, so a signed difference survives the wrap
    return (int32_t)(*gxmDev.sceneNotification.address - gxmDev.fenceValue) >= 0;
}

uint32_t GxmDevice_ScenesSubmitted(void)
{
    return gxmDev.sceneNotification.value;
}

uint32_t GxmDevice_ScenesRetired(void)
{
    return gxmDev.sceneNotification.address ? *gxmDev.sceneNotification.address : 0;
}

uint32_t GxmDevice_FrameIndex(void)
{
    return gxmDev.frameIndex;
}
