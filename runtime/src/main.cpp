#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <crtdbg.h>
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#else
#include <unistd.h>
#endif

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "gx_guest_write.h"
#include "memory.h"
#include "system_bridge.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "fiber_manager.h"
#include "hle_stubs.h"
#include "host_platform.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"
#include "recomp_mod_loader.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

// Defined in `runtime/src/hle/vi.cpp` (used by GX/VI HLE).
extern std::atomic_bool g_auroraFrameActive;
extern "C" int g_gxFrameCount;
extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath);
bool OS_HLE_InterruptsEnabled() noexcept;

namespace {

// Defined below, beside the fatal-log machinery.
std::string FormatHostStackTrace(unsigned framesToSkip = 0);

void ServiceGuestTimingDuringAuroraFrameWait() {
    // Aurora can block inside FIFO drains before control returns to GX HLE, for as long as a
    // whole display period. Keep VI retraces, alarms and audio moving at wall-clock cadence here
    // while still suppressing guest rescheduling and recursive Aurora work.
    VI_HLE_ProcessRetracesDeferred(8);
    OS_HLE_ProcessAlarmsDeferred(8);
    Audio_HLE_PollDeferred();
}

#if defined(_WIN32)
int __cdecl WindowsCrtReportHook(int reportType, char* message, int* returnValue) {
    if (returnValue) {
        *returnValue = 0;
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report type=" << reportType;
    if (message) {
        std::cerr << ": " << message;
    } else {
        std::cerr << '\n';
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report stack:\n" << FormatHostStackTrace(1);
    std::cerr.flush();
    return TRUE;
}

void ConfigureWindowsFatalDialogBehavior() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook(WindowsCrtReportHook);
}

using LoadLibraryExA_t = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
LoadLibraryExA_t g_realLoadLibraryExA = nullptr;

// Wine's LoadLibraryExA rejects LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR with
// ERROR_INVALID_PARAMETER whenever hFile is null, even though that flag is
// documented as meaningless (and real Windows silently ignores it) outside of
// a dependent-DLL load. Dawn's DynamicLib::Open always ORs it into the flags
// it passes when opening "vulkan-1.dll", which otherwise loads fine under
// Wine once that one bit is stripped. Patching webgpu_dawn.dll's own IAT
// entry (rather than Dawn's source, which we don't build) keeps the fix
// self-contained and a no-op on real Windows.
HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (hFile == nullptr && lpLibFileName != nullptr &&
        (dwFlags & LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) != 0 &&
        _stricmp(lpLibFileName, "vulkan-1.dll") == 0) {
        dwFlags &= ~static_cast<DWORD>(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    }
    return g_realLoadLibraryExA(lpLibFileName, hFile, dwFlags);
}

bool ImportModuleNameMatchesKernel32(const char* moduleName) {
    // Recent toolchains often record kernel32 imports against a virtual API
    // Set forwarder (e.g. "api-ms-win-core-libraryloader-l1-*.dll") instead of
    // the literal "kernel32.dll", so match both. The prefix length must be the
    // literal's strlen (29), not 30: with 30, _strnicmp also compares the
    // literal's NUL terminator against the real forwarder name's next byte
    // (e.g. '-' in "...libraryloader-l1-2-0.dll"), which never matches, so the
    // prefix check silently failed for every real forwarder-named import.
    constexpr char kLibraryLoaderPrefix[] = "api-ms-win-core-libraryloader";
    return _stricmp(moduleName, "kernel32.dll") == 0 ||
           _strnicmp(moduleName, kLibraryLoaderPrefix, sizeof(kLibraryLoaderPrefix) - 1) == 0;
}

void PatchDawnVulkanLoaderForWine() {
    const HMODULE dawnModule = ::GetModuleHandleA("webgpu_dawn.dll");
    if (dawnModule == nullptr) {
        RT_LOG(RT_TAG_RUNTIME) << "[runtime] webgpu_dawn.dll not loaded; skipping Wine Vulkan loader patch." << std::endl;
        return;  // Statically-linked Dawn build; nothing to patch.
    }
    auto* base = reinterpret_cast<BYTE*>(dawnModule);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dosHeader->e_lfanew);
    const auto& importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0) {
        RT_LOG(RT_TAG_RUNTIME) << "[runtime] webgpu_dawn.dll has no import directory; skipping Wine Vulkan loader patch." << std::endl;
        return;
    }

    auto* importDescriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirectory.VirtualAddress);
    for (; importDescriptor->Name != 0; ++importDescriptor) {
        const char* moduleName = reinterpret_cast<const char*>(base + importDescriptor->Name);
        if (importDescriptor->FirstThunk == 0 || !ImportModuleNameMatchesKernel32(moduleName)) {
            continue;
        }
        auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + importDescriptor->FirstThunk);
        int entryCount = 0;

        if (importDescriptor->OriginalFirstThunk == 0) {
            // No Import Lookup Table: FirstThunk is the only thunk array, and
            // by the time GetModuleHandleA above succeeded the loader had
            // already bound it - its entries are resolved function addresses,
            // not name-table RVAs, so they must not be walked as
            // IMAGE_IMPORT_BY_NAME (that would dereference a function address
            // as if it were an RVA). Identify LoadLibraryExA by its resolved
            // address instead.
            const HMODULE kernel32Module = ::GetModuleHandleA("kernel32.dll");
            const auto realLoadLibraryExA = kernel32Module
                ? reinterpret_cast<LoadLibraryExA_t>(::GetProcAddress(kernel32Module, "LoadLibraryExA"))
                : nullptr;
            if (!realLoadLibraryExA) {
                RT_LOG(RT_TAG_RUNTIME) << "[runtime] Could not resolve kernel32.dll!LoadLibraryExA; skipping module '"
                    << moduleName << "' (no Import Lookup Table to walk by name)." << std::endl;
                continue;
            }
            for (; addressThunk->u1.Function != 0; ++addressThunk, ++entryCount) {
                if (reinterpret_cast<LoadLibraryExA_t>(addressThunk->u1.Function) != realLoadLibraryExA) {
                    continue;
                }
                g_realLoadLibraryExA = realLoadLibraryExA;
                DWORD oldProtect = 0;
                if (::VirtualProtect(&addressThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    addressThunk->u1.Function = reinterpret_cast<ULONGLONG>(&HookedLoadLibraryExA);
                    DWORD ignored = 0;
                    ::VirtualProtect(&addressThunk->u1.Function, sizeof(void*), oldProtect, &ignored);
                    RT_LOG(RT_TAG_RUNTIME) << "[runtime] Patched webgpu_dawn.dll's LoadLibraryExA import (module="
                        << moduleName << ") to work around a Wine LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR bug." << std::endl;
                } else {
                    RT_LOG(RT_TAG_RUNTIME) << "[runtime] Found LoadLibraryExA import but VirtualProtect failed, error="
                        << ::GetLastError() << std::endl;
                }
                return;
            }
            RT_LOG(RT_TAG_RUNTIME) << "[runtime] Matched import module '" << moduleName << "' (" << entryCount
                << " entries, no Import Lookup Table) but did not find a bound LoadLibraryExA in it." << std::endl;
            continue;
        }

        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + importDescriptor->OriginalFirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addressThunk, ++entryCount) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) {
                continue;
            }
            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), "LoadLibraryExA") != 0) {
                continue;
            }
            g_realLoadLibraryExA = reinterpret_cast<LoadLibraryExA_t>(addressThunk->u1.Function);
            DWORD oldProtect = 0;
            if (::VirtualProtect(&addressThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                addressThunk->u1.Function = reinterpret_cast<ULONGLONG>(&HookedLoadLibraryExA);
                DWORD ignored = 0;
                ::VirtualProtect(&addressThunk->u1.Function, sizeof(void*), oldProtect, &ignored);
                RT_LOG(RT_TAG_RUNTIME) << "[runtime] Patched webgpu_dawn.dll's LoadLibraryExA import (module="
                    << moduleName << ") to work around a Wine LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR bug." << std::endl;
            } else {
                RT_LOG(RT_TAG_RUNTIME) << "[runtime] Found LoadLibraryExA import but VirtualProtect failed, error="
                    << ::GetLastError() << std::endl;
            }
            return;
        }
        RT_LOG(RT_TAG_RUNTIME) << "[runtime] Matched import module '" << moduleName << "' (" << entryCount
            << " entries) but did not find LoadLibraryExA in it." << std::endl;
    }
    RT_LOG(RT_TAG_RUNTIME) << "[runtime] No kernel32-like import module found in webgpu_dawn.dll; Wine Vulkan loader patch not applied." << std::endl;
}

class WindowsTimerResolutionGuard {
public:
    WindowsTimerResolutionGuard() {
        const MMRESULT result = ::timeBeginPeriod(1);
        if (result == TIMERR_NOERROR) {
            armed_ = true;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "timeBeginPeriod(1) failed: " << result << std::endl;
        }
    }

    ~WindowsTimerResolutionGuard() {
        if (armed_) {
            ::timeEndPeriod(1);
        }
    }

    WindowsTimerResolutionGuard(const WindowsTimerResolutionGuard&) = delete;
    WindowsTimerResolutionGuard& operator=(const WindowsTimerResolutionGuard&) = delete;

private:
    bool armed_ = false;
};
#endif
} // namespace

namespace {

std::string g_lastEntryLabel;
std::atomic_bool g_auroraInitialized{false};
std::atomic_flag g_abortSignalHandled = ATOMIC_FLAG_INIT;
std::atomic_bool g_fatalErrorReported{false};
std::atomic_bool g_fatalPopupShown{false};
std::atomic<int> g_lastExitCode{0};
std::atomic_bool g_exitCodeSet{false};

struct ProcessTranscriptState {
    bool enabled = false;
    std::filesystem::path path;
    std::ofstream file;
    std::mutex fileMutex;
    std::atomic_bool initialized{false};
    int savedStdoutFd = -1;
    int savedStderrFd = -1;
    int stdoutPipeReadFd = -1;
    int stdoutPipeWriteFd = -1;
    int stderrPipeReadFd = -1;
    int stderrPipeWriteFd = -1;
    std::thread stdoutThread;
    std::thread stderrThread;
};

std::filesystem::path GetDefaultRuntimeLogDirectory() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "Logs";
}

// Every entry in the Logs root - both the per-run folders written by this
// scheme and any flat .log files left over from the previous one - is removed
// once it is older than the retention window.
void PruneOldRunLogs(const std::filesystem::path& logRoot) {
    constexpr auto kRetention = std::chrono::hours(24 * 4);

    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(logRoot, ec)) {
        std::error_code entryEc;
        const auto writeTime = std::filesystem::last_write_time(entry.path(), entryEc);
        if (entryEc) {
            continue;
        }
        if (now - writeTime > kRetention) {
            std::filesystem::remove_all(entry.path(), entryEc);
        }
    }
}

// Logs/<product>_<epochSeconds>_pid<pid>/ - one folder per run. The console
// transcript and every crash artifact for the run land in here, so "zip this
// folder" is a complete diagnostic. Created lazily so even a crash before
// transcript setup still has somewhere to write.
const std::filesystem::path& GetRunLogDirectory() {
    static const std::filesystem::path runDirectory = [] {
        const std::filesystem::path logRoot = GetDefaultRuntimeLogDirectory();

        std::error_code ec;
        std::filesystem::create_directories(logRoot, ec);
        PruneOldRunLogs(logRoot);

#if defined(_WIN32)
        const unsigned long pid = ::GetCurrentProcessId();
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        const auto now = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        std::ostringstream name;
        name << (RuntimeProduct::IsRetroRewind() ? "retro_rewind" : "base")
             << "_" << secs << "_pid" << pid;
        const std::filesystem::path directory = logRoot / name.str();
        std::filesystem::create_directories(directory, ec);
        return directory;
    }();
    return runDirectory;
}

ProcessTranscriptState& GetProcessTranscriptState() {
    static ProcessTranscriptState state;
    return state;
}

void WriteProcessTranscriptChunk(ProcessTranscriptState& state, const char* data, size_t size) {
    if (!state.enabled || !state.file || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.fileMutex);
    state.file.write(data, static_cast<std::streamsize>(size));
    state.file.flush();
}

void PumpTranscriptPipe(ProcessTranscriptState& state, int readFd, int mirrorFd) {
    std::array<char, 4096> buffer{};
    for (;;) {
#if defined(_WIN32)
        const int bytesRead = _read(readFd, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
        const ssize_t bytesRead = ::read(readFd, buffer.data(), buffer.size());
#endif
        if (bytesRead <= 0) {
            break;
        }

        if (mirrorFd >= 0) {
            size_t offset = 0;
            while (offset < static_cast<size_t>(bytesRead)) {
#if defined(_WIN32)
                const int written = _write(mirrorFd,
                                           buffer.data() + offset,
                                           static_cast<unsigned int>(static_cast<size_t>(bytesRead) - offset));
#else
                const ssize_t written = ::write(mirrorFd,
                                                buffer.data() + offset,
                                                static_cast<size_t>(bytesRead) - offset);
#endif
                if (written <= 0) {
                    break;
                }
                offset += static_cast<size_t>(written);
            }
        }

        WriteProcessTranscriptChunk(state, buffer.data(), static_cast<size_t>(bytesRead));
    }
}

#if defined(_WIN32)
int GetFileDescriptor(FILE* file) {
    return _fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return _dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return _dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    _close(fd);
}
#else
int GetFileDescriptor(FILE* file) {
    return fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return ::dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return ::dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    ::close(fd);
}
#endif

bool InstallTranscriptPipe(int& outReadFd, int& outWriteFd, int targetFd) {
#if defined(_WIN32)
    int pipeFds[2]{-1, -1};
    if (_pipe(pipeFds, 8192, _O_BINARY) != 0) {
        return false;
    }
#else
    int pipeFds[2]{-1, -1};
    if (::pipe(pipeFds) != 0) {
        return false;
    }
#endif

    outReadFd = pipeFds[0];
    outWriteFd = pipeFds[1];
    if (targetFd == GetFileDescriptor(stdout)) {
        std::fflush(stdout);
    } else if (targetFd == GetFileDescriptor(stderr)) {
        std::fflush(stderr);
    }

    if (DuplicateFileDescriptorTo(outWriteFd, targetFd) < 0) {
        CloseFileDescriptor(outReadFd);
        CloseFileDescriptor(outWriteFd);
        outReadFd = -1;
        outWriteFd = -1;
        return false;
    }
    return true;
}

#if defined(_WIN32)
void AttachParentConsoleForDiagnostics() {
    // GUI-subsystem products get no console and no bound stdout/stderr unless a parent already
    // redirected them (pipe/file: leave alone) or has a console to attach to (bind only the
    // streams still unbound). With no parent console, fall back to NUL rather than leaving
    // _fileno(stdout) == -2, which would stop InitializeProcessTranscript from redirecting into
    // the log file at all.
    const HANDLE outHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE errHandle = ::GetStdHandle(STD_ERROR_HANDLE);
    const bool haveOut = outHandle != nullptr && outHandle != INVALID_HANDLE_VALUE;
    const bool haveErr = errHandle != nullptr && errHandle != INVALID_HANDLE_VALUE;
    if (haveOut && haveErr) {
        return;
    }

    const bool attached =
        ::GetConsoleWindow() != nullptr || ::AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    const char* const sink = attached ? "CONOUT$" : "NUL";
    if (!haveOut) {
        (void)std::freopen(sink, "w", stdout);
    }
    if (!haveErr) {
        (void)std::freopen(sink, "w", stderr);
    }
    std::cout.clear();
    std::cerr.clear();
}
#else
void AttachParentConsoleForDiagnostics() {}
#endif

// The setup writes build-fingerprint.json beside every product executable; its SetupVersion is
// the only version identity the runtime has (products are compiled locally, so nothing is baked
// into the binary). Surface it at the top of the transcript so every attached log self-identifies.
std::string ReadInstalledSetupVersion() {
    const auto directory = RuntimeConfigFile::ExecutableDirectory();
    if (!directory) {
        return {};
    }
    std::ifstream file(*directory / "build-fingerprint.json", std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = "\"SetupVersion\"";
    const auto keyPos = text.find(kKey);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colon = text.find(':', keyPos + kKey.size());
    const auto open = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
    const auto close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    return text.substr(open + 1, close - open - 1);
}

void InitializeProcessTranscript(int argc, char** argv) {
    auto& state = GetProcessTranscriptState();
    if (state.initialized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const std::filesystem::path path = GetRunLogDirectory() / "console.log";
    state.file.open(path.string(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!state.file) {
        return;
    }

    state.enabled = true;
    state.path = path;

#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif

    const std::string setupVersion = ReadInstalledSetupVersion();

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "[runtime] WiiCompiled "
                   << (setupVersion.empty() ? "version unknown" : setupVersion) << "\n";
        state.file << "[runtime] process transcript started\n";
        state.file << "[runtime] pid=" << pid << "\n";
        state.file << "[runtime] argv=";
        for (int i = 0; i < argc; ++i) {
            if (i != 0) {
                state.file << ' ';
            }
            state.file << argv[i];
        }
        state.file << "\n";
        state.file.flush();
    }

    state.savedStdoutFd = DuplicateFileDescriptor(GetFileDescriptor(stdout));
    state.savedStderrFd = DuplicateFileDescriptor(GetFileDescriptor(stderr));

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    auto restoreFailedSetup = [&state]() {
        if (state.savedStdoutFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
        }
        if (state.savedStderrFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
        }
        if (state.stdoutPipeReadFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeReadFd);
            state.stdoutPipeReadFd = -1;
        }
        if (state.stdoutPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeWriteFd);
            state.stdoutPipeWriteFd = -1;
        }
        if (state.stderrPipeReadFd >= 0) {
            CloseFileDescriptor(state.stderrPipeReadFd);
            state.stderrPipeReadFd = -1;
        }
        if (state.stderrPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stderrPipeWriteFd);
            state.stderrPipeWriteFd = -1;
        }
        if (state.savedStdoutFd >= 0) {
            CloseFileDescriptor(state.savedStdoutFd);
            state.savedStdoutFd = -1;
        }
        if (state.savedStderrFd >= 0) {
            CloseFileDescriptor(state.savedStderrFd);
            state.savedStderrFd = -1;
        }
        state.enabled = false;
        state.file.close();
    };

    if (!InstallTranscriptPipe(state.stdoutPipeReadFd, state.stdoutPipeWriteFd, GetFileDescriptor(stdout)) ||
        !InstallTranscriptPipe(state.stderrPipeReadFd, state.stderrPipeWriteFd, GetFileDescriptor(stderr))) {
        restoreFailedSetup();
        return;
    }

    state.stdoutThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stdoutPipeReadFd, state.savedStdoutFd);
    });
    state.stderrThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stderrPipeReadFd, state.savedStderrFd);
    });
}

void ShutdownProcessTranscript() {
    auto& state = GetProcessTranscriptState();
    if (!state.enabled) {
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();

    if (state.savedStdoutFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
    }
    if (state.savedStderrFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
    }

    if (state.stdoutPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeWriteFd);
        state.stdoutPipeWriteFd = -1;
    }
    if (state.stderrPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stderrPipeWriteFd);
        state.stderrPipeWriteFd = -1;
    }

    if (state.stdoutThread.joinable()) {
        state.stdoutThread.join();
    }
    if (state.stderrThread.joinable()) {
        state.stderrThread.join();
    }

    if (state.stdoutPipeReadFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeReadFd);
        state.stdoutPipeReadFd = -1;
    }
    if (state.stderrPipeReadFd >= 0) {
        CloseFileDescriptor(state.stderrPipeReadFd);
        state.stderrPipeReadFd = -1;
    }
    if (state.savedStdoutFd >= 0) {
        CloseFileDescriptor(state.savedStdoutFd);
        state.savedStdoutFd = -1;
    }
    if (state.savedStderrFd >= 0) {
        CloseFileDescriptor(state.savedStderrFd);
        state.savedStderrFd = -1;
    }

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "\n[runtime] process transcript ended\n";
        state.file.flush();
    }
    state.file.close();
    state.enabled = false;
}

// The one host stack walker. Every caller - the CRT report hook, the fatal log
// and the stderr crash dump - goes through this, so the log and the console see
// exactly the same frames, including the TranslatedFunctionRegistry fallback for
// addresses DbgHelp cannot name.
std::string FormatHostStackTrace(unsigned framesToSkip) {
#if defined(_WIN32)
    static std::atomic_bool s_symbolsReady{false};
    HANDLE process = GetCurrentProcess();
    if (!s_symbolsReady.load(std::memory_order_acquire)) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(process, nullptr, TRUE)) {
            s_symbolsReady.store(true, std::memory_order_release);
        }
    }
    const bool symbolsReady = s_symbolsReady.load(std::memory_order_acquire);

    void* frames[64]{};
    const USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(framesToSkip),
                                                  static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::ostringstream out;
    if (captured == 0) {
        out << "[runtime] host stack trace unavailable (CaptureStackBackTrace returned 0)\n";
        return out.str();
    }
    out << "[runtime] host stack trace (most recent call first):\n";
    for (USHORT i = 0; i < captured; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = "?";
        DWORD64 moduleBase = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]),
                               &module) != 0 &&
            module != nullptr) {
            moduleBase = reinterpret_cast<DWORD64>(module);
            (void)GetModuleFileNameA(module, modulePath, MAX_PATH);
        }

        const char* symbolName = "?";
        std::string translatedFuncName;
        uint32_t ppcAddress = 0;
        DWORD64 symbolDisp = 0;
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (symbolsReady && SymFromAddr(process, addr, &symbolDisp, symbol)) {
            symbolName = symbol->Name;
        } else if (auto info = TranslatedFunctionRegistry::FindByHostAddress(static_cast<uintptr_t>(addr))) {
            // DbgHelp could not name it; the translated-function registry can.
            translatedFuncName = info->name;
            ppcAddress = info->address;
            if (!translatedFuncName.empty()) {
                symbolName = translatedFuncName.c_str();
                symbolDisp = addr - reinterpret_cast<DWORD64>(info->entryPoint);
            }
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const char* fileName = nullptr;
        DWORD lineNumber = 0;
        if (symbolsReady && SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            fileName = line.FileName;
            lineNumber = line.LineNumber;
        }

        out << "  ["
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(i)
            << std::setfill(' ') << "] "
            << modulePath << "!" << symbolName
            << " + 0x" << std::hex << std::uppercase << symbolDisp
            << " (0x" << addr;
        if (moduleBase != 0) {
            out << ", module+0x" << (addr - moduleBase);
        }
        if (ppcAddress != 0) {
            out << ", PPC:0x" << std::setw(8) << std::setfill('0') << ppcAddress << std::setfill(' ');
        }
        out << std::dec << std::nouppercase;
        if (fileName) {
            out << ", " << fileName << ":" << lineNumber;
        }
        out << ")\n";
    }
    return out.str();
#else
    (void)framesToSkip;
    return {};
#endif
}

void WriteFatalLogImpl(std::string_view reason, std::string_view extraDetails = {},
                       const uint32_t* missingGuestTarget = nullptr) {
    const std::filesystem::path runDirectory = GetRunLogDirectory();
    std::string fileName = "crash_";
    fileName.append(reason);
    fileName.append(".txt");
    std::ofstream out((runDirectory / fileName).string(), std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << "[runtime] fatal log" << std::endl;
    out << "[runtime] reason: " << reason << std::endl;
    out << "[runtime] timestamp(seconds): " << nowSecs << std::endl;
    out << "[runtime] entry: " << (g_lastEntryLabel.empty() ? "<unknown>" : g_lastEntryLabel) << std::endl;
    if (!extraDetails.empty()) {
        out << "[runtime] details: " << extraDetails << std::endl;
    }
    if (missingGuestTarget) {
        out << "[runtime] guest jump target: 0x" << std::hex << std::uppercase
            << *missingGuestTarget << std::dec << std::endl;
    }

    const CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        // Exceptions can unwind CpuContextScope before we get here.
        // Fall back to the persistent CPU context snapshot so fatal logs still include registers.
        cpu = &GetPersistentCpuContext();
    }
    if (cpu) {
        SystemBridge::DumpCrashHeuristics(out, cpu, missingGuestTarget);
        SystemBridge::DumpCpuState(out, cpu);
    } else {
        out << "[runtime] CPU context unavailable." << std::endl;
    }

    // Guest memory snapshots so the heap/object state can be walked offline.
    // Written once per process: several fatal paths can fire in sequence
    // (e.g. an exception followed by the exit-code report) and the snapshots
    // are large.
    static std::atomic_bool s_memorySnapshotWritten{false};
    if (!s_memorySnapshotWritten.exchange(true, std::memory_order_acq_rel)) {
        SystemBridge::WriteGuestMemorySnapshot(out, (runDirectory / "mem1.bin").string().c_str());
    }

    out.flush();
    RT_LOG(RT_TAG_RUNTIME) << "crash artifacts written to " << runDirectory.string() << std::endl;
}

void SetRuntimeExitCodeImpl(int code) {
    g_lastExitCode.store(code, std::memory_order_relaxed);
    g_exitCodeSet.store(true, std::memory_order_relaxed);
}

} // namespace

namespace {

void DumpHostStackTrace() {
#if defined(_WIN32)
    static std::atomic_flag s_inProgress = ATOMIC_FLAG_INIT;
    if (s_inProgress.test_and_set()) {
        return;
    }
    const std::string trace = FormatHostStackTrace(1);
    std::fputs(trace.c_str(), stderr);
    std::fflush(stderr);
    s_inProgress.clear();
#else
    RT_LOGF(RT_TAG_RUNTIME, "Host stack trace unavailable on this platform\n");
    std::fflush(stderr);
#endif
}

} // namespace

extern "C" void DumpHostStackTraceForRuntimeHelper() {
    DumpHostStackTrace();
}

void MarkFatalErrorReported() {
    g_fatalErrorReported.store(true, std::memory_order_release);
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    if (g_fatalPopupShown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    try {
        std::string message;
        message.reserve(category.size() + details.size() + 220);
        message.append("The game stopped because ");
        message.append(category.empty() ? "a fatal error occurred." : category);
        message.append(".\n\n");
        if (details.empty()) {
            message.append("No additional details were available.");
        } else {
            constexpr size_t kMaxPopupDetails = 4096;
            message.append(details.data(), std::min(details.size(), kMaxPopupDetails));
            if (details.size() > kMaxPopupDetails) {
                message.append("\n\n[Additional details were written to the crash log.]");
            }
        }
        message.append("\n\nSee the WiiCompiled Logs folder for the full diagnostic.");
#if defined(_WIN32)
        ::MessageBoxA(nullptr, message.c_str(), "WiiCompiled - Fatal Error",
                      MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
#else
        // The shipped product is Windows-first. Keep non-Windows builds safe
        // and retain the console diagnostic when no native dialog is available.
        RT_LOGF(RT_TAG_RUNTIME, "fatal dialog: %s\n", message.c_str());
#endif
    } catch (...) {
        // Reporting a crash must never throw or mask the original failure.
    }
}

namespace RuntimeCrash {

void WriteCrashArtifacts(std::string_view reason, std::string_view extraDetails,
                         const uint32_t* missingGuestTarget) noexcept {
    try {
        WriteFatalLogImpl(reason, extraDetails, missingGuestTarget);
    } catch (...) {
        // Crash reporting must never mask the original failure.
    }
}

[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    RT_LOG(RT_TAG_RUNTIME) << "InvokeIndirectCpu: target 0x" << std::hex << target
              << " not translated (missing function)" << std::dec << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Caller LR = 0x" << std::hex << (cpu ? cpu->lr : 0u)
              << std::dec << std::endl;
    try {
        SystemBridge::DumpCrashHeuristics(std::cerr, cpu, &target);
        SystemBridge::DumpCpuState(cpu);
    } catch (...) {
    }
    std::fflush(stderr);

    std::ostringstream message;
    message << "The game stopped because it tried to execute guest address 0x"
            << std::hex << target
            << ", but that function was not translated or registered.\n\n"
            << "Caller LR: 0x" << (cpu ? cpu->lr : 0u);
    if (target == 0) {
        message << "\n\nA jump to address 0 usually means a virtual call through a bad "
                   "object pointer; the crash log heuristics have details.";
    }
    WriteCrashArtifacts("missing_target", message.str(), &target);
    ShowRuntimeFatalPopup("Missing translated function", message.str());
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

} // namespace RuntimeCrash


namespace {

void RuntimeAuroraLogCallback(AuroraLogLevel level, const char* module,
                              const char* message, unsigned int len) {
    const std::string_view moduleView = module != nullptr ? std::string_view(module) : std::string_view{};
    const std::string_view messageView = message != nullptr ? std::string_view(message, len) : std::string_view{};
    std::cerr << "[aurora] [" << static_cast<int>(level) << "] [" << moduleView << "] "
              << messageView << std::endl;
    if (level == LOG_FATAL) {
        ShowRuntimeFatalPopup("Aurora reported a fatal renderer error", messageView);
    }
}

const TranslatedFunctionInfo* ResolveEntry() {
    const auto* entry = TranslatedFunctionRegistry::FindByAddressPtr(kDefaultEntryAddress);
    if (!entry) {
        std::ostringstream oss;
        oss << "No translated function registered at address 0x" << std::hex << kDefaultEntryAddress;
        throw std::runtime_error(oss.str());
    }
    return entry;
}

void SeedCpuContext(CpuContext& cpu) {
    cpu.gpr[1] = 0x81700000u;
}

void DumpAccessViolationReport(const Memory::AccessViolation& ex,
                               std::string_view entryLabel) {
    const uint32_t address = ex.address();
    const size_t length = ex.length();
    const CpuContext* cpu = TryGetCpuContext();

    RT_LOG(RT_TAG_RUNTIME) << "===== Memory Access Violation =====" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Reason : " << ex.reason() << std::endl;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Address: 0x" << std::setw(8) << std::setfill('0') << address
              << " (+0x" << length << ")" << std::dec << std::setfill(' ') << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Entry  : " << (entryLabel.empty() ? "(unknown)" : std::string(entryLabel)) << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Mode   : strict (trap on unmapped)" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "r1 seed: 0x81700000" << std::endl;
    if (cpu) {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: " << cpu << "  r1=0x"
                  << std::hex << std::uppercase << cpu->gpr[1] << std::dec << std::nouppercase << std::endl;
        RT_LOG(RT_TAG_RUNTIME) << "Last recorded PC : 0x" << std::hex << std::uppercase << cpu->pc
                  << std::dec << std::nouppercase << std::endl;
    } else {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: (null)" << std::endl;
    }

    SystemBridge::DumpCpuState(cpu);

    const auto regions = Memory::DescribeRegions();
    if (regions.empty()) {
        RT_LOG(RT_TAG_RUNTIME) << "Memory not initialized; no regions mapped." << std::endl;
        return;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Mapped regions:" << std::endl;
    uint64_t bestDistance = std::numeric_limits<uint64_t>::max();
    std::string bestRegion;
    bool insideRegion = false;

    for (const auto& region : regions) {
        const uint64_t base = region.baseAddress;
        const uint64_t end = base + region.sizeBytes;
        const bool contains = address >= base && address < end;
        if (contains) {
            insideRegion = true;
            bestDistance = 0;
            bestRegion = region.name;
        } else {
            const uint64_t distance = address < base ? base - address : address - end + 1;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestRegion = region.name;
            }
        }

        std::cerr << "  - " << region.name
                  << " 0x" << std::hex << std::setw(8) << std::setfill('0') << region.baseAddress
                  << " .. 0x" << std::setw(8) << (end - 1)
                  << std::dec << std::setfill(' ')
                  << " (" << region.sizeBytes / 1024 << " KiB";
        if (contains) {
            std::cerr << ", <-- access landed here";
        }
        std::cerr << ")" << std::endl;
    }

    if (!bestRegion.empty() && !insideRegion) {
        RT_LOG(RT_TAG_RUNTIME) << "Nearest region: " << bestRegion << " (" << bestDistance << " bytes away)" << std::endl;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Verify that the installed game data and runtime build match." << std::endl;
}

#if defined(_WIN32)
PVOID g_vectoredSehHandle = nullptr;
constexpr DWORD kCppExceptionCodeGcc = 0x20474343; // "GCC" exception code
constexpr DWORD kCppExceptionCodeMsvc = 0xE06D7363;
// AddressSanitizer uses STATUS_FATAL_APP_EXIT when it detects an error and wants to report it.
// We must let ASan's handler run so it can print file/line information.
constexpr DWORD kAsanFatalAppExit = 0x40000015; // STATUS_FATAL_APP_EXIT

void ReportStructuredException(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        RT_LOG(RT_TAG_RUNTIME) << "Structured exception occurred, but no diagnostic info was captured." << std::endl;
        return;
    }

    const auto* record = info->ExceptionRecord;
    const auto code = record->ExceptionCode;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Structured exception 0x" << code;
    if (!g_lastEntryLabel.empty()) {
        std::cerr << " while executing " << g_lastEntryLabel;
    }
    std::cerr << std::dec << std::nouppercase << std::endl;

    const auto faultAddress = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Fault address: 0x" << faultAddress << std::dec << std::nouppercase << std::endl;

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto accessType = record->ExceptionInformation[0];
        const auto accessed = record->ExceptionInformation[1];
        RT_LOG(RT_TAG_RUNTIME) << "Access type: " << (accessType ? "write" : "read")
                  << " at 0x" << std::hex << std::uppercase << accessed << std::dec << std::nouppercase << std::endl;

        // Guardrail: flag raw GX gather pipe touches (0xCC00_8xxx) which must be routed through HLE.
        // Direct stores to this MMIO region (e.g., translated stfs/stb -0x8000(r4) with r4=0xCC010000)
        // will fault on the host. Emit an explicit hint so we know to fix the translation/HLE path instead
        // of chasing generic access violations.
        constexpr uintptr_t kGxGatherLo = 0xCC008000;
        constexpr uintptr_t kGxGatherHi = 0xCC009000; // one page past the 0x100-byte gather range for clarity
        if (accessed >= kGxGatherLo && accessed < kGxGatherHi) {
            RT_LOG(RT_TAG_RUNTIME) << "HINT: guest attempted a direct GX gather pipe write (0xCC00_8xxx)." << std::endl;
            RT_LOG(RT_TAG_RUNTIME) << "      These must go through GX_HLE_FIFO_Write*; check the translated function for" << std::endl;
            RT_LOG(RT_TAG_RUNTIME) << "      literal stores to -0x8000(r4) after lis r4,0xCC01 and route them via HLE." << std::endl;
        }
    }

#if defined(_M_X64) || defined(__x86_64__)
    const auto rip = info->ContextRecord ? info->ContextRecord->Rip : 0;
    const auto rsp = info->ContextRecord ? info->ContextRecord->Rsp : 0;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "RIP=0x" << rip << " RSP=0x" << rsp << std::dec << std::nouppercase << std::endl;
#elif defined(_M_IX86)
    const auto eip = info->ContextRecord ? info->ContextRecord->Eip : 0;
    const auto esp = info->ContextRecord ? info->ContextRecord->Esp : 0;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "EIP=0x" << eip << " ESP=0x" << esp << std::dec << std::nouppercase << std::endl;
#endif

    // Always dump CPU state on crash.
    RT_LOG(RT_TAG_RUNTIME) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    std::cerr.flush();

    RT_LOG(RT_TAG_RUNTIME) << "Enable /DEBUG builds or capture a dump for full stack details." << std::endl;
    std::cerr.flush();
}



LONG CALLBACK SehLogger(EXCEPTION_POINTERS* info) {
    // Guest-space faults are the flat memory interception mechanism (MMIO,
    // deferred EFB reads, the executable-write guard, unmapped pages). The
    // flat module registers its own handler first, but registration order is
    // not guaranteed once another VEH is installed later, so consult it here
    // too - resolving a fault twice is a no-op.
    if (GuestFlat::HandleAccessViolation(info)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_suppressSehReporting && g_sehJumpTarget) {
        g_sehLastExceptionCode = info->ExceptionRecord->ExceptionCode;
        g_sehLastExceptionAddress = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        g_sehLastAccessType = 0;
        g_sehLastAccessedAddress = 0;
        if (g_sehLastExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
            g_sehLastAccessType = static_cast<uint32_t>(info->ExceptionRecord->ExceptionInformation[0]);
            g_sehLastAccessedAddress = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        }
        longjmp(*g_sehJumpTarget, 1);
    }
    if (g_suppressSehReporting) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Let C++ exceptions propagate to std::terminate so we can log their what().
    if (info->ExceptionRecord->ExceptionCode == kCppExceptionCodeGcc ||
        info->ExceptionRecord->ExceptionCode == kCppExceptionCodeMsvc) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode == 0x40010006 || // DBG_PRINTEXCEPTION_C
        info->ExceptionRecord->ExceptionCode == 0x4001000A || // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
        info->ExceptionRecord->ExceptionCode == 0x406D1388 || // SetThreadName
        info->ExceptionRecord->ExceptionCode == kAsanFatalAppExit) { // ASan reporting - let it print first
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Guard against re-entrancy: if we crash while reporting, don't recurse
    static std::atomic_flag s_inCrashHandler = ATOMIC_FLAG_INIT;
    if (s_inCrashHandler.test_and_set()) {
        std::_Exit(EXIT_FAILURE);
    }

    // Report the structured exception with detailed information
    ReportStructuredException(info);
    const auto* record = info->ExceptionRecord;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    std::ostringstream popupDetails;
    popupDetails << "A native Windows exception (0x" << std::hex << std::uppercase << code << ") occurred";
    if (!g_lastEntryLabel.empty()) {
        popupDetails << " while executing " << g_lastEntryLabel;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        popupDetails << ".\n\nThe game attempted a "
                     << (record->ExceptionInformation[0] ? "write" : "read")
                     << " at host address 0x" << record->ExceptionInformation[1];
    }
    popupDetails << ".\n\nThe process transcript and crash log contain the full CPU and stack diagnostics.";
    ShowRuntimeFatalPopup("a native crash occurred", popupDetails.str());
    DumpHostStackTrace();

    WriteFatalLogImpl("seh");

    // CRITICAL: Explicitly flush all output to ensure visibility with PowerShell redirection
    std::cerr << '\n';
    RT_LOG(RT_TAG_RUNTIME) << "===== FLUSHING OUTPUT BEFORE EXIT =====" << std::endl;
    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);

    std::_Exit(EXIT_FAILURE);
}

void InstallSehLogger() {
    if (!g_vectoredSehHandle) {
        g_vectoredSehHandle = AddVectoredExceptionHandler(1, SehLogger);
    }
}
#endif

void AbortSignalHandler(int signum) {
    // Guard against re-entrancy if multiple aborts are raised in quick succession
    if (g_abortSignalHandled.test_and_set()) {
        std::_Exit(EXIT_FAILURE);
    }

    // abort() bypasses atexit, so the guest-memory fault summary has to be
    // emitted here too. It is idempotent, so a later AtExitHandler is a no-op.
    GuestFlat::LogFaultSummary();

    ShowRuntimeFatalPopup("a fatal internal error occurred",
                          "The process called abort while running the game or Aurora renderer.\n\n"
                          "This usually means an unimplemented function, failed renderer assertion, "
                          "or another unrecoverable runtime condition was reached.");

    // Skip detailed dump if already reported by another handler
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(EXIT_FAILURE);
    }

    WriteFatalLogImpl("sigabrt");

    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(EXIT_FAILURE);
}

} // namespace

// Retained as the thin public wrapper over WriteFatalLogImpl (declared in
// system_bridge.h); it has no in-tree callers because every fatal path now goes
// through RuntimeCrash::WriteCrashArtifacts.
void WriteFatalLog(std::string_view reason) {
    WriteFatalLogImpl(reason);
}

void SetRuntimeExitCode(int code) {
    SetRuntimeExitCodeImpl(code);
}

// Global handler called via atexit() to flush buffers before any exit
static void AtExitHandler() {
    // End-of-run guest memory report. This runs before the fatal-report check
    // below because the counters describe the whole session and are just as
    // interesting after a crash as after a clean exit.
    GuestFlat::LogFaultSummary();

    // Skip if already reported by another handler
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        return;
    }
    if (g_exitCodeSet.load(std::memory_order_relaxed) &&
        g_lastExitCode.load(std::memory_order_relaxed) != 0) {
        ShowRuntimeFatalPopup("the runtime exited with an error",
                              "The game stopped after reporting a fatal error. Check the process transcript and crash log for details.");
        WriteFatalLogImpl("exitcode");
    }

    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
}

// Global terminate handler for uncaught exceptions
static void TerminateHandler() {
    // Skip detailed dump if already reported
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    std::string terminateDetails;
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            terminateDetails = std::string("Unhandled C++ exception: ") + e.what();
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled C++ exception: " << e.what() << std::endl;
        } catch (...) {
            terminateDetails = "Unhandled non-std C++ exception.";
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled non-std C++ exception." << std::endl;
        }
    } else {
        terminateDetails = "std::terminate() without current exception.";
    }
    ShowRuntimeFatalPopup("an unhandled C++ exception occurred", terminateDetails);
    const std::string hostStackSummary = FormatHostStackTrace(1);
    if (!hostStackSummary.empty()) {
        terminateDetails.append("\n");
        terminateDetails.append(hostStackSummary);
    }
    WriteFatalLogImpl("terminate", terminateDetails);
    RT_LOG(RT_TAG_RUNTIME) << "std::terminate() called - program exiting" << std::endl;
    std::cerr.flush();
    DumpHostStackTrace();
    if (const CpuContext* cpu = TryGetCpuContext()) {
        RT_LOG(RT_TAG_RUNTIME) << "CPU state at terminate:" << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

int RuntimeMain(int argc, char** argv) {
    // Must run before the transcript duplicates stdout/stderr: it decides what
    // those descriptors are mirrored to now that the products are GUI-subsystem.
    AttachParentConsoleForDiagnostics();
#if defined(_WIN32)
    ConfigureWindowsFatalDialogBehavior();
    InstallSehLogger();
    WindowsTimerResolutionGuard timerResolutionGuard;
#endif
    InitializeProcessTranscript(argc, argv);
#if defined(_WIN32)
    // Must run after transcript init, or its diagnostics have no console.log to land in yet.
    PatchDawnVulkanLoaderForWine();
#endif
    std::signal(SIGABRT, AbortSignalHandler);
    // Install exit/terminate handlers to ensure we get crash info
    std::atexit(AtExitHandler);
    std::set_terminate(TerminateHandler);

    std::string currentEntryLabel;

    try {
        if (argc != 1) {
            throw std::invalid_argument("The game runtime does not accept command-line options; use Config.toml through the installed host.");
        }
        RuntimeConfigFile::LogLoadedConfig();
        SystemBridge::Initialize();
        TranslatedFunctionRegistry::Finalize();

        // Initialize Aurora (graphics backend)
        // We use auto backend (or specific if needed) and set a default window size.
        // This is required for GX commands (like texture loading) to work.
        AuroraConfig auroraConfig = {};
        auroraConfig.appName = RuntimeProduct::Active().displayName.data();
        const auto applicationDataDirectory = RuntimeConfigFile::ApplicationDataDirectory();
        const auto rendererCacheDirectory = applicationDataDirectory / "Cache";
        std::error_code rendererPathError;
        std::filesystem::create_directories(rendererCacheDirectory, rendererPathError);
        if (rendererPathError) {
            RT_LOG(RT_TAG_RUNTIME) << "Unable to create renderer cache directory "
                      << rendererCacheDirectory << ": " << rendererPathError.message() << std::endl;
        }
        const std::string auroraUserPath = applicationDataDirectory.string();
        const std::string auroraCachePath = rendererCacheDirectory.string();
        auroraConfig.userPath = auroraUserPath.c_str();
        auroraConfig.cachePath = auroraCachePath.c_str();
        auroraConfig.logCallback = &RuntimeAuroraLogCallback;
        auroraConfig.logLevel = LOG_DEBUG;
        const bool configWidescreen = RuntimeConfigFile::WidescreenEnabled(true);
        auroraConfig.windowWidth = configWidescreen ? 854 : 640;
        auroraConfig.windowHeight = 480;
        auroraConfig.windowWidth = RuntimeConfigFile::WindowWidth(auroraConfig.windowWidth);
        auroraConfig.windowHeight = RuntimeConfigFile::WindowHeight(auroraConfig.windowHeight);
        auroraConfig.hasWindowPosition = RuntimeConfigFile::WindowPosition(
            auroraConfig.windowPosX, auroraConfig.windowPosY);
        auroraConfig.allowJoystickBackgroundEvents = true;
        auroraConfig.disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
        // Dolphin-style custom textures. Aurora indexes <userPath>/texture_replacements
        // once during aurora_initialize, so both knobs only take effect on the next launch.
        // Dumps name each unmatched texture the way its replacement would have to be named,
        // which is only useful while the index is live - hence the conjunction.
        auroraConfig.allowTextureReplacements = RuntimeConfigFile::TextureReplacements(false);
        auroraConfig.allowTextureDumps = auroraConfig.allowTextureReplacements &&
                                         RuntimeConfigFile::TextureDumps(false);
        // No vsync knob: aurora always configures a non-blocking present mode.
        auroraConfig.desiredBackend = BACKEND_AUTO;
        const float resolutionMultiplier = RuntimeConfigFile::ResolutionMultiplier(1.0f);
        ConfigureMkwDynamicAspect(configWidescreen, auroraConfig.windowWidth, auroraConfig.windowHeight);
        VISetFrameBufferScale(resolutionMultiplier);
        // One table for both directions. RuntimeConfigFile::IsSupportedGraphicsApi
        // whitelists exactly these config names, so an unrecognised value has
        // already been rejected (and reported) at parse time.
        struct GraphicsBackendEntry {
            const char* configName;
            AuroraBackend backend;
        };
        static constexpr std::array<GraphicsBackendEntry, 3> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"d3d12", BACKEND_D3D12}, {"vulkan", BACKEND_VULKAN},
        }};
        const auto backendDisplayName = [](AuroraBackend value) -> const char* {
            for (const auto& entry : kGraphicsBackends) {
                if (entry.backend == value) {
                    return entry.configName;
                }
            }
            return "unknown";
        };

        const std::string backend = RuntimeConfigFile::GraphicsApi("auto");
        AuroraBackend configuredBackend = BACKEND_AUTO;
        for (const auto& entry : kGraphicsBackends) {
            if (backend == entry.configName) {
                configuredBackend = entry.backend;
                break;
            }
        }
        // Vulkan is preferred under Wine/Proton, but explicitly configuring "d3d12" still opts back into it,
        // Windows is unaffected: "auto" still tries D3D12 first there
        auroraConfig.desiredBackend =
            (configuredBackend == BACKEND_AUTO && RuntimeHostPlatform::IsRunningUnderWine())
                ? BACKEND_VULKAN
                : configuredBackend;
        const AuroraBackend requestedBackend = configuredBackend;

        const AuroraInfo auroraInfo = aurora_initialize(0, nullptr, &auroraConfig);
        if (requestedBackend != BACKEND_AUTO && auroraInfo.backend != requestedBackend) {
            RT_LOG(RT_TAG_RUNTIME) << "graphics_api=\"" << backend
                      << "\" is not available on this system; aurora fell back to \""
                      << backendDisplayName(auroraInfo.backend)
                      << "\". See the [aurora::gpu] lines above for the reason." << std::endl;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "graphics backend: " << backendDisplayName(auroraInfo.backend)
                      << std::endl;
        }
        aurora_set_frame_worker_wait_callback(ServiceGuestTimingDuringAuroraFrameWait);
        GxGuestWrite::InstallAuroraHooks();
        UpdateMkwDynamicAspectSurface(auroraInfo.windowSize.native_fb_width,
                                      auroraInfo.windowSize.native_fb_height);
        settings_overlay::InitializeRuntimeSettings();
        RT_LOG(RT_TAG_CONFIG) << "video.widescreen=" << (configWidescreen ? "true" : "false")
                  << " SCGetAspectRatio=" << (configWidescreen ? 1 : 0)
                  << " resolutionMultiplier=" << resolutionMultiplier
                  << " window=" << auroraInfo.windowSize.width << "x" << auroraInfo.windowSize.height
                  << " native=" << auroraInfo.windowSize.native_fb_width << "x"
                  << auroraInfo.windowSize.native_fb_height
                  << " viewportPolicy=" << (g_dynamicAspectRatioEnabled ? "stretch" : "fit")
                  << " presentAspect="
                  << (g_dynamicAspectRatioEnabled ? "surface (dynamic EGG canvas)" : "4:3")
                  << std::endl;
        g_auroraInitialized.store(true, std::memory_order_release);

        auto entry = ResolveEntry();
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        SeedCpuContext(cpu);

        // Initialize the fiber-based threading system
        Fiber::GuestFiberManager::Initialize();

        CpuContextScope cpuScope(&cpu);

        std::string label = entry->name;
        if (label.empty()) {
            std::ostringstream oss;
            oss << "0x" << std::hex << entry->address;
            label = oss.str();
        }
        currentEntryLabel = label;
        g_lastEntryLabel = currentEntryLabel;

        InvokeIndirectCpu(entry->address, &cpu);
        const uint32_t result = cpu.gpr[3];
        RT_LOG(RT_TAG_RUNTIME) << label << " => 0x" << std::hex << result << std::dec << " (" << result << ")" << std::endl;

        // Shutdown fiber system
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        SetRuntimeExitCodeImpl(0);
        ShutdownProcessTranscript();
        return 0;
    } catch (const Memory::AccessViolation& ex) {
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        DumpAccessViolationReport(ex, currentEntryLabel);
        std::ostringstream details;
        details << "addr=0x" << std::hex << std::uppercase << ex.address()
                << " len=0x" << ex.length()
                << std::dec << std::nouppercase
                << " reason=" << ex.reason();
        ShowRuntimeFatalPopup("a guest memory access was out of bounds", details.str());
        WriteFatalLogImpl("access_violation", details.str());
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        ShutdownProcessTranscript();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        SystemBridge::DumpCpuState(TryGetCpuContext());
        ShowRuntimeFatalPopup("a runtime exception occurred", ex.what());
        WriteFatalLogImpl("exception");
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        ShutdownProcessTranscript();
        return 1;
    }
}

int main(int argc, char** argv) {
    return RuntimeMain(argc, argv);
}
extern "C" bool g_dynamicAspectRatioEnabled = false;
