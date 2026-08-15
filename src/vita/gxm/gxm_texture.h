// The engine's image formats mapped onto GXM textures.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_memory.h"

// the engine's image file format byte (r_image_load_obj.cpp)
enum GxmImageFormat
{
    GXM_IMG_A8R8G8B8 = 1,
    GXM_IMG_X8R8G8B8 = 2,
    GXM_IMG_A8L8 = 3,
    GXM_IMG_L8 = 4,
    GXM_IMG_A8 = 5,
    GXM_IMG_WAVELET_A8R8G8B8 = 6,
    GXM_IMG_WAVELET_X8R8G8B8 = 7,
    GXM_IMG_WAVELET_A8L8 = 8,
    GXM_IMG_WAVELET_L8 = 9,
    GXM_IMG_WAVELET_A8 = 10,
    GXM_IMG_DXT1 = 11,
    GXM_IMG_DXT3 = 12,
    GXM_IMG_DXT5 = 13
};

struct GxmTexture
{
    SceGxmTexture texture;
    GxmAlloc memory;            // payload owned by this texture
    uint32_t imageFormat;
    uint16_t width;
    uint16_t height;
    uint16_t depth;             // slice count for an emulated volume, else 1
    uint8_t mipCount;
    bool isCube;

    // for an emulated volume: (slices, 1/slices) as the shader's volumeLayout
    float volumeLayout[2];
};

bool GxmTexture_Create(GxmTexture *texture, uint32_t imageFormat,
                       uint32_t width, uint32_t height, uint32_t mipCount, bool cube);

// lays a volume out as a vertical strip of slices and fills volumeLayout
bool GxmTexture_Create3D(GxmTexture *texture, uint32_t imageFormat,
                         uint32_t width, uint32_t height, uint32_t depth);

// uploads one mip; src is tightly packed in the engine's layout
bool GxmTexture_Upload(GxmTexture *texture, uint32_t mipLevel, uint32_t face,
                       const void *src, uint32_t srcSize);

void GxmTexture_SetFilter(GxmTexture *texture, bool linear, bool clampToEdge);
void GxmTexture_Free(GxmTexture *texture);

// bytes a mip level occupies in the engine's packing
uint32_t GxmTexture_LevelSize(uint32_t imageFormat, uint32_t width, uint32_t height);

uint32_t GxmTexture_BytesResident(void);
