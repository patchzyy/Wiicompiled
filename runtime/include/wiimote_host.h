#pragma once

// Physical Wii Remotes on the PC. Each one is attached to one of the four
// virtual remotes of the emulated Bluetooth controller. SDL HIDAPI reports pass
// through unchanged; the optional Linux libxwiimote backend synthesizes a
// limited HID protocol from kernel events. Both use the game's own WPAD/KPAD.

#include "bluetooth/bt_controller.h"

#include <cstddef>
#include <string>

namespace Wiimote {

struct SlotInfo {
    bool attached = false;
    // The game disconnected it and the platform could not power it off; any
    // button wakes it up again.
    bool asleep = false;
    Bluetooth::LinkState link = Bluetooth::LinkState::Down;
    std::string model;     // "Wii Remote" / "Wii Remote Plus"
    std::string extension; // empty when none
    int battery = -1;      // 0..4 like the game's battery bars, -1 unknown
};

struct PairingInfo {
    bool supported = false; // this platform can discover and pair remotes itself
    bool searching = false;
    std::string status;     // last pairing outcome, for the overlay
};

// Must run before SDL's joystick subsystem starts (aurora_initialize).
void ConfigureSdlHints(bool enabled);
// Starts device scanning when Wii Remotes are enabled.
void Initialize();
// Stops the threads. SDL remotes power off; kernel-managed remotes are released.
void Shutdown();

Bluetooth::RemoteHost& Host();

SlotInfo Slot(size_t slot);
PairingInfo Pairing();
// The console's SYNC button: look for remotes in pairing mode for a while.
void StartPairing();
// Ends the guest connection. SDL remotes power off; kernel-managed remotes
// sleep until another button press, keeping their OS Bluetooth connection.
void Disconnect(size_t slot);
// Setup problems worth showing (kernel driver, missing permissions), else empty.
std::string PlatformNotice();
// Connected remotes waiting for a free player slot.
size_t WaitingRemotes();

} // namespace Wiimote
