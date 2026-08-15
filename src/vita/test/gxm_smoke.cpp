// Standalone check that the GXM layer works on hardware.
//
// Brings up the device, uploads a texture and a quad, and draws with depth test
// and blending on. Also loads the shader archive if it is present, so a device
// that boots this has proved memory, programs, state, layout, draws and input.

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../gxm/gxm_buffer.h"
#include "../gxm/gxm_device.h"
#include "../gxm/gxm_draw.h"
#include "../gxm/gxm_program.h"
#include "../gxm/gxm_shader_archive.h"
#include "../gxm/gxm_state.h"
#include "../gxm/gxm_texture.h"
#include "../gxm/gxm_vertex.h"
#include "../input/vita_input.h"
#include "shaders/smoke_shaders.h"

#define SHADER_ARCHIVE_PATH "app0:shaders.kgxp"
#define LOG_PATH            "ux0:data/kisakcod/smoke.log"

struct SmokeVertex
{
    float x, y, z;
    float u, v;
    uint32_t colour;        // RGBA in memory, matching what U8N reads
};

static const SmokeVertex s_quad[4] =
{
    { -0.6f, -0.6f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFF },
    {  0.6f, -0.6f, 0.0f, 1.0f, 1.0f, 0xFF80FFFF },
    {  0.6f,  0.6f, 0.0f, 1.0f, 0.0f, 0xFFFF80FF },
    { -0.6f,  0.6f, 0.0f, 0.0f, 0.0f, 0xFFFFFF80 },
};

static const uint16_t s_indices[6] = { 0, 1, 2, 0, 2, 3 };

static GxmBuffer s_vertexBuffer;
static GxmBuffer s_indexBuffer;
static GxmTexture s_texture;
static GxmVertexLayout s_layout;
static SceGxmVertexProgram *s_vertexProgram;
static SceGxmFragmentProgram *s_fragmentProgram;
static int s_vertexShader = -1;
static int s_fragmentShader = -1;
static bool s_quit;

static void SmokeLog(const char *format, ...)
{
    va_list args;
    FILE *file = fopen(LOG_PATH, "a");
    if (!file)
        return;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fclose(file);
}

static void SmokeKey(int key, bool down)
{
    if (key == 0x1B && down)        // K_ESCAPE, mapped from Start
        s_quit = true;
}

// a checkerboard, so filtering and uv orientation are both obvious on screen
static void SmokeFillTexture(void)
{
    const uint32_t size = 64;
    uint32_t *pixels = (uint32_t *)s_texture.memory.base;
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const bool dark = ((x / 8) + (y / 8)) & 1;
            pixels[y * size + x] = dark ? 0xFF404040u : 0xFFE0E0E0u;
        }
    }
}

static bool SmokeInitResources(void)
{
    s_vertexShader = GxmProgram_Register(smoke_v_gxp, sizeof(smoke_v_gxp));
    s_fragmentShader = GxmProgram_Register(smoke_f_gxp, sizeof(smoke_f_gxp));
    if (s_vertexShader < 0 || s_fragmentShader < 0)
        return false;

    // the smoke shader takes position, uv and colour from one interleaved stream
    static const GxmStreamSource sources[3] =
    {
        { 0, 0, 2 },                    // position, FLOAT3
        { 0, 12, 1 },                   // uv, FLOAT2
        { 0, 20, 8 },                   // colour, UBYTE4N
    };
    static const GxmStreamRouting routing[3] =
    {
        { 0, 0 },                       // position -> position
        { 1, 4 },                       // uv -> texcoord0
        { 2, 2 },                       // colour -> colour0
    };
    const uint16_t strides[GXM_MAX_VERTEX_STREAMS] = { sizeof(SmokeVertex), 0 };

    if (!GxmVertex_BuildLayout(GxmProgram_Get(s_vertexShader), sources, 3,
                               routing, 3, strides, &s_layout))
        return false;

    s_vertexProgram = GxmProgram_Vertex(s_vertexShader, s_layout.attributes,
                                        s_layout.attributeCount, s_layout.streams,
                                        s_layout.streamCount);
    if (!s_vertexProgram)
        return false;

    GxmProgramState blend;
    memset(&blend, 0, sizeof(blend));
    blend.blendEnabled = true;
    blend.blend.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G
                          | SCE_GXM_COLOR_MASK_B | SCE_GXM_COLOR_MASK_A;
    blend.blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;

    s_fragmentProgram = GxmProgram_Fragment(s_fragmentShader, &blend, s_vertexShader);
    if (!s_fragmentProgram)
        return false;

    if (!GxmBuffer_Create(&s_vertexBuffer, sizeof(s_quad), false))
        return false;
    memcpy(s_vertexBuffer.memory.base, s_quad, sizeof(s_quad));

    if (!GxmBuffer_Create(&s_indexBuffer, sizeof(s_indices), false))
        return false;
    memcpy(s_indexBuffer.memory.base, s_indices, sizeof(s_indices));

    if (!GxmTexture_Create(&s_texture, GXM_IMG_A8R8G8B8, 64, 64, 1, false))
        return false;
    SmokeFillTexture();
    GxmTexture_SetFilter(&s_texture, true, true);
    return true;
}

static void SmokeDraw(float spin)
{
    GxmRenderState render;
    GxmProgramState program;
    GxmState_Decode(0x8000 | 0x100 | 0x60 | 0x18000000,     // cull back, blend, colour write
                    0x1 | 0xC,                              // depth write, depth test lessequal
                    &render, &program);
    GxmState_Apply(GxmDevice_Context(), &render);

    GxmDraw_SetVertexProgram(s_vertexShader, s_vertexProgram);
    GxmDraw_SetFragmentProgram(s_fragmentShader, s_fragmentProgram);

    // a spin around Z, so a still frame still proves the transform is being applied
    const float c = __builtin_cosf(spin), s = __builtin_sinf(spin);
    const float mvp[16] =
    {
        c * 0.56f, s, 0.0f, 0.0f,
        -s * 0.56f, c, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    GxmDraw_SetVertexConstants(0, mvp, 4);

    GxmDraw_SetTexture(0, &s_texture.texture);
    GxmDraw_SetStream(0, s_vertexBuffer.memory.base);
    GxmDraw_Indexed(SCE_GXM_PRIMITIVE_TRIANGLES, (const uint16_t *)s_indexBuffer.memory.base, 6);
}

int main(void)
{
    SmokeLog("--- gxm smoke test ---\n");

    if (!GxmDevice_Init())
    {
        SmokeLog("device init failed\n");
        return -1;
    }
    GxmProgram_Init();
    GxmDraw_Init();
    GxmRing_Init(1024 * 1024);
    VitaInput_Init(SmokeKey, NULL);

    if (GxmShaderArchive_Load(SHADER_ARCHIVE_PATH))
        SmokeLog("shader archive: %u entries\n", GxmShaderArchive_Count());
    else
        SmokeLog("shader archive absent, drawing with the built-in shaders only\n");

    if (!SmokeInitResources())
    {
        SmokeLog("resource setup failed\n");
        GxmDevice_Shutdown();
        return -1;
    }

    SmokeLog("textures resident: %u bytes, cdram used: %u bytes\n",
             GxmTexture_BytesResident(), GxmMem_BytesUsed(GXM_MEM_CDRAM));

    float spin = 0.0f;
    while (!s_quit)
    {
        VitaInput_Frame();
        GxmRing_BeginFrame();

        GxmDevice_BeginFrame();
        SmokeDraw(spin);
        GxmDevice_EndFrame();

        spin += 0.02f;
    }

    SmokeLog("drew %u frames, %u draws, ring peak %u bytes\n",
             GxmDevice_FrameIndex(), GxmDraw_DrawCount(), GxmRing_BytesPeak());

    GxmTexture_Free(&s_texture);
    GxmBuffer_Free(&s_indexBuffer);
    GxmBuffer_Free(&s_vertexBuffer);
    GxmShaderArchive_Unload();
    GxmRing_Shutdown();
    VitaInput_Shutdown();
    GxmProgram_Shutdown();
    GxmDevice_Shutdown();

    sceKernelExitProcess(0);
    return 0;
}
