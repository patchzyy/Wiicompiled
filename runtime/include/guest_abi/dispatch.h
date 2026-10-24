#pragma once

#include "guest_abi/cpu_context.h"
#include "guest_abi/exec_guard.h"
#include "guest_abi/guest_region.h"
#include "isa/ppc_isa_config.h"
#include "isa/ppc_isa_cr.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace RuntimeCrash {
[[noreturn]] void FatalMissingGuestTarget(uint32_t target, struct CpuContext* cpu) noexcept;
} // namespace RuntimeCrash

inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx);

uint32_t FilterScnRendererPathMaskOption(uint32_t pathMask) noexcept;

// Some game-facing runtime options alter arguments at well-defined ABI
// boundaries. Keep this independent of the dispatch mechanism: generated
// static calls deliberately bypass InvokeDirectCpu for performance.
// (used for the path mask filtering in ScnRenderer::createPath: depth of
// field is always removed, bloom when the user disabled it)
inline void ApplyRuntimeCallOptions(uint32_t target, CpuContext* ctx) {
    if (::GuestRegion::Is<0x8023BD38u>(target)) {
        // ScnRenderer::createPath receives the post-processing path mask in r4.
        ctx->gpr[4] = FilterScnRendererPathMaskOption(ctx->gpr[4]);
    }
}

// Persistent per-thread CPU context used across translated function calls.
CpuContext& GetPersistentCpuContext();

enum class FunctionKind : uint8_t {
    BaseTranslated = 0,
    ModTranslated = 1,
    Native = 2,
};

inline constexpr uint32_t kPpcAllNonvolatileFprMask = 0xffffc000u;
inline constexpr uint32_t kBaseTranslatedFunctionPriority = 0;
inline constexpr uint32_t kModTranslatedFunctionPriorityBase = 100;
inline constexpr uint32_t kNativeFunctionPriority = 10000;

struct TranslatedFunctionInfo {
    uint32_t address = 0;
    const char* name = "";
    uint64_t moduleId = 0;
    uint32_t priority = 0;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    void* entryPoint = nullptr; // Raw typed function pointer for indirect-call resolution
    void (*rawCpuInvoker)(CpuContext*) = nullptr;
    bool mustRemainDynamicallyDispatchable = true;
    FunctionKind kind = FunctionKind::BaseTranslated;
};

struct RawDispatchRecord {
    uint32_t address = 0;
    void (*entry)(CpuContext*) = nullptr;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    bool preserveNonvolatileGprs = false;
};

// Translator-owned indirect dispatch is split by the high address byte and
// then by 4 KiB guest pages. Each populated segment stores only the contiguous
// page range that contains verified function entry points. The final search is
// bounded to at most the 1024 aligned instruction addresses in one guest page.
struct StaticIndirectDispatchPage {
    uint32_t firstEntry = 0;
    uint16_t entryCount = 0;
    // Explicit tail padding to an 8-byte stride. The emitter writes it as a
    // literal third initializer, so it is part of the generated-data contract.
    uint16_t reserved = 0;
};

struct StaticIndirectDispatchSegment {
    const StaticIndirectDispatchPage* pages = nullptr;
    uint16_t firstPage = 0;
    uint16_t pageCount = 0;
};

struct StaticIndirectDispatchTable {
    const char* profileName = nullptr;
    const StaticIndirectDispatchSegment* segments = nullptr; // Exactly 256 high-byte segments.
    const RawDispatchRecord* entries = nullptr;
    size_t entryCount = 0;
};

inline const RawDispatchRecord* FindStaticIndirectDispatchEntry(
    const StaticIndirectDispatchTable* table,
    uint32_t address) noexcept {
    if (!table || !table->segments || !table->entries || table->entryCount == 0) {
        return nullptr;
    }

    const auto& segment = table->segments[address >> 24];
    const uint32_t pageNumber = (address >> 12) & 0x0FFFu;
    if (!segment.pages || pageNumber < segment.firstPage) {
        return nullptr;
    }
    const uint32_t relativePage = pageNumber - segment.firstPage;
    if (relativePage >= segment.pageCount) {
        return nullptr;
    }

    const auto& page = segment.pages[relativePage];
    const size_t first = page.firstEntry;
    const size_t count = page.entryCount;
    if (first > table->entryCount || count > table->entryCount - first) {
        return nullptr;
    }

    size_t lower = first;
    size_t upper = first + count;
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2;
        const uint32_t candidate = table->entries[middle].address;
        if (candidate < address) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    return lower < first + count && table->entries[lower].address == address
        ? &table->entries[lower]
        : nullptr;
}

void RegisterStaticIndirectDispatchTable(const StaticIndirectDispatchTable* table);
inline std::atomic<const StaticIndirectDispatchTable*> g_publishedStaticIndirectDispatchTable{nullptr};

class StaticIndirectDispatchTableRegistrar {
public:
    explicit StaticIndirectDispatchTableRegistrar(const StaticIndirectDispatchTable* table) {
        RegisterStaticIndirectDispatchTable(table);
    }
};

inline unsigned PpcLowestSetBitIndex(uint32_t value) noexcept {
    return static_cast<unsigned>(__builtin_ctz(value));
}

class PpcNonvolatileFprGuard {
public:
    explicit PpcNonvolatileFprGuard(CpuContext* cpu, uint32_t mask = kPpcAllNonvolatileFprMask) noexcept
        : cpu_(cpu), mask_(mask & kPpcAllNonvolatileFprMask) {
        if (!cpu_ || mask_ == 0) {
            return;
        }
        // Real masks are a single contiguous register run - f14..f31 for the
        // conservative default and f23..f31 or similar for a measured writer -
        // so resolve the run once and copy it as one straight-line block instead
        // of testing 18 mask bits. Any other mask keeps the per-bit loop, which
        // saves and restores exactly the same registers.
        const uint32_t lowest = mask_ & (0u - mask_);
        const uint32_t aboveRun = mask_ + lowest;
        if ((aboveRun & (aboveRun - 1u)) == 0u) {
            const unsigned first = PpcLowestSetBitIndex(mask_);
            const unsigned end = aboveRun == 0u ? 32u : PpcLowestSetBitIndex(aboveRun);
            firstSlot_ = static_cast<uint8_t>(first - 14u);
            slotCount_ = static_cast<uint8_t>(end - first);
            const size_t count = slotCount_;
            for (size_t i = 0; i < count; ++i) {
                saved_[firstSlot_ + i] = cpu_->fpr[first + i];
            }
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            if ((mask_ & (1u << (14 + i))) == 0) {
                continue;
            }
            saved_[i] = cpu_->fpr[14 + i];
        }
    }

    ~PpcNonvolatileFprGuard() noexcept {
        if (!cpu_ || mask_ == 0) {
            return;
        }
        if (slotCount_ != 0) {
            const unsigned first = 14u + firstSlot_;
            const size_t count = slotCount_;
            for (size_t i = 0; i < count; ++i) {
                cpu_->fpr[first + i] = saved_[firstSlot_ + i];
            }
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            if ((mask_ & (1u << (14 + i))) == 0) {
                continue;
            }
            cpu_->fpr[14 + i] = saved_[i];
        }
    }

    PpcNonvolatileFprGuard(const PpcNonvolatileFprGuard&) = delete;
    PpcNonvolatileFprGuard& operator=(const PpcNonvolatileFprGuard&) = delete;

private:
    CpuContext* cpu_ = nullptr;
    uint32_t mask_ = 0;
    // Contiguous-run description of mask_. slotCount_ == 0 means the mask is not
    // a single run, so the destructor mirrors the constructor's per-bit loop.
    uint8_t firstSlot_ = 0;
    uint8_t slotCount_ = 0;
    // Every element selected by mask_ is written before it is read. Leaving the
    // remaining slots uninitialized avoids zero-filling all 18 values around a
    // call that may preserve only one or two architectural FPRs.
    std::array<PPC_FPR, 18> saved_;
};

class PpcNonvolatileGprGuard {
public:
    explicit PpcNonvolatileGprGuard(CpuContext* cpu, bool enabled = true) noexcept
        : cpu_(enabled ? cpu : nullptr) {
        if (!cpu_) {
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            saved_[i] = cpu_->gpr[14 + i];
        }
    }

    ~PpcNonvolatileGprGuard() noexcept {
        if (!cpu_) {
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            cpu_->gpr[14 + i] = saved_[i];
        }
    }

    PpcNonvolatileGprGuard(const PpcNonvolatileGprGuard&) = delete;
    PpcNonvolatileGprGuard& operator=(const PpcNonvolatileGprGuard&) = delete;

private:
    CpuContext* cpu_ = nullptr;
    // Left uninitialized: enabled writes all 18 slots before reading them, disabled reads none,
    // so skipping the zero-fill saves 72 bytes of stack work on the common disabled hot path.
    std::array<uint32_t, 18> saved_;
};

// OSLoadContext models rfi-style context restoration and intentionally replaces
// the full guest register file instead of returning like a normal ABI call. It
// can never be guarded.
inline bool ShouldPreserveNonvolatileGprsForRawCpuCall(const TranslatedFunctionInfo* info) noexcept {
    return info->kind == FunctionKind::Native && info->address != MKW_GADDR(801A1F58);
}

inline uint32_t NonvolatileFprGuardMaskFor(const TranslatedFunctionInfo* info) noexcept {
    if (!info || info->nonvolatileFprWriteMask == 0) {
        return 0;
    }
    return info->nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask;
}

template <uint32_t Target>
struct KnownTranslatedCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr bool kMustRemainDynamicallyDispatchable = true;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

// Translator-emitted trait specializations, one macro shared by every generated shard.
// `addr` is the 8-digit hex entry point, `winner` the resolved symbol (mods/Retro Rewind can
// publish their own name). A specialization only exists for a single statically bound winner,
// so overridable/dynamically-dispatchable are hardcoded here rather than passed in.
#define MKW_TRANSLATED_TRAIT(addr, winner, nonvolatile_fpr_write_mask)                    \
    extern "C" void winner(CpuContext* ctx);                                              \
    template <>                                                                           \
    struct KnownTranslatedCpuCall<0x##addr##u> {                                          \
        static constexpr bool kAvailable = true;                                          \
        static constexpr uint32_t kNonvolatileFprWriteMask = nonvolatile_fpr_write_mask;  \
        static constexpr bool kMustRemainDynamicallyDispatchable = false;                 \
        static constexpr void (*Entry)(CpuContext*) = &winner;                            \
    }

template <uint32_t Target>
struct KnownNativeCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

template <uint32_t Target>
struct KnownTypedNativeCpuCall {
    static constexpr bool kAvailable = false;
};

template <typename T, typename = void>
struct IsPureTypedNativeCpuCall : std::false_type {};
template <typename T>
struct IsPureTypedNativeCpuCall<T, std::void_t<decltype(T::kPure)>> : std::bool_constant<T::kPure> {};

#include "guest_abi/native_cpu_calls.inc"

// Direct-mapped thread-local memo in front of the sorted registry lookup every bctrl performs;
// a miss just re-runs the sorted lookup. 512 entries (12 KiB) covers the per-frame indirect
// working set while staying L1/L2 resident, unlike 4096 which would thrash L2. Must stay a
// power of two, the index masks with (size - 1).
inline constexpr size_t kIndirectDispatchCacheEntries = 512;

// Namespace-scope `inline thread_local`, not function-local `static thread_local`, to avoid a
// thread-static init epoch check on every bctrl (same as g_currentCpuContext in guest_abi/cpu_context.h).
struct IndirectResolvedDispatchMemoEntry {
    bool valid;
    uint32_t address;
    const TranslatedFunctionInfo* info;
};
inline thread_local IndirectResolvedDispatchMemoEntry
    g_indirectResolvedDispatchMemo[kIndirectDispatchCacheEntries]{};

struct IndirectRawDispatchMemoEntry {
    bool valid;
    uint32_t address;
    const RawDispatchRecord* record;
};
inline thread_local IndirectRawDispatchMemoEntry
    g_indirectRawDispatchMemo[kIndirectDispatchCacheEntries]{};

class TranslatedFunctionRegistry {
public:
    static void Register(TranslatedFunctionInfo info);
    static void Finalize();
    static inline bool IsLookupPublished() noexcept {
        return lookupPublished_.load(std::memory_order_acquire);
    }
    static inline const TranslatedFunctionInfo* FindByAddressPtr(uint32_t address) {
        if (!lookupPublished_.load(std::memory_order_acquire)) {
            return FindByAddressPtrSlow(address);
        }

        auto& cached = g_indirectResolvedDispatchMemo[
            ((address >> 2) * 2654435761u) & (kIndirectDispatchCacheEntries - 1)];
        if (cached.valid && cached.address == address) {
            return cached.info;
        }

        const auto* info = FindByAddressPtrSlow(address);
        cached = {true, address, info};
        return info;
    }

    // Hot wrapper: only the direct-mapped memo probe stays inline. Inlining the
    // generated-table binary search alongside it pushed the whole function past
    // the inliner's budget and out-of-lined the probe itself, which is the part
    // every bctrl executes.
    static MKW_PPC_FORCE_INLINE const RawDispatchRecord* FindRawByAddressPtr(uint32_t address) {
        if (!lookupPublished_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        auto& cached = g_indirectRawDispatchMemo[
            ((address >> 2) * 2654435761u) & (kIndirectDispatchCacheEntries - 1)];
        if (cached.valid && cached.address == address) {
            return cached.record;
        }

        return FindRawByAddressPtrMiss(address, cached);
    }

private:
    // Memo miss: resolve against the header-inline generated table first; only dynamically
    // registered records or a not-yet-published table fall through to the slow path.
    static MKW_PPC_NO_INLINE MKW_PPC_COLD const RawDispatchRecord* FindRawByAddressPtrMiss(
        uint32_t address, IndirectRawDispatchMemoEntry& cached) {
        const RawDispatchRecord* record = nullptr;
        if (const auto* table =
                g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire)) {
            record = FindStaticIndirectDispatchEntry(table, address);
        }
        if (record == nullptr) {
            record = FindRawByAddressPtrSlow(address);
        }
        cached = {true, address, record};
        return record;
    }

    static const TranslatedFunctionInfo* FindByAddressPtrSlow(uint32_t address);
    static const RawDispatchRecord* FindRawByAddressPtrSlow(uint32_t address);

    // Finalize publishes immutable registry, generated-table, and dynamic raw
    // dispatch storage. Header-inlined cache hits are enabled only after that
    // release publication, so a pre-finalization miss can never poison them.
    inline static std::atomic_bool lookupPublished_{false};
};

// Cold, profile-owned registration data is emitted in compact table shards.
// Keeping it out of generated function bodies avoids one static constructor
// per translated function.
struct BulkTranslatedFunctionRecord {
    uint32_t address;
    const char* name;
    void (*entry)(CpuContext*);
    FunctionKind kind;
    bool preservesNonvolatileFprs;
    uint32_t nonvolatileFprWriteMask;
    uint32_t priority;
    uint64_t moduleId;
    bool mustRemainDynamicallyDispatchable;
};

void RegisterBulkTranslatedFunctions(const BulkTranslatedFunctionRecord* records, size_t count);

class BulkTranslatedFunctionRegistrar {
public:
    BulkTranslatedFunctionRegistrar(const BulkTranslatedFunctionRecord* records, size_t count) {
        RegisterBulkTranslatedFunctions(records, count);
    }
};

MKW_PPC_FORCE_INLINE bool TryDispatchResolvedCpuTarget(const TranslatedFunctionInfo* info, CpuContext* cpu) {
    if (!info || !info->rawCpuInvoker) {
        return false;
    }

    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(info->address);
    PpcNonvolatileFprGuard fprGuard(cpu, NonvolatileFprGuardMaskFor(info));
    PpcNonvolatileGprGuard gprGuard(cpu, ShouldPreserveNonvolatileGprsForRawCpuCall(info));
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        info->rawCpuInvoker(cpu);
        return true;
    }
    info->rawCpuInvoker(cpu);
    return true;
}

inline bool TryDispatchRawCpuTarget(const RawDispatchRecord* record, CpuContext* cpu) {
    if (!record || !record->entry) {
        return false;
    }
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(record->address);
    PpcNonvolatileGprGuard gprGuard(cpu, record->preserveNonvolatileGprs);
    PpcNonvolatileFprGuard fprGuard(cpu, record->nonvolatileFprWriteMask);
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        record->entry(cpu);
    } else {
        record->entry(cpu);
    }
    return true;
}

template <uint32_t Target>
inline void DispatchKnownTranslatedCpuTargetStatic(CpuContext* cpu) {
    const auto invokeKnownTranslated = [&]() {
        KnownTranslatedCpuCall<Target>::Entry(cpu);
    };

    // A statically resolved, non-overridable translated call already running
    // in this CpuContext needs no registry, context, or diagnostic target
    // transition. This is the normal generated-to-generated hot path.
    if (TryGetCpuContext() == cpu) {
        if constexpr (KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
            invokeKnownTranslated();
            return;
        } else {
            PpcNonvolatileFprGuard fprGuard(
                cpu, KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask);
            invokeKnownTranslated();
            return;
        }
    }

    CpuContextScope contextScope(cpu);
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(Target);
    if constexpr (KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
        invokeKnownTranslated();
        return;
    } else {
        PpcNonvolatileFprGuard fprGuard(cpu, KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask);
        invokeKnownTranslated();
    }
}

[[noreturn]] inline void ReportMissingCpuTarget(uint32_t target, CpuContext* cpu) {
    // Centralized crash path: stderr diagnostics, heuristics, crash artifacts
    // in the run's log folder, popup, exit.
    RuntimeCrash::FatalMissingGuestTarget(target, cpu);
}

template <uint32_t Target>
inline const TranslatedFunctionInfo* ResolveDirectCpuTargetInfo() {
    return TranslatedFunctionRegistry::FindByAddressPtr(Target);
}

template <uint32_t Target>
inline const TranslatedFunctionInfo* ResolveDirectCpuTargetInfoCached() {
    // Only latch a result once the registry is published (immutable for the process lifetime);
    // a pre-finalization miss or provisional winner must never be cached. 0 = not yet resolved,
    // 1 = negative-cache sentinel, avoiding a second ready flag on the hot path.
    static std::atomic<uintptr_t> cachedValue{0};
    constexpr uintptr_t kNegativeCache = 1;
    const uintptr_t cached = cachedValue.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached == kNegativeCache
            ? nullptr
            : reinterpret_cast<const TranslatedFunctionInfo*>(cached);
    }
    if (!TranslatedFunctionRegistry::IsLookupPublished()) {
        return ResolveDirectCpuTargetInfo<Target>();
    }
    const auto* info = ResolveDirectCpuTargetInfo<Target>();
    cachedValue.store(info != nullptr
                          ? reinterpret_cast<uintptr_t>(info)
                          : kNegativeCache,
                      std::memory_order_release);
    return info;
}

// Caches the "is the published winner still base translation?" verdict for state-free fast
// paths guarding tens of thousands of call sites. Same latching rule as above: 0 = unresolved,
// 1 = negative, 2 = positive, and a verdict computed pre-publication is never stored.
template <uint32_t Target>
inline bool IsBaseTranslatedCpuTargetActive() {
    static std::atomic<uint8_t> cachedVerdict{0};
    constexpr uint8_t kNegativeVerdict = 1;
    constexpr uint8_t kPositiveVerdict = 2;
    const uint8_t cached = cachedVerdict.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached == kPositiveVerdict;
    }
    const bool publishable = TranslatedFunctionRegistry::IsLookupPublished();
    const auto* info = ResolveDirectCpuTargetInfo<Target>();
    const bool active = info != nullptr && info->kind == FunctionKind::BaseTranslated;
    if (publishable) {
        cachedVerdict.store(active ? kPositiveVerdict : kNegativeVerdict,
                            std::memory_order_release);
    }
    return active;
}

template <uint32_t Target>
inline void InvokeDirectCpu(CpuContext* ctx) {
    static_assert(Target != 0, "InvokeDirectCpu cannot target address 0");
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    ApplyRuntimeCallOptions(Target, cpu);
    if constexpr (KnownNativeCpuCall<Target>::kAvailable) {
        const auto invokeKnownNative = [&]() {
            PpcNonvolatileGprGuard gprGuard(cpu);
            if (TryGetCpuContext() != cpu) {
                CpuContextScope scope(cpu);
                KnownNativeCpuCall<Target>::Entry(cpu);
                return;
            }
            KnownNativeCpuCall<Target>::Entry(cpu);
        };
        if constexpr (KnownNativeCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
            invokeKnownNative();
        } else {
            PpcNonvolatileFprGuard fprGuard(cpu, KnownNativeCpuCall<Target>::kNonvolatileFprWriteMask);
            invokeKnownNative();
        }
        return;
    }

    if constexpr (IsPureTypedNativeCpuCall<KnownTypedNativeCpuCall<Target>>::value) {
        KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        return;
    } else if constexpr (KnownTypedNativeCpuCall<Target>::kAvailable) {
        PpcNonvolatileGprGuard gprGuard(cpu);
        if (TryGetCpuContext() != cpu) {
            CpuContextScope scope(cpu);
            KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        } else {
            KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        }
        return;
    }

    if constexpr (KnownTranslatedCpuCall<Target>::kAvailable) {
        DispatchKnownTranslatedCpuTargetStatic<Target>(cpu);
        return;
    }

    const auto* registeredInfo = ResolveDirectCpuTargetInfoCached<Target>();
    if (TryDispatchResolvedCpuTarget(registeredInfo, cpu)) {
        return;
    }

    ReportMissingCpuTarget(Target, cpu);
}

// Cold tail of InvokeIndirectJump: a bctr target that is not a registered function.
[[noreturn]] MKW_PPC_NO_INLINE MKW_PPC_COLD void ReportMissingIndirectJumpTarget(uint32_t target, CpuContext* cpu);

// The common indirect case: an unguarded record called from its own CpuContext, so the full
// TryDispatchRawCpuTarget would only set the execution address and call.
MKW_PPC_FORCE_INLINE bool TryDispatchUnguardedRawCpuTarget(uint32_t target, CpuContext* cpu) {
    const RawDispatchRecord* record = TranslatedFunctionRegistry::FindRawByAddressPtr(target);
    if (record == nullptr || record->entry == nullptr || record->preserveNonvolatileGprs ||
        record->nonvolatileFprWriteMask != 0 || TryGetCpuContext() != cpu) {
        return false;
    }
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(record->address);
    record->entry(cpu);
    return true;
}

MKW_PPC_NO_INLINE inline void InvokeIndirectJumpSlow(uint32_t target, CpuContext* cpu) {
    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {
        return;
    }

    // Cold fallback for dynamically registered or signature-only targets.
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(target);
    if (info) {
        if (TryDispatchResolvedCpuTarget(info, cpu)) {
            return;
        }
    }
    ReportMissingIndirectJumpTarget(target, cpu);
}

// Indirect jump (bctr without link) - used for switch tables.
inline void InvokeIndirectJump(uint32_t target, CpuContext* ctx) {
    // Use the caller's context, not the persistent global context!
    // This is essential for tail calls via bctr where the 'this' pointer (r3)
    // must be passed correctly to the target function.
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (TryDispatchUnguardedRawCpuTarget(target, cpu)) {
        return;
    }
    InvokeIndirectJumpSlow(target, cpu);
}

MKW_PPC_NO_INLINE inline void InvokeIndirectCpuSlow(uint32_t target, CpuContext* cpu) {
    if (target == 0) {
        ReportMissingCpuTarget(target, cpu);
    }
    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {
        return;
    }
    if (TryDispatchResolvedCpuTarget(TranslatedFunctionRegistry::FindByAddressPtr(target), cpu)) {
        return;
    }
    ReportMissingCpuTarget(target, cpu);
}

inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    ApplyRuntimeCallOptions(target, cpu);
    if (TryDispatchUnguardedRawCpuTarget(target, cpu)) {
        return;
    }
    InvokeIndirectCpuSlow(target, cpu);
}
