#include "gxm_fence.h"
#include "gxm_device.h"

uint32_t GxmFence_Insert(void)
{
    return GxmDevice_ScenesSubmitted() + 1;
}

bool GxmFence_Reached(uint32_t fence)
{
    if (!fence)
        return true;

    // the counter rises by one per scene, so a signed difference survives the wrap; the fence is
    // the value the open scene will write, so it is reached only once that scene has retired
    return (int32_t)(GxmDevice_ScenesRetired() - fence) >= 0;
}
