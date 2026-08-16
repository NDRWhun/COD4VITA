// Standalone check that the GXM layer works on hardware.

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
#include "../platform/vita_memory.h"
#include "../platform/vita_selftest.h"
#include "../platform/vita_threads.h"
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

// the same quad wound both ways; with cull-back set only the front-facing one survives
static const uint16_t s_indices[12] =
{
    0, 1, 2, 0, 2, 3,       // counter-clockwise
    0, 2, 1, 0, 3, 2,       // clockwise
};

static GxmBuffer s_vertexBuffer;
static GxmBuffer s_indexBuffer;
static GxmTexture s_texture;
static GxmVertexLayout s_layout;
static SceGxmVertexProgram *s_vertexProgram;
static SceGxmFragmentProgram *s_fragmentProgram;
static int s_vertexShader = -1;
static int s_fragmentShader = -1;

// the clear pass: a full-screen triangle, since GXM has no clear entry point
static GxmBuffer s_clearVertices;
static GxmBuffer s_clearIndices;
static SceGxmVertexProgram *s_clearVertexProgram;
static SceGxmFragmentProgram *s_clearFragmentProgram;
static int s_clearVertexShader = -1;
static int s_clearFragmentShader = -1;

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

static bool SmokeInitClear(void);

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

    return SmokeInitClear();
}

static bool SmokeInitClear(void)
{
    s_clearVertexShader = GxmProgram_Register(clear_v_gxp, sizeof(clear_v_gxp));
    s_clearFragmentShader = GxmProgram_Register(clear_f_gxp, sizeof(clear_f_gxp));
    if (s_clearVertexShader < 0 || s_clearFragmentShader < 0)
        return false;

    static const GxmStreamSource sources[1] = { { 0, 0, 1 } };      // position, FLOAT2
    static const GxmStreamRouting routing[1] = { { 0, 0 } };        // position -> position
    const uint16_t strides[GXM_MAX_VERTEX_STREAMS] = { sizeof(float) * 2, 0 };

    GxmVertexLayout layout;
    if (!GxmVertex_BuildLayout(GxmProgram_Get(s_clearVertexShader), sources, 1,
                               routing, 1, strides, &layout))
        return false;

    s_clearVertexProgram = GxmProgram_Vertex(s_clearVertexShader, layout.attributes,
                                             layout.attributeCount, layout.streams,
                                             layout.streamCount);
    if (!s_clearVertexProgram)
        return false;

    GxmProgramState opaque;
    memset(&opaque, 0, sizeof(opaque));
    opaque.blendEnabled = false;
    opaque.blend.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G
                           | SCE_GXM_COLOR_MASK_B | SCE_GXM_COLOR_MASK_A;

    s_clearFragmentProgram = GxmProgram_Fragment(s_clearFragmentShader, &opaque,
                                                 s_clearVertexShader);
    if (!s_clearFragmentProgram)
        return false;

    // one triangle large enough to cover the screen
    static const float triangle[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    static const uint16_t indices[3] = { 0, 1, 2 };

    if (!GxmBuffer_Create(&s_clearVertices, sizeof(triangle), false))
        return false;
    memcpy(s_clearVertices.memory.base, triangle, sizeof(triangle));

    if (!GxmBuffer_Create(&s_clearIndices, sizeof(indices), false))
        return false;
    memcpy(s_clearIndices.memory.base, indices, sizeof(indices));
    return true;
}

// depth func always plus depth write, so the clear resets colour and depth together
static void SmokeClear(void)
{
    GxmRenderState render;
    GxmProgramState program;
    GxmState_Decode(0x4000 | 0x18000000,        // cull none, colour write, no blending
                    0x1,                        // depth write, depth test always
                    &render, &program);
    GxmState_Apply(GxmDevice_Context(), &render);

    GxmDraw_SetVertexProgram(s_clearVertexShader, s_clearVertexProgram);
    GxmDraw_SetFragmentProgram(s_clearFragmentShader, s_clearFragmentProgram);

    static const float clearColour[4] = { 0.05f, 0.10f, 0.20f, 1.0f };
    GxmDraw_SetFragmentConstants(0, clearColour, 1);

    GxmDraw_SetStream(0, s_clearVertices.memory.base);
    GxmDraw_Indexed(SCE_GXM_PRIMITIVE_TRIANGLES,
                    (const uint16_t *)s_clearIndices.memory.base, 3);
}

// draws the quad at an x offset, using one of the two windings
static bool SmokeDrawQuad(float spin, float offsetX, uint32_t indexOffset)
{
    const float c = __builtin_cosf(spin), s = __builtin_sinf(spin);
    const float mvp[16] =
    {
        c * 0.28f, s * 0.5f, 0.0f, 0.0f,
        -s * 0.28f, c * 0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        offsetX, 0.0f, 0.0f, 1.0f,
    };
    GxmDraw_SetVertexConstants(0, mvp, 4);

    GxmDraw_SetTexture(0, &s_texture.texture);
    GxmDraw_SetStream(0, s_vertexBuffer.memory.base);

    const uint16_t *indices = (const uint16_t *)s_indexBuffer.memory.base + indexOffset;
    return GxmDraw_Indexed(SCE_GXM_PRIMITIVE_TRIANGLES, indices, 6);
}

static bool SmokeDraw(float spin)
{
    GxmRenderState render;
    GxmProgramState program;
    // cull back, so exactly one of the two windings should survive
    GxmState_Decode(0x8000 | 0x100 | 0x60 | 0x18000000,     // cull back, blend, colour write
                    0x1 | 0xC,                              // depth write, depth test lessequal
                    &render, &program);
    GxmState_Apply(GxmDevice_Context(), &render);

    GxmDraw_SetVertexProgram(s_vertexShader, s_vertexProgram);
    GxmDraw_SetFragmentProgram(s_fragmentShader, s_fragmentProgram);

    const bool left = SmokeDrawQuad(spin, -0.45f, 0);       // counter-clockwise
    const bool right = SmokeDrawQuad(spin, 0.45f, 6);       // clockwise
    return left && right;
}

int main(void)
{
    SmokeLog("--- gxm smoke test ---\n");

    char report[256];
    if (VitaMem_Init() && VitaThreads_Init())
    {
        VitaSelfTest_Memory(report, sizeof(report));
        SmokeLog("%s", report);
        VitaSelfTest_Threads(report, sizeof(report));
        SmokeLog("%s", report);
        VitaSelfTest_Files(report, sizeof(report));
        SmokeLog("%s", report);
        VitaSelfTest_Clocks(report, sizeof(report));
        SmokeLog("%s", report);
    }
    else
    {
        SmokeLog("platform init failed\n");
    }

    if (!GxmDevice_Init())
    {
        VitaMemStats main, cdram;
        VitaMem_GetStats(VITA_MEM_MAIN, &main);
        VitaMem_GetStats(VITA_MEM_CDRAM, &cdram);
        SmokeLog("device init failed; arenas hold main %u KB, cdram %u KB\n",
                 main.reserved / 1024, cdram.reserved / 1024);
        return -1;
    }
    GxmProgram_Init();
    GxmDraw_Init();
    GxmRing_Init(1024 * 1024);
    VitaInput_Init(SmokeKey, NULL);

    if (GxmShaderArchive_Load(SHADER_ARCHIVE_PATH))
    {
        // registering every blob proves the patcher accepts the translated corpus
        const uint32_t count = GxmShaderArchive_Count();
        const uint32_t registered = GxmShaderArchive_RegisterAll();
        SmokeLog("shader archive: %u/%u blobs registered\n", registered, count);
    }
    else
    {
        SmokeLog("shader archive absent, drawing with the built-in shaders only\n");
    }

    if (!SmokeInitResources())
    {
        SmokeLog("resource setup failed\n");
        GxmDevice_Shutdown();
        return -1;
    }

    VitaMemStats cdram;
    VitaMem_GetStats(VITA_MEM_CDRAM, &cdram);
    SmokeLog("textures resident: %u bytes; cdram arena %u KB reserved, %u KB used, %u allocations\n",
             GxmTexture_BytesResident(), cdram.reserved / 1024, cdram.used / 1024,
             cdram.liveAllocations);

    // two seconds at vsync, so even a brief run leaves a timing sample
    const uint32_t REPORT_FRAMES = 120;

    float spin = 0.0f;
    bool firstDrawLogged = false;

    uint64_t windowStart = sceKernelGetProcessTimeWide();
    uint64_t previousFrame = windowStart;
    uint32_t worstUs = 0;
    uint32_t bestUs = 0xFFFFFFFFu;

    while (!s_quit)
    {
        VitaInput_Frame();
        GxmRing_BeginFrame();

        GxmDevice_BeginFrame();
        SmokeClear();
        const bool drew = SmokeDraw(spin);
        GxmDevice_EndFrame();

        const uint64_t now = sceKernelGetProcessTimeWide();
        const uint32_t frameUs = (uint32_t)(now - previousFrame);
        previousFrame = now;
        if (frameUs > worstUs)
            worstUs = frameUs;
        if (frameUs < bestUs)
            bestUs = frameUs;

        // report early and then rarely, so a force-quit still leaves evidence
        if (!firstDrawLogged)
        {
            SmokeLog("first frame: draw %s\n", drew ? "submitted" : "REJECTED");
            firstDrawLogged = true;
        }
        else if ((GxmDevice_FrameIndex() % REPORT_FRAMES) == 0)
        {
            // the display callback waits on vsync, so 16667 us average means locked not saturated
            const uint32_t windowUs = (uint32_t)(now - windowStart);
            SmokeLog("frame %u, %u draws, avg %u us, best %u us, worst %u us over %u ms\n",
                     GxmDevice_FrameIndex(), GxmDraw_DrawCount(), windowUs / REPORT_FRAMES,
                     bestUs, worstUs, windowUs / 1000);
            windowStart = now;
            worstUs = 0;
            bestUs = 0xFFFFFFFFu;
        }

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
