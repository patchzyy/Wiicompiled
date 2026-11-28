// SPDX-License-Identifier: GPL-3.0-only
#if defined(MKW_ENABLE_XWIIMOTE)
#include "wiimote_xwiimote.h"

#include <xwiimote.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>

namespace Wiimote::Native {
namespace {
constexpr unsigned kInputs = XWII_IFACE_CORE | XWII_IFACE_ACCEL;
// Wii report bytes, not SDL button numbers or GameCube PAD bits. In particular
// do not rotate the D-pad: WPAD/KPAD handles the game's sideways orientation.
uint16_t Button(unsigned code) {
    switch (code) {
    case XWII_KEY_LEFT: return 0x0100;
    case XWII_KEY_RIGHT: return 0x0200;
    case XWII_KEY_DOWN: return 0x0400;
    case XWII_KEY_UP: return 0x0800;
    case XWII_KEY_PLUS: return 0x1000;
    case XWII_KEY_TWO: return 0x0001;
    case XWII_KEY_ONE: return 0x0002;
    case XWII_KEY_B: return 0x0004;
    case XWII_KEY_A: return 0x0008;
    case XWII_KEY_MINUS: return 0x0010;
    case XWII_KEY_HOME: return 0x0080;
    default: return 0;
    }
}
std::string Failure(const char* operation, int ret) {
    return std::string(operation) + ": " + std::strerror(-ret) + " (" + std::to_string(ret) + ")";
}
void TraceReport(const char* direction, const uint8_t* data, size_t size) {
    std::ostringstream message;
    message << "[xwiimote-trace] " << direction << ':';
    for (size_t i = 0; i < size; ++i)
        message << ' ' << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]);
    std::cerr << message.str() << '\n';
}
}

std::vector<std::string> Enumerate(std::string& error) {
    error.clear();
    std::vector<std::string> paths;
    xwii_monitor* monitor = xwii_monitor_new(false, true);
    if (!monitor) { error = "Cannot create libxwiimote device monitor"; return paths; }
    while (char* path = xwii_monitor_poll(monitor)) {
        paths.emplace_back(path);
        std::free(path);
    }
    xwii_monitor_unref(monitor);
    return paths;
}

std::unique_ptr<Device> Device::Open(const std::string& path, std::string& error) {
    error.clear();
    auto device = std::unique_ptr<Device>(new Device);
    const char* trace = std::getenv("MKW_XWIIMOTE_TRACE");
    device->m_trace = trace && std::strcmp(trace, "1") == 0;
    int ret = xwii_iface_new(&device->m_iface, path.c_str());
    if (ret) { error = Failure("xwii_iface_new", ret); return nullptr; }
    char* type = nullptr;
    ret = xwii_iface_get_devtype(device->m_iface, &type);
    const bool gen1 = !ret && type && std::strcmp(type, "gen10") == 0;
    std::free(type);
    if (!gen1) { error = "Experimental xwiimote backend supports gen10 (RVL-CNT-01) only"; return nullptr; }
    // HID_UNIQ is the kernel's Bluetooth device address, used only for slot
    // identity. No userspace pairing or per-machine allowlist is involved.
    std::ifstream uevent(path + "/uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        if (line.rfind("HID_UNIQ=", 0) != 0) continue;
        const auto address = line.substr(9);
        if (address.size() != 17) break;
        bool valid = true;
        for (size_t i = 0; i < address.size(); ++i) {
            const unsigned char ch = address[i];
            if (i % 3 == 2) valid &= ch == ':';
            else if (std::isxdigit(ch)) device->m_address += static_cast<char>(std::tolower(ch));
            else valid = false;
        }
        if (!valid) device->m_address.clear();
        break;
    }
    if (device->m_address.empty()) {
        error = "Kernel Wii Remote has no valid Bluetooth address";
        return nullptr;
    }
    if ((xwii_iface_available(device->m_iface) & kInputs) != kInputs) {
        error = "Kernel core/accelerometer interfaces unavailable; check device access and connection";
        return nullptr;
    }
    ret = xwii_iface_open(device->m_iface, kInputs | XWII_IFACE_WRITABLE);
    if (ret) {
        xwii_iface_close(device->m_iface, XWII_IFACE_ALL);
        ret = xwii_iface_open(device->m_iface, kInputs);
        if (ret) { error = Failure("open core/accel input devices", ret); return nullptr; }
        std::cerr << "[xwiimote] Read-only input access; rumble unavailable\n";
    } else {
        device->m_writable = true;
    }
    if ((xwii_iface_opened(device->m_iface) & kInputs) != kInputs) {
        error = "libxwiimote did not open both required input interfaces";
        return nullptr;
    }
    ret = xwii_iface_watch(device->m_iface, true);
    if (ret) { error = Failure("watch device removal", ret); return nullptr; }
    for (unsigned i = 1; i <= 4; ++i) {
        bool value = false;
        if (xwii_iface_get_led(device->m_iface, i, &value) == 0 && value)
            device->m_originalLeds |= 1u << (i + 3);
    }
    uint8_t battery = 0;
    if (xwii_iface_get_battery(device->m_iface, &battery) == 0) device->m_protocol.SetBattery(battery);
    std::cerr << "[xwiimote] Opened Gen 1 core+accel, writable=" << device->m_writable
              << "; synthetic calibration; no extensions/physical IR/speaker\n";
    return device;
}

Device::~Device() {
    if (!m_iface) return;
    if (m_rumbleTouched) xwii_iface_rumble(m_iface, false);
    for (unsigned i = 1; i <= 4; ++i) {
        const unsigned mask = 1u << (i + 3);
        if (m_ledsTouched & mask) xwii_iface_set_led(m_iface, i, (m_originalLeds & mask) != 0);
    }
    xwii_iface_unref(m_iface);
}

int Device::Read(Report& report, int timeoutMs) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_protocol.Healthy()) return -ENOBUFS;
        if (m_protocol.PopReply(report)) {
            if (m_trace) TraceReport("in", report.data(), report.size());
            return static_cast<int>(report.size());
        }
        bool keyEvent = false;
        for (unsigned n = 0; n < 64; ++n) {
            xwii_event event{};
            const int ret = xwii_iface_dispatch(m_iface, &event, sizeof(event));
            if (ret == -EAGAIN) break;
            if (ret) return ret;
            if (event.type == XWII_EVENT_KEY) {
                const uint16_t bit = Button(event.v.key.code);
                if (m_trace && bit && event.v.key.state != 2)
                    std::cerr << "[xwiimote-trace] key=" << event.v.key.code << " state=" << event.v.key.state << '\n';
                if (bit && event.v.key.state == 1) ++m_stats.keyPresses;
                if (bit && event.v.key.state == 0) ++m_stats.keyReleases;
                if (event.v.key.state) m_buttons |= bit;
                else m_buttons &= ~bit;
                m_protocol.SetButtons(m_buttons);
                // Deliver a transition before consuming the next one, so a
                // quick press/release does not vanish between motion samples.
                if (bit && event.v.key.state != 2) { keyEvent = true; break; }
            } else if (event.type == XWII_EVENT_ACCEL) {
                ++m_stats.accelEvents;
                m_accel = {event.v.abs[0].x, event.v.abs[0].y, event.v.abs[0].z};
                m_protocol.SetAccel(m_accel[0], m_accel[1], m_accel[2]);
            } else if (event.type == XWII_EVENT_WATCH) {
                if ((xwii_iface_opened(m_iface) & kInputs) != kInputs ||
                    (xwii_iface_available(m_iface) & kInputs) != kInputs) return -ENODEV;
                if (m_logged.insert(0x10000).second)
                    std::cerr << "[xwiimote] Device change; physical extensions remain hidden from guest\n";
            }
        }
        if (!m_protocol.Healthy()) return -ENOBUFS;
        if (keyEvent && m_protocol.Sample(report)) return static_cast<int>(report.size());
        if (Clock::now() >= m_nextSample) {
            m_nextSample = Clock::now() + std::chrono::milliseconds(10);
            if (m_protocol.Sample(report)) return static_cast<int>(report.size());
        }
        // Bound the lock hold to 10 ms so output can run on the manager's writer.
        pollfd fd{xwii_iface_get_fd(m_iface), POLLIN, 0};
        lock.unlock();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int ret = poll(&fd, 1, static_cast<int>(std::clamp<int64_t>(remaining, 0, 10)));
        if (ret < 0 && errno != EINTR) return -errno;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -ENODEV;
    } while (Clock::now() < deadline);
    return 0;
}

int Device::Write(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || size < 2) return -EINVAL;
    if (m_trace) TraceReport("out", data, size);
    const bool rumble = (data[1] & 1) != 0;
    if (rumble != m_appliedRumble) {
        const int ret = m_writable ? xwii_iface_rumble(m_iface, rumble) : -EACCES;
        if (!ret) { m_rumbleTouched = true; m_appliedRumble = rumble; ++m_stats.rumbleChanges; }
        if (ret) ++m_stats.outputErrors;
        if (ret && m_logged.insert(0x20000).second)
            std::cerr << "[xwiimote] " << Failure("rumble", ret) << '\n';
    }
    if (data[0] == 0x11) {
        for (unsigned i = 1; i <= 4; ++i) {
            const unsigned mask = 1u << (i + 3);
            const int ret = xwii_iface_set_led(m_iface, i, (data[1] & mask) != 0);
            if (ret) {
                ++m_stats.outputErrors;
                if (m_logged.insert(0x30000 + i).second)
                    std::cerr << "[xwiimote] " << Failure("physical LED output", ret)
                              << "; guest LED state remains emulated\n";
            } else { m_ledsTouched |= mask; ++m_stats.ledWrites; }
        }
    }
    if (data[0] == 0x15) {
        uint8_t battery = 0;
        if (xwii_iface_get_battery(m_iface, &battery) == 0) m_protocol.SetBattery(battery);
    }
    const bool accepted = m_protocol.Output(data, size);
    if (!accepted) {
        const uint32_t key = size >= 5 && (data[0] == 0x16 || data[0] == 0x17)
                                 ? (data[0] << 24) | (data[2] << 16) | (data[3] << 8) | data[4]
                                 : data[0];
        if (m_logged.insert(key).second)
            std::cerr << "[xwiimote] Rejected output id/address 0x" << std::hex << key << std::dec
                      << " size=" << size << '\n';
    }
    return m_protocol.Healthy() ? static_cast<int>(size) : -ENOBUFS;
}

void Device::ResetProtocol() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_protocol = Protocol{};
    m_protocol.SetButtons(m_buttons);
    m_protocol.SetAccel(m_accel[0], m_accel[1], m_accel[2]);
    uint8_t battery = 0;
    if (xwii_iface_get_battery(m_iface, &battery) == 0) m_protocol.SetBattery(battery);
    m_nextSample = {};
}
DeviceStats Device::Stats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}
} // namespace Wiimote::Native
#endif
