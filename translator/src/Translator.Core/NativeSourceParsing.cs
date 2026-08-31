using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Translator.Core;

internal sealed record NativeSourceFile(string FullPath, string RelativePath, string Content);

internal static class NativeSourceParsing
{
    public static IReadOnlyList<NativeSourceFile> ReadDirectory(string directory)
    {
        var root = Path.GetFullPath(directory);
        if (!Directory.Exists(root))
            return [];

        return Directory.EnumerateFiles(root, "*.*", SearchOption.AllDirectories)
            .Where(static path => path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                                  path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                                  path.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase))
            .Order(StringComparer.OrdinalIgnoreCase)
            .Select(path => new NativeSourceFile(
                path,
                Path.GetRelativePath(root, path).Replace('\\', '/'),
                ApplyGuestAddressTable(StripCommentsAndLiterals(File.ReadAllText(path)))))
            .ToArray();
    }

    // The runtime names guest addresses by their PAL identity; the project's region table (the
    // same header the compiler resolves them through) turns them into the addresses the built
    // executable uses, so every regex below reads what the C++ will actually register.
    private static string ApplyGuestAddressTable(string source) =>
        GuestAddressTable.Current is { } table ? table.Rewrite(source) : source;

    public static IEnumerable<string> SplitArguments(string arguments)
    {
        var start = 0;
        var depth = 0;
        for (var index = 0; index < arguments.Length; index++)
        {
            var character = arguments[index];
            if (character is '(' or '[' or '<') depth++;
            else if (character is ')' or ']' or '>') depth = Math.Max(0, depth - 1);
            else if (character == ',' && depth == 0)
            {
                yield return arguments[start..index];
                start = index + 1;
            }
        }
        yield return arguments[start..];
    }

    public static bool IsFloatingPointValueArgument(string argument) =>
        !argument.Contains('*', StringComparison.Ordinal) &&
        !argument.Contains('&', StringComparison.Ordinal) &&
        Regex.IsMatch(argument, @"\b(float|double)\b", RegexOptions.CultureInvariant);

    public static string StripCommentsAndLiterals(string content)
    {
        var output = new StringBuilder(content.Length);
        var inLineComment = false;
        var inBlockComment = false;
        var inLiteral = false;
        var quote = '\0';
        for (var index = 0; index < content.Length; index++)
        {
            var character = content[index];
            var next = index + 1 < content.Length ? content[index + 1] : '\0';
            if (inLineComment)
            {
                if (character == '\n') { inLineComment = false; output.Append(character); }
                continue;
            }
            if (inBlockComment)
            {
                if (character == '*' && next == '/') { inBlockComment = false; index++; }
                continue;
            }
            if (inLiteral)
            {
                output.Append(' ');
                if (character == '\\' && index + 1 < content.Length) { output.Append(' '); index++; }
                else if (character == quote) inLiteral = false;
                continue;
            }
            if (character == '/' && next == '/') { inLineComment = true; index++; continue; }
            if (character == '/' && next == '*') { inBlockComment = true; index++; continue; }
            if (character is '\'' or '"') { inLiteral = true; quote = character; output.Append(' '); continue; }
            output.Append(character);
        }
        return output.ToString();
    }
}
