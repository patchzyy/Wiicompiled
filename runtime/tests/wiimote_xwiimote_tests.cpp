// SPDX-License-Identifier: GPL-3.0-only
// Deterministic libxwiimote ABI fake: tests the actual transport without a
// controller, sysfs writes, Bluetooth operations, or input-device permissions.
#include "wiimote_xwiimote.h"
#include <xwiimote.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unistd.h>

struct xwii_iface { unsigned opened = 0; };
struct xwii_monitor { bool reported = false; };
namespace {
std::deque<xwii_event> events;
std::string devicePath;
const char* deviceType = "gen10";
unsigned available = XWII_IFACE_CORE | XWII_IFACE_ACCEL;
bool readonly = false, failLeds = false, rumbling = false;
unsigned leds = 0x40;
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #__VA_ARGS__); ++failures; } } while (0)
}
extern "C" {
xwii_monitor* xwii_monitor_new(bool, bool) { return new xwii_monitor; }
void xwii_monitor_unref(xwii_monitor* m) { delete m; }
char* xwii_monitor_poll(xwii_monitor* m) {
    if (m->reported) return nullptr;
    m->reported = true;
    return strdup(devicePath.c_str());
}
int xwii_iface_new(xwii_iface** dev, const char*) { *dev = new xwii_iface; return 0; }
void xwii_iface_unref(xwii_iface* dev) { delete dev; }
int xwii_iface_get_devtype(xwii_iface*, char** type) { *type = strdup(deviceType); return 0; }
unsigned xwii_iface_available(xwii_iface*) { return available; }
int xwii_iface_open(xwii_iface* dev, unsigned mask) {
    if (readonly && (mask & XWII_IFACE_WRITABLE)) { dev->opened = XWII_IFACE_ACCEL; return -EACCES; }
    dev->opened = mask & XWII_IFACE_ALL;
    return 0;
}
void xwii_iface_close(xwii_iface* dev, unsigned mask) { dev->opened &= ~mask; }
unsigned xwii_iface_opened(xwii_iface* dev) { return dev->opened; }
int xwii_iface_watch(xwii_iface*, bool) { return 0; }
int xwii_iface_get_led(xwii_iface*, unsigned led, bool* state) {
    *state = (leds & (1u << (led + 3))) != 0; return 0;
}
int xwii_iface_set_led(xwii_iface*, unsigned led, bool state) {
    if (failLeds) return -EACCES;
    const unsigned mask = 1u << (led + 3);
    if (state) leds |= mask; else leds &= ~mask;
    return 0;
}
int xwii_iface_get_battery(xwii_iface*, uint8_t* value) { *value = 75; return 0; }
int xwii_iface_get_fd(xwii_iface*) { return -1; }
int xwii_iface_dispatch(xwii_iface*, xwii_event* event, size_t) {
    if (events.empty()) return -EAGAIN;
    *event = events.front(); events.pop_front(); return 0;
}
int xwii_iface_rumble(xwii_iface*, bool on) { rumbling = on; return 0; }
}

int main() {
    using Wiimote::Native::Device;
    using Wiimote::Native::Report;
    char fixture[] = "/tmp/wiimote-transport-XXXXXX";
    const char* directory = mkdtemp(fixture);
    if (!directory) return EXIT_FAILURE;
    devicePath = directory;
    struct Cleanup {
        ~Cleanup() { std::filesystem::remove_all(devicePath); }
    } cleanup;
    std::ofstream(devicePath + "/uevent") << "HID_UNIQ=02:00:00:00:00:01\n";
    std::string error;
    CHECK(Wiimote::Native::Enumerate(error) == std::vector<std::string>({devicePath}));
    {
        // Captured MKW startup: an out-of-range EEPROM probe is expected,
        // then mode/IR/LED ACKs must succeed even if sysfs denies LED access.
        failLeds = true;
        auto startup = Device::Open(devicePath, error);
        CHECK(startup != nullptr);
        if (!startup) return EXIT_FAILURE;
        Report reply;
        const std::vector<Report> commands{{0x17, 0, 0, 0x17, 0x70, 0, 1},
                                          {0x12, 6, 0x30}, {0x1A, 2}, {0x11, 0x12}};
        for (size_t i = 0; i < commands.size(); ++i) {
            startup->Write(commands[i].data(), commands[i].size());
            CHECK(startup->Read(reply, 0) > 0);
            if (i == 0) CHECK(reply[0] == 0x21 && reply[3] == 0xF8);
            else CHECK(reply[0] == 0x22 && reply[3] == commands[i][0] && reply[4] == 0);
        }
        const uint8_t status[] = {0x15, 0};
        startup->Write(status, sizeof(status));
        CHECK(startup->Read(reply, 0) == 7 && (reply[3] & 0xF0) == 0x10);
        CHECK(leds == 0x40 && startup->Stats().outputErrors == 4);
        failLeds = false;
    }
    deviceType = "gen20";
    CHECK(!Device::Open(devicePath, error));
    deviceType = "gen10";
    std::ofstream(devicePath + "/uevent") << "HID_UNIQ=invalid\n";
    CHECK(!Device::Open(devicePath, error));
    std::ofstream(devicePath + "/uevent") << "HID_UNIQ=02:00:00:00:00:01\n";
    auto device = Device::Open(devicePath, error);
    CHECK(device && device->Writable() && error.empty());
    if (!device) return EXIT_FAILURE;
    CHECK(device->Address() == "020000000001");
    Report report;
    const uint16_t masks[] = {0x0100, 0x0200, 0x0800, 0x0400, 8, 4, 0x1000, 0x10, 0x80, 2, 1};
    for (unsigned key = XWII_KEY_LEFT; key <= XWII_KEY_TWO; ++key) {
        xwii_event event{};
        event.type = XWII_EVENT_KEY;
        event.v.key.code = key;
        event.v.key.state = 1;
        events.push_back(event);
        device->ResetProtocol();
        CHECK(device->Read(report, 0) == 3);
        CHECK(((report[1] << 8) | report[2]) == masks[key]);
        event.v.key.state = 0;
        events.push_back(event);
        device->ResetProtocol();
        CHECK(device->Read(report, 0) == 3 && report[1] == 0 && report[2] == 0);
    }
    xwii_event event{};
    event.type = XWII_EVENT_ACCEL;
    event.v.abs[0] = {3, 2, 106};
    events.push_back(event);
    device->ResetProtocol();
    const uint8_t mode[] = {0x12, 6, 0x31};
    CHECK(device->Write(mode, sizeof(mode)) == 3);
    CHECK(device->Read(report, 0) == 5 && report[0] == 0x22);
    CHECK(device->Read(report, 0) == 6 && report == Report({0x31, 0x60, 0x60, 128, 128, 154}));
    const uint8_t status[] = {0x15, 0};
    device->Write(status, sizeof(status));
    CHECK(device->Read(report, 0) == 7 && report[6] == 150 && (report[3] & 2) == 0);
    const uint8_t on[] = {0x11, 0x13};
    device->Write(on, sizeof(on));
    CHECK(rumbling && leds == 0x10);
    CHECK(device->Read(report, 0) == 5 && report[4] == 0);
    failLeds = true;
    const uint8_t ledsOff[] = {0x11, 3};
    device->Write(ledsOff, sizeof(ledsOff));
    CHECK(device->Read(report, 0) == 5 && report[4] == 0);
    CHECK(leds == 0x10); // physical failure is recorded, not reported as a Wii error
    failLeds = false;
    device->ResetProtocol();
    event.type = XWII_EVENT_KEY;
    event.v.key.code = XWII_KEY_A;
    event.v.key.state = 1;
    events.push_back(event);
    event.v.key.state = 0;
    events.push_back(event);
    CHECK(device->Read(report, 0) == 3 && report[2] == 8);
    CHECK(device->Read(report, 0) == 3 && report[2] == 0); // queued fast tap survives
    const auto stats = device->Stats();
    CHECK(stats.keyPresses == 12 && stats.keyReleases == 12 && stats.accelEvents == 1);
    CHECK(stats.rumbleChanges == 1 && stats.ledWrites == 4 && stats.outputErrors == 4);
    device.reset();
    CHECK(!rumbling && leds == 0x40); // cleanup restores LEDs and stops rumble
    readonly = true;
    device = Device::Open(devicePath, error);
    CHECK(device && !device->Writable());
    if (!device) return EXIT_FAILURE;
    device->Write(on, sizeof(on));
    CHECK(device->Read(report, 0) == 5 && report[4] == 0 && !rumbling);
    CHECK(device->Stats().outputErrors == 1);
    event.type = XWII_EVENT_WATCH;
    events.push_back(event);
    available = XWII_IFACE_CORE; // lost accelerometer: detach, never send stale motion
    CHECK(device->Read(report, 0) == -ENODEV);
    device.reset();
    CHECK(!Device::Open(devicePath, error));
    if (failures) return EXIT_FAILURE;
    std::puts("libxwiimote transport tests passed (fake ABI)");
}
