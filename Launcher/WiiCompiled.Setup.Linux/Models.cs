using System.Reflection;

namespace WiiCompiled.Setup.Linux;

internal static class ProductInfo
{
    public const string Name = "WiiCompiled";
    public static readonly string Version =
        typeof(ProductInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()!
            .InformationalVersion;
}

/// <summary>One installed product's record inside install-state.json.</summary>
internal sealed class ProductInstallRecord
{
    public string Profile { get; set; } = "";
    public string InstallDirectory { get; set; } = "";
    public string ExecutableName { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string RelSha256 { get; set; } = "";
    public string BuiltUtc { get; set; } = "";
}

/// <summary>
/// The whole flat state document this tool keeps at ~/.local/share/WiiCompiled/install-state.json.
/// Deliberately not a fingerprint tree: local-build.sh already does its own incremental-rebuild
/// caching, so this only needs to remember where things were installed and what they were built
/// against, not decide when to rebuild.
/// </summary>
internal sealed class InstallState
{
    public int SchemaVersion { get; set; } = 1;
    public string Workspace { get; set; } = "";
    public List<ProductInstallRecord> Products { get; set; } = new();

    /// <summary>
    /// The <c>DATA</c> folder of a game Wheel Wizard already extracted (<c>--game-dir</c>), read in place through
    /// <c>dvd_root</c>. Empty when the game was extracted into the workspace's own <c>Assets/DATA</c>.
    /// </summary>
    public string GameDataDir { get; set; } = "";
}
