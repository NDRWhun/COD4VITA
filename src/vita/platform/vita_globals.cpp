#include <universal/q_shared.h>

#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_dvars.h>

// the ledger at the last boot OOM: PMem 96 + sounds 34 + vertex pools 25 + hunk 10 + newlib
// overhead, with common's sound tail still to land; main keeps ~45 MB for the GXM side
extern "C" {
unsigned int _newlib_heap_size_user = 240 * 1024 * 1024;
}

// storage only; R_RegisterDvars in r_dvars.cpp still assigns all three
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;
const dvar_t *r_fullscreen;

// r_cinematic.cpp defines this under CINEMA, which this target does not build
CinematicGlob cinematicGlob;
