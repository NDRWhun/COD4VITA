// Lookup for the offline-baked GXP blobs, keyed by SM3 bytecode hash.
#pragma once

#include <stdint.h>

enum GxmShaderStage
{
    GXM_STAGE_VERTEX = 0,
    GXM_STAGE_FRAGMENT = 1
};

bool GxmShaderArchive_Load(const char *path);
void GxmShaderArchive_Unload(void);

// FNV-1a over the token stream, matching scripts/vita/ff_shader_scan.py
uint32_t GxmShaderArchive_HashBytecode(const void *bytecode, uint32_t byteCount);

// length of an SM3 token stream in bytes, version token through END
uint32_t GxmShaderArchive_BytecodeLength(const void *bytecode, uint32_t maxBytes);

// registers the blob with the program cache on first use; returns a shader handle
int GxmShaderArchive_Lookup(uint32_t hash, GxmShaderStage stage, uint32_t alphaTest);

uint32_t GxmShaderArchive_Count(void);
uint32_t GxmShaderArchive_MissCount(void);

// diagnostic; the normal path registers lazily on first lookup
uint32_t GxmShaderArchive_RegisterAll(void);
