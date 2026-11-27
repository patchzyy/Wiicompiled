using System.Text;

namespace WiiCompiled.Setup.Linux;

/// <summary>
/// `wiicompiled-setup self-test`: checks the product-currency match logic without a game, a build
/// or a toolchain. `--sync-state-workspace DIR` additionally validates a real .mkw-sync-state that
/// build-cache.sh's mkw_sync_tree wrote into DIR. Exit code 0 only when every check passes.
/// </summary>
internal static class ProductCurrencySelfTest
{
    private const string ShaA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private const string ShaB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    private static int _failures;
    private static int _passes;

    public static int Run(string? syncStateWorkspace)
    {
        _failures = _passes = 0;
        InputsMatchCases();
        IsCurrentCases();
        SyncStateCases();
        WorkspaceIdentityCases();
        ManifestCases();
        RelativeInsideCases();
        FileSystemCases();
        if (!OperatingSystem.IsWindows()) RealPathCases();
        if (!string.IsNullOrEmpty(syncStateWorkspace)) RealSyncStateCase(syncStateWorkspace);
        Console.WriteLine($"{_passes} passed, {_failures} failed");
        return _failures == 0 ? 0 : 1;
    }

    private static void Check(string name, bool condition)
    {
        if (condition) _passes++;
        else _failures++;
        Console.WriteLine($"{(condition ? "PASS" : "FAIL")} {name}");
    }

    private static Dictionary<string, string> Inputs(params (string Key, string Value)[] pairs) =>
        pairs.ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal);

    private static void InputsMatchCases()
    {
        var current = Inputs(("a", "1"), ("b", "2"));
        Check("inputs: identical match", ProductCurrency.InputsMatch(Inputs(("b", "2"), ("a", "1")), current));
        Check("inputs: changed value", !ProductCurrency.InputsMatch(Inputs(("a", "1"), ("b", "3")), current));
        Check("inputs: extra recorded key", !ProductCurrency.InputsMatch(Inputs(("a", "1"), ("b", "2"), ("c", "3")), current));
        Check("inputs: missing recorded key", !ProductCurrency.InputsMatch(Inputs(("a", "1")), current));
        Check("inputs: renamed key", !ProductCurrency.InputsMatch(Inputs(("a", "1"), ("B", "2")), current));
        Check("inputs: null record", !ProductCurrency.InputsMatch(null, current));
        Check("inputs: null current", !ProductCurrency.InputsMatch(current, null));
        Check("inputs: both empty", !ProductCurrency.InputsMatch(Inputs(), Inputs()));
        Check("inputs: case-sensitive value", !ProductCurrency.InputsMatch(Inputs(("a", "X"), ("b", "2")), Inputs(("a", "x"), ("b", "2"))));
    }

    private static ProductProvenanceRecord Record(params ProductOutputRecord[] outputs) =>
        ProductCurrency.CreateRecord(Inputs(("a", "1")), outputs);

    private static void IsCurrentCases()
    {
        var exe = new ProductOutputRecord { Path = "/i/WiiCompiled", Size = 10, Sha256 = ShaA };
        var pem = new ProductOutputRecord { Path = "/i/cacert.pem", Size = 3, Sha256 = ShaB };
        var disk = new Dictionary<string, (long, string)>(StringComparer.Ordinal)
        {
            ["/i/WiiCompiled"] = (10, ShaA),
            ["/i/cacert.pem"] = (3, ShaB),
        };
        (long Size, string Sha256)? Observe(string path) => disk.TryGetValue(path, out var value) ? value : null;
        var inputs = Inputs(("a", "1"));
        string[] required = ["/i/WiiCompiled"];

        Check("current: everything matches", ProductCurrency.IsCurrent(Record(exe, pem), inputs, required, Observe));
        Check("current: no record", !ProductCurrency.IsCurrent(null, inputs, required, Observe));
        Check("current: no current inputs", !ProductCurrency.IsCurrent(Record(exe, pem), null, required, Observe));
        Check("current: inputs changed", !ProductCurrency.IsCurrent(Record(exe, pem), Inputs(("a", "2")), required, Observe));

        var oldSchema = Record(exe, pem); oldSchema.SchemaVersion = 0;
        Check("current: old schema", !ProductCurrency.IsCurrent(oldSchema, inputs, required, Observe));
        var otherKind = Record(exe, pem); otherKind.Kind = "something-else";
        Check("current: other kind", !ProductCurrency.IsCurrent(otherKind, inputs, required, Observe));
        var noInputs = Record(exe, pem); noInputs.Inputs = null;
        Check("current: record without inputs", !ProductCurrency.IsCurrent(noInputs, inputs, required, Observe));
        var noOutputs = Record(exe, pem); noOutputs.Outputs = null;
        Check("current: record without outputs", !ProductCurrency.IsCurrent(noOutputs, inputs, required, Observe));
        Check("current: empty outputs", !ProductCurrency.IsCurrent(Record(), inputs, Array.Empty<string>(), Observe));
        Check("current: executable not recorded", !ProductCurrency.IsCurrent(Record(pem), inputs, required, Observe));
        Check("current: duplicate output", !ProductCurrency.IsCurrent(Record(exe, exe), inputs, required, Observe));
        Check("current: relative output path", !ProductCurrency.IsCurrent(
            Record(new ProductOutputRecord { Path = "i/WiiCompiled", Size = 10, Sha256 = ShaA }), inputs, Array.Empty<string>(), Observe));
        Check("current: malformed recorded hash", !ProductCurrency.IsCurrent(
            Record(new ProductOutputRecord { Path = "/i/WiiCompiled", Size = 10, Sha256 = "AAAA" }), inputs, required, Observe));

        disk["/i/WiiCompiled"] = (10, ShaB);
        Check("current: executable content changed", !ProductCurrency.IsCurrent(Record(exe, pem), inputs, required, Observe));
        disk["/i/WiiCompiled"] = (11, ShaA);
        Check("current: executable size changed", !ProductCurrency.IsCurrent(Record(exe, pem), inputs, required, Observe));
        disk["/i/WiiCompiled"] = (10, ShaA);
        disk.Remove("/i/cacert.pem");
        Check("current: companion deleted", !ProductCurrency.IsCurrent(Record(exe, pem), inputs, required, Observe));
    }

    private static string State(long written, string dirs, params string[] entries)
    {
        var text = new StringBuilder("mkw-sync-state-v1\n").Append("written\t").Append(written).Append('\n')
            .Append("dirs\t").Append(dirs).Append('\n');
        foreach (var entry in entries) text.Append(entry).Append('\n');
        return text.Append("end\t").Append(entries.Length).Append('\n').ToString();
    }

    private static void SyncStateCases()
    {
        var valid = State(1000, "runtime\tprojects", $"{ShaA}\t5\t900\truntime/a b.cpp", $"{ShaB}\t7\t901\tprojects/x/recomp.yml");
        var parsed = ProductCurrency.ParseSyncState(valid);
        Check("state: valid state parses", parsed is { Written: 1000, Entries.Count: 2, Directories.Count: 2 } &&
                                           parsed.Entries[0].Path == "runtime/a b.cpp" && parsed.Entries[0].Size == 5 &&
                                           parsed.Entries[0].Mtime == 900);
        Check("state: empty state parses", ProductCurrency.ParseSyncState(State(5, "runtime")) is { Entries.Count: 0 });
        Check("state: missing trailing newline", ProductCurrency.ParseSyncState(valid[..^1]) is null);
        Check("state: truncated (no end line)", ProductCurrency.ParseSyncState(string.Join('\n', valid.Split('\n')[..4]) + "\n") is null);
        Check("state: count mismatch", ProductCurrency.ParseSyncState(valid.Replace("end\t2", "end\t3")) is null);
        Check("state: old header", ProductCurrency.ParseSyncState(valid.Replace("mkw-sync-state-v1", "mkw-sync-state-v0")) is null);
        Check("state: empty file", ProductCurrency.ParseSyncState("") is null);
        Check("state: bad written", ProductCurrency.ParseSyncState(valid.Replace("written\t1000", "written\t-1")) is null);
        Check("state: bad hash", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA[..63]}\t5\t900\truntime/a")) is null);
        Check("state: uppercase hash", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA.ToUpperInvariant()}\t5\t900\truntime/a")) is null);
        Check("state: bad size", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA}\t5x\t900\truntime/a")) is null);
        Check("state: path outside dirs", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA}\t5\t900\tother/a")) is null);
        Check("state: parent traversal", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA}\t5\t900\truntime/../x")) is null);
        Check("state: duplicate path", ProductCurrency.ParseSyncState(State(1000, "runtime", $"{ShaA}\t5\t900\truntime/a", $"{ShaB}\t5\t900\truntime/a")) is null);
        Check("state: overlapping dirs", ProductCurrency.ParseSyncState(State(1000, "runtime\truntime/src")) is null);
        Check("state: no dirs", ProductCurrency.ParseSyncState("mkw-sync-state-v1\nwritten\t1\ndirs\nend\t0\n") is null);
        Check("state: CRLF rejected", ProductCurrency.ParseSyncState(valid.Replace("\n", "\r\n")) is null);
    }

    private static void WorkspaceIdentityCases()
    {
        var state = ProductCurrency.ParseSyncState(State(1000, "runtime\tprojects",
            $"{ShaA}\t5\t900\truntime/a", $"{ShaB}\t7\t901\tprojects/b"))!;
        var facts = new Dictionary<string, FileFacts>(StringComparer.Ordinal)
        {
            ["runtime/a"] = new(5, 900),
            ["projects/b"] = new(7, 901),
        };
        var listing = new Dictionary<string, List<string>>(StringComparer.Ordinal)
        {
            ["runtime"] = ["runtime/a"],
            ["projects"] = ["projects/b"],
        };
        var hashes = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["runtime/a"] = ShaA,
            ["projects/b"] = ShaB,
        };
        var hashCalls = 0;
        FileFacts? Facts(string path) => facts.TryGetValue(path, out var value) ? value : null;
        string? Hash(string path) { hashCalls++; return hashes.TryGetValue(path, out var value) ? value : null; }
        IReadOnlyCollection<string>? List(string directory) => listing.TryGetValue(directory, out var value) ? value : null;

        var identity = ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List);
        Check("workspace: untouched workspace has an identity", identity is { Length: 64 });
        Check("workspace: settled files are not re-hashed", hashCalls == 0);
        var reordered = ProductCurrency.ParseSyncState(State(2000, "runtime\tprojects",
            $"{ShaB}\t7\t901\tprojects/b", $"{ShaA}\t5\t900\truntime/a"))!;
        Check("workspace: identity ignores entry order and record time",
            ProductCurrency.WorkspaceIdentity(reordered, Facts, Hash, List) == identity);
        var otherContent = ProductCurrency.ParseSyncState(State(1000, "runtime\tprojects",
            $"{ShaB}\t5\t900\truntime/a", $"{ShaB}\t7\t901\tprojects/b"))!;
        Check("workspace: identity follows content", ProductCurrency.WorkspaceIdentity(otherContent, Facts, Hash, List) != identity);

        facts["runtime/a"] = new(6, 900);
        Check("workspace: size changed", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        facts["runtime/a"] = new(5, 950);
        Check("workspace: mtime changed", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        facts.Remove("runtime/a");
        Check("workspace: file deleted", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        facts["runtime/a"] = new(5, 900);

        listing["runtime"] = ["runtime/a", "runtime/new.cpp"];
        Check("workspace: extra file", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        listing["runtime"] = [];
        Check("workspace: file missing from listing", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        listing.Remove("runtime");
        Check("workspace: unlistable directory", ProductCurrency.WorkspaceIdentity(state, Facts, Hash, List) is null);
        listing["runtime"] = ["runtime/a"];

        // runtime/a was written in the record's own second, so only its content can vouch for it.
        var racy = ProductCurrency.ParseSyncState(State(900, "runtime\tprojects",
            $"{ShaA}\t5\t900\truntime/a", $"{ShaB}\t7\t899\tprojects/b"))!;
        facts["projects/b"] = new(7, 899);
        hashCalls = 0;
        Check("workspace: same-second file with recorded content is trusted",
            ProductCurrency.WorkspaceIdentity(racy, Facts, Hash, List) is not null && hashCalls == 1);
        hashes["runtime/a"] = ShaB;
        Check("workspace: same-second file rewritten with same size and mtime",
            ProductCurrency.WorkspaceIdentity(racy, Facts, Hash, List) is null);
        hashes.Remove("runtime/a");
        Check("workspace: same-second file unreadable", ProductCurrency.WorkspaceIdentity(racy, Facts, Hash, List) is null);
    }

    private static string Manifest(params string[] entries)
    {
        var text = new StringBuilder("mkw-sync-manifest-v1\n");
        foreach (var entry in entries) text.Append(entry).Append('\n');
        return text.Append("end\t").Append(entries.Length).Append('\n').ToString();
    }

    private static void ManifestCases()
    {
        var text = Manifest($"{ShaA}\t5\tusr/bin/wiicompiled-setup", $"{ShaB}\t7\tusr/toolchain/bin/clang-18");
        var bytes = Encoding.UTF8.GetBytes(text);
        var sizes = new Dictionary<string, long>(StringComparer.Ordinal)
        {
            ["usr/bin/wiicompiled-setup"] = 5,
            ["usr/toolchain/bin/clang-18"] = 7,
        };
        long? SizeOf(string path) => sizes.TryGetValue(path, out var size) ? size : null;

        Check("manifest: parses", ProductCurrency.ParseManifest(text) is { Count: 2 });
        Check("manifest: empty manifest parses", ProductCurrency.ParseManifest(Manifest()) is { Count: 0 });
        Check("manifest: count mismatch", ProductCurrency.ParseManifest(text.Replace("end\t2", "end\t1")) is null);
        Check("manifest: duplicate path", ProductCurrency.ParseManifest(Manifest($"{ShaA}\t5\ta", $"{ShaB}\t5\ta")) is null);
        Check("manifest: wrong header", ProductCurrency.ParseManifest(text.Replace("manifest-v1", "manifest-v2")) is null);

        var identity = ProductCurrency.ToolkitIdentity(bytes, ["usr/bin/wiicompiled-setup"], SizeOf);
        Check("toolkit: identity is the manifest hash", identity == ProductCurrency.Sha256Hex(bytes));
        Check("toolkit: required tool not listed", ProductCurrency.ToolkitIdentity(bytes, ["usr/bin/other"], SizeOf) is null);
        sizes["usr/toolchain/bin/clang-18"] = 8;
        Check("toolkit: listed file changed size", ProductCurrency.ToolkitIdentity(bytes, [], SizeOf) is null);
        sizes.Remove("usr/toolchain/bin/clang-18");
        Check("toolkit: listed file missing", ProductCurrency.ToolkitIdentity(bytes, [], SizeOf) is null);
        Check("toolkit: malformed manifest", ProductCurrency.ToolkitIdentity(Encoding.UTF8.GetBytes("junk\n"), [], SizeOf) is null);
        Check("toolkit: invalid UTF-8", ProductCurrency.ToolkitIdentity([0xff, 0xfe, 0x0a], [], SizeOf) is null);
    }

    private static void RelativeInsideCases()
    {
        Check("inside: nested file", ProductCurrency.RelativeInside("/mnt/app", "/mnt/app/usr/bin/x") == "usr/bin/x");
        Check("inside: sibling with shared prefix", ProductCurrency.RelativeInside("/mnt/app", "/mnt/app2/usr/bin/x") is null);
        Check("inside: the root itself", ProductCurrency.RelativeInside("/mnt/app", "/mnt/app") is null);
        Check("inside: outside", ProductCurrency.RelativeInside("/mnt/app", "/usr/bin/clang") is null);
        Check("inside: root with trailing slash", ProductCurrency.RelativeInside("/mnt/app/", "/mnt/app/x") == "x");
    }

    private static void FileSystemCases()
    {
        var root = Path.Combine(Path.GetTempPath(), "wiicompiled-selftest-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "runtime", "sub"));
            File.WriteAllText(Path.Combine(root, "runtime", "a.cpp"), "alpha");
            File.WriteAllText(Path.Combine(root, "runtime", "sub", "b.h"), "beta!!");
            var old = DateTime.UtcNow.AddMinutes(-5);
            File.SetLastWriteTimeUtc(Path.Combine(root, "runtime", "a.cpp"), old);
            File.SetLastWriteTimeUtc(Path.Combine(root, "runtime", "sub", "b.h"), old);
            var mtime = new DateTimeOffset(old).ToUnixTimeSeconds();
            var written = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            var stateText = State(written, "runtime",
                $"{ShaA}\t5\t{mtime}\truntime/a.cpp", $"{ShaB}\t6\t{mtime}\truntime/sub/b.h");
            var state = ProductCurrency.ParseSyncState(stateText)!;
            string? IdentityOf(SyncState syncState) => ProductCurrency.WorkspaceIdentity(syncState,
                relative => ProductCurrencyProbe.Facts(Path.Combine(root, relative)),
                relative => ProductCurrencyProbe.Observe(Path.Combine(root, relative))?.Sha256,
                directory => ProductCurrencyProbe.ListFiles(root, directory));
            string? Identity() => IdentityOf(state);

            // Recorded in the files' own second: trusted only because their bytes still hash right.
            var alpha = ProductCurrency.Sha256Hex(Encoding.UTF8.GetBytes("alpha"));
            var beta = ProductCurrency.Sha256Hex(Encoding.UTF8.GetBytes("beta!!"));
            var sameSecond = ProductCurrency.ParseSyncState(State(mtime, "runtime",
                $"{alpha}\t5\t{mtime}\truntime/a.cpp", $"{beta}\t6\t{mtime}\truntime/sub/b.h"))!;
            Check("fs: same-second files with recorded content", IdentityOf(sameSecond) is not null);
            var sameSecondWrong = ProductCurrency.ParseSyncState(State(mtime, "runtime",
                $"{ShaA}\t5\t{mtime}\truntime/a.cpp", $"{beta}\t6\t{mtime}\truntime/sub/b.h"))!;
            Check("fs: same-second file with other content", IdentityOf(sameSecondWrong) is null);

            Check("fs: listing finds nested files",
                ProductCurrencyProbe.ListFiles(root, "runtime") is { Count: 2 } list && list.Contains("runtime/sub/b.h"));
            Check("fs: missing directory lists empty", ProductCurrencyProbe.ListFiles(root, "absent") is { Count: 0 });
            Check("fs: untouched workspace has an identity", Identity() is not null);

            File.AppendAllText(Path.Combine(root, "runtime", "a.cpp"), "!");
            Check("fs: edited file invalidates", Identity() is null);
            File.WriteAllText(Path.Combine(root, "runtime", "a.cpp"), "alpha");
            File.SetLastWriteTimeUtc(Path.Combine(root, "runtime", "a.cpp"), old);
            Check("fs: restored file is trusted again", Identity() is not null);

            File.WriteAllText(Path.Combine(root, "runtime", "sub", "new.cpp"), "x");
            Check("fs: added file invalidates", Identity() is null);
            File.Delete(Path.Combine(root, "runtime", "sub", "new.cpp"));

            File.WriteAllText(Path.Combine(root, "runtime", "sub", "b.h"), "BETA!!");
            Check("fs: same-size edit with a new mtime invalidates", Identity() is null);

            var observed = ProductCurrencyProbe.Observe(Path.Combine(root, "runtime", "a.cpp"));
            Check("fs: observe hashes content", observed is { Size: 5 } o &&
                                                o.Sha256 == ProductCurrency.Sha256Hex(Encoding.UTF8.GetBytes("alpha")));
            Check("fs: observe missing file", ProductCurrencyProbe.Observe(Path.Combine(root, "nope")) is null);

            var before = ProductCurrencyProbe.DirectoryDigest(Path.Combine(root, "runtime"));
            File.AppendAllText(Path.Combine(root, "runtime", "sub", "b.h"), "x");
            Check("fs: directory digest follows content",
                before is not null && before != ProductCurrencyProbe.DirectoryDigest(Path.Combine(root, "runtime")));
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); } catch (IOException) { }
        }
    }

    private static void RealPathCases()
    {
        var root = Path.Combine(Path.GetTempPath(), "wiicompiled-selftest-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "real", "bin"));
            File.WriteAllText(Path.Combine(root, "real", "bin", "clang-18"), "x");
            File.CreateSymbolicLink(Path.Combine(root, "real", "bin", "clang"), "clang-18");
            Directory.CreateSymbolicLink(Path.Combine(root, "link"), Path.Combine(root, "real"));
            var realRoot = ProductCurrencyProbe.RealPath(root);
            Check("realpath: resolves directory and file links",
                realRoot is not null &&
                ProductCurrencyProbe.RealPath(Path.Combine(root, "link", "bin", "clang")) == realRoot + "/real/bin/clang-18");
            Check("realpath: resolves parent after a link",
                ProductCurrencyProbe.RealPath(Path.Combine(root, "link", "bin", "..", "bin", "clang-18")) == realRoot + "/real/bin/clang-18");
            Check("realpath: missing path", ProductCurrencyProbe.RealPath(Path.Combine(root, "link", "nope")) is null);
            Check("fs: listing refuses links", ProductCurrencyProbe.ListFiles(root, "real") is null);
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); } catch (IOException) { }
        }
    }

    private static void RealSyncStateCase(string workspace)
    {
        var path = Path.Combine(workspace, ProductCurrencyProbe.SyncStateName);
        var state = File.Exists(path) ? ProductCurrency.ParseSyncState(File.ReadAllText(path)) : null;
        Check($"real state: {path} parses", state is not null);
        if (state is null) return;
        var identity = ProductCurrency.WorkspaceIdentity(state,
            relative => ProductCurrencyProbe.Facts(Path.Combine(workspace, relative)),
            relative => ProductCurrencyProbe.Observe(Path.Combine(workspace, relative))?.Sha256,
            directory => ProductCurrencyProbe.ListFiles(workspace, directory));
        Check($"real state: workspace matches its sync state ({state.Entries.Count} files)", identity is not null);
        if (identity is not null) Console.WriteLine($"workspace identity {identity}");
    }
}
