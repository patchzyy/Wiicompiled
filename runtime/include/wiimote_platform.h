#pragma once

// Host Bluetooth services the Wii Remote manager needs from the OS: finding and
// pairing remotes in pairing mode, naming a HID device, and dropping a remote's
// connection (which powers it off, as a console does).

#include <atomic>
#include <string>

struct SDL_hid_device_info;

namespace Wiimote::Platform {

struct Capabilities {
    bool pairing = false;
    bool powerOff = false;
};

Capabilities Query();

// One search for remotes in pairing mode (red SYNC or 1+2); pairs and connects
// each one found. Blocks for a few seconds. Returns the number connected.
int DiscoverAndPair(const std::atomic<bool>& cancel, std::string& status);

// Disconnects the remote; it switches itself off. False when not possible.
bool PowerOff(const std::string& address);

// Bluetooth address of a HID device as 12 lowercase hex digits, or empty.
std::string AddressFor(const SDL_hid_device_info& info);

// Bluetooth name ("Nintendo RVL-CNT-01-TR", ...) or the HID product string.
std::string NameFor(const std::string& address, const SDL_hid_device_info& info);

// False only when the OS knows the remote is not connected right now.
bool MaybeConnected(const std::string& address);

// False when the OS driver would fight over the device (Linux hid-wiimote).
bool Usable(const SDL_hid_device_info& info);

// Setup problems worth showing in the overlay, else empty.
std::string Notice();

} // namespace Wiimote::Platform
