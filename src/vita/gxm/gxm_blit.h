// Textured quads, for the surface copies D3D did with StretchRect.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_rendertarget.h"

// copies src into dst, then re-enters src and repaints it from dst, which the scene switch lost
bool GxmBlit_Resolve(GxmRenderTarget *src, GxmRenderTarget *dst);

// draws texture over a pixel rect of the open scene's target, in that target's pixel space
bool GxmBlit_Rect(const SceGxmTexture *texture, int x, int y, int width, int height,
                  uint32_t targetWidth, uint32_t targetHeight);

void GxmBlit_Shutdown(void);

// blits dropped because a program, buffer or scene was missing; non-zero means copies were lost
uint32_t GxmBlit_DroppedBlits(void);
