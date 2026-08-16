#include <universal/q_shared.h>

#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_dvars.h>

// malloc only serves stdio and the GXM host allocations here; the engine hunk comes from memblocks,
// so the heap stays small and leaves the rest of user memory for sceKernelAllocMemBlock
extern "C" {
unsigned int _newlib_heap_size_user = 48 * 1024 * 1024;
}

// storage only; R_RegisterDvars in r_dvars.cpp still assigns all three
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;
const dvar_t *r_fullscreen;

// r_cinematic.cpp defines this under CINEMA, which this target does not build
CinematicGlob cinematicGlob;
