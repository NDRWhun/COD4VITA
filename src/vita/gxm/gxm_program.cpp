#include "gxm_program.h"
#include "gxm_device.h"

#include <vita/platform/vita_memory.h>

#include <stdlib.h>
#include <string.h>

#define GXM_MAX_VERTEX_PROGRAMS   1024
#define GXM_MAX_FRAGMENT_PROGRAMS 4096

struct GxmShaderEntry
{
    const SceGxmProgram *program;
    SceGxmShaderPatcherId id;
};

struct GxmVertexEntry
{
    int shader;
    uint32_t layoutHash;
    SceGxmVertexProgram *program;
};

struct GxmFragmentEntry
{
    int shader;
    int linkedVertex;
    uint32_t stateKey;
    SceGxmFragmentProgram *program;
};

static GxmShaderEntry s_shaders[GXM_MAX_SHADERS];
static uint32_t s_shaderCount;

static GxmVertexEntry s_vertexPrograms[GXM_MAX_VERTEX_PROGRAMS];
static uint32_t s_vertexCount;

static GxmFragmentEntry s_fragmentPrograms[GXM_MAX_FRAGMENT_PROGRAMS];
static uint32_t s_fragmentCount;

bool GxmProgram_Init(void)
{
    memset(s_shaders, 0, sizeof(s_shaders));
    memset(s_vertexPrograms, 0, sizeof(s_vertexPrograms));
    memset(s_fragmentPrograms, 0, sizeof(s_fragmentPrograms));
    s_shaderCount = s_vertexCount = s_fragmentCount = 0;
    return true;
}

int GxmProgram_Register(const void *gxp, uint32_t size)
{
    // SceGxmProgram is opaque, so vet the blob tag and let the library check the rest
    if (!gxp || size < 16 || memcmp(gxp, "GXP\0", 4) != 0)
        return -1;

    const SceGxmProgram *program = (const SceGxmProgram *)gxp;
    if (sceGxmProgramCheck(program) < 0)
        return -1;

    // scan, patcher call and slot claim under one lock
    VitaMem_GpuLock();
    if (s_shaderCount >= GXM_MAX_SHADERS)
    {
        VitaMem_GpuUnlock();
        return -1;
    }

    SceGxmShaderPatcherId id;
    if (sceGxmShaderPatcherRegisterProgram(GxmDevice_ShaderPatcher(), program, &id) < 0)
    {
        VitaMem_GpuUnlock();
        return -1;
    }

    const int handle = (int)s_shaderCount++;
    s_shaders[handle].program = program;
    s_shaders[handle].id = id;
    VitaMem_GpuUnlock();
    return handle;
}

const SceGxmProgram *GxmProgram_Get(int handle)
{
    if (handle < 0 || (uint32_t)handle >= s_shaderCount)
        return NULL;
    return s_shaders[handle].program;
}

static uint32_t GxmProgram_HashLayout(const SceGxmVertexAttribute *attributes,
                                      uint32_t attributeCount,
                                      const SceGxmVertexStream *streams,
                                      uint32_t streamCount)
{
    uint32_t h = 2166136261u;
    const unsigned char *bytes = (const unsigned char *)attributes;
    for (uint32_t i = 0; i < attributeCount * sizeof(*attributes); ++i)
    {
        h ^= bytes[i];
        h *= 16777619u;
    }
    bytes = (const unsigned char *)streams;
    for (uint32_t i = 0; i < streamCount * sizeof(*streams); ++i)
    {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

SceGxmVertexProgram *GxmProgram_Vertex(int handle,
                                       const SceGxmVertexAttribute *attributes,
                                       uint32_t attributeCount,
                                       const SceGxmVertexStream *streams,
                                       uint32_t streamCount)
{
    const uint32_t layoutHash =
        GxmProgram_HashLayout(attributes, attributeCount, streams, streamCount);

    VitaMem_GpuLock();
    if (handle < 0 || (uint32_t)handle >= s_shaderCount)
    {
        VitaMem_GpuUnlock();
        return NULL;
    }

    for (uint32_t i = 0; i < s_vertexCount; ++i)
    {
        if (s_vertexPrograms[i].shader == handle && s_vertexPrograms[i].layoutHash == layoutHash)
        {
            SceGxmVertexProgram *cached = s_vertexPrograms[i].program;
            VitaMem_GpuUnlock();
            return cached;
        }
    }

    SceGxmVertexProgram *program = NULL;
    if (s_vertexCount >= GXM_MAX_VERTEX_PROGRAMS ||
        sceGxmShaderPatcherCreateVertexProgram(GxmDevice_ShaderPatcher(), s_shaders[handle].id,
                                               attributes, attributeCount,
                                               streams, streamCount, &program) < 0)
    {
        VitaMem_GpuUnlock();
        return NULL;
    }

    GxmVertexEntry *entry = &s_vertexPrograms[s_vertexCount++];
    entry->shader = handle;
    entry->layoutHash = layoutHash;
    entry->program = program;
    VitaMem_GpuUnlock();
    return program;
}

SceGxmFragmentProgram *GxmProgram_Fragment(int handle, const GxmProgramState *state,
                                           int linkedVertexHandle)
{
    const uint32_t key = GxmState_ProgramKey(state);

    VitaMem_GpuLock();
    if (handle < 0 || (uint32_t)handle >= s_shaderCount)
    {
        VitaMem_GpuUnlock();
        return NULL;
    }

    for (uint32_t i = 0; i < s_fragmentCount; ++i)
    {
        const GxmFragmentEntry *entry = &s_fragmentPrograms[i];
        if (entry->shader == handle && entry->stateKey == key &&
            entry->linkedVertex == linkedVertexHandle)
        {
            SceGxmFragmentProgram *cached = entry->program;
            VitaMem_GpuUnlock();
            return cached;
        }
    }

    // a null blend info writes the output register straight out, colour mask included, so a
    // masked write needs the blend info even with the blend funcs left at NONE
    const SceGxmBlendInfo *blend =
        (state->blendEnabled || state->blend.colorMask != SCE_GXM_COLOR_MASK_ALL)
            ? &state->blend : NULL;
    const SceGxmProgram *linked = GxmProgram_Get(linkedVertexHandle);

    SceGxmFragmentProgram *program = NULL;
    if (s_fragmentCount >= GXM_MAX_FRAGMENT_PROGRAMS ||
        sceGxmShaderPatcherCreateFragmentProgram(GxmDevice_ShaderPatcher(), s_shaders[handle].id,
                                                 SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE,
                                                 blend, linked, &program) < 0)
    {
        VitaMem_GpuUnlock();
        return NULL;
    }

    GxmFragmentEntry *entry = &s_fragmentPrograms[s_fragmentCount++];
    entry->shader = handle;
    entry->linkedVertex = linkedVertexHandle;
    entry->stateKey = key;
    entry->program = program;
    VitaMem_GpuUnlock();
    return program;
}

void GxmProgram_Shutdown(void)
{
    SceGxmShaderPatcher *patcher = GxmDevice_ShaderPatcher();

    VitaMem_GpuLock();
    for (uint32_t i = 0; i < s_fragmentCount; ++i)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, s_fragmentPrograms[i].program);
    for (uint32_t i = 0; i < s_vertexCount; ++i)
        sceGxmShaderPatcherReleaseVertexProgram(patcher, s_vertexPrograms[i].program);
    for (uint32_t i = 0; i < s_shaderCount; ++i)
        sceGxmShaderPatcherUnregisterProgram(patcher, s_shaders[i].id);

    s_shaderCount = s_vertexCount = s_fragmentCount = 0;
    VitaMem_GpuUnlock();
}

uint32_t GxmProgram_VertexCount(void)
{
    return s_vertexCount;
}

uint32_t GxmProgram_FragmentCount(void)
{
    return s_fragmentCount;
}
