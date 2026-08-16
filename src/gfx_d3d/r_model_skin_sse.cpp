#ifdef KISAK_VITA

#include <universal/q_shared.h>
#include "r_model_skin.h"

#include <xanim/xanim.h>
#include <universal/profile.h>

#include <arm_neon.h>

// NEON skinning; the SSE arm below documents the vertex, bone and blend layouts

// packed unit vectors decode as (packed - shift) / scale, encode as packed * scale + shift
static const float k_encodeShift[4] = { 127.0f, 127.0f, 127.0f, -192.0f };
static const float k_encodeScale[4] = { 127.0f, 127.0f, 127.0f, 255.0f };

// ARMv7 NEON has no float divide, so the decode multiplies by the reciprocals
static const float k_decodeScale[4] = { 1.0f / 127.0f, 1.0f / 127.0f, 1.0f / 127.0f, 1.0f / 255.0f };

static const uint32_t k_wLaneMask[4] = { 0, 0, 0, 0xFFFFFFFFu };

static inline float32x4_t SplatX(float32x4_t v) { return vdupq_lane_f32(vget_low_f32(v), 0); }
static inline float32x4_t SplatY(float32x4_t v) { return vdupq_lane_f32(vget_low_f32(v), 1); }
static inline float32x4_t SplatZ(float32x4_t v) { return vdupq_lane_f32(vget_high_f32(v), 0); }
static inline float32x4_t SplatW(float32x4_t v) { return vdupq_lane_f32(vget_high_f32(v), 1); }

// vertexBlend entries are pre-scaled byte offsets into the bone-matrix array
static inline const DObjSkelMat *BoneAt(const DObjSkelMat *boneMatrix, unsigned byteOffset)
{
    return (const DObjSkelMat *)((const char *)boneMatrix + byteOffset);
}

// [Black Ops] rebuild an affine position (x, y, z, +/-1) with a clean binormal sign
static inline float32x4_t LoadSkinPosition(float32x4_t xyzAndBinormalSign)
{
    const uint32x4_t sign = vandq_u32(vreinterpretq_u32_f32(xyzAndBinormalSign), vdupq_n_u32(0x80000000u));
    const float32x4_t signedOne =
        vreinterpretq_f32_u32(vorrq_u32(sign, vreinterpretq_u32_f32(vdupq_n_f32(1.0f))));
    return vbslq_f32(vld1q_u32(k_wLaneMask), signedOne, xyzAndBinormalSign);
}

// affine transform (rotation + translation)
static inline float32x4_t TransformPoint(const DObjSkelMat *m, float32x4_t p)
{
    float32x4_t r = vmlaq_f32(vld1q_f32(m->origin), vld1q_f32(m->axis[0]), SplatX(p));
    r = vmlaq_f32(r, vld1q_f32(m->axis[1]), SplatY(p));
    return vmlaq_f32(r, vld1q_f32(m->axis[2]), SplatZ(p));
}

// linear transform (rotation only)
static inline float32x4_t TransformDir(const DObjSkelMat *m, float32x4_t d)
{
    float32x4_t r = vmulq_f32(vld1q_f32(m->axis[0]), SplatX(d));
    r = vmlaq_f32(r, vld1q_f32(m->axis[1]), SplatY(d));
    return vmlaq_f32(r, vld1q_f32(m->axis[2]), SplatZ(d));
}

// repack transformed xyz, taking w from wSource
static inline float32x4_t PackXyzW(float32x4_t xyz, float32x4_t wSource)
{
    return vbslq_f32(vld1q_u32(k_wLaneMask), wSource, xyz);
}

// decode a byte-packed unit vector to a direction, scaled by its packed length
static inline float32x4_t DecodeUnitVec(uint32_t packed)
{
    const uint16x8_t bytes = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(packed)));
    float32x4_t d = vcvtq_f32_u32(vmovl_u16(vget_low_u16(bytes)));
    d = vmulq_f32(vsubq_f32(d, vld1q_f32(k_encodeShift)), vld1q_f32(k_decodeScale));
    return vmulq_f32(d, SplatW(d));
}

// transform a packed unit vector by a bone's rotation and re-encode it (as floats ready to pack)
static inline float32x4_t SkinUnitVec(const DObjSkelMat *m, uint32_t packed)
{
    const float32x4_t transformed = TransformDir(m, DecodeUnitVec(packed));
    return vmlaq_f32(vld1q_f32(k_encodeShift),
                     PackXyzW(transformed, vld1q_f32(m->origin)),
                     vld1q_f32(k_encodeScale));
}

// weight (uint16) -> [0, 1), broadcast to all 4 lanes
static inline float32x4_t DecodeWeight(uint16_t weight)
{
    return vmulq_n_f32(vcvtq_f32_u32(vdupq_n_u32(weight)), 1.0f / 65536.0f);
}

// pack an encoded normal & tangent (each a float4) into 8 output bytes
// vcvtq_s32_f32 truncates where the SSE convert rounds, so the half restores the rounding
static inline uint8x8_t PackNormalTangent(float32x4_t encNormal, float32x4_t encTangent)
{
    const float32x4_t half = vdupq_n_f32(0.5f);
    const int16x4_t n = vqmovn_s32(vcvtq_s32_f32(vaddq_f32(encNormal, half)));
    const int16x4_t t = vqmovn_s32(vcvtq_s32_f32(vaddq_f32(encTangent, half)));
    return vqmovun_s16(vcombine_s16(n, t));
}

// Full skin: blend the position across (1 + numWeights) bones; transform the
// normal & tangent by the primary bone. Returns the packed normal/tangent so the
// caller can also emit it to a separate normal stream.
template <int numWeights>
static inline uint8x8_t Neon_SkinVertex(const GfxPackedVertex *src, const uint16_t *blend,
                                        const DObjSkelMat *boneMatrix, GfxPackedVertex *dst)
{
    const uint32_t *words = (const uint32_t *)src;
    const float32x4_t position = LoadSkinPosition(vld1q_f32((const float *)src));
    const uint8x8_t colorTexCoord = vld1_u8((const uint8_t *)src + 16);

    const DObjSkelMat *bone0 = BoneAt(boneMatrix, blend[0]);
    const float32x4_t pos0 = PackXyzW(TransformPoint(bone0, position), position);
    float32x4_t outPos = pos0;
    for (int w = 1; w <= numWeights; ++w)
    {
        const DObjSkelMat *bone = BoneAt(boneMatrix, blend[2 * w - 1]);
        const float32x4_t bonePos = PackXyzW(TransformPoint(bone, position), position);
        outPos = vmlaq_f32(outPos, DecodeWeight(blend[2 * w]), vsubq_f32(bonePos, pos0));
    }

    const uint8x8_t normalTangent = PackNormalTangent(SkinUnitVec(bone0, words[6]),
                                                      SkinUnitVec(bone0, words[7]));
    vst1q_f32((float *)dst, outPos);
    vst1_u8((uint8_t *)dst + 16, colorTexCoord);
    vst1_u8((uint8_t *)dst + 24, normalTangent);
    return normalTangent;
}

// Simple skin: blend the position only; the normal/tangent are supplied by the
// caller (a separate precomputed normal stream) and passed straight through.
template <int numWeights>
static inline void Neon_SkinVertexSimple(const GfxPackedVertex *src, const uint16_t *blend,
                                         const DObjSkelMat *boneMatrix, GfxPackedVertex *dst,
                                         uint8x8_t normalTangent, GfxPackedVertexNormal *dstNormal)
{
    const float32x4_t position = LoadSkinPosition(vld1q_f32((const float *)src));
    const uint8x8_t colorTexCoord = vld1_u8((const uint8_t *)src + 16);

    const DObjSkelMat *bone0 = BoneAt(boneMatrix, blend[0]);
    const float32x4_t pos0 = PackXyzW(TransformPoint(bone0, position), position);
    float32x4_t outPos = pos0;
    for (int w = 1; w <= numWeights; ++w)
    {
        const DObjSkelMat *bone = BoneAt(boneMatrix, blend[2 * w - 1]);
        const float32x4_t bonePos = PackXyzW(TransformPoint(bone, position), position);
        outPos = vmlaq_f32(outPos, DecodeWeight(blend[2 * w]), vsubq_f32(bonePos, pos0));
    }

    vst1q_f32((float *)dst, outPos);
    vst1_u8((uint8_t *)dst + 16, colorTexCoord);
    vst1_u8((uint8_t *)dst + 24, normalTangent);
    vst1_u8((uint8_t *)dstNormal, normalTangent);
}

// Rigid skin: single bone, no blend. Matches retail (raw binormal sign).
static inline uint8x8_t Neon_SkinVertexRigid(const DObjSkelMat *bone, const GfxPackedVertex *src,
                                             GfxPackedVertex *dst)
{
    const uint32_t *words = (const uint32_t *)src;
    const float32x4_t srcPos = vld1q_f32((const float *)src);
    const uint8x8_t colorTexCoord = vld1_u8((const uint8_t *)src + 16);

    const float32x4_t outPos = PackXyzW(TransformPoint(bone, srcPos), srcPos);
    const uint8x8_t normalTangent = PackNormalTangent(SkinUnitVec(bone, words[6]),
                                                      SkinUnitVec(bone, words[7]));
    vst1q_f32((float *)dst, outPos);
    vst1_u8((uint8_t *)dst + 16, colorTexCoord);
    vst1_u8((uint8_t *)dst + 24, normalTangent);
    return normalTangent;
}

static inline void Neon_SkinVertexRigidSimple(const DObjSkelMat *bone, const GfxPackedVertex *src,
                                              GfxPackedVertex *dst, uint8x8_t normalTangent,
                                              GfxPackedVertexNormal *dstNormal)
{
    const float32x4_t srcPos = vld1q_f32((const float *)src);
    const uint8x8_t colorTexCoord = vld1_u8((const uint8_t *)src + 16);

    const float32x4_t outPos = PackXyzW(TransformPoint(bone, srcPos), srcPos);
    vst1q_f32((float *)dst, outPos);
    vst1_u8((uint8_t *)dst + 16, colorTexCoord);
    vst1_u8((uint8_t *)dst + 24, normalTangent);
    vst1_u8((uint8_t *)dstNormal, normalTangent);
}

// dstVertNormals == null: verts only; non-null: also emit the normal stream
template <int numWeights>
static void SkinWeightBlock(const GfxPackedVertex *srcVerts, const uint16_t *vertexBlend, int vertCount,
                            const DObjSkelMat *boneMatrix, GfxPackedVertex *dstVerts,
                            GfxPackedVertexNormal *dstVertNormals, int *pVertexIndex)
{
    iassert(dstVerts);
    iassert(srcVerts);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(srcVerts) & 15));

    int vertIndex = *pVertexIndex;
    for (int i = 0; i < vertCount; ++i)
    {
        const uint8x8_t normalTangent = Neon_SkinVertex<numWeights>(&srcVerts[vertIndex], vertexBlend,
                                                                    boneMatrix, &dstVerts[vertIndex]);
        if (dstVertNormals)
            vst1_u8((uint8_t *)&dstVertNormals[vertIndex], normalTangent);
        vertexBlend += 2 * numWeights + 1;
        ++vertIndex;
    }
    *pVertexIndex = vertIndex;
}

template <int numWeights>
static void SkinWeightBlockInOut(const GfxPackedVertex *srcVerts, const uint16_t *vertexBlend, int vertCount,
                                 const DObjSkelMat *boneMatrix, const GfxPackedVertexNormal *srcVertNormals,
                                 GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *dstVerts,
                                 int *pVertexIndex)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(srcVerts);
    iassert(srcVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(srcVerts) & 15));

    int vertIndex = *pVertexIndex;
    for (int i = 0; i < vertCount; ++i)
    {
        const uint8x8_t normalTangent = vld1_u8((const uint8_t *)&srcVertNormals[vertIndex]);
        Neon_SkinVertexSimple<numWeights>(&srcVerts[vertIndex], vertexBlend, boneMatrix,
                                          &dstVerts[vertIndex], normalTangent, &dstVertNormals[vertIndex]);
        vertexBlend += 2 * numWeights + 1;
        ++vertIndex;
    }
    *pVertexIndex = vertIndex;
}

// weighted dispatchers: run each weight-count block over its slice
static void R_SkinXSurfaceWeightSse_Impl(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                         const DObjSkelMat *boneMatrix, GfxPackedVertex *outVerts,
                                         GfxPackedVertexNormal *outNormals)
{
    PROF_SCOPED("SkinXSurfaceWeight");

    int vertIndex = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    if (vertexInfo->vertCount[0])
    {
        SkinWeightBlock<0>(inVerts, blend, vertexInfo->vertCount[0], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += vertexInfo->vertCount[0];
    }
    if (vertexInfo->vertCount[1])
    {
        SkinWeightBlock<1>(inVerts, blend, vertexInfo->vertCount[1], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += 3 * vertexInfo->vertCount[1];
    }
    if (vertexInfo->vertCount[2])
    {
        SkinWeightBlock<2>(inVerts, blend, vertexInfo->vertCount[2], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += 5 * vertexInfo->vertCount[2];
    }
    if (vertexInfo->vertCount[3])
        SkinWeightBlock<3>(inVerts, blend, vertexInfo->vertCount[3], boneMatrix, outVerts, outNormals, &vertIndex);
}

static void R_SkinXSurfaceWeightSse(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                    const DObjSkelMat *boneMatrix, GfxPackedVertex *outVerts)
{
    R_SkinXSurfaceWeightSse_Impl(inVerts, vertexInfo, boneMatrix, outVerts, nullptr);
}

static void R_SkinXSurfaceWeightSseOut(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                       const DObjSkelMat *boneMatrix, GfxPackedVertexNormal *outNormals,
                                       GfxPackedVertex *outVerts)
{
    R_SkinXSurfaceWeightSse_Impl(inVerts, vertexInfo, boneMatrix, outVerts, outNormals);
}

static void R_SkinXSurfaceWeightSseInOut(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                         const DObjSkelMat *boneMatrix, const GfxPackedVertexNormal *srcVertNormals,
                                         GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *outVerts)
{
    PROF_SCOPED("SkinXSurfaceWeight");

    int vertIndex = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    if (vertexInfo->vertCount[0])
    {
        SkinWeightBlockInOut<0>(inVerts, blend, vertexInfo->vertCount[0], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += vertexInfo->vertCount[0];
    }
    if (vertexInfo->vertCount[1])
    {
        SkinWeightBlockInOut<1>(inVerts, blend, vertexInfo->vertCount[1], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += 3 * vertexInfo->vertCount[1];
    }
    if (vertexInfo->vertCount[2])
    {
        SkinWeightBlockInOut<2>(inVerts, blend, vertexInfo->vertCount[2], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += 5 * vertexInfo->vertCount[2];
    }
    if (vertexInfo->vertCount[3])
        SkinWeightBlockInOut<3>(inVerts, blend, vertexInfo->vertCount[3], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
}

// rigid skinning: one bone per vertex list, no blend
static void R_SkinXSurfaceRigidSse(const XSurface *surf, int totalVertCount,
                                   const DObjSkelMat *boneMatrix, GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            __builtin_prefetch(&src[4]);
            Neon_SkinVertexRigid(bone, src, dst);
            ++src;
            ++dst;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
}

static void R_SkinXSurfaceRigidSseOut(const XSurface *surf, int totalVertCount, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            __builtin_prefetch(&src[4]);
            const uint8x8_t normalTangent = Neon_SkinVertexRigid(bone, src, dst);
            vst1_u8((uint8_t *)dstVertNormals, normalTangent);
            ++src;
            ++dst;
            ++dstVertNormals;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
}

static void R_SkinXSurfaceRigidSseInOut(const XSurface *surf, int totalVertCount, const DObjSkelMat *boneMatrix,
                                        const GfxPackedVertexNormal *srcVertNormals,
                                        GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            __builtin_prefetch(&src[4]);
            Neon_SkinVertexRigidSimple(bone, src, dst,
                                       vld1_u8((const uint8_t *)srcVertNormals), dstVertNormals);
            ++src;
            ++dst;
            ++srcVertNormals;
            ++dstVertNormals;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
}

void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *skinVertNormalIn, GfxPackedVertexNormal *skinVertNormalOut,
                                      GfxPackedVertex *skinVerticesOut)
{
    if (skinVertNormalOut)
    {
        if (skinVertNormalIn)
        {
            if (xsurf->deformed)
                R_SkinXSurfaceWeightSseInOut(xsurf->verts0, &xsurf->vertInfo, boneMatrix,
                                             skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
            else
                R_SkinXSurfaceRigidSseInOut(xsurf, xsurf->vertCount, boneMatrix,
                                            skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
        }
        else if (xsurf->deformed)
        {
            R_SkinXSurfaceWeightSseOut(xsurf->verts0, &xsurf->vertInfo, boneMatrix, skinVertNormalOut, skinVerticesOut);
        }
        else
        {
            R_SkinXSurfaceRigidSseOut(xsurf, xsurf->vertCount, boneMatrix, skinVertNormalOut, skinVerticesOut);
        }
    }
    else
    {
        iassert(!skinVertNormalIn);
        if (xsurf->deformed)
            R_SkinXSurfaceWeightSse(xsurf->verts0, &xsurf->vertInfo, boneMatrix, skinVerticesOut);
        else
            R_SkinXSurfaceRigidSse(xsurf, xsurf->vertCount, boneMatrix, skinVerticesOut);
    }
}

#else

// (Aislop)
// LWSS: had the AI fix this file (intrinsics...) and then backport additions from BLOPS.
// Before ~(580us) -- After ~(25us).
//
// KISAKTODO: if someone wants to they could take some time, sit down, and make this even better with modern AVX

// This file uses MMX (__m64), which aliases the x87 register stack. EMMS is issued
// once per skinning batch (see _mm_empty() in the R_SkinXSurface* entry points), not
// per inline helper. Disabled before the intrinsic headers so it also covers the
// inline MMX intrinsics defined there (e.g. _mm_cvtpu16_ps in <xmmintrin.h>).
#pragma warning(disable : 4799)

#include <universal/q_shared.h>
#include "r_model_skin.h"

#include <xanim/xanim.h>
#include <universal/profile.h>

#include <xmmintrin.h>
#include <mmintrin.h>

// ---------------------------------------------------------------------------
// SSE / MMX vertex skinning.
//
// A GfxPackedVertex is two 16-byte lanes:
//   [0] = xyz + binormalSign          (position, w = binormal sign)
//   [1] = color | texCoord | normal | tangent   (the last two are PackedUnitVec)
// A bone (DObjSkelMat) is four 16-byte rows: axis[0..2] (rotation) + origin.
//
// vertexBlend holds, per vertex: bone0, then (bone_i, weight_i) pairs, so the
// stride for N weights is 2*N + 1.
//
// Black Ops upgrade (weighted path only): the output position's w is rebuilt to
// a clean +/-1 binormal sign via LoadSkinPosition() instead of trusting the raw
// packed value. See mask/one constants below. Rigid path matches retail (raw w).
// ---------------------------------------------------------------------------

// blend weights are uint16 in [0, 65535]; 1/65536 normalizes them to [0, 1)
static const __m128 sse_weightScale = { { 1.0f / 65536.0f, 1.0f / 65536.0f, 1.0f / 65536.0f, 1.0f / 65536.0f } };

// packed unit vectors decode as (packed - shift) / scale, encode as packed * scale + shift
static const __m128 sse_encodeShift = { { 127.0f, 127.0f, 127.0f, -192.0f } };
static const __m128 sse_encodeScale = { { 127.0f, 127.0f, 127.0f, 255.0f } };

// Black Ops binormal-sign normalization: keep xyz fully, keep only the sign bit of w, then OR in 1.0
static const __declspec(align(16)) unsigned int k_binormalSignMask[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x80000000u };
static const __m128 sse_wOne = { { 0.0f, 0.0f, 0.0f, 1.0f } };

// ---------------------------------------------------------------------------
// intrinsic helpers
// ---------------------------------------------------------------------------

static __forceinline __m128 SplatX(__m128 v) { return _mm_shuffle_ps(v, v, 0x00); }
static __forceinline __m128 SplatY(__m128 v) { return _mm_shuffle_ps(v, v, 0x55); }
static __forceinline __m128 SplatZ(__m128 v) { return _mm_shuffle_ps(v, v, 0xAA); }
static __forceinline __m128 SplatW(__m128 v) { return _mm_shuffle_ps(v, v, 0xFF); }

// vertexBlend entries are pre-scaled byte offsets into the bone-matrix array
static __forceinline const DObjSkelMat *BoneAt(const DObjSkelMat *boneMatrix, unsigned byteOffset)
{
    return (const DObjSkelMat *)((const char *)boneMatrix + byteOffset);
}

// [Black Ops] rebuild an affine position (x, y, z, +/-1) with a clean binormal sign
static __forceinline __m128 LoadSkinPosition(__m128 xyzAndBinormalSign)
{
    __m128 signMask = _mm_load_ps((const float *)k_binormalSignMask);
    __m128 hi = _mm_unpackhi_ps(xyzAndBinormalSign, xyzAndBinormalSign);   // (z, z, sign, sign)
    __m128 wOne = _mm_or_ps(_mm_and_ps(signMask, hi), sse_wOne);           // w = +/-1.0
    return _mm_shuffle_ps(xyzAndBinormalSign, wOne, 0xC4);                 // (x, y, z, +/-1)
}

// affine transform (rotation + translation)
static __forceinline __m128 TransformPoint(const DObjSkelMat *m, __m128 p)
{
    return _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(_mm_load_ps(m->axis[0]), SplatX(p)),
                   _mm_mul_ps(_mm_load_ps(m->axis[1]), SplatY(p))),
        _mm_add_ps(_mm_mul_ps(_mm_load_ps(m->axis[2]), SplatZ(p)),
                   _mm_load_ps(m->origin)));
}

// linear transform (rotation only)
static __forceinline __m128 TransformDir(const DObjSkelMat *m, __m128 d)
{
    return _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(_mm_load_ps(m->axis[0]), SplatX(d)),
                   _mm_mul_ps(_mm_load_ps(m->axis[1]), SplatY(d))),
        _mm_mul_ps(_mm_load_ps(m->axis[2]), SplatZ(d)));
}

// repack transformed xyz, taking w from wSource
static __forceinline __m128 PackXyzW(__m128 xyz, __m128 wSource)
{
    return _mm_shuffle_ps(xyz, _mm_unpackhi_ps(xyz, wSource), 0xC4);
}

// decode a byte-packed unit vector to a direction, scaled by its packed length
static __forceinline __m128 DecodeUnitVec(uint32_t packed)
{
    __m64 bytes = _m_punpcklbw(_mm_cvtsi32_si64(packed), _mm_setzero_si64());
    __m128 d = _mm_div_ps(_mm_sub_ps(_mm_cvtpu16_ps(bytes), sse_encodeShift), sse_encodeScale);
    return _mm_mul_ps(d, SplatW(d));
}

// transform a packed unit vector by a bone's rotation and re-encode it (as floats ready to pack)
static __forceinline __m128 SkinUnitVec(const DObjSkelMat *m, uint32_t packed)
{
    __m128 transformed = TransformDir(m, DecodeUnitVec(packed));
    return _mm_add_ps(_mm_mul_ps(PackXyzW(transformed, _mm_load_ps(m->origin)), sse_encodeScale), sse_encodeShift);
}

// weight (uint16) -> [0, 1), broadcast to all 4 lanes
static __forceinline __m128 DecodeWeight(uint16_t weight)
{
    __m64 w = _mm_cvtsi32_si64(weight);
    w = _m_punpcklwd(w, w);
    return _mm_mul_ps(_mm_cvtpu16_ps(_m_punpcklwd(w, w)), sse_weightScale);
}

// pack an encoded normal & tangent (each a float4) into 8 output bytes
static __forceinline __m64 PackNormalTangent(__m128 encNormal, __m128 encTangent)
{
    return _m_packuswb(
        _m_packuswb(_mm_cvt_ps2pi(encNormal), _mm_cvt_ps2pi(_mm_movehl_ps(encNormal, encNormal))),
        _m_packuswb(_mm_cvt_ps2pi(encTangent), _mm_cvt_ps2pi(_mm_movehl_ps(encTangent, encTangent))));
}

// ---------------------------------------------------------------------------
// per-vertex skinners (templated on the number of extra blend weights)
// ---------------------------------------------------------------------------

// Full skin: blend the position across (1 + numWeights) bones; transform the
// normal & tangent by the primary bone. Returns the packed normal/tangent so the
// caller can also emit it to a separate normal stream. [Black Ops binormal fix]
template <int numWeights>
static __forceinline __m64 Sse_SkinVertex(const GfxPackedVertex *src, const uint16_t *blend,
                                   const DObjSkelMat *boneMatrix, __m64 *dst)
{
    const __m128 *v = (const __m128 *)src;
    __m128 position = LoadSkinPosition(v[0]);
    __m64 colorTexCoord = *(const __m64 *)&v[1].m128_u64[0];

    const DObjSkelMat *bone0 = BoneAt(boneMatrix, blend[0]);
    __m128 pos0 = PackXyzW(TransformPoint(bone0, position), position);
    __m128 outPos = pos0;
    for (int w = 1; w <= numWeights; ++w)
    {
        const DObjSkelMat *bone = BoneAt(boneMatrix, blend[2 * w - 1]);
        __m128 bonePos = PackXyzW(TransformPoint(bone, position), position);
        outPos = _mm_add_ps(outPos, _mm_mul_ps(DecodeWeight(blend[2 * w]), _mm_sub_ps(bonePos, pos0)));
    }

    __m64 normalTangent = PackNormalTangent(SkinUnitVec(bone0, v[1].m128_u32[2]),
                                            SkinUnitVec(bone0, v[1].m128_u32[3]));
    _mm_stream_ps((float *)dst, outPos);
    _mm_stream_pi(dst + 2, colorTexCoord);
    _mm_stream_pi(dst + 3, normalTangent);
    return normalTangent;
}

// Simple skin: blend the position only; the normal/tangent are supplied by the
// caller (a separate precomputed normal stream) and passed straight through.
template <int numWeights>
static __forceinline void Sse_SkinVertexSimple(const GfxPackedVertex *src, const uint16_t *blend,
                                        const DObjSkelMat *boneMatrix, __m64 *dst,
                                        __m64 normalTangent, GfxPackedVertexNormal *dstNormal)
{
    const __m128 *v = (const __m128 *)src;
    __m128 position = LoadSkinPosition(v[0]);
    __m64 colorTexCoord = *(const __m64 *)&v[1].m128_u64[0];

    const DObjSkelMat *bone0 = BoneAt(boneMatrix, blend[0]);
    __m128 pos0 = PackXyzW(TransformPoint(bone0, position), position);
    __m128 outPos = pos0;
    for (int w = 1; w <= numWeights; ++w)
    {
        const DObjSkelMat *bone = BoneAt(boneMatrix, blend[2 * w - 1]);
        __m128 bonePos = PackXyzW(TransformPoint(bone, position), position);
        outPos = _mm_add_ps(outPos, _mm_mul_ps(DecodeWeight(blend[2 * w]), _mm_sub_ps(bonePos, pos0)));
    }

    _mm_stream_ps((float *)dst, outPos);
    _mm_stream_pi(dst + 2, colorTexCoord);
    _mm_stream_pi(dst + 3, normalTangent);
    _mm_stream_pi((__m64 *)dstNormal, normalTangent);
}

// Rigid skin: single bone, no blend. Matches retail (raw binormal sign).
static __forceinline __m64 Sse_SkinVertexRigid(const DObjSkelMat *bone, const GfxPackedVertex *src, __m64 *dst)
{
    const __m128 *v = (const __m128 *)src;
    __m128 srcPos = v[0];
    __m64 colorTexCoord = *(const __m64 *)&v[1].m128_u64[0];

    __m128 outPos = PackXyzW(TransformPoint(bone, srcPos), srcPos);
    __m64 normalTangent = PackNormalTangent(SkinUnitVec(bone, v[1].m128_u32[2]),
                                            SkinUnitVec(bone, v[1].m128_u32[3]));
    _mm_stream_ps((float *)dst, outPos);
    _mm_stream_pi(dst + 2, colorTexCoord);
    _mm_stream_pi(dst + 3, normalTangent);
    return normalTangent;
}

static __forceinline void Sse_SkinVertexRigidSimple(const DObjSkelMat *bone, const GfxPackedVertex *src, __m64 *dst,
                                             __m64 normalTangent, GfxPackedVertexNormal *dstNormal)
{
    const __m128 *v = (const __m128 *)src;
    __m128 srcPos = v[0];
    __m64 colorTexCoord = *(const __m64 *)&v[1].m128_u64[0];

    __m128 outPos = PackXyzW(TransformPoint(bone, srcPos), srcPos);
    _mm_stream_ps((float *)dst, outPos);
    _mm_stream_pi(dst + 2, colorTexCoord);
    _mm_stream_pi(dst + 3, normalTangent);
    _mm_stream_pi((__m64 *)dstNormal, normalTangent);
}

// ---------------------------------------------------------------------------
// weighted block loops (templated on weight count)
// dstVertNormals == null: verts only; non-null: also emit the normal stream.
// ---------------------------------------------------------------------------

template <int numWeights>
static void SkinWeightBlock(const GfxPackedVertex *srcVerts, const uint16_t *vertexBlend, int vertCount,
                            const DObjSkelMat *boneMatrix, GfxPackedVertex *dstVerts,
                            GfxPackedVertexNormal *dstVertNormals, int *pVertexIndex)
{
    iassert(dstVerts);
    iassert(srcVerts);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(srcVerts) & 15));

    int vertIndex = *pVertexIndex;
    for (int i = 0; i < vertCount; ++i)
    {
        __m64 normalTangent = Sse_SkinVertex<numWeights>(&srcVerts[vertIndex], vertexBlend, boneMatrix,
                                                         (__m64 *)&dstVerts[vertIndex]);
        if (dstVertNormals)
            _mm_stream_pi((__m64 *)&dstVertNormals[vertIndex], normalTangent);
        vertexBlend += 2 * numWeights + 1;
        ++vertIndex;
    }
    *pVertexIndex = vertIndex;
}

template <int numWeights>
static void SkinWeightBlockInOut(const GfxPackedVertex *srcVerts, const uint16_t *vertexBlend, int vertCount,
                                 const DObjSkelMat *boneMatrix, const GfxPackedVertexNormal *srcVertNormals,
                                 GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *dstVerts, int *pVertexIndex)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(srcVerts);
    iassert(srcVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(srcVerts) & 15));

    int vertIndex = *pVertexIndex;
    for (int i = 0; i < vertCount; ++i)
    {
        __m64 normalTangent = *(const __m64 *)&srcVertNormals[vertIndex];
        Sse_SkinVertexSimple<numWeights>(&srcVerts[vertIndex], vertexBlend, boneMatrix,
                                         (__m64 *)&dstVerts[vertIndex], normalTangent, &dstVertNormals[vertIndex]);
        vertexBlend += 2 * numWeights + 1;
        ++vertIndex;
    }
    *pVertexIndex = vertIndex;
}

// ---------------------------------------------------------------------------
// weighted dispatchers: run each weight-count block over its slice
// ---------------------------------------------------------------------------

static void R_SkinXSurfaceWeightSse_Impl(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                         const DObjSkelMat *boneMatrix, GfxPackedVertex *outVerts,
                                         GfxPackedVertexNormal *outNormals)
{
    PROF_SCOPED("SkinXSurfaceWeight");

    int vertIndex = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    if (vertexInfo->vertCount[0])
    {
        SkinWeightBlock<0>(inVerts, blend, vertexInfo->vertCount[0], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += vertexInfo->vertCount[0];
    }
    if (vertexInfo->vertCount[1])
    {
        SkinWeightBlock<1>(inVerts, blend, vertexInfo->vertCount[1], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += 3 * vertexInfo->vertCount[1];
    }
    if (vertexInfo->vertCount[2])
    {
        SkinWeightBlock<2>(inVerts, blend, vertexInfo->vertCount[2], boneMatrix, outVerts, outNormals, &vertIndex);
        blend += 5 * vertexInfo->vertCount[2];
    }
    if (vertexInfo->vertCount[3])
        SkinWeightBlock<3>(inVerts, blend, vertexInfo->vertCount[3], boneMatrix, outVerts, outNormals, &vertIndex);

    _mm_empty();
}

static void R_SkinXSurfaceWeightSse(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                    const DObjSkelMat *boneMatrix, GfxPackedVertex *outVerts)
{
    R_SkinXSurfaceWeightSse_Impl(inVerts, vertexInfo, boneMatrix, outVerts, nullptr);
}

static void R_SkinXSurfaceWeightSseOut(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                       const DObjSkelMat *boneMatrix, GfxPackedVertexNormal *outNormals,
                                       GfxPackedVertex *outVerts)
{
    R_SkinXSurfaceWeightSse_Impl(inVerts, vertexInfo, boneMatrix, outVerts, outNormals);
}

static void R_SkinXSurfaceWeightSseInOut(const GfxPackedVertex *inVerts, const XSurfaceVertexInfo *vertexInfo,
                                         const DObjSkelMat *boneMatrix, const GfxPackedVertexNormal *srcVertNormals,
                                         GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *outVerts)
{
    PROF_SCOPED("SkinXSurfaceWeight");

    int vertIndex = 0;
    const uint16_t *blend = vertexInfo->vertsBlend;
    if (vertexInfo->vertCount[0])
    {
        SkinWeightBlockInOut<0>(inVerts, blend, vertexInfo->vertCount[0], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += vertexInfo->vertCount[0];
    }
    if (vertexInfo->vertCount[1])
    {
        SkinWeightBlockInOut<1>(inVerts, blend, vertexInfo->vertCount[1], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += 3 * vertexInfo->vertCount[1];
    }
    if (vertexInfo->vertCount[2])
    {
        SkinWeightBlockInOut<2>(inVerts, blend, vertexInfo->vertCount[2], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);
        blend += 5 * vertexInfo->vertCount[2];
    }
    if (vertexInfo->vertCount[3])
        SkinWeightBlockInOut<3>(inVerts, blend, vertexInfo->vertCount[3], boneMatrix, srcVertNormals, dstVertNormals, outVerts, &vertIndex);

    _mm_empty();
}

// ---------------------------------------------------------------------------
// rigid skinning: one bone per vertex list, no blend
// ---------------------------------------------------------------------------

static void R_SkinXSurfaceRigidSse(const XSurface *surf, int totalVertCount,
                                   const DObjSkelMat *boneMatrix, GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            _mm_prefetch((const char *)&src[4], 0);
            Sse_SkinVertexRigid(bone, src, (__m64 *)dst);
            ++src;
            ++dst;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
    _mm_empty();
}

static void R_SkinXSurfaceRigidSseOut(const XSurface *surf, int totalVertCount, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *dstVertNormals, GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            _mm_prefetch((const char *)&src[4], 0);
            __m64 normalTangent = Sse_SkinVertexRigid(bone, src, (__m64 *)dst);
            _mm_stream_pi((__m64 *)dstVertNormals, normalTangent);
            ++src;
            ++dst;
            ++dstVertNormals;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
    _mm_empty();
}

static void R_SkinXSurfaceRigidSseInOut(const XSurface *surf, int totalVertCount, const DObjSkelMat *boneMatrix,
                                        const __m64 *srcVertNormals, GfxPackedVertexNormal *dstVertNormals,
                                        GfxPackedVertex *dstVerts)
{
    iassert(dstVerts);
    iassert(dstVertNormals);
    iassert(!(reinterpret_cast<uintptr_t>(dstVerts) & 15));
    iassert(!(reinterpret_cast<uintptr_t>(boneMatrix) & 15));

    PROF_SCOPED("SkinXSurfaceRigid");

    const GfxPackedVertex *src = (const GfxPackedVertex *)surf->verts0;
    GfxPackedVertex *dst = dstVerts;
    for (unsigned list = 0; list < surf->vertListCount; ++list)
    {
        const XRigidVertList *vertList = &surf->vertList[list];
        const DObjSkelMat *bone = BoneAt(boneMatrix, vertList->boneOffset);
        for (int i = 0; i < vertList->vertCount; ++i)
        {
            _mm_prefetch((const char *)&src[4], 0);
            Sse_SkinVertexRigidSimple(bone, src, (__m64 *)dst, *srcVertNormals, dstVertNormals);
            ++src;
            ++dst;
            ++srcVertNormals;
            ++dstVertNormals;
        }
    }
    iassert(dst - dstVerts == totalVertCount);
    _mm_empty();
}

// ---------------------------------------------------------------------------
// public entry point
// ---------------------------------------------------------------------------

void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *xsurf, const DObjSkelMat *boneMatrix,
                                      GfxPackedVertexNormal *skinVertNormalIn, GfxPackedVertexNormal *skinVertNormalOut,
                                      GfxPackedVertex *skinVerticesOut)
{
    if (skinVertNormalOut)
    {
        if (skinVertNormalIn)
        {
            if (xsurf->deformed)
                R_SkinXSurfaceWeightSseInOut(xsurf->verts0, &xsurf->vertInfo, boneMatrix,
                                             skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
            else
                R_SkinXSurfaceRigidSseInOut(xsurf, xsurf->vertCount, boneMatrix,
                                            (const __m64 *)skinVertNormalIn, skinVertNormalOut, skinVerticesOut);
        }
        else if (xsurf->deformed)
        {
            R_SkinXSurfaceWeightSseOut(xsurf->verts0, &xsurf->vertInfo, boneMatrix, skinVertNormalOut, skinVerticesOut);
        }
        else
        {
            R_SkinXSurfaceRigidSseOut(xsurf, xsurf->vertCount, boneMatrix, skinVertNormalOut, skinVerticesOut);
        }
    }
    else
    {
        iassert(!skinVertNormalIn);
        if (xsurf->deformed)
            R_SkinXSurfaceWeightSse(xsurf->verts0, &xsurf->vertInfo, boneMatrix, skinVerticesOut);
        else
            R_SkinXSurfaceRigidSse(xsurf, xsurf->vertCount, boneMatrix, skinVerticesOut);
    }
}

#endif
