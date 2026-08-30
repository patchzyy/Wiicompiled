using System.Globalization;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using Translator.Core.Loading;

namespace Translator.Cli.Configuration;

internal sealed class TranslationProjectConfig
{
    private TranslationProjectConfig(
        string sourcePath,
        string workspaceRoot,
        ProjectIdentity identity,
        ProjectMemory memory,
        ProjectInputs inputs,
        ProjectTranslation translation,
        ProjectRuntime runtime,
        ProjectOutput output,
        IReadOnlyDictionary<string, ProjectProfile> profiles)
    {
        SourcePath = sourcePath;
        WorkspaceRoot = workspaceRoot;
        Identity = identity;
        Memory = memory;
        Inputs = inputs;
        Translation = translation;
        Runtime = runtime;
        Output = output;
        Profiles = profiles;
    }

    public string SourcePath { get; }
    public string WorkspaceRoot { get; }
    public ProjectIdentity Identity { get; }
    public ProjectMemory Memory { get; }
    public ProjectInputs Inputs { get; }
    public ProjectTranslation Translation { get; }
    public ProjectRuntime Runtime { get; }
    public ProjectOutput Output { get; }
    public IReadOnlyDictionary<string, ProjectProfile> Profiles { get; }

    public static TranslationProjectConfig Load(string path)
    {
        var sourcePath = Path.GetFullPath(path);
        if (!File.Exists(sourcePath))
        {
            throw new FileNotFoundException("Translation project manifest not found.", sourcePath);
        }

        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(UnderscoredNamingConvention.Instance)
            .Build();
        var dto = deserializer.Deserialize<ProjectDto>(File.ReadAllText(sourcePath))
            ?? throw new InvalidDataException($"Translation project manifest is empty: {sourcePath}");
        if (dto.SchemaVersion != 1)
        {
            throw new InvalidDataException($"Unsupported translation project schema_version {dto.SchemaVersion}; expected 1.");
        }

        var manifestDirectory = Path.GetDirectoryName(sourcePath)!;
        var workspaceRoot = ResolvePath(manifestDirectory, dto.WorkspaceRoot ?? ".");
        var identity = new ProjectIdentity(
            Require(dto.Project?.Id, "project.id"),
            dto.Project?.DisplayName ?? dto.Project?.Id!,
            dto.Project?.GameId,
            dto.Project?.Region,
            dto.Project?.BaseManifestFormat ?? "recomp-base-manifest",
            dto.Project?.BaseManifestStem ?? "base");
        var memory = new ProjectMemory(
            ParseUInt32(dto.Memory?.Base, MemoryLayout.RamBase, "memory.base"),
            ParseInt32(dto.Memory?.Size, MemoryLayout.RamSize, "memory.size"),
            ParseOptionalUInt32(dto.Memory?.SdaBase, "memory.sda_base"),
            ParseOptionalUInt32(dto.Memory?.Sda2Base, "memory.sda2_base"));

        var dolDto = dto.Inputs?.Dol ?? throw new InvalidDataException("Translation project requires inputs.dol.");
        var dol = ResolveInput(workspaceRoot, dolDto, "inputs.dol");
        ProjectRelInput? rel = null;
        if (dto.Inputs?.Rel is { Path.Length: > 0 } relDto)
        {
            var relInput = ResolveInput(workspaceRoot, relDto, "inputs.rel");
            rel = new ProjectRelInput(
                relInput.Path,
                relInput.Sha256,
                ParseUInt32(relDto.LoadAddress, null, "inputs.rel.load_address"));
        }

        var entryPoints = (dto.Translation?.EntryPoints ?? [])
            .Select((value, index) => ParseUInt32(value, null, $"translation.entry_points[{index}]"))
            .ToArray();
        string? functionMapPath = null;
        if (dto.Translation?.FunctionMap is { } functionMapDto)
        {
            functionMapPath = ResolvePath(
                workspaceRoot,
                Require(functionMapDto.Path, "translation.function_map.path"));
        }
        var translation = new ProjectTranslation(
            entryPoints,
            functionMapPath,
            dto.Translation?.AllowUnsupportedInstructions ?? false);

        var abiDirectories = (dto.Runtime?.NativeAbiDirectories ?? [])
            .Select(pathValue => ResolvePath(workspaceRoot, pathValue))
            .ToArray();
        var runtime = new ProjectRuntime(
            abiDirectories,
            ResolvePath(workspaceRoot, dto.Runtime?.NativeRegistrationRoot ?? "runtime/src"),
            dto.Runtime?.GuestAddressTable is { } guestAddressTable
                ? ResolvePath(workspaceRoot, guestAddressTable)
                : null);
        var outputRoot = ResolvePath(workspaceRoot, dto.Output?.Root ?? "generated");
        var output = new ProjectOutput(
            outputRoot,
            ResolveOutput(outputRoot, dto.Output?.Functions, "functions"),
            ResolveOutput(outputRoot, dto.Output?.RuntimeConfig, "RuntimeConfig.h"),
            ResolveOutput(outputRoot, dto.Output?.DataInitializer, "data_sections_init.cpp"),
            ResolveOutput(outputRoot, dto.Output?.BaseManifest, Path.Combine("base", "base_manifest.json")));

        var profiles = new Dictionary<string, ProjectProfile>(StringComparer.OrdinalIgnoreCase);
        foreach (var (name, profileDto) in dto.Profiles ?? new Dictionary<string, ProfileDto>())
        {
            profiles[name] = new ProjectProfile(
                name,
                profileDto.Enabled ?? true,
                ResolveOptionalPath(workspaceRoot, profileDto.CodePul),
                ResolveOptionalPath(workspaceRoot, profileDto.ModRoot),
                profileDto.Region,
                ParseOptionalUInt32(profileDto.ModuleGuestBase, $"profiles.{name}.module_guest_base"),
                ParseOptionalUInt32(profileDto.ModuleLinkBase, $"profiles.{name}.module_link_base"),
                ResolveOptionalPath(workspaceRoot, profileDto.Output),
                profileDto.EnableRetroWfc ?? false,
                profileDto.RetroWfcPayload,
                ParseOptionalUInt32(
                    profileDto.RetroWfcLegacyBootstrapHook,
                    $"profiles.{name}.retro_wfc_legacy_bootstrap_hook"),
                profileDto.RequiresGameId,
                NormalizeHash(profileDto.RequiresDolSha256),
                ResolveRiivolution(profileDto.Riivolution, name));
        }

        var config = new TranslationProjectConfig(
            sourcePath,
            workspaceRoot,
            identity,
            memory,
            new ProjectInputs(dol, rel),
            translation,
            runtime,
            output,
            profiles);
        config.ValidateInputs();
        return config;
    }

    public ProjectProfile? GetProfile(string? name)
    {
        if (string.IsNullOrWhiteSpace(name) || string.Equals(name, "base", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        return Profiles.TryGetValue(name, out var profile)
            ? profile
            : throw new InvalidDataException($"Project '{Identity.Id}' has no profile named '{name}'.");
    }

    public void ValidateProfile(ProjectProfile profile)
    {
        if (!profile.Enabled)
        {
            throw new InvalidDataException($"Profile '{profile.Name}' is disabled.");
        }
        if (!string.IsNullOrWhiteSpace(profile.RequiresGameId) &&
            !string.Equals(profile.RequiresGameId, Identity.GameId, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"Profile '{profile.Name}' requires game ID {profile.RequiresGameId}, but project is {Identity.GameId ?? "unspecified"}.");
        }
        if (!string.IsNullOrWhiteSpace(profile.RequiresDolSha256) &&
            !string.Equals(profile.RequiresDolSha256, FileSha256(Inputs.Dol.Path), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException($"Profile '{profile.Name}' is not compatible with this DOL hash.");
        }
        if (profile.EnableRetroWfc && string.IsNullOrWhiteSpace(profile.RetroWfcPayload))
        {
            throw new InvalidDataException($"Profile '{profile.Name}' enables Retro WFC but has no payload.");
        }
    }

    /// <summary>The pinned Small Data Area bases (r13/r2), or a diagnostic naming which manifest keys are missing.</summary>
    public (uint Sda1Base, uint Sda2Base) RequireSdaBases()
    {
        if (Memory.SdaBase is { } sda1 && Memory.Sda2Base is { } sda2)
        {
            return (sda1, sda2);
        }

        var missing = Memory.SdaBase is null
            ? Memory.Sda2Base is null ? "memory.sda_base and memory.sda2_base" : "memory.sda_base"
            : "memory.sda2_base";
        throw new InvalidDataException(
            $"Translation project '{Identity.Id}' is missing {missing} in {SourcePath}. " +
            "These are the r13/r2 Small Data Area bases the boot code installs (_SDA_BASE_ and _SDA2_BASE_); " +
            "read them from the lis/ori pair in __init_registers of the pinned DOL and add them as hex strings " +
            "under the manifest's memory: section.");
    }

    private void ValidateInputs()
    {
        ValidateInput(Inputs.Dol, "DOL");
        if (Inputs.Rel is not null)
        {
            ValidateInput(Inputs.Rel, "REL");
        }
        if (Translation.FunctionMapPath is { } functionMapPath && !File.Exists(functionMapPath))
        {
            throw new FileNotFoundException("Configured function map was not found.", functionMapPath);
        }
        if (Memory.Size <= 0)
        {
            throw new InvalidDataException("memory.size must be positive.");
        }
    }

    private static ProjectBinaryInput ResolveInput(string root, BinaryInputDto input, string field)
    {
        var path = ResolvePath(root, Require(input.Path, $"{field}.path"));
        return new ProjectBinaryInput(path, NormalizeHash(input.Sha256));
    }

    private static void ValidateInput(ProjectBinaryInput input, string label)
    {
        if (!File.Exists(input.Path))
        {
            throw new FileNotFoundException($"Configured {label} input was not found.", input.Path);
        }
        if (!string.IsNullOrWhiteSpace(input.Sha256))
        {
            var actual = FileSha256(input.Path);
            if (!string.Equals(actual, input.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException($"Configured {label} SHA-256 does not match {input.Path}. Expected {input.Sha256}, got {actual}.");
            }
        }
    }

    private static string FileSha256(string path) =>
        ChecksumUtilities.Sha256HexOfFile(path);

    private static string? NormalizeHash(string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : value.Trim().ToLowerInvariant();

    private static string ResolveOutput(string outputRoot, string? value, string fallback) =>
        ResolvePath(outputRoot, string.IsNullOrWhiteSpace(value) ? fallback : value);

    private static string? ResolveOptionalPath(string root, string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : ResolvePath(root, value);

    private static string ResolvePath(string root, string value)
    {
        var expanded = Environment.ExpandEnvironmentVariables(value);
        return Path.GetFullPath(Path.IsPathRooted(expanded) ? expanded : Path.Combine(root, expanded));
    }

    private static string Require(string? value, string field) =>
        !string.IsNullOrWhiteSpace(value) ? value : throw new InvalidDataException($"Translation project requires {field}.");

    // Riivolution declarations are verbatim distribution metadata, not paths on this machine:
    // the XML lives inside the pack the runtime mounts, so it stays a forward-slashed
    // pack-relative string and is never resolved against the workspace root.
    private static ProjectRiivolution? ResolveRiivolution(RiivolutionDto? dto, string profileName)
    {
        if (dto is null)
        {
            return null;
        }

        var xml = NormalizePackRelativePath(dto.Xml);
        if (xml.Length == 0)
        {
            throw new InvalidDataException($"Translation project requires profiles.{profileName}.riivolution.xml.");
        }

        var entries = dto.Options ?? [];
        var options = new List<ProjectRiivolutionOption>(entries.Count);
        for (var index = 0; index < entries.Count; index++)
        {
            var field = $"profiles.{profileName}.riivolution.options[{index}]";
            var option = entries[index].Option?.Trim() ?? string.Empty;
            if (option.Length == 0)
            {
                throw new InvalidDataException($"Translation project requires {field}.option.");
            }
            options.Add(new ProjectRiivolutionOption(
                entries[index].Section?.Trim() ?? string.Empty,
                option,
                ParseUInt32(entries[index].Choice, 0u, $"{field}.choice")));
        }

        return new ProjectRiivolution(xml, options);
    }

    private static string NormalizePackRelativePath(string? value)
    {
        var text = (value ?? string.Empty).Trim().Replace('\\', '/');
        while (true)
        {
            if (text.StartsWith("./", StringComparison.Ordinal))
            {
                text = text[2..];
                continue;
            }
            if (text.StartsWith("/", StringComparison.Ordinal))
            {
                text = text[1..];
                continue;
            }
            return text;
        }
    }

    private static uint ParseUInt32(string? value, uint? fallback, string field)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return fallback ?? throw new InvalidDataException($"Translation project requires {field}.");
        }
        var text = value.Trim();
        var style = NumberStyles.Integer;
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            text = text[2..];
            style = NumberStyles.HexNumber;
        }
        return uint.TryParse(text, style, CultureInfo.InvariantCulture, out var result)
            ? result
            : throw new InvalidDataException($"Invalid unsigned integer '{value}' for {field}.");
    }

    private static uint? ParseOptionalUInt32(string? value, string field) =>
        string.IsNullOrWhiteSpace(value) ? null : ParseUInt32(value, null, field);

    private static int ParseInt32(string? value, int fallback, string field)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return fallback;
        }
        var parsed = ParseUInt32(value, null, field);
        return parsed <= int.MaxValue
            ? (int)parsed
            : throw new InvalidDataException($"{field} exceeds Int32 range.");
    }

    private sealed class ProjectDto
    {
        public int SchemaVersion { get; init; }
        public string? WorkspaceRoot { get; init; }
        public IdentityDto? Project { get; init; }
        public MemoryDto? Memory { get; init; }
        public InputsDto? Inputs { get; init; }
        public TranslationDto? Translation { get; init; }
        public RuntimeDto? Runtime { get; init; }
        public OutputDto? Output { get; init; }
        public Dictionary<string, ProfileDto>? Profiles { get; init; }
    }

    private sealed class IdentityDto
    {
        public string? Id { get; init; }
        public string? DisplayName { get; init; }
        public string? GameId { get; init; }
        public string? Region { get; init; }
        public string? BaseManifestFormat { get; init; }
        public string? BaseManifestStem { get; init; }
    }

    private sealed class MemoryDto
    {
        public string? Base { get; init; }
        public string? Size { get; init; }
        public string? SdaBase { get; init; }
        public string? Sda2Base { get; init; }
    }

    private sealed class InputsDto
    {
        public BinaryInputDto? Dol { get; init; }
        public RelInputDto? Rel { get; init; }
    }

    private class BinaryInputDto
    {
        public string Path { get; init; } = string.Empty;
        public string? Sha256 { get; init; }
    }

    private sealed class RelInputDto : BinaryInputDto
    {
        public string? LoadAddress { get; init; }
    }

    // Translation behaviour (state-free ABI, register residency, leaf inlining, flat guest memory) is
    // unconditional; only inputs are declared here. function_map is one: every function start in the
    // image, which the recursive pass seeds from.
    private sealed class TranslationDto
    {
        public List<string>? EntryPoints { get; init; }
        public FunctionMapDto? FunctionMap { get; init; }
        public bool? AllowUnsupportedInstructions { get; init; }
    }

    private sealed class FunctionMapDto
    {
        public string? Path { get; init; }
    }

    private sealed class RuntimeDto
    {
        public List<string>? NativeAbiDirectories { get; init; }
        public string? NativeRegistrationRoot { get; init; }
        // runtime/include/region/<game>.h for a non-PAL executable; absent for PAL.
        public string? GuestAddressTable { get; init; }
    }

    private sealed class OutputDto
    {
        public string? Root { get; init; }
        public string? Functions { get; init; }
        public string? RuntimeConfig { get; init; }
        public string? DataInitializer { get; init; }
        public string? BaseManifest { get; init; }
    }

    private sealed class ProfileDto
    {
        public bool? Enabled { get; init; }
        public string? CodePul { get; init; }
        public string? ModRoot { get; init; }
        public string? Region { get; init; }
        public string? ModuleGuestBase { get; init; }
        public string? ModuleLinkBase { get; init; }
        public string? Output { get; init; }
        public bool? EnableRetroWfc { get; init; }
        public string? RetroWfcPayload { get; init; }
        public string? RetroWfcLegacyBootstrapHook { get; init; }
        public string? RequiresGameId { get; init; }
        public string? RequiresDolSha256 { get; init; }
        public RiivolutionDto? Riivolution { get; init; }
    }

    private sealed class RiivolutionDto
    {
        public string? Xml { get; init; }
        public List<RiivolutionOptionDto>? Options { get; init; }
    }

    private sealed class RiivolutionOptionDto
    {
        public string? Section { get; init; }
        public string? Option { get; init; }
        public string? Choice { get; init; }
    }
}

internal sealed record ProjectIdentity(
    string Id,
    string DisplayName,
    string? GameId,
    string? Region,
    string BaseManifestFormat,
    string BaseManifestStem);
internal sealed record ProjectMemory(uint Base, int Size, uint? SdaBase = null, uint? Sda2Base = null);
internal record ProjectBinaryInput(string Path, string? Sha256);
internal sealed record ProjectRelInput(string Path, string? Sha256, uint LoadAddress) : ProjectBinaryInput(Path, Sha256);
internal sealed record ProjectInputs(ProjectBinaryInput Dol, ProjectRelInput? Rel);
internal sealed record ProjectTranslation(
    IReadOnlyList<uint> EntryPoints,
    string? FunctionMapPath,
    bool AllowUnsupportedInstructions);
internal sealed record ProjectRuntime(
    IReadOnlyList<string> NativeAbiDirectories,
    string NativeRegistrationRoot,
    string? GuestAddressTablePath);
internal sealed record ProjectOutput(
    string Root,
    string Functions,
    string RuntimeConfig,
    string DataInitializer,
    string BaseManifest);
internal sealed record ProjectProfile(
    string Name,
    bool Enabled,
    string? CodePul,
    string? ModRoot,
    string? Region,
    uint? ModuleGuestBase,
    uint? ModuleLinkBase,
    string? Output,
    bool EnableRetroWfc,
    string? RetroWfcPayload,
    uint? RetroWfcLegacyBootstrapHook,
    string? RequiresGameId,
    string? RequiresDolSha256,
    ProjectRiivolution? Riivolution);
internal sealed record ProjectRiivolution(
    string Xml,
    IReadOnlyList<ProjectRiivolutionOption> Options);
internal sealed record ProjectRiivolutionOption(
    string Section,
    string Option,
    uint Choice);
