#include "gxm_pipeline.h"
#include "gxm_buffer.h"
#include "gxm_clear_shaders.h"
#include "gxm_device.h"
#include "gxm_draw.h"
#include "gxm_program.h"
#include "gxm_rendertarget.h"
#include "gxm_shader_archive.h"
#include "gxm_state.h"

#include <vita/platform/vita_system.h>

#include <string.h>

#define GFXS0_POLYMODE_LINE 0x80000000u

#define CLEAR_TARGET  1
#define CLEAR_ZBUFFER 2
#define CLEAR_STENCIL 4

struct GxmSamplerUnit
{
    const SceGxmTexture *source;
    SceGxmTexture bound;            // per-unit copy: GXM keeps sampler state in the texture
    uint32_t samplerState;
    bool hasTexture;
    bool dirty;
};

struct GxmViewportState
{
    float xOffset, xScale;
    float yOffset, yScale;
    float zOffset, zScale;
    uint32_t xMin, yMin, xMax, yMax;
};

static uint32_t s_stateBits0;
static uint32_t s_stateBits1;
static GxmRenderState s_render;
static GxmProgramState s_program;
static bool s_renderApplied;

static GxmViewportState s_viewport;
static bool s_viewportApplied;

static bool s_scissorEnabled;
static uint32_t s_scissorMinX, s_scissorMinY, s_scissorMaxX, s_scissorMaxY;

static int s_vertexShader = -1;
static const GxmVertexLayout *s_layout;
static uint32_t s_fragmentHash;
static bool s_hasFragmentHash;
static bool s_programsApplied;

static int s_cachedVertexShader = -1;
static const GxmVertexLayout *s_cachedLayout;
static uint32_t s_cachedStrides[GXM_MAX_VERTEX_STREAMS];
static SceGxmVertexProgram *s_cachedVertexProgram;

static uint32_t s_cachedFragmentHash;
static uint32_t s_cachedAlphaTest = 0xFFFFFFFFu;
static int s_cachedFragmentShader = -1;

static uint32_t s_cachedProgramKey = 0xFFFFFFFFu;
static int s_cachedProgramVertex = -1;
static SceGxmFragmentProgram *s_cachedFragmentProgram;

static const uint8_t *s_streamData[GXM_MAX_VERTEX_STREAMS];
static uint32_t s_streamStride[GXM_MAX_VERTEX_STREAMS];
static bool s_streamApplied[GXM_MAX_VERTEX_STREAMS];
static const uint16_t *s_indexBase;

static GxmSamplerUnit s_units[GXM_PIPELINE_TEXTURE_UNITS];

// a slot the program fetches but nothing bound holds whatever the context had, which is null
// on a fresh one; parking every free slot here keeps that fetch inside mapped memory
static GxmBuffer s_dummyStream;

static GxmBuffer s_clearVertices;
static GxmBuffer s_clearIndices;
static SceGxmVertexProgram *s_clearVertexProgram;
static int s_clearVertexShader = -1;
static int s_clearFragmentShader = -1;
static const SceGxmProgramParameter *s_clearColorParam;

static uint32_t s_unresolvedDraws;

static bool GxmPipeline_InitClear(void)
{
    s_clearVertexShader = GxmProgram_Register(clear_v_gxp, sizeof(clear_v_gxp));
    s_clearFragmentShader = GxmProgram_Register(clear_f_gxp, sizeof(clear_f_gxp));
    if (s_clearVertexShader < 0 || s_clearFragmentShader < 0)
        return false;

    const GxmStreamSource source = { 0, 0, 1 };      // stream 0, offset 0, D3DDECLTYPE_FLOAT2
    const GxmStreamRouting routing = { 0, 0 };       // source 0 drives position
    const uint16_t strides[GXM_MAX_VERTEX_STREAMS] = { 8, 0 };

    GxmVertexLayout layout;
    if (!GxmVertex_BuildLayout(GxmProgram_Get(s_clearVertexShader), &source, 1,
                               &routing, 1, strides, &layout))
        return false;

    s_clearVertexProgram = GxmProgram_Vertex(s_clearVertexShader, layout.attributes,
                                             layout.attributeCount, layout.streams,
                                             layout.streamCount);
    if (!s_clearVertexProgram)
        return false;

    s_clearColorParam = sceGxmProgramFindParameterByName(GxmProgram_Get(s_clearFragmentShader), "c");
    if (!s_clearColorParam)
        return false;

    // the corners of the clip box, so the quad covers exactly the viewport
    static const float corners[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    static const uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 };

    if (!GxmBuffer_Create(&s_clearVertices, sizeof(corners), false))
        return false;
    memcpy(s_clearVertices.memory.base, corners, sizeof(corners));

    if (!GxmBuffer_Create(&s_clearIndices, sizeof(indices), false))
        return false;
    memcpy(s_clearIndices.memory.base, indices, sizeof(indices));
    return true;
}

bool GxmPipeline_Init(void)
{
    memset(s_units, 0, sizeof(s_units));
    memset(s_streamData, 0, sizeof(s_streamData));
    memset(s_streamStride, 0, sizeof(s_streamStride));
    s_indexBase = NULL;
    s_unresolvedDraws = 0;

    // the patcher programs behind these caches die with the device, so a hit would dangle
    s_vertexShader = -1;
    s_layout = NULL;
    s_fragmentHash = 0;
    s_hasFragmentHash = false;
    s_cachedVertexShader = -1;
    s_cachedLayout = NULL;
    s_cachedVertexProgram = NULL;
    memset(s_cachedStrides, 0, sizeof(s_cachedStrides));
    s_cachedFragmentHash = 0;
    s_cachedAlphaTest = 0xFFFFFFFFu;
    s_cachedFragmentShader = -1;
    s_cachedProgramKey = 0xFFFFFFFFu;
    s_cachedProgramVertex = -1;
    s_cachedFragmentProgram = NULL;

    // large enough that a fetch off a slot nothing bound stays inside it at any usable stride
    if (!GxmBuffer_Create(&s_dummyStream, 64 * 1024, false))
        return false;
    memset(s_dummyStream.memory.base, 0, s_dummyStream.memory.size);

    if (!GxmPipeline_InitClear())
        return false;

    GxmPipeline_Reset();
    return true;
}

void GxmPipeline_Shutdown(void)
{
    GxmBuffer_Free(&s_dummyStream);
    GxmBuffer_Free(&s_clearIndices);
    GxmBuffer_Free(&s_clearVertices);
    s_clearVertexProgram = NULL;
    s_clearColorParam = NULL;
    s_clearVertexShader = s_clearFragmentShader = -1;
}

static void GxmPipeline_Invalidate(void)
{
    s_renderApplied = false;
    s_viewportApplied = false;
    s_programsApplied = false;
    for (uint32_t i = 0; i < GXM_MAX_VERTEX_STREAMS; ++i)
        s_streamApplied[i] = false;
    for (uint32_t i = 0; i < GXM_PIPELINE_TEXTURE_UNITS; ++i)
        s_units[i].dirty = true;
}

void GxmPipeline_DropBuffers(void)
{
    memset(s_streamData, 0, sizeof(s_streamData));
    memset(s_streamApplied, 0, sizeof(s_streamApplied));
    s_indexBase = NULL;
}

void GxmPipeline_SceneChanged(void)
{
    GxmPipeline_Invalidate();
}

void GxmPipeline_Reset(void)
{
    SceGxmContext *context = GxmDevice_Context();
    if (!context)
        return;

    // the renderer drives front and back faces separately and never moves the stencil reference
    sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_ENABLED);
    sceGxmSetFrontStencilRef(context, 0);
    sceGxmSetBackStencilRef(context, 0);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);

    GxmPipeline_Invalidate();
}

void GxmPipeline_SetStateBits(uint32_t stateBits0, uint32_t stateBits1)
{
    if (s_renderApplied && stateBits0 == s_stateBits0 && stateBits1 == s_stateBits1)
        return;

    s_stateBits0 = stateBits0;
    s_stateBits1 = stateBits1;
    GxmState_Decode(stateBits0, stateBits1, &s_render, &s_program);
    s_renderApplied = false;

    // blend, colour mask and alpha test are baked into the fragment program
    s_programsApplied = false;
}

void GxmPipeline_SetViewport(int x, int y, int width, int height,
                             float nearValue, float farValue)
{
    if (width <= 0 || height <= 0)
        return;

    // GXM screen space has its origin top left, where D3D clip space has y up
    s_viewport.xScale = (float)width * 0.5f;
    s_viewport.xOffset = (float)x + s_viewport.xScale;
    s_viewport.yScale = (float)height * -0.5f;
    s_viewport.yOffset = (float)y + (float)height * 0.5f;

    // the engine's projection matrices are D3D form, so clip z arrives in [0, w]
    s_viewport.zScale = farValue - nearValue;
    s_viewport.zOffset = nearValue;

    s_viewport.xMin = (uint32_t)x;
    s_viewport.yMin = (uint32_t)y;
    s_viewport.xMax = (uint32_t)(x + width - 1);
    s_viewport.yMax = (uint32_t)(y + height - 1);

    s_viewportApplied = false;
}

void GxmPipeline_SetScissor(bool enabled, int x, int y, int width, int height)
{
    if (enabled && (width <= 0 || height <= 0))
        enabled = false;

    if (enabled)
    {
        if (x < 0)
        {
            width += x;
            x = 0;
        }
        if (y < 0)
        {
            height += y;
            y = 0;
        }
        if (width <= 0 || height <= 0)
            enabled = false;
    }

    s_scissorEnabled = enabled;
    if (enabled)
    {
        s_scissorMinX = (uint32_t)x;
        s_scissorMinY = (uint32_t)y;
        s_scissorMaxX = (uint32_t)(x + width - 1);
        s_scissorMaxY = (uint32_t)(y + height - 1);
    }
    s_viewportApplied = false;
}

void GxmPipeline_SetVertexShader(int shaderHandle, const GxmVertexLayout *layout)
{
    if (s_vertexShader == shaderHandle && s_layout == layout)
        return;

    s_vertexShader = shaderHandle;
    s_layout = layout;
    s_programsApplied = false;
}

void GxmPipeline_SetFragmentShader(uint32_t bytecodeHash)
{
    if (s_hasFragmentHash && s_fragmentHash == bytecodeHash)
        return;

    s_fragmentHash = bytecodeHash;
    s_hasFragmentHash = true;
    s_programsApplied = false;
}

void GxmPipeline_SetTexture(uint32_t unit, const SceGxmTexture *texture)
{
    if (unit >= GXM_PIPELINE_TEXTURE_UNITS)
        return;

    GxmSamplerUnit *slot = &s_units[unit];
    if (slot->source == texture)
        return;

    slot->source = texture;
    slot->hasTexture = texture != NULL;
    slot->dirty = true;
}

static SceGxmTextureAddrMode GxmPipeline_AddrMode(uint32_t d3dAddress)
{
    switch (d3dAddress)
    {
    case 2:  return SCE_GXM_TEXTURE_ADDR_MIRROR;
    case 3:  return SCE_GXM_TEXTURE_ADDR_CLAMP;
    default: return SCE_GXM_TEXTURE_ADDR_REPEAT;
    }
}

// D3DTEXF_ANISOTROPIC has no GXM counterpart, so it lands on linear
static void GxmPipeline_ApplySampler(SceGxmTexture *texture, uint32_t samplerState)
{
    const bool minPoint = ((samplerState >> 8) & 0xF) == 1;
    const bool magPoint = ((samplerState >> 12) & 0xF) == 1;
    const uint32_t mipFilter = (samplerState >> 16) & 0xF;

    SceGxmTextureFilter minFilter;
    SceGxmTextureMipFilter mipMode;
    if (mipFilter == 0)
    {
        minFilter = minPoint ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
        mipMode = SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
    }
    else if (mipFilter == 1)
    {
        minFilter = minPoint ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
        mipMode = SCE_GXM_TEXTURE_MIP_FILTER_ENABLED;
    }
    else
    {
        minFilter = minPoint ? SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT
                             : SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR;
        mipMode = SCE_GXM_TEXTURE_MIP_FILTER_ENABLED;
    }

    sceGxmTextureSetMinFilter(texture, minFilter);
    sceGxmTextureSetMagFilter(texture, magPoint ? SCE_GXM_TEXTURE_FILTER_POINT
                                                : SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMipFilter(texture, mipMode);
    sceGxmTextureSetUAddrMode(texture, GxmPipeline_AddrMode((samplerState >> 20) & 3));
    sceGxmTextureSetVAddrMode(texture, GxmPipeline_AddrMode((samplerState >> 22) & 3));
}

void GxmPipeline_SetSamplerState(uint32_t unit, uint32_t decodedSamplerState)
{
    if (unit >= GXM_PIPELINE_TEXTURE_UNITS)
        return;

    GxmSamplerUnit *slot = &s_units[unit];
    if (slot->samplerState == decodedSamplerState)
        return;

    slot->samplerState = decodedSamplerState;
    slot->dirty = true;
}

void GxmPipeline_SetStream(uint32_t index, const void *base, uint32_t offsetInBytes,
                           uint32_t stride)
{
    if (index >= GXM_MAX_VERTEX_STREAMS)
        return;

    const uint8_t *data = base ? (const uint8_t *)base + offsetInBytes : NULL;
    if (s_streamData[index] == data && s_streamStride[index] == stride)
        return;

    // the stride is part of the vertex program, so a change reselects one
    if (s_streamStride[index] != stride)
        s_programsApplied = false;

    s_streamData[index] = data;
    s_streamStride[index] = stride;
    s_streamApplied[index] = false;
}

void GxmPipeline_SetIndexBuffer(const void *base)
{
    s_indexBase = (const uint16_t *)base;
}

static bool GxmPipeline_ResolvePrograms(void)
{
    if (s_vertexShader < 0 || !s_layout || !s_hasFragmentHash)
        return false;

    if (s_cachedVertexShader != s_vertexShader || s_cachedLayout != s_layout ||
        memcmp(s_cachedStrides, s_streamStride, sizeof(s_cachedStrides)) != 0)
    {
        GxmVertexLayout patched = *s_layout;
        for (uint32_t i = 0; i < patched.streamCount; ++i)
        {
            if (s_streamStride[i])
                patched.streams[i].stride = (uint16_t)s_streamStride[i];
        }

        s_cachedVertexProgram = GxmProgram_Vertex(s_vertexShader, patched.attributes,
                                                  patched.attributeCount, patched.streams,
                                                  patched.streamCount);
        s_cachedVertexShader = s_vertexShader;
        s_cachedLayout = s_layout;
        memcpy(s_cachedStrides, s_streamStride, sizeof(s_cachedStrides));
    }
    if (!s_cachedVertexProgram)
        return false;

    // the alpha test lives in the fragment bytecode, so it picks the archive variant
    const uint32_t alphaTest = (uint32_t)s_program.alphaTest;
    if (s_cachedFragmentShader < 0 || s_cachedFragmentHash != s_fragmentHash ||
        s_cachedAlphaTest != alphaTest)
    {
        s_cachedFragmentShader = GxmShaderArchive_Lookup(s_fragmentHash, GXM_STAGE_FRAGMENT,
                                                         alphaTest);
        s_cachedFragmentHash = s_fragmentHash;
        s_cachedAlphaTest = alphaTest;
        s_cachedProgramKey = 0xFFFFFFFFu;
    }
    if (s_cachedFragmentShader < 0)
        return false;

    const uint32_t programKey = GxmState_ProgramKey(&s_program);
    if (s_cachedProgramKey != programKey || s_cachedProgramVertex != s_vertexShader)
    {
        s_cachedFragmentProgram = GxmProgram_Fragment(s_cachedFragmentShader, &s_program,
                                                      s_vertexShader);
        s_cachedProgramKey = programKey;
        s_cachedProgramVertex = s_vertexShader;
    }
    if (!s_cachedFragmentProgram)
        return false;

    GxmDraw_SetVertexProgram(s_vertexShader, s_cachedVertexProgram);
    GxmDraw_SetFragmentProgram(s_cachedFragmentShader, s_cachedFragmentProgram);
    return true;
}

static void GxmPipeline_ApplyRenderState(SceGxmContext *context)
{
    GxmState_Apply(context, &s_render);

    const SceGxmPolygonMode mode = (s_stateBits0 & GFXS0_POLYMODE_LINE)
        ? SCE_GXM_POLYGON_MODE_TRIANGLE_LINE : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    sceGxmSetFrontPolygonMode(context, mode);
    sceGxmSetBackPolygonMode(context, mode);
}

static void GxmPipeline_ApplyViewport(SceGxmContext *context)
{
    sceGxmSetViewport(context, s_viewport.xOffset, s_viewport.xScale,
                      s_viewport.yOffset, s_viewport.yScale,
                      s_viewport.zOffset, s_viewport.zScale);

    uint32_t minX = s_viewport.xMin, minY = s_viewport.yMin;
    uint32_t maxX = s_viewport.xMax, maxY = s_viewport.yMax;

    if (s_scissorEnabled)
    {
        if (s_scissorMinX > minX) minX = s_scissorMinX;
        if (s_scissorMinY > minY) minY = s_scissorMinY;
        if (s_scissorMaxX < maxX) maxX = s_scissorMaxX;
        if (s_scissorMaxY < maxY) maxY = s_scissorMaxY;
    }

    // an empty intersection would be a backwards rect, which GXM reads as the whole surface
    if (minX > maxX || minY > maxY)
    {
        sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_ALL, 0, 0, 0, 0);
        return;
    }
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, minX, minY, maxX, maxY);
}

static void GxmPipeline_ApplyTextures(void)
{
    for (uint32_t unit = 0; unit < GXM_PIPELINE_TEXTURE_UNITS; ++unit)
    {
        GxmSamplerUnit *slot = &s_units[unit];
        if (!slot->dirty)
            continue;

        if (!slot->hasTexture)
        {
            GxmDraw_SetTexture(unit, NULL);
        }
        else
        {
            slot->bound = *slot->source;

            // a released image leaves a zeroed texture behind, which samples from low memory
            if ((uintptr_t)sceGxmTextureGetData(&slot->bound) < GXM_LOWEST_MAPPED)
            {
                static uint32_t reported;
                if (!reported++)
                    VitaSys_LogPrintf("texture unit %u has data %p; using the dummy\n", unit,
                                      sceGxmTextureGetData(&slot->bound));
                GxmDraw_SetTexture(unit, NULL);
                slot->dirty = false;
                continue;
            }

            GxmPipeline_ApplySampler(&slot->bound, slot->samplerState);
            GxmDraw_SetTexture(unit, &slot->bound);
        }
        slot->dirty = false;
    }
}

bool GxmPipeline_DrawIndexed(uint32_t firstIndex, uint32_t triangleCount)
{
    if (!GxmRenderTarget_SceneOpen() || !s_indexBase || !triangleCount)
        return false;

    if (!s_programsApplied)
    {
        if (!GxmPipeline_ResolvePrograms())
        {
            if (!s_unresolvedDraws)
                VitaSys_LogPrintf("draw dropped: no program for vertex %i fragment %#x\n",
                                  s_vertexShader, (unsigned)s_fragmentHash);
            ++s_unresolvedDraws;
            return false;
        }
        s_programsApplied = true;
    }

    SceGxmContext *context = GxmDevice_Context();

    if (!s_renderApplied)
    {
        GxmPipeline_ApplyRenderState(context);
        s_renderApplied = true;
    }
    if (!s_viewportApplied)
    {
        GxmPipeline_ApplyViewport(context);
        s_viewportApplied = true;
    }

    // a stream an attribute reads would otherwise draw from whatever the slot last held; the
    // count runs past unused streams below the highest, so only the mask may be demanded.
    // a wrapped offset lands in low memory, which the GPU faults on instead of ignoring
    for (uint32_t mask = s_layout->streamMask; mask; mask &= mask - 1)
    {
        const uint32_t i = (uint32_t)__builtin_ctz(mask);
        if ((uintptr_t)s_streamData[i] < GXM_LOWEST_MAPPED)
        {
            if (!s_unresolvedDraws)
                VitaSys_LogPrintf("draw dropped: stream %u is %p (mask %#x, vs %i, fs %#x)\n",
                                  i, (const void *)s_streamData[i],
                                  (unsigned)s_layout->streamMask, s_vertexShader,
                                  (unsigned)s_fragmentHash);
            ++s_unresolvedDraws;
            return false;
        }
    }

    if ((uintptr_t)(s_indexBase + firstIndex) < GXM_LOWEST_MAPPED)
    {
        if (!s_unresolvedDraws)
            VitaSys_LogPrintf("draw dropped: index %p + %u (vs %i, fs %#x)\n",
                              (const void *)s_indexBase, firstIndex, s_vertexShader,
                              (unsigned)s_fragmentHash);
        ++s_unresolvedDraws;
        return false;
    }

    for (uint32_t i = 0; i < GXM_MAX_VERTEX_STREAMS; ++i)
    {
        if (s_streamApplied[i])
            continue;
        GxmDraw_SetStream(i, s_streamData[i] ? s_streamData[i] : s_dummyStream.memory.base);
        s_streamApplied[i] = true;
    }

    GxmPipeline_ApplyTextures();

    return GxmDraw_Indexed(SCE_GXM_PRIMITIVE_TRIANGLES, s_indexBase + firstIndex,
                           triangleCount * 3);
}

bool GxmPipeline_Clear(uint32_t whichToClear, const float color[4], float depth,
                       uint8_t stencil)
{
    // the quad covers the viewport, as D3D's clear does, so an unset one would clear nothing
    if (!GxmRenderTarget_SceneOpen() || !s_clearVertexProgram || s_viewport.xScale <= 0.0f)
        return false;

    // the quad's clip z is fixed at the far plane
    if ((whichToClear & CLEAR_ZBUFFER) && depth != 1.0f)
        return false;

    GxmProgramState state;
    memset(&state, 0, sizeof(state));
    state.alphaTest = GXM_ATEST_NONE;
    state.blend.colorMask = (whichToClear & CLEAR_TARGET)
        ? (SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G |
           SCE_GXM_COLOR_MASK_B | SCE_GXM_COLOR_MASK_A)
        : 0;

    SceGxmFragmentProgram *fragment =
        GxmProgram_Fragment(s_clearFragmentShader, &state, s_clearVertexShader);
    if (!fragment)
        return false;

    SceGxmContext *context = GxmDevice_Context();

    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthBias(context, 0, 0);
    sceGxmSetBackDepthBias(context, 0, 0);

    const SceGxmDepthWriteMode depthWrite = (whichToClear & CLEAR_ZBUFFER)
        ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthWriteEnable(context, depthWrite);
    sceGxmSetBackDepthWriteEnable(context, depthWrite);

    if (whichToClear & CLEAR_STENCIL)
    {
        sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                                  SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_REPLACE,
                                  SCE_GXM_STENCIL_OP_REPLACE, 0xFF, 0xFF);
        sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                                 SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_REPLACE,
                                 SCE_GXM_STENCIL_OP_REPLACE, 0xFF, 0xFF);
        sceGxmSetFrontStencilRef(context, stencil);
        sceGxmSetBackStencilRef(context, stencil);
    }
    else
    {
        sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                                  SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                                  SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
        sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                                 SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                                 SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
    }

    // D3D clears the literal depth, so the quad's z must land on it whatever the depth range
    sceGxmSetViewport(context, s_viewport.xOffset, s_viewport.xScale,
                      s_viewport.yOffset, s_viewport.yScale, 0.0f, 1.0f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE,
                        s_viewport.xMin, s_viewport.yMin, s_viewport.xMax, s_viewport.yMax);

    // driven straight off the context, so the shared constant shadow keeps the engine's values
    sceGxmSetVertexProgram(context, s_clearVertexProgram);
    sceGxmSetFragmentProgram(context, fragment);

    void *uniforms = NULL;
    if (sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms) < 0 || !uniforms)
        return false;
    if (sceGxmSetUniformDataF(uniforms, s_clearColorParam, 0, 4, color) < 0)
        return false;

    sceGxmSetVertexStream(context, 0, s_clearVertices.memory.base);
    const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
                                  SCE_GXM_INDEX_FORMAT_U16, s_clearIndices.memory.base, 6);

    GxmPipeline_Invalidate();
    return result >= 0;
}

uint32_t GxmPipeline_UnresolvedDraws(void)
{
    return s_unresolvedDraws;
}

void GxmPipeline_ResetCounters(void)
{
    s_unresolvedDraws = 0;
}
