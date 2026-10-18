#pragma once
#include "guest_abi/cpu_context.h"

#include <cmath>
#include <cstdint>

// Condition Register Fields
#define CR_LT 0
#define CR_GT 1
#define CR_EQ 2
#define CR_SO 3

[[noreturn]] void PPC_Undefined(uint32_t pc, uint32_t rawInstruction, const char* details);

// Used by generated code when the translator encounters privileged/unmodeled PPC instructions
// (e.g. rfi). This is intentionally loud so missing HLE hooks are easy to find.
#define UNDEFINED(pc, rawInstruction, details) PPC_Undefined((pc), (rawInstruction), (details))

// Helper to set CR bits (Signed)
inline void SetCR(CpuContext* cpu, int field, int32_t a, int32_t b) {
    uint32_t crField = 0;
    if (a < b) crField |= 0x8; // LT
    if (a > b) crField |= 0x4; // GT
    if (a == b) crField |= 0x2; // EQ
    crField |= (cpu->xer >> 31) & 1u; // SO
    
    int shift = (7 - field) * 4;
    uint32_t mask = 0xF << shift;
    cpu->cr = (cpu->cr & ~mask) | (crField << shift);
}

// Helper to set CR bits (Unsigned)
inline void SetCR(CpuContext* cpu, int field, uint32_t a, uint32_t b) {
    uint32_t crField = 0;
    if (a < b) crField |= 0x8; // LT
    if (a > b) crField |= 0x4; // GT
    if (a == b) crField |= 0x2; // EQ
    crField |= (cpu->xer >> 31) & 1u; // SO
    
    int shift = (7 - field) * 4;
    uint32_t mask = 0xF << shift;
    cpu->cr = (cpu->cr & ~mask) | (crField << shift);
}

// Helper to set CR bits (Floating-point)
inline void SetCRFloat(CpuContext* cpu, int field, double a, double b) {
    uint32_t crField = 0;
    if (std::isnan(a) || std::isnan(b)) {
        // Unordered: LT/GT/EQ clear, SO set.
        crField = 0x1;
    } else {
        if (a < b) crField |= 0x8; // LT
        if (a > b) crField |= 0x4; // GT
        if (a == b) crField |= 0x2; // EQ
    }

    int shift = (7 - field) * 4;
    uint32_t mask = 0xF << shift;
    cpu->cr = (cpu->cr & ~mask) | (crField << shift);
}

// Helper to get CR bit
inline bool GetCRBit(CpuContext* cpu, int field, int bit) {
    int shift = (7 - field) * 4 + (3 - bit);
    return (cpu->cr >> shift) & 1;
}
