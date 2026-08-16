// The GXM side of a material pass: shader records and the vertex-layout cache.
#pragma once

#include <stdint.h>

#include "gxm_shader_archive.h"
#include "gxm_vertex.h"

#define GXM_MATERIAL_MAX_SOURCES 9
#define GXM_MATERIAL_MAX_ROUTING 16

// what one entry of MaterialVertexStreamRouting::decl becomes
struct GxmVertexDecl
{
    GxmStreamSource sources[GXM_MATERIAL_MAX_SOURCES];
    GxmStreamRouting routing[GXM_MATERIAL_MAX_ROUTING];
    uint32_t sourceCount;
    uint32_t routingCount;
};

// what MaterialVertexShaderProgram::vs and MaterialPixelShaderProgram::ps become
struct GxmMaterialShader
{
    uint32_t hash;              // FNV-1a over the SM3 token stream
    int handle;                 // shader archive handle; -1 while a fragment stage is unresolved
};

GxmVertexDecl *GxmMaterial_CreateVertexDecl(const GxmStreamSource *sources, uint32_t sourceCount,
                                            const GxmStreamRouting *routing,
                                            uint32_t routingCount);
void GxmMaterial_FreeVertexDecl(GxmVertexDecl *decl);

// a vertex shader resolves now; a fragment shader stays a hash so the pipeline can pick the
// alpha-test variant that matches the live state bits
GxmMaterialShader *GxmMaterial_CreateShader(const void *bytecode, uint32_t byteCount,
                                            GxmShaderStage stage);
void GxmMaterial_FreeShader(GxmMaterialShader *shader);

// cached on the (vertex shader, declaration) pair; the pipeline patches the stream strides
const GxmVertexLayout *GxmMaterial_Layout(const GxmMaterialShader *vertexShader,
                                          const GxmVertexDecl *decl);

uint32_t GxmMaterial_LayoutCount(void);
