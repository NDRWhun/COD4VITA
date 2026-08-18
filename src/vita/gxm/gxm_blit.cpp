#include "gxm_blit.h"
#include "gxm_blit_shaders.h"
#include "gxm_buffer.h"
#include "gxm_device.h"
#include "gxm_pipeline.h"
#include "gxm_program.h"
#include "gxm_state.h"
#include "gxm_vertex.h"

#include <vita/platform/vita_system.h>

#include <string.h>

#define GXM_BLIT_QUADS 64

struct GxmBlitVertex
{
    float position[3];
    float texCoord[2];
    uint32_t color;
};

static int s_vertexShader = -1;
static int s_fragmentShader = -1;
static SceGxmVertexProgram *s_vertexProgram;
static SceGxmFragmentProgram *s_fragmentProgram;
static const SceGxmProgramParameter *s_matrixParam;

static SceGxmTexture s_bound;
static GxmBuffer s_vertices;
static GxmBuffer s_indices;
static uint32_t s_nextQuad;
static bool s_ready;
static bool s_failed;

static uint32_t s_droppedBlits;

// the identity, so the shader's dot products pass clip-space positions through untouched
static const float s_identity[16] =
{
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

static bool GxmBlit_Init(void)
{
    s_vertexShader = GxmProgram_Register(blit_v_gxp, sizeof(blit_v_gxp));
    s_fragmentShader = GxmProgram_Register(blit_f_gxp, sizeof(blit_f_gxp));
    if (s_vertexShader < 0 || s_fragmentShader < 0)
        return false;

    const GxmStreamSource sources[3] =
    {
        { 0, 0, 2 },                                // position, D3DDECLTYPE_FLOAT3
        { 0, 12, 1 },                               // texcoord, D3DDECLTYPE_FLOAT2
        { 0, 20, 4 }                                // colour, D3DDECLTYPE_D3DCOLOR
    };
    const GxmStreamRouting routing[3] = { { 0, 0 }, { 1, 4 }, { 2, 2 } };
    const uint16_t strides[GXM_MAX_VERTEX_STREAMS] = { sizeof(GxmBlitVertex), 0 };

    GxmVertexLayout layout;
    if (!GxmVertex_BuildLayout(GxmProgram_Get(s_vertexShader), sources, 3, routing, 3,
                               strides, &layout))
        return false;

    s_vertexProgram = GxmProgram_Vertex(s_vertexShader, layout.attributes, layout.attributeCount,
                                        layout.streams, layout.streamCount);
    if (!s_vertexProgram)
        return false;

    GxmProgramState state;
    memset(&state, 0, sizeof(state));
    state.alphaTest = GXM_ATEST_NONE;
    state.blend.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G |
                            SCE_GXM_COLOR_MASK_B | SCE_GXM_COLOR_MASK_A;

    s_fragmentProgram = GxmProgram_Fragment(s_fragmentShader, &state, s_vertexShader);
    if (!s_fragmentProgram)
        return false;

    s_matrixParam = sceGxmProgramFindParameterByName(GxmProgram_Get(s_vertexShader), "c");
    if (!s_matrixParam)
        return false;

    // a ring of quads, so a rewrite never lands in vertices a queued draw still reads
    if (!GxmBuffer_Create(&s_vertices, sizeof(GxmBlitVertex) * 4 * GXM_BLIT_QUADS, false))
        return false;

    static const uint16_t quadIndices[6] = { 0, 1, 2, 2, 1, 3 };
    if (!GxmBuffer_Create(&s_indices, sizeof(quadIndices), false))
        return false;
    memcpy(s_indices.memory.base, quadIndices, sizeof(quadIndices));

    return true;
}

static bool GxmBlit_Ready(void)
{
    if (s_ready)
        return true;
    if (s_failed)
        return false;

    s_ready = GxmBlit_Init();
    s_failed = !s_ready;
    return s_ready;
}

void GxmBlit_Shutdown(void)
{
    GxmBuffer_Free(&s_indices);
    GxmBuffer_Free(&s_vertices);
    s_vertexProgram = NULL;
    s_fragmentProgram = NULL;
    s_matrixParam = NULL;
    s_vertexShader = s_fragmentShader = -1;
    s_ready = false;
    s_failed = false;
}

// a render target sampled through a linear texture only honours clamp addressing
static void GxmBlit_Sampler(SceGxmTexture *texture)
{
    sceGxmTextureSetMinFilter(texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMipFilter(texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
}

bool GxmBlit_Rect(const SceGxmTexture *texture, int x, int y, int width, int height,
                  uint32_t targetWidth, uint32_t targetHeight)
{
    if (!texture || width <= 0 || height <= 0 || !targetWidth || !targetHeight ||
        !GxmRenderTarget_SceneOpen() || !GxmBlit_Ready())
    {
        ++s_droppedBlits;
        return false;
    }

    // sampling an unmapped surface faults the context, not just the draw
    if ((uintptr_t)sceGxmTextureGetData(texture) < GXM_LOWEST_MAPPED)
    {
        if (!s_droppedBlits)
            VitaSys_LogPrintf("blit dropped: texture data is %p (%ix%i at %i,%i)\n",
                              sceGxmTextureGetData(texture), width, height, x, y);
        ++s_droppedBlits;
        return false;
    }

    GxmBlitVertex *quad = (GxmBlitVertex *)s_vertices.memory.base + s_nextQuad * 4;
    s_nextQuad = (s_nextQuad + 1) % GXM_BLIT_QUADS;

    // the target's pixel space has y down, where clip space has y up
    const float left = 2.0f * (float)x / (float)targetWidth - 1.0f;
    const float right = 2.0f * (float)(x + width) / (float)targetWidth - 1.0f;
    const float top = 1.0f - 2.0f * (float)y / (float)targetHeight;
    const float bottom = 1.0f - 2.0f * (float)(y + height) / (float)targetHeight;

    const float corners[4][4] = {
        { left, top, 0.0f, 0.0f },
        { right, top, 1.0f, 0.0f },
        { left, bottom, 0.0f, 1.0f },
        { right, bottom, 1.0f, 1.0f }
    };

    for (uint32_t i = 0; i < 4; ++i)
    {
        quad[i].position[0] = corners[i][0];
        quad[i].position[1] = corners[i][1];
        quad[i].position[2] = 0.0f;
        quad[i].texCoord[0] = corners[i][2];
        quad[i].texCoord[1] = corners[i][3];
        quad[i].color = 0xFFFFFFFFu;
    }

    SceGxmContext *context = GxmDevice_Context();

    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontDepthBias(context, 0, 0);
    sceGxmSetBackDepthBias(context, 0, 0);
    sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
    sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);

    // the copy owns the whole target, so neither the engine's viewport nor its scissor applies
    sceGxmSetViewport(context, (float)targetWidth * 0.5f, (float)targetWidth * 0.5f,
                      (float)targetHeight * 0.5f, (float)targetHeight * -0.5f, 0.0f, 1.0f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
                        targetWidth - 1, targetHeight - 1);

    sceGxmSetVertexProgram(context, s_vertexProgram);
    sceGxmSetFragmentProgram(context, s_fragmentProgram);

    void *uniforms = NULL;
    bool ok = sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms) >= 0 && uniforms &&
              sceGxmSetUniformDataF(uniforms, s_matrixParam, 0, 16, s_identity) >= 0;

    if (ok)
    {
        // a per-unit copy, because GXM keeps the sampler state inside the control block
        s_bound = *texture;
        GxmBlit_Sampler(&s_bound);
        ok = sceGxmSetFragmentTexture(context, 0, &s_bound) >= 0;
    }

    if (ok)
    {
        sceGxmSetVertexStream(context, 0, quad);
        ok = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                        s_indices.memory.base, 6) >= 0;
    }

    // the context was driven straight, so every binding the pipeline cached is now stale
    GxmPipeline_Reset();

    if (!ok)
        ++s_droppedBlits;
    return ok;
}

bool GxmBlit_Resolve(GxmRenderTarget *src, GxmRenderTarget *dst)
{
    if (!src || !dst || !GxmBlit_Ready())
    {
        ++s_droppedBlits;
        return false;
    }

    // entering dst ends src's scene, which is what stores src's colour to memory
    if (!GxmRenderTarget_Begin(dst))
    {
        ++s_droppedBlits;
        return false;
    }
    if (!GxmBlit_Rect(GxmRenderTarget_Texture(src), 0, 0, (int)dst->width, (int)dst->height,
                      dst->width, dst->height))
        return false;

    // GXM does not reload a colour surface, so src comes back from the copy just made
    if (!GxmRenderTarget_Begin(src))
    {
        ++s_droppedBlits;
        return false;
    }
    return GxmBlit_Rect(GxmRenderTarget_Texture(dst), 0, 0, (int)src->width, (int)src->height,
                        src->width, src->height);
}

uint32_t GxmBlit_DroppedBlits(void)
{
    return s_droppedBlits;
}
