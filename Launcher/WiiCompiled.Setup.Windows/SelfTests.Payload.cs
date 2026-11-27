using System.IO.Compression;
using System.Text.Json;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// Self-tests of the payload inventory: streamed-hash extraction, the stat check that lets an
/// install reuse unchanged components, publication recovery, and the identities derived from it.
/// </summary>
internal static partial class SelfTests
{
    private static void RegisterPayloadTests(List<string> failures)
    {
        Test("Stream and disk toolkit identity parity", TestStreamDiskIdentityParity, failures);
        Test("Release payload verification", TestReleasePayloadVerification, failures);
        Test("Payload inventory stat check", TestPayloadInventoryStatCheck, failures);
        Test("Asset-only and translator-only extraction", TestPartialPayloadExtraction, failures);
        Test("Published toolkit retry recovery", TestPublishedToolkitRecovery, failures);
        Test("Workspace caches survive commit and rollback", TestWorkspaceCachesHeldAcrossPublication, failures);
        Test("Preverified toolkit identities", TestPreverifiedToolkitIdentities, failures);
        Test("Narrowed cache identities", TestNarrowedCacheIdentities, failures);
        Test("Progress JSON stays monotonic", TestProgressJsonMonotonic, failures);
        Test("Upgrade from older toolkit state and manifest", TestUpgradeFixtures, failures);
    }

    private static readonly string[] FakeProjects = ["mkwii", "mkwii-ntsc-u", "mkwii-ntsc-j"];

    /// <summary>
    /// Stands in for <c>Translator.Cli list-translation-inputs</c>: the regional projects port from
    /// the PAL project, and one lists runtime files the native registrations already cover.
    /// </summary>
    private static IReadOnlyList<string> FakeTranslationInputs(string translator, string project,
        CancellationToken cancellationToken)
    {
        var directory = Path.GetDirectoryName(project)!;
        var projects = Path.GetDirectoryName(directory)!;
        var workspace = Path.GetDirectoryName(projects)!;
        var pal = Path.Combine(projects, "mkwii");
        return Path.GetFileName(directory) switch
        {
            "mkwii" =>
            [
                project, Path.Combine(directory, "MAP.txt"),
                // A profile's staged Code.pul: user-owned, only present on an installed workspace.
                Path.Combine(workspace, "PulsarPacks", "completed", "RetroRewind", "Code.pul")
            ],
            "mkwii-ntsc-u" =>
            [
                project, Path.Combine(directory, "region_port.json"), Path.Combine(pal, "recomp.yml"),
                Path.Combine(pal, "MAP.txt")
            ],
            _ =>
            [
                project, Path.Combine(workspace, "runtime", "include", "guest_abi", "guest_versions.inc"),
                Path.Combine(workspace, "runtime", "src", "main.cpp"),
                Path.Combine(workspace, "runtime", "native-index.json")
            ]
        };
    }

    /// <summary>The fake toolkit plus every file class the identities treat specially.</summary>
    private static void CreateFakePayload(string root)
    {
        CreateFakeToolkit(root);
        void Write(string relative, string content)
        {
            var path = Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
        }
        Write("BuildWorkspace/projects/mkwii/MAP.txt", "map");
        Write("BuildWorkspace/projects/mkwii-ntsc-u/region_port.json", "{}");
        Write("BuildWorkspace/aurora-main/docs/notes.md", "notes");
        Write("BuildWorkspace/aurora-main/.github/workflow.yml", "ci");
        Write("BuildWorkspace/aurora-main/.clang-format", "style");
        Write("BuildWorkspace/aurora-main/extern/README", "extern");
        Write("BuildWorkspace/runtime/third_party/cryptopp/LICENSE", "license");
        Write("BuildWorkspace/runtime/third_party/.hidden/state.bin", "hidden");
        Write("BuildWorkspace/runtime/include/guest_abi/guest_versions.inc", "versions");
        Write("BuildWorkspace/" + ToolkitFingerprint.NativeConfigureFlagsFileName, "-DCMAKE_BUILD_TYPE=Release");
        Write("BuildWorkspace/Dependencies/native_prebuilt/provenance.json", "{\"BuiltUtc\":\"1\"}");
        Write("Toolkit/CMake/share/cmake-4.3/Modules/Foo.cmake", "module");
        Write("Toolkit/CMake/doc/cmake/LICENSE.rst", "cmake license");
        Write("Toolkit/Redist/vcruntime140.dll", "redist");
        Write("Toolkit/Translator/Translator.Cli.pdb", "symbols");
        WriteFakeNativeIndex(root, "\"a\"");
    }

    /// <summary>A staged payload root plus its inventory, manifest and ZIP, built like Build-Installer.ps1.</summary>
    private sealed record FakeRelease(string Root, string Zip, PayloadManifest Manifest,
        ToolkitFingerprintComponents Disk);

    private static FakeRelease BuildFakeRelease(string directory, bool includeInventory = true)
    {
        var root = Path.Combine(directory, "payload");
        CreateFakePayload(root);
        var disk = ToolkitFingerprint.ComputeComponents(root, lister: FakeTranslationInputs,
            strictTranslationInputs: true);
        var packages = ToolkitFingerprint.ComputePackageComponents(root);
        var manifest = new PayloadManifest
        {
            SchemaVersion = PayloadManifest.CurrentSchemaVersion,
            ToolkitFingerprint = disk.Compile,
            ToolkitPackageFingerprint = packages.Package,
            ToolchainPackageFingerprint = packages.Toolchain,
            TranslatorPackageFingerprint = packages.Translator,
            RuntimeAssetsFingerprint = ToolkitFingerprint.ComputeRuntimeAssets(root),
            TranslationFingerprint = disk.Translation,
            NativeToolchainFingerprint = disk.NativeToolchain,
            TranslationFingerprintsByRelease = disk.TranslationByRelease.ToDictionary()
        };
        if (includeInventory)
        {
            var inventory = ConsoleCommands.BuildPayloadInventory(root, FakeTranslationInputs);
            inventory.ToolkitFingerprint = disk.Compile;
            inventory.ToolkitPackageFingerprint = packages.Package;
            inventory.Write(Path.Combine(root, PayloadInventory.FileName));
        }
        var zip = Path.Combine(directory, "payload.zip");
        ZipFile.CreateFromDirectory(root, zip);
        return new FakeRelease(root, zip, manifest, disk);
    }

    private static Dictionary<string, PayloadFileIdentity> ExtractRelease(PayloadArchive payload, string install)
    {
        var extracted = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        payload.ExtractDirectory(InstalledLayout.ToolkitDirectoryName, InstalledLayout.Toolkit(install), extracted);
        payload.ExtractDirectory(InstalledLayout.WorkspaceDirectoryName, InstalledLayout.Workspace(install),
            extracted);
        return extracted;
    }

    private static void WithTempDirectory(string name, Action<string> test)
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkwc-{name}-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try { test(directory); }
        finally
        {
            if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true);
        }
    }

    /// <summary>
    /// The install path derives every identity from the hashes it streamed out of the payload and
    /// the shipped inventory; repair derives them from the disk. Both must agree bit for bit across
    /// every selection rule (nodtool, runtime/assets, provenance.json, the widened aurora-main and
    /// runtime/third_party sets, the native index replacement, and the per-release input lists).
    /// </summary>
    private static void TestStreamDiskIdentityParity() => WithTempDirectory("parity", directory =>
    {
        var release = BuildFakeRelease(directory);
        if (release.Disk.TranslationByRelease.Count != FakeProjects.Length)
            throw new Exception("Not every translation project received a per-release identity.");

        using var payload = PayloadArchive.OpenZip(release.Zip);
        var inventory = payload.TryReadInventory(release.Manifest)
                        ?? throw new Exception("The shipped inventory was not read back.");
        var install = Path.Combine(directory, "install");
        var extracted = ExtractRelease(payload, install);
        inventory.VerifyExtracted(InstalledLayout.ToolkitEntryPrefix, extracted);
        inventory.VerifyExtracted(InstalledLayout.WorkspaceEntryPrefix, extracted);
        foreach (var (path, identity) in extracted)
        {
            var file = Path.Combine(install, path.Replace('/', Path.DirectorySeparatorChar));
            if (new FileInfo(file).Length != identity.Size ||
                InputValidation.Sha256File(file) != identity.Sha256)
                throw new Exception($"The streamed identity of {path} is not the extracted file's.");
        }

        var streamed = InstallerEngine.ComputeInventoryComponents(inventory, install, release.Manifest, _ => { })
                       ?? throw new Exception("The inventory did not reproduce the release identities.");
        var stagedPul = Path.Combine(install, "BuildWorkspace", "PulsarPacks", "completed", "RetroRewind", "Code.pul");
        Directory.CreateDirectory(Path.GetDirectoryName(stagedPul)!);
        File.WriteAllText(stagedPul, "user-owned pul");
        var installedDisk = ToolkitFingerprint.ComputeComponents(install, lister: FakeTranslationInputs,
            strictTranslationInputs: true);
        if (!streamed.SameIdentities(release.Disk) || !installedDisk.SameIdentities(release.Disk))
            throw new Exception("Stream-derived and disk-derived toolkit identities disagree.");
        if (ToolkitFingerprint.ComputePackageComponentsFromHashes(inventory.Hashes()) !=
            ToolkitFingerprint.ComputePackageComponents(install))
            throw new Exception("Stream-derived and disk-derived package identities disagree.");

        // Tampering with what was streamed must be caught, not folded into an identity.
        var tampered = new Dictionary<string, PayloadFileIdentity>(extracted)
        {
            ["Toolkit/Ninja/ninja.exe"] = new(5, new string('0', 64))
        };
        try
        {
            inventory.VerifyExtracted(InstalledLayout.ToolkitEntryPrefix, tampered);
            throw new Exception("A payload file that does not match the inventory was accepted.");
        }
        catch (InvalidDataException) { }
        var unlisted = new Dictionary<string, PayloadFileIdentity>(extracted)
        {
            ["Toolkit/extra.dll"] = new(1, new string('1', 64))
        };
        try
        {
            inventory.VerifyExtracted(InstalledLayout.ToolkitEntryPrefix, unlisted);
            throw new Exception("A payload file missing from the inventory was accepted.");
        }
        catch (InvalidDataException) { }

        // A manifest the inventory cannot reproduce discards the shortcut instead of trusting it.
        var foreign = new PayloadManifest
        {
            SchemaVersion = 3, ToolkitFingerprint = new string('9', 64),
            ToolkitPackageFingerprint = release.Manifest.ToolkitPackageFingerprint
        };
        if (InstallerEngine.ComputeInventoryComponents(inventory, install, foreign, _ => { }) is not null)
            throw new Exception("Inventory identities were accepted for a different release.");
        // A native index that no longer hashes to its entry must not be trusted for the shortcut.
        File.AppendAllText(Path.Combine(install, "BuildWorkspace", "runtime", "native-index.json"), " ");
        if (InstallerEngine.ComputeInventoryComponents(inventory, install, release.Manifest, _ => { }) is not null)
            throw new Exception("A modified native index was trusted for the inventory identities.");
    });

    /// <summary>
    /// The release-build check accepts a payload whose archive matches its inventory, and rejects
    /// one that gained, lost or changed a file after the inventory was written.
    /// </summary>
    private static void TestReleasePayloadVerification() => WithTempDirectory("verify", directory =>
    {
        var release = BuildFakeRelease(directory);
        using (var payload = PayloadArchive.OpenZip(release.Zip))
        {
            var count = ConsoleCommands.VerifyPayloadInventory(payload, release.Manifest);
            if (count != Directory.EnumerateFiles(release.Root, "*", SearchOption.AllDirectories)
                    .Count(file => !Path.GetFileName(file).Equals(PayloadInventory.FileName, StringComparison.Ordinal)))
                throw new Exception("The release check did not hash every Toolkit and BuildWorkspace file.");
        }

        void Reject(string description, Action<ZipArchive> corrupt)
        {
            var zip = Path.Combine(directory, $"corrupt-{Guid.NewGuid():N}.zip");
            File.Copy(release.Zip, zip);
            using (var archive = ZipFile.Open(zip, ZipArchiveMode.Update)) corrupt(archive);
            using var payload = PayloadArchive.OpenZip(zip);
            try
            {
                ConsoleCommands.VerifyPayloadInventory(payload, release.Manifest);
                throw new Exception($"The release check accepted a payload with {description}.");
            }
            catch (InvalidDataException) { }
        }

        Reject("a changed file", archive =>
        {
            var entry = archive.Entries.First(e => e.FullName.Replace('\\', '/') == "Toolkit/Ninja/ninja.exe");
            entry.Delete();
            using var writer = new StreamWriter(archive.CreateEntry("Toolkit/Ninja/ninja.exe").Open());
            writer.Write("different bytes");
        });
        Reject("a missing file", archive =>
            archive.Entries.First(e => e.FullName.Replace('\\', '/') == "BuildWorkspace/projects/mkwii/MAP.txt").Delete());
        Reject("an unlisted file", archive =>
        {
            using var writer = new StreamWriter(archive.CreateEntry("BuildWorkspace/extra.txt").Open());
            writer.Write("extra");
        });
        Reject("no inventory", archive =>
            archive.Entries.First(e => e.FullName.Replace('\\', '/') == PayloadInventory.FileName).Delete());
    });

    private static void TestPayloadInventoryStatCheck() => WithTempDirectory("statcheck", directory =>
    {
        var release = BuildFakeRelease(directory);
        using var payload = PayloadArchive.OpenZip(release.Zip);
        var inventory = payload.TryReadInventory(release.Manifest)!;
        var install = Path.Combine(directory, "install");
        ExtractRelease(payload, install);

        void Expect(bool clean, string description, string prefix = "Toolkit/", bool timestamps = true,
            IReadOnlyCollection<string>? excluded = null)
        {
            var problems = inventory.StatCheck(install, prefix, timestamps, excluded);
            if (clean != (problems.Count == 0))
                throw new Exception(description + (problems.Count == 0 ? "" : ": " + string.Join("; ", problems)));
        }

        Expect(true, "A freshly extracted toolkit failed its stat check");
        Expect(true, "A freshly extracted workspace failed its stat check", "BuildWorkspace/", timestamps: false);

        var cache = Path.Combine(install, "BuildWorkspace", "generated", "cache.bin");
        Directory.CreateDirectory(Path.GetDirectoryName(cache)!);
        File.WriteAllText(cache, "cache");
        Expect(true, "A build cache was reported as foreign workspace content", "BuildWorkspace/", false);

        var foreign = Path.Combine(install, "BuildWorkspace", "runtime", "foreign.txt");
        File.WriteAllText(foreign, "foreign");
        Expect(false, "An unexpected workspace file was accepted", "BuildWorkspace/", false);
        File.Delete(foreign);
        var dependency = Path.Combine(install, "BuildWorkspace", "Dependencies", "zlib", ".mkwc-layout");
        File.Delete(dependency);
        Expect(false, "A quarantined dependency file was accepted", "BuildWorkspace/", false);
        File.WriteAllText(dependency, "zlib");
        Expect(true, "A restored dependency file was rejected", "BuildWorkspace/", false);

        var ninja = Path.Combine(install, "Toolkit", "Ninja", "ninja.exe");
        var ninjaBytes = File.ReadAllBytes(ninja);
        File.Delete(ninja);
        Expect(false, "A missing toolkit file was accepted");
        File.WriteAllBytes(ninja, ninjaBytes);
        PayloadArchive.NormalizeExtractedTimestamp(ninja);
        Expect(true, "A restored toolkit file was rejected");

        var cmake = Path.Combine(install, "Toolkit", "CMake", "bin", "cmake.exe");
        File.AppendAllText(cmake, "!");
        PayloadArchive.NormalizeExtractedTimestamp(cmake);
        Expect(false, "A toolkit file of the wrong size was accepted");
        File.WriteAllText(cmake, "cmake");
        PayloadArchive.NormalizeExtractedTimestamp(cmake);

        var clang = Path.Combine(install, "Toolkit", "llvm-mingw", "bin", "clang-22.exe");
        File.SetLastWriteTimeUtc(clang, DateTime.UtcNow);
        Expect(false, "A toolkit file rewritten in place (same size) was accepted");
        Expect(true, "The timestamp rule was applied where it was not requested", timestamps: false);
        PayloadArchive.NormalizeExtractedTimestamp(clang);
        // A FAT32/exFAT volume reads the normalized stamp back shifted by the UTC offset.
        File.SetLastWriteTimeUtc(clang, PayloadInventory.NormalizedTimestampUtc.AddHours(-9));
        Expect(true, "A normalized timestamp shifted by a UTC offset was rejected");
        PayloadArchive.NormalizeExtractedTimestamp(clang);

        var extra = Path.Combine(install, "Toolkit", "llvm-mingw", "bin", "injected.dll");
        File.WriteAllText(extra, "injected");
        PayloadArchive.NormalizeExtractedTimestamp(extra);
        Expect(false, "A foreign toolkit file was accepted");
        File.Delete(extra);

        var translator = Path.Combine(install, "Toolkit", "Translator", "Translator.Cli.exe");
        File.WriteAllText(translator, "a different translator build");
        Expect(false, "A changed translator passed the whole-toolkit stat check");
        Expect(true, "The toolchain stat check looked at the translator",
            excluded: [ToolkitFingerprint.TranslatorPrefix]);

        var link = Path.Combine(install, "Toolkit", "linked");
        if (TryCreateDirectoryLink(link, Path.Combine(install, "Toolkit", "Ninja")))
        {
            try
            {
                Expect(false, "A linked directory inside the toolkit was accepted",
                    excluded: [ToolkitFingerprint.TranslatorPrefix]);
            }
            finally
            {
                Directory.Delete(link);
            }
        }
    });

    /// <summary>
    /// An unchanged toolkit with stale runtime assets extracts only runtime/assets, and a
    /// translator-only update extracts and publishes only Toolkit/Translator, leaving the installed
    /// toolchain untouched.
    /// </summary>
    private static void TestPartialPayloadExtraction() => WithTempDirectory("partial", directory =>
    {
        var release = BuildFakeRelease(directory);
        using var payload = PayloadArchive.OpenZip(release.Zip);
        var inventory = payload.TryReadInventory(release.Manifest)!;

        var assetsOnly = Path.Combine(directory, "assets-only");
        var extracted = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        payload.ExtractDirectory("BuildWorkspace/runtime/assets/",
            Path.Combine(InstalledLayout.Workspace(assetsOnly), "runtime", "assets"), extracted);
        inventory.VerifyExtracted("BuildWorkspace/runtime/assets/", extracted);
        if (extracted.Count == 0 || extracted.Keys.Any(path =>
                !path.StartsWith("BuildWorkspace/runtime/assets/", StringComparison.Ordinal)))
            throw new Exception("The asset-only extraction extracted something other than runtime/assets.");
        if (Directory.EnumerateFiles(assetsOnly, "*", SearchOption.AllDirectories).Count() != extracted.Count ||
            ToolkitFingerprint.ComputeRuntimeAssets(assetsOnly) != release.Manifest.RuntimeAssetsFingerprint)
            throw new Exception("The asset-only extraction does not reproduce the release's runtime assets.");

        // An installation with the previous translator.
        var install = Path.Combine(directory, "install");
        ExtractRelease(payload, install);
        var translator = Path.Combine(install, "Toolkit", "Translator", "Translator.Cli.exe");
        File.WriteAllText(translator, "previous translator");
        var compiler = Path.Combine(install, "Toolkit", "llvm-mingw", "bin", "clang-22.exe");
        var compilerStamp = File.GetLastWriteTimeUtc(compiler);

        var staging = Path.Combine(install, "staging");
        var stagedToolkit = InstalledLayout.Toolkit(staging);
        extracted.Clear();
        payload.ExtractDirectory(ToolkitFingerprint.TranslatorPrefix, Path.Combine(stagedToolkit, "Translator"),
            extracted);
        inventory.VerifyExtracted(ToolkitFingerprint.TranslatorPrefix, extracted);
        if (extracted.Keys.Any(path => !path.StartsWith(ToolkitFingerprint.TranslatorPrefix, StringComparison.Ordinal)))
            throw new Exception("The translator-only extraction extracted toolchain files.");
        var entries = new List<InstallTransactionEntry>();
        InstallerEngine.AddToolkitEntries(entries, ToolkitRefresh.TranslatorOnly, staging, install);
        if (entries.Count != 1 || !entries[0].DestinationPath.EndsWith(Path.Combine("Toolkit", "Translator"),
                StringComparison.Ordinal))
            throw new Exception("A translator-only update did not publish exactly Toolkit/Translator.");
        using (var transaction = InstallTransaction.Begin(install, null, entries.ToArray()))
        {
            transaction.Publish();
            transaction.Commit();
        }
        if (File.ReadAllText(translator) != "translator-v1" || File.GetLastWriteTimeUtc(compiler) != compilerStamp)
            throw new Exception("The translator-only publication did not replace exactly the translator.");
        if (inventory.StatCheck(install, InstalledLayout.ToolkitEntryPrefix, true).Count != 0)
            throw new Exception("The toolkit is not the release's after a translator-only update.");

        // The transaction refuses overlapping roots, which is why the plans never combine them.
        var overlapping = new[]
        {
            InstallTransactionEntry.Directory(Path.Combine(directory, "payload", "Toolkit"),
                InstalledLayout.Toolkit(install)),
            InstallTransactionEntry.Directory(Path.Combine(directory, "payload", "Toolkit", "Translator"),
                Path.Combine(InstalledLayout.Toolkit(install), "Translator"))
        };
        try
        {
            using var transaction = InstallTransaction.Begin(install, null, overlapping);
            throw new Exception("Overlapping Toolkit and Toolkit/Translator entries were accepted.");
        }
        catch (Exception ex) when (ex is InvalidOperationException or ArgumentException or InvalidDataException) { }
    });

    /// <summary>
    /// Security software removing published files (under Toolkit/ or BuildWorkspace/Dependencies/)
    /// must be repaired by one more publication of just the damaged roots, keeping the build caches,
    /// instead of leaving the user re-running setup into the same failure.
    /// </summary>
    /// <summary>
    /// Build caches are held outside both workspaces while one is published, and end up in whichever
    /// workspace is installed afterwards: the new one after a commit, the old one after a rollback.
    /// </summary>
    private static void TestWorkspaceCachesHeldAcrossPublication() => WithTempDirectory("caches", directory =>
    {
        foreach (var committed in new[] { true, false })
        {
            var installed = Path.Combine(directory, $"installed-{committed}");
            var staged = Path.Combine(directory, $"staged-{committed}");
            var held = Path.Combine(directory, $"held-{committed}");
            var cache = Path.Combine(installed, "native-build", "CMakeCache.txt");
            Directory.CreateDirectory(Path.GetDirectoryName(cache)!);
            File.WriteAllText(cache, "kept");
            Directory.CreateDirectory(Path.Combine(staged, "generated"));
            File.WriteAllText(Path.Combine(installed, "shipped.txt"), "old");
            File.WriteAllText(Path.Combine(staged, "shipped.txt"), "new");

            InstallerEngine.HoldWorkspaceCaches(installed, staged, held, CancellationToken.None);
            if (Directory.Exists(Path.Combine(installed, "native-build")) || Directory.Exists(Path.Combine(staged, "generated")))
                throw new Exception("Caches were left in a workspace being replaced.");
            if (committed)
            {
                Directory.Delete(installed, recursive: true);
                Directory.Move(staged, installed);
            }
            InstallerEngine.RestoreWorkspaceCaches(installed, held);

            if (!File.Exists(cache) || File.ReadAllText(cache) != "kept")
                throw new Exception($"The build caches were lost after a {(committed ? "commit" : "rollback")}.");
            if (File.ReadAllText(Path.Combine(installed, "shipped.txt")) != (committed ? "new" : "old"))
                throw new Exception("The restored caches landed in the wrong workspace.");
        }
    });

    private static void TestPublishedToolkitRecovery() => WithTempDirectory("recovery", directory =>
    {
        var release = BuildFakeRelease(directory);
        using var payload = PayloadArchive.OpenZip(release.Zip);
        var inventory = payload.TryReadInventory(release.Manifest)!;
        var install = Path.Combine(directory, "install");
        ExtractRelease(payload, install);
        var cache = Path.Combine(install, "BuildWorkspace", "native-build", "CMakeCache.txt");
        Directory.CreateDirectory(Path.GetDirectoryName(cache)!);
        File.WriteAllText(cache, "kept");

        var engine = new InstallerEngine(new DelegatingInstallReporter(_ => { }));
        InstallerEngine.InstallContext Context() => new(new Installation(install), Path.Combine(install, "staging"),
            payload, release.Manifest, inventory, new GameRelease(), null,
            new InstallOptions { GamePath = "game.iso", InstallDirectory = install }, null, null, false);

        var intact = engine.EnsurePublishedToolkitIntact(Context(), CancellationToken.None)
                     ?? throw new Exception("An intact publication did not yield verified identities.");
        if (!intact.SameIdentities(release.Disk))
            throw new Exception("The verified identities are not the release's.");

        var ninja = Path.Combine(install, "Toolkit", "Ninja", "ninja.exe");
        var dependency = Path.Combine(install, "BuildWorkspace", "Dependencies", "zlib", ".mkwc-layout");
        File.Delete(ninja);
        File.Delete(dependency);
        var recovered = engine.EnsurePublishedToolkitIntact(Context(), CancellationToken.None);
        if (!File.Exists(ninja) || !File.Exists(dependency) || File.ReadAllText(cache) != "kept")
            throw new Exception("The damaged roots were not republished with the build caches kept.");
        if (recovered is null || !recovered.SameIdentities(release.Disk))
            throw new Exception("A recovered publication did not yield the release's identities.");
        if (inventory.StatCheck(install, "Toolkit/", true).Count != 0 ||
            inventory.StatCheck(install, "BuildWorkspace/", false).Count != 0)
            throw new Exception("The recovered installation still differs from the release.");

        // Damage only the workspace: the toolkit must not be republished needlessly.
        var compiler = Path.Combine(install, "Toolkit", "llvm-mingw", "bin", "clang-22.exe");
        var toolkitNode = new FileInfo(compiler).CreationTimeUtc;
        File.Delete(dependency);
        engine.EnsurePublishedToolkitIntact(Context(), CancellationToken.None);
        if (!File.Exists(dependency) || new FileInfo(compiler).CreationTimeUtc != toolkitNode)
            throw new Exception("Workspace-only damage republished the toolkit too.");
    });

    private static void TestPreverifiedToolkitIdentities() => WithTempDirectory("preverified", root =>
    {
        CreateFakeToolkit(root);
        var fingerprint = ToolkitFingerprint.Compute(root);
        var installation = new Installation(root);
        installation.WriteToolkitState(fingerprint, ToolkitFingerprint.ComputePackage(root), "wiicompiled-v1",
            ToolkitFingerprint.ComputeRuntimeAssets(root));
        var repair = new ProductRepairService(installation, new DelegatingInstallReporter(_ => { }));
        var preverified = new ToolkitFingerprintComponents(fingerprint, "translation", "native");

        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp"), "tampered");
        if (!ReferenceEquals(repair.VerifyToolkitForCompilation(fingerprint, preverified: preverified), preverified))
            throw new Exception("Identities proved by the installing operation were not used.");
        try
        {
            repair.VerifyToolkitForCompilation(fingerprint);
            throw new Exception("A modified toolkit was accepted without preverified identities.");
        }
        catch (InvalidDataException ex)
        {
            if (!ToolkitIntegrity.IsFailure(ex))
                throw new Exception("A toolkit integrity failure is not recognizable as one, so setup could not recover.");
        }
        try
        {
            repair.VerifyToolkitForCompilation(fingerprint,
                preverified: preverified with { Compile = new string('0', 64) });
            throw new Exception("Preverified identities of a different toolkit skipped verification.");
        }
        catch (InvalidDataException) { }
    });

    /// <summary>
    /// The native-toolchain identity follows what compiles objects, not the scripts' source text;
    /// the compile identity covers every byte the precompiled-package gate hashes; the package halves
    /// split exactly at Toolkit/Translator; and each release's translation identity follows only the
    /// project files that release reads.
    /// </summary>
    private static void TestNarrowedCacheIdentities() => WithTempDirectory("narrowed", directory =>
    {
        var root = Path.Combine(directory, "payload");
        CreateFakePayload(root);
        ToolkitFingerprintComponents Components() =>
            ToolkitFingerprint.ComputeComponents(root, lister: FakeTranslationInputs, strictTranslationInputs: true);
        string PathOf(string relative) => Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
        var before = Components();

        void Change(string relative, bool compile, bool translation, bool native, string description)
        {
            var path = PathOf(relative);
            var original = File.ReadAllText(path);
            var previous = Components();
            File.WriteAllText(path, original + " changed");
            var after = Components();
            File.WriteAllText(path, original);
            if ((after.Compile != previous.Compile) != compile ||
                (after.Translation != previous.Translation) != translation ||
                (after.NativeToolchain != previous.NativeToolchain) != native)
                throw new Exception(description);
        }

        Change("BuildWorkspace/LocalBuild.ps1", compile: true, translation: true, native: false,
            "The build script's source text decided the native-toolchain identity.");
        Change("BuildWorkspace/NativeBuildFlags.ps1", compile: true, translation: false, native: false,
            "The flags script's source text decided the native-toolchain identity.");
        Change("BuildWorkspace/" + ToolkitFingerprint.NativeConfigureFlagsFileName, compile: true,
            translation: false, native: true, "A configure flag change kept the native build directory.");
        Change("Toolkit/Redist/vcruntime140.dll", compile: true, translation: false, native: false,
            "A copied runtime DLL decided the native-toolchain identity.");
        Change("Toolkit/CMake/doc/cmake/LICENSE.rst", compile: true, translation: false, native: false,
            "CMake documentation decided the native-toolchain identity.");
        Change("Toolkit/CMake/share/cmake-4.3/Modules/Foo.cmake", compile: true, translation: false, native: true,
            "A CMake module change kept the native build directory.");
        Change("Toolkit/Ninja/ninja.exe", compile: true, translation: false, native: true,
            "A Ninja change kept the native build directory.");
        Change("BuildWorkspace/aurora-main/docs/notes.md", compile: true, translation: false, native: false,
            "An aurora-main file the precompiled-package gate hashes is outside the compile identity.");
        Change("BuildWorkspace/aurora-main/extern/README", compile: true, translation: false, native: false,
            "An aurora-main/extern file is outside the compile identity.");
        Change("BuildWorkspace/runtime/third_party/cryptopp/LICENSE", compile: true, translation: false,
            native: false, "A vendored file the precompiled-package gate hashes is outside the compile identity.");
        Change("BuildWorkspace/aurora-main/.github/workflow.yml", compile: true, translation: false, native: false,
            "A source-extension aurora-main file left the compile identity.");
        Change("BuildWorkspace/aurora-main/.clang-format", compile: false, translation: false, native: false,
            "A dot file no identity consumes changed the compile identity.");
        Change("BuildWorkspace/runtime/third_party/.hidden/state.bin", compile: false, translation: false,
            native: false, "A vendored dot file changed the compile identity.");
        Change("Toolkit/nodtool.exe", compile: false, translation: false, native: false,
            "nodtool changed the compile identity.");
        Change("BuildWorkspace/Dependencies/native_prebuilt/provenance.json", compile: false, translation: false,
            native: false, "A native_prebuilt provenance rewrite changed the compile identity.");

        // Per-release translation identities follow only that release's project inputs.
        void ChangeRelease(string relative, string[] changed, string description)
        {
            var path = PathOf(relative);
            var original = File.ReadAllText(path);
            var previous = Components();
            File.WriteAllText(path, original + " changed");
            var after = Components();
            File.WriteAllText(path, original);
            foreach (var project in FakeProjects)
                if ((after.TranslationByRelease[project] != previous.TranslationByRelease[project]) !=
                    changed.Contains(project))
                    throw new Exception(description + $" ({project})");
            if (after.Translation == previous.Translation)
                throw new Exception("A translation project change kept the whole-project translation identity.");
        }

        ChangeRelease("BuildWorkspace/projects/mkwii-ntsc-j/recomp.yml", ["mkwii-ntsc-j"],
            "One release's project change invalidated another release's translation.");
        ChangeRelease("BuildWorkspace/projects/mkwii/MAP.txt", ["mkwii", "mkwii-ntsc-u"],
            "A PAL map change did not follow the regional project that ports from it.");
        var registrations = Components();
        File.WriteAllText(PathOf("BuildWorkspace/runtime/src/main.cpp"), "int main_changed();");
        WriteFakeNativeIndex(root, "\"a\"");
        var sameRegistrations = Components();
        if (!FakeProjects.All(project =>
                sameRegistrations.TranslationByRelease[project] == registrations.TranslationByRelease[project]))
            throw new Exception("A runtime change with unchanged registrations invalidated a release translation.");
        WriteFakeNativeIndex(root, "\"a\", \"b\"");
        var newRegistrations = Components();
        if (FakeProjects.Any(project =>
                newRegistrations.TranslationByRelease[project] == registrations.TranslationByRelease[project]))
            throw new Exception("A native registration change kept a release translation.");

        // A lister that fails, or names a file outside the toolkit, can only cost a retranslation.
        var failed = ToolkitFingerprint.ComputeComponents(root,
            lister: (_, _, _) => throw new InvalidDataException("unavailable"));
        if (failed.TranslationByRelease.Count != 0 || failed.Compile != newRegistrations.Compile)
            throw new Exception("A failing input lister produced per-release identities.");
        try
        {
            ToolkitFingerprint.ComputeComponents(root, lister: (_, _, _) => [Path.Combine(directory, "outside.txt")],
                strictTranslationInputs: true);
            throw new Exception("A translation input outside the toolkit was accepted at release-build time.");
        }
        catch (InvalidDataException) { }

        // A rejected compile identity stops the walk before the toolkit's translator is started.
        var listerRan = false;
        try
        {
            ToolkitFingerprint.ComputeComponents(root,
                lister: (_, _, _) => { listerRan = true; return []; },
                verifyCompile: compile =>
                {
                    if (compile != newRegistrations.Compile)
                        throw new Exception("The early compile identity differs from the final one.");
                    throw new InvalidDataException("rejected");
                });
            throw new Exception("A rejected compile identity was not thrown.");
        }
        catch (InvalidDataException) { }
        if (listerRan) throw new Exception("The translator lister ran after the compile identity was rejected.");

        // Package halves.
        var packages = ToolkitFingerprint.ComputePackageComponents(root);
        File.WriteAllText(PathOf("Toolkit/Translator/Translator.Cli.exe"), "translator-v9");
        var translatorChanged = ToolkitFingerprint.ComputePackageComponents(root);
        if (translatorChanged.Package == packages.Package || translatorChanged.Translator == packages.Translator ||
            translatorChanged.Toolchain != packages.Toolchain)
            throw new Exception("A translator change did not split exactly at Toolkit/Translator.");
        File.WriteAllText(PathOf("Toolkit/nodtool.exe"), "nodtool-v9");
        var toolChanged = ToolkitFingerprint.ComputePackageComponents(root);
        if (toolChanged.Toolchain == translatorChanged.Toolchain ||
            toolChanged.Translator != translatorChanged.Translator)
            throw new Exception("A toolchain-side change did not split exactly at Toolkit/Translator.");

        // The selection is a superset of the precompiled-package gate's tree hash.
        foreach (var relative in new[]
                 {
                     "BuildWorkspace/aurora-main/LICENSE", "BuildWorkspace/aurora-main/lib/x/font.ttf",
                     "BuildWorkspace/aurora-main/build/notes", "BuildWorkspace/runtime/third_party/a/b.bin"
                 })
            if (!ToolkitFingerprint.IsCompileInput(relative))
                throw new Exception($"{relative} is hashed by the precompiled-package gate but not by the compile identity.");
        if (before.Compile == Components().Compile)
            throw new Exception("The sequence of changes above did not change the compile identity.");
    });

    /// <summary>
    /// Wheel Wizard renders the NDJSON stream as a progress bar: steps a release skips must never make
    /// it walk backwards, and stage identifiers are passed through unchanged.
    /// </summary>
    private static void TestProgressJsonMonotonic()
    {
        var original = Console.Out;
        var captured = new StringWriter();
        Console.SetOut(captured);
        try
        {
            var reporter = new NdjsonInstallReporter("1.0.0");
            reporter.Progress(InstallStages.Validate, "a", 1);
            reporter.Progress(InstallStages.ExtractToolkit, "b", 4);
            reporter.Progress(InstallStages.PrepareRetro, "c", 3);
            reporter.Progress(InstallStages.Publish, "d", 10);
            reporter.Progress(InstallStages.BuildBase, "e", 5);
            reporter.Progress(InstallStages.Publish, "f", 99);
            reporter.Success("C:\\install");
        }
        finally
        {
            Console.SetOut(original);
        }

        var lines = captured.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries);
        var progress = lines.Select(line => JsonDocument.Parse(line).RootElement)
            .Where(element => element.GetProperty("type").GetString() == "progress").ToList();
        var percents = progress.Select(element => element.GetProperty("percent").GetInt32()).ToList();
        var stages = progress.Select(element => element.GetProperty("stage").GetString()).ToList();
        if (!percents.SequenceEqual([1, 4, 4, 10, 10, 99]))
            throw new Exception("Progress percentages were not clamped monotonic: " + string.Join(",", percents));
        if (!stages.SequenceEqual([
                InstallStages.Validate, InstallStages.ExtractToolkit, InstallStages.PrepareRetro,
                InstallStages.Publish, InstallStages.BuildBase, InstallStages.Publish
            ]))
            throw new Exception("Progress stage identifiers were not passed through unchanged.");
        if (JsonDocument.Parse(lines[^1]).RootElement.GetProperty("type").GetString() != "result")
            throw new Exception("The result line is not the last line.");
    }

    /// <summary>
    /// Installations and payloads from before the inventory and the split identities: everything
    /// they lack must fall back to the whole-toolkit behaviour, never to a guess.
    /// </summary>
    private static void TestUpgradeFixtures() => WithTempDirectory("upgrade", directory =>
    {
        // toolkit-state.json as v0.9.x wrote it.
        var statePath = Path.Combine(directory, ToolkitState.FileName);
        File.WriteAllText(statePath, """
            {
              "SchemaVersion": 2,
              "ToolkitFingerprint": "aaaa",
              "ToolkitPackageFingerprint": "bbbb",
              "RuntimeAssetsFingerprint": "cccc",
              "TranslationFingerprint": "dddd",
              "NativeToolchainFingerprint": "eeee",
              "ToolkitReleaseTag": "v0.9.5",
              "SetupVersion": "0.9.5",
              "UpdatedUtc": "2026-01-01T00:00:00+00:00"
            }
            """);
        var state = JsonState.TryRead<ToolkitState>(statePath) ?? throw new Exception("An older toolkit state was rejected.");
        if (state.ToolkitPackageFingerprint != "bbbb" || state.ToolchainPackageFingerprint != "" ||
            state.TranslatorPackageFingerprint != "" || state.TranslationFingerprintsByRelease is not { Count: 0 } ||
            state.NativePrebuiltGateProvenanceSha256 != "")
            throw new Exception("An older toolkit state did not read back with empty additive fields.");
        // ...so a changed package can never take the translator-only path.
        if (InstallerEngine.PlanToolkitRefresh(true, false, false, true, () => true,
                sameToolchainPackage: !string.IsNullOrEmpty(state.ToolchainPackageFingerprint), () => true) !=
            ToolkitRefresh.Full)
            throw new Exception("An installation without split package identities skipped a toolkit refresh.");

        // The current state round-trips its additive fields.
        var current = new ToolkitState
        {
            ToolkitFingerprint = "a", ToolkitPackageFingerprint = "b", ToolchainPackageFingerprint = "c",
            TranslatorPackageFingerprint = "d", NativePrebuiltGateProvenanceSha256 = "e",
            TranslationFingerprintsByRelease = new Dictionary<string, string> { ["mkwii"] = "f" }
        };
        JsonState.Write(statePath, current);
        var readBack = JsonState.TryRead<ToolkitState>(statePath)!;
        if (readBack.ToolchainPackageFingerprint != "c" || readBack.TranslatorPackageFingerprint != "d" ||
            readBack.NativePrebuiltGateProvenanceSha256 != "e" ||
            readBack.TranslationFingerprintsByRelease.GetValueOrDefault("mkwii") != "f")
            throw new Exception("The additive toolkit-state fields do not round-trip.");

        // A schema-3 manifest without the additive identities.
        var manifestJson = """
            {
                "SchemaVersion": 3, "ProductVersion": "0.9.5",
                "SupportedReleases": [ { "GameId": "RMCP01", "DolSha256": "{{DOL}}", "RelSha256": "{{REL}}" } ],
                "ToolkitReleaseTag": "", "BuildModel": "local-translation-and-compilation",
                "ToolkitFingerprint": "toolkit", "ToolkitPackageFingerprint": "package",
                "RuntimeAssetsFingerprint": "assets", "TranslationFingerprint": "translation",
                "NativeToolchainFingerprint": "native"
            }
            """.Replace("{{DOL}}", new string('a', 64)).Replace("{{REL}}", new string('b', 64));
        using (var stream = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(manifestJson)))
        {
            var manifest = PayloadArchive.ParseManifest(stream);
            if (manifest.ToolchainPackageFingerprint != "" || manifest.TranslationFingerprintsByRelease is not { Count: 0 } ||
                manifest.NativePrebuiltGateProvenanceSha256 != "")
                throw new Exception("A manifest without additive identities did not read back empty.");
        }

        // A payload without an inventory, or with one for another release, gets no shortcut.
        var release = BuildFakeRelease(Path.Combine(directory, "no-inventory"), includeInventory: false);
        using (var payload = PayloadArchive.OpenZip(release.Zip))
            if (payload.TryReadInventory(release.Manifest) is not null)
                throw new Exception("A payload without an inventory produced one.");
        var withInventory = BuildFakeRelease(Path.Combine(directory, "inventory"));
        using (var payload = PayloadArchive.OpenZip(withInventory.Zip))
        {
            var other = new PayloadManifest
            {
                ToolkitFingerprint = withInventory.Manifest.ToolkitFingerprint, ToolkitPackageFingerprint = "other"
            };
            if (payload.TryReadInventory(other) is not null)
                throw new Exception("An inventory describing another toolkit package was trusted.");
        }

        // The build-script argument carries only names it can pass through unambiguously.
        var hex = new string('c', 64);
        var formatted = LocalBuildService.FormatTranslationFingerprintsByRelease(new Dictionary<string, string>
        {
            ["mkwii-ntsc-u"] = hex, ["mkwii"] = hex, ["bad;name"] = hex, ["mkwii-ntsc-j"] = "short"
        });
        if (formatted != $"mkwii={hex};mkwii-ntsc-u={hex}")
            throw new Exception("The per-release translation identities were not formatted canonically: " + formatted);
    });
}
