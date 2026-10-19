namespace WiiCompiled.Setup.Windows;

/// <summary>
/// Stable stage identifiers reported by <c>--progress-json</c>. These are part of the public
/// WheelWizard contract (docs/WHEELWIZARD_CONTRACT.md): the strings may gain new members, but an
/// existing identifier never changes meaning.
/// </summary>
internal static class InstallStages
{
    public const string Validate = "validate";
    public const string ExtractToolkit = "extract-toolkit";
    public const string ExtractDisc = "extract-disc";
    public const string PrepareRetro = "prepare-retro";
    public const string BuildBase = "build-base";
    public const string BuildRetro = "build-retro";
    public const string Publish = "publish";
}
