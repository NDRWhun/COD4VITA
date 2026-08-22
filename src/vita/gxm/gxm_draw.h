// The draw path: programs, constants, textures, streams, submit.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_vertex.h"

#define GXM_MAX_CONSTANT_REGISTERS 64
#define GXM_MAX_TEXTURE_UNITS      16

bool GxmDraw_Init(void);
void GxmDraw_Shutdown(void);

// handles come from GxmProgram_Register; the programs from the program cache
void GxmDraw_SetVertexProgram(int shaderHandle, const SceGxmVertexProgram *program);
void GxmDraw_SetFragmentProgram(int shaderHandle, const SceGxmFragmentProgram *program);

// register ranges, as SetVertexShaderConstantF supplies them
void GxmDraw_SetVertexConstants(uint32_t startRegister, const float *values,
                                uint32_t registerCount);
void GxmDraw_SetFragmentConstants(uint32_t startRegister, const float *values,
                                  uint32_t registerCount);

// a null texture binds a dummy rather than leaving the unit unbound, which would fault
void GxmDraw_SetTexture(uint32_t unit, const SceGxmTexture *texture);
void GxmDraw_SetVolumeLayout(uint32_t unit, const float layout[2]);
void GxmDraw_UnbindTextures(void);
void GxmDraw_SetStream(uint32_t streamIndex, const void *data);

bool GxmDraw_Indexed(SceGxmPrimitiveType primitive, const uint16_t *indices,
                     uint32_t indexCount);

uint32_t GxmDraw_DrawCount(void);

// constants the engine set outside the shadow's range; non-zero means a wrong assumption
uint32_t GxmDraw_DroppedConstants(void);

void GxmDraw_ResetCounters(void);
