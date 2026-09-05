#pragma once

// Flat 4 GiB guest address space: a guest access is just `*(T*)(kFlatGuestBase + addr)` plus a
// byte swap, no page-table load/branch. Two views alias the same physical memory: the GUEST
// view at kFlatGuestBase has page protections as the interception mechanism (unmapped/MMIO/
// executable/deferred-read pages are uncommitted or protected), while the HOST view is a plain
// alias native runtime code (image loading, DVD reads, HLE, GX) writes through unchecked.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace GuestFlat {

// Fixed base so the emitted access is `[reg + imm64-in-register]` with no load
// of a global.
inline constexpr uint64_t kGuestSpaceSize = 0x1'0000'0000ull;
inline constexpr size_t kGuestPageSize = 0x1000;
#ifdef MKW_PLATFORM_IOS
// No fixed base works on every device: probing an iPhone 17 Pro and an iPad
// Pro M5 gave disjoint sets of free 4 GiB windows (448 GiB only, versus 12-48
// GiB), and the extended-virtual-addressing entitlement changes neither. So the
// base is whatever the kernel hands out at Initialize().
//
// Note that memory_access.h routes every flat access to the checked path on
// Apple, so this has no reader yet. It is groundwork, not a hot path.
extern uint8_t* g_flatGuestBase;
#define MKW_FLAT_GUEST_BASE (GuestFlat::g_flatGuestBase)
#else
#if defined(__x86_64__)
// 16 TiB: clear of the Windows ASan shadow (32 TiB) and of the usual image/heap
// placement.
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'1000'0000'0000ull;
#elif defined(__aarch64__) && defined(__APPLE__)
// Keep this well above the low address ranges that Darwin's ASLR may use for
// a PIE executable and its shared cache. Apple Silicon's user VA is wider
// than Linux's 39-bit minimum, so this 512 GiB region is available while the
// Linux AArch64 target retains its 64 GiB placement below.
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'0080'0000'0000ull;
#elif defined(__aarch64__)
// 16 TiB (this arch's x86_64 sibling value) is unreachable on any AArch64
// kernel configured for 39-bit virtual addresses (512 GiB ceiling) - common on
// older/embedded targets (e.g. this project's own tested Jetson/L4T board, kernel
// 4.9). There mmap() silently ignores a hint above the ceiling and hands back an
// address near the top of the real range instead, which this module's caller
// then rejects as "already occupied". 64 GiB is reachable on every AArch64 VA
// width in real use (39-bit minimum and up) and was confirmed via direct mmap
// probing to sit far below where the PIE image, heap, shared libraries and
// stack actually land (all clustered above ~340 GiB on a 39-bit/512 GiB system).
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'0010'0000'0000ull;
#else
#error "guest_flat_memory.h has no fixed flat guest base chosen for this architecture"
#endif

// Fixed base so the emitted access is `[reg + imm64-in-register]` with no load
// of a global.
#define MKW_FLAT_GUEST_BASE (reinterpret_cast<uint8_t*>(GuestFlat::kFixedFlatGuestBase))
#endif

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

// True when a host VM page covers more than one 4 KiB Wii page. In that
// configuration, guest-view page protection cannot safely represent per-Wii-
// page MMIO, deferred-read, or executable-write state, so general translated
// accesses must use the checked Memory::* path.
// Windows user mode and x86-64 always use a 4 KiB base page, so those builds
// fold this to a compile-time false: it appears in every flat access and must
// not become a hot-path load. Only AArch64, where the page size is a kernel
// configuration (4/16/64 KiB), has to probe it at runtime.
#if defined(_WIN32) || defined(__x86_64__)
#define MKW_GUEST_FLAT_FIXED_PAGE_SIZE 1
#endif

#if defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)
inline constexpr bool RequiresCheckedAccess() noexcept { return false; }
#else
extern bool g_requiresCheckedAccess;
inline bool RequiresCheckedAccess() noexcept { return g_requiresCheckedAccess; }
#endif

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
