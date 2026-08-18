// GPU-visible allocation: kernel memblock plus a GXM mapping, CPU address == GPU address.
#pragma once

#include <psp2/types.h>
#include <stdint.h>

enum GxmMemDomain
{
    GXM_MEM_MAIN,           // CPU-cached; GPU snoops the CPU caches
    GXM_MEM_MAIN_UNCACHED,  // CPU-uncached; for data written once and never re-read
    GXM_MEM_CDRAM,          // video memory, 256KB granularity
    GXM_MEM_PHYCONT,        // physically contiguous, its own partition, 1MB granularity
    GXM_MEM_DOMAIN_COUNT
};

// nothing the GPU may touch is mapped below this, so a lower address is a null or wrapped pointer
#define GXM_LOWEST_MAPPED 0x1000000u

// CDRAM held back for a zone's geometry buffers together, sized for the heaviest level's set
#define GXM_CDRAM_GEOMETRY_RESERVE (60u * 1024u * 1024u)

enum GxmMapKind
{
    GXM_MAP_PLAIN,
    GXM_MAP_VERTEX_USSE,
    GXM_MAP_FRAGMENT_USSE
};

struct GxmAlloc
{
    SceUID uid;             // -1 when the payload came from the shared arena
    void *base;
    uint32_t size;          // rounded up to the domain's page size
    uint32_t usseOffset;    // shader-core offset; USSE allocations only
    GxmMemDomain domain;
    GxmMapKind mapKind;
};

// small allocations come from the suballocating arena instead of their own memblock
bool GxmMem_AllocPooled(GxmAlloc *out, uint32_t size, GxmMemDomain domain, uint32_t alignment);

// gpuAttr is SCE_GXM_MEMORY_ATTRIB_READ, optionally ORed with _WRITE
bool GxmMem_Alloc(GxmAlloc *out, uint32_t size, GxmMemDomain domain, uint32_t gpuAttr);
bool GxmMem_AllocVertexUsse(GxmAlloc *out, uint32_t size);
bool GxmMem_AllocFragmentUsse(GxmAlloc *out, uint32_t size);
// the caller must have ensured the GPU is done with the memory; unmapping live memory page-faults
void GxmMem_Free(GxmAlloc *a);

uint32_t GxmMem_BytesUsed(GxmMemDomain domain);
uint32_t GxmMem_BytesPeak(GxmMemDomain domain);

uint32_t GxmMem_FreeMain(void);
uint32_t GxmMem_FreeCdram(void);
