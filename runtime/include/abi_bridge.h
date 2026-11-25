#pragma once
#include "memory.h"
#include "ppc_runtime.h"
#include "system_bridge.h"
#include "game_graphics_options.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <type_traits>

#include "guest_abi/dispatch.h"

void InitializePersistentCpuContext();

std::optional<TranslatedFunctionInfo> FindNearestTranslatedFunction(uint32_t address);
// Look up a registered function by host (native) address.
// This enables stack trace symbolication for translated functions.
std::optional<TranslatedFunctionInfo> FindTranslatedFunctionByHostAddress(uintptr_t hostAddr);

template <typename Signature>
class AbiTrampoline;

template <typename Ret, typename... Args>
class AbiTrampoline<Ret(Args...)> {
public:
    using TargetFn = Ret (*)(Args...);

    AbiTrampoline(uint32_t address,
                  const char* name,
                  TargetFn target,
                  FunctionKind kind = FunctionKind::BaseTranslated,
                  bool preservesNonvolatileFprs = false,
                  uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask,
                  uint32_t priority = 0,
                  uint64_t moduleId = 0,
                  void (*rawCpuInvoker)(CpuContext*) = nullptr,
                  bool mustRemainDynamicallyDispatchable = true)
    {
        TranslatedFunctionInfo info;
        info.address = address;
        info.name = name ? name : "";
        info.moduleId = moduleId;
        info.priority = priority;
        const bool effectivePreservesNonvolatileFprs =
            preservesNonvolatileFprs || kind == FunctionKind::Native;
        info.nonvolatileFprWriteMask = effectivePreservesNonvolatileFprs ? 0 : (nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
        info.entryPoint = reinterpret_cast<void*>(target);
        info.rawCpuInvoker = rawCpuInvoker;
        info.mustRemainDynamicallyDispatchable = mustRemainDynamicallyDispatchable;
        info.kind = kind;
        TranslatedFunctionRegistry::Register(std::move(info));
    }

    static std::tuple<Args...> BuildArgs(CpuContext* cpu) {
        size_t gprIndex = 0;
        size_t fprIndex = 0;
        return std::tuple<Args...>{LoadArgument<Args>(cpu, gprIndex, fprIndex)...};
    }

    template <typename T>
    static std::remove_reference_t<T> LoadArgument(CpuContext* cpu, size_t& gprIndex, size_t& fprIndex) {
        using CleanT = std::remove_reference_t<T>;
        if constexpr (std::is_floating_point_v<CleanT>) {
            if (fprIndex >= 13) {
                throw std::out_of_range("FPR argument overflow");
            }
            return static_cast<CleanT>(cpu->fpr[1 + fprIndex++].d);
        } else {
            if (gprIndex >= 8) {
                size_t stackArgIndex = gprIndex - 8;
                uint32_t sp = cpu->gpr[1];
                uint32_t argAddr = sp + 8 + static_cast<uint32_t>(stackArgIndex * 4);
                gprIndex++;
                try {
                    return static_cast<CleanT>(Memory::Read32(argAddr));
                } catch (const Memory::AccessViolation&) {
                    RT_LOG(RT_TAG_RUNTIME) << "Stack parameter read failed at 0x" << std::hex << argAddr
                              << " (SP=0x" << sp << ", argIndex=" << std::dec << (gprIndex - 1) << ")" << std::endl;
                    throw;
                }
            }
            return static_cast<CleanT>(cpu->gpr[3 + gprIndex++]);
        }
    }

    static void InvokeCpu(TargetFn target, CpuContext* cpu) {
        auto args = BuildArgs(cpu);
        if constexpr (std::is_void_v<Ret>) {
            std::apply(target, args);
        } else {
            auto result = std::apply(target, args);
            if constexpr (std::is_floating_point_v<Ret>) {
                cpu->fpr[1].d = static_cast<double>(result);
            } else {
                cpu->gpr[3] = static_cast<uint32_t>(result);
            }
        }
    }
};

template <auto Target>
struct AbiRawCpuThunk {
    static void Invoke(CpuContext* cpu) {
        if constexpr (std::is_invocable_v<decltype(Target), CpuContext*>) {
            (void)Target(cpu);
        } else {
            AbiTrampoline<std::remove_pointer_t<decltype(Target)>>::InvokeCpu(Target, cpu);
        }
    }
};

template <typename Ret>
class AbiTrampoline<Ret(CpuContext*)> {
public:
    using TargetFn = Ret (*)(CpuContext*);

    AbiTrampoline(uint32_t address,
                  const char* name,
                  TargetFn target,
                  FunctionKind kind = FunctionKind::BaseTranslated,
                  bool preservesNonvolatileFprs = false,
                  uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask,
                  uint32_t priority = 0,
                  uint64_t moduleId = 0,
                  void (*rawCpuInvoker)(CpuContext*) = nullptr,
                  bool mustRemainDynamicallyDispatchable = true)
    {
        TranslatedFunctionInfo info;
        info.address = address;
        info.name = name ? name : "";
        info.moduleId = moduleId;
        info.priority = priority;
        // Deliberately *not* mirroring the typed specialization's
        // `|| kind == FunctionKind::Native`. A CpuContext* native receives the
        // whole guest register file and can legitimately write f14-f31 through
        // it, so it stays conservative until the individual address has been
        // measured clean and its registration opts out below.
        info.nonvolatileFprWriteMask = preservesNonvolatileFprs ? 0 : (nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
        info.entryPoint = reinterpret_cast<void*>(target);
        info.rawCpuInvoker = rawCpuInvoker ? rawCpuInvoker : reinterpret_cast<void (*)(CpuContext*)>(target);
        info.mustRemainDynamicallyDispatchable = mustRemainDynamicallyDispatchable;
        info.kind = kind;
        TranslatedFunctionRegistry::Register(std::move(info));
    }
};

#define MKW_DETAIL_CAT(a, b) a##b
#define MKW_DETAIL_MAKE_UNIQUE(a, b) MKW_DETAIL_CAT(a, b)

// Hand-written registration macros, given the PAL address. Generated code never registers
// per function; every translated function reaches the registry via the bulk
// BulkTranslatedFunctionRecord tables in the translator's *_registration TUs.
#define REGISTER_TRANSLATED_FUNCTION(address, fn) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_trampoline_, __COUNTER__)(::GuestRegion::Translate(address), #fn, fn, FunctionKind::BaseTranslated, false, kPpcAllNonvolatileFprMask, 0, 0, nullptr, KnownTranslatedCpuCall<address>::kMustRemainDynamicallyDispatchable)

#define REGISTER_NATIVE_FUNCTION(address, fn) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_native_trampoline_, __COUNTER__)(::GuestRegion::Translate(address), #fn, fn, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, &AbiRawCpuThunk<&fn>::Invoke)

#define REGISTER_NATIVE_FUNCTION_AS(address, fn, pretty_name) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_native_trampoline_named_, __COUNTER__)(::GuestRegion::Translate(address), pretty_name, fn, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, &AbiRawCpuThunk<&fn>::Invoke)

// A native that calls through with MKW_GUEST_NEXT, so a mod overlay at its address still runs.
#define REGISTER_NATIVE_WRAPPER_AS(address, fn, pretty_name) REGISTER_NATIVE_FUNCTION_AS(address, fn, pretty_name)

// A native that deliberately takes over its address from any mod overlay there.
#define REGISTER_NATIVE_REPLACEMENT_AS(address, fn, pretty_name) REGISTER_NATIVE_FUNCTION_AS(address, fn, pretty_name)
