// On-device checks for the allocator and the threading layer.
#pragma once

#include <stdint.h>

bool VitaSelfTest_Memory(char *report, uint32_t reportSize);
bool VitaSelfTest_Threads(char *report, uint32_t reportSize);
bool VitaSelfTest_Files(char *report, uint32_t reportSize);

// probes the clock ceiling by trying each rate and reading back what stuck
bool VitaSelfTest_Clocks(char *report, uint32_t reportSize);
