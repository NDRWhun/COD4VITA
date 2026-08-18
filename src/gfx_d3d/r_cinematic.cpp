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
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vita/gxm/gxm_image.h>
#include <vita/platform/vita_system.h>
#include <malloc.h>

// sceAvPlayer feeds the bink code images; the cinematic materials are unchanged
static SceAvPlayerHandle s_player;
static bool s_playerLive;
static bool s_moduleLoaded;
static bool s_started;
static bool s_finished;
// the player reads inactive until it has demuxed enough to begin, which is not the end
static bool s_everActive;
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

static SceUID s_audioThread = -1;
static int s_audioPort = -1;
static volatile bool s_audioRun;

static uint32_t s_allocFailures;
static uint32_t s_allocBytes;

static void *Cin_Alloc(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    void *p = memalign(alignment ? alignment : 16, size);
    if (p)
        s_allocBytes += size;
    else
        ++s_allocFailures;
    return p;
}

static void Cin_Free(void *arg, void *ptr)
{
    (void)arg;
    free(ptr);
}

static int Cin_AudioThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint32_t grain = 0;
    while (s_audioRun)
    {
        SceAvPlayerFrameInfo frame;
        memset(&frame, 0, sizeof(frame));
        if (s_playerLive && sceAvPlayerIsActive(s_player) &&
            sceAvPlayerGetAudioData(s_player, &frame))
        {
            const uint32_t channels = frame.details.audio.channelCount ?
                                      frame.details.audio.channelCount : 2;
            const uint32_t samples = frame.details.audio.size / (2 * channels);
            if (s_audioPort < 0 && samples)
            {
                s_audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, samples,
                                                  frame.details.audio.sampleRate,
                                                  channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                                                : SCE_AUDIO_OUT_MODE_STEREO);
                grain = samples;
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
            if (s_audioPort >= 0 && samples == grain)
                sceAudioOutOutput(s_audioPort, frame.pData);
        }
        else
        {
            sceKernelDelayThread(4000);
        }
    }
    if (s_audioPort >= 0)
    {
        sceAudioOutReleasePort(s_audioPort);
        s_audioPort = -1;
    }
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
    s_audioRun = false;
    if (s_audioThread >= 0)
    {
        sceKernelWaitThreadEnd(s_audioThread, NULL, NULL);
        sceKernelDeleteThread(s_audioThread);
        s_audioThread = -1;
    }
    if (s_playerLive)
    {
        sceAvPlayerStop(s_player);
        sceAvPlayerClose(s_player);
        s_playerLive = false;
    }
    s_started = false;
    s_finished = false;
    s_everActive = false;
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
    FILE *probe = fopen(path, "rb");
    if (!probe)
    {
        VitaSys_LogPrintf("cinematic: no %s, skipping\n", path);
        s_started = true;
        s_finished = true;
        return;
    }
    fclose(probe);

    if (!s_moduleLoaded)
    {
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        s_moduleLoaded = true;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = Cin_Alloc;
    init.memoryReplacement.deallocate = Cin_Free;
    init.memoryReplacement.allocateTexture = Cin_Alloc;
    init.memoryReplacement.deallocateTexture = Cin_Free;
    // the load pegs every core at higher priority, and a starved demuxer gives up
    init.basePriority = 96;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = SCE_TRUE;

    // failure is zero or an 0x806A code
    s_player = sceAvPlayerInit(&init);
    if (!s_player || ((uint32_t)s_player & 0xFFFF0000u) == 0x806A0000u)
    {
        VitaSys_LogPrintf("cinematic: sceAvPlayerInit failed 0x%08x\n", (unsigned)s_player);
        s_started = true;
        s_finished = true;
        return;
    }
    s_playerLive = true;
    const int added = sceAvPlayerAddSource(s_player, path);

    SceKernelFreeMemorySizeInfo pools;
    memset(&pools, 0, sizeof(pools));
    pools.size = sizeof(pools);
    sceKernelGetFreeMemorySize(&pools);
    VitaSys_LogPrintf("cinematic: addsource 0x%08x, phycont %i KB user %i KB free\n",
                      (unsigned)added, pools.size_phycont / 1024, pools.size_user / 1024);

    s_volume = volume;
    s_audioRun = true;
    s_audioThread = sceKernelCreateThread("kcod_cinematic", Cin_AudioThread, 160, 32 * 1024,
                                          0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (s_audioThread >= 0)
        sceKernelStartThread(s_audioThread, 0, NULL);

    // the planes must exist before the first draw
    if (!s_plane[0] && !Cin_CreatePlanes(16, 16))
    {
        R_Cinematic_StopPlayback();
        s_started = true;
        s_finished = true;
        return;
    }

    VitaSys_LogPrintf("cinematic: playing %s\n", path);
    s_everActive = false;
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

    if (!s_playerLive || !sceAvPlayerIsActive(s_player))
    {
        // a player that never comes up must still end, or the loadscreen waits on it forever
        if (s_playerLive && (s_everActive || VitaSys_Milliseconds() - s_startMs > 5000))
        {
            VitaSys_LogPrintf("cinematic: over after %u ms, %u frames shown\n",
                              VitaSys_Milliseconds() - s_startMs, s_frameSeen);
            s_finished = true;
        }
        if (s_finished && s_hasNext)
        {
            char next[64];
            I_strncpyz(next, s_next, sizeof(next));
            s_hasNext = false;
            R_Cinematic_StartPlayback(next, s_nextFlags, s_volume);
        }
        return;
    }

    s_everActive = true;

    // one line every 2 s while live, so a silent decoder names itself
    static unsigned s_lastReport;
    const unsigned now = VitaSys_Milliseconds();
    if (now - s_lastReport > 2000)
    {
        s_lastReport = now;
        VitaSys_LogPrintf("cinematic: t=%lli ms, %u KB allocated, %u alloc failures\n",
                          (long long)sceAvPlayerCurrentTime(s_player),
                          s_allocBytes / 1024, s_allocFailures);
    }

    SceAvPlayerFrameInfo frame;
    memset(&frame, 0, sizeof(frame));
    if (sceAvPlayerGetVideoData(s_player, &frame) && frame.pData)
    {
        const uint32_t width = frame.details.video.width;
        const uint32_t height = frame.details.video.height;

        if (!s_frameSeen++)
            VitaSys_LogPrintf("cinematic: first frame %ux%u\n", width, height);
        if (width && height &&
            ((width != s_planeW || height != s_planeH) ? Cin_CreatePlanes(width, height) : true))
        {
            void *bits; uint32_t pitch, slice;
            if (GxmImage_MapLevelWrite(s_plane[0], 0, 0, &bits, &pitch, &slice))
            {
                uint8_t *dst = (uint8_t *)bits;
                const uint8_t *from = (const uint8_t *)frame.pData;
                for (uint32_t row = 0; row < height; ++row, dst += pitch, from += width)
                    memcpy(dst, from, width);
                GxmImage_UnmapLevelWrite(s_plane[0], 0, 0, bits);
            }

            // NV12: the chroma pairs unzip into the two planes
            const uint8_t *uv = (const uint8_t *)frame.pData + width * height;
            const uint32_t chromaW = width / 2, chromaH = height / 2;
            void *cbBits, *crBits;
            uint32_t p2, s2;
            if (GxmImage_MapLevelWrite(s_plane[1], 0, 0, &cbBits, &p2, &s2) &&
                GxmImage_MapLevelWrite(s_plane[2], 0, 0, &crBits, &p2, &s2))
            {
                uint8_t *cb = (uint8_t *)cbBits, *cr = (uint8_t *)crBits;
                for (uint32_t row = 0; row < chromaH; ++row, cb += p2, cr += p2)
                {
                    for (uint32_t x = 0; x < chromaW; ++x)
                    {
                        cb[x] = uv[x * 2];
                        cr[x] = uv[x * 2 + 1];
                    }
                    uv += width;
                }
                GxmImage_UnmapLevelWrite(s_plane[1], 0, 0, cbBits);
                GxmImage_UnmapLevelWrite(s_plane[2], 0, 0, crBits);
            }
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

void __cdecl R_Cinematic_Shutdown() { R_Cinematic_StopPlayback(); }
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