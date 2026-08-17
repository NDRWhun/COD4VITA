#include "gxm_material.h"
#include "gxm_program.h"

#include <vita/platform/vita_system.h>

#include <stdlib.h>
#include <string.h>

#define GXM_LAYOUT_BUCKETS 256

struct GxmLayoutEntry
{
    GxmLayoutEntry *next;
    const GxmMaterialShader *shader;
    const GxmVertexDecl *decl;
    GxmVertexLayout layout;
};

static GxmLayoutEntry *s_buckets[GXM_LAYOUT_BUCKETS];
static uint32_t s_layoutCount;

static uint32_t GxmMaterial_Bucket(const void *shader, const void *decl)
{
    const uint32_t mixed = (uint32_t)(uintptr_t)shader * 2654435761u
                         ^ (uint32_t)(uintptr_t)decl * 2246822519u;
    return (mixed >> 8) & (GXM_LAYOUT_BUCKETS - 1);
}

GxmVertexDecl *GxmMaterial_CreateVertexDecl(const GxmStreamSource *sources, uint32_t sourceCount,
                                            const GxmStreamRouting *routing,
                                            uint32_t routingCount)
{
    if (sourceCount > GXM_MATERIAL_MAX_SOURCES || routingCount > GXM_MATERIAL_MAX_ROUTING)
        return NULL;

    GxmVertexDecl *decl = (GxmVertexDecl *)malloc(sizeof(GxmVertexDecl));
    if (!decl)
        return NULL;

    memset(decl, 0, sizeof(*decl));
    for (uint32_t i = 0; i < sourceCount; ++i)
        decl->sources[i] = sources[i];
    for (uint32_t i = 0; i < routingCount; ++i)
        decl->routing[i] = routing[i];

    decl->sourceCount = sourceCount;
    decl->routingCount = routingCount;
    return decl;
}

// entries hold the pointers they were keyed on, so a freed key must take its layouts with it
static void GxmMaterial_PurgeLayouts(const GxmMaterialShader *shader, const GxmVertexDecl *decl)
{
    for (uint32_t bucket = 0; bucket < GXM_LAYOUT_BUCKETS; ++bucket)
    {
        GxmLayoutEntry **link = &s_buckets[bucket];
        while (*link)
        {
            GxmLayoutEntry *entry = *link;
            if ((shader && entry->shader == shader) || (decl && entry->decl == decl))
            {
                *link = entry->next;
                free(entry);
                --s_layoutCount;
                continue;
            }
            link = &entry->next;
        }
    }
}

void GxmMaterial_FreeVertexDecl(GxmVertexDecl *decl)
{
    if (!decl)
        return;

    GxmMaterial_PurgeLayouts(NULL, decl);
    free(decl);
}

GxmMaterialShader *GxmMaterial_CreateShader(const void *bytecode, uint32_t byteCount,
                                            GxmShaderStage stage)
{
    const uint32_t length = GxmShaderArchive_BytecodeLength(bytecode, byteCount);
    if (!length)
    {
        const uint32_t *words = (const uint32_t *)bytecode;
        VitaSys_LogPrintf("shader: no end token in %u bytes, head %08x %08x %08x %08x\n",
                          byteCount, words[0], words[1], words[2], words[3]);
        VitaSys_LogFlush();
        return NULL;
    }

    const uint32_t hash = GxmShaderArchive_HashBytecode(bytecode, length);

    // the archive keys the fragment variants on alpha test; 0 is the one every shader has
    const int handle = GxmShaderArchive_Lookup(hash, stage, 0);
    if (handle < 0)
    {
        const uint32_t *words = (const uint32_t *)bytecode;
        VitaSys_LogPrintf("shader: no archive entry for hash %08x stage %i len %u of %u, "
                          "head %08x %08x %08x %08x\n", hash, (int)stage, length, byteCount,
                          words[0], words[1], words[2], words[3]);

        // the first missed stream is dumped whole, so the divergent bytes can be diffed offline
        static bool dumped;
        if (!dumped && length <= 512)
        {
            dumped = true;
            char line[128];
            for (uint32_t i = 0; i < length / 4; i += 4)
            {
                int n = 0;
                for (uint32_t j = i; j < i + 4 && j < length / 4; ++j)
                    n += snprintf(line + n, sizeof(line) - n, " %08x", words[j]);
                VitaSys_LogPrintf("  dw%02u%s\n", i, line);
            }
        }
        VitaSys_LogFlush();
        return NULL;
    }

    GxmMaterialShader *shader = (GxmMaterialShader *)malloc(sizeof(GxmMaterialShader));
    if (!shader)
        return NULL;

    shader->hash = hash;
    shader->handle = stage == GXM_STAGE_VERTEX ? handle : -1;
    return shader;
}

void GxmMaterial_FreeShader(GxmMaterialShader *shader)
{
    if (!shader)
        return;

    GxmMaterial_PurgeLayouts(shader, NULL);
    free(shader);
}

const GxmVertexLayout *GxmMaterial_Layout(const GxmMaterialShader *vertexShader,
                                          const GxmVertexDecl *decl)
{
    if (!vertexShader || !decl || vertexShader->handle < 0)
        return NULL;

    const uint32_t bucket = GxmMaterial_Bucket(vertexShader, decl);
    for (GxmLayoutEntry *entry = s_buckets[bucket]; entry; entry = entry->next)
    {
        if (entry->shader == vertexShader && entry->decl == decl)
            return &entry->layout;
    }

    // the live stream strides belong to the pipeline, which patches them at draw time
    static const uint16_t strides[GXM_MAX_VERTEX_STREAMS] = { 0 };

    GxmLayoutEntry *entry = (GxmLayoutEntry *)malloc(sizeof(GxmLayoutEntry));
    if (!entry)
        return NULL;

    if (!GxmVertex_BuildLayout(GxmProgram_Get(vertexShader->handle),
                               decl->sources, decl->sourceCount,
                               decl->routing, decl->routingCount,
                               strides, &entry->layout))
    {
        free(entry);
        return NULL;
    }

    entry->shader = vertexShader;
    entry->decl = decl;
    entry->next = s_buckets[bucket];
    s_buckets[bucket] = entry;
    ++s_layoutCount;
    return &entry->layout;
}

uint32_t GxmMaterial_LayoutCount(void)
{
    return s_layoutCount;
}
