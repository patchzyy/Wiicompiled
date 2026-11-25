// IOS Async Stubs
// Synchronous IOS functions are now in hle/storage/ which provides proper
// NAND filesystem redirection to Dolphin's Wii directory.

#include "hle_stubs.h"
#include "memory.h"
#include "hle/bluetooth_hle.h"
#include "hle/net/network.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);
bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess);
extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode);
extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd);
extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence);
extern "C" int32_t NAND_IOS_Ioctl_HLE(uint32_t fd, uint32_t cmd, uint32_t inBufPtr, uint32_t inLen,
                                      uint32_t outBufPtr, uint32_t outLen);
extern "C" int32_t NAND_IOS_Ioctlv_HLE(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                       uint32_t vectorPtr);

namespace {

void SetIosReturn(CpuContext* ctx, int32_t result)
{
    if (ctx) {
        ctx->gpr[3] = static_cast<uint32_t>(result);
    }
}

// Every async entry point completes the same way: hand the result to the guest
// callback, then report "queued successfully" (0) to the caller.
void CompleteAsync(CpuContext* ctx, uint32_t callback, int32_t result, uint32_t callbackArg)
{
    NandQueueIosCallback(callback, result, callbackArg);
    SetIosReturn(ctx, 0);
}

// Shared network-fd body of IOS_IoctlAsync / IOS_IoctlvAsync. Returns false when
// the fd is not ours, leaving the caller to take the NAND path. `start` performs
// the matching Network_HLE_Start*Async call and `blockingFallback` the matching
// synchronous ioctl; neither call signature is changed here.
template <typename Start, typename BlockingFallback>
bool TryDeferredNetworkAsync(CpuContext* ctx, uint32_t fd, uint32_t callback,
                             uint32_t callbackArg, Start&& start,
                             BlockingFallback&& blockingFallback)
{
    if (!Network_HLE_IsFd(fd)) {
        return false;
    }

    const NetworkDeferredContract::StartOutcome deferred = start();
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::Started) {
        SetIosReturn(ctx, 0);
        return true;
    }
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::ImmediateResult) {
        CompleteAsync(ctx, callback, deferred.result, callbackArg);
        return true;
    }

    const int32_t result = blockingFallback();
    CompleteAsync(ctx, callback, result, callbackArg);
    return true;
}

// IOS_ReadAsync and IOS_WriteAsync differ only in the NAND entry point they call.
void IosBufferAsync(CpuContext* ctx, int32_t (*op)(uint32_t fd, uint32_t bufferPtr, uint32_t length))
{
    const uint32_t fd = ctx->gpr[3];
    const uint32_t bufferPtr = ctx->gpr[4];
    const uint32_t length = ctx->gpr[5];
    const uint32_t callback = ctx->gpr[6];
    const uint32_t callbackArg = ctx->gpr[7];
    const int32_t result = op(fd, bufferPtr, length);
    CompleteAsync(ctx, callback, result, callbackArg);
}

} // namespace

// ============================================================================
// Async IOS functions - these still need separate handling
// ============================================================================

// 0x80194158 -> IOS_IoctlAsync
extern "C" void IOS_IoctlAsync_80194158(CpuContext* ctx)
{
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t inBuf = ctx->gpr[5];
    const uint32_t inLen = ctx->gpr[6];
    const uint32_t outBuf = ctx->gpr[7];
    const uint32_t outLen = ctx->gpr[8];
    const uint32_t callback = ctx->gpr[9];
    const uint32_t callbackArg = ctx->gpr[10];
    if (TryDeferredNetworkAsync(
            ctx, fd, callback, callbackArg,
            [&] {
                return Network_HLE_StartIoctlAsync(fd, cmd, inBuf, inLen, outBuf, outLen,
                                                   callback, callbackArg);
            },
            [&] { return Network_HLE_Ioctl(fd, cmd, inBuf, inLen, outBuf, outLen); })) {
        return;
    }

    const int32_t result = NAND_IOS_Ioctl_HLE(fd, cmd, inBuf, inLen, outBuf, outLen);
    CompleteAsync(ctx, callback, result, callbackArg); // Success
}
PPC_NATIVE_OVERRIDE_VOID(80194158, IOS_IoctlAsync_80194158, (CpuContext* ctx), (ctx));

// 0x801937E0 -> IOS_OpenAsync
extern "C" void IOS_OpenAsync_HLE(CpuContext* ctx) {
    const uint32_t pathPtr = ctx->gpr[3];
    const uint32_t mode = ctx->gpr[4];
    const uint32_t callback = ctx->gpr[5];
    const uint32_t callbackArg = ctx->gpr[6];
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : "(null)";
    if (pathPtr) {
        if (const int32_t btFd = Bluetooth_HLE_OpenDevice(path)) {
            CompleteAsync(ctx, callback, btFd, callbackArg);
            return;
        }
        if (const int32_t netFd = Network_HLE_OpenDevice(path, mode)) {
            CompleteAsync(ctx, callback, netFd, callbackArg);
            return;
        }
    }
    const int32_t result = NAND_IOS_Open_HLE(pathPtr, mode);
    CompleteAsync(ctx, callback, result, callbackArg); // Queued successfully
}
PPC_NATIVE_OVERRIDE_VOID(801937E0, IOS_OpenAsync_HLE, (CpuContext* ctx), (ctx));

// 0x80193A18 -> IOS_CloseAsync
extern "C" void IOS_CloseAsync_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t callback = ctx->gpr[4];
    const uint32_t callbackArg = ctx->gpr[5];
    if (Bluetooth_HLE_IsFd(fd)) {
        const int32_t result = Bluetooth_HLE_Close(fd);
        CompleteAsync(ctx, callback, result, callbackArg);
        return;
    }
    if (Network_HLE_IsFd(fd)) {
        const int32_t result = Network_HLE_Close(fd);
        CompleteAsync(ctx, callback, result, callbackArg);
        return;
    }
    const int32_t result = NAND_IOS_Close_HLE(fd);
    CompleteAsync(ctx, callback, result, callbackArg);
}
PPC_NATIVE_OVERRIDE_VOID(80193A18, IOS_CloseAsync_HLE, (CpuContext* ctx), (ctx));

// 0x80193B80 -> IOS_ReadAsync
extern "C" void IOS_ReadAsync_HLE(CpuContext* ctx) {
    IosBufferAsync(ctx, NAND_IOS_Read_HLE);
}
PPC_NATIVE_OVERRIDE_VOID(80193B80, IOS_ReadAsync_HLE, (CpuContext* ctx), (ctx));

// 0x80193D88 -> IOS_WriteAsync
extern "C" void IOS_WriteAsync_HLE(CpuContext* ctx) {
    IosBufferAsync(ctx, NAND_IOS_Write_HLE);
}
PPC_NATIVE_OVERRIDE_VOID(80193D88, IOS_WriteAsync_HLE, (CpuContext* ctx), (ctx));

// 0x80193F90 -> IOS_SeekAsync
extern "C" void IOS_SeekAsync_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const int32_t offset = static_cast<int32_t>(ctx->gpr[4]);
    const int32_t whence = static_cast<int32_t>(ctx->gpr[5]);
    const uint32_t callback = ctx->gpr[6];
    const uint32_t callbackArg = ctx->gpr[7];
    const int32_t result = NAND_IOS_Seek_HLE(fd, offset, whence);
    CompleteAsync(ctx, callback, result, callbackArg);
}
PPC_NATIVE_OVERRIDE_VOID(80193F90, IOS_SeekAsync_HLE, (CpuContext* ctx), (ctx));

// 0x801944FC -> IOS_IoctlvAsync
extern "C" void IOS_IoctlvAsync_HLE(CpuContext* ctx)
{
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t numIn = ctx->gpr[5];
    const uint32_t numOut = ctx->gpr[6];
    const uint32_t vectorPtr = ctx->gpr[7];
    const uint32_t callback = ctx->gpr[8];
    const uint32_t callbackArg = ctx->gpr[9];
    if (Bluetooth_HLE_IsFd(fd)) {
        int32_t result = 0;
        if (Bluetooth_HLE_IoctlvAsync(fd, cmd, numIn, numOut, vectorPtr, callback, callbackArg, result)) {
            CompleteAsync(ctx, callback, result, callbackArg);
        } else {
            SetIosReturn(ctx, 0);
        }
        return;
    }
    if (TryDeferredNetworkAsync(
            ctx, fd, callback, callbackArg,
            [&] {
                return Network_HLE_StartIoctlvAsync(fd, cmd, numIn, numOut, vectorPtr,
                                                    callback, callbackArg);
            },
            [&] { return Network_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr); })) {
        return;
    }

    const int32_t result = NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr);
    CompleteAsync(ctx, callback, result, callbackArg);
}
PPC_NATIVE_OVERRIDE_VOID(801944FC, IOS_IoctlvAsync_HLE, (CpuContext* ctx), (ctx));
