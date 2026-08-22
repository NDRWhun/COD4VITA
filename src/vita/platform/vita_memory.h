// Suballocating allocator over the kernel's memory blocks.
#pragma once

#include <psp2/types.h>
#include <stdint.h>

// video memory comes in 256KB pages, so arenas carve out of a few large blocks
enum VitaMemArena
{
    VITA_MEM_MAIN,              // CPU-cached
    VITA_MEM_MAIN_UNCACHED,     // write-combined, for data the CPU only writes
    VITA_MEM_CDRAM,             // video memory
    VITA_MEM_PHYCONT,           // physically contiguous, a partition of its own
    VITA_MEM_ARENA_COUNT
};

struct VitaMemStats
{
    uint32_t reserved;          // taken from the kernel
    uint32_t used;              // handed out, including per-allocation headers
    uint32_t peak;
    uint32_t blocks;            // kernel blocks backing the arena
    uint32_t liveAllocations;
    uint32_t largestFreeRun;
};

bool VitaMem_Init(void);
void VitaMem_Shutdown(void);

// GPU-visible arenas map each kernel block once, so every suballocation is reachable
void VitaMem_SetGpuMapping(VitaMemArena arena, bool mapped, uint32_t gpuAttr);

void *VitaMem_Alloc(VitaMemArena arena, uint32_t size, uint32_t alignment);
void VitaMem_Free(void *pointer);

// the size the caller asked for, or 0 if the pointer is not ours
uint32_t VitaMem_SizeOf(const void *pointer);

void VitaMem_GetStats(VitaMemArena arena, VitaMemStats *stats);

// take this around every driver-state sceGxm call: libgxm serialises nothing and two threads map memory
void VitaMem_GpuLock(void);
void VitaMem_GpuUnlock(void);
