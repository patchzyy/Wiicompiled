#include "hle/bluetooth_hle.h"

#include "bluetooth/bt_controller.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "runtime_log.h"
#include "wiimote_host.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace BluetoothUsb {

constexpr const char* kDevicePath = "/dev/usb/oh1/57e/305";
constexpr int32_t kDeviceFd = 5;
constexpr int32_t kIpcSuccess = 0;
constexpr int32_t kIpcEinval = -4;

constexpr uint32_t kCtrlMsg = 0;
constexpr uint32_t kBulkMsg = 1;
constexpr uint32_t kIntrMsg = 2;

constexpr uint8_t kEndpointEvents = 0x81;
constexpr uint8_t kEndpointAclIn = 0x82;
constexpr uint8_t kEndpointAclOut = 0x02;

// The guest copies each delivered packet into a GKI buffer that its 2 ms
// Bluetooth tick frees; a pool it cannot refill drops the packet, so ACL data is
// paced. Plenty for four remotes reporting at 200 Hz.
constexpr double kAclPacketsPerMs = 3.0;
constexpr double kAclBurst = 6.0;

struct HeldRead {
    bool active = false;
    uint32_t buffer = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t callbackArg = 0;
};

struct Device {
    bool open = false;
    std::unique_ptr<Bluetooth::Controller> controller;
    HeldRead eventRead;
    HeldRead aclRead;
    double aclTokens = kAclBurst;
    uint64_t lastRefillUs = 0;
};

Device& Dev() {
    static Device device;
    return device;
}

uint64_t NowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

Bluetooth::Controller& ControllerFor(Device& device) {
    if (!device.controller) {
        device.controller = std::make_unique<Bluetooth::Controller>(Wiimote::Host());
    }
    return *device.controller;
}

std::vector<uint8_t> ReadGuest(uint32_t address, uint32_t length) {
    std::vector<uint8_t> data(length);
    if (length != 0) {
        std::memcpy(data.data(), Memory::GetPointer(address, length), length);
    }
    return data;
}

void Complete(HeldRead& read, const std::vector<uint8_t>& data) {
    const uint32_t size = static_cast<uint32_t>(std::min<size_t>(data.size(), read.length));
    if (size < data.size()) {
        RT_LOG(RT_TAG_HLE) << "Bluetooth: packet of " << data.size() << " bytes truncated to " << size << std::endl;
    }
    if (size != 0) {
        std::memcpy(Memory::GetPointer(read.buffer, size), data.data(), size);
    }
    read.active = false;
    NandQueueIosCallback(read.callback, static_cast<int32_t>(size), read.callbackArg);
}

// Hands queued events and ACL data to whichever reads the guest has armed.
bool Deliver(Device& device, uint64_t nowUs) {
    Bluetooth::Controller& controller = ControllerFor(device);
    bool delivered = false;
    std::vector<uint8_t> packet;
    if (device.eventRead.active && controller.PopEvent(packet)) {
        Complete(device.eventRead, packet);
        delivered = true;
    }
    if (nowUs > device.lastRefillUs) {
        device.aclTokens = std::min(kAclBurst, device.aclTokens + (nowUs - device.lastRefillUs) / 1000.0 * kAclPacketsPerMs);
        device.lastRefillUs = nowUs;
    }
    if (device.aclRead.active && device.aclTokens >= 1.0 && controller.PopAcl(packet)) {
        device.aclTokens -= 1.0;
        Complete(device.aclRead, packet);
        delivered = true;
    }
    return delivered;
}

bool Hold(HeldRead& read, uint32_t buffer, uint32_t length, uint32_t callback, uint32_t callbackArg) {
    if (length != 0) {
        Memory::GetPointer(buffer, length);
    }
    if (read.active) {
        RT_LOG(RT_TAG_HLE) << "Bluetooth: a second read on the same endpoint replaced the first" << std::endl;
        NandQueueIosCallback(read.callback, kIpcEinval, read.callbackArg);
    }
    read = {true, buffer, length, callback, callbackArg};
    return true;
}

// Executes a transfer. Returns true with `result` when it completed now.
bool Transfer(uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr, uint32_t callback,
              uint32_t callbackArg, bool async, int32_t& result) {
    Device& device = Dev();
    Bluetooth::Controller& controller = ControllerFor(device);
    const uint64_t nowUs = NowUs();
    try {
        if (cmd == kCtrlMsg && numIn == 6 && numOut == 1) {
            const auto requestType = RuntimeHle::ReadIoVector(vectorPtr, 0);
            const auto lengthVector = RuntimeHle::ReadIoVector(vectorPtr, 4);
            const auto data = RuntimeHle::ReadIoVector(vectorPtr, 6);
            const uint8_t bmRequestType = Memory::Read8(requestType.address);
            // wLength is little-endian in the request block.
            const uint16_t wLength = static_cast<uint16_t>(Memory::Read8(lengthVector.address) |
                                                           (Memory::Read8(lengthVector.address + 1) << 8));
            if (bmRequestType != 0x20 || wLength > data.size) {
                RT_LOG(RT_TAG_HLE) << "Bluetooth: unexpected control transfer type 0x" << std::hex
                                   << static_cast<int>(bmRequestType) << std::dec << std::endl;
                result = kIpcEinval;
                return true;
            }
            const std::vector<uint8_t> command = ReadGuest(data.address, wLength);
            controller.ExecuteCommand(command.data(), command.size(), nowUs);
            result = wLength;
            return true;
        }
        if ((cmd == kBulkMsg || cmd == kIntrMsg) && numIn == 2 && numOut == 1) {
            const auto endpointVector = RuntimeHle::ReadIoVector(vectorPtr, 0);
            const auto lengthVector = RuntimeHle::ReadIoVector(vectorPtr, 1);
            const auto data = RuntimeHle::ReadIoVector(vectorPtr, 2);
            const uint8_t endpoint = Memory::Read8(endpointVector.address);
            // Bulk / interrupt lengths are big-endian.
            const uint16_t length = Memory::Read16(lengthVector.address);
            if (length > data.size) {
                result = kIpcEinval;
                return true;
            }
            if (cmd == kBulkMsg && endpoint == kEndpointAclOut) {
                const std::vector<uint8_t> packet = ReadGuest(data.address, length);
                controller.ReceiveAcl(packet.data(), packet.size(), nowUs);
                result = length;
                return true;
            }
            if (async && cmd == kBulkMsg && endpoint == kEndpointAclIn) {
                Hold(device.aclRead, data.address, length, callback, callbackArg);
                Deliver(device, nowUs);
                return false;
            }
            if (async && cmd == kIntrMsg && endpoint == kEndpointEvents) {
                Hold(device.eventRead, data.address, length, callback, callbackArg);
                Deliver(device, nowUs);
                return false;
            }
            RT_LOG(RT_TAG_HLE) << "Bluetooth: unsupported transfer cmd " << cmd << " endpoint 0x" << std::hex
                               << static_cast<int>(endpoint) << std::dec << (async ? "" : " (sync)") << std::endl;
            result = kIpcEinval;
            return true;
        }
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_HLE, "Bluetooth transfer", e);
        result = kIpcEinval;
        return true;
    }
    RT_LOG(RT_TAG_HLE) << "Bluetooth: unsupported ioctlv " << cmd << " (" << numIn << " in, " << numOut << " out)"
                       << std::endl;
    result = kIpcEinval;
    return true;
}

} // namespace BluetoothUsb

int32_t Bluetooth_HLE_OpenDevice(const char* path) {
    if (path == nullptr || std::strcmp(path, BluetoothUsb::kDevicePath) != 0) {
        return 0;
    }
    BluetoothUsb::Device& device = BluetoothUsb::Dev();
    if (device.open) {
        RT_LOG(RT_TAG_HLE) << "Bluetooth: device opened twice" << std::endl;
    }
    device.open = true;
    device.eventRead = {};
    device.aclRead = {};
    device.aclTokens = BluetoothUsb::kAclBurst;
    device.lastRefillUs = BluetoothUsb::NowUs();
    BluetoothUsb::ControllerFor(device).Open(device.lastRefillUs);
    RT_LOG(RT_TAG_HLE) << "Bluetooth: " << BluetoothUsb::kDevicePath << " opened" << std::endl;
    return BluetoothUsb::kDeviceFd;
}

bool Bluetooth_HLE_IsFd(uint32_t fd) {
    return static_cast<int32_t>(fd) == BluetoothUsb::kDeviceFd;
}

int32_t Bluetooth_HLE_Close(uint32_t fd) {
    if (!Bluetooth_HLE_IsFd(fd)) {
        return BluetoothUsb::kIpcEinval;
    }
    BluetoothUsb::Device& device = BluetoothUsb::Dev();
    // IOS cancels the transfers still queued on a device it closes.
    for (BluetoothUsb::HeldRead* read : {&device.eventRead, &device.aclRead}) {
        if (read->active) {
            read->active = false;
            NandQueueIosCallback(read->callback, BluetoothUsb::kIpcEinval, read->callbackArg);
        }
    }
    BluetoothUsb::ControllerFor(device).Close();
    device.open = false;
    RT_LOG(RT_TAG_HLE) << "Bluetooth: " << BluetoothUsb::kDevicePath << " closed" << std::endl;
    return BluetoothUsb::kIpcSuccess;
}

bool Bluetooth_HLE_IoctlvAsync(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr,
                               uint32_t callback, uint32_t callbackArg, int32_t& result) {
    (void)fd;
    if (!BluetoothUsb::Dev().open) {
        result = BluetoothUsb::kIpcEinval;
        return true;
    }
    return BluetoothUsb::Transfer(cmd, numIn, numOut, vectorPtr, callback, callbackArg, true, result);
}

int32_t Bluetooth_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    (void)fd;
    if (!BluetoothUsb::Dev().open) {
        return BluetoothUsb::kIpcEinval;
    }
    int32_t result = BluetoothUsb::kIpcEinval;
    BluetoothUsb::Transfer(cmd, numIn, numOut, vectorPtr, 0, 0, false, result);
    return result;
}

bool Bluetooth_HLE_ProcessCompletions(CpuContext* cpu) {
    (void)cpu;
    BluetoothUsb::Device& device = BluetoothUsb::Dev();
    if (!device.open) {
        return false;
    }
    const uint64_t nowUs = BluetoothUsb::NowUs();
    try {
        BluetoothUsb::ControllerFor(device).Tick(nowUs);
        return BluetoothUsb::Deliver(device, nowUs);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_HLE, "Bluetooth delivery", e);
        return false;
    }
}
