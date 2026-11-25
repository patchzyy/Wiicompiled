#if defined(_WIN32)

#include "wiimote_platform.h"

#include "runtime_log.h"

#include <SDL3/SDL_hidapi.h>

#include <windows.h>
#include <bluetoothapis.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace Wiimote::Platform {
namespace Detail {

// bthioctl.h IOCTL_BTH_DISCONNECT_DEVICE: CTL_CODE(FILE_DEVICE_BLUETOOTH, 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS).
constexpr DWORD kIoctlBthDisconnectDevice = 0x0041000C;
const GUID kHidServiceClass = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
// Each inquiry lasts this many 1.28 s units.
constexpr UCHAR kInquiryUnits = 3;

struct Radio {
    HANDLE handle = nullptr;
    BLUETOOTH_RADIO_INFO info{};
};

class Radios {
public:
    Radios() {
        BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(params)};
        HANDLE radio = nullptr;
        HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
        if (find == nullptr) return;
        do {
            Radio entry;
            entry.handle = radio;
            entry.info.dwSize = sizeof(entry.info);
            if (BluetoothGetRadioInfo(radio, &entry.info) == ERROR_SUCCESS) {
                m_radios.push_back(entry);
            } else {
                CloseHandle(radio);
            }
        } while (BluetoothFindNextRadio(find, &radio));
        BluetoothFindRadioClose(find);
    }
    ~Radios() {
        for (const Radio& radio : m_radios) CloseHandle(radio.handle);
    }
    Radios(const Radios&) = delete;
    Radios& operator=(const Radios&) = delete;
    const std::vector<Radio>& All() const { return m_radios; }

private:
    std::vector<Radio> m_radios;
};

bool IsRemoteName(const WCHAR* name) {
    const std::wstring text = name;
    return text.rfind(L"Nintendo RVL-CNT-01", 0) == 0 && text.find(L"-UC") == std::wstring::npos;
}

std::string Hex(const BLUETOOTH_ADDRESS& address) {
    char text[13];
    std::snprintf(text, sizeof(text), "%012llx", static_cast<unsigned long long>(address.ullLong & 0xFFFFFFFFFFFFull));
    return text;
}

bool ParseHex(const std::string& text, BLUETOOTH_ADDRESS& address) {
    if (text.size() != 12) return false;
    address.ullLong = std::stoull(text, nullptr, 16);
    return true;
}

ULONGLONG FileTimeOf(const SYSTEMTIME& time) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) return 0;
    return (static_cast<ULONGLONG>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
}

ULONGLONG Now() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    return FileTimeOf(now);
}

// The remote takes the host's address as PIN after its SYNC button, its own
// address after 1+2; both as raw bytes, least significant first.
DWORD Authenticate(const Radio& radio, BLUETOOTH_DEVICE_INFO& device, const BLUETOOTH_ADDRESS& pinAddress) {
    WCHAR pin[6];
    for (int i = 0; i < 6; ++i) pin[i] = pinAddress.rgBytes[i];
    return BluetoothAuthenticateDevice(nullptr, radio.handle, &device, pin, 6);
}

// A remote that was just paired needs a few seconds to connect its HID service;
// until then it must not be taken for a stale pairing.
constexpr auto kPairingSettleTime = std::chrono::seconds(30);
std::mutex g_recentMutex;
std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> g_recent;

bool RecentlyPaired(const std::string& address) {
    std::lock_guard<std::mutex> lock(g_recentMutex);
    const auto now = std::chrono::steady_clock::now();
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [&](const auto& entry) { return now - entry.second > kPairingSettleTime; }),
                   g_recent.end());
    return std::any_of(g_recent.begin(), g_recent.end(), [&](const auto& entry) { return entry.first == address; });
}

void MarkPaired(const std::string& address) {
    std::lock_guard<std::mutex> lock(g_recentMutex);
    g_recent.emplace_back(address, std::chrono::steady_clock::now());
}

std::vector<BLUETOOTH_DEVICE_INFO> Devices(const Radio& radio, bool inquiry) {
    std::vector<BLUETOOTH_DEVICE_INFO> devices;
    BLUETOOTH_DEVICE_SEARCH_PARAMS params{sizeof(params)};
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnUnknown = TRUE;
    params.fReturnConnected = TRUE;
    params.fIssueInquiry = inquiry ? TRUE : FALSE;
    params.cTimeoutMultiplier = inquiry ? kInquiryUnits : 0;
    params.hRadio = radio.handle;
    BLUETOOTH_DEVICE_INFO device{sizeof(device)};
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&params, &device);
    if (find == nullptr) return devices;
    do {
        devices.push_back(device);
        device = BLUETOOTH_DEVICE_INFO{sizeof(device)};
    } while (BluetoothFindNextDevice(find, &device));
    BluetoothFindDeviceClose(find);
    return devices;
}

} // namespace Detail

using namespace Detail;

Capabilities Query() {
    // Cached: radios rarely change and this is asked often.
    static std::mutex mutex;
    static Capabilities cached;
    static std::chrono::steady_clock::time_point checked{};
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (checked == std::chrono::steady_clock::time_point{} || now - checked > std::chrono::seconds(10)) {
        checked = now;
        const Radios radios;
        cached.pairing = cached.powerOff = !radios.All().empty();
    }
    return cached;
}

int DiscoverAndPair(const std::atomic<bool>& cancel, std::string& status) {
    const Radios radios;
    if (radios.All().empty()) {
        status = "No Bluetooth adapter found.";
        return 0;
    }
    int connected = 0;
    for (const Radio& radio : radios.All()) {
        // A remote Windows remembers without a bond can never reconnect.
        for (BLUETOOTH_DEVICE_INFO& device : Devices(radio, false)) {
            if (IsRemoteName(device.szName) && device.fRemembered && !device.fConnected && !device.fAuthenticated) {
                BluetoothRemoveDevice(&device.Address);
            }
        }
        if (cancel) return connected;
        const ULONGLONG started = Now();
        std::vector<ULONGLONG> seen;
        for (BLUETOOTH_DEVICE_INFO& found : Devices(radio, true)) {
            if (cancel) return connected;
            if (!IsRemoteName(found.szName) || std::find(seen.begin(), seen.end(), found.Address.ullLong) != seen.end()) {
                continue;
            }
            seen.push_back(found.Address.ullLong);
            const std::string address = Hex(found.Address);
            if (RecentlyPaired(address)) continue;
            // The inquiry results can be seconds old: act on the current state.
            BLUETOOTH_DEVICE_INFO device = found;
            BluetoothGetDeviceInfo(radio.handle, &device);
            if (device.fConnected) continue;
            // Only a remote in pairing mode answers an inquiry; remembered ones
            // that did not answer keep their old timestamp.
            const bool answered = !device.fRemembered || FileTimeOf(found.stLastSeen) >= started;
            if (!answered) continue;
            if (device.fRemembered) {
                // Paired before, but in pairing mode now: it forgot this PC.
                BluetoothRemoveDevice(&device.Address);
                device.fAuthenticated = FALSE;
                device.fRemembered = FALSE;
            }
            DWORD result = Authenticate(radio, device, radio.info.address);
            const bool permanent = result == ERROR_SUCCESS;
            if (!permanent) {
                result = Authenticate(radio, device, device.Address);
            }
            if (result == ERROR_SUCCESS) {
                // Windows only keeps the pairing once the services were read.
                DWORD count = 0;
                BluetoothEnumerateInstalledServices(radio.handle, &device, &count, nullptr);
            }
            const DWORD service =
                BluetoothSetServiceState(radio.handle, &device, const_cast<GUID*>(&kHidServiceClass), BLUETOOTH_SERVICE_ENABLE);
            if (result == ERROR_SUCCESS || service == ERROR_SUCCESS) {
                ++connected;
                MarkPaired(address);
                status = permanent ? "Wii Remote paired. It reconnects with any button from now on."
                                   : "Wii Remote connected for this session (press SYNC to pair it for good).";
                RT_LOG(RT_TAG_HLE) << "Wii Remote " << address << (permanent ? " paired (SYNC)" : " connected (1+2)")
                                   << std::endl;
            } else {
                status = "Could not pair the Wii Remote. Press its red SYNC button and try again.";
                RT_LOG(RT_TAG_HLE) << "Wii Remote " << address << ": pairing failed (" << result << ", " << service
                                   << ")" << std::endl;
            }
        }
    }
    return connected;
}

bool PowerOff(const std::string& address) {
    BLUETOOTH_ADDRESS target{};
    if (!ParseHex(address, target)) return false;
    const Radios radios;
    for (const Radio& radio : radios.All()) {
        ULONGLONG raw = target.ullLong;
        DWORD returned = 0;
        if (DeviceIoControl(radio.handle, kIoctlBthDisconnectDevice, &raw, sizeof(raw), nullptr, 0, &returned,
                            nullptr)) {
            return true;
        }
    }
    return false;
}

std::string AddressFor(const SDL_hid_device_info& info) {
    // Bluetooth HID devices report the remote's address as their serial number.
    std::string hex;
    if (info.serial_number != nullptr) {
        for (const wchar_t* c = info.serial_number; *c != L'\0'; ++c) {
            if (*c < 128 && std::isxdigit(static_cast<int>(*c))) hex += static_cast<char>(std::tolower(*c));
        }
    }
    return hex.size() == 12 && hex != "000000000000" ? hex : std::string();
}

std::string NameFor(const std::string& address, const SDL_hid_device_info& info) {
    BLUETOOTH_ADDRESS target{};
    if (ParseHex(address, target)) {
        const Radios radios;
        for (const Radio& radio : radios.All()) {
            BLUETOOTH_DEVICE_INFO device{sizeof(device)};
            device.Address = target;
            if (BluetoothGetDeviceInfo(radio.handle, &device) == ERROR_SUCCESS && device.szName[0] != L'\0') {
                std::string name;
                for (const WCHAR* c = device.szName; *c != L'\0'; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
                return name;
            }
        }
    }
    std::string name;
    if (info.product_string != nullptr) {
        for (const wchar_t* c = info.product_string; *c != L'\0'; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
    }
    return name;
}

bool MaybeConnected(const std::string& address) {
    BLUETOOTH_ADDRESS target{};
    if (!ParseHex(address, target)) return true;
    const Radios radios;
    for (const Radio& radio : radios.All()) {
        BLUETOOTH_DEVICE_INFO device{sizeof(device)};
        device.Address = target;
        if (BluetoothGetDeviceInfo(radio.handle, &device) == ERROR_SUCCESS) {
            return device.fConnected != FALSE;
        }
    }
    return true;
}

bool Usable(const SDL_hid_device_info&) {
    return true;
}

std::string Notice() {
    return Query().pairing ? std::string() : std::string("No Bluetooth adapter found.");
}

} // namespace Wiimote::Platform

#endif
