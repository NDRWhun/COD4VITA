#include "gxm_fence.h"
#include "gxm_device.h"

#include <vita/platform/vita_system.h>

uint32_t GxmFence_Insert(void)
{
    return GxmDevice_ScenesSubmitted() + 1;
}

bool GxmFence_Reached(uint32_t fence)
{
    if (!fence)
        return true;

    // a fence taken before any scene was submitted has nothing to retire it, ever
    if (GxmDevice_ScenesSubmitted() == 0)
    {
        static bool reported;
        if (!reported)
        {
            reported = true;
            VitaSys_LogPrintf("fence %u polled with no scene submitted\n", fence);
            VitaSys_LogFlush();
        }
        return true;
    }

    // the counter rises by one per scene, so a signed difference survives the wrap; the fence is
    // the value the open scene will write, so it is reached only once that scene has retired
    return (int32_t)(GxmDevice_ScenesRetired() - fence) >= 0;
}
