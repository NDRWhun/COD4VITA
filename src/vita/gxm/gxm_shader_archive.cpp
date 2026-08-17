#include "gxm_shader_archive.h"
#include "gxm_program.h"

#include <vita/platform/vita_memory.h>
#include <vita/platform/vita_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_MAGIC   0x5058474B      // 'KGXP'
#define ARCHIVE_VERSION 1

#define SM3_END_TOKEN   0x0000FFFF
#define SM3_COMMENT     0xFFFE

struct GxmArchiveHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t blobBase;
};

struct GxmArchiveEntry
{
    uint32_t hash;
    uint8_t stage;
    uint8_t alphaTest;
    uint16_t reserved;
    uint32_t offset;
    uint32_t size;
};

static uint8_t *s_archive;
static uint32_t s_archiveSize;
static const GxmArchiveEntry *s_entries;
static uint32_t s_entryCount;
static int *s_handles;                  // registered handle per entry, -1 until first use
static uint32_t s_missCount;

bool GxmShaderArchive_Load(const char *path)
{
    GxmShaderArchive_Unload();

    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < (long)sizeof(GxmArchiveHeader))
    {
        fclose(file);
        return false;
    }

    s_archive = (uint8_t *)malloc((size_t)size);
    if (!s_archive)
    {
        fclose(file);
        return false;
    }

    const size_t read = fread(s_archive, 1, (size_t)size, file);
    fclose(file);

    if (read != (size_t)size)
    {
        GxmShaderArchive_Unload();
        return false;
    }
    s_archiveSize = (uint32_t)size;

    const GxmArchiveHeader *header = (const GxmArchiveHeader *)s_archive;
    if (header->magic != ARCHIVE_MAGIC || header->version != ARCHIVE_VERSION)
    {
        GxmShaderArchive_Unload();
        return false;
    }

    const uint64_t indexEnd = (uint64_t)sizeof(GxmArchiveHeader)
                            + (uint64_t)header->count * sizeof(GxmArchiveEntry);
    if (indexEnd > s_archiveSize || header->blobBase > s_archiveSize)
    {
        GxmShaderArchive_Unload();
        return false;
    }

    s_entries = (const GxmArchiveEntry *)(s_archive + sizeof(GxmArchiveHeader));
    s_entryCount = header->count;

    s_handles = (int *)malloc(sizeof(int) * s_entryCount);
    if (!s_handles)
    {
        GxmShaderArchive_Unload();
        return false;
    }
    for (uint32_t i = 0; i < s_entryCount; ++i)
        s_handles[i] = -1;

    VitaSys_LogPrintf("shader archive %s: %u entries, %u bytes\n", path, s_entryCount,
                      s_archiveSize);
    return true;
}

void GxmShaderArchive_Unload(void)
{
    free(s_archive);
    free(s_handles);
    s_archive = NULL;
    s_handles = NULL;
    s_entries = NULL;
    s_archiveSize = 0;
    s_entryCount = 0;
    s_missCount = 0;
}

uint32_t GxmShaderArchive_BytecodeLength(const void *bytecode, uint32_t maxBytes)
{
    const uint32_t *tokens = (const uint32_t *)bytecode;
    const uint32_t limit = maxBytes / 4;
    uint32_t i = 1;                     // skip the version token

    while (i < limit)
    {
        const uint32_t token = tokens[i];
        if (token == SM3_END_TOKEN)
            return (i + 1) * 4;

        if ((token & 0xFFFF) == SM3_COMMENT)
            i += 1 + ((token >> 16) & 0x7FFF);
        else
            i += 1 + ((token >> 24) & 0xF);
    }
    return 0;
}

uint32_t GxmShaderArchive_HashBytecode(const void *bytecode, uint32_t byteCount)
{
    const uint8_t *bytes = (const uint8_t *)bytecode;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < byteCount; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// the index is sorted on (hash, stage, alphaTest), so the same order searches it
static int GxmShaderArchive_Compare(const GxmArchiveEntry *entry, uint32_t hash,
                                    uint32_t stage, uint32_t alphaTest)
{
    if (entry->hash != hash)
        return entry->hash < hash ? -1 : 1;
    if (entry->stage != stage)
        return entry->stage < stage ? -1 : 1;
    if (entry->alphaTest != alphaTest)
        return entry->alphaTest < alphaTest ? -1 : 1;
    return 0;
}

int GxmShaderArchive_Lookup(uint32_t hash, GxmShaderStage stage, uint32_t alphaTest)
{
    if (!s_entries)
        return -1;

    uint32_t low = 0, high = s_entryCount;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2;
        const int order = GxmShaderArchive_Compare(&s_entries[middle], hash, stage, alphaTest);
        if (order == 0)
        {
            // the lazy register caches into s_handles, so two threads asking for the same variant
            // must not both register it; GxmProgram_Register takes the same recursive lock
            VitaMem_GpuLock();
            if (s_handles[middle] < 0)
            {
                const GxmArchiveEntry *entry = &s_entries[middle];
                if ((uint64_t)entry->offset + entry->size > s_archiveSize)
                {
                    VitaMem_GpuUnlock();
                    return -1;
                }
                s_handles[middle] =
                    GxmProgram_Register(s_archive + entry->offset, entry->size);
            }
            const int handle = s_handles[middle];
            VitaMem_GpuUnlock();
            return handle;
        }
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }

    ++s_missCount;
    return -1;
}

uint32_t GxmShaderArchive_RegisterAll(void)
{
    uint32_t registered = 0;
    for (uint32_t i = 0; i < s_entryCount; ++i)
    {
        if (s_handles[i] < 0)
        {
            const GxmArchiveEntry *entry = &s_entries[i];
            if ((uint64_t)entry->offset + entry->size > s_archiveSize)
                continue;
            s_handles[i] = GxmProgram_Register(s_archive + entry->offset, entry->size);
        }
        if (s_handles[i] >= 0)
            ++registered;
    }
    return registered;
}

uint32_t GxmShaderArchive_Count(void)
{
    return s_entryCount;
}

uint32_t GxmShaderArchive_MissCount(void)
{
    return s_missCount;
}
