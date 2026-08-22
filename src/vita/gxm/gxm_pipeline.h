// The D3D-shaped state the engine sets, resolved onto GXM at draw time.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_vertex.h"

#define GXM_PIPELINE_TEXTURE_UNITS 16

bool GxmPipeline_Init(void);
void GxmPipeline_Shutdown(void);

// pushes the state the pipeline owns to the context and drops every cached binding
void GxmPipeline_Reset(void);

// GXM resets viewport and clip state per scene, so the caches must follow
void GxmPipeline_SceneChanged(void);

// R_ChangeState's two words; blend, colour mask and alpha test pick a fragment program
void GxmPipeline_SetStateBits(uint32_t stateBits0, uint32_t stateBits1);

void GxmPipeline_SetViewport(int x, int y, int width, int height,
                             float nearValue, float farValue);

// GXM has one region clip, so the scissor is intersected with the viewport rather than set
void GxmPipeline_SetScissor(bool enabled, int x, int y, int width, int height);

// layout comes from GxmVertex_BuildLayout for this shader and vertex declaration
void GxmPipeline_SetVertexShader(int shaderHandle, uint32_t bytecodeHash, const GxmVertexLayout *layout);

// named by SM3 bytecode hash, because the alpha test mode picks the archive variant
void GxmPipeline_SetFragmentShader(uint32_t bytecodeHash);

void GxmPipeline_SetTexture(uint32_t unit, const SceGxmTexture *texture);

// the word R_DecodeSamplerState produces; filter and address only, GXM has no sampler object
void GxmPipeline_SetSamplerState(uint32_t unit, uint32_t decodedSamplerState);

void GxmPipeline_SetStream(uint32_t index, const void *base, uint32_t offsetInBytes,
                           uint32_t stride);
void GxmPipeline_SetIndexBuffer(const void *base);

// zone geometry lives in gpu buffers now, so freeing one must drop every pointer into it
void GxmPipeline_DropBuffers(void);

bool GxmPipeline_DrawIndexed(uint32_t firstIndex, uint32_t triangleCount);

// GXM has no clear entry point; the quad sits at the far plane, so other depths are refused
bool GxmPipeline_Clear(uint32_t whichToClear, const float color[4], float depth,
                       uint8_t stencil);

// draws dropped because a program would not resolve; non-zero means shaders are missing
uint32_t GxmPipeline_UnresolvedDraws(void);
void GxmPipeline_ResetCounters(void);
