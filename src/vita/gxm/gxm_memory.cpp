#include "gxm_memory.h"
#include <vita/platform/vita_breadcrumb.h>

#include <psp2/kernel/sysmem.h>

#include "../platform/vita_memory.h"
#include <psp2/gxm.h>
#include <string.h>

static uint32_t s_bytesUsed[GXM_MEM_DOMAIN_COUNT];
static uint32_t s_bytesPeak[GXM_MEM_DOMAIN_COUNT];

// CDRAM is handed out in 256KB pages, main memory in 4KB pages
static uint32_t GxmMem_PageSize(GxmMemDomain domain)
{
    return domain == GXM_MEM_CDRAM ? 256 * 1024 : 4 * 1024;
}

static SceKernelMemBlockType GxmMem_BlockType(GxmMemDomain domain)
{
    switch (domain)
    {
    case GXM_MEM_CDRAM:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    case GXM_MEM_MAIN_UNCACHED:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    default:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    }
}

static bool GxmMem_Reserve(GxmAlloc *out, uint32_t size, GxmMemDomain domain)
{
    const uint32_t page = GxmMem_PageSize(domain);
    const uint32_t aligned = (size + page - 1) & ~(page - 1);

    memset(out, 0, sizeof(*out));

    VitaSys_Breadcrumb("GxmMem: AllocMemBlock domain=%d size=%u", (int)domain, aligned);
    SceUID uid = sceKernelAllocMemBlock("gxm", GxmMem_BlockType(domain), aligned, NULL);
    VitaSys_Breadcrumb("GxmMem: AllocMemBlock uid=0x%08x", (unsigned)uid);
    if (uid < 0)
        return false;

    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0)
    {
        sceKernelFreeMemBlock(uid);
        return false;
    }
    VitaSys_Breadcrumb("GxmMem: base=%p, mapping", base);

    out->uid = uid;
    out->base = base;
    out->size = aligned;
    out->domain = domain;

    s_bytesUsed[domain] += aligned;
    if (s_bytesUsed[domain] > s_bytesPeak[domain])
        s_bytesPeak[domain] = s_bytesUsed[domain];
    return true;
}

static void GxmMem_Release(GxmAlloc *a)
{
    s_bytesUsed[a->domain] -= a->size;
    sceKernelFreeMemBlock(a->uid);
    memset(a, 0, sizeof(*a));
}

bool GxmMem_Alloc(GxmAlloc *out, uint32_t size, GxmMemDomain domain, uint32_t gpuAttr)
{
    if (!GxmMem_Reserve(out, size, domain))
        return false;

    // the ORed attribute flags are an int in C++; libgxm declares the parameter as the enum
    VitaSys_Breadcrumb("GxmMem: sceGxmMapMemory %u", out->size);
    if (sceGxmMapMemory(out->base, out->size, (SceGxmMemoryAttribFlags)gpuAttr) < 0)
    {
        GxmMem_Release(out);
        return false;
    }
    return true;
}

bool GxmMem_AllocVertexUsse(GxmAlloc *out, uint32_t size)
{
    if (!GxmMem_Reserve(out, size, GXM_MEM_MAIN))
        return false;

    if (sceGxmMapVertexUsseMemory(out->base, out->size, &out->usseOffset) < 0)
    {
        GxmMem_Release(out);
        return false;
    }
    out->mapKind = GXM_MAP_VERTEX_USSE;
    return true;
}

bool GxmMem_AllocFragmentUsse(GxmAlloc *out, uint32_t size)
{
    if (!GxmMem_Reserve(out, size, GXM_MEM_MAIN))
        return false;

    if (sceGxmMapFragmentUsseMemory(out->base, out->size, &out->usseOffset) < 0)
    {
        GxmMem_Release(out);
        return false;
    }
    out->mapKind = GXM_MAP_FRAGMENT_USSE;
    return true;
}

bool GxmMem_AllocPooled(GxmAlloc *out, uint32_t size, GxmMemDomain domain, uint32_t alignment)
{
    memset(out, 0, sizeof(*out));

    const VitaMemArena arena = domain == GXM_MEM_CDRAM ? VITA_MEM_CDRAM
                             : domain == GXM_MEM_MAIN_UNCACHED ? VITA_MEM_MAIN_UNCACHED
                             : VITA_MEM_MAIN;

    void *base = VitaMem_Alloc(arena, size, alignment);
    if (!base)
        return false;

    out->uid = -1;
    out->base = base;
    out->size = size;
    out->domain = domain;
    out->mapKind = GXM_MAP_PLAIN;

    s_bytesUsed[domain] += size;
    if (s_bytesUsed[domain] > s_bytesPeak[domain])
        s_bytesPeak[domain] = s_bytesUsed[domain];
    return true;
}

void GxmMem_Free(GxmAlloc *a)
{
    if (!a->base)
        return;

    if (a->uid < 0)
    {
        s_bytesUsed[a->domain] -= a->size;
        VitaMem_Free(a->base);
        memset(a, 0, sizeof(*a));
        return;
    }

    switch (a->mapKind)
    {
    case GXM_MAP_VERTEX_USSE:
        sceGxmUnmapVertexUsseMemory(a->base);
        break;
    case GXM_MAP_FRAGMENT_USSE:
        sceGxmUnmapFragmentUsseMemory(a->base);
        break;
    default:
        sceGxmUnmapMemory(a->base);
        break;
    }

    GxmMem_Release(a);
}

uint32_t GxmMem_BytesUsed(GxmMemDomain domain)
{
    return s_bytesUsed[domain];
}

uint32_t GxmMem_BytesPeak(GxmMemDomain domain)
{
    return s_bytesPeak[domain];
}
