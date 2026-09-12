#pragma once

namespace MusicAttenuation {

struct MacOSAudioStatus {
    bool available = false;
    bool playing = false;
};

// Queries output activity without capturing audio. Requires Core Audio process
// objects (macOS 14.2+); older systems return an unavailable sample.
MacOSAudioStatus QueryMacOSExternalAudio() noexcept;

} // namespace MusicAttenuation
