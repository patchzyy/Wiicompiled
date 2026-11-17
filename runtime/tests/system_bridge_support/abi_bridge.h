#pragma once

#include "ppc_runtime.h"
#include "guest_abi/guest_region.h"
#include <optional>

struct TranslatedFunctionInfo {
    uint32_t address;
    const char* name;
};
class TranslatedFunctionRegistry {
public:
    static const TranslatedFunctionInfo* FindByAddressPtr(uint32_t address);
};
std::optional<TranslatedFunctionInfo> FindNearestTranslatedFunction(uint32_t address);
void InitializePersistentCpuContext();
CpuContext& GetPersistentCpuContext();
void InvokeIndirectCpu(uint32_t address, CpuContext* cpu);
