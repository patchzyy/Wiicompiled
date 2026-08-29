namespace WiiCompiled.Setup;

using System.Runtime.InteropServices;

internal static class Program
{
    private static int Main(string[] args)
    {
        var progressJson = CommandLine.WantsProgressJson(args);

        try
        {
            PlatformChecks.EnsureSupportedHost();

            if (args.Length == 0 && GetConsoleProcessList(new uint[1], 1) == 1)
            {
                Console.Out.WriteLine(
                    "Mario Kart WiiCompiled is installed through Wheel Wizard - download it from " +
                    "https://github.com/TeamWheelWizard/WheelWizard");
                Console.ReadKey(intercept: true);
                return 0;
            }

            CommandLine command;
            try
            {
                command = CommandLine.Parse(args);
            }
            catch (ArgumentException ex) when (args.Length > 0)
            {
                // A command line was supplied, so a caller is driving this process. Rejecting the
                // command with a modal dialog would hang that caller forever.
                if (progressJson) new NdjsonInstallReporter().Failure(ex.Message);
                else Console.Error.WriteLine(ex.Message);
                return 1;
            }

            if (command.Mode == AppMode.Version)
                return ConsoleCommands.Version();

            if (command.Mode == AppMode.Help)
                return ConsoleCommands.Help();

            if (command.Mode == AppMode.SelfTest)
                return SelfTests.Run();

            if (command.Mode == AppMode.VerifyInputs)
                return ConsoleCommands.VerifyInputs(command);

            if (command.Mode == AppMode.EmitPayloadIdentities)
                return ConsoleCommands.EmitPayloadIdentities(command);

            if (command.Mode == AppMode.CheckProducts)
            {
                using var cancellationSignal = CancellationSignal.ObserveEnvironment();
                return ConsoleCommands.CheckProducts(command, cancellationSignal.Token);
            }

            if (command.Mode is AppMode.LaunchBase or AppMode.LaunchRetro)
                return GameLaunchService.LaunchAsync(
                    command.Mode == AppMode.LaunchBase ? BuildProfile.Base : BuildProfile.RetroRewind)
                    .GetAwaiter().GetResult();

            if (command.Mode is AppMode.SilentInstall or AppMode.RepairProducts)
            {
                using var cancellationSignal = CancellationSignal.ObserveEnvironment();
                return command.Mode == AppMode.SilentInstall
                    ? ConsoleCommands.Install(command, cancellationSignal.Token).GetAwaiter().GetResult()
                    : ConsoleCommands.RepairProducts(command, cancellationSignal.Token)
                        .GetAwaiter().GetResult();
            }

            if (command.Mode is AppMode.Uninstall or AppMode.SilentUninstall)
            {
                UninstallService.StartWorker(command.InstallDirectory!,
                    quiet: command.Mode == AppMode.SilentUninstall || command.Quiet);
                return 0;
            }

            if (command.Mode == AppMode.UninstallWorker)
                return UninstallService.RunWorker(command.InstallDirectory!, command.Quiet);

            return ConsoleCommands.Help();
        }
        catch (Exception ex)
        {
            // A caller reading the NDJSON protocol must always receive a terminal result line, even
            // when the failure happened before the installer was reached (a bad command line, for
            // instance). ConsoleCommands already reports its own failures, so this only fires for
            // errors raised outside it.
            if (progressJson)
            {
                new NdjsonInstallReporter().Failure(ex.Message);
                Console.Error.WriteLine(ex);
            }
            else
            {
                Console.Error.WriteLine(ex);
            }
            return 1;
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleProcessList(
        [Out] uint[] processList,
        uint processCount);
}

internal static class PlatformChecks
{
    public static void EnsureSupportedHost()
    {
        if (!OperatingSystem.IsWindows() || !Environment.Is64BitOperatingSystem)
            throw new PlatformNotSupportedException("WiiCompiled requires 64-bit Windows.");
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 18362))
            throw new PlatformNotSupportedException("WiiCompiled requires Windows 10 version 1903 or newer.");
    }
}

internal static class ProductInfo
{
    public const string Name = "WiiCompiled";
    public const string Version = "0.2.24";

    /// <summary>
    /// The setup executable is copied into the installation under this name. It is the launcher and
    /// launch entry point named by the Wheel Wizard contract, so the name is part of that interface.
    /// </summary>
    public const string SetupCopyName = "WiiCompiled-Setup.exe";

    public const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\WiiCompiled";
    public static string DefaultInstallDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Programs", "WiiCompiled");
}
