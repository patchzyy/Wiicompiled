#include "music_attenuation.h"

#include "memory.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#endif

namespace MusicAttenuation {
namespace {

constexpr uint32_t kSoundManagerPointer = 0x809C2898u;
constexpr uint32_t kArchivePlayerOffset = 0x5BCu;
constexpr uint32_t kSoundPlayersOffset = 0x34u;
constexpr uint32_t kSoundPlayerVolumeOffset = 0x2Cu;
constexpr uint32_t kSoundPlayerStride = 0x5Cu;
// The PAL revo_kart.brsar allocates 12 SoundPlayers, with player 11 being
// PL_MAX rather than a playable bus. The game's PlayersVolumeMgr likewise
// owns 11 volume tracks, so the usable buses are players 0 through 10.
constexpr size_t kSoundPlayerCount = 11;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_externalMediaPlaying{false};
std::atomic<bool> g_mediaControlAvailable{false};
std::atomic<bool> g_mediaControlInitializationComplete{false};
std::atomic<float> g_musicVolume{1.0f};
std::atomic<float> g_soundEffectsVolume{1.0f};
std::atomic<float> g_uiVolume{1.0f};
std::atomic<float> g_voicesVolume{1.0f};
std::once_flag g_monitorStart;

// These fields are touched only from the guest scheduler thread.
uint32_t g_soundPlayerArray = 0;
std::array<float, kSoundPlayerCount> g_requestedSoundPlayerVolumes{};
std::array<float, kSoundPlayerCount> g_lastAppliedSoundPlayerVolumes{};
std::array<bool, kSoundPlayerCount> g_haveSoundPlayerVolumes{};

float ClampSoundPlayerVolume(float volume) noexcept {
    // Match nw4r::snd::SoundPlayer::SetVolume at 0x800A35E0 exactly,
    // including its NaN behavior (unordered compares select the upper bound).
    if (volume <= 1.0f) {
        return volume < 0.0f ? 0.0f : volume;
    }
    return 1.0f;
}

bool WriteGuestFloat(uint32_t address, float value) noexcept {
    try {
        Memory::Write32(address, std::bit_cast<uint32_t>(value));
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

bool ReadGuestFloat(uint32_t address, float& value) noexcept {
    uint32_t bits = 0;
    if (!Memory::TryRead32(address, bits)) {
        return false;
    }
    value = std::bit_cast<float>(bits);
    return true;
}

uint32_t ResolveSoundPlayerArray() noexcept {
    uint32_t soundManager = 0;
    uint32_t archivePlayer = 0;
    uint32_t soundPlayers = 0;
    if (!Memory::TryRead32(kSoundManagerPointer, soundManager) || soundManager == 0 ||
        !Memory::TryRead32(soundManager + kArchivePlayerOffset, archivePlayer) || archivePlayer == 0 ||
        !Memory::TryRead32(archivePlayer + kSoundPlayersOffset, soundPlayers)) {
        return 0;
    }

    // revo_kart.brsar names player 0 YPPL_BGM. Players 1-10 are the
    // independent UI, object, engine, race, vehicle, and voice players.
    return soundPlayers;
}

enum class SoundCategory {
    Music,
    SoundEffects,
    Ui,
    Voices,
};

SoundCategory CategoryForPlayer(size_t playerIndex) noexcept {
    switch (playerIndex) {
    case 0:
        return SoundCategory::Music;
    case 1:
    case 7:
        return SoundCategory::Ui;
    case 8:
    case 9:
    case 10:
        return SoundCategory::Voices;
    default:
        return SoundCategory::SoundEffects;
    }
}

float CategoryVolume(size_t playerIndex) noexcept {
    switch (CategoryForPlayer(playerIndex)) {
    case SoundCategory::Music:
        return g_musicVolume.load(std::memory_order_acquire);
    case SoundCategory::SoundEffects:
        return g_soundEffectsVolume.load(std::memory_order_acquire);
    case SoundCategory::Ui:
        return g_uiVolume.load(std::memory_order_acquire);
    case SoundCategory::Voices:
        return g_voicesVolume.load(std::memory_order_acquire);
    }
    return 1.0f;
}

bool FindSoundPlayerIndex(uint32_t soundPlayer, uint32_t soundPlayerArray, size_t& playerIndex) noexcept {
    if (soundPlayer == 0 || soundPlayerArray == 0 || soundPlayer < soundPlayerArray) {
        return false;
    }
    const uint32_t offset = soundPlayer - soundPlayerArray;
    if (offset % kSoundPlayerStride != 0) {
        return false;
    }
    const size_t index = static_cast<size_t>(offset / kSoundPlayerStride);
    if (index >= kSoundPlayerCount) {
        return false;
    }
    playerIndex = index;
    return true;
}

float AppliedSoundPlayerVolume(size_t playerIndex, float requestedVolume, bool attenuated) noexcept {
    if (playerIndex == 0 && attenuated) {
        return 0.0f;
    }
    return requestedVolume * CategoryVolume(playerIndex);
}

bool ShouldAttenuate() noexcept {
    return g_enabled.load(std::memory_order_acquire) &&
           g_externalMediaPlaying.load(std::memory_order_acquire);
}

#if defined(_WIN32)
void MonitorWindowsMediaSessions() noexcept {
    using namespace winrt::Windows::Media::Control;
    using namespace std::chrono_literals;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        g_mediaControlAvailable.store(true, std::memory_order_release);
        g_mediaControlInitializationComplete.store(true, std::memory_order_release);

        for (;;) {
            bool playing = false;
            try {
                for (const auto& session : manager.GetSessions()) {
                    const auto info = session.GetPlaybackInfo();
                    if (info.PlaybackStatus() ==
                        GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                        playing = true;
                        break;
                    }
                }
            } catch (const winrt::hresult_error&) {
                // Sessions can disappear between enumeration and the playback
                // query. Treat that sample as inactive and retry shortly.
            }
            g_externalMediaPlaying.store(playing, std::memory_order_release);
            std::this_thread::sleep_for(250ms);
        }
    } catch (const winrt::hresult_error& error) {
        g_mediaControlAvailable.store(false, std::memory_order_release);
        g_externalMediaPlaying.store(false, std::memory_order_release);
        g_mediaControlInitializationComplete.store(true, std::memory_order_release);
        std::wcerr << L"[audio] Windows media-session monitoring unavailable: "
                   << error.message().c_str() << std::endl;
    }
}
#endif

void StartMonitor() noexcept {
#if defined(_WIN32)
    // The process owns this monitor for its remaining lifetime. Keeping it
    // detached avoids shutdown ordering between WinRT and static audio state.
    std::thread(MonitorWindowsMediaSessions).detach();
#else
    g_mediaControlAvailable.store(false, std::memory_order_release);
    g_mediaControlInitializationComplete.store(true, std::memory_order_release);
#endif
}

} // namespace

void SetEnabled(bool enabled) noexcept {
    if (enabled) {
        std::call_once(g_monitorStart, StartMonitor);
    }
    g_enabled.store(enabled, std::memory_order_release);
}

bool IsExternalMediaPlaying() noexcept {
    return g_externalMediaPlaying.load(std::memory_order_acquire);
}

bool IsMediaControlAvailable() noexcept {
    return g_mediaControlAvailable.load(std::memory_order_acquire);
}

bool IsMediaControlInitializationComplete() noexcept {
    return g_mediaControlInitializationComplete.load(std::memory_order_acquire);
}

void SetMusicVolume(float volume) noexcept {
    g_musicVolume.store(ClampSoundPlayerVolume(volume), std::memory_order_release);
}

void SetSoundEffectsVolume(float volume) noexcept {
    g_soundEffectsVolume.store(ClampSoundPlayerVolume(volume), std::memory_order_release);
}

void SetUiVolume(float volume) noexcept {
    g_uiVolume.store(ClampSoundPlayerVolume(volume), std::memory_order_release);
}

void SetVoicesVolume(float volume) noexcept {
    g_voicesVolume.store(ClampSoundPlayerVolume(volume), std::memory_order_release);
}

void TickGuest() noexcept {
    const bool attenuated = ShouldAttenuate();
    const uint32_t soundPlayerArray = ResolveSoundPlayerArray();
    if (soundPlayerArray == 0) {
        g_soundPlayerArray = 0;
        g_haveSoundPlayerVolumes.fill(false);
        return;
    }

    if (soundPlayerArray != g_soundPlayerArray) {
        g_soundPlayerArray = soundPlayerArray;
        g_haveSoundPlayerVolumes.fill(false);
    }

    for (size_t playerIndex = 0; playerIndex < kSoundPlayerCount; ++playerIndex) {
        const uint32_t soundPlayer = soundPlayerArray + static_cast<uint32_t>(playerIndex) * kSoundPlayerStride;
        float currentVolume = 0.0f;
        if (!ReadGuestFloat(soundPlayer + kSoundPlayerVolumeOffset, currentVolume)) {
            continue;
        }

        // Direct/generated stores can bypass the SetVolume override. When the
        // observed value is not the value last written by this layer, treat it
        // as the game's new base volume and multiply it on the way out. This
        // also preserves the requested BGM volume while it is muted at zero.
        if (!g_haveSoundPlayerVolumes[playerIndex] ||
            currentVolume != g_lastAppliedSoundPlayerVolumes[playerIndex]) {
            g_requestedSoundPlayerVolumes[playerIndex] = ClampSoundPlayerVolume(currentVolume);
            g_haveSoundPlayerVolumes[playerIndex] = true;
        }

        const float applied = AppliedSoundPlayerVolume(
            playerIndex, g_requestedSoundPlayerVolumes[playerIndex], attenuated);
        if (currentVolume != applied &&
            WriteGuestFloat(soundPlayer + kSoundPlayerVolumeOffset, applied)) {
            g_lastAppliedSoundPlayerVolumes[playerIndex] = applied;
        } else if (currentVolume == applied) {
            g_lastAppliedSoundPlayerVolumes[playerIndex] = applied;
        }
    }
}

void SetSoundPlayerVolume(uint32_t soundPlayer, float requestedVolume) {
    const float clamped = ClampSoundPlayerVolume(requestedVolume);
    float applied = clamped;
    const uint32_t soundPlayerArray = ResolveSoundPlayerArray();
    size_t playerIndex = 0;
    if (FindSoundPlayerIndex(soundPlayer, soundPlayerArray, playerIndex)) {
        if (soundPlayerArray != g_soundPlayerArray) {
            g_soundPlayerArray = soundPlayerArray;
            g_haveSoundPlayerVolumes.fill(false);
        }
        g_requestedSoundPlayerVolumes[playerIndex] = clamped;
        g_haveSoundPlayerVolumes[playerIndex] = true;
        applied = AppliedSoundPlayerVolume(playerIndex, clamped, ShouldAttenuate());
        g_lastAppliedSoundPlayerVolumes[playerIndex] = applied;
    }
    // Preserve the original function's access semantics: write back to the guest object
    // regardless of whether it was found in the tracked array. Routed through WriteGuestFloat
    // (as every other guest write in this file is) so a stale/unmapped `soundPlayer` pointer -
    // e.g. a scene-teardown race - can't throw an uncaught Memory::AccessViolation through this
    // native-override boundary and take down the whole runtime.
    WriteGuestFloat(soundPlayer + kSoundPlayerVolumeOffset, applied);
}

} // namespace MusicAttenuation
