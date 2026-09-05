using System.Text.Json;
using WiiCompiled.Setup.Common;
using WiiCompiled.Setup.Windows;
using Xunit;

public sealed class ContractTests : IDisposable
{
    readonly string root = Path.Combine(Path.GetTempPath(), "mkwc-tests-" + Guid.NewGuid().ToString("N"));
    string Install => Path.Combine(root, "Install");
    public ContractTests() => Directory.CreateDirectory(root);
    public void Dispose() => Directory.Delete(root, true);

    [Fact]
    public void RejectsAmbiguousOrIncompleteCommands()
    {
        foreach (string[] args in new string[][] { ["--silent", "--launch-base"], ["--silent", "--game"], ["--unknown"], [] })
            Assert.Throws<ArgumentException>(() => Options.Parse(args));
    }

    [Fact]
    public void ParsesPathsWithoutChangingSpacesQuotesOrUnicode()
    {
        var path = "/Users/Zoë/Games/a \"quoted\" disc.rvz";
        var options = Options.Parse(["--silent", "--game", path, "--install-dir", Install, "--portable", "--retro-dir", root, "--download-retro-wfc-payload"]);
        Assert.Equal(path, options.Game);
        Assert.True(options.Download);
        Assert.False(options.Skip);
        Assert.True(options.Portable);
    }

    [Fact]
    public void MissingOrOldStateCannotAuthorizeLaunch()
    {
        Assert.True(MacSetup.Check(Install, null, null).NeedsRepair);
        Assert.True(MacSetup.Check(Install, new State { SchemaVersion = 0 }, null).NeedsRepair);
    }

    [Fact]
    public void PortableRootIsFoundFromNativeAppExecutableDirectory()
    {
        PortableRoot.Create(root);
        var executableDirectory = Path.Combine(Install, "RetroRewind.app", "Contents", "MacOS");
        Directory.CreateDirectory(executableDirectory);
        Assert.Equal(root, PortableRoot.TryFind(executableDirectory));
        Assert.Equal(Path.Combine(root, "UserData", "Config.toml"), RuntimeConfiguration.ResolveConfigPath(executableDirectory));
    }

    [Fact]
    public void ProductReportDetectsTamperingAndDoesNotRebuildForAssetOnlyUpdates()
    {
        PortableRoot.Create(root);
        var retro = Path.Combine(root, "RetroRewind6");
        Directory.CreateDirectory(Path.Combine(retro, "Binaries"));
        File.WriteAllText(Path.Combine(retro, "Binaries", "Code.pul"), "compile input");
        Directory.CreateDirectory(Path.Combine(root, "DATA"));
        var config = RuntimeConfiguration.ResolveConfigPath(Install);
        RuntimeConfiguration.SetDvdRoot(config, Path.Combine(root, "DATA"));
        RuntimeConfiguration.SetRetroRewindRoot(config, retro);
        string MakeProduct(string product)
        {
            var contents = Path.Combine(Install, product + ".app", "Contents");
            Directory.CreateDirectory(Path.Combine(contents, "MacOS"));
            Directory.CreateDirectory(Path.Combine(contents, "Resources", "wii_bootstrap"));
            File.WriteAllText(Path.Combine(contents, "Resources", "dsp_coef.bin"), "dsp");
            File.WriteAllText(Path.Combine(contents, "Resources", "initial_pipeline_cache.db"), "pipelines");
            var binary = Path.Combine(contents, "MacOS", product);
            File.WriteAllText(binary, "test executable");
            return binary;
        }
        var baseBinary = MakeProduct("WiiCompiled");
        var retroBinary = MakeProduct("RetroRewind");
        var state = new State { SchemaVersion = 1, SetupVersion = MacSetup.Version, InstallDir = Install,
            RetroRewindInstalled = true, RetroRoot = retro, CompileHash = CompileInputsFingerprint.Compute(retro).CompileInputsSha256,
            ToolkitHash = MacSetup.ToolkitHash(), BaseHash = MacSetup.HashTree(Path.Combine(Install, "WiiCompiled.app")), RetroHash = MacSetup.HashTree(Path.Combine(Install, "RetroRewind.app")) };
        Assert.False(MacSetup.Check(Install, state, retro).NeedsRepair);
        File.WriteAllText(Path.Combine(retro, "track.szs"), "asset update");
        Assert.False(MacSetup.Check(Install, state, retro).NeedsRepair);
        File.WriteAllText(Path.Combine(retro, "Binaries", "Code.pul"), "new code");
        Assert.Equal("compile-inputs-changed", MacSetup.Check(Install, state, retro).Retro.Status);
        File.WriteAllText(baseBinary, "tampered");
        Assert.Equal("broken", MacSetup.Check(Install, state, retro).Base.Status);
        var serialized = JsonSerializer.Serialize(state, MacSetup.Json);
        Assert.Contains("\"schemaVersion\":1", serialized);
    }
    [Fact]
    public void InterruptedPublicationRestoresPreviousInstallAndConfiguration()
    {
        PortableRoot.Create(root);
        Directory.CreateDirectory(Install + ".previous");
        File.WriteAllText(Path.Combine(Install + ".previous", "previous.txt"), "old product");
        Directory.CreateDirectory(Install);
        File.WriteAllText(Path.Combine(Install, "partial.txt"), "new product");
        var config = RuntimeConfiguration.ResolveConfigPath(Install);
        File.WriteAllText(config, "old config");
        var snapshot = RuntimeConfiguration.Capture(config);
        File.WriteAllText(Install + ".config-backup", JsonSerializer.Serialize(snapshot, MacSetup.Json));
        File.WriteAllText(config, "new config");
        MacSetup.Recover(Install);
        Assert.True(File.Exists(Path.Combine(Install, "previous.txt")));
        Assert.False(File.Exists(Path.Combine(Install, "partial.txt")));
        Assert.Equal("old config", File.ReadAllText(config));
        Assert.False(File.Exists(Install + ".config-backup"));
    }

}
