using System.Text;

namespace Translator.Core.CodeGen;

/// <summary>
/// The single correct implementation for embedding an arbitrary string as a C++ string literal in
/// generated source. Values reaching this (map-file symbol names, YAML config text, mod metadata)
/// are not guaranteed to be free of control characters; a bare '\' + '"' escape leaves a raw
/// newline/carriage-return/NUL able to terminate the literal early or otherwise corrupt the
/// generated .cpp before it reaches the compiler.
/// </summary>
public static class CxxStringLiteralEscaping
{
    public static string Escape(string value)
    {
        var sb = new StringBuilder(value.Length);
        foreach (var ch in value)
        {
            switch (ch)
            {
                case '\\':
                    sb.Append("\\\\");
                    break;
                case '"':
                    sb.Append("\\\"");
                    break;
                case '\n':
                    sb.Append("\\n");
                    break;
                case '\r':
                    sb.Append("\\r");
                    break;
                case '\t':
                    sb.Append("\\t");
                    break;
                default:
                    if (ch < 0x20 || ch == 0x7f)
                    {
                        // Fixed-width octal (always exactly 3 digits) rather than \xHH: a hex escape
                        // greedily consumes every following hex digit, so "\x01" + "2" would otherwise
                        // reparse as the single byte 0x12 instead of the two bytes 0x01 ' ' '2'.
                        sb.Append('\\').Append(System.Convert.ToString(ch, 8).PadLeft(3, '0'));
                    }
                    else
                    {
                        sb.Append(ch);
                    }
                    break;
            }
        }

        return sb.ToString();
    }
}
