#include "wheel_ffb.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace wheel_ffb {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kLogitechVid = 0x046D;
constexpr uint16_t kWheelPids[] = {
    0xC202, 0xC20E, 0xC293, 0xC294, 0xC295, 0xC298, 0xC299, 0xC29A, 0xC29B, 0xC29C,
    0xC24F, 0xC260, 0xC261, 0xC262, 0xC266, 0xC267, 0xC268, 0xC26D, 0xC26E, 0xC272,
    0xCA03, 0xCA04,
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
constexpr uint16_t kAuroraStickDeadZone = 8000;
constexpr uint16_t kWheelStickDeadZone = 600;
constexpr int16_t kPedalThreshold = -8000;
constexpr auto kSpringRefresh = std::chrono::seconds(10);
constexpr uint32_t kSpringLength = 30000;

struct Session {
    bool active = false;
    uint32_t port = 0;
    SDL_JoystickID instance = 0;
    SDL_Haptic* haptic = nullptr;
    uint32_t features = 0;
    SDL_HapticEffectID springId = -1;
    SDL_HapticEffectID sineId = -1;
    SDL_HapticEffect sine{};
    bool sineRunning = false;
    int16_t sineMagnitude = 0;
    Clock::time_point sineRunStamp{};
    Clock::time_point springRunStamp{};
    bool motorOn = false;
    Clock::time_point motorStamp{};
    Clock::duration motorAccum{};
    Clock::time_point windowStart{};
    float level = 0.0f;
    int failures = 0;
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

bool IsKnownWheel(SDL_Joystick* joystick) {
    return IsWheelDevice(SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick));
}

SDL_Joystick* JoystickForPort(uint32_t port) {
    const int32_t index = PADGetIndexForPort(port);
    if (index < 0) {
        return nullptr;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    return gamepad != nullptr ? SDL_GetGamepadJoystick(gamepad) : nullptr;
}

bool NamesRelated(const char* joystickName, const char* hapticName) {
    if (joystickName == nullptr || hapticName == nullptr || *joystickName == '\0' ||
        *hapticName == '\0') {
        return false;
    }
    return std::strstr(joystickName, hapticName) != nullptr ||
           std::strstr(hapticName, joystickName) != nullptr;
}

SDL_Haptic* OpenMatchingHaptic(SDL_Joystick* joystick) {
    int count = 0;
    SDL_HapticID* ids = SDL_GetHaptics(&count);
    if (ids == nullptr) {
        return nullptr;
    }
    const char* joystickName = SDL_GetJoystickName(joystick);
    SDL_Haptic* chosen = nullptr;
    for (int pass = 0; pass < 2 && chosen == nullptr; ++pass) {
        for (int i = 0; i < count; ++i) {
            const bool related = NamesRelated(joystickName, SDL_GetHapticNameForID(ids[i]));
            if (pass == 0 ? !related : related) {
                continue;
            }
            SDL_Haptic* haptic = SDL_OpenHaptic(ids[i]);
            if (haptic == nullptr) {
                continue;
            }
            if (SDL_GetNumHapticAxes(haptic) >= 1 &&
                (SDL_GetHapticFeatures(haptic) & (SDL_HAPTIC_SPRING | SDL_HAPTIC_CONSTANT)) != 0) {
                chosen = haptic;
                break;
            }
            SDL_CloseHaptic(haptic);
        }
    }
    SDL_free(ids);
    return chosen;
}

SDL_HapticEffect SpringEffect(int springPercent) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SPRING;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = kSpringLength;
    const auto coeff = static_cast<int16_t>(springPercent * 0x7FFF / 100);
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
    g_session = Session{};
    g_status = status;
}

bool Guard(bool ok) {
    if (ok) {
        g_session.failures = 0;
        return true;
    }
    RT_LOG(RT_TAG_CONFIG) << "wheel ffb: effect call failed: " << SDL_GetError() << std::endl;
    if (++g_session.failures >= kMaxFailures) {
        CloseSession("Device error, retrying");
        g_retryAfter = Clock::now() + kRetryDelay;
    }
    return false;
}

void OpenSession(uint32_t port, SDL_Joystick* joystick) {
    SDL_Haptic* haptic = OpenMatchingHaptic(joystick);
    if (haptic == nullptr) {
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
    g_session.haptic = haptic;
    g_session.features = SDL_GetHapticFeatures(haptic);
    g_session.motorStamp = Clock::now();
    g_session.windowStart = g_session.motorStamp;
    if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(g_session.instance)) {
        SDL_RumbleGamepad(gamepad, 0, 0, 0);
    }
    if (PADDeadZones* zones = PADGetDeadZones(port)) {
        if (zones->stickDeadZone == kAuroraStickDeadZone) {
            zones->stickDeadZone = kWheelStickDeadZone;
        }
    }
    if ((g_session.features & SDL_HAPTIC_GAIN) != 0) {
        SDL_SetHapticGain(haptic, g_strength);
    }
    bool springRunning = false;
    if ((g_session.features & SDL_HAPTIC_SPRING) != 0) {
        SDL_HapticEffect effect = SpringEffect(g_spring);
        g_session.springId = SDL_CreateHapticEffect(haptic, &effect);
        springRunning = g_session.springId >= 0 &&
                        SDL_RunHapticEffect(haptic, g_session.springId, SDL_HAPTIC_INFINITY);
        g_session.springRunStamp = Clock::now();
    }
    if (!springRunning && (g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
        SDL_SetHapticAutocenter(haptic, g_spring);
    }
    if ((g_session.features & SDL_HAPTIC_SINE) != 0) {
        g_session.sine = SineEffect();
        g_session.sineId = SDL_CreateHapticEffect(haptic, &g_session.sine);
    }
    if (springRunning) {
        g_status = g_session.sineId >= 0 ? "Active" : "Active, no vibration support";
    } else if ((g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
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
            SDL_Joystick* candidate = JoystickForPort(port);
            if (candidate != nullptr &&
                (IsKnownWheel(candidate) || RuntimeConfigFile::FfbForceWheel())) {
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

bool IsWheelDevice(uint16_t vendor, uint16_t product) {
    return vendor == kLogitechVid &&
           std::find(std::begin(kWheelPids), std::end(kWheelPids), product) != std::end(kWheelPids);
}

void NotifyControllersChanged() { g_dirty = true; }

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
        if ((g_session.features & SDL_HAPTIC_GAIN) != 0) {
            SDL_SetHapticGain(g_session.haptic, g_strength);
        }
        SDL_HapticEffect effect = SpringEffect(g_spring);
        if (Guard(SDL_UpdateHapticEffect(g_session.haptic, g_session.springId, &effect))) {
            Guard(SDL_RunHapticEffect(g_session.haptic, g_session.springId, SDL_HAPTIC_INFINITY));
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
        target = static_cast<int>(base * static_cast<float>(g_vibration) / 100.0f);
    }
    if (target == 0) {
        if (g_session.sineRunning && Guard(SDL_StopHapticEffect(g_session.haptic, g_session.sineId))) {
            g_session.sineRunning = false;
            g_session.sineMagnitude = 0;
        }
        return;
    }
    const bool stale = now - g_session.sineRunStamp >= kSineRefresh;
    if (g_session.sineRunning && !stale &&
        std::abs(target - g_session.sineMagnitude) < kVibrationEpsilon) {
        return;
    }
    if (std::abs(target - g_session.sineMagnitude) >= kVibrationEpsilon) {
        g_session.sine.periodic.magnitude = static_cast<int16_t>(target);
        if (!Guard(SDL_UpdateHapticEffect(g_session.haptic, g_session.sineId, &g_session.sine))) {
            return;
        }
        g_session.sineMagnitude = static_cast<int16_t>(target);
    }
    if ((!g_session.sineRunning || stale) &&
        Guard(SDL_RunHapticEffect(g_session.haptic, g_session.sineId, kSineIterations))) {
        g_session.sineRunning = true;
        g_session.sineRunStamp = now;
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
    g_session.motorOn = command == PAD_MOTOR_RUMBLE;
    return true;
}

bool IsWheelPort(uint32_t port) {
    SDL_Joystick* joystick = JoystickForPort(port);
    return joystick != nullptr &&
           (IsKnownWheel(joystick) || RuntimeConfigFile::FfbForceWheel());
}

const char* StatusText() { return g_status; }

float SteeringPosition() {
    if (!g_session.active) {
        return 0.0f;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(g_session.instance);
    if (gamepad == nullptr) {
        return 0.0f;
    }
    return static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
}

void ApplyStrength(int percent) {
    g_strength = std::clamp(percent, 0, 100);
    if (g_session.active && (g_session.features & SDL_HAPTIC_GAIN) != 0) {
        SDL_SetHapticGain(g_session.haptic, g_strength);
    }
}

void ApplySpring(int percent) {
    g_spring = std::clamp(percent, 0, 100);
    if (!g_session.active) {
        return;
    }
    if (g_session.springId >= 0) {
        SDL_HapticEffect effect = SpringEffect(g_spring);
        Guard(SDL_UpdateHapticEffect(g_session.haptic, g_session.springId, &effect));
    } else if ((g_session.features & SDL_HAPTIC_AUTOCENTER) != 0) {
        SDL_SetHapticAutocenter(g_session.haptic, g_spring);
    }
}

void ApplyVibration(int percent) {
    g_vibration = std::clamp(percent, 0, 100);
}

void ApplySteeringSensitivity(int percent) {
    g_steering = std::clamp(percent, 100, 900);
}

uint32_t PedalButtons(uint32_t port) {
    const int32_t index = PADGetIndexForPort(port);
    if (index < 0) {
        return 0;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    if (gamepad == nullptr) {
        return 0;
    }
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
    if (joystick == nullptr || !IsKnownWheel(joystick)) {
        return 0;
    }
    const int axes = SDL_GetNumJoystickAxes(joystick);
    uint32_t pressed = 0;
    if (axes > 1 && SDL_GetJoystickAxis(joystick, 1) < kPedalThreshold) {
        pressed |= kPedalAccelerate;
    }
    if (axes > 2 && SDL_GetJoystickAxis(joystick, 2) < kPedalThreshold) {
        pressed |= kPedalBrake;
    }
    return pressed;
}

int32_t ShapeSteering(uint32_t port, int32_t stickX) {
    const int32_t index = PADGetIndexForPort(port);
    if (index < 0) {
        return stickX;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<uint32_t>(index));
    if (gamepad == nullptr || !IsKnownWheel(SDL_GetGamepadJoystick(gamepad))) {
        return stickX;
    }
    int32_t raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    if (std::abs(raw) <= kWheelStickDeadZone) {
        return 0;
    }
    raw = raw > 0 ? raw - kWheelStickDeadZone : raw + kWheelStickDeadZone;
    return std::clamp(raw * g_steering / (100 * 256), -127, 127);
}

} // namespace wheel_ffb
