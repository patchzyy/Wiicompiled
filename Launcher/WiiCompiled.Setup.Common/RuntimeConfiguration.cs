using System.Text;
using Tomlyn;
using Tomlyn.Parsing;
using Tomlyn.Syntax;

namespace WiiCompiled.Setup.Common;

public sealed record RuntimeConfigSnapshot(bool Existed, byte[] Contents);

/// <summary>
/// Reads and writes the runtime's <c>Config.toml</c>. Every entry point takes the file it operates on
/// since its location depends on the installation (portable <c>UserData</c> vs. per-user app data);
/// callers obtain it once via <see cref="ResolveConfigPath"/>. Shared by both installers - Linux
/// never creates a <see cref="PortableRoot.MarkerFileName"/> marker file, so
/// <see cref="ResolveConfigPath"/>/<see cref="FormatPathValue"/>'s portable-root lookups always miss
/// there and this degrades to the same plain per-user-app-data, always-absolute-path behavior a
/// non-portable Windows install already gets.
/// </summary>
public static class RuntimeConfiguration
{
    public const string ConfigFileName = "Config.toml";

    /// <summary>The per-user configuration an ordinary (non-portable) installation shares.</summary>
    public static string ApplicationDataConfigPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "WiiCompiled", ConfigFileName);

    /// <summary>
    /// The configuration file that governs <paramref name="installDirectory"/>. This mirrors
    /// <c>RuntimeConfigFile::ResolveConfigPath</c> in the runtime: portable roots win, everything
    /// else is per-user application data.
    /// </summary>
    public static string ResolveConfigPath(string? installDirectory)
    {
        if (!string.IsNullOrWhiteSpace(installDirectory) &&
            PortableRoot.TryFind(installDirectory) is { } portableRoot)
            return Path.Combine(PortableRoot.UserDataDirectory(portableRoot), ConfigFileName);
        return ApplicationDataConfigPath;
    }

    public static RuntimeConfigSnapshot Capture(string configPath) =>
        File.Exists(configPath)
            ? new RuntimeConfigSnapshot(true, File.ReadAllBytes(configPath))
            : new RuntimeConfigSnapshot(false, []);

    public static void Restore(string configPath, RuntimeConfigSnapshot snapshot)
    {
        if (snapshot.Existed)
            FileSystemUtilities.WriteAtomic(configPath, snapshot.Contents);
        else
            File.Delete(configPath);
    }

    public static void SetDvdRoot(string configPath, string dvdRoot) =>
        SetPath(configPath, "dvd_root", dvdRoot);

    /// <summary>
    /// Records the one canonical Retro Rewind installation Wheel Wizard owns. The runtime builds its
    /// asset overlay by scanning this directory live at launch, so an asset-only Retro Rewind update
    /// needs no backend work: the next launch simply reads the new files.
    /// </summary>
    public static void SetRetroRewindRoot(string configPath, string retroRewindRoot) =>
        SetPath(configPath, "retro_rewind_root", retroRewindRoot);

    /// <summary>The canonical Retro Rewind root, or null when no installation has recorded one.</summary>
    public static string? GetRetroRewindRoot(string configPath) =>
        GetResolvedPath(configPath, "retro_rewind_root");

    public static void RemoveRetroRewindRootIfOwned(string configPath, string retroRewindRoot) =>
        RemovePathIfOwned(configPath, "retro_rewind_root", retroRewindRoot);

    public static void RemoveDvdRootIfOwned(string configPath, string dvdRoot) =>
        RemovePathIfOwned(configPath, "dvd_root", dvdRoot);

    /// <summary>The stored string of a <c>[paths]</c> key, exactly as the file holds it.</summary>
    public static string? GetPath(string configPath, string key) =>
        TryLoad(configPath) is { } document && Find(document, ["paths"], key)?.Item.Value is StringValueSyntax value
            ? value.Value!
            : null;

    /// <summary>
    /// A <c>[paths]</c> value as the runtime will actually use it. The runtime resolves every relative
    /// <c>[paths]</c> value against the directory holding <c>Config.toml</c> (never the working
    /// directory), so the host must resolve it the same way before comparing or reading it.
    /// </summary>
    public static string? GetResolvedPath(string configPath, string key)
    {
        var stored = GetPath(configPath, key);
        return string.IsNullOrWhiteSpace(stored) ? null : ResolveAgainstConfig(configPath, stored);
    }

    public static string ConfigDirectory(string configPath) =>
        Path.GetDirectoryName(Path.GetFullPath(configPath))
        ?? throw new InvalidOperationException($"{configPath} has no containing directory.");

    public static string ResolveAgainstConfig(string configPath, string value) =>
        Path.GetFullPath(value, ConfigDirectory(configPath));

    private static void SetPath(string configPath, string key, string value)
    {
        var document = Parse(File.Exists(configPath) ? File.ReadAllText(configPath) : DefaultConfig);
        var section = new[] { "paths" };
        var newValue = new StringValueSyntax(FormatPathValue(configPath, value));
        if (Find(document, section, key) is { } existing)
        {
            LastToken(newValue).TrailingTrivia = LastToken(existing.Item.Value!).TrailingTrivia;
            existing.Item.Value = newValue;
        }
        else
        {
            Append(document, section, new KeyValueSyntax(key, newValue));
        }
        Save(configPath, document);
    }

    /// <summary>
    /// Paths inside the portable root are stored relative to the config directory (forward slashes,
    /// needing no TOML escaping) so the tree keeps working if the user moves or renames the root.
    /// Everything else, including all non-portable paths, is stored absolute.
    /// </summary>
    private static string FormatPathValue(string configPath, string value)
    {
        var full = Path.GetFullPath(value);
        var configDirectory = ConfigDirectory(configPath);
        if (PortableRoot.TryFind(configDirectory) is not { } root || !PortableRoot.Contains(root, full))
            return full;
        var relative = Path.GetRelativePath(configDirectory, full);
        return Path.IsPathRooted(relative)
            ? full
            : relative.Replace(Path.DirectorySeparatorChar, '/');
    }

    private static void RemovePathIfOwned(string configPath, string key, string expectedValue)
    {
        if (TryLoad(configPath) is not { } document ||
            Find(document, ["paths"], key) is not { } entry ||
            entry.Item.Value is not StringValueSyntax configured)
            return;
        // A portable installation stores this relative, so ownership is decided on the path the
        // runtime would actually resolve, not on the stored text.
        if (!ResolveAgainstConfig(configPath, configured.Value!)
                .Equals(Path.GetFullPath(expectedValue), StringComparison.OrdinalIgnoreCase))
            return;
        var previous = entry.Previous?.EndOfLineToken ?? entry.Table?.EndOfLineToken;
        if (previous is not null && entry.Item.EndOfLineToken?.TrailingTrivia is { } trivia)
            (previous.TrailingTrivia ??= []).AddRange(trivia);
        entry.Items.RemoveChild(entry.Item);
        Save(configPath, document);
    }

    private const string DefaultConfig = "# WiiCompiled user configuration\n\n";

    private sealed record Entry(
        KeyValueSyntax Item, SyntaxList<KeyValueSyntax> Items, TableSyntax? Table, KeyValueSyntax? Previous);

    private static DocumentSyntax Parse(string text) => SyntaxParser.ParseStrict(text);

    private static DocumentSyntax? TryLoad(string configPath)
    {
        if (!File.Exists(configPath)) return null;
        try { return Parse(File.ReadAllText(configPath)); }
        catch (TomlException) { return null; }
    }

    /// <summary>Re-parses the edited tree before replacing the file, so an invalid edit never reaches disk.</summary>
    private static void Save(string configPath, DocumentSyntax document)
    {
        var text = document.ToString();
        Parse(text);
        FileSystemUtilities.WriteAtomic(configPath, new UTF8Encoding(false).GetBytes(text));
    }

    private static IEnumerable<TableSyntax> Tables(DocumentSyntax document) =>
        document.Tables.OfType<TableSyntax>();

    private static string[] Parts(KeySyntax key) =>
        [Part(key.Key!), .. key.DotKeys.Select(dotted => Part(dotted.Key!))];

    private static string Part(BareKeyOrStringValueSyntax part) =>
        part is StringValueSyntax quoted ? quoted.Value! : ((BareKeySyntax)part).Key!.Text!;

    /// <summary>Matches by TOML meaning, so <c>"dvd_root"</c> and <c>dvd_root</c> are the same key.</summary>
    private static Entry? Find(DocumentSyntax document, string[] section, string key)
    {
        string[] wanted = [.. section, key];
        var scopes = Tables(document)
            .Select(table => (Table: (TableSyntax?)table, Prefix: Parts(table.Name!), Items: table.Items))
            .Prepend((Table: null, Prefix: [], Items: document.KeyValues));
        foreach (var (table, prefix, items) in scopes)
        {
            KeyValueSyntax? previous = null;
            foreach (var item in items)
            {
                if (prefix.Concat(Parts(item.Key!)).SequenceEqual(wanted))
                    return new Entry(item, items, table, previous);
                previous = item;
            }
        }
        return null;
    }

    private static void Append(DocumentSyntax document, string[] section, KeyValueSyntax item)
    {
        var text = document.ToString();
        var newline = text.Contains("\r\n") ? "\r\n" : "\n";
        var table = Tables(document).FirstOrDefault(t => Parts(t.Name!).SequenceEqual(section));
        if (table is null)
        {
            var name = new KeySyntax(section[0]);
            foreach (var part in section.Skip(1)) name.DotKeys.Add(new DottedKeyItemSyntax(part));
            table = new TableSyntax(name) { EndOfLineToken = NewLine(newline) };
            var blankLine = text.EndsWith("\n\n") || text.EndsWith("\n\r\n");
            var gap = text.Length == 0 ? 0 : !text.EndsWith('\n') ? 2 : blankLine ? 0 : 1;
            for (var i = 0; i < gap; i++) (table.LeadingTrivia ??= []).Add(new SyntaxTrivia(TokenKind.NewLine, newline));
            document.Tables.Add(table);
        }

        // Blank lines and comments after the last entry stay after the new one.
        var last = table.Items.LastOrDefault();
        var before = last is null ? table.EndOfLineToken ??= NewLine(newline) : last.EndOfLineToken ??= NewLine(newline);
        item.EndOfLineToken = NewLine(newline);
        item.EndOfLineToken.TrailingTrivia = before.TrailingTrivia;
        before.TrailingTrivia = null;
        table.Items.Add(item);
    }

    private static SyntaxToken NewLine(string text) => new(TokenKind.NewLine, text);

    private static SyntaxToken LastToken(SyntaxNode node)
    {
        while (node is not SyntaxToken)
            node = node.GetChild(node.ChildrenCount - 1)!;
        return (SyntaxToken)node;
    }
}
