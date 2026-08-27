#include "settings_overlay.h"
#include "audio_backend.h"
#include "controller_mapping_wizard.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

struct ControllerButtonItem {
    const char* configKey;
    const char* label;
    PADButton padButton;
};

constexpr std::array<ControllerButtonItem, PAD_BUTTON_COUNT> kControllerButtons = {{
    {"a", "A", PAD_BUTTON_A},
    {"b", "B", PAD_BUTTON_B},
    {"x", "X", PAD_BUTTON_X},
    {"y", "Y", PAD_BUTTON_Y},
    {"start", "Start", PAD_BUTTON_START},
    {"z", "Z", PAD_TRIGGER_Z},
    {"l", "L", PAD_TRIGGER_L},
    {"r", "R", PAD_TRIGGER_R},
    {"up", "D-pad Up", PAD_BUTTON_UP},
    {"down", "D-pad Down", PAD_BUTTON_DOWN},
    {"left", "D-pad Left", PAD_BUTTON_LEFT},
    {"right", "D-pad Right", PAD_BUTTON_RIGHT},
}};

struct NativeButtonItem {
    const char* configName;
    const char* label;
    uint32_t nativeButton;
};

constexpr std::array<NativeButtonItem, SDL_GAMEPAD_BUTTON_COUNT + 1> kNativeButtons = {{
    {"unmapped", "Unmapped / analog trigger", PAD_NATIVE_BUTTON_INVALID},
    {"south", "South (A / Cross)", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", "East (B / Circle)", SDL_GAMEPAD_BUTTON_EAST},
    {"west", "West (X / Square)", SDL_GAMEPAD_BUTTON_WEST},
    {"north", "North (Y / Triangle)", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", "Back / Select", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", "Guide / Home", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", "Start / Options", SDL_GAMEPAD_BUTTON_START},
    {"left_stick", "Left stick click", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"right_stick", "Right stick click", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"left_shoulder", "Left shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"right_shoulder", "Right shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"dpad_up", "D-pad Up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpad_down", "D-pad Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpad_left", "D-pad Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpad_right", "D-pad Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"misc1", "Misc 1 / Share", SDL_GAMEPAD_BUTTON_MISC1},
    {"right_paddle1", "Right paddle 1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"left_paddle1", "Left paddle 1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"right_paddle2", "Right paddle 2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"left_paddle2", "Left paddle 2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", "Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"misc2", "Misc 2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", "Misc 3 / GC L click", SDL_GAMEPAD_BUTTON_MISC3},
    {"misc4", "Misc 4 / GC R click", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", "Misc 5", SDL_GAMEPAD_BUTTON_MISC5},
    {"misc6", "Misc 6", SDL_GAMEPAD_BUTTON_MISC6},
}};

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

const NativeButtonItem* FindNativeButton(std::string value) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return value == item.configName;
    });
    return it == kNativeButtons.end() ? nullptr : &*it;
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};

std::string TrimBindingToken(const std::string& token) {
    const size_t begin = token.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = token.find_last_not_of(" \t");
    return token.substr(begin, end - begin + 1);
}

// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {TrimBindingToken(value), {}};
    }
    return {TrimBindingToken(value.substr(0, comma)), TrimBindingToken(value.substr(comma + 1))};
}

const NativeButtonItem& NativeButtonForValue(uint32_t nativeButton) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(), [&](const NativeButtonItem& item) {
        return nativeButton == item.nativeButton;
    });
    return it == kNativeButtons.end() ? kNativeButtons.front() : *it;
}

void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
    PADBlockInput(visible);
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = native->nativeButton;
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

void DrawControllerSettings() {
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    const char* currentName = PADGetName(static_cast<uint32_t>(g_controllerPort));
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(name != nullptr ? name : "Unknown controller")) {
                PADSetPortForIndex(index, static_cast<uint32_t>(g_controllerPort));
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeButtonForValue(primaryNative).configName;
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeButtonForValue(altNative).configName;
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(kClassicProPreset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, kClassicProPreset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }

    ImGui::SeparatorText("Button mapping");
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##primary", current.label)) {
            for (const auto& candidate : kNativeButtons) {
                const bool selected = candidate.nativeButton == mappingIt->nativeButton;
                if (ImGui::Selectable(candidate.label, selected)) {
                    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                    PADSetButtonMapping(port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                    writeBinding(i, candidate.nativeButton,
                                 altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                    PADSerializeMappings();
                    mappings = PADGetButtonMappings(port, &mappingCount);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(190.0f);
                if (ImGui::BeginCombo("##alt", altLabel)) {
                    for (const auto& candidate : kNativeButtons) {
                        const bool isNone = candidate.nativeButton == PAD_NATIVE_BUTTON_INVALID;
                        const bool selected = candidate.nativeButton == altIt->nativeButton;
                        if (ImGui::Selectable(isNone ? "None" : candidate.label, selected)) {
                            const uint32_t port = static_cast<uint32_t>(g_controllerPort);
                            PADSetAltButtonMapping(
                                port, PADButtonMapping{candidate.nativeButton, kControllerButtons[i].padButton});
                            writeBinding(i, mappingIt->nativeButton, candidate.nativeButton);
                            if (isNone) {
                                altRowExpanded[i] = false;
                            }
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }

}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Waiting for Windows Media Control...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
            ImGui::TextDisabled("Windows Media Control is unavailable.");
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    constexpr std::array<const char*, 3> kFrameInterpolationModes{
        "Off", "120 FPS", "180 FPS",
    };
    const char* currentFrameInterpolationMode =
        kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)];
    bool frameInterpolationModeChanged = false;
    if (ImGui::BeginCombo("Race frame interpolation (experimental)", currentFrameInterpolationMode)) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImGui::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "WiiCompiled";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawTopBar() {
    if (!g_topBarVisible || !ImGui::BeginMainMenuBar()) {
        return;
    }

    ImGui::TextUnformatted("WiiCompiled");
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImGui::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImGui::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        ImGui::EndMenu();
    }

    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - hideWidth - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    if (shouldHide) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    PADBlockInput(g_topBarVisible);
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawTopBar();
    controller_mapping_wizard::Draw();
    // The wizard captures raw presses; keep them out of the game even when the
    // top bar is hidden mid-setup.
    PADBlockInput(g_topBarVisible || controller_mapping_wizard::IsActive());
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
