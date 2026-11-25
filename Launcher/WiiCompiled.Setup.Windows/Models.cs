using System.Text.Json.Serialization;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

internal enum RetroWfcPayloadMode
{
    NotApplicable,
    Online,
    Skipped
}

internal sealed class InstallOptions
{
    public required string GamePath { get; init; }

    /// <summary>
    /// The canonical Retro Rewind installation Wheel Wizard owns. Its resolved path is recorded as
    /// <c>retro_rewind_root</c>; only its compile inputs are ever snapshotted.
    /// </summary>
    public string? RetroDirectoryPath { get; init; }

    public RetroWfcPayloadMode RetroWfcPayloadMode { get; init; }
    public required string InstallDirectory { get; init; }

    /// <summary>
    /// Establish a portable installation: the parent of <see cref="InstallDirectory"/> becomes the
    /// portable root, gets the <c>portable.txt</c> marker and a <c>UserData</c> directory, and all
    /// runtime user state and <c>[paths]</c> settings stay inside it.
    /// </summary>
    public bool Portable { get; init; }

    public bool HasRetroRewind => RetroDirectoryPath is not null;
}

/// <summary>A clean retail release the payload installs, as pinned by its projects/*/recomp.yml.</summary>
internal sealed class GameRelease
{
    public string GameId { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string RelSha256 { get; set; } = "";
}

internal sealed class PayloadManifest
{
    public const int CurrentSchemaVersion = 3;

    public int SchemaVersion { get; set; }
    public string ProductVersion { get; set; } = "";
    public List<GameRelease> SupportedReleases { get; set; } = [];
    public string ToolkitReleaseTag { get; set; } = "";
    public string BuildModel { get; set; } = "";

    /// <summary>
    /// Content identities computed once at release-build time (<c>Build-Installer.ps1 --emit-payload-identities</c>)
    /// so install doesn't re-hash gigabytes per user; compilation alone re-derives the toolkit identity from disk.
    /// </summary>
    public string ToolkitFingerprint { get; set; } = "";
    public string ToolkitPackageFingerprint { get; set; } = "";
    public string RuntimeAssetsFingerprint { get; set; } = "";
    public string TranslationFingerprint { get; set; } = "";
    public string NativeToolchainFingerprint { get; set; } = "";
}

internal sealed class DiscHeader
{
    [JsonPropertyName("game_id")]
    public string GameId { get; set; } = "";

    [JsonPropertyName("internal_name")]
    public string InternalName { get; set; } = "";

    [JsonPropertyName("region")]
    public string Region { get; set; } = "";

    [JsonPropertyName("revision")]
    public int Revision { get; set; }
}

internal sealed class InstallState
{
    [JsonRequired]
    public int SchemaVersion { get; set; } = 1;

    /// <summary>
    /// The setup version that produced this installation. This is the field WheelWizard compares
    /// against the latest published <c>v*</c> release tag; see docs/WHEELWIZARD_CONTRACT.md.
    /// </summary>
    public string SetupVersion { get; set; } = ProductInfo.Version;

    public string ProductVersion { get; set; } = ProductInfo.Version;

    /// <summary>The directory this state describes, so a frontend can confirm what it found.</summary>
    public string InstallDir { get; set; } = "";

    public DateTime InstalledUtc { get; set; } = DateTime.UtcNow;
    public bool RetroRewindInstalled { get; set; }
    public string ToolkitReleaseTag { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string RelSha256 { get; set; } = "";
    public string RetroRewindCodePulSha256 { get; set; } = "";
    public string RetroRewindCompileInputsSha256 { get; set; } = "";
    public string RetroWfcPayloadMode { get; set; } = "";
    public string RetroWfcPayloadSha256 { get; set; } = "";
    public long RetroWfcPayloadLength { get; set; }

    /// <summary>
    /// The <c>RetroRewind6</c> directory this installation was pointed at. The runtime's actual copy is
    /// <c>[paths] retro_rewind_root</c>; this mirror lets uninstall drop only the setting it owns.
    /// </summary>
    public string RetroRewindRoot { get; set; } = "";
}

/// <summary>
/// Written next to every locally produced product (<c>Base</c>, <c>RetroRewind</c>): the exact inputs
/// it was built from, so launch/update can decide without re-running anything whether it's still correct.
/// </summary>
internal sealed class ProductFingerprint
{
    [JsonRequired]
    public int SchemaVersion { get; set; } = 1;
    public string Profile { get; set; } = "";
    public string SetupVersion { get; set; } = ProductInfo.Version;
    public string ToolkitFingerprint { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string RelSha256 { get; set; } = "";
    /// <summary>
    /// Identity of the exact executable produced by this build. A valid input provenance is not
    /// enough if a partial copy, third-party replacement, or disk corruption changed the product
    /// after it was built.
    /// </summary>
    public string ExecutableSha256 { get; set; } = "";
    public string CodePulSha256 { get; set; } = "";
    /// <summary>
    /// Identity of every translator-consumed Retro Rewind input this product was built from. An
    /// asset-only change to the canonical installation does not alter it, which is exactly why an
    /// asset-only Retro Rewind update needs no backend work at all.
    /// </summary>
    public string RetroRewindCompileInputsSha256 { get; set; } = "";
    public string RetroWfcPayloadMode { get; set; } = "";
    public string RetroWfcPayloadSha256 { get; set; } = "";
    public long RetroWfcPayloadLength { get; set; }
    public DateTimeOffset BuiltUtc { get; set; } = DateTimeOffset.UtcNow;

    public const string FileName = "build-fingerprint.json";
}

/// <summary>
/// Written whenever the toolkit (translator + build workspace + compiler) is installed or replaced;
/// records compile, packaged-tool, and copied-runtime identities. Launch checks compare the compile
/// identity against each product's <see cref="ProductFingerprint.ToolkitFingerprint"/> in O(1).
/// </summary>
internal sealed class ToolkitState
{
    [JsonRequired]
    public int SchemaVersion { get; set; } = 2;
    public string ToolkitFingerprint { get; set; } = "";
    public string ToolkitPackageFingerprint { get; set; } = "";
    /// <summary>Identity of the runtime assets copied verbatim beside each product.</summary>
    public string RuntimeAssetsFingerprint { get; set; } = "";
    public string TranslationFingerprint { get; set; } = "";
    public string NativeToolchainFingerprint { get; set; } = "";
    public string ToolkitReleaseTag { get; set; } = "";
    public string SetupVersion { get; set; } = ProductInfo.Version;
    public DateTimeOffset UpdatedUtc { get; set; } = DateTimeOffset.UtcNow;

    public const string FileName = "toolkit-state.json";
}
