#pragma once

#include <cstdint>

struct PADStatus;

// Real Wii Remotes paired over Bluetooth.
//
// SDL 3 ships a HIDAPI driver for them (Wii Remote alone, with Nunchuk, with a
// Classic Controller, and the Wii U Pro Controller) that exposes each as a
// regular SDL gamepad; it is off by default on SDL's side and ConfigureSdlHints
// turns it on. A remote, alone or with a Nunchuk or Classic Controller, is
// handed to the game as a real Wii Remote with that extension: the KPAD/WPAD
// HLE builds a KPADStatus (and the WPADCLStatus behind KPADGetUnifiedWpadStatus)
// from it every frame (see ReadKpadSample), so the game's own code does wheelies,
// tricks, Wii Wheel steering and the Classic Controller layout, and plugging an
// extension in or out mid-game switches control scheme like on the console. Only
// the Wii U Pro Controller, which has no Wii-era equivalent, goes through
// aurora's PAD layer as a GameCube pad.
namespace WiiRemoteInput {

enum class Kind : uint8_t {
    NotWii,
    Remote,             // Wii Remote with no extension
    RemoteWithNunchuk,
    RemoteWithClassic,
    WiiUPro,
};

// Must run before SDL's joystick subsystem is initialized (aurora does that
// inside aurora_initialize); SDL only consults the hint on its first device scan.
void ConfigureSdlHints(bool enabled);

// Classifies a gamepad by the name SDL's Wii driver reports for it.
Kind KindForName(const char* gamepadName);
// Classification of the SDL gamepad currently assigned to a game port.
Kind KindForPort(uint32_t port);
const char* KindLabel(Kind kind);

// One frame of a Wii Remote in the units KPAD uses.
struct KpadSample {
    uint32_t hold = 0;      // WPAD button bits (WPAD_BUTTON_*), Nunchuk C/Z included
    float acc[3] = {};      // remote accelerometer in g, KPAD frame (rest: y = -1)
    bool hasNunchuk = false;
    float stick[2] = {};    // Nunchuk stick, -1..1, +y up
    float nunchukAcc[3] = {};
    bool hasClassic = false;
    uint32_t clHold = 0;         // WPAD_CL_BUTTON_* bits
    float clLStick[2] = {};      // Classic sticks, -1..1, +y up
    float clRStick[2] = {};
    int16_t clLStickRaw[2] = {}; // as WPADCLStatus reports them: -512..511, centre 0, +y up
    int16_t clRStickRaw[2] = {};
    uint8_t clTriggerL = 0;      // 0..255; SDL only exposes the digital click
    uint8_t clTriggerR = 0;
};

// What the game should see on `chan`: the controller SDL has there right now,
// or, for a few seconds after a remote vanished, the kind it had. SDL's driver
// destroys and re-creates the joystick when an extension is plugged in or out,
// and the console never disconnects for that, so the gap is papered over with
// neutral input instead of a "communications interrupted" prompt.
Kind EffectiveKind(uint32_t chan);
// True when the game reads `chan` through KPAD: a Wii Remote alone, with a
// Nunchuk or with a Classic Controller (live or within the swap grace period).
bool IsRemoteChannel(uint32_t chan);
// Reads the current state of the remote on `chan`; false when IsRemoteChannel
// is false. During the swap grace period the sample is neutral.
bool ReadKpadSample(uint32_t chan, KpadSample& sample);

// Remote accelerometer for the overlay readout: the SDL sample in g after the
// zero-point correction (x right across the face, y out of the button face, z
// towards the user) and the KPAD vector built from it. False when the port has
// no remote or SDL has not delivered a sample yet.
bool ReadAccelDebug(uint32_t chan, float sdlG[3], float kpadAcc[3]);

// Accelerometer zero-point calibration. SDL's Wii driver tries to read the
// remote's factory calibration block, but over Bluetooth that read often times
// out ("Using fallback accelerometer calibration" in console.log) and it falls
// back to a nominal 0x200 zero point; a real remote then carries a per-axis
// bias of up to ~0.2 g, which held sideways as a wheel is a permanent steering
// offset.
// StartAccelCalibration expects the remote lying still with the buttons up;
// Poll() then collects samples for about a second and stores the difference to
// the ideal (0, 1, 0) g in Config.toml.
void StartAccelCalibration(uint32_t chan);
// Forgets the stored correction.
void ClearAccelCalibration();
bool IsAccelCalibrating();
// 0..1 while a calibration is collecting samples.
float AccelCalibrationProgress();
// Outcome of the last calibration attempt for the overlay, or nullptr.
const char* AccelCalibrationMessage();

// Reports "no controller" on the GameCube side for every port served through
// KPAD, so the game never sees the same remote twice. Runs after aurora's PADRead.
void HideRemotesFromPad(PADStatus* statuses, uint32_t count);

// Dolphin-style continuous scanning. SDL's HIDAPI Wii driver drops a remote on
// a failed Bluetooth read or an extension change and never re-adds it on its
// own. Poll() runs once per PADRead and, while no Wii controller is present,
// periodically forces SDL to re-enumerate HIDAPI by toggling the driver hint.
void Poll();
// Forces one re-enumeration right now (settings overlay "Rescan now").
void RescanNow();
// True while Poll() is looking for a remote (no Wii controller connected).
bool IsScanning();
// True where looking means periodic rescans; elsewhere Poll() waits for hotplug.
bool PeriodicRescanEnabled();
// Rescans issued since a Wii controller was last seen.
uint32_t ScanCount();

} // namespace WiiRemoteInput
