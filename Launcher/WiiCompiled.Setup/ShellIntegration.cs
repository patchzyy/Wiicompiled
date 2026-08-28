using Microsoft.Win32;

namespace WiiCompiled.Setup;

internal static class ShellIntegration
{
    private const string ShortcutFileName = "wiicompiled (base) (beta).lnk";

    public static void RegisterUninstaller(string installDirectory, bool retroInstalled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(ProductInfo.UninstallKey, writable: true)
                        ?? throw new InvalidOperationException("Could not register the uninstaller.");
        var cli = Path.Combine(installDirectory, ProductInfo.SetupCopyName);
        key.SetValue("DisplayName", ProductInfo.Name);
        key.SetValue("DisplayVersion", ProductInfo.Version);
        key.SetValue("Publisher", "WiiCompiled");
        key.SetValue("InstallLocation", installDirectory);
        key.SetValue("DisplayIcon", cli);
        key.SetValue("UninstallString", $"\"{cli}\" --uninstall --install-dir \"{installDirectory}\"");
        key.SetValue("QuietUninstallString", $"\"{cli}\" --silent-uninstall --install-dir \"{installDirectory}\"");
        key.SetValue("NoModify", 1, RegistryValueKind.DWord);
        key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        key.SetValue("EstimatedSize", EstimateSizeKb(installDirectory), RegistryValueKind.DWord);
        key.SetValue("Comments", retroInstalled ? "Includes the Retro Rewind profile" : "WiiCompiled");
    }

    public static void UnregisterUninstaller() =>
        Registry.CurrentUser.DeleteSubKeyTree(ProductInfo.UninstallKey, throwOnMissingSubKey: false);

    /// <summary>Creates the desktop and Start Menu shortcuts that launch the base game.</summary>
    public static void CreateShortcuts(string installDirectory)
    {
        var cli = Path.Combine(installDirectory, ProductInfo.SetupCopyName);
        var shellType = Type.GetTypeFromProgID("WScript.Shell")
                        ?? throw new InvalidOperationException("The Windows Script Host shell is unavailable.");
        dynamic shell = Activator.CreateInstance(shellType)!;
        foreach (var path in ShortcutPaths())
        {
            dynamic shortcut = shell.CreateShortcut(path);
            shortcut.TargetPath = cli;
            shortcut.Arguments = "--launch-base";
            shortcut.WorkingDirectory = installDirectory;
            shortcut.IconLocation = cli + ",0";
            shortcut.Description = "Play Mario Kart Wii (base game)";
            shortcut.Save();
        }
    }

    public static void RemoveShortcuts() => RemoveShortcuts(ShortcutPaths());

    internal static void RemoveShortcuts(IEnumerable<string> shortcutPaths)
    {
        var failures = new List<Exception>();
        foreach (var path in shortcutPaths)
            DeleteFileBestEffort(path, failures);

        if (failures.Count != 0)
            throw new AggregateException("One or more WiiCompiled shortcuts could not be removed.", failures);
    }

    private static string[] ShortcutPaths() =>
    [
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutFileName),
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.StartMenu), "Programs", ShortcutFileName),
    ];

    private static void DeleteFileBestEffort(string path, List<Exception> failures)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception ex)
        {
            failures.Add(new IOException($"Could not delete shortcut {path}: {ex.Message}", ex));
        }
    }

    private static int EstimateSizeKb(string directory)
    {
        try
        {
            // Enumerating FileInfo directly reuses the size the directory walk already reads from
            // the filesystem, instead of re-stat'ing every one of the ~26,000 installed files a
            // second time via `new FileInfo(path).Length`.
            var bytes = new DirectoryInfo(directory).EnumerateFiles("*", SearchOption.AllDirectories)
                .Sum(file => file.Length);
            return (int)Math.Min(int.MaxValue, (bytes + 1023) / 1024);
        }
        catch
        {
            return 0;
        }
    }
}
