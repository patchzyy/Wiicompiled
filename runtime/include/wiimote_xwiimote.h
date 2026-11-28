// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "wiimote_native_protocol.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct xwii_iface;

namespace Wiimote::Native {

struct DeviceStats {
    uint64_t keyPresses = 0;
    uint64_t keyReleases = 0;
    uint64_t accelEvents = 0;
    uint64_t rumbleChanges = 0;
    uint64_t ledWrites = 0;
    uint64_t outputErrors = 0;
};

// Enumerates kernel-owned remotes, without pairing or changing Bluetooth state.
std::vector<std::string> Enumerate(std::string& error);

class Device {
public:
    static std::unique_ptr<Device> Open(const std::string& syspath, std::string& error);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    // Same framing as SDL HIDAPI: id first, no HID transaction prefix.
    int Read(Report& report, int timeoutMs);
    int Write(const uint8_t* data, size_t size);
    void ResetProtocol(); // guest link boundary; keeps current physical input
    bool Writable() const { return m_writable; }
    const std::string& Address() const { return m_address; }
    DeviceStats Stats();

private:
    Device() = default;
    xwii_iface* m_iface = nullptr;
    bool m_writable = false;
    bool m_rumbleTouched = false;
    uint8_t m_ledsTouched = 0;
    uint8_t m_originalLeds = 0;
    bool m_appliedRumble = false;
    bool m_trace = false;
    uint16_t m_buttons = 0;
    std::string m_address;
    DeviceStats m_stats;
    std::array<int32_t, 3> m_accel{};
    std::mutex m_mutex;
    Protocol m_protocol;
    std::chrono::steady_clock::time_point m_nextSample{};
    std::set<uint32_t> m_logged;
};
} // namespace Wiimote::Native
