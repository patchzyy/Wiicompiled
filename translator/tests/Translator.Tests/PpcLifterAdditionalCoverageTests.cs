using System;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class PpcLifterAdditionalCoverageTests
{
    private static PpcRegisterOperand Gpr(int index) => new($"r{index}", index);

    private static PpcInstruction Instruction(uint raw, string mnemonic, params PpcOperand[] operands)
        => PpcInstruction.Synthetic(0x80000000, raw, mnemonic, operands);

    [Fact]
    public void LinkedConditionalBranchSetsLrBeforeEitherPath()
    {
        var branch = PpcDecoder.Decode(0x80004394, 0x41800029);
        var ir = Assert.Single(new PpcLifter().Lift(new[] { branch })).Ir;

        Assert.Equal("bltl", branch.Mnemonic);
        Assert.Equal(2, ir.Count);

        var lrAssign = Assert.IsType<IrAssign>(ir[0]);
        Assert.Equal("lr", lrAssign.Destination);
        Assert.Equal(unchecked((int)0x80004398u), lrAssign.Value.Constant);
        Assert.Equal(unchecked((int)branch.EndAddress), lrAssign.Value.Constant);

        var decision = Assert.IsType<IrBranch>(ir[1]);
        Assert.Equal("raw", decision.Condition);
        Assert.Equal("link_branch_80004398_800043BC", decision.TrueLabel);
        Assert.Equal("0x80004398", decision.FalseLabel);

        Assert.Equal($"link_branch_{branch.EndAddress:X8}_{branch.BranchTargets.First():X8}", decision.TrueLabel);
        Assert.Equal($"0x{branch.EndAddress:X8}", decision.FalseLabel);
    }

    [Fact]
    public void LiftsAdditionalNonDotArithmeticAndLogicalForms()
    {
        var lifter = new PpcLifter();

        var orc = Instruction(0, "orc", Gpr(3), Gpr(4), Gpr(5));
        var orcIr = Assert.Single(lifter.Lift(new[] { orc })).Ir;
        Assert.Equal("orc", Assert.IsType<IrBinary>(Assert.Single(orcIr)).Op);

        var addc = Instruction(0, "addc", Gpr(6), Gpr(6), Gpr(7));
        var addcIr = Assert.Single(lifter.Lift(new[] { addc })).Ir;
        Assert.Equal("r6_addc_left", Assert.IsType<IrAssign>(addcIr[0]).Destination);
        Assert.DoesNotContain(addcIr, ins => ins is IrSetCrField);

        var subfc = Instruction(0, "subfc", Gpr(8), Gpr(9), Gpr(8));
        var subfcIr = Assert.Single(lifter.Lift(new[] { subfc })).Ir;
        Assert.Equal("r8_subfc_min", Assert.IsType<IrAssign>(subfcIr[0]).Destination);
        Assert.DoesNotContain(subfcIr, ins => ins is IrSetCrField);

        var extsb = Instruction(0, "extsb", Gpr(10), Gpr(11));
        var extsbBinary = Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { extsb })).Ir));
        Assert.Equal("sext", extsbBinary.Op);
        Assert.Equal(8, extsbBinary.Right.Constant);

        var adde = Instruction(0, "adde", Gpr(12), Gpr(12), Gpr(13));
        var addeIr = Assert.Single(lifter.Lift(new[] { adde })).Ir;
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(addeIr[1]).Target);
        Assert.DoesNotContain(addeIr, ins => ins is IrSetCrField);

        var subfze = Instruction(0, "subfze", Gpr(14), Gpr(14));
        var subfzeIr = Assert.Single(lifter.Lift(new[] { subfze })).Ir;
        Assert.Equal("r14_not", Assert.IsType<IrBinary>(subfzeIr[1]).Destination);
        Assert.DoesNotContain(subfzeIr, ins => ins is IrSetCrField);

        var cmpwTwoOperand = Instruction(0, "cmpw", Gpr(15), Gpr(16));
        var cmpwIr = Assert.Single(lifter.Lift(new[] { cmpwTwoOperand })).Ir;
        var cmpwSetCr = Assert.IsType<IrSetCrField>(Assert.Single(cmpwIr));
        Assert.Equal(0, cmpwSetCr.FieldIndex);
        Assert.False(cmpwSetCr.IsUnsigned);
        Assert.Equal("r15", cmpwSetCr.Left.RegisterName);
        Assert.Equal("r16", cmpwSetCr.Right.RegisterName);

        var nand = Instruction(0, "nand", Gpr(17), Gpr(18), Gpr(19));
        Assert.Equal("nand", Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { nand })).Ir)).Op);

        var eqv = Instruction(0, "eqv.", Gpr(17), Gpr(18), Gpr(19));
        var eqvIr = Assert.Single(lifter.Lift(new[] { eqv })).Ir;
        Assert.Equal("eqv", Assert.IsType<IrBinary>(eqvIr[0]).Op);
        Assert.IsType<IrSetCrField>(eqvIr[1]);

        var extsh = Instruction(0, "extsh", Gpr(20), Gpr(21));
        var extshBinary = Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { extsh })).Ir));
        Assert.Equal("sext", extshBinary.Op);
        Assert.Equal(16, extshBinary.Right.Constant);

        var divw = Instruction(0, "divw", Gpr(22), Gpr(23), Gpr(24));
        Assert.Equal("div", Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { divw })).Ir)).Op);

        var slwi = Instruction(0, "slwi", Gpr(25), Gpr(26), new PpcImmediateOperand(3));
        Assert.Equal("shl", Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { slwi })).Ir)).Op);

        var addic = Instruction(0, "addic", Gpr(27), Gpr(27), new PpcImmediateOperand(4));
        var addicIr = Assert.Single(lifter.Lift(new[] { addic })).Ir;
        Assert.Equal("r27_addic_src", Assert.IsType<IrAssign>(addicIr[0]).Destination);
        Assert.DoesNotContain(addicIr, ins => ins is IrSetCrField);
    }

    [Fact]
    public void LiftsAdditionalMemoryAndControlFormsAndRejectsInvalidUpdateBases()
    {
        var lifter = new PpcLifter();

        var dcbzL = Instruction(0, "dcbz_l", Gpr(3), Gpr(4));
        var dcbzLIr = Assert.Single(lifter.Lift(new[] { dcbzL })).Ir;
        Assert.Equal("r4_addr_dcbz", Assert.IsType<IrBinary>(dcbzLIr[0]).Destination);
        Assert.Equal("memset_zero_32", Assert.IsType<IrCall>(dcbzLIr[2]).Target);

        var rfi = Instruction(0, "rfi");
        Assert.IsType<IrReturn>(Assert.Single(Assert.Single(lifter.Lift(new[] { rfi })).Ir));

        var lfdu = Instruction(0, "lfdu", new PpcRegisterOperand("f1", 1), new PpcDisplacementOperand(8, "r2", 2));
        var lfduIr = Assert.Single(lifter.Lift(new[] { lfdu })).Ir;
        Assert.Equal("r2_addr", Assert.IsType<IrBinary>(lfduIr[0]).Destination);
        Assert.Equal("f1", Assert.IsType<IrLoad>(lfduIr[1]).Destination);
        Assert.Equal("r2", Assert.IsType<IrAssign>(lfduIr[2]).Destination);

        var invalidLfdu = Instruction(0, "lfdu", new PpcRegisterOperand("f1", 1), new PpcDisplacementOperand(8, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLfdu }));

        var invalidLfsu = Instruction(0, "lfsu", new PpcRegisterOperand("f2", 2), new PpcDisplacementOperand(4, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLfsu }));

        var invalidLhzu = Instruction(0, "lhzu", Gpr(3), new PpcDisplacementOperand(2, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLhzu }));

        var invalidStbu = Instruction(0, "stbu", Gpr(4), new PpcDisplacementOperand(1, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidStbu }));

        var invalidSthu = Instruction(0, "sthu", Gpr(5), new PpcDisplacementOperand(2, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidSthu }));
    }

    [Fact]
    public void LiftsLegacyOpcodeAliasesAndR0StoreForms()
    {
        var lifter = new PpcLifter();

        var andis = Instruction((3u << 21) | (4u << 16) | 0x1234u, "opc_29");
        var andisIr = Assert.Single(lifter.Lift(new[] { andis })).Ir;
        var andisBinary = Assert.IsType<IrBinary>(andisIr[0]);
        Assert.Equal("r4", andisBinary.Destination);
        Assert.Equal("and", andisBinary.Op);
        Assert.IsType<IrSetCrField>(andisIr[1]);

        var legacySthu = Instruction((5u << 21) | (6u << 16) | 0x0002u, "opc_45");
        var legacySthuIr = Assert.Single(lifter.Lift(new[] { legacySthu })).Ir;
        Assert.Equal("r6_sthu_ea", Assert.IsType<IrBinary>(legacySthuIr[0]).Destination);
        Assert.Equal("r6", Assert.IsType<IrAssign>(legacySthuIr[2]).Destination);

        var stwR0 = Instruction(0, "stw", Gpr(7), new PpcDisplacementOperand(12, "r0", 0));
        var stwR0Ir = Assert.Single(lifter.Lift(new[] { stwR0 })).Ir;
        Assert.Collection(
            stwR0Ir,
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("r7_ea", assign.Destination);
                Assert.Equal(12, assign.Value.Constant);
            },
            ins =>
            {
                var store = Assert.IsType<IrStore>(ins);
                Assert.Equal("r7_ea", store.Address.Base);
            });
    }

    [Fact]
    public void LiftsAdditionalIndexedFloatingAndRotateMaskForms()
    {
        var lifter = new PpcLifter();

        var bgtlr = Instruction(0, "bgtlr");
        var bgtlrBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bgtlr })).Ir));
        Assert.Equal("bgt", bgtlrBranch.Condition);
        Assert.Equal("return", bgtlrBranch.TrueLabel);

        var sc = Instruction(0, "sc");
        Assert.Equal("OSSystemCall", Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { sc })).Ir)).Target);

        var tw = Instruction((31u << 21) | (4u << 16) | (5u << 11), "tw");
        var trap = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { tw })).Ir));
        Assert.Equal("PPC_TrapWord", trap.Target);
        Assert.Equal(31, trap.Arguments[0].Constant);
        Assert.Equal("r4", trap.Arguments[1].RegisterName);
        Assert.Equal("r5", trap.Arguments[2].RegisterName);

        var twi = Instruction((12u << 21) | (6u << 16) | 0xFFFEu, "twi");
        var immediateTrap = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { twi })).Ir));
        Assert.Equal("PPC_TrapWord", immediateTrap.Target);
        Assert.Equal(12, immediateTrap.Arguments[0].Constant);
        Assert.Equal("r6", immediateTrap.Arguments[1].RegisterName);
        Assert.Equal(-2, immediateTrap.Arguments[2].Constant);

        var addis = Instruction(0, "addis", Gpr(3), Gpr(0), new PpcImmediateOperand(0x1234));
        var addisBinary = Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { addis })).Ir));
        Assert.Equal("add", addisBinary.Op);
        Assert.Equal(0x12340000, addisBinary.Right.Constant);
        Assert.Equal(0, addisBinary.Left.Constant);

        var lfdx = Instruction((1u << 21) | (2u << 16) | (3u << 11), "lfdx");
        var lfdxIr = Assert.Single(lifter.Lift(new[] { lfdx })).Ir;
        Assert.Equal("addr_lfdx_80000000_loc", Assert.IsType<IrBinary>(lfdxIr[0]).Destination);
        Assert.Equal("f1", Assert.IsType<IrLoad>(lfdxIr[1]).Destination);

        var lfsx = Instruction((4u << 21) | (5u << 16) | (6u << 11), "lfsx");
        var lfsxIr = Assert.Single(lifter.Lift(new[] { lfsx })).Ir;
        Assert.Equal("f4", Assert.IsType<IrLoad>(lfsxIr[1]).Destination);

        var lfsux = Instruction((7u << 21) | (8u << 16) | (9u << 11), "lfsux");
        var lfsuxIr = Assert.Single(lifter.Lift(new[] { lfsux })).Ir;
        Assert.Equal("addr_lfsux_80000000_loc", Assert.IsType<IrBinary>(lfsuxIr[0]).Destination);
        Assert.Equal("r8", Assert.IsType<IrAssign>(lfsuxIr[2]).Destination);
        var badLfsux = Instruction((7u << 21) | (0u << 16) | (9u << 11), "lfsux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { badLfsux }));

        var lfdux = Instruction((7u << 21) | (8u << 16) | (9u << 11), "lfdux");
        var lfduxIr = Assert.Single(lifter.Lift(new[] { lfdux })).Ir;
        Assert.Equal("addr_lfdux_80000000_loc", Assert.IsType<IrBinary>(lfduxIr[0]).Destination);
        Assert.Equal("f7", Assert.IsType<IrLoad>(lfduxIr[1]).Destination);
        Assert.Equal("r8", Assert.IsType<IrAssign>(lfduxIr[2]).Destination);
        var badLfdux = Instruction((7u << 21) | (0u << 16) | (9u << 11), "lfdux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { badLfdux }));

        var stfdx = Instruction((10u << 21) | (11u << 16) | (12u << 11), "stfdx");
        var stfdxStore = Assert.IsType<IrStore>(Assert.Single(Assert.Single(lifter.Lift(new[] { stfdx })).Ir.Skip(1)));
        Assert.Equal("f10", stfdxStore.Source.RegisterName);

        var stfsx = Instruction((13u << 21) | (14u << 16) | (15u << 11), "stfsx");
        var stfsxStore = Assert.IsType<IrStore>(Assert.Single(Assert.Single(lifter.Lift(new[] { stfsx })).Ir.Skip(1)));
        Assert.Equal("f13", stfsxStore.Source.RegisterName);

        var stfsux = Instruction((16u << 21) | (17u << 16) | (18u << 11), "stfsux");
        var stfsuxIr = Assert.Single(lifter.Lift(new[] { stfsux })).Ir;
        Assert.Equal("r17", Assert.IsType<IrBinary>(stfsuxIr[0]).Destination);
        Assert.Equal("f16", Assert.IsType<IrStore>(stfsuxIr[1]).Source.RegisterName);
        var badStfsux = Instruction((16u << 21) | (0u << 16) | (18u << 11), "stfsux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { badStfsux }));

        var stfdux = Instruction((16u << 21) | (17u << 16) | (18u << 11), "stfdux");
        var stfduxIr = Assert.Single(lifter.Lift(new[] { stfdux })).Ir;
        Assert.Equal("addr_stfdux_80000000_loc", Assert.IsType<IrBinary>(stfduxIr[0]).Destination);
        Assert.Equal("f16", Assert.IsType<IrStore>(stfduxIr[1]).Source.RegisterName);
        Assert.Equal("r17", Assert.IsType<IrAssign>(stfduxIr[2]).Destination);
        var badStfdux = Instruction((16u << 21) | (0u << 16) | (18u << 11), "stfdux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { badStfdux }));

        var sthux = Instruction((19u << 21) | (20u << 16) | (21u << 11), "sthux");
        var sthuxIr = Assert.Single(lifter.Lift(new[] { sthux })).Ir;
        Assert.Equal("addr_sthux_80000000_loc", Assert.IsType<IrBinary>(sthuxIr[0]).Destination);
        Assert.Equal("r20", Assert.IsType<IrAssign>(sthuxIr[2]).Destination);
        var badSthux = Instruction((19u << 21) | (0u << 16) | (21u << 11), "sthux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { badSthux }));

        var stfiwx = Instruction((22u << 21) | (23u << 16) | (24u << 11), "stfiwx");
        var stfiwxCall = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { stfiwx })).Ir.Skip(1)));
        Assert.Equal("PPC_Stfiwx", stfiwxCall.Target);
        Assert.Equal("f22", stfiwxCall.Arguments[1].RegisterName);

        var clrlwi = Instruction(0, "clrlwi", Gpr(25), Gpr(26), new PpcImmediateOperand(8));
        var clrlwiIr = Assert.Single(lifter.Lift(new[] { clrlwi })).Ir;
        Assert.Single(clrlwiIr);
        Assert.Equal("and", Assert.IsType<IrBinary>(clrlwiIr[0]).Op);

        var rlwinm = Instruction(0, "rlwinm", Gpr(27), Gpr(28), new PpcImmediateOperand(5), new PpcImmediateOperand(3), new PpcImmediateOperand(7));
        var rlwinmIr = Assert.Single(lifter.Lift(new[] { rlwinm })).Ir;
        Assert.Contains(rlwinmIr, ins => ins is IrBinary binary && binary.Destination == "r27_rot");

        var rlwimi = Instruction(0, "rlwimi", Gpr(29), Gpr(30), new PpcImmediateOperand(4), new PpcImmediateOperand(8), new PpcImmediateOperand(12));
        var rlwimiIr = Assert.Single(lifter.Lift(new[] { rlwimi })).Ir;
        Assert.Contains(rlwimiIr, ins => ins is IrBinary binary && binary.Destination == "r29_mdest");

        var rlwnm = Instruction(0, "rlwnm", Gpr(31), Gpr(1), Gpr(2), new PpcImmediateOperand(4), new PpcImmediateOperand(9));
        var rlwnmIr = Assert.Single(lifter.Lift(new[] { rlwnm })).Ir;
        Assert.Contains(rlwnmIr, ins => ins is IrBinary binary && binary.Destination == "r31_rot");
        Assert.Contains(rlwnmIr, ins => ins is IrBinary binary && binary.Destination == "r31" && binary.Op == "and");
    }

    [Fact]
    public void LiftsAdditionalSprShiftAndControlDotForms()
    {
        var lifter = new PpcLifter();

        var orcDot = Instruction(0, "orc.", Gpr(3), Gpr(4), Gpr(5));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { orcDot })).Ir, ins => ins is IrSetCrField);

        var divwuDot = Instruction(0, "divwu.", Gpr(6), Gpr(7), Gpr(8));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { divwuDot })).Ir, ins => ins is IrSetCrField);

        var rlwnmDot = Instruction(0, "rlwnm.", Gpr(9), Gpr(10), Gpr(11), new PpcImmediateOperand(3), new PpcImmediateOperand(9));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { rlwnmDot })).Ir, ins => ins is IrSetCrField);

        var mfsprHid0 = Instruction(0, "mfspr", Gpr(12), new PpcImmediateOperand(1008));
        Assert.Equal("hid0", Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfsprHid0 })).Ir)).Value.RegisterName);

        var mfsprFallback = Instruction(0, "mfspr", Gpr(13), new PpcImmediateOperand(31));
        var mfsprFallbackIr = Assert.Single(lifter.Lift(new[] { mfsprFallback })).Ir;
        Assert.IsType<IrComment>(mfsprFallbackIr[0]);
        Assert.Equal("PPC_ReadSpr", Assert.IsType<IrCall>(mfsprFallbackIr[1]).Target);

        var mtsprHid1 = Instruction(0, "mtspr", new PpcImmediateOperand(1009), Gpr(14));
        Assert.Equal("hid1", Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mtsprHid1 })).Ir)).Destination);

        var mtsprFallback = Instruction(0, "mtspr", new PpcImmediateOperand(16), Gpr(15));
        var mtsprFallbackIr = Assert.Single(lifter.Lift(new[] { mtsprFallback })).Ir;
        Assert.IsType<IrComment>(mtsprFallbackIr[0]);
        Assert.Equal("PPC_WriteSpr", Assert.IsType<IrCall>(mtsprFallbackIr[1]).Target);

        var mtsr = Instruction(0, "mtsr", new PpcImmediateOperand(3), Gpr(16));
        Assert.IsType<IrComment>(Assert.Single(Assert.Single(lifter.Lift(new[] { mtsr })).Ir));

        var mtsrin = Instruction(0, "mtsrin", Gpr(16), Gpr(17));
        Assert.IsType<IrComment>(Assert.Single(Assert.Single(lifter.Lift(new[] { mtsrin })).Ir));

        var mfsr = Instruction(0, "mfsr", Gpr(17), new PpcImmediateOperand(3));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfsr })).Ir)).Value.Constant);

        var mfsrin = Instruction(0, "mfsrin", Gpr(18), Gpr(19));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfsrin })).Ir)).Value.Constant);

        var mulhwuDot = Instruction(0, "mulhwu.", Gpr(18), Gpr(19), Gpr(20));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { mulhwuDot })).Ir, ins => ins is IrSetCrField);

        var mulhwDot = Instruction(0, "mulhw.", Gpr(21), Gpr(22), Gpr(23));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { mulhwDot })).Ir, ins => ins is IrSetCrField);

        var bdz = Instruction(0, "bdz", new PpcImmediateOperand(unchecked((int)0x80000010)));
        Assert.Equal("bdz", Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bdz })).Ir.Skip(1))).Condition);

        var srwi = Instruction(0, "srwi", Gpr(24), Gpr(25), new PpcImmediateOperand(5));
        Assert.Equal("shr", Assert.IsType<IrBinary>(Assert.Single(Assert.Single(lifter.Lift(new[] { srwi })).Ir)).Op);

        var slwDot = Instruction(0, "slw.", Gpr(26), Gpr(27), Gpr(28));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { slwDot })).Ir, ins => ins is IrSetCrField);

        var andisDot = Instruction(0, "andis.", Gpr(29), Gpr(30), new PpcImmediateOperand(0x1234));
        var andisDotIr = Assert.Single(lifter.Lift(new[] { andisDot })).Ir;
        var andisDotBinary = Assert.IsType<IrBinary>(andisDotIr[0]);
        Assert.Equal(0x12340000, andisDotBinary.Right.Constant);
        Assert.Contains(andisDotIr, ins => ins is IrSetCrField);

        var andcDot = Instruction(0, "andc.", Gpr(31), Gpr(1), Gpr(2));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { andcDot })).Ir, ins => ins is IrSetCrField);

        var xorDot = Instruction(0, "xor.", Gpr(3), Gpr(4), Gpr(5));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { xorDot })).Ir, ins => ins is IrSetCrField);

        var bltlr = Instruction(0, "bltlr");
        Assert.Equal("blt", Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bltlr })).Ir)).Condition);

        var cntlzwDot = Instruction(0, "cntlzw.", Gpr(6), Gpr(7));
        Assert.Contains(Assert.Single(lifter.Lift(new[] { cntlzwDot })).Ir, ins => ins is IrSetCrField);

        var invalidLwzu = Instruction(0, "lwzu", Gpr(8), new PpcDisplacementOperand(4, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLwzu }));

        var bclrl = Instruction((12u << 21) | (8u << 16), "bclrl", new PpcConditionRegisterOperand("cr2", 8));
        var bclrlBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bclrl })).Ir));
        Assert.Contains("call_lr_80000000_80000004", bclrlBranch.TrueLabel, StringComparison.Ordinal);
        Assert.Equal("raw", bclrlBranch.Condition);
        Assert.Contains("GetCRBit(ctx, 2, 0)", bclrlBranch.ConditionRegister, StringComparison.Ordinal);
    }

    [Fact]
    public void LiftsAdditionalIndexedHalfwordAndStoreUpdateForms()
    {
        var lifter = new PpcLifter();

        var stfdu = Instruction(0, "stfdu", new PpcRegisterOperand("f1", 1), new PpcDisplacementOperand(8, "r2", 2));
        var stfduIr = Assert.Single(lifter.Lift(new[] { stfdu })).Ir;
        Assert.Equal("r2", Assert.IsType<IrBinary>(stfduIr[0]).Destination);
        Assert.Equal("f1", Assert.IsType<IrStore>(stfduIr[1]).Source.RegisterName);

        var stfsu = Instruction(0, "stfsu", new PpcRegisterOperand("f3", 3), new PpcDisplacementOperand(4, "r4", 4));
        var stfsuIr = Assert.Single(lifter.Lift(new[] { stfsu })).Ir;
        Assert.Equal("r4", Assert.IsType<IrBinary>(stfsuIr[0]).Destination);
        Assert.Equal("f3", Assert.IsType<IrStore>(stfsuIr[1]).Source.RegisterName);

        var invalidStwux = Instruction(0, "stwux", Gpr(5), Gpr(0), Gpr(6));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidStwux }));

        var invalidLwzux = Instruction(0, "lwzux", Gpr(7), Gpr(0), Gpr(8));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLwzux }));

        var lhzx = Instruction((9u << 21) | (10u << 16) | (11u << 11), "lhzx", Gpr(9), Gpr(10), Gpr(11));
        var lhzxIr = Assert.Single(lifter.Lift(new[] { lhzx })).Ir;
        Assert.Equal("r9", Assert.IsType<IrLoad>(lhzxIr[1]).Destination);

        var lhaux = Instruction((12u << 21) | (13u << 16) | (14u << 11), "lhaux", Gpr(12), Gpr(13), Gpr(14));
        var lhauxIr = Assert.Single(lifter.Lift(new[] { lhaux })).Ir;
        Assert.Equal("r12", Assert.IsType<IrLoad>(lhauxIr[1]).Destination);
        Assert.Equal("r13", Assert.IsType<IrAssign>(lhauxIr[4]).Destination);

        var invalidLhaux = Instruction((12u << 21) | (0u << 16) | (14u << 11), "lhaux", Gpr(12), Gpr(0), Gpr(14));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLhaux }));

        var lhzux = Instruction((15u << 21) | (16u << 16) | (17u << 11), "lhzux", Gpr(15), Gpr(16), Gpr(17));
        var lhzuxIr = Assert.Single(lifter.Lift(new[] { lhzux })).Ir;
        Assert.Equal("r15", Assert.IsType<IrLoad>(lhzuxIr[1]).Destination);
        Assert.Equal("r16", Assert.IsType<IrAssign>(lhzuxIr[2]).Destination);

        var invalidLhzux = Instruction((15u << 21) | (0u << 16) | (17u << 11), "lhzux", Gpr(15), Gpr(0), Gpr(17));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLhzux }));
    }
}
