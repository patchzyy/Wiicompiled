#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect169;
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameInterpolationFps;
    std::optional<bool> metalFxSpatialUpscaling;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> vsync;
    std::optional<bool> gxWorker;
    std::optional<bool> disableCopyFilter;
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    std::optional<bool> showFps;
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<float> audioVolume;
    std::optional<float> audioMusicVolume;
    std::optional<float> audioSoundEffectsVolume;
    std::optional<float> audioUiVolume;
    std::optional<float> audioVoicesVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    std::optional<bool> attenuateMusicWhenMediaPlays;
    // Real Wii Remotes passed through to the game's own Bluetooth stack. Off hands
    // Nintendo controllers (e.g. the Wii U Pro Controller) to SDL as plain gamepads.
    std::optional<bool> wiiRemotes;
    // Look for remotes in pairing mode (1+2 / SYNC) while none is connected.
    std::optional<bool> wiiAutoPair;
    // The console's Wii Remote settings (SYSCONF BT.BAR, BT.SENS, BT.SPKV, BT.MOT).
    std::optional<bool> wiiSensorBarAbove;
    std::optional<int32_t> wiiIrSensitivity;
    std::optional<int32_t> wiiSpeakerVolume;
    std::optional<bool> wiiRumble;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresenceEnabled;
    // The application ID of the WiiCompiled Discord application. This is only
    // used by the base product; Retro Rewind supplies its own ID through the
    // standard Dolphin /dev/dolphin interface.
    std::optional<std::string> discordClientId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    // The one canonical Retro Rewind installation, owned and updated by the frontend. Setup records
    // it here instead of copying the pack, so an asset-only update is visible on the next launch.
    std::optional<std::string> retroRewindRoot;
    std::vector<std::string> overlayRoots;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<bool> rumbleEnabled;
    std::optional<int32_t> muteHotkey;
    std::map<std::string, std::string> controllerExpressions;
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
inline constexpr const char* kApplicationDirectoryName = "WiiCompiled";

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, Cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 240 was offered by an early build and is no longer supported; a saved 240 is
// migrated to 180 at the parse site.
inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return value == 0 || value == 120 || value == 180;
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#ifdef _WIN32
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / kApplicationDirectoryName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        return std::filesystem::path(xdgDataHome) / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "# WiiCompiled user configuration\n"
              "# Set paths.dvd_root to an extracted Mario Kart Wii DATA directory.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_16_9 = false\n"
              "resolution_multiplier = 1.0\n"
              "frame_interpolation_fps = 0\n"
              "display_mode = \"windowed\"\n"
              "graphics_api = \"auto\"\n"
              "skip_unready_pipelines = true\n"
              "vsync = false\n"
              "# Runs GX command processing on its own thread. Read once, at startup.\n"
              "gx_worker = true\n"
              "disable_copy_filter = true\n"
              "show_fps = true\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = false\n"
              "texture_dumps = false\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "music_volume = 1.0\n"
              "sound_effects_volume = 1.0\n"
              "ui_volume = 1.0\n"
              "voices_volume = 1.0\n"
              "muted = false\n"
              "attenuate_music_when_media_plays = false\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest\n"
              "# thread exactly as the runtime did before.\n"
              "mix_worker = true\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[discord]\n"
              "# Rich Presence talks only to a locally-running Discord client.\n"
              "# Retro Rewind supplies its official app ID automatically. Set this\n"
              "# to WiiCompiled's Discord application ID for basic base-game presence.\n"
              "enabled = true\n"
              "# client_id = \"123456789012345678\"\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\MarioKartWii\\\\DATA\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n"
              "# retro_rewind_root = \"D:\\\\RetroRewind\\\\RetroRewind6\"\n"
              "# overlay_roots = [\"D:\\\\RetroRewind\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.forceAspect169 = FindConfigValue<bool>(document, "video", "force_16_9");
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        const uint32_t migrated = *value == 240u ? 180u : *value;
        if (IsSupportedFrameInterpolationFps(migrated)) {
            config.frameInterpolationFps = migrated;
        }
    }
    config.metalFxSpatialUpscaling =
        FindConfigValue<bool>(document, "video", "metalfx_spatial_upscaling");
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.vsync = FindConfigValue<bool>(document, "video", "vsync");
    config.gxWorker = FindConfigValue<bool>(document, "video", "gx_worker");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~0x10u) == 0) {
        config.disabledPostProcessingPaths = *value & 0x10u;
    }

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMusicVolume = readVolume("music_volume");
    config.audioSoundEffectsVolume = readVolume("sound_effects_volume");
    config.audioUiVolume = readVolume("ui_volume");
    config.audioVoicesVolume = readVolume("voices_volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.attenuateMusicWhenMediaPlays =
        FindConfigValue<bool>(document, "audio", "attenuate_music_when_media_plays");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiAutoPair = FindConfigValue<bool>(document, "controller", "wii_auto_pair");
    if (const auto bar = FindConfigValue<std::string>(document, "controller", "wii_sensor_bar")) {
        if (*bar == "above" || *bar == "below") config.wiiSensorBarAbove = *bar == "above";
    }
    if (const auto value = FindConfigInt(document, "controller", "wii_ir_sensitivity"); value && *value >= 1 && *value <= 5) {
        config.wiiIrSensitivity = *value;
    }
    if (const auto value = FindConfigInt(document, "controller", "wii_speaker_volume"); value && *value >= 0 && *value <= 127) {
        config.wiiSpeakerVolume = *value;
    }
    config.wiiRumble = FindConfigValue<bool>(document, "controller", "wii_rumble");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresenceEnabled = FindConfigValue<bool>(document, "discord", "enabled");
    config.discordClientId = FindConfigValue<std::string>(document, "discord", "client_id");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    config.retroRewindRoot = FindConfigValue<std::string>(document, "paths", "retro_rewind_root");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Sets section.key (section may be a dotted table path) to a TOML literal, keeping the rest of the
// document's keys, order and comments. Returns nullopt when the document or value does not parse.
inline std::optional<std::string> UpdateSettingText(const std::string& text, std::string_view section,
                                                    std::string_view key, std::string_view value) {
    try {
        auto root = toml::parse_str<toml::ordered_type_config>(text);
        auto literal = toml::parse_str<toml::ordered_type_config>("v = " + std::string(value));
        auto* table = &root;
        std::istringstream path{std::string(section)};
        for (std::string part; std::getline(path, part, '.');) {
            auto& parent = table->as_table();
            if (!parent.contains(part)) {
                parent[part] = toml::ordered_value(toml::ordered_table{});
            }
            table = &parent.at(part);
            if (!table->is_table()) {
                return std::nullopt;
            }
        }
        if (table->as_table_fmt().fmt == toml::table_format::implicit) {
            table->as_table_fmt().fmt = toml::table_format::multiline;
        }
        auto& slot = table->as_table()[std::string(key)];
        auto& updated = literal.as_table().at("v");
        updated.comments() = slot.comments();
        slot = std::move(updated);
        return toml::format(root);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Update one TOML value without discarding unrelated settings or user-specific paths. This is
// used by the in-game F10 settings bar. An unparseable file is left untouched.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ostringstream existing;
    if (std::ifstream input(path, std::ios::binary); input) {
        existing << input.rdbuf();
    }
    const auto updated = UpdateSettingText(existing.str(), section, key, value);
    if (!updated) {
        std::cerr << "[runtime-config] Not updating invalid " << PathToUtf8(path) << std::endl;
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    output << *updated;
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetMetalFxSpatialUpscaling(bool value) {
    Mutable().metalFxSpatialUpscaling = value;
    return WriteSetting("video", "metalfx_spatial_upscaling", value ? "true" : "false");
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetVsync(bool value) {
    Mutable().vsync = value;
    return WriteSetting("video", "vsync", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetMusicVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioMusicVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "music_volume", formatted.str());
}

inline bool SetSoundEffectsVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioSoundEffectsVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "sound_effects_volume", formatted.str());
}

inline bool SetUiVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioUiVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "ui_volume", formatted.str());
}

inline bool SetVoicesVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVoicesVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "voices_volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool SetAttenuateMusicWhenMediaPlays(bool value) {
    Mutable().attenuateMusicWhenMediaPlays = value;
    return WriteSetting("audio", "attenuate_music_when_media_plays", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool ForceAspect169Enabled(bool fallback = false) {
    return Get().forceAspect169.value_or(fallback);
}

inline bool SetForceAspect169(bool value) {
    Mutable().forceAspect169 = value;
    return WriteSetting("video", "force_16_9", value ? "true" : "false");
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline bool MetalFxSpatialUpscaling(bool fallback = false) {
    return Get().metalFxSpatialUpscaling.value_or(fallback);
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float MusicVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioMusicVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float SoundEffectsVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioSoundEffectsVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float UiVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioUiVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float VoicesVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVoicesVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

inline bool GxWorkerEnabled(bool fallback = true) {
    return Get().gxWorker.value_or(fallback);
}

// Whether background music should duck automatically for other media playback.
inline bool AttenuateMusicWhenMediaPlays(bool fallback = false) {
    return Get().attenuateMusicWhenMediaPlays.value_or(fallback);
}

// Bluetooth Wii Remotes. Read once before SDL's joystick subsystem comes up, so a
// change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = false) {
    return Get().wiiRemotes.value_or(fallback);
}

inline bool SetWiiRemotesEnabled(bool value) {
    if (!WriteSetting("controller", "wii_remotes", value ? "true" : "false")) return false;
    Mutable().wiiRemotes = value;
    return true;
}

inline bool WiiAutoPairEnabled(bool fallback = true) {
    return Get().wiiAutoPair.value_or(fallback);
}

inline bool SetWiiAutoPairEnabled(bool value) {
    Mutable().wiiAutoPair = value;
    return WriteSetting("controller", "wii_auto_pair", value ? "true" : "false");
}

// Defaults are the console's.
inline bool WiiSensorBarAbove(bool fallback = false) {
    return Get().wiiSensorBarAbove.value_or(fallback);
}

inline bool SetWiiSensorBarAbove(bool value) {
    Mutable().wiiSensorBarAbove = value;
    return WriteSetting("controller", "wii_sensor_bar", value ? "\"above\"" : "\"below\"");
}

inline int32_t WiiIrSensitivity(int32_t fallback = 3) {
    return Get().wiiIrSensitivity.value_or(fallback);
}

inline bool SetWiiIrSensitivity(int32_t value) {
    Mutable().wiiIrSensitivity = value;
    return WriteSetting("controller", "wii_ir_sensitivity", std::to_string(value));
}

inline int32_t WiiSpeakerVolume(int32_t fallback = 0x59) {
    return Get().wiiSpeakerVolume.value_or(fallback);
}

inline bool SetWiiSpeakerVolume(int32_t value) {
    Mutable().wiiSpeakerVolume = value;
    return WriteSetting("controller", "wii_speaker_volume", std::to_string(value));
}

inline bool WiiRumbleEnabled(bool fallback = true) {
    return Get().wiiRumble.value_or(fallback);
}

inline bool SetWiiRumbleEnabled(bool value) {
    Mutable().wiiRumble = value;
    return WriteSetting("controller", "wii_rumble", value ? "true" : "false");
}

// Target frame rate for frame interpolation, or 0 to disable it.
inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return Get().frameInterpolationFps.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool Vsync(bool fallback = false) {
    return Get().vsync.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool TextureReplacements(bool fallback = false) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = 0) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & 0x10u;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "windowed") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory (docs/WHEELWIZARD_CONTRACT.md).
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

/// The canonical Retro Rewind installation the frontend owns, or "" when none is recorded.
inline std::string RetroRewindRoot(std::string fallback = "") {
    return Get().retroRewindRoot.value_or(std::move(fallback));
}

inline bool DiscordPresenceEnabled(bool fallback = true) {
    return Get().discordPresenceEnabled.value_or(fallback);
}

inline std::string DiscordClientId(std::string fallback = "1543984562369990706") {
    return Get().discordClientId.value_or(std::move(fallback));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.vsync) {
                std::cout << " vsync=" << (*config.vsync ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresenceEnabled) {
                std::cout << " discord_enabled=" << (*config.discordPresenceEnabled ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
            if (config.retroRewindRoot) {
                std::cout << " retro_rewind_root=" << *config.retroRewindRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
