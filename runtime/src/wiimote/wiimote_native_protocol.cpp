// SPDX-License-Identifier: GPL-3.0-only
#include "wiimote_native_protocol.h"

#include <algorithm>

namespace Wiimote::Native {
namespace {
size_t DataSize(uint8_t mode) {
    switch (mode) {
    case 0x30: return 3;
    case 0x31: return 6;
    case 0x32: return 11;
    case 0x33: return 18;
    case 0x34: case 0x35: case 0x36: case 0x37: return 22;
    default: return 0; // extension-only and interleaved modes are unsupported
    }
}
bool HasAccel(uint8_t mode) {
    return mode == 0x31 || mode == 0x33 || mode == 0x35 || mode == 0x37;
}
uint8_t MemoryError(uint8_t space, uint32_t address) {
    if (space == 0) return 8;
    if (space != 4 && space != 8) return 6;
    return ((address >> 16) & 0xFE) == 0xA0 ? 8 : 7;
}
}

Protocol::Protocol() {
    // Not a dump of a physical remote: nominal zero=512, +1g=616.
    // Two ten-byte calibration blocks with sum+0x55 checksums.
    std::array<uint8_t, 10> calibration{128, 128, 128, 0, 154, 154, 154, 0, 0, 0};
    uint8_t checksum = 0x55;
    for (size_t i = 0; i < 9; ++i) checksum += calibration[i];
    calibration[9] = checksum;
    for (size_t offset : {0x16u, 0x20u})
        std::copy(calibration.begin(), calibration.end(), m_eeprom.begin() + offset);
}

void Protocol::SetButtons(uint16_t buttons) {
    buttons &= 0x1F9F;
    m_dirty |= buttons != m_buttons;
    m_buttons = buttons;
}
void Protocol::SetAccel(int32_t x, int32_t y, int32_t z) {
    const auto raw = [](int32_t value) {
        return static_cast<uint16_t>(std::clamp<int64_t>(static_cast<int64_t>(value) + 512, 0, 1023));
    };
    const std::array<uint16_t, 3> values{raw(x), raw(y), raw(z)};
    if (HasAccel(m_mode)) m_dirty |= values != m_accel;
    m_accel = values;
}
void Protocol::SetBattery(uint8_t percent) {
    // hid-wiimote maps the physical byte to 0..100 with a maximum of 0xC8.
    m_battery = static_cast<uint8_t>(std::min<unsigned>(percent, 100) * 2);
}
Report Protocol::WithButtons(uint8_t id, size_t size) const {
    Report report(size, 0);
    report[0] = id;
    report[1] = static_cast<uint8_t>(m_buttons >> 8);
    report[2] = static_cast<uint8_t>(m_buttons);
    return report;
}
void Protocol::Push(Report report) {
    // Never silently drop command replies. A stalled guest disconnects instead.
    if (m_replies.size() == 512) { m_overflow = true; return; }
    m_replies.push_back(std::move(report));
}
void Protocol::Ack(uint8_t id, uint8_t error) {
    auto report = WithButtons(0x22, 5);
    report[3] = id;
    report[4] = error;
    Push(std::move(report));
}
uint8_t* Protocol::Memory(uint8_t space, uint32_t address, size_t size) {
    if (space == 0 && address < m_eeprom.size() && size <= m_eeprom.size() - address)
        return m_eeprom.data() + address;
    // Only virtual IR/speaker configuration registers. No A4/A6 extension
    // bus: the kernel initializes physical extensions independently.
    if (space == 4 || space == 8) {
        address &= 0xFEFFFF; // I2C R/W address bit is ignored by the remote
        const size_t offset = address & 0xFFFF;
        if (offset >= 256 || size > 256 - offset) return nullptr;
        if ((address >> 16) == 0xB0) return m_irRegisters.data() + offset;
        if ((address >> 16) == 0xA2) return m_speakerRegisters.data() + offset;
    }
    return nullptr;
}

bool Protocol::Output(const uint8_t* p, size_t size) {
    if (!p || size < 2) return false;
    m_rumble = (p[1] & 1) != 0;
    const bool ack = (p[1] & 2) != 0;
    switch (p[0]) {
    case 0x10: return true;
    case 0x11:
        // Guest-visible LED state belongs to the virtual remote. Host sysfs
        // permissions do not constitute a Wii HID protocol error.
        m_leds = p[1] & 0xF0;
        if (ack) Ack(p[0], 0);
        return true;
    case 0x12:
        if (size < 3) return false;
        if (!DataSize(p[2])) { Ack(p[0], 3); return false; }
        m_mode = p[2];
        m_continuous = (p[1] & 4) != 0;
        m_dirty = true;
        if (ack) Ack(p[0], 0);
        return true;
    case 0x13: case 0x1A:
        m_ir = (p[1] & 4) != 0;
        if (ack) Ack(p[0], 0);
        return true;
    case 0x14:
        m_speaker = (p[1] & 4) != 0;
        if (ack) Ack(p[0], 0);
        return true;
    case 0x19:
        if (ack) Ack(p[0], 0);
        return true;
    case 0x18: return true; // audio discarded, as in the SDL host path
    case 0x15: {
        auto report = WithButtons(0x20, 7);
        report[3] = m_leds | (m_battery < 0x20 ? 1 : 0) |
                    (m_speaker ? 4 : 0) | (m_ir ? 8 : 0);
        // Extension-present is deliberately clear even if physically attached.
        report[6] = m_battery;
        Push(std::move(report));
        return true;
    }
    case 0x16: {
        if (size < 6 || p[5] == 0 || p[5] > 16 || size < 6u + p[5]) return false;
        const uint32_t address = (p[2] << 16) | (p[3] << 8) | p[4];
        auto* memory = Memory(p[1] & 0x0C, address, p[5]);
        const uint8_t error = memory ? 0 : MemoryError(p[1] & 0x0C, address);
        if (memory) std::copy_n(p + 6, p[5], memory);
        Ack(p[0], error);
        return error == 0;
    }
    case 0x17: {
        if (size < 7) return false;
        const uint32_t address = (p[2] << 16) | (p[3] << 8) | p[4];
        const size_t count = (p[5] << 8) | p[6];
        if (count == 0) return false;
        auto* memory = Memory(p[1] & 0x0C, address, count);
        if (!memory) {
            auto report = WithButtons(0x21, 22);
            report[3] = 0xF0 | MemoryError(p[1] & 0x0C, address);
            report[4] = static_cast<uint8_t>(address >> 8);
            report[5] = static_cast<uint8_t>(address);
            Push(std::move(report));
            return false;
        }
        for (size_t offset = 0; offset < count; offset += 16) {
            const size_t chunk = std::min<size_t>(16, count - offset);
            auto report = WithButtons(0x21, 22);
            report[3] = static_cast<uint8_t>((chunk - 1) << 4);
            report[4] = static_cast<uint8_t>((address + offset) >> 8);
            report[5] = static_cast<uint8_t>(address + offset);
            std::copy_n(memory + offset, chunk, report.begin() + 6);
            Push(std::move(report));
        }
        return true;
    }
    default:
        Ack(p[0], 3);
        return false;
    }
}
bool Protocol::PopReply(Report& report) {
    if (m_replies.empty()) return false;
    report = std::move(m_replies.front());
    m_replies.pop_front();
    return true;
}
bool Protocol::Sample(Report& report) {
    if (!m_continuous && !m_dirty) return false;
    report = WithButtons(m_mode, DataSize(m_mode));
    // Unimplemented IR/extension payloads represent no tracked dots/no device.
    std::fill(report.begin() + 3, report.end(), 0xFF);
    if (HasAccel(m_mode)) {
        report[1] |= static_cast<uint8_t>((m_accel[0] & 3) << 5);
        report[2] |= static_cast<uint8_t>((m_accel[1] & 2) << 4);
        report[2] |= static_cast<uint8_t>((m_accel[2] & 2) << 5);
        for (size_t i = 0; i < 3; ++i) report[3 + i] = static_cast<uint8_t>(m_accel[i] >> 2);
    }
    m_dirty = false;
    return true;
}
} // namespace Wiimote::Native
