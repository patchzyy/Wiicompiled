using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Tests;

internal static class TranslatorCppTestHarness
{
    private static readonly string[] LeanRuntimeSources =
    {
        Path.Combine("runtime", "src", "abi_bridge.cpp"),
        Path.Combine("runtime", "src", "fpu_helpers.cpp"),
        Path.Combine("runtime", "src", "memory.cpp"),
        Path.Combine("runtime", "src", "ppc_helpers.cpp"),
        Path.Combine("runtime", "src", "ppc_quantized.cpp"),
    };

    public static string BuildCompileArguments(
        string repoRoot,
        string tempRoot,
        IEnumerable<string> generatedFiles,
        string harnessPath,
        string outputPath,
        bool includeDataSections = true)
    {
        var supportSource = EnsureSupportSource(tempRoot);
        var runtimeSources = LeanRuntimeSources.Select(path => Path.Combine(repoRoot, path));
        var args = new StringBuilder();
        args.Append("-std=c++17 ");
        args.Append("-D_CRT_SECURE_NO_WARNINGS ");
        args.Append("-march=x86-64-v3 ");
        args.Append("-fno-fast-math -ffp-contract=off ");
        if (!RuntimeHeadersDefineRestrictMacro(repoRoot))
        {
            // Fallback for the window between an emitter change using MKW_RESTRICT and the runtime
            // headers defining it. Stops firing once headers catch up, so it can't hide a real regression.
            args.Append("-DMKW_RESTRICT=__restrict ");
        }
        args.Append("-Wno-error=unused-but-set-variable ");
        args.Append(string.Join(' ', runtimeSources.Select(Quote))).Append(' ');
        args.Append(Quote(supportSource)).Append(' ');
        args.Append(string.Join(' ', generatedFiles.Select(Quote))).Append(' ');

        if (includeDataSections)
        {
            var dataSectionsInit = Path.Combine(repoRoot, "generated", "data_sections_init.cpp");
            if (File.Exists(dataSectionsInit))
            {
                args.Append(Quote(dataSectionsInit)).Append(' ');
            }

            var dataSectionsBlobs = Path.Combine(repoRoot, "generated", "data_sections_init_blobs.S");
            if (File.Exists(dataSectionsBlobs))
            {
                args.Append(Quote(dataSectionsBlobs)).Append(' ');
            }
        }

        args.Append(Quote(harnessPath)).Append(' ');
        args.Append($"-I{Quote(Path.Combine(repoRoot, "runtime", "include"))} ");
        args.Append($"-I{Quote(Path.Combine(repoRoot, "generated"))} ");
        args.Append($"-o {Quote(outputPath)}");
        return args.ToString();
    }

    private static bool RuntimeHeadersDefineRestrictMacro(string repoRoot)
    {
        var includeDirectory = Path.Combine(repoRoot, "runtime", "include");
        if (!Directory.Exists(includeDirectory))
        {
            return false;
        }

        foreach (var header in Directory.EnumerateFiles(includeDirectory, "*.h", SearchOption.AllDirectories))
        {
            if (File.ReadAllText(header).Contains("define MKW_RESTRICT", StringComparison.Ordinal))
            {
                return true;
            }
        }

        return false;
    }

    public static ProgramImage CreateImage(params (uint Address, uint Value)[] words)
    {
        var maxOffset = 0;
        foreach (var (address, _) in words)
        {
            var offset = checked((int)(address - MemoryLayout.RamBase));
            maxOffset = Math.Max(maxOffset, offset + 4);
        }

        var memory = new byte[Math.Max(maxOffset, 4)];
        foreach (var (address, value) in words)
        {
            var offset = checked((int)(address - MemoryLayout.RamBase));
            memory[offset + 0] = (byte)(value >> 24);
            memory[offset + 1] = (byte)(value >> 16);
            memory[offset + 2] = (byte)(value >> 8);
            memory[offset + 3] = (byte)value;
        }

        return new ProgramImage(
            memory,
            new AddressRange(MemoryLayout.RamBase, checked(MemoryLayout.RamBase + (uint)memory.Length)),
            new AddressRange(MemoryLayout.RamBase, checked(MemoryLayout.RamBase + (uint)memory.Length)),
            new AddressRange(0, 0),
            sha256: "test");
    }

    private static string EnsureSupportSource(string tempRoot)
    {
        Directory.CreateDirectory(tempRoot);
        var path = Path.Combine(tempRoot, "translator_test_support.cpp");
        const string contents = """
            #include "system_bridge.h"
            #include "ppc_runtime.h"
            #include "recomp_mod_loader.h"

            #include <csetjmp>
            #include <atomic>
            #include <cstddef>
            #include <cstdint>
            #include <cstdlib>
            #include <ostream>
            #include <string_view>

            bool g_suppressSehReporting = false;
            thread_local jmp_buf* g_sehJumpTarget = nullptr;
            thread_local uint32_t g_sehLastExceptionCode = 0;
            thread_local uintptr_t g_sehLastExceptionAddress = 0;
            thread_local uintptr_t g_sehLastAccessedAddress = 0;
            thread_local uint32_t g_sehLastAccessType = 0;

            void DebugTrackPcImpl(uint32_t) {}
            void DebugPumpAurora() {}
            #if defined(__GNUC__) || defined(__clang__)
            extern "C" void DumpRecentPcTrace(size_t) __attribute__((weak));
            void MarkFatalErrorReported() __attribute__((weak));
            #endif
            extern "C" void DumpRecentPcTrace(size_t) {}
            void MarkFatalErrorReported() {}
            extern "C" void DumpHostStackTraceForRuntimeHelper() {}
            extern "C" void OS_HLE_ProcessAlarms(int) {}
            void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept
            {
                std::abort();
            }

            namespace RecompMod {
            std::atomic<bool> g_executableWriteGuardEnabled{false};
            std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
            std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
            std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};
            ScopedTranslatedExecutionAddress::ScopedTranslatedExecutionAddress(uint32_t) noexcept {}
            ScopedTranslatedExecutionAddress::~ScopedTranslatedExecutionAddress() noexcept {}
            uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
            bool HandleExecutableWrite(uint32_t, size_t, uint64_t) { return false; }
            void CheckExecutableWrite(uint32_t, size_t, uint64_t) {}
            }

            RuntimeOptions SystemBridge::ParseCommandLine(int, char**) { return {}; }
            void SystemBridge::Initialize(const RuntimeOptions&) {}
            void SystemBridge::DumpRegisteredFunctions() {}
            void SystemBridge::DumpMemoryLayout() {}
            void SystemBridge::PrintUsage() {}
            void SystemBridge::DumpCpuState(const CpuContext*) {}
            void SystemBridge::DumpCpuState(std::ostream&, const CpuContext*) {}
            """;

        if (!File.Exists(path) || !string.Equals(File.ReadAllText(path), contents, StringComparison.Ordinal))
        {
            File.WriteAllText(path, contents);
        }

        return path;
    }

    private static string Quote(string path) => $"\"{path}\"";
}
