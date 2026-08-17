// GfxImage textures keyed by the engine's D3D pixel format, refcounted so images can share one.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_texture.h"

// the _D3DFORMAT codes the engine hands to Image_Create*Texture_PC
enum GxmD3DFormat
{
    GXM_D3DFMT_A8R8G8B8 = 21,
    GXM_D3DFMT_X8R8G8B8 = 22,
    GXM_D3DFMT_R5G6B5   = 23,
    GXM_D3DFMT_A8       = 28,
    GXM_D3DFMT_A8B8G8R8 = 32,
    GXM_D3DFMT_L8       = 50,
    GXM_D3DFMT_A8L8     = 51,
    GXM_D3DFMT_G16R16F  = 112,
    GXM_D3DFMT_R32F     = 114,
    GXM_D3DFMT_DXT1     = 0x31545844,
    GXM_D3DFMT_DXT3     = 0x33545844,
    GXM_D3DFMT_DXT5     = 0x35545844
};

// GxmTexture sits at offset 0: engine code casts GfxTexture::basemap straight to it
struct GxmImage
{
    GxmTexture texture;
    uint32_t refCount;
};

// mipCount 0 means the full chain, matching what D3D's CreateTexture does with 0 levels
GxmImage *GxmImage_Create2D(uint32_t d3dFormat, uint32_t width, uint32_t height,
                            uint32_t mipCount);
GxmImage *GxmImage_Create3D(uint32_t d3dFormat, uint32_t width, uint32_t height, uint32_t depth);
GxmImage *GxmImage_CreateCube(uint32_t d3dFormat, uint32_t edgeLength, uint32_t mipCount);

void GxmImage_AddRef(GxmImage *image);
void GxmImage_Release(GxmImage *image);

// GXM texture memory is CPU-visible, so a D3D lock is just the level's address and pitch
bool GxmImage_MapLevel(const GxmImage *image, uint32_t mipLevel, uint32_t face,
                       void **bits, uint32_t *rowPitch, uint32_t *slicePitch);

// a write mapping: swizzled layouts stage the linear copy, and the unmap reorders it into place
bool GxmImage_MapLevelWrite(const GxmImage *image, uint32_t mipLevel, uint32_t face,
                            void **bits, uint32_t *rowPitch, uint32_t *slicePitch);
void GxmImage_UnmapLevelWrite(const GxmImage *image, uint32_t mipLevel, uint32_t face, void *bits);

// the D3D format the texture carries, read back from GXM so render-target views work too
uint32_t GxmImage_FormatOf(const GxmTexture *texture);

uint32_t GxmImage_Count(void);
