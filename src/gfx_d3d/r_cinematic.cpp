#include <universal/q_shared.h>
#include "r_cinematic.h"

#include <universal/assertive.h>
#include <qcommon/threads.h>
#include <universal/com_memory.h>
#include "r_init.h"
#ifndef KISAK_VITA
#include <msslib/mss.h>
#endif
#include <sound/snd_local.h>
#include <win32/win_local.h>
#include "rb_state.h"
#include "r_image.h"
#include <database/database.h>
#include <qcommon/com_fileaccess.h>
#include <cgame/cg_local.h>
#include <universal/profile.h>

// #define CINEMA

#ifdef CINEMA

#ifndef USE_SEPARATE_BLIT_TEXTURE
#error You need to turn on this Flag for the Separate Y, Cb, Cr, A Texture decoding in bink (see: binktextures.cpp)
#endif

CinematicGlob cinematicGlob;
bool g_cinematicThreadInitialized;
CinematicThreadState g_cinematicThreadState;

int __cdecl CinematicHunk_Alloc(CinematicHunk* hunk, int size)
{
    const char* v2; // eax
    char* alloced; // [esp+0h] [ebp-4h]

    iassert( hunk->base );
    iassert( hunk->atFront );
    iassert( hunk->atBack );
    iassert( hunk->end );
    iassert( size >= 0 );
    alloced = (char*)hunk->atFront;
    hunk->atFront = &alloced[size];
    if (hunk->atFront <= hunk->atBack)
        return (int)alloced;
    if (!alwaysfails)
    {
        v2 = va("CinematicHunk_Alloc failed: 0x%08x 0x%08x 0x%08x\n", hunk->atFront, size, hunk->atBack);
        MyAssertHandler(".\\r_cinematic.cpp", 376, 0, v2);
    }
    return -1;
}

void R_Cinematic_RelinquishIO()
{
    iassert( cinematicGlob.hasFileIO );
    iassert( cinematicGlob.bink );
    BinkControlBackgroundIO(cinematicGlob.bink, 1);
    R_Cinematic_CheckBinkError();
    Sys_ResumeDatabaseThread(THREAD_OWNER_CINEMATICS);
    cinematicGlob.hasFileIO = 0;
}

void R_Cinematic_CheckBinkError()
{
    const char *v0; // eax
    const char *binkError; // [esp+0h] [ebp-4h]

    binkError = (const char *)BinkGetError();
    if (binkError)
    {
        if (*binkError)
        {
            v0 = va("BinkGetError(): \"%s\"", binkError);
            MyAssertHandler(".\\r_cinematic.cpp", 282, 0, "%s\n\t%s", "!binkError || binkError[0] == '\\0'", v0);
        }
    }
}

void __cdecl R_Cinematic_InitBinkVolumes()
{
    int v0; // [esp+0h] [ebp-50h]
    int v1; // [esp+28h] [ebp-28h]
    int volumes[8]; // [esp+30h] [ebp-20h] BYREF

    if ((int)(__int64)(cinematicGlob.playbackVolume * 32768.0) < 0x8000)
        v1 = (__int64)(cinematicGlob.playbackVolume * 32768.0);
    else
        v1 = 0x8000;
    if (v1 > 0)
        v0 = v1;
    else
        v0 = 0;
    memset(&volumes[2], 0, 24);
    volumes[0] = v0;
    volumes[1] = v0;
    BinkSetMixBinVolumes(cinematicGlob.bink, 0, 0, volumes, 8);
    R_Cinematic_CheckBinkError();
    volumes[0] = 0;
    volumes[1] = 0;
    memset(&volumes[3], 0, 20);
    volumes[2] = v0;
    BinkSetMixBinVolumes(cinematicGlob.bink, 1, 0, volumes, 8);
    R_Cinematic_CheckBinkError();
    memset(volumes, 0, 12);
    memset(&volumes[4], 0, 16);
    volumes[3] = v0;
    BinkSetMixBinVolumes(cinematicGlob.bink, 2, 0, volumes, 8);
    R_Cinematic_CheckBinkError();
    memset(volumes, 0, 16);
    volumes[6] = 0;
    volumes[7] = 0;
    volumes[4] = v0;
    volumes[5] = v0;
    BinkSetMixBinVolumes(cinematicGlob.bink, 3, 0, volumes, 8);
    R_Cinematic_CheckBinkError();
    memset(volumes, 0, 24);
    volumes[6] = v0;
    volumes[7] = v0;
    BinkSetMixBinVolumes(cinematicGlob.bink, 4, 0, volumes, 8);
    R_Cinematic_CheckBinkError();
}

void __cdecl R_Cinematic_Init()
{
    iassert(!g_cinematicThreadInitialized || g_cinematicThreadState == CINEMATIC_THREAD_STATE_TO_HOST_BETWEEN_UPDATES);

    memset(&cinematicGlob, 0, sizeof(cinematicGlob));

    cinematicGlob.activeImageFrame = -1;
    R_Cinematic_ReserveMemory();

    iassert( cinematicGlob.currentPaused == CINEMATIC_NOT_PAUSED );
    iassert( cinematicGlob.targetPaused == CINEMATIC_NOT_PAUSED );

    if (!g_cinematicThreadInitialized)
    {
        g_cinematicThreadInitialized = 1;
        g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_BETWEEN_UPDATES;
        Sys_SpawnCinematicsThread(R_Cinematic_Thread);
        KISAK_NULLSUB();
        cinematicGlob.atHighPriority = 1;
    }
}

void R_Cinematic_ReserveMemory()
{
    cinematicGlob.memPool = Z_Malloc(0xD00000, "R_Cinematic_ReserveMemory", 18);
}

void __cdecl  R_Cinematic_Thread(uint32_t threadContext)
{
    iassert(threadContext == THREAD_CONTEXT_CINEMATIC);
    while (1)
    {
        R_CinematicThread_WaitForHostEvent();
        R_Cinematic_UpdateFrame_Core2();
        g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_BETWEEN_UPDATES;
        Sys_ResetCinematicsHostOutstandingRequestEvent();
        Sys_SetCinematicsThreadOutstandingRequestEvent();
    }
}

void R_Cinematic_UpdateFrame_Core2()
{
    bool localTargetChanged; // [esp+3h] [ebp-109h]
    char localTargetCinematic[256]; // [esp+4h] [ebp-108h] BYREF
    uint32_t localTargetPlaybackFlags; // [esp+108h] [ebp-4h]

    Sys_EnterCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
    localTargetChanged = cinematicGlob.targetCinematicChanged;
    if (cinematicGlob.targetCinematicChanged)
    {
        I_strncpyz(localTargetCinematic, cinematicGlob.targetCinematicName, 256);
        cinematicGlob.targetCinematicChanged = 0;
        localTargetPlaybackFlags = cinematicGlob.playbackFlags;
    }
    else
    {
        localTargetCinematic[0] = 0;
        localTargetPlaybackFlags = 0;
    }
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
    R_Cinematic_UpdateFrame_Core(localTargetChanged, localTargetCinematic, localTargetPlaybackFlags);
}

void __cdecl R_Cinematic_UpdateFrame_Core(
    bool localTargetChanged,
    char *localTargetCinematic,
    uint32_t localPlaybackFlags)
{
    bool isCinematicBeingPlayed; // [esp+3h] [ebp-1h]

    iassert( localTargetChanged || !localPlaybackFlags );
    iassert(localTargetChanged || !localTargetCinematic || !localTargetCinematic[0]);

    cinematicGlob.underrun = 0;

    if (localTargetChanged)
    {
        if (cinematicGlob.currentCinematicName[0])
            R_Cinematic_StopPlayback_Now();
        if (*localTargetCinematic && R_Cinematic_StartPlayback_Now(localTargetCinematic, localPlaybackFlags))
            I_strncpyz(cinematicGlob.currentCinematicName, localTargetCinematic, 256);
        else
            cinematicGlob.currentCinematicName[0] = 0;
    }
    isCinematicBeingPlayed = cinematicGlob.currentCinematicName[0] != 0;
    if (cinematicGlob.currentCinematicName[0])
    {
        cinematicGlob.framesStopped = 0;
    }
    else
    {
        iassert(cinematicGlob.activeImageFrame == CINEMATIC_INVALID_IMAGE_FRAME);
        if (R_Cinematic_AreHunksOpen())
        {
            if (++cinematicGlob.framesStopped >= 5)
                R_Cinematic_HunksClose();
        }
    }
    if (isCinematicBeingPlayed)
    {
        iassert( cinematicGlob.bink );
        if (!R_Cinematic_Advance())
            cinematicGlob.cinematicFinished = 1;
    }
}

char __cdecl R_Cinematic_AreHunksOpen()
{
    if (CinematicHunk_IsOpen(&cinematicGlob.masterHunk))
    {
        iassert( CinematicHunk_IsOpen( &cinematicGlob.binkHunk ) );
        iassert( CinematicHunk_IsOpen( &cinematicGlob.residentHunk ) );
        return 1;
    }
    else
    {
        iassert( !CinematicHunk_IsOpen( &cinematicGlob.binkHunk ) );
        iassert( !CinematicHunk_IsOpen( &cinematicGlob.residentHunk ) );
        return 0;
    }
}

char __cdecl CinematicHunk_IsOpen(CinematicHunk *hunk)
{
    if (hunk->base)
    {
        iassert( hunk->atFront );
        iassert( hunk->atBack );
        iassert( hunk->end );
        return 1;
    }
    else
    {
        iassert( !hunk->atFront );
        iassert( !hunk->atBack );
        iassert( !hunk->end );
        return 0;
    }
}

void R_Cinematic_HunksClose()
{
    CinematicHunk_Close(&cinematicGlob.masterHunk);
    CinematicHunk_Close(&cinematicGlob.binkHunk);
    CinematicHunk_Close(&cinematicGlob.residentHunk);
}

void __cdecl CinematicHunk_Close(CinematicHunk *hunk)
{
    iassert( hunk->base );
    iassert( hunk->atFront );
    iassert( hunk->atBack );
    iassert( hunk->end );
    hunk->base = 0;
    hunk->atFront = 0;
    hunk->atBack = 0;
    hunk->end = 0;
}

char __cdecl R_Cinematic_Advance()
{
    CinematicEnum targetPaused; // [esp+A0h] [ebp-44h]
    BINKREALTIME binkRealtime; // [esp+A4h] [ebp-40h] BYREF
    uint32_t percentageFull; // [esp+DCh] [ebp-8h]
    int skipped; // [esp+E0h] [ebp-4h]

    PROF_SCOPED("R_Cinematic_Advance");

    targetPaused = cinematicGlob.targetPaused;

    iassert((targetPaused == CINEMATIC_PAUSED || targetPaused == CINEMATIC_NOT_PAUSED));
    iassert((cinematicGlob.currentPaused == CINEMATIC_PAUSED || cinematicGlob.currentPaused == CINEMATIC_NOT_PAUSED));
    iassert( cinematicGlob.bink );
    iassert( !cinematicGlob.underrun );

    percentageFull = R_Cinematic_GetPercentageFull();

    if (percentageFull < 40 && (cinematicGlob.playbackFlags & 1) == 0)
    {
        cinematicGlob.underrun = 1;
        targetPaused = CINEMATIC_PAUSED;
    }
    if (targetPaused != cinematicGlob.currentPaused)
    {
        BinkPause(cinematicGlob.bink, targetPaused == CINEMATIC_PAUSED);
        R_Cinematic_CheckBinkError();
        cinematicGlob.currentPaused = targetPaused;
    }

    int wait;

    {
        PROF_SCOPED("BinkWait");
        wait = BinkWait(cinematicGlob.bink);
    }
    
    if (!wait && !cinematicGlob.underrun)
    {
        Lock_Bink_textures(&cinematicGlob.binkTextureSet);
        {
            PROF_SCOPED("BinkDoFrame");
            skipped = BinkDoFrame(cinematicGlob.bink);
        }
        if (!skipped)
        {
            iassert((cinematicGlob.binkTextureSet.bink_buffers.FrameNum == 0 || cinematicGlob.binkTextureSet.bink_buffers.FrameNum == 1));
            cinematicGlob.activeImageFrame = cinematicGlob.binkTextureSet.bink_buffers.FrameNum;
            cinematicGlob.activeImageFrameTextureSet = cinematicGlob.activeTextureSet;
        }
        Unlock_Bink_textures(dx.device, &cinematicGlob.binkTextureSet, cinematicGlob.bink);
        if ((cinematicGlob.playbackFlags & 2) != 0 || cinematicGlob.bink->FrameNum != cinematicGlob.bink->Frames)
        {
            {
                PROF_SCOPED("BinkNextFrame");
                BinkNextFrame(cinematicGlob.bink);
            }
        }
    }

    R_Cinematic_CheckBinkError();

    if ((cinematicGlob.playbackFlags & 2) != 0 || cinematicGlob.bink->FrameNum != cinematicGlob.bink->Frames)
    {
        iassert( cinematicGlob.bink );
        BinkGetRealtime(cinematicGlob.bink, &binkRealtime, 0);
        R_Cinematic_UpdateTimeInMsec(&binkRealtime);
        if ((cinematicGlob.playbackFlags & 8) != 0)
        {
            percentageFull = 100;
        }
        else if (binkRealtime.ReadBufferUsed < binkRealtime.ReadBufferSize)
        {
            percentageFull = 100 * binkRealtime.ReadBufferUsed / cinematicGlob.binkIOSize;
        }
        else
        {
            percentageFull = 100;
        }
        if (cinematicGlob.hasFileIO)
        {
            if (percentageFull > 0x5F)
                R_Cinematic_RelinquishIO();
        }
        else if (percentageFull < 0x32)
        {
            if ((cinematicGlob.playbackFlags & 8) != 0)
                MyAssertHandler(
                    ".\\r_cinematic.cpp",
                    1181,
                    0,
                    "%s",
                    "!(cinematicGlob.playbackFlags & CINEMATIC_PLAYBACKFLAGS_MEMORY_RESIDENT)");
            R_Cinematic_SeizeIO();
        }
        return 1;
    }
    else
    {
        if (cinematicGlob.hasFileIO)
            R_Cinematic_RelinquishIO();
        return 0;
    }
}

uint32_t __cdecl R_Cinematic_GetPercentageFull()
{
    BINKREALTIME binkRealtime; // [esp+4h] [ebp-38h] BYREF

    iassert( cinematicGlob.bink );
    BinkGetRealtime(cinematicGlob.bink, &binkRealtime, 0);
    if ((cinematicGlob.playbackFlags & 8) != 0)
        return 100;
    if (binkRealtime.ReadBufferUsed < binkRealtime.ReadBufferSize)
        return 100 * binkRealtime.ReadBufferUsed / cinematicGlob.binkIOSize;
    return 100;
}

void R_Cinematic_SeizeIO()
{
    iassert( !cinematicGlob.hasFileIO );
    iassert( cinematicGlob.bink );
    BinkControlBackgroundIO(cinematicGlob.bink, 2);
    R_Cinematic_CheckBinkError();
    Sys_SuspendDatabaseThread(THREAD_OWNER_CINEMATICS);
    cinematicGlob.hasFileIO = 1;
}

void __cdecl R_Cinematic_UpdateTimeInMsec(const BINKREALTIME *binkRealtime)
{
    const char *v1; // eax
    unsigned __int64 frameNum; // [esp+8h] [ebp-20h]
    unsigned __int64 frameRateDiv; // [esp+10h] [ebp-18h]
    unsigned __int64 timeInMsec; // [esp+18h] [ebp-10h]
    unsigned __int64 frameRate; // [esp+20h] [ebp-8h]

    frameNum = binkRealtime->FrameNum;
    if ((uint32_t)frameNum < 0x80000000)
    {
        frameRateDiv = binkRealtime->FrameRateDiv;
        frameRate = binkRealtime->FrameRate;
        timeInMsec = 1000 * frameNum * frameRateDiv / frameRate;
        cinematicGlob.timeInMsec = timeInMsec;
        if (timeInMsec != (uint32_t)timeInMsec)
        {
            v1 = va(
                "%08x:%08x, %08x:%08x, %08x:%08x, %08x:%08x",
                HIDWORD(timeInMsec),
                (uint32_t)timeInMsec,
                HIDWORD(frameNum),
                (uint32_t)frameNum,
                HIDWORD(frameRate),
                (uint32_t)frameRate,
                HIDWORD(frameRateDiv),
                (uint32_t)frameRateDiv);
            MyAssertHandler(".\\r_cinematic.cpp", 1063, 0, "%s\n\t%s", "cinematicGlob.timeInMsec == timeInMsec", v1);
        }
    }
    else
    {
        cinematicGlob.timeInMsec = 0;
    }
}

void R_Cinematic_StopPlayback_Now()
{
    cinematicGlob.activeImageFrame = -1;
    if (cinematicGlob.hasFileIO)
        R_Cinematic_RelinquishIO();
    BinkClose(cinematicGlob.bink);
    cinematicGlob.bink = 0;
    CinematicHunk_Reset(&cinematicGlob.binkHunk);
}

void __cdecl CinematicHunk_Reset(CinematicHunk *hunk)
{
    iassert( hunk->base );
    iassert( hunk->atFront );
    iassert( hunk->atBack );
    iassert( hunk->end );
    hunk->atFront = hunk->base;
    hunk->atBack = hunk->end;
}

char __cdecl R_Cinematic_StartPlayback_Now(const char *filename, uint32_t playbackFlags)
{
    _DIG_DRIVER *Driver; // eax
    uint32_t TrackIDsToPlay[5]; // [esp+1Ch] [ebp-9Ch] BYREF
    char errText[132]; // [esp+30h] [ebp-88h] BYREF

    TrackIDsToPlay[0] = 0;
    TrackIDsToPlay[1] = 1;
    TrackIDsToPlay[2] = 2;
    TrackIDsToPlay[3] = 3;
    TrackIDsToPlay[4] = 4;
    if ((playbackFlags & 4) == 0 || cinematicGlob.atHighPriority)
    {
        if ((playbackFlags & 4) == 0 && cinematicGlob.atHighPriority)
        {
            KISAK_NULLSUB();
            cinematicGlob.atHighPriority = 0;
        }
    }
    else
    {
        KISAK_NULLSUB();
        cinematicGlob.atHighPriority = 1;
    }
    iassert( !cinematicGlob.bink );
    cinematicGlob.activeImageFrame = -1;
    cinematicGlob.activeTextureSet ^= 1u;
    if (R_Cinematic_AreHunksOpen())
        R_Cinematic_HunksReset(cinematicGlob.activeTextureSet, playbackFlags);
    else
        R_Cinematic_HunksOpen(cinematicGlob.activeTextureSet, playbackFlags);
    iassert( CinematicHunk_IsEmpty( &cinematicGlob.binkHunk ) );
    R_Cinematic_CheckBinkError();
    BinkSetMemory(R_Cinematic_Bink_Alloc, R_Cinematic_Bink_Free);
    R_Cinematic_CheckBinkError();
#ifndef KISAK_OPENAL
    Driver = MSS_GetDriver();
    BinkSetSoundSystem(BinkOpenMiles, (uint32_t)Driver);
    R_Cinematic_CheckBinkError();
#else
    // KISAK_OPENAL TODO: Bink has no OpenAL sound-system adapter.
    // Cinematics currently play without synced audio under OpenAL.
#endif
    BinkSetSoundTrack(5, TrackIDsToPlay);
    R_Cinematic_CheckBinkError();
    cinematicGlob.timeInMsec = 0;
    errText[0] = 0;
    if (R_Cinematic_BinkOpen(filename, playbackFlags, errText, 0x80u)
        || (Com_PrintWarning(8, "R_Cinematic_BinkOpen '%s' failed: %s; trying default.\n", filename, errText),
            CinematicHunk_Reset(&cinematicGlob.binkHunk),
            errText[0] = 0,
            R_Cinematic_BinkOpen("default", playbackFlags, errText, 0x80u)))
    {
        R_Cinematic_CheckBinkError();
        iassert( cinematicGlob.bink );
        R_Cinematic_InitBinkVolumes();
        memset(&cinematicGlob.binkTextureSet, 0, sizeof(cinematicGlob.binkTextureSet));
        BinkGetFrameBuffersInfo(cinematicGlob.bink, &cinematicGlob.binkTextureSet.bink_buffers);
        R_Cinematic_CheckBinkError();
        R_Cinematic_InitBinkTextures();
        BinkRegisterFrameBuffers(cinematicGlob.bink, &cinematicGlob.binkTextureSet.bink_buffers);
        R_Cinematic_CheckBinkError();
        cinematicGlob.currentPaused = CINEMATIC_NOT_PAUSED;
        return 1;
    }
    else
    {
        Com_PrintWarning(8, "R_Cinematic_BinkOpen '%s' failed: %s; not playing movie.\n", "default", errText);

        iassert(cinematicGlob.activeImageFrame == CINEMATIC_INVALID_IMAGE_FRAME);

        CinematicHunk_Reset(&cinematicGlob.binkHunk);
        cinematicGlob.cinematicFinished = 1;
        RB_UnbindAllImages();
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = rgp.blackImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = rgp.grayImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = rgp.grayImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.blackImage;
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] );
        return 0;
    }
}

bool __cdecl CinematicHunk_IsEmpty(CinematicHunk *hunk)
{
    iassert( hunk->base );
    iassert( hunk->atFront );
    iassert( hunk->atBack );
    iassert( hunk->end );
    return hunk->atFront == hunk->base && hunk->atBack == hunk->end;
}

void __cdecl R_Cinematic_HunksOpen(int activeTexture, char playbackFlags)
{
    CinematicHunk_Open(&cinematicGlob.masterHunk, (char *)cinematicGlob.memPool, 0xD00000);
    R_Cinematic_HunksAllocate(activeTexture, playbackFlags);
}

void __cdecl CinematicHunk_Open(CinematicHunk *hunk, char *memory, int size)
{
    iassert( !hunk->base );
    iassert( !hunk->atFront );
    iassert( !hunk->atBack );
    iassert( !hunk->end );
    hunk->base = memory;
    hunk->atFront = memory;
    hunk->end = &memory[size];
    hunk->atBack = hunk->end;
}

void __cdecl R_Cinematic_HunksAllocate(int activeTexture, char playbackFlags)
{
    char *residentBufferBase; // [esp+8h] [ebp-14h]
    int newResidentBufferSize; // [esp+Ch] [ebp-10h]
    int newBinkBufferSize; // [esp+10h] [ebp-Ch]
    char *binkBufferBase; // [esp+14h] [ebp-8h]

    newBinkBufferSize = 0x89AC00;
    newResidentBufferSize = 0;
    if ((playbackFlags & 8) != 0 && (playbackFlags & 0x20) == 0)
    {
        newResidentBufferSize = 0x71AC00;
        newBinkBufferSize = 0x180000;
    }
    binkBufferBase = (char*)CinematicHunk_Alloc(&cinematicGlob.masterHunk, 0x435800);
    residentBufferBase = (char*)CinematicHunk_Alloc(&cinematicGlob.masterHunk, newResidentBufferSize);
    CinematicHunk_Open(&cinematicGlob.binkHunk, binkBufferBase, newBinkBufferSize);
    CinematicHunk_Open(&cinematicGlob.residentHunk, residentBufferBase, newResidentBufferSize);
}

void __cdecl R_Cinematic_HunksReset(int activeTexture, char playbackFlags)
{
    CinematicHunk_Close(&cinematicGlob.binkHunk);
    CinematicHunk_Close(&cinematicGlob.residentHunk);
    CinematicHunk_Reset(&cinematicGlob.masterHunk);
    R_Cinematic_HunksAllocate(activeTexture, playbackFlags);
}

static void __cdecl R_Cinematic_ReleaseImages(CinematicTextureSet *textureSet)
{
    int frameIter; // [esp+0h] [ebp-4h]

    for (frameIter = 0; frameIter != 2; ++frameIter)
    {
        Image_Release(&textureSet->imageY[frameIter]);
        Image_Release(&textureSet->imageCr[frameIter]);
        Image_Release(&textureSet->imageCb[frameIter]);
        Image_Release(&textureSet->imageA[frameIter]);
    }
    Image_Release(&textureSet->drawImageY);
    Image_Release(&textureSet->drawImageCr);
    Image_Release(&textureSet->drawImageCb);
    Image_Release(&textureSet->drawImageA);
}

IDirect3DTexture9 *__cdecl R_Cinematic_MakeBinkTexture_PC(
    GfxImage *image,
    uint32_t width,
    uint32_t height,
    int baseImageFlags)
{
    Image_Setup(image, width, height, 1, baseImageFlags | 3, D3DFMT_L8);
    return image->texture.map;
}

void R_Cinematic_MakeBinkDrawTextures()
{
    bool useAlpha; // [esp+Bh] [ebp-9h]
    int frameIter; // [esp+Ch] [ebp-8h]
    CinematicTextureSet *textureSet; // [esp+10h] [ebp-4h]

    useAlpha = 0;
    for (frameIter = 0; frameIter != 2; ++frameIter)
    {
        if (cinematicGlob.binkTextureSet.bink_buffers.Frames[frameIter].APlane.Allocate)
            useAlpha = 1;
    }
    textureSet = &cinematicGlob.textureSets[cinematicGlob.activeTextureSet];
    cinematicGlob.binkTextureSet.tex_draw.Ytexture = R_Cinematic_MakeBinkTexture_PC(
        &textureSet->drawImageY,
        cinematicGlob.binkTextureSet.bink_buffers.YABufferWidth,
        cinematicGlob.binkTextureSet.bink_buffers.YABufferHeight,
        0x10000);
    cinematicGlob.binkTextureSet.tex_draw.cRtexture = R_Cinematic_MakeBinkTexture_PC(
        &textureSet->drawImageCr,
        cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferWidth,
        cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferHeight,
        0x10000);
    cinematicGlob.binkTextureSet.tex_draw.cBtexture = R_Cinematic_MakeBinkTexture_PC(
        &textureSet->drawImageCb,
        cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferWidth,
        cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferHeight,
        0x10000);

    if (useAlpha)
    {
        cinematicGlob.binkTextureSet.tex_draw.Atexture = R_Cinematic_MakeBinkTexture_PC(
            &textureSet->drawImageA,
            cinematicGlob.binkTextureSet.bink_buffers.YABufferWidth,
            cinematicGlob.binkTextureSet.bink_buffers.YABufferHeight,
            0x10000);
    }
    else
    {
        cinematicGlob.binkTextureSet.tex_draw.Atexture = 0;
    }
}

void R_Cinematic_InitBinkTextures()
{
    BINKFRAMETEXTURES *textures; // [esp+40h] [ebp-10h]
    int frameIter; // [esp+48h] [ebp-8h]
    CinematicTextureSet *textureSet; // [esp+4Ch] [ebp-4h]

    g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_NEED_UNBIND_ALL_IMAGES;
    Sys_ResetCinematicsHostOutstandingRequestEvent();
    Sys_SetCinematicsThreadOutstandingRequestEvent();
    R_CinematicThread_WaitForHostEvent();
    textureSet = &cinematicGlob.textureSets[cinematicGlob.activeTextureSet];
    R_Cinematic_ReleaseImages(textureSet);
    if (cinematicGlob.binkTextureSet.bink_buffers.TotalFrames != 2)
        MyAssertHandler(
            ".\\r_cinematic.cpp",
            867,
            0,
            "%s",
            "cinematicGlob.binkTextureSet.bink_buffers.TotalFrames == CINEMATIC_IMAGES_REQUIRED");
    for (frameIter = 0; frameIter != 2; ++frameIter)
    {
        textures = &cinematicGlob.binkTextureSet.textures[frameIter];
        textures->Ytexture = R_Cinematic_MakeBinkTexture_PC(
            &textureSet->imageY[frameIter],
            cinematicGlob.binkTextureSet.bink_buffers.YABufferWidth,
            cinematicGlob.binkTextureSet.bink_buffers.YABufferHeight,
            0x40000);
        textures->cRtexture = R_Cinematic_MakeBinkTexture_PC(
            &textureSet->imageCr[frameIter],
            cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferWidth,
            cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferHeight,
            0x40000);
        textures->cBtexture = R_Cinematic_MakeBinkTexture_PC(
            &textureSet->imageCb[frameIter],
            cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferWidth,
            cinematicGlob.binkTextureSet.bink_buffers.cRcBBufferHeight,
            0x40000);
        if (cinematicGlob.binkTextureSet.bink_buffers.Frames[frameIter].APlane.Allocate)
            textures->Atexture = R_Cinematic_MakeBinkTexture_PC(
                &textureSet->imageA[frameIter],
                cinematicGlob.binkTextureSet.bink_buffers.YABufferWidth,
                cinematicGlob.binkTextureSet.bink_buffers.YABufferHeight,
                0x40000);
        else
            textures->Atexture = 0;
    }
    R_Cinematic_MakeBinkDrawTextures();
}

void* __stdcall R_Cinematic_Bink_Alloc(uint32_t bytes)
{
    return (void*)CinematicHunk_Alloc(&cinematicGlob.binkHunk, bytes);
}

void __stdcall R_Cinematic_Bink_Free(void *ptr)
{
    ;
}

bool __cdecl R_Cinematic_BinkOpen(
    const char *filename,
    uint32_t playbackFlags,
    char *errText,
    uint32_t errTextSize)
{
    char *cwd; // [esp+4h] [ebp-20Ch]
    char filepath[2][256]; // [esp+8h] [ebp-208h] BYREF

    cwd = Sys_Cwd();
    iassert( cwd );
    filepath[1][0] = 0;
    if ((playbackFlags & 0x20) != 0)
    {
        if ((playbackFlags & 8) == 0)
            MyAssertHandler(
                ".\\r_cinematic.cpp",
                1507,
                0,
                "%s\n\t(playbackFlags) = %i",
                "(playbackFlags & 0x00000008)",
                playbackFlags);
        _snprintf(filepath[0], 0x100u, "video/%s.%s", filename, "bik");
    }
    else
    {
        _snprintf(filepath[0], 0x100u, "%s\\main\\video\\%s.%s", cwd, filename, "bik");
        _snprintf(filepath[1], 0x100u, "%s\\raw\\video\\%s.%s", cwd, filename, "bik");
    }
    if (R_Cinematic_BinkOpenPath(filepath[0], playbackFlags, errText, errTextSize))
        return 1;
    return filepath[1][0] && R_Cinematic_BinkOpenPath(filepath[1], playbackFlags, errText, errTextSize);
}

char __cdecl R_Cinematic_BinkOpenPath(
    const char *filepath,
    char playbackFlags,
    char *errText,
    uint32_t errTextSize)
{
    const char *Error; // eax
    RawFile *rawfile; // [esp+5Ch] [ebp-Ch]
    const void *filledBuffer; // [esp+60h] [ebp-8h] BYREF
    uint32_t flags; // [esp+64h] [ebp-4h]

    if ((playbackFlags & 8) != 0)
    {
        if ((cinematicGlob.playbackFlags & 1) == 0)
        {
            g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_ENTERING_BINK;
            Sys_ResetCinematicsHostOutstandingRequestEvent();
            Sys_SetCinematicsThreadOutstandingRequestEvent();
            R_CinematicThread_WaitForHostEvent();
        }
        if ((playbackFlags & 0x20) != 0)
        {
            rawfile = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, filepath).rawfile;
            if (!rawfile)
            {
                if (errText)
                    _snprintf(errText, errTextSize, "Couldn't find rawfile '%s' in db", filepath);
                return 0;
            }
            filledBuffer = rawfile->buffer;
        }
        else if (!R_Cinematic_BinkOpenPath_MemoryResident(filepath, &filledBuffer, errText, errTextSize))
        {
            return 0;
        }
        flags = (BINKFROMMEMORY | BINKALPHA | BINKSNDTRACK | 0x400);
        iassert(flags == 0x4104400); // lwss add
        cinematicGlob.bink = (BINK *)BinkOpen((const char*)filledBuffer, flags);
        if (!cinematicGlob.bink)
            CinematicHunk_Reset(&cinematicGlob.residentHunk);
        if ((cinematicGlob.playbackFlags & 1) == 0)
        {
            g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_EXITED_BINK;
            Sys_ResetCinematicsHostOutstandingRequestEvent();
            Sys_SetCinematicsThreadOutstandingRequestEvent();
            R_CinematicThread_WaitForHostEvent();
        }
    }
    else
    {
        if ((cinematicGlob.playbackFlags & 1) == 0)
        {
            g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_ENTERING_BINK;
            Sys_ResetCinematicsHostOutstandingRequestEvent();
            Sys_SetCinematicsThreadOutstandingRequestEvent();
            R_CinematicThread_WaitForHostEvent();
        }
        if (CinematicHunk_GetFreeSpace(&cinematicGlob.binkHunk) <= 1572864)
            MyAssertHandler(
                ".\\r_cinematic.cpp",
                1440,
                0,
                "%s",
                "CinematicHunk_GetFreeSpace( &cinematicGlob.binkHunk ) > BINK_MISC_BUFFER_SIZE");
        cinematicGlob.binkIOSize = CinematicHunk_GetFreeSpace(&cinematicGlob.binkHunk) - 1572864;
        BinkSetIOSize(cinematicGlob.binkIOSize);
        flags = (BINKIOSIZE | BINKALPHA | BINKSNDTRACK | 0x400);
        iassert(flags == 0x1104400); // lwss add
        cinematicGlob.bink = (BINK *)BinkOpen(filepath, flags);
        R_CinematicThread_EndBinkAsync();
    }
    if (cinematicGlob.bink)
        return 1;
    if (errText)
    {
        Error = (const char *)BinkGetError();
        _snprintf(errText, errTextSize, "BinkOpen: %s", Error);
    }
    return 0;
}

int __cdecl CinematicHunk_GetFreeSpace(CinematicHunk *hunk)
{
    iassert( hunk->base );
    iassert( hunk->atFront );
    iassert( hunk->atBack );
    iassert( hunk->end );
    return (char *)hunk->atBack - (char *)hunk->atFront;
}

bool R_CinematicThread_EndBinkAsync()
{
    bool result; // eax

    result = cinematicGlob.playbackFlags & 1;
    if ((cinematicGlob.playbackFlags & 1) == 0)
    {
        g_cinematicThreadState = CINEMATIC_THREAD_STATE_TO_HOST_EXITED_BINK;
        Sys_ResetCinematicsHostOutstandingRequestEvent();
        Sys_SetCinematicsThreadOutstandingRequestEvent();
        return R_CinematicThread_WaitForHostEvent();
    }
    return result;
}

char __cdecl R_Cinematic_BinkOpenPath_MemoryResident(
    const char *filename,
    const void **outPtr,
    char *errText,
    uint32_t errTextSize)
{
    void *allocedBuffer; // [esp+Ch] [ebp-18h]
    FILE *fileHandle; // [esp+10h] [ebp-14h]
    int fileSize; // [esp+14h] [ebp-10h]
    int numberOfBytesRead; // [esp+1Ch] [ebp-8h]
    int freeBufferSpace; // [esp+20h] [ebp-4h]

    fileHandle = fopen(filename, "rb");
    if (!fileHandle)
    {
        if (errText)
            _snprintf(errText, errTextSize, "Open failed");
        return 0;
    }
    fileSize = FileWrapper_GetFileSize(fileHandle);
    if (fileSize <= 0)
    {
        if (errText)
        {
            if (fileSize)
                _snprintf(errText, errTextSize, "GetFileSize failed (%i)", fileSize);
            else
                _snprintf(errText, errTextSize, "Zero file size");
        }
    LABEL_14:
        fclose(fileHandle);
        return 0;
    }
    freeBufferSpace = CinematicHunk_GetFreeSpace(&cinematicGlob.residentHunk);
    if (fileSize > freeBufferSpace)
    {
        if (errText)
            _snprintf(errText, errTextSize, "Cinematic too big (%i > %i)", fileSize, freeBufferSpace);
        goto LABEL_14;
    }
    allocedBuffer = (void *)CinematicHunk_Alloc(&cinematicGlob.residentHunk, fileSize);
    *outPtr = allocedBuffer;
    numberOfBytesRead = fread(allocedBuffer, 1u, fileSize, fileHandle);
    fclose(fileHandle);
    if (numberOfBytesRead == fileSize)
        return 1;
    if (errText)
        _snprintf(errText, errTextSize, "Read was short (wanted %i got %i)", fileSize, numberOfBytesRead);
    CinematicHunk_Reset(&cinematicGlob.residentHunk);
    return 0;
}

bool R_CinematicThread_WaitForHostEvent()
{
    bool result; // eax

    while (1)
    {
        result = Sys_WaitForCinematicsHostOutstandingRequestEventTimeout(0x10u);
        if (result)
            break;
        if (cinematicGlob.hasFileIO && R_Cinematic_GetPercentageFull() > 0x5F)
            R_Cinematic_RelinquishIO();
    }
    return result;
}

void __cdecl R_Cinematic_Shutdown()
{
    int setIter; // [esp+0h] [ebp-4h]

    R_Cinematic_StopPlayback();
    R_Cinematic_SyncNow();
    for (setIter = 0; setIter != 2; ++setIter)
        R_Cinematic_ReleaseImages(&cinematicGlob.textureSets[setIter]);
    Z_Free(cinematicGlob.memPool, 18);
}

void __cdecl R_Cinematic_StartPlayback(char *name, uint32_t playbackFlags, float volume)
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    R_Cinematic_StartPlayback_Internal(name, playbackFlags, volume);
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void __cdecl R_Cinematic_StartPlayback_Internal(char *name, uint32_t playbackFlags, float volume)
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
    I_strncpyz(cinematicGlob.targetCinematicName, name, 256);
    cinematicGlob.targetCinematicChanged = 1;
    cinematicGlob.cinematicFinished = 0;
    cinematicGlob.targetPaused = CINEMATIC_NOT_PAUSED;
    cinematicGlob.playbackFlags = playbackFlags;
    cinematicGlob.playbackVolume = volume;
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
}

void __cdecl R_Cinematic_StartNextPlayback()
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    if (R_Cinematic_IsNextReady_Internal())
    {
        R_Cinematic_StartPlayback_Internal(
            cinematicGlob.nextCinematicName,
            cinematicGlob.nextCinematicPlaybackFlags,
            cinematicGlob.playbackVolume);
        cinematicGlob.nextCinematicName[0] = 0;
    }
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

bool __cdecl R_Cinematic_IsNextReady_Internal()
{
    return cinematicGlob.nextCinematicName[0] != 0;
}

void __cdecl R_Cinematic_StopPlayback()
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
    cinematicGlob.targetCinematicName[0] = 0;
    cinematicGlob.targetCinematicChanged = 1;
    cinematicGlob.cinematicFinished = 0;
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC_TARGET_CHANGE);
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void __cdecl R_Cinematic_UpdateFrame()
{
    bool v0; // [esp+0h] [ebp-44h]

    PROF_SCOPED("R_Cinematic_UpdateFrame");

    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    v0 = !cinematicGlob.fullSyncNextUpdate && (cinematicGlob.playbackFlags & 1) == 0;
    cinematicGlob.fullSyncNextUpdate = 0;
    if (g_cinematicThreadState == CINEMATIC_THREAD_STATE_FROM_HOST_GO)
        MyAssertHandler(
            ".\\r_cinematic.cpp",
            1984,
            0,
            "%s",
            "g_cinematicThreadState != CINEMATIC_THREAD_STATE_FROM_HOST_GO");
    if (R_Cinematic_ThreadFinish(v0))
    {
        g_cinematicThreadState = CINEMATIC_THREAD_STATE_FROM_HOST_GO;
        Sys_ResetCinematicsThreadOutstandingRequestEvent();
        Sys_SetCinematicsHostOutstandingRequestEvent();
        R_Cinematic_ThreadFinish(v0);
    }
    R_Cinematic_UpdateRendererImages();
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void R_Cinematic_UpdateRendererImages()
{
    if (cinematicGlob.activeImageFrame == -1)
    {
        RB_UnbindAllImages();
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = rgp.blackImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = rgp.grayImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = rgp.grayImage;
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.blackImage;
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] );
    }
    else
    {
        R_Cinematic_SetRendererImagesToFrame(cinematicGlob.activeImageFrame);
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] );
        iassert( gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] );
    }
}

void __cdecl R_Cinematic_SetRendererImagesToFrame(int frameToSetTo)
{
    CinematicTextureSet *textureSet; // [esp+0h] [ebp-4h]

    textureSet = &cinematicGlob.textureSets[cinematicGlob.activeImageFrameTextureSet];
    iassert( frameToSetTo != CINEMATIC_INVALID_IMAGE_FRAME );
    RB_UnbindAllImages();
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = &textureSet->drawImageY;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = &textureSet->drawImageCr;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = &textureSet->drawImageCb;
    if (textureSet->drawImageA.texture.basemap)
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = &textureSet->drawImageA;
    else
        gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.whiteImage;
}

char __cdecl R_Cinematic_ThreadFinish(bool midBinkIsOkay)
{
    do
    {
        while (2)
        {
            if (Sys_WaitForCinematicsThreadOutstandingRequestEventTimeout(1))
            {
                switch (g_cinematicThreadState)
                {
                case CINEMATIC_THREAD_STATE_TO_HOST_BETWEEN_UPDATES:
                    return 1;
                case CINEMATIC_THREAD_STATE_TO_HOST_ENTERING_BINK:
                    g_cinematicThreadState = CINEMATIC_THREAD_STATE_FROM_HOST_GO_BINK;
                    goto LABEL_14;
                case CINEMATIC_THREAD_STATE_TO_HOST_EXITED_BINK:
                    g_cinematicThreadState = CINEMATIC_THREAD_STATE_FROM_HOST_GO;
                    goto LABEL_14;
                case CINEMATIC_THREAD_STATE_TO_HOST_NEED_UNBIND_ALL_IMAGES:
                    RB_UnbindAllImages();
                    g_cinematicThreadState = CINEMATIC_THREAD_STATE_FROM_HOST_GO;
                LABEL_14:
                    Sys_ResetCinematicsThreadOutstandingRequestEvent();
                    Sys_SetCinematicsHostOutstandingRequestEvent();
                    continue;
                default:
                    if (!alwaysfails)
                        MyAssertHandler(".\\r_cinematic.cpp", 1861, 0, "R_Cinematic_ThreadFinish: Can't happen.");
                    return 1;
                }
            }
            break;
        }
    } while (!midBinkIsOkay || g_cinematicThreadState != CINEMATIC_THREAD_STATE_FROM_HOST_GO_BINK);
    return 0;
}

void __cdecl R_Cinematic_SyncNow()
{
    cinematicGlob.fullSyncNextUpdate = 1;
    R_Cinematic_UpdateFrame();
}

void __cdecl R_Cinematic_DrawLetterbox_OptionalCinematic(bool drawCinematic, float letterboxAlpha)
{
    float rectY; // [esp+3Ch] [ebp-2Ch]
    float width; // [esp+44h] [ebp-24h]
    float height; // [esp+48h] [ebp-20h]
    float letterboxHalfHeight; // [esp+4Ch] [ebp-1Ch]
    float color[4]; // [esp+50h] [ebp-18h] BYREF
    float movieHeight; // [esp+60h] [ebp-8h]
    float aspectRatio; // [esp+64h] [ebp-4h]

    width = (float)vidConfig.displayWidth;
    height = (float)vidConfig.displayHeight;
    aspectRatio = vidConfig.aspectRatioDisplayPixel;
    movieHeight = width * vidConfig.aspectRatioDisplayPixel / (float)1.7777778;
    if (height < (double)movieHeight)
        movieHeight = (float)vidConfig.displayHeight;
    letterboxHalfHeight = (height - movieHeight) * 0.5;
    color[0] = 0.0;
    color[1] = 0.0;
    color[2] = 0.0;
    color[3] = letterboxAlpha;
    R_AddCmdDrawStretchPic(0.0, 0.0, width, letterboxHalfHeight, 0.0, 0.0, 1.0, 1.0, color, rgp.whiteMaterial);
    rectY = height - letterboxHalfHeight;
    R_AddCmdDrawStretchPic(0.0, rectY, width, letterboxHalfHeight, 0.0, 0.0, 1.0, 1.0, color, rgp.whiteMaterial);
    if (drawCinematic)
        R_AddCmdDrawStretchPic(
            0.0,
            letterboxHalfHeight,
            width,
            movieHeight,
            0.0,
            0.0,
            1.0,
            1.0,
            colorWhite,
            rgp.cinematicMaterial);
}

void __cdecl R_Cinematic_DrawStretchPic_Letterboxed()
{
    R_Cinematic_DrawLetterbox_OptionalCinematic(1, 1.0);
}

bool __cdecl R_Cinematic_IsFinished()
{
    return cinematicGlob.cinematicFinished;
}

bool __cdecl R_Cinematic_IsStarted()
{
    return !R_Cinematic_IsFinished() && cinematicGlob.currentCinematicName[0];
}

bool __cdecl R_Cinematic_IsNextReady()
{
    return R_Cinematic_IsNextReady_Internal();
}

bool __cdecl R_Cinematic_IsUnderrun()
{
    return cinematicGlob.underrun;
}

void __cdecl R_Cinematic_BeginLostDevice()
{
    uint32_t setIter; // [esp+4h] [ebp-8h]
    CinematicTextureSet *textureSet; // [esp+8h] [ebp-4h]

    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    if (cinematicGlob.hasFileIO)
        R_Cinematic_RelinquishIO();
    if (R_Cinematic_IsStarted() && cinematicGlob.currentPaused == CINEMATIC_NOT_PAUSED)
    {
        cinematicGlob.currentPaused = CINEMATIC_PAUSED;
        BinkPause(cinematicGlob.bink, 1);
    }
    for (setIter = 0; setIter != 2; ++setIter)
    {
        textureSet = &cinematicGlob.textureSets[setIter];
        Image_Release(&textureSet->drawImageY);
        Image_Release(&textureSet->drawImageCr);
        Image_Release(&textureSet->drawImageCb);
        Image_Release(&textureSet->drawImageA);
    }
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void __cdecl R_Cinematic_ClearTexture(IDirect3DTexture9 *texture, int width, int height, uint8_t clearValue)
{
    const char *v4; // eax
    HRESULT hr; // [esp+0h] [ebp-Ch]
    _D3DLOCKED_RECT lockedRect; // [esp+4h] [ebp-8h] BYREF

    hr = texture->LockRect(0, &lockedRect, 0, 0x2000u);
    if (hr >= 0)
    {
        iassert( lockedRect.Pitch >= width );
        memset((uint8_t *)lockedRect.pBits, clearValue, lockedRect.Pitch * height);
        texture->UnlockRect(0);
    }
    else
    {
        v4 = DXGetErrorDescription9A(hr);
        Com_PrintError(8, "LockRect failed with error 0x%08x - %s", hr, v4);
    }
}

void R_Cinematic_ClearBinkDrawTextures()
{
    BINKFRAMETEXTURES *textures = &cinematicGlob.binkTextureSet.tex_draw;
    BINKFRAMEBUFFERS *buffers = &cinematicGlob.binkTextureSet.bink_buffers;

    iassert( textures->Ytexture );
    iassert( textures->cRtexture );
    iassert( textures->cBtexture );

    R_Cinematic_ClearTexture(
        textures->Ytexture,
        buffers->YABufferWidth,
        buffers->YABufferHeight,
        0);
    R_Cinematic_ClearTexture(
        textures->cRtexture,
        buffers->cRcBBufferWidth,
        buffers->cRcBBufferHeight,
        0x80u);
    R_Cinematic_ClearTexture(
        textures->cBtexture,
        buffers->cRcBBufferWidth,
        buffers->cRcBBufferHeight,
        0x80u);
    if (textures->Atexture)
        R_Cinematic_ClearTexture(
            textures->Atexture,
            buffers->YABufferWidth,
            buffers->YABufferHeight,
            0);
}

void __cdecl R_Cinematic_EndLostDevice()
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    if (cinematicGlob.bink)
    {
        R_Cinematic_MakeBinkDrawTextures();
        R_Cinematic_ClearBinkDrawTextures();
    }
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void __cdecl R_Cinematic_SetPaused(CinematicEnum paused) 
{
    iassert(paused == CINEMATIC_PAUSED || paused == CINEMATIC_NOT_PAUSED);
    cinematicGlob.targetPaused = paused;
}

void R_Cinematic_SetNextPlayback(const char *name, uint32_t playbackFlags)
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    I_strncpyz(cinematicGlob.nextCinematicName, name, 256);
    cinematicGlob.nextCinematicPlaybackFlags = playbackFlags;
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

void R_Cinematic_UnsetNextPlayback()
{
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    Sys_EnterCriticalSection(CRITSECT_CINEMATIC);
    I_strncpyz(cinematicGlob.nextCinematicName, "", 256);
    cinematicGlob.nextCinematicPlaybackFlags = 0;
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
    Sys_LeaveCriticalSection(CRITSECT_CINEMATIC);
}

bool R_Cinematic_IsPending()
{
    return cinematicGlob.targetCinematicName[0] != 0;
}

#else
#include <psp2/videodec.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/dmac.h>
#include <vita/gxm/gxm_image.h>
#include <vita/platform/vita_system.h>
#include <malloc.h>

// the hardware decoders feed the bink code images; the cinematic materials are unchanged
static bool s_started;
static bool s_finished;
static unsigned s_startMs;
static uint32_t s_frameSeen;
static char s_next[64];
static uint32_t s_nextFlags;
static bool s_hasNext;
static float s_volume = 1.0f;

static GfxImage s_planeImage[3];        // Y, Cb, Cr
static GxmImage *s_plane[3];
static uint32_t s_planeW, s_planeH;
static GfxImage s_alphaImage;           // constant white: the movies carry no alpha plane
static GxmImage *s_alphaPlane;

static bool s_moduleLoaded;
static SceUID s_workerThread = -1;
static int s_audioPort = -1;
static volatile bool s_workerRun;
static volatile bool s_workerDone;

// one flat table per track, resolved from the mp4's chunk maps at open
struct CinSample
{
    uint32_t offset;
    uint32_t size;
    uint32_t ptsMs;
};
struct CinTrack
{
    CinSample *samples;
    uint32_t count;
    uint32_t next;
};

static FILE *s_mp4;
static CinTrack s_vid;
static CinTrack s_aud;
static uint8_t *s_spsPps;
static uint32_t s_spsPpsLen;
static uint32_t s_nalLenSize = 4;
static uint32_t s_vidW, s_vidH;
static uint32_t s_aacRate, s_aacCh;

static uint8_t *s_esBuf;
static uint32_t s_esBufSize;
static SceAvcdecCtrl s_avcCtrl;
static bool s_avcLive;
static SceUID s_avcFrameUid = -1;
static void *s_avcFrameBase;
static SceUID s_avcOutUid = -1;
static void *s_avcOutBase;

// the decoder wants raw physically contiguous blocks it maps itself, free of gxm
static void *Cin_PhycontAlloc(SceUID *uid, uint32_t size)
{
    size = (size + 0xFFFFFu) & ~0xFFFFFu;
    *uid = sceKernelAllocMemBlock("kcod_avc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                  size, NULL);
    if (*uid < 0)
        return NULL;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0)
    {
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return NULL;
    }
    return base;
}

static void Cin_PhycontFree(SceUID *uid, void **base)
{
    if (*uid >= 0)
        sceKernelFreeMemBlock(*uid);
    *uid = -1;
    *base = NULL;
}

// textures spill into phycont, so what the decoder needs is taken once and kept
static uint32_t s_avcFrameSize;
static uint32_t s_avcOutSize;

static bool Cin_HoldPhycont(SceUID *uid, void **base, uint32_t *held, uint32_t size)
{
    if (*base && *held >= size)
        return true;
    Cin_PhycontFree(uid, base);
    *base = Cin_PhycontAlloc(uid, size);
    *held = *base ? ((size + 0xFFFFFu) & ~0xFFFFFu) : 0;
    return *base != NULL;
}
static uint32_t s_avcPitch, s_avcRows;
static SceAudiodecCtrl s_aacCtrl;
static SceAudiodecInfo s_aacInfo;
static bool s_aacLive;
static uint8_t *s_aacPcm;

// the worker stages a converted frame here; the render thread uploads it to the planes
static uint8_t *s_stage[3];
static uint8_t *s_bounce;
static uint32_t s_stageW, s_stageH;
static volatile uint32_t s_stageSerial;
static uint32_t s_shownSerial;

// ---- mp4 box parsing: big endian, boxes are {u32 size, u32 type} ----

static uint32_t Cin_BE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// finds a child box inside [start,end); returns payload span or NULL
static const uint8_t *Cin_FindBox(const uint8_t *start, const uint8_t *end, const char *type,
                                  const uint8_t **boxEnd)
{
    const uint8_t *p = start;
    while (p + 8 <= end)
    {
        const uint32_t size = Cin_BE32(p);
        if (size < 8 || p + size > end)
            break;
        if (!memcmp(p + 4, type, 4))
        {
            *boxEnd = p + size;
            return p + 8;
        }
        p += size;
    }
    return NULL;
}

struct CinStbl
{
    const uint8_t *stsd, *stsdEnd;
    const uint8_t *stts, *sttsEnd;
    const uint8_t *stsz, *stszEnd;
    const uint8_t *stsc, *stscEnd;
    const uint8_t *stco, *stcoEnd;
    uint32_t timescale;
};

// expands the chunk maps into one flat {offset,size,pts} table
static bool Cin_BuildTrack(const CinStbl *st, CinTrack *out)
{
    const uint32_t sampleCount = Cin_BE32(st->stsz + 8);
    if (!sampleCount || sampleCount > 200000)
        return false;
    const uint32_t fixedSize = Cin_BE32(st->stsz + 4);

    out->samples = (CinSample *)malloc(sampleCount * sizeof(CinSample));
    if (!out->samples)
        return false;
    out->count = sampleCount;
    out->next = 0;

    for (uint32_t i = 0; i < sampleCount; ++i)
        out->samples[i].size = fixedSize ? fixedSize : Cin_BE32(st->stsz + 12 + i * 4);

    // stts runs to per-sample times
    const uint32_t sttsCount = Cin_BE32(st->stts + 4);
    uint64_t tick = 0;
    uint32_t sample = 0;
    for (uint32_t e = 0; e < sttsCount && sample < sampleCount; ++e)
    {
        const uint32_t n = Cin_BE32(st->stts + 8 + e * 8);
        const uint32_t delta = Cin_BE32(st->stts + 12 + e * 8);
        for (uint32_t i = 0; i < n && sample < sampleCount; ++i, ++sample)
        {
            out->samples[sample].ptsMs = (uint32_t)(tick * 1000u / st->timescale);
            tick += delta;
        }
    }

    // stsc entries applied across stco chunks
    const uint32_t stscCount = Cin_BE32(st->stsc + 4);
    const uint32_t chunkCount = Cin_BE32(st->stco + 4);
    sample = 0;
    for (uint32_t e = 0; e < stscCount && sample < sampleCount; ++e)
    {
        const uint32_t firstChunk = Cin_BE32(st->stsc + 8 + e * 12);
        const uint32_t perChunk = Cin_BE32(st->stsc + 12 + e * 12);
        const uint32_t lastChunk = (e + 1 < stscCount)
            ? Cin_BE32(st->stsc + 8 + (e + 1) * 12) - 1 : chunkCount;
        for (uint32_t c = firstChunk; c <= lastChunk && sample < sampleCount; ++c)
        {
            uint32_t offset = Cin_BE32(st->stco + 8 + (c - 1) * 4);
            for (uint32_t i = 0; i < perChunk && sample < sampleCount; ++i, ++sample)
            {
                out->samples[sample].offset = offset;
                offset += out->samples[sample].size;
            }
        }
    }
    return true;
}

// pulls sps/pps out of the avc1 box's avcC and stores them with start codes
static bool Cin_ParseAvcC(const uint8_t *p, const uint8_t *end)
{
    if (p + 8 > end)
        return false;
    s_nalLenSize = (uint32_t)(p[4] & 3) + 1;
    uint8_t blob[256];
    uint32_t at = 0;
    const uint8_t *q = p + 5;
    const uint32_t spsCount = *q++ & 0x1F;
    for (uint32_t i = 0; i < spsCount && q + 2 <= end; ++i)
    {
        const uint32_t len = ((uint32_t)q[0] << 8) | q[1];
        q += 2;
        if (q + len > end || at + len + 4 > sizeof(blob))
            return false;
        blob[at++] = 0; blob[at++] = 0; blob[at++] = 0; blob[at++] = 1;
        memcpy(blob + at, q, len);
        at += len; q += len;
    }
    if (q >= end)
        return false;
    const uint32_t ppsCount = *q++;
    for (uint32_t i = 0; i < ppsCount && q + 2 <= end; ++i)
    {
        const uint32_t len = ((uint32_t)q[0] << 8) | q[1];
        q += 2;
        if (q + len > end || at + len + 4 > sizeof(blob))
            return false;
        blob[at++] = 0; blob[at++] = 0; blob[at++] = 0; blob[at++] = 1;
        memcpy(blob + at, q, len);
        at += len; q += len;
    }
    s_spsPps = (uint8_t *)malloc(at);
    if (!s_spsPps)
        return false;
    memcpy(s_spsPps, blob, at);
    s_spsPpsLen = at;
    return true;
}

static bool Cin_OpenMp4(const char *path)
{
    s_mp4 = fopen(path, "rb");
    if (!s_mp4)
        return false;

    // moov is loaded whole; mdat stays on disk
    uint8_t head[16];
    uint8_t *moov = NULL;
    uint32_t moovSize = 0;
    long at = 0;
    while (fseek(s_mp4, at, SEEK_SET) == 0 && fread(head, 1, 8, s_mp4) == 8)
    {
        const uint32_t size = Cin_BE32(head);
        if (size < 8)
            break;
        if (!memcmp(head + 4, "moov", 4))
        {
            moovSize = size - 8;
            moov = (uint8_t *)malloc(moovSize);
            if (!moov || fread(moov, 1, moovSize, s_mp4) != moovSize)
            {
                free(moov);
                return false;
            }
            break;
        }
        at += size;
    }
    if (!moov)
        return false;

    const uint8_t *end = moov + moovSize;
    const uint8_t *trak = moov, *trakEnd;
    bool okVideo = false;
    while ((trak = Cin_FindBox(trak, end, "trak", &trakEnd)) != NULL)
    {
        const uint8_t *e1, *e2, *e3;
        const uint8_t *mdia = Cin_FindBox(trak, trakEnd, "mdia", &e1);
        if (!mdia) { trak = trakEnd; continue; }
        const uint8_t *hdlr = Cin_FindBox(mdia, e1, "hdlr", &e2);
        const uint8_t *mdhd = Cin_FindBox(mdia, e1, "mdhd", &e2);
        const uint8_t *minf = Cin_FindBox(mdia, e1, "minf", &e2);
        if (!hdlr || !mdhd || !minf) { trak = trakEnd; continue; }
        const uint8_t *stbl = Cin_FindBox(minf, e2, "stbl", &e3);
        if (!stbl) { trak = trakEnd; continue; }

        CinStbl st;
        memset(&st, 0, sizeof(st));
        st.timescale = Cin_BE32(mdhd + 12);
        if (!st.timescale)
            st.timescale = 1000;
        st.stsd = Cin_FindBox(stbl, e3, "stsd", &st.stsdEnd);
        st.stts = Cin_FindBox(stbl, e3, "stts", &st.sttsEnd);
        st.stsz = Cin_FindBox(stbl, e3, "stsz", &st.stszEnd);
        st.stsc = Cin_FindBox(stbl, e3, "stsc", &st.stscEnd);
        st.stco = Cin_FindBox(stbl, e3, "stco", &st.stcoEnd);
        if (!st.stsd || !st.stts || !st.stsz || !st.stsc || !st.stco)
        {
            trak = trakEnd;
            continue;
        }

        if (!memcmp(hdlr + 8, "vide", 4) && !s_vid.samples)
        {
            const uint8_t *se;
            const uint8_t *avc1 = Cin_FindBox(st.stsd + 8, st.stsdEnd, "avc1", &se);
            if (avc1)
            {
                s_vidW = ((uint32_t)avc1[24] << 8) | avc1[25];
                s_vidH = ((uint32_t)avc1[26] << 8) | avc1[27];
                const uint8_t *ae;
                const uint8_t *avcC = Cin_FindBox(avc1 + 78, se, "avcC", &ae);
                if (avcC && Cin_ParseAvcC(avcC, ae) && Cin_BuildTrack(&st, &s_vid))
                    okVideo = true;
            }
        }
        else if (!memcmp(hdlr + 8, "soun", 4) && !s_aud.samples)
        {
            const uint8_t *se;
            const uint8_t *mp4a = Cin_FindBox(st.stsd + 8, st.stsdEnd, "mp4a", &se);
            if (mp4a)
            {
                s_aacCh = ((uint32_t)mp4a[16] << 8) | mp4a[17];
                s_aacRate = (((uint32_t)mp4a[24] << 8) | mp4a[25]);
                if (Cin_BuildTrack(&st, &s_aud))
                {
                    if (!s_aacCh || s_aacCh > 2)
                        s_aacCh = 2;
                    if (!s_aacRate)
                        s_aacRate = 44100;
                }
            }
        }
        trak = trakEnd;
    }
    free(moov);
    return okVideo;
}

// ---- hardware decoders ----

static bool Cin_OpenAvc(void)
{
    const uint32_t alignW = (s_vidW + 15) & ~15u;
    const uint32_t alignH = (s_vidH + 15) & ~15u;

    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = alignW;
    init.vertical = alignH;
    init.numOfRefFrames = 3;
    init.numOfStreams = 1;
    int rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (rc < 0)
    {
        VitaSys_LogPrintf("cinematic: videodec init 0x%08x\n", (unsigned)rc);
        return false;
    }

    SceAvcdecQueryDecoderInfo query;
    query.horizontal = alignW;
    query.vertical = alignH;
    query.numOfRefFrames = 3;
    SceAvcdecDecoderInfo info;
    memset(&info, 0, sizeof(info));
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
    if (rc >= 0)
        Cin_HoldPhycont(&s_avcFrameUid, &s_avcFrameBase, &s_avcFrameSize, info.frameMemSize);
    if (rc < 0 || !s_avcFrameBase)
    {
        VitaSys_LogPrintf("cinematic: decoder memory 0x%08x (%u KB)\n", (unsigned)rc,
                          info.frameMemSize / 1024);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        return false;
    }

    memset(&s_avcCtrl, 0, sizeof(s_avcCtrl));
    s_avcCtrl.frameBuf.pBuf = s_avcFrameBase;
    s_avcCtrl.frameBuf.size = s_avcFrameSize;
    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &s_avcCtrl, &query);
    if (rc < 0)
    {
        VitaSys_LogPrintf("cinematic: create decoder 0x%08x\n", (unsigned)rc);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        return false;
    }

    s_avcPitch = alignW;
    s_avcRows = alignH;
    if (!Cin_HoldPhycont(&s_avcOutUid, &s_avcOutBase, &s_avcOutSize,
                         alignW * alignH * 3 / 2))
    {
        VitaSys_LogPrintf("cinematic: no output frame memory\n");
        sceAvcdecDeleteDecoder(&s_avcCtrl);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        return false;
    }
    s_avcLive = true;
    return true;
}

static bool Cin_OpenAac(void)
{
    if (!s_aud.samples)
        return false;

    SceAudiodecInitParam initParam;
    memset(&initParam, 0, sizeof(initParam));
    initParam.size = sizeof(initParam.aac);
    initParam.aac.totalStreams = 1;
    int rc = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &initParam);
    if (rc < 0)
    {
        VitaSys_LogPrintf("cinematic: audiodec init 0x%08x\n", (unsigned)rc);
        return false;
    }

    memset(&s_aacInfo, 0, sizeof(s_aacInfo));
    s_aacInfo.aac.size = sizeof(s_aacInfo.aac);
    s_aacInfo.aac.isAdts = 0;
    s_aacInfo.aac.ch = s_aacCh;
    s_aacInfo.aac.samplingRate = s_aacRate;
    s_aacInfo.aac.isSbr = 0;

    s_aacPcm = (uint8_t *)memalign(64, 2048 * 2 * 2);
    memset(&s_aacCtrl, 0, sizeof(s_aacCtrl));
    s_aacCtrl.size = sizeof(s_aacCtrl);
    s_aacCtrl.wordLength = 16;
    s_aacCtrl.pInfo = &s_aacInfo;
    s_aacCtrl.maxPcmSize = 2048 * 2 * 2;
    s_aacCtrl.pPcm = s_aacPcm;

    rc = s_aacPcm ? sceAudiodecCreateDecoder(&s_aacCtrl, SCE_AUDIODEC_TYPE_AAC) : -1;
    if (rc < 0)
    {
        VitaSys_LogPrintf("cinematic: aac decoder 0x%08x\n", (unsigned)rc);
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
        free(s_aacPcm);
        s_aacPcm = NULL;
        return false;
    }
    s_aacLive = true;
    return true;
}

// reads one sample and rewrites its length prefixes as start codes, sps/pps first
static uint32_t Cin_LoadVideoSample(const CinSample *sample)
{
    uint32_t at = 0;
    memcpy(s_esBuf, s_spsPps, s_spsPpsLen);
    at = s_spsPpsLen;

    if (fseek(s_mp4, (long)sample->offset, SEEK_SET) != 0 ||
        fread(s_esBuf + at, 1, sample->size, s_mp4) != sample->size)
        return 0;

    uint8_t *p = s_esBuf + at;
    const uint8_t *end = p + sample->size;
    while (p + s_nalLenSize < end)
    {
        uint32_t len = 0;
        for (uint32_t i = 0; i < s_nalLenSize; ++i)
            len = (len << 8) | p[i];
        if (s_nalLenSize == 4)
        {
            p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
        }
        else
        {
            // shorter prefixes cannot become a 4-byte start code in place
            return 0;
        }
        p += s_nalLenSize + len;
    }
    return at + sample->size;
}

static void Cin_StageDecodedFrame(const SceAvcdecPicture *picture)
{
    const uint32_t width = picture->frame.horizontalSize;
    const uint32_t height = picture->frame.verticalSize;
    if (!width || !height)
        return;

    if (s_stageW != width || s_stageH != height)
    {
        for (int i = 0; i < 3; ++i)
        {
            free(s_stage[i]);
            s_stage[i] = NULL;
        }
        s_stage[0] = (uint8_t *)malloc(width * height);
        s_stage[1] = (uint8_t *)malloc((width / 2) * (height / 2));
        s_stage[2] = (uint8_t *)malloc((width / 2) * (height / 2));
        if (!s_stage[0] || !s_stage[1] || !s_stage[2])
            return;
        s_stageW = width;
        s_stageH = height;
    }

    // the output block is uncached, so the bulk moves by dma before the cpu touches rows
    const uint32_t lumaBytes = s_avcPitch * s_avcRows;
    if (!s_bounce)
        s_bounce = (uint8_t *)memalign(64, lumaBytes * 3 / 2);
    if (!s_bounce)
        return;
    if (sceDmacMemcpy(s_bounce, s_avcOutBase, lumaBytes * 3 / 2) < 0)
        memcpy(s_bounce, s_avcOutBase, lumaBytes * 3 / 2);

    for (uint32_t row = 0; row < height; ++row)
        memcpy(s_stage[0] + row * width, s_bounce + row * s_avcPitch, width);

    // raster output carries planar chroma: a cb plane then a cr plane at half pitch
    const uint8_t *cbPlane = s_bounce + lumaBytes;
    const uint32_t chromaPitch = s_avcPitch / 2;
    const uint8_t *crPlane = cbPlane + chromaPitch * (s_avcRows / 2);
    const uint32_t chromaW = width / 2, chromaH = height / 2;
    for (uint32_t row = 0; row < chromaH; ++row)
    {
        memcpy(s_stage[1] + row * chromaW, cbPlane + row * chromaPitch, chromaW);
        memcpy(s_stage[2] + row * chromaW, crPlane + row * chromaPitch, chromaW);
    }
    ++s_stageSerial;
}

static int Cin_WorkerThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    const unsigned begin = VitaSys_Milliseconds();
    uint32_t audioGrain = 0;
    uint32_t decodeErrors = 0;

    while (s_workerRun && ((s_avcLive && s_vid.next < s_vid.count) ||
                           (s_aacLive && s_aud.next < s_aud.count)))
    {
        const uint32_t vidBefore = s_vid.next;
        const uint32_t audBefore = s_aud.next;

        // one block per pass: its output blocks in real time, so draining it starves video
        if (s_aacLive && s_aud.next < s_aud.count &&
            s_aud.samples[s_aud.next].ptsMs <= VitaSys_Milliseconds() - begin + 30)
        {
            const CinSample *sample = &s_aud.samples[s_aud.next++];
            if (sample->size <= s_esBufSize &&
                fseek(s_mp4, (long)sample->offset, SEEK_SET) == 0 &&
                fread(s_esBuf, 1, sample->size, s_mp4) == sample->size)
            {
                s_aacCtrl.pEs = s_esBuf;
                s_aacCtrl.maxEsSize = sample->size;
                s_aacCtrl.inputEsSize = sample->size;
                if (sceAudiodecDecode(&s_aacCtrl) >= 0 &&
                    s_aacCtrl.outputPcmSize)
                {
                    const uint32_t frames = s_aacCtrl.outputPcmSize / (2 * s_aacCh);
                    if (s_audioPort < 0 && frames)
                    {
                        s_audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                                          (int)frames, (int)s_aacRate,
                                                          s_aacCh == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                                                       : SCE_AUDIO_OUT_MODE_STEREO);
                        audioGrain = frames;
                        if (s_audioPort >= 0)
                        {
                            int volume[2];
                            volume[0] = volume[1] = (int)(s_volume * SCE_AUDIO_VOLUME_0DB);
                            sceAudioOutSetVolume(s_audioPort,
                                                 (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                                                          SCE_AUDIO_VOLUME_FLAG_R_CH),
                                                 volume);
                        }
                    }
                    if (s_audioPort >= 0 && frames == audioGrain)
                        sceAudioOutOutput(s_audioPort, s_aacPcm);
                }
            }
            /* fall through to video */;
        }

        for (int burst = 0; burst < 2 && s_avcLive && s_vid.next < s_vid.count &&
             s_vid.samples[s_vid.next].ptsMs <= VitaSys_Milliseconds() - begin; ++burst)
        {
            const CinSample *sample = &s_vid.samples[s_vid.next++];
            const uint32_t esLen = (sample->size + s_spsPpsLen <= s_esBufSize)
                ? Cin_LoadVideoSample(sample) : 0;
            if (esLen)
            {
                SceAvcdecAu au;
                memset(&au, 0, sizeof(au));
                au.pts.upper = 0xFFFFFFFFu;
                au.pts.lower = 0xFFFFFFFFu;
                au.dts.upper = 0xFFFFFFFFu;
                au.dts.lower = 0xFFFFFFFFu;
                au.es.pBuf = s_esBuf;
                au.es.size = esLen;

                SceAvcdecPicture picture;
                memset(&picture, 0, sizeof(picture));
                picture.size = sizeof(picture);
                picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER;
                picture.frame.framePitch = s_avcPitch;
                picture.frame.frameWidth = s_avcPitch;
                picture.frame.frameHeight = s_avcRows;
                picture.frame.pPicture[0] = s_avcOutBase;
                picture.frame.pPicture[1] = (uint8_t *)s_avcOutBase + s_avcPitch * s_avcRows;

                SceAvcdecPicture *pictures[1] = { &picture };
                SceAvcdecArrayPicture array;
                memset(&array, 0, sizeof(array));
                array.numOfElm = 1;
                array.pPicture = pictures;

                const int rc = sceAvcdecDecode(&s_avcCtrl, &au, &array);
                if (rc < 0)
                {
                    if (decodeErrors++ < 3)
                        VitaSys_LogPrintf("cinematic: decode 0x%08x at sample %u\n",
                                          (unsigned)rc, s_vid.next - 1);
                }
                else if (array.numOfOutput && sample->ptsMs + 120 >= VitaSys_Milliseconds() - begin)
                {
                    Cin_StageDecodedFrame(&picture);
                }
            }
            /* next burst */;
        }

        if (s_vid.next == vidBefore && s_aud.next == audBefore)
            sceKernelDelayThread(2000);
    }

    s_workerDone = true;
    return 0;
}

static void Cin_FillImage(GfxImage *image, GxmImage *gxm, uint32_t width, uint32_t height,
                          const char *name)
{
    memset(image, 0, sizeof(*image));
    image->mapType = MAPTYPE_2D;
    image->texture.basemap = (IDirect3DBaseTexture9 *)gxm;
    image->width = (uint16_t)width;
    image->height = (uint16_t)height;
    image->depth = 1;
    image->name = name;
}

static bool Cin_CreatePlanes(uint32_t width, uint32_t height)
{
    static const char *names[3] = { "$cinematicY", "$cinematicCb", "$cinematicCr" };
    for (int i = 0; i < 3; ++i)
    {
        if (s_plane[i])
            GxmImage_Release(s_plane[i]);
        const uint32_t w = i ? width / 2 : width;
        const uint32_t h = i ? height / 2 : height;
        s_plane[i] = GxmImage_Create2D(GXM_D3DFMT_L8, w, h, 1);
        if (!s_plane[i])
            return false;
        Cin_FillImage(&s_planeImage[i], s_plane[i], w, h, names[i]);

        // neutral yuv until the first frame
        void *bits; uint32_t pitch, slice;
        if (GxmImage_MapLevelWrite(s_plane[i], 0, 0, &bits, &pitch, &slice))
        {
            memset(bits, i ? 128 : 0, slice);
            GxmImage_UnmapLevelWrite(s_plane[i], 0, 0, bits);
        }
    }
    s_planeW = width;
    s_planeH = height;
    return true;
}

void __cdecl R_Cinematic_Init()
{
    // textures spill into phycont during a load, so the decoder takes its share first
    if (!Cin_HoldPhycont(&s_avcFrameUid, &s_avcFrameBase, &s_avcFrameSize, 7u * 1024u * 1024u) ||
        !Cin_HoldPhycont(&s_avcOutUid, &s_avcOutBase, &s_avcOutSize, 960u * 544u * 3u / 2u))
        VitaSys_LogPrintf("cinematic: could not reserve decoder memory at boot\n");

    if (!s_alphaPlane)
    {
        s_alphaPlane = GxmImage_Create2D(GXM_D3DFMT_L8, 1, 1, 1);
        if (s_alphaPlane)
        {
            void *bits; uint32_t pitch, slice;
            if (GxmImage_MapLevelWrite(s_alphaPlane, 0, 0, &bits, &pitch, &slice))
            {
                *(uint8_t *)bits = 255;
                GxmImage_UnmapLevelWrite(s_alphaPlane, 0, 0, bits);
            }
            Cin_FillImage(&s_alphaImage, s_alphaPlane, 1, 1, "$cinematicA");
        }
    }
}

void __cdecl R_Cinematic_StopPlayback()
{
    s_workerRun = false;
    if (s_workerThread >= 0)
    {
        sceKernelWaitThreadEnd(s_workerThread, NULL, NULL);
        sceKernelDeleteThread(s_workerThread);
        s_workerThread = -1;
    }
    if (s_audioPort >= 0)
    {
        sceAudioOutReleasePort(s_audioPort);
        s_audioPort = -1;
    }
    if (s_aacLive)
    {
        sceAudiodecDeleteDecoder(&s_aacCtrl);
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
        s_aacLive = false;
    }
    free(s_aacPcm);
    s_aacPcm = NULL;
    if (s_avcLive)
    {
        sceAvcdecDeleteDecoder(&s_avcCtrl);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        s_avcLive = false;
    }
    free(s_esBuf);
    s_esBuf = NULL;
    s_esBufSize = 0;
    free(s_spsPps);
    s_spsPps = NULL;
    s_spsPpsLen = 0;
    free(s_vid.samples);
    free(s_aud.samples);
    memset(&s_vid, 0, sizeof(s_vid));
    memset(&s_aud, 0, sizeof(s_aud));
    for (int i = 0; i < 3; ++i)
    {
        free(s_stage[i]);
        s_stage[i] = NULL;
    }
    free(s_bounce);
    s_bounce = NULL;
    s_stageW = s_stageH = 0;
    s_stageSerial = s_shownSerial = 0;
    if (s_mp4)
    {
        fclose(s_mp4);
        s_mp4 = NULL;
    }
    s_started = false;
    s_finished = false;
    s_workerDone = false;
}

void __cdecl R_Cinematic_StartPlayback(char *name, uint32_t playbackFlags, float volume)
{
    (void)playbackFlags;
    R_Cinematic_StopPlayback();

    char clean[64];
    I_strncpyz(clean, name ? name : "", sizeof(clean));
    char *dot = strrchr(clean, '.');
    if (dot)
        *dot = 0;
    char path[128];
    Com_sprintf(path, sizeof(path), "ux0:data/kisakcod/video/%s.mp4", clean);

    if (!s_moduleLoaded)
    {
        const int mv = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        const int ma = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
        VitaSys_LogPrintf("cinematic: modules avcdec 0x%08x audiocodec 0x%08x\n",
                          (unsigned)mv, (unsigned)ma);
        s_moduleLoaded = true;
    }

    if (!Cin_OpenMp4(path))
    {
        VitaSys_LogPrintf("cinematic: no usable video in %s\n", path);
        R_Cinematic_StopPlayback();
        s_started = true;
        s_finished = true;
        return;
    }

    // the es buffer holds the largest sample plus the parameter sets
    uint32_t maxSample = 0;
    for (uint32_t i = 0; i < s_vid.count; ++i)
        if (s_vid.samples[i].size > maxSample)
            maxSample = s_vid.samples[i].size;
    for (uint32_t i = 0; i < s_aud.count; ++i)
        if (s_aud.samples[i].size > maxSample)
            maxSample = s_aud.samples[i].size;
    s_esBufSize = maxSample + s_spsPpsLen + 64;
    s_esBuf = (uint8_t *)memalign(64, s_esBufSize);

    const bool haveVideo = s_esBuf && Cin_OpenAvc();
    const bool haveAudio = s_esBuf && Cin_OpenAac();
    if (!haveVideo && !haveAudio)
    {
        R_Cinematic_StopPlayback();
        s_started = true;
        s_finished = true;
        return;
    }

    if (!s_plane[0] && !Cin_CreatePlanes(16, 16))
    {
        R_Cinematic_StopPlayback();
        s_started = true;
        s_finished = true;
        return;
    }

    s_volume = volume;
    s_workerRun = true;
    s_workerDone = false;
    s_workerThread = sceKernelCreateThread("kcod_cinematic", Cin_WorkerThread, 160, 64 * 1024,
                                           0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (s_workerThread >= 0)
        sceKernelStartThread(s_workerThread, 0, NULL);

    VitaSys_LogPrintf("cinematic: playing %s (%ux%u, %u video + %u audio samples, video %s audio %s)\n",
                      path, s_vidW, s_vidH, s_vid.count, s_aud.count,
                      haveVideo ? "on" : "OFF", haveAudio ? "on" : "OFF");
    s_frameSeen = 0;
    s_startMs = VitaSys_Milliseconds();
    s_started = true;
    s_finished = false;
}

void __cdecl R_Cinematic_UpdateFrame()
{
    // the menus draw the cinematic material with nothing playing; black beats a fatal
    if (!s_plane[0] && !Cin_CreatePlanes(16, 16))
        return;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = &s_planeImage[0];
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = &s_planeImage[1];
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = &s_planeImage[2];
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.whiteImage;

    if (!s_started)
        return;

    const uint32_t serial = s_stageSerial;
    if (serial != s_shownSerial && s_stage[0])
    {
        s_shownSerial = serial;
        if ((s_stageW != s_planeW || s_stageH != s_planeH)
                ? Cin_CreatePlanes(s_stageW, s_stageH) : true)
        {
            if (!s_frameSeen++)
                VitaSys_LogPrintf("cinematic: first frame %ux%u\n", s_stageW, s_stageH);

            void *bits;
            uint32_t pitch, slice;
            if (GxmImage_MapLevelWrite(s_plane[0], 0, 0, &bits, &pitch, &slice))
            {
                for (uint32_t row = 0; row < s_stageH; ++row)
                    memcpy((uint8_t *)bits + row * pitch, s_stage[0] + row * s_stageW, s_stageW);
                GxmImage_UnmapLevelWrite(s_plane[0], 0, 0, bits);
            }
            const uint32_t chromaW = s_stageW / 2, chromaH = s_stageH / 2;
            for (int p = 1; p <= 2; ++p)
            {
                if (GxmImage_MapLevelWrite(s_plane[p], 0, 0, &bits, &pitch, &slice))
                {
                    for (uint32_t row = 0; row < chromaH; ++row)
                        memcpy((uint8_t *)bits + row * pitch, s_stage[p] + row * chromaW, chromaW);
                    GxmImage_UnmapLevelWrite(s_plane[p], 0, 0, bits);
                }
            }
        }
    }

    if (s_workerDone && !s_finished)
    {
        VitaSys_LogPrintf("cinematic: over after %u ms, %u frames shown\n",
                          VitaSys_Milliseconds() - s_startMs, s_frameSeen);
        s_finished = true;
        if (s_hasNext)
        {
            char next[64];
            I_strncpyz(next, s_next, sizeof(next));
            s_hasNext = false;
            R_Cinematic_StartPlayback(next, s_nextFlags, s_volume);
        }
    }
}

void __cdecl R_Cinematic_DrawStretchPic_Letterboxed()
{
    if (!s_started || s_finished || !rgp.cinematicMaterial)
        return;
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdDrawStretchPic(0.0f, 0.0f, 960.0f, 544.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                           white, rgp.cinematicMaterial);
}

void __cdecl R_Cinematic_Shutdown()
{
    R_Cinematic_StopPlayback();
    Cin_PhycontFree(&s_avcFrameUid, &s_avcFrameBase);
    Cin_PhycontFree(&s_avcOutUid, &s_avcOutBase);
    s_avcFrameSize = 0;
    s_avcOutSize = 0;
}
void __cdecl R_Cinematic_StartNextPlayback()
{
    if (s_hasNext)
    {
        char next[64];
        I_strncpyz(next, s_next, sizeof(next));
        s_hasNext = false;
        R_Cinematic_StartPlayback(next, s_nextFlags, s_volume);
    }
}
void __cdecl R_Cinematic_SyncNow() {}
bool __cdecl R_Cinematic_IsFinished() { return !s_started || s_finished; }
bool __cdecl R_Cinematic_IsStarted() { return s_started && !s_finished; }
bool R_Cinematic_IsPending() { return s_hasNext; }
bool __cdecl R_Cinematic_IsNextReady() { return true; }
bool __cdecl R_Cinematic_IsUnderrun() { return false; }
void __cdecl R_Cinematic_BeginLostDevice() {}
void __cdecl R_Cinematic_EndLostDevice() {}
void __cdecl R_Cinematic_SetPaused(CinematicEnum paused) { (void)paused; }
void R_Cinematic_SetNextPlayback(const char *name, uint32_t playbackFlags)
{
    I_strncpyz(s_next, name ? name : "", sizeof(s_next));
    s_nextFlags = playbackFlags;
    s_hasNext = s_next[0] != 0;
}
void R_Cinematic_UnsetNextPlayback() { s_hasNext = false; }
#endif