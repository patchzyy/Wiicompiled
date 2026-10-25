using System.Text.RegularExpressions;
using Translator.Core.Analysis;

namespace Translator.Core;

/// <summary>
/// The one definition of every marker the translator writes into generated C++ and every macro
/// spelling it reads back out of hand-written runtime C++. Emitter and parsers share these members
/// so copies can't drift the way hand-duplicated literals once did, silently dropping a registration.
/// </summary>
public static partial class GeneratedMarkers
{
    // ---------------------------------------------------------------------
    // Markers emitted into generated C++ (see CxxLinearCodeGenerator).
    // ---------------------------------------------------------------------

    /// <summary>The guest ABI contract line, without its comment prefix.</summary>
    public static string GuestAbi(GuestAbiContract contract) =>
        $"RECOMP_GUEST_ABI gpr_read=0x{contract.GprReadBeforeWriteMask:X8} " +
        $"gpr_write=0x{contract.GprPossibleWriteMask:X8} " +
        $"gpr_return=0x{contract.GprReturnMask:X8} " +
        $"fpr_read=0x{contract.FprReadBeforeWriteMask:X8} " +
        $"fpr_write=0x{contract.FprPossibleWriteMask:X8} " +
        $"fpr_return=0x{contract.FprReturnMask:X8} " +
        $"cr_read=0x{contract.CrReadBeforeWriteMask:X2} " +
        $"cr_write=0x{contract.CrPossibleWriteMask:X2} " +
        $"xer_read={(contract.ReadsXerBeforeWrite ? 1 : 0)} " +
        $"xer_write={(contract.MayWriteXer ? 1 : 0)} " +
        $"fence={(contract.HasFullSynchronizationFence ? 1 : 0)}";

    /// <summary>The registration facts of a base-translation function.</summary>
    public static string BaseRegistration(
        uint entryPoint,
        string symbol,
        bool preservesNonvolatileFprs,
        uint nonvolatileFprWriteMask) =>
        $"// RECOMP_REGISTRATION base 0x{entryPoint:X8} {symbol} " +
        $"preserves={Bool(preservesNonvolatileFprs)} fpr_mask=0x{nonvolatileFprWriteMask:X8}";

    /// <summary>
    /// The registration facts of a mod overlay function. <paramref name="prettyName"/>
    /// is already escaped for a C++ string literal by the caller.
    /// </summary>
    public static string ModRegistration(
        uint entryPoint,
        string symbol,
        string prettyName,
        bool preservesNonvolatileFprs,
        uint nonvolatileFprWriteMask,
        uint priority,
        ulong moduleId) =>
        $"// RECOMP_REGISTRATION mod 0x{entryPoint:X8} {symbol} \"{prettyName}\" " +
        $"preserves={Bool(preservesNonvolatileFprs)} fpr_mask=0x{nonvolatileFprWriteMask:X8} " +
        $"priority={priority} module_id={moduleId}";

    private static string Bool(bool value) => value ? "true" : "false";

    [GeneratedRegex(@"RECOMP_GUEST_ABI gpr_read=0x(?<gr>[0-9A-Fa-f]{8}) gpr_write=0x(?<gw>[0-9A-Fa-f]{8}) gpr_return=0x(?<gret>[0-9A-Fa-f]{8}) fpr_read=0x(?<fr>[0-9A-Fa-f]{8}) fpr_write=0x(?<fw>[0-9A-Fa-f]{8}) fpr_return=0x(?<fret>[0-9A-Fa-f]{8}) cr_read=0x(?<crr>[0-9A-Fa-f]{2}) cr_write=0x(?<crw>[0-9A-Fa-f]{2}) xer_read=(?<xr>[01]) xer_write=(?<xw>[01]) fence=(?<fence>[01])", RegexOptions.CultureInvariant)]
    public static partial Regex GuestAbiPattern();

    [GeneratedRegex(@"// RECOMP_REGISTRATION base 0x(?<address>[0-9A-Fa-f]{8}) (?<symbol>[A-Za-z_][A-Za-z0-9_]*) preserves=(?<preserves>true|false) fpr_mask=0x(?<mask>[0-9A-Fa-f]+)", RegexOptions.CultureInvariant)]
    public static partial Regex BaseRegistrationPattern();

    /// <summary>
    /// Every group the emitter writes, module id included. A copy of this pattern
    /// used to stop at the priority field, so the resolved dispatch profile and
    /// the shard emitter disagreed about what a well-formed marker even was.
    /// </summary>
    [GeneratedRegex("""// RECOMP_REGISTRATION mod 0x(?<address>[0-9A-Fa-f]{8}) (?<symbol>[A-Za-z_][A-Za-z0-9_]*) "(?<name>[^"]*)" preserves=(?<preserves>true|false) fpr_mask=0x(?<mask>[0-9A-Fa-f]+) priority=(?<priority>[0-9]+) module_id=(?<moduleId>[0-9]+)""", RegexOptions.CultureInvariant)]
    public static partial Regex ModRegistrationPattern();

    /// <summary>
    /// Matches a whole registration marker line. The marker is translator-internal
    /// metadata: a compiled shard has no use for it, so it is dropped rather than
    /// carried into tens of thousands of lines of shard text.
    /// </summary>
    [GeneratedRegex(@"^// RECOMP_REGISTRATION [^\r\n]*\r?\n?", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    public static partial Regex RegistrationLinePattern();

    // ---------------------------------------------------------------------
    // Registration macros in hand-written runtime C++.
    // ---------------------------------------------------------------------

    /// <summary>
    /// <c>REGISTER_NATIVE_FUNCTION(0x…, symbol)</c> and its <c>_AS</c> alias form;
    /// the <c>as</c> group tells the two apart. <c>REGISTER_NATIVE_WRAPPER_AS</c> and
    /// <c>REGISTER_NATIVE_REPLACEMENT_AS</c> are alias forms too, named by the
    /// <c>modOverlay</c> group.
    /// </summary>
    [GeneratedRegex(@"REGISTER_NATIVE_(?:FUNCTION(?<as>_AS)?|(?<modOverlay>WRAPPER|REPLACEMENT)_AS)\s*\(\s*0x(?<address>[0-9A-Fa-f]{8})\s*,\s*(?<symbol>[A-Za-z_][A-Za-z0-9_]*)", RegexOptions.CultureInvariant)]
    public static partial Regex NativeFunctionRegistrationPattern();

    [GeneratedRegex(@"REGISTER_TRANSLATED_FUNCTION\s*\(\s*0x(?<address>[0-9A-Fa-f]{8})\s*,\s*(?<symbol>[A-Za-z_][A-Za-z0-9_]*)", RegexOptions.CultureInvariant)]
    public static partial Regex TranslatedFunctionRegistrationPattern();

    /// <summary>
    /// The registration half of <c>PPC_NATIVE_OVERRIDE</c>: address and symbol
    /// only. Deliberately a prefix match, so an override whose argument list
    /// contains a semicolon still registers.
    /// </summary>
    [GeneratedRegex(@"PPC_NATIVE_OVERRIDE(?:_VOID)?\s*\(\s*(?<address>[0-9A-Fa-f]{8})\s*,\s*(?<symbol>[A-Za-z_][A-Za-z0-9_]*)", RegexOptions.CultureInvariant)]
    public static partial Regex NativeOverridePattern();

    /// <summary>
    /// The same macro read for its signature rather than its registration: the
    /// effect analyzer needs the argument list, which only the full
    /// statement-terminated form can capture.
    /// </summary>
    [GeneratedRegex(@"PPC_NATIVE_OVERRIDE(?<void>_VOID)?\s*\(\s*(?<address>[0-9A-Fa-f]{8})\s*,\s*(?<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,?\s*(?<tail>[^;]*?)\);", RegexOptions.CultureInvariant | RegexOptions.Singleline)]
    public static partial Regex NativeOverrideSignaturePattern();

    /// <summary>
    /// The void-stub form read for its declared host parameter list, which is
    /// what the void-stub ABI provider infers argument registers from.
    /// </summary>
    [GeneratedRegex(@"PPC_NATIVE_OVERRIDE_VOID\s*\(\s*(?<addr>[0-9A-Fa-f]+)\s*,\s*[^,]+,\s*\((?<args>[^)]*)\)\s*,", RegexOptions.CultureInvariant | RegexOptions.Singleline)]
    public static partial Regex NativeOverrideVoidArgumentsPattern();

    [GeneratedRegex(@"GX_FATAL_STUB\s*\(\s*(?<address>[0-9A-Fa-f]{8})", RegexOptions.CultureInvariant)]
    public static partial Regex FatalStubPattern();
}
