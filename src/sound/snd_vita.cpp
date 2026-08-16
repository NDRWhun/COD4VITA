#ifdef KISAK_VITA
// sceAudio device, software mixer and the snd_mss lower half. Mirrors snd_openal.cpp
// function-for-function; see snd_local.h for why these keep their historical MSS_ prefix.
#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include "snd_vita.h"

#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include <vita/platform/vita_memory.h>

#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>

#include <psp2/audioout.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <math.h>
#include <string.h>

// SCE_AUDIO_OUT_PORT_TYPE_MAIN only accepts 48 kHz, so every voice resamples to this rate
#define VITA_SND_OUTPUT_RATE    48000
// 1024 frames = 21.3 ms per sceAudioOutOutput; two buffers, so one grain of slack to mix in
#define VITA_SND_GRAIN          1024
#define VITA_SND_VOICE_COUNT    SND_MAX_CHANNELS
#define VITA_SND_STREAM_COUNT   (SND_MAX_CHANNELS - SND_FIRST_STREAM_CHANNEL)
// 0.74 s at 44.1 kHz - the slack a stalled SD-card read gets to recover in before a dropout.
// Must stay a power of two: the ring index has to survive the uint32 frame counters wrapping.
#define VITA_SND_RING_FRAMES    32768
#define VITA_SND_REFILL_FRAMES  8192
#define VITA_SND_MAX_BLOCK      8192

#define VITA_SND_MIX_PRIORITY    128
#define VITA_SND_STREAM_PRIORITY 144

struct VitaVoice
{
    const uint8_t *data;
    uint32_t frameCount;
    int format;
    int channels;
    int bits;
    int baseRate;
    int rate;
    uint32_t blockSize;
    uint32_t framesPerBlock;

    bool active;
    bool playing;
    bool paused;
    bool looping;
    bool finished;
    bool isStream;

    float volume;
    float pan;
    bool spatialize;

    uint64_t pos;               // 32.32 frame position, relative to the ring read cursor for streams
    uint64_t step;              // 32.32 source frames per output frame
    uint32_t positionFrames;    // position within the sample, for save info

    float curL;
    float curR;

    int16_t *adpcmCache;        // one decoded ADPCM block
    uint32_t adpcmCacheFrames;
    int adpcmBlock;             // block currently decoded into adpcmCache, -1 if none
};

struct VitaStream
{
    bool active;
    bool isMp3;
    bool looping;
    bool eof;
    int fsHandle;
    int channels;
    int rate;
    drwav wav;
    drmp3 mp3;

    int16_t *ring;
    uint32_t capacity;          // frames
    uint32_t write;             // frames produced, absolute
    uint32_t read;              // frames consumed, absolute
    uint32_t loopBase;          // value of read when the current loop pass began
};

static VitaVoice s_voice[VITA_SND_VOICE_COUNT];
static VitaStream s_stream[VITA_SND_STREAM_COUNT];

static SceUID s_mixLock = -1;
static SceUID s_streamLock = -1;
static SceUID s_mixThread = -1;
static SceUID s_streamThread = -1;
static volatile bool s_quit;
static bool s_active;
static int s_port = -1;

static int16_t s_outBuffer[2][VITA_SND_GRAIN * 2];
static float s_mixL[VITA_SND_GRAIN];
static float s_mixR[VITA_SND_GRAIN];

static volatile int s_cpuPercent;
static uint32_t s_underruns;

static void VitaSnd_MixLock(void) { sceKernelLockMutex(s_mixLock, 1, NULL); }
static void VitaSnd_MixUnlock(void) { sceKernelUnlockMutex(s_mixLock, 1); }
static void VitaSnd_StreamLock(void) { sceKernelLockMutex(s_streamLock, 1, NULL); }
static void VitaSnd_StreamUnlock(void) { sceKernelUnlockMutex(s_streamLock, 1); }

// IMA/DVI ADPCM, the format CoD4's fastfile loaded_sounds use when info.format is 17
static const int kImaIndexTable[16] =
{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int kImaStepTable[89] =
{
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
    279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static uint32_t VitaSnd_ImaFramesPerBlock(uint32_t blockSize, int channels)
{
    const uint32_t header = 4u * (uint32_t)channels;
    if (blockSize <= header)
        return 0;
    return ((blockSize - header) * 2) / (uint32_t)channels + 1;
}

static void VitaSnd_DecodeImaBlock(const uint8_t *block, uint32_t blockSize, int channels,
                                   int16_t *out, uint32_t frames)
{
    int predictor[2];
    int index[2];

    for (int c = 0; c < channels; ++c)
    {
        predictor[c] = (int16_t)(block[c * 4] | (block[c * 4 + 1] << 8));
        index[c] = block[c * 4 + 2];
        if (index[c] < 0)
            index[c] = 0;
        if (index[c] > 88)
            index[c] = 88;
        out[c] = (int16_t)predictor[c];
    }

    const uint8_t *p = block + 4 * channels;
    const uint8_t *end = block + blockSize;
    uint32_t frame = 1;

    while (frame < frames && p + 4 * channels <= end)
    {
        for (int c = 0; c < channels; ++c)
        {
            for (int b = 0; b < 4; ++b)
            {
                const uint8_t byte = p[c * 4 + b];
                for (int half = 0; half < 2; ++half)
                {
                    const int nibble = half ? (byte >> 4) : (byte & 0xF);
                    const int step = kImaStepTable[index[c]];
                    int diff = step >> 3;
                    if (nibble & 1)
                        diff += step >> 2;
                    if (nibble & 2)
                        diff += step >> 1;
                    if (nibble & 4)
                        diff += step;
                    if (nibble & 8)
                        diff = -diff;

                    predictor[c] += diff;
                    if (predictor[c] > 32767)
                        predictor[c] = 32767;
                    if (predictor[c] < -32768)
                        predictor[c] = -32768;

                    index[c] += kImaIndexTable[nibble];
                    if (index[c] < 0)
                        index[c] = 0;
                    if (index[c] > 88)
                        index[c] = 88;

                    const uint32_t f = frame + (uint32_t)(b * 2 + half);
                    if (f < frames)
                        out[f * channels + c] = (int16_t)predictor[c];
                }
            }
        }
        p += 4 * channels;
        frame += 8;
    }
}

static inline void VitaSnd_FetchFrame(VitaVoice *voice, uint32_t frame, float *left, float *right)
{
    if (voice->format == 17)
    {
        const int block = (int)(frame / voice->framesPerBlock);
        if (block != voice->adpcmBlock)
        {
            const uint32_t offset = (uint32_t)block * voice->blockSize;
            VitaSnd_DecodeImaBlock(voice->data + offset, voice->blockSize, voice->channels,
                                   voice->adpcmCache, voice->framesPerBlock);
            voice->adpcmBlock = block;
        }
        const uint32_t local = frame - (uint32_t)block * voice->framesPerBlock;
        const int16_t *src = voice->adpcmCache + local * voice->channels;
        *left = (float)src[0];
        *right = (voice->channels == 2) ? (float)src[1] : *left;
        return;
    }

    if (voice->bits == 8)
    {
        const uint8_t *src = voice->data + frame * voice->channels;
        *left = ((float)src[0] - 128.0f) * 256.0f;
        *right = (voice->channels == 2) ? (((float)src[1] - 128.0f) * 256.0f) : *left;
        return;
    }

    const int16_t *src = (const int16_t *)voice->data + frame * voice->channels;
    *left = (float)src[0];
    *right = (voice->channels == 2) ? (float)src[1] : *left;
}

static void VitaSnd_VoiceGains(const VitaVoice *voice, float *left, float *right)
{
    if (!voice->spatialize)
    {
        *left = voice->volume;
        *right = voice->volume;
        return;
    }

    // Miles' own convention: a centred mono 3D sample sits at half volume in each speaker
    *left = voice->volume * (1.0f - voice->pan) * 0.5f;
    *right = voice->volume * (1.0f + voice->pan) * 0.5f;
}

static void VitaSnd_MixSampleVoice(VitaVoice *voice, int frames)
{
    float targetL;
    float targetR;
    VitaSnd_VoiceGains(voice, &targetL, &targetR);

    const float rampL = (targetL - voice->curL) / (float)frames;
    const float rampR = (targetR - voice->curR) / (float)frames;
    float gainL = voice->curL;
    float gainR = voice->curR;

    const uint64_t total = (uint64_t)voice->frameCount << 32;
    uint64_t pos = voice->pos;

    for (int i = 0; i < frames; ++i)
    {
        if (pos >= total)
        {
            if (!voice->looping)
            {
                voice->finished = 1;
                voice->playing = 0;
                break;
            }
            pos -= total;
        }

        const uint32_t frame = (uint32_t)(pos >> 32);
        uint32_t next = frame + 1;
        if (next >= voice->frameCount)
            next = voice->looping ? 0 : frame;
        else if (voice->format == 17 && (next / voice->framesPerBlock) != (frame / voice->framesPerBlock))
            next = frame;       // avoid decoding two ADPCM blocks for one output frame

        float l0, r0, l1, r1;
        VitaSnd_FetchFrame(voice, frame, &l0, &r0);
        VitaSnd_FetchFrame(voice, next, &l1, &r1);

        const float t = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
        const float l = l0 + (l1 - l0) * t;
        const float r = r0 + (r1 - r0) * t;

        s_mixL[i] += l * gainL;
        s_mixR[i] += r * gainR;

        gainL += rampL;
        gainR += rampR;
        pos += voice->step;
    }

    voice->pos = pos;
    voice->positionFrames = (uint32_t)(pos >> 32);
    voice->curL = targetL;
    voice->curR = targetR;
}

static void VitaSnd_MixStreamVoice(VitaVoice *voice, int index, int frames)
{
    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];
    if (!stream->ring)
        return;

    const uint32_t write = __atomic_load_n(&stream->write, __ATOMIC_ACQUIRE);
    const uint32_t read = stream->read;
    uint32_t avail = write - read;

    float targetL;
    float targetR;
    VitaSnd_VoiceGains(voice, &targetL, &targetR);

    const float rampL = (targetL - voice->curL) / (float)frames;
    const float rampR = (targetR - voice->curR) / (float)frames;
    float gainL = voice->curL;
    float gainR = voice->curR;

    const uint32_t capacity = stream->capacity;
    const int channels = stream->channels;
    uint64_t pos = voice->pos;
    int mixed = 0;

    for (int i = 0; i < frames; ++i)
    {
        const uint32_t frame = (uint32_t)(pos >> 32);
        if (frame + 1 >= avail)
            break;

        const int16_t *a = stream->ring + ((read + frame) % capacity) * channels;
        const int16_t *b = stream->ring + ((read + frame + 1) % capacity) * channels;
        const float t = (float)(uint32_t)pos * (1.0f / 4294967296.0f);

        const float l0 = (float)a[0];
        const float r0 = (channels == 2) ? (float)a[1] : l0;
        const float l1 = (float)b[0];
        const float r1 = (channels == 2) ? (float)b[1] : l1;

        s_mixL[i] += (l0 + (l1 - l0) * t) * gainL;
        s_mixR[i] += (r0 + (r1 - r0) * t) * gainR;

        gainL += rampL;
        gainR += rampR;
        pos += voice->step;
        ++mixed;
    }

    const uint32_t consumed = (uint32_t)(pos >> 32);
    if (consumed)
    {
        __atomic_store_n(&stream->read, read + consumed, __ATOMIC_RELEASE);
        pos &= 0xFFFFFFFFull;
    }
    voice->pos = pos;

    if (mixed < frames)
    {
        if (__atomic_load_n(&stream->eof, __ATOMIC_ACQUIRE) && avail <= consumed + 1)
        {
            voice->finished = 1;
            voice->playing = 0;
        }
        else
        {
            __atomic_fetch_add(&s_underruns, 1, __ATOMIC_RELAXED);
        }
    }

    voice->curL = targetL;
    voice->curR = targetR;
}

static void VitaSnd_MixGrain(int16_t *out)
{
    memset(s_mixL, 0, sizeof(s_mixL));
    memset(s_mixR, 0, sizeof(s_mixR));

    VitaSnd_MixLock();
    for (int i = 0; i < VITA_SND_VOICE_COUNT; ++i)
    {
        VitaVoice *voice = &s_voice[i];
        if (!voice->active || !voice->playing || voice->paused)
            continue;

        if (voice->isStream)
            VitaSnd_MixStreamVoice(voice, i, VITA_SND_GRAIN);
        else if (voice->data && voice->frameCount)
            VitaSnd_MixSampleVoice(voice, VITA_SND_GRAIN);
    }
    VitaSnd_MixUnlock();

    for (int i = 0; i < VITA_SND_GRAIN; ++i)
    {
        float l = s_mixL[i];
        float r = s_mixR[i];
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

static int VitaSnd_MixThreadEntry(SceSize args, void *argp)
{
    int buffer = 0;
    uint64_t busy = 0;
    uint64_t window = 0;

    while (!s_quit)
    {
        const uint64_t start = sceKernelGetProcessTimeWide();
        VitaSnd_MixGrain(s_outBuffer[buffer]);
        busy += sceKernelGetProcessTimeWide() - start;

        sceAudioOutOutput(s_port, s_outBuffer[buffer]);
        buffer ^= 1;

        window += (uint64_t)VITA_SND_GRAIN * 1000000ull / VITA_SND_OUTPUT_RATE;
        if (window >= 1000000ull)
        {
            s_cpuPercent = (int)(busy * 100ull / window);
            busy = 0;
            window = 0;
        }
    }
    return 0;
}

static size_t VitaSnd_StreamRead(void *userData, void *out, size_t bytes)
{
    VitaStream *stream = (VitaStream *)userData;
    return (size_t)FS_Read((uint8_t *)out, (uint32_t)bytes, stream->fsHandle);
}

// FS_Seek's zip branch rotates its three origin values by one relative to SEEK_SET/CUR/END,
// which is inherited from the decompiled engine and unsafe to fix inside FS_Seek itself.
static int VitaSnd_FixupZipSeekOrigin(int fsHandle, int trueOrigin)
{
    static const int kZipOriginRotation[3] = { 2, 0, 1 };
    return FS_IsFileInZip(fsHandle) ? kZipOriginRotation[trueOrigin] : trueOrigin;
}

static drwav_bool32 VitaSnd_WavSeek(void *userData, int offset, drwav_seek_origin origin)
{
    VitaStream *stream = (VitaStream *)userData;
    FS_Seek(stream->fsHandle, offset, VitaSnd_FixupZipSeekOrigin(stream->fsHandle, (int)origin));
    return DRWAV_TRUE;
}

static drwav_bool32 VitaSnd_WavTell(void *userData, drwav_int64 *cursor)
{
    VitaStream *stream = (VitaStream *)userData;
    *cursor = FS_FTell(stream->fsHandle);
    return DRWAV_TRUE;
}

static drmp3_bool32 VitaSnd_Mp3Seek(void *userData, int offset, drmp3_seek_origin origin)
{
    VitaStream *stream = (VitaStream *)userData;
    FS_Seek(stream->fsHandle, offset, VitaSnd_FixupZipSeekOrigin(stream->fsHandle, (int)origin));
    return DRMP3_TRUE;
}

static drmp3_bool32 VitaSnd_Mp3Tell(void *userData, drmp3_int64 *cursor)
{
    VitaStream *stream = (VitaStream *)userData;
    *cursor = FS_FTell(stream->fsHandle);
    return DRMP3_TRUE;
}

// Tops one stream's ring back up. Runs on the feeder thread and on the caller's thread for
// the first fill; the caller must hold s_streamLock.
static void VitaSnd_StreamRefill(VitaStream *stream)
{
    static int16_t scratch[VITA_SND_REFILL_FRAMES * 2];

    if (!stream->active || !stream->ring || stream->eof)
        return;

    const uint32_t read = __atomic_load_n(&stream->read, __ATOMIC_ACQUIRE);
    uint32_t space = stream->capacity - (stream->write - read) - 1;
    if (space > VITA_SND_REFILL_FRAMES)
        space = VITA_SND_REFILL_FRAMES;
    if (!space)
        return;

    uint64_t frames = stream->isMp3
        ? drmp3_read_pcm_frames_s16(&stream->mp3, space, scratch)
        : drwav_read_pcm_frames_s16(&stream->wav, space, scratch);

    if (frames == 0)
    {
        if (!stream->looping)
        {
            __atomic_store_n(&stream->eof, true, __ATOMIC_RELEASE);
            return;
        }

        if (stream->isMp3)
            drmp3_seek_to_pcm_frame(&stream->mp3, 0);
        else
            drwav_seek_to_pcm_frame(&stream->wav, 0);

        frames = stream->isMp3
            ? drmp3_read_pcm_frames_s16(&stream->mp3, space, scratch)
            : drwav_read_pcm_frames_s16(&stream->wav, space, scratch);
        if (frames == 0)
        {
            // zero-length file: stop rather than spin the feeder thread on it
            __atomic_store_n(&stream->eof, true, __ATOMIC_RELEASE);
            return;
        }
        stream->loopBase = stream->write;
    }

    const uint32_t channels = (uint32_t)stream->channels;
    const uint32_t head = stream->write % stream->capacity;
    const uint32_t firstRun = stream->capacity - head;
    const uint32_t copy = ((uint32_t)frames < firstRun) ? (uint32_t)frames : firstRun;

    memcpy(stream->ring + head * channels, scratch, copy * channels * sizeof(int16_t));
    if (copy < (uint32_t)frames)
    {
        memcpy(stream->ring, scratch + copy * channels,
               ((uint32_t)frames - copy) * channels * sizeof(int16_t));
    }

    __atomic_store_n(&stream->write, stream->write + (uint32_t)frames, __ATOMIC_RELEASE);
}

static int VitaSnd_StreamThreadEntry(SceSize args, void *argp)
{
    while (!s_quit)
    {
        for (int i = 0; i < VITA_SND_STREAM_COUNT; ++i)
        {
            VitaSnd_StreamLock();
            VitaSnd_StreamRefill(&s_stream[i]);
            VitaSnd_StreamUnlock();
        }
        sceKernelDelayThread(10000);
    }
    return 0;
}

bool VitaSnd_Init(void)
{
    if (s_active)
        return true;

    memset(s_voice, 0, sizeof(s_voice));
    memset(s_stream, 0, sizeof(s_stream));

    for (int i = 0; i < VITA_SND_STREAM_COUNT; ++i)
    {
        VitaStream *stream = &s_stream[i];
        stream->capacity = VITA_SND_RING_FRAMES;
        stream->ring = (int16_t *)VitaMem_Alloc(VITA_MEM_MAIN,
                                                VITA_SND_RING_FRAMES * 2 * sizeof(int16_t), 16);
        if (!stream->ring)
        {
            Com_PrintError(9, "ERROR: out of memory reserving the sound stream ring buffers\n");
            VitaSnd_Shutdown();
            return false;
        }
    }

    s_mixLock = sceKernelCreateMutex("kcod_snd_mix", 0, 0, NULL);
    s_streamLock = sceKernelCreateMutex("kcod_snd_stream", 0, 0, NULL);
    if (s_mixLock < 0 || s_streamLock < 0)
    {
        Com_PrintError(9, "ERROR: sound mutex creation failed\n");
        VitaSnd_Shutdown();
        return false;
    }

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_SND_GRAIN,
                                 VITA_SND_OUTPUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0)
    {
        Com_PrintError(9, "ERROR: sceAudioOutOpenPort failed (0x%08x)\n", s_port);
        VitaSnd_Shutdown();
        return false;
    }

    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);

    s_quit = false;
    s_mixThread = sceKernelCreateThread("kcod_snd_mix", VitaSnd_MixThreadEntry,
                                        VITA_SND_MIX_PRIORITY, 64 * 1024, 0,
                                        SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    s_streamThread = sceKernelCreateThread("kcod_snd_stream", VitaSnd_StreamThreadEntry,
                                           VITA_SND_STREAM_PRIORITY, 128 * 1024, 0,
                                           SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (s_mixThread < 0 || s_streamThread < 0)
    {
        Com_PrintError(9, "ERROR: sound thread creation failed\n");
        VitaSnd_Shutdown();
        return false;
    }

    s_active = true;
    sceKernelStartThread(s_mixThread, 0, NULL);
    sceKernelStartThread(s_streamThread, 0, NULL);
    return true;
}

void VitaSnd_Shutdown(void)
{
    s_quit = true;

    if (s_mixThread >= 0)
    {
        SceUInt timeout = 2000000;
        sceKernelWaitThreadEnd(s_mixThread, NULL, &timeout);
        sceKernelDeleteThread(s_mixThread);
        s_mixThread = -1;
    }
    if (s_streamThread >= 0)
    {
        SceUInt timeout = 2000000;
        sceKernelWaitThreadEnd(s_streamThread, NULL, &timeout);
        sceKernelDeleteThread(s_streamThread);
        s_streamThread = -1;
    }

    for (int i = 0; i < VITA_SND_STREAM_COUNT; ++i)
    {
        VitaStream *stream = &s_stream[i];
        if (stream->active)
        {
            if (stream->isMp3)
                drmp3_uninit(&stream->mp3);
            else
                drwav_uninit(&stream->wav);
            FS_FCloseFile(stream->fsHandle);
            stream->active = false;
        }
        if (stream->ring)
        {
            VitaMem_Free(stream->ring);
            stream->ring = NULL;
        }
    }

    for (int i = 0; i < VITA_SND_VOICE_COUNT; ++i)
    {
        if (s_voice[i].adpcmCache)
            VitaMem_Free(s_voice[i].adpcmCache);
    }
    memset(s_voice, 0, sizeof(s_voice));

    if (s_port >= 0)
    {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
    }
    if (s_mixLock >= 0)
    {
        sceKernelDeleteMutex(s_mixLock);
        s_mixLock = -1;
    }
    if (s_streamLock >= 0)
    {
        sceKernelDeleteMutex(s_streamLock);
        s_streamLock = -1;
    }

    s_active = false;
}

bool VitaSnd_IsActive(void)
{
    return s_active;
}

int VitaSnd_OutputRate(void)
{
    return VITA_SND_OUTPUT_RATE;
}

int VitaSnd_CpuPercent(void)
{
    return s_cpuPercent;
}

uint32_t VitaSnd_UnderrunCount(void)
{
    return __atomic_load_n(&s_underruns, __ATOMIC_RELAXED);
}

static void VitaSnd_VoiceUpdateStep(VitaVoice *voice)
{
    int rate = voice->rate;
    if (rate < 1)
        rate = 1;
    if (rate > VITA_SND_OUTPUT_RATE * 4)
        rate = VITA_SND_OUTPUT_RATE * 4;
    voice->step = ((uint64_t)(uint32_t)rate << 32) / VITA_SND_OUTPUT_RATE;
}

bool VitaSnd_VoiceSetSample(int index, const VitaSndSample *sample)
{
    if (!s_active)
        return false;

    if (sample->format != 1 && sample->format != 17)
    {
        static int reported = 0;
        if (reported != sample->format)
        {
            Com_PrintError(9, "ERROR: unsupported sound format %i - the Vita mixer plays PCM and IMA ADPCM only\n",
                           sample->format);
            reported = sample->format;
        }
        return false;
    }
    if (sample->channels != 1 && sample->channels != 2)
        return false;
    if (sample->format == 1 && sample->bits != 8 && sample->bits != 16)
        return false;
    if (!sample->data || !sample->frameCount || sample->rate <= 0)
        return false;

    uint32_t framesPerBlock = 0;
    uint32_t blockFrames = 0;
    int16_t *cache = NULL;
    if (sample->format == 17)
    {
        if (sample->blockSize == 0 || sample->blockSize > VITA_SND_MAX_BLOCK)
            return false;
        framesPerBlock = VitaSnd_ImaFramesPerBlock(sample->blockSize, sample->channels);
        if (!framesPerBlock)
            return false;
        // info.samples is the asset's claim; only whole blocks that are present can be decoded
        blockFrames = (sample->dataLen / sample->blockSize) * framesPerBlock;
        if (!blockFrames)
            return false;

        // grown outside the mix lock so a first-time ADPCM sound never stalls the mixer
        VitaVoice *voice = &s_voice[index];
        if (voice->adpcmCacheFrames < framesPerBlock)
        {
            cache = (int16_t *)VitaMem_Alloc(VITA_MEM_MAIN,
                                             framesPerBlock * 2 * sizeof(int16_t), 16);
            if (!cache)
                return false;
        }
    }

    VitaSnd_MixLock();
    VitaVoice *voice = &s_voice[index];
    if (cache)
    {
        if (voice->adpcmCache)
            VitaMem_Free(voice->adpcmCache);
        voice->adpcmCache = cache;
        voice->adpcmCacheFrames = framesPerBlock;
    }

    voice->data = sample->data;
    voice->frameCount = (sample->format == 17 && sample->frameCount > blockFrames)
        ? blockFrames
        : sample->frameCount;
    voice->format = sample->format;
    voice->channels = sample->channels;
    voice->bits = sample->bits;
    voice->baseRate = sample->rate;
    voice->rate = sample->rate;
    voice->blockSize = sample->blockSize;
    voice->framesPerBlock = framesPerBlock;
    voice->adpcmBlock = -1;
    voice->active = 1;
    voice->playing = 0;
    voice->paused = 0;
    voice->finished = 0;
    voice->isStream = 0;
    voice->looping = 0;
    voice->spatialize = 0;
    voice->pan = 0.0f;
    voice->pos = 0;
    voice->positionFrames = 0;
    voice->curL = 0.0f;
    voice->curR = 0.0f;
    VitaSnd_VoiceUpdateStep(voice);
    VitaSnd_MixUnlock();
    return true;
}

void VitaSnd_VoiceStop(int index)
{
    if (!s_active)
        return;

    VitaSnd_MixLock();
    VitaVoice *voice = &s_voice[index];
    voice->active = 0;
    voice->playing = 0;
    voice->paused = 0;
    voice->finished = 0;
    voice->isStream = 0;
    voice->data = NULL;
    voice->frameCount = 0;
    voice->pos = 0;
    voice->positionFrames = 0;
    voice->curL = 0.0f;
    voice->curR = 0.0f;
    voice->adpcmBlock = -1;
    VitaSnd_MixUnlock();
}

void VitaSnd_VoicePlay(int index)
{
    if (!s_active)
        return;

    VitaSnd_MixLock();
    VitaVoice *voice = &s_voice[index];
    if (voice->active && !voice->finished)
    {
        // start at the target gain: a sample shorter than one grain would otherwise spend
        // its whole length ramping up from silence
        if (!voice->playing)
            VitaSnd_VoiceGains(voice, &voice->curL, &voice->curR);
        voice->playing = 1;
        voice->paused = 0;
    }
    VitaSnd_MixUnlock();
}

void VitaSnd_VoicePause(int index)
{
    if (!s_active)
        return;

    VitaSnd_MixLock();
    s_voice[index].paused = 1;
    VitaSnd_MixUnlock();
}

bool VitaSnd_VoiceFinished(int index)
{
    if (!s_active)
        return true;

    return s_voice[index].finished;
}

void VitaSnd_VoiceSetLooping(int index, bool looping)
{
    if (!s_active)
        return;

    s_voice[index].looping = looping;
}

void VitaSnd_VoiceSetVolume(int index, float volume)
{
    if (!s_active)
        return;

    s_voice[index].volume = volume;
}

float VitaSnd_VoiceGetVolume(int index)
{
    if (!s_active)
        return 0.0f;

    return s_voice[index].volume;
}

void VitaSnd_VoiceSetPan(int index, float pan, bool spatialize)
{
    if (!s_active)
        return;

    if (pan < -1.0f)
        pan = -1.0f;
    if (pan > 1.0f)
        pan = 1.0f;
    s_voice[index].pan = pan;
    s_voice[index].spatialize = spatialize;
}

void VitaSnd_VoiceSetRate(int index, int rate)
{
    if (!s_active)
        return;

    VitaSnd_MixLock();
    s_voice[index].rate = rate;
    VitaSnd_VoiceUpdateStep(&s_voice[index]);
    VitaSnd_MixUnlock();
}

int VitaSnd_VoiceGetRate(int index)
{
    if (!s_active)
        return 0;

    return s_voice[index].rate;
}

void VitaSnd_VoiceSeekFrame(int index, uint32_t frame)
{
    if (!s_active)
        return;

    VitaSnd_MixLock();
    VitaVoice *voice = &s_voice[index];
    if (frame >= voice->frameCount)
        frame = voice->frameCount ? voice->frameCount - 1 : 0;
    voice->pos = (uint64_t)frame << 32;
    voice->positionFrames = frame;
    voice->adpcmBlock = -1;
    VitaSnd_MixUnlock();
}

uint32_t VitaSnd_VoicePositionFrames(int index)
{
    if (!s_active)
        return 0;

    return s_voice[index].positionFrames;
}

bool VitaSnd_StreamOpen(int index, const char *path, bool isMp3, bool looping,
                        int *channels, int *rate, uint64_t *totalFrames)
{
    if (!s_active)
        return false;

    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];

    VitaSnd_StreamLock();
    if ((FS_FOpenFileReadStream(path, &stream->fsHandle) & 0x80000000) != 0)
    {
        VitaSnd_StreamUnlock();
        return false;
    }

    stream->isMp3 = isMp3;
    const bool decoderOk = isMp3
        ? drmp3_init(&stream->mp3, VitaSnd_StreamRead, VitaSnd_Mp3Seek, VitaSnd_Mp3Tell, NULL, stream, NULL)
        : drwav_init(&stream->wav, VitaSnd_StreamRead, VitaSnd_WavSeek, VitaSnd_WavTell, stream, NULL);
    if (!decoderOk)
    {
        FS_FCloseFile(stream->fsHandle);
        VitaSnd_StreamUnlock();
        return false;
    }

    stream->channels = isMp3 ? (int)stream->mp3.channels : (int)stream->wav.channels;
    stream->rate = isMp3 ? (int)stream->mp3.sampleRate : (int)stream->wav.sampleRate;
    if (stream->channels != 1 && stream->channels != 2)
    {
        if (isMp3)
            drmp3_uninit(&stream->mp3);
        else
            drwav_uninit(&stream->wav);
        FS_FCloseFile(stream->fsHandle);
        VitaSnd_StreamUnlock();
        return false;
    }

    stream->looping = looping;
    stream->eof = false;
    stream->write = 0;
    stream->read = 0;
    stream->loopBase = 0;
    stream->active = true;

    // dr_mp3 has to scan for its frame count, unlike dr_wav which reads it from the header
    *totalFrames = isMp3 ? drmp3_get_pcm_frame_count(&stream->mp3) : stream->wav.totalPCMFrameCount;
    *channels = stream->channels;
    *rate = stream->rate;

    VitaSnd_StreamRefill(stream);
    VitaSnd_StreamUnlock();

    VitaSnd_MixLock();
    VitaVoice *voice = &s_voice[index];
    voice->data = NULL;
    voice->frameCount = 0;
    voice->format = 1;
    voice->channels = stream->channels;
    voice->bits = 16;
    voice->baseRate = stream->rate;
    voice->rate = stream->rate;
    voice->active = 1;
    voice->playing = 0;
    voice->paused = 0;
    voice->finished = 0;
    voice->isStream = 1;
    voice->looping = looping;
    voice->spatialize = 0;
    voice->pan = 0.0f;
    voice->pos = 0;
    voice->positionFrames = 0;
    voice->curL = 0.0f;
    voice->curR = 0.0f;
    VitaSnd_VoiceUpdateStep(voice);
    VitaSnd_MixUnlock();
    return true;
}

void VitaSnd_StreamClose(int index)
{
    if (!s_active)
        return;

    VitaSnd_VoiceStop(index);

    VitaSnd_StreamLock();
    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];
    if (stream->active)
    {
        if (stream->isMp3)
            drmp3_uninit(&stream->mp3);
        else
            drwav_uninit(&stream->wav);
        FS_FCloseFile(stream->fsHandle);
        stream->active = false;
    }
    stream->eof = false;
    stream->write = 0;
    stream->read = 0;
    stream->loopBase = 0;
    VitaSnd_StreamUnlock();
}

bool VitaSnd_StreamIsOpen(int index)
{
    if (!s_active)
        return false;

    return s_stream[index - SND_FIRST_STREAM_CHANNEL].active;
}

bool VitaSnd_StreamHasData(int index)
{
    if (!s_active)
        return false;

    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];
    if (!stream->active)
        return false;
    if (!__atomic_load_n(&stream->eof, __ATOMIC_ACQUIRE))
        return true;
    return (__atomic_load_n(&stream->write, __ATOMIC_ACQUIRE) - stream->read) > 1;
}

// Only valid before the channel starts playing: the ring is reset, not resynchronised.
void VitaSnd_StreamSeekFrame(int index, uint64_t frame)
{
    if (!s_active)
        return;

    VitaSnd_StreamLock();
    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];
    if (stream->active)
    {
        if (stream->isMp3)
            drmp3_seek_to_pcm_frame(&stream->mp3, frame);
        else
            drwav_seek_to_pcm_frame(&stream->wav, frame);
        stream->eof = false;
        stream->write = 0;
        stream->read = 0;
        stream->loopBase = 0;
        VitaSnd_StreamRefill(stream);
    }
    VitaSnd_StreamUnlock();

    VitaSnd_MixLock();
    s_voice[index].pos = 0;
    VitaSnd_MixUnlock();
}

uint64_t VitaSnd_StreamPlayedFrames(int index)
{
    if (!s_active)
        return 0;

    VitaStream *stream = &s_stream[index - SND_FIRST_STREAM_CHANNEL];
    return (uint64_t)(__atomic_load_n(&stream->read, __ATOMIC_ACQUIRE) - stream->loopBase);
}

void MSS_InitFailed()
{
    if (Dvar_GetInt("r_vc_compile") != 2)
        Com_Printf(9, "Vita sound system initialization failed\n");
}

char __cdecl MSS_Init()
{
    int integer = snd_khz->current.integer;
    int hertz;
    if (integer == 11)
    {
        hertz = 11025;
    }
    else if (integer == 44)
    {
        hertz = 44100;
    }
    else
    {
        if (integer != 22)
            Com_Printf(9, "invalid value %i for snd_khz, using 22 khz instead\n", snd_khz->current.integer);
        hertz = 22050;
    }

    Com_Printf(
        9,
        "Attempting %i kHz %i bit [%s] sound\n",
        hertz / 1000,
        16,
        snd_outputConfigurationStrings[snd_outputConfiguration->current.integer]);

    if (!VitaSnd_Init())
        return 0;

    Com_Printf(9, "sceAudio main port open: %i Hz 16 bit stereo, %i sample grain\n",
               VitaSnd_OutputRate(), VITA_SND_GRAIN);

    g_snd.Initialized2d = 1;
    g_snd.Initialized3d = 1;
    g_snd.max_2D_channels = 8;
    g_snd.max_3D_channels = 32;
    g_snd.max_stream_channels = VITA_SND_STREAM_COUNT;
    g_snd.playback_rate = hertz + hertz / 2;
    if (g_snd.playback_rate >= 0xAC44)
        g_snd.playback_rate = 0x7FFFFFFF;
    g_snd.playback_channels = 2;
    g_snd.timescale = 1.0;
    return 1;
}

void MSS_InitChannels()
{
    vitaGlob.isMultiChannel = 0;
    g_snd.ambient_track = 1;
}

void MSS_InitEq()
{
    vitaGlob.eqFilter = 0;
#ifndef KISAK_XBOX
    vitaGlob.eqLerp = 1.0f;
#endif

    for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (int band = 0; band < 3; ++band)
        {
            for (int channelIndex = 0; channelIndex < 64; ++channelIndex)
            {
                SndEqParams *params = &vitaGlob.eq[eqIndex].params[band][channelIndex];
                params->enabled = 0;
                params->freq = 20000.0f;
                params->gain = 1.0f;
                params->q = 1.0f;
                params->type = SND_EQTYPE_FIRST;
            }
        }
    }
}

// Miles' AIL_startup is a global SDK init with no device involved; sceAudio has no such
// stage, so the only real check is the port open in MSS_Init.
bool __cdecl MSS_Startup()
{
    return true;
}

void MSS_ShutdownCleanup()
{
    VitaSnd_Shutdown();
    memset(&vitaGlob, 0, sizeof(vitaGlob));
}

float MSS_GetDryLevel()
{
    return 1.0f;
}

float MSS_GetWetLevel(const snd_alias_t *pAlias)
{
    iassert(g_snd.effect->wetlevel >= 0 && g_snd.effect->wetlevel <= 1);

    if (!pAlias)
        return g_snd.effect->wetlevel;

    if (!snd_enableReverb->current.enabled || (pAlias->flags & 0x10) != 0)
        return 0.0f;
    else
        return g_snd.effect->wetlevel;
}

// A genuine no-op, as on the OpenAL side: Miles does 3-band parametric EQ per entchannel and
// the software mixer has no biquad stage. snd_enableEq already defaults off upstream.
void __cdecl MSS_ApplyEqFilter(int index, int entchannel)
{
}

void __cdecl MSS_ResumeSample(int i, int frametime)
{
    if (g_snd.chaninfo[i].startDelay)
    {
        int remaining = g_snd.chaninfo[i].startDelay - frametime;
        g_snd.chaninfo[i].startDelay = (remaining > 0) ? remaining : 0;
        if (!g_snd.chaninfo[i].startDelay)
            VitaSnd_VoicePlay(i);
    }
}

int __cdecl MSS_DigitalFormatType(int waveFormat, int bits, int channels)
{
    int digitalFormat;

    if (waveFormat != 1 && waveFormat != 17)
        Com_Error(ERR_FATAL, "unknown wave format %i", waveFormat);
    if (channels != 1 && channels != 2)
        Com_Error(ERR_FATAL, "Sound has %i channels; only 1 or 2 channels are supported.\n", channels);
    if (bits != 8 && bits != 16)
        Com_Error(ERR_FATAL, "Sound uses %i bits per channel; only 8 or 16 bit channels are supported.\n", bits);
    digitalFormat = 0;
    if (waveFormat == 17)
        digitalFormat = 4;
    if (bits == 16)
        digitalFormat |= 1u;
    if (channels == 2)
        return digitalFormat | 2;
    return digitalFormat;
}

uint8_t *__cdecl MSS_Alloc(uint32_t bytes, uint32_t rate)
{
    if (IsFastFileLoad())
        return (uint8_t *)MSS_Alloc_FastFile((int)bytes);
    else
        return MSS_Alloc_LoadObj(bytes, rate);
}

uint8_t *__cdecl MSS_Alloc_LoadObj(uint32_t bytes, uint32_t rate)
{
    return Hunk_Alloc(bytes, "MSS_Alloc", 15);
}

uint32_t *__cdecl MSS_Alloc_FastFile(int bytes)
{
    return (uint32_t *)Z_Malloc(bytes, "MSS_Alloc", 15);
}

#endif
