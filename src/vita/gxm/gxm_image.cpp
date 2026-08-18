#include "gxm_image.h"

#include <vita/platform/vita_system.h>

#include <stdlib.h>
#include <string.h>

static uint32_t s_imageCount;

// a zone load swizzles thousands of levels, and a megabyte taken and returned each time leaves
// the heap too broken up to serve the next one
static void *s_stage;
static uint32_t s_stageSize;
static bool s_stageBusy;

static void *GxmImage_StageAcquire(uint32_t size)
{
    // a second loader thread gets its own buffer rather than sharing this one mid-level
    if (s_stageBusy)
        return malloc(size);

    if (size > s_stageSize)
    {
        void *grown = realloc(s_stage, size);
        if (!grown)
        {
            VitaSys_LogPrintf("swizzle stage: %u bytes refused, holding %u\n", size, s_stageSize);
            VitaSys_LogFlush();
            return NULL;
        }
        s_stage = grown;
        s_stageSize = size;
    }
    s_stageBusy = true;
    return s_stage;
}

static void GxmImage_StageRelease(void *bits)
{
    if (bits == s_stage)
        s_stageBusy = false;
    else
        free(bits);
}

static bool GxmImage_ImageFormat(uint32_t d3dFormat, uint32_t *imageFormat, bool *compressed)
{
    switch (d3dFormat)
    {
    case GXM_D3DFMT_A8R8G8B8: *imageFormat = GXM_IMG_A8R8G8B8; *compressed = false; return true;
    case GXM_D3DFMT_X8R8G8B8: *imageFormat = GXM_IMG_X8R8G8B8; *compressed = false; return true;
    case GXM_D3DFMT_A8L8:     *imageFormat = GXM_IMG_A8L8;     *compressed = false; return true;
    case GXM_D3DFMT_L8:       *imageFormat = GXM_IMG_L8;       *compressed = false; return true;
    case GXM_D3DFMT_A8:       *imageFormat = GXM_IMG_A8;       *compressed = false; return true;
    case GXM_D3DFMT_DXT1:     *imageFormat = GXM_IMG_DXT1;     *compressed = true;  return true;
    case GXM_D3DFMT_DXT3:     *imageFormat = GXM_IMG_DXT3;     *compressed = true;  return true;
    case GXM_D3DFMT_DXT5:     *imageFormat = GXM_IMG_DXT5;     *compressed = true;  return true;
    default:
        return false;
    }
}

static uint32_t GxmImage_FullMipCount(uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t count = 1;
    for (uint32_t extent = 1; extent < width || extent < height || extent < depth; extent *= 2)
        ++count;
    return count;
}

static GxmImage *GxmImage_Alloc(void)
{
    GxmImage *image = (GxmImage *)malloc(sizeof(GxmImage));
    if (!image)
        return NULL;

    memset(image, 0, sizeof(*image));
    image->refCount = 1;
    ++s_imageCount;
    return image;
}

GxmImage *GxmImage_Create2D(uint32_t d3dFormat, uint32_t width, uint32_t height,
                            uint32_t mipCount)
{
    uint32_t imageFormat;
    bool compressed;
    if (!GxmImage_ImageFormat(d3dFormat, &imageFormat, &compressed))
        return NULL;

    if (!mipCount)
        mipCount = GxmImage_FullMipCount(width, height, 1);

    GxmImage *image = GxmImage_Alloc();
    if (!image)
        return NULL;

    if (!GxmTexture_Create(&image->texture, imageFormat, width, height, mipCount, false))
    {
        free(image);
        --s_imageCount;
        return NULL;
    }
    return image;
}

GxmImage *GxmImage_Create3D(uint32_t d3dFormat, uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t imageFormat;
    bool compressed;
    if (!GxmImage_ImageFormat(d3dFormat, &imageFormat, &compressed))
        return NULL;

    GxmImage *image = GxmImage_Alloc();
    if (!image)
        return NULL;

    if (!GxmTexture_Create3D(&image->texture, imageFormat, width, height, depth))
    {
        free(image);
        --s_imageCount;
        return NULL;
    }
    return image;
}

GxmImage *GxmImage_CreateCube(uint32_t d3dFormat, uint32_t edgeLength, uint32_t mipCount)
{
    uint32_t imageFormat;
    bool compressed;
    if (!GxmImage_ImageFormat(d3dFormat, &imageFormat, &compressed))
        return NULL;

    if (!mipCount)
        mipCount = GxmImage_FullMipCount(edgeLength, edgeLength, 1);

    GxmImage *image = GxmImage_Alloc();
    if (!image)
        return NULL;

    if (!GxmTexture_Create(&image->texture, imageFormat, edgeLength, edgeLength, mipCount, true))
    {
        free(image);
        --s_imageCount;
        return NULL;
    }
    return image;
}

void GxmImage_AddRef(GxmImage *image)
{
    if (image)
        ++image->refCount;
}

void GxmImage_Release(GxmImage *image)
{
    if (!image || --image->refCount)
        return;

    GxmTexture_Free(&image->texture);
    free(image);
    --s_imageCount;
}

bool GxmImage_MapLevel(const GxmImage *image, uint32_t mipLevel, uint32_t face,
                       void **bits, uint32_t *rowPitch, uint32_t *slicePitch)
{
    const GxmTexture *texture = &image->texture;
    if (!texture->memory.base || mipLevel >= texture->mipCount)
        return false;

    const uint32_t faces = texture->isCube ? 6u : 1u;
    if (face >= faces)
        return false;

    const uint32_t format = texture->imageFormat;
    const bool compressed = format >= GXM_IMG_DXT1;
    const bool linearLayout = GxmTexture_IsLinearLayout(texture);

    uint32_t offset = 0;
    for (uint32_t f = 0; f < faces; ++f)
    {
        uint32_t w = texture->width, h = texture->height;
        for (uint32_t level = 0; level < texture->mipCount; ++level)
        {
            const uint32_t size = GxmTexture_LevelSizeEx(format, w, h, linearLayout);
            if (f == face && level == mipLevel)
            {
                // one row of blocks for a compressed format, one row of texels otherwise
                *rowPitch = GxmTexture_LevelSizeEx(format, w, compressed ? 4u : 1u, linearLayout);
                *bits = (uint8_t *)texture->memory.base + offset;

                // a volume is a vertical strip, so a slice is its share of the rows
                *slicePitch = texture->depth > 1 ? size / texture->depth : size;
                return true;
            }
            offset += size;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    return false;
}

// block formats always, and cube faces even uncompressed, carry the swizzled layout
static bool GxmImage_LevelNeedsSwizzle(const GxmTexture *texture)
{
    bool isBlock = false;
    GxmTexture_ElemBytes(texture->imageFormat, &isBlock);
    return isBlock || texture->isCube;
}

bool GxmImage_MapLevelWrite(const GxmImage *image, uint32_t mipLevel, uint32_t face,
                            void **bits, uint32_t *rowPitch, uint32_t *slicePitch)
{
    if (!GxmImage_MapLevel(image, mipLevel, face, bits, rowPitch, slicePitch))
        return false;
    if (!GxmImage_LevelNeedsSwizzle(&image->texture))
        return true;

    void *stage = GxmImage_StageAcquire(*slicePitch);
    if (!stage)
        return false;
    *bits = stage;
    return true;
}

void GxmImage_UnmapLevelWrite(const GxmImage *image, uint32_t mipLevel, uint32_t face, void *bits)
{
    void *real;
    uint32_t rowPitch, slicePitch;
    if (!bits || !GxmImage_MapLevel(image, mipLevel, face, &real, &rowPitch, &slicePitch))
        return;
    if (bits == real)
        return;

    const GxmTexture *texture = &image->texture;
    uint32_t w = texture->width >> mipLevel;
    uint32_t h = texture->height >> mipLevel;
    if (!w) w = 1;
    if (!h) h = 1;

    bool isBlock = false;
    const uint32_t elem = GxmTexture_ElemBytes(texture->imageFormat, &isBlock);
    if (isBlock)
    {
        w = (w + 3) / 4;
        h = (h + 3) / 4;
    }
    GxmTexture_SwizzleGrid((uint8_t *)real, (const uint8_t *)bits, w, h, elem);
    GxmImage_StageRelease(bits);
}

uint32_t GxmImage_FormatOf(const GxmTexture *texture)
{
    switch (sceGxmTextureGetFormat(&texture->texture))
    {
    case SCE_GXM_TEXTURE_FORMAT_A8R8G8B8:     return GXM_D3DFMT_A8R8G8B8;
    case SCE_GXM_TEXTURE_FORMAT_A8B8G8R8:     return GXM_D3DFMT_A8B8G8R8;
    case SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB: return GXM_D3DFMT_X8R8G8B8;
    case SCE_GXM_TEXTURE_FORMAT_R5G6B5:       return GXM_D3DFMT_R5G6B5;
    case SCE_GXM_TEXTURE_FORMAT_A8L8:         return GXM_D3DFMT_A8L8;
    case SCE_GXM_TEXTURE_FORMAT_L8:           return GXM_D3DFMT_L8;
    case SCE_GXM_TEXTURE_FORMAT_A8:           return GXM_D3DFMT_A8;
    case SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR:    return GXM_D3DFMT_DXT1;
    case SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR:    return GXM_D3DFMT_DXT3;
    case SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR:    return GXM_D3DFMT_DXT5;
    case SCE_GXM_TEXTURE_FORMAT_F32_R:        return GXM_D3DFMT_R32F;
    case SCE_GXM_TEXTURE_FORMAT_F16F16_GR:    return GXM_D3DFMT_G16R16F;
    default:                                  return 0;
    }
}

uint32_t GxmImage_Count(void)
{
    return s_imageCount;
}
