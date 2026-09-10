namespace WiiCompiled.Setup.Linux;

internal static class ProductInfo
{
    public const string Name = "WiiCompiled";
    public const string Version = "0.2.31";
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
}
