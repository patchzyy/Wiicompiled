#include "motion_trick.h"

#include "input_bindings.h"
#include "runtime_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

namespace MotionTrick {
namespace {

// Long enough for the game to see the press, short enough not to hold a trick.
constexpr int kPulseFrames = 4;
// Sensors report zeros or stale data for a moment after being switched on.
constexpr int kWarmupFrames = 6;
constexpr float kIntensityDecay = 0.85f;
// Real sensors always jitter; identical readings mean something else (Steam,
// for one) switched the stream off, so ask for it again.
constexpr int kStaleFrames = 30;
constexpr uint64_t kResyncIntervalUs = 2'000'000;

struct PortState {
    SDL_JoystickID id = 0;
    bool gyro = false;
    bool accel = false;
    int warmup = 0;
    MotionSample previous;
    int staleFrames = 0;
    uint64_t lastResyncUs = 0;
    ShakeDetector detector;
    float displayIntensity = 0.0f;
    Direction pulse = Direction::None;
    int pulseFrames = 0;
    Direction lastTrick = Direction::None;
    uint64_t lastTrickUs = 0;
};

std::atomic<bool> g_enabled{RuntimeConfigFile::MotionTrickEnabled(false)};
std::atomic<int> g_sensitivity{std::clamp(RuntimeConfigFile::MotionTrickSensitivity(kDefaultSensitivity),
                                          kMinSensitivity, kMaxSensitivity)};

std::mutex g_mutex;
std::array<PortState, PAD_CHANMAX> g_ports;

SDL_Gamepad* GamepadForPort(uint32_t port) {
    const s32 index = PADGetIndexForPort(port);
    return index < 0 ? nullptr : PADGetSDLGamepadForIndex(static_cast<u32>(index));
}

uint16_t ButtonFor(Direction direction) {
    switch (direction) {
    case Direction::Up: return PAD_BUTTON_UP;
    case Direction::Down: return PAD_BUTTON_DOWN;
    case Direction::Left: return PAD_BUTTON_LEFT;
    case Direction::Right: return PAD_BUTTON_RIGHT;
    default: return 0;
    }
}

bool HeldByOtherPort(uint32_t port, SDL_JoystickID id) {
    for (uint32_t other = 0; other < g_ports.size(); ++other) {
        if (other != port && g_ports[other].id == id) {
            return true;
        }
    }
    return false;
}

void SetSensors(SDL_Gamepad* gamepad, const PortState& state, bool enabled) {
    if (state.gyro) SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, enabled);
    if (state.accel) SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, enabled);
}

void Attach(uint32_t port, SDL_Gamepad* gamepad) {
    PortState& state = g_ports[port];
    const SDL_JoystickID id = gamepad != nullptr ? SDL_GetGamepadID(gamepad) : 0;
    if (id == state.id) {
        return;
    }
    if (state.id != 0 && !HeldByOtherPort(port, state.id)) {
        if (SDL_Gamepad* previous = SDL_GetGamepadFromID(state.id)) {
            SetSensors(previous, state, false);
        }
    }
    state = PortState{};
    if (gamepad == nullptr) {
        return;
    }
    state.id = id;
    state.gyro = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO) &&
                 SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true);
    state.accel = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL) &&
                  SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true);
    state.warmup = kWarmupFrames;
}

bool SameReading(const MotionSample& a, const MotionSample& b) {
    return a.hasGyro == b.hasGyro && a.hasAccel == b.hasAccel &&
           std::memcmp(a.gyro, b.gyro, sizeof(a.gyro)) == 0 && std::memcmp(a.accel, b.accel, sizeof(a.accel)) == 0;
}

// Returns true when the sensors were restarted.
bool ResyncIfStale(SDL_Gamepad* gamepad, PortState& state, const MotionSample& sample, uint64_t now) {
    const bool stale = SameReading(sample, state.previous) && SDL_GetKeyboardFocus() != nullptr;
    state.previous = sample;
    state.staleFrames = stale ? state.staleFrames + 1 : 0;
    if (state.staleFrames < kStaleFrames || now - state.lastResyncUs < kResyncIntervalUs) {
        return false;
    }
    SetSensors(gamepad, state, false);
    SetSensors(gamepad, state, true);
    state.staleFrames = 0;
    state.lastResyncUs = now;
    state.warmup = kWarmupFrames;
    state.detector.Reset();
    return true;
}

MotionSample ReadSample(SDL_Gamepad* gamepad, const PortState& state) {
    MotionSample sample;
    sample.hasGyro = state.gyro && SDL_GetGamepadSensorData(gamepad, SDL_SENSOR_GYRO, sample.gyro, 3);
    sample.hasAccel = state.accel && SDL_GetGamepadSensorData(gamepad, SDL_SENSOR_ACCEL, sample.accel, 3) &&
                      (sample.accel[0] != 0.0f || sample.accel[1] != 0.0f || sample.accel[2] != 0.0f);
    return sample;
}

} // namespace

bool Enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) noexcept {
    g_enabled.store(enabled, std::memory_order_relaxed);
    RuntimeConfigFile::SetMotionTrickEnabled(enabled);
}

int Sensitivity() noexcept {
    return g_sensitivity.load(std::memory_order_relaxed);
}

void SetSensitivity(int level) noexcept {
    level = std::clamp(level, kMinSensitivity, kMaxSensitivity);
    g_sensitivity.store(level, std::memory_order_relaxed);
    RuntimeConfigFile::SetMotionTrickSensitivity(level);
}

bool PortHasMotion(uint32_t port) noexcept {
    SDL_Gamepad* gamepad = GamepadForPort(port);
    return gamepad != nullptr &&
           (SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO) || SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL));
}

uint64_t NowUs() noexcept {
    return SDL_GetTicksNS() / 1000;
}

void Apply(PADStatus* statuses) noexcept {
    if (statuses == nullptr) {
        return;
    }
    const bool enabled = Enabled();
    const int sensitivity = Sensitivity();
    const bool blocked = InputBindings::InputBlocked();
    const uint64_t now = NowUs();

    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        SDL_Gamepad* gamepad =
            enabled && statuses[port].err == PAD_ERR_NONE ? GamepadForPort(port) : nullptr;
        Attach(port, gamepad);
        PortState& state = g_ports[port];
        if (gamepad == nullptr || (!state.gyro && !state.accel)) {
            continue;
        }
        if (state.warmup > 0) {
            --state.warmup;
            continue;
        }

        const MotionSample sample = ReadSample(gamepad, state);
        if (ResyncIfStale(gamepad, state, sample, now)) {
            continue;
        }
        state.detector.SetSensitivity(sensitivity);
        const Direction fired = state.detector.Update(sample, now);
        state.displayIntensity = std::max(state.detector.Intensity(), state.displayIntensity * kIntensityDecay);
        if (fired != Direction::None) {
            state.lastTrick = fired;
            state.lastTrickUs = now;
            state.pulse = fired;
            state.pulseFrames = kPulseFrames;
        }
        if (blocked) {
            state.pulseFrames = 0;
        }
        if (state.pulseFrames > 0) {
            statuses[port].button |= ButtonFor(state.pulse);
            --state.pulseFrames;
        }
    }
}

PortStatus Status(uint32_t port) noexcept {
    if (port >= PAD_CHANMAX) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const PortState& state = g_ports[port];
    return {state.displayIntensity, state.lastTrick, state.lastTrickUs};
}

} // namespace MotionTrick
