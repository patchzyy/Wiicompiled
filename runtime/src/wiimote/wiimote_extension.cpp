#include "wiimote_extension.h"

#include <algorithm>

namespace Wiimote {
namespace ExtensionDetail {

constexpr uint8_t kReportStatus = 0x20;
constexpr uint8_t kReportReadReply = 0x21;
constexpr uint8_t kReportWriteMemory = 0x16;
constexpr uint8_t kReportReadMemory = 0x17;
constexpr uint32_t kExtensionRegisters = 0xA40000;
constexpr uint8_t kRegisterCalibration = 0x20;
constexpr uint8_t kRegisterKey = 0x40;
constexpr uint8_t kRegisterEncryption = 0xF0;
constexpr uint8_t kRegisterIdentifier = 0xFA;
constexpr uint8_t kEncryptionOn = 0xAA;
constexpr uint8_t kEncryptionOff = 0x55;

struct ExtensionSpan {
    size_t offset;
    size_t length;
};

// Where the extension bytes sit in each data report.
std::optional<ExtensionSpan> SpanFor(uint8_t id) {
    switch (id) {
    case 0x32: return ExtensionSpan{3, 8};
    case 0x34: return ExtensionSpan{3, 19};
    case 0x35: return ExtensionSpan{6, 16};
    case 0x36: return ExtensionSpan{13, 9};
    case 0x37: return ExtensionSpan{16, 6};
    case 0x3D: return ExtensionSpan{1, 21};
    default: return std::nullopt;
    }
}

bool InExtensionSpace(uint32_t address) {
    return (address & 0xFF0000) == kExtensionRegisters;
}

} // namespace ExtensionDetail

void ExtensionState::Reset() {
    m_type = Type::Unknown;
    m_format = 0;
    m_encrypted = false;
    m_tables.reset();
    m_haveCalibration = false;
}

void ExtensionState::ObserveOutput(const uint8_t* report, size_t size) {
    using namespace ExtensionDetail;
    if (size < 6) return;
    const bool registerSpace = (report[1] & 0x04) != 0;
    const uint32_t address = (report[2] << 16) | (report[3] << 8) | report[4];
    if (report[0] == kReportReadMemory) {
        m_readAddress = registerSpace ? address : 0;
        return;
    }
    if (!registerSpace || report[0] != kReportWriteMemory || !InExtensionSpace(address)) return;
    const size_t length = std::min<size_t>(report[5], size - 6);
    bool keyChanged = false;
    for (size_t i = 0; i < length; ++i) {
        const size_t reg = (address & 0xFF) + i;
        const uint8_t value = report[6 + i];
        if (reg >= kRegisterKey && reg < kRegisterKey + m_key.size()) {
            m_key[reg - kRegisterKey] = value;
            keyChanged = true;
        } else if (reg == kRegisterEncryption) {
            if (value == kEncryptionOn) m_encrypted = true;
            if (value == kEncryptionOff) Reset();
        }
    }
    if (keyChanged) {
        m_tables = ExtCrypto::FromKey(m_key);
    }
}

void ExtensionState::ObserveInput(const uint8_t* report, size_t size) {
    using namespace ExtensionDetail;
    if (size >= 4 && report[0] == kReportStatus) {
        if ((report[3] & 0x02) == 0) Reset();
        return;
    }
    if (size < 7 || report[0] != kReportReadReply || (report[3] & 0x0F) != 0 || !InExtensionSpace(m_readAddress)) {
        return;
    }
    const size_t length = std::min<size_t>((report[3] >> 4) + 1, size - 6);
    const uint32_t first = ((report[4] << 8) | report[5]) & 0xFF;
    for (size_t i = 0; i < length; ++i) {
        const uint32_t reg = first + static_cast<uint32_t>(i);
        uint8_t value = report[6 + i];
        if (reg >= kRegisterIdentifier) {
            // The identifier is read before the game turns encryption on.
            if (reg == 0xFE) m_format = value;
            if (reg == 0xFF) m_type = value == 0x00 ? Type::Nunchuk : value == 0x01 ? Type::Classic : Type::Unknown;
        } else if (reg >= kRegisterCalibration && reg < kRegisterCalibration + m_calibration.size()) {
            if (m_encrypted) {
                if (!m_tables) continue;
                m_tables->Decrypt(&value, reg, 1);
            }
            m_calibration[reg - kRegisterCalibration] = value;
            m_haveCalibration = true;
        }
    }
}

uint8_t ExtensionState::Calibration(size_t index, uint8_t fallback) const {
    return m_haveCalibration ? m_calibration[index] : fallback;
}

void ExtensionState::Neutralize(std::vector<uint8_t>& report) const {
    using namespace ExtensionDetail;
    if (report.empty() || m_type == Type::Unknown || (m_encrypted && !m_tables)) return;
    const auto span = SpanFor(report[0]);
    if (!span || report.size() < span->offset + 6) return;
    uint8_t* data = report.data() + span->offset;
    std::array<uint8_t, 6> plain{};
    std::copy_n(data, plain.size(), plain.begin());
    if (m_encrypted) m_tables->Decrypt(plain.data(), 0, plain.size());

    if (m_type == Type::Nunchuk) {
        plain[0] = Calibration(10, 0x80);
        plain[1] = Calibration(13, 0x80);
        plain[5] |= 0x03; // C and Z are active low
    } else if (m_format == 0x01) {
        const uint8_t lx = Calibration(2, 0x80) >> 2;
        const uint8_t ly = Calibration(5, 0x80) >> 2;
        const uint8_t rx = Calibration(8, 0x80) >> 3;
        const uint8_t ry = Calibration(11, 0x80) >> 3;
        plain[0] = static_cast<uint8_t>(((rx >> 3) & 0x03) << 6 | (lx & 0x3F));
        plain[1] = static_cast<uint8_t>(((rx >> 1) & 0x03) << 6 | (ly & 0x3F));
        plain[2] = static_cast<uint8_t>((rx & 0x01) << 7 | (ry & 0x1F));
        plain[3] = 0x00;
        plain[4] = 0xFF; // buttons are active low
        plain[5] = 0xFF;
    } else {
        return;
    }

    if (m_encrypted) m_tables->Encrypt(plain.data(), 0, plain.size());
    std::copy(plain.begin(), plain.end(), data);
}

} // namespace Wiimote
