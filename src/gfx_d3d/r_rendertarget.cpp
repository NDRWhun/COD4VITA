#include <universal/q_shared.h>
#include "r_rendertarget.h"
#include "r_init.h"
#include "r_image.h"
#include "r_dvars.h"
#include "rb_backend.h"
#include "rb_logfile.h"

#ifdef KISAK_VITA
#include <vita/gxm/gxm_device.h>
#include <vita/gxm/gxm_rendertarget.h>
#include <vita/gxm/gxm_texture.h>

// the GXM objects behind GfxRenderTargetSurface, indexed by GfxRenderTargetId
static GxmRenderTarget s_gxmColor[R_RENDERTARGET_COUNT];
static GxmDepthStencil s_gxmDepth[R_RENDERTARGET_COUNT];
static bool s_gxmColorOwned[R_RENDERTARGET_COUNT];
static bool s_gxmDepthOwned[R_RENDERTARGET_COUNT];

// the colour surface handed to the render target's GfxImage; the payload stays with s_gxmColor
static GxmTexture s_gxmImageView[R_RENDERTARGET_COUNT];

static GxmDepthStencil s_gxmSharedDepth;
static bool s_gxmSharedDepthOwned;
static GxmDepthStencil s_gxmCookieDepth;
static bool s_gxmCookieDepthOwned;

static bool R_GxmRenderTargetFormat(_D3DFORMAT format, SceGxmColorFormat *colorFormat,
                                    SceGxmTextureFormat *textureFormat)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        *colorFormat = SCE_GXM_COLOR_FORMAT_A8R8G8B8;
        *textureFormat = SCE_GXM_TEXTURE_FORMAT_A8R8G8B8;
        return true;
    case D3DFMT_A8B8G8R8:
        *colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
        *textureFormat = SCE_GXM_TEXTURE_FORMAT_A8B8G8R8;
        return true;
    case D3DFMT_R5G6B5:
        *colorFormat = SCE_GXM_COLOR_FORMAT_R5G6B5;
        *textureFormat = SCE_GXM_TEXTURE_FORMAT_R5G6B5;
        return true;
    case D3DFMT_R32F:
        *colorFormat = SCE_GXM_COLOR_FORMAT_F32_R;
        *textureFormat = SCE_GXM_TEXTURE_FORMAT_F32_R;
        return true;
    case D3DFMT_G16R16F:
        *colorFormat = SCE_GXM_COLOR_FORMAT_F16F16_GR;
        *textureFormat = SCE_GXM_TEXTURE_FORMAT_F16F16_GR;
        return true;
    default:
        return false;
    }
}
#endif

//GfxRenderTarget *gfxRenderTargets 85b5db38     gfx_d3d : r_rendertarget.obj

void __cdecl AssertUninitializedRenderTarget(const GfxRenderTarget *renderTarget)
{
    iassert(renderTarget);
    iassert(renderTarget->image == NULL);
    iassert(renderTarget->surface.color == NULL);
    iassert(renderTarget->surface.depthStencil == NULL);
    iassert(renderTarget->width == 0);
    iassert(renderTarget->height == 0);
}

#ifndef KISAK_VITA
bool __cdecl R_IsDepthStencilFormatOk(_D3DFORMAT renderTargetFormat, _D3DFORMAT depthStencilFormat)
{
    return dx.d3d9->CheckDeviceFormat(
        dx.adapterIndex,
        D3DDEVTYPE_HAL,
        D3DFMT_X8R8G8B8,
        2,
        D3DRTYPE_SURFACE,
        depthStencilFormat) >= 0
        && 
        dx.d3d9->CheckDepthStencilMatch(
            dx.adapterIndex,
            D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8,
            renderTargetFormat,
            depthStencilFormat) >= 0;
}

int __cdecl R_GetDepthStencilFormat(_D3DFORMAT renderTargetFormat)
{
    if (R_IsDepthStencilFormatOk(renderTargetFormat, D3DFMT_D24FS8))
        return 83;
    else
        return 75;
}
#else
// GXM depth-stencil surfaces are S8D24 and nothing else is offered
int __cdecl R_GetDepthStencilFormat(_D3DFORMAT renderTargetFormat)
{
    (void)renderTargetFormat;
    return D3DFMT_D24S8;
}
#endif

void __cdecl R_InitRenderTargets()
{
    R_InitRenderTargets_PC();
}

void R_InitRenderTargets_PC()
{
    _D3DFORMAT backBufferFormat; // [esp+0h] [ebp-4h]

    backBufferFormat = R_InitFrameBufferRenderTarget();
    if (!g_allocateMinimalResources)
    {
        if (r_floatz->current.enabled)
            R_InitFullscreenRenderTargetImage(
                4,
                FULLSCREEN_SCENE,
                0,
                D3DFMT_R32F,
                RENDERTARGET_USAGE_RENDER,
                &gfxRenderTargets[R_RENDERTARGET_FLOAT_Z]);
        R_ShareRenderTarget(R_RENDERTARGET_RESOLVED_SCENE, R_RENDERTARGET_DYNAMICSHADOWS);
        R_ShareRenderTarget(R_RENDERTARGET_RESOLVED_SCENE, R_RENDERTARGET_RESOLVED_POST_SUN);
        R_InitFullscreenRenderTargetImage(
            10,
            FULLSCREEN_DISPLAY,
            0,
            backBufferFormat,
            RENDERTARGET_USAGE_TEXTURE,
            gfxRenderTargets);
        R_InitShadowmapRenderTarget(2, 1024, 2u, &gfxRenderTargets[R_RENDERTARGET_SHADOWMAP_SUN]);
        R_InitShadowmapRenderTarget(3, 512, 4u, &gfxRenderTargets[R_RENDERTARGET_SHADOWMAP_SPOT]);
        R_InitShadowCookieRenderTarget(&gfxRenderTargets[R_RENDERTARGET_SHADOWCOOKIE]);
        R_InitShadowCookieBlurRenderTarget(&gfxRenderTargets[R_RENDERTARGET_SHADOWCOOKIE_BLUR]);
        R_InitFullscreenRenderTargetImage(
            5,
            FULLSCREEN_SCENE,
            2,
            backBufferFormat,
            RENDERTARGET_USAGE_RENDER,
            &gfxRenderTargets[R_RENDERTARGET_POST_EFFECT_0]);
        R_InitFullscreenRenderTargetImage(
            6,
            FULLSCREEN_SCENE,
            2,
            backBufferFormat,
            RENDERTARGET_USAGE_RENDER,
            &gfxRenderTargets[R_RENDERTARGET_POST_EFFECT_1]);
        R_InitFullscreenRenderTargetImage(
            7,
            FULLSCREEN_SCENE,
            2,
            backBufferFormat,
            RENDERTARGET_USAGE_RENDER,
            &gfxRenderTargets[R_RENDERTARGET_PINGPONG_0]);
        R_InitFullscreenRenderTargetImage(
            8,
            FULLSCREEN_SCENE,
            2,
            backBufferFormat,
            RENDERTARGET_USAGE_RENDER,
            &gfxRenderTargets[R_RENDERTARGET_PINGPONG_1]);
    }
}

void __cdecl R_ShareRenderTarget(GfxRenderTargetId idFrom, GfxRenderTargetId idTo)
{
    GfxRenderTarget *v2; // ecx
    GfxRenderTarget *v3; // edx

    AssertUninitializedRenderTarget(&gfxRenderTargets[idTo]);
    v2 = &gfxRenderTargets[idFrom];
    v3 = &gfxRenderTargets[idTo];
    v3->image = v2->image;
    v3->surface.color = v2->surface.color;
    v3->surface.depthStencil = v2->surface.depthStencil;
    v3->width = v2->width;
    v3->height = v2->height;
#ifndef KISAK_VITA
    if (gfxRenderTargets[idTo].surface.color)
        gfxRenderTargets[idTo].surface.color->AddRef();
    if (gfxRenderTargets[idTo].surface.depthStencil)
        gfxRenderTargets[idTo].surface.depthStencil->AddRef();
#endif
}

void __cdecl R_InitFullscreenRenderTargetImage(
    int imageProgType,
    FullscreenType screenType,
    int picmip,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget)
{
    uint16_t v6; // [esp+0h] [ebp-20h]
    uint16_t v7; // [esp+4h] [ebp-1Ch]
    int fullscreenWidth; // [esp+18h] [ebp-8h] BYREF
    int fullscreenHeight; // [esp+1Ch] [ebp-4h] BYREF

    R_GetFullScreenRes(screenType, &fullscreenWidth, &fullscreenHeight);
    if (fullscreenWidth >> picmip > 1)
        v7 = fullscreenWidth >> picmip;
    else
        v7 = 1;
    if (fullscreenHeight >> picmip > 1)
        v6 = fullscreenHeight >> picmip;
    else
        v6 = 1;
    R_InitRenderTargetImage(imageProgType, v7, v6, format, usage, renderTarget);
    if (usage == RENDERTARGET_USAGE_RENDER_SHARE_SCENE)
    {
        if (!alwaysfails)
            MyAssertHandler(
                ".\\r_rendertarget.cpp",
                751,
                0,
                "RENDERTARGET_USAGE_RENDER_SHARE_SCENE only implemented for XBOX");
    }
    else if (usage == RENDERTARGET_USAGE_RENDER)
    {
    Com_Printf(8, "  assigning shared depth-stencil\n");
        renderTarget->surface.depthStencil = R_AssignSingleSampleDepthStencilSurface();
    Com_Printf(8, "  depth-stencil assigned\n");
    }
    Com_Printf(8, "  tracking fullscreen texture\n");
    Image_TrackFullscreenTexture(renderTarget->image, fullscreenWidth, fullscreenHeight, picmip, format);
    Com_Printf(8, "  render target image done\n");
}

void __cdecl R_GetFullScreenRes(FullscreenType screenType, int *fullscreenWidth, int *fullscreenHeight)
{
    uint32_t sceneHeight; // [esp+0h] [ebp-8h]
    uint32_t sceneWidth; // [esp+4h] [ebp-4h]

    if ((uint32_t)screenType > FULLSCREEN_SCENE)
        MyAssertHandler(
            ".\\r_rendertarget.cpp",
            467,
            0,
            "%s\n\t(screenType) = %i",
            "(screenType == FULLSCREEN_DISPLAY || screenType == FULLSCREEN_MIXED || screenType == FULLSCREEN_SCENE)",
            screenType);
    if (screenType)
        sceneWidth = vidConfig.sceneWidth;
    else
        sceneWidth = vidConfig.displayWidth;
    *fullscreenWidth = sceneWidth;
    if (screenType == FULLSCREEN_SCENE)
        sceneHeight = vidConfig.sceneHeight;
    else
        sceneHeight = vidConfig.displayHeight;
    *fullscreenHeight = sceneHeight;
}

void __cdecl R_GetFrameBufferDepthStencilRes(int *depthStencilWidth, int *depthStencilHeight)
{
#ifdef KISAK_RADIANT
    // P5.3 multi-window: the editor shares ONE depth-stencil surface across all child
    // views (the per-window swap chains carry only colour — d3dpp.EnableAutoDepthStencil
    // is 0). D3D9 requires the depth surface to be >= the bound colour render target, so
    // the shared depth must cover the LARGEST view at its LARGEST size. vidConfig.display
    // is only the first (device-creating) window's initial client size, so a larger or
    // grown view would undersize it. Size the shared depth to the whole virtual desktop
    // → it covers any editor window at any resize without ever recreating the depth on
    // resize (R_Hwnd_Resize only recreates colour swap chains). The real editor sizes the
    // device for the camera view (the largest); virtual-desktop is a strictly-safe superset.
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    *depthStencilWidth  = vw > vidConfig.displayWidth  ? vw : vidConfig.displayWidth;
    *depthStencilHeight = vh > vidConfig.displayHeight ? vh : vidConfig.displayHeight;
#else
    *depthStencilWidth = vidConfig.displayWidth;
    *depthStencilHeight = vidConfig.displayHeight;
#endif
}

#ifdef KISAK_VITA
IDirect3DSurface9 *__cdecl R_AssignSingleSampleDepthStencilSurface()
{
    int depthStencilWidth; // [esp+4h] [ebp-8h] BYREF
    int depthStencilHeight; // [esp+8h] [ebp-4h] BYREF

    if (!dx.singleSampleDepthStencilSurface && dx.multiSampleType == D3DMULTISAMPLE_NONE)
        dx.singleSampleDepthStencilSurface = gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.depthStencil;
    if (dx.singleSampleDepthStencilSurface)
        return dx.singleSampleDepthStencilSurface;

    R_GetFrameBufferDepthStencilRes(&depthStencilWidth, &depthStencilHeight);
    if (!GxmDepthStencil_Create(&s_gxmSharedDepth, depthStencilWidth, depthStencilHeight))
        Com_Error(
            ERR_FATAL,
            "Couldn't create a %i x %i depth-stencil surface\n",
            depthStencilWidth,
            depthStencilHeight);
    s_gxmSharedDepthOwned = true;
    dx.singleSampleDepthStencilSurface = (IDirect3DSurface9 *)&s_gxmSharedDepth;
    return dx.singleSampleDepthStencilSurface;
}
#else
IDirect3DSurface9 *__cdecl R_AssignSingleSampleDepthStencilSurface()
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-Ch]
    int depthStencilWidth; // [esp+4h] [ebp-8h] BYREF
    int depthStencilHeight; // [esp+8h] [ebp-4h] BYREF

    if (!dx.singleSampleDepthStencilSurface && dx.multiSampleType == D3DMULTISAMPLE_NONE)
        dx.singleSampleDepthStencilSurface = gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.depthStencil;
    if (dx.singleSampleDepthStencilSurface)
    {
        dx.singleSampleDepthStencilSurface->AddRef();
        return dx.singleSampleDepthStencilSurface;
    }
    else
    {
        R_GetFrameBufferDepthStencilRes(&depthStencilWidth, &depthStencilHeight);
        hr = dx.device->CreateDepthStencilSurface(
            depthStencilWidth,
            depthStencilHeight,
            dx.depthStencilFormat,
            D3DMULTISAMPLE_NONE,
            0,
            0,
            &dx.singleSampleDepthStencilSurface,
            0);
        if (hr < 0)
        {
            v1 = R_ErrorDescription(hr);
            Com_Error(
                ERR_FATAL,
                "Couldn't create a %i x %i depth-stencil surface: %s\n",
                depthStencilWidth,
                depthStencilHeight,
                v1);
        }
        iassert( dx.singleSampleDepthStencilSurface );
        return dx.singleSampleDepthStencilSurface;
    }
}
#endif

void __cdecl R_AssignImageToRenderTargetDepthStencil(GfxRenderTargetSurface *surface, GfxImage *image)
{
#ifdef KISAK_VITA
    // GXM cannot expose a depth-stencil surface as a texture, so the depth shadowmap path is out
    (void)surface;
    (void)image;
    Com_Error(ERR_FATAL, "Depth-texture render targets are unsupported on GXM\n");
#else
    surface->depthStencil = Image_GetSurface(image);
#endif
}

void __cdecl R_InitRenderTargetImage(
    int imageProgType,
    uint16_t width,
    uint16_t height,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget)
{
    uint32_t renderTargetId; // [esp+0h] [ebp-4h]

    AssertUninitializedRenderTarget(renderTarget);
    iassert( (width > 0) );
    iassert( (height > 0) );
    renderTargetId = renderTarget - gfxRenderTargets;
    if (renderTargetId >= 0xF)
        MyAssertHandler(
            ".\\r_rendertarget.cpp",
            683,
            0,
            "renderTargetId doesn't index R_RENDERTARGET_COUNT\n\t%i not in [0, %i)",
            renderTargetId,
            15);
    Com_Printf(8, "  RT[%i] %ix%i fmt=0x%08x usage=%i: alloc image\n", renderTargetId, width, height, format, usage);
    renderTarget->image = Image_AllocProg(imageProgType, 6u, 0);
    iassert( renderTarget->image );
#ifdef KISAK_VITA
    if (!usage)
        R_AssignImageToRenderTargetDepthStencil(&renderTarget->surface, renderTarget->image);

    Com_Printf(8, "  RT[%i] image=%p, mapping format\n", renderTargetId, renderTarget->image);
    SceGxmColorFormat colorFormat;
    SceGxmTextureFormat textureFormat;
    if (!R_GxmRenderTargetFormat(format, &colorFormat, &textureFormat))
        Com_Error(ERR_FATAL, "No GXM render target format for D3D format 0x%08x\n", format);
    Com_Printf(8, "  RT[%i] creating gxm target\n", renderTargetId);
    if (!GxmRenderTarget_Create(&s_gxmColor[renderTargetId], width, height, colorFormat, textureFormat))
        Com_Error(ERR_FATAL, "Couldn't create a %i x %i render target\n", width, height);
    s_gxmColorOwned[renderTargetId] = true;
    renderTarget->surface.color = (IDirect3DSurface9 *)&s_gxmColor[renderTargetId];

    Com_Printf(8, "  RT[%i] gxm target ok, building image view\n", renderTargetId);
    // the image samples the render target's colour memory, so the view owns nothing
    s_gxmImageView[renderTargetId].texture = *GxmRenderTarget_Texture(&s_gxmColor[renderTargetId]);
    s_gxmImageView[renderTargetId].width = width;
    s_gxmImageView[renderTargetId].height = height;
    s_gxmImageView[renderTargetId].depth = 1;
    s_gxmImageView[renderTargetId].mipCount = 1;
    renderTarget->image->mapType = MAPTYPE_2D;
    renderTarget->image->width = width;
    renderTarget->image->height = height;
    renderTarget->image->depth = 1;
    renderTarget->image->texture.basemap = (IDirect3DBaseTexture9 *)&s_gxmImageView[renderTargetId];
#else
    Image_SetupRenderTarget(renderTarget->image, width, height, format);
    if (usage)
        R_AssignImageToRenderTargetColor(&renderTarget->surface, renderTarget->image);
    else
        R_AssignImageToRenderTargetDepthStencil(&renderTarget->surface, renderTarget->image);
#endif
    renderTarget->width = width;
    renderTarget->height = height;
}

void __cdecl R_AssignImageToRenderTargetColor(GfxRenderTargetSurface *surface, GfxImage *image)
{
#ifdef KISAK_VITA
    // the colour surface and the image's texture are created together in R_InitRenderTargetImage
    (void)surface;
    (void)image;
    Com_Error(ERR_FATAL, "R_AssignImageToRenderTargetColor has no GXM equivalent\n");
#else
    surface->color = Image_GetSurface(image);
#endif
}

void __cdecl R_InitShadowmapRenderTarget(
    int imageProgType,
    uint16_t tileRes,
    uint16_t tileRowCount,
    GfxRenderTarget *renderTarget)
{
    const char *v4; // eax
    const char *v5; // eax
    uint16_t totalHeight; // [esp+0h] [ebp-10h]
    int hr; // [esp+4h] [ebp-Ch]
    int hra; // [esp+4h] [ebp-Ch]
    RenderTargetUsage usage; // [esp+8h] [ebp-8h]

    AssertUninitializedRenderTarget(renderTarget);
    totalHeight = tileRowCount * tileRes;
    if (((tileRes - 1) & tileRes) != 0)
        MyAssertHandler(
            ".\\r_rendertarget.cpp",
            802,
            0,
            "%s\n\t(totalWidth) = %i",
            "((((totalWidth) & ((totalWidth) - 1)) == 0))",
            tileRes);
    if (((totalHeight - 1) & totalHeight) != 0)
        MyAssertHandler(
            ".\\r_rendertarget.cpp",
            803,
            0,
            "%s\n\t(totalHeight) = %i",
            "((((totalHeight) & ((totalHeight) - 1)) == 0))",
            totalHeight);
    usage = (RenderTargetUsage)(gfxMetrics.shadowmapBuildTechType != TECHNIQUE_BUILD_SHADOWMAP_DEPTH);
    R_InitAndTrackRenderTargetImage(
        imageProgType,
        tileRes,
        totalHeight,
        gfxMetrics.shadowmapFormatPrimary,
        usage,
        renderTarget);
#ifdef KISAK_VITA
    (void)v4;
    (void)v5;
    (void)hr;
    (void)hra;
    if (!usage)
        Com_Error(ERR_FATAL, "Depth-texture shadowmaps are unsupported on GXM\n");

    const uint32_t shadowmapId = renderTarget - gfxRenderTargets;
    if (!GxmDepthStencil_Create(&s_gxmDepth[shadowmapId], tileRes, totalHeight))
        Com_Error(ERR_FATAL, "Couldn't create a %i x %i depth-stencil surface\n", tileRes, totalHeight);
    s_gxmDepthOwned[shadowmapId] = true;
    renderTarget->surface.depthStencil = (IDirect3DSurface9 *)&s_gxmDepth[shadowmapId];
#else
    if (usage)
    {
        hra = dx.device->CreateDepthStencilSurface(
            tileRes,
            totalHeight,
            gfxMetrics.shadowmapFormatSecondary,
            D3DMULTISAMPLE_NONE,
            0,
            0,
            &renderTarget->surface.depthStencil,
            0);
        if (hra < 0)
        {
            v5 = R_ErrorDescription(hra);
            Com_Error(ERR_FATAL, "Couldn't create a %i x %i depth-stencil surface: %s\n", tileRes, totalHeight, v5);
        }
    }
    else
    {
        hr = dx.device->CreateRenderTarget(
            tileRes,
            totalHeight,
            gfxMetrics.shadowmapFormatSecondary,
            D3DMULTISAMPLE_NONE,
            0,
            0,
            (IDirect3DSurface9 **)&renderTarget->surface,
            0);
        if (hr < 0)
        {
            v4 = R_ErrorDescription(hr);
            Com_Error(ERR_FATAL, "Couldn't create a %i x %i render target surface: %s\n", tileRes, totalHeight, v4);
        }
    }
#endif
}

void __cdecl R_InitAndTrackRenderTargetImage(
    int imageProgType,
    uint16_t width,
    uint16_t height,
    _D3DFORMAT format,
    RenderTargetUsage usage,
    GfxRenderTarget *renderTarget)
{
    R_InitRenderTargetImage(imageProgType, width, height, format, usage, renderTarget);
    Image_TrackTexture(renderTarget->image, 3, format, width, height, 1);
}

void __cdecl R_InitShadowCookieBlurRenderTarget(GfxRenderTarget *renderTarget)
{
    AssertUninitializedRenderTarget(renderTarget);
    R_InitAndTrackRenderTargetImage(1, 0x80u, 0x80u, D3DFMT_A8R8G8B8, RENDERTARGET_USAGE_RENDER, renderTarget);
    R_AssignShadowCookieDepthStencilSurface(&renderTarget->surface);
}

void __cdecl R_InitShadowCookieRenderTarget(GfxRenderTarget *renderTarget)
{
    AssertUninitializedRenderTarget(renderTarget);
    R_InitAndTrackRenderTargetImage(0, 0x80u, 0x80u, D3DFMT_A8R8G8B8, RENDERTARGET_USAGE_RENDER, renderTarget);
    iassert( renderTarget->surface.color );
    R_AssignShadowCookieDepthStencilSurface(&renderTarget->surface);
}

void __cdecl R_AssignShadowCookieDepthStencilSurface(GfxRenderTargetSurface *surface)
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-8h]
    _D3DFORMAT depthStencilFormat; // [esp+4h] [ebp-4h]

    if (gfxRenderTargets[R_RENDERTARGET_SHADOWCOOKIE].surface.depthStencil)
    {
        surface->depthStencil = gfxRenderTargets[R_RENDERTARGET_SHADOWCOOKIE].surface.depthStencil;
#ifndef KISAK_VITA
        surface->depthStencil->AddRef();
#endif
    }
    else
    {
#ifdef KISAK_VITA
        (void)v1;
        (void)hr;
        (void)depthStencilFormat;
        if (!GxmDepthStencil_Create(&s_gxmCookieDepth, 128, 128))
            Com_Error(ERR_FATAL, "Couldn't create a %i x %i depth-stencil surface\n", 128, 128);
        s_gxmCookieDepthOwned = true;
        surface->depthStencil = (IDirect3DSurface9 *)&s_gxmCookieDepth;
#else
        depthStencilFormat = (_D3DFORMAT)R_GetDepthStencilFormat(D3DFMT_A8R8G8B8);
        hr = dx.device->CreateDepthStencilSurface(
            128u,
            128u,
            depthStencilFormat,
            D3DMULTISAMPLE_NONE,
            0,
            0,
            &surface->depthStencil,
            0);
        if (hr < 0)
        {
            v1 = R_ErrorDescription(hr);
            Com_Error(ERR_FATAL, "Couldn't create a %i x %i depth-stencil surface: %s\n", 128, 128, v1);
        }
#endif
    }
}

const char *__cdecl R_DescribeFormat(_D3DFORMAT format)
{
    const char *result; // eax

    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A8B8G8R8:
        result = "24-bit color with 8-bit alpha";
        break;
    case D3DFMT_R5G6B5:
        result = "16-bit color";
        break;
    case D3DFMT_A1R5G5B5:
        result = "15-bit color with 1-bit alpha";
        break;
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D16:
        result = "16-bit depth without stencil";
        break;
    case D3DFMT_D15S1:
        result = "15-bit depth with 1-bit stencil";
        break;
    case D3DFMT_D24S8:
        result = "24-bit depth with 8-bit stencil";
        break;
    case D3DFMT_D24X8:
        result = "24-bit depth without stencil";
        break;
    default:
        result = va("unknown format 0x%08x", format);
        break;
    }
    return result;
}

#ifdef KISAK_VITA
void __cdecl R_InitFrameBufferRenderTarget_Win32(GfxRenderTarget *renderTarget)
{
    iassert( renderTarget );
    renderTarget->width = vidConfig.displayWidth;
    renderTarget->height = vidConfig.displayHeight;

    // the display target rebinds the rotating back buffer on every scene entry
    renderTarget->surface.color = (IDirect3DSurface9 *)GxmRenderTarget_Display();
    iassert( renderTarget->surface.color );
    if (g_allocateMinimalResources)
        renderTarget->surface.depthStencil = 0;
    else
        renderTarget->surface.depthStencil = (IDirect3DSurface9 *)GxmRenderTarget_DisplayDepth();
}
#else
void __cdecl R_InitFrameBufferRenderTarget_Win32(GfxRenderTarget *renderTarget)
{
    const char *v1; // eax
    const char *v2; // eax
    const char *v3; // eax
    int depthStencilWidth; // [esp+0h] [ebp-10h] BYREF
    int depthStencilHeight; // [esp+4h] [ebp-Ch] BYREF
    HRESULT v6; // [esp+8h] [ebp-8h]
    HRESULT hr; // [esp+Ch] [ebp-4h]

    iassert( renderTarget );
    renderTarget->width = vidConfig.displayWidth;
    renderTarget->height = vidConfig.displayHeight;
    hr = dx.device->GetSwapChain(0, &dx.windows[0].swapChain);
    if (hr < 0)
    {
        v1 = R_ErrorDescription(hr);
        Com_Error(ERR_FATAL, "Couldn't get an interface to the swap chain: %s\n", v1);
    }
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("dx.device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &renderTarget->surface.color )\n");
        v6 = dx.device->GetBackBuffer(
            0,
            0,
            D3DBACKBUFFER_TYPE_MONO,
            &renderTarget->surface.color);
        if (v6 < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(v6);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_rendertarget.cpp (%i) dx.device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &renderTarget->surface.col"
                    "or ) failed: %s\n",
                    895,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    iassert( renderTarget->surface.color );
    if (g_allocateMinimalResources)
    {
        renderTarget->surface.depthStencil = 0;
    }
    else
    {
        R_GetFrameBufferDepthStencilRes(&depthStencilWidth, &depthStencilHeight);
        hr = dx.device->CreateDepthStencilSurface(
            depthStencilWidth,
            depthStencilHeight,
            dx.depthStencilFormat,
            dx.multiSampleType,
            dx.multiSampleQuality,
            0,
            &renderTarget->surface.depthStencil,
            0);
        if (hr < 0)
        {
            v3 = R_ErrorDescription(hr);
            Com_Error(
                ERR_FATAL,
                "Couldn't create a %i x %i depth-stencil surface: %s\n",
                depthStencilWidth,
                depthStencilHeight,
                v3);
        }
    }
}
#endif

_D3DFORMAT __cdecl R_InitFrameBufferRenderTarget()
{
    const char *v0; // eax
    const char *v1; // eax
    _D3DSURFACE_DESC surfaceDesc; // [esp+0h] [ebp-20h] BYREF

    R_InitFrameBufferRenderTarget_Win32(&gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER]);
    R_ShareRenderTarget(R_RENDERTARGET_FRAME_BUFFER, R_RENDERTARGET_SCENE);
    v0 = R_DescribeFormat(D3DFMT_A8R8G8B8);
    Com_Printf(8, "Requested frame buffer to be %s\n", v0);
#ifdef KISAK_VITA
    // the display buffers are created SCE_GXM_COLOR_FORMAT_A8B8G8R8 in gxm_device.cpp
    const GxmRenderTarget *frameBuffer =
        (const GxmRenderTarget *)gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color;
    surfaceDesc.Format = D3DFMT_A8B8G8R8;
    surfaceDesc.Width = frameBuffer->width;
    surfaceDesc.Height = frameBuffer->height;
#else
    gfxRenderTargets[R_RENDERTARGET_FRAME_BUFFER].surface.color->GetDesc(&surfaceDesc);
#endif
    iassert( surfaceDesc.Format != D3DFMT_UNKNOWN );
    v1 = R_DescribeFormat(surfaceDesc.Format);
    Com_Printf(8, "DirectX returned a frame buffer that is %s\n", v1);
    if (!g_allocateMinimalResources)
        R_InitFullscreenRenderTargetImage(
            9,
            FULLSCREEN_SCENE,
            0,
            surfaceDesc.Format,
            RENDERTARGET_USAGE_RENDER,
            &gfxRenderTargets[R_RENDERTARGET_RESOLVED_SCENE]);
    return surfaceDesc.Format;
}

void __cdecl R_ShutdownRenderTargets()
{
    int renderTargetId; // [esp+0h] [ebp-4h]

    for (renderTargetId = 0; renderTargetId < 15; ++renderTargetId)
    {
#ifdef KISAK_VITA
        // only the id that created a surface frees it; shared ids just drop the pointer
        if (s_gxmColorOwned[renderTargetId])
            GxmRenderTarget_Free(&s_gxmColor[renderTargetId]);
        if (s_gxmDepthOwned[renderTargetId])
            GxmDepthStencil_Free(&s_gxmDepth[renderTargetId]);
        s_gxmColorOwned[renderTargetId] = false;
        s_gxmDepthOwned[renderTargetId] = false;
        memset(&s_gxmImageView[renderTargetId], 0, sizeof(s_gxmImageView[renderTargetId]));
#else
        if (gfxRenderTargets[renderTargetId].surface.color)
            gfxRenderTargets[renderTargetId].surface.color->Release();
        if (gfxRenderTargets[renderTargetId].surface.depthStencil)
            gfxRenderTargets[renderTargetId].surface.depthStencil->Release();
#endif
        if (gfxRenderTargets[renderTargetId].image)
            Image_Release(gfxRenderTargets[renderTargetId].image);
    }
#ifdef KISAK_VITA
    if (s_gxmSharedDepthOwned)
        GxmDepthStencil_Free(&s_gxmSharedDepth);
    if (s_gxmCookieDepthOwned)
        GxmDepthStencil_Free(&s_gxmCookieDepth);
    s_gxmSharedDepthOwned = false;
    s_gxmCookieDepthOwned = false;
#endif
    memset(gfxRenderTargets, 0, sizeof(gfxRenderTargets));
    dx.singleSampleDepthStencilSurface = 0;
}


const char *s_renderTargetNames[15] =
{
  "R_RENDERTARGET_SAVED_SCREEN",
  "R_RENDERTARGET_FRAME_BUFFER",
  "R_RENDERTARGET_SCENE",
  "R_RENDERTARGET_RESOLVED_POST_SUN",
  "R_RENDERTARGET_RESOLVED_SCENE",
  "R_RENDERTARGET_FLOAT_Z",
  "R_RENDERTARGET_DYNAMICSHADOWS",
  "R_RENDERTARGET_PINGPONG_0",
  "R_RENDERTARGET_PINGPONG_1",
  "R_RENDERTARGET_SHADOWCOOKIE",
  "R_RENDERTARGET_SHADOWCOOKIE_BLUR",
  "R_RENDERTARGET_POST_EFFECT_0",
  "R_RENDERTARGET_POST_EFFECT_1",
  "R_RENDERTARGET_SHADOWMAP_SUN",
  "R_RENDERTARGET_SHADOWMAP_SPOT"
}; // idb
const char *__cdecl R_RenderTargetName(GfxRenderTargetId renderTargetId)
{
    return s_renderTargetNames[renderTargetId];
}

