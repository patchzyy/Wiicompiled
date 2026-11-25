#pragma once

// Test dispatch seam; the scheduler and fiber implementations are compiled unchanged.
#include "ppc_runtime.h"
#include "guest_abi/guest_region.h"

#include <cstring>

struct TranslatedFunctionInfo {
    void (*entry)(CpuContext*);
};

class TranslatedFunctionRegistry {
public:
    static const TranslatedFunctionInfo* FindByAddressPtr(uint32_t address);
};

CpuContext& GetPersistentCpuContext();
void InvokeIndirectCpu(uint32_t address, CpuContext* cpu);
[[noreturn]] void InvokeIndirectJump(uint32_t address, CpuContext* cpu);

#define REGISTER_NATIVE_FUNCTION(address, fn)
