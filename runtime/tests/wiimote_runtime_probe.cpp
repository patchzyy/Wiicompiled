// SPDX-License-Identifier: GPL-3.0-only
#include "wiimote_host.h"
#include "runtime_config.h"

#include <array>
#include <chrono>
#include <iostream>
#include <thread>

// The probe does not initialize guest memory or a settings overlay. These two
// callbacks are the only substitutes; manager/platform/transport code is real.
extern "C" bool PAD_HLE_RumbleEnabled() { return true; }
namespace InputBindings { bool InputBlocked() noexcept { return false; } }

int main(int argc, char** argv) {
#if defined(MKW_ENABLE_XWIIMOTE)
    constexpr bool native = true;
    std::cout << "Compiled production transports: SDL HIDAPI + Linux libxwiimote\n";
#else
    constexpr bool native = false;
    std::cout << "Compiled production transports: SDL HIDAPI\n";
#endif
    if (argc == 2 && std::string(argv[1]) == "--describe") return 0;
    if (argc == 2 && std::string(argv[1]) == "--expect-xwiimote") return native ? 0 : 1;
    if (argc == 2 && std::string(argv[1]) == "--expect-sdl") return native ? 1 : 0;
    if (argc != 1) return 1;
    // Never read or modify an installed game's configuration. The caller must
    // create portable.txt directly beside this executable before any live run.
    const auto executable = RuntimeConfigFile::ExecutableDirectory();
    const auto portable = RuntimeConfigFile::PortableRootDirectory();
    if (!portable || !executable || *portable != *executable) {
        std::cerr << "Refusing live run: portable.txt must be beside the probe executable\n";
        return 2;
    }
    if (!RuntimeConfigFile::SetWiiAutoPairEnabled(false)) return 3;
    Wiimote::ConfigureSdlHints(true);
    Wiimote::Initialize();
    auto& host = Wiimote::Host();
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    size_t linked = Bluetooth::kRemoteSlots;
    size_t samples = 0, presses = 0, releases = 0, motionChanges = 0;
    uint16_t previousButtons = 0;
    std::array<uint16_t, 3> previousAccel{};
    while (Clock::now() < deadline) {
        if (linked == Bluetooth::kRemoteSlots) {
            for (size_t slot = 0; slot < Bluetooth::kRemoteSlots; ++slot) {
                if (!host.IsAttached(slot)) continue;
                linked = slot;
                std::cout << "Production manager attached player " << slot + 1 << '\n';
                // A guest link boundary, without claiming to run WPAD/KPAD.
                host.OnLinkStateChanged(slot, Bluetooth::LinkState::Ready);
                const uint8_t status[] = {0x15, 0};
                const uint8_t calibration[] = {0x17, 0, 0, 0, 0x16, 0, 10};
                const uint8_t mode[] = {0x12, 6, 0x31};
                host.SendOutputReport(slot, status, sizeof(status));
                host.SendOutputReport(slot, calibration, sizeof(calibration));
                host.SendOutputReport(slot, mode, sizeof(mode));
                std::cout << "Press/release buttons and tilt for the remaining test window.\n" << std::flush;
                break;
            }
        } else {
            std::vector<uint8_t> report;
            while (host.PopInputReport(linked, report)) {
                if (report[0] < 0x30) {
                    std::cout << "Guest-facing command reply id=0x" << std::hex << unsigned(report[0]) << std::dec
                              << " size=" << report.size() << '\n';
                    continue;
                }
                if (report[0] != 0x31 || report.size() != 6) continue;
                ++samples;
                const uint16_t buttons = ((report[1] << 8) | report[2]) & 0x1F9F;
                if (buttons & ~previousButtons) ++presses;
                if (previousButtons & ~buttons) ++releases;
                if (buttons != previousButtons)
                    std::cout << "Buttons 0x" << std::hex << previousButtons << " -> 0x" << buttons << std::dec << '\n';
                previousButtons = buttons;
                const std::array<uint16_t, 3> accel{
                    static_cast<uint16_t>((report[3] << 2) | ((report[1] >> 5) & 3)),
                    static_cast<uint16_t>((report[4] << 2) | ((report[2] >> 4) & 2)),
                    static_cast<uint16_t>((report[5] << 2) | ((report[2] >> 5) & 2))};
                if (samples > 1 && accel != previousAccel) ++motionChanges;
                previousAccel = accel;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Wiimote::Shutdown();
    std::cout << "Production discovery probe: player=" << (linked < Bluetooth::kRemoteSlots ? linked + 1 : 0)
              << " samples=" << samples << " presses=" << presses << " releases=" << releases
              << " motion_changes=" << motionChanges << '\n';
    std::cout << "This validates production discovery/report delivery, not game steering or WPAD initialization.\n";
    return samples && presses && releases && motionChanges ? 0 : 4;
}
