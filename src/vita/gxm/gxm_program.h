// Registry and cache for the offline-compiled GXP programs.
//
// Fragment programs bake in blend, colour mask and MSAA, so one pixel shader turns
// into several GXM programs. They are created on first use and kept, never per draw.
#pragma once

#include <psp2/gxm.h>
#include <stdint.h>

#include "gxm_state.h"

#define GXM_MAX_SHADERS 2560

bool GxmProgram_Init(void);
void GxmProgram_Shutdown(void);

// takes a GXP blob owned by the caller and validates it; returns a handle, or -1
int GxmProgram_Register(const void *gxp, uint32_t size);

const SceGxmProgram *GxmProgram_Get(int handle);

SceGxmVertexProgram *GxmProgram_Vertex(int handle,
                                       const SceGxmVertexAttribute *attributes,
                                       uint32_t attributeCount,
                                       const SceGxmVertexStream *streams,
                                       uint32_t streamCount);

// linkedVertex lets the patcher remap texcoords across gaps in the vertex outputs
SceGxmFragmentProgram *GxmProgram_Fragment(int handle, const GxmProgramState *state,
                                           int linkedVertexHandle);

uint32_t GxmProgram_VertexCount(void);
uint32_t GxmProgram_FragmentCount(void);
