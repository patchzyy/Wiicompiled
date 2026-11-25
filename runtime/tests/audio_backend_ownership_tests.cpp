#include "audio_backend.h"
#include <SDL3/SDL_init.h>

#include <iostream>
#include <stdexcept>

struct SDL_AudioStream { int id; };

namespace {
int references = 1;
int initCalls = 0;
int quitCalls = 0;
int streams = 0;
int opens = 0;
bool failInit = false;
bool failOpen = false;
bool failResume = false;
bool failGain = false;
float gain = 1.0f;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

extern "C" const char* SDL_GetError() { return "injected failure"; }
extern "C" bool SDL_InitSubSystem(uint32_t flags) {
    Require(flags == SDL_INIT_AUDIO, "unexpected subsystem");
    ++initCalls;
    if (failInit) return false;
    ++references;
    return true;
}
extern "C" void SDL_QuitSubSystem(uint32_t) { --references; ++quitCalls; }
extern "C" SDL_AudioStream* SDL_OpenAudioDeviceStream(
    SDL_AudioDeviceID, const SDL_AudioSpec* spec, SDL_AudioStreamCallback, void*) {
    ++opens;
    Require(references == 2 && spec->channels == 2, "stream must own an audio reference");
    if (failOpen) return nullptr;
    ++streams;
    return new SDL_AudioStream{opens};
}
extern "C" void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    Require(stream && streams == 1, "stream destroyed twice");
    --streams;
    delete stream;
}
extern "C" bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*) { return !failResume; }
extern "C" bool SDL_SetAudioStreamGain(SDL_AudioStream*, float value) {
    gain = value;
    return !failGain;
}
extern "C" int SDL_GetAudioStreamQueued(SDL_AudioStream*) { return 0; }
extern "C" bool SDL_PutAudioStreamData(SDL_AudioStream*, const void*, int) { return true; }

int main() {
    try {
        auto& audio = AudioBackend::Instance();
        const int16_t samples[] = {1, 2};
        audio.Shutdown();
        Require(references == 1 && quitCalls == 0, "shutdown released somebody else's reference");
        failInit = true;
        Require(!audio.Init(32000, 2), "failed subsystem init accepted");
        audio.Shutdown();
        Require(references == 1 && quitCalls == 0, "failed init acquired ownership");
        failInit = false;
        failOpen = true;
        Require(!audio.Init(32000, 2), "failed open accepted");
        audio.Shutdown();
        Require(references == 1 && quitCalls == 1, "failed open leaked subsystem reference");
        failOpen = false;
        Require(audio.Init(32000, 2), "initial stream failed");
        const int initialized = initCalls;
        const int opened = opens;
        Require(audio.Init(32000, 2) && opens == opened, "same format reopened stream");
        Require(audio.Init(48000, 2) && initCalls == initialized && streams == 1,
                "format replacement acquired another subsystem reference");
        failOpen = true;
        Require(!audio.Init(32000, 2) && streams == 0, "failed replacement retained a stream");
        Require(!audio.PushSamplesLE16(samples, 2), "failed replacement stayed ready");
        failOpen = false;
        Require(audio.Init(48000, 2) && streams == 1, "old format falsely reported ready");
        for (int failure = 0; failure < 2; ++failure) {
            failResume = failure == 0;
            failGain = failure == 1;
            Require(!audio.Init(32000, 2) && streams == 0, "failed setup leaked a stream");
            failResume = failGain = false;
            Require(audio.Init(48000, 2) && initCalls == initialized,
                    "retry lost or duplicated subsystem ownership");
        }
        audio.SetMasterVolume(0.25f);
        audio.SetSilenced(true);
        Require(gain == 0.0f && audio.Init(32000, 2) && gain == 0.0f,
                "format replacement lost silence");
        audio.SetSilenced(false);
        Require(gain == 0.25f, "silence lost user volume");
        audio.Shutdown();
        audio.Shutdown();
        Require(streams == 0 && references == 1 && quitCalls == 2, "shutdown was unbalanced");
        Require(audio.Init(32000, 2), "shutdown/init failed");
        audio.Shutdown();
        Require(references == 1 && quitCalls == 3, "new lifetime was unbalanced");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
