// gx_init.cpp - GX Initialization and FIFO Management
#include "gx_internal.h"
#include "runtime_log.h"

#include <cstdio>

// Forward declarations for HLE functions used by GXInit
extern "C" void __GX__FifoInit_8016d180();
extern "C" void __GX__PEInit_8016ee14();
extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode);
extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s);
// GX__SetCPUFifo_8016c94c is already declared in gx_internal.h.
extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa);
extern "C" GXFifoObj* GXInit(void* base, u32 size);

// ============================================================================
// GXInit
// ============================================================================


/**
 * GXInit HLE - Initialize the Graphics subsystem
 * This replaces the translated function that writes to MMIO addresses.
 */
extern "C" uint32_t GX__Init_8016b850(uint32_t fifoBase, uint32_t fifoSize)
{
    constexpr uint32_t kFifoObjAddr = MKW_GADDR(80343740);
    constexpr uint32_t kGXDataAddr = MKW_GADDR(803437C0);
    constexpr uint32_t kGXDataSize = 0x600u;

    GXInit(GuestToHostPtr(fifoBase, fifoSize), fifoSize);

    // Initialize GXData structure in guest memory
    try {
        for (uint32_t offset = 0; offset < kGXDataSize; offset += 4) {
            Memory::Write32(kGXDataAddr + offset, 0);
        }
        Memory::Write8(kGXDataAddr + 0x5f8, 0);
        Memory::Write8(kGXDataAddr + 0x5f9, 1);
        Memory::Write8(kGXDataAddr + 0x5fa, 1);
        Memory::Write32(kGXDataAddr + 0x5e4, 0);
        Memory::Write32(kGXDataAddr + 0x5e8, 0);
        Memory::Write32(kGXDataAddr + 0x5fc, 0);
        Memory::Write32(kGXDataPtrAddr, kGXDataAddr);
        // GXData+0x5F8 was just cleared; drop any stale recording shadow with it.
        BeginDisplayListRecording(0, 0);
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOGF(RT_TAG_GX, "Memory access violation during GXData init: %s\n", e.what());
    }

    __GX__FifoInit_8016d180();
    GX__InitFifoBase_8016c7c8(kFifoObjAddr, fifoBase, fifoSize);
    GX__SetCPUFifo_8016c94c(kFifoObjAddr);
    GX__SetGPFifo_8016cb2c(kFifoObjAddr);
    __GX__PEInit_8016ee14();

    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x254, 0);
            Memory::Write32(gd + 0x174, 0x0f0000ff);
            Memory::Write32(gd + 0x7c, 0x22000000);
            Memory::Write32(gd + 0x170, 0x27000000);
            const uint32_t baseRegs[] = {0x30, 0x38};
            for (int i = 0; i < 2; ++i) {
                uint32_t r = baseRegs[i];
                Memory::Write32(gd + 0x108 + i * 0x10, r << 24);
                Memory::Write32(gd + 0x128 + i * 0x10, (r + 1) << 24);
                Memory::Write32(gd + 0x10c + i * 0x10, (r + 2) << 24);
                Memory::Write32(gd + 0x12c + i * 0x10, (r + 3) << 24);
                Memory::Write32(gd + 0x110 + i * 0x10, (r + 4) << 24);
                Memory::Write32(gd + 0x130 + i * 0x10, (r + 5) << 24);
                Memory::Write32(gd + 0x114 + i * 0x10, (r + 6) << 24);
                Memory::Write32(gd + 0x134 + i * 0x10, (r + 7) << 24);
            }
        }
    } catch (...) {}

    __GX__SetTmemConfig_80171458(2);
    RT_LOGF(RT_TAG_GX, "GX initialized, FIFO at 0x%08X (base=0x%08X size=0x%08X)\n",
            kFifoObjAddr, fifoBase, fifoSize);

    return kFifoObjAddr;
}
PPC_NATIVE_OVERRIDE(8016b850, GX__Init_8016b850, uint32_t, (uint32_t fifoBase, uint32_t fifoSize), (fifoBase, fifoSize));

// ============================================================================
// FIFO Management
// ============================================================================

extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s) { GXInitFifoBase((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)), GuestToHostPtr(ba, s), s); }

extern "C" void GX__SetCPUFifo_8016c94c(uint32_t fa) { GXSetCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016c94c, GX__SetCPUFifo_8016c94c, (uint32_t fa), (fa));

extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa) { GXSetGPFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016cb2c, GX__SetGPFifo_8016cb2c, (uint32_t fa), (fa));

extern "C" void __GX__SaveFifo_8016cdbc(uint32_t fa) { GXSaveCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016cdbc, __GX__SaveFifo_8016cdbc, (uint32_t fa), (fa));

extern "C" void GX__GetCPUFifo_8016cf10(uint32_t fa) { auto* d=(GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)); auto* s=GXGetCPUFifo(); if(d&&s) std::memcpy(d,s,sizeof(GXFifoObj)); }
PPC_NATIVE_OVERRIDE_VOID(8016cf10, GX__GetCPUFifo_8016cf10, (uint32_t fa), (fa));

extern "C" void __GX__FifoInit_8016d180()
{
    constexpr uint32_t kCpInterruptId = 0x11u;
    constexpr uint32_t kCpInterruptMask = 0x4000u;
    constexpr uint32_t kCpInterruptHandlerAddr = MKW_GADDR(8016c668);
    constexpr uint32_t kGxCurrentThreadPtrAddr = MKW_GADDR(803867c4);
    constexpr uint32_t kGxThreadQueueAddr = MKW_GADDR(803867c0);
    constexpr uint32_t kCpuFifoObjAddr = MKW_GADDR(80343de4);
    constexpr uint32_t kGpFifoObjAddr = MKW_GADDR(80343dc0);
    constexpr size_t kFifoObjSize = 0x24u;
    constexpr uint32_t kFifoWrapFlagAddr = MKW_GADDR(803867b0);
    constexpr uint32_t kFifoWrapFlag2Addr = MKW_GADDR(803867b1);

    __OSSetInterruptHandler_801a65f8_hle(kCpInterruptId, kCpInterruptHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kCpInterruptMask);

    try { Memory::Write32(kGxCurrentThreadPtrAddr, OS__GetCurrentThread_801a98b0_hle()); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write32(kGxThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}

    auto zeroBlock = [](uint32_t addr, size_t sizeBytes) {
        for (size_t offset = 0; offset < sizeBytes; offset += 4) {
            Memory::Write32(addr + static_cast<uint32_t>(offset), 0);
        }
    };

    try { zeroBlock(kCpuFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}
    try { zeroBlock(kGpFifoObjAddr, kFifoObjSize); } catch (const ::Memory::AccessViolation&) {}

    try { Memory::Write8(kFifoWrapFlagAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try { Memory::Write8(kFifoWrapFlag2Addr, 0); } catch (const ::Memory::AccessViolation&) {}
}

extern "C" void __GX__PEInit_8016ee14()
{
    constexpr uint32_t kPeTokenInterruptId = 0x12u;
    constexpr uint32_t kPeFinishInterruptId = 0x13u;
    constexpr uint32_t kPeTokenInterruptMask = 0x1000u;
    constexpr uint32_t kPeFinishInterruptMask = 0x2000u;
    constexpr uint32_t kPeTokenHandlerAddr = MKW_GADDR(8016eccc);
    constexpr uint32_t kPeFinishHandlerAddr = MKW_GADDR(8016ed94);
    constexpr uint32_t kPeThreadQueueAddr = MKW_GADDR(803867d0);

    __OSSetInterruptHandler_801a65f8_hle(kPeTokenInterruptId, kPeTokenHandlerAddr);
    __OSSetInterruptHandler_801a65f8_hle(kPeFinishInterruptId, kPeFinishHandlerAddr);
    __OSUnmaskInterrupts_801a69bc_hle(kPeTokenInterruptMask);
    __OSUnmaskInterrupts_801a69bc_hle(kPeFinishInterruptMask);

    try { Memory::Write32(kPeThreadQueueAddr, 0); } catch (const ::Memory::AccessViolation&) {}
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint16_t cur = Memory::Read16(gd + 0x0Au);
            Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(cur | 0x000Fu));
        }
    } catch (const ::Memory::AccessViolation&) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ee14, __GX__PEInit_8016ee14, (), ());

// ============================================================================
// Display List Recording
// ============================================================================

extern "C" void GX__BeginDisplayList_80172e00(uint32_t la, uint32_t s) {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (!gd) return;
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
        if (Memory::Read8(gd + 0x5F9u)) std::memcpy(Memory::GetPointer(MKW_GADDR(80344110), 0x600), Memory::GetPointer(gd, 0x600), 0x600);
        Memory::Write32(MKW_GADDR(80344094), la + s - 4u);
        Memory::Write32(MKW_GADDR(803440AC), 0);
        Memory::Write32(MKW_GADDR(80344090), la);
        Memory::Write32(MKW_GADDR(80344098), s);
        Memory::Write32(MKW_GADDR(803440A4), la);
        Memory::Write32(MKW_GADDR(803440A8), la);
        Memory::Write8(gd + 0x5F8u, 1u);
        // Mirror the guest fifo-object fields the FIFO write path consumes so
        // HleFifoWrite never has to read them back out of guest memory.
        BeginDisplayListRecording(la, s);
        GXFlush();
        GX__GetCPUFifo_8016cf10(MKW_GADDR(80344710));
        GX__SetCPUFifo_8016c94c(MKW_GADDR(80344090));
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80172e00, GX__BeginDisplayList_80172e00, (uint32_t la, uint32_t s), (la, s));

extern "C" uint32_t GX__EndDisplayList_80172eb4() {
    try {
        GXFlush();
        GX__GetCPUFifo_8016cf10(MKW_GADDR(80344090));
        const uint8_t wrapped = Memory::Read8(kDlFifoAddr + kDlWrapFlagOffset);
        GX__SetCPUFifo_8016c94c(MKW_GADDR(80344710));

        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            if (Memory::Read8(gd + 0x5F9u) != 0) {
                const int32_t interruptLevel = OS__DisableInterrupts_801a65ac();
                const uint32_t savedWord8 = Memory::Read32(gd + 0x08u);
                std::memcpy(Memory::GetPointer(gd, 0x600),
                            Memory::GetPointer(MKW_GADDR(80344110), 0x600),
                            0x600);
                Memory::Write32(gd + 0x08u, savedWord8);
                OS__RestoreInterrupts_801a65d4(interruptLevel);
            }
            Memory::Write8(gd + 0x5F8u, 0);
        }

        // Publish the cached cursor/count before the count is read back below.
        // Done unconditionally: the original read of GXData+0x5F8 returned
        // "inactive" whenever the GXData pointer was null, so recording must
        // stop here even on the gd == 0 path.
        EndDisplayListRecording();

        return wrapped == 0 ? Memory::Read32(kDlCountAddr) : 0;
    } catch (...) {
        // Leaving the shadow active would silently swallow every subsequent
        // FIFO write into a dead list.
        EndDisplayListRecording();
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(80172EB4, GX__EndDisplayList_80172eb4, uint32_t, (), ());

// ============================================================================
// Flush
// ============================================================================

extern "C" void GX__Flush_8016e654() { GXFlush(); }
PPC_NATIVE_OVERRIDE_VOID(8016e654, GX__Flush_8016e654, (), ());
