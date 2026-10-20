using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class CodeGenTests
{
    private sealed class FixedGuestAbiProvider : IGuestFunctionAbiProvider
    {
        private readonly Dictionary<string, GuestFunctionAbi> _abis;

        public FixedGuestAbiProvider(params (string Target, GuestFunctionAbi Abi)[] abis)
        {
            _abis = abis.ToDictionary(
                static item => item.Target,
                static item => item.Abi,
                StringComparer.OrdinalIgnoreCase);
        }

        public bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi) =>
            _abis.TryGetValue(target, out abi!);
    }

    [Fact]
    public void CodeGenerator_CompilesAndRunsThroughBridge()
    {
        var function = new IrFunction(
            "add_two",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                    new IrReturn(IrValue.Register("r3"))
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32,
            ["r4"] = ValueRepresentation.Int32,
            ["r3_0"] = ValueRepresentation.Int32,
            ["r3_1"] = ValueRepresentation.Int32 // SSA versions
        });
        var signature = new FunctionAbiClassification("add_two", ValueRepresentation.Int32);

        // Convert to SSA
        var ssa = new SsaTransformer().Convert(function);
        // Linear generator expects entryPoint for function mapping
        var code = new CxxLinearCodeGenerator().Emit(0x80001000, ssa, signature, types);

        var root = Translator.Core.Loading.ProjectPaths.FindRepositoryRoot();
        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_codegen_tests");
        Directory.CreateDirectory(tempDir);

        var genPath = Path.Combine(tempDir, "generated.cpp");
        var harnessPath = Path.Combine(tempDir, "harness.cpp");
        File.WriteAllText(genPath, code);

        var harness = new StringBuilder();
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("#include \"abi_bridge.h\"");
        harness.AppendLine("// Mocks for runtime symbols");
        harness.AppendLine("void TranslatedFunctionRegistry::Register(TranslatedFunctionInfo info) {}");
        harness.AppendLine("CpuContext& GetPersistentCpuContext() { static CpuContext ctx; return ctx; }");
        
        harness.AppendLine("extern \"C\" void add_two(CpuContext* ctx);");
        harness.AppendLine("int main() {");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    ctx.gpr[3] = 5;");
        harness.AppendLine("    ctx.gpr[4] = 7;");
        harness.AppendLine("    add_two(&ctx);");
        harness.AppendLine("    if (ctx.gpr[3] != 12) { return 1; }");
        harness.AppendLine("    std::cout << ctx.gpr[3];");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");
        File.WriteAllText(harnessPath, harness.ToString());

        var exePath = Path.Combine(tempDir, "a.out");
        var gpp = new ProcessStartInfo
        {
            FileName = "g++",
            ArgumentList =
            {
                "-std=c++20",
                "-Wall",
                "-Werror",
                "-Wno-error=unused-but-set-variable",
                "-I", Path.Combine(root, "runtime", "include"),
                genPath,
                harnessPath,
                "-o", exePath
            },
            RedirectStandardError = true,
            RedirectStandardOutput = true
        };

        using (var proc = Process.Start(gpp)!)
        {
            proc.WaitForExit(20000);
            if (proc.ExitCode != 0)
            {
                var output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
                throw new InvalidOperationException($"g++ failed: {output}\n\nGenerated Code:\n{code}");
            }
        }

        var run = new ProcessStartInfo
        {
            FileName = exePath,
            RedirectStandardOutput = true
        };
        using var runProc = Process.Start(run)!;
        runProc.WaitForExit(5000);
        var outputText = runProc.StandardOutput.ReadToEnd().Trim();

        Assert.Equal(0, runProc.ExitCode);
        Assert.Equal("12", outputText);
    }

    [Fact]
    public void CodeGenerator_ReturnsAfterNonReturningGuestCallTarget()
    {
        var function = new IrFunction(
            "tail_call_bridge",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "func_8170B834", Array.Empty<IrValue>()),
                    new IrAssign("r3", IrValue.Imm(1)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("tail_call_bridge", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80008004,
            ssa,
            signature,
            types,
            nonReturningCallTargets: new HashSet<uint> { 0x8170B834u });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8170B834u>(ctx);", StringComparison.Ordinal);
        Assert.True(callIndex >= 0);
        var returnIndex = code.IndexOf("return;", callIndex, StringComparison.Ordinal);
        var assignmentIndex = code.IndexOf("ctx->gpr[3] = 1;", callIndex, StringComparison.Ordinal);
        Assert.True(returnIndex > callIndex);
        Assert.True(assignmentIndex > returnIndex);
    }

    [Fact]
    public void CodeGenerator_DispatchesNonReturningGuestCallLrToLocalContinuation()
    {
        var function = new IrFunction(
            "local_lr_continuation",
            "0x80661104",
            new[]
            {
                new IrBasicBlock("0x80661104", new IrInstruction[]
                {
                    new IrCall(string.Empty, "func_8172CC0C", Array.Empty<IrValue>()),
                    new IrReturn(null)
                }),
                new IrBasicBlock("0x80661144", new IrInstruction[]
                {
                    new IrAssign("r3", IrValue.Imm(1)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("local_lr_continuation", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80661078,
            ssa,
            signature,
            types,
            nonReturningCallTargets: new HashSet<uint> { 0x8172CC0Cu });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8172CC0Cu>(ctx);", StringComparison.Ordinal);
        var localCaseIndex = code.IndexOf("case 0x80661144u:", callIndex, StringComparison.Ordinal);
        var localGotoIndex = code.IndexOf("goto loc_80661144;", localCaseIndex, StringComparison.Ordinal);
        var registryIndex = code.IndexOf("TranslatedFunctionRegistry::FindByAddressPtr(ctx->lr)", callIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(localCaseIndex > callIndex);
        Assert.True(localGotoIndex > localCaseIndex);
        Assert.True(registryIndex > localGotoIndex);
    }

    [Fact]
    public void CodeGenerator_FallsThroughAfterNonReturningGuestCallWhenLrIsNextInstruction()
    {
        var function = new IrFunction(
            "same_block_lr_continuation",
            "0x80661104",
            new[]
            {
                new IrBasicBlock("0x80661104", new IrInstruction[]
                {
                    new IrAssign("lr", IrValue.Imm(unchecked((int)0x80661144u))),
                    new IrCall(string.Empty, "func_8172CC0C", Array.Empty<IrValue>()),
                    new IrAssign("r3", IrValue.Imm(1)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("same_block_lr_continuation", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80661078,
            ssa,
            signature,
            types,
            nonReturningCallTargets: new HashSet<uint> { 0x8172CC0Cu });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8172CC0Cu>(ctx);", StringComparison.Ordinal);
        var fallthroughGuardIndex = code.IndexOf("if (ctx->lr != 0x80661144u)", callIndex, StringComparison.Ordinal);
        var returnIndex = code.IndexOf("return;", fallthroughGuardIndex, StringComparison.Ordinal);
        var assignmentIndex = code.IndexOf("ctx->gpr[3] = 1;", callIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(fallthroughGuardIndex > callIndex);
        Assert.True(returnIndex > fallthroughGuardIndex);
        Assert.True(assignmentIndex > returnIndex);
    }

    [Fact]
    public void CodeGenerator_DispatchesGuestCallLrContinuationWithoutMarkingTargetNonReturning()
    {
        var function = new IrFunction(
            "lr_continuation_call",
            "0x800E591C",
            new[]
            {
                new IrBasicBlock("0x800E591C", new IrInstruction[]
                {
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
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("lr_continuation_call", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x800E591C,
            ssa,
            signature,
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179AC3Cu });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8179AC3Cu>(ctx);", StringComparison.Ordinal);
        var fallthroughGuardIndex = code.IndexOf("if (ctx->lr != 0x800E5920u)", callIndex, StringComparison.Ordinal);
        var localCaseIndex = code.IndexOf("case 0x800E5934u:", fallthroughGuardIndex, StringComparison.Ordinal);
        var returnIndex = code.IndexOf("return;", localCaseIndex, StringComparison.Ordinal);
        var fallthroughAssignmentIndex = code.IndexOf("ctx->gpr[3] = 8;", callIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(fallthroughGuardIndex > callIndex);
        Assert.True(localCaseIndex > fallthroughGuardIndex);
        Assert.True(returnIndex > localCaseIndex);
        Assert.True(fallthroughAssignmentIndex > returnIndex);
    }

    [Fact]
    public void CodeGenerator_DispatchesGuestCallLrToInstructionInsideBasicBlock()
    {
        var function = new IrFunction(
            "mid_block_lr_continuation",
            "0x80535118",
            new[]
            {
                new IrBasicBlock("0x80535118", new IrInstruction[]
                {
                    new IrTracePpc(0x80535118u, "lhz r0, 0x1a(r3)", "0xA003001A"),
                    new IrAssign("lr", IrValue.Imm(unchecked((int)0x80535130u))),
                    new IrCall(string.Empty, "0x8179B000", Array.Empty<IrValue>()),
                    new IrTracePpc(0x80535130u, "addi r4, r4, 1", "0x38840001"),
                    new IrAssign("r4", IrValue.Imm(1)),
                    new IrTracePpc(0x8053513Cu, "cmpwi r0, 0", "0x2C000000"),
                    new IrAssign("r3", IrValue.Imm(2)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("mid_block_lr_continuation", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80534DF8,
            ssa,
            signature,
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179B000u });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8179B000u>(ctx);", StringComparison.Ordinal);
        var localCaseIndex = code.IndexOf("case 0x8053513Cu:", callIndex, StringComparison.Ordinal);
        var localGotoIndex = code.IndexOf("goto loc_8053513C;", localCaseIndex, StringComparison.Ordinal);
        var instructionLabelIndex = code.IndexOf("loc_8053513C:", localGotoIndex, StringComparison.Ordinal);
        var assignmentIndex = code.IndexOf("ctx->gpr[3] = 2;", instructionLabelIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(localCaseIndex > callIndex);
        Assert.True(localGotoIndex > localCaseIndex);
        Assert.True(instructionLabelIndex > localGotoIndex);
        Assert.True(assignmentIndex > instructionLabelIndex);
    }

    [Fact]
    public void CodeGenerator_WrapsScalarPpcPsqStPayloadAsScalarPair()
    {
        var function = new IrFunction(
            "psq_store_passthrough",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(
                        string.Empty,
                        "PPC_PsqSt",
                        new[]
                        {
                            IrValue.Register("r3"),
                            IrValue.Register("f1"),
                            IrValue.Imm(0),
                            IrValue.Imm(0)
                        }),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32,
            ["f1"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("psq_store_passthrough", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008000, ssa, signature, types);

        Assert.Contains("PPC_PsqStInline<0u, 0u>(", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsFromScalarInline", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_ExtractsPs0WhenScalarStoreConsumesPairedRegister()
    {
        var function = new IrFunction(
            "paired_stfs_source",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(
                        "f2",
                        "PPC_PsqL",
                        new[]
                        {
                            IrValue.Register("r3"),
                            IrValue.Imm(0),
                            IrValue.Imm(0)
                        }),
                    new IrStore(new IrAddress("r4", 0), IrValue.Register("f2"), 4),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32,
            ["r4"] = ValueRepresentation.Int32,
            ["f2"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification(
            "paired_stfs_source",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008010, ssa, signature, types);

        Assert.Contains("ctx->fpr[2].d = PPC_PsqLInline<0u, 0u>(ctx, ctx->gpr[3]);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat32(ctx->gpr[4], PPC_PsToScalarInline(ctx->fpr[2].d));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_EmitsDirectGpuFifoStoresForKnownGatherPipeAddress()
    {
        var function = new IrFunction(
            "known_gx_fifo_stores",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrAssign("r5", IrValue.Imm(unchecked((int)0xCC010000))),
                    new IrAssign("r6", IrValue.Imm(unchecked((int)0xCC010100))),
                    new IrStore(new IrAddress("r5", -32768), IrValue.Register("r3"), 1),
                    new IrStore(new IrAddress("r5", -32768), IrValue.Register("r4"), 2),
                    new IrStore(new IrAddress("r5", -32768), IrValue.Register("r7"), 4),
                    new IrStore(new IrAddress("r5", -32768), IrValue.Register("f1"), 4),
                    new IrStore(new IrAddress("r6", -32768), IrValue.Register("r3"), 1),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32,
            ["r5"] = ValueRepresentation.UInt32,
            ["r6"] = ValueRepresentation.UInt32,
            ["r7"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification(
            "known_gx_fifo_stores",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008018, ssa, signature, types);

        Assert.Contains("GX_HLE_FIFO_Write8(static_cast<uint8_t>(ctx->gpr[3]));", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_Write16(static_cast<uint16_t>(ctx->gpr[4]));", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_Write32(static_cast<uint32_t>(ctx->gpr[7]));", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_WriteFloat(static_cast<float>(ctx->fpr[1].d));", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWrite8((ctx->gpr[6] + -32768), static_cast<uint32_t>(ctx->gpr[3]));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_ReadsKnownMmioAddressesThroughCheckedHelper()
    {
        var function = new IrFunction(
            "known_mmio_load",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrAssign("r4", IrValue.Imm(unchecked((int)0xCD000000))),
                    new IrLoad("r0", new IrAddress("r4", 0xC0), 4),
                    new IrLoad("r5", new IrAddress("r3", 8), 4),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r0"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32,
            ["r5"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification(
            "known_mmio_load",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008020, ssa, signature, types);

        Assert.Contains("= Memory::Read32(", code, StringComparison.Ordinal);
        Assert.Contains("= MemoryInline::FlatRead32((ctx->gpr[3] + 8));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_ReusesFctiwzStackLowWordForMatchingLoad()
    {
        var function = new IrFunction(
            "fctiwz_low_word_stack_load",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("f0", IrValue.Register("f1"), IrValue.Imm(0), "fctiwz"),
                    new IrStore(new IrAddress("r1", -16), IrValue.Register("f0"), 8),
                    new IrLoad("r3", new IrAddress("r1", -12), 4),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["f0"] = ValueRepresentation.Float64,
            ["f1"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification(
            "fctiwz_low_word_stack_load",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x8000801C, ssa, signature, types);

        Assert.Contains("fctiwzword0 = PPC_FprLowWordInline(ctx->fpr[0].d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWriteFloat64((ctx->gpr[1] + -16), ctx->fpr[0].d);", code, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[3] = fctiwzword0;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[3] = MemoryInline::FlatRead32((ctx->gpr[1] + -12));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_KeepsFctiwzStackStoreAcrossGuestCallBarrier()
    {
        var function = new IrFunction(
            "fctiwz_low_word_call_barrier",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("f0", IrValue.Register("f1"), IrValue.Imm(0), "fctiwz"),
                    new IrStore(new IrAddress("r1", -16), IrValue.Register("f0"), 8),
                    new IrLoad("r3", new IrAddress("r1", -12), 4),
                    new IrCall(string.Empty, "func_80001234", Array.Empty<IrValue>()),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["f0"] = ValueRepresentation.Float64,
            ["f1"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification(
            "fctiwz_low_word_call_barrier",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x8000801E, ssa, signature, types);

        Assert.Contains("fctiwzword0 = PPC_FprLowWordInline(ctx->fpr[0].d);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((ctx->gpr[1] + -16), ctx->fpr[0].d);", code, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[3] = fctiwzword0;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_DoesNotReuseFctiwzStackLowWordAcrossOverlappingAccess()
    {
        var function = new IrFunction(
            "fctiwz_low_word_overlap",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("f0", IrValue.Register("f1"), IrValue.Imm(0), "fctiwz"),
                    new IrStore(new IrAddress("r1", -16), IrValue.Register("f0"), 8),
                    new IrStore(new IrAddress("r1", -12), IrValue.Register("r4"), 4),
                    new IrLoad("r3", new IrAddress("r1", -12), 4),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32,
            ["f0"] = ValueRepresentation.Float64,
            ["f1"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification(
            "fctiwz_low_word_overlap",
            ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x8000801D, ssa, signature, types);

        Assert.DoesNotContain("PPC_FprLowWordInline", code, StringComparison.Ordinal);
        Assert.Contains("ctx->gpr[3] = MemoryInline::FlatRead32((ctx->gpr[1] + -12));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_InlinesMidRangeSaveFprThunkEntries()
    {
        var function = new IrFunction(
            "save_fpr_midrange_thunk",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "func_80021504", Array.Empty<IrValue>()),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r11"] = ValueRepresentation.Int32,
            ["f26"] = ValueRepresentation.Float64,
            ["f27"] = ValueRepresentation.Float64,
            ["f28"] = ValueRepresentation.Float64,
            ["f29"] = ValueRepresentation.Float64,
            ["f30"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("save_fpr_midrange_thunk", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008020, ssa, signature, types);

        Assert.DoesNotContain("InvokeDirectCpu<0x80021504u>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((ctx->gpr[11] + -48), ctx->fpr[26].d);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((ctx->gpr[11] + -8), ctx->fpr[31].d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("DebugTrackPcIfLightweight", code, StringComparison.Ordinal);
        Assert.Contains("// RECOMP_REGISTRATION base 0x80008020 save_fpr_midrange_thunk preserves=true fpr_mask=0x00000000", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_InlinesMidRangeRestFprThunkEntries()
    {
        var function = new IrFunction(
            "rest_fpr_midrange_thunk",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "func_80021550", Array.Empty<IrValue>()),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r11"] = ValueRepresentation.Int32,
            ["f26"] = ValueRepresentation.Float64,
            ["f27"] = ValueRepresentation.Float64,
            ["f28"] = ValueRepresentation.Float64,
            ["f29"] = ValueRepresentation.Float64,
            ["f30"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("rest_fpr_midrange_thunk", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80008024, ssa, signature, types);

        Assert.DoesNotContain("InvokeDirectCpu<0x80021550u>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("ctx->fpr[26].d = MemoryInline::FlatReadFloat64((ctx->gpr[11] + -48));", code, StringComparison.Ordinal);
        Assert.Contains("ctx->fpr[31].d = MemoryInline::FlatReadFloat64((ctx->gpr[11] + -8));", code, StringComparison.Ordinal);
        Assert.DoesNotContain("DebugTrackPcIfLightweight", code, StringComparison.Ordinal);
        Assert.Contains("// RECOMP_REGISTRATION base 0x80008024 rest_fpr_midrange_thunk preserves=false fpr_mask=0xFC000000", code, StringComparison.Ordinal);
    }

    [Fact]
    public void Bridge_PassesFloatingPointRegisters()
    {
        var function = new IrFunction(
            "float_passthrough",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrReturn(IrValue.Register("f1"))
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float32
        });
        var signature = new FunctionAbiClassification("float_passthrough", ValueRepresentation.Float32);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80002000, ssa, signature, types);

        var root = Translator.Core.Loading.ProjectPaths.FindRepositoryRoot();
        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_codegen_float_tests");
        Directory.CreateDirectory(tempDir);

        var genPath = Path.Combine(tempDir, "generated_float.cpp");
        var harnessPath = Path.Combine(tempDir, "harness_float.cpp");
        File.WriteAllText(genPath, code);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cmath>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("#include \"abi_bridge.h\"");
        harness.AppendLine("// Mocks for runtime symbols");
        harness.AppendLine("void TranslatedFunctionRegistry::Register(TranslatedFunctionInfo info) {}");
        harness.AppendLine("CpuContext& GetPersistentCpuContext() { static CpuContext ctx; return ctx; }");

        harness.AppendLine("extern \"C\" void float_passthrough(CpuContext* ctx);");
        harness.AppendLine("int main() {");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    ctx.gpr[3] = 123; // ensure we don't accidentally read gpr for float args");
        harness.AppendLine("    ctx.fpr[1].d = 3.5;");
        harness.AppendLine("    float_passthrough(&ctx);");
        harness.AppendLine("    if (std::fabs(ctx.fpr[1].d - 3.5) > 0.0001) { return 1; }");
        harness.AppendLine("    std::cout << ctx.fpr[1].d;");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");
        File.WriteAllText(harnessPath, harness.ToString());

        var exePath = Path.Combine(tempDir, "float_test.out");
        var gpp = new ProcessStartInfo
        {
            FileName = "g++",
            ArgumentList =
            {
                "-std=c++20",
                "-Wall",
                "-Werror",
                "-Wno-error=unused-but-set-variable",
                "-I", Path.Combine(root, "runtime", "include"),
                genPath,
                harnessPath,
                "-o", exePath
            },
            RedirectStandardError = true,
            RedirectStandardOutput = true
        };

        using (var proc = Process.Start(gpp)!)
        {
            proc.WaitForExit(20000);
            if (proc.ExitCode != 0)
            {
                var output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
                throw new InvalidOperationException($"g++ failed: {output}");
            }
        }

        var run = new ProcessStartInfo
        {
            FileName = exePath,
            RedirectStandardOutput = true
        };
        using var runProc = Process.Start(run)!;
        runProc.WaitForExit(5000);
        var outputText = runProc.StandardOutput.ReadToEnd().Trim();

        Assert.Equal(0, runProc.ExitCode);
        Assert.Equal("3.5", outputText);
    }

    [Fact]
    public void Bridge_SimpleIntegerPassThrough()
    {
        var function = new IrFunction(
            "simple_int",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"),
                    new IrReturn(IrValue.Register("r3"))
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32
        });
        var signature = new FunctionAbiClassification("simple_int", ValueRepresentation.Int32);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80003000, ssa, signature, types);

        var root = Translator.Core.Loading.ProjectPaths.FindRepositoryRoot();
        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_codegen_int_tests");
        Directory.CreateDirectory(tempDir);

        var genPath = Path.Combine(tempDir, "generated_int.cpp");
        var harnessPath = Path.Combine(tempDir, "harness_int.cpp");
        File.WriteAllText(genPath, code);

        var harness = new StringBuilder();
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("#include \"abi_bridge.h\"");
        harness.AppendLine("// Mocks for runtime symbols");
        harness.AppendLine("void TranslatedFunctionRegistry::Register(TranslatedFunctionInfo info) {}");
        harness.AppendLine("CpuContext& GetPersistentCpuContext() { static CpuContext ctx; return ctx; }");

        harness.AppendLine("extern \"C\" void simple_int(CpuContext* ctx);");
        harness.AppendLine("int main() {");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    ctx.gpr[3] = 10;");
        harness.AppendLine("    simple_int(&ctx);");
        harness.AppendLine("    // Should be 11");
        harness.AppendLine("    if (ctx.gpr[3] != 11) { return 1; }");
        harness.AppendLine("    std::cout << ctx.gpr[3];");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");
        File.WriteAllText(harnessPath, harness.ToString());

        var exePath = Path.Combine(tempDir, "int_test.out");
        var gpp = new ProcessStartInfo
        {
            FileName = "g++",
            ArgumentList =
            {
                "-std=c++20",
                "-Wall",
                "-Werror",
                "-Wno-error=unused-but-set-variable",
                "-I", Path.Combine(root, "runtime", "include"),
                genPath,
                harnessPath,
                "-o", exePath
            },
            RedirectStandardError = true,
            RedirectStandardOutput = true
        };

        using (var proc = Process.Start(gpp)!)
        {
            proc.WaitForExit(20000);
            if (proc.ExitCode != 0)
            {
                var output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
                throw new InvalidOperationException($"g++ failed: {output}");
            }
        }

        var run = new ProcessStartInfo
        {
            FileName = exePath,
            RedirectStandardOutput = true
        };
        using var runProc = Process.Start(run)!;
        runProc.WaitForExit(5000);
        var outputText = runProc.StandardOutput.ReadToEnd().Trim();

        Assert.Equal(0, runProc.ExitCode);
        Assert.Equal("11", outputText);
    }

    [Fact]
    public void CodeGenerator_EmitsDirectRegisterAccessWithinBasicBlock()
    {
        var function = new IrFunction(
            "cache_test",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                    new IrBinary("r6", IrValue.Register("r3"), IrValue.Register("r5"), "add"),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32,
            ["r4"] = ValueRepresentation.Int32,
            ["r5"] = ValueRepresentation.Int32,
            ["r6"] = ValueRepresentation.Int32
        });
        var signature = new FunctionAbiClassification("cache_test", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80004000, ssa, signature, types);

        Assert.DoesNotContain("_cached_", code);
        Assert.Contains("ctx->gpr[5] = (ctx->gpr[3] + ctx->gpr[4]);", code);
        Assert.Contains("ctx->gpr[6] = (ctx->gpr[3] + ctx->gpr[5]);", code);
    }

    [Fact]
    public void CodeGenerator_UsesPowerPcDivisionHelpers()
    {
        var function = new IrFunction(
            "div_test",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "div"),
                    new IrBinary("r5", IrValue.Register("r5"), IrValue.Register("r6"), "divu"),
                    new IrReturn(IrValue.Register("r3"))
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.Int32,
            ["r4"] = ValueRepresentation.Int32,
            ["r5"] = ValueRepresentation.Int32,
            ["r6"] = ValueRepresentation.Int32
        });
        var signature = new FunctionAbiClassification("div_test", ValueRepresentation.Int32);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80005000, ssa, signature, types);

        Assert.Contains("PPC_Divw(", code, StringComparison.Ordinal);
        Assert.Contains("PPC_Divwu(", code, StringComparison.Ordinal);
        Assert.DoesNotContain("? static_cast<int32_t>", code, StringComparison.Ordinal);
        Assert.DoesNotContain("? static_cast<uint32_t>", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_NormalizesMixedPairedStateAcrossMergeEdges()
    {
        var function = new IrFunction(
            "mixed_ps_merge",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBranch("beq", "paired", "scalar")
                }),
                new IrBasicBlock("paired", new IrInstruction[]
                {
                    new IrCall("f5", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrJump("merge")
                }),
                new IrBasicBlock("scalar", new IrInstruction[]
                {
                    new IrAssign("f5", IrValue.Register("f3")),
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
        var signature = new FunctionAbiClassification("mixed_ps_merge", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80006000, ssa, signature, types);

        Assert.Contains("ctx->fpr[5].d = PPC_PsToScalarInline(ctx->fpr[5].d);", code, StringComparison.Ordinal);
        Assert.Contains("goto loc_merge;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_ClearsImplicitF1PairedStateAfterGuestCallForFlowAnalysis()
    {
        var function = new IrFunction(
            "guest_call_scalar_return",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsMul", new[] { IrValue.Register("f2"), IrValue.Register("f3") }),
                    new IrCall(string.Empty, "func_80001234", Array.Empty<IrValue>()),
                    new IrAssign("f31", IrValue.Register("f1")),
                    new IrJump("merge")
                }),
                new IrBasicBlock("merge", new IrInstruction[]
                {
                    new IrCall(string.Empty, "PPC_Fcmp", new[] { IrValue.Imm(0), IrValue.Register("f31"), IrValue.Register("f4") }),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f4"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("guest_call_scalar_return", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80006100, ssa, signature, types);

        Assert.Contains("InvokeDirectCpu<0x80001234u>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("ctx->fpr[31].d = ctx->fpr[1].d;", code, StringComparison.Ordinal);
        Assert.Contains("PPC_Fcmp(0, ctx->fpr[31].d, ctx->fpr[4].d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_Fcmp(0, PPC_PsToScalarInline(ctx->fpr[31].d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_NormalizesProvidedPairedScalarFloatReturnAfterGuestCall()
    {
        var function = new IrFunction(
            "guest_call_paired_scalar_return",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall("f1", "PPC_PsMul", new[] { IrValue.Register("f2"), IrValue.Register("f3") }),
                    new IrCall(string.Empty, "func_8019ADE0", Array.Empty<IrValue>()),
                    new IrAssign("f31", IrValue.Register("f1")),
                    new IrCall(string.Empty, "PPC_Fcmp", new[] { IrValue.Imm(0), IrValue.Register("f31"), IrValue.Register("f4") }),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f4"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("guest_call_paired_scalar_return", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator(
            new FixedGuestAbiProvider(
                ("func_8019ADE0", new GuestFunctionAbi(returnsPairedScalarFloat: true))))
            .Emit(0x80006108, ssa, signature, types);

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8019ADE0u>(ctx);", StringComparison.Ordinal);
        var normalizeIndex = code.IndexOf("ctx->fpr[1].d = PPC_PsToScalarInline(ctx->fpr[1].d);", StringComparison.Ordinal);
        var assignIndex = code.IndexOf("ctx->fpr[31].d = ctx->fpr[1].d;", StringComparison.Ordinal);

        Assert.True(callIndex >= 0, code);
        Assert.True(normalizeIndex > callIndex, code);
        Assert.True(assignIndex > normalizeIndex, code);
        Assert.Contains("PPC_Fcmp(0, ctx->fpr[31].d, ctx->fpr[4].d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_Fcmp(0, PPC_PsToScalarInline(ctx->fpr[31].d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_NormalizesPairedAbiFloatArgumentsBeforeGuestCall()
    {
        var function = new IrFunction(
            "guest_call_float_arg",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall("f29", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrAssign("f3", IrValue.Register("f29")),
                    new IrCall(string.Empty, "func_807D5B38", new[] { IrValue.Register("f3") }),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f29"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("guest_call_float_arg", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator(
            new FixedGuestAbiProvider(
                ("func_807D5B38", new GuestFunctionAbi(new[] { "f3" }))))
            .Emit(0x80006110, ssa, signature, types);

        Assert.Contains("ctx->fpr[3].d = ctx->fpr[29].d;", code, StringComparison.Ordinal);
        Assert.Contains("ctx->fpr[3].d = PPC_PsToScalarInline(ctx->fpr[3].d);", code, StringComparison.Ordinal);
        Assert.Contains("InvokeDirectCpu<0x807D5B38u>(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_PreservesPairedAbiFloatRegistersForPointerOnlyGuestCall()
    {
        var function = new IrFunction(
            "guest_call_pointer_only",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall("f3", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrCall(string.Empty, "func_8019ACCC", new[] { IrValue.Register("f3") }),
                    new IrCall("f4", "PPC_PsNeg", new[] { IrValue.Register("f3") }),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f4"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("guest_call_pointer_only", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80006120, ssa, signature, types);

        Assert.Contains("InvokeDirectCpu<0x8019ACCCu>(ctx);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->fpr[3].d = PPC_PsToScalarInline(ctx->fpr[3].d);", code, StringComparison.Ordinal);
        Assert.Contains("ctx->fpr[4].d = PPC_PsNegInline(ctx->fpr[3].d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsFromScalarInline(ctx->fpr[3].d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_CachesContextRegistersForLeafFunctionsWhenEnabled()
    {
        var function = new IrFunction(
            "leaf_register_cache",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                    new IrCall("f1", "PPC_PsMul", new[] { IrValue.Register("f2"), IrValue.Register("f3") }),
                    new IrStore(new IrAddress("r3", 4), IrValue.Register("f1"), 8),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("leaf_register_cache", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006200,
            ssa,
            signature,
            types);

        Assert.Contains("uint32_t cached_r3 = ctx->gpr[3];", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t cached_r4 = ctx->gpr[4];", code, StringComparison.Ordinal);
        Assert.Contains("double cached_f1 = 0.0;", code, StringComparison.Ordinal);
        Assert.Contains("cached_r3 = (cached_r3 + cached_r4);", code, StringComparison.Ordinal);
        Assert.Contains("cached_f1 = PPC_PsMulInline(PPC_PsFromScalarInline(cached_f2), PPC_PsFromScalarInline(cached_f3));", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((cached_r3 + 4), cached_f1);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[4] = cached_r4;", code, StringComparison.Ordinal);

        var flushIndex = code.IndexOf("ctx->gpr[3] = cached_r3;", StringComparison.Ordinal);
        var returnIndex = code.IndexOf("    return;", flushIndex, StringComparison.Ordinal);
        Assert.True(flushIndex >= 0, code);
        Assert.True(returnIndex > flushIndex, code);
    }

    [Fact]
    public void CodeGenerator_KeepsEntryLoadWhenReturnFlushCanObserveUnwrittenPath()
    {
        var function = new IrFunction(
            "conditional_register_cache_initializer",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBranch("bne", "write", "exit", "cr0")
                }),
                new IrBasicBlock("write", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r4"), IrValue.Imm(1), "add"),
                    new IrReturn(null)
                }),
                new IrBasicBlock("exit", new IrInstruction[]
                {
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["cr0"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("conditional_register_cache_initializer", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006204,
            ssa,
            signature,
            types);

        Assert.Contains("uint32_t cached_r3 = ctx->gpr[3];", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t cached_r4 = ctx->gpr[4];", code, StringComparison.Ordinal);
    }
    [Fact]
    public void CodeGenerator_DoesNotElideDirectOnlyFprSaveAfterRegisterModification()
    {
        var function = new IrFunction(
            "modified_direct_fpr_save_no_elision",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-64), "add"),
                    new IrCall("f31", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrStore(new IrAddress("r1", 40), IrValue.Register("f31"), 8),
                    new IrCall("f31", "PPC_PsAdd", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrLoad("f31", new IrAddress("r1", 40), 8),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("modified_direct_fpr_save_no_elision", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006223,
            new SsaTransformer().Convert(function),
            signature,
            types);

        Assert.Contains("MemoryInline::FlatWriteRamFloat64((cached_r1 + 40), cached_f31);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat64((cached_r1 + 40))", code, StringComparison.Ordinal);
        Assert.DoesNotContain("leaf_stack_saved_f31_entry", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_DoesNotElideFprStackRestoresAcrossGuestCalls()
    {
        var function = new IrFunction(
            "nonleaf_fpr_restore_no_elision",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-128), "add"),
                    new IrStore(new IrAddress("r1", 112), IrValue.Register("f31"), 8),
                    new IrBinary("tmp_psq_store", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("tmp_psq_store"), IrValue.Register("f31"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrCall(string.Empty, "func_80001234", Array.Empty<IrValue>()),
                    new IrBinary("tmp_psq_load", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall("f31", "PPC_PsqL", new[] { IrValue.Register("tmp_psq_load"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrLoad("f31", new IrAddress("r1", 112), 8),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["f31"] = ValueRepresentation.Float64,
            ["tmp_psq_store"] = ValueRepresentation.UInt32,
            ["tmp_psq_load"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("nonleaf_fpr_restore_no_elision", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006224,
            ssa,
            signature,
            types);

        Assert.Contains("InvokeDirectCpu<0x80001234u>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqStInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsqLInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat64((cached_r1 + 112))", code, StringComparison.Ordinal);
        Assert.Contains("tmp_psq_store", code, StringComparison.Ordinal);
        Assert.DoesNotContain("tmp_psq_load", code, StringComparison.Ordinal);
        Assert.DoesNotContain("leaf_stack_saved_f31_entry", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_DoesNotElidePsqStackLoadWhenValueIsUsedBeforeScalarRestore()
    {
        var function = new IrFunction(
            "used_psq_restore_no_elision",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-128), "add"),
                    new IrStore(new IrAddress("r1", 112), IrValue.Register("f31"), 8),
                    new IrBinary("tmp_psq_store", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("tmp_psq_store"), IrValue.Register("f31"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrCall(string.Empty, "func_80001234", Array.Empty<IrValue>()),
                    new IrBinary("tmp_psq_load", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall("f31", "PPC_PsqL", new[] { IrValue.Register("tmp_psq_load"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrStore(new IrAddress("r1", 40), IrValue.Register("f31"), 8),
                    new IrLoad("f31", new IrAddress("r1", 112), 8),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["f31"] = ValueRepresentation.Float64,
            ["tmp_psq_store"] = ValueRepresentation.UInt32,
            ["tmp_psq_load"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("used_psq_restore_no_elision", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006226,
            ssa,
            signature,
            types);

        Assert.Contains("PPC_PsqLInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((cached_r1 + 40), cached_f31);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat64((cached_r1 + 112))", code, StringComparison.Ordinal);
        Assert.DoesNotContain("leaf_stack_saved_f31_entry", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_DoesNotElideFprStackRestoresWhenSlotIsOtherwiseAccessed()
    {
        var function = new IrFunction(
            "overlapping_fpr_restore_no_elision",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-128), "add"),
                    new IrStore(new IrAddress("r1", 112), IrValue.Register("f31"), 8),
                    new IrBinary("tmp_psq_store", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("tmp_psq_store"), IrValue.Register("f31"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrLoad("f1", new IrAddress("r1", 116), 4),
                    new IrBinary("tmp_psq_load", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall("f31", "PPC_PsqL", new[] { IrValue.Register("tmp_psq_load"), IrValue.Imm(0), IrValue.Imm(0) }),
                    new IrLoad("f31", new IrAddress("r1", 112), 8),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64,
            ["tmp_psq_store"] = ValueRepresentation.UInt32,
            ["tmp_psq_load"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("overlapping_fpr_restore_no_elision", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006228,
            ssa,
            signature,
            types);

        Assert.DoesNotContain("PPC_PsqLInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat32((cached_r1 + 116))", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat64((cached_r1 + 112))", code, StringComparison.Ordinal);
        Assert.DoesNotContain("leaf_stack_saved_f31_entry", code, StringComparison.Ordinal);
    }

    [Fact]
    public void RuntimeNativeAbiProvider_LoadsTypedVoidStubsButSkipsCpuContextStubs()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), "mkw_native_abi_tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempDir);
        File.WriteAllText(
            Path.Combine(tempDir, "gx_test.cpp"),
            """
            PPC_NATIVE_OVERRIDE_VOID(801733b4, GX__SetViewport_801733b4, (float l, uint32_t t, uint32_t out), (l, t, out));
            PPC_NATIVE_OVERRIDE_VOID(801A0870, OSSetAlarm_HLE_801a0870, (CpuContext* ctx), (ctx));
            """);

        var provider = RuntimeNativeFunctionAbiProvider.LoadVoidStubAbisFromDirectory(tempDir);

        Assert.True(provider.TryGetGuestFunctionAbi("func_801733B4", out var abi));
        Assert.True(abi.PreservesVolatileContext);
        Assert.False(abi.WritesGprReturnRegister);
        Assert.True(abi.HasScalarFloatArgument("f1"));
        Assert.True(abi.HasArgumentRegister("f1"));
        Assert.True(abi.HasArgumentRegister("r3"));
        Assert.True(abi.HasArgumentRegister("r4"));
        Assert.False(provider.TryGetGuestFunctionAbi("func_801A0870", out _));
    }

    [Fact]
    public void CodeGenerator_EmitsForwardDeclarationsAndAliasStub()
    {
        var function = new IrFunction(
            "named-call-target",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "func_helper_target", Array.Empty<IrValue>()),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment();
        var signature = new FunctionAbiClassification("named-call-target", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80007000, ssa, signature, types);

        Assert.Contains("extern \"C\" void func_helper_target(CpuContext* ctx);", code, StringComparison.Ordinal);
        Assert.Contains("extern \"C\" void func_80007000(CpuContext* MKW_RESTRICT ctx)", code, StringComparison.Ordinal);
        Assert.Contains("named_call_target(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_EmitsUndefinedJumpTableAndImplicitReturnPaths()
    {
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32,
            ["cr0"] = ValueRepresentation.UInt32,
            ["f5"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("coverage_codegen", ValueRepresentation.Void);

        var undefinedFunction = new IrFunction(
            "undefined_case",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrUndefined(0x80008000, 0x7C000008, "trap for coverage")
                })
            });
        var undefinedCode = new CxxLinearCodeGenerator().Emit(
            0x80008000,
            new SsaTransformer().Convert(undefinedFunction),
            signature,
            types,
            );
        Assert.Contains("UNDEFINED(0x80008000u, 0x7C000008u, \"trap for coverage\");", undefinedCode, StringComparison.Ordinal);
        Assert.Contains("return;", undefinedCode, StringComparison.Ordinal);

        var jumpTableFunction = new IrFunction(
            "jump_table_case",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrJumpTable("r3", new[]
                    {
                        new IrJumpTableCase(0x80008100, "case_a"),
                        new IrJumpTableCase(0x80008110, "case_b")
                    })
                }),
                new IrBasicBlock("case_a", new IrInstruction[] { new IrReturn(null) }),
                new IrBasicBlock("case_b", new IrInstruction[] { new IrReturn(null) })
            });
        var jumpTableCode = new CxxLinearCodeGenerator().Emit(
            0x80008010,
            new SsaTransformer().Convert(jumpTableFunction),
            signature,
            types,
            );
        Assert.Contains("switch (static_cast<uint32_t>(ctx->gpr[3]))", jumpTableCode, StringComparison.Ordinal);
        Assert.Contains("case 0x80008100u:", jumpTableCode, StringComparison.Ordinal);
        Assert.Contains("InvokeIndirectJump(ctx->gpr[3], ctx);", jumpTableCode, StringComparison.Ordinal);

        var implicitReturnFunction = new IrFunction(
            "implicit_return_case",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrAssign("tmp_local", IrValue.Imm(1))
                })
            });
        var implicitReturnCode = new CxxLinearCodeGenerator().Emit(
            0x80008020,
            new SsaTransformer().Convert(implicitReturnFunction),
            signature,
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation> { ["tmp_local"] = ValueRepresentation.Int32 }),
            );
        Assert.DoesNotContain("tmp_local_0 = 1;", implicitReturnCode, StringComparison.Ordinal);
        Assert.Contains("return;", implicitReturnCode, StringComparison.Ordinal);
    }

    [Fact]
    public void CodeGenerator_NormalizesScalarInputsToPairedAndParsesInvalidFloatRegisters()
    {
        var normalizeMethod = typeof(CxxLinearCodeGenerator).GetMethod(
            "EmitNormalizePairedStateOnEdge",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(normalizeMethod);

        var sb = new StringBuilder();
        normalizeMethod!.Invoke(
            null,
            new object?[]
            {
                sb,
                "    ",
                "from",
                "to",
                new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase)
                {
                    ["from"] = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
                },
                new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase)
                {
                    ["to"] = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "f5" }
                }
            });

        Assert.Contains("ctx->fpr[5].d = PPC_PsFromScalarInline(ctx->fpr[5].d);", sb.ToString(), StringComparison.Ordinal);

        var parseMethod = typeof(CxxLinearCodeGenerator).GetMethod("ParseFloatRegisterIndex", BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(parseMethod);
        Assert.Equal(int.MaxValue, (int)parseMethod!.Invoke(null, new object?[] { "not_a_float_reg" })!);
    }
}
