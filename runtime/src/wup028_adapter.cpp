#include "wup028_adapter.h"

#include "runtime_log.h"
#include "runtime_config.h"

#include <dolphin/pad.h>
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>
#include <usbiodef.h>
#include <winusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Wup028Adapter {
namespace {

constexpr uint16_t kNintendoVendor = 0x057e;
constexpr uint16_t kAdapterProduct = 0x0337;
constexpr size_t kReportSize = 37;
constexpr auto kInputReportTimeout = std::chrono::milliseconds(500);

std::mutex g_mutex;
std::array<PADStatus, PAD_CHANMAX> g_statuses{};
std::array<uint8_t, PAD_CHANMAX> g_rumble{};
std::array<int8_t, PAD_CHANMAX> g_portAssignments{{-1, -1, -1, -1}};
std::thread g_worker;
std::atomic_bool g_stop{false};
std::atomic_bool g_running{false};
std::atomic_bool g_connected{false};
AdapterInfo g_info;

struct Device {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    UCHAR inputPipe = 0;
    UCHAR outputPipe = 0;

    ~Device() { Close(); }
    void Close() {
        if (usb != nullptr) WinUsb_Free(usb);
        if (event != nullptr) CloseHandle(event);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        usb = nullptr;
        event = nullptr;
        file = INVALID_HANDLE_VALUE;
    }
};

bool Transfer(Device& device, bool input, UCHAR pipe, UCHAR* data, ULONG size, ULONG& transferred,
              DWORD timeoutMs, bool* timedOut = nullptr) {
    if (timedOut != nullptr) *timedOut = false;
    ResetEvent(device.event);
    OVERLAPPED operation{};
    operation.hEvent = device.event;
    const BOOL started = input ? WinUsb_ReadPipe(device.usb, pipe, data, size, &transferred, &operation)
                               : WinUsb_WritePipe(device.usb, pipe, data, size, &transferred, &operation);
    if (started) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    const DWORD wait = WaitForSingleObject(device.event, timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        return WinUsb_GetOverlappedResult(device.usb, &operation, &transferred, FALSE);
    }
    CancelIoEx(device.file, &operation);
    WaitForSingleObject(device.event, INFINITE);
    WinUsb_GetOverlappedResult(device.usb, &operation, &transferred, FALSE);
    if (timedOut != nullptr && wait == WAIT_TIMEOUT) *timedOut = true;
    return false;
}

struct DeviceMatch {
    std::wstring path;
    std::string name;
};

std::string WideToUtf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

DeviceMatch FindAdapter() {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return {};

    DeviceMatch result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, index, &iface)) break;

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA deviceInfo{sizeof(deviceInfo)};
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, required, nullptr, &deviceInfo)) continue;

        std::wstring lower(detail->DevicePath);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lower.find(L"vid_057e") != std::wstring::npos && lower.find(L"pid_0337") != std::wstring::npos) {
            result.path = detail->DevicePath;
            std::array<wchar_t, 256> description{};
            if (SetupDiGetDeviceRegistryPropertyW(devices, &deviceInfo, SPDRP_FRIENDLYNAME, nullptr,
                                                   reinterpret_cast<BYTE*>(description.data()),
                                                   static_cast<DWORD>(description.size() * sizeof(wchar_t)), nullptr) ||
                SetupDiGetDeviceRegistryPropertyW(devices, &deviceInfo, SPDRP_DEVICEDESC, nullptr,
                                                   reinterpret_cast<BYTE*>(description.data()),
                                                   static_cast<DWORD>(description.size() * sizeof(wchar_t)), nullptr)) {
                result.name = WideToUtf8(description.data());
            }
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

bool Open(Device& device, std::string& name, std::string& error) {
    const auto match = FindAdapter();
    if (match.path.empty()) {
        error = "No VID 057E / PID 0337 adapter is present";
        return false;
    }
    name = match.name.empty() ? "WUP-028-compatible adapter" : match.name;
    device.file = CreateFileW(match.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (device.file == INVALID_HANDLE_VALUE || !WinUsb_Initialize(device.file, &device.usb)) {
        error = "WinUSB could not open the adapter (Windows error " + std::to_string(GetLastError()) + ")";
        device.Close();
        return false;
    }
    device.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (device.event == nullptr) {
        error = "Could not create the WinUSB transfer event";
        device.Close();
        return false;
    }

    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (!WinUsb_QueryInterfaceSettings(device.usb, 0, &descriptor)) {
        error = "WinUSB could not query the adapter interface";
        device.Close();
        return false;
    }
    for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(device.usb, 0, index, &pipe)) continue;
        if ((pipe.PipeType == UsbdPipeTypeInterrupt || pipe.PipeType == UsbdPipeTypeBulk) &&
            USB_ENDPOINT_DIRECTION_IN(pipe.PipeId) && device.inputPipe == 0) {
            device.inputPipe = pipe.PipeId;
        } else if ((pipe.PipeType == UsbdPipeTypeInterrupt || pipe.PipeType == UsbdPipeTypeBulk) &&
                   USB_ENDPOINT_DIRECTION_OUT(pipe.PipeId) && device.outputPipe == 0) {
            device.outputPipe = pipe.PipeId;
        }
    }
    if (device.inputPipe == 0 || device.outputPipe == 0) {
        error = "The adapter has no usable input/output endpoint";
        device.Close();
        return false;
    }

    UCHAR command = 0x13; // Enable the adapter's 37-byte input stream.
    ULONG written = 0;
    if (!Transfer(device, false, device.outputPipe, &command, 1, written, 1000) || written != 1) {
        error = "The adapter rejected input initialization on endpoint 0x";
        const char hex[] = "0123456789ABCDEF";
        error += hex[device.outputPipe >> 4];
        error += hex[device.outputPipe & 15];
        device.Close();
        return false;
    }
    return true;
}

int8_t Axis(uint8_t raw) {
    constexpr int kCenter = 128;
    constexpr int kCenterTolerance = 10;
    if (raw >= kCenter - kCenterTolerance && raw <= kCenter + kCenterTolerance) return 0;
    return static_cast<int8_t>(std::clamp(static_cast<int>(raw) - kCenter, -128, 127));
}

PADStatus DecodePort(const uint8_t* p) {
    PADStatus out{};
    // The WUP-028 protocol uses type 1 for wired pads and type 2 for
    // WaveBird/wireless receivers. Testing only bit 0x10 drops every type-2
    // controller and makes otherwise valid ports appear empty.
    if ((p[0] & 0x30) == 0) {
        out.err = PAD_ERR_NO_CONTROLLER;
        return out;
    }
    if (p[1] & 0x01) out.button |= PAD_BUTTON_A;
    if (p[1] & 0x02) out.button |= PAD_BUTTON_B;
    if (p[1] & 0x04) out.button |= PAD_BUTTON_X;
    if (p[1] & 0x08) out.button |= PAD_BUTTON_Y;
    if (p[1] & 0x10) out.button |= PAD_BUTTON_LEFT;
    if (p[1] & 0x20) out.button |= PAD_BUTTON_RIGHT;
    if (p[1] & 0x40) out.button |= PAD_BUTTON_DOWN;
    if (p[1] & 0x80) out.button |= PAD_BUTTON_UP;
    if (p[2] & 0x01) out.button |= PAD_BUTTON_START;
    if (p[2] & 0x02) out.button |= PAD_TRIGGER_Z;
    if (p[2] & 0x04) out.button |= PAD_TRIGGER_R;
    if (p[2] & 0x08) out.button |= PAD_TRIGGER_L;
    out.stickX = Axis(p[3]);
    out.stickY = Axis(p[4]);
    out.substickX = Axis(p[5]);
    out.substickY = Axis(p[6]);
    out.triggerL = p[7];
    out.triggerR = p[8];
    out.err = PAD_ERR_NONE;
    return out;
}

bool SendRumble(Device& device, const std::array<uint8_t, PAD_CHANMAX>& motors) {
    std::array<UCHAR, 5> report{{0x11, motors[0], motors[1], motors[2], motors[3]}};
    ULONG written = 0;
    return Transfer(device, false, device.outputPipe, report.data(), report.size(), written, 1000) &&
           written == report.size();
}

bool RefreshInputStream(Device& device) {
    UCHAR command = 0x13;
    ULONG written = 0;
    return Transfer(device, false, device.outputPipe, &command, 1, written, 1000) && written == 1;
}

void ClearConnectedPorts(const char* reason) {
    std::lock_guard lock(g_mutex);
    g_rumble.fill(0);
    for (size_t port = 0; port < g_info.ports.size(); ++port) {
        if (g_info.ports[port]) {
            g_info.ports[port] = false;
            ++g_info.portChangeSequence[port];
            RT_LOG(RT_TAG_RUNTIME) << "GameCube adapter port " << (port + 1)
                                   << " controller disconnected (" << reason << ")" << std::endl;
        }
        g_statuses[port] = {};
        g_statuses[port].err = PAD_ERR_NO_CONTROLLER;
        g_info.portStatus[port] = 0;
    }
}

void Worker() {
    std::string lastError;
    while (!g_stop.load(std::memory_order_acquire)) {
        Device device;
        std::string name;
        std::string error;
        if (!Open(device, name, error)) {
            g_connected.store(false, std::memory_order_release);
            {
                std::lock_guard lock(g_mutex);
                g_info.state = error.starts_with("No VID") ? ConnectionState::Searching : ConnectionState::DriverError;
                g_info.deviceName = name;
                g_info.detail = error;
                g_info.pollRateHz = 0.0f;
                g_info.inputEndpoint = 0;
                g_info.outputEndpoint = 0;
                g_info.ports.fill(false);
                g_info.portStatus.fill(0);
            }
            if (error != lastError && !error.starts_with("No VID")) {
                RT_LOG(RT_TAG_RUNTIME) << "GameCube adapter: " << error << std::endl;
            }
            lastError = error;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        RT_LOG(RT_TAG_RUNTIME) << name << " connected (input endpoint 0x" << std::hex
                               << static_cast<unsigned>(device.inputPipe) << ", output endpoint 0x"
                               << static_cast<unsigned>(device.outputPipe) << std::dec << ")" << std::endl;
        lastError.clear();
        g_connected.store(true, std::memory_order_release);
        {
            std::lock_guard lock(g_mutex);
            g_info.state = ConnectionState::Connected;
            g_info.deviceName = name;
            g_info.detail = "Receiving native GameCube reports";
            g_info.inputEndpoint = device.inputPipe;
            g_info.outputEndpoint = device.outputPipe;
        }
        std::array<uint8_t, PAD_CHANMAX> sentRumble{};
        auto rateStart = std::chrono::steady_clock::now();
        uint32_t rateReports = 0;
        std::array<bool, PAD_CHANMAX> reportedPorts{};
        auto lastReport = std::chrono::steady_clock::now();

        while (!g_stop.load(std::memory_order_acquire)) {
            std::array<UCHAR, kReportSize> report{};
            ULONG read = 0;
            bool timedOut = false;
            if (!Transfer(device, true, device.inputPipe, report.data(), report.size(), read, 100, &timedOut)) {
                if (timedOut && std::chrono::steady_clock::now() - lastReport < kInputReportTimeout) continue;
                break;
            }
            if (read != report.size() || report[0] != 0x21) {
                if (std::chrono::steady_clock::now() - lastReport >= kInputReportTimeout) break;
                continue;
            }
            lastReport = std::chrono::steady_clock::now();
            ++rateReports;
            std::array<PADStatus, PAD_CHANMAX> decoded{};
            for (size_t port = 0; port < decoded.size(); ++port) decoded[port] = DecodePort(report.data() + 1 + port * 9);
            std::array<uint8_t, PAD_CHANMAX> desired{};
            std::array<int8_t, PAD_CHANMAX> transitions{};
            bool refreshStream = false;
            {
                std::lock_guard lock(g_mutex);
                g_statuses = decoded;
                desired = g_rumble;
                for (size_t port = 0; port < decoded.size(); ++port) {
                    g_info.portStatus[port] = report[1 + port * 9];
                    const bool present = decoded[port].err == PAD_ERR_NONE;
                    if (present != reportedPorts[port]) {
                        reportedPorts[port] = present;
                        g_info.ports[port] = present;
                        ++g_info.portChangeSequence[port];
                        transitions[port] = present ? 1 : -1;
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                const float seconds = std::chrono::duration<float>(now - rateStart).count();
                if (seconds >= 1.0f) {
                    g_info.pollRateHz = static_cast<float>(rateReports) / seconds;
                    rateReports = 0;
                    rateStart = now;
                    refreshStream = true;
                }
            }
            for (size_t port = 0; port < transitions.size(); ++port) {
                if (transitions[port] != 0) {
                    RT_LOG(RT_TAG_RUNTIME) << "GameCube adapter port " << (port + 1) << " controller "
                                           << (transitions[port] > 0 ? "connected" : "disconnected") << std::endl;
                }
            }
            if (desired != sentRumble) {
                if (!SendRumble(device, desired)) break;
                sentRumble = desired;
            }
            if (refreshStream && !RefreshInputStream(device)) break;
        }
        // Do not leave a motor latched on when stopping or abandoning this handle.
        SendRumble(device, {});
        g_connected.store(false, std::memory_order_release);
        ClearConnectedPorts("adapter unavailable");
        {
            std::lock_guard lock(g_mutex);
            g_info.state = ConnectionState::Searching;
            g_info.detail = "Adapter disconnected; waiting for reconnect";
        g_info.pollRateHz = 0.0f;
        }
        if (!g_stop.load(std::memory_order_acquire)) {
            RT_LOG(RT_TAG_RUNTIME) << "WUP-028 GameCube adapter disconnected; waiting for reconnect" << std::endl;
        }
    }
}

} // namespace

void Initialize() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    for (auto& status : g_statuses) status.err = PAD_ERR_NO_CONTROLLER;
    for (size_t gamePort = 0; gamePort < g_portAssignments.size(); ++gamePort) {
        g_portAssignments[gamePort] = static_cast<int8_t>(RuntimeConfigFile::GameCubeAdapterPort(gamePort));
    }
    g_stop.store(false, std::memory_order_release);
    g_worker = std::thread(Worker);
}

void Shutdown() {
    if (!g_running.exchange(false)) return;
    g_stop.store(true, std::memory_order_release);
    if (g_worker.joinable()) g_worker.join();
    g_connected.store(false, std::memory_order_release);
}

bool Read(std::array<PADStatus, 4>& statuses) {
    if (!g_connected.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_mutex);
    for (auto& status : statuses) status.err = PAD_ERR_NO_CONTROLLER;
    for (size_t gamePort = 0; gamePort < statuses.size(); ++gamePort) {
        const int physicalPort = g_portAssignments[gamePort];
        if (physicalPort >= 0) statuses[gamePort] = g_statuses[static_cast<size_t>(physicalPort)];
    }
    return true;
}

void SetPortAssignment(uint32_t gamePort, int physicalPort) {
    if (gamePort >= g_portAssignments.size() || physicalPort < -1 || physicalPort >= PAD_CHANMAX) return;
    std::lock_guard lock(g_mutex);
    const int oldPhysicalPort = g_portAssignments[gamePort];
    if (oldPhysicalPort >= 0) g_rumble[static_cast<size_t>(oldPhysicalPort)] = 0;
    if (physicalPort >= 0) {
        for (auto& assignment : g_portAssignments) {
            if (assignment == physicalPort) {
                assignment = -1;
                g_rumble[static_cast<size_t>(physicalPort)] = 0;
            }
        }
    }
    g_portAssignments[gamePort] = static_cast<int8_t>(physicalPort);
}

int GetPortAssignment(uint32_t gamePort) {
    if (gamePort >= g_portAssignments.size()) return -1;
    std::lock_guard lock(g_mutex);
    return g_portAssignments[gamePort];
}

bool SetRumble(uint32_t port, bool enabled) {
    if (port >= g_rumble.size() || !g_connected.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_mutex);
    if (!g_connected.load(std::memory_order_acquire)) return false;
    const int physicalPort = g_portAssignments[port];
    if (physicalPort < 0) return false;
    const size_t adapterPort = static_cast<size_t>(physicalPort);
    if (g_statuses[adapterPort].err != PAD_ERR_NONE) {
        g_rumble[adapterPort] = 0;
        return false;
    }
    g_rumble[adapterPort] = enabled ? 1 : 0;
    return true;
}

AdapterInfo GetInfo() {
    std::lock_guard lock(g_mutex);
    return g_info;
}

} // namespace Wup028Adapter
