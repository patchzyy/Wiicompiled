#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include <csetjmp>

#include "memory.h"

// Windows' SehLogger longjmps out of a vectored exception handler, where plain setjmp/longjmp is
// the norm. A POSIX signal handler jumping back to here must use the sig-prefixed pair instead:
// only sigsetjmp/siglongjmp save and restore the process signal mask, which is what keeps SIGSEGV
// from staying blocked (and a second fault during the same ctor loop from escalating instead of
// trapping) after the first recovered fault.
#if defined(_WIN32)
using MkwJmpBuf = jmp_buf;
#define MKW_SETJMP(buf) setjmp(buf)
#else
using MkwJmpBuf = sigjmp_buf;
#define MKW_SETJMP(buf) sigsetjmp(buf, 1)
#endif

// Global flag to suppress SEH reporting (caught by system_bridge)
extern bool g_suppressSehReporting;
// Jump buffer for SEH recovery
extern thread_local MkwJmpBuf* g_sehJumpTarget;
// SEH details for the most recent trapped exception (used during ctor execution).
extern thread_local uint32_t g_sehLastExceptionCode;
extern thread_local uintptr_t g_sehLastExceptionAddress;
extern thread_local uintptr_t g_sehLastAccessedAddress;
extern thread_local uint32_t g_sehLastAccessType;

void WriteFatalLog(std::string_view reason);
void SetRuntimeExitCode(int code);
void MarkFatalErrorReported();

// Centralized crash reporting (defined in main.cpp). Every fatal path funnels
// through these so the per-run log folder always receives the same artifact
// set: crash_<reason>.txt with registers/backtrace/heuristics, plus MEM1/MEM2
// snapshots a developer can walk offline.
namespace RuntimeCrash {

// Writes crash_<reason>.txt (and, once per process, the guest memory
// snapshots) into the current run's log folder. Safe to call from any fatal
// path; never throws.
void WriteCrashArtifacts(std::string_view reason,
                         std::string_view extraDetails = {},
                         const uint32_t* missingGuestTarget = nullptr) noexcept;

// Full fatal path for a guest jump to an untranslated/invalid target:
// stderr diagnostics, crash artifacts, popup, exit.
[[noreturn]] void FatalMissingGuestTarget(uint32_t target, struct CpuContext* cpu) noexcept;

} // namespace RuntimeCrash

// Shows a user-facing explanation for a fatal runtime error. Declared here with
// its two siblings; all three are defined in main.cpp so translated dispatch,
// HLE, memory and Aurora callbacks share one popup and duplicate failures do
// not stack dialogs. (The ISA package declares this independently at
// isa/ppc_isa_context.h - that is its standalone host seam, not a duplicate.)
void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept;

// Mario Kart Wii's translated entry point. The products always boot here, so
// this is applied as the default while parsing the command line; there is no
// flag to override it.
inline constexpr uint32_t kDefaultEntryAddress = 0x800060A4u;

class SystemBridge {
public:
    static void Initialize();

private:
    static const Memory::RegionConfig* FindRegionConfig(const Memory::Config& config, std::string_view name);
    static void SeedLowMemDefaults(const Memory::Config& config);
    
public:
    static void DumpCpuState(const struct CpuContext* cpu);
    static void DumpCpuState(std::ostream& os, const struct CpuContext* cpu);

    // Crash-path helpers shared by the centralized reporter in main.cpp.
    // DumpCrashHeuristics prints plain-language "likely cause" hints derived
    // from the register state; WriteGuestMemorySnapshot writes MEM1 to
    // `mem1Path`, MEM2 to `mem1Path + ".mem2"` and the Kamek overlay to
    // `mem1Path + ".kamek"`, logging outcomes to `os`.
    static void DumpCrashHeuristics(std::ostream& os, const struct CpuContext* cpu,
                                    const uint32_t* missingGuestTarget);
    static void WriteGuestMemorySnapshot(std::ostream& os, const std::filesystem::path& mem1Path);
};
