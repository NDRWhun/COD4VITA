// D3D's scissor rect, as a GXM region clip.
#pragma once

// a viewport change resets clip to viewport: re-issue, passing the viewport rect to unscissor
void GxmScissor_Set(int x, int y, int width, int height);
