// Software mixer and sceAudio output behind the snd_driver interface.
#pragma once

#ifdef KISAK_VITA

#include <stdint.h>

// A loaded sound as the mixer reads it: either interleaved PCM or a raw IMA-ADPCM block run.
struct VitaSndSample
{
    const uint8_t *data;
    uint32_t dataLen;
    uint32_t frameCount;
    int format;                 // 1 = PCM, 17 = IMA ADPCM
    int channels;
    int bits;                   // PCM only: 8 or 16
    int rate;
    uint32_t blockSize;         // ADPCM only: bytes per block
};

bool VitaSnd_Init(void);
void VitaSnd_Shutdown(void);
bool VitaSnd_IsActive(void);
int VitaSnd_OutputRate(void);

// share of the mixer thread's grain period spent mixing, averaged over the last second
int VitaSnd_CpuPercent(void);
uint32_t VitaSnd_UnderrunCount(void);

bool VitaSnd_VoiceSetSample(int index, const VitaSndSample *sample);
void VitaSnd_VoiceStop(int index);
void VitaSnd_VoicePlay(int index);
void VitaSnd_VoicePause(int index);
bool VitaSnd_VoiceFinished(int index);
void VitaSnd_VoiceSetLooping(int index, bool looping);
void VitaSnd_VoiceSetVolume(int index, float volume);
float VitaSnd_VoiceGetVolume(int index);
void VitaSnd_VoiceSetPan(int index, float pan, bool spatialize);
void VitaSnd_VoiceSetRate(int index, int rate);
int VitaSnd_VoiceGetRate(int index);
void VitaSnd_VoiceSeekFrame(int index, uint32_t frame);
uint32_t VitaSnd_VoicePositionFrames(int index);

// Streams decode on their own thread into a ring the mixer drains, so a slow read costs
// buffered slack rather than a dropout. The file is opened on the calling thread.
bool VitaSnd_StreamOpen(int index, const char *path, bool isMp3, bool looping,
                        int *channels, int *rate, uint64_t *totalFrames);
void VitaSnd_StreamClose(int index);
bool VitaSnd_StreamIsOpen(int index);
bool VitaSnd_StreamHasData(int index);
void VitaSnd_StreamSeekFrame(int index, uint64_t frame);
uint64_t VitaSnd_StreamPlayedFrames(int index);

#endif
