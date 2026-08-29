#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct CpuContext;

namespace RecompMod {

using InitializerFn = void (*)();

struct MemoryReservation {
    uint32_t start = 0;
    uint32_t end = 0;
    std::string name;
};

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

// Two initializer phases, both emitted by the translator's mod data-patch
// writer. (A third, plain RegisterInitializer/RunInitializers pair existed with
// no registrant on either side and was removed.)
void RegisterMemoryInitializer(InitializerFn fn);
void RunMemoryInitializers();
void RegisterPostRelInitializer(InitializerFn fn);
void RunPostRelInitializers();

// The generated call site passes a UTF-8 literal; it is decoded once here and
// stays a path from then on.
void RegisterDvdOverlayRoot(std::string root);
const std::vector<std::filesystem::path>& DvdOverlayRoots();

// Riivolution settings pinned by the distribution's recomp.yml. The XML path is
// relative to the pack/overlay root; option selections use Riivolution's 1-based
// choice index (0 disables the option).
struct RiivolutionOptionSelection {
    std::string section;
    std::string option;
    uint32_t choice = 0;
};

void RegisterRiivolutionXml(const char* packRelativePath);
void RegisterRiivolutionOption(const char* sectionName, const char* optionName, unsigned int choice);
const std::string& RiivolutionXml();
const std::vector<RiivolutionOptionSelection>& RiivolutionOptionSelections();

void RegisterMemoryReservation(uint32_t start, uint32_t end, std::string name);
const std::vector<MemoryReservation>& MemoryReservations();

uint32_t CurrentTranslatedExecutionAddress() noexcept;

void RegisterExecutableRange(uint32_t start, uint32_t end, std::string name);
bool HandleExecutableWrite(uint32_t address, size_t length, uint64_t value);
void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value);

inline bool ExecutableWriteGuardMayHit(uint32_t address, size_t length) noexcept {
    if (length == 0 || !g_executableWriteGuardEnabled.load(std::memory_order_relaxed)) {
        return false;
    }

    const uint64_t endExclusive = static_cast<uint64_t>(address) + length;
    const uint32_t firstCoarsePage = address >> kExecutableWriteGuardCoarsePageShift;
    const uint64_t lastCoarsePage64 = (endExclusive - 1) >> kExecutableWriteGuardCoarsePageShift;
    const uint32_t lastCoarsePage = lastCoarsePage64 >= kExecutableWriteGuardCoarsePageCount
        ? kExecutableWriteGuardCoarsePageCount - 1
        : static_cast<uint32_t>(lastCoarsePage64);
    bool coarseHit = false;
    for (uint32_t page = firstCoarsePage; page <= lastCoarsePage; ++page) {
        if (g_executableWriteGuardCoarsePages[page].load(std::memory_order_relaxed) != 0) {
            coarseHit = true;
            break;
        }
    }
    if (!coarseHit) {
        return false;
    }

    const uint32_t firstMidPage = address >> kExecutableWriteGuardMidPageShift;
    const uint64_t lastMidPage64 = (endExclusive - 1) >> kExecutableWriteGuardMidPageShift;
    const uint32_t lastMidPage = lastMidPage64 >= kExecutableWriteGuardMidPageCount
        ? kExecutableWriteGuardMidPageCount - 1
        : static_cast<uint32_t>(lastMidPage64);
    bool midHit = false;
    for (uint32_t page = firstMidPage; page <= lastMidPage; ++page) {
        if (g_executableWriteGuardMidPages[page].load(std::memory_order_relaxed) != 0) {
            midHit = true;
            break;
        }
    }
    if (!midHit) {
        return false;
    }

    const uint32_t firstPage = address >> kExecutableWriteGuardPageShift;
    const uint64_t lastPage64 = (endExclusive - 1) >> kExecutableWriteGuardPageShift;
    const uint32_t lastPage = lastPage64 >= kExecutableWriteGuardPageCount
        ? kExecutableWriteGuardPageCount - 1
        : static_cast<uint32_t>(lastPage64);
    for (uint32_t page = firstPage; page <= lastPage; ++page) {
        if (g_executableWriteGuardPages[page].load(std::memory_order_relaxed) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace RecompMod
