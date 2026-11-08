using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

// A callee can resume these bodies at any instruction label, so nothing a pass
// learned from the instructions above a label may be trusted below it.
public class ResumableBodyCodeGenTests
{
    private const uint Entry = 0x80001000u;
    private const uint Hook = 0x8179AC3Cu;

    // The hook call comes first, so every later guest instruction is a resume target.
    private static IrFunction Body(string name, params IrInstruction[][] guestInstructions)
    {
        var instructions = new List<IrInstruction>
        {
            new IrAssign("lr", IrValue.Imm(unchecked((int)(Entry + 4)))),
            new IrCall(string.Empty, $"0x{Hook:X8}", Array.Empty<IrValue>()),
            new IrTracePpc(Entry, "bl", "0x48000001")
        };
        for (var i = 0; i < guestInstructions.Length; i++)
        {
            instructions.AddRange(guestInstructions[i]);
            instructions.Add(new IrTracePpc(Entry + 4 + (uint)i * 4, "op", "0x60000000"));
        }
        instructions.Add(new IrReturn(null));
        return new IrFunction(name, $"0x{Entry:X8}",
            new[] { new IrBasicBlock($"0x{Entry:X8}", instructions) });
    }

    private static string Emit(
        IrFunction function,
        bool resumable,
        Dictionary<string, ValueRepresentation>? registers = null,
        uint? moduleLinkBase = null,
        uint? moduleGuestBase = null,
        uint moduleLinkedCodeSize = 0,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        IReadOnlyDictionary<uint, GuestAbiContract>? stateFreeAbiContracts = null,
        IReadOnlyDictionary<uint, string>? stateFreeCallSymbols = null) =>
        new CxxLinearCodeGenerator().Emit(
            Entry,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(registers ?? new Dictionary<string, ValueRepresentation>()),
            lrContinuationCallTargets: resumable ? new HashSet<uint> { Hook } : new HashSet<uint>(),
            moduleLinkBase: moduleLinkBase,
            moduleGuestBase: moduleGuestBase,
            moduleLinkedCodeSize: moduleLinkedCodeSize,
            guestAbiContracts: guestAbiContracts,
            stateFreeAbiContracts: stateFreeAbiContracts,
            stateFreeCallSymbols: stateFreeCallSymbols);

    // An empty callee: reads and writes nothing.
    private static GuestAbiContract EmptyContract() => GuestAbiContractAnalyzer.Analyze(
        new IrFunction("empty", "entry", new[] { new IrBasicBlock("entry", new IrInstruction[] { new IrReturn(null) }) }));

    private static string ResumedAt(string code, uint address)
    {
        var label = code.IndexOf($"loc_{address:X8}: ;", StringComparison.Ordinal);
        Assert.True(label >= 0, code);
        return code[label..];
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void FctiwLowWordIsReloadedFromTheStackSlot(bool resumable)
    {
        // A resume at the load must read the slot, not a word taken from f0 before the hook ran.
        var code = Emit(Body("fctiw_resumable",
                new IrInstruction[] { new IrBinary("f0", IrValue.Register("f1"), IrValue.Imm(0), "fctiwz") },
                new IrInstruction[] { new IrStore(new IrAddress("r1", -16), IrValue.Register("f0"), 8) },
                new IrInstruction[] { new IrLoad("r3", new IrAddress("r1", -12), 4) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r1"] = ValueRepresentation.UInt32,
                ["r3"] = ValueRepresentation.UInt32,
                ["f0"] = ValueRepresentation.Float64,
                ["f1"] = ValueRepresentation.Float64
            });

        if (!resumable)
        {
            Assert.Contains("fctiwzword", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("fctiwzword", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void GqrIsReadAtTheUse(bool resumable)
    {
        // A resume past the GQR write must not use the value that write would have set.
        var code = Emit(Body("gqr_specialize_resumable",
                new IrInstruction[] { new IrAssign("r3", IrValue.Imm(0x00040004)) },
                new IrInstruction[] { new IrAssign("gqr2", IrValue.Register("r3")) },
                new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(2) })
                }),
            resumable);

        if (!resumable)
        {
            Assert.Contains("Known", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("Known", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void GqrIsNotHoistedIntoALocal(bool resumable)
    {
        var psqLoad = new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0) });
        var code = Emit(Body("gqr_hoist_resumable",
                new IrInstruction[] { psqLoad },
                new IrInstruction[] { psqLoad }),
            resumable);

        if (!resumable)
        {
            Assert.Contains("mkw_gqr0", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("mkw_gqr0", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ConstantRamStoreKeepsTheRuntimeMmioCheck(bool resumable)
    {
        var code = Emit(Body("ram_store_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Imm(unchecked((int)0x80400000u))) },
                new IrInstruction[] { new IrStore(new IrAddress("r4", 0), IrValue.Register("r5"), 4) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["r5"] = ValueRepresentation.UInt32
            });

        if (!resumable)
        {
            Assert.Contains("MemoryInline::FlatWriteRam32(", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("MemoryInline::FlatWriteRam32(", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWrite32(", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void StackStoreThroughACopyOfR1KeepsTheRuntimeMmioCheck(bool resumable)
    {
        // A resume at the store skips the copy, so r4 is not known to point at the stack.
        var code = Emit(Body("stack_copy_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Register("r1")) },
                new IrInstruction[] { new IrStore(new IrAddress("r4", 0), IrValue.Register("r5"), 4) },
                new IrInstruction[] { new IrStore(new IrAddress("r1", 8), IrValue.Register("r5"), 4) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r1"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["r5"] = ValueRepresentation.UInt32
            });

        if (!resumable)
        {
            Assert.Equal(2, CountOccurrences(code, "MemoryInline::FlatWriteRam32("));
            return;
        }

        // r1 itself is still the stack pointer after a resume.
        Assert.Equal(1, CountOccurrences(code, "MemoryInline::FlatWriteRam32("));
        Assert.Contains("MemoryInline::FlatWrite32(r4,", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void PsqAccessThroughACopyOfR1UsesTheCheckedHelpers(bool resumable)
    {
        // The stack helpers skip the executable-write and deferred-read handling,
        // which a resume that skipped the copy still needs.
        var code = Emit(Body("psq_stack_copy_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Register("r1")) },
                new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0) })
                },
                new IrInstruction[]
                {
                    new IrCall(string.Empty, "PPC_PsqSt",
                        new[] { IrValue.Register("r4"), IrValue.Register("f1"), IrValue.Imm(0), IrValue.Imm(0) })
                }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r1"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["f1"] = ValueRepresentation.Float64
            });

        if (!resumable)
        {
            Assert.Contains("PPC_PsqLStackInline<", code, StringComparison.Ordinal);
            Assert.Contains("PPC_PsqStStackInline<", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("StackInline<", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqLInline<", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqStInline<", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void OverwrittenPsqStackLoadThroughACopyOfR1IsKept(bool resumable)
    {
        // A resume at the psq_l skips the copy, so its read is a real guest access.
        var code = Emit(Body("psq_overwritten_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Register("r1")) },
                new IrInstruction[]
                {
                    new IrBinary("psq_addr", IrValue.Register("r4"), IrValue.Imm(8), "add"),
                    new IrCall("f14", "PPC_PsqL", new[] { IrValue.Register("psq_addr"), IrValue.Imm(0), IrValue.Imm(0) })
                },
                new IrInstruction[] { new IrLoad("f14", new IrAddress("r1", 0), 8) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r1"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["psq_addr"] = ValueRepresentation.UInt32,
                ["f14"] = ValueRepresentation.Float64
            });

        if (!resumable)
        {
            Assert.DoesNotContain("PPC_PsqL", code, StringComparison.Ordinal);
            return;
        }

        Assert.Contains("PPC_PsqLInline<", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void PublishedContractIsAFullFence(bool resumable)
    {
        // Resuming at the copy skips the write, so incoming r4 is read.
        var code = Emit(Body("contract_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Imm(1)) },
                new IrInstruction[] { new IrAssign("r3", IrValue.Register("r4")) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r3"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32
            },
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [Hook] = EmptyContract() });

        if (!resumable)
        {
            Assert.Contains("fence=0", code, StringComparison.Ordinal);
            Assert.DoesNotContain("gpr_read=0xFFFFFFFF", code, StringComparison.Ordinal);
            return;
        }

        Assert.Contains("gpr_read=0xFFFFFFFF", code, StringComparison.Ordinal);
        Assert.Contains("fence=1", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void CallsKeepTheFullContextBoundary(bool resumable)
    {
        // A state-free signature only returns what CFG liveness wants, and a resume can read more.
        const uint callee = 0x80002000u;
        const string symbol = "callee_state_free";
        var code = Emit(Body("state_free_resumable",
                new IrInstruction[] { new IrCall(string.Empty, $"0x{callee:X8}", Array.Empty<IrValue>()) }),
            resumable,
            new Dictionary<string, ValueRepresentation> { ["lr"] = ValueRepresentation.UInt32 },
            guestAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [Hook] = EmptyContract(),
                [callee] = EmptyContract()
            },
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [callee] = EmptyContract() },
            stateFreeCallSymbols: new Dictionary<uint, string> { [callee] = symbol });

        // The extern declaration is emitted either way.
        if (!resumable)
        {
            Assert.True(CountOccurrences(code, symbol + "(") > 1, code);
            return;
        }

        Assert.Equal(1, CountOccurrences(code, symbol + "("));
        Assert.Contains($"InvokeDirectCpu<0x{callee:X8}u>(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void InlineFprRestoreThunkRetagsTheRestoredRegisters()
    {
        // _rest_fpr_23 reloads f23 as a scalar, so the paired tag from the psq_l is gone.
        var code = Emit(Body("thunk_restore_resumable",
                new IrInstruction[]
                {
                    new IrCall("f23", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0) })
                },
                new IrInstruction[] { new IrCall(string.Empty, "0x80021544", Array.Empty<IrValue>()) },
                new IrInstruction[] { new IrStore(new IrAddress("r5", 0), IrValue.Register("f23"), 4) }),
            resumable: true,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["r5"] = ValueRepresentation.UInt32,
                ["r11"] = ValueRepresentation.UInt32,
                ["f23"] = ValueRepresentation.Float64
            });

        Assert.DoesNotContain("InvokeDirectCpu<0x80021544u>", code, StringComparison.Ordinal);
        // The store label expects scalar f23, so neither the fallthrough nor a resume converts it.
        Assert.DoesNotContain("PPC_PsToScalarInline(f23", code, StringComparison.Ordinal);
        var caseStart = code.IndexOf($"case 0x{Entry + 12:X8}u:", StringComparison.Ordinal);
        Assert.True(caseStart >= 0, code);
        var caseBody = code[caseStart..code.IndexOf("goto", caseStart, StringComparison.Ordinal)];
        Assert.DoesNotContain("f23", caseBody, StringComparison.Ordinal);
    }

    [Fact]
    public void InlineSaveThunkKeepsALivePairedValueIntact()
    {
        // The thunk is expanded in place and touches no FPR, so nothing may
        // convert f1 between the psq_l and the ps_add on the fallthrough.
        var code = Emit(Body("thunk_paired_resumable",
                new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0) })
                },
                new IrInstruction[] { new IrCall(string.Empty, "0x800215A0", Array.Empty<IrValue>()) },
                new IrInstruction[]
                {
                    new IrCall("f2", "PPC_PsAdd", new[] { IrValue.Register("f1"), IrValue.Register("f1") })
                }),
            resumable: true,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["r11"] = ValueRepresentation.UInt32,
                ["f1"] = ValueRepresentation.Float64,
                ["f2"] = ValueRepresentation.Float64
            });

        Assert.DoesNotContain("InvokeDirectCpu<0x800215A0u>", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsToScalarInline(f1", code, StringComparison.Ordinal);
    }

    private static int CountOccurrences(string text, string value)
    {
        var count = 0;
        for (var index = text.IndexOf(value, StringComparison.Ordinal);
             index >= 0;
             index = text.IndexOf(value, index + value.Length, StringComparison.Ordinal))
        {
            count++;
        }
        return count;
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void LinkedAddressRemapChecksTheRuntimeAddress(bool resumable)
    {
        const uint linkBase = 0x803992E0u;
        const uint guestBase = 0x81800000u;
        var code = Emit(Body("remap_resumable",
                new IrInstruction[] { new IrAssign("r4", IrValue.Imm(unchecked((int)(linkBase + 0x10)))) },
                new IrInstruction[] { new IrLoad("r5", new IrAddress("r4", 8), 4) }),
            resumable,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["r5"] = ValueRepresentation.UInt32
            },
            moduleLinkBase: linkBase,
            moduleGuestBase: guestBase,
            moduleLinkedCodeSize: 0x1000);

        if (!resumable)
        {
            Assert.Contains($"FlatRead32(0x{guestBase + 0x18:X8}u)", code, StringComparison.Ordinal);
            return;
        }

        // The fallthrough still takes the remapped literal; a resume that skipped
        // the constant keeps its own address.
        Assert.Contains(
            $"== 0x{linkBase + 0x18:X8}u ? 0x{guestBase + 0x18:X8}u : ",
            ResumedAt(code, Entry + 8),
            StringComparison.Ordinal);
    }

    [Fact]
    public void ResumeDispatchConvertsToTheTargetLabelsPairedState()
    {
        // f1 is paired from the psq_l on, but scalar at the hook call. A resume
        // to the ps_add label has to enter in the paired form that label expects.
        var code = Emit(Body("paired_resumable",
                new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0) })
                },
                new IrInstruction[]
                {
                    new IrCall("f2", "PPC_PsAdd", new[] { IrValue.Register("f1"), IrValue.Register("f1") })
                }),
            resumable: true,
            new Dictionary<string, ValueRepresentation>
            {
                ["lr"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["f1"] = ValueRepresentation.Float64,
                ["f2"] = ValueRepresentation.Float64
            });

        var caseStart = code.IndexOf($"case 0x{Entry + 8:X8}u:", StringComparison.Ordinal);
        Assert.True(caseStart >= 0, code);
        var caseBody = code[caseStart..code.IndexOf("goto", caseStart, StringComparison.Ordinal)];
        Assert.Contains("PPC_PsFromScalarInline(", caseBody, StringComparison.Ordinal);

        // The fallthrough already holds f1 paired, so it reaches the label unchanged.
        var label = code.IndexOf($"loc_{Entry + 8:X8}: ;", StringComparison.Ordinal);
        var previousLine = code.LastIndexOf('\n', code.LastIndexOf('\n', label) - 1);
        Assert.DoesNotContain("Scalar", code[previousLine..label], StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsFromScalarInline(", code[label..], StringComparison.Ordinal);
    }
}
