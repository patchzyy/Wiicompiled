#pragma once

#include <atomic>
#include <cstdint>

namespace RecompMod {

// Diagnostic-only, read by fatal reporters (unmapped access, executable-write, CPU dumps).
// Defined here as `inline thread_local` with a constant initializer, not `extern thread_local`
// in the .cpp: every indirect dispatch scopes this, so an out-of-line ctor/dtor would cost two
// un-inlinable calls plus a register spill each for three instructions of work.
inline thread_local uint32_t g_currentTranslatedExecutionAddress = 0;

class ScopedTranslatedExecutionAddress {
public:
    explicit ScopedTranslatedExecutionAddress(uint32_t address) noexcept
        : previous_(g_currentTranslatedExecutionAddress) {
        // Address 0 means "nothing new to report" - the enclosing scope's value
        // stays visible, and the unconditional restore below keeps nesting exact.
        if (address != 0) {
            g_currentTranslatedExecutionAddress = address;
        }
    }

    ~ScopedTranslatedExecutionAddress() noexcept {
        g_currentTranslatedExecutionAddress = previous_;
    }

    ScopedTranslatedExecutionAddress(const ScopedTranslatedExecutionAddress&) = delete;
    ScopedTranslatedExecutionAddress& operator=(const ScopedTranslatedExecutionAddress&) = delete;

private:
    uint32_t previous_ = 0;
};

inline constexpr uint32_t kExecutableWriteGuardPageShift = 12;
inline constexpr uint32_t kExecutableWriteGuardPageCount = 1u << (32 - kExecutableWriteGuardPageShift);
inline constexpr uint32_t kExecutableWriteGuardCoarsePageShift = 20;
inline constexpr uint32_t kExecutableWriteGuardCoarsePageCount = 1u << (32 - kExecutableWriteGuardCoarsePageShift);
inline constexpr uint32_t kExecutableWriteGuardMidPageShift = 16;
inline constexpr uint32_t kExecutableWriteGuardMidPageCount = 1u << (32 - kExecutableWriteGuardMidPageShift);

extern std::atomic<bool> g_executableWriteGuardEnabled;
extern std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount];
extern std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount];
extern std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount];

} // namespace RecompMod
