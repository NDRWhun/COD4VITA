// Builds a GXM vertex layout from the engine's stream routing.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#define GXM_MAX_VERTEX_ATTRIBUTES 9
#define GXM_MAX_VERTEX_STREAMS    2

// one entry of the engine's per-declaration source table
struct GxmStreamSource
{
    uint8_t stream;         // 255 marks the source as absent
    uint8_t offset;
    uint8_t type;           // D3DDECLTYPE
};

struct GxmStreamRouting
{
    uint8_t source;
    uint8_t dest;
};

struct GxmVertexLayout
{
    SceGxmVertexAttribute attributes[GXM_MAX_VERTEX_ATTRIBUTES];
    SceGxmVertexStream streams[GXM_MAX_VERTEX_STREAMS];
    uint32_t attributeCount;
    uint32_t streamCount;

    // attributes fed from a D3DCOLOR field, which is BGRA in memory
    uint32_t bgraMask;
};

// strides are indexed by stream; a stream with no attributes is dropped
bool GxmVertex_BuildLayout(const SceGxmProgram *vertexProgram,
                           const GxmStreamSource *sources, uint32_t sourceCount,
                           const GxmStreamRouting *routing, uint32_t routingCount,
                           const uint16_t *strides,
                           GxmVertexLayout *out);
