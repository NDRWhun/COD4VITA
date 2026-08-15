// Cinematic frames as GXM YUV textures.
//
// The decoder hands back planar YUV, and GXM samples that natively with the colour
// conversion in the texture format, so a frame needs no conversion pass and the
// cinematic quad samples one texture instead of the three the PC path used.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_memory.h"

enum GxmVideoPlanes
{
    GXM_VIDEO_NV12 = 2,     // luma plane then interleaved chroma
    GXM_VIDEO_PLANAR = 3    // luma, then the two chroma planes
};

struct GxmVideoFrame
{
    SceGxmTexture texture;
    GxmAlloc memory;
    uint32_t width;
    uint32_t height;
    uint32_t lumaStride;
    uint32_t chromaStride;
    uint32_t chromaOffset;
    uint32_t secondChromaOffset;    // planar only
    GxmVideoPlanes planes;
};

bool GxmVideo_CreateFrame(GxmVideoFrame *frame, uint32_t width, uint32_t height,
                          GxmVideoPlanes planes);
void GxmVideo_FreeFrame(GxmVideoFrame *frame);

// copies a decoded frame in; strides are the decoder's, in bytes
bool GxmVideo_Upload(GxmVideoFrame *frame, const void *luma, uint32_t lumaStride,
                     const void *chroma, uint32_t chromaStride,
                     const void *secondChroma, uint32_t secondChromaStride);

const SceGxmTexture *GxmVideo_Texture(const GxmVideoFrame *frame);
