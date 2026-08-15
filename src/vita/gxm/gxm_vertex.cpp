#include "gxm_vertex.h"

#include <string.h>

#define STREAM_SOURCE_ABSENT 255

// dest index -> the D3D usage the engine's s_streamDestInfo assigns it
struct GxmDestUsage
{
    uint8_t usage;          // D3DDECLUSAGE
    uint8_t usageIndex;
};

static const GxmDestUsage s_destUsage[12] =
{
    { 0, 0 },                                   // position
    { 3, 0 },                                   // normal
    { 10, 0 }, { 10, 1 },                       // colour 0/1
    { 5, 0 }, { 5, 1 }, { 5, 2 }, { 5, 3 },     // texcoord 0-7
    { 5, 4 }, { 5, 5 }, { 5, 6 }, { 5, 7 },
};

struct GxmFormat
{
    SceGxmAttributeFormat format;
    uint8_t components;
};

// D3DDECLTYPE -> how GXM should unpack the field
static bool GxmVertex_Format(uint8_t d3dType, GxmFormat *out)
{
    switch (d3dType)
    {
    case 0:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;  out->components = 1; return true;
    case 1:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;  out->components = 2; return true;
    case 2:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;  out->components = 3; return true;
    case 3:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;  out->components = 4; return true;
    case 4:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;  out->components = 4; return true;
    case 5:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_U8;   out->components = 4; return true;
    case 6:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_S16;  out->components = 2; return true;
    case 7:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_S16;  out->components = 4; return true;
    case 8:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;  out->components = 4; return true;
    case 9:  out->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N; out->components = 2; return true;
    case 10: out->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N; out->components = 4; return true;
    case 11: out->format = SCE_GXM_ATTRIBUTE_FORMAT_U16N; out->components = 2; return true;
    case 12: out->format = SCE_GXM_ATTRIBUTE_FORMAT_U16N; out->components = 4; return true;
    case 15: out->format = SCE_GXM_ATTRIBUTE_FORMAT_F16;  out->components = 2; return true;
    case 16: out->format = SCE_GXM_ATTRIBUTE_FORMAT_F16;  out->components = 4; return true;
    default: return false;
    }
}

static bool GxmVertex_Semantic(uint8_t usage, SceGxmParameterSemantic *out)
{
    switch (usage)
    {
    case 0:  *out = SCE_GXM_PARAMETER_SEMANTIC_POSITION; return true;
    case 3:  *out = SCE_GXM_PARAMETER_SEMANTIC_NORMAL; return true;
    case 5:  *out = SCE_GXM_PARAMETER_SEMANTIC_TEXCOORD; return true;
    case 6:  *out = SCE_GXM_PARAMETER_SEMANTIC_TANGENT; return true;
    case 10: *out = SCE_GXM_PARAMETER_SEMANTIC_COLOR; return true;
    default: return false;
    }
}

bool GxmVertex_BuildLayout(const SceGxmProgram *vertexProgram,
                           const GxmStreamSource *sources, uint32_t sourceCount,
                           const GxmStreamRouting *routing, uint32_t routingCount,
                           const uint16_t *strides,
                           GxmVertexLayout *out)
{
    memset(out, 0, sizeof(*out));

    if (!vertexProgram || !sources || !routing || !strides)
        return false;

    bool streamUsed[GXM_MAX_VERTEX_STREAMS];
    memset(streamUsed, 0, sizeof(streamUsed));

    for (uint32_t i = 0; i < routingCount; ++i)
    {
        if (out->attributeCount >= GXM_MAX_VERTEX_ATTRIBUTES)
            return false;
        if (routing[i].source >= sourceCount ||
            routing[i].dest >= sizeof(s_destUsage) / sizeof(s_destUsage[0]))
            return false;

        const GxmStreamSource *source = &sources[routing[i].source];
        if (source->stream == STREAM_SOURCE_ABSENT)
            continue;
        if (source->stream >= GXM_MAX_VERTEX_STREAMS)
            return false;

        GxmFormat format;
        if (!GxmVertex_Format(source->type, &format))
            return false;

        const GxmDestUsage *dest = &s_destUsage[routing[i].dest];
        SceGxmParameterSemantic semantic;
        if (!GxmVertex_Semantic(dest->usage, &semantic))
            return false;

        // the shader may not consume this input; skipping keeps the layout minimal
        const SceGxmProgramParameter *parameter =
            sceGxmProgramFindParameterBySemantic(vertexProgram, semantic, dest->usageIndex);
        if (!parameter)
            continue;

        SceGxmVertexAttribute *attribute = &out->attributes[out->attributeCount];
        attribute->streamIndex = source->stream;
        attribute->offset = source->offset;
        attribute->format = (uint8_t)format.format;
        attribute->componentCount = format.components;
        attribute->regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(parameter);

        if (source->type == 4)                  // D3DCOLOR reaches the shader as BGRA
            out->bgraMask |= 1u << out->attributeCount;

        streamUsed[source->stream] = true;
        ++out->attributeCount;
    }

    if (!out->attributeCount)
        return false;

    // streams must be described up to the highest one an attribute references
    for (uint32_t i = 0; i < GXM_MAX_VERTEX_STREAMS; ++i)
    {
        if (!streamUsed[i])
            continue;
        out->streams[i].stride = strides[i];
        out->streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
        out->streamCount = i + 1;
    }

    return out->streamCount != 0;
}
