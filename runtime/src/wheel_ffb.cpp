#include "wheel_ffb.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

namespace wheel_ffb {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kLogitechVid = 0x046D;
constexpr uint16_t kUnlistedWheelPids[] = {0xC202, 0xC20E, 0xC293, 0xC29C, 0xCA04};
constexpr uint16_t kDrivingForcePids[] = {
    0xC202, 0xC20E, 0xC24F, 0xC260, 0xC293, 0xC294, 0xC295,
    0xC298, 0xC299, 0xC29A, 0xC29B, 0xC29C, 0xCA03, 0xCA04,
};

constexpr float kVibrationFloor = 2600.0f;
constexpr int kVibrationEpsilon = 655;
constexpr uint16_t kSpringDeadband = 0x0CCD;
constexpr auto kDutyWindow = std::chrono::milliseconds(100);
constexpr auto kReconcileInterval = std::chrono::seconds(1);
constexpr auto kRetryDelay = std::chrono::seconds(2);
constexpr int kMaxFailures = 8;
constexpr uint32_t kSineIterations = 4;
constexpr auto kSineRefresh = std::chrono::seconds(1);
constexpr auto kOpenBackoff = std::chrono::seconds(30);
constexpr uint16_t kWheelStickDeadZone = 600;
constexpr int16_t kPedalThreshold = -8000;
constexpr int16_t kPedalRestZone = 24000;
constexpr int kMaxTrackedAxes = 8;
constexpr auto kSpringRefresh = std::chrono::seconds(10);
constexpr uint32_t kSpringLength = 30000;

struct Session {
    bool active = false;
    uint32_t port = 0;
    SDL_JoystickID instance = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_Haptic* haptic = nullptr;
    uint32_t features = 0;
    SDL_HapticEffectID springId = -1;
    SDL_HapticEffectID sineId = -1;
    bool sineRunning = false;
    int16_t sineMagnitude = 0;
    Clock::time_point sineRunStamp{};
    Clock::time_point springRunStamp{};
    bool motorOn = false;
    Clock::time_point motorStamp{};
    Clock::duration motorAccum{};
    Clock::time_point windowStart{};
    float level = 0.0f;
    int springFailures = 0;
    int sineFailures = 0;
};

Session g_session;
bool g_dirty = true;
Clock::time_point g_nextReconcile{};
Clock::time_point g_retryAfter{};
const char* g_status = "No force feedback device found";
int g_strength = RuntimeConfigFile::FfbStrength();
int g_spring = RuntimeConfigFile::FfbSpring();
int g_vibration = RuntimeConfigFile::FfbVibration();
int g_steering = RuntimeConfigFile::SteeringSensitivity();
struct InputState {
    SDL_JoystickID instance = 0;
    std::array<bool, kMaxTrackedAxes> restsHigh{};
    uint32_t suppressedPedals = 0;
    bool suppressSteering = false;
    bool pedalMappingKnown = false;
    uint32_t mappedPedals = 0;
};

std::array<InputState, PAD_CHANMAX> g_inputStates{};
bool g_inputBlocked = false;

InputState& StateForPort(uint32_t port, SDL_Joystick* joystick) {
    auto& state = g_inputStates[port];
    const SDL_JoystickID instance = SDL_GetJoystickID(joystick);
    if (state.instance != instance) {
        state = InputState{};
        state.instance = instance;
    }
    return state;
}

uint32_t MappedPedals(InputState& state) {
    if (state.pedalMappingKnown) {
        return state.mappedPedals;
    }
    state.pedalMappingKnown = true;
    char* mapping = SDL_GetGamepadMappingForID(state.instance);
    if (mapping == nullptr) {
        return 0;
    }
    std::string_view remaining(mapping);
    while (!remaining.empty()) {
        const size_t end = remaining.find(',');
        const std::string_view field = remaining.substr(0, end);
        if (field.size() >= 4 && field[1] == ':' && (field[0] == 'a' || field[0] == 'b')) {
            std::string_view binding = field.substr(2);
            if (binding.front() == '+' || binding.front() == '-') {
                binding.remove_prefix(1);
            }
            if (binding.size() >= 2 && binding[0] == 'a' && binding[1] >= '0' && binding[1] <= '9') {
                state.mappedPedals |= field[0] == 'a' ? PAD_BUTTON_A : PAD_BUTTON_B;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(end + 1);
    }
    SDL_free(mapping);
    return state.mappedPedals;
}

bool IsKnownWheel(SDL_Joystick* joystick) {
    return IsWheelInstance(SDL_GetJoystickID(joystick));
}

SDL_Joystick* JoystickForPort(uint32_t port) {
    if (port >= PAD_CHANMAX) {
        return nullptr;
    }
    uint32_t keyCount = 0;
    if (PADGetKeyButtonBindings(port, &keyCount) != nullptr) {
        return nullptr;
    }
    const int32_t index = PADGetIndexForPort(port);
    if (index < 0) {
        return nullptr;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    return gamepad != nullptr ? SDL_GetGamepadJoystick(gamepad) : nullptr;
}

SDL_Joystick* WheelForPort(uint32_t port) {
    SDL_Joystick* joystick = JoystickForPort(port);
    if (joystick == nullptr) {
        return nullptr;
    }
    return IsKnownWheel(joystick) ? joystick : nullptr;
}

bool HasWheelEffects(SDL_Haptic* haptic) {
    return SDL_GetNumHapticAxes(haptic) >= 1 &&
           (SDL_GetHapticFeatures(haptic) &
            (SDL_HAPTIC_SPRING | SDL_HAPTIC_SINE | SDL_HAPTIC_AUTOCENTER)) != 0;
}

SDL_Haptic* OpenMatchingHaptic(SDL_Joystick* joystick) {
    // Prefer SDL's device association. On some Windows drivers the separate
    // DirectInput haptic interface is only visible through enumeration.
    if (SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick)) {
        if (HasWheelEffects(haptic)) {
            return haptic;
        }
        SDL_CloseHaptic(haptic);
    }
    const char* joystickName = SDL_GetJoystickName(joystick);
    if (joystickName == nullptr || *joystickName == '\0') {
        return nullptr;
    }
    // Two identical joysticks are ambiguous even with one haptic interface.
    int joystickCount = 0;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&joystickCount);
    if (joysticks == nullptr) {
        return nullptr;
    }
    int matchingJoysticks = 0;
    for (int i = 0; i < joystickCount; ++i) {
        const char* name = SDL_GetJoystickNameForID(joysticks[i]);
        if (name != nullptr && std::strcmp(joystickName, name) == 0) {
            ++matchingJoysticks;
        }
    }
    SDL_free(joysticks);
    if (matchingJoysticks != 1) {
        return nullptr;
    }
    int count = 0;
    SDL_HapticID* ids = SDL_GetHaptics(&count);
    if (ids == nullptr) {
        return nullptr;
    }
    SDL_Haptic* chosen = nullptr;
    bool ambiguous = false;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetHapticNameForID(ids[i]);
        if (name == nullptr || std::strcmp(joystickName, name) != 0) {
            continue;
        }
        SDL_Haptic* haptic = SDL_OpenHaptic(ids[i]);
        if (haptic == nullptr) {
            ambiguous = true;
            break;
        }
        const int axes = SDL_GetNumHapticAxes(haptic);
        const uint32_t features = SDL_GetHapticFeatures(haptic);
        if (axes < 0 || features == 0) {
            SDL_CloseHaptic(haptic);
            ambiguous = true;
            break;
        }
        if (axes == 0 ||
            (features & (SDL_HAPTIC_SPRING | SDL_HAPTIC_SINE | SDL_HAPTIC_AUTOCENTER)) == 0) {
            SDL_CloseHaptic(haptic);
            continue;
        }
        if (chosen != nullptr) {
            SDL_CloseHaptic(haptic);
            ambiguous = true;
            break;
        }
        chosen = haptic;
    }
    SDL_free(ids);
    if (ambiguous) {
        if (chosen != nullptr) {
            SDL_CloseHaptic(chosen);
        }
        return nullptr;
    }
    return chosen;
}

SDL_HapticEffect SpringEffect(int springPercent) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SPRING;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = kSpringLength;
    const int strength = (g_session.features & SDL_HAPTIC_GAIN) != 0 ? 100 : g_strength;
    const auto coeff = static_cast<int16_t>(springPercent * strength * 0x7FFF / 10000);
    effect.condition.right_sat[0] = 0xFFFF;
    effect.condition.left_sat[0] = 0xFFFF;
    effect.condition.right_coeff[0] = coeff;
    effect.condition.left_coeff[0] = coeff;
    effect.condition.deadband[0] = kSpringDeadband;
    return effect;
}

SDL_HapticEffect SineEffect() {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SINE;
    effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.periodic.period = 50;
    effect.periodic.length = 1000;
    effect.periodic.magnitude = 0;
    return effect;
}

void CloseSession(const char* status) {
    if (g_session.haptic != nullptr) {
        SDL_StopHapticEffects(g_session.haptic);
        if ((g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
            SDL_SetHapticAutocenter(g_session.haptic, 0);
        }
        if (g_session.sineId >= 0) {
            SDL_DestroyHapticEffect(g_session.haptic, g_session.sineId);
        }
        if (g_session.springId >= 0) {
            SDL_DestroyHapticEffect(g_session.haptic, g_session.springId);
        }
        SDL_CloseHaptic(g_session.haptic);
        RT_LOG(RT_TAG_CONFIG) << "wheel ffb: closed session on port " << g_session.port + 1
                              << std::endl;
    }
    if (g_session.joystick != nullptr) {
        SDL_CloseJoystick(g_session.joystick);
    }
    g_session = Session{};
    g_status = status;
}

bool Guard(bool ok, int& failures) {
    if (ok) {
        failures = 0;
        return true;
    }
    RT_LOG(RT_TAG_CONFIG) << "wheel ffb: effect call failed: " << SDL_GetError() << std::endl;
    if (++failures >= kMaxFailures) {
        CloseSession("Device error, retrying");
        g_retryAfter = Clock::now() + kRetryDelay;
    }
    return false;
}

void OpenSession(uint32_t port, SDL_Joystick* joystick) {
    // Hold a joystick reference until after the haptic closes, even if Aurora
    // closes its gamepad during reassignment or removal.
    SDL_Joystick* retained = SDL_OpenJoystick(SDL_GetJoystickID(joystick));
    if (retained == nullptr) {
        g_status = "Device unavailable, retrying";
        g_retryAfter = Clock::now() + kRetryDelay;
        return;
    }
    SDL_Haptic* haptic = OpenMatchingHaptic(joystick);
    if (haptic == nullptr) {
        SDL_CloseJoystick(retained);
        RT_LOG(RT_TAG_CONFIG) << "wheel ffb: no usable haptic device for "
                              << (SDL_GetJoystickName(joystick) != nullptr
                                      ? SDL_GetJoystickName(joystick)
                                      : "wheel")
                              << std::endl;
        g_status = "No force feedback device found";
        g_retryAfter = Clock::now() + kOpenBackoff;
        return;
    }
    g_session.active = true;
    g_session.port = port;
    g_session.instance = SDL_GetJoystickID(joystick);
    g_session.joystick = retained;
    g_session.haptic = haptic;
    g_session.features = SDL_GetHapticFeatures(haptic);
    g_session.motorStamp = Clock::now();
    g_session.windowStart = g_session.motorStamp;
    if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(g_session.instance)) {
        SDL_RumbleGamepad(gamepad, 0, 0, 0);
    }
    if ((g_session.features & SDL_HAPTIC_GAIN) != 0 && !SDL_SetHapticGain(haptic, g_strength)) {
        CloseSession("Unable to set force strength, retrying");
        g_retryAfter = Clock::now() + kRetryDelay;
        return;
    }
    bool springRunning = false;
    if ((g_session.features & SDL_HAPTIC_SPRING) != 0) {
        SDL_HapticEffect effect = SpringEffect(g_spring);
        g_session.springId = SDL_CreateHapticEffect(haptic, &effect);
        springRunning = g_session.springId >= 0 &&
                        SDL_RunHapticEffect(haptic, g_session.springId, SDL_HAPTIC_INFINITY);
        g_session.springRunStamp = Clock::now();
        if (!springRunning && g_session.springId >= 0) {
            SDL_DestroyHapticEffect(haptic, g_session.springId);
            g_session.springId = -1;
        }
    }
    bool autocenterRunning = false;
    if (!springRunning && (g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
        const int strength = (g_session.features & SDL_HAPTIC_GAIN) != 0 ? 100 : g_strength;
        autocenterRunning = SDL_SetHapticAutocenter(haptic, g_spring * strength / 100);
    }
    if ((g_session.features & SDL_HAPTIC_SINE) != 0) {
        SDL_HapticEffect effect = SineEffect();
        g_session.sineId = SDL_CreateHapticEffect(haptic, &effect);
    }
    if (!springRunning && !autocenterRunning && g_session.sineId < 0) {
        CloseSession("No usable force feedback effects, retrying");
        g_retryAfter = Clock::now() + kOpenBackoff;
        return;
    }
    if (springRunning) {
        g_status = g_session.sineId >= 0 ? "Active" : "Active, no vibration support";
    } else if (autocenterRunning) {
        g_status = "Active, autocenter fallback";
    } else {
        g_status = "Active, no centering support";
    }
    const char* name = SDL_GetJoystickName(joystick);
    RT_LOG(RT_TAG_CONFIG) << "wheel ffb: opened " << (name != nullptr ? name : "wheel")
                          << " on port " << port + 1 << " (" << g_status << ")" << std::endl;
}

void Reconcile() {
    const bool enabled = RuntimeConfigFile::FfbEnabled();
    uint32_t wheelPort = UINT32_MAX;
    SDL_Joystick* joystick = nullptr;
    if (enabled) {
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            if (SDL_Joystick* candidate = WheelForPort(port)) {
                wheelPort = port;
                joystick = candidate;
                break;
            }
        }
    }
    if (wheelPort == UINT32_MAX) {
        const char* status = enabled ? "No force feedback device found" : "Force feedback off";
        if (g_session.active) {
            CloseSession(status);
        } else {
            g_status = status;
        }
        return;
    }
    if (g_session.active && g_session.port == wheelPort &&
        g_session.instance == SDL_GetJoystickID(joystick)) {
        return;
    }
    if (g_session.active) {
        CloseSession("No force feedback device found");
    }
    if (Clock::now() < g_retryAfter) {
        return;
    }
    OpenSession(wheelPort, joystick);
}

} // namespace

bool IsWheelInstance(uint32_t instance) {
    if (SDL_GetJoystickTypeForID(instance) == SDL_JOYSTICK_TYPE_WHEEL) {
        return true;
    }
    if (RuntimeConfigFile::FfbForceWheel()) {
        char guid[33] = {};
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(instance), guid, sizeof(guid));
        if (!RuntimeConfigFile::FfbWheelGuid().empty() && RuntimeConfigFile::FfbWheelGuid() == guid) {
            return true;
        }
    }
    if (SDL_GetJoystickVendorForID(instance) != kLogitechVid) {
        return false;
    }
    const uint16_t product = SDL_GetJoystickProductForID(instance);
    return std::find(std::begin(kUnlistedWheelPids), std::end(kUnlistedWheelPids), product) !=
           std::end(kUnlistedWheelPids);
}

bool HasBuiltinLayout(uint32_t instance) {
    if (SDL_GetJoystickVendorForID(instance) != kLogitechVid) {
        return false;
    }
    const uint16_t product = SDL_GetJoystickProductForID(instance);
    return std::find(std::begin(kDrivingForcePids), std::end(kDrivingForcePids), product) !=
           std::end(kDrivingForcePids);
}

void NotifyControllersChanged() {
    g_dirty = true;
    // Preserve held-input suppression; only the mapping cache needs invalidation.
    for (auto& state : g_inputStates) {
        state.pedalMappingKnown = false;
        state.mappedPedals = 0;
    }
}

void Tick() {
    const auto now = Clock::now();
    if (g_dirty || now >= g_nextReconcile) {
        g_dirty = false;
        g_nextReconcile = now + kReconcileInterval;
        Reconcile();
    }
    if (!g_session.active) {
        return;
    }
    if (g_session.springId >= 0 && now - g_session.springRunStamp >= kSpringRefresh) {
        g_session.springRunStamp = now;
        SDL_HapticEffect effect = SpringEffect(g_spring);
        // Count a refresh as successful only when every operation succeeds.
        const bool gainOk = (g_session.features & SDL_HAPTIC_GAIN) == 0 ||
                            SDL_SetHapticGain(g_session.haptic, g_strength);
        Guard(gainOk && SDL_UpdateHapticEffect(g_session.haptic, g_session.springId, &effect) &&
              SDL_RunHapticEffect(g_session.haptic, g_session.springId, SDL_HAPTIC_INFINITY),
              g_session.springFailures);
        if (!g_session.active) {
            return;
        }
    }
    if (g_session.motorOn) {
        g_session.motorAccum += now - g_session.motorStamp;
        g_session.motorStamp = now;
    }
    const auto elapsed = now - g_session.windowStart;
    if (elapsed < kDutyWindow) {
        return;
    }
    const float duty =
        std::clamp(std::chrono::duration<float>(g_session.motorAccum).count() /
                       std::chrono::duration<float>(elapsed).count(),
                   0.0f, 1.0f);
    g_session.motorAccum = {};
    g_session.windowStart = now;
    g_session.level += (duty - g_session.level) * 0.3f;
    if (g_session.sineId < 0) {
        return;
    }
    int target = 0;
    if (g_session.level >= 0.02f) {
        const float base = kVibrationFloor + g_session.level * (32767.0f - kVibrationFloor);
        const int strength = (g_session.features & SDL_HAPTIC_GAIN) != 0 ? 100 : g_strength;
        target = static_cast<int>(base * static_cast<float>(g_vibration * strength) / 10000.0f);
    }
    if (target == 0) {
        if (g_session.sineRunning && Guard(SDL_StopHapticEffect(g_session.haptic, g_session.sineId), g_session.sineFailures)) {
            g_session.sineRunning = false;
            g_session.sineMagnitude = 0;
        }
        return;
    }
    const bool stale = now - g_session.sineRunStamp >= kSineRefresh;
    bool attempted = false;
    bool ok = true;
    if (std::abs(target - g_session.sineMagnitude) >= kVibrationEpsilon) {
        SDL_HapticEffect effect = SineEffect();
        effect.periodic.magnitude = static_cast<int16_t>(target);
        attempted = true;
        ok = SDL_UpdateHapticEffect(g_session.haptic, g_session.sineId, &effect);
        if (ok) {
            g_session.sineMagnitude = static_cast<int16_t>(target);
        }
    }
    if (ok && (!g_session.sineRunning || stale)) {
        attempted = true;
        ok = SDL_RunHapticEffect(g_session.haptic, g_session.sineId, kSineIterations);
        if (ok) {
            g_session.sineRunning = true;
            g_session.sineRunStamp = now;
        }
    }
    if (attempted) {
        Guard(ok, g_session.sineFailures);
    }
}

bool OnMotorCommand(int32_t chan, uint32_t command) {
    if (!g_session.active || g_session.sineId < 0 ||
        static_cast<uint32_t>(chan) != g_session.port) {
        return false;
    }
    const auto now = Clock::now();
    if (g_session.motorOn) {
        g_session.motorAccum += now - g_session.motorStamp;
    }
    g_session.motorStamp = now;
    g_session.motorOn = !g_inputBlocked && command == PAD_MOTOR_RUMBLE;
    return true;
}

bool IsWheelPort(uint32_t port) { return WheelForPort(port) != nullptr; }

const char* StatusText() { return g_status; }

float SteeringPosition(uint32_t port) {
    SDL_Joystick* joystick = WheelForPort(port);
    SDL_Gamepad* gamepad = joystick != nullptr ? SDL_GetGamepadFromID(SDL_GetJoystickID(joystick)) : nullptr;
    if (gamepad == nullptr) {
        return 0.0f;
    }
    const int32_t raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    return static_cast<float>(raw) / (raw < 0 ? 32768.0f : 32767.0f);
}

void ApplyStrength(int percent) {
    g_strength = std::clamp(percent, 0, 100);
    if (g_session.active && (g_session.features & SDL_HAPTIC_GAIN) != 0) {
        if (!SDL_SetHapticGain(g_session.haptic, g_strength)) {
            CloseSession("Unable to set force strength, retrying");
            g_retryAfter = Clock::now() + kRetryDelay;
        }
    } else if (g_session.active) {
        ApplySpring(g_spring);
    }
}

void ApplySpring(int percent) {
    g_spring = std::clamp(percent, 0, 100);
    if (!g_session.active) {
        return;
    }
    if (g_session.springId >= 0) {
        SDL_HapticEffect effect = SpringEffect(g_spring);
        if (!SDL_UpdateHapticEffect(g_session.haptic, g_session.springId, &effect) ||
            !SDL_RunHapticEffect(g_session.haptic, g_session.springId, SDL_HAPTIC_INFINITY)) {
            CloseSession("Unable to set centering, retrying");
            g_retryAfter = Clock::now() + kRetryDelay;
        }
    } else if ((g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
        const int strength = (g_session.features & SDL_HAPTIC_GAIN) != 0 ? 100 : g_strength;
        if (!SDL_SetHapticAutocenter(g_session.haptic, g_spring * strength / 100)) {
            CloseSession("Unable to set centering, retrying");
            g_retryAfter = Clock::now() + kRetryDelay;
        }
    }
}

void Shutdown() {
    CloseSession("Force feedback off");
}

void ApplyVibration(int percent) {
    g_vibration = std::clamp(percent, 0, 100);
}

void ApplySteeringSensitivity(int percent) {
    g_steering = std::clamp(percent, 100, 900);
}

bool UsesMappedPedals(uint32_t port) {
    SDL_Joystick* joystick = WheelForPort(port);
    return joystick != nullptr && MappedPedals(StateForPort(port, joystick)) != 0;
}

uint32_t PedalButtons(uint32_t port) {
    SDL_Joystick* joystick = WheelForPort(port);
    if (joystick == nullptr) {
        if (port < PAD_CHANMAX) {
            g_inputStates[port] = InputState{};
        }
        return 0;
    }
    auto& state = StateForPort(port, joystick);
    // Once an SDL layout maps either pedal axis, let that layout own both
    // controls. Mixing a partial mapping with guesses can turn the accelerator
    // into an automatic brake when its raw axis crosses the opposite extreme.
    if (MappedPedals(state) != 0) {
        state.suppressedPedals = 0;
        return 0;
    }
    const int axes = std::min(SDL_GetNumJoystickAxes(joystick), kMaxTrackedAxes);
    int accel = RuntimeConfigFile::AcceleratorAxis();
    int brake = RuntimeConfigFile::BrakeAxis();
    uint32_t pressed = 0;
    if (axes == 2 && accel < 0 && brake < 0) {
        const int16_t value = SDL_GetJoystickAxis(joystick, 1);
        if (value < kPedalThreshold) {
            pressed |= PAD_BUTTON_A;
        } else if (value > -kPedalThreshold) {
            pressed |= PAD_BUTTON_B;
        }
    } else {
        int learned[2] = {-1, -1};
        int count = 0;
        for (int axis = 1; axis < axes; ++axis) {
            if (SDL_GetJoystickAxis(joystick, axis) > kPedalRestZone) {
                state.restsHigh[axis] = true;
            }
            if (state.restsHigh[axis] && count < 2) {
                learned[count++] = axis;
            }
        }
        if (accel < 0) {
            accel = count > 0 ? learned[0] : -1;
        }
        if (brake < 0) {
            brake = count > 1 ? learned[1] : -1;
        }
        if (brake == accel) {
            brake = -1;
        }
        if (accel >= 0 && accel < axes && SDL_GetJoystickAxis(joystick, accel) < kPedalThreshold) {
            pressed |= PAD_BUTTON_A;
        }
        if (brake >= 0 && brake < axes && SDL_GetJoystickAxis(joystick, brake) < kPedalThreshold) {
            pressed |= PAD_BUTTON_B;
        }
    }
    if (g_inputBlocked) {
        state.suppressedPedals = pressed;
        return 0;
    }
    state.suppressedPedals &= pressed;
    return pressed & ~state.suppressedPedals;
}

int32_t ShapeSteering(uint32_t port, int32_t stickX) {
    SDL_Joystick* joystick = WheelForPort(port);
    if (joystick == nullptr) {
        return stickX;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(SDL_GetJoystickID(joystick));
    if (gamepad == nullptr) {
        return stickX;
    }
    int32_t raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    auto& state = StateForPort(port, joystick);
    const bool centered = std::abs(raw) <= kWheelStickDeadZone;
    if (g_inputBlocked) {
        state.suppressSteering = !centered;
        return 0;
    }
    if (centered) {
        state.suppressSteering = false;
        return 0;
    }
    if (state.suppressSteering) {
        return 0;
    }
    const int32_t range = (raw > 0 ? 32767 : 32768) - kWheelStickDeadZone;
    raw = raw > 0 ? raw - kWheelStickDeadZone : raw + kWheelStickDeadZone;
    const int64_t scaled = static_cast<int64_t>(raw) * g_steering * 127 / (100 * range);
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, -127, 127));
}

void SetInputBlocked(bool blocked) {
    // Snapshot held raw inputs at both transitions, including when no guest
    // PADRead happened while the overlay was open. Require release to re-arm.
    if (blocked || g_inputBlocked) {
        g_inputBlocked = true;
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            PedalButtons(port);
            ShapeSteering(port, 0);
        }
    }
    g_inputBlocked = blocked;
    if (blocked && g_session.active) {
        g_session.motorOn = false;
        g_session.motorAccum = {};
        g_session.level = 0.0f;
        if (g_session.sineRunning && Guard(SDL_StopHapticEffect(g_session.haptic, g_session.sineId), g_session.sineFailures)) {
            g_session.sineRunning = false;
            g_session.sineMagnitude = 0;
        }
    }
}

} // namespace wheel_ffb
