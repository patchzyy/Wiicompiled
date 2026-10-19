#pragma once

#include <cstdint>

namespace MusicAttenuation {

// Enables the optional external-media integration (Windows media sessions,
// Core Audio output activity on macOS, or MPRIS over D-Bus on Linux).
// The monitor is started lazily the first time
// this is enabled.
void SetEnabled(bool enabled) noexcept;
void SetMusicVolume(float volume) noexcept;
void SetSoundEffectsVolume(float volume) noexcept;
void SetUiVolume(float volume) noexcept;
void SetVoicesVolume(float volume) noexcept;
bool IsExternalMediaPlaying() noexcept;
bool IsMediaControlAvailable() noexcept;
bool IsMediaControlInitializationComplete() noexcept;

// Called from the guest scheduler/audio path. This applies state changes to
// the live SoundPlayer buses, so changing a category or entering/leaving
// attenuation never requires a scene or race restart.
void TickGuest() noexcept;

// Accurate native replacement for nw4r::snd::SoundPlayer::SetVolume. Only
// the category multipliers are applied here; the game's requested bus volume
// remains the base value.
void SetSoundPlayerVolume(uint32_t soundPlayer, float requestedVolume);

} // namespace MusicAttenuation
