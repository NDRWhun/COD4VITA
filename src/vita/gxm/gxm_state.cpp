#include "gxm_state.h"

#include <string.h>

// the state words carry D3D9 enum values directly; the engine's translation tables
// are identity maps, so the nibbles are D3DBLEND / D3DBLENDOP / D3DSTENCILOP
#define GFXS0_SRCBLEND_RGB_SHIFT    0
#define GFXS0_DSTBLEND_RGB_SHIFT    4
#define GFXS0_BLENDOP_RGB_SHIFT     8
#define GFXS0_BLENDOP_RGB_MASK      0x700
#define GFXS0_BLEND_RGB_MASK        0x7FF
#define GFXS0_BLENDOP_ALPHA_MASK    0x7000000
#define GFXS0_ATEST_MASK            0x3000
#define GFXS0_ATEST_GT_0            0x1000
#define GFXS0_ATEST_LT_128          0x2000
#define GFXS0_ATEST_GE_128          0x3000
#define GFXS0_CULL_SHIFT            14
#define GFXS0_CULL_MASK             0xC000
#define GFXS0_SRCBLEND_ALPHA_SHIFT  16
#define GFXS0_DSTBLEND_ALPHA_SHIFT  20
#define GFXS0_BLENDOP_ALPHA_SHIFT   24
#define GFXS0_COLORWRITE_RGB        0x8000000
#define GFXS0_COLORWRITE_ALPHA      0x10000000

#define GFXS1_DEPTHWRITE            0x1
#define GFXS1_DEPTHTEST_DISABLE     0x2
#define GFXS1_DEPTHTEST_SHIFT       2
#define GFXS1_DEPTHTEST_MASK        0xC
#define GFXS1_POLYGON_OFFSET_SHIFT  4
#define GFXS1_POLYGON_OFFSET_MASK   0x30
#define GFXS1_STENCIL_FRONT_ENABLE  0x40
#define GFXS1_STENCIL_BACK_ENABLE   0x80

static SceGxmBlendFactor GxmState_BlendFactor(uint32_t d3dBlend)
{
    switch (d3dBlend)
    {
    case 1:  return SCE_GXM_BLEND_FACTOR_ZERO;
    case 2:  return SCE_GXM_BLEND_FACTOR_ONE;
    case 3:  return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case 4:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:  return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case 6:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:  return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case 8:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:  return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case 10: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return SCE_GXM_BLEND_FACTOR_ONE;
    }
}

static SceGxmBlendFunc GxmState_BlendFunc(uint32_t d3dBlendOp)
{
    switch (d3dBlendOp)
    {
    case 1:  return SCE_GXM_BLEND_FUNC_ADD;
    case 2:  return SCE_GXM_BLEND_FUNC_SUBTRACT;
    case 3:  return SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    case 4:  return SCE_GXM_BLEND_FUNC_MIN;
    case 5:  return SCE_GXM_BLEND_FUNC_MAX;
    default: return SCE_GXM_BLEND_FUNC_NONE;
    }
}

static SceGxmStencilOp GxmState_StencilOp(uint32_t d3dOp)
{
    switch (d3dOp)
    {
    case 1:  return SCE_GXM_STENCIL_OP_KEEP;
    case 2:  return SCE_GXM_STENCIL_OP_ZERO;
    case 3:  return SCE_GXM_STENCIL_OP_REPLACE;
    case 4:  return SCE_GXM_STENCIL_OP_INCR;       // D3D INCRSAT saturates, as GXM INCR does
    case 5:  return SCE_GXM_STENCIL_OP_DECR;
    case 6:  return SCE_GXM_STENCIL_OP_INVERT;
    case 7:  return SCE_GXM_STENCIL_OP_INCR_WRAP;
    case 8:  return SCE_GXM_STENCIL_OP_DECR_WRAP;
    default: return SCE_GXM_STENCIL_OP_KEEP;
    }
}

static SceGxmStencilFunc GxmState_StencilFunc(uint32_t d3dFunc)
{
    switch (d3dFunc)
    {
    case 1:  return SCE_GXM_STENCIL_FUNC_NEVER;
    case 2:  return SCE_GXM_STENCIL_FUNC_LESS;
    case 3:  return SCE_GXM_STENCIL_FUNC_EQUAL;
    case 4:  return SCE_GXM_STENCIL_FUNC_LESS_EQUAL;
    case 5:  return SCE_GXM_STENCIL_FUNC_GREATER;
    case 6:  return SCE_GXM_STENCIL_FUNC_NOT_EQUAL;
    case 7:  return SCE_GXM_STENCIL_FUNC_GREATER_EQUAL;
    default: return SCE_GXM_STENCIL_FUNC_ALWAYS;
    }
}

static SceGxmDepthFunc GxmState_DepthFunc(uint32_t stateBits1)
{
    if (stateBits1 & GFXS1_DEPTHTEST_DISABLE)
        return SCE_GXM_DEPTH_FUNC_ALWAYS;

    switch ((stateBits1 & GFXS1_DEPTHTEST_MASK) >> GFXS1_DEPTHTEST_SHIFT)
    {
    case 1:  return SCE_GXM_DEPTH_FUNC_LESS;
    case 2:  return SCE_GXM_DEPTH_FUNC_EQUAL;
    case 3:  return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    default: return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
}

// the four offset levels the engine exposes; units are depth buffer steps
static const int s_depthBiasUnits[4] = { 0, 1, 2, 16 };

static void GxmState_DecodeStencil(uint32_t stateBits1, GxmRenderState *render)
{
    render->stencilEnabled = (stateBits1 & GFXS1_STENCIL_FRONT_ENABLE) != 0;
    if (!render->stencilEnabled)
    {
        render->frontFunc = render->backFunc = SCE_GXM_STENCIL_FUNC_ALWAYS;
        render->frontFail = render->frontDepthFail = render->frontPass = SCE_GXM_STENCIL_OP_KEEP;
        render->backFail = render->backDepthFail = render->backPass = SCE_GXM_STENCIL_OP_KEEP;
        return;
    }

    // with no separate back state the front bits drive both faces
    if (!(stateBits1 & GFXS1_STENCIL_BACK_ENABLE))
        stateBits1 = (stateBits1 & 0xFFFFF) | ((stateBits1 & 0xFFF00) << 12);

    render->frontPass = GxmState_StencilOp((stateBits1 >> 8) & 7);
    render->frontFail = GxmState_StencilOp((stateBits1 >> 11) & 7);
    render->frontDepthFail = GxmState_StencilOp((stateBits1 >> 14) & 7);
    render->frontFunc = GxmState_StencilFunc((stateBits1 >> 17) & 7);

    render->backPass = GxmState_StencilOp((stateBits1 >> 20) & 7);
    render->backFail = GxmState_StencilOp((stateBits1 >> 23) & 7);
    render->backDepthFail = GxmState_StencilOp((stateBits1 >> 26) & 7);
    render->backFunc = GxmState_StencilFunc((stateBits1 >> 29) & 7);
}

void GxmState_Decode(uint32_t stateBits0, uint32_t stateBits1,
                     GxmRenderState *render, GxmProgramState *program)
{
    memset(render, 0, sizeof(*render));
    memset(program, 0, sizeof(*program));

    switch (stateBits0 & GFXS0_CULL_MASK)
    {
    case 0x8000:  render->cull = SCE_GXM_CULL_CCW; break;    // cull back
    case 0xC000:  render->cull = SCE_GXM_CULL_CW; break;     // cull front
    default:      render->cull = SCE_GXM_CULL_NONE; break;
    }

    render->depthFunc = GxmState_DepthFunc(stateBits1);
    render->depthWrite = (stateBits1 & GFXS1_DEPTHWRITE)
        ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;

    const uint32_t offset = (stateBits1 & GFXS1_POLYGON_OFFSET_MASK) >> GFXS1_POLYGON_OFFSET_SHIFT;
    render->depthBiasFactor = offset ? 1 : 0;
    render->depthBiasUnits = s_depthBiasUnits[offset];

    GxmState_DecodeStencil(stateBits1, render);

    // blending is on only when a blend op is set, matching R_ForceSetBlendState
    program->blendEnabled = (stateBits0 & GFXS0_BLENDOP_RGB_MASK) != 0;

    // a material that declares no alpha blend op gets the rgb blend replicated into the
    // alpha field; R_ChangeState does this before touching the device, so most materials
    // reach the hardware with alpha blending set even though the bits do not say so
    if (program->blendEnabled && (stateBits0 & GFXS0_BLENDOP_ALPHA_MASK) == 0)
        stateBits0 = (stateBits0 & 0xF800FFFF) | ((stateBits0 & GFXS0_BLEND_RGB_MASK) << 16);

    uint8_t mask = 0;
    if (stateBits0 & GFXS0_COLORWRITE_RGB)
        mask |= SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B;
    if (stateBits0 & GFXS0_COLORWRITE_ALPHA)
        mask |= SCE_GXM_COLOR_MASK_A;

    program->blend.colorMask = mask;
    program->blend.colorFunc = GxmState_BlendFunc((stateBits0 >> GFXS0_BLENDOP_RGB_SHIFT) & 7);
    program->blend.alphaFunc = GxmState_BlendFunc((stateBits0 >> GFXS0_BLENDOP_ALPHA_SHIFT) & 7);
    program->blend.colorSrc = GxmState_BlendFactor((stateBits0 >> GFXS0_SRCBLEND_RGB_SHIFT) & 0xF);
    program->blend.colorDst = GxmState_BlendFactor((stateBits0 >> GFXS0_DSTBLEND_RGB_SHIFT) & 0xF);
    program->blend.alphaSrc = GxmState_BlendFactor((stateBits0 >> GFXS0_SRCBLEND_ALPHA_SHIFT) & 0xF);
    program->blend.alphaDst = GxmState_BlendFactor((stateBits0 >> GFXS0_DSTBLEND_ALPHA_SHIFT) & 0xF);

    switch (stateBits0 & GFXS0_ATEST_MASK)
    {
    case GFXS0_ATEST_GT_0:   program->alphaTest = GXM_ATEST_GT_0; break;
    case GFXS0_ATEST_LT_128: program->alphaTest = GXM_ATEST_LT_128; break;
    case GFXS0_ATEST_GE_128: program->alphaTest = GXM_ATEST_GE_128; break;
    default:                 program->alphaTest = GXM_ATEST_NONE; break;
    }
}

void GxmState_Apply(SceGxmContext *context, const GxmRenderState *state)
{
    sceGxmSetCullMode(context, state->cull);

    sceGxmSetFrontDepthFunc(context, state->depthFunc);
    sceGxmSetBackDepthFunc(context, state->depthFunc);
    sceGxmSetFrontDepthWriteEnable(context, state->depthWrite);
    sceGxmSetBackDepthWriteEnable(context, state->depthWrite);

    sceGxmSetFrontDepthBias(context, state->depthBiasFactor, state->depthBiasUnits);
    sceGxmSetBackDepthBias(context, state->depthBiasFactor, state->depthBiasUnits);

    sceGxmSetFrontStencilFunc(context, state->frontFunc, state->frontFail,
                              state->frontDepthFail, state->frontPass, 0xFF, 0xFF);
    sceGxmSetBackStencilFunc(context, state->backFunc, state->backFail,
                             state->backDepthFail, state->backPass, 0xFF, 0xFF);
}

uint32_t GxmState_ProgramKey(const GxmProgramState *program)
{
    if (!program->blendEnabled)
        return (uint32_t)program->alphaTest << 28 | (uint32_t)program->blend.colorMask << 24;

    return (uint32_t)program->alphaTest << 28
         | (uint32_t)program->blend.colorMask << 24
         | (uint32_t)program->blend.colorFunc << 20
         | (uint32_t)program->blend.alphaFunc << 16
         | (uint32_t)program->blend.colorSrc << 12
         | (uint32_t)program->blend.colorDst << 8
         | (uint32_t)program->blend.alphaSrc << 4
         | (uint32_t)program->blend.alphaDst
         | 0x800000u;                       // marks blending as enabled
}
