// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Wiimote::Native {

using Report = std::vector<uint8_t>;

// A deliberately limited, game-facing RVL-CNT-01 command processor. The
// kernel owns the physical HID protocol; none of these commands reach hidraw.
// Single-threaded; the transport serializes calls.
class Protocol {
public:
    Protocol();
    void SetButtons(uint16_t buttons); // Wii wire byte order, e.g. A = 0x0008
    void SetAccel(int32_t x, int32_t y, int32_t z); // kernel raw value minus 512
    void SetBattery(uint8_t percent);
    // Returns false on malformed/unsupported commands (errors are still queued).
    bool Output(const uint8_t* data, size_t size);
    bool PopReply(Report& report);
    bool Sample(Report& report); // caller clocks at 100 Hz; replies take priority
    bool Rumble() const { return m_rumble; }
    uint8_t Leds() const { return m_leds; }
    uint8_t Mode() const { return m_mode; }
    bool Healthy() const { return !m_overflow; }

private:
    Report WithButtons(uint8_t id, size_t size) const;
    void Push(Report report);
    void Ack(uint8_t id, uint8_t error);
    uint8_t* Memory(uint8_t space, uint32_t address, size_t size);
    uint16_t m_buttons = 0;
    std::array<uint16_t, 3> m_accel{512, 512, 512};
    uint8_t m_battery = 0; // unknown until the transport reads capacity
    uint8_t m_leds = 0;
    uint8_t m_mode = 0x30;
    bool m_continuous = false;
    bool m_dirty = true;
    bool m_rumble = false;
    bool m_ir = false;
    bool m_speaker = false;
    bool m_overflow = false;
    std::array<uint8_t, 0x1700> m_eeprom{}; // volatile synthetic EEPROM
    std::array<uint8_t, 256> m_irRegisters{};
    std::array<uint8_t, 256> m_speakerRegisters{};
    std::deque<Report> m_replies;
};

} // namespace Wiimote::Native
