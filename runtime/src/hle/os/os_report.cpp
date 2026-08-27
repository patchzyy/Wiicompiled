// OSReport formatting/logging.

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "hle/guest_printf.h"
#include "ppc_runtime.h"
#include "system_bridge.h"

// Thread-local cache used to store host-side copies of guest strings so we can
// safely return const char* pointers that remain valid for the duration of a
// single OSReport call. OS__Report will clear this each time it's invoked.
static thread_local std::vector<std::string> g_guest_string_cache;

// Forward declaration for the helper used by the formatter.
static const char* GetGuestString(uint32_t guestAddr);

// ============================================================================
// OSReport Implementation
// ============================================================================

// ----------------------------------------------------------------------------
// OSReport formatting helpers
// ----------------------------------------------------------------------------

struct OsReportVarArgState {
    CpuContext* cpu = nullptr;
    size_t gprIndex = 0; // r4 maps to index 0
    size_t fprIndex = 0; // f1 maps to index 0
    bool missingArgWarned = false;
};

static uint32_t NextOsReportU32(OsReportVarArgState& state)
{
    if (state.cpu && state.gprIndex < 7) {
        return state.cpu->gpr[4 + state.gprIndex++];
    }
    if (state.cpu) {
        const uint32_t stackArg = state.cpu->gpr[1] + 8u +
                                  static_cast<uint32_t>((state.gprIndex++ - 7u) * 4u);
        try {
            return Memory::Read32(stackArg);
        } catch (const Memory::AccessViolation&) {
        }
    }
    if (!state.missingArgWarned) {
        std::cout << "[OSReport] warning: missing GPR vararg; defaulting to 0" << std::endl;
        state.missingArgWarned = true;
    }
    ++state.gprIndex;
    return 0;
}

static uint64_t NextOsReportU64(OsReportVarArgState& state)
{
    // Align to 8-byte boundary relative to the full argument list.
    // The OSReport fixed arg (fmt) is in r3, so r4 corresponds to an offset of 4 bytes.
    // PPC varargs align 64-bit values to 8 bytes, which means we must skip r4 when needed.
    if (((state.gprIndex + 1u) & 1u) != 0) {
        ++state.gprIndex;
    }
    uint64_t hi = NextOsReportU32(state);
    uint64_t lo = NextOsReportU32(state);
    return (hi << 32) | lo;
}

static double NextOsReportDouble(OsReportVarArgState& state)
{
    if (state.cpu && state.fprIndex < 8) {
        return state.cpu->fpr[1 + state.fprIndex++].d;
    }
    return 0.0;
}

static std::string ReadGuestStringForReport(uint32_t guestAddr)
{
    const char* s = GetGuestString(guestAddr);
    if (!s) {
        return "(null)";
    }
    return std::string(s);
}

static void HLE_LogOSReport(CpuContext* cpu, const char* fmt)
{
    if (!fmt) {
        std::cout << "[OSReport] (null fmt)" << std::endl;
        return;
    }

    OsReportVarArgState state{cpu};
    size_t renderedLen = 0;
    std::string buffer = RuntimeHle::FormatGuestPrintf(
        fmt,
        renderedLen,
        [&state]() { return NextOsReportU32(state); },
        [&state]() { return NextOsReportU64(state); },
        [&state]() { return NextOsReportDouble(state); },
        [](uint32_t address) { return ReadGuestStringForReport(address); });

    // nw4r warnings arrive as "<file>:<line> Warning:" plus a bare newline, so
    // consecutive identical messages never land back to back. Blank lines are
    // transparent to the repeat tracker so the pair still collapses.
    static thread_local std::string lastBuffer;
    static thread_local size_t repeated = 0;
    const bool blank = buffer.find_first_not_of(" \t\r\n") == std::string::npos;
    if (blank) {
        if (repeated != 0) {
            return;
        }
    } else if (buffer == lastBuffer) {
        ++repeated;
        return;
    } else {
        if (repeated != 0) {
            std::cout << "[OSReport] previous message repeated " << repeated << " time(s)" << std::endl;
            repeated = 0;
        }
        lastBuffer = buffer;
    }

    std::cout << "[OSReport] " << buffer;

    // OSReport strings don't always end in \n, so flush explicitly
    if (buffer.empty() || buffer.back() != '\n') {
        std::cout << std::endl;
    } else {
        std::cout << std::flush;
    }

    // Translator correctness diagnostic: NW4R assertion reports normally lose
    // the guest caller because OS__Report is an HLE boundary. The context is
    // synchronized at this boundary, so capture the guest backchain at the
    // first warning/panic instead of attributing the later PPCHalt unwind.
    //
    // The dump is expensive and stdio is an unbuffered pipe, so a guest that
    // warns every frame would stall the game thread on backpressure. One dump
    // per distinct site, with an overall cap.
    if (buffer.find(" Warning:") != std::string::npos ||
        buffer.find(" Panic:") != std::string::npos) {
        constexpr size_t kMaxWarningDumps = 8;
        static thread_local std::set<std::string> dumpedSites;
        static thread_local size_t dumpsEmitted = 0;
        if (dumpsEmitted < kMaxWarningDumps && dumpedSites.insert(buffer).second) {
            ++dumpsEmitted;
            SystemBridge::DumpCpuState(cpu);
            if (dumpsEmitted == kMaxWarningDumps) {
                std::cerr << "[runtime] guest warning context dumps capped at " << kMaxWarningDumps
                          << "; further warnings log the message only." << std::endl;
            }
        }
    }
}

// Read a NULL-terminated string from guest memory and return a host-owned
// const char* pointer that remains valid for the duration of the calling
// thread's OS__Report invocation. This uses a thread-local cache to store
// copies so that earlier pointers don't get invalidated by later calls.
static const char* GetGuestString(uint32_t guestAddr)
{
    if (guestAddr == 0) {
        return nullptr;
    }

    constexpr size_t kMaxGuestStringLength = 4096; // safety cap
    std::string s;
    s.reserve(128);
    try {
        for (size_t i = 0; i < kMaxGuestStringLength; ++i) {
            const uint8_t b = ::Memory::Read8(guestAddr + static_cast<uint32_t>(i));
            if (b == 0) {
                break;
            }
            s.push_back(static_cast<char>(b));
        }
    } catch (const ::Memory::AccessViolation&) {
        // Fallthrough; we will present a replacement that indicates invalid memory
        s = "<invalid_guest_memory>";
    }

    // Push into the thread-local cache and return a stable pointer
    g_guest_string_cache.push_back(std::move(s));
    return g_guest_string_cache.back().c_str();
}

// Keep the full CPU context: variadic integer arguments use r4-r10/stack while
// variadic floating-point arguments use f1-f8.
extern "C" void OS__Report_801a25d0(CpuContext* ctx)
{
    // Clear the thread-local cache of host-side string copies so that each
    // call of OS__Report has fresh buffers and returned const char* values
    // remain valid for the duration of this function.
    g_guest_string_cache.clear();

    const char* fmt_str = GetGuestString(ctx ? ctx->gpr[3] : 0);
    HLE_LogOSReport(ctx, fmt_str);
}

PPC_NATIVE_OVERRIDE_VOID(801A25D0, OS__Report_801a25d0, (CpuContext* ctx), (ctx));
