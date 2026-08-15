#include "gxm_video.h"

#include <string.h>

// a YUV plane's stride rounds up to a multiple of 8 texels (GPU guide, YUV linear textures)
static uint32_t GxmVideo_Stride(uint32_t width)
{
    return (width + 7) & ~7u;
}

bool GxmVideo_CreateFrame(GxmVideoFrame *frame, uint32_t width, uint32_t height,
                          GxmVideoPlanes planes)
{
    memset(frame, 0, sizeof(*frame));

    if (!width || !height || (width & 1) || (height & 1))
        return false;
    if (planes == GXM_VIDEO_PLANAR && (width & 15))
        return false;               // a mipless 3-plane texture needs a width multiple of 16

    frame->width = width;
    frame->height = height;
    frame->planes = planes;
    frame->lumaStride = GxmVideo_Stride(width);

    const uint32_t lumaSize = frame->lumaStride * height;
    uint32_t total = lumaSize;

    if (planes == GXM_VIDEO_NV12)
    {
        frame->chromaStride = GxmVideo_Stride(width);       // two interleaved samples per pair
        frame->chromaOffset = lumaSize;
        total += frame->chromaStride * (height / 2);
    }
    else
    {
        frame->chromaStride = GxmVideo_Stride(width / 2);
        frame->chromaOffset = lumaSize;
        frame->secondChromaOffset = lumaSize + frame->chromaStride * (height / 2);
        total += frame->chromaStride * (height / 2) * 2;
    }

    if (!GxmMem_Alloc(&frame->memory, total, GXM_MEM_MAIN_UNCACHED, SCE_GXM_MEMORY_ATTRIB_READ))
        return false;

    const SceGxmTextureFormat format = (planes == GXM_VIDEO_NV12)
        ? SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC0
        : SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0;

    // mip count zero packs the planes with no padding between them
    if (sceGxmTextureInitLinear(&frame->texture, frame->memory.base, format,
                                width, height, 0) < 0)
    {
        GxmMem_Free(&frame->memory);
        memset(frame, 0, sizeof(*frame));
        return false;
    }

    sceGxmTextureSetMinFilter(&frame->texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&frame->texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&frame->texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&frame->texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return true;
}

static void GxmVideo_CopyPlane(uint8_t *dst, uint32_t dstStride,
                               const uint8_t *src, uint32_t srcStride,
                               uint32_t bytesPerRow, uint32_t rows)
{
    if (dstStride == srcStride && dstStride == bytesPerRow)
    {
        memcpy(dst, src, (size_t)bytesPerRow * rows);
        return;
    }
    for (uint32_t y = 0; y < rows; ++y)
    {
        memcpy(dst, src, bytesPerRow);
        dst += dstStride;
        src += srcStride;
    }
}

bool GxmVideo_Upload(GxmVideoFrame *frame, const void *luma, uint32_t lumaStride,
                     const void *chroma, uint32_t chromaStride,
                     const void *secondChroma, uint32_t secondChromaStride)
{
    if (!frame->memory.base || !luma || !chroma)
        return false;
    if (frame->planes == GXM_VIDEO_PLANAR && !secondChroma)
        return false;

    uint8_t *base = (uint8_t *)frame->memory.base;
    const uint32_t halfHeight = frame->height / 2;

    GxmVideo_CopyPlane(base, frame->lumaStride, (const uint8_t *)luma, lumaStride,
                       frame->width, frame->height);

    if (frame->planes == GXM_VIDEO_NV12)
    {
        GxmVideo_CopyPlane(base + frame->chromaOffset, frame->chromaStride,
                           (const uint8_t *)chroma, chromaStride, frame->width, halfHeight);
        return true;
    }

    GxmVideo_CopyPlane(base + frame->chromaOffset, frame->chromaStride,
                       (const uint8_t *)chroma, chromaStride, frame->width / 2, halfHeight);
    GxmVideo_CopyPlane(base + frame->secondChromaOffset, frame->chromaStride,
                       (const uint8_t *)secondChroma, secondChromaStride,
                       frame->width / 2, halfHeight);
    return true;
}

void GxmVideo_FreeFrame(GxmVideoFrame *frame)
{
    if (!frame->memory.base)
        return;
    GxmMem_Free(&frame->memory);
    memset(frame, 0, sizeof(*frame));
}

const SceGxmTexture *GxmVideo_Texture(const GxmVideoFrame *frame)
{
    return &frame->texture;
}
