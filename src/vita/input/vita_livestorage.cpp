#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <client/client.h>
#include <win32/win_storage.h>

// LiveStorage_Init and LiveStorage_ReadStats are both KISAK_MP-only, so nothing ever fetches
static playerStatNetworkData statData;

static const CaCItem s_cacItems[61] =
{
  { 0, 0, STAT_TYPE_SECONDARY },
  { 1, 15, STAT_TYPE_SECONDARY },
  { 2, 0, STAT_TYPE_SECONDARY },
  { 3, 42, STAT_TYPE_SECONDARY },
  { 4, 54, STAT_TYPE_SECONDARY },
  { 10, 0, STAT_TYPE_PRIMARY },
  { 11, 0, STAT_TYPE_PRIMARY },
  { 12, 12, STAT_TYPE_PRIMARY },
  { 13, 27, STAT_TYPE_PRIMARY },
  { 14, 39, STAT_TYPE_PRIMARY },
  { 20, 0, STAT_TYPE_PRIMARY },
  { 21, 45, STAT_TYPE_PRIMARY },
  { 22, 51, STAT_TYPE_PRIMARY },
  { 23, 24, STAT_TYPE_PRIMARY },
  { 24, 36, STAT_TYPE_PRIMARY },
  { 25, 0, STAT_TYPE_PRIMARY },
  { 26, 9, STAT_TYPE_PRIMARY },
  { 50, 0, STAT_TYPE_EQUIPMENT },
  { 55, 0, STAT_TYPE_EQUIPMENT },
  { 60, 21, STAT_TYPE_PRIMARY },
  { 61, 0, STAT_TYPE_PRIMARY },
  { 62, 48, STAT_TYPE_PRIMARY },
  { 64, 33, STAT_TYPE_PRIMARY },
  { 65, 6, STAT_TYPE_PRIMARY },
  { 70, 30, STAT_TYPE_PRIMARY },
  { 71, 0, STAT_TYPE_PRIMARY },
  { 80, 0, STAT_TYPE_PRIMARY },
  { 81, 0, STAT_TYPE_PRIMARY },
  { 82, 18, STAT_TYPE_PRIMARY },
  { 90, 0, STAT_TYPE_EQUIPMENT },
  { 91, 22, STAT_TYPE_EQUIPMENT },
  { 100, 0, STAT_TYPE_GRENADE },
  { 101, 0, STAT_TYPE_GRENADE },
  { 102, 0, STAT_TYPE_GRENADE },
  { 103, 0, STAT_TYPE_GRENADE },
  { 150, 34, STAT_TYPE_ABILITY },
  { 151, 10, STAT_TYPE_WEAPON },
  { 152, 25, STAT_TYPE_ABILITY },
  { 153, 43, STAT_TYPE_ABILITY },
  { 154, 0, STAT_TYPE_ABILITY },
  { 155, 13, STAT_TYPE_EQUIPMENT },
  { 156, 0, STAT_TYPE_WEAPON },
  { 157, 7, STAT_TYPE_ABILITY },
  { 158, 16, STAT_TYPE_ABILITY },
  { 160, 0, STAT_TYPE_WEAPON },
  { 161, 0, STAT_TYPE_ABILITY },
  { 162, 0, STAT_TYPE_ABILITY },
  { 163, 28, STAT_TYPE_WEAPON },
  { 164, 19, STAT_TYPE_WEAPON },
  { 165, 31, STAT_TYPE_EQUIPMENT },
  { 166, 37, STAT_TYPE_WEAPON },
  { 167, 0, STAT_TYPE_WEAPON },
  { 173, 40, STAT_TYPE_EQUIPMENT },
  { 176, 0, STAT_TYPE_EQUIPMENT },
  { 184, 0, STAT_TYPE_EQUIPMENT },
  { 185, 22, STAT_TYPE_EQUIPMENT },
  { 186, 0, STAT_TYPE_EQUIPMENT },
  { 190, 0, STAT_TYPE_EQUIPMENT },
  { 191, 0, STAT_TYPE_EQUIPMENT },
  { 192, 0, STAT_TYPE_EQUIPMENT },
  { 193, 0, STAT_TYPE_EQUIPMENT }
};

static const CaCItem *LiveStorage_FindCacItem(int itemIndex)
{
    for (uint32_t i = 0; i < sizeof(s_cacItems) / sizeof(s_cacItems[0]); ++i)
    {
        if (s_cacItems[i].itemIndex == itemIndex)
            return &s_cacItems[i];
    }
    return NULL;
}

static void LiveStorage_ValidateSlotItem(int itemIndex, int level, uint32_t typeMask)
{
    const CaCItem *cacItem = LiveStorage_FindCacItem(itemIndex);

    if (!cacItem)
        Com_Error(ERR_DROP, "No create-a-class item found at index %d\n", itemIndex);
    if (level < cacItem->minLevel)
        Com_Error(
            ERR_DROP,
            "Create-a-class item %d is too high level %d < %d.\n",
            itemIndex,
            level,
            cacItem->minLevel);
    if ((cacItem->type & typeMask) == 0)
        Com_Error(
            ERR_DROP,
            "Create-a-class item %d is not the expected type for this slot (0x%x & 0x%x) == 0.\n\n",
            itemIndex,
            typeMask,
            cacItem->type);
}

static bool LiveStorage_CaCHasOverkill(int controllerIndex, int index)
{
    return LiveStorage_GetStat(controllerIndex, 10 * (index / 10) + 6) == 166;
}

void __cdecl LiveStorage_ValidateCaCStat(int controllerIndex, int index, int value)
{
    if (index < 200 || index >= 250)
        Com_Error(ERR_DROP, "trying to set invalid create-a-class stat.  You can't set stat %d.\n", index);

    // there is no rank progression in the single player build, so every item is gated at level 0
    const int rank = 0;

    switch (index % 10)
    {
    case 1:
        LiveStorage_ValidateSlotItem(value, rank, 1u);
        break;
    case 3:
        if (LiveStorage_CaCHasOverkill(controllerIndex, index))
            LiveStorage_ValidateSlotItem(value, rank, 3u);
        else
            LiveStorage_ValidateSlotItem(value, rank, 2u);
        break;
    case 5:
        LiveStorage_ValidateSlotItem(value, rank, 4u);
        break;
    case 6:
        LiveStorage_ValidateSlotItem(value, rank, 8u);
        break;
    case 7:
        LiveStorage_ValidateSlotItem(value, rank, 0x10u);
        break;
    case 8:
        LiveStorage_ValidateSlotItem(value, rank, 0x20u);
        break;
    default:
        return;
    }
}

// stats below 2000 are bytes packed after the buffer checksum, the rest are dwords
int __cdecl LiveStorage_GetStat(int __formal, int index)
{
    if ((uint32_t)index > 0xDAA)
    {
        MyAssertHandler(__FILE__, __LINE__, 0, "%s\n\t(index) = %i", "(index >= 0 && index < 3499)", index);
        return 0;
    }
    if (!statData.statsFetched)
        return 0;
    if (index < 2000)
        return statData.playerStats[index + 4];
    if (index < 3498)
        return *(uint32_t *)&statData.playerStats[4 * index - 5996];

    MyAssertHandler(__FILE__, __LINE__, 0, "%s", va("Unhandled stat index %i", index));
    return 0;
}

void __cdecl LiveStorage_SetStat(int __formal, int index, uint32_t value)
{
    if ((uint32_t)index > 0xDAA)
    {
        MyAssertHandler(__FILE__, __LINE__, 0, "%s\n\t(index) = %i", "(index >= 0 && index < 3499)", index);
        return;
    }
    if (!statData.statsFetched)
    {
        Com_Printf(14, "Tried to set stat index %i before we have obtained player stats\n", index);
        return;
    }

    if (index < 2000)
    {
        if (value >= 0x100)
        {
            CL_DumpReliableCommands(0);
            Com_Error(
                ERR_SERVERDISCONNECT,
                "Trying to set index %i (which is a byte value) to invalid value %i",
                index,
                value);
        }
        if (statData.playerStats[index + 4] != value)
        {
            statData.playerStats[index + 4] = (unsigned char)value;
            statData.statWriteNeeded = 1;
        }
    }
    else if (index < 3498)
    {
        uint32_t *slot = (uint32_t *)&statData.playerStats[4 * index - 5996];
        if (*slot != value)
        {
            *slot = value;
            statData.statWriteNeeded = 1;
        }
    }
    else
    {
        MyAssertHandler(__FILE__, __LINE__, 0, "%s", va("Unhandled stat index %i", index));
    }
}

void __cdecl LiveStorage_NewUser()
{
    memset(&statData, 0, sizeof(statData));
}
