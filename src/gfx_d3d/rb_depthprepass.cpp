#include <universal/q_shared.h>
#include "rb_depthprepass.h"
#include "r_rendercmds.h"
#include "rb_backend.h"
#include "r_utils.h"
#include "r_state.h"
#include "r_meshdata.h"

#ifdef KISAK_VITA
#include <vita/gxm/gxm_pipeline.h>
#endif

void R_DepthPrepassCallback(const void *userData, GfxCmdBufContext context, GfxCmdBufContext prepassContext)
{
    int height; // [esp+10h] [ebp-54h]
    int width; // [esp+14h] [ebp-50h]
    int y; // [esp+18h] [ebp-4Ch]
#ifndef KISAK_VITA
    IDirect3DDevice9 *device; // [esp+20h] [ebp-44h]
    tagRECT v6; // [esp+24h] [ebp-40h] BYREF
#endif
    GfxDrawSurfListInfo info; // [esp+34h] [ebp-30h] BYREF
    MaterialTechniqueType baseTechType; // [esp+5Ch] [ebp-8h]
    const GfxViewInfo *viewInfo; // [esp+60h] [ebp-4h]

    viewInfo = (const GfxViewInfo * )userData;
    height = viewInfo->scissorViewport.height;
    width = viewInfo->scissorViewport.width;
    y = viewInfo->scissorViewport.y;
#ifdef KISAK_VITA
    GxmPipeline_SetScissor(true, viewInfo->scissorViewport.x, y, width, height);
#else
    device = context.state->prim.device;
    v6.left = viewInfo->scissorViewport.x;
    v6.top = y;
    v6.right = width + v6.left;
    v6.bottom = height + y;
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, 1u);
    device->SetScissorRect(&v6);
#endif
    if (viewInfo->needsFloatZ)
    {
        iassert( R_HaveFloatZ() );
        R_SetRenderTarget(context, R_RENDERTARGET_FLOAT_Z);
        baseTechType = TECHNIQUE_BUILD_FLOAT_Z;
        R_DrawQuadMesh(context, rgp.shadowClearMaterial, &viewInfo->fullSceneViewMesh->meshData);
        context.source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][0] = 0.0;
        context.source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][1] = 0.0;
        context.source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][2] = 0.0;
        context.source->input.consts[CONST_SRC_CODE_DEPTH_FROM_CLIP][3] = 1.0;
        R_DirtyCodeConstant(context.source, CONST_SRC_CODE_DEPTH_FROM_CLIP);
    }
    else
    {
        R_SetRenderTarget(context, R_RENDERTARGET_DYNAMICSHADOWS);
        baseTechType = TECHNIQUE_DEPTH_PREPASS;
    }
    qmemcpy(&info, &viewInfo->litInfo, sizeof(info));
    info.baseTechType = baseTechType;
    R_DrawSurfs(context, 0, &info);
    qmemcpy(&info, &viewInfo->decalInfo, sizeof(info));
    info.baseTechType = baseTechType;
    R_DrawSurfs(context, 0, &info);
#ifdef KISAK_VITA
    GxmPipeline_SetScissor(false, 0, 0, 0, 0);
#else
    context.state->prim.device->SetRenderState(D3DRS_SCISSORTESTENABLE, 0);
#endif
}

void R_DepthPrepass(
    GfxRenderTargetId renderTargetId,
    const GfxViewInfo *viewInfo,
    GfxCmdBuf *cmdBuf)
{
    GfxCmdBufSourceState state; // [sp+50h] [-F00h] BYREF

    R_InitCmdBufSourceState(&state, &viewInfo->input, 1);
    R_SetRenderTargetSize(&state, renderTargetId);
    R_SetViewportStruct(&state, &viewInfo->sceneViewport);
    R_DrawCall(R_DepthPrepassCallback, viewInfo, &state, viewInfo, 0, &viewInfo->viewParms, cmdBuf, 0);
}
