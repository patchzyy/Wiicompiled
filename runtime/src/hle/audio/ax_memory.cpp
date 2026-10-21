#include "ax_internal.h"

#include "abi_bridge.h"
#include "ax_mix_kernels.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace AxDspHle {
namespace {

std::atomic<bool> g_loggedMixAddressOutOfRange{false};

} // namespace

uint32_t g_axTaskPtr = kAxDspTaskAddr;

// The state the worker-safe accessors in ax_internal.h resolve against; the
// invariant they preserve is documented there.
std::atomic<uint8_t*> g_mixMem1{nullptr};
std::atomic<uint8_t*> g_mixMem2{nullptr};

thread_local bool t_onMixWorker = false;

// A guest access the worker had to skip is a real, audible degradation, so it
// reaches stderr unconditionally - but only once, because the site it fires from
// runs per voice per 3 ms frame.
void ReportMixAddressOutOfRange(uint32_t addr, size_t bytes) {
    if (g_loggedMixAddressOutOfRange.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    RT_LOGF(RT_TAG_AUDIO,
                 "worker skipped a guest access outside MEM1/MEM2 at 0x%08X (%zu bytes)\n",
                 addr, bytes);
    std::fflush(stderr);
}

void RefreshMixMemoryMap() {
    uint8_t* mem1 = nullptr;
    uint8_t* mem2 = nullptr;
    if (Memory::Contains(Memory::kMem1CachedBase, Memory::kMem1Size)) {
        mem1 = Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size);
    }
    if (Memory::Contains(Memory::kMem2CachedBase, Memory::kMem2Size)) {
        mem2 = Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size);
    }
    g_mixMem1.store(mem1, std::memory_order_relaxed);
    g_mixMem2.store(mem2, std::memory_order_relaxed);
}

uint32_t ReadGuestU32OrZero(uint32_t addr) {
    uint32_t value = 0;
    Memory::TryRead32(addr, value);
    return value;
}

void MarkDspInitialized() {
    Memory::TryWrite32(kDspInitializedAddr, 1);
}

void ResetDspTaskGlobals() {
    Memory::TryWrite32(kDspAssertPendingAddr, 0);
    Memory::TryWrite32(kDspAssertTaskAddr, 0);
    Memory::TryWrite32(MKW_GADDR(80386618), 0);
    Memory::TryWrite32(kDspCurrentTaskAddr, 0);
    Memory::TryWrite32(kDspFirstTaskAddr, 0);
    Memory::TryWrite32(kDspRunningTaskAddr, 0);
}

void LinkSingleDspTask(uint32_t taskPtr) {
    if (taskPtr == 0) {
        return;
    }
    g_axTaskPtr = taskPtr;
    Memory::TryWrite32(kDspCurrentTaskAddr, taskPtr);
    Memory::TryWrite32(kDspFirstTaskAddr, taskPtr);
    Memory::TryWrite32(kDspRunningTaskAddr, taskPtr);
    Memory::TryWrite32(taskPtr + 0x38u, 0);
    Memory::TryWrite32(taskPtr + 0x3cu, 0);
}

uint32_t HashEctorGuest(uint32_t addr, uint32_t length) {
    if (length == 0 || length > 0x10000u || !Memory::Contains(addr, length)) {
        return 0;
    }

    uint32_t crc = 0;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= Memory::Read8(addr + i);
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

void InvokeAxTaskCallback(uint32_t callbackOffset, bool passTask) {
    if (g_axTaskPtr == 0) {
        return;
    }
    const uint32_t callback = Memory::Read32(g_axTaskPtr + callbackOffset);
    if (callback == 0) {
        return;
    }

    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    CpuContextScope scope(cpu);
    if (passTask) {
        cpu->gpr[3] = g_axTaskPtr;
    }
    InvokeIndirectCpu(callback, cpu);
}

std::atomic<uint32_t> g_aramWindowGeneration{1};

namespace {

// The two guest aliases a DSP ARAM address can resolve through. Dolphin's Wii DSP ARAM
// path treats bit 0x10000000 as the MEM2/EXRAM selector; otherwise reads wrap through
// MEM1 using the next-power-of-two retail RAM mask (24 MB real, 32 MB masked).
struct AramAliases {
    uint32_t physical;
    uint32_t cached;
};

AramAliases AramAliasesFor(uint32_t addr) {
    if (addr & 0x10000000u) {
        const uint32_t exramOffset = addr & static_cast<uint32_t>(Memory::kMem2Size - 1u);
        return {0x10000000u | exramOffset, 0x90000000u | exramOffset};
    }
    const uint32_t ramOffset = addr & 0x01ffffffu;
    return {ramOffset, 0x80000000u | ramOffset};
}

} // namespace

bool ResolveAramWindow(uint32_t addr, AramWindow& window) {
    // A window is page-aligned and smaller than both the MEM1 wrap mask and the MEM2 size,
    // so every address inside it maps contiguously and keeps the MEM1/MEM2 selector bit.
    const uint32_t begin = addr & ~(kAramWindowSize - 1u);
    const AramAliases aliases = AramAliasesFor(begin);
    const uint32_t candidates[2] = {aliases.physical, aliases.cached};

    // MixResolveRange, not the page-table walk: this also runs on the mix worker.
    for (const uint32_t candidate : candidates) {
        if (const uint8_t* host = MixResolveRange(candidate, kAramWindowSize)) {
            window.begin = begin;
            window.end = begin + kAramWindowSize;
            window.generation = g_aramWindowGeneration.load(std::memory_order_relaxed);
            window.host = host;
            return true;
        }
    }

    window.begin = 0;
    window.end = 0;
    window.host = nullptr;
    return false;
}

uint8_t ReadAramByteSlow(uint32_t addr) {
    // These single-byte probes are not redundant with ResolveAramWindow's:
    // that one asks for a whole kAramWindowSize page, so it fails for the last
    // <4 KiB of MEM1/MEM2 where this still succeeds.
    const AramAliases aliases = AramAliasesFor(addr);
    if (const uint8_t* host = MixResolveRange(aliases.physical, 1)) {
        return *host;
    }
    if (const uint8_t* host = MixResolveRange(aliases.cached, 1)) {
        return *host;
    }
    // Nothing else backs DSP ARAM: real AX traffic is MEM1/MEM2, and no code
    // path in the runtime ever writes a separate ARAM store, so an address that
    // resolves to neither alias reads as silence.
    return 0;
}

void WriteGuestS32Buffer(uint32_t addr, const int* src, size_t count) {
    if (!addr) {
        return;
    }
    // AX mix buffers are contiguous guest ranges; resolve the range once instead
    // of paying a per-word address translation.
    if (uint8_t* host = MixResolveRange(addr, count * sizeof(uint32_t))) {
        AxMixKernels::StoreBigEndian32(host, src, count);
        return;
    }
    if (t_onMixWorker) {
        ReportMixAddressOutOfRange(addr, count * sizeof(uint32_t));
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        Memory::Write32(addr + static_cast<uint32_t>(i * 4), static_cast<uint32_t>(src[i]));
    }
}

void ReadGuestS32Buffer(uint32_t addr, int* dst, size_t count) {
    if (const uint8_t* host = MixResolveRange(addr, count * sizeof(uint32_t))) {
        AxMixKernels::LoadBigEndian32(dst, host, count);
        return;
    }
    if (t_onMixWorker) {
        ReportMixAddressOutOfRange(addr, count * sizeof(uint32_t));
        std::fill(dst, dst + count, 0);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<int32_t>(Memory::Read32(addr + static_cast<uint32_t>(i * 4)));
    }
}

} // namespace AxDspHle
