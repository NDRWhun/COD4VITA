#include "gxm_buffer.h"

#include <psp2/gxm.h>
#include <string.h>

#define GXM_RING_FRAMES 3

struct GxmRing
{
    GxmAlloc memory;
    uint32_t frameSize;
    uint32_t frame;
    uint32_t offset;
    uint32_t peak;
    bool ready;
};

static GxmRing s_ring;

bool GxmBuffer_Create(GxmBuffer *buffer, uint32_t size, bool cpuCached)
{
    memset(buffer, 0, sizeof(*buffer));
    if (!size)
        return false;

    // pooled: a fastfile brings thousands of model buffers, and a memblock each is a kernel
    // object per buffer rounded up to a page
    if (cpuCached)
    {
        if (!GxmMem_Alloc(&buffer->memory, size, GXM_MEM_MAIN, SCE_GXM_MEMORY_ATTRIB_READ))
            return false;
    }
    else
    {
        // small buffers leave the CDRAM reserve alone; the movie decoder keeps its phycont
        const bool cdramSpare = GxmMem_FreeCdram() > size + GXM_CDRAM_GEOMETRY_RESERVE;
        const bool phySpare = GxmMem_FreePhycont() > size + GXM_PHYCONT_DECODER_RESERVE;
        bool ok = (cdramSpare && GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_CDRAM, 4)) ||
                  (phySpare && GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_PHYCONT, 4)) ||
                  GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_MAIN_UNCACHED, 4) ||
                  GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_CDRAM, 4);

        // the ballast opens a contiguous hole that only a buffer this large may take
        if (!ok && size >= 8u * 1024u * 1024u && GxmMem_BallastRelease())
            ok = GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_CDRAM, 4);
        if (!ok && !GxmMem_AllocPooled(&buffer->memory, size, GXM_MEM_PHYCONT, 4))
            return false;
    }

    buffer->size = buffer->memory.size;
    buffer->regionSize = buffer->memory.size;
    buffer->regionCount = 1;
    buffer->region = 0;
    return true;
}

bool GxmBuffer_CreateFramed(GxmBuffer *buffer, uint32_t size, uint32_t frames)
{
    if (!frames)
        frames = 1;
    if (!GxmBuffer_Create(buffer, size * frames, false))
        return false;

    buffer->size = size;
    buffer->regionSize = size;
    buffer->regionCount = frames;
    buffer->region = 0;
    return true;
}

void *GxmBuffer_Base(const GxmBuffer *buffer)
{
    return (uint8_t *)buffer->memory.base + buffer->region * buffer->regionSize;
}

void GxmBuffer_Discard(GxmBuffer *buffer)
{
    if (buffer->regionCount > 1)
        buffer->region = (buffer->region + 1) % buffer->regionCount;
}

void GxmBuffer_Free(GxmBuffer *buffer)
{
    if (!buffer->memory.base)
        return;
    const bool wasLarge = buffer->memory.size >= 8u * 1024u * 1024u;
    GxmMem_Free(&buffer->memory);
    memset(buffer, 0, sizeof(*buffer));
    // a level's big buffer just left, so the hole it came from is held again
    if (wasLarge)
        GxmMem_BallastInit();
}

void GxmBuffer_SwapColorBytes(void *vertices, uint32_t vertexCount, uint32_t stride,
                              uint32_t colorOffset)
{
    uint8_t *cursor = (uint8_t *)vertices + colorOffset;
    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const uint8_t b = cursor[0];
        cursor[0] = cursor[2];
        cursor[2] = b;
        cursor += stride;
    }
}

void GxmBuffer_FixColorAttributes(void *vertices, uint32_t vertexCount,
                                  const GxmVertexLayout *layout)
{
    for (uint32_t i = 0; i < layout->attributeCount; ++i)
    {
        if (!(layout->bgraMask & (1u << i)))
            continue;

        const SceGxmVertexAttribute *attribute = &layout->attributes[i];
        const uint32_t stride = layout->streams[attribute->streamIndex].stride;
        if (stride)
            GxmBuffer_SwapColorBytes(vertices, vertexCount, stride, attribute->offset);
    }
}

bool GxmRing_Init(uint32_t size)
{
    memset(&s_ring, 0, sizeof(s_ring));

    // one region per in-flight frame, so a write never lands in what the GPU reads
    const uint32_t frameSize = (size + GXM_RING_FRAMES - 1) / GXM_RING_FRAMES;
    if (!GxmMem_Alloc(&s_ring.memory, frameSize * GXM_RING_FRAMES,
                      GXM_MEM_MAIN_UNCACHED, SCE_GXM_MEMORY_ATTRIB_READ))
        return false;

    s_ring.frameSize = frameSize;
    s_ring.ready = true;
    return true;
}

void GxmRing_BeginFrame(void)
{
    if (!s_ring.ready)
        return;
    s_ring.frame = (s_ring.frame + 1) % GXM_RING_FRAMES;
    s_ring.offset = 0;
}

void *GxmRing_Alloc(uint32_t size, uint32_t alignment)
{
    if (!s_ring.ready || !size)
        return NULL;

    const uint32_t aligned = (s_ring.offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > s_ring.frameSize)
        return NULL;                    // the caller decides what to do; nothing is dropped silently

    s_ring.offset = aligned + size;
    if (s_ring.offset > s_ring.peak)
        s_ring.peak = s_ring.offset;

    return (uint8_t *)s_ring.memory.base + s_ring.frame * s_ring.frameSize + aligned;
}

void GxmRing_Shutdown(void)
{
    if (!s_ring.ready)
        return;
    GxmMem_Free(&s_ring.memory);
    memset(&s_ring, 0, sizeof(s_ring));
}

uint32_t GxmRing_BytesUsed(void)
{
    return s_ring.offset;
}

uint32_t GxmRing_BytesPeak(void)
{
    return s_ring.peak;
}
