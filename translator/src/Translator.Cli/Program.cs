using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Translator.Cli.Configuration;
using Translator.Core;
using Translator.Core.CodeGen;
using Translator.Core.Build;
using Translator.Core.Analysis;
using Translator.Core.Analysis.BasicBlocks;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Mods;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Ir;
using Translator.Core.IO;
using Translator.Core.Parsing.Kamek;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;
using Translator.Core.Translation;

// All formats here (YAML, JSON, hex, hashes) are culture-invariant. Under Turkish locale, YamlDotNet's
// UnderscoredNamingConvention lowercased "Id" to dotless "ıd", breaking the "id" key match. Pin globally.
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

var command = args.Length > 0 ? args[0] : "info";
var (projectOption, tailWithoutProject) = ExtractOption(args.Skip(1).ToArray(), "--project");
var (profileOption, tail) = ExtractOption(tailWithoutProject, "--profile");
var project = !string.IsNullOrWhiteSpace(projectOption)
    ? TranslationProjectConfig.Load(projectOption)
    : null;
var profile = project?.GetProfile(profileOption);
if (profile is not null)
{
    project!.ValidateProfile(profile);
}
// The function map is the project's function-start oracle. Loading it here
// also publishes the CodeWarrior save/restore thunk ranges, which three static
// passes (guest ABI contracts, inline-thunk emission, inferred ABI) look up by
// symbol name instead of carrying the same four literal addresses each.
var functionMap = project?.Translation.FunctionMapPath is { } functionMapPath
    ? FunctionMap.Load(functionMapPath)
    : null;
if (functionMap is not null)
{
    GuestSaveRestoreThunks.Current = GuestSaveRestoreThunks.FromFunctionMap(functionMap);
}
// The runtime's native registrations are spelled as PAL identities; a non-PAL project supplies
// the region table that turns them into this executable's addresses before any source scan.
if (project?.Runtime.GuestAddressTablePath is { } guestAddressTablePath)
{
    GuestAddressTable.Current = GuestAddressTable.Load(guestAddressTablePath);
}
var root = project?.WorkspaceRoot ?? Directory.GetCurrentDirectory();
var preferCachedInputs = HasFlag(tail, "--prefer-cached-inputs");
var dolFile = new Lazy<DolFile>(LoadDol);
var relFile = new Lazy<RelFile?>(LoadRel);
var image = new Lazy<ProgramImage>(LoadImage);
var canonicalIrStore = new CanonicalIrStore();
// Hoisted out of the translator factory so the base pipeline can prewarm the
// recursive ABI cache in one stable pass, exactly like translate-mod does.
var inferredGuestAbi = new Lazy<InferredGuestFunctionAbiProvider>(() =>
    new InferredGuestFunctionAbiProvider(image.Value, canonicalIrStore));
var runtimeNativeIndex = new Lazy<RuntimeNativeIndex>(() =>
    RuntimeNativeIndexBuilder.Build(RequireProject().Runtime.NativeRegistrationRoot));
// Void-stub signatures are only trusted as declared guest ABIs when the stub
// lives under runtime.native_abi_directories (the vetted set); everywhere else
// the C++ signature is documentation and the inferred ABI stays authoritative.
var declaredNativeAbi = new Lazy<RuntimeNativeFunctionAbiProvider?>(() =>
{
    var runtime = RequireProject().Runtime;
    if (runtime.NativeAbiDirectories.Count == 0)
        return null;
    var root = Path.GetFullPath(runtime.NativeRegistrationRoot);
    var prefixes = runtime.NativeAbiDirectories
        .Select(directory =>
        {
            var relative = Path.GetRelativePath(root, Path.GetFullPath(directory)).Replace('\\', '/');
            return relative == "." ? "" : relative.TrimEnd('/') + "/";
        })
        .ToArray();
    var scopedAddresses = runtimeNativeIndex.Value.Registrations
        .Where(registration => prefixes.Any(prefix =>
            registration.SourceFile.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)))
        .Select(static registration => registration.Address)
        .ToHashSet();
    return RuntimeNativeFunctionAbiProvider.FromIndex(runtimeNativeIndex.Value, scopedAddresses);
});
var translator = new Lazy<FunctionTranslator>(() =>
{
    var inferredAbi = inferredGuestAbi.Value;
    var providers = new List<IGuestFunctionAbiProvider>();
    if (declaredNativeAbi.Value is { } nativeAbi)
        providers.Add(nativeAbi);
    providers.Add(inferredAbi);
    var abiProvider = new CompositeGuestFunctionAbiProvider(providers.ToArray());
    return new FunctionTranslator(image.Value, abiProvider);
});
// The runtime source index is built lazily once and shared by every consumer,
// so ABI/effect analysis never rescans the C++ within one translator run.
var runtimeNativeGuestEffects = new Lazy<RuntimeNativeGuestEffectSet>(() =>
    runtimeNativeIndex.Value.ToGuestEffectSet());
var baseTranslationExclusions = new Lazy<IReadOnlySet<uint>>(() =>
    runtimeNativeIndex.Value.Registrations
        .Where(static registration => registration.ExcludesBaseTranslation)
        .Select(static registration => registration.Address)
        .ToHashSet());
// The base translation is shared across products, so mod-safety decisions must cover every enabled
// profile's patch set, not just --profile. The launcher reuses one profile-less base for both vanilla
// and Retro Rewind; keying on --profile alone let mod-patched callees get leaf-inlined into base
// callers, baking vanilla code into the modded product (character-select corruption, LinkList panics).
var modPatchProfiles = new Lazy<IReadOnlyList<Translator.Cli.Configuration.ProjectProfile>>(
    () => CollectModPatchProfiles(project, profile));
var modPatchedAddresses = new Lazy<IReadOnlySet<uint>>(() =>
    BuildModPatchedAddresses(modPatchProfiles.Value));
var leafInliningBlockedTargets = new Lazy<IReadOnlySet<uint>>(() =>
    BuildLeafInliningBlockedTargets(
        modPatchedAddresses.Value, runtimeNativeGuestEffects.Value, baseTranslationExclusions.Value));

// Leaf inlining is a front-end decision: the splice happens before SSA, so the
// canonical graph the emission wave lowers is already inlined and every
// discovery entry point has to agree on it. The emission-time counterpart is
// WithProjectCodegenPolicy.
TranslationOptions WithProjectFrontEndPolicy(
    TranslationOptions options, ProjectTranslation translation) =>
    options with
    {
        AllowUnsupportedInstructions = translation.AllowUnsupportedInstructions,
        // Splicing a body whose runtime winner is a native registration, a
        // dropped translation or a Kamek overlay would silently keep executing
        // the original bytes.
        LeafInliningBlockedTargets = leafInliningBlockedTargets.Value
    };
string? translateModInputCacheDirectory = null;
string? translateModPublishedOutputDirectory = null;
List<AddressRange>? _executableRanges = null;
// Callback discovery now runs on the discovery worker pool, so the range cache
// must be built by exactly one thread and published only once it is complete.
var _executableRangesGate = new object();
// Argument registers checked for callback function pointers at every call site
var argumentRegisters = new[] { "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10" };

// Argument validation runs before any command can execute. `--help` used to
// fall straight through to the command's parser, which ignored every argument
// it did not recognise, so `emit-build-shards --help` ran the command with its
// defaults and rewrote generated/build_shards.
if (HandleCommandLineOptions(command, tail) is { } commandLineExitCode)
{
    return commandLineExitCode;
}

return command switch
{
    "help" or "--help" or "-h" or "-?" => ShowGlobalHelp(),
    "info" or "--info" or "--version" => RunInfo(),
    "generate-data-init" => RunGenerateDataInit(),
    "translate-recursive" => RunTranslateRecursive(tail),
    "translate-mod" => RunTranslateMod(tail),
    "emit-build-shards" => RunEmitBuildShards(tail),
    "emit-base-manifest" => RunEmitBaseManifest(tail),
    "check-base-mod-awareness" => RunCheckBaseModAwareness(tail),
    _ => ShowHelp(command)
};

/// <summary>
/// May the workspace's completed base translation be reused for the Code.pul about to build?
/// Exit 0 = yes, 3 = no, anything else = error (also treat as no). Answering wrong in the permissive
/// direction bakes vanilla code into a modded product, so every uncertainty exits non-zero.
/// </summary>
int RunCheckBaseModAwareness(string[] argsTail)
{
    var metadataPath = OptionValue(argsTail, "--translation-output-metadata")
        ?? Path.Combine(root, "generated", "base_translation_output.json");
    var codePul = OptionValue(argsTail, "--code-pul") ?? profile?.CodePul;
    if (string.IsNullOrWhiteSpace(codePul))
    {
        Console.Error.WriteLine("[translator] check-base-mod-awareness needs --code-pul or a profile that has one.");
        return 1;
    }
    if (profile is null)
    {
        Console.Error.WriteLine("[translator] check-base-mod-awareness needs --profile.");
        return 1;
    }
    if (!File.Exists(codePul))
    {
        Console.Error.WriteLine($"[translator] Code.pul not found: {codePul}");
        return 1;
    }

    var awarenessPath = Path.Combine(
        Path.GetDirectoryName(Path.GetFullPath(metadataPath))!, BaseTranslationModAwareness.FileName);
    var awareness = BaseTranslationModAwarenessFile.TryRead(awarenessPath);
    if (awareness is null)
    {
        Console.WriteLine(
            $"[translator] No usable mod-patch awareness detail beside {metadataPath}; the base must retranslate.");
        return 3;
    }
    if (!awareness.CoversCodePul(profile.Name, codePul, out var reason))
    {
        Console.WriteLine($"[translator] The base translation cannot be reused for this Code.pul: {reason}.");
        return 3;
    }
    Console.WriteLine(
        "[translator] This Code.pul patches exactly the translated functions the base translation was " +
        "built around; the completed base translation stays valid.");
    return 0;
}

int RunEmitBuildShards(string[] argsTail)
{
    var baseMetadata = OptionValue(argsTail, "--base-metadata") ?? Path.Combine(root, "generated", "base_translation_output.json");
    var baseFunctions = OptionValue(argsTail, "--base-functions-dir") ?? Path.Combine(root, "generated", "functions");
    var output = OptionValue(argsTail, "--out") ?? Path.Combine(root, "generated", "build_shards");
    var nativeSources = OptionValue(argsTail, "--native-source-dir") ?? Path.Combine(root, "runtime", "src");
    var resolvedProfile = OptionValue(argsTail, "--resolved-profile");
    var retroCpp = OptionValue(argsTail, "--retro-cpp-dir");

    try
    {
        var nativeSourcePath = Path.GetFullPath(nativeSources);
        var nativeIndex = RuntimeNativeIndexBuilder.Build(nativeSourcePath);
        var result = TranslatedBuildShardEmitter.Emit(new TranslatedBuildShardOptions(
            Path.GetFullPath(baseMetadata),
            Path.GetFullPath(baseFunctions),
            Path.GetFullPath(output),
            nativeSourcePath,
            string.IsNullOrWhiteSpace(resolvedProfile) ? null : Path.GetFullPath(resolvedProfile),
            string.IsNullOrWhiteSpace(retroCpp) ? null : Path.GetFullPath(retroCpp),
            NativeIndex: nativeIndex));
        Console.WriteLine("[translator] emitted stable translated build graph");
        Console.WriteLine($"  base functions: {result.BaseFunctionCount:N0}");
        Console.WriteLine($"  shared base functions: {result.SharedBaseFunctionCount:N0}");
        Console.WriteLine($"  profile-sensitive targets/callers: {result.ProfileSensitiveTargetCount:N0}/{result.ProfileSensitiveCallerCount:N0}");
        Console.WriteLine($"  mod functions: {result.ModFunctionCount:N0}");
        Console.WriteLine($"  base/mod shards: {result.BaseShardCount:N0}/{result.ModShardCount:N0}");
        if (result.BaseCommonShardMapPath is { } shardMapPath)
        {
            Console.WriteLine(result.BaseCommonBoundariesReused
                ? $"  base_common boundaries: reused frozen table {shardMapPath}"
                : $"  base_common boundaries: packed and recorded {shardMapPath}");
        }
        if (result.BaseCommonBalance is { } balance)
        {
            // Frozen membership drifts as new functions are translated into
            // existing ranges. Surfacing the spread is what makes that visible
            // before it starts costing build throughput.
            Console.WriteLine(
                $"  base_common weight spread: {balance.ShardCount:N0} shards, " +
                $"min {balance.MinCompileCostWeight:N0}, mean {balance.MeanCompileCostWeight:N0}, " +
                $"max {balance.MaxCompileCostWeight:N0} (max/min {balance.SpreadRatio:F2}x)");
        }
        Console.WriteLine($"  CMake graph: {result.CMakeManifestPath}");
        return 0;
    }
    catch (Exception ex) when (ex is IOException or InvalidDataException or ArgumentException)
    {
        Console.Error.WriteLine($"emit-build-shards failed: {ex.Message}");
        return 1;
    }
}

byte[] LoadBinaryInput(string spec, string outDir, string downloadFileName)
{
    if (Uri.TryCreate(spec, UriKind.Absolute, out var uri) &&
        (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps))
    {
        Directory.CreateDirectory(outDir);
        var downloadPath = Path.Combine(outDir, downloadFileName);
        var cachedPath = ResolveCachedTranslateModInput(downloadPath, downloadFileName, spec);
        if (preferCachedInputs && cachedPath is not null)
        {
            Console.Error.WriteLine($"[translator] using cached input {cachedPath}");
            var cachedBytes = File.ReadAllBytes(cachedPath);
            if (!string.Equals(cachedPath, downloadPath, StringComparison.OrdinalIgnoreCase))
            {
                File.WriteAllBytes(downloadPath, cachedBytes);
                File.WriteAllText(downloadPath + ".source", spec);
            }
            return cachedBytes;
        }
        Console.Error.WriteLine($"[translator] fetching {uri}");
        using var client = new System.Net.Http.HttpClient();
        var bytes = client.GetByteArrayAsync(uri).GetAwaiter().GetResult();
        File.WriteAllBytes(downloadPath, bytes);
        File.WriteAllText(downloadPath + ".source", spec);
        return bytes;
    }

    return File.ReadAllBytes(Path.GetFullPath(Path.Combine(root, spec)));
}

string? ResolveCachedTranslateModInput(string outputPath, string fileName, string spec)
{
    if (CachedTranslateModInputMatchesSpec(outputPath, spec))
    {
        return outputPath;
    }

    if (!string.IsNullOrWhiteSpace(translateModInputCacheDirectory))
    {
        var priorPublishedPath = Path.Combine(translateModInputCacheDirectory, fileName);
        if (CachedTranslateModInputMatchesSpec(priorPublishedPath, spec))
        {
            return priorPublishedPath;
        }
    }

    return null;
}

bool CachedTranslateModInputMatchesSpec(string path, string spec)
{
    // A cached download is only valid for the spec that produced it; the
    // sidecar records that spec so a changed profile URL forces a re-fetch
    // instead of silently reusing bytes from the previous endpoint.
    var sourcePath = path + ".source";
    return File.Exists(path) &&
        File.Exists(sourcePath) &&
        string.Equals(File.ReadAllText(sourcePath).Trim(), spec.Trim(), StringComparison.Ordinal);
}

static uint AlignUp(uint value, uint alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    {
        throw new ArgumentException("Alignment must be a non-zero power of two.", nameof(alignment));
    }
    return checked((value + alignment - 1u) & ~(alignment - 1u));
}

int RunInfo()
{
    var asm = Assembly.GetExecutingAssembly();
    var version = asm.GetName().Version?.ToString() ?? "0.0.0";
    Console.WriteLine($"PowerPC static recompiler (C#)\nVersion : {version}\nRuntime : {System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription}");
    if (project is not null)
    {
        Console.WriteLine($"Project : {project.Identity.DisplayName} ({project.Identity.Id})");
        Console.WriteLine($"Memory  : 0x{project.Memory.Base:X8}+0x{project.Memory.Size:X}");
        Console.WriteLine($"DOL     : {project.Inputs.Dol.Path}");
        Console.WriteLine(project.Inputs.Rel is null
            ? "REL     : none"
            : $"REL     : {project.Inputs.Rel.Path} @ 0x{project.Inputs.Rel.LoadAddress:X8}");
    }
    return 0;
}

int RunTranslateRecursive(string[] argsTail)
{
    if (argsTail.Length == 0 || argsTail[0].StartsWith("--", StringComparison.Ordinal))
    {
        Console.Error.WriteLine("Usage: translator translate-recursive <start_addr> [--outdir path] [--output-metadata path] [--threads N] [--prune-stale]");
        return 1;
    }

    var startAddr = ParseAddress(argsTail[0]);
    var allowUnsupportedInstructions = RequireProject().Translation.AllowUnsupportedInstructions;
    var outDir = OptionValue(argsTail, "--outdir") ?? RequireProject().Output.Functions;
    var outputMetadataPath = OptionValue(argsTail, "--output-metadata");
    var productionSourceBundlePath = OptionValue(argsTail, "--production-source-bundle");
    var emitFunctionFiles = !HasFlag(argsTail, "--no-function-files");
    if (!emitFunctionFiles && string.IsNullOrWhiteSpace(productionSourceBundlePath))
        throw new ArgumentException("--no-function-files requires --production-source-bundle.");
    BaseTranslationOutputMetadata? previousOutputMetadata = null;
    if (HasFlag(argsTail, "--prune-stale") &&
        outputMetadataPath is not null &&
        File.Exists(outputMetadataPath))
    {
        try
        {
            previousOutputMetadata = BaseTranslationOutputMetadataFile.Read(outputMetadataPath);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(
                $"[translator] Warning: existing output metadata could not be read; stale pruning will use one-time tree migration: {ex.Message}");
        }
    }
    var pruneStale = HasFlag(argsTail, "--prune-stale");
    var threadsOption = OptionValue(argsTail, "--threads");
    var maxThreads = threadsOption != null ? Math.Max(1, ParseInt(threadsOption)) : Math.Max(1, Environment.ProcessorCount);
    // Discovery batches: cheap front-end-only work items, so the batch is sized
    // generously to keep queue overhead off the hot path. Deliberately larger
    // than the mod emission wave below, which carries whole C++ emissions.
    var batchSize = Math.Max(64, maxThreads * 8);
    Console.WriteLine($"[translator] Using up to {maxThreads} worker thread(s).");

    // Log start time
    var startTime = DateTime.Now;
    Console.WriteLine($"[translator] Starting translation at {startTime:HH:mm:ss}");

    try
    {
        Directory.CreateDirectory(outDir);

        var visited = new HashSet<uint>();
        var translated = new HashSet<uint>();
        // Per translated function: sorted, coalesced [start, endExclusive) pairs of
        // the instruction addresses its own control flow executes (leaf-inlined
        // callees are decoded separately and never appear here). Feeds the
        // fall-through interior-entry detection for the base manifest.
        var translatedFlowCoverage = new Dictionary<uint, uint[]>();
        var gqrUnknownEntryRoots = new HashSet<uint> { startAddr };
        var translatedFunctionEnds = new Dictionary<uint, uint>();
        // Every map entry is a function start, so the whole boundary set is known
        // before the first decode instead of accumulating as discovery proceeds.
        var knownBaseFunctionEntryPoints = BuildKnownBaseFunctionEntryPoints(
            outDir,
            RequireProject().Translation.EntryPoints
                .Append(startAddr)
                .Concat(functionMap?.Addresses ?? Array.Empty<uint>()),
            includeGeneratedHistory: false);
        Console.WriteLine($"[translator] Loaded {knownBaseFunctionEntryPoints.Count:N0} known base function start(s) for tail-call boundary detection.");
        if (functionMap is not null)
        {
            Console.WriteLine(
                $"[translator] Function map: {functionMap.Addresses.Count:N0} entry point(s), " +
                $"{functionMap.NamedCount:N0} named ({functionMap.SourcePath}).");
        }

        var nativeGuestEffects = runtimeNativeGuestEffects.Value;
        Console.WriteLine($"[translator] Native guest-effect contracts: {nativeGuestEffects.Contracts.Count:N0} total, " +
                          $"{nativeGuestEffects.PreciseContracts.Count:N0} precise, " +
                          $"{nativeGuestEffects.ConservativeContracts.Count:N0} full-context.");
        var residentTranslationExclusions = baseTranslationExclusions.Value;
        Console.WriteLine(
            $"[translator] Leaf inlining: {leafInliningBlockedTargets.Value.Count:N0} address(es) are not " +
            "inlinable (native, excluded from translation, or mod-patched).");

        var queue = new Queue<(uint addr, int depth)>();
        queue.Enqueue((startAddr, 0));
        visited.Add(startAddr);

        // Map entries the call-graph walk never reaches. They are translated, but
        // on a decode or lift failure they are skipped with a count instead of
        // failing the run: unlike a walked call target, nothing proves the map
        // entry is really code, and one noisy line must not block a release.
        var speculativeSeeds = new Queue<uint>(functionMap?.Addresses ?? Array.Empty<uint>());
        var speculativeSkips = new List<(uint Address, string Reason)>();

        var count = 0;
        var totals = new List<TranslationMetrics>();
        var printedDepths = new HashSet<int>();
        var filesUpdated = 0;
        var filesUnchanged = 0;
        var emittedPaths = pruneStale ? new HashSet<string>(StringComparer.OrdinalIgnoreCase) : null;
        var baseSummaries = new Dictionary<uint, GqrFunctionSummary>();
        var baseWork = new Dictionary<uint, (
            string Name,
            string Path,
            int MaxInstructions,
            int MaxBytes,
            bool AllowUnsupported)>();
        var baseOutputPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        BaseTranslationOutputMetadata? completedOutputMetadata = null;
        BaseTranslationModAwareness? completedModAwareness = null;

        void TrackEmittedPath(string path)
        {
            if (emittedPaths is null)
            {
                return;
            }

            lock (emittedPaths)
            {
                emittedPaths.Add(Path.GetFullPath(path));
            }
        }

        var parallelOptions = new ParallelOptions { MaxDegreeOfParallelism = maxThreads };

        void ProcessQueue(bool speculative = false)
        {
            while (queue.Count > 0)
            {
                var batch = new List<(uint Address, int Depth, string Name)>(batchSize);
                var batchAddresses = new HashSet<uint>();
                var candidates = QueueBatch.Dequeue(
                    queue,
                    batchSize,
                    next => ShouldSkip(next) || !batchAddresses.Add(next.addr));
                foreach (var next in candidates)
                {
                    var fallbackName = $"func_{next.addr:X8}";
                    var name = ResolveFunctionName(next.addr, preferred: null, fallbackName);
                    // Every queued item is a translator-discovered function start.
                    // Register the whole wave before decoding so peer boundaries
                    // are deterministic and cannot be swallowed by batch order.
                    knownBaseFunctionEntryPoints.Add(next.addr);
                    batch.Add((next.addr, next.depth, name));
                }

                if (batch.Count == 0)
                {
                    continue;
                }

                foreach (var depth in batch.Select(b => b.Depth).Distinct().OrderBy(d => d))
                {
                    LogDepth(depth);
                }

                TranslateBatch(batch, speculative);
            }
        }

        // Batches drain one at a time so a callee of a prior speculative entry arrives with a known
        // caller/GQR state instead of as another unknown root. Everything here is speculative, so a
        // "call" decoded from an unreached map entry isn't proven code; treat failures as soft, not fatal.
        void ProcessSpeculativeSeeds()
        {
            while (speculativeSeeds.Count > 0)
            {
                var batch = new List<(uint Address, int Depth, string Name)>(batchSize);
                var candidates = QueueBatch.Dequeue(
                    speculativeSeeds,
                    batchSize,
                    address => translated.Contains(address) || !visited.Add(address));
                foreach (var address in candidates)
                {
                    // Nothing calls this from the translated set, so its entry
                    // GQR state is unconstrained.
                    gqrUnknownEntryRoots.Add(address);
                    batch.Add((address, 0, ResolveFunctionName(address, preferred: null, $"func_{address:X8}")));
                }

                if (batch.Count == 0)
                {
                    continue;
                }

                TranslateBatch(batch, speculative: true);
                ProcessQueue(speculative: true);
            }
        }

        void LogDepth(int depth)
        {
            if (printedDepths.Add(depth))
            {
                Console.WriteLine($"Translating functions on depth {depth}...");
            }
        }

        bool ShouldSkip((uint addr, int depth) item)
        {
            if (translated.Contains(item.addr))
            {
                return true;
            }

            return false;
        }

        // `speculative` selects the failure policy for the whole batch: a walked
        // call target that will not decode is a translator bug and stops the run,
        // while an address the configured map claims - directly, or through the calls
        // decoded out of one - is dropped with a counted warning.
        void TranslateBatch(List<(uint Address, int Depth, string Name)> batch, bool speculative = false)
        {
            var results = new FunctionDiscoveryResult?[batch.Count];
            IndexedParallel.For(batch.Count, parallelOptions, i => results[i] = Attempt(batch[i], speculative));

            // Re-run only translations that swallowed a newly revealed sibling entry, or direct
            // branches could bypass a mod overlay registered there. Both scans are pure per-result
            // so they run on the same worker pool; the dedup filter below keeps first sighting.
            var discoveryTargets = new uint[results.Length][];
            var swallowedSiblingEntries = new uint[results.Length][];
            IndexedParallel.For(results.Length, parallelOptions, i =>
            {
                if (results[i] is not { } result) return;
                discoveryTargets[i] = DiscoverDiscoveryTranslationTargets(result).ToArray();
                swallowedSiblingEntries[i] = DiscoverSwallowedSiblingEntries(result).ToArray();
            });
            var newlyKnownEntries = Enumerable.Range(0, results.Length)
                .Where(i => results[i] is not null)
                .SelectMany(i => discoveryTargets[i].Concat(swallowedSiblingEntries[i]))
                .Where(knownBaseFunctionEntryPoints.Add)
                .ToHashSet();
            if (newlyKnownEntries.Count != 0)
            {
                // The boundary set is final for this batch once the filter above has run,
                // so the affected re-translations are independent of one another.
                var affected = new List<int>();
                for (var i = 0; i < results.Length; i++)
                {
                    if (results[i] is not { } result) continue;
                    var entry = result.EntryPoint;
                    if (result.Instructions.Any(instruction =>
                            instruction.Address != entry && newlyKnownEntries.Contains(instruction.Address)))
                    {
                        affected.Add(i);
                    }
                }

                IndexedParallel.For(affected.Count, parallelOptions, k =>
                {
                    var i = affected[k];
                    results[i] = Attempt(batch[i], speculative);
                    if (results[i] is { } result)
                    {
                        discoveryTargets[i] = DiscoverDiscoveryTranslationTargets(result).ToArray();
                    }
                });
            }

            // The compact GQR projection is a pure walk of the final SSA graph and the
            // canonical store serializes its own dictionary, so both move off the serial
            // commit. Every address in a batch is distinct and untranslated, so the store
            // sees one Put per key regardless of completion order.
            var summaries = new GqrFunctionSummary[results.Length];
            IndexedParallel.For(results.Length, parallelOptions, i =>
            {
                if (results[i] is not { } result) return;
                summaries[i] = GqrFunctionSummary.Create(result.Ssa.Function);
                canonicalIrStore.Put(batch[i].Address, result.Ssa.Function);
            });

            // Update state sequentially (thread-safe)
            for (var i = 0; i < batch.Count; i++)
            {
                if (results[i] is not { } result) continue;
                CommitResult(batch[i], result, discoveryTargets[i], summaries[i]);
            }
        }

        FunctionDiscoveryResult? Attempt((uint Address, int Depth, string Name) work, bool speculative)
        {
            if (!speculative)
            {
                return TranslateWork(work);
            }

            try
            {
                return TranslateWork(work);
            }
            catch (Exception ex)
            {
                lock (speculativeSkips)
                {
                    speculativeSkips.Add((work.Address, ex.Message));
                }

                return null;
            }
        }

        static IEnumerable<uint> DiscoverSwallowedSiblingEntries(FunctionDiscoveryResult result)
        {
            var instructionsByAddress = result.Instructions.ToDictionary(instruction => instruction.Address);
            foreach (var branch in result.Instructions.Where(instruction => instruction.IsUnconditionalBranch))
            {
                foreach (var target in branch.BranchTargets ?? Array.Empty<uint>())
                {
                    if (target == result.EntryPoint ||
                        !instructionsByAddress.TryGetValue(target, out var targetInstruction))
                    {
                        continue;
                    }

                    if (targetInstruction.Mnemonic.Equals("stwu", StringComparison.OrdinalIgnoreCase) &&
                        targetInstruction.OperandText.StartsWith("r1,", StringComparison.OrdinalIgnoreCase))
                    {
                        yield return target;
                    }
                }
            }
        }

        FunctionDiscoveryResult TranslateWork((uint Address, int Depth, string Name) work)
        {
            var options = WithProjectFrontEndPolicy(new TranslationOptions(
                work.Name,
                KnownFunctionEntryPoints: knownBaseFunctionEntryPoints),
                RequireProject().Translation);
            return translator.Value.Discover(work.Address, options);
        }

        void CommitResult(
            (uint Address, int Depth, string Name) work,
            FunctionDiscoveryResult result,
            IReadOnlyList<uint> discoveryTargets,
            GqrFunctionSummary summary)
        {
            // File writing moved to parallel section above
            totals.Add(result.Metrics);
            count++;
            translated.Add(work.Address);
            translatedFunctionEnds[work.Address] = GetFunctionInstructionEnd(result);
            translatedFlowCoverage[work.Address] = CoalesceInstructionRanges(result);

            foreach (var target in discoveryTargets)
            {
                knownBaseFunctionEntryPoints.Add(target);
                if (visited.Add(target))
                {
                    queue.Enqueue((target, work.Depth + 1));
                }
            }

            // Discovery needs decoded PPC instructions and SSA temporarily. The
            // whole-program pass only needs its compact GQR projection; release
            // the full graph here and rebuild it once, with final facts, during
            // the emission wave.
            var outputPath = Path.Combine(outDir, $"{work.Name}.cpp");
            if (!baseOutputPaths.Add(Path.GetFullPath(outputPath)))
            {
                throw new InvalidOperationException(
                    $"Translator output path collision for '{outputPath}' while retaining function 0x{work.Address:X8}.");
            }
            // Both the projection and the canonical encoding were produced by the
            // parallel stage above; this only records them.
            baseSummaries[work.Address] = summary;
            baseWork[work.Address] = (
                work.Name,
                outputPath,
                TranslationOptions.Default.MaxInstructions,
                TranslationOptions.Default.MaxBytes,
                allowUnsupportedInstructions);
        }

        // The call-graph walk from the entry point, then everything the configured map
        // knows about that the walk never reached. Those two sources replace the
        // constructor, vtable, function-pointer and adjacent-prologue scans, which
        // guessed at exactly this set - and the hand-maintained force-translate
        // list that existed to paper over the addresses they still missed.
        ProcessQueue();
        var walkReachable = count;
        ProcessSpeculativeSeeds();
        if (functionMap is not null)
        {
            Console.WriteLine(
                $"[translator] Function starts: {walkReachable:N0} reached by the call-graph walk, " +
                $"{count - walkReachable:N0} added speculatively from the map.");
            if (speculativeSkips.Count != 0)
            {
                foreach (var (address, reason) in speculativeSkips.OrderBy(static skip => skip.Address).Take(20))
                {
                    Console.WriteLine($"[translator]   skipped map entry 0x{address:X8}: {reason}");
                }
                Console.WriteLine(
                    $"[translator] Skipped {speculativeSkips.Count:N0} map entry/entries that did not translate; " +
                    "these are seeded only by the map and are never reached by a call.");
            }
        }

        if (baseSummaries.Count > 0)
        {
            // Wall-clock accounting for the serial whole-program section; prints
            // per-phase elapsed time so regressions are visible in build logs.
            var lastPhaseSeconds = 0d;
            void LogPhase(string label)
            {
                var now = (DateTime.Now - startTime).TotalSeconds;
                Console.WriteLine($"[translator] phase '{label}': {now - lastPhaseSeconds:F1}s (t={now:F1}s)");
                lastPhaseSeconds = now;
            }
            LogPhase("discovery");
            // Function starts are discovered from the current inputs, not from a
            // previous generated tree. If a later discovery source identified a
            // boundary inside an earlier decode, rebuild only that owner's compact
            // summary with the final deterministic boundary set.
            var finalBoundaries = knownBaseFunctionEntryPoints.OrderBy(address => address).ToArray();
            var boundaryRepairAddresses = translatedFunctionEnds
                .Where(pair => ContainsInteriorBoundary(pair.Key, pair.Value, finalBoundaries))
                .Select(pair => pair.Key)
                .OrderBy(address => address)
                .ToArray();
            if (boundaryRepairAddresses.Length != 0)
            {
                Console.WriteLine($"[translator] Reanalyzing {boundaryRepairAddresses.Length:N0} function(s) affected by later-discovered boundaries...");
                var repaired = new FunctionDiscoveryResult[boundaryRepairAddresses.Length];
                IndexedParallel.For(boundaryRepairAddresses.Length, parallelOptions, i =>
                {
                    var address = boundaryRepairAddresses[i];
                    var work = baseWork[address];
                    repaired[i] = translator.Value.Discover(address, WithProjectFrontEndPolicy(new TranslationOptions(
                        work.Name,
                        MaxInstructions: work.MaxInstructions,
                        MaxBytes: work.MaxBytes,
                        AllowUnsupportedInstructions: work.AllowUnsupported,
                        KnownFunctionEntryPoints: knownBaseFunctionEntryPoints),
                        RequireProject().Translation));
                });
                for (var i = 0; i < boundaryRepairAddresses.Length; i++)
                {
                    var address = boundaryRepairAddresses[i];
                    baseSummaries[address] = GqrFunctionSummary.Create(repaired[i].Ssa.Function);
                    canonicalIrStore.Put(address, repaired[i].Ssa.Function);
                    translatedFunctionEnds[address] = GetFunctionInstructionEnd(repaired[i]);
                    translatedFlowCoverage[address] = CoalesceInstructionRanges(repaired[i]);
                }
            }

            LogPhase("boundary repair");

            // Entries another translation's own flow executes through. Branch
            // targets surface as block labels and are excluded downstream by the
            // manifest builder's label rule; this catches the label-less
            // fall-through case (e.g. a function map's split-switch artifact),
            // which must not split the patch-homing base function ranges.
            var interiorEntryPoints = ComputeInteriorEntryPoints(translatedFlowCoverage);

            {
                var inlinedSites = totals.Sum(static metrics => metrics.InlinedCallSites);
                var inlinedFunctions = totals.Count(static metrics => metrics.InlinedCallSites > 0);
                var inlinedGuestInstructions = totals.Sum(static metrics => metrics.InlinedGuestInstructions);
                Console.WriteLine(
                    $"[translator] Leaf inlining: {inlinedSites:N0} direct call site(s) spliced across " +
                    $"{inlinedFunctions:N0} function(s), {inlinedGuestInstructions:N0} guest instruction(s) duplicated.");
            }

            Console.WriteLine("[translator] Computing interprocedural GQR constants and callee write summaries...");
            var gqrAnalysis = GqrInterproceduralAnalysis.Analyze(
                baseSummaries,
                gqrUnknownEntryRoots);
            var directGqrTargets = baseSummaries.Values
                .SelectMany(summary => summary.DirectGuestTargets)
                .Where(baseSummaries.ContainsKey)
                .ToHashSet();
            var guardedRoots = baseSummaries.Keys.Where(address => !directGqrTargets.Contains(address)).ToHashSet();
            guardedRoots.Add(startAddr);
            var guardedGqrAnalysis = GqrInterproceduralAnalysis.Analyze(
                baseSummaries,
                guardedRoots);
            var gqrSpecializedFunctions = gqrAnalysis.EntryConstants.Count(pair => pair.Value.Count != 0);
            var gqrSpecializedRegisters = gqrAnalysis.EntryConstants.Sum(pair => pair.Value.Count);
            var guardedGqrFunctions = guardedGqrAnalysis.EntryConstants.Count(pair =>
                pair.Value.Count > gqrAnalysis.EntryConstants[pair.Key].Count);
            Console.WriteLine($"[translator] Proven GQR entry constants: {gqrSpecializedRegisters:N0} across {gqrSpecializedFunctions:N0} function(s); reachable functions: {gqrAnalysis.ReachableFunctions.Count:N0}.");
            Console.WriteLine($"[translator] Runtime-guarded direct-call GQR contexts: {guardedGqrFunctions:N0} function(s).");
            LogPhase("GQR interprocedural x2");

            var emissionAddresses = baseSummaries.Keys.OrderBy(address => address).ToArray();
            var localGuestAbiContracts = new Dictionary<uint, GuestAbiContract>(emissionAddresses.Length);
            var canonicalGuestFunctions = new Dictionary<uint, IrFunction>(emissionAddresses.Length);
            var irInstructionCounts = new Dictionary<uint, int>(emissionAddresses.Length);
            // Each slot is an independent pure decode plus contract analysis, so
            // compute them in parallel into position-indexed arrays and fill the
            // dictionaries afterwards in the original ascending order. Dictionary
            // insertion order is preserved, so every downstream enumeration of
            // these maps is byte-for-byte the same as the serial version.
            var canonicalByIndex = new IrFunction[emissionAddresses.Length];
            var localContractByIndex = new GuestAbiContract[emissionAddresses.Length];
            var irInstructionCountByIndex = new int[emissionAddresses.Length];
            IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
            {
                var address = emissionAddresses[i];
                if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    throw new InvalidOperationException($"Missing canonical SSA while building ABI contract for 0x{address:X8}.");
                canonicalByIndex[i] = canonicalSsa;
                localContractByIndex[i] = GuestAbiContractAnalyzer.Analyze(canonicalSsa);
                irInstructionCountByIndex[i] = canonicalSsa.Blocks.Sum(block => block.Instructions.Count);
            });
            for (var i = 0; i < emissionAddresses.Length; i++)
            {
                var address = emissionAddresses[i];
                canonicalGuestFunctions[address] = canonicalByIndex[i];
                localGuestAbiContracts[address] = localContractByIndex[i];
                irInstructionCounts[address] = irInstructionCountByIndex[i];
            }

            LogPhase("canonical decode + local contracts");

            // Compose guest calls in execution order. This preserves a callee
            // input only when it is still read-before-write at the caller entry,
            // rather than retaining every descendant dependency forever.
            var interproceduralGuestAbi = GuestAbiInterproceduralAnalyzer.Analyze(
                canonicalGuestFunctions,
                nativeGuestEffects.Contracts);
            var guestAbiContracts = interproceduralGuestAbi.Contracts
                .ToDictionary(static pair => pair.Key, static pair => pair.Value);
            var recursiveComponents = interproceduralGuestAbi.StronglyConnectedComponents
                .Count(component => component.Count > 1 ||
                    guestAbiContracts[component[0]].DirectCallTargets.Contains(component[0]));
            Console.WriteLine(
                $"[translator] SSA guest-state call graph: " +
                $"{interproceduralGuestAbi.StronglyConnectedComponents.Count:N0} component(s), " +
                $"{recursiveComponents:N0} recursive.");

            LogPhase("interprocedural guest ABI");

            // Calls to a registered native observe the native implementation's
            // contract, not the excluded PPC body that happens to share its address.
            foreach (var pair in nativeGuestEffects.Contracts)
                guestAbiContracts[pair.Key] = pair.Value;

            // Determine the state that is actually consumed after each direct
            // call. Public CpuContext wrappers retain the complete architectural
            // behavior; native internal entries return only this union of
            // call-site-live values.
            var demandedStateFreeOutputs = new Dictionary<uint, GuestStateMask>();
            var stateLivenessByFunction = new Dictionary<uint, GuestStateLivenessResult>(emissionAddresses.Length);
            // The per-function liveness solve is pure; only the demanded-output
            // union is order sensitive, so that stays sequential in ascending
            // address order below.
            var livenessByIndex = new GuestStateLivenessResult[emissionAddresses.Length];
            IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
            {
                var address = emissionAddresses[i];
                var contract = guestAbiContracts[address];
                // Start from the public CpuContext wrapper, not the ABI return masks alone: masks-only
                // let compact descendant variants drop live volatile GPR/FPR/CR/LR effects (e.g. f2-f4
                // through 0x8022F80C -> 0x80085040) that aren't source-language return values.
                var exitLive = GuestStateLivenessAnalyzer.MaterializedContextExit(contract);
                livenessByIndex[i] = GuestStateLivenessAnalyzer.Analyze(
                    canonicalGuestFunctions[address], guestAbiContracts, exitLive);
            });
            for (var i = 0; i < emissionAddresses.Length; i++)
            {
                var address = emissionAddresses[i];
                var liveness = livenessByIndex[i];
                stateLivenessByFunction[address] = liveness;
                foreach (var call in liveness.DirectCalls)
                {
                    demandedStateFreeOutputs[call.Target] = demandedStateFreeOutputs
                        .GetValueOrDefault(call.Target)
                        .Union(call.Outputs);
                }
            }

            LogPhase("guest state liveness");

            // Residency, boundary narrowing and the stack elisions all come from
            // WithProjectCodegenPolicy below, which every emitting path shares.
            var residentPolicy = RequireProject().Translation;
            // A Kamek write into a translated function makes a mod overlay its runtime winner, with
            // effects this liveness pass never computed, so callers need the full residency fence.
            // Only raw patch addresses are needed here since the base manifest doesn't exist yet.
            var modOverridableCallTargets = BuildModOverridableCallTargets(
                modPatchedAddresses.Value, translatedFunctionEnds);
            if (modOverridableCallTargets.Count != 0)
            {
                Console.WriteLine(
                    $"[translator] Mod-overridable direct-call targets: {modOverridableCallTargets.Count:N0} " +
                    "function(s) keep the full residency boundary.");
            }
            var directlyCalledGuestFunctions = localGuestAbiContracts.Values
                .SelectMany(static contract => contract.DirectCallTargets)
                .Where(localGuestAbiContracts.ContainsKey)
                .ToHashSet();
            static int StateFreeInputValueCount(GuestAbiContract contract) =>
                BitOperations.PopCount(contract.GprReadBeforeWriteMask) +
                BitOperations.PopCount(contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) +
                ((contract.CrReadBeforeWriteMask | contract.CrPossibleWriteMask) != 0 ? 1 : 0) +
                (contract.ReadsXerBeforeWrite || contract.MayWriteXer ? 1 : 0) +
                (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr ? 1 : 0) +
                (contract.ReadsLrBeforeWrite || contract.MayWriteLr ? 1 : 0) +
                (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr ? 1 : 0) +
                BitOperations.PopCount((uint)(contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask)) +
                BitOperations.PopCount((uint)(contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask));
            static int StateFreeOutputValueCount(GuestAbiContract contract) =>
                BitOperations.PopCount(contract.GprPossibleWriteMask) +
                BitOperations.PopCount(contract.FprPossibleWriteMask) +
                (contract.CrPossibleWriteMask != 0 ? 1 : 0) +
                (contract.MayWriteXer ? 1 : 0) + (contract.MayWriteCtr ? 1 : 0) +
                (contract.MayWriteLr ? 1 : 0) + (contract.MayWriteFpscr ? 1 : 0) +
                BitOperations.PopCount((uint)contract.GqrPossibleWriteMask) +
                BitOperations.PopCount((uint)contract.HidPossibleWriteMask);

            GuestAbiContract PlannedStateFreeContract(uint address)
            {
                var publicContract = guestAbiContracts[address];
                var demandedOutputs = demandedStateFreeOutputs.GetValueOrDefault(address);
                var demandedGprOutputs = publicContract.GprPossibleWriteMask & demandedOutputs.Gpr;
                return publicContract with
                {
                    // A may-write value is also an input unless every exit
                    // overwrites it. On the no-write path the native result
                    // must return the incoming architectural value.
                    GprReadBeforeWriteMask = GuestStateLivenessAnalyzer.RequiredStateFreeGprInputs(
                        publicContract, demandedOutputs.Gpr),
                    GprPossibleWriteMask = demandedGprOutputs,
                    FprPossibleWriteMask = publicContract.FprPossibleWriteMask & demandedOutputs.Fpr,
                    CrPossibleWriteMask = (byte)(publicContract.CrPossibleWriteMask & demandedOutputs.Cr),
                    MayWriteXer = publicContract.MayWriteXer && demandedOutputs.Xer,
                    MayWriteCtr = publicContract.MayWriteCtr && demandedOutputs.Ctr,
                    MayWriteLr = publicContract.MayWriteLr && demandedOutputs.Lr,
                    MayWriteFpscr = publicContract.MayWriteFpscr && demandedOutputs.Fpscr,
                    GqrPossibleWriteMask = (byte)(publicContract.GqrPossibleWriteMask & demandedOutputs.Gqr),
                    HidPossibleWriteMask = (byte)(publicContract.HidPossibleWriteMask & demandedOutputs.Hid)
                };
            }

            // Admit closed direct-call components as a unit. A region is
            // state-free only when all of its translated descendants can use
            // the same representation; external/native targets remain real
            // synchronization boundaries and exclude that component for now.
            var components = interproceduralGuestAbi.StronglyConnectedComponents;
            var componentByAddress = components
                .SelectMany(static (component, index) => component.Select(address => (address, index)))
                .ToDictionary(static pair => pair.address, static pair => pair.index);
            var eligibleComponents = components
                .Select((component, index) => new
                {
                    index,
                    eligible = component.All(address =>
                        !residentTranslationExclusions.Contains(address) &&
                        !localGuestAbiContracts[address].HasFullSynchronizationFence &&
                        GuestStateLivenessAnalyzer.CanDeconstructWithoutContext(canonicalGuestFunctions[address]) &&
                        (!directlyCalledGuestFunctions.Contains(address) ||
                         (StateFreeInputValueCount(PlannedStateFreeContract(address)) <= 4 &&
                          StateFreeOutputValueCount(PlannedStateFreeContract(address)) <= 2)))
                })
                .Where(static item => item.eligible)
                .Select(static item => item.index)
                .ToHashSet();
            bool removedComponent;
            do
            {
                removedComponent = false;
                foreach (var componentIndex in eligibleComponents.ToArray())
                {
                    var hasUnsafeDescendant = components[componentIndex]
                        .SelectMany(address => localGuestAbiContracts[address].DirectCallTargets)
                        .Any(target => !componentByAddress.TryGetValue(target, out var targetComponent) ||
                                       !eligibleComponents.Contains(targetComponent));
                    if (!hasUnsafeDescendant) continue;
                    eligibleComponents.Remove(componentIndex);
                    removedComponent = true;
                }
            } while (removedComponent);
            var stateFreeAbiFunctions = eligibleComponents
                .SelectMany(index => components[index])
                .Where(directlyCalledGuestFunctions.Contains)
                .ToHashSet();
            Console.WriteLine(
                $"[translator] Automatic state-free region ABI: {stateFreeAbiFunctions.Count:N0} directly called function(s) " +
                $"across {eligibleComponents.Count:N0} closed component(s).");

            // The two suffixes are appended to the same sanitized stem rather than
            // derived from each other: rewriting "_resident" into "_statefree"
            // with an unanchored Replace also rewrote any guest symbol whose own
            // name happened to contain that substring.
            static string CxxSymbolStem(string name)
            {
                var builder = new StringBuilder(name.Length + 16);
                foreach (var ch in name.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? name[2..] : name)
                    builder.Append(char.IsLetterOrDigit(ch) || ch == '_' ? ch : '_');
                if (builder.Length == 0 || !(char.IsLetter(builder[0]) || builder[0] == '_'))
                    builder.Insert(0, "func_");
                return builder.ToString();
            }
            var stateFreeCallSymbols = stateFreeAbiFunctions.ToDictionary(
                address => address,
                address => CxxSymbolStem(baseWork[address].Name) + "_statefree");
            var stateFreeCallSiteVariantsByCaller = new Dictionary<
                uint, IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>>();
            var stateFreeEntryVariantsByTarget = new Dictionary<
                uint, IReadOnlyList<GuestStateFreeCallVariant>>();
            Dictionary<string, uint> SelectGqrConstants(uint address, out bool useGuardedConstants)
            {
                var safeConstants = gqrAnalysis.EntryConstants[address];
                var guardedConstants = guardedGqrAnalysis.EntryConstants[address];
                useGuardedConstants = guardedConstants.Count > safeConstants.Count;
                return new Dictionary<string, uint>(
                    useGuardedConstants ? guardedConstants : safeConstants,
                    StringComparer.OrdinalIgnoreCase);
            }

            TranslationOptions BuildEmissionOptions(
                uint address,
                IReadOnlyDictionary<uint, GuestAbiContract> stateFreeAbiContracts)
            {
                var work = baseWork[address];
                var selectedGqrConstants = SelectGqrConstants(address, out var useGuardedConstants);
                return WithProjectCodegenPolicy(
                    new TranslationOptions(
                        work.Name,
                        MaxInstructions: work.MaxInstructions,
                        MaxBytes: work.MaxBytes,
                        AllowUnsupportedInstructions: work.AllowUnsupported,
                        KnownFunctionEntryPoints: knownBaseFunctionEntryPoints,
                        GqrEntryConstants: selectedGqrConstants,
                        GqrCalleeWriteMasks: gqrAnalysis.WriteMasks,
                        GqrConstantsRequireRuntimeGuard: useGuardedConstants && selectedGqrConstants.Count != 0,
                        GuestAbiContracts: guestAbiContracts,
                        EmitStateFreeLeafVariant: stateFreeAbiFunctions.Contains(address),
                        StateFreeAbiContracts: stateFreeAbiContracts,
                        StateFreeCallSymbols: stateFreeCallSymbols,
                        StateFreeCallSiteVariants: stateFreeCallSiteVariantsByCaller.GetValueOrDefault(address),
                         StateFreeEntryVariants: stateFreeEntryVariantsByTarget.GetValueOrDefault(address),
                         ModOverridableCallTargets: modOverridableCallTargets),
                    residentPolicy,
                    address);
            }

            // Populate the recursive ABI inference cache once, in stable global order, before
            // refinement and parallel emission, so cyclic call graphs don't depend on thread count.
            LogPhase("state-free selection + leaf residency");
            ReportAbiPrewarm(inferredGuestAbi.Value.Prewarm(emissionAddresses));
            LogPhase("ABI prewarm");

            // State-free entries use the transformed leaf's exact live-at-entry
            // interface rather than the public architectural contract. Generate
            // that signature first so direct callers never receive a state pointer
            // or unrelated register dependencies.
            var stateFreeAbiContracts = new Dictionary<uint, GuestAbiContract>(stateFreeAbiFunctions.Count);
            var stateFreeInterfaceLeaves = stateFreeAbiFunctions
                .OrderBy(static address => address)
                .ToArray();
            // Seed every admitted signature before lowering any body. A caller
            // must never fall back to CpuContext merely because its callee has a
            // numerically higher address and has not reached the prepass yet.
            foreach (var address in stateFreeInterfaceLeaves)
                stateFreeAbiContracts[address] = PlannedStateFreeContract(address);

            GuestAbiContract RefineStateFreeContract(uint address)
            {
                if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    throw new InvalidOperationException($"Missing canonical SSA for state-free ABI prepass at 0x{address:X8}.");
                var analyzed = translator.Value.LowerCanonical(
                    address,
                    canonicalSsa,
                    BuildEmissionOptions(address, stateFreeAbiContracts));
                // The emitter reports the refined interface directly now. The RECOMP_STATE_FREE_ABI
                // comment it still emits is only for cross-run consumers that parse source files.
                var emitted = analyzed.Emission?.StateFree;
                if (emitted is null || emitted.EntryPoint != address)
                    throw new InvalidOperationException($"Missing state-free ABI metadata for 0x{address:X8}.");
                var plannedContract = PlannedStateFreeContract(address);
                return plannedContract with
                {
                    GprReadBeforeWriteMask = emitted.GprInputMask,
                    GprPossibleWriteMask = emitted.Contract.GprPossibleWriteMask & plannedContract.GprPossibleWriteMask,
                    FprReadBeforeWriteMask = emitted.Contract.FprReadBeforeWriteMask,
                    FprPossibleWriteMask = emitted.Contract.FprPossibleWriteMask & plannedContract.FprPossibleWriteMask,
                    CrReadBeforeWriteMask = emitted.Contract.CrReadBeforeWriteMask,
                    GqrReadBeforeWriteMask = emitted.Contract.GqrReadBeforeWriteMask,
                    HidReadBeforeWriteMask = emitted.Contract.HidReadBeforeWriteMask,
                    HidPossibleWriteMask = (byte)(emitted.Contract.HidPossibleWriteMask & plannedContract.HidPossibleWriteMask),
                    ReadsXerBeforeWrite = emitted.Contract.ReadsXerBeforeWrite,
                    ReadsCtrBeforeWrite = emitted.Contract.ReadsCtrBeforeWrite,
                    ReadsLrBeforeWrite = emitted.Contract.ReadsLrBeforeWrite
                };
            }

            // Refine callees before callers. Recursive SCC members are solved
            // together to a stable signature; the component graph itself is a
            // DAG, so every ordinary component needs only one lowering pass.
            var eligibleComponentEdges = eligibleComponents.ToDictionary(
                componentIndex => componentIndex,
                componentIndex => components[componentIndex]
                    .SelectMany(address => localGuestAbiContracts[address].DirectCallTargets)
                    .Where(componentByAddress.ContainsKey)
                    .Select(target => componentByAddress[target])
                    .Where(targetComponent => targetComponent != componentIndex && eligibleComponents.Contains(targetComponent))
                    .Distinct()
                    .ToArray());
            var callersByComponent = eligibleComponents.ToDictionary(
                componentIndex => componentIndex,
                static _ => new List<int>());
            foreach (var (callerComponent, calleeComponents) in eligibleComponentEdges)
                foreach (var calleeComponent in calleeComponents)
                    callersByComponent[calleeComponent].Add(callerComponent);
            var pendingCallees = eligibleComponentEdges.ToDictionary(
                static pair => pair.Key,
                static pair => pair.Value.Length);
            var readyComponents = new SortedSet<int>(pendingCallees
                .Where(static pair => pair.Value == 0)
                .Select(static pair => pair.Key));
            var processedComponents = 0;
            while (readyComponents.Count != 0)
            {
                var componentIndex = readyComponents.Min;
                readyComponents.Remove(componentIndex);
                ++processedComponents;
                var members = components[componentIndex]
                    .Where(stateFreeAbiFunctions.Contains)
                    .OrderBy(static address => address)
                    .ToArray();
                var changed = true;
                var refinementPass = 0;
                while (changed)
                {
                    if (++refinementPass > 64)
                        throw new InvalidOperationException($"State-free ABI signatures did not converge for component {componentIndex}.");
                    changed = false;
                    foreach (var address in members)
                    {
                        var refined = RefineStateFreeContract(address);
                        if (stateFreeAbiContracts[address] == refined) continue;
                        stateFreeAbiContracts[address] = refined;
                        changed = true;
                    }
                }

                foreach (var callerComponent in callersByComponent[componentIndex])
                    if (--pendingCallees[callerComponent] == 0)
                        readyComponents.Add(callerComponent);
            }
            if (processedComponents != eligibleComponents.Count)
                throw new InvalidOperationException("State-free component graph was not acyclic after SCC condensation.");

            // A single union signature recreates CpuContext traffic as Win x64 stack args/hidden
            // struct-return. Materialize compact variants instead; the four-input/two-output cap
            // fits the native ABI, larger regions need inlining/fusion, not another oversized signature.
            static string StateMaskSortKey(GuestStateMask mask) =>
                $"{mask.Gpr:X8}:{mask.Fpr:X8}:{mask.Cr:X2}:{(mask.Xer ? 1 : 0)}:{(mask.Ctr ? 1 : 0)}:" +
                $"{(mask.Lr ? 1 : 0)}:{(mask.Fpscr ? 1 : 0)}:{mask.Gqr:X2}:{mask.Hid:X2}";
            var stateFreeLivenessContracts = new Dictionary<uint, GuestAbiContract>(guestAbiContracts);
            foreach (var (address, contract) in stateFreeAbiContracts)
                stateFreeLivenessContracts[address] = contract;
            var callsByTarget = stateLivenessByFunction
                .SelectMany(pair => pair.Value.DirectCalls.Select(call => (Caller: pair.Key, Call: call)))
                .Where(item => stateFreeAbiFunctions.Contains(item.Call.Target))
                .GroupBy(static item => item.Call.Target)
                .OrderBy(static group => group.Key);
            var mutableCallSiteVariants = new Dictionary<
                uint, Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>>();
            var compactVariantCount = 0;
            foreach (var targetGroup in callsByTarget)
            {
                var target = targetGroup.Key;
                var variantsByOutput = new Dictionary<GuestStateMask, GuestStateFreeCallVariant>();
                var rankedOutputs = targetGroup
                    .GroupBy(static item => item.Call.Outputs)
                    .OrderByDescending(static group => group.Count())
                    .ThenBy(group => StateMaskSortKey(group.Key), StringComparer.Ordinal)
                    .Take(4)
                    .ToArray();
                var variantIndex = 0;
                foreach (var outputGroup in rankedOutputs)
                {
                    var output = outputGroup.Key;
                    var targetFunction = canonicalGuestFunctions[target];
                    var entryLive = GuestStateLivenessAnalyzer.Analyze(
                        targetFunction, stateFreeLivenessContracts, output).BlockLiveIn[targetFunction.EntryLabel];
                    var publicContract = guestAbiContracts[target];
                    var specializedContract = publicContract with
                    {
                        GprReadBeforeWriteMask = entryLive.Gpr,
                        FprReadBeforeWriteMask = entryLive.Fpr,
                        CrReadBeforeWriteMask = entryLive.Cr,
                        ReadsXerBeforeWrite = entryLive.Xer,
                        ReadsCtrBeforeWrite = entryLive.Ctr,
                        ReadsLrBeforeWrite = entryLive.Lr ||
                            targetFunction.Blocks.SelectMany(static block => block.Instructions)
                                .OfType<IrCall>()
                                .Any(static call => string.IsNullOrWhiteSpace(call.Destination)),
                        ReadsFpscrBeforeWrite = entryLive.Fpscr,
                        // The state-free refinement pass also discovers GQR
                        // reads hidden inside templated PSQ helpers.  Preserve
                        // those body-derived inputs in every narrower variant;
                        // otherwise its definition and caller prototype disagree.
                        GqrReadBeforeWriteMask = (byte)(entryLive.Gqr |
                            stateFreeAbiContracts[target].GqrReadBeforeWriteMask),
                        HidReadBeforeWriteMask = entryLive.Hid,
                        GprPossibleWriteMask = publicContract.GprPossibleWriteMask & output.Gpr,
                        FprPossibleWriteMask = publicContract.FprPossibleWriteMask & output.Fpr,
                        CrPossibleWriteMask = (byte)(publicContract.CrPossibleWriteMask & output.Cr),
                        MayWriteXer = publicContract.MayWriteXer && output.Xer,
                        MayWriteCtr = publicContract.MayWriteCtr && output.Ctr,
                        MayWriteLr = publicContract.MayWriteLr && output.Lr,
                        MayWriteFpscr = publicContract.MayWriteFpscr && output.Fpscr,
                        GqrPossibleWriteMask = (byte)(publicContract.GqrPossibleWriteMask & output.Gqr),
                        HidPossibleWriteMask = (byte)(publicContract.HidPossibleWriteMask & output.Hid)
                    };
                    if (StateFreeInputValueCount(specializedContract) > 4 ||
                        StateFreeOutputValueCount(specializedContract) > 2 ||
                        irInstructionCounts[target] > 96 ||
                        specializedContract == stateFreeAbiContracts[target])
                        continue;
                    var variant = new GuestStateFreeCallVariant(
                        target,
                        $"{stateFreeCallSymbols[target]}_v{variantIndex++}",
                        specializedContract);
                    variantsByOutput[output] = variant;
                    ++compactVariantCount;
                }
                if (variantsByOutput.Count == 0) continue;
                stateFreeEntryVariantsByTarget[target] = variantsByOutput.Values
                    .OrderBy(static variant => variant.Symbol, StringComparer.Ordinal)
                    .ToArray();
                foreach (var item in targetGroup)
                {
                    if (!variantsByOutput.TryGetValue(item.Call.Outputs, out var variant)) continue;
                    if (!mutableCallSiteVariants.TryGetValue(item.Caller, out var callerVariants))
                    {
                        callerVariants = new Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>();
                        mutableCallSiteVariants[item.Caller] = callerVariants;
                    }
                    callerVariants[new GuestStateFreeCallSiteKey(
                        item.Call.BlockLabel, item.Call.Target, item.Call.CallOrdinal)] = variant;
                }
            }
            foreach (var (caller, variants) in mutableCallSiteVariants)
                stateFreeCallSiteVariantsByCaller[caller] = variants;

            if (stateFreeInterfaceLeaves.Length != 0)
                Console.WriteLine($"[translator] Derived {stateFreeInterfaceLeaves.Length:N0} state-free interfaces and " +
                                  $"{compactVariantCount:N0} compact call-site specialization(s) from liveness.");
            LogPhase("state-free refinement");

            var outputMetadata = outputMetadataPath is null
                ? null
                : new BaseTranslationFunctionMetadata[emissionAddresses.Length];
            using var bundleWriter = productionSourceBundlePath is null
                ? null
                : TranslationSourceBundle.CreateWriter(productionSourceBundlePath, emissionAddresses.Length);
            // Emission is one barrier-free parallel pass: waves used to re-synchronise every few
            // hundred functions and ran only as fast as the slowest straggler. Now each worker
            // publishes into an index slot and a single writer appends in ascending index order,
            // so the bundle stays byte-identical without workers ever waiting on each other.
            var collectSources = bundleWriter is not null;
            var pendingSources = new string?[collectSources ? emissionAddresses.Length : 0];
            // `emissionDone[i]` is published with a release write after the source
            // slot is filled, so the writer's acquire read of it also makes the
            // source visible. `completionPulse` only wakes the writer; it carries
            // no state of its own.
            var emissionDone = new bool[pendingSources.Length];
            using var completionPulse = new ManualResetEventSlim(false);
            var emissionAborted = false;
            Task? pendingBundleWrite = null;
            try
            {
                if (bundleWriter is not null && emissionAddresses.Length != 0)
                {
                    TranslationSourceBundleWriter writer = bundleWriter;
                    // A dedicated thread rather than a pool thread: this task now
                    // lives for the whole emission and spends most of it blocked
                    // waiting on the next index, which is exactly the shape the
                    // thread pool is worst at while the parallel pass saturates it.
                    pendingBundleWrite = Task.Factory.StartNew(
                        () =>
                        {
                            for (var nextToWrite = 0; nextToWrite < emissionAddresses.Length; nextToWrite++)
                            {
                                while (!Volatile.Read(ref emissionDone[nextToWrite]))
                                {
                                    // Reset before re-testing so a completion signalled
                                    // between the test and the wait can never be lost,
                                    // and re-test the abort flag so a failed emission
                                    // never leaves this task waiting on an index that
                                    // will never arrive.
                                    completionPulse.Reset();
                                    if (Volatile.Read(ref emissionDone[nextToWrite])) break;
                                    if (Volatile.Read(ref emissionAborted)) return;
                                    completionPulse.Wait();
                                }

                                var address = emissionAddresses[nextToWrite];
                                writer.Write(new TranslationSourceBundleEntry(
                                    address,
                                    Path.GetRelativePath(outDir, baseWork[address].Path).Replace('\\', '/'),
                                    pendingSources[nextToWrite]!));
                                // Drop the reference once appended so completed-but-
                                // unwritten source stays bounded by how far the writer
                                // trails the workers.
                                pendingSources[nextToWrite] = null;
                            }
                        },
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default);
                }

                try
                {
                    IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
                    {
                        var address = emissionAddresses[i];
                        var work = baseWork[address];
                        var options = BuildEmissionOptions(address, stateFreeAbiContracts);
                        if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                            throw new InvalidOperationException($"Missing canonical SSA for final lowering at 0x{address:X8}.");
                        var emitted = translator.Value.LowerCanonical(address, canonicalSsa, options);
                        var path = work.Path;
                        if (outputMetadata is not null)
                        {
                            var metadata = BaseTranslationFunctionMetadata.FromTranslation(outDir, path, emitted);
                            if (interiorEntryPoints.Contains(address))
                            {
                                metadata = metadata with { InteriorToOtherTranslation = true };
                            }
                            outputMetadata[i] = metadata;
                        }
                        if (collectSources)
                        {
                            pendingSources[i] = emitted.CxxCode;
                            Volatile.Write(ref emissionDone[i], true);
                            completionPulse.Set();
                        }
                        if (emitFunctionFiles)
                        {
                            if (FileOutput.WriteTextIfChanged(path, emitted.CxxCode)) Interlocked.Increment(ref filesUpdated);
                            else Interlocked.Increment(ref filesUnchanged);
                            TrackEmittedPath(path);
                        }
                    });
                }
                catch
                {
                    // Release the writer before unwinding: the index it is waiting
                    // on may never complete now.
                    Volatile.Write(ref emissionAborted, true);
                    completionPulse.Set();
                    throw;
                }

                var last = pendingBundleWrite;
                pendingBundleWrite = null;
                last?.GetAwaiter().GetResult();
            }
            finally
            {
                // Only reached with work still in flight when emission itself threw.
                // Wait without observing the writer's own fault so the original
                // failure propagates, but never let the task outlive the writer the
                // enclosing `using` is about to dispose.
                if (pendingBundleWrite is not null)
                {
                    Task.WaitAny(pendingBundleWrite);
                }
            }
            bundleWriter?.Complete();
            LogPhase("emission wave + bundle write");
            if (outputMetadata is not null)
            {
                var bundleReference = productionSourceBundlePath is null
                    ? null
                    : Path.GetRelativePath(
                        Path.GetDirectoryName(Path.GetFullPath(outputMetadataPath!))!,
                        Path.GetFullPath(productionSourceBundlePath)).Replace('\\', '/');
                var metadataUnsupportedInstructionCount = emissionAddresses.Sum(address =>
                {
                    if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    {
                        throw new InvalidOperationException(
                            $"Missing canonical SSA while computing translation quality for 0x{address:X8}.");
                    }
                    return canonicalSsa.Blocks.Sum(block =>
                        block.Instructions.Count(static instruction => instruction is IrUndefined));
                });
                completedOutputMetadata = BaseTranslationOutputMetadata.Create(
                    outputMetadata,
                    new TranslationQualityMetadata(
                        metadataUnsupportedInstructionCount,
                        InvalidSsaFunctionCount: 0),
                    null,
                    bundleReference,
                    BuildModPatchAwareness(modPatchProfiles.Value));
                // The digest stamp above answers "was this exact Code.pul present?". This record
                // answers the question that actually decides reuse - "would a different Code.pul
                // have changed anything here?" - by keeping the translated function ranges beside
                // the patched addresses that landed inside them.
                completedModAwareness = BaseTranslationModAwareness.Create(
                    null,
                    completedOutputMetadata.Functions.Count,
                    BuildModPatchAwarenessDetail(modPatchProfiles.Value),
                    translatedFunctionEnds);
            }
        }

        static bool ContainsInteriorBoundary(uint start, uint end, uint[] boundaries)
        {
            var index = Array.BinarySearch(boundaries, start);
            index = index >= 0 ? index + 1 : ~index;
            return index < boundaries.Length && boundaries[index] < end;
        }

        Console.WriteLine($"Recursively translated {count} functions.");
        var unsupportedInstructionCount = totals.Sum(item => item.UnsupportedInstructionCount);
        if (unsupportedInstructionCount > 0)
        {
            Console.WriteLine(
                $"[translator] DEVELOPER OUTPUT: preserved {unsupportedInstructionCount:N0} unsupported instruction(s) " +
                "as runtime traps; release metadata will reject this translation.");
        }
        if (pruneStale && emittedPaths is not null)
        {
            var removed = PruneStaleGeneratedFiles(outDir, emittedPaths, previousOutputMetadata);
            Console.WriteLine($"[translator] Pruned {removed} stale generated file(s).");
        }
        if (completedOutputMetadata is not null)
        {
            var updated = BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(
                outputMetadataPath!,
                completedOutputMetadata);
            Console.WriteLine(updated
                ? $"[translator] Updated base translation metadata: {outputMetadataPath}"
                : $"[translator] Base translation metadata unchanged: {outputMetadataPath}");
        }
        if (completedModAwareness is not null)
        {
            var awarenessPath = Path.Combine(
                Path.GetDirectoryName(Path.GetFullPath(outputMetadataPath!))!,
                BaseTranslationModAwareness.FileName);
            BaseTranslationModAwarenessFile.WriteIfChangedAtomic(awarenessPath, completedModAwareness);
            Console.WriteLine($"[translator] Recorded mod-patch awareness detail: {awarenessPath}");
        }
        Console.WriteLine($"[translator] File writes: {filesUpdated} updated, {filesUnchanged} unchanged.");
    }
    catch (Exception ex)
    {
        // Log end time and total duration even if there's an exception
        var endTime = DateTime.Now;
        var totalDuration = endTime - startTime;
        Console.WriteLine($"[translator] Translation failed at {endTime:HH:mm:ss}, total time: {totalDuration.TotalSeconds:F2} seconds");
        Console.WriteLine($"[translator] Error: {ex.Message}");
        throw; // Re-throw the exception to maintain original behavior
    }

    // Log end time and total duration
    var endTime2 = DateTime.Now;
    var totalDuration2 = endTime2 - startTime;
    Console.WriteLine($"[translator] Translation completed at {endTime2:HH:mm:ss}, total time: {totalDuration2.TotalSeconds:F2} seconds");

    return 0;
}

int RunTranslateMod(string[] argsTail)
{
    var outDir = OptionValue(argsTail, "--out") ?? profile?.Output;
    if (string.IsNullOrWhiteSpace(outDir))
    {
        return RunTranslateModCore(argsTail, null);
    }

    var publishedOutput = Path.GetFullPath(outDir);
    var previousInputCache = translateModInputCacheDirectory;
    var previousPublishedOutput = translateModPublishedOutputDirectory;
    translateModInputCacheDirectory = publishedOutput;
    translateModPublishedOutputDirectory = publishedOutput;
    try
    {
        var result = TransactionalDirectoryOutput.GenerateAndPublish(
            publishedOutput,
            stagingDirectory => RunTranslateModCore(argsTail, stagingDirectory),
            (stagingDirectory, exitCode) => PreserveTranslateModFailureDiagnostics(
                stagingDirectory,
                publishedOutput,
                exitCode));
        foreach (var warning in result.Warnings)
        {
            Console.Error.WriteLine($"[translator] Warning: {warning}");
        }
        if (result.Publication is { } publication)
        {
            Console.WriteLine(
                $"[translator] published mod output: {publication.AddedFiles:N0} added, " +
                $"{publication.UpdatedFiles:N0} updated, {publication.RemovedFiles:N0} removed, " +
                $"{publication.UnchangedFiles:N0} unchanged");
        }
        return result.ExitCode;
    }
    finally
    {
        translateModInputCacheDirectory = previousInputCache;
        translateModPublishedOutputDirectory = previousPublishedOutput;
    }
}

void PreserveTranslateModFailureDiagnostics(string stagingDirectory, string publishedOutput, int exitCode)
{
    var diagnosticsDirectory = publishedOutput + ".failed";
    if (Directory.Exists(diagnosticsDirectory))
    {
        Directory.Delete(diagnosticsDirectory, recursive: true);
    }
    Directory.CreateDirectory(diagnosticsDirectory);

    var copied = 0;
    foreach (var source in Directory.EnumerateFiles(stagingDirectory, "*", SearchOption.TopDirectoryOnly)
                 .Where(path => path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase) ||
                                path.EndsWith(".json", StringComparison.OrdinalIgnoreCase)))
    {
        File.Copy(source, Path.Combine(diagnosticsDirectory, Path.GetFileName(source)), overwrite: true);
        copied++;
    }
    File.WriteAllText(
        Path.Combine(diagnosticsDirectory, "failure.txt"),
        $"translate-mod exited with code {exitCode}. Preserved {copied} top-level diagnostic file(s).{Environment.NewLine}");
    Console.Error.WriteLine($"[translator] Preserved failure diagnostics: {diagnosticsDirectory}");
}

int RunTranslateModCore(string[] argsTail, string? outputDirectoryOverride)
{
    // Wall-clock accounting mirroring RunTranslateRecursive's LogPhase output.
    var modPhaseStart = DateTime.Now;
    var modLastPhaseSeconds = 0d;
    void LogModPhase(string label)
    {
        var now = (DateTime.Now - modPhaseStart).TotalSeconds;
        Console.WriteLine($"[translator] mod phase '{label}': {now - modLastPhaseSeconds:F1}s (t={now:F1}s)");
        modLastPhaseSeconds = now;
    }

    var codePul = OptionValue(argsTail, "--code-pul") ?? profile?.CodePul;
    var baseManifestPath = OptionValue(argsTail, "--base-manifest");
    var outDir = outputDirectoryOverride ?? OptionValue(argsTail, "--out") ?? profile?.Output;
    if (string.IsNullOrWhiteSpace(codePul) ||
        string.IsNullOrWhiteSpace(baseManifestPath) ||
        string.IsNullOrWhiteSpace(outDir))
    {
        WriteUsage(Console.Error, new[] { "translate-mod" });
        return 1;
    }

    var modRoot = OptionValue(argsTail, "--mod-root") ?? profile?.ModRoot;
    var baseTranslationOutputMetadataPath = OptionValue(argsTail, "--base-translation-output-metadata")
        ?? Path.Combine(root, "generated", "base_translation_output.json");
    // The base translation bakes mod-safety decisions (leaf-inlining blocks, residency fences)
    // around the patch set it knew about; a base tree that never saw this Code.pul can splice
    // mod-patched callee bodies inline, silently running vanilla code where the mod's hooks must
    // win (this shipped once as a character-select LinkList.h:573 panic).
    if (File.Exists(baseTranslationOutputMetadataPath) && File.Exists(codePul))
    {
        var guardedBaseMetadata = BaseTranslationOutputMetadataFile.Read(baseTranslationOutputMetadataPath);
        var codePulSha256 = ChecksumUtilities.Sha256HexOfFile(codePul);
        var baseKnowsThisPul = guardedBaseMetadata.ModPatchAwareness is { } awareness &&
            awareness.Any(entry => string.Equals(entry.CodePulSha256, codePulSha256, StringComparison.OrdinalIgnoreCase));
        // A different Code.pul is not automatically a different base translation. The decisions the
        // stamp above protects are made from the patched addresses alone, so a pul that patches the
        // same translated functions produces the same base tree - and the awareness record beside
        // the metadata is what proves that, address by address.
        if (!baseKnowsThisPul && profile is not null)
        {
            var awarenessDetail = BaseTranslationModAwarenessFile.TryRead(Path.Combine(
                Path.GetDirectoryName(Path.GetFullPath(baseTranslationOutputMetadataPath))!,
                BaseTranslationModAwareness.FileName));
            if (awarenessDetail is not null &&
                awarenessDetail.TranslatedFunctionCount == guardedBaseMetadata.Functions.Count)
            {
                if (awarenessDetail.CoversCodePul(profile.Name, codePul, out var mismatch))
                {
                    Console.WriteLine(
                        "[translator] The base translation predates this Code.pul but patches the same " +
                        "translated functions, so its mod-safety decisions still hold.");
                    baseKnowsThisPul = true;
                }
                else
                {
                    Console.Error.WriteLine(
                        $"[translator] The base translation cannot be reused for this Code.pul: {mismatch}.");
                }
            }
        }
        if (!baseKnowsThisPul)
        {
            Console.Error.WriteLine(
                $"[translator] The base translation ({baseTranslationOutputMetadataPath}) was produced without " +
                $"awareness of this mod's Code.pul (sha256 {codePulSha256}). Its leaf-inlining and residency " +
                "decisions may bypass the mod's patches, which silently breaks the modded product. " +
                "Retranslate the base with this mod's Code.pul present (it is picked up automatically from " +
                "every enabled project profile), then re-run translate-mod.");
            return 1;
        }
    }
    else if (!File.Exists(baseTranslationOutputMetadataPath))
    {
        Console.Error.WriteLine(
            $"[translator] Warning: no base translation output metadata at {baseTranslationOutputMetadataPath}; " +
            "cannot verify the base translation is aware of this mod's patch set.");
    }
    var skipRetroWfc = HasFlag(argsTail, "--skip-retro-wfc");
    var explicitRetroWfcPayloadSpec = OptionValue(argsTail, "--retro-wfc-payload");
    var retroWfcPayloadSpec = skipRetroWfc
        ? null
        : explicitRetroWfcPayloadSpec ??
          (profile?.EnableRetroWfc == true ? profile.RetroWfcPayload : null);
    var requestedRetroWfc = !string.IsNullOrWhiteSpace(retroWfcPayloadSpec);
    if (requestedRetroWfc && profile?.EnableRetroWfc != true)
    {
        Console.Error.WriteLine("[translator] Retro WFC lowering is project-specific and requires a profile with enable_retro_wfc: true.");
        return 1;
    }
    var modName = OptionValue(argsTail, "--mod-name")
        ?? (!string.IsNullOrWhiteSpace(modRoot) ? Path.GetFileName(Path.GetFullPath(modRoot)) : "Pulsar Mod");
    var region = OptionValue(argsTail, "--region") ?? profile?.Region ?? "P";
    var moduleGuestBase = OptionValue(argsTail, "--module-guest-base") is { } moduleBaseText
        ? ParseAddress(moduleBaseText)
        : profile?.ModuleGuestBase ?? 0x81700000u;
    var moduleLinkBaseText = OptionValue(argsTail, "--module-link-base");
    var moduleLinkBase = moduleLinkBaseText is not null
        ? ParseAddress(moduleLinkBaseText)
        : profile?.ModuleLinkBase ?? 0x803992E0u;
    Directory.CreateDirectory(outDir);

    var pul = KamekPulFile.Load(codePul);
    var selected = pul.SelectRegion(region);
    if (requestedRetroWfc && profile?.RetroWfcLegacyBootstrapHook is { } legacyBootstrapHook)
    {
        selected = RetroWfcBootstrapSuppressor.Suppress(selected, legacyBootstrapHook);
        Console.WriteLine(
            $"[translator] Suppressed legacy Retro WFC payload bootstrap hook at 0x{legacyBootstrapHook:X8}; " +
            "the shared payload is initialized statically.");
    }
    var baseManifest = JsonSerializer.Deserialize<BaseManifest>(File.ReadAllText(baseManifestPath))
        ?? throw new InvalidDataException($"Failed to parse base manifest: {baseManifestPath}");
    RetroWfcContractLoweringPlan? retroWfcLoweringPlan = null;
    RetroWfcPayloadLoadResult? retroWfcPayload = null;

    if (!string.IsNullOrWhiteSpace(retroWfcPayloadSpec))
    {
        byte[] payloadImage;
        try
        {
            payloadImage = LoadBinaryInput(retroWfcPayloadSpec, outDir, "retro_wfc_payload.bin");
            File.WriteAllBytes(Path.Combine(outDir, "retro_wfc_payload.bin"), payloadImage);
        }
        catch (Exception ex) when (ex is IOException or System.Net.Http.HttpRequestException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine($"Failed to read Retro WFC payload '{retroWfcPayloadSpec}': {ex.Message}");
            return 1;
        }

        var helperModuleOffset = AlignUp(checked(selected.CodeSize + selected.BssSize), 0x20u);
        try
        {
            retroWfcPayload = RetroWfcPayload.Parse(
                payloadImage,
                baseManifest,
                moduleGuestBase,
                helperModuleOffset,
                retroWfcPayloadSpec);
        }
        catch (InvalidDataException ex)
        {
            Console.Error.WriteLine($"Failed to parse Retro WFC payload '{retroWfcPayloadSpec}': {ex.Message}");
            return 2;
        }

        retroWfcLoweringPlan = retroWfcPayload.LoweringPlan;
        Console.WriteLine($"[translator] Retro WFC shared payload validated: {retroWfcLoweringPlan.StaticBytePatches.Count:N0} static byte patch(es), {retroWfcLoweringPlan.ExecutableHooks.Count:N0} executable hook(s), {retroWfcLoweringPlan.StaticPointers.Count:N0} static pointer(s).");
    }

    var patchPlan = KamekPatchPlanner.Build(selected, baseManifest, moduleGuestBase);
    var continuationPlan = ContinuationPlanner.Build(selected, baseManifest, moduleGuestBase);

    if (patchPlan.Unsupported.Any())
    {
        Console.Error.WriteLine("[translator] translate-mod failed: Code.pul has unsupported commands.");
        Console.Error.WriteLine(patchPlan.BuildTextReport("Code.pul patch classification"));
        return 2;
    }

    var droppedReservedWrites = patchPlan.DroppedReservedRegionWrites.Count();
    if (droppedReservedWrites > 0)
    {
        Console.WriteLine(
            $"[translator] Dropped {droppedReservedWrites} Code.pul write(s) to the WFC codehandler probe word " +
            "(0x80001920); the recomp keeps it clean so the WFC anticheat probe reads an untouched console.");
    }

    var baseManifestDirectory = Path.GetDirectoryName(Path.GetFullPath(baseManifestPath))!;

    var retroWfcOverlayStaticBytePatches = retroWfcLoweringPlan?.StaticBytePatches
        .Where(p => string.Equals(p.LoweringKind, "overlayBytePatch", StringComparison.Ordinal))
        .ToList();
    var retroWfcResolvedExecutableHooks = retroWfcLoweringPlan?.ExecutableHooks
        .Where(h => h.TargetAddress.HasValue)
        .ToList();
    var additionalModuleImage = retroWfcPayload?.RelocatedImage;
    uint? additionalModuleImageOffset = retroWfcPayload?.Summary.HelperModuleOffset;
    var overlayBuild = OverlayFunctionBuilder.BuildAndWrite(
        selected,
        baseManifest,
        patchPlan,
        baseManifestDirectory,
        outDir,
        additionalModuleImage: additionalModuleImage,
        additionalModuleImageOffset: additionalModuleImageOffset,
        retroWfcStaticBytePatches: retroWfcOverlayStaticBytePatches,
        retroWfcExecutableHooks: retroWfcResolvedExecutableHooks);

    if (retroWfcLoweringPlan is not null)
    {
        var overlayStaticBytePatchCount = retroWfcOverlayStaticBytePatches?.Count ?? 0;
        var staticBytePatchesNeedingAnotherLowering = retroWfcLoweringPlan.StaticBytePatches.Count - overlayStaticBytePatchCount;
        var failedRetroWfcOverlayPatches = overlayBuild.Diagnostics.Count(d => d.Target.StartsWith("RetroWFC#", StringComparison.Ordinal));
        var unresolvedExecutableHooks = retroWfcLoweringPlan.ExecutableHooks.Count(h => !h.TargetAddress.HasValue);
        var unresolvedStaticPointers = retroWfcLoweringPlan.StaticPointers.Count(p => !p.TargetAddress.HasValue);
        var resolvedExecutablePointerWrites = retroWfcLoweringPlan.StaticPointers.Count(p => p.SectionExecutable && p.TargetAddress.HasValue);
        if (staticBytePatchesNeedingAnotherLowering != 0 ||
            failedRetroWfcOverlayPatches != 0 ||
            unresolvedExecutableHooks != 0 ||
            unresolvedStaticPointers != 0 ||
            resolvedExecutablePointerWrites != 0)
        {
            Console.Error.WriteLine("[translator] translate-mod stopped: Retro WFC supported static overlay patches were emitted, but unresolved payload lowering remains.");
            Console.Error.WriteLine($"[translator] Static byte overlays emitted: {overlayStaticBytePatchCount:N0}/{retroWfcLoweringPlan.StaticBytePatches.Count:N0}; semantic hooks resolved: {(retroWfcResolvedExecutableHooks?.Count ?? 0):N0}/{retroWfcLoweringPlan.ExecutableHooks.Count:N0}; failed overlay patches: {failedRetroWfcOverlayPatches:N0}; unresolved hooks: {unresolvedExecutableHooks:N0}; unresolved pointer writes: {unresolvedStaticPointers:N0}; executable pointer writes pending overlay support: {resolvedExecutablePointerWrites:N0}.");
            Console.Error.WriteLine("[translator] This is fail-closed; no final translated mod build is emitted that ignores unresolved payload lowering.");
            Console.Error.WriteLine(retroWfcLoweringPlan.BuildTextReport());
            return 2;
        }
    }

    var relocatedModuleImagePath = Path.Combine(outDir, "overlay_images", overlayBuild.ModuleImageFile);
    var relocatedModuleImage = File.ReadAllBytes(relocatedModuleImagePath);

    continuationPlan = ContinuationPlanner.AddModuleTailJumpContinuations(
        continuationPlan,
        baseManifest,
        moduleGuestBase,
        relocatedModuleImage);
    if (retroWfcResolvedExecutableHooks is { Count: > 0 })
    {
        continuationPlan = ContinuationPlanner.AddRetroWfcExecutableHookContinuations(
            continuationPlan,
            baseManifest,
            retroWfcResolvedExecutableHooks);
    }

    var discoveredKamekFunctionStarts = ModFunctionDiscovery.DiscoverKamekFunctions(
        selected,
        moduleGuestBase,
        relocatedModuleImage);
    var retroWfcSemanticFunctionStarts = retroWfcLoweringPlan is null
        ? Enumerable.Empty<ModFunctionStart>()
        : retroWfcLoweringPlan.ExecutableHooks
            .Where(h => h.TargetAddress.HasValue && string.Equals(h.TargetKind, "moduleFunction", StringComparison.Ordinal))
            .Select(h => new ModFunctionStart(h.TargetAddress!.Value, $"Retro WFC semantic action {h.TargetActionId ?? string.Join(",", h.SemanticActionIds)}"))
            .Concat(retroWfcLoweringPlan.StaticPointers
                .Where(p => p.TargetAddress.HasValue && string.Equals(p.TargetKind, "moduleFunction", StringComparison.Ordinal))
                .Select(p => new ModFunctionStart(p.TargetAddress!.Value, $"Retro WFC semantic action {p.TargetActionId ?? string.Join(",", p.SemanticActionIds)}")));
    var retroWfcInitializerTarget = retroWfcPayload?.Summary.InitializationTargetAddress;
    var retroWfcInitializerFunctionStarts = retroWfcInitializerTarget is { } initializerTarget
        ? new[] { new ModFunctionStart(initializerTarget, "Retro WFC payload initialization") }
        : Enumerable.Empty<ModFunctionStart>();
    var retroWfcInitializationCallbacks = retroWfcPayload?.Summary.InitializationCallbacks;
    var retroWfcInitializationCallbackStarts = (retroWfcInitializationCallbacks ?? Array.Empty<RetroWfcHelperInitCallback>())
        .Select(c => new ModFunctionStart(
            c.TargetAddress,
            string.IsNullOrWhiteSpace(c.Symbol)
                ? $"Retro WFC payload initialization callback {c.Kind}"
                : $"Retro WFC payload initialization callback {c.Kind} {c.Symbol}"));
    var kamekFunctionStarts = discoveredKamekFunctionStarts
        .Concat(retroWfcSemanticFunctionStarts)
        .Concat(retroWfcInitializerFunctionStarts)
        .Concat(retroWfcInitializationCallbackStarts)
        .GroupBy(s => s.Address)
        .Select(g => new ModFunctionStart(g.Key, string.Join("; ", g.Select(s => s.Reason).Distinct(StringComparer.Ordinal))))
        .OrderBy(s => s.Address)
        .ToList();
    var cppFailureCount = 0;
    var emitCpp = HasFlag(argsTail, "--emit-cpp");
    var maxThreads = OptionValue(argsTail, "--threads") is { } threadsText
        ? Math.Max(1, ParseInt(threadsText))
        : Math.Max(1, Environment.ProcessorCount);
    LogModPhase("setup + patch planning");
    if (emitCpp)
    {
        var nonReturningModuleCallTargets = FindNonReturningModuleCallTargets(
            continuationPlan,
            kamekFunctionStarts,
            moduleGuestBase,
            checked(overlayBuild.ModuleGuestBase + (uint)relocatedModuleImage.Length));
        cppFailureCount = EmitModCpp(
            outDir,
            baseManifest,
            overlayBuild,
            continuationPlan,
            retroWfcResolvedExecutableHooks,
            kamekFunctionStarts,
            moduleLinkBase,
            selected.CodeSize,
            nonReturningModuleCallTargets,
            maxThreads,
            argsTail);
    }

    LogModPhase("mod C++ emission");
    var dvdOverlayRoots = ModDvdOverlayRoots.Discover(modRoot);
    var riivolutionOptions = profile?.Riivolution?.Options
        .Select(option => new ModRiivolutionOption(option.Section, option.Option, option.Choice))
        .ToList();
    var emitFullInitializerSet = emitCpp &&
        cppFailureCount == 0;
    ModDataPatchWriter.Write(
        Path.Combine(outDir, "cpp", "mod_data_patches.cpp"),
        patchPlan,
        moduleLinkBase,
        baseManifest,
        dvdOverlayRoots,
        relocatedModuleImage,
        SHA1.HashData(selected.CodeBlob),
        retroWfcLoweringPlan?.StaticPointers,
        selected.CodeSize,
        selected.BssSize,
        checked(moduleGuestBase + selected.CtorStart),
        checked(moduleGuestBase + selected.CtorEnd),
        emitFullInitializerSet,
        emitFullInitializerSet ? retroWfcInitializerTarget : null,
        emitFullInitializerSet
            ? retroWfcPayload?.Summary.HelperModuleBase
            : null,
        emitFullInitializerSet
            ? retroWfcPayload?.Summary.InitializationSuccessReturnValue
            : null,
        Path.Combine(
            translateModPublishedOutputDirectory ?? outDir,
            "cpp",
            "mod_data_patches_blobs"),
        profile?.Riivolution?.Xml,
        riivolutionOptions);

    if (emitCpp && cppFailureCount == 0)
    {
        WriteResolvedDispatchProfile(
            outDir,
            RequireProject().Output.Functions,
            Path.Combine(root, "generated", "functions_manifest.txt"),
            baseTranslationOutputMetadataPath,
            Path.Combine(outDir, "cpp"),
            RequireProject().Runtime.NativeRegistrationRoot,
            translateModPublishedOutputDirectory ?? outDir);
    }

    LogModPhase("data patches + dispatch profile");
    Console.WriteLine("[translator] translate-mod complete");
    Console.WriteLine($"  mod: {modName}");
    Console.WriteLine($"  region: {region}");
    Console.WriteLine($"  module base: 0x{moduleGuestBase:X8}");
    Console.WriteLine($"  module link base: 0x{moduleLinkBase:X8}");
    Console.WriteLine($"  module image: overlay_images/{overlayBuild.ModuleImageFile}");
    Console.WriteLine($"  module patches applied: {overlayBuild.ModulePatchCount:N0}");
    Console.WriteLine($"  overlay functions: {overlayBuild.OverlayFunctions.Count:N0}");
    Console.WriteLine($"  continuations: {continuationPlan.Entries.Count:N0}");
    Console.WriteLine($"  discovered module functions: {kamekFunctionStarts.Count:N0}");
    Console.WriteLine($"  data patches: {patchPlan.DataPatches.Count():N0}");
    Console.WriteLine($"  dvd overlay roots: {dvdOverlayRoots.Count:N0}");
    return overlayBuild.Diagnostics.Count == 0 && cppFailureCount == 0 ? 0 : 3;
}

void WriteResolvedDispatchProfile(
    string outDir,
    string baseFunctionsDir,
    string baseFunctionsManifest,
    string baseTranslationOutputMetadataPath,
    string modCppDir,
    string runtimeSourceDir,
    string publishedOutDir)
{
    var candidates = new List<ResolvedDispatchEntry>();
    // The per-function sources carry their registration facts as a
    // RECOMP_REGISTRATION marker comment (see CxxLinearCodeGenerator); nothing
    // registers per function at compile time any more.
    var baseRegistration = GeneratedMarkers.BaseRegistrationPattern();
    var modRegistration = GeneratedMarkers.ModRegistrationPattern();
    var nativeRegistration = GeneratedMarkers.NativeFunctionRegistrationPattern();
    var guestAbiMetadata = GeneratedMarkers.GuestAbiPattern();

    IEnumerable<string> BaseFiles()
    {
        if (File.Exists(baseFunctionsManifest))
        {
            return File.ReadLines(baseFunctionsManifest)
                .Where(line => !string.IsNullOrWhiteSpace(line))
                .Select(line => Path.IsPathRooted(line) ? line : Path.Combine(baseFunctionsDir, Path.GetFileName(line)));
        }
        return Directory.EnumerateFiles(baseFunctionsDir, "*.cpp", SearchOption.AllDirectories);
    }

    static IEnumerable<(string File, string Text)> ReadFiles(IEnumerable<string> files)
    {
        foreach (var file in files)
            if (File.Exists(file)) yield return (file, File.ReadAllText(file));
    }

    IEnumerable<(string File, string Text)> BaseSources()
    {
        var metadataPath = baseTranslationOutputMetadataPath;
        if (File.Exists(metadataPath))
        {
            var metadata = BaseTranslationOutputMetadataFile.Read(metadataPath);
            if (!string.IsNullOrWhiteSpace(metadata.SourceBundlePath))
            {
                var bundlePath = Path.IsPathRooted(metadata.SourceBundlePath)
                    ? metadata.SourceBundlePath
                    : Path.Combine(Path.GetDirectoryName(metadataPath)!, metadata.SourceBundlePath);
                foreach (var entry in TranslationSourceBundle.Read(bundlePath).Entries)
                    yield return (Path.Combine(baseFunctionsDir, entry.VirtualPath), entry.Source);
                yield break;
            }
        }
        foreach (var source in ReadFiles(BaseFiles())) yield return source;
    }

    IEnumerable<(string File, string Text)> ModSources()
    {
        var bundlePath = Path.Combine(Path.GetDirectoryName(modCppDir)!, "translated_sources.bin");
        if (File.Exists(bundlePath))
        {
            foreach (var entry in TranslationSourceBundle.Read(bundlePath).Entries)
                yield return (Path.Combine(modCppDir, entry.VirtualPath), entry.Source);
            yield break;
        }
        foreach (var source in ReadFiles(Directory.EnumerateFiles(modCppDir, "*.cpp", SearchOption.AllDirectories)))
            yield return source;
    }

    void ReadTranslated(
        IEnumerable<(string File, string Text)> sources,
        Regex regex,
        string kind,
        uint defaultPriority,
        Func<string, string>? sourceFileMapper = null)
    {
        foreach (var (file, text) in sources)
        {
            var abi = guestAbiMetadata.Match(text);
            uint AbiHex(string group, uint fallback = uint.MaxValue) => abi.Success
                ? uint.Parse(abi.Groups[group].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                : fallback;
            foreach (Match match in regex.Matches(text))
            {
                var address = GuestTargetParser.ParseHexAddress(match.Groups["address"].Value);
                var publicSymbol = match.Groups["symbol"].Value;
                var symbol = publicSymbol;
                var nameGroup = match.Groups["name"];
                var priorityGroup = match.Groups["priority"];
                var priority = priorityGroup.Success
                    ? uint.Parse(priorityGroup.Value, CultureInfo.InvariantCulture)
                    : defaultPriority;
                var preserves = bool.Parse(match.Groups["preserves"].Value);
                var fprMask = GuestTargetParser.ParseHexAddress(match.Groups["mask"].Value);
                candidates.Add(new ResolvedDispatchEntry(
                    address, symbol, nameGroup.Success ? nameGroup.Value : publicSymbol, kind, priority,
                    true, AbiHex("gr"), AbiHex("gw"), AbiHex("gw"), AbiHex("gret", 0),
                    AbiHex("fr"), AbiHex("fw"), AbiHex("fret", 0),
                    (byte)AbiHex("crr", byte.MaxValue), (byte)AbiHex("crw", byte.MaxValue),
                    !abi.Success || abi.Groups["xr"].Value == "1",
                    !abi.Success || abi.Groups["xw"].Value == "1",
                    !abi.Success || abi.Groups["fence"].Value == "1",
                    preserves, fprMask, true, sourceFileMapper?.Invoke(file) ?? Path.GetFullPath(file)));
            }
        }
    }

    // Every fact the base half of this profile needs - entry point, registration
    // symbol, non-volatile FPR facts and the guest ABI marker - is already in the
    // translator-owned output metadata. Re-reading and regex-scanning ~200 MiB of
    // generated C++ to recover them was pure duplicated work. The bundle scan
    // stays as the fallback for metadata written before build facts existed.
    void ReadBaseTranslated()
    {
        BaseTranslationOutputMetadata? metadata = null;
        if (File.Exists(baseTranslationOutputMetadataPath))
        {
            metadata = BaseTranslationOutputMetadataFile.Read(baseTranslationOutputMetadataPath);
        }
        if (metadata is null || metadata.Functions.Any(static function => function.Build is null))
        {
            ReadTranslated(BaseSources(), baseRegistration, "base", 0);
            return;
        }

        foreach (var function in metadata.Functions)
        {
            var build = function.Build!;
            // The marker text is identical to the `// RECOMP_GUEST_ABI ...` line
            // the emitter writes into the source, so the same pattern and the
            // same missing-metadata fallbacks apply.
            var abi = guestAbiMetadata.Match(build.GuestAbiComment ?? string.Empty);
            uint AbiHex(string group, uint fallback = uint.MaxValue) => abi.Success
                ? uint.Parse(abi.Groups[group].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                : fallback;
            candidates.Add(new ResolvedDispatchEntry(
                function.EntryPoint, build.Symbol, build.Symbol, "base", 0,
                true, AbiHex("gr"), AbiHex("gw"), AbiHex("gw"), AbiHex("gret", 0),
                AbiHex("fr"), AbiHex("fw"), AbiHex("fret", 0),
                (byte)AbiHex("crr", byte.MaxValue), (byte)AbiHex("crw", byte.MaxValue),
                !abi.Success || abi.Groups["xr"].Value == "1",
                !abi.Success || abi.Groups["xw"].Value == "1",
                !abi.Success || abi.Groups["fence"].Value == "1",
                build.PreservesNonvolatileFprs, build.NonvolatileFprWriteMask, true,
                Path.GetFullPath(Path.Combine(baseFunctionsDir, function.RelativePath))));
        }
    }

    ReadBaseTranslated();
    ReadTranslated(
        ModSources(),
        modRegistration,
        "rr",
        100,
        file => Path.GetFullPath(Path.Combine(
            publishedOutDir,
            "cpp",
            Path.GetRelativePath(modCppDir, file))));

    void AddNative(uint address, string symbol, string sourceFile)
    {
        candidates.Add(new ResolvedDispatchEntry(
            address,
            symbol,
            symbol,
            "native", 10000, false,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000018u,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000002u,
            byte.MaxValue, byte.MaxValue, true, true, true,
            false, 0xFFFFC000u, true, Path.GetFullPath(sourceFile)));
    }

    // Generated mod support sources can own native winners too. Keep these in
    // the same resolved profile as runtime-native registrations so direct-call
    // traits and immutable indirect dispatch tables cannot bypass them.
    var modDataPatchesPath = Path.Combine(modCppDir, "mod_data_patches.cpp");
    if (File.Exists(modDataPatchesPath))
    {
        var text = Regex.Replace(File.ReadAllText(modDataPatchesPath), @"//[^\r\n]*", string.Empty);
        foreach (Match match in nativeRegistration.Matches(text))
        {
            AddNative(
                GuestTargetParser.ParseHexAddress(match.Groups["address"].Value),
                match.Groups["symbol"].Value,
                modDataPatchesPath);
        }
    }

    foreach (var registration in runtimeNativeIndex.Value.Registrations
                 .Where(static registration => !registration.IsTranslatedOverride))
    {
        AddNative(
            registration.Address,
            registration.Symbol,
            Path.Combine(runtimeSourceDir, registration.SourceFile));
    }

    var indirectTargets = CollectDataSectionFunctionPointerTargets();
    candidates = candidates
        .Select(entry => entry with
        {
            MustRemainDynamicallyDispatchable = entry.Kind == "native" ||
                                                entry.Address >= 0x81700000u ||
                                                indirectTargets.Contains(entry.Address)
        })
        .ToList();

    static int KindRank(string kind) => kind switch { "native" => 2, "rr" => 1, _ => 0 };
    var groups = candidates.GroupBy(entry => entry.Address).ToArray();
    var resolved = groups
        .Select(group => group
            .OrderByDescending(entry => entry.Priority)
            .ThenByDescending(entry => KindRank(entry.Kind))
            .First())
        .OrderBy(entry => entry.Address)
        .ToArray();

    // A native registration always outranks a mod overlay at the same address, deliberately for
    // functions the runtime must own (hardware, the strap scene). But when the mod's whole point
    // is replacing that function, e.g. Retro Rewind's DVDOpen override at 0x8015E2BC, the overlay
    // is silently dropped. Name every such address so the tradeoff is a choice, not an accident.
    var nativeOverriddenModPatches = groups
        .Where(group =>
            group.Any(entry => entry.Kind == "rr") &&
            group.OrderByDescending(entry => entry.Priority)
                 .ThenByDescending(entry => KindRank(entry.Kind))
                 .First().Kind == "native")
        .Select(group => new
        {
            Address = group.Key,
            Native = group.First(entry => entry.Kind == "native").Symbol,
            NativeSource = group.First(entry => entry.Kind == "native").SourceFile,
            Mod = group.First(entry => entry.Kind == "rr").Name
        })
        .OrderBy(entry => entry.Address)
        .ToArray();

    foreach (var collision in nativeOverriddenModPatches)
    {
        Console.WriteLine(
            $"[translator] WARNING: mod overlay for 0x{collision.Address:X8} ({collision.Mod}) is " +
            $"overridden by native {collision.Native} ({collision.NativeSource}); the mod's patch " +
            "at that address will not run.");
    }
    if (nativeOverriddenModPatches.Length != 0)
    {
        Console.WriteLine(
            $"[translator] {nativeOverriddenModPatches.Length} mod overlay(s) lost to native registrations; " +
            "see resolved_dispatch_profile.json -> NativeOverriddenModPatches.");
    }

    JsonOutput.WriteIfChanged(
        Path.Combine(outDir, "resolved_dispatch_profile.json"),
        new
        {
            Format = "mkwii-resolved-dispatch-profile",
            FormatVersion = 1,
            NativeOverriddenModPatches = nativeOverriddenModPatches,
            Entries = resolved
        },
        JsonOutput.Indented);

    Console.WriteLine($"[translator] resolved dispatch profile: {resolved.Length:N0} winning address(es)");
}

int EmitModCpp(
    string outDir,
    BaseManifest baseManifest,
    OverlayBuildResult overlayBuild,
    ContinuationPlan continuationPlan,
    IReadOnlyCollection<RetroWfcExecutableHookPlan>? retroWfcExecutableHooks,
    IReadOnlyList<ModFunctionStart> kamekFunctionStarts,
    uint moduleLinkBase,
    uint moduleLinkedCodeSize,
    IReadOnlySet<uint> nonReturningCallTargets,
    int maxThreads,
    // The translate-mod command line, so mod emission honours the same codegen
    // policy overrides (residency, boundary narrowing) as base emission instead
    // of quietly diverging from it.
    string[] argsTail)
{
    var modImage = BuildSyntheticModProgramImage(outDir, overlayBuild);
    var inferredAbi = new InferredGuestFunctionAbiProvider(modImage);
    var abiProvider = declaredNativeAbi.Value is { } nativeAbi
        ? new CompositeGuestFunctionAbiProvider(nativeAbi, inferredAbi)
        : new CompositeGuestFunctionAbiProvider(inferredAbi);
    var modTranslator = new FunctionTranslator(modImage, abiProvider);
    var parallelOptions = new ParallelOptions { MaxDegreeOfParallelism = maxThreads };
    // Mod emission waves: each item is a full translate+emit, and each wave is a
    // barrier that also drains newly discovered continuations, so it is
    // deliberately smaller than the discovery batch above - a long wave delays
    // the discoveries the next wave has to translate.
    var waveSize = Math.Max(16, maxThreads * 4);
    Console.WriteLine($"  translating mod C++ with up to {maxThreads} worker thread(s) in waves of {waveSize}");

    var cppRoot = Path.Combine(outDir, "cpp");
    if (Directory.Exists(cppRoot))
    {
        Directory.Delete(cppRoot, recursive: true);
    }
    Directory.CreateDirectory(cppRoot);
    var bundledModSources = new Dictionary<string, TranslationSourceBundleEntry>(StringComparer.OrdinalIgnoreCase);

    var unresolved = new List<string>();
    var translated = 0;
    var baseFunctions = new BaseFunctionIndex(baseManifest.Functions);
    var knownFunctionEntryPoints = baseManifest.Functions
        .Select(f => f.Start)
        .Concat(kamekFunctionStarts.Select(s => s.Address))
        .ToHashSet();
    var queuedContinuationAddresses = continuationPlan.Entries.Select(e => e.Address).ToHashSet();
    var discoveredContinuationQueue = new Queue<ContinuationEntry>();
    var linkedHookLrBasesByTarget = retroWfcExecutableHooks is null
        ? new Dictionary<uint, uint[]>()
        : retroWfcExecutableHooks
            .Where(h => h.TargetAddress.HasValue && RetroWfcHookSetsLinkRegister(h))
            .GroupBy(h => h.TargetAddress!.Value)
            .ToDictionary(
                g => g.Key,
                g => g.Select(h => h.ContinuationAddress).Distinct().ToArray());
    var lrContinuationCallTargets = linkedHookLrBasesByTarget.Keys.ToHashSet();
    var linkedCallFallthroughLrOverrides = retroWfcExecutableHooks is null
        ? new Dictionary<uint, uint>()
        : retroWfcExecutableHooks
            .Where(h => h.TargetAddress.HasValue && RetroWfcHookSetsLinkRegister(h))
            .GroupBy(h => checked(h.Address + 4u))
            .ToDictionary(g => g.Key, g => g.First().ContinuationAddress);

    ModTranslationAttempt[] TranslateWave(IReadOnlyList<ModTranslationWork> wave)
    {
        // Some dynamically discovered module/continuation waves bypass
        // ProcessBoundedWaves. Prewarm every parallel emission boundary as a
        // final deterministic guard; phase-prewarmed entries are cheap hits.
        ReportAbiPrewarm(inferredAbi.Prewarm(wave.Select(static work => work.Address)));
        return ModTranslationWaveRunner.Translate(() => modTranslator, wave, parallelOptions);
    }

    void CommitWave(
        IReadOnlyList<ModTranslationAttempt> attempts,
        Action<FunctionTranslationResult, ModTranslationWork>? onSuccess = null)
    {
        foreach (var attempt in attempts)
        {
            if (attempt.Result is null)
            {
                unresolved.Add(attempt.Error!);
                continue;
            }

            var virtualPath = Path.GetRelativePath(cppRoot, attempt.Work.OutputPath).Replace('\\', '/');
            bundledModSources[virtualPath] = new TranslationSourceBundleEntry(
                attempt.Work.Address,
                virtualPath,
                attempt.Result.CxxCode);
            translated++;
            onSuccess?.Invoke(attempt.Result, attempt.Work);
        }
    }

    void ProcessBoundedWaves(
        IEnumerable<ModTranslationWork> source,
        Action<FunctionTranslationResult, ModTranslationWork>? onSuccess = null)
    {
        var ordered = source
            .OrderBy(work => work.Address)
            .ThenBy(work => work.OutputPath, StringComparer.Ordinal)
            .ToArray();
        // Populate the recursive ABI cache in a stable global order before any worker emits C++,
        // so shared closure reuse and cyclic call graphs never depend on thread count, wave size,
        // or worker order.
        ReportAbiPrewarm(inferredAbi.Prewarm(ordered.Select(static work => work.Address)));
        // Commit callbacks here only append to the discovered-continuation queue that a later
        // phase drains, so nothing in this phase reads back its own commits; wave barriers would
        // only cost idle workers, so it runs as one parallel batch and the runner re-sorts by the
        // same key, leaving commit order and the emitted bundle unchanged.
        CommitWave(TranslateWave(ordered), onSuccess);
    }

    void RecordDiscoveredBaseContinuations(FunctionTranslationResult result, string reason)
    {
        var targets = DiscoverTranslationTargets(result)
            .Concat(DiscoverCallbackTargets(result, argumentRegisters, requireFunctionStart: false))
            .Distinct();

        foreach (var target in targets)
        {
            var section = baseManifest.Sections.FirstOrDefault(s => target >= s.GuestStart && target < s.GuestEnd);
            if (section is null || !section.Executable)
            {
                continue;
            }

            var containing = baseFunctions.FindContaining(target);
            if (containing is null || containing.Start == target)
            {
                continue;
            }

            if (!queuedContinuationAddresses.Add(target))
            {
                continue;
            }

            discoveredContinuationQueue.Enqueue(new ContinuationEntry(
                target,
                containing.Start,
                containing.End,
                section.Name,
                result.EntryPoint,
                KamekCommandId.Write32,
                reason));
        }
    }

    void RecordDiscoveredLrRelativeBaseContinuations(
        FunctionTranslationResult result,
        IReadOnlyList<uint> lrBases,
        string reason)
    {
        if (lrBases.Count == 0)
        {
            return;
        }

        foreach (var offset in DiscoverLrRelativeIndirectJumpOffsets(result).Distinct())
        {
            foreach (var lrBase in lrBases)
            {
                var target = unchecked(lrBase + (uint)offset);
                var section = baseManifest.Sections.FirstOrDefault(s => target >= s.GuestStart && target < s.GuestEnd);
                if (section is null || !section.Executable)
                {
                    continue;
                }

                var containing = baseFunctions.FindContaining(target);
                if (containing is null || containing.Start == target)
                {
                    continue;
                }

                if (!queuedContinuationAddresses.Add(target))
                {
                    continue;
                }

                discoveredContinuationQueue.Enqueue(new ContinuationEntry(
                    target,
                    containing.Start,
                    containing.End,
                    section.Name,
                    result.EntryPoint,
                    KamekCommandId.Branch,
                    $"{reason}; LR-relative jump offset {offset:+#;-#;0}"));
            }
        }
    }

    ModTranslationWork CreateContinuationWork(ContinuationEntry continuation)
    {
        var name = $"rr_continue_{continuation.Address:X8}";
        var options = WithProjectCodegenPolicy(
            new TranslationOptions(
                PreferredName: name,
                MaxInstructions: TranslationOptions.Default.MaxInstructions,
                MaxBytes: checked((int)(continuation.ContainingFunctionEnd - continuation.Address)),
                EmitModRegistration: true,
                ModRegistrationPriority: TranslationOptions.Default.ModRegistrationPriority,
                ModRegistrationModuleId: 1,
                NonReturningCallTargets: nonReturningCallTargets,
                LrContinuationCallTargets: lrContinuationCallTargets,
                LinkedCallFallthroughLrOverrides: linkedCallFallthroughLrOverrides,
                KnownFunctionEntryPoints: knownFunctionEntryPoints),
            RequireProject().Translation,
            continuation.Address);
        return new ModTranslationWork(
            continuation.Address,
            options,
            Path.Combine(cppRoot, "continuations", $"{name}.cpp"));
    }

    var overlayWorks = overlayBuild.OverlayFunctions.Select(overlay =>
    {
        var name = $"rr_overlay_{overlay.Start:X8}";
        var maxBytes = checked((int)(overlay.End - overlay.Start));
        if (baseFunctions.FindContaining(overlay.Start) is { } baseFunction &&
            baseFunction.Start == overlay.Start &&
            overlay.End < baseFunction.End)
        {
            maxBytes = checked((int)(baseFunction.End - overlay.Start));
        }
        var options = WithProjectCodegenPolicy(
            new TranslationOptions(
                PreferredName: name,
                MaxInstructions: TranslationOptions.Default.MaxInstructions,
                MaxBytes: maxBytes,
                EmitModRegistration: true,
                ModRegistrationPriority: TranslationOptions.Default.ModRegistrationPriority,
                ModRegistrationModuleId: 1,
                NonReturningCallTargets: nonReturningCallTargets,
                LrContinuationCallTargets: lrContinuationCallTargets,
                LinkedCallFallthroughLrOverrides: linkedCallFallthroughLrOverrides,
                KnownFunctionEntryPoints: knownFunctionEntryPoints),
            RequireProject().Translation,
            overlay.Start);
        return new ModTranslationWork(
            overlay.Start,
            options,
            Path.Combine(cppRoot, "overlays", $"{name}.cpp"));
    });
    ProcessBoundedWaves(overlayWorks, (result, work) =>
        RecordDiscoveredBaseContinuations(result, $"base continuation discovered from overlay 0x{work.Address:X8}"));

    ProcessBoundedWaves(
        continuationPlan.Entries.Select(CreateContinuationWork),
        (result, work) => RecordDiscoveredBaseContinuations(
            result,
            $"base continuation discovered from continuation 0x{work.Address:X8}"));

    void DrainDiscoveredContinuations()
    {
        while (discoveredContinuationQueue.Count > 0)
        {
            var wave = QueueBatch.Dequeue(discoveredContinuationQueue, waveSize)
                .Select(CreateContinuationWork)
                .ToArray();
            CommitWave(TranslateWave(wave), (result, work) =>
                RecordDiscoveredBaseContinuations(
                    result,
                    $"base continuation discovered from continuation 0x{work.Address:X8}"));
        }
    }

    DrainDiscoveredContinuations();

    var moduleImagePath = Path.Combine(outDir, "overlay_images", overlayBuild.ModuleImageFile);
    var moduleEnd = checked(overlayBuild.ModuleGuestBase + (uint)new FileInfo(moduleImagePath).Length);
    var moduleQueue = new Queue<ModFunctionStart>(kamekFunctionStarts.OrderBy(s => s.Address));
    var queuedModuleStarts = new HashSet<uint>(kamekFunctionStarts.Select(s => s.Address));
    var emittedModuleStarts = new HashSet<uint>();
    while (moduleQueue.Count > 0)
    {
        var wave = new List<ModTranslationWork>(waveSize);
        foreach (var start in QueueBatch.Dequeue(
                     moduleQueue,
                     waveSize,
                     start => !emittedModuleStarts.Add(start.Address)))
        {
            var name = ModuleFunctionName(start.Address);
            var options = WithProjectCodegenPolicy(
                new TranslationOptions(
                    PreferredName: name,
                    MaxInstructions: TranslationOptions.Default.MaxInstructions,
                    MaxBytes: 0x10000,
                    EmitModRegistration: true,
                    ModRegistrationPriority: TranslationOptions.Default.ModRegistrationPriority,
                    ModRegistrationModuleId: 1,
                    NonReturningCallTargets: nonReturningCallTargets,
                    LrContinuationCallTargets: lrContinuationCallTargets,
                    LinkedCallFallthroughLrOverrides: linkedCallFallthroughLrOverrides,
                    KnownFunctionEntryPoints: knownFunctionEntryPoints,
                    ModuleLinkBase: moduleLinkBase,
                    ModuleGuestBase: overlayBuild.ModuleGuestBase,
                    ModuleLinkedCodeSize: moduleLinkedCodeSize),
                RequireProject().Translation,
                start.Address);
            wave.Add(new ModTranslationWork(
                start.Address,
                options,
                Path.Combine(cppRoot, "module", $"{name}.cpp")));
        }

        if (wave.Count == 0)
        {
            continue;
        }

        var attempts = TranslateWave(wave);
        var discoveredModuleTargets = attempts
            .Where(attempt => attempt.Result is not null)
            .SelectMany(attempt => DirectModuleTargets(
                    attempt.Result!, overlayBuild.ModuleGuestBase, moduleEnd)
                .Select(target => (Target: target, Source: attempt.Work.Address)))
            .GroupBy(item => item.Target)
            .Select(group => (Target: group.Key, Source: group.Min(item => item.Source)))
            .OrderBy(item => item.Target)
            .ToArray();
        var newlyKnownModuleTargets = new HashSet<uint>();
        foreach (var (target, source) in discoveredModuleTargets)
        {
            if (queuedModuleStarts.Add(target))
            {
                newlyKnownModuleTargets.Add(target);
                knownFunctionEntryPoints.Add(target);
                moduleQueue.Enqueue(new ModFunctionStart(target, $"direct call from 0x{source:X8}"));
            }
        }

        // A target discovered by one worker can be an interior entry swallowed by
        // another function translated in the same wave. Re-run just those affected
        // functions after publishing the new boundary, before any output is committed.
        if (newlyKnownModuleTargets.Count != 0)
        {
            var retry = attempts
                .Where(attempt => attempt.Result is not null &&
                    attempt.Result.Instructions.Any(instruction =>
                        instruction.Address != attempt.Work.Address &&
                        newlyKnownModuleTargets.Contains(instruction.Address)))
                .Select(attempt => attempt.Work)
                .ToArray();
            if (retry.Length != 0)
            {
                var replacements = TranslateWave(retry)
                    .ToDictionary(attempt => attempt.Work.Address);
                attempts = attempts
                    .Select(attempt => replacements.TryGetValue(attempt.Work.Address, out var replacement)
                        ? replacement
                        : attempt)
                    .ToArray();
            }
        }

        CommitWave(attempts, (result, work) =>
        {
            RecordDiscoveredBaseContinuations(result, $"base continuation discovered from module 0x{work.Address:X8}");
            if (linkedHookLrBasesByTarget.TryGetValue(work.Address, out var lrBases))
            {
                RecordDiscoveredLrRelativeBaseContinuations(
                    result,
                    lrBases,
                    $"base continuation discovered from LR-relative hook target 0x{work.Address:X8}");
            }
        });
    }

    DrainDiscoveredContinuations();


    TranslationSourceBundle.Write(
        Path.Combine(outDir, "translated_sources.bin"),
        bundledModSources.Values);

    foreach (var failure in unresolved)
    {
        Console.Error.WriteLine($"[translator]   unresolved: {failure}");
    }
    Console.WriteLine($"  emitted C++ functions: {translated:N0}");
    Console.WriteLine($"  C++ translation failures: {unresolved.Count:N0}");
    return unresolved.Count;
}

static string ModuleFunctionName(uint address)
{
    // Keep the emitted module filename and C++ symbol compact. Kamek map names
    // can contain complete C++ signatures, which makes MSVC object paths exceed
    // CMAKE_OBJECT_PATH_MAX. The guest address is already a stable, unique
    // identity for the translated function and remains useful when inspecting
    // generated output.
    return $"rr_kamek_{address:X8}";
}

ProgramImage BuildSyntheticModProgramImage(string outDir, OverlayBuildResult overlayBuild)
{
    var loadedProject = RequireProject();
    var rel = relFile.Value ?? throw new InvalidOperationException("Kamek mod translation currently requires the project's single configured REL.");
    var relImage = rel.BuildImage(loadedProject.Inputs.Rel!.LoadAddress);
    var baseImage = new ProgramImageBuilder().Build(dolFile.Value, relImage, loadedProject.Memory.Base, loadedProject.Memory.Size);
    var memory = baseImage.Memory.ToArray();

    CopyFileToGuest(
        memory,
        baseImage.MemoryBase,
        overlayBuild.ModuleGuestBase,
        Path.Combine(outDir, "overlay_images", overlayBuild.ModuleImageFile));

    foreach (var overlay in overlayBuild.OverlayFunctions)
    {
        CopyFileToGuest(
            memory,
            baseImage.MemoryBase,
            overlay.Start,
            Path.Combine(outDir, "overlay_images", overlay.ImageFile));
    }

    return new ProgramImage(
        memory,
        baseImage.UsedRange,
        baseImage.DolRange,
        baseImage.RelRange,
        baseImage.Sha256 + "+mod",
        baseImage.MemoryBase);
}

void CopyFileToGuest(byte[] memory, uint memoryBase, uint guestAddress, string path)
{
    var bytes = File.ReadAllBytes(path);
    if (guestAddress < memoryBase)
    {
        throw new InvalidDataException($"Mod image '{path}' at 0x{guestAddress:X8} precedes configured memory base 0x{memoryBase:X8}.");
    }
    var offset = checked((int)(guestAddress - memoryBase));
    if (offset < 0 || offset + bytes.Length > memory.Length)
    {
        throw new InvalidDataException($"Mod image '{path}' at 0x{guestAddress:X8} exceeds translator RAM image.");
    }
    bytes.CopyTo(memory.AsSpan(offset, bytes.Length));
}

IReadOnlySet<uint> FindNonReturningModuleCallTargets(
    ContinuationPlan continuationPlan,
    IReadOnlyList<ModFunctionStart> moduleFunctionStarts,
    uint moduleStart,
    uint moduleEnd)
{
    if (moduleFunctionStarts.Count == 0)
    {
        return new HashSet<uint>();
    }

    var sortedStarts = moduleFunctionStarts
        .Where(start => start.Address >= moduleStart && start.Address < moduleEnd)
        .GroupBy(start => start.Address)
        .Select(group => group.First())
        .OrderBy(start => start.Address)
        .ToArray();
    if (sortedStarts.Length == 0)
    {
        return new HashSet<uint>();
    }

    var result = new HashSet<uint>();
    foreach (var continuation in continuationPlan.Entries)
    {
        if (continuation.SourceCommandId != KamekCommandId.Branch ||
            continuation.SourceCommandAddress < moduleStart ||
            continuation.SourceCommandAddress >= moduleEnd)
        {
            continue;
        }

        var sortedAddresses = sortedStarts.Select(start => start.Address).ToArray();
        var index = Array.BinarySearch(sortedAddresses, continuation.SourceCommandAddress);
        if (index < 0)
        {
            index = ~index - 1;
        }

        if (index >= 0)
        {
            result.Add(sortedStarts[index].Address);
            for (var i = index - 1; i >= 0; --i)
            {
                var candidate = sortedStarts[i];
                if (continuation.SourceCommandAddress - candidate.Address > 0x80u)
                {
                    break;
                }
                if (!candidate.Reason.Contains("scan", StringComparison.OrdinalIgnoreCase))
                {
                    result.Add(candidate.Address);
                    break;
                }
            }
        }
    }

    return result;
}

IEnumerable<uint> DirectModuleTargets(FunctionTranslationResult result, uint moduleStart, uint moduleEnd)
{
    var localInstructions = result.Instructions
        .Select(instruction => instruction.Address)
        .ToHashSet();

    foreach (var instruction in result.Instructions)
    {
        foreach (var target in instruction.BranchTargets)
        {
            if (target >= moduleStart &&
                target < moduleEnd &&
                (target & 0x3u) == 0 &&
                !localInstructions.Contains(target))
            {
                yield return target;
            }
        }
    }
}

IEnumerable<int> DiscoverLrRelativeIndirectJumpOffsets(FunctionTranslationResult result)
{
    var lrOffsets = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
    int? ctrOffset = null;

    foreach (var instruction in result.Instructions)
    {
        var mnemonic = instruction.Mnemonic.ToLowerInvariant();
        if (mnemonic == "mflr" && TryGetInstructionReg(instruction, 0, out var lrDest))
        {
            lrOffsets[lrDest] = 0;
            continue;
        }

        if ((mnemonic == "mr" || mnemonic == "or") &&
            TryGetInstructionReg(instruction, 0, out var moveDest) &&
            TryGetInstructionReg(instruction, 1, out var moveSource) &&
            (mnemonic == "mr" ||
             (instruction.Operands.Count >= 3 &&
              instruction.Operands[2] is PpcRegisterOperand moveSource2 &&
              string.Equals(NormalizeInstructionReg(moveSource2.Name), moveSource, StringComparison.OrdinalIgnoreCase))))
        {
            if (lrOffsets.TryGetValue(moveSource, out var sourceOffset))
            {
                lrOffsets[moveDest] = sourceOffset;
            }
            else
            {
                lrOffsets.Remove(moveDest);
            }
            continue;
        }

        if (mnemonic == "addi" &&
            TryGetInstructionReg(instruction, 0, out var addDest) &&
            TryGetInstructionReg(instruction, 1, out var addBase) &&
            TryGetInstructionImm(instruction, 2, out var imm))
        {
            if (lrOffsets.TryGetValue(addBase, out var baseOffset))
            {
                lrOffsets[addDest] = checked(baseOffset + imm);
            }
            else
            {
                lrOffsets.Remove(addDest);
            }
            continue;
        }

        if (mnemonic == "mtctr" && TryGetInstructionReg(instruction, 0, out var ctrSource))
        {
            ctrOffset = lrOffsets.TryGetValue(ctrSource, out var sourceOffset) ? sourceOffset : null;
            continue;
        }

        if (mnemonic == "bctr")
        {
            if (ctrOffset.HasValue)
            {
                yield return ctrOffset.Value;
            }
            ctrOffset = null;
            continue;
        }

        if (TryInstructionWritesDest(instruction, out var dest))
        {
            lrOffsets.Remove(dest);
        }
    }

    static bool TryGetInstructionReg(PpcInstruction instruction, int index, out string register)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcRegisterOperand operand)
        {
            register = NormalizeInstructionReg(operand.Name);
            return true;
        }

        register = string.Empty;
        return false;
    }

    static bool TryGetInstructionImm(PpcInstruction instruction, int index, out int immediate)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcImmediateOperand operand)
        {
            immediate = operand.Value;
            return true;
        }

        immediate = 0;
        return false;
    }

    static bool TryInstructionWritesDest(PpcInstruction instruction, out string destination)
    {
        destination = string.Empty;
        if (instruction.Operands.Count == 0 || instruction.Operands[0] is not PpcRegisterOperand operand)
        {
            return false;
        }

        var mnemonic = instruction.Mnemonic.ToLowerInvariant();
        if (mnemonic.StartsWith("st", StringComparison.Ordinal) ||
            mnemonic.StartsWith("b", StringComparison.Ordinal) ||
            mnemonic.StartsWith("cmp", StringComparison.Ordinal))
        {
            return false;
        }

        destination = NormalizeInstructionReg(operand.Name);
        return true;
    }

    static string NormalizeInstructionReg(string register) => register.ToLowerInvariant();
}

static bool RetroWfcHookSetsLinkRegister(RetroWfcExecutableHookPlan hook) =>
    hook.TypeName is "call" or "branchCtrLink" ||
    hook.Intent.Contains("Call", StringComparison.Ordinal);

int RunEmitBaseManifest(string[] argsTail)
{
    var loadedProject = RequireProject();
    var outDir = OptionValue(argsTail, "--out") ?? Path.GetDirectoryName(loadedProject.Output.BaseManifest)!;
    var functionsDir = OptionValue(argsTail, "--functions-dir") ?? loadedProject.Output.Functions;
    var outputMetadataPath = OptionValue(argsTail, "--translation-output-metadata");
    var outputMetadata = outputMetadataPath is null
        ? null
        : BaseTranslationOutputMetadataFile.Read(outputMetadataPath);
    outputMetadata?.RequireReleaseEligible(outputMetadataPath!);
    var region = OptionValue(argsTail, "--region") ?? loadedProject.Identity.Region ?? "unknown";

    var rel = relFile.Value ?? throw new InvalidOperationException("Base mod manifest generation currently requires the project's single configured REL.");
    var relImage = rel.BuildImage(loadedProject.Inputs.Rel!.LoadAddress);
    var baseImage = new ProgramImageBuilder().Build(dolFile.Value, relImage, loadedProject.Memory.Base, loadedProject.Memory.Size);
    var result = BaseManifestBuilder.BuildAndWrite(
        dolFile.Value,
        rel,
        relImage,
        baseImage,
        functionsDir,
        outDir,
        loadedProject.Identity.BaseManifestFormat,
        loadedProject.Identity.GameId ?? loadedProject.Identity.Id,
        region,
        loadedProject.Identity.BaseManifestStem,
        loadedProject.Memory.Base,
        dolFile.Value.MemoryRange.Start,
        Path.GetFileName(loadedProject.Inputs.Dol.Path),
        Path.GetFileName(loadedProject.Inputs.Rel.Path),
        outputMetadata);

    Console.WriteLine($"[translator] Wrote base manifest: {result.ManifestPath}");
    Console.WriteLine($"[translator] Wrote function ranges: {result.FunctionRangesPath}");
    Console.WriteLine($"[translator] Sections: {result.Manifest.Sections.Count}");
    Console.WriteLine($"[translator] Function ranges: {result.Manifest.Functions.Count}");
    return 0;
}

int RunGenerateDataInit()
{
    var loadedProject = RequireProject();
    var output = loadedProject.Output.DataInitializer;
    var runtimeConfigOutput = loadedProject.Output.RuntimeConfig;
    var dolPath = loadedProject.Inputs.Dol.Path;
    string? relPath = loadedProject.Inputs.Rel?.Path;

    var dol = DolFile.Load(dolPath);
    
    // Generate runtime configuration header with the manifest's SDA base pointers
    var (dataInitSda1Base, dataInitSda2Base) = loadedProject.RequireSdaBases();
    RuntimeConfigGenerator.GenerateConfigHeader(
        dataInitSda1Base, dataInitSda2Base, runtimeConfigOutput, loadedProject.Identity.DisplayName);

    // Load the REL file for runtime embedding (apply relocations since runtime does not OSLink)
    RelImage? relImage = null;
    if (!string.IsNullOrWhiteSpace(relPath) && loadedProject.Inputs.Rel is { } configuredRel && File.Exists(relPath))
    {
        relImage = RelFile.Load(relPath).BuildImage(
            configuredRel.LoadAddress,
            applyRelocations: true);
    }
    
    // Generate the data section initializer
    var outputDir = Path.GetDirectoryName(output);
    if (!string.IsNullOrEmpty(outputDir))
    {
        Directory.CreateDirectory(outputDir);
    }
    DataSectionGenerator.Generate(
        dol,
        relImage,
        output,
        loadedProject.Identity.DisplayName,
        relPath is null ? "rel_module" : Path.GetFileNameWithoutExtension(relPath));
    
    // Emit the guest symbol table for crash-report symbolization. The function
    // map is already the project's authoritative name source; the runtime only
    // consults this table on the fatal-crash path.
    var guestSymbolOutput = Path.Combine(string.IsNullOrEmpty(outputDir) ? "." : outputDir, "guest_symbol_table.cpp");
    var guestSymbolCount = EmitGuestSymbolTable(functionMap, guestSymbolOutput);

    // Calculate total size
    var dolDataSize = dol.Sections.Where(s => s.HasData).Sum(s => s.Size);
    var relDataSize = relImage?.Data.Length ?? 0;
    var totalSize = dolDataSize + relDataSize;

    Console.WriteLine($"[translator] Generated data section initializer: {output}");
    Console.WriteLine($"[translator] Guest symbol table: {guestSymbolCount:N0} named entries -> {guestSymbolOutput}");
    Console.WriteLine($"[translator] DOL sections: {dolDataSize:N0} bytes");
    Console.WriteLine($"[translator] REL data: {relDataSize:N0} bytes");
    Console.WriteLine($"[translator] Total embedded: {totalSize:N0} bytes ({totalSize / 1024.0 / 1024.0:F2} MB)");

    return 0;
}

// Writes generated/guest_symbol_table.cpp: sorted guest addresses paired with function-map names,
// skipping unnamed rows. Reuses the map already loaded at startup rather than re-parsing the file;
// the hand-rolled loop this replaced split on spaces only and silently lost tab-separated entries.
int EmitGuestSymbolTable(FunctionMap? map, string outputPath)
{
    var entries = new SortedDictionary<uint, string>();
    if (map is not null)
    {
        foreach (var address in map.Addresses)
        {
            if (map.NameOf(address) is { } name)
            {
                entries[address] = name;
            }
        }
    }

    var sb = new StringBuilder(entries.Count * 48 + 1024);
    sb.Append("// Auto-generated by generate-data-init from the project's function map.\n");
    sb.Append("// Guest address -> symbol name table, consumed by the crash reporter.\n");
    sb.Append("#include <cstdint>\n\n");
    sb.Append("extern \"C\" {\n\n");
    // The extern declarations force external linkage: namespace-scope const
    // objects are internal-linkage by default in C++, even inside extern "C".
    sb.Append("extern const uint32_t kGuestMapSymbolCount;\n");
    sb.Append("extern const uint32_t kGuestMapSymbolAddresses[];\n");
    sb.Append("extern const char* const kGuestMapSymbolNames[];\n\n");
    sb.Append("const uint32_t kGuestMapSymbolCount = ")
      .Append(entries.Count.ToString(System.Globalization.CultureInfo.InvariantCulture))
      .Append("u;\n\n");
    sb.Append("const uint32_t kGuestMapSymbolAddresses[] = {\n");
    if (entries.Count == 0)
    {
        sb.Append("    0u,\n");
    }
    foreach (var entry in entries)
    {
        sb.Append("    0x").Append(entry.Key.ToString("X8", System.Globalization.CultureInfo.InvariantCulture)).Append("u,\n");
    }
    sb.Append("};\n\n");
    sb.Append("const char* const kGuestMapSymbolNames[] = {\n");
    if (entries.Count == 0)
    {
        sb.Append("    \"\",\n");
    }
    foreach (var entry in entries)
    {
        sb.Append("    \"");
        foreach (var ch in entry.Value)
        {
            if (ch == '\\' || ch == '"')
            {
                sb.Append('\\').Append(ch);
            }
            else if (ch < 0x20 || ch > 0x7E)
            {
                sb.Append('?');
            }
            else
            {
                sb.Append(ch);
            }
        }
        sb.Append("\",\n");
    }
    sb.Append("};\n\n} // extern \"C\"\n");
    File.WriteAllText(outputPath, sb.ToString());
    return entries.Count;
}

static uint GetFunctionInstructionEnd(FunctionDiscoveryResult result)
{
    return result.Instructions.Count == 0
        ? result.EntryPoint
        : result.Instructions.Max(i => i.EndAddress);
}

// Sorted, coalesced [start, endExclusive) pairs of the instruction addresses the
// function's own control flow executes. Discovery decodes leaf-inlined callees
// separately, so their addresses never appear in result.Instructions.
static uint[] CoalesceInstructionRanges(FunctionDiscoveryResult result)
{
    var addresses = result.Instructions
        .Select(instruction => instruction.Address)
        .Distinct()
        .ToArray();
    Array.Sort(addresses);
    var ranges = new List<uint>();
    foreach (var address in addresses)
    {
        if (ranges.Count != 0 && ranges[^1] == address)
        {
            ranges[^1] = address + 4;
            continue;
        }
        ranges.Add(address);
        ranges.Add(address + 4);
    }
    return ranges.ToArray();
}

// Entry points that lie strictly inside another translation's executed flow: the
// owner decodes and runs the instruction at that address as part of its own body.
static HashSet<uint> ComputeInteriorEntryPoints(IReadOnlyDictionary<uint, uint[]> flowCoverage)
{
    var entries = flowCoverage.Keys.ToArray();
    Array.Sort(entries);
    var interior = new HashSet<uint>();
    foreach (var (owner, ranges) in flowCoverage)
    {
        for (var i = 0; i < ranges.Length; i += 2)
        {
            var index = Array.BinarySearch(entries, ranges[i]);
            if (index < 0) index = ~index;
            for (; index < entries.Length && entries[index] < ranges[i + 1]; index++)
            {
                if (entries[index] != owner)
                {
                    interior.Add(entries[index]);
                }
            }
        }
    }
    return interior;
}

/// <summary>
/// Base functions where a Kamek-patched overlay could win direct-call dispatch at runtime, so
/// callers can't narrow residency to the unpatched contract. Empty if no profile has a Code.pul.
/// </summary>
static IReadOnlySet<uint> BuildModOverridableCallTargets(
    IReadOnlySet<uint> modPatchedAddresses,
    IReadOnlyDictionary<uint, uint> translatedFunctionEnds)
{
    var overridable = new HashSet<uint>();
    if (modPatchedAddresses.Count == 0)
    {
        return overridable;
    }

    var patched = modPatchedAddresses.ToArray();
    Array.Sort(patched);

    foreach (var (start, end) in translatedFunctionEnds)
    {
        // The recorded end is the last instruction's exclusive end, so a patch
        // exactly at `end` belongs to the next function.
        var index = Array.BinarySearch(patched, start);
        if (index < 0) index = ~index;
        if (index < patched.Length && patched[index] < end) overridable.Add(start);
    }

    return overridable;
}

int PruneStaleGeneratedFiles(
    string baseOutDir,
    HashSet<string> emittedPaths,
    BaseTranslationOutputMetadata? previousOutputMetadata = null)
{
    var result = GeneratedOutputPruner.Prune(baseOutDir, emittedPaths, previousOutputMetadata);
    foreach (var warning in result.Warnings)
    {
        Console.Error.WriteLine($"[translator] Warning: {warning}");
    }
    return result.RemovedFiles;
}

IEnumerable<uint> DiscoverTranslationTargets(FunctionTranslationResult result)
{
    return DiscoverTranslationTargetsCore(result.Instructions, result.LinearIr);
}

IEnumerable<uint> DiscoverDiscoveryTranslationTargets(FunctionDiscoveryResult result)
{
    return DiscoverTranslationTargetsCore(result.Instructions, result.LinearIr);
}

IEnumerable<uint> DiscoverTranslationTargetsCore(
    IReadOnlyList<PpcInstruction> instructions,
    IrFunction linearIr)
{
    var localInstructions = new HashSet<uint>(instructions.Select(i => i.Address));
    var emitted = new HashSet<uint>();

    foreach (var ins in instructions)
    {
        if (ins.BranchTargets.Count == 0)
        {
            continue;
        }

        var shouldFollow = ins.IsCall;
        if (!shouldFollow && ins.IsUnconditionalBranch)
        {
            // If the branch jumps outside this function, treat it as a tail call.
            shouldFollow = ins.BranchTargets.Any(target => !localInstructions.Contains(target));
        }

        if (!shouldFollow)
        {
            continue;
        }

        foreach (var target in ins.BranchTargets)
        {
            if (emitted.Add(target))
            {
                yield return target;
            }
        }
    }

    foreach (var target in DiscoverCallbackTargetsCore(instructions, argumentRegisters, requireFunctionStart: true))
    {
        if (emitted.Add(target))
        {
            yield return target;
        }
    }

    foreach (var call in linearIr.Blocks.SelectMany(static block => block.Instructions.OfType<IrCall>()))
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var target) && emitted.Add(target))
        {
            yield return target;
        }
    }
}

// Discovers callback function pointers by checking all argument registers at every call site
IEnumerable<uint> DiscoverCallbackTargets(
    FunctionTranslationResult result,
    string[] argRegs,
    bool requireFunctionStart = true)
{
    return DiscoverCallbackTargetsCore(result.Instructions, argRegs, requireFunctionStart);
}

IEnumerable<uint> DiscoverCallbackTargetsCore(
    IReadOnlyList<PpcInstruction> instructions,
    string[] argRegs,
    bool requireFunctionStart = true)
{
    var constants = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);

    foreach (var ins in instructions)
    {
        TrackConstants(ins, constants);

        // Discover function pointers stored to memory (vtable/dispatcher init).
        if (IsStoreMnemonic(ins.Mnemonic) && TryGetReg(ins, 0, out var rs))
        {
            if (constants.TryGetValue(rs, out var stored) &&
                LooksExecutable(stored) &&
                (!requireFunctionStart || LooksLikeFunctionStart(stored)))
            {
                yield return stored;
            }
        }

        // Discover function-pointer getters that return an executable callback in r3.
        if (ins.IsReturn &&
            constants.TryGetValue("r3", out var returned) &&
            LooksExecutable(returned) &&
            (!requireFunctionStart || LooksLikeFunctionStart(returned)))
        {
            yield return returned;
        }

        // Check both regular calls and tail calls (unconditional branches to external functions)
        var isCallOrTailCall = ins.IsCall || ins.IsUnconditionalBranch;
        if (isCallOrTailCall && ins.BranchTargets.Count == 1)
        {
            // Check all argument registers for values that look like function pointers
            foreach (var reg in argRegs)
            {
                if (constants.TryGetValue(reg, out var candidate) && 
                    LooksExecutable(candidate) && 
                    (!requireFunctionStart || LooksLikeFunctionStart(candidate)))
                {
                    yield return candidate;
                }
            }

            ClearVolatileAfterCall(constants);
        }
    }

    // PpcDecoder only ever produces lowercase mnemonics (every literal and the
    // `opc_`/`xo_`/`fp_` fallbacks), so the per-instruction ToLowerInvariant
    // allocation this used to make was pure overhead.
    static bool IsStoreMnemonic(string mnemonic) =>
        mnemonic.StartsWith("stw", StringComparison.Ordinal) ||
        mnemonic.StartsWith("std", StringComparison.Ordinal);

    void ClearVolatileAfterCall(IDictionary<string, uint> constants)
    {
        constants.Remove("r3");
        constants.Remove("r4");
        constants.Remove("r5");
        constants.Remove("r6");
        constants.Remove("r7");
        constants.Remove("r8");
        constants.Remove("r9");
        constants.Remove("r10");
        constants.Remove("r11");
        constants.Remove("r12");
    }

    void TrackConstants(PpcInstruction ins, IDictionary<string, uint> constants)
    {
        // Decoder mnemonics are lowercase by construction; comparing the string
        // directly avoids one allocation per decoded instruction.
        var mnem = ins.Mnemonic;

        if (TryHandleMove(ins, mnem, constants))
        {
            return;
        }

        if (mnem == "li")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetImm(ins, 1, out var imm))
            {
                constants[rd] = unchecked((uint)imm);
                return;
            }
        }
        else if (mnem == "lis")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetImm(ins, 1, out var imm))
            {
                constants[rd] = unchecked((uint)(imm << 16));
                return;
            }
        }
        else if (mnem == "addi")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var baseVal))
                {
                    constants[rd] = AddImm(baseVal, imm);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "addis")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var baseValHi))
                {
                    constants[rd] = AddImm(baseValHi, imm << 16);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "ori")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var orBase))
                {
                    constants[rd] = unchecked(orBase | (uint)(imm & 0xFFFF));
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "oris")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var orisBase))
                {
                    constants[rd] = unchecked(orisBase | ((uint)imm << 16));
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }

        if (TryWritesDest(ins, out var dest))
        {
            constants.Remove(dest);
        }
    }

    bool TryHandleMove(PpcInstruction ins, string mnemonic, IDictionary<string, uint> constants)
    {
        if (string.Equals(mnemonic, "mr", StringComparison.OrdinalIgnoreCase) && TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var rs))
        {
            if (constants.TryGetValue(rs, out var value))
            {
                constants[rd] = value;
            }
            else
            {
                constants.Remove(rd);
            }
            return true;
        }

        if (string.Equals(mnemonic, "or", StringComparison.OrdinalIgnoreCase) &&
            ins.Operands.Count == 3 &&
            ins.Operands[0] is PpcRegisterOperand dest &&
            ins.Operands[1] is PpcRegisterOperand src1 &&
            ins.Operands[2] is PpcRegisterOperand src2 &&
            string.Equals(src1.Name, src2.Name, StringComparison.OrdinalIgnoreCase))
        {
            var srcName = NormalizeReg(src1.Name);
            var destName = NormalizeReg(dest.Name);
            if (constants.TryGetValue(srcName, out var value))
            {
                constants[destName] = value;
            }
            else
            {
                constants.Remove(destName);
            }
            return true;
        }

        return false;
    }

    bool TryGetBaseValue(string reg, IDictionary<string, uint> constants, out uint value)
    {
        if (string.Equals(reg, "r0", StringComparison.OrdinalIgnoreCase))
        {
            value = 0;
            return true;
        }

        return constants.TryGetValue(reg, out value);
    }

    bool TryWritesDest(PpcInstruction ins, out string dest)
    {
        dest = string.Empty;
        if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand reg)
        {
            return false;
        }

        var m = ins.Mnemonic.ToLowerInvariant();
        if (m.StartsWith("st", StringComparison.Ordinal) || m.StartsWith("b", StringComparison.Ordinal) || m.StartsWith("cmp", StringComparison.Ordinal))
        {
            return false;
        }

        dest = NormalizeReg(reg.Name);
        return true;
    }

    bool TryGetReg(PpcInstruction ins, int index, out string reg)
    {
        if (ins.Operands.Count > index && ins.Operands[index] is PpcRegisterOperand r)
        {
            reg = NormalizeReg(r.Name);
            return true;
        }

        reg = string.Empty;
        return false;
    }

    bool TryGetImm(PpcInstruction ins, int index, out int imm)
    {
        if (ins.Operands.Count > index && ins.Operands[index] is PpcImmediateOperand i)
        {
            imm = i.Value;
            return true;
        }

        imm = 0;
        return false;
    }

    uint AddImm(uint baseVal, int imm) => unchecked(baseVal + (uint)imm);

    string NormalizeReg(string reg) => reg.ToLowerInvariant();
}

/// <summary>
/// Function entry points stored as a data-section word (vtables, dispatch tables, callbacks). Not a
/// discovery seeder; answers which mod-build winners must stay reachable via dispatch table, not a static call.
/// </summary>
HashSet<uint> CollectDataSectionFunctionPointerTargets()
{
    var executableRanges = BuildExecutableRanges();
    bool IsExecutable(uint addr) => executableRanges.Any(r => r.Contains(addr)) && (addr & 3) == 0;

    var candidates = new HashSet<uint>();

    // Scan DOL data sections for words that look like function pointers.
    foreach (var section in dolFile.Value.Sections.Where(s => s.Kind == SectionKind.Data && s.HasData && s.Size >= 4))
    {
        var baseAddr = section.VirtualAddress;
        var span = section.Data.Span;
        for (var offset = 0; offset + 4 <= span.Length; offset += 4)
        {
            var value = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4));
            if (!IsExecutable(value))
            {
                continue;
            }

            if (!LooksLikeFunctionStart(value))
            {
                continue;
            }

            candidates.Add(value);
        }
    }

    // Scan REL data sections (non-executable) using the relocated image bytes.
    var rel = relFile.Value;
    if (rel is null)
    {
        return candidates;
    }
    var relBase = image.Value.RelRange.Start;
    var memory = image.Value.Memory;

    foreach (var section in rel.Sections.Where(s => !s.Executable && s.FileOffset != 0 && s.Size >= 4))
    {
        var absStart = relBase + section.FileOffset;
        if (!image.Value.Contains(absStart, checked((int)section.Size)))
        {
            continue;
        }
        var memOffset = image.Value.GetOffset(absStart, checked((int)section.Size));

        var span = memory.AsSpan(memOffset, (int)section.Size);
        for (var offset = 0; offset + 4 <= span.Length; offset += 4)
        {
            var value = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4));
            if (!IsExecutable(value))
            {
                continue;
            }

            if (!LooksLikeFunctionStart(value))
            {
                continue;
            }

            candidates.Add(value);
        }
    }

    return candidates;
}

    bool LooksExecutable(uint address)
    {
        // Build into a local and publish the finished list under a gate. The
        // previous shape assigned an empty list to the field and filled it in
        // afterwards, which a concurrent reader could observe half-populated.
        var ranges = Volatile.Read(ref _executableRanges);
        if (ranges == null)
        {
            lock (_executableRangesGate)
            {
                ranges = _executableRanges;
                if (ranges == null)
                {
                    ranges = new List<AddressRange>();
                    if (dolFile.IsValueCreated)
                    {
                        foreach (var s in dolFile.Value.Sections)
                        {
                            if (s.IsExecutable && s.Size > 0)
                            {
                                ranges.Add(AddressRange.FromStartAndSize(s.VirtualAddress, s.Size));
                            }
                        }
                    }

                    if (relFile.IsValueCreated && relFile.Value is not null && image.IsValueCreated)
                    {
                        var relBase = image.Value.RelRange.Start;
                        foreach (var s in relFile.Value.Sections)
                        {
                            if (s.Executable && s.Size > 0 && s.FileOffset != 0)
                            {
                                ranges.Add(AddressRange.FromStartAndSize(relBase + s.FileOffset, s.Size));
                            }
                        }
                    }

                    Volatile.Write(ref _executableRanges, ranges);
                }
            }
        }

        return ranges.Any(r => r.Contains(address)) && (address & 3) == 0;
    }

bool LooksLikeFunctionStart(uint address)
{
    var mem = image.Value.Memory;
    if (!image.Value.Contains(address, sizeof(uint)))
    {
        return false;
    }

    var offset = image.Value.GetOffset(address, sizeof(uint));
    var firstWord = BinaryPrimitives.ReadUInt32BigEndian(mem.AsSpan(offset, 4));
    if (firstWord == 0 || firstWord == 0xFFFFFFFF)
    {
        return false;
    }

    // Decode the first instruction and reject if it's an unknown opcode.
    // This prevents treating string data (e.g., "Metrowerks...") as code.
    var firstIns = PpcDecoder.Decode(address, firstWord);
    if (firstIns.Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase))
    {
        return false;
    }

    return true;
}

List<AddressRange> BuildExecutableRanges()
{
    var ranges = new List<AddressRange>();
    ranges.AddRange(dolFile.Value.Sections.Where(s => s.IsExecutable).Select(s => s.Range));

    if (relFile.Value is { } rel)
    {
        var relBase = image.Value.RelRange.Start;
        ranges.AddRange(rel.Sections
            .Where(s => s.Executable && s.FileOffset != 0 && s.Size > 0)
            .Select(s => AddressRange.FromStartAndSize(relBase + s.FileOffset, s.Size)));
    }

    return ranges;
}

TranslationProjectConfig RequireProject() => project ?? throw new InvalidOperationException(
    "This command requires --project <path>. See projects/mkwii/recomp.yml for an example.");

int ShowHelp(string invalidCommand)
{
    Console.Error.WriteLine($"Unrecognized command '{invalidCommand}'.");
    WriteUsage(Console.Error, KnownCommands());
    return 1;
}

static int ShowGlobalHelp()
{
    WriteUsage(Console.Out, KnownCommands());
    return 0;
}

static int ShowCommandHelp(string command)
{
    WriteUsage(Console.Out, new[] { command });
    return 0;
}

static void WriteUsage(TextWriter writer, IReadOnlyList<string> commands)
{
    writer.WriteLine("Usage:");
    foreach (var name in commands)
    {
        writer.WriteLine(CommandUsage(name));
    }
    writer.WriteLine("Every command also accepts [--project path] [--profile name] [--prefer-cached-inputs] [--help].");
}

static string[] KnownCommands() => new[]
{
    "info",
    "generate-data-init",
    "translate-recursive",
    "translate-mod",
    "emit-base-manifest",
    "emit-build-shards",
    "check-base-mod-awareness"
};

/// <summary>
/// Single source of truth for what each command accepts; usage text and the argument validator
/// both derive from this so a flag can't exist in one but not the other, as happened with the
/// hand-written usage strings this replaced.
/// </summary>
static (string? Positional, CommandOption[] Options)? CommandSpec(string command) => command switch
{
    "info" or "--info" or "--version" =>
        (null, Array.Empty<CommandOption>()),
    "generate-data-init" => (null, Array.Empty<CommandOption>()),
    "translate-recursive" => ("<start_addr>", new CommandOption[]
    {
        new("--outdir", "path"),
        new("--output-metadata", "path"),
        new("--production-source-bundle", "path"),
        new("--threads", "N"),
        new("--no-function-files"),
        new("--prune-stale")
    }),
    "translate-mod" => (null, new CommandOption[]
    {
        new("--code-pul", "path/to/Code.pul", Required: true),
        new("--base-manifest", "path", Required: true),
        new("--out", "path", Required: true),
        new("--mod-root", "path"),
        new("--mod-name", "name"),
        new("--base-translation-output-metadata", "path"),
        new("--region", "P"),
        new("--module-guest-base", "0x81700000"),
        new("--module-link-base", "0x803992E0"),
        new("--threads", "N"),
        new("--retro-wfc-payload", "path-or-url"),
        new("--skip-retro-wfc"),
        new("--emit-cpp")
    }),
    "check-base-mod-awareness" => (null, new CommandOption[]
    {
        new("--translation-output-metadata", "path"),
        new("--code-pul", "path")
    }),
    "emit-base-manifest" => (null, new CommandOption[]
    {
        new("--out", "path"),
        new("--functions-dir", "generated/functions"),
        new("--translation-output-metadata", "path"),
        new("--region", "P")
    }),
    "emit-build-shards" => (null, new CommandOption[]
    {
        new("--base-metadata", "path"),
        new("--base-functions-dir", "path"),
        new("--native-source-dir", "path"),
        new("--resolved-profile", "path"),
        new("--retro-cpp-dir", "path"),
        new("--out", "generated/build_shards")
    }),
    _ => null
};

static string CommandUsage(string command)
{
    if (CommandSpec(command) is not { } spec)
    {
        return $"  translator {command}";
    }

    var text = new StringBuilder("  translator ").Append(command);
    if (spec.Positional is { } positional)
    {
        text.Append(' ').Append(positional);
    }
    foreach (var option in spec.Options)
    {
        var body = option.Value is null ? option.Name : $"{option.Name} {option.Value}";
        text.Append(' ').Append(option.Required ? body : $"[{body}]");
    }
    return text.ToString();
}

// Options every command tolerates. --project and --profile are consumed before
// dispatch, so they only reach a command's tail when they were repeated.
static string[] GlobalValueOptions() => new[] { "--project", "--profile" };

static string[] GlobalFlagOptions() => new[] { "--prefer-cached-inputs" };

/// <summary>
/// Returns an exit code when the command line was fully handled here (help, or an option the
/// command's parser would discard), null otherwise. Only inspects option-like tokens; positional
/// addresses, paths and passthrough values are never rejected.
/// </summary>
static int? HandleCommandLineOptions(string command, string[] argsTail)
{
    if (CommandSpec(command) is not { } spec)
    {
        return null;
    }

    if (argsTail.Any(static token =>
            string.Equals(token, "--help", StringComparison.Ordinal) ||
            string.Equals(token, "-h", StringComparison.Ordinal) ||
            string.Equals(token, "-?", StringComparison.Ordinal)))
    {
        return ShowCommandHelp(command);
    }

    var values = new HashSet<string>(StringComparer.Ordinal);
    values.UnionWith(spec.Options.Where(static option => option.Value is not null).Select(static option => option.Name));
    values.UnionWith(GlobalValueOptions());
    var flags = new HashSet<string>(StringComparer.Ordinal);
    flags.UnionWith(spec.Options.Where(static option => option.Value is null).Select(static option => option.Name));
    flags.UnionWith(GlobalFlagOptions());

    for (var index = 0; index < argsTail.Length; index++)
    {
        var token = argsTail[index];
        if (values.Contains(token))
        {
            // Step over the value so an option argument that happens to look
            // like an option of its own is never rejected.
            index++;
            continue;
        }
        if (flags.Contains(token) || !token.StartsWith('-'))
        {
            continue;
        }

        Console.Error.WriteLine($"Unrecognized option '{token}' for command '{command}'.");
        WriteUsage(Console.Error, new[] { command });
        return 2;
    }

    return null;
}

ProgramImage LoadImage()
{
    var loadedProject = RequireProject();
    var dol = dolFile.Value;

    // Generate runtime configuration header with the manifest's SDA base pointers
    var (sda1Base, sda2Base) = loadedProject.RequireSdaBases();
    RuntimeConfigGenerator.GenerateConfigHeader(
        sda1Base,
        sda2Base,
        loadedProject.Output.RuntimeConfig,
        loadedProject.Identity.DisplayName);
    Console.WriteLine(
        $"[translator] SDA bases: r13 (_SDA_BASE_) 0x{sda1Base:X8}, r2 (_SDA2_BASE_) 0x{sda2Base:X8} " +
        $"(entry 0x{dol.EntryPoint:X8}).");

    var relImage = relFile.Value?.BuildImage(loadedProject.Inputs.Rel!.LoadAddress);
    return new ProgramImageBuilder().Build(dol, relImage, loadedProject.Memory.Base, loadedProject.Memory.Size);
}

DolFile LoadDol()
{
    return DolFile.Load(RequireProject().Inputs.Dol.Path);
}

RelFile? LoadRel()
{
    return RequireProject().Inputs.Rel is { } rel ? RelFile.Load(rel.Path) : null;
}

static uint ParseAddress(string text) => GuestTargetParser.ParseHexAddress(text);

// Small prewarm passes happen constantly (every discovered continuation wave);
// only a whole-image-sized one is worth a line of output.
static void ReportAbiPrewarm(GuestAbiPrewarmReport report)
{
    if (report.AddressCount <= 1024) return;
    Console.WriteLine(
        $"[translator] ABI prewarm: {report.AddressCount:N0} address(es); " +
        $"phase A {report.PhaseASeconds:F1}s, phase B {report.PhaseBSeconds:F1}s " +
        $"over {report.PhaseBComputedCount:N0} deferred address(es).");
}

string ResolveFunctionName(uint entry, string? preferred = null, string? fallback = null)
{
    var candidate = !string.IsNullOrWhiteSpace(preferred)
        ? preferred!
        : !string.IsNullOrWhiteSpace(fallback)
            ? fallback!
            : $"func_{entry:X8}";

    var entryToken = entry.ToString("X8");
    return NameContainsAddressToken(candidate, entryToken)
        ? candidate
        : $"{candidate}_{entryToken}";
}

static bool NameContainsAddressToken(string text, string addressToken)
{
    if (string.IsNullOrEmpty(text) || string.IsNullOrEmpty(addressToken))
    {
        return false;
    }

    return text.IndexOf(addressToken, StringComparison.OrdinalIgnoreCase) >= 0;
}

HashSet<uint> BuildKnownBaseFunctionEntryPoints(
    string generatedFunctionsDir,
    IEnumerable<uint> seeds,
    bool includeGeneratedHistory = true)
{
    var starts = new HashSet<uint>(seeds);
    if (includeGeneratedHistory)
    {
        foreach (var start in DiscoverFunctionStartAddressesFromFileNames(generatedFunctionsDir))
        {
            starts.Add(start);
        }
    }

    return starts;
}

static IEnumerable<uint> DiscoverFunctionStartAddressesFromFileNames(string generatedFunctionsDir)
{
    if (!Directory.Exists(generatedFunctionsDir))
    {
        yield break;
    }

    var addressRegex = new Regex("([0-9A-Fa-f]{8})", RegexOptions.CultureInvariant);
    foreach (var path in Directory.EnumerateFiles(generatedFunctionsDir, "*.cpp", SearchOption.AllDirectories))
    {
        var match = addressRegex.Match(Path.GetFileNameWithoutExtension(path));
        if (match.Success &&
            GuestTargetParser.TryParseHexAddress(match.Groups[1].Value, out var value))
        {
            yield return value;
        }
    }
}

static int ParseInt(string text) =>
    text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
        ? int.Parse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
        : int.Parse(text, CultureInfo.InvariantCulture);

static string? OptionValue(string[] argsTail, string name)
{
    var index = Array.IndexOf(argsTail, name);
    if (index >= 0 && index + 1 < argsTail.Length)
    {
        return argsTail[index + 1];
    }
    return null;
}

static (string? Value, string[] Remaining) ExtractOption(string[] values, string name)
{
    for (var index = 0; index < values.Length; index++)
    {
        if (!string.Equals(values[index], name, StringComparison.Ordinal))
        {
            continue;
        }
        if (index + 1 >= values.Length)
        {
            throw new ArgumentException($"{name} requires a value.");
        }

        var remaining = values.Where((_, itemIndex) => itemIndex != index && itemIndex != index + 1).ToArray();
        return (values[index + 1], remaining);
    }

    return (null, values);
}

static bool HasFlag(string[] argsTail, string name) => argsTail.Any(a => string.Equals(a, name, StringComparison.Ordinal));

/// <summary>
/// Addresses a leaf splice must never cover: the decoded bytes aren't what the runtime
/// executes there (native registration/fatal stub, exclusion, or a mod overlay rewrite).
/// </summary>
static IReadOnlySet<uint> BuildLeafInliningBlockedTargets(
    IReadOnlySet<uint> modPatchedAddresses,
    RuntimeNativeGuestEffectSet nativeGuestEffects,
    IReadOnlySet<uint> translationExclusions)
{
    var blocked = new HashSet<uint>(nativeGuestEffects.Contracts.Keys);
    blocked.UnionWith(translationExclusions);
    blocked.UnionWith(modPatchedAddresses);
    return blocked;
}

/// <summary>
/// Every enabled project profile with a Code.pul on disk, ordered by name. Enumerating all
/// of them (not just the selected one) keeps a profile-less base translation valid downstream.
/// </summary>
static IReadOnlyList<Translator.Cli.Configuration.ProjectProfile> CollectModPatchProfiles(
    Translator.Cli.Configuration.TranslationProjectConfig? project,
    Translator.Cli.Configuration.ProjectProfile? selectedProfile)
{
    var profiles = new Dictionary<string, Translator.Cli.Configuration.ProjectProfile>(StringComparer.OrdinalIgnoreCase);
    if (project is not null)
    {
        foreach (var candidate in project.Profiles.Values)
        {
            if (candidate.Enabled &&
                !string.IsNullOrWhiteSpace(candidate.CodePul) &&
                File.Exists(candidate.CodePul))
            {
                profiles[candidate.Name] = candidate;
            }
        }
    }
    // An explicitly selected profile always participates, even if a future
    // caller selects one that is marked disabled in the project file.
    if (selectedProfile is not null &&
        !string.IsNullOrWhiteSpace(selectedProfile.CodePul) &&
        File.Exists(selectedProfile.CodePul))
    {
        profiles[selectedProfile.Name] = selectedProfile;
    }

    return profiles.Values.OrderBy(static p => p.Name, StringComparer.OrdinalIgnoreCase).ToArray();
}

/// <summary>
/// The mod-patch awareness stamp recorded into the base translation output
/// metadata: one entry per profile whose patch set shaped this translation.
/// translate-mod refuses a base tree whose stamp does not cover its Code.pul.
/// </summary>
static IReadOnlyList<BaseTranslationModPatchAwareness> BuildModPatchAwareness(
    IReadOnlyList<Translator.Cli.Configuration.ProjectProfile> patchProfiles) =>
    patchProfiles
        .Select(static patchProfile => new BaseTranslationModPatchAwareness(
            patchProfile.Name,
            ChecksumUtilities.Sha256HexOfFile(patchProfile.CodePul!)))
        .ToArray();

/// <summary>
/// The same profiles, carrying the patch set itself rather than a digest of the file it came from.
/// <see cref="BaseTranslationModAwareness"/> narrows that set down to the addresses this translation
/// could actually see, which is what lets a later Code.pul reuse the completed base tree.
/// </summary>
static IReadOnlyList<(string Profile, string Region, string CodePulSha256, IReadOnlySet<uint> PatchedAddresses)>
    BuildModPatchAwarenessDetail(IReadOnlyList<Translator.Cli.Configuration.ProjectProfile> patchProfiles) =>
    patchProfiles
        .Select(static patchProfile => (
            Profile: patchProfile.Name,
            Region: patchProfile.Region ?? string.Empty,
            CodePulSha256: ChecksumUtilities.Sha256HexOfFile(patchProfile.CodePul!),
            PatchedAddresses: BaseTranslationModAwareness.PatchedAddresses(
                patchProfile.CodePul!, patchProfile.Region ?? string.Empty)))
        .ToArray();

/// <summary>
/// Union of every absolute Kamek command address across the given profiles'
/// Code.pul files: the addresses whose runtime winner can be a mod overlay.
/// </summary>
static IReadOnlySet<uint> BuildModPatchedAddresses(
    IReadOnlyList<Translator.Cli.Configuration.ProjectProfile> patchProfiles)
{
    var patched = new HashSet<uint>();
    foreach (var patchProfile in patchProfiles)
    {
        patched.UnionWith(BaseTranslationModAwareness.PatchedAddresses(
            patchProfile.CodePul!, patchProfile.Region ?? string.Empty));
    }

    return patched;
}

// Every emitting translation path (base, mod overlay, continuation, module, payload) must run
// its options through here so codegen policy can't diverge; mod paths once built TranslationOptions
// independently and silently shipped pre-residency codegen. Leaf inlining stays out of here since it
// rewrites IR before SSA and belongs to discovery (WithProjectFrontEndPolicy); mod, overlay and
// payload builds use a different image and stay un-inlined.
static TranslationOptions WithProjectCodegenPolicy(
    TranslationOptions options,
    ProjectTranslation translation,
    uint entryPoint) =>
    options with
    {
        AllowUnsupportedInstructions = translation.AllowUnsupportedInstructions,
        // Leaf ABI spill elision is structurally self-guarding: it requires a
        // matched save/restore pair inside the emitted body of a single-return
        // leaf, so a partial mod chunk that only carries one half of an ABI
        // prologue is rejected rather than mis-transformed.
        EnableLeafAbiSpillElision = true,
    };

/// <summary>
/// One option carried by one command. <paramref name="Value"/> is the metavariable
/// printed after the option name, or null for a flag; <paramref name="Required"/>
/// only controls whether usage brackets it.
/// </summary>
readonly record struct CommandOption(string Name, string? Value = null, bool Required = false);

sealed record ResolvedDispatchEntry(
    uint Address,
    string Symbol,
    string Name,
    string Kind,
    uint Priority,
    bool DirectCallAvailable,
    uint GprReadMask,
    uint GprWriteMask,
    uint GprClobberMask,
    uint GprReturnMask,
    uint FprReadMask,
    uint FprWriteMask,
    uint FprReturnMask,
    byte CrReadMask,
    byte CrWriteMask,
    bool ReadsXer,
    bool WritesXer,
    bool IsFullSynchronizationFence,
    bool PreservesNonvolatileFprs,
    uint NonvolatileFprWriteMask,
    bool MustRemainDynamicallyDispatchable,
    string SourceFile);


