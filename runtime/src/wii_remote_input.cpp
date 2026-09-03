#include "wii_remote_input.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>

namespace WiiRemoteInput {
namespace {

// Dolphin's continuous scanning polls Bluetooth about once a second; SDL's
// enumeration walks every HID device on the main thread, so stay a bit lazier.
constexpr uint64_t kScanIntervalMs = 2000;
// Right after a remote drops it is almost certainly still there, but the first
// re-open attempts tend to fail on timed-out reads, so retry quickly for a while.
constexpr uint64_t kFastScanIntervalMs = 500;
constexpr uint64_t kFastScanWindowMs = 15000;
// How long the Wii driver hint stays at "0" during a rescan; SDL applies hint
// changes on its next joystick update, once per pumped frame.
constexpr uint64_t kRescanDriverOffMs = 100;

constexpr float kStandardGravity = 9.80665f;

// SDL's Wii driver posts the remote's own buttons as raw joystick buttons
// starting at SDL_GAMEPAD_BUTTON_MISC1, in this order (SDL_hidapi_wii.c,
// EWiiButtons), whatever the extension.
enum RawWiiButton : int {
    kRawA = SDL_GAMEPAD_BUTTON_MISC1,
    kRawB,
    kRawOne,
    kRawTwo,
    kRawPlus,
    kRawMinus,
    kRawHome,
    kRawDpadUp,
    kRawDpadDown,
    kRawDpadLeft,
    kRawDpadRight,
};

// WPAD_BUTTON_* bits as the game reads them from KPADStatus.hold.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadB = 0x0400, kWpadA = 0x0800,
                   kWpadMinus = 0x1000, kWpadZ = 0x2000, kWpadC = 0x4000, kWpadHome = 0x8000;

// WPAD_CL_BUTTON_* bits (WPADCLStatus.clButton / KPADStatus.ex_status.cl.hold):
// the Classic Controller's two button bytes, inverted, high byte first.
constexpr uint32_t kClUp = 0x0001, kClLeft = 0x0002, kClZR = 0x0004, kClX = 0x0008, kClA = 0x0010, kClY = 0x0020,
                   kClB = 0x0040, kClZL = 0x0080, kClR = 0x0200, kClPlus = 0x0400, kClHome = 0x0800,
                   kClMinus = 0x1000, kClL = 0x2000, kClDown = 0x4000, kClRight = 0x8000;

// How long a vanished remote keeps its channel alive with neutral input. SDL's
// in-place reconnect after an extension change takes well under a second; a
// remote that is really gone shows up as disconnected after this.
constexpr uint64_t kExtensionSwapGraceMs = 3000;
// Rescanning closes and re-opens the Bluetooth HID handle, which some Windows
// stacks answer by dropping the link; leave SDL's own reconnect this long first.
constexpr uint64_t kScanStartDelayMs = 3000;

// Per-port memory of the last Wii controller seen there, for EffectiveKind.
struct PortMemory {
    Kind lastKind = Kind::NotWii;
    uint64_t lastSeenMs = 0;
};
std::array<PortMemory, PAD_MAX_CONTROLLERS> g_ports{};

bool g_wiiDriverEnabled = false;
uint64_t g_lastScanMs = 0;
uint32_t g_scanCount = 0;
bool g_scanning = false;
// Non-zero while a rescan has the Wii driver hint switched off (see RescanNow).
uint64_t g_driverOffSinceMs = 0;
// When the current scan started (last Wii controller seen).
uint64_t g_lostAtMs = 0;

// Instance ids whose accelerometers have been switched on. SDL keeps sensors
// off until asked and forgets that when the gamepad is closed, so a re-paired
// remote gets a fresh id and is enabled again.
std::array<SDL_JoystickID, PAD_MAX_CONTROLLERS> g_sensorsEnabledFor{};

// Zero-point correction subtracted from the remote's accelerometer, in g and in
// SDL's sensor frame; loaded from Config.toml on first use, replaced by a
// calibration run. The Nunchuk accelerometer is left uncorrected.
std::array<float, 3> g_accelOffset{};
bool g_accelOffsetLoaded = false;

// Frames sampled by a calibration run (about 1.5 s at 60 Hz) and how far a
// sample may stray from the first one before the run is declared "moved".
constexpr int kCalibrationSamples = 90;
constexpr float kCalibrationMaxDeviationG = 0.15f;
// |mean| outside this range means the remote was not at rest or the
// accelerometer is far off its nominal scale; either way the offset is useless.
constexpr float kCalibrationMinGravityG = 0.8f;
constexpr float kCalibrationMaxGravityG = 1.2f;

struct AccelCalibration {
    bool active = false;
    uint32_t chan = 0;
    int count = 0;
    double sum[3] = {};
    float first[3] = {};
};
AccelCalibration g_calibration;
char g_calibrationMessage[160] = {};

// Last accepted KPAD acc per port, for the remote and for the Nunchuk. Reused on
// frames that bring no sample or a glitched one, so the game never sees a jump.
struct LastAcc {
    bool valid = false;
    float acc[3] = {0.0f, -1.0f, 0.0f};
};
std::array<LastAcc, PAD_MAX_CONTROLLERS> g_lastAcc{};
std::array<LastAcc, PAD_MAX_CONTROLLERS> g_lastNunchukAcc{};

// Any axis beyond this is not a reading the remote's +-3 g sensor can produce.
constexpr float kMaxPlausibleRemoteG = 4.0f;

// Case-sensitive substring test that tolerates a null name.
bool NameContains(const char* name, const char* needle) {
    return name != nullptr && std::strstr(name, needle) != nullptr;
}

// Turns on the remote (and Nunchuk) accelerometers once per gamepad instance.
void EnsureSensors(SDL_Gamepad* gamepad, uint32_t port) {
    const SDL_JoystickID id = SDL_GetGamepadID(gamepad);
    if (g_sensorsEnabledFor[port] == id) {
        return;
    }
    bool allEnabled = true;
    for (SDL_SensorType sensor : {SDL_SENSOR_ACCEL, SDL_SENSOR_ACCEL_L}) {
        if (SDL_GamepadHasSensor(gamepad, sensor) && !SDL_SetGamepadSensorEnabled(gamepad, sensor, true)) {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote on port " << (port + 1)
                                  << ": could not enable an accelerometer: " << SDL_GetError() << std::endl;
            allEnabled = false;
        }
    }
    // Only remember the instance once every sensor is on, so a failed attempt
    // is retried on the next sample instead of leaving the accelerometer off.
    if (allEnabled) {
        g_sensorsEnabledFor[port] = id;
    }
}

// Loads the stored zero-point correction once.
const std::array<float, 3>& AccelOffset() {
    if (!g_accelOffsetLoaded) {
        const std::array<double, 3> stored = RuntimeConfigFile::WiiAccelOffset();
        for (size_t i = 0; i < 3; ++i) g_accelOffset[i] = static_cast<float>(stored[i]);
        g_accelOffsetLoaded = true;
    }
    return g_accelOffset;
}

// Raw SDL sample in m/s^2, rejecting anything SDL has not delivered yet.
bool ReadSdlAccel(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* sdl) {
    return SDL_GamepadSensorEnabled(gamepad, sensor) && SDL_GetGamepadSensorData(gamepad, sensor, sdl, 3) &&
           std::isfinite(sdl[0]) && std::isfinite(sdl[1]) && std::isfinite(sdl[2]);
}

// True for a sample that cannot have come from the sensor. Over Bluetooth on
// Windows the remote delivers, a few times a minute, a report whose
// accelerometer bytes are all zero; SDL decodes that as -0x200 on every axis,
// i.e. (+5.12, -5.12, -5.12) g for the remote (100 units/g) and
// (+2.56, -2.56, -2.56) g for the Nunchuk (200 units/g). Handed to the game as
// is, one such frame is a full-lock steer plus a 9 g "shake". `g` is the
// uncorrected SDL sample.
bool IsGlitchedSample(SDL_SensorType sensor, const float* g) {
    // An exact zero vector is SDL's sensor buffer before the first report, not
    // a reading (the remote never delivers 0 g on all three axes at once).
    if (g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f) {
        return true;
    }
    const float zero = sensor == SDL_SENSOR_ACCEL ? 5.12f : 2.56f;
    if (std::fabs(g[0] - zero) < 0.03f && std::fabs(g[1] + zero) < 0.03f && std::fabs(g[2] + zero) < 0.03f) {
        return true;
    }
    if (sensor == SDL_SENSOR_ACCEL) {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(g[i]) > kMaxPlausibleRemoteG) return true;
        }
    }
    return false;
}

// One accelerometer sample in g, SDL's sensor frame. The remote's own axes are
// +x left, +y towards the user, +z out of the button face (wiibrew, Dolphin);
// SDL_hidapi_wii.c posts (-wiiX, wiiZ, wiiY): x right across the face, y out of
// the face (+1 at rest, buttons up), z towards the user, i.e. away from the tip.
// The remote's sample gets the zero-point correction; the Nunchuk's does not.
// False when there is no sample yet or the sample is a glitch (see above).
bool ReadAccelG(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* g) {
    float sdl[3] = {};
    if (!ReadSdlAccel(gamepad, sensor, sdl)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) g[i] = sdl[i] / kStandardGravity;
    if (IsGlitchedSample(sensor, g)) {
        return false;
    }
    if (sensor == SDL_SENSOR_ACCEL) {
        const std::array<float, 3>& offset = AccelOffset();
        for (int i = 0; i < 3; ++i) g[i] -= offset[i];
    }
    return true;
}

// KPAD's acc is the remote reading as (-wiiX, -wiiZ, wiiY): x right across the
// face, y through the back of the remote (rest: -1 with the buttons up), z
// towards the user. That is SDL's frame with y negated. Held sideways as a
// wheel the rest vector is (1, 0, 0) and a turn shows up as z = sin(angle), so
// the sign of z is the direction of the turn; Cemu does the same conversion for
// real remotes on the Wii U.
void AccelGToKpad(const float* g, float* kpad) {
    kpad[0] = g[0];
    kpad[1] = -g[1];
    kpad[2] = g[2];
}

// Sensor -> KPAD acc for ReadKpadSample.
bool ReadAccelAsKpad(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* kpad) {
    float g[3] = {};
    if (!ReadAccelG(gamepad, sensor, g)) {
        return false;
    }
    AccelGToKpad(g, kpad);
    return true;
}

// Debug trace of every remote sample (controller.wii_accel_trace = true):
// milliseconds, port, WPAD hold bits, the uncorrected SDL sample in g and the
// KPAD acc handed to the game. One CSV per run, truncated at startup.
void TraceSample(uint32_t chan, const KpadSample& sample, const float* rawG, bool haveRaw, bool accepted) {
    static std::ofstream trace;
    static bool opened = false;
    if (!opened) {
        opened = true;
        const std::filesystem::path path = RuntimeConfigFile::ApplicationDataDirectory() / "wii_accel_trace.csv";
        trace.open(path, std::ios::trunc);
        if (trace) {
            trace << "ms,port,hold,raw_x,raw_y,raw_z,ok,kpad_x,kpad_y,kpad_z\n";
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer trace: " << path.string() << std::endl;
        } else {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer trace: could not open " << path.string() << std::endl;
        }
    }
    if (!trace) {
        return;
    }
    char line[192];
    if (haveRaw) {
        std::snprintf(line, sizeof(line), "%llu,%u,%04x,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f\n",
                      static_cast<unsigned long long>(SDL_GetTicks()), chan + 1, sample.hold, rawG[0], rawG[1],
                      rawG[2], accepted ? 1 : 0, sample.acc[0], sample.acc[1], sample.acc[2]);
    } else {
        std::snprintf(line, sizeof(line), "%llu,%u,%04x,,,,0,%.4f,%.4f,%.4f\n",
                      static_cast<unsigned long long>(SDL_GetTicks()), chan + 1, sample.hold, sample.acc[0],
                      sample.acc[1], sample.acc[2]);
    }
    trace << line;
    // A frame per line; flush so a crash or a killed process keeps the tail.
    trace.flush();
}

// Ends a calibration run with a message for the overlay.
void FinishAccelCalibration(const char* message) {
    g_calibration.active = false;
    std::snprintf(g_calibrationMessage, sizeof(g_calibrationMessage), "%s", message);
    RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer calibration: " << message << std::endl;
}

// One frame of a calibration run: accumulates the uncorrected sample and, once
// enough frames are in, stores the mean minus the ideal rest vector (0, 1, 0).
void StepAccelCalibration() {
    if (!g_calibration.active) {
        return;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(g_calibration.chan));
    if (gamepad == nullptr || !IsRemoteChannel(g_calibration.chan)) {
        FinishAccelCalibration("Cancelled: the Wii Remote went away.");
        return;
    }
    EnsureSensors(gamepad, g_calibration.chan);
    float sdl[3] = {};
    if (!ReadSdlAccel(gamepad, SDL_SENSOR_ACCEL, sdl)) {
        return; // no sample this frame; keep waiting
    }
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = sdl[i] / kStandardGravity;
    if (IsGlitchedSample(SDL_SENSOR_ACCEL, g)) {
        return; // a zeroed report, not a movement
    }
    if (g_calibration.count == 0) {
        for (int i = 0; i < 3; ++i) g_calibration.first[i] = g[i];
    } else {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(g[i] - g_calibration.first[i]) > kCalibrationMaxDeviationG) {
                FinishAccelCalibration("Failed: the remote moved. Put it down, buttons up, and try again.");
                return;
            }
        }
    }
    for (int i = 0; i < 3; ++i) g_calibration.sum[i] += g[i];
    if (++g_calibration.count < kCalibrationSamples) {
        return;
    }
    std::array<double, 3> mean{};
    for (int i = 0; i < 3; ++i) mean[i] = g_calibration.sum[i] / g_calibration.count;
    const double length = std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    if (length < kCalibrationMinGravityG || length > kCalibrationMaxGravityG || mean[1] < 0.5) {
        FinishAccelCalibration("Failed: the remote was not resting flat with the buttons up.");
        return;
    }
    const std::array<double, 3> offset = {mean[0], mean[1] - 1.0, mean[2]};
    for (int i = 0; i < 3; ++i) g_accelOffset[i] = static_cast<float>(offset[i]);
    g_accelOffsetLoaded = true;
    const bool saved = RuntimeConfigFile::SetWiiAccelOffset(offset);
    char message[160];
    std::snprintf(message, sizeof(message), "%s offset x %+.3f  y %+.3f  z %+.3f g",
                  saved ? "Calibrated:" : "Calibrated (could not write Config.toml):", offset[0], offset[1],
                  offset[2]);
    FinishAccelCalibration(message);
}

// True when any controller aurora knows about is a Wii device.
bool AnyWiiControllerConnected() {
    const uint32_t count = PADCount();
    for (uint32_t index = 0; index < count; ++index) {
        if (KindForName(PADGetNameForControllerIndex(index)) != Kind::NotWii) {
            return true;
        }
    }
    return false;
}

// Route SDL's input diagnostics (HIDAPI open failures, the Wii driver's
// extension/status messages) into console.log, minus the periodic chatter.
// A sub-warning message is written once: SDL repeats the same line on every
// enumeration (one "couldn't open /dev/hidraw7: Permission denied" per HID
// device per pass), and console.log is unbuffered, so the repeats were a
// per-pass burst of writes on the main thread for no new information.
void SDLCALL LogSdlMessage(void*, int category, SDL_LogPriority priority, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (category == SDL_LOG_CATEGORY_INPUT && priority < SDL_LOG_PRIORITY_WARN &&
        (std::strstr(message, "Motion Plus") != nullptr || std::strstr(message, "Resetting report mode") != nullptr)) {
        return;
    }
    if (category != SDL_LOG_CATEGORY_INPUT && priority < SDL_LOG_PRIORITY_WARN) {
        return;
    }
    if (priority < SDL_LOG_PRIORITY_WARN) {
        // Device paths in these messages keep changing (/dev/hidrawN climbs with
        // hotplug churn), so cap the set instead of holding one string per line
        // for the whole session.
        static std::unordered_set<std::string> s_seen;
        if (s_seen.size() >= 256) {
            s_seen.clear();
        }
        if (!s_seen.insert(message).second) {
            return;
        }
        RT_LOG("sdl") << message << " (further identical messages suppressed)" << std::endl;
        return;
    }
    RT_LOG("sdl") << message << std::endl;
}

// Whether Poll() drives its own periodic re-enumeration. The 1->0->1 hint
// flip below makes SDL close and re-open every HIDAPI device on the main
// thread, and on Linux that means an open() attempt on every /dev/hidraw node
// (each failing with EACCES until a udev rule grants access), which showed up
// as a frame hitch every scan interval even on an empty menu. It exists for
// Windows Bluetooth stacks, where a remote that drops or is switched on after
// launch is not seen again until the driver re-enumerates. Linux and macOS
// already get hotplug from udev / IOKit: SDL re-enumerates when a device
// appears, so nothing periodic is needed there. The overlay's "Rescan now"
// still works everywhere.
#if defined(_WIN32)
constexpr bool kPeriodicRescan = true;
#else
constexpr bool kPeriodicRescan = false;
#endif

// Second half of a rescan: re-enables the Wii driver once SDL has seen it off.
void FinishRescan(uint64_t now) {
    if (g_driverOffSinceMs == 0 || now - g_driverOffSinceMs < kRescanDriverOffMs) {
        return;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
    g_driverOffSinceMs = 0;
    g_lastScanMs = now;
    ++g_scanCount;
    // The first few and then every tenth, so a remote that never comes back
    // leaves a trail in console.log without flooding it.
    if (g_scanCount <= 3 || g_scanCount % 10 == 0) {
        RT_LOG(RT_TAG_CONFIG) << "Wii Remote rescan #" << g_scanCount << ": HIDAPI Wii driver re-enabled ("
                              << PADCount() << " controller(s) known to aurora)" << std::endl;
    }
}

} // namespace

// Enables SDL's HIDAPI Wii driver and player LEDs, and routes SDL's input log.
void ConfigureSdlHints(bool enabled) {
    // A rescan may be mid-flight; drop its bookkeeping so Poll() is not left
    // waiting for a FinishRescan() that can no longer happen.
    g_driverOffSinceMs = 0;
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, enabled ? "1" : "0")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII << ": " << SDL_GetError()
                              << std::endl;
    }
    // Light the player LED that matches the SDL player index, like the console does.
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED, "1")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED << ": "
                              << SDL_GetError() << std::endl;
    }
    RT_LOG(RT_TAG_CONFIG) << "Bluetooth Wii Remote support " << (enabled ? "enabled" : "disabled") << std::endl;
    g_wiiDriverEnabled = enabled;
    if (enabled) {
        SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
        SDL_SetLogOutputFunction(LogSdlMessage, nullptr);
    }
}

// Starts a rescan by disabling the Wii driver hint; Poll() finishes it.
void RescanNow() {
    if (!g_wiiDriverEnabled || g_driverOffSinceMs != 0) {
        return;
    }
    // SDL only closes the HID handle of a remote it dropped while the Wii driver
    // is disabled, and only re-opens it when the driver is enabled again; both
    // must happen on separate joystick updates, so the hint stays at "0" until
    // FinishRescan() a few frames later. Flipping 1->0->1 within one frame does
    // nothing: SDL only ever sees the final "1".
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "0");
    g_driverOffSinceMs = SDL_GetTicks();
    if (g_scanCount < 3) {
        RT_LOG(RT_TAG_CONFIG) << "Wii Remote rescan #" << (g_scanCount + 1) << ": HIDAPI Wii driver disabled"
                              << std::endl;
    }
}

// Per-frame scanning state machine: rescans while no Wii controller is present.
// Also advances an accelerometer calibration run, which needs a sample per frame
// whether or not the game is reading KPAD at that moment.
void Poll() {
    StepAccelCalibration();
    // Remember what each port had, so EffectiveKind can bridge a swap.
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        (void)EffectiveKind(port);
    }
    if (!g_wiiDriverEnabled) {
        return;
    }
    // Always complete a rescan in progress so the driver is never left disabled.
    FinishRescan(SDL_GetTicks());
    if (g_driverOffSinceMs != 0) {
        return;
    }
    if (AnyWiiControllerConnected()) {
        if (g_scanning) {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote found after " << g_scanCount << " rescan(s)" << std::endl;
        }
        g_scanning = false;
        g_scanCount = 0;
        g_lastScanMs = SDL_GetTicks();
        return;
    }
    if (!RuntimeConfigFile::WiiContinuousScanEnabled(true)) {
        g_scanning = false;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (!g_scanning) {
        RT_LOG(RT_TAG_CONFIG) << "No Wii Remote connected; "
                              << (kPeriodicRescan ? "scanning for one" : "waiting for one to be paired")
                              << " (press 1+2 on the remote)" << std::endl;
        g_scanning = true;
        g_lostAtMs = now;
    }
    if (!kPeriodicRescan || now - g_lostAtMs < kScanStartDelayMs) {
        return;
    }
    const uint64_t interval = now - g_lostAtMs < kFastScanWindowMs ? kFastScanIntervalMs : kScanIntervalMs;
    if (now - g_lastScanMs < interval) {
        return;
    }
    RescanNow();
}

// True while Poll() is looking for a remote.
bool IsScanning() {
    return g_scanning;
}

// Whether looking for a remote means periodic rescans or waiting for hotplug.
bool PeriodicRescanEnabled() {
    return kPeriodicRescan;
}

// Number of rescans since a Wii controller was last seen.
uint32_t ScanCount() {
    return g_scanCount;
}

// Maps the gamepad name SDL's Wii driver reports to a Kind.
Kind KindForName(const char* name) {
    // Names come from SDL's hidapi Wii driver: "Nintendo Wii Remote",
    // "Nintendo Wii Remote with Nunchuk", "Nintendo Wii Remote with Classic
    // Controller" and "Nintendo Wii U Pro Controller".
    if (NameContains(name, "Wii U Pro Controller")) return Kind::WiiUPro;
    if (!NameContains(name, "Wii Remote")) return Kind::NotWii;
    if (NameContains(name, "Nunchuk")) return Kind::RemoteWithNunchuk;
    if (NameContains(name, "Classic Controller")) return Kind::RemoteWithClassic;
    return Kind::Remote;
}

// Kind of the SDL gamepad assigned to a game port, NotWii when empty.
Kind KindForPort(uint32_t port) {
    if (port >= PAD_MAX_CONTROLLERS) return Kind::NotWii;
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(port));
    if (gamepad == nullptr) return Kind::NotWii;
    return KindForName(SDL_GetGamepadName(gamepad));
}

// Human-readable name of a Kind for the settings overlay.
const char* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Remote: return "Wii Remote";
    case Kind::RemoteWithNunchuk: return "Wii Remote + Nunchuk";
    case Kind::RemoteWithClassic: return "Wii Remote + Classic Controller";
    case Kind::WiiUPro: return "Wii U Pro Controller";
    default: return "Not a Wii controller";
    }
}

// True for the kinds the game reads through KPAD.
static bool IsKpadKind(Kind kind) {
    return kind == Kind::Remote || kind == Kind::RemoteWithNunchuk || kind == Kind::RemoteWithClassic;
}

// Live kind of the port, or the remembered one while a swap is in flight.
// Called from the guest thread only (PADRead, KPADRead, WPADProbe and the
// overlay's Draw all run there), so the port memory needs no locking.
Kind EffectiveKind(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) return Kind::NotWii;
    PortMemory& memory = g_ports[chan];
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    const Kind live = gamepad != nullptr ? KindForName(SDL_GetGamepadName(gamepad)) : Kind::NotWii;
    const uint64_t now = SDL_GetTicks();
    if (live != Kind::NotWii) {
        memory.lastKind = live;
        memory.lastSeenMs = now;
        return live;
    }
    if (gamepad != nullptr) {
        // Another controller took the port: the remote is not coming back here.
        memory.lastKind = Kind::NotWii;
        return Kind::NotWii;
    }
    if (IsKpadKind(memory.lastKind) && memory.lastSeenMs != 0 && now - memory.lastSeenMs < kExtensionSwapGraceMs) {
        return memory.lastKind;
    }
    return Kind::NotWii;
}

// True when the game reads the port through KPAD (live or bridging a swap).
bool IsRemoteChannel(uint32_t chan) {
    return IsKpadKind(EffectiveKind(chan));
}

// Marks KPAD-served ports as "no controller" in the GameCube pad statuses.
void HideRemotesFromPad(PADStatus* statuses, uint32_t count) {
    for (uint32_t port = 0; port < count && port < PAD_MAX_CONTROLLERS; ++port) {
        if (IsRemoteChannel(port)) {
            statuses[port] = {};
            statuses[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
}

// Neutral sample of the remembered kind, for the frames of an extension swap.
void FillGraceSample(uint32_t chan, Kind kind, KpadSample& sample) {
    sample = {};
    for (int i = 0; i < 3; ++i) sample.acc[i] = g_lastAcc[chan].acc[i];
    sample.hasNunchuk = kind == Kind::RemoteWithNunchuk;
    sample.hasClassic = kind == Kind::RemoteWithClassic;
}

// SDL's stick axis (-32767..32767, y down) as WPADCLStatus carries it: WPAD
// normalises every Classic Controller stick, whatever the report's resolution,
// to a signed 10-bit value, -512..511 with 0 at the centre and +y up (RVL SDK
// WPAD.h; wut's WPADStatusClassic documents the same range).
int16_t ClassicStickRaw(Sint16 axis, bool invert) {
    float value = static_cast<float>(axis) / 32767.0f;
    if (invert) value = -value;
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::clamp(std::lround(value * 512.0f), -512L, 511L));
}

// Samples buttons, accelerometers and the extension of the remote on a port.
bool ReadKpadSample(uint32_t chan, KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    const Kind kind = gamepad != nullptr ? KindForName(SDL_GetGamepadName(gamepad)) : Kind::NotWii;
    if (!IsKpadKind(kind)) {
        const Kind remembered = EffectiveKind(chan);
        if (!IsKpadKind(remembered)) {
            return false;
        }
        FillGraceSample(chan, remembered, sample);
        return true;
    }
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
    if (joystick == nullptr) {
        return false;
    }
    EnsureSensors(gamepad, chan);

    sample = {};
    const auto raw = [&](int index, uint32_t bit) {
        if (SDL_GetJoystickButton(joystick, index)) sample.hold |= bit;
    };
    raw(kRawA, kWpadA);
    raw(kRawB, kWpadB);
    raw(kRawOne, kWpadOne);
    raw(kRawTwo, kWpadTwo);
    raw(kRawPlus, kWpadPlus);
    raw(kRawMinus, kWpadMinus);
    raw(kRawHome, kWpadHome);
    raw(kRawDpadUp, kWpadUp);
    raw(kRawDpadDown, kWpadDown);
    raw(kRawDpadLeft, kWpadLeft);
    raw(kRawDpadRight, kWpadRight);

    float rawG[3] = {};
    const bool haveRaw = ReadSdlAccel(gamepad, SDL_SENSOR_ACCEL, rawG);
    for (float& v : rawG) v /= kStandardGravity;
    LastAcc& last = g_lastAcc[chan];
    const bool accepted = ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL, sample.acc);
    if (accepted) {
        last.valid = true;
        for (int i = 0; i < 3; ++i) last.acc[i] = sample.acc[i];
    } else {
        // No sample this frame or a glitched one: repeat the last good reading
        // (rest pose, buttons up, until there is one).
        for (int i = 0; i < 3; ++i) sample.acc[i] = last.acc[i];
    }
    if (RuntimeConfigFile::WiiAccelTraceEnabled(false)) {
        TraceSample(chan, sample, rawG, haveRaw, accepted);
    }

    if (kind == Kind::RemoteWithClassic) {
        sample.hasClassic = true;
        // The driver posts the extension's buttons as joystick buttons numbered
        // by SDL_GAMEPAD_BUTTON_*: a/b/x/y by position (a on the east), +/-,
        // Home, the L/R clicks as shoulders, the D-pad as buttons 11-14 (never
        // through SDL's gamepad mapping, which expects a hat), ZL/ZR as the
        // trigger axes.
        const auto cl = [&](int index, uint32_t bit) {
            if (SDL_GetJoystickButton(joystick, index)) sample.clHold |= bit;
        };
        cl(SDL_GAMEPAD_BUTTON_EAST, kClA);
        cl(SDL_GAMEPAD_BUTTON_SOUTH, kClB);
        cl(SDL_GAMEPAD_BUTTON_NORTH, kClX);
        cl(SDL_GAMEPAD_BUTTON_WEST, kClY);
        cl(SDL_GAMEPAD_BUTTON_START, kClPlus);
        cl(SDL_GAMEPAD_BUTTON_BACK, kClMinus);
        cl(SDL_GAMEPAD_BUTTON_GUIDE, kClHome);
        cl(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kClL);
        cl(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kClR);
        cl(SDL_GAMEPAD_BUTTON_DPAD_UP, kClUp);
        cl(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kClDown);
        cl(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kClLeft);
        cl(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kClRight);
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) sample.clHold |= kClZL;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0) sample.clHold |= kClZR;
        const Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        const Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        const Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
        sample.clLStick[0] = std::clamp(static_cast<float>(lx) / 32767.0f, -1.0f, 1.0f);
        sample.clLStick[1] = std::clamp(-static_cast<float>(ly) / 32767.0f, -1.0f, 1.0f);
        sample.clRStick[0] = std::clamp(static_cast<float>(rx) / 32767.0f, -1.0f, 1.0f);
        sample.clRStick[1] = std::clamp(-static_cast<float>(ry) / 32767.0f, -1.0f, 1.0f);
        sample.clLStickRaw[0] = ClassicStickRaw(lx, false);
        sample.clLStickRaw[1] = ClassicStickRaw(ly, true);
        sample.clRStickRaw[0] = ClassicStickRaw(rx, false);
        sample.clRStickRaw[1] = ClassicStickRaw(ry, true);
        // Only the full-press click of L/R reaches SDL; report it as a full pull.
        sample.clTriggerL = (sample.clHold & kClL) ? 255 : 0;
        sample.clTriggerR = (sample.clHold & kClR) ? 255 : 0;
    }

    if (kind == Kind::RemoteWithNunchuk) {
        sample.hasNunchuk = true;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) sample.hold |= kWpadC;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) sample.hold |= kWpadZ;
        sample.stick[0] = static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        // SDL's y grows downwards; KPAD's stick y is up-positive.
        sample.stick[1] = -static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        for (float& v : sample.stick) v = std::clamp(v, -1.0f, 1.0f);
        LastAcc& lastNunchuk = g_lastNunchukAcc[chan];
        if (ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL_L, sample.nunchukAcc)) {
            lastNunchuk.valid = true;
            for (int i = 0; i < 3; ++i) lastNunchuk.acc[i] = sample.nunchukAcc[i];
        } else {
            for (int i = 0; i < 3; ++i) sample.nunchukAcc[i] = lastNunchuk.acc[i];
        }
    }
    return true;
}

// Corrected SDL sample and KPAD vector of the remote on a port, for the overlay.
bool ReadAccelDebug(uint32_t chan, float sdlG[3], float kpadAcc[3]) {
    if (!IsRemoteChannel(chan)) {
        return false;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    if (gamepad == nullptr) {
        return false;
    }
    EnsureSensors(gamepad, chan);
    if (!ReadAccelG(gamepad, SDL_SENSOR_ACCEL, sdlG)) {
        return false;
    }
    AccelGToKpad(sdlG, kpadAcc);
    return true;
}

// Begins collecting rest samples from the remote on `chan`.
void StartAccelCalibration(uint32_t chan) {
    if (!IsRemoteChannel(chan)) {
        FinishAccelCalibration("No Wii Remote on this port.");
        return;
    }
    g_calibration = {};
    g_calibration.active = true;
    g_calibration.chan = chan;
    g_calibrationMessage[0] = '\0';
}

// Drops the stored correction and goes back to SDL's raw reading.
void ClearAccelCalibration() {
    g_calibration.active = false;
    g_accelOffset = {};
    g_accelOffsetLoaded = true;
    RuntimeConfigFile::SetWiiAccelOffset({0.0, 0.0, 0.0});
    std::snprintf(g_calibrationMessage, sizeof(g_calibrationMessage), "Calibration cleared.");
}

// True while a calibration run is collecting samples.
bool IsAccelCalibrating() {
    return g_calibration.active;
}

// Share of the calibration samples collected so far, 0..1; 0 when idle.
float AccelCalibrationProgress() {
    return g_calibration.active ? static_cast<float>(g_calibration.count) / kCalibrationSamples : 0.0f;
}

// Outcome of the last calibration run for the overlay, or nullptr before any.
const char* AccelCalibrationMessage() {
    return g_calibrationMessage[0] != '\0' ? g_calibrationMessage : nullptr;
}

} // namespace WiiRemoteInput
