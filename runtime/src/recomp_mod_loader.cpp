#include "recomp_mod_loader.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <filesystem>

#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "system_bridge.h"

namespace {

struct ExecutableRange {
    uint32_t start = 0;
    uint32_t end = 0;
    std::string name;
};

std::vector<RecompMod::InitializerFn>& MemoryInitializers() {
    static std::vector<RecompMod::InitializerFn> initializers;
    return initializers;
}

std::vector<RecompMod::InitializerFn>& PostRelInitializers() {
    static std::vector<RecompMod::InitializerFn> initializers;
    return initializers;
}

std::vector<std::filesystem::path>& OverlayRoots() {
    static std::vector<std::filesystem::path> roots;
    return roots;
}

std::string& RiivolutionXmlPath() {
    static std::string path;
    return path;
}

std::vector<RecompMod::RiivolutionOptionSelection>& RiivolutionSelections() {
    static std::vector<RecompMod::RiivolutionOptionSelection> selections;
    return selections;
}

std::vector<RecompMod::MemoryReservation>& Reservations() {
    static std::vector<RecompMod::MemoryReservation> reservations;
    return reservations;
}

std::mutex& ModMutex() {
    static std::mutex mutex;
    return mutex;
}

bool& MemoryInitializersRan() {
    static bool ran = false;
    return ran;
}

bool& PostRelInitializersRan() {
    static bool ran = false;
    return ran;
}

std::vector<ExecutableRange>& ExecutableRanges() {
    static std::vector<ExecutableRange> ranges;
    return ranges;
}

void MarkExecutableGuardPages(uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    const uint32_t firstPage = start >> RecompMod::kExecutableWriteGuardPageShift;
    const uint32_t lastPage = (end - 1) >> RecompMod::kExecutableWriteGuardPageShift;
    for (uint32_t page = firstPage; page <= lastPage; ++page) {
        RecompMod::g_executableWriteGuardPages[page].store(1, std::memory_order_relaxed);
    }
    const uint32_t firstCoarsePage = start >> RecompMod::kExecutableWriteGuardCoarsePageShift;
    const uint32_t lastCoarsePage = (end - 1) >> RecompMod::kExecutableWriteGuardCoarsePageShift;
    for (uint32_t page = firstCoarsePage; page <= lastCoarsePage; ++page) {
        RecompMod::g_executableWriteGuardCoarsePages[page].store(1, std::memory_order_relaxed);
        // Keep ordinary data stores on a single biased-page lookup. A page
        // containing any executable bytes retains the exact guard hierarchy.
        MemoryInline::g_fullWritablePageBias[page] = 0;
    }
    const uint32_t firstMidPage = start >> RecompMod::kExecutableWriteGuardMidPageShift;
    const uint32_t lastMidPage = (end - 1) >> RecompMod::kExecutableWriteGuardMidPageShift;
    for (uint32_t page = firstMidPage; page <= lastMidPage; ++page) {
        RecompMod::g_executableWriteGuardMidPages[page].store(1, std::memory_order_relaxed);
    }
}

void AddExecutableRangeLocked(uint32_t start, uint32_t end, std::string name) {
    if (end <= start) {
        return;
    }

    auto& ranges = ExecutableRanges();
    const auto duplicate = std::find_if(ranges.begin(), ranges.end(), [&](const ExecutableRange& range) {
        return range.start == start && range.end == end && range.name == name;
    });
    if (duplicate != ranges.end()) {
        return;
    }

    ranges.push_back({start, end, std::move(name)});
    MarkExecutableGuardPages(start, end);
    // Flat guest accesses never consult the guard tables, so the host pages
    // fully covered by the range become read-only in the guest view. Native
    // runtime writers use the unprotected host alias and are unaffected.
    GuestFlat::RegisterExecutableRange(start, end);
    RecompMod::g_executableWriteGuardEnabled.store(true, std::memory_order_release);
}

void AddMem1AliasesLocked(uint32_t start, uint32_t end, const std::string& name) {
    if (end <= start) {
        return;
    }

    const uint64_t length = static_cast<uint64_t>(end) - start;
    auto addFromOffset = [&](uint32_t offset) {
        if (offset + length > 24ull * 1024ull * 1024ull) {
            return;
        }
        AddExecutableRangeLocked(0x80000000u + offset, static_cast<uint32_t>(0x80000000ull + offset + length), name + " [cached]");
        AddExecutableRangeLocked(0xC0000000u + offset, static_cast<uint32_t>(0xC0000000ull + offset + length), name + " [uncached]");
    };

    if (start < 0x01800000u && end <= 0x01800000u) {
        addFromOffset(start);
    } else if (start >= 0x80000000u && end <= 0x81800000u) {
        addFromOffset(start - 0x80000000u);
    } else if (start >= 0xC0000000u && end <= 0xC1800000u) {
        addFromOffset(start - 0xC0000000u);
    }
}

void AddMem2AliasesLocked(uint32_t start, uint32_t end, const std::string& name) {
    if (end <= start) {
        return;
    }

    const uint64_t length = static_cast<uint64_t>(end) - start;
    auto addFromOffset = [&](uint32_t offset) {
        if (offset + length > Memory::kMem2Size) {
            return;
        }
        AddExecutableRangeLocked(Memory::kMem2CachedBase + offset,
                                 static_cast<uint32_t>(Memory::kMem2CachedBase + offset + length),
                                 name + " [cached]");
        AddExecutableRangeLocked(Memory::kMem2UncachedBase + offset,
                                 static_cast<uint32_t>(Memory::kMem2UncachedBase + offset + length),
                                 name + " [uncached]");
    };

    if (start >= Memory::kMem2PhysicalBase && end <= Memory::kMem2PhysicalEnd) {
        addFromOffset(start - Memory::kMem2PhysicalBase);
    } else if (start >= Memory::kMem2CachedBase && end <= Memory::kMem2CachedEnd) {
        addFromOffset(start - Memory::kMem2CachedBase);
    } else if (start >= Memory::kMem2UncachedBase && end <= Memory::kMem2UncachedEnd) {
        addFromOffset(start - Memory::kMem2UncachedBase);
    }
}

bool Intersects(uint32_t address, size_t length, const ExecutableRange& range) {
    const uint64_t writeStart = address;
    const uint64_t writeEnd = writeStart + length;
    return writeStart < range.end && writeEnd > range.start;
}

std::optional<ExecutableRange> FindExecutableRange(uint32_t address, size_t length) {
    std::lock_guard<std::mutex> lock(ModMutex());
    for (const auto& range : ExecutableRanges()) {
        if (Intersects(address, length, range)) {
            return range;
        }
    }
    return std::nullopt;
}

bool IsKnownBaseRelLoaderWrite(const ExecutableRange& range) {
    if (range.name.find("StaticR.rel") == std::string::npos) {
        return false;
    }

    const auto* cpu = TryGetCpuContext();
    if (!cpu) {
        return false;
    }

    // The game copies and relocates REL sections before executing them. Those
    // writes are normal loading/linking, not runtime code patches.
    return (cpu->lr >= 0x8000A000u && cpu->lr < 0x8000A400u) ||
           (cpu->lr >= 0x801A6000u && cpu->lr < 0x801A7000u);
}

} // namespace

namespace RecompMod {

std::atomic<bool> g_executableWriteGuardEnabled{false};
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};

void RegisterMemoryInitializer(InitializerFn fn) {
    if (!fn) {
        return;
    }

    std::lock_guard<std::mutex> lock(ModMutex());
    MemoryInitializers().push_back(fn);
}

void RunMemoryInitializers() {
    std::vector<InitializerFn> pending;
    {
        std::lock_guard<std::mutex> lock(ModMutex());
        if (MemoryInitializersRan()) {
            return;
        }
        MemoryInitializersRan() = true;
        pending = MemoryInitializers();
    }

    for (auto* fn : pending) {
        fn();
    }

    if (!pending.empty()) {
        RT_LOG(RT_TAG_MOD) << "Ran " << pending.size() << " recomp mod memory initializer(s)" << std::endl;
    }
}

void RegisterPostRelInitializer(InitializerFn fn) {
    if (!fn) {
        return;
    }

    std::lock_guard<std::mutex> lock(ModMutex());
    PostRelInitializers().push_back(fn);
}

void RunPostRelInitializers() {
    std::vector<InitializerFn> pending;
    {
        std::lock_guard<std::mutex> lock(ModMutex());
        if (PostRelInitializersRan()) {
            return;
        }
        PostRelInitializersRan() = true;
        pending = PostRelInitializers();
    }

    for (auto* fn : pending) {
        fn();
    }

    if (!pending.empty()) {
        RT_LOG(RT_TAG_MOD) << "Ran " << pending.size() << " recomp mod post-REL initializer(s)" << std::endl;
    }
}

void RegisterDvdOverlayRoot(std::string root) {
    if (root.empty()) {
        return;
    }

    // Documented exception to the "relative to Config.toml" rule: overlay roots
    // are registered by mod code shipped beside the executable, so they resolve
    // against the executable directory instead.
    const std::filesystem::path base =
        RuntimeConfigFile::ExecutableDirectory().value_or(std::filesystem::current_path());
    std::filesystem::path resolved = RuntimeConfigFile::ResolveRelativeTo(base, root);

    std::lock_guard<std::mutex> lock(ModMutex());
    auto& roots = OverlayRoots();
    const auto it = std::find(roots.begin(), roots.end(), resolved);
    if (it == roots.end()) {
        roots.push_back(std::move(resolved));
    }
}

const std::vector<std::filesystem::path>& DvdOverlayRoots() {
    return OverlayRoots();
}

void RegisterRiivolutionXml(const char* packRelativePath) {
    if (!packRelativePath || packRelativePath[0] == '\0') {
        return;
    }

    std::lock_guard<std::mutex> lock(ModMutex());
    RiivolutionXmlPath() = packRelativePath;
}

void RegisterRiivolutionOption(const char* sectionName, const char* optionName, unsigned int choice) {
    if (!optionName || optionName[0] == '\0') {
        return;
    }

    std::lock_guard<std::mutex> lock(ModMutex());
    auto& selections = RiivolutionSelections();
    const auto existing = std::find_if(selections.begin(), selections.end(), [&](const RiivolutionOptionSelection& selection) {
        return selection.section == (sectionName ? sectionName : "") && selection.option == optionName;
    });
    if (existing != selections.end()) {
        existing->choice = choice;
        return;
    }
    selections.push_back({sectionName ? sectionName : "", optionName, choice});
}

const std::string& RiivolutionXml() {
    return RiivolutionXmlPath();
}

const std::vector<RiivolutionOptionSelection>& RiivolutionOptionSelections() {
    return RiivolutionSelections();
}

void RegisterMemoryReservation(uint32_t start, uint32_t end, std::string name) {
    if (end <= start) {
        return;
    }

    std::lock_guard<std::mutex> lock(ModMutex());
    auto& reservations = Reservations();
    const auto duplicate = std::find_if(reservations.begin(), reservations.end(), [&](const MemoryReservation& reservation) {
        return reservation.start == start && reservation.end == end && reservation.name == name;
    });
    if (duplicate != reservations.end()) {
        return;
    }

    reservations.push_back({start, end, std::move(name)});
}

const std::vector<MemoryReservation>& MemoryReservations() {
    return Reservations();
}

// ScopedTranslatedExecutionAddress and its thread-local backing variable live
// in the header: the indirect-dispatch path constructs one per call and cannot
// afford a cross-TU call pair (no LTO in this build).

uint32_t CurrentTranslatedExecutionAddress() noexcept {
    return g_currentTranslatedExecutionAddress;
}

void RegisterExecutableRange(uint32_t start, uint32_t end, std::string name) {
    if (end <= start) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ModMutex());
        if (name.empty()) {
            std::ostringstream fallback;
            fallback << "0x" << std::hex << std::uppercase << start << "-0x" << end;
            name = fallback.str();
        }

        AddExecutableRangeLocked(start, end, name);
        AddMem1AliasesLocked(start, end, name);
        AddMem2AliasesLocked(start, end, name);
    }

    // Memory mappings can already exist when a mod publishes its executable
    // sections. Reclassify the startup-only writable fast path now; otherwise
    // a formerly homogeneous data page could retain a stale direct-write bias.
    Memory::RefreshWritableFastPathsForExecutableRanges();
}

[[noreturn]] void ReportForbiddenExecutableWrite(uint32_t address, size_t length, uint64_t value,
                                                 const ExecutableRange& range);

bool HandleExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    if (!ExecutableWriteGuardMayHit(address, length)) {
        return false;
    }

    const auto hit = FindExecutableRange(address, length);
    if (!hit.has_value()) {
        return false;
    }

    if (IsKnownBaseRelLoaderWrite(*hit)) {
        return false;
    }

    ReportForbiddenExecutableWrite(address, length, value, *hit);
}

void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    if (!ExecutableWriteGuardMayHit(address, length)) {
        return;
    }

    const auto hit = FindExecutableRange(address, length);
    if (!hit.has_value()) {
        return;
    }

    ReportForbiddenExecutableWrite(address, length, value, *hit);
}

[[noreturn]] void ReportForbiddenExecutableWrite(uint32_t address, size_t length, uint64_t value,
                                                 const ExecutableRange& range) {
    RT_LOG(RT_TAG_MOD) << "FATAL unsupported executable write" << std::endl;
    std::cerr << "  address: 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address << std::endl;
    std::cerr << "  value:   0x" << std::setw(16) << value << std::endl;
    std::cerr << "  width:   " << std::dec << length << std::endl;
    std::cerr << "  range:   0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << range.start
              << "-0x" << std::setw(8) << range.end << " " << range.name << std::dec << std::endl;

    if (auto* cpu = TryGetCpuContext()) {
        std::cerr << "  caller pc: 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << cpu->pc << std::endl;
        std::cerr << "  caller lr: 0x" << std::setw(8) << cpu->lr << std::dec << std::endl;
        SystemBridge::DumpCpuState(cpu);
    } else {
        std::cerr << "  caller pc: unavailable" << std::endl;
    }

    std::cerr << "  reason: executable runtime writes are forbidden" << std::endl;
    std::ostringstream message;
    message << "The game stopped because translated code attempted to write to executable memory at 0x"
            << std::hex << std::uppercase << address
            << ". Runtime executable patches are not supported in this build.";
    ShowRuntimeFatalPopup("forbidden executable memory write", message.str());
    std::abort();
}

} // namespace RecompMod
