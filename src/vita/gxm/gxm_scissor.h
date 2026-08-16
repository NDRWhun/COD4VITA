// D3D's scissor rect, as a GXM region clip.
#pragma once

// a viewport change re-establishes clip == viewport, so re-issue after one
// pass the viewport rect back in to return to unscissored drawing
void GxmScissor_Set(int x, int y, int width, int height);
