#include "external_audio_macos.h"

#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Process {
    pid_t pid;
    UInt32 output;
    bool disappeared = false;
};
std::vector<Process> processes;
bool supported = true;
bool queryFailed = false;
int sizeRaces = 0;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Expect(bool available, bool playing, const char* message) {
    const auto status = MusicAttenuation::QueryMacOSExternalAudio();
    Require(status.available == available && status.playing == playing, message);
}
} // namespace

// Fake only the Core Audio boundary so the production enumeration and process
// filtering run unchanged, without depending on other apps on the test machine.
// Use the public selector ABI values so these mocks also build with older SDKs.
extern "C" Boolean AudioObjectHasProperty(AudioObjectID object,
                                         const AudioObjectPropertyAddress* address) {
    Require(object == kAudioObjectSystemObject &&
            address->mSelector == 'prs#',
            "Must query the system process list");
    return supported;
}

extern "C" OSStatus AudioObjectGetPropertyDataSize(
    AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32* size) {
    if (queryFailed) return kAudioHardwareUnspecifiedError;
    *size = static_cast<UInt32>(processes.size() * sizeof(AudioObjectID));
    return noErr;
}

extern "C" OSStatus AudioObjectGetPropertyData(
    AudioObjectID object, const AudioObjectPropertyAddress* address, UInt32,
    const void*, UInt32* size, void* data) {
    Require(address->mScope == kAudioObjectPropertyScopeGlobal, "Must use global scope");
    if (object == kAudioObjectSystemObject) {
        if (sizeRaces > 0) {
            --sizeRaces;
            return kAudioHardwareBadPropertySizeError;
        }
        for (size_t i = 0; i < processes.size(); ++i) {
            static_cast<AudioObjectID*>(data)[i] = static_cast<AudioObjectID>(i + 100);
        }
        *size = static_cast<UInt32>(processes.size() * sizeof(AudioObjectID));
        return noErr;
    }
    const auto& process = processes.at(object - 100);
    if (process.disappeared) return kAudioHardwareBadObjectError;
    if (address->mSelector == 'ppid') {
        std::memcpy(data, &process.pid, sizeof(process.pid));
        *size = sizeof(process.pid);
    } else {
        Require(address->mSelector == 'piro',
                "Input-only activity must not count as playback");
        Require(process.pid != getpid(), "Must exclude the game's own output");
        std::memcpy(data, &process.output, sizeof(process.output));
        *size = sizeof(process.output);
    }
    return noErr;
}

int main() {
    supported = false;
    Expect(false, false, "Older macOS must report unavailable");
    supported = true;
    Expect(true, false, "An empty process list is available but inactive");
    processes = {{getpid(), 1}};
    Expect(true, false, "Game audio alone must not trigger muting");
    processes.push_back({getpid() + 1, 0});
    Expect(true, false, "An idle or input-only app must not trigger muting");
    processes.back().output = 1;
    Expect(true, true, "External playback must trigger muting");
    processes.back().output = 0;
    Expect(true, false, "Stopping playback must restore music");
    processes.back().output = 1;
    processes.back().disappeared = true;
    Expect(true, false, "Exiting apps must not leave music muted");
    processes.push_back({getpid() + 2, 1});
    Expect(true, true, "A disappearing app must not hide another active player");
    sizeRaces = 1;
    Expect(true, true, "A growing process list must be retried");
    sizeRaces = 3;
    Expect(false, false, "List retries must be bounded");
    Expect(true, true, "Later polls must recover from list races");
    queryFailed = true;
    Expect(false, false, "Query failure must clear playing state");
    queryFailed = false;
    Expect(true, true, "Monitoring must recover from query failure");
    processes = {{0, 1}};
    Expect(true, false, "Unknown PIDs must not count as external playback");
    std::cout << "macOS external audio tests passed\n";
}
