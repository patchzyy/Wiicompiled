#pragma once

#include <array>
#include <cstdint>
#include <string>

struct PADStatus;

namespace Wup028Adapter {

enum class ConnectionState : uint8_t {
    Searching,
    DriverError,
    Connected,
};

struct AdapterInfo {
    ConnectionState state = ConnectionState::Searching;
    std::string deviceName;
    std::string detail;
    float pollRateHz = 0.0f;
    uint8_t inputEndpoint = 0;
    uint8_t outputEndpoint = 0;
    std::array<bool, 4> ports{};
    // Raw adapter type/status byte. High nibble 1 is wired, 2 is wireless.
    std::array<uint8_t, 4> portStatus{};
    std::array<uint64_t, 4> portChangeSequence{};
};

// Starts the project-owned WUP-028 worker. Safe to call more than once.
void Initialize();
void Shutdown();

// Returns true while an official adapter is open. Each entry is a native
// GameCube port; an empty port is represented by PAD_ERR_NO_CONTROLLER.
bool Read(std::array<PADStatus, 4>& statuses);
// Assigns a physical adapter port to a game port. Pass -1 to leave the game
// port under Aurora's normal controller assignment.
void SetPortAssignment(uint32_t gamePort, int physicalPort);
int GetPortAssignment(uint32_t gamePort);
// Returns true when the command belongs to an active WUP-028, allowing the
// caller to avoid also sending it to Aurora's unrelated controller backend.
bool SetRumble(uint32_t port, bool enabled);
AdapterInfo GetInfo();

} // namespace Wup028Adapter
