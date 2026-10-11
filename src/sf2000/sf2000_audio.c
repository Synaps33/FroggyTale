#include "sf2000_audio.h"
#include "audio_system.h"
#include "data_win.h"
#include "utils.h"
#include "binary_utils.h"
#include "stb_ds.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations for stb_vorbis (compiled separately in VENDOR_SRCS) */
typedef struct stb_vorbis stb_vorbis;
typedef struct {
   char *alloc_buffer;
   int   alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

typedef struct {
   unsigned int sample_rate;
   int channels;
   unsigned int setup_memory_required;
   unsigned int setup_temp_memory_required;
   unsigned int temp_memory_required;
   int max_frame_size;
} stb_vorbis_info;

extern stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const stb_vorbis_alloc *alloc_buffer);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
extern int stb_vorbis_seek_start(stb_vorbis *f);
extern void stb_vorbis_close(stb_vorbis *f);

/* SF2000 hardware sample rate */
#define SF2000_AUDIO_RATE 22050
#define SF2000_AUDIO_CHANNELS 2

/* 22050 Hz / 60 fps ~= 368 frames per tick */
#define SAMPLES_PER_FRAME (SF2000_AUDIO_RATE / 60)

#define AUDIO_STREAM_INDEX_BASE 300000
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_SOUND_INSTANCES 16
#define MAX_AUDIO_STREAMS 16

static inline uint16_t readU16LE(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t readU32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* WAV header parser helper */
static bool parseWavHeader(const uint8_t* data, uint32_t size,
                          const uint8_t** outAudioData, uint32_t* outAudioDataLen,
                          int32_t* outChannels, int32_t* outSampleRate, int32_t* outBitsPerSample) {
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint32_t offset = 12;
    *outAudioData = NULL;
    *outAudioDataLen = 0;
    *outChannels = 0;
    *outSampleRate = 0;
    *outBitsPerSample = 0;

    while (offset + 8 <= size) {
        char chunkId[4];
        memcpy(chunkId, data + offset, 4);
        uint32_t chunkLen = readU32LE(data + offset + 4);
        offset += 8;

        if (offset + chunkLen > size) break;

        if (memcmp(chunkId, "fmt ", 4) == 0 && chunkLen >= 16) {
            *outChannels = readU16LE(data + offset + 2);
            *outSampleRate = (int32_t)readU32LE(data + offset + 4);
            *outBitsPerSample = readU16LE(data + offset + 14);
        } else if (memcmp(chunkId, "data", 4) == 0) {
            *outAudioData = data + offset;
            *outAudioDataLen = chunkLen;
            break;
        }
        offset += chunkLen;
    }
    return (*outAudioData != NULL && *outAudioDataLen > 0 &&
            *outChannels > 0 && *outSampleRate > 0 && *outBitsPerSample == 16);
}

typedef struct {
    bool active;
    char* filePath;
    float initialGain;
    float initialPitch;
} AudioStreamEntry;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    int32_t instanceId;
    int32_t soundIndex;
    float gain;
    float pitch;

    /* For embedded SFX */
    const int16_t* pcmData;
    uint32_t totalSamples; /* per channel */
    uint32_t samplePos;
    uint32_t samplePosFrac;
    int32_t channels;
    int32_t sampleRate;

    /* For streamed BGM (OGG Vorbis) */
    bool isStream;
    stb_vorbis* vorbis;
    int16_t streamScratch[1024];
    int32_t scratchSamples;
    int32_t scratchPos;
} SoundSlot;

struct Sf2000AudioSystem {
    AudioSystem base;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
    SoundSlot slots[MAX_SOUND_INSTANCES];
    int32_t nextInstanceId;
    float masterGain;
};

static Sf2000AudioSystem* s_audio = NULL;

/* External callback to libretro frontend in main.c */
extern void sf2000_send_audio(const int16_t *samples, unsigned frames);

static void sf2000AudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    sys->base.dw = dataWin;
    sys->fileSystem = fileSystem;
    arrput(sys->base.audioGroups, dataWin);
}

static void sf2000AudioDestroy(AudioSystem* audio) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->slots[i].isStream && sys->slots[i].vorbis) {
            stb_vorbis_close(sys->slots[i].vorbis);
            sys->slots[i].vorbis = NULL;
        }
    }
    for (int i = 0; i < MAX_AUDIO_STREAMS; i++) {
        if (sys->streams[i].active && sys->streams[i].filePath) {
            free(sys->streams[i].filePath);
            sys->streams[i].filePath = NULL;
        }
    }
    free(audio->groupGains);
    arrfree(audio->audioGroups);
    s_audio = NULL;
    free(audio);
}

static void sf2000AudioUpdate(AudioSystem* audio, float deltaTime) {
    AudioSystem_updateGroupGains(audio, deltaTime);
}

static int32_t sf2000AudioCreateStream(AudioSystem* audio, const char* filename) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    int32_t slot = -1;
    for (int i = 0; i < MAX_AUDIO_STREAMS; i++) {
        if (!sys->streams[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    char* resolved = sys->fileSystem ? sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename) : NULL;
    if (!resolved) {
        resolved = safeStrdup(filename);
    }

    sys->streams[slot].active = true;
    sys->streams[slot].filePath = resolved;
    sys->streams[slot].initialGain = 1.0f;
    sys->streams[slot].initialPitch = 1.0f;

    return AUDIO_STREAM_INDEX_BASE + slot;
}

static bool sf2000AudioDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    int32_t slot = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= MAX_AUDIO_STREAMS) return false;
    if (!sys->streams[slot].active) return false;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->slots[i].active && sys->slots[i].soundIndex == streamIndex) {
            if (sys->slots[i].isStream && sys->slots[i].vorbis) {
                stb_vorbis_close(sys->slots[i].vorbis);
                sys->slots[i].vorbis = NULL;
            }
            sys->slots[i].active = false;
        }
    }

    if (sys->streams[slot].filePath) {
        free(sys->streams[slot].filePath);
        sys->streams[slot].filePath = NULL;
    }
    sys->streams[slot].active = false;
    return true;
}

static SoundSlot* findFreeSlot(Sf2000AudioSystem* sys) {
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (!sys->slots[i].active) return &sys->slots[i];
    }
    /* Steal oldest non-streaming slot if full */
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (!sys->slots[i].isStream) {
            return &sys->slots[i];
        }
    }
    return &sys->slots[0];
}

static int32_t sf2000AudioPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    (void)priority;
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    DataWin* dw = audio->dw;
    if (!dw) return -1;

    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    const char* streamPath = NULL;
    Sound* sound = NULL;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (streamSlot < 0 || streamSlot >= MAX_AUDIO_STREAMS || !sys->streams[streamSlot].active) {
            return -1;
        }
        streamPath = sys->streams[streamSlot].filePath;
    } else {
        if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) {
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundSlot* slot = findFreeSlot(sys);
    if (!slot) return -1;

    if (slot->active && slot->isStream && slot->vorbis) {
        stb_vorbis_close(slot->vorbis);
        slot->vorbis = NULL;
    }

    slot->active = false;
    slot->paused = false;
    slot->loop = loop;
    slot->instanceId = sys->nextInstanceId++;
    slot->soundIndex = soundIndex;
    slot->samplePos = 0;
    slot->samplePosFrac = 0;
    slot->gain = 1.0f;
    slot->pitch = 1.0f;

    if (isStream) {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(streamPath, &err, NULL);
        if (!v) return -1;

        stb_vorbis_info info = stb_vorbis_get_info(v);
        slot->isStream = true;
        slot->vorbis = v;
        slot->channels = info.channels;
        slot->sampleRate = (int32_t)info.sample_rate;
        slot->scratchSamples = 0;
        slot->scratchPos = 0;
        slot->active = true;
        return slot->instanceId;
    }

    /* Embedded SFX path */
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    if (inAudo && sound->audioFile >= 0 && (uint32_t)sound->audioFile < dw->audo.count) {
        DataWin_loadAudoIfNeeded(dw, (uint32_t)sound->audioFile);
        AudioEntry* entry = &dw->audo.entries[sound->audioFile];
        if (entry->present && entry->data) {
            const uint8_t* audioData = NULL;
            uint32_t audioLen = 0;
            int32_t ch = 0, rate = 0, bits = 0;
            if (parseWavHeader(entry->data, entry->dataSize, &audioData, &audioLen, &ch, &rate, &bits)) {
                slot->isStream = false;
                slot->pcmData = (const int16_t*)audioData;
                slot->channels = ch;
                slot->sampleRate = rate;
                slot->totalSamples = audioLen / (ch * sizeof(int16_t));
                slot->gain = sound->volume;
                slot->pitch = sound->pitch;
                slot->active = true;
                return slot->instanceId;
            }
        }
    }

    return -1;
}

static void sf2000AudioStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            if (slot->isStream && slot->vorbis) {
                stb_vorbis_close(slot->vorbis);
                slot->vorbis = NULL;
            }
            slot->active = false;
        }
    }
}

static void sf2000AudioStopAll(AudioSystem* audio) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active) {
            if (slot->isStream && slot->vorbis) {
                stb_vorbis_close(slot->vorbis);
                slot->vorbis = NULL;
            }
            slot->active = false;
        }
    }
}

static bool sf2000AudioIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            return !slot->paused;
        }
    }
    return false;
}

static void sf2000AudioPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            slot->paused = true;
        }
    }
}

static void sf2000AudioResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            slot->paused = false;
        }
    }
}

static void sf2000AudioPauseAll(AudioSystem* audio) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->slots[i].active) sys->slots[i].paused = true;
    }
}

static void sf2000AudioResumeAll(AudioSystem* audio) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->slots[i].active) sys->slots[i].paused = false;
    }
}

static void sf2000AudioSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    (void)timeMs;
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            slot->gain = gain;
        }
    }
}

static float sf2000AudioGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            return slot->gain;
        }
    }
    return 1.0f;
}

static void sf2000AudioSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            slot->pitch = pitch;
        }
    }
}

static float sf2000AudioGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &sys->slots[i];
        if (slot->active && (slot->instanceId == soundOrInstance || slot->soundIndex == soundOrInstance)) {
            return slot->pitch;
        }
    }
    return 1.0f;
}

static void sf2000AudioSetMasterGain(AudioSystem* audio, float gain) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)audio;
    sys->masterGain = gain;
}

static void noopSetSpatial(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t id, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float z, MAYBE_UNUSED float ref, MAYBE_UNUSED float max, MAYBE_UNUSED float factor) {}
static void noopSetListener(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float z) {}
static float noopGetPos(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t id) { return 0.0f; }
static void noopSetPos(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t id, MAYBE_UNUSED float pos) {}
static float noopGetLength(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t id) { return 1.0f; }
static void noopSetChannelCount(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t c) {}
static void noopSetMasterGainForListener(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED float gain, MAYBE_UNUSED int32_t listenerId) {}
static void noopSetGroupGain(AudioSystem* audio, int32_t groupIndex, float gain, uint32_t timeMs) {
    AudioSystem_setGroupGain(audio, groupIndex, gain, timeMs);
}
static void noopGroupLoad(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t g) {}
static bool noopGroupIsLoaded(MAYBE_UNUSED AudioSystem* a, MAYBE_UNUSED int32_t g) { return true; }

static AudioSystemVtable s_sf2000AudioVtable = {
    .init = sf2000AudioInit,
    .destroy = sf2000AudioDestroy,
    .update = sf2000AudioUpdate,
    .playSound = sf2000AudioPlaySound,
    .setSoundSpatial = noopSetSpatial,
    .setListenerPosition = noopSetListener,
    .stopSound = sf2000AudioStopSound,
    .stopAll = sf2000AudioStopAll,
    .isPlaying = sf2000AudioIsPlaying,
    .pauseSound = sf2000AudioPauseSound,
    .resumeSound = sf2000AudioResumeSound,
    .pauseAll = sf2000AudioPauseAll,
    .resumeAll = sf2000AudioResumeAll,
    .suspend = sf2000AudioPauseAll,
    .resume = sf2000AudioResumeAll,
    .setSoundGain = sf2000AudioSetSoundGain,
    .getSoundGain = sf2000AudioGetSoundGain,
    .setSoundPitch = sf2000AudioSetSoundPitch,
    .getSoundPitch = sf2000AudioGetSoundPitch,
    .getTrackPosition = noopGetPos,
    .setTrackPosition = noopSetPos,
    .getSoundLength = noopGetLength,
    .setMasterGain = sf2000AudioSetMasterGain,
    .setMasterGainForListener = noopSetMasterGainForListener,
    .setChannelCount = noopSetChannelCount,
    .setGroupGain = noopSetGroupGain,
    .groupLoad = noopGroupLoad,
    .groupIsLoaded = noopGroupIsLoaded,
    .createStream = sf2000AudioCreateStream,
    .destroyStream = sf2000AudioDestroyStream,
};

Sf2000AudioSystem* Sf2000AudioSystem_create(DataWin* dataWin) {
    Sf2000AudioSystem* sys = (Sf2000AudioSystem*)safeCalloc(1, sizeof(Sf2000AudioSystem));
    sys->base.vtable = &s_sf2000AudioVtable;
    sys->base.dw = dataWin;
    sys->nextInstanceId = SOUND_INSTANCE_ID_BASE;
    sys->masterGain = 1.0f;
    s_audio = sys;
    return sys;
}

/* Mix buffer for 1 video frame */
static int32_t s_mixAccum[SAMPLES_PER_FRAME * SF2000_AUDIO_CHANNELS];
static int16_t s_mixOut[SAMPLES_PER_FRAME * SF2000_AUDIO_CHANNELS];

void Sf2000AudioSystem_renderFrame(void) {
    if (!s_audio) return;

    memset(s_mixAccum, 0, sizeof(s_mixAccum));

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundSlot* slot = &s_audio->slots[i];
        if (!slot->active || slot->paused) continue;

        float effectiveGain = slot->gain * s_audio->masterGain;
        if (!slot->isStream) {
            effectiveGain *= AudioSystem_soundGroupGain(&s_audio->base, slot->soundIndex);
        }
        int32_t gainQ12 = (int32_t)(effectiveGain * 4096.0f);
        if (gainQ12 <= 0) continue;

        if (slot->isStream) {
            /* Streaming OGG Vorbis */
            stb_vorbis* v = slot->vorbis;
            if (!v) continue;

            for (int s = 0; s < SAMPLES_PER_FRAME; s++) {
                if (slot->scratchPos >= slot->scratchSamples) {
                    int n = stb_vorbis_get_samples_short_interleaved(v, slot->channels,
                                                                     slot->streamScratch,
                                                                     1024);
                    if (n <= 0) {
                        if (slot->loop) {
                            stb_vorbis_seek_start(v);
                            n = stb_vorbis_get_samples_short_interleaved(v, slot->channels,
                                                                         slot->streamScratch,
                                                                         1024);
                        } else {
                            slot->active = false;
                            break;
                        }
                    }
                    slot->scratchSamples = n;
                    slot->scratchPos = 0;
                }

                int16_t left, right;
                if (slot->channels == 1) {
                    left = right = slot->streamScratch[slot->scratchPos++];
                } else {
                    left = slot->streamScratch[slot->scratchPos++];
                    right = slot->streamScratch[slot->scratchPos++];
                }

                s_mixAccum[s * 2]     += (left * gainQ12) >> 12;
                s_mixAccum[s * 2 + 1] += (right * gainQ12) >> 12;
            }
        } else {
            /* Embedded SFX */
            if (!slot->pcmData) continue;

            /* Fixed point resampling ratio: (sampleRate * pitch) / 22050 */
            uint32_t step = (uint32_t)(((uint64_t)slot->sampleRate * (uint32_t)(slot->pitch * 65536.0f)) / SF2000_AUDIO_RATE);
            if (step == 0) step = 65536;

            for (int s = 0; s < SAMPLES_PER_FRAME; s++) {
                if (slot->samplePos >= slot->totalSamples) {
                    if (slot->loop) {
                        slot->samplePos = 0;
                    } else {
                        slot->active = false;
                        break;
                    }
                }

                int16_t left, right;
                if (slot->channels == 1) {
                    left = right = slot->pcmData[slot->samplePos];
                } else {
                    left  = slot->pcmData[slot->samplePos * 2];
                    right = slot->pcmData[slot->samplePos * 2 + 1];
                }

                s_mixAccum[s * 2]     += (left * gainQ12) >> 12;
                s_mixAccum[s * 2 + 1] += (right * gainQ12) >> 12;

                slot->samplePosFrac += step;
                slot->samplePos += (slot->samplePosFrac >> 16);
                slot->samplePosFrac &= 0xFFFF;
            }
        }
    }

    /* Clamp to 16-bit PCM and send to frontend */
    for (int i = 0; i < SAMPLES_PER_FRAME * SF2000_AUDIO_CHANNELS; i++) {
        int32_t val = s_mixAccum[i];
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        s_mixOut[i] = (int16_t)val;
    }

    sf2000_send_audio(s_mixOut, SAMPLES_PER_FRAME);
}
