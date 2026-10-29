using System.Runtime.InteropServices;

namespace WiiCompiled.Setup.Common;

/// <summary>
/// Resolves the `nodtool` binary both installers use for Wii disc validation/extraction (see
/// NodToolInfoParser.cs), replacing the earlier dependency on `dolphin-tool`/`DolphinTool.exe`. A
/// caller can supply one directly; otherwise this downloads the matching prebuilt release binary
/// from encounter/nod and caches it at Launcher/artifacts/nodtool[.exe].
///
/// Shared by: WiiCompiled.Setup.Linux/DiscTool.cs (falls back to this at end-user install time on
/// a plain git checkout), and WiiCompiled.Setup.Common.Cli (invoked once at packaging time by both
/// build-appimage.sh and Build-Installer.ps1 to acquire the copy each bundles).
/// </summary>
public static class NodToolProvider
{
    public const string Version = "v2.0.0-alpha.10";

    public static async Task<string> ResolveAsync(string workspace, CancellationToken cancellationToken)
    {
        var cacheName = OperatingSystem.IsWindows() ? "nodtool.exe" : "nodtool";
        var cachePath = Path.Combine(workspace, "Launcher", "artifacts", cacheName);
        if (File.Exists(cachePath)) return cachePath;

        var url = $"https://github.com/encounter/nod/releases/download/{Version}/{AssetName()}";

        Directory.CreateDirectory(Path.GetDirectoryName(cachePath)!);
        var tempPath = cachePath + ".tmp";
        using (var http = new HttpClient())
        using (var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken))
        {
            response.EnsureSuccessStatusCode();
            await using var fileStream = File.Create(tempPath);
            await response.Content.CopyToAsync(fileStream, cancellationToken);
        }
        File.Move(tempPath, cachePath, overwrite: true);
        if (!OperatingSystem.IsWindows())
        {
            File.SetUnixFileMode(cachePath,
                UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                UnixFileMode.GroupRead | UnixFileMode.GroupExecute |
                UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
        }
        return cachePath;
    }

    private static string AssetName()
    {
        if (OperatingSystem.IsWindows())
        {
            return RuntimeInformation.OSArchitecture switch
            {
                Architecture.X64 => "nodtool-windows-x86_64.exe",
                Architecture.Arm64 => "nodtool-windows-arm64.exe",
                Architecture.X86 => "nodtool-windows-x86.exe",
                var other => throw new PlatformNotSupportedException($"No prebuilt nodtool release for Windows {other}"),
            };
        }
        if (OperatingSystem.IsMacOS())
        {
            return RuntimeInformation.OSArchitecture switch
            {
                Architecture.X64 => "nodtool-macos-x86_64",
                Architecture.Arm64 => "nodtool-macos-arm64",
                var other => throw new PlatformNotSupportedException($"No prebuilt nodtool release for macOS {other}"),
            };
        }
        return RuntimeInformation.OSArchitecture switch
        {
            Architecture.X64 => "nodtool-linux-x86_64",
            Architecture.Arm64 => "nodtool-linux-aarch64",
            Architecture.X86 => "nodtool-linux-i686",
            var other => throw new PlatformNotSupportedException($"No prebuilt nodtool release for Linux {other}"),
        };
    }
}
