#include "music_attenuation.h"

#include "memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#elif defined(__linux__)
#include <dlfcn.h>

#include <string>
#include <vector>
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

uint32_t FloatBits(float value) noexcept {
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsFloat(uint32_t bits) noexcept {
    static_assert(sizeof(float) == sizeof(uint32_t));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

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
        Memory::Write32(address, FloatBits(value));
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
    value = BitsFloat(bits);
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
#elif defined(__linux__)

// For linux - watches MPRIS players on the D-Bus session bus and drives the
// same g_externalMediaPlaying / g_mediaControl* atomics.
//
// libdbus-1 is loaded with dlopen rather than linked so a machine with no
// session bus (headless, D-Bus absent, some containers) can still run the game,
// here, the monitor reports "unavailable" like the Windows does
// when the media-session API is missing.

extern "C" {

struct MprisDBusConnection;
struct MprisDBusMessage;

// Mirrors DBusError from dbus-errors.h (frozen public layout).
struct MprisDBusError {
    const char* name;
    const char* message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void* padding1;
};

// Mirrors DBusMessageIter from dbus-message.h
struct MprisDBusIter {
    void* dummy1;
    void* dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    int pad2;
    void* pad3;
};

} // extern "C"

constexpr int kMprisBusSession = 0;   // DBUS_BUS_SESSION
constexpr int kMprisTypeInvalid = 0;  // DBUS_TYPE_INVALID
constexpr int kMprisTypeString = 's'; // DBUS_TYPE_STRING
constexpr int kMprisTypeArray = 'a';  // DBUS_TYPE_ARRAY
constexpr int kMprisTypeVariant = 'v';// DBUS_TYPE_VARIANT
// Short blocking timeout so a wedged player can never stall the poll thread for
// the full libdbus default (25s).
constexpr int kMprisSendTimeoutMs = 500;
constexpr char kMprisNamePrefix[] = "org.mpris.MediaPlayer2.";

struct MprisDBus {
    void* handle = nullptr;

    MprisDBusConnection* (*bus_get_private)(int, MprisDBusError*) = nullptr;
    void (*connection_close)(MprisDBusConnection*) = nullptr;
    void (*connection_unref)(MprisDBusConnection*) = nullptr;
    void (*connection_set_exit_on_disconnect)(MprisDBusConnection*, unsigned int) = nullptr;
    void (*error_init)(MprisDBusError*) = nullptr;
    void (*error_free)(MprisDBusError*) = nullptr;
    MprisDBusMessage* (*message_new_method_call)(const char*, const char*, const char*,
                                                const char*) = nullptr;
    unsigned int (*message_append_args)(MprisDBusMessage*, int, ...) = nullptr;
    MprisDBusMessage* (*send_with_reply_and_block)(MprisDBusConnection*, MprisDBusMessage*, int,
                                                  MprisDBusError*) = nullptr;
    void (*message_unref)(MprisDBusMessage*) = nullptr;
    unsigned int (*message_iter_init)(MprisDBusMessage*, MprisDBusIter*) = nullptr;
    unsigned int (*message_iter_next)(MprisDBusIter*) = nullptr;
    void (*message_iter_recurse)(MprisDBusIter*, MprisDBusIter*) = nullptr;
    int (*message_iter_get_arg_type)(MprisDBusIter*) = nullptr;
    void (*message_iter_get_basic)(MprisDBusIter*, void*) = nullptr;

    template <typename Fn>
    bool Bind(Fn& fn, const char* symbol) noexcept {
        fn = reinterpret_cast<Fn>(dlsym(handle, symbol));
        return fn != nullptr;
    }

    bool Load() noexcept {
        handle = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            return false;
        }
        const bool ok =
            Bind(bus_get_private, "dbus_bus_get_private") &&
            Bind(connection_close, "dbus_connection_close") &&
            Bind(connection_unref, "dbus_connection_unref") &&
            Bind(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
            Bind(error_init, "dbus_error_init") &&
            Bind(error_free, "dbus_error_free") &&
            Bind(message_new_method_call, "dbus_message_new_method_call") &&
            Bind(message_append_args, "dbus_message_append_args") &&
            Bind(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
            Bind(message_unref, "dbus_message_unref") &&
            Bind(message_iter_init, "dbus_message_iter_init") &&
            Bind(message_iter_next, "dbus_message_iter_next") &&
            Bind(message_iter_recurse, "dbus_message_iter_recurse") &&
            Bind(message_iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
            Bind(message_iter_get_basic, "dbus_message_iter_get_basic");
        if (!ok) {
            dlclose(handle);
            handle = nullptr;
            return false;
        }
        return true;
    }

    // org.freedesktop.DBus.ListNames
    bool ListNames(MprisDBusConnection* conn, std::vector<std::string>& out) noexcept {
        MprisDBusMessage* msg = message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
        if (msg == nullptr) {
            return false;
        }
        MprisDBusError err;
        error_init(&err);
        MprisDBusMessage* reply = send_with_reply_and_block(conn, msg, kMprisSendTimeoutMs, &err);
        message_unref(msg);
        if (reply == nullptr) {
            error_free(&err);
            return false;
        }
        error_free(&err);

        MprisDBusIter iter;
        MprisDBusIter sub;
        if (message_iter_init(reply, &iter) &&
            message_iter_get_arg_type(&iter) == kMprisTypeArray) {
            message_iter_recurse(&iter, &sub);
            while (message_iter_get_arg_type(&sub) == kMprisTypeString) {
                const char* name = nullptr;
                message_iter_get_basic(&sub, &name);
                if (name != nullptr) {
                    out.emplace_back(name);
                }
                message_iter_next(&sub);
            }
        }
        message_unref(reply);
        return true;
    }

    // org.freedesktop.DBus.Properties.Get(Player, "PlaybackStatus") for one
    // MPRIS player - the reply is a variant wrapping a string (eg: "Playing").
    bool PlaybackStatus(MprisDBusConnection* conn, const char* busName, std::string& out) noexcept {
        MprisDBusMessage* msg = message_new_method_call(
            busName, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get");
        if (msg == nullptr) {
            return false;
        }
        const char* iface = "org.mpris.MediaPlayer2.Player";
        const char* prop = "PlaybackStatus";
        if (!message_append_args(msg, kMprisTypeString, &iface, kMprisTypeString, &prop,
                                 kMprisTypeInvalid)) {
            message_unref(msg);
            return false;
        }
        MprisDBusError err;
        error_init(&err);
        MprisDBusMessage* reply = send_with_reply_and_block(conn, msg, kMprisSendTimeoutMs, &err);
        message_unref(msg);
        if (reply == nullptr) {
            error_free(&err);
            return false;
        }
        error_free(&err);

        bool haveValue = false;
        MprisDBusIter iter;
        MprisDBusIter variant;
        if (message_iter_init(reply, &iter) &&
            message_iter_get_arg_type(&iter) == kMprisTypeVariant) {
            message_iter_recurse(&iter, &variant);
            if (message_iter_get_arg_type(&variant) == kMprisTypeString) {
                const char* value = nullptr;
                message_iter_get_basic(&variant, &value);
                if (value != nullptr) {
                    out.assign(value);
                    haveValue = true;
                }
            }
        }
        message_unref(reply);
        return haveValue;
    }
};

void MonitorLinuxMprisSessions() noexcept {
    using namespace std::chrono_literals;

    MprisDBus dbus;
    if (!dbus.Load()) {
        g_mediaControlAvailable.store(false, std::memory_order_release);
        g_externalMediaPlaying.store(false, std::memory_order_release);
        g_mediaControlInitializationComplete.store(true, std::memory_order_release);
        std::cerr << "[audio] MPRIS media monitoring unavailable: libdbus-1 could not be loaded"
                  << std::endl;
        return;
    }

    MprisDBusError err;
    dbus.error_init(&err);
    MprisDBusConnection* conn = dbus.bus_get_private(kMprisBusSession, &err);
    if (conn == nullptr) {
        g_mediaControlAvailable.store(false, std::memory_order_release);
        g_externalMediaPlaying.store(false, std::memory_order_release);
        g_mediaControlInitializationComplete.store(true, std::memory_order_release);
        std::cerr << "[audio] MPRIS media monitoring unavailable: "
                  << (err.message != nullptr ? err.message : "no D-Bus session bus") << std::endl;
        dbus.error_free(&err);
        return;
    }
    dbus.error_free(&err);
    // A private bus connection defaults to terminating the process when the bus drops.
    dbus.connection_set_exit_on_disconnect(conn, 0u);

    for (bool firstPoll = true;; firstPoll = false) {
        bool playing = false;
        std::vector<std::string> names;
        const bool queried = dbus.ListNames(conn, names);
        if (queried) {
            for (const auto& name : names) {
                if (name.compare(0, sizeof(kMprisNamePrefix) - 1, kMprisNamePrefix) != 0) {
                    continue;
                }
                std::string status;
                if (dbus.PlaybackStatus(conn, name.c_str(), status) && status == "Playing") {
                    playing = true;
                    break;
                }
            }
        }
        g_externalMediaPlaying.store(playing, std::memory_order_release);
        g_mediaControlAvailable.store(queried, std::memory_order_release);
        if (firstPoll) {
            g_mediaControlInitializationComplete.store(true, std::memory_order_release);
            if (!queried) {
                std::cerr << "[audio] MPRIS media monitoring unavailable: session bus query failed"
                          << std::endl;
            }
        }
        std::this_thread::sleep_for(250ms);
    }
}
#endif

void StartMonitor() noexcept {
#if defined(_WIN32)
    // The process owns this monitor for its remaining lifetime. Keeping it
    // detached avoids shutdown ordering between WinRT and static audio state.
    std::thread(MonitorWindowsMediaSessions).detach();
#elif defined(__linux__)
    // Detached so there is no shutdown ordering to manage against static audio state.
    std::thread(MonitorLinuxMprisSessions).detach();
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
    // Preserve the original function's access semantics. An invalid player is
    // a guest bug and must not be converted into a silent successful call.
    Memory::Write32(soundPlayer + kSoundPlayerVolumeOffset, FloatBits(applied));
}

} // namespace MusicAttenuation
