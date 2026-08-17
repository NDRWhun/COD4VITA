#ifdef KISAK_VITA
// Vita implementation of the snd_driver interface declared in snd_local.h, over the software
// mixer in snd_vita.cpp. Mirrors snd_driver_openal.cpp function-for-function.
#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include "snd_vita.h"

#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <vita/platform/vita_system.h>
#include <universal/com_sndalias.h>
#include <universal/profile.h>

#ifdef KISAK_SP
#include <cgame/cg_main.h>
#endif

#include <math.h>

VitaLocal vitaGlob;

const dvar_t *snd_khz;
const dvar_t *snd_outputConfiguration;

void __cdecl TRACK_snd_driver()
{
    track_static_alloc_internal(&vitaGlob, sizeof(vitaGlob), "vitaGlob", 13);
}

bool __cdecl SND_IsMultiChannel()
{
    return vitaGlob.isMultiChannel;
}

char __cdecl SND_InitDriver()
{
    snd_khz = Dvar_RegisterInt("snd_khz", 44, DvarLimits(11, 44), DVAR_ARCHIVE | DVAR_LATCH, "The game sound frequency.");
    snd_outputConfiguration = Dvar_RegisterEnum(
        "snd_outputConfiguration",
        snd_outputConfigurationStrings,
        0,
        DVAR_ARCHIVE | DVAR_LATCH,
        "Sound output configuration");

    if (MSS_Startup())
    {
        if (MSS_Init())
        {
            MSS_InitChannels();
            MSS_InitEq();
            return 1;
        }
        else
        {
            MSS_ShutdownCleanup();
            MSS_InitFailed();
            return 0;
        }
    }
    else
    {
        MSS_InitFailed();
        return 0;
    }
}

void __cdecl SND_ShutdownDriver()
{
    // no R_Cinematic_* call here: CINEMA is not built for this target
    MSS_ShutdownCleanup();
}

int __cdecl SND_GetDriverCPUPercentage()
{
    return VitaSnd_CpuPercent();
}

// Turns the listener-relative direction into the stereo pan the mixer applies. Positive is
// right, matching the axis flip Miles fed to AIL_set_sample_3D_position.
static void SND_ApplyVoicePosition(int index, const float *org)
{
    float delta[3];
    int listenerIndex = SND_GetListenerIndexNearestToOrigin(org);
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);

    const float length = Vec3Length(transformed);
    const float pan = (length > 0.001f) ? (-transformed[1] / length) : 0.0f;
    VitaSnd_VoiceSetPan(index, pan, g_snd.chaninfo[index].soundFileInfo.srcChannelCount != 2);
}

void __cdecl SND_Set3DPosition(int index, const float *org)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    SND_ApplyVoicePosition(index, org);
}

void __cdecl SND_Stop2DChannel(int index)
{
    iassert((index >= 0 && index < 0 + g_snd.max_2D_channels));

    VitaSnd_VoiceStop(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause2DChannel(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    VitaSnd_VoicePause(index);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause2DChannel(int index, int timeshift)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    if (!g_snd.chaninfo[index].startDelay)
        VitaSnd_VoicePlay(index);

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is2DChannelFree(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_Stop3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    VitaSnd_VoiceStop(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    VitaSnd_VoicePause(index);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause3DChannel(int index, int timeshift)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    if (!g_snd.chaninfo[index].startDelay)
        VitaSnd_VoicePlay(index);

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is3DChannelFree(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_StopStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    VitaSnd_StreamClose(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_PauseStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    VitaSnd_VoicePause(index);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_UnpauseStreamChannel(int index, int timeshift)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!g_snd.chaninfo[index].startDelay)
        VitaSnd_VoicePlay(index);

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_IsStreamChannelFree(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!VitaSnd_StreamIsOpen(index))
        return 1;

    if (g_snd.chaninfo[index].paused || g_snd.chaninfo[index].startDelay)
        return 0;

    return g_snd.chaninfo[index].alias0 == 0;
}

// Fills in what the mixer needs to read a loaded sound. Returns false for anything the mixer
// cannot decode, which the callers turn into "not played" rather than silence-with-a-handle.
static bool SND_StartLoadedSample(int index, const MssSoundCOD4 *sound)
{
    VitaSndSample sample;
    sample.data = sound->data;
    sample.dataLen = sound->info.data_len;
    sample.format = sound->info.format;
    sample.channels = sound->info.channels;
    sample.bits = sound->info.bits;
    sample.rate = sound->info.rate;
    sample.blockSize = sound->info.block_size;

    if (sound->info.format == 17)
    {
        sample.frameCount = sound->info.samples;
    }
    else
    {
        const uint32_t frameBytes = (uint32_t)sound->info.channels * (uint32_t)(sound->info.bits / 8);
        sample.frameCount = frameBytes ? (sound->info.data_len / frameBytes) : 0;
    }

    return VitaSnd_VoiceSetSample(index, &sample);
}

void __cdecl SND_ApplyChannelMap(int index, const snd_alias_t *alias, int srcChannelCount)
{
    // No speaker map to apply: the Vita's only output is 2-channel, so Com_GetSpeakerMap's
    // per-speaker levels have nowhere to go.
}

int __cdecl SND_StartAlias2DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    if (!SND_HasFreeVoice(entchannel))
        return -1;

    int index = SND_FindFree2DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;

    if (index < 0)
        return -1;

    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;
    if (!SND_StartLoadedSample(index, sound))
        return SND_SetPlaybackIdNotPlayed(index);

    MSS_ApplyEqFilter(index, entchannel);

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    int rate = SnapFloatToInt((float)sound->info.rate * pitch);
    VitaSnd_VoiceSetRate(index, rate);

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags)].volume;

    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    SND_ApplyChannelMap(index, startAliasInfo->alias0, sound->info.channels);
    VitaSnd_VoiceSetPan(index, 0.0f, false);
    SND_Set2DChannelVolume(index, realVolume);
    VitaSnd_VoiceSetLooping(index, (startAliasInfo->alias0->flags & 1) != 0);

    // Duration at the pitch-adjusted rate, matching Miles' AIL_sample_ms_position query after
    // the playback rate is set: a pitched-up sound reports a proportionally shorter duration.
    int total_msec = rate > 0 ? (int)((int64_t)sound->info.samples * 1000 / rate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    if (start_msec)
        VitaSnd_VoiceSeekFrame(index, (uint32_t)((int64_t)start_msec * rate / 1000));

    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        VitaSnd_VoicePlay(index);
    }

    total_msec += startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        total_msec = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, total_msec, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, total_msec);

    if (playbackId != -1)
        SND_AddVoice(entchannel);

    return playbackId;
}

int __cdecl SND_StartAlias3DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel))
        return -1;
    int index = SND_FindFree3DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;
    if (index < 0)
        return -1;
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;
    if (!SND_StartLoadedSample(index, sound))
        return SND_SetPlaybackIdNotPlayed(index);

    float distMin = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMin
        + startAliasInfo->alias1->distMin * startAliasInfo->lerp;
    float distMax = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMax
        + startAliasInfo->alias1->distMax * startAliasInfo->lerp;

    MSS_ApplyEqFilter(index, entchannel);

    const float *listener = g_snd.listeners[SND_GetListenerIndexNearestToOrigin(startAliasInfo->org)].orient.origin;
    float diff[3];
    Vec3Sub(listener, startAliasInfo->org, diff);
    float distance = Vec3Length(diff);
    float attenuation = SND_Attenuate(startAliasInfo->alias0->volumeFalloffCurve, distance, distMin, distMax);
    float realVolume = startAliasInfo->volume
        * attenuation
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    realVolume = realVolume * g_snd.volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    // distMin/distMax fed Miles' own 3D distance model; SND_Attenuate's curve above has
    // already produced the final gain, so there is nothing left to hand the mixer.
    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    int rate = SnapFloatToInt((float)sound->info.rate * pitch);
    VitaSnd_VoiceSetRate(index, rate);

    // srcChannelCount decides whether the pan applies, so it has to be known before the
    // position and volume go in - SND_SetSoundFileChannelInfo below only repeats it.
    g_snd.chaninfo[index].soundFileInfo.srcChannelCount = sound->info.channels;
    SND_Set3DChannelVolume(index, realVolume);
    SND_Set3DPosition(index, startAliasInfo->org);
    VitaSnd_VoiceSetLooping(index, (startAliasInfo->alias0->flags & 1) != 0);

    int total_msec = rate > 0 ? (int)((int64_t)sound->info.samples * 1000 / rate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    if (start_msec)
        VitaSnd_VoiceSeekFrame(index, (uint32_t)((int64_t)start_msec * rate / 1000));

    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        VitaSnd_VoicePlay(index);
    }
    int totalMsecForChan = total_msec + startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        totalMsecForChan = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, totalMsecForChan, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

void __cdecl SND_Set3DStreamPosition(int index, int listenerIndex, const float *org)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    float delta[3];
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);

    const float length = Vec3Length(transformed);
    const float pan = (length > 0.001f) ? (-transformed[1] / length) : 0.0f;
    VitaSnd_VoiceSetPan(index, pan, g_snd.chaninfo[index].soundFileInfo.srcChannelCount != 2);
}

float __cdecl SND_GetStream3DVolumeFallOff(int index, int listenerIndex)
{
    iassert(index >= ((0 + 8) + 32) && index < g_snd.max_stream_channels + ((0 + 8) + 32));

    const snd_alias_t *alias0 = g_snd.chaninfo[index].alias0;
    const snd_alias_t *alias1 = g_snd.chaninfo[index].alias1;
    if (!SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
        MyAssertHandler(
            ".\\win32\\snd_driver.cpp",
            585,
            0,
            "%s",
            "SND_IsAliasChannel3D( SNDALIASFLAGS_GET_CHANNEL( alias0->flags ) )");

    float diff[3];
    Vec3Sub(g_snd.listeners[listenerIndex].orient.origin, g_snd.chaninfo[index].org, diff);
    float dist = Vec3Length(diff);
    float lerp = g_snd.chaninfo[index].lerp;
    float mindist = (1.0f - lerp) * alias0->distMin + alias1->distMin * lerp;
    float maxdist = (1.0f - lerp) * alias0->distMax + alias1->distMax * lerp;
    return SND_Attenuate(alias0->volumeFalloffCurve, dist, mindist, maxdist);
}

int __cdecl SND_StartAliasStreamOnChannel(SndStartAliasInfo *startAliasInfo, int index)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_STREAMED);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_STREAMED);
    iassert((index >= ((0 + 8) + 32) && index < ((0 + 8) + 32) + g_snd.max_stream_channels));

    bool fsInitialized = FS_Initialized();
    iassert(fsInitialized);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    if (!SND_HasFreeVoice(entchannel))
        return -1;

    char filename[128];
    Com_GetSoundFileName(startAliasInfo->alias0, filename, 128);

    if (!startAliasInfo->alias0->soundFile->exists)
    {
        Com_DPrintf(
            9,
            "Tried to play streamed sound '%s' from alias '%s', but it was not found at load time.\n",
            filename,
            startAliasInfo->alias0->aliasName);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    char realname[256];
    Com_sprintf(realname, 0x100u, "sound/%s", filename);

    VitaSnd_StreamClose(index);

    size_t nameLen = strlen(filename);
    bool isMp3 = nameLen >= 4 && I_stricmp(filename + nameLen - 4, ".mp3") == 0;
    bool looping = (startAliasInfo->alias0->flags & 1) != 0;

    int srcChannelCount = 0;
    int baserate = 0;
    uint64_t totalFrames = 0;
    if (!VitaSnd_StreamOpen(index, realname, isMp3, looping, &srcChannelCount, &baserate, &totalFrames))
    {
        Com_PrintError(9, "Couldn't play stream '%s' from alias '%s'\n", realname, startAliasInfo->alias0->aliasName);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    MSS_ApplyEqFilter(index, entchannel);

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    int rate = SnapFloatToInt((float)baserate * pitch);
    VitaSnd_VoiceSetRate(index, rate);

    int total_msec = rate > 0 ? (int)((int64_t)totalFrames * 1000 / rate) : 0;

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    if (startAliasInfo->timeshift >= total_msec && total_msec != 0)
    {
        VitaSnd_StreamClose(index);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    if (total_msec == 0 && totalFrames == 0)
    {
        VitaSnd_StreamClose(index);
        Com_PrintError(1, "ERROR: Sound file '%s' is zero length, invalid\n", realname);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    if (start_msec > 0 && baserate > 0)
        VitaSnd_StreamSeekFrame(index, (uint64_t)start_msec * (uint64_t)baserate / 1000);

    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        VitaSnd_VoicePlay(index);
    }

    int totalMsecForChan = total_msec + startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        totalMsecForChan = 0;

    float *org = g_snd.chaninfo[index].org;
    *org = startAliasInfo->org[0];
    org[1] = startAliasInfo->org[1];
    org[2] = startAliasInfo->org[2];
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, srcChannelCount, baserate, totalMsecForChan, start_msec, SFLS_LOADED);

    if (SND_IsAliasChannel3D((g_snd.chaninfo[index].alias0->flags & 0x3F00) >> 8))
    {
        SND_GetCurrent3DPosition(g_snd.chaninfo[index].sndEnt, g_snd.chaninfo[index].offset, g_snd.chaninfo[index].org);
        int listenerIndex = SND_GetListenerIndexNearestToOrigin(g_snd.chaninfo[index].org);
        SND_Set3DStreamPosition(index, listenerIndex, g_snd.chaninfo[index].org);
        realVolume = SND_GetStream3DVolumeFallOff(index, listenerIndex) * realVolume;
    }
    else
    {
        VitaSnd_VoiceSetPan(index, 0.0f, false);
        SND_ApplyChannelMap(index, startAliasInfo->alias0, srcChannelCount);
    }
    SND_SetStreamChannelVolume(index, realVolume);

    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

void __cdecl SND_SetRoomtype(int roomtype)
{
    // Genuine no-op: the software mixer has no reverb bus. See PORTING notes / excised list.
}

void __cdecl SND_UpdateEqs()
{
    for (int channelIndex = 0; channelIndex < SND_MAX_CHANNELS; ++channelIndex)
        MSS_ApplyEqFilter(channelIndex, g_snd.chaninfo[channelIndex].entchannel);
}

void __cdecl SND_SetEqParams(
    uint32_t entchannel,
    int eqIndex,
    uint32_t band,
    SND_EQTYPE type,
    float gain,
    float freq,
    float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));

    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    vitaGlob.eq[eqIndex].params[band][entchannel].gain = gain;
    vitaGlob.eq[eqIndex].params[band][entchannel].freq = freq;
    vitaGlob.eq[eqIndex].params[band][entchannel].q = q;
    vitaGlob.eq[eqIndex].params[band][entchannel].type = type;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_SetEqType(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));

    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    vitaGlob.eq[eqIndex].params[band][entchannel].type = type;
}

void __cdecl SND_SetEqFreq(uint32_t entchannel, int eqIndex, uint32_t band, float freq)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));

    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    vitaGlob.eq[eqIndex].params[band][entchannel].freq = freq;
}

void __cdecl SND_SetEqGain(uint32_t entchannel, int eqIndex, uint32_t band, float gain)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));
    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    vitaGlob.eq[eqIndex].params[band][entchannel].gain = gain;
}

void __cdecl SND_SetEqQ(uint32_t entchannel, int eqIndex, uint32_t band, float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));

    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    vitaGlob.eq[eqIndex].params[band][entchannel].q = q;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_DisableEq(uint32_t entchannel, int eqIndex, uint32_t band)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(vitaGlob.eq));

    vitaGlob.eq[eqIndex].params[band][entchannel].enabled = 0;
}

void __cdecl SND_SaveEq(MemoryFile *memFile)
{
    for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (int band = 0; band < 3; ++band)
        {
            for (int entchannel = 0; entchannel < 64; ++entchannel)
                MemFile_WriteData(memFile, 20, &vitaGlob.eq[eqIndex].params[band][entchannel]);
        }
    }
}

void __cdecl SND_RestoreEq(MemoryFile *memFile)
{
    for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (int band = 0; band < 3; ++band)
        {
            for (int entchannel = 0; entchannel < 64; ++entchannel)
                MemFile_ReadData(memFile, 20, (uint8_t *)&vitaGlob.eq[eqIndex].params[band][entchannel]);
        }
    }
}

void __cdecl SND_PrintEqParams()
{
    Com_Printf(9, "Current EQ Settings\n---------------\n");
    for (int entchannel = 0; entchannel < g_snd.entchannel_count; ++entchannel)
    {
        snd_entchannel_info_t *channelName = SND_GetEntChannelName(entchannel);
        Com_Printf(9, "+ %s\n", channelName->name);
        for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
        {
            for (int band = 0; band < 3; ++band)
            {
                const SndEqParams *params = &vitaGlob.eq[eqIndex].params[band][entchannel];
                if (params->enabled)
                    Com_Printf(9, "\t%i %s %f Hz %f dB %f q\n", band, snd_eqTypeStrings[params->type], params->freq, params->gain, params->q);
            }
        }
    }
}

float __cdecl SND_Get2DChannelVolume(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return VitaSnd_VoiceGetVolume(index);
}

void __cdecl SND_Set2DChannelVolume(int index, float volume)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    VitaSnd_VoiceSetVolume(index, volume);
}

float __cdecl SND_Get3DChannelVolume(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return VitaSnd_VoiceGetVolume(index);
}

void __cdecl SND_Set3DChannelVolume(int index, float volume)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    VitaSnd_VoiceSetVolume(index, volume);
}

float __cdecl SND_GetStreamChannelVolume(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    return VitaSnd_VoiceGetVolume(index);
}

void __cdecl SND_SetStreamChannelVolume(int index, float volume)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    VitaSnd_VoiceSetVolume(index, volume);
}

int __cdecl SND_Get2DChannelPlaybackRate(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return VitaSnd_VoiceGetRate(index);
}

void __cdecl SND_Set2DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    VitaSnd_VoiceSetRate(index, rate);
}

int __cdecl SND_Get3DChannelPlaybackRate(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return VitaSnd_VoiceGetRate(index);
}

void __cdecl SND_Set3DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    VitaSnd_VoiceSetRate(index, rate);
}

int __cdecl SND_GetStreamChannelPlaybackRate(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    return VitaSnd_VoiceGetRate(index);
}

void __cdecl SND_SetStreamChannelPlaybackRate(int index, int rate)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    VitaSnd_VoiceSetRate(index, rate);
}

void __cdecl SND_Update2DChannelReverb(int index)
{
    // no reverb bus in the software mixer; the room type and wet level carry no signal here
}

void __cdecl SND_Update3DChannelReverb(int index)
{
}

void __cdecl SND_UpdateStreamChannelReverb(int index)
{
}

int __cdecl SND_Get2DChannelLength(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_Get3DChannelLength(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_GetStreamChannelLength(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    return g_snd.chaninfo[index].totalMsec;
}

static float SND_SampleChannelFraction(int index)
{
    const int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    const int totalMsec = g_snd.chaninfo[index].totalMsec;
    if (totalMsec <= 0 || baserate <= 0)
        return 0.0f;

    const float offsetMsec = (float)VitaSnd_VoicePositionFrames(index) * 1000.0f / (float)baserate;
    return offsetMsec / (float)totalMsec;
}

void __cdecl SND_Get2DChannelSaveInfo(int index, snd_save_2D_sample_t *info)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    info->fraction = SND_SampleChannelFraction(index);
    info->pitch = g_snd.chaninfo[index].pitch;

    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = VitaSnd_VoiceGetVolume(index) / g_snd.volume;
}

void __cdecl SND_Set2DChannelFromSaveInfo(int index, snd_save_2D_sample_t *info)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    SND_Set2DChannelVolume(index, info->volume * g_snd.volume);
}

void __cdecl SND_Get3DChannelSaveInfo(int index, snd_save_3D_sample_t *info)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    info->fraction = SND_SampleChannelFraction(index);
    info->pitch = g_snd.chaninfo[index].pitch;

    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = VitaSnd_VoiceGetVolume(index) / g_snd.volume;

    info->org[0] = g_snd.chaninfo[index].org[0];
    info->org[1] = g_snd.chaninfo[index].org[1];
    info->org[2] = g_snd.chaninfo[index].org[2];
}

void __cdecl SND_GetStreamChannelSaveInfo(int index, snd_save_stream_t *info)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    const int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    const int totalMsec = g_snd.chaninfo[index].totalMsec;
    if (totalMsec > 0 && baserate > 0)
    {
        const uint64_t totalFrames = (uint64_t)totalMsec * baserate / 1000;
        info->fraction = totalFrames ? (float)VitaSnd_StreamPlayedFrames(index) / (float)totalFrames : 0.0f;
    }
    else
    {
        info->fraction = 0.0f;
    }

    int rate = VitaSnd_VoiceGetRate(index);
    if (g_snd.chaninfo[index].timescale)
        rate = SnapFloatToInt((float)rate / g_snd.timescale);
    info->rate = rate;

    info->basevolume = g_snd.chaninfo[index].basevolume;
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = VitaSnd_VoiceGetVolume(index) / g_snd.volume;

    float *org = g_snd.chaninfo[index].org;
    info->org[0] = org[0];
    info->org[1] = org[1];
    info->org[2] = org[2];
}

void __cdecl SND_SetStreamChannelFromSaveInfo(int index, snd_save_stream_t *info)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    SND_SetStreamChannelVolume(index, info->volume * g_snd.volume);
}

int __cdecl SND_GetSoundFileSize(uint32_t *pSoundFile)
{
    iassert(pSoundFile);

    return pSoundFile[2];
}

void __cdecl SND_DriverPostUpdate()
{
#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
    KISAK_NULLSUB();
}

void __cdecl SND_Update2DChannel(int i, int frametime)
{
    iassert(i >= 0 && i < 0 + g_snd.max_2D_channels);

    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    if (chaninfo->paused)
        return;

    const snd_alias_t *alias0 = chaninfo->alias0;
    const snd_alias_t *alias1 = chaninfo->alias1;
    iassert(alias0);
    iassert(alias1);

    bool finishedOrChaining = (!chaninfo->startDelay && VitaSnd_VoiceFinished(i))
        || (alias0->chainAliasName && chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0);
    if (finishedOrChaining)
    {
        SND_StopChannelAndPlayChainAlias(i);
        return;
    }

    float volume = chaninfo->basevolume;
    if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (alias0->flags & 4) != 0)
        volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;
    iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
    volume = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
    SND_Set2DChannelVolume(i, volume * g_snd.volume);
    MSS_ResumeSample(i, frametime);
}

void __cdecl SND_Update3DChannel(int i, int frametime)
{
    iassert(i >= (0 + 8) && i < (0 + 8) + g_snd.max_3D_channels);

    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    if (chaninfo->paused)
        return;

    const snd_alias_t *alias0 = chaninfo->alias0;
    const snd_alias_t *alias1 = chaninfo->alias1;
    iassert(alias0);
    iassert(alias1);

    const float lerp = chaninfo->lerp;
    const int timeleft = chaninfo->totalMsec + chaninfo->startTime - g_snd.time;
    bool finishedOrChaining = (!chaninfo->startDelay && VitaSnd_VoiceFinished(i))
        || (alias0->chainAliasName && timeleft <= 0);
    if (finishedOrChaining)
    {
        SND_StopChannelAndPlayChainAlias(i);
        return;
    }

    float org[3];
    SND_GetCurrent3DPosition(chaninfo->sndEnt, chaninfo->offset, org);
    SND_Set3DPosition(i, org);

    const float distMin = (1.0f - lerp) * alias0->distMin + alias1->distMin * lerp;
    const float distMax = (1.0f - lerp) * alias0->distMax + alias1->distMax * lerp;
    snd_listener *listener = &g_snd.listeners[SND_GetListenerIndexNearestToOrigin(org)];
    float diff[3];
    Vec3Sub(listener->orient.origin, org, diff);
    const float radius = Vec3Length(diff);

    float volume = chaninfo->basevolume * SND_Attenuate(alias0->volumeFalloffCurve, radius, distMin, distMax);
    if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (alias0->flags & 4) != 0)
        volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;
    iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
    volume = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
    SND_Set3DChannelVolume(i, volume * g_snd.volume);
    MSS_ResumeSample(i, frametime);
}

void __cdecl SND_UpdateStreamChannel(int i, int frametime)
{
    iassert(i >= ((0 + 8) + 32) && i < ((0 + 8) + 32) + g_snd.max_stream_channels);

    snd_channel_info_t *chaninfo = &g_snd.chaninfo[i];
    if (chaninfo->paused || (i < 45 && !SND_UpdateBackgroundVolume(i - 40, frametime)))
        return;

    const snd_alias_t *alias0 = chaninfo->alias0;
    const snd_alias_t *alias1 = chaninfo->alias1;
    iassert(alias0);
    iassert(alias1);

    // the feeder thread keeps the ring topped up; this only asks whether anything is left
    bool stillPlaying = chaninfo->startDelay || VitaSnd_StreamHasData(i) || !VitaSnd_VoiceFinished(i);
    if (!stillPlaying)
    {
        SND_StopChannelAndPlayChainAlias(i);
        return;
    }

    float volume = chaninfo->basevolume;
    if (SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
    {
        SND_GetCurrent3DPosition(chaninfo->sndEnt, chaninfo->offset, chaninfo->org);
        int listenerIndex = SND_GetListenerIndexNearestToOrigin(chaninfo->org);
        SND_Set3DStreamPosition(i, listenerIndex, chaninfo->org);
        volume = SND_GetStream3DVolumeFallOff(i, listenerIndex) * volume;
    }
    if (g_snd.slaveLerp != 0.0 && !chaninfo->master && (alias0->flags & 4) != 0)
        volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;

    iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);

    volume = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
    SND_SetStreamChannelVolume(i, volume * g_snd.volume);

    if (chaninfo->startDelay)
    {
        const int remaining = chaninfo->startDelay - frametime;
        chaninfo->startDelay = (remaining > 0) ? remaining : 0;
        if (!chaninfo->startDelay)
            VitaSnd_VoicePlay(i);
    }
}

void __cdecl SND_SetHWND(HWND hwnd)
{
    // no window handle to bind: sceAudio has no DirectSound equivalent
}

void __cdecl SND_SetData(MssSoundCOD4 *mssSound, void *srcData)
{
    // ADPCM stays compressed - the mixer decodes it a block at a time, and halving a block
    // stream by decimation the way the PCM branch does would corrupt it.
    if (mssSound->info.rate > g_snd.playback_rate && mssSound->info.format != 17)
    {
        // a header this path cannot resample would write through a zero-byte allocation
        if ((mssSound->info.bits != 8 && mssSound->info.bits != 16) ||
            mssSound->info.channels < 1 || mssSound->info.channels > 2 ||
            !mssSound->info.samples)
        {
            const LoadedSound *owner =
                (const LoadedSound *)((const uint8_t *)mssSound - offsetof(LoadedSound, sound));
            VitaSys_LogPrintf("snd: '%s' fmt %i rate %u bits %i ch %i samples %u kept as-is\n",
                              owner->name ? owner->name : "?", mssSound->info.format,
                              mssSound->info.rate, mssSound->info.bits,
                              mssSound->info.channels, mssSound->info.samples);
            VitaSys_LogFlush();
            mssSound->data = MSS_Alloc(mssSound->info.data_len, mssSound->info.rate);
            Com_Memcpy(mssSound->data, srcData, mssSound->info.data_len);
            mssSound->info.data_ptr = mssSound->data;
            return;
        }
        const uint32_t srcFrameCount = mssSound->info.samples;
        const uint32_t channels = mssSound->info.channels;
        uint32_t rate = mssSound->info.rate;
        uint32_t frameCount = srcFrameCount;

        while (rate > g_snd.playback_rate)
        {
            rate /= 2;
            frameCount /= 2;
        }

        const uint32_t bytesPerSample = (uint32_t)(mssSound->info.bits / 8);
        const uint32_t newDataLen = frameCount * channels * bytesPerSample;
        mssSound->data = MSS_Alloc(newDataLen, rate);

        // a divide per frame is minutes of load on this cpu, so the ratio walks as a remainder
        const uint32_t step = frameCount ? srcFrameCount / frameCount : 0;
        const uint32_t rem = frameCount ? srcFrameCount % frameCount : 0;
        uint32_t srcFrame = 0, err = 0;

        if (bytesPerSample == 2)
        {
            const int16_t *src16 = (const int16_t *)srcData;
            int16_t *dst16 = (int16_t *)mssSound->data;
            for (uint32_t i = 0; i < frameCount; ++i)
            {
                for (uint32_t c = 0; c < channels; ++c)
                    dst16[i * channels + c] = src16[srcFrame * channels + c];
                srcFrame += step;
                err += rem;
                if (err >= frameCount)
                {
                    err -= frameCount;
                    ++srcFrame;
                }
            }
        }
        else
        {
            const uint8_t *src8 = (const uint8_t *)srcData;
            uint8_t *dst8 = mssSound->data;
            for (uint32_t i = 0; i < frameCount; ++i)
            {
                for (uint32_t c = 0; c < channels; ++c)
                    dst8[i * channels + c] = src8[srcFrame * channels + c];
                srcFrame += step;
                err += rem;
                if (err >= frameCount)
                {
                    err -= frameCount;
                    ++srcFrame;
                }
            }
        }

        mssSound->info.rate = rate;
        mssSound->info.samples = frameCount;
        mssSound->info.data_len = newDataLen;
    }
    else
    {
        mssSound->data = MSS_Alloc(mssSound->info.data_len, mssSound->info.rate);
        Com_Memcpy(mssSound->data, srcData, mssSound->info.data_len);
    }

    mssSound->info.data_ptr = mssSound->data;
    mssSound->info.initial_ptr = mssSound->data;
}

#ifdef KISAK_SP
void SND_SetEqLerp(float lerp)
{
    if (lerp < 0.0f || lerp > 1.0f)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\xenon\\snd_driver.cpp",
            1740,
            0,
            "%s\n\t(lerp) = %g",
            "lerp >= 0 && lerp <= 1",
            lerp);

    vitaGlob.eqLerp = lerp;
    SND_UpdateEqs();
}
#endif

#endif
