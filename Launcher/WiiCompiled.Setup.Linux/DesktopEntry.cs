namespace WiiCompiled.Setup.Linux;

/// <summary>
/// freedesktop.org .desktop application-menu entries. Replaces ShellIntegration.cs's registry
/// uninstall entry (no Linux analogue for an unpackaged tool - Windows already skips that step for
/// portable installs, this just applies that same behavior universally) and .lnk shortcuts.
/// </summary>
internal static class DesktopEntry
{
    private static string ApplicationsDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "applications");

    private static string PathFor(string profile) =>
        Path.Combine(ApplicationsDirectory, $"wiicompiled-{profile}.desktop");

    public static void Create(string profile, string displayName, string exePath)
    {
        // macOS has no freedesktop menu; the installed .app bundle is the launchable entry itself.
        if (OperatingSystem.IsMacOS()) return;
        // exePath is the installed native runtime binary itself (e.g.
        // .../Install/Base/WiiCompiled) - each profile already gets its own .desktop file here,
        // so there is no need to route through the setup tool's own launch-base/launch-retro
        // subcommand dispatch first. Unquoted: the Desktop Entry spec's Exec grammar doesn't take
        // a bare '"'-wrapped path, and none is needed here anyway - the only part of this path
        // that varies is the username, which Unix forbids containing whitespace.
        Directory.CreateDirectory(ApplicationsDirectory);
        var contents =
            "[Desktop Entry]\n" +
            "Type=Application\n" +
            $"Name={displayName}\n" +
            $"Exec={exePath}\n" +
            "Categories=Game;\n" +
            "Terminal=false\n";
        File.WriteAllText(PathFor(profile), contents);
    }

    public static void Remove(string profile)
    {
        var path = PathFor(profile);
        if (File.Exists(path)) File.Delete(path);
    }
}
