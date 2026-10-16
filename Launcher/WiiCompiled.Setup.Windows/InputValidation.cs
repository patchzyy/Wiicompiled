using System.Diagnostics;
using System.Buffers.Binary;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

internal static class InputValidation
{
    private static readonly HashSet<string> SupportedDiscImageExtensions = new(
        [".iso", ".gcm", ".gcz", ".ciso", ".wbfs", ".wia", ".rvz"],
        StringComparer.OrdinalIgnoreCase);

    public static void ValidateExtension(string gamePath)
    {
        if (!File.Exists(gamePath))
            throw new FileNotFoundException("The selected game image does not exist.", gamePath);
        var extension = Path.GetExtension(gamePath);
        if (!SupportedDiscImageExtensions.Contains(extension))
            throw new InvalidDataException(
                "Select a complete Wii disc image in ISO, GCM, GCZ, CISO, WBFS, WIA, or RVZ format.");
    }

    public static async Task<DiscHeader> ReadDiscHeaderAsync(string nodTool, string gamePath,
        CancellationToken cancellationToken = default)
    {
        ValidateExtension(gamePath);
        var result = await ProcessRunner.RunAsync(nodTool,
            ["info", Path.GetFullPath(gamePath)], null, cancellationToken);
        if (result.ExitCode != 0)
            throw new InvalidDataException("nodtool could not read this disc image. " + result.CombinedOutput.Trim());

        var info = NodToolInfoParser.Parse(result.StandardOutput);
        return new DiscHeader
        {
            GameId = info.GameId,
            InternalName = info.Title,
            Region = RegionFromGameId(info.GameId),
            Revision = info.Revision,
        };
    }

    /// <summary>The disc header of a game Wheel Wizard already extracted, read from its <c>sys/boot.bin</c>.</summary>
    public static DiscHeader ReadDiscHeaderFromDirectory(string dataDirectory)
    {
        var header = ExtractedGameDirectory.ReadHeader(dataDirectory);
        return new DiscHeader
        {
            GameId = header.GameId,
            InternalName = header.InternalName,
            Region = RegionFromGameId(header.GameId),
            Revision = header.Revision,
        };
    }

    private static string RegionFromGameId(string gameId) => gameId.Length >= 4
        ? gameId[3] switch
        {
            'P' => "PAL",
            'E' => "NTSC-U",
            'J' => "NTSC-J",
            'K' => "Korea",
            'W' => "Taiwan",
            _ => gameId[3].ToString(),
        }
        : "Unknown";

    public static void EnsureCompatibleDisc(DiscHeader header, PayloadManifest manifest)
    {
        if (!header.GameId.Equals(manifest.ExpectedGameId, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"This build supports Mario Kart Wii PAL ({manifest.ExpectedGameId}). " +
                $"The selected image is {header.GameId} ({header.InternalName}, {header.Region}).");
        }
    }

    // Thin forwarding wrappers: the actual download/RSA-verification logic lives in
    // WiiCompiled.Setup.Common.RetroWfcPayload (shared with WiiCompiled.Setup.Linux) so there's one
    // copy of it, not two. Kept under these names so every existing call site here
    // (ProductRepairService.cs, LocalBuildService.cs, Installation.cs, SelfTests.cs) is unchanged.
    public const string CurrentRetroWfcPayloadUri = RetroWfcPayload.CurrentRetroWfcPayloadUri;

    public static string ValidateStagedRetroWfcPayloadDirectory(string stagedDirectory,
        RSAParameters? signingKey = null) =>
        RetroWfcPayload.ValidateStagedRetroWfcPayloadDirectory(stagedDirectory, signingKey);

    public static string ResolveRetroWfcPayloadFile(string stagedDirectory,
        RSAParameters? signingKey = null) =>
        RetroWfcPayload.ResolveRetroWfcPayloadFile(stagedDirectory, signingKey);

    public static string ComputeRetroWfcPayloadSha256(string stagedDirectory,
        RSAParameters? signingKey = null) =>
        RetroWfcPayload.ComputeRetroWfcPayloadSha256(stagedDirectory, signingKey);

    public static void ValidateRetroWfcPayloadUri(string uriText) =>
        RetroWfcPayload.ValidateRetroWfcPayloadUri(uriText);

    public static Task<RetroWfcPayloadSnapshot> DownloadRetroWfcPayloadAsync(string uriText,
        string destinationDirectory, CancellationToken cancellationToken) =>
        RetroWfcPayload.DownloadRetroWfcPayloadAsync(uriText, destinationDirectory, cancellationToken);

    internal static bool IsTransientRetroWfcDownloadFailure(Exception exception,
        CancellationToken cancellationToken) =>
        RetroWfcPayload.IsTransientRetroWfcDownloadFailure(exception, cancellationToken);

    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}

internal sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError)
{
    public string CombinedOutput => StandardOutput + Environment.NewLine + StandardError;
}

internal static class ProcessRunner
{
    /// <summary>Runs a redirected child process to completion. <paramref name="configure"/> sets up a
    /// working directory or scrubbed environment; <paramref name="capture"/> is off for callers that only
    /// forward output live, so a build's output isn't buffered in memory for nobody to read.</summary>
    public static async Task<ProcessResult> RunAsync(string executable, IReadOnlyList<string> arguments,
        Action<string>? output, CancellationToken cancellationToken,
        Action<ProcessStartInfo>? configure = null, bool capture = true,
        Action<Exception>? onTerminationFailure = null)
    {
        var info = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        configure?.Invoke(info);

        using var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        var stdout = new List<string>();
        var stderr = new List<string>();
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) { if (capture) stdout.Add(e.Data); output?.Invoke(e.Data); } };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) { if (capture) stderr.Add(e.Data); output?.Invoke(e.Data); } };
        if (!process.Start()) throw new InvalidOperationException($"Could not start {executable}.");
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await WaitForExitAsync(process, cancellationToken, onTerminationFailure);
        return new ProcessResult(process.ExitCode, string.Join(Environment.NewLine, stdout),
            string.Join(Environment.NewLine, stderr));
    }

    public static async Task WaitForExitAsync(Process process, CancellationToken cancellationToken,
        Action<Exception>? onTerminationFailure = null)
    {
        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            try
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
            }
            catch (Exception ex)
            {
                onTerminationFailure?.Invoke(ex);
            }
            await process.WaitForExitAsync(CancellationToken.None);
            process.WaitForExit();
            throw;
        }
        process.WaitForExit();
    }
}
