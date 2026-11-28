// SPDX-License-Identifier: GPL-3.0-only
// Production manager/adapter with independent per-device libxwiimote ABI fakes.
// No Bluetooth pairing implementation, real HID enumeration, or input devices.
#include "../src/wiimote/wiimote_host.cpp"
#include <xwiimote.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

struct xwii_iface {
    std::string path;
    unsigned opened = 0;
};
struct xwii_monitor { size_t next = 0; };
namespace {
std::vector<std::string> candidates;
std::map<std::string, std::deque<xwii_event>> pending;
std::mutex fakeMutex;
unsigned created = 0, live = 0;
bool blocked = false;

void Key(const std::string& path, unsigned key, unsigned value) {
    std::lock_guard<std::mutex> lock(fakeMutex);
    xwii_event event{};
    event.type = XWII_EVENT_KEY;
    event.v.key = {key, value};
    pending[path].push_back(event);
}
void Accel(const std::string& path, int x) {
    std::lock_guard<std::mutex> lock(fakeMutex);
    xwii_event event{};
    event.type = XWII_EVENT_ACCEL;
    event.v.abs[0] = {x, 0, 104};
    pending[path].push_back(event);
}
}
extern "C" {
xwii_monitor* xwii_monitor_new(bool, bool) { return new xwii_monitor; }
void xwii_monitor_unref(xwii_monitor* monitor) { delete monitor; }
char* xwii_monitor_poll(xwii_monitor* monitor) {
    return monitor->next < candidates.size() ? strdup(candidates[monitor->next++].c_str()) : nullptr;
}
int xwii_iface_new(xwii_iface** device, const char* path) {
    *device = new xwii_iface{path};
    ++created;
    ++live;
    return 0;
}
void xwii_iface_unref(xwii_iface* device) { delete device; --live; }
int xwii_iface_get_devtype(xwii_iface*, char** type) { *type = strdup("gen10"); return 0; }
unsigned xwii_iface_available(xwii_iface*) { return XWII_IFACE_CORE | XWII_IFACE_ACCEL; }
int xwii_iface_open(xwii_iface* device, unsigned mask) { device->opened = mask & XWII_IFACE_ALL; return 0; }
void xwii_iface_close(xwii_iface* device, unsigned mask) { device->opened &= ~mask; }
unsigned xwii_iface_opened(xwii_iface* device) { return device->opened; }
int xwii_iface_watch(xwii_iface*, bool) { return 0; }
int xwii_iface_get_led(xwii_iface*, unsigned, bool* value) { *value = false; return 0; }
int xwii_iface_set_led(xwii_iface*, unsigned, bool) { return 0; }
int xwii_iface_get_battery(xwii_iface*, uint8_t* value) { *value = 70; return 0; }
int xwii_iface_rumble(xwii_iface*, bool) { return 0; }
int xwii_iface_get_fd(xwii_iface*) { return -1; }
int xwii_iface_dispatch(xwii_iface* device, xwii_event* event, size_t) {
    std::lock_guard<std::mutex> lock(fakeMutex);
    auto& queue = pending[device->path];
    if (queue.empty()) return -EAGAIN;
    *event = queue.front();
    queue.pop_front();
    return 0;
}
bool PAD_HLE_RumbleEnabled() { return true; }
}
namespace InputBindings { bool InputBlocked() noexcept { return blocked; } }
namespace Wiimote::Platform {
Capabilities Query() { return {}; }
int DiscoverAndPair(const std::atomic<bool>&, std::string&) { assert(false); return 0; }
bool PowerOff(const std::string&) { assert(false); return false; }
std::string AddressFor(const SDL_hid_device_info&) { return {}; }
std::string NameFor(const std::string&, const SDL_hid_device_info&) { return {}; }
bool MaybeConnected(const std::string&) { return false; }
bool Usable(const SDL_hid_device_info&) { return false; }
std::string Notice() { return {}; }
}
namespace Wiimote::HostDetail {
struct ManagerTestAccess {
    static void Scan(Manager& manager) { manager.EnumerateNative(); manager.AssignWaiting(); }
    static std::shared_ptr<Remote> Get(Manager& manager, size_t slot) { return manager.At(slot); }
    static void Die(Manager& manager, size_t slot) { manager.m_slots[slot]->alive = false; manager.Reap(); }
    static void Stop(Manager& manager) { manager.m_running = true; manager.Stop(); }
};
}

int main() {
    using namespace Wiimote::HostDetail;
    char name[] = "/tmp/wiimote-manager-XXXXXX";
    const char* directory = mkdtemp(name);
    assert(directory);
    const std::filesystem::path root = directory;
    const auto Fixture = [&](const char* leaf, unsigned number) {
        auto path = (root / leaf).string();
        std::filesystem::create_directory(path);
        // Locally administered test addresses, unrelated to any physical device.
        std::ofstream(path + "/uevent") << "HID_UNIQ=02:00:00:00:00:0" << number << '\n';
        return path;
    };
    const auto first = Fixture("first", 1), second = Fixture("second", 2);
    const auto reopened = Fixture("second-new-syspath", 2);
    candidates = {first, second};
    Manager manager;
    ManagerTestAccess::Scan(manager);
    ManagerTestAccess::Scan(manager);
    assert(manager.IsAttached(0) && manager.IsAttached(1));
    assert(ManagerTestAccess::Get(manager, 0)->address == "020000000001");
    assert(ManagerTestAccess::Get(manager, 1)->address == "020000000002");
    const auto count = created;
    for (int i = 0; i < 3; ++i) ManagerTestAccess::Scan(manager);
    assert(created == count && live == 2);
    // A second sysfs path for the same Bluetooth identity must not add a player.
    candidates.push_back(Fixture("first-alias", 1));
    ManagerTestAccess::Scan(manager);
    assert(!manager.IsAttached(2) && live == 2);
    candidates = {first, second};
    for (size_t slot = 0; slot < 2; ++slot) {
        manager.OnLinkStateChanged(slot, Bluetooth::LinkState::Ready);
        const uint8_t mode[] = {0x12, 6, 0x31};
        manager.SendOutputReport(slot, mode, sizeof(mode));
    }
    Key(first, XWII_KEY_A, 1);
    Key(second, XWII_KEY_TWO, 1);
    Accel(first, 15);
    Accel(second, 30);
    bool saw[2] = {false, false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !(saw[0] && saw[1])) {
        for (size_t slot = 0; slot < 2; ++slot) {
            std::vector<uint8_t> report;
            while (manager.PopInputReport(slot, report)) {
                if (report[0] != 0x31) continue;
                const auto buttons = ((report[1] << 8) | report[2]) & 0x1F9F;
                assert(buttons == 0 || buttons == (slot == 0 ? 8 : 1));
                const auto x = (report[3] << 2) | ((report[1] >> 5) & 3);
                if (buttons == (slot == 0 ? 8 : 1) && x == (slot == 0 ? 527 : 542)) saw[slot] = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(saw[0] && saw[1]);
    // The overlay clears native HOME/buttons, preserving accelerometer bits.
    auto remote = ManagerTestAccess::Get(manager, 0);
    {
        std::lock_guard<std::mutex> lock(remote->inputMutex);
        remote->input.clear();
        remote->input.push_back({0x31, 0x60, 0xE0, 128, 128, 154});
    }
    blocked = true;
    std::vector<uint8_t> neutral;
    assert(manager.PopInputReport(0, neutral));
    assert(neutral[1] == 0x60 && neutral[2] == 0x60);
    blocked = false;
    // Reconnect waits for the old guest link to end instead of stealing slot 3.
    ManagerTestAccess::Die(manager, 1);
    candidates = {reopened, first};
    ManagerTestAccess::Scan(manager);
    assert(!manager.IsAttached(1) && !manager.IsAttached(2));
    manager.OnLinkStateChanged(1, Bluetooth::LinkState::Down);
    ManagerTestAccess::Scan(manager);
    assert(manager.IsAttached(1));
    assert(ManagerTestAccess::Get(manager, 1)->address == "020000000002");
    remote.reset();
    // Five candidates never create more than the four guest slots.
    candidates = {first, reopened, Fixture("third", 3), Fixture("fourth", 4), Fixture("fifth", 5)};
    for (int i = 0; i < 4; ++i) ManagerTestAccess::Scan(manager);
    assert(live == Bluetooth::kRemoteSlots);
    ManagerTestAccess::Stop(manager);
    assert(live == 0);
    std::filesystem::remove_all(root);
    std::puts("PASS native manager: separate slots/buttons/motion, overlay mask, stable reconnect, capacity, no duplicate opens");
}
