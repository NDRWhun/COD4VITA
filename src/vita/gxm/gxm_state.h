// Translates the engine's two state words into GXM state.
//
// The split matters: cull, depth and stencil are context state, but blend, colour
// mask and alpha test are compiled into the fragment program, so they select a
// program instance instead of being set per draw.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

enum GxmAlphaTest
{
    GXM_ATEST_NONE,
    GXM_ATEST_GT_0,
    GXM_ATEST_LT_128,
    GXM_ATEST_GE_128
};

// applied to the context before a draw
struct GxmRenderState
{
    SceGxmCullMode cull;
    SceGxmDepthFunc depthFunc;
    SceGxmDepthWriteMode depthWrite;
    int depthBiasFactor;
    int depthBiasUnits;

    bool stencilEnabled;
    SceGxmStencilFunc frontFunc, backFunc;
    SceGxmStencilOp frontFail, frontDepthFail, frontPass;
    SceGxmStencilOp backFail, backDepthFail, backPass;
};

// baked into the fragment program at creation
struct GxmProgramState
{
    bool blendEnabled;
    SceGxmBlendInfo blend;
    GxmAlphaTest alphaTest;
};

void GxmState_Decode(uint32_t stateBits0, uint32_t stateBits1,
                     GxmRenderState *render, GxmProgramState *program);

void GxmState_Apply(SceGxmContext *context, const GxmRenderState *state);

// identifies the fragment program instance this state needs
uint32_t GxmState_ProgramKey(const GxmProgramState *program);
