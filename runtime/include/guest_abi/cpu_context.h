#pragma once

#include "isa/ppc_isa_fpenv.h"

#include <cstdint>

union PPC_FPR {
    uint64_t raw;
    double d;
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        float ps1; // Low word (Least Significant)
        float ps0; // High word (Most Significant)
#else
        float ps0; // High word
        float ps1; // Low word
#endif
    } paired;
};

// PowerPC CPU Context
struct CpuContext {
    // Standard GPRs
    uint32_t gpr[32];

    // Special Purpose Registers defined by standard PPC
    uint32_t cr;        // Condition Register
    uint32_t lr;        // Link Register
    uint32_t ctr;       // Count Register
    uint32_t xer;       // Integer Exception Register
    uint32_t fpscr;     // Floating-Point Status and Control Register

    // Program State
    uint32_t pc;        // Program Counter (not used alot tho)

    // Floating Point Registers (Modified for Paired Single support)
    PPC_FPR  fpr[32];

    // Broadway Specific Extensions
    uint32_t gqr[8];    // Graphics Quantization Registers
    uint32_t hid0;      // HID0
    uint32_t hid1;      // HID1
    uint32_t hid2;      // HID2

    uint32_t srr0;      // Save/Restore Register 0
    uint32_t srr1;      // Save/Restore Register 1
    uint32_t msr;       // Machine State Register
};

inline thread_local CpuContext* g_currentCpuContext = nullptr;

class CpuContextScope {
public:
    explicit CpuContextScope(CpuContext* ctx)
        : previous_(g_currentCpuContext)
    {
        g_currentCpuContext = ctx;

        savedMxcsr_ = MkwGetHostFpControl();
        if (ctx != nullptr)
            MkwApplyHostNiMode(ctx->fpscr);
    }

    ~CpuContextScope()
    {
        g_currentCpuContext = previous_;
        if (previous_ != nullptr)
            MkwApplyHostNiMode(previous_->fpscr);
        else
            MkwRestoreHostMxcsr(savedMxcsr_);
    }

    CpuContextScope(const CpuContextScope&) = delete;
    CpuContextScope& operator=(const CpuContextScope&) = delete;

private:
    CpuContext* previous_ = nullptr;
    uint32_t savedMxcsr_ = 0;
};

inline CpuContext* TryGetCpuContext() noexcept
{
    return g_currentCpuContext;
}
