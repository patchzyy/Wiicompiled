using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

/// <summary>
/// Register residency writes a resident register back to CpuContext only while its local may differ from the
/// context (written since the entry load, the last reload or the last write-back).
/// </summary>
public sealed class DirtyRegisterWritebackCodeGenTests
{
    private const string Lf = "\n";

    private static string Emit(IrFunction function, RepresentationEnvironment? types = null)
    {
        return new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types ?? new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()))
            .ReplaceLineEndings(Lf);
    }

    private static int Count(string text, string needle) =>
        text.Split(needle, StringSplitOptions.None).Length - 1;

    private static string BlockText(string code, string label)
    {
        var start = code.IndexOf($"\n{label}:\n", StringComparison.Ordinal);
        Assert.True(start >= 0, $"missing block {label}");
        var end = code.IndexOf("\n}\n", start, StringComparison.Ordinal);
        Assert.True(end > start, $"unterminated block {label}");
        return code[start..end];
    }

    private static IrInstruction Inc(string reg) =>
        new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(1), "add");

    [Fact]
    public void StraightLineWritesBackOnlyRegistersChangedSinceTheLastSync()
    {
        // r3 and r4 are modified before the call, so the call flushes both. Afterwards only r3 changes, so the
        // return must publish r3 and nothing else: r4 was reloaded by the call and is equal to CpuContext.
        var function = new IrFunction("dirty_straight", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                Inc("r4"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                Inc("r3"),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(2, Count(code, "ctx->gpr[3] = r3;"));
        Assert.Equal(1, Count(code, "ctx->gpr[4] = r4;"));
        var call = code.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.True(call > 0);
        Assert.DoesNotContain("ctx->gpr[4] = r4;", code[call..], StringComparison.Ordinal);
    }

    [Fact]
    public void CleanRegisterIsNotPublishedAgainAfterABoundaryReloadedIt()
    {
        var function = new IrFunction("dirty_clean_after_call", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(1, Count(code, "ctx->gpr[3] = r3;"));
        Assert.Contains("r3 = ctx->gpr[3];", code, StringComparison.Ordinal);
    }

    [Fact]
    public void RegisterWrittenOnlyAfterTheLastBoundaryIsNotFlushedAtThatBoundary()
    {
        var function = new IrFunction("dirty_late_write", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                Inc("r5"),
                new IrReturn(null)
            })
        });

        var code = Emit(function);
        var call = code.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);

        var beforeCall = code[..call];
        Assert.Contains("ctx->gpr[3] = r3;", beforeCall, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[5] = r5;", beforeCall, StringComparison.Ordinal);
        var afterCall = code[call..];
        Assert.Contains("ctx->gpr[5] = r5;", afterCall, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[3] = r3;", afterCall, StringComparison.Ordinal);
    }

    [Fact]
    public void JoinOfDirtyAndCleanPathsKeepsTheRegisterDirty()
    {
        // entry: r3 modified.  a: r5 modified, then a call (flushes + reloads everything).  b: r6 modified.
        // At merge: r3 and r6 are dirty on the path through b, r5 is clean on both paths. The return publishes r3
        // and r6 (not r5); the call in a publishes r3 and r5 (not r6, not yet modified there).
        var function = new IrFunction("dirty_join", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                new IrBranch("beq", "a", "b")
            }),
            new IrBasicBlock("a", new IrInstruction[]
            {
                Inc("r5"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrJump("merge")
            }),
            new IrBasicBlock("b", new IrInstruction[]
            {
                Inc("r6"),
                new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        var blockA = BlockText(code, "loc_a");
        Assert.Contains("ctx->gpr[3] = r3;", blockA, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[5] = r5;", blockA, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[6] = r6;", blockA, StringComparison.Ordinal);

        var merge = BlockText(code, "loc_merge");
        Assert.Contains("ctx->gpr[3] = r3;", merge, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[6] = r6;", merge, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[5] = r5;", merge, StringComparison.Ordinal);
    }

    [Fact]
    public void LoopBackEdgeCarriesDirtyStateIntoTheLoopHeader()
    {
        // r3 is modified inside the loop body, and the call in the header runs on every iteration: on the second
        // iteration r3 is dirty at the header's call, so that call must publish it even though it is clean on
        // the first entry.
        var function = new IrFunction("dirty_loop", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrJump("head")
            }),
            new IrBasicBlock("head", new IrInstruction[]
            {
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                Inc("r3"),
                new IrBranch("bne", "head", "exit")
            }),
            new IrBasicBlock("exit", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        var head = BlockText(code, "loc_head");
        var call = head.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.True(call > 0);
        Assert.Contains("ctx->gpr[3] = r3;", head[..call], StringComparison.Ordinal);
    }

    [Fact]
    public void CleanFunctionEntryPublishesNothingAtAnExitThatFollowsNoWrite()
    {
        // r3 is written only on the "set" path; the "skip" path returns at once, publishing nothing.
        var function = new IrFunction("dirty_skip", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("beq", "set", "skip")
            }),
            new IrBasicBlock("set", new IrInstruction[]
            {
                Inc("r3"),
                new IrReturn(null)
            }),
            new IrBasicBlock("skip", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(1, Count(code, "ctx->gpr[3] = r3;"));
        Assert.DoesNotContain("ctx->gpr[3] = r3;", BlockText(code, "loc_skip"), StringComparison.Ordinal);
    }

    [Fact]
    public void ConditionRegisterAndCountRegisterAreTrackedLikeGprs()
    {
        var function = new IrFunction("dirty_cr_ctr", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("ctr", IrValue.Register("r3")),
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        // cr and ctr are published once (at the call) and reloaded; the return has nothing left to publish.
        Assert.Equal(1, Count(code, "ctx->cr = cr;"));
        Assert.Equal(1, Count(code, "ctx->ctr = ctr;"));
    }

    [Fact]
    public void IndirectJumpPublishesDirtyRegistersAndTrackingContinuesPastTheExit()
    {
        // The default arm of a jump table leaves the frame; its write-back must not make the register look clean
        // on the case edges.
        var function = new IrFunction("dirty_jump_table", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                new IrJumpTable("r4", new[]
                {
                    new IrJumpTableCase(0x80001010u, "t0"),
                    new IrJumpTableCase(0x80001020u, "t1")
                })
            }),
            new IrBasicBlock("t0", new IrInstruction[]
            {
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }),
            new IrBasicBlock("t1", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        // default arm, t0's call and t1's return each publish r3 (dirty from entry on every path).
        Assert.Equal(3, Count(code, "ctx->gpr[3] = r3;"));
        Assert.Contains("ctx->gpr[3] = r3;", BlockText(code, "loc_t1"), StringComparison.Ordinal);
    }

    [Fact]
    public void EdgeNormalizationWriteMakesTheRegisterDirtyOnThatEdgeOnly()
    {
        // f5 is produced as a paired value, flushed and reloaded by a call (clean), then normalized to scalar on the
        // edge into "merge". The edge write makes f5 dirty at merge's return; the other predecessor leaves it clean.
        var function = new IrFunction("dirty_edge", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("beq", "paired", "scalar")
            }),
            new IrBasicBlock("paired", new IrInstruction[]
            {
                new IrCall("f5", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrJump("merge")
            }),
            new IrBasicBlock("scalar", new IrInstruction[]
            {
                new IrAssign("f5", IrValue.Register("f3")),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[]
            {
                new IrCall("f6", "PPC_PsNeg", new[] { IrValue.Register("f5") }),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f5"] = ValueRepresentation.Float64,
            ["f6"] = ValueRepresentation.Float64,
            ["cr0"] = ValueRepresentation.UInt32
        });

        var code = Emit(function, types);

        Assert.Contains("f5.d = PPC_PsToScalarInline(f5.d);", code, StringComparison.Ordinal);
        var merge = BlockText(code, "loc_merge");
        // f5 reached merge clean on both paths, and normalization wrote it on one edge: dirty, so published.
        Assert.Contains("ctx->fpr[5] = f5;", merge, StringComparison.Ordinal);
        // f6 is produced in merge itself.
        Assert.Contains("ctx->fpr[6] = f6;", merge, StringComparison.Ordinal);
    }

    [Fact]
    public void IndirectCallIsAFullBoundaryAndLeavesEveryRegisterClean()
    {
        var function = new IrFunction("dirty_indirect_call", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                Inc("r4"),
                new IrIndirectCall(string.Empty, IrValue.Register("r5"), Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(1, Count(code, "ctx->gpr[3] = r3;"));
        Assert.Equal(1, Count(code, "ctx->gpr[4] = r4;"));
    }

    [Fact]
    public void FunctionWithLrContinuationLabelsKeepsPublishingEveryWrittenRegister()
    {
        // The callee may resume this body at the continuation label with state the dataflow does not model, so a
        // function that has one stays on the old write-back-everything-written behaviour.
        var function = new IrFunction("dirty_lr_continuation", "0x800E591C", new[]
        {
            new IrBasicBlock("0x800E591C", new IrInstruction[]
            {
                new IrAssign("r4", IrValue.Imm(5)),
                new IrAssign("lr", IrValue.Imm(unchecked((int)0x800E5920u))),
                new IrCall(string.Empty, "0x8179AC3C", Array.Empty<IrValue>()),
                new IrAssign("r3", IrValue.Imm(8)),
                new IrReturn(null)
            }),
            new IrBasicBlock("0x800E5934", new IrInstruction[]
            {
                new IrAssign("r3", IrValue.Imm(1)),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x800E591C,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179AC3Cu }).ReplaceLineEndings(Lf);

        var continuation = BlockText(code, "loc_800E5934");
        Assert.Contains("ctx->gpr[3] = r3;", continuation, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[4] = r4;", continuation, StringComparison.Ordinal);
    }

    private static GuestAbiContract Contract(uint gprRead = 0, uint gprWrite = 0) =>
        new(
            GprReadBeforeWriteMask: gprRead,
            GprPossibleWriteMask: gprWrite,
            GprReturnMask: 0,
            FprReadBeforeWriteMask: 0,
            FprPossibleWriteMask: 0,
            FprReturnMask: 0,
            CrReadBeforeWriteMask: 0,
            CrPossibleWriteMask: 0,
            ReadsXerBeforeWrite: false,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

    [Fact]
    public void NarrowContractFlushLeavesTheFlushedRegisterCleanWithoutAReload()
    {
        // The callee only reads r3: the call publishes r3 and does not reload it, so the return must not publish
        // it a second time. r4 (written, outside the contract) stays dirty across the call.
        var function = new IrFunction("dirty_narrow", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                Inc("r4"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            guestAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = Contract(gprRead: 1u << 3)
            }).ReplaceLineEndings(Lf);

        Assert.Equal(1, Count(code, "ctx->gpr[3] = r3;"));
        Assert.Equal(1, Count(code, "ctx->gpr[4] = r4;"));
        var call = code.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        Assert.True(call > 0);
        Assert.DoesNotContain("ctx->gpr[3] = r3;", code[call..], StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[4] = r4;", code[call..], StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeFastArmResultStaysDirtyAfterTheSlowArmCleanedIt()
    {
        // Fast arm: the result lands in the r3 local only (dirty). Slow arm: flush + call + reload (clean). After
        // the join r3 is dirty, so the return publishes it; r4 was dirty before the call and is not touched by
        // the fast arm, so it is dirty too.
        var function = new IrFunction("dirty_state_free", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r4"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = Contract(gprRead: 1u << 3, gprWrite: 1u << 3)
            },
            stateFreeCallSymbols: new Dictionary<uint, string> { [0x80002000u] = "callee_native" })
            .ReplaceLineEndings(Lf);

        var elseArm = code.IndexOf("} else {", StringComparison.Ordinal);
        var slowEnd = code.IndexOf("\n    }\n", elseArm, StringComparison.Ordinal);
        Assert.True(elseArm > 0 && slowEnd > elseArm, code);
        var afterCall = code[slowEnd..];
        Assert.Contains("ctx->gpr[3] = r3;", afterCall, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[4] = r4;", afterCall, StringComparison.Ordinal);
        // The slow arm itself publishes what is dirty at that point (r4 only: r3 is not yet written there).
        var slowArm = code[elseArm..slowEnd];
        Assert.Contains("ctx->gpr[4] = r4;", slowArm, StringComparison.Ordinal);
    }

    [Fact]
    public void BlockNoRecordedEdgeReachesStartsWithEverythingDirty()
    {
        var function = new IrFunction("dirty_unreached", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Inc("r3"),
                new IrReturn(null)
            }),
            new IrBasicBlock("orphan", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("ctx->gpr[3] = r3;", BlockText(code, "loc_orphan"), StringComparison.Ordinal);
    }
}
