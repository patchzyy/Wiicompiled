#pragma once

// Flat 4 GiB guest address space constants used by translated accesses; see guest_flat_memory.h.
#include <cstddef>
#include <cstdint>

namespace GuestFlat {

// Fixed base so the emitted access is `[reg + imm64-in-register]` with no load
// of a global.
inline constexpr uint64_t kGuestSpaceSize = 0x1'0000'0000ull;
inline constexpr size_t kGuestPageSize = 0x1000;
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
#error "guest_abi/flat_memory.h has no fixed flat guest base chosen for this architecture"
#endif

#define MKW_FLAT_GUEST_BASE (reinterpret_cast<uint8_t*>(GuestFlat::kFixedFlatGuestBase))

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

} // namespace GuestFlat
