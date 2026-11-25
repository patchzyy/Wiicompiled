#pragma once

// Flat 4 GiB guest address space: a guest access is just `*(T*)(kFlatGuestBase + addr)` plus a
// byte swap, no page-table load/branch. Two views alias the same physical memory: the GUEST
// view at kFlatGuestBase has page protections as the interception mechanism (unmapped/MMIO/
// executable/deferred-read pages are uncommitted or protected), while the HOST view is a plain
// alias native runtime code (image loading, DVD reads, HLE, GX) writes through unchecked.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "guest_abi/flat_memory.h"

namespace GuestFlat {

enum class Backing {
    Owned,
    Mem1,
    Mem2,
};

struct RegionRequest {
    uint32_t base = 0;
    uint64_t size = 0;
    Backing backing = Backing::Owned;
};

struct FaultCounters {
    uint32_t mmio = 0;      // MMIO access that reached the handler
    uint32_t efb = 0;       // deferred (EFB) read materialized from a trap
    uint32_t xguard = 0;    // executable-page write trap
    uint32_t unmapped = 0;  // guest touches that landed outside every mapped region
    uint32_t unmappedRegions = 0;  // distinct 64 KiB blocks committed on demand
};

// True once the reservation exists and translated code may use the flat path.
bool IsActive();

// Reserves the 4 GiB space (once per process) and maps every requested region
// into both views. Throws std::runtime_error with a precise diagnosis when the
// reservation, the section objects or a view cannot be created - a silent
// fallback would let translated code read from an unmapped constant base.
void Initialize(const std::vector<RegionRequest>& regions);

// Host-view pointer for a mapped guest address, or nullptr when the address is
// outside every mapped region. This is what the page table and
// Memory::GetPointer hand out.
uint8_t* HostPointer(uint32_t guestAddress);

// Deferred (EFB) reads: the covered guest pages are made PAGE_NOACCESS in the
// guest view so a flat read traps and materializes the copy.
void ProtectDeferredRange(uint32_t address, size_t length);
void UnprotectDeferredRange(uint32_t address, size_t length);

// Executable-write guard: pages fully covered by a registered executable range
// become PAGE_READONLY in the guest view. Registration order does not matter -
// ranges registered before the mapping exists are re-applied by Initialize.
void RegisterExecutableRange(uint32_t start, uint32_t end);

FaultCounters Counters();

// End-of-run report for the counters above. An unmapped touch is a wild guest
// pointer whose block this module silently committed so execution could go on,
// which makes it invisible unless it is repeated at shutdown; a nonzero count
// is therefore reported as a warning. Idempotent, so every exit path (normal
// return, caught exception, abort handler) may call it.
void LogFaultSummary() noexcept;

// Returns true when the access violation was a guest-space fault this module
// resolved; the caller must then resume execution. `faultAddress` is the raw
// host pointer the access violation trapped on (Windows: ExceptionInformation[1];
// POSIX: siginfo_t::si_addr) and `isWrite` is whether it was a write access
// (Windows: ExceptionInformation[0] != 0; POSIX: derived from the ucontext).
// The platform-specific handler that calls this is expected to have already
// done that extraction - this function only ever works with the parsed pair.
bool HandleAccessViolation(void* faultAddress, bool isWrite) noexcept;

} // namespace GuestFlat
