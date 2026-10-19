#include "abi_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "guest_small_data.h"
#include "runtime_log.h"

// Nothing reads CpuContext by offset; these asserts just canary accidental field reordering
// (the order is a cache-locality choice explained at the struct definition).
static_assert(offsetof(CpuContext, gpr) == 0);
static_assert(offsetof(CpuContext, cr) == 128);
static_assert(offsetof(CpuContext, lr) == 132);
static_assert(offsetof(CpuContext, ctr) == 136);
static_assert(offsetof(CpuContext, xer) == 140);
static_assert(offsetof(CpuContext, fpscr) == 144);
static_assert(offsetof(CpuContext, pc) == 148);
static_assert(offsetof(CpuContext, fpr) == 152);
static_assert(offsetof(CpuContext, gqr) == 408);
static_assert(offsetof(CpuContext, hid0) == 440);
static_assert(offsetof(CpuContext, hid1) == 444);
static_assert(offsetof(CpuContext, hid2) == 448);
static_assert(offsetof(CpuContext, srr0) == 452);
static_assert(offsetof(CpuContext, srr1) == 456);
static_assert(offsetof(CpuContext, msr) == 460);
static_assert(sizeof(CpuContext) == 464);

namespace {
std::vector<TranslatedFunctionInfo>& Registry() {
    static std::vector<TranslatedFunctionInfo> entries;
    return entries;
}

size_t& PriorityOverrideCount() {
    static size_t count = 0;
    return count;
}

std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint32_t, size_t>& AddressIndex() {
    static std::unordered_map<uint32_t, size_t> index;
    return index;
}

std::vector<RawDispatchRecord>& DynamicRawDispatchEntries() {
    static std::vector<RawDispatchRecord> entries;
    return entries;
}

const StaticIndirectDispatchTable*& GeneratedIndirectDispatchTable() {
    static const StaticIndirectDispatchTable* table = nullptr;
    return table;
}

std::unordered_map<uint32_t, std::vector<size_t>>& AddressEntries() {
    static std::unordered_map<uint32_t, std::vector<size_t>> entries;
    return entries;
}

struct HostEntryRef {
    uintptr_t addr = 0;
    size_t index = 0;
};

std::vector<HostEntryRef>& HostIndex() {
    static std::vector<HostEntryRef> index;
    return index;
}

std::atomic_bool& RegistryFrozen() {
    static std::atomic_bool frozen{false};
    return frozen;
}

const char* KindLabel(FunctionKind kind) {
    switch (kind) {
    case FunctionKind::BaseTranslated:
        return "base translated";
    case FunctionKind::ModTranslated:
        return "mod translated";
    case FunctionKind::Native:
        return "native";
    }
    return "unknown";
}

uint32_t DefaultPriorityForKind(FunctionKind kind) {
    switch (kind) {
    case FunctionKind::BaseTranslated:
        return kBaseTranslatedFunctionPriority;
    case FunctionKind::ModTranslated:
        return kModTranslatedFunctionPriorityBase;
    case FunctionKind::Native:
        return kNativeFunctionPriority;
    }
    return 0;
}

uint32_t EffectivePriority(const TranslatedFunctionInfo& info) {
    return info.priority != 0 ? info.priority : DefaultPriorityForKind(info.kind);
}

uint32_t KindTieBreaker(FunctionKind kind) {
    switch (kind) {
    case FunctionKind::BaseTranslated:
        return 0;
    case FunctionKind::ModTranslated:
        return 1;
    case FunctionKind::Native:
        return 2;
    }
    return 0;
}

bool IsBetterCandidate(const TranslatedFunctionInfo& candidate, const TranslatedFunctionInfo& current) {
    const uint32_t candidatePriority = EffectivePriority(candidate);
    const uint32_t currentPriority = EffectivePriority(current);
    if (candidatePriority != currentPriority) {
        return candidatePriority > currentPriority;
    }

    const uint32_t candidateKindRank = KindTieBreaker(candidate.kind);
    const uint32_t currentKindRank = KindTieBreaker(current.kind);
    if (candidateKindRank != currentKindRank) {
        return candidateKindRank > currentKindRank;
    }

    return candidate.moduleId > current.moduleId;
}

bool IsSameRegistration(const TranslatedFunctionInfo& a, const TranslatedFunctionInfo& b) {
    return a.address == b.address &&
           a.kind == b.kind &&
           EffectivePriority(a) == EffectivePriority(b) &&
           a.moduleId == b.moduleId;
}

void RebuildIndicesLocked() {
    auto& entries = Registry();
    auto& addrIndex = AddressIndex();
    auto& addressEntries = AddressEntries();
    auto& rawDispatchEntries = DynamicRawDispatchEntries();
    auto& hostIndex = HostIndex();

    addrIndex.clear();
    addressEntries.clear();
    rawDispatchEntries.clear();
    hostIndex.clear();
    addrIndex.reserve(entries.size());
    addressEntries.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        addressEntries[entry.address].push_back(i);
        auto addressIt = addrIndex.find(entry.address);
        if (addressIt == addrIndex.end() || IsBetterCandidate(entry, entries[addressIt->second])) {
            addrIndex[entry.address] = i;
        }
        if (entry.entryPoint) {
            hostIndex.push_back({reinterpret_cast<uintptr_t>(entry.entryPoint), i});
        }
    }

    std::sort(hostIndex.begin(), hostIndex.end(), [](const HostEntryRef& a, const HostEntryRef& b) {
        return a.addr < b.addr;
    });

    const bool translatedWinnersAreGenerated = GeneratedIndirectDispatchTable() != nullptr;
    rawDispatchEntries.reserve(translatedWinnersAreGenerated ? 256u : addrIndex.size());
    for (const auto& [address, index] : addrIndex) {
        const auto& entry = entries[index];
        if (!entry.rawCpuInvoker || !entry.mustRemainDynamicallyDispatchable) {
            continue;
        }
        if (translatedWinnersAreGenerated && entry.kind != FunctionKind::Native) {
            continue;
        }
        rawDispatchEntries.push_back(RawDispatchRecord{
            .address = address,
            .entry = entry.rawCpuInvoker,
            .nonvolatileFprWriteMask = NonvolatileFprGuardMaskFor(&entry),
            .preserveNonvolatileGprs = ShouldPreserveNonvolatileGprsForRawCpuCall(&entry),
        });
    }
    std::sort(rawDispatchEntries.begin(), rawDispatchEntries.end(),
              [](const RawDispatchRecord& a, const RawDispatchRecord& b) {
                  return a.address < b.address;
              });
}

std::string ValidateGeneratedIndirectDispatchLocked() {
    const auto* table = GeneratedIndirectDispatchTable();
    if (!table) {
        return {};
    }
    if (!table->profileName || !table->segments || !table->entries || table->entryCount == 0) {
        return "Invalid generated indirect dispatch table";
    }

    uint32_t previousAddress = 0;
    for (size_t i = 0; i < table->entryCount; ++i) {
        const auto& record = table->entries[i];
        if ((record.address & 3u) != 0 || !record.entry ||
            (i != 0 && record.address <= previousAddress) ||
            FindStaticIndirectDispatchEntry(table, record.address) != &record) {
            std::ostringstream message;
            message << "Malformed generated indirect dispatch entry " << i
                    << " for profile '" << table->profileName << "'";
            return message.str();
        }
        previousAddress = record.address;

        const auto winner = AddressIndex().find(record.address);
        if (winner == AddressIndex().end()) {
            std::ostringstream message;
            message << "Generated indirect dispatch target 0x" << std::hex << record.address
                    << " is not registered for profile '" << table->profileName << "'";
            return message.str();
        }
        const auto& info = Registry()[winner->second];
        if (info.kind == FunctionKind::Native || info.rawCpuInvoker != record.entry ||
            NonvolatileFprGuardMaskFor(&info) != record.nonvolatileFprWriteMask ||
            record.preserveNonvolatileGprs) {
            std::ostringstream message;
            message << "Stale generated indirect dispatch winner at 0x" << std::hex
                    << record.address << " for profile '" << table->profileName << "'";
            return message.str();
        }
    }

    for (const auto& [address, index] : AddressIndex()) {
        const auto& info = Registry()[index];
        if ((info.kind == FunctionKind::BaseTranslated || info.kind == FunctionKind::ModTranslated) &&
            info.rawCpuInvoker && !FindStaticIndirectDispatchEntry(table, address)) {
            std::ostringstream message;
            message << "Generated indirect dispatch profile '" << table->profileName
                    << "' omits translated winner 0x" << std::hex << address;
            return message.str();
        }
    }
    return {};
}
}

uint32_t FilterScnRendererPathMaskOption(uint32_t pathMask) noexcept {
    return RuntimeGameGraphicsOptions::FilterScnRendererPathMask(pathMask);
}

void ReportMissingIndirectJumpTarget(uint32_t target, CpuContext* cpu) {
    // If not registered, this is likely a switch table jump to an intra-function label.
    // The translator should have recognized this pattern and generated proper switch code.
    RT_LOG(RT_TAG_RUNTIME) << "InvokeIndirectJump: target 0x"
              << std::hex << target << std::dec
              << " is not a registered function.\n"
              << "This is likely a switch statement jump table that the translator\n"
              << "failed to recognize. The function containing this bctr needs\n"
              << "proper switch pattern recognition." << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Indirect jump context: pc=0x" << std::hex << cpu->pc
              << " lr=0x" << cpu->lr << " ctr=0x" << cpu->ctr
              << " r1=0x" << cpu->gpr[1] << " r3=0x" << cpu->gpr[3]
              << " r29=0x" << cpu->gpr[29] << " r30=0x" << cpu->gpr[30]
              << " r31=0x" << cpu->gpr[31] << std::dec << std::endl;
    DumpHostStackTraceForRuntimeHelper();
    std::fflush(stderr);
    std::ostringstream message;
    message << "The game stopped because an indirect jump targeted guest address 0x"
            << std::hex << target
            << ", but that address is not a registered translated function.\n\n"
            << "The translator may have missed a switch/jump-table target.";
    // Same artifact set as RuntimeCrash::FatalMissingGuestTarget, which is this
    // path's sibling for indirect *calls*: MarkFatalErrorReported below stops the
    // atexit reporter, so the crash log has to be written here.
    RuntimeCrash::WriteCrashArtifacts("missing_jump_target", message.str(), &target);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("Missing indirect jump target", message.str());
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

void RegisterStaticIndirectDispatchTable(const StaticIndirectDispatchTable* table) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    if (RegistryFrozen().load(std::memory_order_acquire)) {
        RT_LOG(RT_TAG_RUNTIME) << "ERROR: Generated indirect dispatch table registered after finalization" << std::endl;
        ShowRuntimeFatalPopup("translated dispatch initialization failed",
                              "A generated indirect-dispatch table was registered after the function registry was finalized.");
        std::abort();
    }
    auto*& registered = GeneratedIndirectDispatchTable();
    if (registered && registered != table) {
        RT_LOG(RT_TAG_RUNTIME) << "ERROR: Multiple generated indirect dispatch profiles were linked" << std::endl;
        ShowRuntimeFatalPopup("translated dispatch initialization failed",
                              "Multiple generated indirect-dispatch profiles were linked into the same product.");
        std::abort();
    }
    registered = table;
    // Mirror into the header-visible atomic under the same lock, so the
    // inlined miss path and this owning static can never disagree.
    g_publishedStaticIndirectDispatchTable.store(table, std::memory_order_release);
}

void RegisterBulkTranslatedFunctions(const BulkTranslatedFunctionRecord* records, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        TranslatedFunctionInfo info;
        info.address = record.address;
        info.name = record.name ? record.name : "";
        info.moduleId = record.moduleId;
        info.priority = record.priority;
        info.nonvolatileFprWriteMask = record.preservesNonvolatileFprs
            ? 0u
            : (record.nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
        info.entryPoint = reinterpret_cast<void*>(record.entry);
        info.rawCpuInvoker = record.entry;
        info.mustRemainDynamicallyDispatchable = record.mustRemainDynamicallyDispatchable;
        info.kind = record.kind;
        TranslatedFunctionRegistry::Register(std::move(info));
    }
}

void TranslatedFunctionRegistry::Register(TranslatedFunctionInfo info) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    if (RegistryFrozen().load(std::memory_order_acquire)) {
        RT_LOG(RT_TAG_RUNTIME) << "ERROR: Attempted to register function after registry was finalized: 0x"
                  << std::hex << info.address << " '" << info.name << "'" << std::dec << std::endl;
        ShowRuntimeFatalPopup("translated function registration failed",
                              "A translated function was registered after guest execution had already been finalized.");
        std::abort();
    }
    info.priority = EffectivePriority(info);

    auto& entries = Registry();
    auto& addrIndex = AddressIndex();
    auto& addressEntries = AddressEntries();

    auto sameAddressIt = addressEntries.find(info.address);
    if (sameAddressIt != addressEntries.end()) {
        for (const size_t existingIndex : sameAddressIt->second) {
            const auto& existing = entries[existingIndex];
            if (IsSameRegistration(existing, info)) {
                RT_LOG(RT_TAG_RUNTIME) << "Duplicate " << KindLabel(info.kind) << " registration for " << info.name << " (0x"
                          << std::hex << info.address << ") ignored" << std::dec << std::endl;
                return;
            }
        }
    }

    entries.push_back(std::move(info));
    const size_t index = entries.size() - 1;
    addressEntries[entries[index].address].push_back(index);
    auto bestAddressIt = addrIndex.find(entries[index].address);
    if (bestAddressIt == addrIndex.end() || IsBetterCandidate(entries[index], entries[bestAddressIt->second])) {
        if (bestAddressIt != addrIndex.end()) {
            // Expected on every mod entry - a Retro Rewind boot has thousands.
            // Counted here and reported once by Finalize instead of per address.
            ++PriorityOverrideCount();
        }
        addrIndex[entries[index].address] = index;
    }
    if (entries[index].entryPoint) {
        HostIndex().push_back({reinterpret_cast<uintptr_t>(entries[index].entryPoint), index});
    }
}

void TranslatedFunctionRegistry::Finalize() {
    if (const size_t overrides = PriorityOverrideCount(); overrides != 0) {
        RT_LOG(RT_TAG_RUNTIME) << overrides
                  << " registration(s) took over an address by priority" << std::endl;
    }
    std::string validationError;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        // Publication is one-way: cached pointers into the rebuilt vectors
        // remain valid for the lifetime of the process.
        if (RegistryFrozen().load(std::memory_order_acquire)) {
            return;
        }
        RebuildIndicesLocked();
        validationError = ValidateGeneratedIndirectDispatchLocked();
        if (validationError.empty()) {
            // Duplicate-registration tracking is initialization-only. AddressIndex
            // owns every published winner after this point, and Register rejects
            // later writes, so release the per-address candidate vectors before
            // the game starts.
            auto& addressEntries = AddressEntries();
            addressEntries.clear();
            addressEntries.rehash(0);
            RegistryFrozen().store(true, std::memory_order_release);
            lookupPublished_.store(true, std::memory_order_release);
        }
    }
    if (!validationError.empty()) {
        throw std::runtime_error("[" RT_TAG_RUNTIME "] " + validationError);
    }
}

const TranslatedFunctionInfo* TranslatedFunctionRegistry::FindByAddressPtrSlow(uint32_t address) {
    if (RegistryFrozen().load(std::memory_order_acquire)) {
        const auto it = AddressIndex().find(address);
        return it != AddressIndex().end() ? &Registry()[it->second] : nullptr;
    }

    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto it = AddressIndex().find(address);
    if (it != AddressIndex().end()) {
        return &Registry()[it->second];
    }
    for (auto& entry : Registry()) {
        if (entry.address == address) {
            return &entry;
        }
    }
    return nullptr;
}

const RawDispatchRecord* TranslatedFunctionRegistry::FindRawByAddressPtrSlow(uint32_t address) {
    if (!RegistryFrozen().load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (const auto* generated = FindStaticIndirectDispatchEntry(GeneratedIndirectDispatchTable(), address)) {
        return generated;
    }
    const auto& entries = DynamicRawDispatchEntries();
    const auto it = std::lower_bound(entries.begin(), entries.end(), address,
        [](const RawDispatchRecord& entry, uint32_t target) {
            return entry.address < target;
        });
    return it != entries.end() && it->address == address ? &*it : nullptr;
}

std::optional<TranslatedFunctionInfo> FindTranslatedFunctionByHostAddress(uintptr_t hostAddr) {
    const bool frozen = RegistryFrozen().load(std::memory_order_acquire);
    if (!frozen) {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        RebuildIndicesLocked();
    }

    const auto& entries = Registry();
    const auto& sorted = HostIndex();
    if (sorted.empty()) {
        return std::nullopt;
    }

    auto it = std::upper_bound(sorted.begin(), sorted.end(), hostAddr,
                               [](uintptr_t addr, const HostEntryRef& ref) { return addr < ref.addr; });

    if (it == sorted.begin()) {
        return std::nullopt;
    }
    --it;

    uintptr_t funcStart = it->addr;
    uintptr_t funcEnd;
    auto nextIt = it + 1;
    if (nextIt != sorted.end()) {
        funcEnd = nextIt->addr;
    } else {
        funcEnd = funcStart + 256 * 1024;
    }

    if (hostAddr >= funcStart && hostAddr < funcEnd) {
        return entries[it->index];
    }
    return std::nullopt;
}

std::optional<TranslatedFunctionInfo> FindNearestTranslatedFunction(uint32_t address) {
    std::optional<TranslatedFunctionInfo> best;
    uint32_t bestDelta = UINT32_MAX;
    const auto scan = [&] {
        for (const auto& info : Registry()) {
            if (address < info.address) {
                continue;
            }
            const uint32_t delta = address - info.address;
            if (delta < bestDelta) {
                bestDelta = delta;
                best = info;
            }
        }
    };
    if (RegistryFrozen().load(std::memory_order_acquire)) {
        scan();
    } else {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        scan();
    }
    return best;
}

namespace {
CpuContext g_persistentCpu{};
}

CpuContext& GetPersistentCpuContext() {
    return g_persistentCpu;
}

void InitializePersistentCpuContext() {
    // Idempotent by contract: generated mod code calls this again before each of
    // its initializers, after the boot path has already established r1 and live
    // guest state in the persistent context. Never clear the context here; only
    // seed the PowerPC ABI environmental registers if no one has yet:
    // r2 = _SDA2_BASE_ (read-only small data), r13 = _SDA_BASE_.
    if (g_persistentCpu.gpr[2] == 0) {
        g_persistentCpu.gpr[2] = GuestSmallData::kSda2Base;
    }
    if (g_persistentCpu.gpr[13] == 0) {
        g_persistentCpu.gpr[13] = GuestSmallData::kSda1Base;
    }
}

[[noreturn]] void PPC_Undefined(uint32_t pc, uint32_t rawInstruction, const char* details)
{
    std::fprintf(stderr,
                 "[runtime] UNDEFINED guest instruction: pc=0x%08X raw=0x%08X %s\n",
                 pc,
                 rawInstruction,
                 details ? details : "");
    char message[256]{};
    std::snprintf(message, sizeof(message),
                  "The game stopped because it reached an unsupported guest instruction at PC 0x%08X (instruction 0x%08X).\n\n%s",
                  pc, rawInstruction, details ? details : "No additional details were provided.");
    ShowRuntimeFatalPopup("Unsupported guest instruction", message);
    std::abort();
}
