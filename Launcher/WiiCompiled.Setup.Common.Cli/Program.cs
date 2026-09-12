using WiiCompiled.Setup.Common;

// A packaging-time-only helper - never shipped, never run by an end user. Both
// Launcher/build-flatpak.sh and Launcher/Build-Installer.ps1 invoke this to obtain the nodtool
// binary they bundle, so there is exactly one place (NodToolProvider) that knows the pinned
// version/URL/platform-asset mapping, instead of a separate copy per packaging script.
//
// Usage: WiiCompiled.Setup.Common.Cli --workspace <repo-root>
// Prints the resolved nodtool path to stdout.

string? workspace = null;
for (var i = 0; i < args.Length; i++)
{
    if (args[i] == "--workspace" && i + 1 < args.Length)
    {
        workspace = args[++i];
    }
}

if (workspace is null)
{
    Console.Error.WriteLine("Usage: WiiCompiled.Setup.Common.Cli --workspace <repo-root>");
    return 1;
}

var path = await NodToolProvider.ResolveAsync(workspace, CancellationToken.None);
Console.WriteLine(path);
return 0;
