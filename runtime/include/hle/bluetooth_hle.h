#pragma once

// IOS side of the emulated Bluetooth module (/dev/usb/oh1/57e/305): routes the
// guest's USB control / bulk / interrupt transfers to Bluetooth::Controller and
// completes the held event and ACL reads from the alarm pump.

#include <cstdint>

struct CpuContext;

// Returns the fd for the Bluetooth device path, 0 when `path` is not ours.
int32_t Bluetooth_HLE_OpenDevice(const char* path);
bool Bluetooth_HLE_IsFd(uint32_t fd);
// Cancels the held reads (their callbacks are queued) and closes the device.
int32_t Bluetooth_HLE_Close(uint32_t fd);
// True when the request completed now with `result`; false when it is held
// until data arrives (its callback is queued later by the pump).
bool Bluetooth_HLE_IoctlvAsync(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr,
                               uint32_t callback, uint32_t callbackArg, int32_t& result);
int32_t Bluetooth_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr);
// Runs the controller and completes held reads. Emulation thread only.
bool Bluetooth_HLE_ProcessCompletions(CpuContext* cpu);
