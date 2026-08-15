#include "gxm_draw.h"
#include "gxm_device.h"
#include "gxm_program.h"
#include "gxm_texture.h"

#include <string.h>

struct GxmStage
{
    int shader;
    const SceGxmProgramParameter *constants;    // the "c" array the translator emits
    uint32_t constantWords;                     // how many words the array holds
    float shadow[GXM_MAX_CONSTANT_REGISTERS * 4];
    uint32_t highWater;                         // words worth uploading
    bool dirty;
};

static GxmStage s_vertex;
static GxmStage s_fragment;
static uint32_t s_drawCount;
static uint32_t s_droppedConstants;
static GxmTexture s_dummyTexture;       // stands in for an unbound sampler unit

bool GxmDraw_Init(void)
{
    memset(&s_vertex, 0, sizeof(s_vertex));
    memset(&s_fragment, 0, sizeof(s_fragment));
    s_vertex.shader = s_fragment.shader = -1;
    s_drawCount = 0;
    s_droppedConstants = 0;

    if (!GxmTexture_Create(&s_dummyTexture, GXM_IMG_A8R8G8B8, 1, 1, 1, false))
        return false;
    *(uint32_t *)s_dummyTexture.memory.base = 0xFF000000u;
    return true;
}

void GxmDraw_Shutdown(void)
{
    GxmTexture_Free(&s_dummyTexture);
}

static void GxmDraw_BindStage(GxmStage *stage, int shaderHandle)
{
    if (stage->shader == shaderHandle)
        return;

    // the shadow survives a program change: the engine sets constants for a pass before
    // binding its shaders, and D3D keeps constants across shader changes anyway
    stage->shader = shaderHandle;
    stage->constants = NULL;
    stage->constantWords = 0;
    stage->dirty = true;

    const SceGxmProgram *program = GxmProgram_Get(shaderHandle);
    if (!program)
        return;

    stage->constants = sceGxmProgramFindParameterByName(program, "c");
    if (stage->constants)
        stage->constantWords = sceGxmProgramParameterGetArraySize(stage->constants) * 4;
}

void GxmDraw_SetVertexProgram(int shaderHandle, const SceGxmVertexProgram *program)
{
    GxmDraw_BindStage(&s_vertex, shaderHandle);
    sceGxmSetVertexProgram(GxmDevice_Context(), program);
}

void GxmDraw_SetFragmentProgram(int shaderHandle, const SceGxmFragmentProgram *program)
{
    GxmDraw_BindStage(&s_fragment, shaderHandle);
    sceGxmSetFragmentProgram(GxmDevice_Context(), program);
}

static void GxmDraw_StageConstants(GxmStage *stage, uint32_t startRegister,
                                   const float *values, uint32_t registerCount)
{
    // measured high-water is 32 vertex and 22 fragment registers, so an overflow means a
    // wrong assumption rather than a tight budget; count it instead of dropping it quietly
    if (startRegister >= GXM_MAX_CONSTANT_REGISTERS)
    {
        ++s_droppedConstants;
        return;
    }
    if (startRegister + registerCount > GXM_MAX_CONSTANT_REGISTERS)
    {
        ++s_droppedConstants;
        registerCount = GXM_MAX_CONSTANT_REGISTERS - startRegister;
    }

    const uint32_t firstWord = startRegister * 4;
    const uint32_t words = registerCount * 4;

    memcpy(&stage->shadow[firstWord], values, words * sizeof(float));
    if (firstWord + words > stage->highWater)
        stage->highWater = firstWord + words;
    stage->dirty = true;
}

void GxmDraw_SetVertexConstants(uint32_t startRegister, const float *values,
                                uint32_t registerCount)
{
    GxmDraw_StageConstants(&s_vertex, startRegister, values, registerCount);
}

void GxmDraw_SetFragmentConstants(uint32_t startRegister, const float *values,
                                  uint32_t registerCount)
{
    GxmDraw_StageConstants(&s_fragment, startRegister, values, registerCount);
}

// a fragment program sampling a unit that was never bound faults, so unbinding has to
// leave something valid behind rather than nothing
void GxmDraw_SetTexture(uint32_t unit, const SceGxmTexture *texture)
{
    if (unit >= GXM_MAX_TEXTURE_UNITS)
        return;

    if (!texture)
    {
        if (s_dummyTexture.memory.base)
            sceGxmSetFragmentTexture(GxmDevice_Context(), unit, &s_dummyTexture.texture);
        return;
    }
    sceGxmSetFragmentTexture(GxmDevice_Context(), unit, texture);
}

void GxmDraw_UnbindTextures(void)
{
    for (uint32_t unit = 0; unit < GXM_MAX_TEXTURE_UNITS; ++unit)
        GxmDraw_SetTexture(unit, NULL);
}

uint32_t GxmDraw_DroppedConstants(void)
{
    return s_droppedConstants;
}

void GxmDraw_SetStream(uint32_t streamIndex, const void *data)
{
    if (streamIndex >= GXM_MAX_VERTEX_STREAMS)
        return;
    sceGxmSetVertexStream(GxmDevice_Context(), streamIndex, data);
}

static bool GxmDraw_UploadConstants(GxmStage *stage, bool isVertex)
{
    if (!stage->constants || !stage->highWater)
        return true;

    SceGxmContext *context = GxmDevice_Context();
    void *buffer = NULL;
    const int result = isVertex
        ? sceGxmReserveVertexDefaultUniformBuffer(context, &buffer)
        : sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
    if (result < 0 || !buffer)
        return false;

    // never write past what the program declared, however much the engine set
    uint32_t words = stage->highWater;
    if (stage->constantWords && words > stage->constantWords)
        words = stage->constantWords;

    return sceGxmSetUniformDataF(buffer, stage->constants, 0, words, stage->shadow) >= 0;
}

bool GxmDraw_Indexed(SceGxmPrimitiveType primitive, const uint16_t *indices,
                     uint32_t indexCount)
{
    if (!indices || !indexCount)
        return false;

    if (!GxmDraw_UploadConstants(&s_vertex, true))
        return false;
    if (!GxmDraw_UploadConstants(&s_fragment, false))
        return false;

    const int result = sceGxmDraw(GxmDevice_Context(), primitive,
                                  SCE_GXM_INDEX_FORMAT_U16, indices, indexCount);
    if (result < 0)
        return false;

    ++s_drawCount;
    return true;
}

uint32_t GxmDraw_DrawCount(void)
{
    return s_drawCount;
}

void GxmDraw_ResetCounters(void)
{
    s_drawCount = 0;
}
