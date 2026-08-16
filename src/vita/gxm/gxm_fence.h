// GPU progress fences, for the backend's pool of "has the GPU caught up yet" queries.
#pragma once

#include <stdint.h>

// never 0, so the token survives being kept in one of the engine's pointer-shaped slots
uint32_t GxmFence_Insert(void);

bool GxmFence_Reached(uint32_t fence);
