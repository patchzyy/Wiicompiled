#include "settings_overlay.h"
#include "gx_worker.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wiimote_host.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
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
bool g_exitPromptOpen = false;
bool g_restartPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
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
bool g_vsync = RuntimeConfigFile::Vsync();
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_forceAspect169 = RuntimeConfigFile::ForceAspect169Enabled();
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

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

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
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
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

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
    GX_SYNC_INLINE();
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
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
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y.
        if (const char* name = PADGetNameForControllerIndex(static_cast<uint32_t>(controllerIndex));
            name != nullptr && std::string_view(name).find("Wii U Pro") != std::string_view::npos) {
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
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled();
const bool g_wiiRemotesAtLaunch = g_wiiRemotesEnabled;
bool g_wiiRemotesSaveFailed = false;
bool g_wiiAutoPair = RuntimeConfigFile::WiiAutoPairEnabled(true);
bool g_wiiSensorBarAbove = RuntimeConfigFile::WiiSensorBarAbove(false);
int g_wiiIrSensitivity = RuntimeConfigFile::WiiIrSensitivity(3);


constexpr float kControllerMenuWidth = 460.0f;

void PushMenuTextWrap() {
    ImGui::PushTextWrapPos(0.0f);
}

std::string WiiRemoteLabel(const Wiimote::SlotInfo& info) {
    std::string label = info.model;
    if (!info.extension.empty()) {
        label += " + " + info.extension;
    }
    return label;
}

// Pairing, the console's Wii Remote settings and the driver switch; applies to every remote.
void DrawWiiRemoteSetup() {
    if (!ImGui::CollapsingHeader("Wii Remote setup (Bluetooth)")) {
        return;
    }
    ImGui::Indent();
    PushMenuTextWrap();
    const std::string notice = Wiimote::PlatformNotice();
    if (!notice.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", notice.c_str());
    }
    const Wiimote::PairingInfo pairing = Wiimote::Pairing();
    if (pairing.supported) {
        ImGui::TextUnformatted("Press the red SYNC button (or 1 and 2) on the Wii Remote, like on a Wii.");
        ImGui::TextDisabled("Paired before? Just press any button on it.");
    } else if (g_wiiRemotesEnabled) {
        ImGui::TextUnformatted("Pair the Wii Remote in your system's Bluetooth settings, then press any button on it.");
    }
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!pairing.supported);
    if (ImGui::Button("Pair a Wii Remote")) {
        Wiimote::StartPairing();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Searches for a minute, like pressing SYNC on the console.");
    }
    ImGui::SameLine();
    if (pairing.searching) {
        ImGui::TextDisabled("Searching...");
    } else if (!pairing.status.empty()) {
        ImGui::TextDisabled("%s", pairing.status.c_str());
    }
    if (const size_t waiting = Wiimote::WaitingRemotes(); waiting != 0) {
        ImGui::TextDisabled("%zu more Wii Remote%s connected; joins when a player leaves.", waiting,
                            waiting == 1 ? "" : "s");
    }
    if (ImGui::Checkbox("Look for Wii Remotes automatically", &g_wiiAutoPair)) {
        RuntimeConfigFile::SetWiiAutoPairEnabled(g_wiiAutoPair);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii Remote is connected, keep searching for one in pairing mode.\n"
                          "Searching can briefly disturb Bluetooth headphones.");
    }
    ImGui::SetNextItemWidth(160.0f);
    int sensorBar = g_wiiSensorBarAbove ? 1 : 0;
    if (ImGui::Combo("Sensor bar", &sensorBar, "Below TV\0Above TV\0")) {
        g_wiiSensorBarAbove = sensorBar == 1;
        RuntimeConfigFile::SetWiiSensorBarAbove(g_wiiSensorBarAbove);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect the next time the game starts.");
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Pointer sensitivity", &g_wiiIrSensitivity, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        RuntimeConfigFile::SetWiiIrSensitivity(g_wiiIrSensitivity);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The sensor bar sensitivity from the Wii settings.\nTakes effect the next time the game starts.");
    }
    if (ImGui::Checkbox("Use Wii Remotes", &g_wiiRemotesEnabled)) {
        g_wiiRemotesSaveFailed = !RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
        if (g_wiiRemotesSaveFailed) g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
        g_restartPromptOpen = g_wiiRemotesEnabled != g_wiiRemotesAtLaunch && !g_wiiRemotesSaveFailed;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect after restarting the game. Turn this off to use a Wii U Pro Controller as a gamepad.");
    }
    if (g_wiiRemotesSaveFailed) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Could not save this setting to Config.toml.");
    }
    if (g_wiiRemotesEnabled != g_wiiRemotesAtLaunch) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Restart the game to apply this change.");
    }
    ImGui::Unindent();
}

// The Wii Remote the game has on this player's channel.
void DrawWiiRemoteDetails(uint32_t port, const Wiimote::SlotInfo& info) {
    ImGui::Text("%s", WiiRemoteLabel(info).c_str());
    if (info.battery >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("Battery %d/4", info.battery);
    }
    PushMenuTextWrap();
    if (info.asleep) {
        ImGui::TextUnformatted("Disconnected by the game. Press any button on it to reconnect.");
    } else if (info.link == Bluetooth::LinkState::Ready) {
        ImGui::TextUnformatted("The game reads it like a real Wii Remote. Nothing to set up.");
    } else {
        ImGui::TextUnformatted("Connecting to the game...");
    }
    ImGui::PopTextWrapPos();
    if (!info.asleep && ImGui::Button("Disconnect")) {
        Wiimote::Disconnect(port);
    }
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

void ApplyKeyboardPreset(uint32_t port) {
    const std::array<int, PAD_BUTTON_COUNT> keys = {
        PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
        SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    };
    for (size_t i = 0; i < keys.size(); ++i)
        PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
    const std::array<int, PAD_AXIS_COUNT> axes = {
        SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
        SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
        SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
    };
    uint32_t axisCount = 0;
    auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i)
        PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
    PADSerializeMappings();
}

bool KeyboardActive(uint32_t port) {
    uint32_t count = 0;
    return PADGetKeyButtonBindings(port, &count) != nullptr;
}

void SetKeyboardActive(uint32_t port, bool active) {
    if (KeyboardActive(port) == active) return;
    PADSetKeyboardActive(port, active);
    PADSerializeMappings();
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    if (active && buttons != nullptr && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        })) {
        ApplyKeyboardPreset(port);
    }
}

void DrawKeyboardSettings(uint32_t port) {
    PushMenuTextWrap();
    ImGui::TextUnformatted("Default: WASD to steer, mouse left to accelerate, Space to brake.");
    ImGui::PopTextWrapPos();
    if (!ImGui::CollapsingHeader("Customize keys")) return;
    ImGui::Indent();
    if (ImGui::Button("Reset to WASD + mouse")) ApplyKeyboardPreset(port);
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    ImGui::SeparatorText("Buttons");
    for (uint32_t i = 0; buttons != nullptr && i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), buttons[i].scancode, RebindKind::KeyboardButton,
                       buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Sticks and triggers");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; axes != nullptr && i < axisCount; ++i) {
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        DrawKeyBinding(label.c_str(), axes[i].scancode, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    ImGui::Unindent();
}

int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImGui::Separator();
    if (ImGui::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

void DrawGamepadMapping(uint32_t port) {
    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(port, &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings = PADGetAltButtonMappings(port, &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding button without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != static_cast<int>(port)) {
        altRowExpandedPort = static_cast<int>(port);
        altRowExpanded.fill(false);
    }

    ImGui::TextUnformatted("Layout:");
    ImGui::SameLine();
    if (ImGui::SmallButton("GameCube")) {
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
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };
    ImGui::SameLine();
    if (ImGui::SmallButton("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::TextDisabled("Click a binding, then press the input to use. Triggers: LT/L2, RT/R2.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float bindingX = ImGui::GetCursorPosX() + ImGui::CalcTextSize("D-pad Right").x +
                           ImGui::GetStyle().ItemSpacing.x * 2.0f;
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
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(port, updated);
                else PADSetButtonMapping(port, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::SameLine(bindingX);
        ImGui::BeginGroup();
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::PopID();
    }
}

std::string ControllerLabel(uint32_t index) {
    const char* name = PADGetNameForControllerIndex(index);
    return name != nullptr ? name : "Unknown controller";
}

// One row per player: the device combo replaces separate assign/unassign/keyboard controls.
void DrawPlayerRow(uint32_t port) {
    const bool keyboard = KeyboardActive(port);
    const int32_t assigned = PADGetIndexForPort(port);
    std::string preview = "None";
    if (keyboard) {
        preview = "Keyboard and mouse";
    } else if (assigned >= 0) {
        preview = ControllerLabel(static_cast<uint32_t>(assigned));
    } else if (const Wiimote::SlotInfo remote = Wiimote::Slot(port); remote.attached) {
        preview = WiiRemoteLabel(remote);
    }

    ImGui::PushID(static_cast<int>(port));
    const std::string label = "Player " + std::to_string(port + 1);
    ImGui::AlignTextToFramePadding();
    if (ImGui::RadioButton(label.c_str(), g_controllerPort == static_cast<int>(port))) {
        g_controllerPort = static_cast<int>(port);
    }
    ImGui::SameLine(ImGui::CalcTextSize("Player 0").x + ImGui::GetFrameHeight() + 16.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        g_controllerPort = static_cast<int>(port);
        if (ImGui::Selectable("None", !keyboard && assigned < 0)) {
            SetKeyboardActive(port, false);
            PADClearPort(port);
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        const uint32_t controllerCount = PADCount();
        for (uint32_t index = 0; index < controllerCount; ++index) {
            std::string item = ControllerLabel(index);
            for (uint32_t other = 0; other < PAD_MAX_CONTROLLERS; ++other) {
                if (other != port && PADGetIndexForPort(other) == static_cast<s32>(index)) {
                    item += " (Player " + std::to_string(other + 1) + ")";
                    break;
                }
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(item.c_str(), !keyboard && assigned == static_cast<int32_t>(index))) {
                SetKeyboardActive(port, false);
                PADSetPortForIndex(index, port);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        if (ImGui::Selectable("Keyboard and mouse", keyboard)) {
            SetKeyboardActive(port, true);
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void DrawControllerSettings() {
    ImGui::Dummy(ImVec2(kControllerMenuWidth, 0.0f));
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        DrawPlayerRow(port);
    }
    bool anyRemote = false;
    for (uint32_t player = 0; player < PAD_MAX_CONTROLLERS; ++player) {
        anyRemote |= Wiimote::Slot(player).attached;
    }
    if (PADCount() == 0 && !anyRemote) {
        ImGui::TextDisabled("No controller detected. Plug one in, or pair a Wii Remote below.");
    }

    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
    ImGui::SeparatorText(("Player " + std::to_string(port + 1)).c_str());
    const Wiimote::SlotInfo remote = Wiimote::Slot(port);
    if (remote.attached) {
        DrawWiiRemoteDetails(port, remote);
        if (KeyboardActive(port) || PADGetIndexForPort(port) >= 0) {
            ImGui::Spacing();
        }
    }
    if (KeyboardActive(port)) {
        DrawKeyboardSettings(port);
    } else if (PADGetIndexForPort(port) < 0) {
        if (!remote.attached) {
            ImGui::TextDisabled("No device. Pick one in the list above.");
        }
    } else {
        ImGui::TextUnformatted("Ready to play.");
        if (ImGui::CollapsingHeader("Customize buttons")) {
            ImGui::Indent();
            DrawGamepadMapping(port);
            if (ImGui::TreeNode("Advanced: Dolphin expressions")) {
                DrawExpressionSettings();
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
    }

    ImGui::Spacing();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSetup();
    DrawRumbleSettings();
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
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
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
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            ImGui::TextDisabled("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            ImGui::TextDisabled("Media controls are unavailable.");
#endif
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    if (ImGui::Checkbox("Force 16:9", &g_forceAspect169)) {
        SetMkwForceAspect169(g_forceAspect169);
        RuntimeConfigFile::SetForceAspect169(g_forceAspect169);
    }
    ImGui::TextDisabled("Keep a 16:9 image with black bars when the window has another shape.");
    ImGui::Separator();
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
        GX_SYNC_INLINE();
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
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (ImGui::Checkbox("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        ImGui::TextDisabled("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        ImGui::TextDisabled("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        ImGui::TextDisabled("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        ImGui::TextDisabled("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        if (metalFxSupported) {
            ImGui::TextDisabled("Render below output resolution for sharper lower-cost output.");
        } else {
            ImGui::TextDisabled("MetalFX spatial upscaling is unavailable on this device.");
        }
        break;
    }
#endif
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        GX_SYNC_INLINE();
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        GX_SYNC_INLINE();
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("VSync", &g_vsync)) {
        aurora_enable_vsync(g_vsync);
        RuntimeConfigFile::SetVsync(g_vsync);
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
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    const double refreshRate = aurora_get_display_refresh_rate();
    const bool interpolationInactive = targetFps > 60 && !aurora_display_shows_interpolation();
    const bool unevenPacing =
        !g_vsync && !interpolationInactive && refreshRate > 0.0 && targetFps > refreshRate + 1.0;
    if (!g_showFps && !unevenPacing && !interpolationInactive) {
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
        if (g_showFps && presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else if (g_showFps) {
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
        if (interpolationInactive) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%u FPS interpolation has no effect on a %.0f Hz display.", targetFps,
                               refreshRate);
        }
        if (unevenPacing) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%u FPS interpolation on a %.0f Hz display without VSync is not smooth.\n"
                               "Enable VSync for smooth motion.",
                               targetFps, refreshRate);
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
        float nextY = startY + titleSize.y + 12.0f;
        if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            ImGui::SetWindowFontScale(0.9f);
            char line[96];
            std::snprintf(line, sizeof(line), "Compiling shaders, please hold on: %u remaining",
                          aurora_get_queued_pipeline_count());
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImGui::TextUnformatted(line);
            nextY = ImGui::GetCursorPosY() + 4.0f;
        }
        constexpr const char* kHint = "Press F10 to open settings";
        ImGui::SetWindowFontScale(0.8f);
        const ImVec2 hintSize = ImGui::CalcTextSize(kHint);
        ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - hintSize.x) * 0.5f), nextY));
        ImGui::TextUnformatted(kHint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
    }
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawRestartPrompt() {
    constexpr const char* kTitle = "Restart required";
    if (g_restartPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_restartPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(g_wiiRemotesEnabled ? "Wii Remotes will be used after you restart the game."
                                               : "Wii Remotes will stop being used after you restart the game.");
    if (ImGui::Button("Exit now", ImVec2(140.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Restart later", ImVec2(140.0f, 0.0f))) g_restartPromptOpen = false;
    if (!g_restartPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    constexpr float kHintMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + ImGui::GetFrameHeight() + kHintMargin),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Settings input hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (const char* line : {"Settings open - game controls disabled.",
                                 "Press F10 to return to the game."}) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::End();
    if (!ImGui::BeginMainMenuBar()) return;

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

    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, viewport->Size.y * 0.75f));
    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        // Nest capture under this menu so opening/closing the modal preserves
        // the settings popup and its current port and scroll position.
        DrawRebindPrompt();
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
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImGui::MenuItem("X")) {
        g_exitPromptOpen = true;
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
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
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

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active ||
                         g_exitPromptOpen || g_restartPromptOpen || g_topBarVisible || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
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
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
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
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_restartPromptOpen) {
                g_restartPromptOpen = false;
            } else if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            } else {
                g_exitPromptOpen = true;
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    Wiimote::Shutdown();
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

void Draw() noexcept {
    // ImGui is only touched on this thread; the render worker replays a frozen copy.
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawTopBar();
    DrawExitPrompt();
    DrawRestartPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
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
