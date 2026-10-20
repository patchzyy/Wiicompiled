// SYSCONF items behind the Wii Remote stack. The runtime never parses a
// SYSCONF (SCCheckStatus is HLE'd), so these accessors serve the Bluetooth items
// directly: BT.DINF registers the four virtual remotes of the emulated
// controller, and the console settings come from Config.toml.

#include "bluetooth/bt_controller.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace BluetoothSysconf {

constexpr uint32_t kDeviceInfoSize = 0x461;
constexpr uint32_t kCmpDeviceInfoSize = 0x205;
constexpr uint32_t kDeviceEntrySize = 0x46;
// [0..9] registered remotes, [10..13] the remote last connected on each channel.
constexpr uint32_t kActiveEntriesStart = 10;
constexpr const char kRemoteName[] = "Nintendo RVL-CNT-01";

void Fill(uint32_t buffer, uint32_t size, uint8_t value) {
    std::memset(Memory::GetPointer(buffer, size), value, size);
}

void WriteEntry(uint32_t buffer, uint32_t index, size_t slot) {
    const uint32_t entry = buffer + 1 + index * kDeviceEntrySize;
    // SYSCONF keeps addresses most significant byte first.
    const Bluetooth::BdAddr addr = Bluetooth::Controller::RemoteAddress(slot);
    for (size_t i = 0; i < addr.size(); ++i) {
        Memory::Write8(entry + static_cast<uint32_t>(i), addr[addr.size() - 1 - i]);
    }
    std::memcpy(Memory::GetPointer(entry + 6, sizeof(kRemoteName)), kRemoteName, sizeof(kRemoteName));
}

void SetReturn(CpuContext* ctx, uint32_t value) {
    ctx->gpr[3] = value;
}

} // namespace BluetoothSysconf

extern "C" void SCGetBtDeviceInfoArray_HLE(CpuContext* ctx) {
    const uint32_t buffer = ctx->gpr[3];
    try {
        BluetoothSysconf::Fill(buffer, BluetoothSysconf::kDeviceInfoSize, 0);
        Memory::Write8(buffer, static_cast<uint8_t>(Bluetooth::kRemoteSlots));
        for (size_t slot = 0; slot < Bluetooth::kRemoteSlots; ++slot) {
            BluetoothSysconf::WriteEntry(buffer, static_cast<uint32_t>(slot), slot);
            BluetoothSysconf::WriteEntry(buffer, BluetoothSysconf::kActiveEntriesStart + static_cast<uint32_t>(slot), slot);
        }
        BluetoothSysconf::SetReturn(ctx, 1);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_HLE, "SCGetBtDeviceInfoArray", e);
        BluetoothSysconf::SetReturn(ctx, 0);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801B1ED0, SCGetBtDeviceInfoArray_HLE, (CpuContext* ctx), (ctx));

// WUD writes the list back after reconciling it with the controller's link
// keys, and WPAD whenever a channel's remote changes. The virtual remotes are
// fixed, so the list is rebuilt on every boot.
extern "C" void SCSetBtDeviceInfoArray_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, 1);
}
PPC_NATIVE_OVERRIDE_VOID(801B1EDC, SCSetBtDeviceInfoArray_HLE, (CpuContext* ctx), (ctx));

// Remotes paired with 1+2 on the game's own sync screens. The virtual remotes
// are all registered, so the list is always empty.
extern "C" void SCGetBtCmpDevInfoArray_HLE(CpuContext* ctx) {
    try {
        BluetoothSysconf::Fill(ctx->gpr[3], BluetoothSysconf::kCmpDeviceInfoSize, 0);
        BluetoothSysconf::SetReturn(ctx, 1);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_HLE, "SCGetBtCmpDevInfoArray", e);
        BluetoothSysconf::SetReturn(ctx, 0);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801B1EE8, SCGetBtCmpDevInfoArray_HLE, (CpuContext* ctx), (ctx));

extern "C" void SCSetBtCmpDevInfoArray_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, 1);
}
PPC_NATIVE_OVERRIDE_VOID(801B1EF4, SCSetBtCmpDevInfoArray_HLE, (CpuContext* ctx), (ctx));

extern "C" void SCGetBtDpdSensibility_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, static_cast<uint32_t>(std::clamp(RuntimeConfigFile::WiiIrSensitivity(), 1, 5)));
}
PPC_NATIVE_OVERRIDE_VOID(801B1F00, SCGetBtDpdSensibility_HLE, (CpuContext* ctx), (ctx));

extern "C" void SCGetWpadMotorMode_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, RuntimeConfigFile::WiiRumbleEnabled() ? 1u : 0u);
}
PPC_NATIVE_OVERRIDE_VOID(801B1F68, SCGetWpadMotorMode_HLE, (CpuContext* ctx), (ctx));

// The Home Menu's vibration switch, saved by WPADSaveConfig.
extern "C" void SCSetWpadMotorMode_HLE(CpuContext* ctx) {
    const bool enabled = (ctx->gpr[3] & 0xFF) == 1;
    if (enabled != RuntimeConfigFile::WiiRumbleEnabled()) {
        RuntimeConfigFile::SetWiiRumbleEnabled(enabled);
    }
    BluetoothSysconf::SetReturn(ctx, 1);
}
PPC_NATIVE_OVERRIDE_VOID(801B1FBC, SCSetWpadMotorMode_HLE, (CpuContext* ctx), (ctx));

extern "C" void SCGetWpadSensorBarPosition_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, RuntimeConfigFile::WiiSensorBarAbove() ? 1u : 0u);
}
PPC_NATIVE_OVERRIDE_VOID(801B1FC4, SCGetWpadSensorBarPosition_HLE, (CpuContext* ctx), (ctx));

extern "C" void SCGetWpadSpeakerVolume_HLE(CpuContext* ctx) {
    BluetoothSysconf::SetReturn(ctx, static_cast<uint32_t>(std::clamp(RuntimeConfigFile::WiiSpeakerVolume(), 0, 0x7F)));
}
PPC_NATIVE_OVERRIDE_VOID(801B2018, SCGetWpadSpeakerVolume_HLE, (CpuContext* ctx), (ctx));

// The Home Menu's speaker volume, saved by WPADSaveConfig.
extern "C" void SCSetWpadSpeakerVolume_HLE(CpuContext* ctx) {
    const int32_t volume = std::min<int32_t>(static_cast<int32_t>(ctx->gpr[3] & 0xFF), 0x7F);
    if (volume != RuntimeConfigFile::WiiSpeakerVolume()) {
        RuntimeConfigFile::SetWiiSpeakerVolume(volume);
    }
    BluetoothSysconf::SetReturn(ctx, 1);
}
PPC_NATIVE_OVERRIDE_VOID(801B206C, SCSetWpadSpeakerVolume_HLE, (CpuContext* ctx), (ctx));
