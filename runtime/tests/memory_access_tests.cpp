#include "memory.h"
#include "guest_flat_memory.h"
#include "hle/hollywood_gpio.h"
#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_fpenv.h"
#include "system_bridge.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

// Link with memory.cpp and the platform's real guest_flat_memory implementation.
namespace RecompMod {
std::atomic<bool> g_executableWriteGuardEnabled{false};
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};
uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
bool HandleExecutableWrite(uint32_t, size_t, uint64_t) { return false; }
}

void SystemBridge::DumpCpuState(const CpuContext*) {}
void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::abort(); }
bool HollywoodGpio_Read32(uint32_t, uint32_t&) { return false; }
bool HollywoodGpio_Write32(uint32_t, uint32_t) { return false; }
extern "C" void GX_HLE_FIFO_WriteFloat(float) { std::abort(); }
extern "C" void GX_HLE_FIFO_Write32(uint32_t) { std::abort(); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t) { std::abort(); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t) { std::abort(); }

namespace {
constexpr size_t kBankSize = 2 * MemoryInline::kPageSize + 0x1000;
constexpr uint32_t kSmallBase = 0xE0000000u;
constexpr uint32_t kTopBase = 0xFFFF0000u;
constexpr std::array<uint32_t, 6> kBanks{
    0u, 0x80000000u, 0xC0000000u, 0x10000000u, 0x90000000u, 0xD0000000u};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Memory::Config TestConfig() {
    Memory::Config config;
    for (uint32_t base : kBanks)
        config.regions.push_back({"bank", base, kBankSize});
    config.regions.push_back({"small", kSmallBase, 0x1000});
    config.regions.push_back({"top", kTopBase, 0x10000});
    return config;
}

void TestInvalidConfig() {
    const auto* saved = Memory::GetPointer(0x80000000u);
    for (const auto& region : std::array<Memory::RegionConfig, 3>{
             Memory::RegionConfig{"huge", 0x80000000u, SIZE_MAX},
             Memory::RegionConfig{"wrap", kTopBase, 0x10001},
             Memory::RegionConfig{"wrap", 1u, SIZE_MAX}}) {
        Memory::Config invalid;
        invalid.regions.push_back(region);
        bool rejected = false;
        try {
            Memory::Init(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "invalid region size was accepted");
        Require(Memory::GetPointer(0x80000000u) == saved,
                "invalid region size discarded the existing memory map");
    }
}

void TestRanges() {
    for (uint32_t base : kBanks) {
        const uint32_t end = base + static_cast<uint32_t>(kBankSize);
        Require(Memory::Contains(base, 0), "empty range at bank start failed");
        Require(Memory::Contains(end, 0), "empty range at bank end failed");
        Require(!Memory::Contains(end + 1, 0), "empty range beyond bank end passed");
        Require(Memory::GetPointer(end, 0) == Memory::GetPointer(base) + kBankSize,
                "bank end pointer changed");
        Require(Memory::Contains(base, kBankSize), "whole bank range failed");
        Require(!Memory::Contains(base, kBankSize + 1), "range beyond bank passed");
        Require(Memory::Contains(end - 8, 8), "exact endpoint failed");
        Require(!Memory::Contains(end - 8, 9), "one-byte overrun passed");
        Require(!Memory::Contains(end, 1), "nonempty bank end passed");
        Require(Memory::Contains(base + MemoryInline::kPageSize - 4, 16),
                "valid page-crossing range failed");

        for (uint32_t address : {base, base + 1, end - 1, end}) {
            for (size_t length : {SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - size_t(address),
                                  size_t(GuestFlat::kGuestSpaceSize)}) {
                uint8_t* pointer = Memory::GetPointer(base);
                Require(!MemoryInline::TryGetPointerFast(address, length, pointer) && !pointer,
                        "fast range lookup accepted an oversized native length");
                Require(!Memory::Contains(address, length),
                        "region lookup accepted an oversized native length");
                Require(!MemoryInline::TryGetWritablePointerFast(address, length, pointer),
                        "writable lookup accepted an oversized native length");
            }
        }
        uint8_t* pointer = nullptr;
        Require(MemoryInline::TryGetPointerFast(end, 0, pointer),
                "partial-page end pointer failed");
        Require(!MemoryInline::TryGetWritablePointerFast(end, 0, pointer),
                "writable lookup accepted an empty range");
    }

    Require(!Memory::Contains(0x70000000u, 0), "unmapped empty range passed");
    Require(Memory::Contains(UINT32_MAX, 1), "last guest byte failed");
    Require(!Memory::Contains(UINT32_MAX, 2), "guest address wrap passed");
    Require(Memory::Contains(kTopBase, 0x10000), "range ending at 4 GiB failed");
    Require(!Memory::Contains(kTopBase, 0x10001), "range exceeding 4 GiB passed");
    bool rejected = false;
    try {
        (void)Memory::GetPointer(0x80000001u, SIZE_MAX);
    } catch (const Memory::AccessViolation& error) {
        rejected = error.address() == 0x80000001u && error.length() == SIZE_MAX;
    }
    Require(rejected, "GetPointer did not reject the overflowing range");

    for (size_t bank = 0; bank < 2; ++bank) {
        const uint32_t physical = bank == 0 ? 0u : 0x10000000u;
        const uint32_t value = 0x12345678u + static_cast<uint32_t>(bank);
        Memory::Write32(physical + 0x100, value);
        Require(Memory::Read32(physical + 0x80000100u) == value &&
                Memory::Read32(physical + 0xC0000100u) == value,
                "cached/uncached aliases lost coherence");
    }

    Require(MemoryInline::ResolveRangeHost(0u, 0, 0, true, false) == nullptr,
            "resolved range accepted zero length");
    Require(MemoryInline::ResolveRangeHost(0u, 0, UINT32_MAX, true, false) == nullptr,
            "resolved range accepted an enormous length");
    Require(MemoryInline::ResolveRangeHost(0u, 0, MemoryInline::kPageSize + 1, true, false) == nullptr,
            "resolved range exceeded its maximum length");
    Require(MemoryInline::ResolveRangeHost(UINT32_MAX, 0, 2, true, false) == nullptr,
            "resolved range wrapped the guest address");
    Require(MemoryInline::ResolveRangeHost(0xCBFFFFFFu, 0, 2, false, true) == nullptr,
            "resolved write crossed into MMIO");
    Require(MemoryInline::ResolveRangeHost(0xCDFFFFFFu, 0, 2, false, true) == nullptr,
            "resolved write crossed out of MMIO");
    if (!GuestFlat::RequiresCheckedAccess()) {
        Require(MemoryInline::ResolveRangeHost(UINT32_MAX, 0, 1, true, false) ==
                    MKW_FLAT_GUEST_BASE + UINT32_MAX,
                "resolved range ending at 4 GiB failed");
        Require(MemoryInline::ResolveRangeHost(UINT32_MAX, 1, 1, true, false) == MKW_FLAT_GUEST_BASE,
                "effective-address addition stopped wrapping at 32 bits");
        Require(MemoryInline::ResolveRangeHost(4u, -4, 1, true, false) == MKW_FLAT_GUEST_BASE,
                "negative effective-address offset failed");
    }
}

void TestWritableRanges() {
    uint8_t* pointer = nullptr;
    Require(MemoryInline::TryGetWritablePointerFast(kSmallBase + 0xFFC, 4, pointer),
            "small mapping exact endpoint failed");
    Require(!MemoryInline::TryGetWritablePointerFast(kSmallBase + 0xFFC, 5, pointer),
            "small mapping overrun passed");
    Require(!MemoryInline::TryGetWritablePointerFast(kSmallBase + 0x1001, 1, pointer),
            "small mapping offset beyond limit passed");
    Require(!MemoryInline::TryGetWritablePointerFast(UINT32_MAX, 2, pointer),
            "writable range wrapped the guest address");
    const uint32_t executable = 0x80004000u;
    RecompMod::g_executableWriteGuardPages[executable >> 12].store(1);
    RecompMod::g_executableWriteGuardCoarsePages[executable >> 20].store(1);
    Memory::RefreshWritableFastPathsForExecutableRanges();
    Require(MemoryInline::g_sparseWritablePageTables[executable >> 20] != nullptr,
            "test did not reach sparse writable lookup");
    Require(MemoryInline::TryGetWritablePointerFast(executable - 8, 8, pointer),
            "sparse writable exact endpoint failed");
    Require(!MemoryInline::TryGetWritablePointerFast(executable - 1, 2, pointer),
            "sparse writable range crossed an executable page");
    Require(!MemoryInline::TryGetWritablePointerFast(executable, 1, pointer),
            "sparse writable range entered an executable page");
    Require(!MemoryInline::TryGetWritablePointerFast(executable - 1, SIZE_MAX, pointer),
            "sparse writable lookup accepted an overflowing length");
    RecompMod::g_executableWriteGuardPages[executable >> 12].store(0);
    RecompMod::g_executableWriteGuardCoarsePages[executable >> 20].store(0);
    Memory::RefreshWritableFastPathsForExecutableRanges();
}

bool CountRead(void* user) {
    ++*static_cast<unsigned*>(user);
    return true;
}

void TestDeferredRanges() {
    unsigned calls = 0;
    Require(Memory::RegisterDeferredRead(0x80000001u, SIZE_MAX, CountRead, &calls) == 0,
            "deferred registration accepted an overflowing native length");
    Require(Memory::RegisterDeferredRead(UINT32_MAX, 2, CountRead, &calls) == 0,
            "deferred registration accepted a wrapped guest range");
    Require(Memory::RegisterDeferredRead(0x80000100u, 0, CountRead, &calls) == 0,
            "deferred registration accepted an empty range");
    Require(Memory::RegisterDeferredRead(0x80000100u, 4, nullptr, &calls) == 0,
            "deferred registration accepted no callback");
    Require(Memory::RegisterDeferredRead(0x80000100u, 4, CountRead, &calls) != 0,
            "valid deferred registration failed");
    Require(!MemoryInline::ResolveDeferredReads(0x80000001u, SIZE_MAX) && calls == 0,
            "overflowing resolution consumed a deferred read");
    Require(MemoryInline::ResolveDeferredReads(0x80000101u, 0) && calls == 0,
            "empty resolution consumed a deferred read");
    Require(MemoryInline::ResolveDeferredReads(0x80000104u, 1) && calls == 0,
            "touching endpoints counted as overlapping");
    Require(MemoryInline::ResolveDeferredReads(0x800000FFu, 2) && calls == 1,
            "overlapping resolution did not consume the deferred read once");
    Require(MemoryInline::ResolveDeferredReads(0x80000100u, 4) && calls == 1,
            "materialized deferred read fired twice");
    Require(Memory::RegisterDeferredRead(UINT32_MAX, 1, CountRead, &calls) != 0,
            "deferred range ending at 4 GiB failed");
    Require(MemoryInline::ResolveDeferredReads(UINT32_MAX, 1) && calls == 2,
            "last-byte deferred range did not resolve");
    Memory::ClearDeferredReads();
}

void TestDeferredEndpoint() {
#if !defined(__APPLE__)
    // The macOS mapping backend has no deferred-read fault handler.
    if (GuestFlat::RequiresCheckedAccess()) return;
    unsigned calls = 0;
    Require(Memory::RegisterDeferredRead(UINT32_MAX, 1, CountRead, &calls) != 0,
            "last-byte deferred registration failed");
    Require(GuestFlat::HandleAccessViolation(MKW_FLAT_GUEST_BASE + UINT32_MAX, false),
            "last-byte deferred fault was not handled");
    Require(calls == 1, "last-byte deferred fault did not materialize its callback");
    Require(MemoryInline::ResolveDeferredReads(UINT32_MAX, 1) && calls == 1,
            "last-byte fault left a pending callback");
    Memory::ClearDeferredReads();
#endif
}

struct SingleCase { uint32_t single; uint64_t widened; };
constexpr SingleCase kSingles[]{
    {0x00000000u, 0x0000000000000000ull}, {0x80000000u, 0x8000000000000000ull},
    {0x00000001u, 0x36A0000000000000ull}, {0x80000001u, 0xB6A0000000000000ull},
    {0x0040000Bu, 0x38000002C0000000ull}, {0x8040000Bu, 0xB8000002C0000000ull},
    {0x007FFFFFu, 0x380FFFFFC0000000ull}, {0x807FFFFFu, 0xB80FFFFFC0000000ull},
    {0x00800000u, 0x3810000000000000ull}, {0x80800000u, 0xB810000000000000ull},
    {0x00800001u, 0x3810000020000000ull}, {0x3F800000u, 0x3FF0000000000000ull},
    {0xBF800000u, 0xBFF0000000000000ull}, {0x7F7FFFFFu, 0x47EFFFFFE0000000ull},
    {0x7F800000u, 0x7FF0000000000000ull}, {0xFF800000u, 0xFFF0000000000000ull},
    {0x7F800001u, 0x7FF0000020000000ull}, {0xFF800001u, 0xFFF0000020000000ull},
    {0x7FC12345u, 0x7FF82468A0000000ull}, {0xFFC12345u, 0xFFF82468A0000000ull},
};

MKW_PPC_NO_INLINE double FlatSingle(uint32_t address) {
    return MemoryInline::FlatReadFloat32(address);
}

MKW_PPC_NO_INLINE double ResolvedSingle(uint8_t* host, uint32_t offset, uint32_t address) {
    return MemoryInline::ReadResolvedFloat32(host, offset, address);
}

struct PendingSingle {
    uint32_t address;
    uint32_t bits;
    unsigned calls = 0;
};

bool MaterializeSingle(void* user) {
    auto& pending = *static_cast<PendingSingle*>(user);
    Memory::Write32(pending.address, pending.bits);
    ++pending.calls;
    return true;
}

void CheckSingle(uint32_t address, const SingleCase& value) {
    Memory::Write32(address, value.single);
    CpuContext cpu{};
    cpu.fpr[2].d = FlatSingle(address);
    Require(PpcBitCastToU64Inline(cpu.fpr[2].d) == value.widened, "flat single load changed the stored bits");
    auto* host = Memory::GetPointer(address - 4, 8);
    cpu.fpr[3].d = ResolvedSingle(host, 4, address);
    Require(PpcBitCastToU64Inline(cpu.fpr[3].d) == value.widened, "resolved single load changed the stored bits");
    cpu.fpr[4].d = ResolvedSingle(nullptr, 0, address);
    Require(PpcBitCastToU64Inline(cpu.fpr[4].d) == value.widened, "fallback single load changed the stored bits");
    Require(PpcBitCastToU64Inline(MemoryInline::ReadResolvedFallbackFloat32(address)) == value.widened,
            "checked single load changed the stored bits");
}

void TestSingleLoads() {
    struct RestoreControl {
        uint32_t saved = MkwGetHostFpControl();
        ~RestoreControl() { MkwRestoreHostMxcsr(saved); }
    } restore;
    std::array<uint32_t, 4> controls{
        restore.saved & ~kMkwFpControlFlushToZeroBits,
        restore.saved | kMkwFpControlFlushToZeroBits,
        restore.saved & ~kMkwFpControlFlushToZeroBits,
        restore.saved | kMkwFpControlFlushToZeroBits};
#if defined(__x86_64__)
    controls[2] |= 1u << 15;
    controls[3] &= ~(1u << 15);
#endif
    for (uint32_t control : controls) {
        MkwRestoreHostMxcsr(control);
        for (const auto& value : kSingles) {
            for (uint32_t bank : kBanks) {
                CheckSingle(bank + 0x100u, value);
                CheckSingle(bank + MemoryInline::kPageSize - 2, value);
                CheckSingle(bank + static_cast<uint32_t>(kBankSize) - 4, value);
            }
            CheckSingle(kSmallBase + 4, value);
            CheckSingle(UINT32_MAX - 3, value);
            PendingSingle pending{0x80000200u, value.single};
            Memory::Write32(pending.address, 0);
            Require(Memory::RegisterDeferredRead(pending.address, 4, MaterializeSingle, &pending) != 0,
                    "deferred single registration failed");
            Require(PpcBitCastToU64Inline(ResolvedSingle(nullptr, 0, pending.address)) == value.widened &&
                        pending.calls == 1,
                    "single fallback did not materialize the deferred value");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        Memory::Init(TestConfig());
        const std::string_view selection = argc > 1 ? argv[1] : "all";
        Require(selection == "all" || selection == "ranges" || selection == "single-loads" ||
                    selection == "deferred-endpoint",
                "expected all, ranges, single-loads or deferred-endpoint");
        if (selection == "all" || selection == "ranges") {
            TestRanges();
            TestWritableRanges();
            TestDeferredRanges();
            TestInvalidConfig();
        }
        if (selection == "all" || selection == "deferred-endpoint") TestDeferredEndpoint();
        if (selection == "all" || selection == "single-loads") TestSingleLoads();
        Memory::Reset();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
