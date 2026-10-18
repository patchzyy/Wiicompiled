#pragma once
#include "ppc_isa_fpenv.h"
#include "guest_abi/cpu_context.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept;

inline CpuContext* CurrentCpuContext()
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        std::cerr << "[runtime] CRITICAL: CurrentCpuContext is NULL. "
                  << "Did you forget to create a CpuContextScope?" << std::endl;
        ShowRuntimeFatalPopup("Runtime context failure",
                              "The game stopped because a translated function tried to run without a CPU context.");
        std::abort();
    }

    if (cpu->gpr[1] == 0) {
        std::cerr << "[runtime] CRITICAL: Guest Stack Pointer (r1) is NULL (0x00000000). "
                  << "The emulated program has crashed." << std::endl;
        ShowRuntimeFatalPopup("Guest execution failure",
                              "The game stopped because the guest stack pointer became null while translated code was running.");
        std::abort(); // Stop immediately so you can debug the cause.
    }
    return cpu;
}

#include "guest_abi/guest_cr.h"

extern "C" void DumpHostStackTraceForRuntimeHelper();
void MarkFatalErrorReported();
