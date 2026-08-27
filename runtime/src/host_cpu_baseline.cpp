// Host ISA guard. Every other product target builds with -march=x86-64-v3, so a pre-Haswell
// Intel or pre-Excavator AMD machine would otherwise die on an illegal-instruction fault with no
// explanation. This TU alone skips that flag (own CMake object library, excluded from unity
// build/PCH) and runs from a priority-101 C initializer, ahead of every C++ dynamic initializer
// and thus the first AVX2 code that could execute. Keep it free of anything that could pull in
// vectorized code: no iostreams, no std::string, no runtime-wide headers.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <cpuid.h>
#include <windows.h>

namespace {

void HostCpuId(unsigned leaf, unsigned subleaf, unsigned regs[4]) {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
    regs[0] = eax;
    regs[1] = ebx;
    regs[2] = ecx;
    regs[3] = edx;
}

unsigned HostCpuIdMaxLeaf(unsigned base) {
    unsigned regs[4] = {0, 0, 0, 0};
    HostCpuId(base, 0, regs);
    return regs[0];
}

// XGETBV through inline assembly rather than the _xgetbv intrinsic: the GNU
// driver gates that intrinsic behind -mxsave, which this file is specifically
// compiled without. Only reachable once CPUID has reported OSXSAVE.
uint64_t ReadXcr0() {
    unsigned eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}

struct CpuFeature {
    const char* name;
    unsigned leaf;
    unsigned subleaf;
    unsigned reg;  // index into the eax/ebx/ecx/edx array filled by HostCpuId
    unsigned bit;
};

// Everything x86-64-v2 implies
constexpr CpuFeature kRequiredFeatures[] = {
    {"SSE3", 1, 0, 2, 0, false},
    {"SSSE3", 1, 0, 2, 9, false},
    {"CMPXCHG16B", 1, 0, 2, 13, false},
    {"SSE4.1", 1, 0, 2, 19, false},
    {"SSE4.2", 1, 0, 2, 20, false},
    {"POPCNT", 1, 0, 2, 23, false},
    {"LAHF-SAHF", 0x80000001u, 0, 2, 0, false},
};

// Fixed-capacity text accumulation: no allocation, no exceptions, nothing that
// could route through code this file is trying to stay ahead of.
struct TextBuffer {
    char data[1024] = {};
    size_t used = 0;

    void Append(const char* text) {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && used + 1 < sizeof(data)) {
            data[used++] = *text++;
        }
        data[used] = '\0';
    }
};

// True when the host can run this build. Otherwise `missing` holds the absent
// feature names, comma separated.
bool CollectMissingBaselineFeatures(TextBuffer& missing) {
    const unsigned maxBasic = HostCpuIdMaxLeaf(0);
    const unsigned maxExtended = HostCpuIdMaxLeaf(0x80000000u);

    bool ok = true;
    bool haveOsXsave = false;

    for (const CpuFeature& feature : kRequiredFeatures) {
        const bool leafAvailable = (feature.leaf & 0x80000000u) != 0
                                       ? feature.leaf <= maxExtended
                                       : feature.leaf <= maxBasic;

        bool present = false;
        if (leafAvailable) {
            unsigned regs[4] = {0, 0, 0, 0};
            HostCpuId(feature.leaf, feature.subleaf, regs);
            present = (regs[feature.reg] & (1u << feature.bit)) != 0;
        }

        if (present) {
            continue;
        }

        if (!ok) {
            missing.Append(", ");
        }
        missing.Append(feature.name);
        ok = false;
    }

    return ok;
}

// stdio is NOT usable from a .CRT$XIC initializer - the UCRT has not stood it
// up yet, and fprintf(stderr, ...) faults there. Verified on this toolchain:
// WriteFile on the raw standard-error handle and MessageBoxA both work, printf
// does not. Anything added to this reporting path has to respect that.
void WriteStdErrEarly(const char* text) {
    const HANDLE handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    DWORD written = 0;
    ::WriteFile(handle, text, static_cast<DWORD>(length), &written, nullptr);
}

[[noreturn]] void ReportUnsupportedCpu(const char* missing) {
    TextBuffer message;
    message.Append(
        "This build needs a processor that supports the x86-64-v2 "
        "instruction set.\n\nMissing on this machine: ");
    message.Append(missing);
    message.Append(
        "\n\nx86-64-v2 is supported by Intel Nehalem-era processors and newer "
        "and AMD Family 10h-era processors and newer.");

    // The tag matches RT_TAG_RUNTIME in runtime_log.h. It is spelled out here
    // because this translation unit must not include runtime-wide headers (see
    // the file comment): runtime_log.h pulls in <iostream> and memory.h, and
    // this code runs before any C++ dynamic initializer.
    WriteStdErrEarly("[runtime] ");
    WriteStdErrEarly(message.data);
    WriteStdErrEarly("\n");

    ::MessageBoxA(nullptr, message.data, "WiiCompiled - Unsupported Processor",
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
    // Leave through the OS rather than exit(): the C++ dynamic initializers
    // have not run yet, so there is no constructed program state to unwind and
    // the teardown path itself lives in AVX2 translation units.
    ::ExitProcess(1u);
}

}  // namespace

extern "C" int MkwHostCpuBaselineInit() {
    TextBuffer missing;
    if (!CollectMissingBaselineFeatures(missing)) {
        ReportUnsupportedCpu(missing.data);
    }
    return 0;
}

// Priorities 0-100 are reserved for the implementation; 101 is the earliest a
// user constructor can request, which puts this ahead of every default-priority
// constructor in the image.
__attribute__((constructor(101))) static void MkwHostCpuBaselineCtor() {
    MkwHostCpuBaselineInit();
}
