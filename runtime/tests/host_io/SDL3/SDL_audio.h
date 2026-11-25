#pragma once

#include <cstdint>

using SDL_AudioDeviceID = uint32_t;
using SDL_AudioFormat = uint32_t;
struct SDL_AudioStream;
struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
};
using SDL_AudioStreamCallback = void (*)(void*, SDL_AudioStream*, int, int);
inline constexpr SDL_AudioFormat SDL_AUDIO_S16LE = 0x8010u;
inline constexpr SDL_AudioDeviceID SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFFu;

extern "C" {
const char* SDL_GetError();
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec*,
                                           SDL_AudioStreamCallback, void*);
void SDL_DestroyAudioStream(SDL_AudioStream*);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*);
bool SDL_SetAudioStreamGain(SDL_AudioStream*, float);
int SDL_GetAudioStreamQueued(SDL_AudioStream*);
bool SDL_PutAudioStreamData(SDL_AudioStream*, const void*, int);
}
