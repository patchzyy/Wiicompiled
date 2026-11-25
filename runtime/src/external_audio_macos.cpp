#if defined(__APPLE__)

#include "external_audio_macos.h"

#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <vector>

namespace MusicAttenuation {
namespace {

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140200
constexpr auto kProcessObjectList = kAudioHardwarePropertyProcessObjectList;
constexpr auto kProcessPid = kAudioProcessPropertyPID;
constexpr auto kProcessRunningOutput = kAudioProcessPropertyIsRunningOutput;
#else
// Public Core Audio selector ABI values, for SDKs predating process objects.
// The runtime HasProperty check still determines whether the OS supports them.
constexpr AudioObjectPropertySelector kProcessObjectList = 'prs#';
constexpr AudioObjectPropertySelector kProcessPid = 'ppid';
constexpr AudioObjectPropertySelector kProcessRunningOutput = 'piro';
#endif

template <typename T>
bool ReadAudioProcessProperty(AudioObjectID object, AudioObjectPropertySelector selector,
                              T& value) noexcept {
    const AudioObjectPropertyAddress address{
        selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr &&
           size == sizeof(value);
}

} // namespace

MacOSAudioStatus QueryMacOSExternalAudio() noexcept {
    const AudioObjectPropertyAddress address{
        kProcessObjectList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    // Probe the property instead of raising the game's minimum macOS version.
    if (!AudioObjectHasProperty(kAudioObjectSystemObject, &address)) {
        return {};
    }

    // Processes may start between the size and data queries. Retry a changed
    // list a bounded number of times; subsequent monitor polls also retry.
    for (int attempt = 0; attempt < 3; ++attempt) {
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                          0, nullptr, &size) != noErr ||
            size % sizeof(AudioObjectID) != 0) {
            return {};
        }
        if (size == 0) {
            return {true, false};
        }
        std::vector<AudioObjectID> processes(size / sizeof(AudioObjectID));
        const auto result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                       0, nullptr, &size, processes.data());
        if (result == kAudioHardwareBadPropertySizeError) {
            continue;
        }
        if (result != noErr || size % sizeof(AudioObjectID) != 0 ||
            size / sizeof(AudioObjectID) > processes.size()) {
            return {};
        }
        const pid_t ownPid = getpid();
        for (size_t index = 0; index < size / sizeof(AudioObjectID); ++index) {
            pid_t pid = 0;
            UInt32 runningOutput = 0;
            // An exiting process may disappear mid-query. Never count an
            // unknown PID or our own SDL output as external playback.
            if (ReadAudioProcessProperty(processes[index], kProcessPid, pid) &&
                pid > 0 && pid != ownPid &&
                ReadAudioProcessProperty(processes[index], kProcessRunningOutput,
                                         runningOutput) && runningOutput != 0) {
                return {true, true};
            }
        }
        return {true, false};
    }
    return {};
}

} // namespace MusicAttenuation

#endif
