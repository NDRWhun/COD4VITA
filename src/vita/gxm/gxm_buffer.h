// Vertex and index buffers in GPU-visible memory, plus the per-frame ring.
#pragma once

#include <stdint.h>

#include "gxm_memory.h"
#include "gxm_vertex.h"

struct GxmBuffer
{
    GxmAlloc memory;
    uint32_t size;
    uint32_t used;

    // one region per frame in flight, for a buffer rewritten every frame
    uint32_t regionSize;
    uint32_t regionCount;
    uint32_t region;
};

bool GxmBuffer_Create(GxmBuffer *buffer, uint32_t size, bool cpuCached);

// size is the caller's buffer size; the allocation is that many times larger
bool GxmBuffer_CreateFramed(GxmBuffer *buffer, uint32_t size, uint32_t frames);

// the region the CPU may write and the GPU is not reading
void *GxmBuffer_Base(const GxmBuffer *buffer);

// call once per lock that discards the previous contents
void GxmBuffer_Discard(GxmBuffer *buffer);
void GxmBuffer_Free(GxmBuffer *buffer);

// D3DCOLOR is ARGB in a dword, so a byte-order read gives BGRA; this puts it right
void GxmBuffer_SwapColorBytes(void *vertices, uint32_t vertexCount, uint32_t stride,
                              uint32_t colorOffset);

// applies the swap to every attribute the layout marked as D3DCOLOR-sourced
void GxmBuffer_FixColorAttributes(void *vertices, uint32_t vertexCount,
                                  const GxmVertexLayout *layout);

// the frame ring, for data that changes every frame
bool GxmRing_Init(uint32_t size);
void GxmRing_Shutdown(void);
void GxmRing_BeginFrame(void);
void *GxmRing_Alloc(uint32_t size, uint32_t alignment);
uint32_t GxmRing_BytesUsed(void);
uint32_t GxmRing_BytesPeak(void);
