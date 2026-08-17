#include "gxm_texture.h"
#include "gxm_memory.h"

#include <vita/platform/vita_system.h>

#include <string.h>

// UBC2 and UBC3 have the strictest requirement at 16 bytes (GPU guide, memory alignment)
#define GXM_TEXTURE_ALIGNMENT 16

static uint32_t s_bytesResident;

struct GxmTextureFormat
{
    SceGxmTextureFormat format;
    uint8_t bytesPerPixel;      // zero for the block compressed formats
    uint8_t blockBytes;         // bytes per 4x4 block, zero when not compressed
};

static bool GxmTexture_Format(uint32_t imageFormat, GxmTextureFormat *out)
{
    switch (imageFormat)
    {
    case GXM_IMG_A8R8G8B8:
        out->format = SCE_GXM_TEXTURE_FORMAT_A8R8G8B8; out->bytesPerPixel = 4; out->blockBytes = 0;
        return true;
    case GXM_IMG_X8R8G8B8:
        out->format = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB; out->bytesPerPixel = 4; out->blockBytes = 0;
        return true;
    case GXM_IMG_A8L8:
        out->format = SCE_GXM_TEXTURE_FORMAT_A8L8; out->bytesPerPixel = 2; out->blockBytes = 0;
        return true;
    case GXM_IMG_L8:
        out->format = SCE_GXM_TEXTURE_FORMAT_L8; out->bytesPerPixel = 1; out->blockBytes = 0;
        return true;
    case GXM_IMG_A8:
        out->format = SCE_GXM_TEXTURE_FORMAT_A8; out->bytesPerPixel = 1; out->blockBytes = 0;
        return true;
    case GXM_IMG_DXT1:
        out->format = SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR; out->bytesPerPixel = 0; out->blockBytes = 8;
        return true;
    case GXM_IMG_DXT3:
        out->format = SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR; out->bytesPerPixel = 0; out->blockBytes = 16;
        return true;
    case GXM_IMG_DXT5:
        out->format = SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR; out->bytesPerPixel = 0; out->blockBytes = 16;
        return true;
    default:
        // the wavelet formats are decoded by the engine before reaching here
        return false;
    }
}

uint32_t GxmTexture_LevelSize(uint32_t imageFormat, uint32_t width, uint32_t height)
{
    GxmTextureFormat format;
    if (!GxmTexture_Format(imageFormat, &format))
        return 0;

    if (format.blockBytes)
    {
        const uint32_t blocksX = (width + 3) / 4;
        const uint32_t blocksY = (height + 3) / 4;
        return blocksX * blocksY * format.blockBytes;
    }
    return width * height * format.bytesPerPixel;
}

uint32_t GxmTexture_ElemBytes(uint32_t imageFormat, bool *isBlock)
{
    GxmTextureFormat format;
    if (!GxmTexture_Format(imageFormat, &format))
    {
        if (isBlock)
            *isBlock = false;
        return 0;
    }
    if (isBlock)
        *isBlock = format.blockBytes != 0;
    return format.blockBytes ? format.blockBytes : format.bytesPerPixel;
}

// even bits of the morton index, compacted to one axis
static uint32_t GxmTexture_MortonAxis(uint32_t value)
{
    value &= 0x55555555u;
    value = (value | (value >> 1)) & 0x33333333u;
    value = (value | (value >> 2)) & 0x0F0F0F0Fu;
    value = (value | (value >> 4)) & 0x00FF00FFu;
    value = (value | (value >> 8)) & 0x0000FFFFu;
    return value;
}

void GxmTexture_SwizzleGrid(uint8_t *dst, const uint8_t *src, uint32_t wide, uint32_t high,
                            uint32_t elemBytes)
{
    // non-pow2 has no morton layout: copied linear, logged
    if (!wide || !high || (wide & (wide - 1)) || (high & (high - 1)))
    {
        VitaSys_LogPrintf("swizzle: %ux%u grid is not pow2, left linear\n", wide, high);
        memcpy(dst, src, (size_t)wide * high * elemBytes);
        return;
    }

    // square morton tiles of the short side, walked along the long side
    const uint32_t tile = wide < high ? wide : high;
    for (uint32_t tileY = 0; tileY < high; tileY += tile)
    {
        for (uint32_t tileX = 0; tileX < wide; tileX += tile)
        {
            for (uint32_t d = 0; d < tile * tile; ++d)
            {
                const uint32_t y = tileY + GxmTexture_MortonAxis(d);
                const uint32_t x = tileX + GxmTexture_MortonAxis(d >> 1);
                memcpy(dst, src + ((size_t)y * wide + x) * elemBytes, elemBytes);
                dst += elemBytes;
            }
        }
    }
}

static uint32_t GxmTexture_TotalSize(uint32_t imageFormat, uint32_t width, uint32_t height,
                                     uint32_t mipCount, uint32_t faces)
{
    uint32_t total = 0;
    for (uint32_t face = 0; face < faces; ++face)
    {
        uint32_t w = width, h = height;
        for (uint32_t level = 0; level < mipCount; ++level)
        {
            total += GxmTexture_LevelSize(imageFormat, w, h);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    return total;
}

static bool GxmTexture_Allocate(GxmTexture *texture, uint32_t imageFormat,
                                uint32_t width, uint32_t height, uint32_t mipCount,
                                uint32_t faces)
{
    GxmTextureFormat format;
    if (!GxmTexture_Format(imageFormat, &format))
        return false;

    const uint32_t size = GxmTexture_TotalSize(imageFormat, width, height, mipCount, faces);
    if (!size)
        return false;

    // pooled: a texture per memblock would round every one up to a 256KB CDRAM page
    if (!GxmMem_AllocPooled(&texture->memory, size, GXM_MEM_CDRAM, GXM_TEXTURE_ALIGNMENT))
    {
        if (!GxmMem_AllocPooled(&texture->memory, size, GXM_MEM_MAIN_UNCACHED,
                                GXM_TEXTURE_ALIGNMENT))
            return false;
    }

    texture->imageFormat = imageFormat;
    texture->width = (uint16_t)width;
    texture->height = (uint16_t)height;
    texture->mipCount = (uint8_t)mipCount;

    s_bytesResident += texture->memory.size;
    return true;
}

bool GxmTexture_Create(GxmTexture *texture, uint32_t imageFormat,
                       uint32_t width, uint32_t height, uint32_t mipCount, bool cube)
{
    memset(texture, 0, sizeof(*texture));
    texture->depth = 1;
    texture->isCube = cube;

    GxmTextureFormat format;
    if (!GxmTexture_Format(imageFormat, &format))
        return false;
    if (!mipCount)
        mipCount = 1;
    if (!GxmTexture_Allocate(texture, imageFormat, width, height, mipCount, cube ? 6 : 1))
        return false;

    // block formats and cube faces live in the swizzled layout; only flat uncompressed is linear
    const bool swizzled = cube || format.blockBytes != 0;
    if (swizzled && ((width & (width - 1)) || (height & (height - 1))))
    {
        VitaSys_LogPrintf("texture img %u %ux%u: swizzled layout needs pow2 sides\n",
                          imageFormat, width, height);
        GxmTexture_Free(texture);
        return false;
    }

    int result;
    if (cube)
        result = sceGxmTextureInitCube(&texture->texture, texture->memory.base, format.format,
                                       width, height, mipCount);
    else if (format.blockBytes)
        result = sceGxmTextureInitSwizzled(&texture->texture, texture->memory.base, format.format,
                                           width, height, mipCount);
    else
        result = sceGxmTextureInitLinear(&texture->texture, texture->memory.base, format.format,
                                         width, height, mipCount);
    if (result < 0)
    {
        VitaSys_LogPrintf("texture img %u %ux%u mips %u cube %i: init failed 0x%08x\n",
                          imageFormat, width, height, mipCount, (int)cube, (unsigned)result);
        GxmTexture_Free(texture);
        return false;
    }
    return true;
}

bool GxmTexture_Create3D(GxmTexture *texture, uint32_t imageFormat,
                         uint32_t width, uint32_t height, uint32_t depth)
{
    // slices stack vertically, so the shader's v coordinate selects one
    if (!GxmTexture_Create(texture, imageFormat, width, height * depth, 1, false))
        return false;

    texture->depth = (uint16_t)depth;
    texture->volumeLayout[0] = (float)depth;
    texture->volumeLayout[1] = 1.0f / (float)depth;

    // a strip has no meaningful mip chain: neighbouring slices would bleed together
    sceGxmTextureSetMipFilter(&texture->texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    return true;
}

bool GxmTexture_Upload(GxmTexture *texture, uint32_t mipLevel, uint32_t face,
                       const void *src, uint32_t srcSize)
{
    if (!texture->memory.base || !src || mipLevel >= texture->mipCount)
        return false;

    const uint32_t faces = texture->isCube ? 6 : 1;
    if (face >= faces)
        return false;

    uint32_t offset = 0;
    for (uint32_t f = 0; f < faces; ++f)
    {
        uint32_t w = texture->width, h = texture->height;
        for (uint32_t level = 0; level < texture->mipCount; ++level)
        {
            const uint32_t size = GxmTexture_LevelSize(texture->imageFormat, w, h);
            if (f == face && level == mipLevel)
            {
                if (srcSize > size)
                    return false;
                memcpy((uint8_t *)texture->memory.base + offset, src, srcSize);
                return true;
            }
            offset += size;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    return false;
}

void GxmTexture_SetFilter(GxmTexture *texture, bool linear, bool clampToEdge)
{
    const SceGxmTextureFilter filter = linear
        ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
    sceGxmTextureSetMinFilter(&texture->texture, filter);
    sceGxmTextureSetMagFilter(&texture->texture, filter);

    const SceGxmTextureAddrMode mode = clampToEdge
        ? SCE_GXM_TEXTURE_ADDR_CLAMP : SCE_GXM_TEXTURE_ADDR_REPEAT;
    sceGxmTextureSetUAddrMode(&texture->texture, mode);
    sceGxmTextureSetVAddrMode(&texture->texture, mode);
}

void GxmTexture_Free(GxmTexture *texture)
{
    if (!texture->memory.base)
        return;

    s_bytesResident -= texture->memory.size;
    GxmMem_Free(&texture->memory);
    memset(texture, 0, sizeof(*texture));
}

uint32_t GxmTexture_BytesResident(void)
{
    return s_bytesResident;
}
