#include "wiimote_host.h"

#include "input_bindings.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wiimote_extension.h"
#include "wiimote_platform.h"

#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_hints.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" bool PAD_HLE_RumbleEnabled();

namespace Wiimote {
namespace HostDetail {

using Clock = std::chrono::steady_clock;
using Report = std::vector<uint8_t>;

constexpr uint16_t kNintendoVendor = 0x057E;
constexpr uint16_t kProductRemote = 0x0306;
constexpr uint16_t kProductRemotePlus = 0x0330;

constexpr auto kScanInterval = std::chrono::milliseconds(500);
constexpr auto kEnumerateInterval = std::chrono::seconds(1);
constexpr auto kProbeCooldown = std::chrono::seconds(3);
constexpr auto kKeepAlive = std::chrono::seconds(1);
constexpr auto kBlinkInterval = std::chrono::milliseconds(500);
constexpr auto kPairingWindow = std::chrono::seconds(60);
constexpr auto kGuestSearchTail = std::chrono::seconds(10);
constexpr auto kAutoPairPause = std::chrono::seconds(4);
constexpr int kReadTimeoutMs = 200;
constexpr size_t kInputQueueLimit = 32;
constexpr uint16_t kButtonMask = 0x9F1F;
constexpr uint8_t kReportSpeakerData = 0x18;

enum class Phase : uint8_t {
    Waiting,    // connected to the PC, offered to the game
    Linked,     // the game's HID channels are open
    Asleep,     // the game let it go and it could not be switched off
    PoweringOff,
};

// Input report sizes including the id: Windows always reads the longest report.
size_t ReportSize(uint8_t id, size_t read) {
    size_t size = 0;
    switch (id) {
    case 0x20: size = 7; break;
    case 0x21: size = 22; break;
    case 0x22: size = 5; break;
    case 0x30: size = 3; break;
    case 0x31: size = 6; break;
    case 0x32: size = 11; break;
    case 0x33: size = 18; break;
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x3D: case 0x3E: case 0x3F: size = 22; break;
    default: return 0;
    }
    return size <= read ? size : 0;
}

uint16_t Buttons(const Report& report) {
    if (report.size() < 3 || report[0] < 0x20 || report[0] == 0x3D) return 0;
    return static_cast<uint16_t>(((report[1] << 8) | report[2]) & kButtonMask);
}

// The game's own battery bars (WPAD's status report thresholds).
int BatteryBars(uint8_t level) {
    if (level < 0x03) return 0;
    if (level < 0x33) return 1;
    if (level < 0x44) return 2;
    if (level < 0x55) return 3;
    return 4;
}

struct Remote {
    std::string path;
    std::string address;
    std::string name;
    bool plus = false;
    SDL_hid_device* device = nullptr;
    std::thread reader;
    std::thread writer;
    std::atomic<bool> alive{true};
    std::atomic<Phase> phase{Phase::Waiting};
    std::atomic<bool> wake{true};
    std::atomic<int> battery{-1};
    std::atomic<int> extension{-1}; // -1 none, else the extension id byte
    std::atomic<bool> extensionAttached{false};
    // Rumble bit of the game's last report, before the vibration setting.
    std::atomic<bool> gameRumble{false};

    std::mutex writeMutex;
    std::condition_variable writeCv;
    std::deque<Report> writes;
    std::mutex inputMutex;
    std::deque<Report> input;
    // Emulation thread only.
    ExtensionState extensionState;
};

class Manager final : public Bluetooth::RemoteHost {
public:
    void Start();
    void Stop();

    bool IsAttached(size_t slot) const override;
    bool ConsumeWake(size_t slot) override;
    bool PopInputReport(size_t slot, std::vector<uint8_t>& report) override;
    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override;
    void OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) override;
    void OnGuestDisconnect(size_t slot) override;
    void OnGuestInquiry(bool active) override;
    const char* DeviceName(size_t slot) const override;

    SlotInfo Slot(size_t slot);
    PairingInfo Pairing();
    void StartPairing();
    void Disconnect(size_t slot);
    size_t Waiting();

private:
    std::shared_ptr<Remote> At(size_t slot) const;
    void ScanLoop();
    void DiscoveryLoop();
    void Enumerate();
    void Reap();
    void AssignWaiting();
    void Open(const SDL_hid_device_info& info);
    void SetPhase(const std::shared_ptr<Remote>& remote, Phase phase);
    void PowerOff(const std::shared_ptr<Remote>& remote);
    static void ReadLoop(std::shared_ptr<Remote> remote);
    static void WriteLoop(std::shared_ptr<Remote> remote);
    static void Queue(Remote& remote, Report report);

    mutable std::mutex m_mutex;
    std::array<std::shared_ptr<Remote>, Bluetooth::kRemoteSlots> m_slots;
    std::array<Bluetooth::LinkState, Bluetooth::kRemoteSlots> m_links{};
    std::vector<std::shared_ptr<Remote>> m_waiting;
    std::map<std::string, size_t> m_lastSlot;
    std::map<std::string, Clock::time_point> m_cooldown;
    std::set<std::string> m_refusedLogged;
    // Remotes the OS knows but that were off when last seen, by path.
    std::map<std::string, std::string> m_dormant;
    Clock::time_point m_lastEnumerate{};
    uint32_t m_deviceChanges = 0;

    std::atomic<bool> m_running{false};
    std::thread m_scanner;
    std::mutex m_scanMutex;
    std::condition_variable m_scanCv;

    std::thread m_discovery;
    std::mutex m_discoveryMutex;
    std::condition_variable m_discoveryCv;
    std::atomic<bool> m_cancelDiscovery{false};
    std::atomic<bool> m_searching{false};
    std::atomic<bool> m_guestSearching{false};
    Clock::time_point m_guestSearchUntil{};
    Clock::time_point m_pairingUntil{};
    std::string m_pairingStatus;
};

// Never destroyed: its threads may outlive static destruction at exit.
Manager& TheManager() {
    static Manager* manager = new Manager();
    return *manager;
}

std::shared_ptr<Remote> Manager::At(size_t slot) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return slot < m_slots.size() ? m_slots[slot] : nullptr;
}

bool Manager::IsAttached(size_t slot) const {
    const auto remote = At(slot);
    if (!remote || !remote->alive) return false;
    const Phase phase = remote->phase;
    return phase == Phase::Waiting || phase == Phase::Linked;
}

bool Manager::ConsumeWake(size_t slot) {
    const auto remote = At(slot);
    return remote && remote->phase == Phase::Waiting && remote->wake.exchange(false);
}

bool Manager::PopInputReport(size_t slot, std::vector<uint8_t>& report) {
    const auto remote = At(slot);
    if (!remote) return false;
    {
        std::lock_guard<std::mutex> lock(remote->inputMutex);
        if (remote->input.empty()) return false;
        report = std::move(remote->input.front());
        remote->input.pop_front();
    }
    remote->extensionState.ObserveInput(report.data(), report.size());
    if (InputBindings::InputBlocked()) {
        // The overlay owns input: the game sees the controller at rest.
        if (Buttons(report) != 0) {
            report[1] &= static_cast<uint8_t>(~(kButtonMask >> 8));
            report[2] &= static_cast<uint8_t>(~kButtonMask);
        }
        remote->extensionState.Neutralize(report);
    }
    return true;
}

void Manager::Queue(Remote& remote, Report report) {
    {
        std::lock_guard<std::mutex> lock(remote.writeMutex);
        remote.writes.push_back(std::move(report));
    }
    remote.writeCv.notify_one();
}

void Manager::SendOutputReport(size_t slot, const uint8_t* report, size_t size) {
    const auto remote = At(slot);
    if (!remote || remote->phase != Phase::Linked || size == 0) return;
    remote->extensionState.ObserveOutput(report, size);
    const bool rumble = size >= 2 && (report[1] & 0x01) != 0;
    const bool rumbleChanged = size >= 2 && remote->gameRumble.exchange(rumble) != rumble;
    // PC Bluetooth cannot feed the speaker smoothly enough to sound right, but
    // WPAD may carry a rumble change only on speaker data.
    if (report[0] == kReportSpeakerData) {
        if (rumbleChanged) Queue(*remote, {0x10, static_cast<uint8_t>(rumble ? 0x01 : 0x00)});
        return;
    }
    Queue(*remote, Report(report, report + size));
}

void Manager::OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) {
    std::shared_ptr<Remote> remote;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_links[slot] = state;
        remote = m_slots[slot];
    }
    if (!remote) return;
    if (state == Bluetooth::LinkState::Ready) {
        if (remote->phase == Phase::PoweringOff) return;
        {
            std::lock_guard<std::mutex> lock(remote->inputMutex);
            remote->input.clear();
        }
        SetPhase(remote, Phase::Linked);
        RT_LOG(RT_TAG_HLE) << "Wii Remote " << (slot + 1) << " connected to the game" << std::endl;
    } else if (state == Bluetooth::LinkState::Down && remote->phase == Phase::Linked) {
        // Lost without the game asking (e.g. the controller was reset): offer it again.
        SetPhase(remote, Phase::Waiting);
        remote->wake = true;
    }
}

void Manager::OnGuestDisconnect(size_t slot) {
    const auto remote = At(slot);
    if (!remote) return;
    RT_LOG(RT_TAG_HLE) << "Wii Remote " << (slot + 1) << " disconnected by the game" << std::endl;
    PowerOff(remote);
}

void Manager::PowerOff(const std::shared_ptr<Remote>& remote) {
    remote->wake = false;
    if (Platform::Query().powerOff && !remote->address.empty()) {
        SetPhase(remote, Phase::PoweringOff);
        std::thread([remote] {
            const bool requested = Platform::PowerOff(remote->address);
            // The HID device goes away once the remote is off; if it does not,
            // keep it asleep so a button press brings it back.
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (requested && remote->alive && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (remote->alive && remote->phase == Phase::PoweringOff) {
                remote->phase = Phase::Asleep;
                remote->writeCv.notify_one();
            }
        }).detach();
    } else {
        SetPhase(remote, Phase::Asleep);
    }
}

void Manager::SetPhase(const std::shared_ptr<Remote>& remote, Phase phase) {
    remote->phase = phase;
    remote->gameRumble = false;
    if (phase != Phase::Linked) {
        std::lock_guard<std::mutex> lock(remote->writeMutex);
        remote->writes.clear();
    }
    remote->writeCv.notify_one();
}

void Manager::OnGuestInquiry(bool active) {
    if (active) {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_guestSearching = true;
    } else {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_guestSearching = false;
        m_guestSearchUntil = Clock::now() + kGuestSearchTail;
    }
    m_discoveryCv.notify_all();
}

const char* Manager::DeviceName(size_t slot) const {
    const auto remote = At(slot);
    return remote && remote->plus ? "Nintendo RVL-CNT-01-TR" : "Nintendo RVL-CNT-01";
}

void Manager::Start() {
    if (m_running.exchange(true)) return;
    if (SDL_hid_init() != 0) {
        RT_LOG(RT_TAG_HLE) << "Wii Remotes: HID access unavailable: " << SDL_GetError() << std::endl;
        m_running = false;
        return;
    }
    m_scanner = std::thread([this] { ScanLoop(); });
    if (Platform::Query().pairing) {
        m_discovery = std::thread([this] { DiscoveryLoop(); });
    }
    const std::string notice = Platform::Notice();
    if (!notice.empty()) {
        RT_LOG(RT_TAG_HLE) << "Wii Remotes: " << notice << std::endl;
    }
}

void Manager::Stop() {
    if (!m_running.exchange(false)) return;
    m_cancelDiscovery = true;
    m_scanCv.notify_all();
    m_discoveryCv.notify_all();
    if (m_scanner.joinable()) m_scanner.join();
    // An inquiry in progress cannot be interrupted; let it finish on its own.
    if (m_discovery.joinable()) m_discovery.detach();

    std::vector<std::shared_ptr<Remote>> remotes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& remote : m_slots) {
            if (remote) remotes.push_back(remote);
            remote.reset();
        }
        remotes.insert(remotes.end(), m_waiting.begin(), m_waiting.end());
        m_waiting.clear();
    }
    for (const auto& remote : remotes) {
        remote->alive = false;
        remote->writeCv.notify_all();
    }
    // A console switches its remotes off when it shuts down.
    for (const auto& remote : remotes) {
        if (remote->writer.joinable()) remote->writer.join();
        if (remote->reader.joinable()) remote->reader.join();
        if (remote->device != nullptr) {
            const uint8_t rumbleOff[] = {0x10, 0x00};
            const uint8_t ledsOff[] = {0x11, 0x00};
            SDL_hid_write(remote->device, rumbleOff, sizeof(rumbleOff));
            SDL_hid_write(remote->device, ledsOff, sizeof(ledsOff));
            SDL_hid_close(remote->device);
            remote->device = nullptr;
        }
        if (Platform::Query().powerOff && !remote->address.empty()) {
            Platform::PowerOff(remote->address);
        }
    }
}

void Manager::ScanLoop() {
    while (m_running) {
        Reap();
        Enumerate();
        AssignWaiting();
        std::unique_lock<std::mutex> lock(m_scanMutex);
        m_scanCv.wait_for(lock, kScanInterval, [this] { return !m_running; });
    }
}

void Manager::Reap() {
    std::vector<std::shared_ptr<Remote>> dead;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& remote : m_slots) {
            if (remote && !remote->alive) {
                dead.push_back(remote);
                remote.reset();
            }
        }
        for (auto it = m_waiting.begin(); it != m_waiting.end();) {
            if (!(*it)->alive) {
                dead.push_back(*it);
                it = m_waiting.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& remote : dead) {
        remote->writeCv.notify_all();
        if (remote->writer.joinable()) remote->writer.join();
        if (remote->reader.joinable()) remote->reader.join();
        if (remote->device != nullptr) SDL_hid_close(remote->device);
        remote->device = nullptr;
        RT_LOG(RT_TAG_HLE) << "Wii Remote " << (remote->address.empty() ? remote->path : remote->address)
                           << " disconnected from the PC" << std::endl;
        m_cooldown[remote->path] = Clock::now() + std::chrono::seconds(1);
        if (!remote->address.empty()) m_dormant[remote->path] = remote->address;
    }
}

void Manager::Enumerate() {
    const Uint32 changes = SDL_hid_device_change_count();
    const auto now = Clock::now();
    bool fullSlots = false;
    bool anyRemote = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fullSlots = std::all_of(m_slots.begin(), m_slots.end(), [](const auto& remote) { return remote != nullptr; });
        anyRemote = !m_waiting.empty() ||
                    std::any_of(m_slots.begin(), m_slots.end(), [](const auto& remote) { return remote != nullptr; });
    }
    bool pairing = false;
    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        pairing = m_guestSearching || now < m_guestSearchUntil || now < m_pairingUntil;
    }
    if (fullSlots) return;
    if (changes == m_deviceChanges) {
        if (now - m_lastEnumerate < kEnumerateInterval) return;
        // Once a remote is connected, skip HID enumeration unless pairing or a known remote came back.
        if (anyRemote && !pairing &&
            std::none_of(m_dormant.begin(), m_dormant.end(),
                         [](const auto& entry) { return Platform::MaybeConnected(entry.second); })) {
            m_lastEnumerate = now;
            return;
        }
    }
    m_deviceChanges = changes;
    m_lastEnumerate = now;

    std::set<std::string> open;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& remote : m_slots) {
            if (remote) open.insert(remote->path);
        }
        for (const auto& remote : m_waiting) open.insert(remote->path);
    }
    std::set<std::string> listed;
    SDL_hid_device_info* list = SDL_hid_enumerate(kNintendoVendor, 0);
    for (const SDL_hid_device_info* info = list; info != nullptr; info = info->next) {
        if (info->path == nullptr || (info->product_id != kProductRemote && info->product_id != kProductRemotePlus)) {
            continue;
        }
        listed.insert(info->path);
        if (open.count(info->path) != 0) continue;
        const auto cooldown = m_cooldown.find(info->path);
        if (cooldown != m_cooldown.end() && now < cooldown->second) continue;
        open.insert(info->path);
        Open(*info);
    }
    SDL_hid_free_enumeration(list);
    for (auto it = m_dormant.begin(); it != m_dormant.end();) {
        it = listed.count(it->first) != 0 ? std::next(it) : m_dormant.erase(it);
    }
}

void Manager::Open(const SDL_hid_device_info& info) {
    const std::string path = info.path;
    const std::string address = Platform::AddressFor(info);
    const std::string name = Platform::NameFor(address, info);
    // Wii U Pro Controllers share the Wii Remote Plus id; balance boards are not used by the game.
    if (name.find("-UC") != std::string::npos || name.find("RVL-WBC") != std::string::npos) {
        m_cooldown[path] = Clock::time_point::max();
        return;
    }
    if (!address.empty() && !Platform::MaybeConnected(address)) {
        // Known to the OS but switched off: check again soon, a button press
        // reconnects it.
        m_cooldown[path] = Clock::now() + kScanInterval;
        m_dormant[path] = address;
        return;
    }
    if (!Platform::Usable(info)) {
        if (m_refusedLogged.insert(path).second) {
            RT_LOG(RT_TAG_HLE) << "Wii Remote " << address << " is held by the system driver: " << Platform::Notice()
                               << std::endl;
        }
        m_cooldown[path] = Clock::now() + std::chrono::seconds(10);
        return;
    }
    SDL_hid_device* device = SDL_hid_open_path(path.c_str());
    if (device == nullptr) {
        m_cooldown[path] = Clock::now() + kProbeCooldown;
        return;
    }
    // A remote the OS remembers but that is switched off still has a HID device;
    // only a write tells.
    const uint8_t status[] = {0x15, 0x00};
    if (SDL_hid_write(device, status, sizeof(status)) < 0) {
        SDL_hid_close(device);
        m_cooldown[path] = Clock::now() + kProbeCooldown;
        if (!address.empty()) m_dormant[path] = address;
        return;
    }
    m_dormant.erase(path);

    auto remote = std::make_shared<Remote>();
    remote->path = path;
    remote->address = address;
    remote->name = name;
    remote->plus = info.product_id == kProductRemotePlus;
    remote->device = device;
    remote->reader = std::thread(ReadLoop, remote);
    remote->writer = std::thread(WriteLoop, remote);
    RT_LOG(RT_TAG_HLE) << (remote->plus ? "Wii Remote Plus " : "Wii Remote ")
                       << (address.empty() ? path : address) << " connected to the PC" << std::endl;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waiting.push_back(remote);
    }
    AssignWaiting();
}

void Manager::AssignWaiting() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_waiting.begin(); it != m_waiting.end();) {
        const std::shared_ptr<Remote>& remote = *it;
        const std::string key = remote->address.empty() ? remote->path : remote->address;
        size_t slot = Bluetooth::kRemoteSlots;
        const auto last = m_lastSlot.find(key);
        // A slot is free once the game has seen its previous remote go.
        const auto free = [this](size_t i) { return !m_slots[i] && m_links[i] == Bluetooth::LinkState::Down; };
        if (last != m_lastSlot.end() && free(last->second)) {
            slot = last->second;
        } else {
            for (size_t i = 0; i < m_slots.size(); ++i) {
                if (free(i)) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot == Bluetooth::kRemoteSlots) return;
        m_slots[slot] = remote;
        m_lastSlot[key] = slot;
        it = m_waiting.erase(it);
    }
}

void Manager::ReadLoop(std::shared_ptr<Remote> remote) {
    std::array<uint8_t, 32> buffer{};
    while (remote->alive) {
        const int read = SDL_hid_read_timeout(remote->device, buffer.data(), buffer.size(), kReadTimeoutMs);
        if (read < 0) {
            remote->alive = false;
            remote->writeCv.notify_all();
            break;
        }
        if (read == 0) continue;
        const size_t size = ReportSize(buffer[0], static_cast<size_t>(read));
        if (size == 0) continue;
        Report report(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));

        if (report[0] == 0x20) {
            remote->battery = BatteryBars(report[6]);
            const bool attached = (report[3] & 0x02) != 0;
            remote->extensionAttached = attached;
            if (!attached) remote->extension = -1;
        } else if (report[0] == 0x21 && (report[3] & 0x0F) == 0 && report[4] == 0x00 && report[5] == 0xFA &&
                   (report[3] >> 4) >= 5) {
            // The game reading the extension id at 0xA400FA (before encryption).
            remote->extension = report[11];
        }

        const Phase phase = remote->phase;
        if (phase == Phase::Linked) {
            std::lock_guard<std::mutex> lock(remote->inputMutex);
            if (remote->input.size() >= kInputQueueLimit) {
                auto stale = std::find_if(remote->input.begin(), remote->input.end(),
                                          [](const Report& r) { return r[0] >= 0x30; });
                remote->input.erase(stale != remote->input.end() ? stale : remote->input.begin());
            }
            remote->input.push_back(std::move(report));
        } else if (Buttons(report) != 0) {
            if (phase == Phase::Asleep) {
                remote->phase = Phase::Waiting;
                remote->writeCv.notify_one();
            }
            if (phase == Phase::Asleep || phase == Phase::Waiting) {
                remote->wake = true;
            }
        }
    }
}

void Manager::WriteLoop(std::shared_ptr<Remote> remote) {
    auto lastWrite = Clock::now();
    auto lastBlink = Clock::time_point{};
    bool blinkOn = false;
    bool appliedRumble = false;
    Phase lastPhase = Phase::Linked; // forces the quiet setup on the first pass
    const auto write = [&](const Report& report) {
        if (SDL_hid_write(remote->device, report.data(), report.size()) < 0) {
            remote->alive = false;
            return false;
        }
        lastWrite = Clock::now();
        return true;
    };
    while (remote->alive) {
        std::deque<Report> batch;
        {
            std::unique_lock<std::mutex> lock(remote->writeMutex);
            remote->writeCv.wait_for(lock, std::chrono::milliseconds(100),
                                     [&] { return !remote->alive || !remote->writes.empty(); });
            batch.swap(remote->writes);
        }
        const bool rumbleAllowed = PAD_HLE_RumbleEnabled();
        const Phase phase = remote->phase;
        // Queued just before the game let go of the remote.
        if (phase != Phase::Linked) batch.clear();
        for (Report& report : batch) {
            if (report.size() >= 2 && !rumbleAllowed) report[1] &= 0xFE;
            if (!write(report)) break;
            if (report.size() >= 2) appliedRumble = (report[1] & 0x01) != 0;
        }
        if (!remote->alive) break;

        const auto now = Clock::now();
        if (phase != lastPhase && phase != Phase::Linked && phase != Phase::PoweringOff) {
            // Stop streaming and rumbling while the game does not own the remote.
            if (!write({0x12, 0x00, 0x30}) || !write({0x11, 0x00})) break;
            appliedRumble = false;
            blinkOn = false;
            lastBlink = now;
        }
        lastPhase = phase;
        if (phase == Phase::Waiting && now - lastBlink >= kBlinkInterval) {
            // Like a remote looking for its console.
            blinkOn = !blinkOn;
            lastBlink = now;
            if (!write({0x11, static_cast<uint8_t>(blinkOn ? 0xF0 : 0x00)})) break;
        } else if (phase == Phase::Linked) {
            const bool wanted = remote->gameRumble && rumbleAllowed;
            if (wanted != appliedRumble || now - lastWrite >= kKeepAlive) {
                // Also how Windows notices a remote that went away.
                if (!write({0x10, static_cast<uint8_t>(wanted ? 0x01 : 0x00)})) break;
                appliedRumble = wanted;
            }
        } else if (phase == Phase::Asleep && now - lastWrite >= kKeepAlive) {
            if (!write({0x10, 0x00})) break;
        }
    }
    remote->writeCv.notify_all();
}

void Manager::DiscoveryLoop() {
    while (m_running) {
        bool search = false;
        bool automatic = false;
        {
            std::unique_lock<std::mutex> lock(m_discoveryMutex);
            const auto now = Clock::now();
            bool anyRemote = false;
            {
                std::lock_guard<std::mutex> slotLock(m_mutex);
                for (const auto& remote : m_slots) anyRemote |= remote != nullptr;
                anyRemote |= !m_waiting.empty();
            }
            const bool requested = m_guestSearching || now < m_guestSearchUntil || now < m_pairingUntil;
            automatic = !requested && !anyRemote && RuntimeConfigFile::WiiAutoPairEnabled();
            search = requested || automatic;
            if (!search) {
                m_searching = false;
                m_discoveryCv.wait_for(lock, std::chrono::seconds(1));
                continue;
            }
        }
        m_searching = true;
        std::string status;
        const int connected = Platform::DiscoverAndPair(m_cancelDiscovery, status);
        {
            std::lock_guard<std::mutex> lock(m_discoveryMutex);
            if (!status.empty()) m_pairingStatus = status;
            if (connected > 0) {
                m_scanCv.notify_all();
            }
        }
        if (automatic && connected == 0) {
            std::unique_lock<std::mutex> lock(m_discoveryMutex);
            m_discoveryCv.wait_for(lock, kAutoPairPause, [this] {
                return !m_running || m_guestSearching || Clock::now() < m_pairingUntil;
            });
        }
    }
    m_searching = false;
}

SlotInfo Manager::Slot(size_t slot) {
    SlotInfo info;
    std::shared_ptr<Remote> remote;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (slot >= m_slots.size()) return info;
        remote = m_slots[slot];
        info.link = m_links[slot];
    }
    if (!remote || !remote->alive || remote->phase == Phase::PoweringOff) {
        info.link = Bluetooth::LinkState::Down;
        return info;
    }
    info.attached = true;
    info.asleep = remote->phase == Phase::Asleep;
    info.model = remote->plus ? "Wii Remote Plus" : "Wii Remote";
    if (remote->extensionAttached) {
        switch (remote->extension.load()) {
        case 0x00: info.extension = "Nunchuk"; break;
        case 0x01: info.extension = "Classic Controller"; break;
        case 0x03: info.extension = "Classic Controller Pro"; break;
        default: info.extension = "Extension"; break;
        }
    }
    info.battery = remote->battery;
    return info;
}

PairingInfo Manager::Pairing() {
    PairingInfo info;
    info.supported = Platform::Query().pairing && m_running;
    info.searching = m_searching;
    std::lock_guard<std::mutex> lock(m_discoveryMutex);
    info.status = m_pairingStatus;
    return info;
}

void Manager::StartPairing() {
    {
        std::lock_guard<std::mutex> lock(m_discoveryMutex);
        m_pairingUntil = Clock::now() + kPairingWindow;
        m_pairingStatus = "Press the red SYNC button (or 1 and 2) on the Wii Remote.";
    }
    m_discoveryCv.notify_all();
}

void Manager::Disconnect(size_t slot) {
    if (const auto remote = At(slot)) {
        PowerOff(remote);
    }
}

size_t Manager::Waiting() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_waiting.size();
}

bool g_enabled = false;

} // namespace HostDetail

using namespace HostDetail;

void ConfigureSdlHints(bool enabled) {
    g_enabled = enabled;
    // The remotes belong to the game's Bluetooth stack: SDL must not open them
    // as gamepads (its own Wii driver, or the Linux kernel driver's evdev nodes).
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, enabled ? "0" : "1");
    if (enabled) {
        std::string ignored = "0x057e/0x0306,0x057e/0x0330";
        if (const char* existing = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES); existing && *existing) {
            ignored = std::string(existing) + "," + ignored;
        }
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, ignored.c_str());
    }
}

void Initialize() {
    if (g_enabled) {
        TheManager().Start();
    }
}

void Shutdown() {
    TheManager().Stop();
}

Bluetooth::RemoteHost& Host() {
    return TheManager();
}

SlotInfo Slot(size_t slot) {
    return TheManager().Slot(slot);
}

PairingInfo Pairing() {
    return TheManager().Pairing();
}

void StartPairing() {
    TheManager().StartPairing();
}

void Disconnect(size_t slot) {
    TheManager().Disconnect(slot);
}

std::string PlatformNotice() {
    return g_enabled ? Platform::Notice() : std::string();
}

size_t WaitingRemotes() {
    return TheManager().Waiting();
}

} // namespace Wiimote
