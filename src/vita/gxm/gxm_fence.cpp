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

    // nothing can retire a fence taken before any scene
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

    // signed difference survives the wrap
    return (int32_t)(GxmDevice_ScenesRetired() - fence) >= 0;
}
