#include "gxm_scissor.h"

#include "gxm_device.h"
#include "gxm_rendertarget.h"

#include <psp2/gxm.h>
#include <stdint.h>

void GxmScissor_Set(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0 || !GxmRenderTarget_SceneOpen())
        return;

    if (x < 0)
    {
        width += x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        y = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    sceGxmSetRegionClip(GxmDevice_Context(), SCE_GXM_REGION_CLIP_OUTSIDE,
                        (uint32_t)x, (uint32_t)y,
                        (uint32_t)(x + width - 1), (uint32_t)(y + height - 1));
}
