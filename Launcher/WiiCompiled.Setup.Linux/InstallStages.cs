namespace WiiCompiled.Setup.Linux;

/// <summary>Stable stage identifiers reported by <c>--progress-json</c>.</summary>
internal static class InstallStages
{
    public const string Validate = "validate";
    public const string ExtractDisc = "extract-disc";
    public const string Build = "build";
    public const string Shortcuts = "shortcuts";
}
