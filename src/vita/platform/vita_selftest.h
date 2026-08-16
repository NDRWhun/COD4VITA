// On-device checks for the allocator and the threading layer.
#pragma once

#include <stdint.h>

bool VitaSelfTest_Memory(char *report, uint32_t reportSize);
bool VitaSelfTest_Threads(char *report, uint32_t reportSize);
