#include "vita_memory.h"

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define VITA_MEM_ALIGNMENT      16
#define VITA_MEM_MAIN_PAGE      (4 * 1024)
#define VITA_MEM_CDRAM_PAGE     (256 * 1024)
#define VITA_MEM_PHYCONT_PAGE   (1024 * 1024)
#define VITA_MEM_MAIN_GROWTH    (4 * 1024 * 1024)
#define VITA_MEM_CDRAM_GROWTH   (4 * 1024 * 1024)
#define VITA_MEM_GUARD          0x4B434F44u     // 'KCOD', to catch a foreign pointer

// a multiple of the alignment, so the payload after a header is aligned on its own
struct VitaMemNode
{
    VitaMemNode *next;
    VitaMemNode *previous;      // address order, for coalescing
    uint32_t size;              // payload bytes after the header
    uint32_t requested;
    uint8_t arena;
    uint8_t free;
    uint16_t padding;
    uint32_t guard;
    uint32_t reserved[2];
};
static_assert(sizeof(VitaMemNode) % VITA_MEM_ALIGNMENT == 0, "header must keep payloads aligned");

struct VitaMemBlock
{
    VitaMemBlock *next;
    SceUID uid;
    uint8_t *base;
    uint32_t size;
    VitaMemNode *first;
};

struct VitaMemArenaState
{
    VitaMemBlock *blocks;
    VitaMemNode *freeList;
    VitaMemStats stats;
    bool gpuMapped;
    uint32_t gpuAttr;
    bool neverGrew;             // the partition gave us nothing; stop asking per allocation
};

static VitaMemArenaState s_arenas[VITA_MEM_ARENA_COUNT];
static SceUID s_lock = -1;

static SceKernelMemBlockType VitaMem_BlockType(VitaMemArena arena)
{
    switch (arena)
    {
    case VITA_MEM_CDRAM:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    case VITA_MEM_MAIN_UNCACHED:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    case VITA_MEM_PHYCONT:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW;
    default:
        return SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    }
}

static uint32_t VitaMem_PageSize(VitaMemArena arena)
{
    if (arena == VITA_MEM_CDRAM)
        return VITA_MEM_CDRAM_PAGE;
    // the phycont partition is handed out a megabyte at a time
    if (arena == VITA_MEM_PHYCONT)
        return VITA_MEM_PHYCONT_PAGE;
    return VITA_MEM_MAIN_PAGE;
}

static SceUID s_gpuLock = -1;

// always taken inside the arena lock, never the other way round, so the two cannot deadlock
void VitaMem_GpuLock(void)
{
    if (s_gpuLock >= 0)
        sceKernelLockMutex(s_gpuLock, 1, NULL);
}

void VitaMem_GpuUnlock(void)
{
    if (s_gpuLock >= 0)
        sceKernelUnlockMutex(s_gpuLock, 1);
}

bool VitaMem_Init(void)
{
    memset(s_arenas, 0, sizeof(s_arenas));
    s_lock = sceKernelCreateMutex("kcod_mem", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    s_gpuLock = sceKernelCreateMutex("kcod_gpu", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    return s_lock >= 0 && s_gpuLock >= 0;
}

static void VitaMem_Enter(void)
{
    if (s_lock >= 0)
        sceKernelLockMutex(s_lock, 1, NULL);
}

static void VitaMem_Leave(void)
{
    if (s_lock >= 0)
        sceKernelUnlockMutex(s_lock, 1);
}

void VitaMem_SetGpuMapping(VitaMemArena arena, bool mapped, uint32_t gpuAttr)
{
    VitaMem_Enter();
    VitaMemArenaState *state = &s_arenas[arena];

    // a GXM restart drops every mapping, so grown blocks must follow the flag
    if (state->gpuMapped != mapped)
    {
        VitaMem_GpuLock();
        for (VitaMemBlock *block = state->blocks; block; block = block->next)
        {
            if (mapped)
                sceGxmMapMemory(block->base, block->size, (SceGxmMemoryAttribFlags)gpuAttr);
            else
                sceGxmUnmapMemory(block->base);
        }
        VitaMem_GpuUnlock();
    }

    state->gpuMapped = mapped;
    state->gpuAttr = gpuAttr;
    VitaMem_Leave();
}

static void VitaMem_PushFree(VitaMemArenaState *state, VitaMemNode *node)
{
    node->free = 1;
    node->next = state->freeList;
    state->freeList = node;
}

static void VitaMem_RemoveFree(VitaMemArenaState *state, VitaMemNode *node)
{
    VitaMemNode **link = &state->freeList;
    while (*link)
    {
        if (*link == node)
        {
            *link = node->next;
            node->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static bool VitaMem_Grow(VitaMemArena arena, uint32_t needed)
{
    VitaMemArenaState *state = &s_arenas[arena];

    // a partition that never yielded a block is not going to start; retrying costs a syscall
    // on every allocation that falls through to it
    if (state->neverGrew)
        return false;

    const uint32_t growth = arena == VITA_MEM_CDRAM ? VITA_MEM_CDRAM_GROWTH : VITA_MEM_MAIN_GROWTH;
    const uint32_t page = VitaMem_PageSize(arena);
    uint32_t size = needed + sizeof(VitaMemBlock) + sizeof(VitaMemNode) + VITA_MEM_ALIGNMENT;
    if (size < growth)
        size = growth;
    size = (size + page - 1) & ~(page - 1);

    SceUID uid = sceKernelAllocMemBlock("kcod_arena", VitaMem_BlockType(arena), size, NULL);
    if (uid < 0)
    {
        state->neverGrew = state->blocks == NULL;
        return false;
    }

    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0)
    {
        sceKernelFreeMemBlock(uid);
        return false;
    }

    if (state->gpuMapped)
    {
        VitaMem_GpuLock();
        const int mapped = sceGxmMapMemory(base, size, (SceGxmMemoryAttribFlags)state->gpuAttr);
        VitaMem_GpuUnlock();
        if (mapped < 0)
        {
            sceKernelFreeMemBlock(uid);
            return false;
        }
    }

    VitaMemBlock *block = (VitaMemBlock *)base;
    block->uid = uid;
    block->base = (uint8_t *)base;
    block->size = size;
    block->next = state->blocks;
    state->blocks = block;

    VitaMemNode *node = (VitaMemNode *)(block->base + sizeof(VitaMemBlock));
    node->size = size - sizeof(VitaMemBlock) - sizeof(VitaMemNode);
    node->requested = 0;
    node->arena = (uint8_t)arena;
    node->padding = 0;
    node->guard = VITA_MEM_GUARD;
    node->previous = NULL;
    node->next = NULL;
    block->first = node;

    VitaMem_PushFree(state, node);

    state->stats.reserved += size;
    state->stats.blocks++;
    return true;
}

static VitaMemNode *VitaMem_NodeAfter(VitaMemNode *node)
{
    return (VitaMemNode *)((uint8_t *)(node + 1) + node->size);
}

static bool VitaMem_InBlock(const VitaMemBlock *block, const void *pointer)
{
    return (const uint8_t *)pointer >= block->base &&
           (const uint8_t *)pointer < block->base + block->size;
}

// every split and every merge moves a boundary, and the node past it has to be told
static void VitaMem_LinkFollowing(VitaMemArenaState *state, VitaMemNode *node)
{
    VitaMemNode *following = VitaMem_NodeAfter(node);
    for (VitaMemBlock *block = state->blocks; block; block = block->next)
    {
        if (VitaMem_InBlock(block, following) && following->guard == VITA_MEM_GUARD)
        {
            following->previous = node;
            return;
        }
    }
}

void *VitaMem_Alloc(VitaMemArena arena, uint32_t size, uint32_t alignment)
{
    if (arena >= VITA_MEM_ARENA_COUNT || !size)
        return NULL;
    if (alignment < VITA_MEM_ALIGNMENT)
        alignment = VITA_MEM_ALIGNMENT;

    VitaMem_Enter();
    VitaMemArenaState *state = &s_arenas[arena];

    const uint32_t wanted = (size + VITA_MEM_ALIGNMENT - 1) & ~(VITA_MEM_ALIGNMENT - 1);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (VitaMemNode *node = state->freeList; node; node = node->next)
        {
            uint8_t *payload = (uint8_t *)(node + 1);
            const uint32_t misalign = (uint32_t)((uintptr_t)payload & (alignment - 1));
            uint32_t shift = misalign ? alignment - misalign : 0;

            // a shift splits off a free node, so grow it until it can hold a header
            while (shift && shift < sizeof(VitaMemNode) + VITA_MEM_ALIGNMENT)
                shift += alignment;

            if (node->size < wanted + shift)
                continue;

            VitaMem_RemoveFree(state, node);

            VitaMemNode *used = node;
            if (shift)
            {
                used = (VitaMemNode *)((uint8_t *)node + shift);
                used->size = node->size - shift;
                used->previous = node;
                used->guard = VITA_MEM_GUARD;
                node->size = shift - sizeof(VitaMemNode);
                node->free = 1;
                node->arena = (uint8_t)arena;
                node->padding = 0;

                VitaMem_LinkFollowing(state, used);
                VitaMem_PushFree(state, node);
            }

            // split when the tail can hold a header plus something worth handing out
            if (used->size >= wanted + sizeof(VitaMemNode) + VITA_MEM_ALIGNMENT)
            {
                VitaMemNode *tail = (VitaMemNode *)((uint8_t *)(used + 1) + wanted);
                tail->size = used->size - wanted - sizeof(VitaMemNode);
                tail->requested = 0;
                tail->arena = (uint8_t)arena;
                tail->padding = 0;
                tail->guard = VITA_MEM_GUARD;
                tail->previous = used;
                tail->next = NULL;

                used->size = wanted;
                VitaMem_LinkFollowing(state, tail);
                VitaMem_PushFree(state, tail);
            }

            used->free = 0;
            used->requested = size;
            used->arena = (uint8_t)arena;
            used->guard = VITA_MEM_GUARD;

            state->stats.used += used->size + sizeof(VitaMemNode);
            state->stats.liveAllocations++;
            if (state->stats.used > state->stats.peak)
                state->stats.peak = state->stats.used;

            VitaMem_Leave();
            return (uint8_t *)(used + 1);
        }

        if (attempt == 0 && !VitaMem_Grow(arena, wanted + alignment))
            break;
    }

    VitaMem_Leave();
    return NULL;
}

// a block whose whole span is one free node goes back to the kernel
static void VitaMem_ReleaseEmptyBlocks(VitaMemArenaState *state)
{
    VitaMemBlock **link = &state->blocks;
    while (*link)
    {
        VitaMemBlock *block = *link;
        VitaMemNode *node = block->first;
        const uint32_t whole = block->size - sizeof(VitaMemBlock) - sizeof(VitaMemNode);

        if (!node->free || node->size != whole)
        {
            link = &block->next;
            continue;
        }

        VitaMem_RemoveFree(state, node);
        *link = block->next;

        const SceUID uid = block->uid;
        void *base = block->base;
        const uint32_t size = block->size;

        state->stats.reserved -= size;
        state->stats.blocks--;

        if (state->gpuMapped)
        {
            VitaMem_GpuLock();
            sceGxmUnmapMemory(base);
            VitaMem_GpuUnlock();
        }
        sceKernelFreeMemBlock(uid);
    }
}

void VitaMem_Free(void *pointer)
{
    if (!pointer)
        return;

    VitaMemNode *node = (VitaMemNode *)pointer - 1;
    if (node->guard != VITA_MEM_GUARD || node->free)
        return;

    VitaMem_Enter();
    VitaMemArenaState *state = &s_arenas[node->arena];

    state->stats.used -= node->size + sizeof(VitaMemNode);
    state->stats.liveAllocations--;

    // coalesce forward, then backward, so neighbouring frees do not fragment the arena
    VitaMemNode *following = VitaMem_NodeAfter(node);
    for (VitaMemBlock *block = state->blocks; block; block = block->next)
    {
        if (!VitaMem_InBlock(block, node))
            continue;
        if (VitaMem_InBlock(block, following) && following->guard == VITA_MEM_GUARD &&
            following->free)
        {
            VitaMem_RemoveFree(state, following);
            node->size += following->size + sizeof(VitaMemNode);
            following->guard = 0;
            VitaMem_LinkFollowing(state, node);
        }
        break;
    }

    VitaMemNode *previous = node->previous;
    if (previous && previous->guard == VITA_MEM_GUARD && previous->free &&
        VitaMem_NodeAfter(previous) == node)
    {
        VitaMem_RemoveFree(state, previous);
        previous->size += node->size + sizeof(VitaMemNode);
        node->guard = 0;
        node = previous;
        VitaMem_LinkFollowing(state, node);
    }

    node->free = 1;
    VitaMem_PushFree(state, node);
    VitaMem_ReleaseEmptyBlocks(state);
    VitaMem_Leave();
}

uint32_t VitaMem_SizeOf(const void *pointer)
{
    if (!pointer)
        return 0;
    const VitaMemNode *node = (const VitaMemNode *)pointer - 1;
    return node->guard == VITA_MEM_GUARD ? node->requested : 0;
}

void VitaMem_GetStats(VitaMemArena arena, VitaMemStats *stats)
{
    VitaMem_Enter();
    *stats = s_arenas[arena].stats;

    stats->largestFreeRun = 0;
    for (const VitaMemNode *node = s_arenas[arena].freeList; node; node = node->next)
    {
        if (node->size > stats->largestFreeRun)
            stats->largestFreeRun = node->size;
    }
    VitaMem_Leave();
}

void VitaMem_Shutdown(void)
{
    for (uint32_t arena = 0; arena < VITA_MEM_ARENA_COUNT; ++arena)
    {
        VitaMemArenaState *state = &s_arenas[arena];
        for (VitaMemBlock *block = state->blocks; block; )
        {
            VitaMemBlock *next = block->next;
            const SceUID uid = block->uid;
            if (state->gpuMapped)
            {
                VitaMem_GpuLock();
                sceGxmUnmapMemory(block->base);
                VitaMem_GpuUnlock();
            }
            sceKernelFreeMemBlock(uid);
            block = next;
        }
        memset(state, 0, sizeof(*state));
    }

    if (s_lock >= 0)
    {
        sceKernelDeleteMutex(s_lock);
        s_lock = -1;
    }
}
