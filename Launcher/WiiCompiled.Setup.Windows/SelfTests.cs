using System.Buffers.Binary;
using System.Security.Cryptography;
using Tomlyn;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

internal static class SelfTests
{
    public static int Run()
    {
        var failures = new List<string>();
        Test("ISO extension", () => TestAcceptedExtension(".iso"), failures);
        Test("GCM extension", () => TestAcceptedExtension(".gcm"), failures);
        Test("GCZ extension", () => TestAcceptedExtension(".gcz"), failures);
        Test("CISO extension", () => TestAcceptedExtension(".ciso"), failures);
        Test("WBFS extension", () => TestAcceptedExtension(".wbfs"), failures);
        Test("WIA extension", () => TestAcceptedExtension(".wia"), failures);
        Test("RVZ extension", () => TestAcceptedExtension(".RVZ"), failures);
        Test("Unsupported extension", () =>
        {
            var path = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".nfs");
            File.WriteAllBytes(path, [0]);
            try
            {
                try { InputValidation.ValidateExtension(path); throw new Exception("NFS was accepted."); }
                catch (InvalidDataException) { }
            }
            finally { File.Delete(path); }
        }, failures);
        Test("Disc compatibility", TestDiscCompatibility, failures);
        Test("Payload manifest releases", TestPayloadManifestReleases, failures);
        Test("Extracted game release validation", TestExtractedGameRelease, failures);
        Test("Payload timestamp normalization", TestPayloadTimestampNormalization, failures);
        Test("Workspace timestamp reconciliation", TestWorkspaceTimestampReconciliation, failures);
        Test("Toolkit accompanies the workspace", TestToolkitRefreshDecision, failures);
        Test("Retro-WFC payload snapshot reconciliation", TestRetroWfcPayloadSnapshotReconciliation, failures);
        Test("Retro-WFC transient retry policy", TestRetroWfcTransientRetryPolicy, failures);
        Test("Host CPU requirement", TestHostCpuRequirement, failures);
        Test("Shared runtime configuration", () => TestPathSettingRoundTrip("dvd_root",
            RuntimeConfiguration.SetDvdRoot, RuntimeConfiguration.RemoveDvdRootIfOwned), failures);
        Test("Canonical Retro Rewind configuration", () => TestPathSettingRoundTrip("retro_rewind_root",
            RuntimeConfiguration.SetRetroRewindRoot, RuntimeConfiguration.RemoveRetroRewindRootIfOwned),
            failures);
        Test("Runtime configuration key matching", TestRuntimeConfigurationKeyMatching, failures);
        Test("Runtime configuration quoted keys", TestRuntimeConfigurationQuotedKeys, failures);
        Test("Install location validation", TestInstallLocationValidation, failures);
        Test("Command line contract", TestCommandLineContract, failures);
        Test("Portable root discovery", TestPortableRootDiscovery, failures);
        Test("Portable install validation failure leaves no marker",
            TestPortableValidationFailureLeavesNoMarker, failures);
        Test("Portable configuration path resolution", TestPortableConfigPathResolution, failures);
        Test("Portable relative path settings", TestPortableRelativePathSettings, failures);
        Test("Portable move healing", TestPortableMoveHealing, failures);
        Test("Install transaction rollback", TestInstallTransactionRollback, failures);
        Test("Install move transient-lock policy", TestInstallMoveTransientLockPolicy, failures);
        Test("Install scratch recovery", TestInstallScratchRecovery, failures);
        Test("Shortcut cleanup is best effort", TestShortcutCleanupIsBestEffort, failures);
        Test("Canonical Retro Rewind resolution", TestCanonicalRetroRewindResolution, failures);
        Test("Retro Rewind compile-inputs golden vector",
            TestCompileInputsFingerprintGoldenVector, failures);
        Test("Retro Rewind compile-inputs fingerprint", TestCompileInputsFingerprint, failures);
        Test("Retro Rewind compile-inputs snapshot", TestCompileInputsSnapshot, failures);
        Test("Toolkit fingerprint", TestToolkitFingerprint, failures);
        Test("Retro Rewind inspection state machine", TestCodePulStateMachine, failures);
        Test("Products report contract", TestProductsReport, failures);
        foreach (var failure in failures) Console.Error.WriteLine("FAILED " + failure);
        return failures.Count == 0 ? 0 : 1;
    }

    private static PayloadManifest MultiReleaseManifest() => new()
    {
        SchemaVersion = PayloadManifest.CurrentSchemaVersion,
        ToolkitFingerprint = "toolkit", ToolkitPackageFingerprint = "package", RuntimeAssetsFingerprint = "assets",
        SupportedReleases =
        [
            new GameRelease { GameId = "RMCP01", DolSha256 = new string('1', 64), RelSha256 = new string('2', 64) },
            new GameRelease { GameId = "RMCE01", DolSha256 = new string('3', 64), RelSha256 = new string('4', 64) },
            new GameRelease { GameId = "RMCJ01", DolSha256 = new string('5', 64), RelSha256 = new string('6', 64) }
        ]
    };

    private static void TestDiscCompatibility()
    {
        var manifest = MultiReleaseManifest();
        foreach (var release in manifest.SupportedReleases)
        {
            var selected = InputValidation.EnsureCompatibleDisc(
                new DiscHeader { GameId = release.GameId.ToLowerInvariant() }, manifest);
            if (!ReferenceEquals(selected, release))
                throw new Exception($"{release.GameId} did not select its own release.");
        }

        foreach (var unsupported in new[] { "RMCK01", "RMCP02", "RSBE01" })
        {
            try
            {
                InputValidation.EnsureCompatibleDisc(new DiscHeader { GameId = unsupported }, manifest);
                throw new Exception($"{unsupported} was accepted.");
            }
            catch (InvalidDataException ex) when (ex.Message.Contains("NTSC-U (RMCE01)", StringComparison.Ordinal) &&
                                                  ex.Message.Contains(unsupported, StringComparison.Ordinal)) { }
        }
    }

    private static void TestPayloadManifestReleases()
    {
        PayloadArchive.ValidateManifest(MultiReleaseManifest());

        // The shape Build-Installer.ps1 writes (PowerShell's ConvertTo-Json of the ordered manifest).
        var dol = new string('a', 64);
        var rel = new string('b', 64);
        var json = $$"""
            {
                "SchemaVersion":  3,
                "ProductVersion":  "1.0.0",
                "SupportedReleases":  [
                    { "GameId":  "RMCP01", "DolSha256":  "{{dol}}", "RelSha256":  "{{rel}}" },
                    { "GameId":  "RMCE01", "DolSha256":  "{{rel}}", "RelSha256":  "{{dol}}" }
                ],
                "ToolkitReleaseTag":  "",
                "BuildModel":  "local-translation-and-compilation",
                "ToolkitFingerprint":  "toolkit",
                "ToolkitPackageFingerprint":  "package",
                "RuntimeAssetsFingerprint":  "assets",
                "TranslationFingerprint":  "translation",
                "NativeToolchainFingerprint":  "native"
            }
            """;
        using (var stream = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(json)))
        {
            var parsed = PayloadArchive.ParseManifest(stream);
            if (parsed.SupportedReleases.Count != 2 || parsed.SupportedReleases[1].GameId != "RMCE01" ||
                parsed.SupportedReleases[1].DolSha256 != rel)
                throw new Exception("The packaged release list was not read back exactly.");
        }

        void Reject(string description, Action<PayloadManifest> corrupt)
        {
            var manifest = MultiReleaseManifest();
            corrupt(manifest);
            try
            {
                PayloadArchive.ValidateManifest(manifest);
                throw new Exception($"A manifest with {description} was accepted.");
            }
            catch (InvalidDataException) { }
        }

        Reject("the previous single-release schema", manifest => manifest.SchemaVersion = 2);
        Reject("no releases", manifest => manifest.SupportedReleases.Clear());
        Reject("an unsupported release", manifest => manifest.SupportedReleases[0].GameId = "RMCK01");
        Reject("a missing input digest", manifest => manifest.SupportedReleases[1].RelSha256 = "");
        Reject("a duplicated release", manifest =>
            manifest.SupportedReleases.Add(new GameRelease
            {
                GameId = "RMCE01", DolSha256 = new string('7', 64), RelSha256 = new string('8', 64)
            }));
    }

    private static void TestExtractedGameRelease()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-release-" + Guid.NewGuid().ToString("N"));
        try
        {
            var data = Path.Combine(root, "GameAssets", "DATA");
            Directory.CreateDirectory(Path.Combine(data, "sys"));
            Directory.CreateDirectory(Path.Combine(data, "files", "rel"));
            File.WriteAllText(Path.Combine(data, "sys", "main.dol"), "ntsc-u dol");
            File.WriteAllText(Path.Combine(data, "sys", "fst.bin"), "fst");
            File.WriteAllText(Path.Combine(data, "files", "rel", "StaticR.rel"), "ntsc-u rel");
            File.WriteAllText(Path.Combine(data, "sys", "boot.bin"), "RMCE01" + new string('\0', 26));
            var release = new GameRelease
            {
                GameId = "RMCE01",
                DolSha256 = InputValidation.Sha256File(Path.Combine(data, "sys", "main.dol")),
                RelSha256 = InputValidation.Sha256File(Path.Combine(data, "files", "rel", "StaticR.rel"))
            };

            InstallerEngine.ValidateExtractedGame(data, release);
            if (new Installation(root).ReadGameId() != "RMCE01")
                throw new Exception("The installed game id was not read from the extracted disc header.");

            void Reject(string description, GameRelease expected)
            {
                try
                {
                    InstallerEngine.ValidateExtractedGame(data, expected);
                    throw new Exception($"{description} was accepted.");
                }
                catch (InvalidDataException) { }
            }

            Reject("Another release's digests", new GameRelease
            {
                GameId = "RMCE01", DolSha256 = new string('1', 64), RelSha256 = release.RelSha256
            });
            Reject("A disc header of another region", new GameRelease
            {
                GameId = "RMCJ01", DolSha256 = release.DolSha256, RelSha256 = release.RelSha256
            });
            File.Delete(Path.Combine(data, "sys", "boot.bin"));
            Reject("An extraction without a disc header", release);
            try
            {
                _ = new Installation(root).ReadGameId();
                throw new Exception("A missing disc header produced a game id.");
            }
            catch (InvalidDataException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestAcceptedExtension(string extension)
    {
        var path = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + extension);
        File.WriteAllBytes(path, [0]);
        try { InputValidation.ValidateExtension(path); } finally { File.Delete(path); }
    }

    private static void TestPayloadTimestampNormalization()
    {
        var path = Path.Combine(Path.GetTempPath(), "mkwc-timestamp-" + Guid.NewGuid().ToString("N"));
        try
        {
            File.WriteAllText(path, "timestamp");
            File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddDays(1));
            PayloadArchive.NormalizeExtractedTimestamp(path);
            if (File.GetLastWriteTimeUtc(path).Year != 2000)
                throw new Exception("Extracted payload timestamps were not normalized.");
        }
        finally
        {
            File.Delete(path);
        }
    }

    private static void TestWorkspaceTimestampReconciliation()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-wsstamp-" + Guid.NewGuid().ToString("N"));
        try
        {
            var installed = Path.Combine(root, "Install", "BuildWorkspace");
            var staged = Path.Combine(root, "staging", "BuildWorkspace");
            Directory.CreateDirectory(Path.Combine(installed, "runtime", "src"));
            Directory.CreateDirectory(Path.Combine(staged, "runtime", "src"));
            File.WriteAllText(Path.Combine(installed, "runtime", "src", "same.cpp"), "int same();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "same.cpp"), "int same();");
            var installedSameStamp = new DateTime(2026, 1, 2, 3, 4, 5, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(Path.Combine(installed, "runtime", "src", "same.cpp"),
                installedSameStamp);
            File.WriteAllText(Path.Combine(installed, "runtime", "src", "edited.cpp"), "int old();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "edited.cpp"), "int new_();");
            File.WriteAllText(Path.Combine(staged, "runtime", "src", "added.cpp"), "int added();");
            foreach (var file in Directory.EnumerateFiles(staged, "*", SearchOption.AllDirectories))
                PayloadArchive.NormalizeExtractedTimestamp(file);

            WorkspaceTimestamps.MarkChangedFiles(installed, staged, _ => { });

            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "same.cpp")) !=
                installedSameStamp)
                throw new Exception("An unchanged workspace file did not inherit the installed " +
                                    "timestamp; clang would reject the previous build's PCHs.");
            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "edited.cpp")).Year == 2000)
                throw new Exception("A changed workspace file kept its normalized timestamp; " +
                                    "Ninja would never recompile it.");
            if (File.GetLastWriteTimeUtc(Path.Combine(staged, "runtime", "src", "added.cpp")).Year == 2000)
                throw new Exception("A new workspace file kept its normalized timestamp.");

            // A fresh installation has no workspace to compare against; the pass must be a no-op.
            WorkspaceTimestamps.MarkChangedFiles(Path.Combine(root, "absent"), staged, _ => { });
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }


    private static void TestToolkitRefreshDecision()
    {
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: false, samePackageContent: true,
                nodToolPresent: true))
            throw new Exception("A republished workspace kept the installed toolkit; the shipped " +
                                "translator and project file could come from different releases.");
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: false,
                nodToolPresent: true))
            throw new Exception("Changed toolkit package content was not extracted.");
        if (!InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: true,
                nodToolPresent: false))
            throw new Exception("A missing nodtool.exe did not force toolkit extraction.");
        if (InstallerEngine.MustRefreshToolkit(sameToolkit: true, samePackageContent: true,
                nodToolPresent: true))
            throw new Exception("An unchanged toolkit was needlessly re-extracted.");
    }

    /// <summary>
    /// Every <c>[paths]</c> key follows the same contract: writing preserves other settings, reads
    /// back resolved, and uninstall removes it only while it still points at the recorded location.
    /// </summary>
    private static void TestPathSettingRoundTrip(string key, Action<string, string> set,
        Action<string, string> removeIfOwned)
    {
        var root = Path.Combine(Path.GetTempPath(), $"mkwc-{key}-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, "Config.toml");
        var owned = Path.Combine(root, "Owned Location");
        try
        {
            Directory.CreateDirectory(owned);
            File.WriteAllText(config,
                "[video]\nwidescreen = true\n\n[paths]\nunrelated_root = \"D:\\\\Elsewhere\"\n");
            set(config, owned);
            var updated = File.ReadAllText(config);
            if (!updated.Contains("widescreen = true", StringComparison.Ordinal) ||
                !updated.Contains("unrelated_root = \"D:\\\\Elsewhere\"", StringComparison.Ordinal) ||
                !updated.Contains(key + " = ", StringComparison.Ordinal))
                throw new Exception($"Writing {key} discarded existing runtime settings.");

            var read = RuntimeConfiguration.GetResolvedPath(config, key);
            if (read is null || !read.Equals(Path.GetFullPath(owned), StringComparison.OrdinalIgnoreCase))
                throw new Exception($"The configured {key} did not round-trip.");

            removeIfOwned(config, Path.Combine(root, "Someone Else"));
            if (RuntimeConfiguration.GetPath(config, key) is null)
                throw new Exception($"A {key} owned by someone else was removed.");
            removeIfOwned(config, owned);
            if (RuntimeConfiguration.GetPath(config, key) is not null)
                throw new Exception($"Uninstall did not remove its owned {key} setting.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// TOML keys are case-sensitive and the runtime compares them exactly, so the host must match
    /// case too, or it silently writes a key the runtime never reads.
    /// </summary>
    private static void TestRuntimeConfigurationKeyMatching()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-tomlcase-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, RuntimeConfiguration.ConfigFileName);
        var owned = Path.Combine(root, "Owned Location");
        try
        {
            Directory.CreateDirectory(owned);
            File.WriteAllText(config, "[Paths]\r\nDvd_Root = \"D:\\\\Wrong\"\r\n");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            if (!File.ReadAllText(config).Contains("Dvd_Root = \"D:\\\\Wrong\"", StringComparison.Ordinal))
                throw new Exception("A differently-cased key the runtime ignores was rewritten.");
            var written = RuntimeConfiguration.GetResolvedPath(config, "dvd_root");
            if (written is null || !written.Equals(Path.GetFullPath(owned), StringComparison.OrdinalIgnoreCase))
                throw new Exception("dvd_root was not written as the exactly-cased key the runtime reads.");

            // An escaped quote does not end the value, so the '#' after it is data rather than the
            // start of a comment - exactly what RemoveComment in runtime_config.h decides.
            File.WriteAllText(config, "[paths]\r\ndvd_root = \"a\\\"#b\"\r\n");
            if (RuntimeConfiguration.GetPath(config, "dvd_root") != "a\"#b")
                throw new Exception("A '#' inside an escaped quoted value was treated as a comment.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// Quoted and bare spellings of a key or table are the same TOML key, so rewriting replaces the
    /// entry in place, keeps everything else byte-identical, and never touches an unparseable file.
    /// </summary>
    private static void TestRuntimeConfigurationQuotedKeys()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-tomlquoted-" + Guid.NewGuid().ToString("N"));
        var config = Path.Combine(root, RuntimeConfiguration.ConfigFileName);
        var owned = Path.Combine(root, "Owned Location");
        var fullOwned = Path.GetFullPath(owned);
        try
        {
            Directory.CreateDirectory(owned);
            foreach (var header in new[] { "[\"paths\"]", "[ 'paths' ]", "[paths]" })
            foreach (var key in new[] { "\"dvd_root\"", "'dvd_root'", "\"dvd\\u005Froot\"", "dvd_root" })
            {
                File.WriteAllText(config,
                    $"# top\r\n[video]\r\nwidescreen = true # keep\r\n\r\n{header}\r\n  {key}=\"D:\\\\Old\" # note\r\nother = 1\r\n");
                RuntimeConfiguration.SetDvdRoot(config, owned);
                var text = File.ReadAllText(config);
                var expected = $"# top\r\n[video]\r\nwidescreen = true # keep\r\n\r\n{header}\r\n  {key}=" +
                               $"\"{fullOwned.Replace("\\", "\\\\")}\" # note\r\nother = 1\r\n";
                if (text != expected)
                    throw new Exception($"{header} {key} was not replaced in place:\r\n{text}");
                if (RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                    throw new Exception($"The replaced {key} could not be read back.");
            }

            File.WriteAllText(config, "[paths]\r\nother = 1\r\n\r\n# next\r\n[video]\r\nx = 2");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            var appended = File.ReadAllText(config);
            if (!appended.StartsWith("[paths]\r\nother = 1\r\ndvd_root = ", StringComparison.Ordinal) ||
                !appended.EndsWith("\r\n\r\n# next\r\n[video]\r\nx = 2", StringComparison.Ordinal))
                throw new Exception("A missing key was not appended to its section:\r\n" + appended);

            File.WriteAllText(config, "dvd_root = \"D:\\\\Root\"\r\n[video]\r\nx = 2\r\n");
            RuntimeConfiguration.SetDvdRoot(config, owned);
            var created = File.ReadAllText(config);
            if (!created.StartsWith("dvd_root = \"D:\\\\Root\"\r\n[video]\r\nx = 2\r\n\r\n[paths]\r\n", StringComparison.Ordinal) ||
                RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                throw new Exception("A missing section was not created without touching a same-named root key:\r\n" + created);

            File.Delete(config);
            RuntimeConfiguration.SetDvdRoot(config, owned);
            if (RuntimeConfiguration.GetResolvedPath(config, "dvd_root") != fullOwned)
                throw new Exception("A missing config file was not created.");

            const string broken = "[paths]\r\ndvd_root = 1\r\ndvd_root = 2\r\n";
            File.WriteAllText(config, broken);
            try
            {
                RuntimeConfiguration.SetDvdRoot(config, owned);
                throw new Exception("An unparseable config was rewritten.");
            }
            catch (TomlException) { }
            if (File.ReadAllText(config) != broken)
                throw new Exception("An unparseable config was modified.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// One location rule set covers both an installation directory and a portable root, so the
    /// rejections a portable install performs are the ones an ordinary install performs too.
    /// </summary>
    private static void TestInstallLocationValidation()
    {
        void Rejects(string description, string path)
        {
            try
            {
                FileSystemUtilities.EnsureUsableLocation(path, "The installation folder");
                throw new Exception(description + " was accepted as an install location.");
            }
            catch (InvalidOperationException) { }
        }

        FileSystemUtilities.EnsureUsableLocation("C:\\Games\\WiiCompiled\\Install", "The installation folder");
        Rejects("A network path", "\\\\server\\share\\WiiCompiled");
        Rejects("An over-long path", "C:\\" + new string('w', 200));
        Rejects("A drive root", "C:\\");
        Rejects("A folder inside Windows",
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "WiiCompiled"));
        Rejects("The user profile itself",
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        // The two entry points must not have separate opinions about any of the above.
        foreach (var rejected in new[] { "\\\\server\\share\\WiiCompiled", "C:\\" })
        {
            try
            {
                PortableRoot.EnsureUsableRoot(rejected);
                throw new Exception($"{rejected} was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
        }
    }

    private static void TestCommandLineContract()
    {
        void Rejects(string description, params string[] args)
        {
            try
            {
                CommandLine.Parse(args);
                throw new Exception(description + " was accepted.");
            }
            catch (ArgumentException) { }
        }

        var version = CommandLine.Parse(["--version"]);
        if (version.Mode != AppMode.Version) throw new Exception("--version was not recognised.");

        var silent = CommandLine.Parse([
            "--silent", "--game", "game.iso", "--install-dir", "C:\\Games\\MKW",
            "--progress-json", "--retro-dir", "D:\\RetroRewind", "--skip-retro-wfc-payload"
        ]);

        if (silent.Mode != AppMode.SilentInstall || !silent.ProgressJson ||
            silent.RetroDirectoryPath != "D:\\RetroRewind" ||
            silent.RetroWfcPayloadMode != RetroWfcPayloadMode.Skipped)
            throw new Exception("The silent install contract did not parse.");

        var launch = CommandLine.Parse(["--launch-base"]);
        if (launch.Mode != AppMode.LaunchBase)
            throw new Exception("The launch contract did not parse.");
        Rejects("runtime argument forwarding", "--launch-base", "--", "--windowed");

        if (CommandLine.Parse(["--launch-retro"]).Mode != AppMode.LaunchRetro)
            throw new Exception("--launch-retro was not recognised.");

        var check = CommandLine.Parse([
            "--check-products", "--install-dir", "C:\\Games\\MKW", "--retro-dir", "D:\\RetroRewind",
            "--progress-json"
        ]);
        if (check.Mode != AppMode.CheckProducts || check.RetroDirectoryPath != "D:\\RetroRewind" ||
            check.RetroWfcPayloadMode != RetroWfcPayloadMode.NotApplicable)
            throw new Exception("The product check contract did not parse.");

        var retroRepair = CommandLine.Parse([
            "--repair-products", "--install-dir", "C:\\Games\\MKW", "--retro-dir", "D:\\RetroRewind",
            "--download-retro-wfc-payload", "--progress-json"
        ]);
        if (retroRepair.Mode != AppMode.RepairProducts ||
            retroRepair.RetroDirectoryPath != "D:\\RetroRewind" ||
            retroRepair.RetroWfcPayloadMode != RetroWfcPayloadMode.Online)
            throw new Exception("The Retro Rewind repair contract did not parse.");

        Rejects("An unknown Retro Rewind source flag", "--silent", "--game", "g.iso", "--retro-zip",
            "a.zip", "--skip-retro-wfc-payload");
        Rejects("--progress-json outside an install", "--launch-base", "--progress-json");
        // The NAND flag is gone: Wheel Wizard writes paths.nand_root into Config.toml itself.
        Rejects("The removed NAND flag", "--silent", "--game", "g.iso", "--nand-dir", "D:\\Wii");
        Rejects("A payload option without the canonical Retro Rewind folder", "--silent", "--game",
            "g.iso", "--skip-retro-wfc-payload");
        Rejects("A Retro Rewind folder without a payload decision", "--silent", "--game", "g.iso",
            "--retro-dir", "b");
        Rejects("Repair without an installation", "--repair-products", "--retro-dir", "rr",
            "--skip-retro-wfc-payload", "--progress-json");
        Rejects("Repair without the canonical Retro Rewind folder", "--repair-products",
            "--install-dir", "fixed", "--progress-json");
        Rejects("Retro Rewind repair without a payload decision", "--repair-products", "--install-dir", "fixed",
            "--retro-dir", "rr", "--progress-json");
        Rejects("A repair payload option without the canonical folder", "--repair-products",
            "--install-dir", "fixed", "--skip-retro-wfc-payload", "--progress-json");

        var portable = CommandLine.Parse([
            "--silent", "--game", "game.iso", "--install-dir", "D:\\Games\\Recomp\\Install",
            "--portable", "--progress-json"
        ]);
        if (portable.Mode != AppMode.SilentInstall || !portable.Portable)
            throw new Exception("The portable install contract did not parse.");

        Rejects("--portable without an install directory", "--silent", "--game", "g.iso", "--portable");
        Rejects("--portable outside an install", "--check-products", "--install-dir", "fixed", "--portable");
        // The config verbs are gone: WheelWizard reads and writes Config.toml directly now.
        Rejects("The removed configuration read verb", "--get-config");
        Rejects("The removed configuration write verb", "--set-config", "--install-dir", "fixed");
        Rejects("A bare setting assignment", "--check-products", "video.show_fps=true");
    }

    /// <summary>
    /// The marker walk-up is the one thing both languages must agree on. The depth bound keeps an
    /// unrelated marker high up a drive from capturing an ordinary installation.
    /// </summary>
    private static void TestPortableRootDiscovery()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-portable-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            var deep = Path.Combine(install, "Base", "config", "a", "b", "c");
            Directory.CreateDirectory(deep);

            if (PortableRoot.TryFind(install) is not null)
                throw new Exception("A directory without the marker was treated as portable.");

            File.WriteAllText(Path.Combine(root, PortableRoot.MarkerFileName), "portable");
            if (PortableRoot.TryFind(root) != root)
                throw new Exception("The marker directory itself was not recognised as the root.");
            if (PortableRoot.TryFind(install) != root)
                throw new Exception("The install directory did not resolve to its portable root.");
            if (PortableRoot.TryFind(Path.Combine(install, "Base", "config", "a")) != root)
                throw new Exception("A path within the depth bound did not resolve to its portable root.");
            if (PortableRoot.TryFind(deep) is not null)
                throw new Exception("The portable search walked further up than its depth bound.");

            if (!PortableRoot.Contains(root, Path.Combine(install, "GameAssets", "DATA")) ||
                PortableRoot.Contains(root, Path.Combine(Path.GetTempPath(), "elsewhere")))
                throw new Exception("Portable containment did not classify paths correctly.");

            try
            {
                PortableRoot.EnsureUsableRoot(Path.GetPathRoot(root)!);
                throw new Exception("A drive root was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
            try
            {
                PortableRoot.EnsureUsableRoot(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
                throw new Exception("A well-known system folder was accepted as a portable root.");
            }
            catch (InvalidOperationException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A rejected game image must not turn the destination's parent into a portable root. Otherwise
    /// the marker walk-up would redirect unrelated later installs into this root's UserData.
    /// </summary>
    private static void TestPortableValidationFailureLeavesNoMarker()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-portable-rejected-" + Guid.NewGuid().ToString("N"));
        var game = Path.Combine(root, "not-a-disc-image.wad");
        try
        {
            Directory.CreateDirectory(root);
            File.WriteAllBytes(game, [0]);
            var options = new InstallOptions
            {
                GamePath = game,
                InstallDirectory = Path.Combine(root, PortableRoot.InstallDirectoryName),
                Portable = true
            };

            try
            {
                new InstallerEngine(new DelegatingInstallReporter(_ => { }))
                    .InstallAsync(options).GetAwaiter().GetResult();
                throw new Exception("An unsupported game image was accepted.");
            }
            catch (InvalidDataException) { }

            if (File.Exists(Path.Combine(root, PortableRoot.MarkerFileName)))
                throw new Exception("A failed portable install left its marker behind.");
            if (Directory.Exists(PortableRoot.UserDataDirectory(root)))
                throw new Exception("A failed portable install left its UserData directory behind.");
            if (PortableRoot.TryFind(options.InstallDirectory) is not null)
                throw new Exception("A failed portable install captured later installs under its parent.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestShortcutCleanupIsBestEffort()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-shortcuts-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(root);
            // File.Delete cannot delete a directory. This deterministically forces the first cleanup
            // to fail while the remaining independently guarded targets must still be attempted.
            var firstShortcut = Path.Combine(root, "first.lnk");
            Directory.CreateDirectory(firstShortcut);
            var secondShortcut = Path.Combine(root, "second.lnk");
            File.WriteAllText(secondShortcut, "shortcut");

            try
            {
                ShellIntegration.RemoveShortcuts([firstShortcut, secondShortcut]);
                throw new Exception("A shortcut deletion failure was not reported.");
            }
            catch (AggregateException) { }

            if (File.Exists(secondShortcut))
                throw new Exception("One failed shortcut deletion prevented the next file deletion.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The configuration location is a property of the installation, not of this process: an install
    /// under a portable root uses that root's UserData, everything else the shared per-user file.
    /// </summary>
    private static void TestPortableConfigPathResolution()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-portable-config-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            Directory.CreateDirectory(install);
            if (!RuntimeConfiguration.ResolveConfigPath(install)
                    .Equals(RuntimeConfiguration.ApplicationDataConfigPath, StringComparison.OrdinalIgnoreCase))
                throw new Exception("A non-portable installation did not use per-user application data.");

            var created = PortableRoot.Create(root);
            if (!Directory.Exists(PortableRoot.UserDataDirectory(created)) ||
                !File.Exists(Path.Combine(created, PortableRoot.MarkerFileName)))
                throw new Exception("Creating a portable root did not produce its marker and user data.");

            var expected = Path.Combine(root, PortableRoot.UserDataDirectoryName, "Config.toml");
            if (!RuntimeConfiguration.ResolveConfigPath(install)
                    .Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new Exception("A portable installation did not use its own UserData configuration.");
            if (!new Installation(install).ConfigPath.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new Exception("The installation did not report its portable configuration path.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A portable installation stores every path inside its root relative to the configuration
    /// directory, so the whole tree survives being moved. Ownership decisions must therefore compare
    /// resolved paths, not stored text.
    /// </summary>
    private static void TestPortableRelativePathSettings()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-portable-paths-" + Guid.NewGuid().ToString("N"));
        try
        {
            PortableRoot.Create(root);
            var install = Path.Combine(root, "Install");
            var config = Path.Combine(root, PortableRoot.UserDataDirectoryName, "Config.toml");
            var dvdRoot = Path.Combine(install, "GameAssets", "DATA");
            var outsideRetro = Path.Combine(Path.GetTempPath(), "mkwc-outside-" + Guid.NewGuid().ToString("N"));
            var insideRetro = Path.Combine(root, "RetroRewind6");

            RuntimeConfiguration.SetDvdRoot(config, dvdRoot);
            var stored = RuntimeConfiguration.GetPath(config, "dvd_root");
            if (stored is null || Path.IsPathRooted(stored))
                throw new Exception("A path inside the portable root was stored absolute.");
            if (stored.Contains('\\', StringComparison.Ordinal))
                throw new Exception("A relative path was not stored with the canonical forward slashes.");
            if (!RuntimeConfiguration.GetResolvedPath(config, "dvd_root")!
                    .Equals(Path.GetFullPath(dvdRoot), StringComparison.OrdinalIgnoreCase))
                throw new Exception("A relative dvd_root did not resolve against the configuration directory.");

            RuntimeConfiguration.SetRetroRewindRoot(config, insideRetro);
            if (Path.IsPathRooted(RuntimeConfiguration.GetPath(config, "retro_rewind_root")!))
                throw new Exception("A Retro Rewind root inside the portable root was stored absolute.");
            RuntimeConfiguration.SetRetroRewindRoot(config, outsideRetro);
            if (!Path.IsPathRooted(RuntimeConfiguration.GetPath(config, "retro_rewind_root")!))
                throw new Exception("A Retro Rewind root outside the portable root was stored relative.");

            RuntimeConfiguration.RemoveDvdRootIfOwned(config, Path.Combine(install, "Elsewhere"));
            if (RuntimeConfiguration.GetPath(config, "dvd_root") is null)
                throw new Exception("A relative dvd_root owned by someone else was removed.");
            RuntimeConfiguration.RemoveDvdRootIfOwned(config, dvdRoot);
            if (RuntimeConfiguration.GetPath(config, "dvd_root") is not null)
                throw new Exception("A relative dvd_root this installation owns was not removed.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A portable root can be moved. Healing adopts the directory the installation was actually found
    /// in and discards the location-bound native build cache - and must never touch a non-portable
    /// installation, where the same disagreement is a real problem to report.
    /// </summary>
    private static void TestPortableMoveHealing()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-move-" + Guid.NewGuid().ToString("N"));
        try
        {
            var install = Path.Combine(root, "Install");
            var nativeBuild = Path.Combine(install, "BuildWorkspace", "native-build");
            Directory.CreateDirectory(nativeBuild);
            File.WriteAllText(Path.Combine(nativeBuild, "CMakeCache.txt"),
                "CMAKE_HOME_DIRECTORY:INTERNAL=D:/Old/Install/BuildWorkspace/runtime");
            var statePath = Path.Combine(install, "install-state.json");
            JsonState.Write(statePath, new InstallState { InstallDir = "D:\\Old\\Install" });

            var plain = new Installation(install);
            if (PortableInstallHealing.HealMovedInstall(plain))
                throw new Exception("A non-portable installation was healed.");
            if (!Directory.Exists(nativeBuild))
                throw new Exception("A non-portable installation lost its native build directory.");

            PortableRoot.Create(root);
            var portable = new Installation(install);
            if (!PortableInstallHealing.HealMovedInstall(portable))
                throw new Exception("A moved portable installation was not healed.");
            var healed = portable.ReadInstallState();
            if (healed is null || !healed.InstallDir.Equals(portable.Root, StringComparison.OrdinalIgnoreCase))
                throw new Exception("Healing did not adopt the actual installation directory.");
            if (Directory.Exists(nativeBuild))
                throw new Exception("Healing kept the location-bound native build cache.");

            if (PortableInstallHealing.HealMovedInstall(portable))
                throw new Exception("An installation already at its recorded location was healed again.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestCanonicalRetroRewindResolution()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-retro-" + Guid.NewGuid().ToString("N"));
        try
        {
            var package = Path.Combine(root, "wrapper", "RetroRewind6");
            Directory.CreateDirectory(Path.Combine(package, "Binaries"));
            File.WriteAllText(Path.Combine(package, "Binaries", "Code.pul"), "pretend-kamek");
            File.WriteAllText(Path.Combine(package, "version.txt"), "1.2.3");

            var expected = InputValidation.Sha256File(Path.Combine(package, "Binaries", "Code.pul"));
            // The canonical folder may be named directly, or reached through the one permitted
            // wrapper level; all three selections resolve to exactly the same installation.
            foreach (var selection in new[] { package, Path.Combine(root, "wrapper"), root })
            {
                var inputs = CompileInputsFingerprint.Compute(selection);
                if (!inputs.RetroRewindRoot.Equals(Path.GetFullPath(package),
                        StringComparison.OrdinalIgnoreCase))
                    throw new Exception($"Selecting {selection} resolved to {inputs.RetroRewindRoot}.");
                if (inputs.CodePulSha256 != expected)
                    throw new Exception($"The Code.pul hash was wrong when selecting {selection}.");
            }

            try
            {
                _ = RetroRewindSource.ResolveRetroRewind6(Path.Combine(package, "Binaries"));
                throw new Exception("A folder without Code.pul was accepted.");
            }
            catch (InvalidDataException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestRetroWfcPayloadSnapshotReconciliation()
    {
        const string shaB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        var root = Path.Combine(Path.GetTempPath(), "mkwc-wfc-snapshot-" + Guid.NewGuid().ToString("N"));
        try
        {
            using var signingRsa = RSA.Create(2048);
            var signingKey = signingRsa.ExportParameters(includePrivateParameters: false);
            byte[] SignedPayload(string payloadGame)
            {
                var image = new byte[0x150];
                "WWFC/Payload"u8.CopyTo(image);
                BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x0C), (uint)image.Length);
                System.Text.Encoding.ASCII.GetBytes(payloadGame).CopyTo(image, 0x138);
                image[^1] = 0x5A;
                signingRsa.SignData(image.AsSpan(0x110), HashAlgorithmName.SHA256,
                    RSASignaturePadding.Pkcs1).CopyTo(image.AsSpan(0x10));
                return image;
            }

            // Every supported release has its own endpoint and staged file, and a payload staged for
            // one region never validates as another region's.
            var endpoints = new HashSet<string>(StringComparer.Ordinal);
            foreach (var gameId in RetroWfcPayload.SupportedGameIds)
            {
                var endpoint = RetroWfcPayload.PayloadUri(gameId);
                InputValidation.ValidateRetroWfcPayloadUri(endpoint);
                if (!endpoints.Add(endpoint))
                    throw new Exception($"{gameId} shares another release's Retro-WFC endpoint.");
                try
                {
                    InputValidation.ValidateRetroWfcPayloadUri(endpoint + "&extra=1");
                    throw new Exception("A non-canonical Retro-WFC endpoint was accepted.");
                }
                catch (InvalidDataException) { }

                var regionalRoot = Path.Combine(root, "regional-" + gameId);
                var regionalPath = Path.Combine(regionalRoot, RetroWfcPayload.PayloadRelativePath(gameId));
                Directory.CreateDirectory(Path.GetDirectoryName(regionalPath)!);
                File.WriteAllBytes(regionalPath, SignedPayload(RetroWfcPayload.PayloadGame(gameId)));
                if (InputValidation.ResolveRetroWfcPayloadFile(regionalRoot, gameId, signingKey) != regionalPath)
                    throw new Exception($"{gameId} did not resolve its own staged payload file.");
                foreach (var otherGameId in RetroWfcPayload.SupportedGameIds.Where(other => other != gameId))
                {
                    var foreignRoot = Path.Combine(root, $"foreign-{gameId}-{otherGameId}");
                    var foreignPath = Path.Combine(foreignRoot, RetroWfcPayload.PayloadRelativePath(otherGameId));
                    Directory.CreateDirectory(Path.GetDirectoryName(foreignPath)!);
                    File.WriteAllBytes(foreignPath, SignedPayload(RetroWfcPayload.PayloadGame(gameId)));
                    try
                    {
                        _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(foreignRoot, otherGameId,
                            signingKey);
                        throw new Exception($"A {gameId} payload was accepted for {otherGameId}.");
                    }
                    catch (InvalidDataException) { }
                    try
                    {
                        _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(regionalRoot, otherGameId,
                            signingKey);
                        throw new Exception($"{otherGameId} accepted a directory staged only for {gameId}.");
                    }
                    catch (InvalidDataException) { }
                }
            }
            try
            {
                _ = RetroWfcPayload.PayloadUri("RMCK01");
                throw new Exception("An unsupported release has a Retro-WFC endpoint.");
            }
            catch (InvalidDataException) { }

            const string gameUnderTest = "RMCE01";
            var installation = new Installation(root);
            Directory.CreateDirectory(installation.RetroDirectory);
            var payloadRoot = Path.Combine(root, "download");
            var payloadPath = Path.Combine(payloadRoot, RetroWfcPayload.PayloadRelativePath(gameUnderTest));
            Directory.CreateDirectory(Path.GetDirectoryName(payloadPath)!);
            var bytes = SignedPayload(RetroWfcPayload.PayloadGame(gameUnderTest));
            File.WriteAllBytes(payloadPath, bytes);
            _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(payloadRoot, gameUnderTest, signingKey);
            try
            {
                _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(payloadRoot, gameUnderTest);
                throw new Exception("A payload signed by a foreign key passed the pinned production key.");
            }
            catch (InvalidDataException) { }
            var shaA = InputValidation.ComputeRetroWfcPayloadSha256(payloadRoot, gameUnderTest, signingKey);
            var snapshot = new RetroWfcPayloadSnapshot(payloadRoot, shaA, bytes.Length);
            JsonState.Write(Path.Combine(installation.RetroDirectory, ProductFingerprint.FileName),
                new ProductFingerprint
                {
                    Profile = "retro-rewind", ToolkitFingerprint = "toolkit",
                    RetroWfcPayloadMode = "downloaded", RetroWfcPayloadSha256 = shaA,
                    RetroWfcPayloadLength = bytes.Length
                });
            JsonState.Write(installation.InstallStatePath, new InstallState
            {
                InstallDir = root, RetroWfcPayloadMode = "downloaded",
                RetroWfcPayloadSha256 = shaA, RetroWfcPayloadLength = bytes.Length
            });
            if (!installation.MatchesRetroWfcPayloadSnapshot("toolkit", snapshot))
                throw new Exception("The same downloaded payload snapshot did not produce a no-op.");
            if (ProductRepairService.PlanRetroWfcPayloadReconciliation(productMatchesSnapshot: true,
                    cacheMatchesSnapshot: true) != ProductRepairService.RetroWfcPayloadAction.None)
                throw new Exception("The same downloaded snapshot did not remain a no-op.");
            if (ProductRepairService.PlanRetroWfcPayloadReconciliation(productMatchesSnapshot: true,
                    cacheMatchesSnapshot: false) !=
                ProductRepairService.RetroWfcPayloadAction.PublishCacheOnly)
                throw new Exception("A damaged local payload cache requested an unnecessary recompile.");

            var changedSnapshot = snapshot with { Sha256 = shaB };
            if (installation.MatchesRetroWfcPayloadSnapshot("toolkit", changedSnapshot))
                throw new Exception("A changed downloaded payload snapshot did not require recompilation.");
            if (ProductRepairService.PlanRetroWfcPayloadReconciliation(productMatchesSnapshot: false,
                    cacheMatchesSnapshot: false) != ProductRepairService.RetroWfcPayloadAction.RebuildRetro)
                throw new Exception("A changed payload did not rebuild Retro Rewind.");

            bytes[0x0F]++;
            File.WriteAllBytes(payloadPath, bytes);
            try
            {
                _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(payloadRoot, gameUnderTest, signingKey);
                throw new Exception("A payload with a malformed declared length was accepted.");
            }
            catch (InvalidDataException) { }

            bytes[0x0F]--;
            bytes[^1] ^= 1;
            File.WriteAllBytes(payloadPath, bytes);
            try
            {
                _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(payloadRoot, gameUnderTest, signingKey);
                throw new Exception("A payload with tampered signed content was accepted.");
            }
            catch (InvalidDataException) { }

            bytes[^1] ^= 1;
            bytes[0] = 0;
            File.WriteAllBytes(payloadPath, bytes);
            try
            {
                _ = InputValidation.ValidateStagedRetroWfcPayloadDirectory(payloadRoot, gameUnderTest, signingKey);
                throw new Exception("A payload with a malformed header was accepted.");
            }
            catch (InvalidDataException) { }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestInstallMoveTransientLockPolicy()
    {
        if (!InstallTransaction.IsTransientLock(new IOException("in use", unchecked((int)0x80070020))))
            throw new Exception("A sharing violation would not be retried.");
        if (!InstallTransaction.IsTransientLock(new IOException("locked", unchecked((int)0x80070021))))
            throw new Exception("A lock violation would not be retried.");
        if (!InstallTransaction.IsTransientLock(new UnauthorizedAccessException("denied")))
            throw new Exception("A transient access-denied would not be retried.");
        if (InstallTransaction.IsTransientLock(new IOException("disk full", unchecked((int)0x80070070))))
            throw new Exception("A permanent I/O failure would be retried.");
        if (InstallTransaction.IsTransientLock(new InvalidDataException("bad")))
            throw new Exception("A non-I/O failure would be retried.");
    }

    private static void TestHostCpuRequirement()
    {
        // Leaf 1 ecx, leaf 7 ebx, leaf 0x80000001 ecx of a v2-only and a v3 processor.
        const uint v2Leaf1 = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23);
        const uint v3Leaf1 = v2Leaf1 | (1u << 12) | (1u << 22) | (1u << 27) | (1u << 28) | (1u << 29);
        const uint v3Leaf7 = (1u << 3) | (1u << 5) | (1u << 8);
        Func<uint, uint[]> Cpu(uint maxBasic, uint leaf1, uint leaf7, uint extended) => leaf => leaf switch
        {
            0 => [maxBasic, 0, 0, 0],
            1 => [0, 0, leaf1, 0],
            7 => [0, leaf7, 0, 0],
            0x80000000u => [0x80000001u, 0, 0, 0],
            0x80000001u => [0, 0, extended, 0],
            _ => [0, 0, 0, 0]
        };
        var nehalem = Cpu(11, v2Leaf1, 0, 1u);
        var haswell = Cpu(13, v3Leaf1, v3Leaf7, 1u | (1u << 5));

        void Expect(IReadOnlyList<string> actual, string expected, string what)
        {
            if (string.Join(", ", actual) != expected)
                throw new Exception($"{what}: expected [{expected}], got [{string.Join(", ", actual)}].");
        }

        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, nehalem, false), "",
            "A v2 processor was rejected for the v2 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, nehalem, false),
            "FMA, MOVBE, OSXSAVE, AVX, F16C, BMI1, AVX2, BMI2, LZCNT", "A v2 processor for the v3 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, haswell, true), "",
            "A v3 processor was rejected");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, haswell, false), "",
            "A v3 processor was rejected for the v2 build");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, haswell, false),
            "operating system support for AVX register state (XCR0 YMM bits)", "Disabled AVX state");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V3, Cpu(6, v3Leaf1, v3Leaf7, 1u | (1u << 5)), true),
            "BMI1, AVX2, BMI2", "Unavailable leaf 7");
        Expect(HostCpuRequirement.FindMissingFeatures(X86CpuProfile.V2, Cpu(11, v2Leaf1, 0, 0), false),
            "LAHF-SAHF", "Missing LAHF/SAHF");

        if (HostCpuRequirement.SelectBuildProfile(nehalem, false) != X86CpuProfile.V2)
            throw new Exception("A v2 processor was not given the v2 build.");
        if (HostCpuRequirement.SelectBuildProfile(haswell, true) != X86CpuProfile.V3)
            throw new Exception("A v3 processor was not given the v3 build.");
        if (HostCpuRequirement.SelectBuildProfile(haswell, false) != X86CpuProfile.V2)
            throw new Exception("A v3 processor without AVX state was given the v3 build.");

        var message = HostCpuRequirement.Describe(X86CpuProfile.V3, ["AVX2"]);
        if (!message.Contains("x86-64-v3") || !message.Contains("Missing on this machine: AVX2"))
            throw new Exception("The unsupported-processor message lost its detail.");
    }

    private static void TestRetroWfcTransientRetryPolicy()
    {
        if (!InputValidation.IsTransientRetroWfcDownloadFailure(
                new HttpRequestException("Connection reset"), CancellationToken.None))
            throw new Exception("A transport failure would not receive its one retry.");
        if (!InputValidation.IsTransientRetroWfcDownloadFailure(
                new HttpRequestException("Service unavailable", null,
                    System.Net.HttpStatusCode.ServiceUnavailable), CancellationToken.None))
            throw new Exception("An HTTP 5xx response would not receive its one retry.");
        if (!InputValidation.IsTransientRetroWfcDownloadFailure(
                new TimeoutException("Timed out"), CancellationToken.None))
            throw new Exception("A download timeout would not receive its one retry.");
        if (InputValidation.IsTransientRetroWfcDownloadFailure(
                new HttpRequestException("Not found", null, System.Net.HttpStatusCode.NotFound),
                CancellationToken.None))
            throw new Exception("A permanent HTTP 4xx response would be retried.");
        if (InputValidation.IsTransientRetroWfcDownloadFailure(
                new InvalidDataException("Malformed payload"), CancellationToken.None))
            throw new Exception("A malformed payload would be retried.");

        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        if (InputValidation.IsTransientRetroWfcDownloadFailure(
                new TimeoutException("Cancelled"), cancelled.Token))
            throw new Exception("Caller cancellation would be retried.");
    }

    private static void TestInstallTransactionRollback()
    {
        var parent = Path.Combine(Path.GetTempPath(),
            "mkwc-transaction-self-test-" + Guid.NewGuid().ToString("N"));
        var installRoot = Path.Combine(parent, "install");
        var liveComponent = Path.Combine(installRoot, "Component");
        var preparedComponent = Path.Combine(parent, "prepared-component");
        var preparedFile = Path.Combine(parent, "prepared-added.json");
        var addedFile = Path.Combine(installRoot, "added.json");
        try
        {
            Directory.CreateDirectory(Path.Combine(liveComponent, "nested"));
            File.WriteAllText(Path.Combine(liveComponent, "nested", "original.txt"), "original-bytes");
            File.WriteAllText(Path.Combine(installRoot, "untouched.txt"), "untouched-bytes");
            Directory.CreateDirectory(preparedComponent);
            File.WriteAllText(Path.Combine(preparedComponent, "replacement.txt"), "replacement-bytes");
            File.WriteAllText(preparedFile, "new-live-file");

            using (var transaction = InstallTransaction.Begin(installRoot, null,
                       InstallTransactionEntry.Directory(preparedComponent, liveComponent),
                       InstallTransactionEntry.File(preparedFile, addedFile)))
            {
                transaction.Publish();
                if (!File.Exists(Path.Combine(liveComponent, "replacement.txt")) ||
                    File.Exists(Path.Combine(liveComponent, "nested", "original.txt")) ||
                    File.ReadAllText(addedFile) != "new-live-file")
                    throw new Exception("The transaction did not publish its prepared replacements.");
                // Deliberately omit Commit. Dispose must restore the exact pre-transaction tree.
            }

            var original = Path.Combine(liveComponent, "nested", "original.txt");
            if (!File.Exists(original) || File.ReadAllText(original) != "original-bytes" ||
                File.Exists(Path.Combine(liveComponent, "replacement.txt")) || File.Exists(addedFile) ||
                Directory.Exists(preparedComponent) || File.Exists(preparedFile) ||
                File.ReadAllText(Path.Combine(installRoot, "untouched.txt")) != "untouched-bytes")
                throw new Exception("Disposing an uncommitted transaction did not restore the exact original tree.");
            if (File.Exists(InstallOperationPaths.GetJournalPath(installRoot)))
                throw new Exception("A rolled-back transaction left its journal behind.");
        }
        finally
        {
            if (Directory.Exists(parent)) Directory.Delete(parent, recursive: true);
        }
    }

    private static void TestInstallScratchRecovery()
    {
        var parent = Path.Combine(Path.GetTempPath(),
            "mkwc-scratch-self-test-" + Guid.NewGuid().ToString("N"));
        var installRoot = Path.Combine(parent, "install");
        var otherInstallRoot = Path.Combine(parent, "other-install");
        InstallScratchSpace? scratch = null;
        InstallScratchSpace? otherScratch = null;
        try
        {
            Directory.CreateDirectory(installRoot);
            Directory.CreateDirectory(otherInstallRoot);
            scratch = InstallScratchSpace.CreateSibling(installRoot);
            File.WriteAllText(Path.Combine(scratch.Root, "owned.txt"), "owned");

            // Scratch is keyed by the installation's scope id, so a neighbouring installation's
            // scratch lives beside it under a different prefix and must survive this recovery.
            otherScratch = InstallScratchSpace.CreateSibling(otherInstallRoot);
            var sentinel = Path.Combine(otherScratch.Root, "preserve.txt");
            File.WriteAllText(sentinel, "preserve");

            using (InstallOperationLock.Acquire(installRoot))
            {
                if (Directory.Exists(scratch.Root))
                    throw new Exception("Lock acquisition did not recover this installation's scratch space.");
                if (!File.Exists(sentinel) || File.ReadAllText(sentinel) != "preserve")
                    throw new Exception("Scratch recovery removed another installation's scratch space.");
            }
        }
        finally
        {
            scratch?.Dispose();
            otherScratch?.Dispose();
            if (Directory.Exists(parent)) Directory.Delete(parent, recursive: true);
        }
    }

    private static void TestCompileInputsFingerprint()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-compile-inputs-" + Guid.NewGuid().ToString("N"));
        try
        {
            var package = Path.Combine(root, "RetroRewind6");
            Directory.CreateDirectory(Path.Combine(package, "Binaries"));
            Directory.CreateDirectory(Path.Combine(package, "RaceAssets"));
            File.WriteAllText(Path.Combine(package, "Binaries", "Code.pul"), "code-A");
            File.WriteAllText(Path.Combine(package, "RaceAssets", "tracks.txt"), "assets-A");

            var first = CompileInputsFingerprint.Compute(package);
            var throughParent = CompileInputsFingerprint.Compute(root);
            if (first.CodePulSha256 != throughParent.CodePulSha256 ||
                first.CompileInputsSha256 != throughParent.CompileInputsSha256)
                throw new Exception("The compile identity changed when selected through its parent folder.");
            if (first.CompileInputsSha256 != CompileInputsFingerprint.ComputeCompileInputsSha256(package))
                throw new Exception("The compile-input hash is not deterministic.");

            // The runtime reads assets live from the canonical installation, so an asset-only Retro
            // Rewind update must not disturb any identity the backend keeps.
            File.WriteAllText(Path.Combine(package, "RaceAssets", "tracks.txt"), "assets-B");
            Directory.CreateDirectory(Path.Combine(package, "RaceAssets", "empty-runtime-directory"));
            var assetsChanged = CompileInputsFingerprint.Compute(package);
            if (assetsChanged.CodePulSha256 != first.CodePulSha256 ||
                assetsChanged.CompileInputsSha256 != first.CompileInputsSha256)
                throw new Exception("An asset-only Retro Rewind update changed a compile identity.");

            // The registered mod directories affect compilation even when empty.
            var files = Path.Combine(package, "files");
            Directory.CreateDirectory(files);
            var filesTopology = CompileInputsFingerprint.Compute(package);
            if (filesTopology.CompileInputsSha256 == assetsChanged.CompileInputsSha256)
                throw new Exception("The top-level files directory topology was not part of compile identity.");
            File.WriteAllText(Path.Combine(files, "runtime-asset.bin"), "asset");
            if (CompileInputsFingerprint.Compute(package).CompileInputsSha256 != filesTopology.CompileInputsSha256)
                throw new Exception("A normal files asset incorrectly triggered a recompile.");

            // Only Code.pul and the recorded file layout should trigger recompilation.
            File.WriteAllText(Path.Combine(package, "unrelated.cpp"), "// ignored");
            var straySource = CompileInputsFingerprint.Compute(package);
            if (straySource.CodePulSha256 != filesTopology.CodePulSha256 ||
                straySource.CompileInputsSha256 != filesTopology.CompileInputsSha256)
                throw new Exception("A stray source file changed a compile identity.");

            var codePulPath = Path.Combine(package, "Binaries", "Code.pul");
            var linkTarget = Path.Combine(package, "Binaries", "Code.pul.copy");
            File.WriteAllText(linkTarget, "linked");
            File.Delete(codePulPath);
            if (TryCreateFileLink(codePulPath, linkTarget))
            {
                try
                {
                    _ = CompileInputsFingerprint.Compute(package);
                    throw new Exception("A linked compile input was accepted into the compile identity.");
                }
                catch (InvalidDataException) { }
                finally
                {
                    File.Delete(codePulPath);
                }
            }

            File.WriteAllText(codePulPath, "code-B");
            var codeChanged = CompileInputsFingerprint.Compute(package);
            if (codeChanged.CodePulSha256 == assetsChanged.CodePulSha256 ||
                codeChanged.CompileInputsSha256 == assetsChanged.CompileInputsSha256)
                throw new Exception("A Code.pul update did not change both compile identities.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// A build snapshot must copy megabytes of compile inputs, not the multi-gigabyte asset tree,
    /// and must be provably the same inputs the canonical installation had when it was read.
    /// </summary>
    private static void TestCompileInputsSnapshot()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-snapshot-" + Guid.NewGuid().ToString("N"));
        try
        {
            var package = Path.Combine(root, "RetroRewind6");
            Directory.CreateDirectory(Path.Combine(package, "Binaries"));
            Directory.CreateDirectory(Path.Combine(package, "files", "Race", "Course"));
            Directory.CreateDirectory(Path.Combine(package, "PulsarEngine", "MarioKartWii"));
            File.WriteAllText(Path.Combine(package, "Binaries", "Code.pul"), "code-A");
            File.WriteAllText(Path.Combine(package, "files", "Race", "Course", "beginner_course.szs"),
                "a pretend multi-gigabyte asset");
            File.WriteAllText(Path.Combine(package, "PulsarEngine", "MarioKartWii", "patch.cpp"), "int patch;");

            var source = CompileInputsFingerprint.Compute(package);
            var snapshot = CompileInputsFingerprint.Snapshot(package, Path.Combine(root, "scratch"));
            if (snapshot.CodePulSha256 != source.CodePulSha256 ||
                snapshot.CompileInputsSha256 != source.CompileInputsSha256)
                throw new Exception("The compile-input snapshot is not identical to its source.");
            // The mod root name becomes the product's relative DVD overlay root, so it is contractual.
            if (Path.GetFileName(snapshot.RetroRewindRoot) != "RetroRewind6")
                throw new Exception("The compile-input snapshot is not named RetroRewind6.");
            if (!File.Exists(Path.Combine(snapshot.RetroRewindRoot, "Binaries", "Code.pul")))
                throw new Exception("The compile-input snapshot is missing the Code.pul.");
            if (File.Exists(Path.Combine(snapshot.RetroRewindRoot, "PulsarEngine", "MarioKartWii", "patch.cpp")))
                throw new Exception("The compile-input snapshot copied Pulsar sources no build consumes.");
            if (File.Exists(Path.Combine(snapshot.RetroRewindRoot, "files", "Race", "Course",
                    "beginner_course.szs")))
                throw new Exception("The compile-input snapshot copied the runtime asset tree.");
            if (!Directory.Exists(Path.Combine(snapshot.RetroRewindRoot, "files")))
                throw new Exception("The compile-input snapshot lost the compile-relevant files topology.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void TestCompileInputsFingerprintGoldenVector()
    {
        const string expected = GoldenCompileInputsDigest;
        var root = Path.Combine(Path.GetTempPath(), "mkwc-vector-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "Binaries"));
            Directory.CreateDirectory(Path.Combine(root, "tracks"));
            Directory.CreateDirectory(Path.Combine(root, "empty", "nested"));
            File.WriteAllBytes(Path.Combine(root, "Binaries", "Code.pul"), [0x00, 0x01, 0x02, 0xff]);
            File.WriteAllBytes(Path.Combine(root, "readme.txt"),
                System.Text.Encoding.UTF8.GetBytes("retro-rewind\n"));
            File.WriteAllBytes(Path.Combine(root, "tracks", "course.bin"), []);

            var actual = CompileInputsFingerprint.ComputeCompileInputsSha256(root);
            if (!actual.Equals(expected, StringComparison.Ordinal))
                throw new Exception($"Compile-inputs golden vector changed: {actual} != {expected}.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The compile-input digest decides whether an installed product must be recompiled, so it is
    /// pinned here: changing the hashed set or its encoding must be a deliberate edit of this value.
    /// </summary>
    private const string GoldenCompileInputsDigest =
        "19b2ead4f83f030aac110b82ac801605fa81b4ed86f7be6a1b2bb9dd20147184";

    private static void TestToolkitFingerprint()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-toolkit-" + Guid.NewGuid().ToString("N"));
        try
        {
            CreateFakeToolkit(root);
            var first = ToolkitFingerprint.Compute(root);
            if (first != ToolkitFingerprint.Compute(root))
                throw new Exception("The toolkit fingerprint is not stable.");

            // A file that has nothing to do with generated code must not invalidate every install.
            File.WriteAllText(Path.Combine(root, "Toolkit", "nodtool.exe"), "irrelevant");
            if (ToolkitFingerprint.Compute(root) != first)
                throw new Exception("An unrelated toolkit file changed the fingerprint.");

            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v2");
            var afterTranslator = ToolkitFingerprint.Compute(root);
            if (afterTranslator == first)
                throw new Exception("A new translator did not change the fingerprint.");

            File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp"), "int main2();");
            var afterRuntimeSource = ToolkitFingerprint.Compute(root);
            if (afterRuntimeSource == afterTranslator)
                throw new Exception("A runtime source change did not change the fingerprint.");

            var patch = Path.Combine(root, "BuildWorkspace", "aurora-main", "cmake", "patches",
                "sdl3.patch");
            Directory.CreateDirectory(Path.GetDirectoryName(patch)!);
            File.WriteAllText(patch, "patch-v1");
            var afterPatch = ToolkitFingerprint.Compute(root);
            if (afterPatch == afterRuntimeSource)
                throw new Exception("An Aurora patch consumed by the provider did not change compile identity.");

            var installation = new Installation(root);
            installation.WriteToolkitState(afterPatch, ToolkitFingerprint.ComputePackage(root),
                "wiicompiled-v1", ToolkitFingerprint.ComputeRuntimeAssets(root));
            var repair = new ProductRepairService(installation, new DelegatingInstallReporter(_ => { }));
            repair.VerifyToolkitForCompilation(afterPatch);

            void ExpectModifiedToolkitRejected(string description)
            {
                try
                {
                    repair.VerifyToolkitForCompilation(afterPatch);
                    throw new Exception(description);
                }
                catch (InvalidDataException) { }
            }

            File.WriteAllText(patch, "patch-v2");
            ExpectModifiedToolkitRejected("A modified mandatory Aurora patch was accepted for compilation.");
            File.Delete(patch);
            ExpectModifiedToolkitRejected("A deleted mandatory Aurora patch was accepted for compilation.");
            File.WriteAllText(patch, "patch-v1");
            repair.VerifyToolkitForCompilation(afterPatch);

            var runtimeAssets = ToolkitFingerprint.ComputeRuntimeAssets(root);
            var pipeline = Path.Combine(root, "BuildWorkspace", "runtime", "assets", "pipeline",
                "initial_pipeline_cache.db");
            File.WriteAllText(pipeline, "pipeline-v2");
            if (ToolkitFingerprint.Compute(root) != afterPatch)
                throw new Exception("A copied-only runtime asset needlessly changed compile identity.");
            var changedRuntimeAssets = ToolkitFingerprint.ComputeRuntimeAssets(root);
            if (changedRuntimeAssets == runtimeAssets)
                throw new Exception("A copied runtime asset did not change its publication identity.");

            var sourceEmptyDirectory = Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii",
                "empty-runtime-directory");
            Directory.CreateDirectory(sourceEmptyDirectory);
            var topologyChangedRuntimeAssets = ToolkitFingerprint.ComputeRuntimeAssets(root);
            if (topologyChangedRuntimeAssets == changedRuntimeAssets)
                throw new Exception("An empty copied runtime-asset directory was omitted from its identity.");

            // Cache-reuse subsets: translator or native registration changes invalidate translation
            // but not toolchain, since CMake/Ninja rebuild exactly the affected objects; a compiler
            // change invalidates only toolchain; a native_prebuilt provenance rewrite invalidates
            // neither. Without a current native index, runtime/src itself decides translation.
            var components = ToolkitFingerprint.ComputeComponents(root);
            if (components.Compile != afterPatch)
                throw new Exception("ComputeComponents disagrees with the compile identity.");
            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"),
                "translator-v3");
            var afterNewTranslator = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewTranslator.Translation == components.Translation)
                throw new Exception("A new translator did not change the translation identity.");
            if (afterNewTranslator.NativeToolchain != components.NativeToolchain)
                throw new Exception("A new translator needlessly changed the native toolchain identity.");
            var runtimeMain = Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp");
            File.WriteAllText(runtimeMain, "int main3();");
            var afterUnindexedRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterUnindexedRuntime.Translation == afterNewTranslator.Translation)
                throw new Exception("A runtime source change without a native index did not change the translation identity.");

            File.WriteAllText(runtimeMain, "int main();");
            if (WriteFakeNativeIndex(root, "\"a\"") !=
                "50b9779591f4b205751361c8899accd362f96ea5dfd32febed1bedca534c523a")
                throw new Exception("The native index source digest no longer matches the translator's.");
            var indexed = ToolkitFingerprint.ComputeComponents(root);
            File.WriteAllText(runtimeMain, "int main3();");
            WriteFakeNativeIndex(root, "\"a\"");
            var afterNewRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewRuntime.Translation != indexed.Translation)
                throw new Exception("A runtime source change with unchanged registrations changed the translation identity.");
            if (afterNewRuntime.NativeToolchain != indexed.NativeToolchain)
                throw new Exception("A runtime source change needlessly changed the native toolchain identity.");
            if (afterNewRuntime.Compile == indexed.Compile)
                throw new Exception("A runtime source change did not change the compile identity.");
            File.WriteAllText(runtimeMain, "int main4();");
            if (ToolkitFingerprint.ComputeComponents(root).Translation == afterNewRuntime.Translation)
                throw new Exception("A stale native index was trusted for the translation identity.");
            File.WriteAllText(runtimeMain, "int main3();");
            WriteFakeNativeIndex(root, "\"a\", \"b\"");
            afterNewRuntime = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewRuntime.Translation == indexed.Translation)
                throw new Exception("A native registration change did not change the translation identity.");
            if (afterNewRuntime.NativeToolchain != indexed.NativeToolchain)
                throw new Exception("A native registration change needlessly changed the native toolchain identity.");
            File.WriteAllText(Path.Combine(root, "Toolkit", "llvm-mingw", "bin", "clang-22.exe"),
                "clang-v2");
            var afterNewCompiler = ToolkitFingerprint.ComputeComponents(root);
            if (afterNewCompiler.NativeToolchain == afterNewRuntime.NativeToolchain)
                throw new Exception("A new compiler did not change the native toolchain identity.");
            if (afterNewCompiler.Translation != afterNewRuntime.Translation)
                throw new Exception("A new compiler needlessly changed the translation identity.");
            File.WriteAllText(Path.Combine(root, "BuildWorkspace", "Dependencies", "native_prebuilt",
                "provenance.json"), "{\"BuiltUtc\":\"rewritten\"}");
            if (ToolkitFingerprint.Compute(root) != afterNewCompiler.Compile)
                throw new Exception(
                    "A native_prebuilt provenance rewrite needlessly changed the compile identity.");

            var runtimeLink = Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii", "linked-assets");
            if (TryCreateDirectoryLink(runtimeLink,
                    Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii", "shared2")))
            {
                try
                {
                    try
                    {
                        _ = ToolkitFingerprint.ComputeRuntimeAssets(root);
                        throw new Exception("A runtime-asset reparse point was accepted into its identity.");
                    }
                    catch (InvalidDataException) { }
                }
                finally
                {
                    Directory.Delete(runtimeLink);
                }
            }

            var product = Path.Combine(root, "product");
            Directory.CreateDirectory(product);
            FileSystemUtilities.CopyDirectory(
                Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii"),
                Path.Combine(product, "wii_bootstrap"));
            File.Copy(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp", "dsp_coef.bin"),
                Path.Combine(product, "dsp_coef.bin"));
            File.Copy(pipeline, Path.Combine(product, "initial_pipeline_cache.db"));
            if (!ToolkitFingerprint.ProductRuntimeAssetsMatch(product, topologyChangedRuntimeAssets))
                throw new Exception("Copied product assets did not match their normalized source identity.");
            Directory.Delete(Path.Combine(product, "wii_bootstrap", "empty-runtime-directory"));
            if (ToolkitFingerprint.ProductRuntimeAssetsMatch(product, topologyChangedRuntimeAssets))
                throw new Exception("A missing copied runtime-asset directory was not detected.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>Writes a native index current for the fake runtime/src; returns its source digest.</summary>
    private static string WriteFakeNativeIndex(string root, string registrations)
    {
        var source = Path.Combine(root, "BuildWorkspace", "runtime", "src");
        var manifest = new System.Text.StringBuilder();
        foreach (var (relative, path) in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)
                     .Where(path => path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                                    path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                                    path.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase))
                     .Select(path => (Path.GetRelativePath(source, path).Replace('\\', '/'), path))
                     .OrderBy(item => item.Item1, StringComparer.Ordinal))
            manifest.Append(InputValidation.Sha256File(path)).Append("  ").Append(relative).Append('\n');
        var digest = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(manifest.ToString())))
            .ToLowerInvariant();
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "native-index.json"),
            "{\n  \"format\": \"wiicompiled-runtime-native-index\",\n  \"sourceDigest\": \"" + digest +
            "\",\n  \"registrations\": [" + registrations + "]\n}\n");
        return digest;
    }

    private static void CreateFakeToolkit(string root)
    {
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "Translator"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "CMake", "bin"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "Ninja"));
        Directory.CreateDirectory(Path.Combine(root, "Toolkit", "llvm-mingw", "bin"));
        foreach (var project in new[] { "mkwii", "mkwii-ntsc-u", "mkwii-ntsc-j" })
            Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "projects", project));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "src"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii", "shared2",
            "wc24"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp"));
        Directory.CreateDirectory(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "pipeline"));
        var aurora = Path.Combine(root, "BuildWorkspace", "aurora-main");
        Directory.CreateDirectory(aurora);
        File.WriteAllText(Path.Combine(aurora, "README.md"), "aurora source");
        foreach (var dependency in InstalledLayout.DependencyNames)
        {
            var directory = Path.Combine(root, "BuildWorkspace", "Dependencies", dependency);
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, ".mkwc-layout"), dependency);
        }
        File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v1");
        File.WriteAllText(Path.Combine(root, "Toolkit", "CMake", "bin", "cmake.exe"), "cmake");
        File.WriteAllText(Path.Combine(root, "Toolkit", "Ninja", "ninja.exe"), "ninja");
        foreach (var executable in new[]
                 {
                     "clang-22.exe", "ld.lld.exe", "x86_64-w64-mingw32-clang.exe",
                     "x86_64-w64-mingw32-clang++.exe", "x86_64-w64-mingw32-windres.exe"
                 })
            File.WriteAllText(Path.Combine(root, "Toolkit", "llvm-mingw", "bin", executable), executable);
        File.WriteAllText(Path.Combine(root, "Toolkit", "nodtool.exe"), "tool");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "LocalBuild.ps1"), "# build");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "NativeBuildFlags.ps1"), "# flags");
        foreach (var project in new[] { "mkwii", "mkwii-ntsc-u", "mkwii-ntsc-j" })
            File.WriteAllText(Path.Combine(root, "BuildWorkspace", "projects", project, "recomp.yml"), "profiles: {}");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.20)");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "src", "main.cpp"), "int main();");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "wii", "shared2", "wc24",
            "bootstrap.bin"), "bootstrap");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "dsp", "dsp_coef.bin"),
            "dsp");
        File.WriteAllText(Path.Combine(root, "BuildWorkspace", "runtime", "assets", "pipeline",
            "initial_pipeline_cache.db"), "pipeline");
    }

    private static void CopyFakeRuntimeAssets(string root, string productDirectory)
    {
        var source = Path.Combine(root, "BuildWorkspace", "runtime", "assets");
        FileSystemUtilities.CopyDirectory(Path.Combine(source, "wii"),
            Path.Combine(productDirectory, "wii_bootstrap"));
        File.Copy(Path.Combine(source, "dsp", "dsp_coef.bin"),
            Path.Combine(productDirectory, "dsp_coef.bin"), overwrite: true);
        File.Copy(Path.Combine(source, "pipeline", "initial_pipeline_cache.db"),
            Path.Combine(productDirectory, "initial_pipeline_cache.db"), overwrite: true);
    }

    private static bool TryCreateFileLink(string linkPath, string targetPath)
    {
        try
        {
            File.CreateSymbolicLink(linkPath, targetPath);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
        {
            return false;
        }
    }

    private static bool TryCreateDirectoryLink(string linkPath, string targetPath)
    {
        try
        {
            Directory.CreateSymbolicLink(linkPath, targetPath);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or NotSupportedException)
        {
            // Developer Mode / symlink permission is not guaranteed on a user's Windows system.
            // The identity check is still covered whenever the platform can create a link.
            return false;
        }
    }

    /// <summary>
    /// The full inspection state machine against the canonical Retro Rewind installation. The
    /// installation itself is never copied here, exactly as in production: only its compile inputs
    /// are ever read, and asset-only changes to it must leave the product current.
    /// </summary>
    private static void TestCodePulStateMachine()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-install-" + Guid.NewGuid().ToString("N"));
        try
        {
            CreateFakeToolkit(root);
            var installation = new Installation(root);
            var fingerprint = ToolkitFingerprint.Compute(root);
            installation.WriteToolkitState(fingerprint, ToolkitFingerprint.ComputePackage(root),
                "wiicompiled-v1", ToolkitFingerprint.ComputeRuntimeAssets(root));

            // Wheel Wizard's own Retro Rewind installation, deliberately outside the install root.
            var canonical = Path.Combine(root, "canonical", "RetroRewind6");
            Directory.CreateDirectory(Path.Combine(canonical, "Binaries"));
            Directory.CreateDirectory(Path.Combine(canonical, "PulsarEngine"));
            File.WriteAllText(Path.Combine(canonical, "Binaries", "Code.pul"), "code-pul-A");
            File.WriteAllText(Path.Combine(canonical, "PulsarEngine", "patch.cpp"), "int patch;");
            File.WriteAllText(Path.Combine(canonical, "asset.txt"), "asset-A");

            Directory.CreateDirectory(installation.BaseDirectory);
            Directory.CreateDirectory(installation.RetroDirectory);
            File.WriteAllBytes(installation.BaseExecutable, new byte[64 * 1024]);
            File.WriteAllBytes(installation.RetroExecutable, new byte[64 * 1024]);
            CopyFakeRuntimeAssets(root, installation.BaseDirectory);
            CopyFakeRuntimeAssets(root, installation.RetroDirectory);
            var gameSys = Path.Combine(installation.GameDataDirectory, "sys");
            var gameRel = Path.Combine(installation.GameDataDirectory, "files", "rel");
            Directory.CreateDirectory(gameSys);
            Directory.CreateDirectory(gameRel);
            File.WriteAllText(Path.Combine(gameSys, "main.dol"), "dol");
            File.WriteAllText(Path.Combine(gameSys, "fst.bin"), "fst");
            File.WriteAllText(Path.Combine(gameRel, "StaticR.rel"), "rel");
            var dolSha = InputValidation.Sha256File(Path.Combine(gameSys, "main.dol"));
            var relSha = InputValidation.Sha256File(Path.Combine(gameRel, "StaticR.rel"));
            var inputsA = CompileInputsFingerprint.Compute(canonical);
            LocalBuildService.WriteFingerprint(installation.BaseDirectory, BuildProfile.Base, fingerprint,
                dolSha, relSha, "", RetroWfcPayloadMode.NotApplicable);
            JsonState.Write(Path.Combine(installation.BaseDirectory, LocalBuildProvenance.FileName),
                new LocalBuildProvenance { SchemaVersion = 1, Profile = "base", DolSha256 = dolSha,
                    RelSha256 = relSha });
            JsonState.Write(Path.Combine(installation.RetroDirectory, ProductFingerprint.FileName),
                new ProductFingerprint
                {
                    Profile = "retro-rewind", ToolkitFingerprint = fingerprint, DolSha256 = dolSha, RelSha256 = relSha,
                    ExecutableSha256 = InputValidation.Sha256File(installation.RetroExecutable),
                    CodePulSha256 = inputsA.CodePulSha256,
                    RetroRewindCompileInputsSha256 = inputsA.CompileInputsSha256,
                    RetroWfcPayloadMode = "skipped"
                });
            JsonState.Write(Path.Combine(installation.RetroDirectory, LocalBuildProvenance.FileName),
                new LocalBuildProvenance { SchemaVersion = 1, Profile = "retro-rewind", DolSha256 = dolSha,
                    RelSha256 = relSha, CodePulSha256 = inputsA.CodePulSha256,
                    RetroWfcPayloadMode = "skipped" });
            JsonState.Write(installation.InstallStatePath, new InstallState
            {
                InstallDir = root, RetroRewindInstalled = true,
                DolSha256 = dolSha, RelSha256 = relSha,
                RetroRewindCodePulSha256 = inputsA.CodePulSha256,
                RetroRewindCompileInputsSha256 = inputsA.CompileInputsSha256,
                RetroWfcPayloadMode = "skipped",
                RetroRewindRoot = canonical
            });

            ProductState Inspect() =>
                installation.CheckRetroRewind(installation.ResolveToolkitFingerprint(),
                    CompileInputsFingerprint.Compute(canonical));

            void Expect(ProductStatus expected, string description)
            {
                var inspected = Inspect();
                if (inspected.Status != expected)
                    throw new Exception(
                        $"{description}: expected {expected}, got {inspected.Status}: {inspected.Detail}");
            }

            Expect(ProductStatus.Current, "A freshly built Retro Rewind");

            // The whole point of the canonical-source model: assets change constantly and the
            // runtime reads them live, so they must never make the backend rebuild anything.
            File.WriteAllText(Path.Combine(canonical, "asset.txt"), "asset-B");
            Directory.CreateDirectory(Path.Combine(canonical, "new-course-directory"));
            Expect(ProductStatus.Current, "An asset-only Retro Rewind update");

            var unreadableCanonical = installation.CheckRetroRewind(fingerprint, null,
                "The canonical Retro Rewind folder cannot be read: missing.");
            if (unreadableCanonical.Status != ProductStatus.InputsMissing ||
                !unreadableCanonical.ActionRequired)
                throw new Exception("A missing canonical Retro Rewind folder did not fail closed.");

            var unchangedInputs = ProductRepairService.FindDesiredInputDrift(installation, fingerprint,
                dolSha, relSha);
            if (unchangedInputs.Base || unchangedInputs.Retro)
                throw new Exception("Current authoritative game inputs requested an unnecessary rebuild.");
            var changedInputs = ProductRepairService.FindDesiredInputDrift(installation, fingerprint,
                new string('a', 64), relSha);
            if (!changedInputs.Base || !changedInputs.Retro)
                throw new Exception("Desired DOL drift did not force both installed products to rebuild.");

            File.Delete(installation.ToolkitStatePath);
            if (!string.IsNullOrEmpty(installation.ResolveToolkitFingerprint()) ||
                Inspect().Status != ProductStatus.Blocked)
                throw new Exception("A missing toolkit-state.json was inferred instead of failing closed.");
            installation.WriteToolkitState(fingerprint, ToolkitFingerprint.ComputePackage(root),
                "wiicompiled-v1", ToolkitFingerprint.ComputeRuntimeAssets(root));
            Expect(ProductStatus.Current, "A restored toolkit-state.json");

            var cmake = Path.Combine(root, "Toolkit", "CMake", "bin", "cmake.exe");
            File.Delete(cmake);
            if (!string.IsNullOrEmpty(installation.ResolveToolkitFingerprint()) ||
                Inspect().Status != ProductStatus.Blocked)
                throw new Exception("A missing bundled build tool did not block product inspection.");
            File.WriteAllText(cmake, "cmake");
            Expect(ProductStatus.Current, "A restored bundled build tool");

            var auroraMarker = Path.Combine(root, "BuildWorkspace", "aurora-main", "README.md");
            File.Delete(auroraMarker);
            if (!string.IsNullOrEmpty(installation.ResolveToolkitFingerprint()) ||
                Inspect().Status != ProductStatus.Blocked)
                throw new Exception("A missing Aurora source tree did not block product inspection early.");
            File.WriteAllText(auroraMarker, "aurora source");

            var dependencyMarker = Path.Combine(root, "BuildWorkspace", "Dependencies", "fmt", ".mkwc-layout");
            File.Delete(dependencyMarker);
            if (!string.IsNullOrEmpty(installation.ResolveToolkitFingerprint()) ||
                Inspect().Status != ProductStatus.Blocked)
                throw new Exception("An empty required dependency tree did not block product inspection early.");
            File.WriteAllText(dependencyMarker, "fmt");
            Expect(ProductStatus.Current, "A restored required toolkit layout");

            var baseDsp = Path.Combine(installation.BaseDirectory, "dsp_coef.bin");
            File.Delete(baseDsp);
            var missingBaseRuntimeAssets = installation.CheckBase(installation.ResolveToolkitFingerprint());
            if (missingBaseRuntimeAssets.Status != ProductStatus.Broken ||
                installation.CheckBaseCore(installation.ResolveToolkitFingerprint()).Status !=
                ProductStatus.Current)
                throw new Exception("Missing copied base runtime assets were not reported as broken.");

            var retroPipeline = Path.Combine(installation.RetroDirectory, "initial_pipeline_cache.db");
            File.Delete(retroPipeline);
            if (Inspect().Status != ProductStatus.Broken)
                throw new Exception("Missing copied Retro Rewind runtime assets were not reported as broken.");

            // Copied support files are republished from the installed workspace: repairing them must
            // never require the translator or the compiler.
            using (InstallOperationLock.Acquire(root))
            {
                var repair = new ProductRepairService(installation, new DelegatingInstallReporter(_ => { }));
                _ = repair.RepairBaseAsync(new ProductRepairService.ReconcileOptions())
                    .GetAwaiter().GetResult();
            }
            if (!File.Exists(baseDsp) ||
                installation.CheckBase(installation.ResolveToolkitFingerprint()).Status != ProductStatus.Current)
                throw new Exception("Runtime-only repair did not restore the base support assets and health.");
            if (!File.Exists(retroPipeline))
                throw new Exception("Runtime-only repair did not restore the Retro Rewind support assets.");
            Expect(ProductStatus.Current, "Runtime-only Retro Rewind repair");

            // Pulsar sources are no longer compile inputs; only the Code.pul built from them is.
            File.WriteAllText(Path.Combine(canonical, "PulsarEngine", "patch.cpp"), "int patched;");
            Expect(ProductStatus.Current, "A changed Pulsar source without a Code.pul change");
            File.WriteAllText(Path.Combine(canonical, "PulsarEngine", "patch.cpp"), "int patch;");

            File.WriteAllText(Path.Combine(canonical, "Binaries", "Code.pul"), "code-pul-B");
            Expect(ProductStatus.CodePulChanged, "A changed canonical Code.pul");
            File.WriteAllText(Path.Combine(canonical, "Binaries", "Code.pul"), "code-pul-A");
            Expect(ProductStatus.Current, "A restored canonical Code.pul");

            var alteredExecutable = File.ReadAllBytes(installation.RetroExecutable);
            alteredExecutable[0] = 1;
            File.WriteAllBytes(installation.RetroExecutable, alteredExecutable);
            var tamperedExecutable = Inspect();
            if (tamperedExecutable.Status != ProductStatus.Blocked || !tamperedExecutable.ActionRequired)
                throw new Exception("A modified Retro Rewind executable did not fail closed.");
            File.WriteAllBytes(installation.RetroExecutable, new byte[64 * 1024]);
            Expect(ProductStatus.Current, "A restored Retro Rewind executable");

            File.Delete(installation.RetroExecutable);
            Expect(ProductStatus.Broken, "A missing Retro Rewind executable");

            File.WriteAllBytes(installation.RetroExecutable, new byte[64 * 1024]);
            File.WriteAllText(Path.Combine(root, "Toolkit", "Translator", "Translator.Cli.exe"), "translator-v2");
            installation.WriteToolkitState(ToolkitFingerprint.Compute(root),
                ToolkitFingerprint.ComputePackage(root), "wiicompiled-v2",
                ToolkitFingerprint.ComputeRuntimeAssets(root));
            Expect(ProductStatus.ToolkitChanged, "A replaced translator");
            if (installation.CheckBase(installation.ResolveToolkitFingerprint()).Status != ProductStatus.ToolkitChanged)
                throw new Exception("A replaced translator did not invalidate the base product.");
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>
    /// The frontend matches the products line's keys case-sensitively and branches on the typed
    /// status alone, so the exact v1 shape is pinned here rather than left to a serializer default.
    /// </summary>
    private static void TestProductsReport()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkwc-products-" + Guid.NewGuid().ToString("N"));
        try
        {
            // A path nothing was installed to must answer "absent", not fail and not ask for a rebuild.
            var missing = ConsoleCommands.InspectProducts(new Installation(root));
            if (missing.RebuildRequired || missing.Base.Reason != "absent" ||
                missing.RetroRewind.Reason != "absent")
                throw new Exception("A missing installation directory was not reported as absent.");
            if (Directory.Exists(root))
                throw new Exception("Inspecting a missing installation created the directory.");

            Directory.CreateDirectory(root);
            var empty = ConsoleCommands.InspectProducts(new Installation(root));
            if (empty.RebuildRequired || empty.Base.Reason != "absent" || empty.RetroRewind.Reason != "absent")
                throw new Exception("An empty installation directory was not reported as absent.");
            if (File.Exists(Path.Combine(root, ToolkitState.FileName)))
                throw new Exception("Inspecting an empty installation wrote installation state.");

            var json = ConsoleCommands.SerializeProductsReport(empty);
            foreach (var key in new[]
                     {
                         "\"type\":\"products\"", "\"setupVersion\":", "\"installDir\":",
                          "\"rebuildRequired\":false", "\"base\":{\"status\":\"absent\",\"detail\":",
                          "\"retroRewind\":{\"status\":\"absent\",\"detail\":"
                     })
                if (!json.Contains(key, StringComparison.Ordinal))
                    throw new Exception($"The products line is missing {key}: {json}");
            // v1 reports a status and a detail per product and nothing else; a frontend must not be
            // able to read a second, possibly disagreeing, opinion out of the same record.
            foreach (var removed in new[] { "\"health\"", "\"action\"", "\"compileRequired\"",
                         "\"packageSyncRequired\"", "\"actionRequired\"" })
                if (json.Contains(removed, StringComparison.Ordinal))
                    throw new Exception($"The products line still carries {removed}: {json}");
            foreach (var wrongCase in new[] { "\"Base\"", "\"RetroRewind\"", "\"SetupVersion\"",
                         "\"InstallDir\"", "\"RebuildRequired\"", "\"Status\"", "\"Detail\"" })
                if (json.Contains(wrongCase, StringComparison.Ordinal))
                    throw new Exception($"The products line used PascalCase for {wrongCase}: {json}");

            foreach (var status in new[]
                     {
                         ProductStatus.Absent, ProductStatus.Current, ProductStatus.ToolkitChanged,
                         ProductStatus.CodePulChanged, ProductStatus.CompileInputsChanged,
                         ProductStatus.PayloadChanged, ProductStatus.InputsMissing,
                         ProductStatus.Blocked, ProductStatus.Broken
                     })
            {
                var reason = new ProductState(status, "").Reason;
                if (!new[]
                    {
                        "absent", "current", "toolkit-changed", "code-pul-changed",
                        "compile-inputs-changed", "payload-changed", "inputs-missing", "blocked",
                        "broken"
                    }.Contains(reason, StringComparer.Ordinal))
                    throw new Exception($"{status} reports the undeclared status {reason}.");
            }
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    private static void Test(string name, Action action, List<string> failures)
    {
        try { action(); }
        catch (Exception ex) { failures.Add($"{name}: {ex.Message}"); }
    }
}
