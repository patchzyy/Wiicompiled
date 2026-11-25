using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class RegisterResidencyCodeGenTests
{
    private static string Emit(
        IrFunction function,
        RepresentationEnvironment? types = null,
        bool stateFree = false,
        uint entryPoint = 0x80001000u,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        IReadOnlySet<uint>? lrContinuationCallTargets = null)
    {
        var contract = GuestAbiContractAnalyzer.Analyze(function);
        return new CxxLinearCodeGenerator().Emit(
            entryPoint,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types ?? new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            guestAbiContracts: guestAbiContracts,
            lrContinuationCallTargets: lrContinuationCallTargets,
            emitStateFreeLeafVariant: stateFree,
            stateFreeAbiContracts: stateFree
                ? new Dictionary<uint, GuestAbiContract> { [entryPoint] = contract }
                : null,
            stateFreeCallSymbols: stateFree
                ? new Dictionary<uint, string> { [entryPoint] = $"{function.Name}_native" }
                : null);
    }

    /// <summary>
    /// A synthetic callee contract. Everything not named here is empty, which is
    /// what makes the narrowing observable.
    /// </summary>
    private static GuestAbiContract NarrowContract(
        uint gprRead = 0,
        uint gprWrite = 0,
        uint fprRead = 0,
        uint fprWrite = 0,
        byte crRead = 0,
        byte crWrite = 0,
        bool readsCtr = false,
        bool writesCtr = false,
        GuestCallBoundaryFlags flags = GuestCallBoundaryFlags.None) =>
        new(
            GprReadBeforeWriteMask: gprRead,
            GprPossibleWriteMask: gprWrite,
            GprReturnMask: 0,
            FprReadBeforeWriteMask: fprRead,
            FprPossibleWriteMask: fprWrite,
            FprReturnMask: 0,
            CrReadBeforeWriteMask: crRead,
            CrPossibleWriteMask: crWrite,
            ReadsXerBeforeWrite: false,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: readsCtr,
            MayWriteCtr: writesCtr,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: flags,
            DirectCallTargets: Array.Empty<uint>());

    private static IrFunction CallerAcross(string name, string target) =>
        new(name, "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrBinary("r5", IrValue.Register("r5"), IrValue.Register("r6"), "add"),
                new IrCall(string.Empty, target, Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r5"), "add"),
                new IrReturn(null)
            })
        });

    private static string FunctionBody(string code, string name)
    {
        var declaration = $"extern \"C\" void {name}(CpuContext* MKW_RESTRICT ctx)";
        var start = code.IndexOf(declaration, StringComparison.Ordinal);
        Assert.True(start >= 0, $"missing definition of {name}");
        var open = code.IndexOf('{', start);
        var depth = 0;
        for (var index = open; index < code.Length; ++index)
        {
            if (code[index] == '{') ++depth;
            else if (code[index] == '}' && --depth == 0) return code[open..(index + 1)];
        }

        throw new InvalidOperationException("unterminated body");
    }

    [Fact]
    public void ResidentBodyLoadsAtEntryAndWritesBackAtReturn()
    {
        var function = new IrFunction("resident_add", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("uint32_t r3 = ctx->gpr[3];", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t r4 = ctx->gpr[4];", code, StringComparison.Ordinal);
        Assert.Contains("    r3 = (r3 + r4);", code, StringComparison.Ordinal);
        // r4 is never written, so it is not part of the write-back set.
        Assert.Contains("    ctx->gpr[3] = r3;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[4] = r4;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[3] = (", code, StringComparison.Ordinal);
    }

    [Fact]
    public void GuestCallIsBracketedByFlushAndReload()
    {
        var function = new IrFunction("resident_call", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

        var body = FunctionBody(Emit(function), "resident_call");
        var flush = body.IndexOf("    ctx->gpr[3] = r3;", StringComparison.Ordinal);
        var call = body.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        var reload = body.IndexOf("    r3 = ctx->gpr[3];", StringComparison.Ordinal);

        Assert.True(flush > 0 && call > flush && reload > call,
            "the call must be preceded by a flush and followed by a reload");
        // A register that is only read still has to be reloaded: the callee may
        // clobber it. It must not be flushed, because it was never modified.
        Assert.Contains("    r4 = ctx->gpr[4];", body, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[4] = r4;", body, StringComparison.Ordinal);
    }

    [Fact]
    public void IndirectJumpFlushesWithoutWritingLocalsBack()
    {
        var function = new IrFunction("resident_tail", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrIndirectJump(IrValue.Register("r3"))
            })
        });

        var body = FunctionBody(Emit(function), "resident_tail");
        var flush = body.IndexOf("ctx->gpr[3] = r3;", StringComparison.Ordinal);
        var jump = body.IndexOf("InvokeIndirectJump(r3, ctx);", StringComparison.Ordinal);

        Assert.True(flush > 0 && jump > flush);
        // Exactly one publication: nothing may be written back after the tail
        // dispatch, because the callee owns the architectural state from there.
        Assert.Equal(
            1,
            body.Split("ctx->gpr[3] = r3;", StringSplitOptions.None).Length - 1);
    }

    [Fact]
    public void ConditionRegisterAndCountRegisterBecomeLocals()
    {
        var function = new IrFunction("resident_loop", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("ctr", IrValue.Register("r3")),
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("bdnz", "entry", "exit", "ctr")
            }),
            new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
        });

        var code = Emit(function);

        Assert.Contains("uint32_t cr = ctx->cr;", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t ctr = ctx->ctr;", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t xer = ctx->xer;", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 0,", code, StringComparison.Ordinal);
        Assert.Contains("if ((ctr != 0))", code, StringComparison.Ordinal);
        Assert.Contains("    ctx->cr = cr;", code, StringComparison.Ordinal);
        Assert.Contains("    ctx->ctr = ctr;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("SetCR(ctx,", code, StringComparison.Ordinal);
    }

    [Fact]
    public void RawBranchConditionTextIsRoutedThroughTheResidentCondition()
    {
        // PpcLifter emits the general bc/bcctr condition as raw C++ text. It is
        // the only register access that bypasses the expression layer, so a
        // resident body must rewrite it or silently read a stale CR/CTR.
        var function = new IrFunction("resident_raw_condition", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch(
                    "raw",
                    "entry",
                    "exit",
                    "(((((ctx->ctr != 0)) ^ false)) && ((GetCRBit(ctx, 0, 2) == true)))")
            }),
            new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
        });

        var code = Emit(function);

        Assert.Contains("(((ctr != 0)) ^ false)", code, StringComparison.Ordinal);
        Assert.Contains("GetCRBitResident(cr, 0, 2)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->ctr !=", code, StringComparison.Ordinal);
        Assert.DoesNotContain("GetCRBit(ctx,", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatRegistersUseValueLocalsAndPreservePairedLanes()
    {
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64
        });
        var function = new IrFunction("resident_float", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("f1", "PPC_PsAdd", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                new IrReturn(null)
            })
        });

        var code = Emit(function, types);

        Assert.Contains("PPC_FPR f1 = ctx->fpr[1];", code, StringComparison.Ordinal);
        Assert.Contains("PPC_FPR f2 = ctx->fpr[2];", code, StringComparison.Ordinal);
        Assert.Contains("PpcSetPairedFprInline(f1,", code, StringComparison.Ordinal);
        // The whole union is written back so PS1 survives the boundary.
        Assert.Contains("    ctx->fpr[1] = f1;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->fpr[2] = f2;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeVariantIsUnchangedWhileThePublicEntryBecomesResident()
    {
        var function = new IrFunction("resident_state_free", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

        var code = Emit(function, stateFree: true);

        // The explicit-state clone is derived from the legacy cached body, so it
        // keeps the cached form while the public CpuContext entry is resident.
        var start = code.IndexOf("resident_state_free_native(", StringComparison.Ordinal);
        Assert.True(start >= 0);
        var variant = code[start..code.IndexOf("RECOMP_STATE_FREE_ABI", start, StringComparison.Ordinal)];
        Assert.DoesNotContain("ctx->gpr[3] = r3;", variant, StringComparison.Ordinal);

        var publicBody = FunctionBody(code, "resident_state_free");
        Assert.Contains("uint32_t r3 = ctx->gpr[3];", publicBody, StringComparison.Ordinal);
        Assert.Contains("    r3 = (r3 + r4);", publicBody, StringComparison.Ordinal);
        Assert.DoesNotContain("cached_r3", publicBody, StringComparison.Ordinal);
    }

    [Fact]
    public void BoundaryNarrowingSyncsOnlyTheContractIntersectionAtADirectCall()
    {
        // The callee reads r4 before writing it and can only write r3, so the
        // boundary needs r3 (it is in this frame's write set and in the callee
        // write set), r4 (the callee reads it), and nothing else on the flush;
        // the reload is limited to r3.
        var contracts = new Dictionary<uint, GuestAbiContract>
        {
            [0x80002000u] = NarrowContract(gprRead: 1u << 4, gprWrite: 1u << 3)
        };

        var body = FunctionBody(
            Emit(
                CallerAcross("narrowed_call", "0x80002000"),
                guestAbiContracts: contracts),
            "narrowed_call");
        var call = body.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.True(call > 0);
        var beforeCall = body[..call];
        var afterCall = body[call..];

        Assert.Contains("ctx->gpr[3] = r3;", beforeCall, StringComparison.Ordinal);
        // r5 is written by this frame but the callee neither reads nor writes it.
        Assert.DoesNotContain("ctx->gpr[5] = r5;", beforeCall, StringComparison.Ordinal);
        Assert.Contains("    r3 = ctx->gpr[3];", afterCall, StringComparison.Ordinal);
        Assert.DoesNotContain("r4 = ctx->gpr[4];", afterCall, StringComparison.Ordinal);
        Assert.DoesNotContain("r5 = ctx->gpr[5];", afterCall, StringComparison.Ordinal);
    }

    [Fact]
    public void BoundaryNarrowingDoesNotApplyToLrContinuationCalls()
    {
        // The callee can resume through ctx->lr into code outside its own
        // contract, which reads every register from the context.
        var function = new IrFunction("lr_continuation_call", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r5", IrValue.Register("r5"), IrValue.Register("r6"), "add"),
                new IrAssign("lr", IrValue.Imm(0x80001010)),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r5"), "add"),
                new IrReturn(null)
            })
        });
        var contracts = new Dictionary<uint, GuestAbiContract>
        {
            [0x80002000u] = NarrowContract(gprWrite: 1u << 3)
        };

        var body = FunctionBody(
            Emit(
                function,
                guestAbiContracts: contracts,
                lrContinuationCallTargets: new HashSet<uint> { 0x80002000u }),
            "lr_continuation_call");
        var call = body.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.True(call > 0);

        Assert.Contains("ctx->gpr[5] = r5;", body[..call], StringComparison.Ordinal);
    }

    [Fact]
    public void BoundaryNarrowingFlushesRegistersTheCalleeOnlyMayWrite()
    {
        // A register the callee may write has to be flushed even though the
        // callee never reads it: the write is only possible, so on the paths
        // where it does not happen the reload after the call would otherwise
        // load a stale architectural value back into the local.
        var contracts = new Dictionary<uint, GuestAbiContract>
        {
            [0x80002000u] = NarrowContract(gprWrite: (1u << 3) | (1u << 5))
        };

        var body = FunctionBody(
            Emit(
                CallerAcross("narrowed_possible_write", "0x80002000"),
                guestAbiContracts: contracts),
            "narrowed_possible_write");
        var call = body.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        var beforeCall = body[..call];
        var afterCall = body[call..];

        Assert.Contains("ctx->gpr[3] = r3;", beforeCall, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[5] = r5;", beforeCall, StringComparison.Ordinal);
        Assert.Contains("    r3 = ctx->gpr[3];", afterCall, StringComparison.Ordinal);
        Assert.Contains("    r5 = ctx->gpr[5];", afterCall, StringComparison.Ordinal);
        Assert.DoesNotContain("r4 = ctx->gpr[4];", afterCall, StringComparison.Ordinal);
    }

    [Fact]
    public void BoundaryNarrowingSyncsConditionAndCountRegistersOnAnyIntersectingField()
    {
        // The contract describes CR per field while the local is the packed
        // register, so a single intersecting field forces the whole sync. CTR is
        // synced whenever the contract cannot prove the callee leaves it alone.
        var function = new IrFunction("narrowed_special", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("ctr", IrValue.Register("r3")),
                new IrSetCrField(2, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBranch("bdnz", "entry", "exit", "ctr")
            }),
            new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
        });

        string BodyFor(GuestAbiContract contract) => FunctionBody(
            Emit(
                function,
                guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = contract }),
            "narrowed_special");

        var touched = BodyFor(NarrowContract(crRead: 1 << 5, crWrite: 1 << 1, writesCtr: true));
        Assert.Contains("ctx->cr = cr;", touched, StringComparison.Ordinal);
        Assert.Contains("    cr = ctx->cr;", touched, StringComparison.Ordinal);
        Assert.Contains("ctx->ctr = ctr;", touched, StringComparison.Ordinal);
        Assert.Contains("    ctr = ctx->ctr;", touched, StringComparison.Ordinal);

        var untouched = BodyFor(NarrowContract(gprWrite: 1u << 3));
        var call = untouched.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        // The exit flush still publishes CR/CTR; only the call boundary drops it.
        Assert.DoesNotContain("ctx->cr = cr;", untouched[..call], StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->ctr = ctr;", untouched[..call], StringComparison.Ordinal);
        Assert.DoesNotContain("    cr = ctx->cr;", untouched[call..], StringComparison.Ordinal);
        Assert.DoesNotContain("    ctr = ctx->ctr;", untouched[call..], StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void BoundaryNarrowingFallsBackToAFullSyncWithoutAPreciseContract(bool haveContract)
    {
        // A missing contract and a full-synchronization fence are the same thing
        // here: neither proves anything about the callee's register usage.
        var contracts = haveContract
            ? new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = NarrowContract(
                    gprWrite: 1u << 3,
                    flags: GuestCallBoundaryFlags.RequiresCompleteContext)
            }
            : new Dictionary<uint, GuestAbiContract>();

        var caller = CallerAcross("narrowed_fence", "0x80002000");
        var narrowed = FunctionBody(
            Emit(caller, guestAbiContracts: contracts),
            "narrowed_fence");

        // Full synchronization, asserted by shape: r5 is flushed even though the
        // callee is declared unable to write it, and r4 is reloaded even though
        // this frame only reads it. Narrowing would have dropped both.
        var call = narrowed.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[5] = r5;", narrowed[..call], StringComparison.Ordinal);
        Assert.Contains("    r4 = ctx->gpr[4];", narrowed[call..], StringComparison.Ordinal);
    }

    [Fact]
    public void BoundaryNarrowingDoesNotApplyToIndirectCalls()
    {
        var function = new IrFunction("narrowed_indirect", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrIndirectCall(string.Empty, IrValue.Register("r12"), Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

        var narrowed = FunctionBody(Emit(function), "narrowed_indirect");

        // There is no callee contract to narrow against, so the boundary stays
        // full: r4 is reloaded after the call even though this frame only reads it.
        Assert.Contains("    r4 = ctx->gpr[4];", narrowed, StringComparison.Ordinal);
    }
}
