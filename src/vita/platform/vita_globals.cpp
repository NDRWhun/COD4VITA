#include <universal/q_shared.h>

#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_dvars.h>

// PMem commits 128 MB up front and common's loaded sounds land here through Z_Malloc; boot with
// 208 ran the heap out at ~7200 assets, and 77 MB of main stood free at that point
extern "C" {
unsigned int _newlib_heap_size_user = 224 * 1024 * 1024;
}

// storage only; R_RegisterDvars in r_dvars.cpp still assigns all three
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;
const dvar_t *r_fullscreen;

// r_cinematic.cpp defines this under CINEMA, which this target does not build
CinematicGlob cinematicGlob;
