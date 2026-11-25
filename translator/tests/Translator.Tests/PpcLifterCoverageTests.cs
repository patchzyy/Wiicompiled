using System;
using System.Linq;
using System.Reflection;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class PpcLifterCoverageTests
{
    private static PpcRegisterOperand Gpr(int index) => new($"r{index}", index);
    private static readonly Type LifterType = typeof(PpcLifter);

    private static PpcInstruction Instruction(uint raw, string mnemonic, params PpcOperand[] operands)
        => PpcInstruction.Synthetic(0x80000000, raw, mnemonic, operands);

    private static T InvokePrivate<T>(string name, params object?[] args)
    {
        var method = LifterType.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (T)method!.Invoke(null, args)!;
    }

    [Fact]
    public void NormalizesCrBitStyleConditionRegistersDuringCompare()
    {
        var instruction = Instruction(
            0x7C032800,
            "cmpw",
            new PpcOperand[]
            {
                new PpcConditionRegisterOperand("cr28", 28),
                Gpr(3),
                Gpr(5)
            });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var setCr = Assert.IsType<IrSetCrField>(Assert.Single(ir));
        Assert.Equal(7, setCr.FieldIndex);
        Assert.Equal("r3", setCr.Left.RegisterName);
        Assert.Equal("r5", setCr.Right.RegisterName);
    }

    [Fact]
    public void LiftsXoriDotAndBarrierInstructions()
    {
        var lifter = new PpcLifter();

        var nop = Instruction(0, "nop");
        Assert.Empty(Assert.Single(lifter.Lift(new[] { nop })).Ir);

        var xori = Instruction(0, "xori.", Gpr(4), Gpr(5), new PpcImmediateOperand(-1));
        var xoriIr = Assert.Single(lifter.Lift(new[] { xori })).Ir;
        var xoriBinary = Assert.IsType<IrBinary>(xoriIr[0]);
        Assert.Equal("r4", xoriBinary.Destination);
        Assert.Equal("xor", xoriBinary.Op);
        Assert.Equal(0xFFFF, xoriBinary.Right.Constant);
        Assert.IsType<IrSetCrField>(xoriIr[1]);

        var sync = Instruction(0, "sync");
        var syncIr = Assert.Single(lifter.Lift(new[] { sync })).Ir;
        Assert.Contains("sync", Assert.IsType<IrComment>(Assert.Single(syncIr)).Text, StringComparison.Ordinal);
    }

    [Fact]
    public void LiftsAliasingCarryInstructions()
    {
        var lifter = new PpcLifter();

        var addc = Instruction(0, "addc.", Gpr(3), Gpr(3), Gpr(4));
        var addcIr = Assert.Single(lifter.Lift(new[] { addc })).Ir;
        var addcTemp = Assert.IsType<IrAssign>(addcIr[0]);
        Assert.Equal("r3_addc_left", addcTemp.Destination);
        Assert.Equal("r3", addcTemp.Value.RegisterName);
        Assert.Equal("add", Assert.IsType<IrBinary>(addcIr[1]).Op);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(addcIr[2]).Target);
        Assert.IsType<IrSetCrField>(addcIr[3]);

        var subfc = Instruction(0, "subfc.", Gpr(5), Gpr(1), Gpr(5));
        var subfcIr = Assert.Single(lifter.Lift(new[] { subfc })).Ir;
        var subfcTemp = Assert.IsType<IrAssign>(subfcIr[0]);
        Assert.Equal("r5_subfc_min", subfcTemp.Destination);
        Assert.Equal("r5", subfcTemp.Value.RegisterName);
        Assert.Equal("sub", Assert.IsType<IrBinary>(subfcIr[1]).Op);
        Assert.Equal("PPC_UpdateCarrySub", Assert.IsType<IrCall>(subfcIr[2]).Target);
        Assert.IsType<IrSetCrField>(subfcIr[3]);

        var lmw = Instruction(0, "lmw", Gpr(30), new PpcDisplacementOperand(8, "r4", 4));
        var lmwIr = Assert.Single(lifter.Lift(new[] { lmw })).Ir;
        Assert.Collection(
            lmwIr,
            op => Assert.Equal("r30", Assert.IsType<IrLoad>(op).Destination),
            op => Assert.Equal("r31", Assert.IsType<IrLoad>(op).Destination));
    }

    [Fact]
    public void LiftsIndexedByteUpdateFormsAndRejectsZeroBase()
    {
        var lifter = new PpcLifter();

        var lbzux = Instruction((3u << 21) | (4u << 16) | (5u << 11), "lbzux");
        var lbzuxIr = Assert.Single(lifter.Lift(new[] { lbzux })).Ir;
        Assert.Equal(3, lbzuxIr.Count);
        Assert.Equal("addr_lbzux_80000000_loc", Assert.IsType<IrBinary>(lbzuxIr[0]).Destination);
        Assert.Equal("r3", Assert.IsType<IrLoad>(lbzuxIr[1]).Destination);
        Assert.Equal("r4", Assert.IsType<IrAssign>(lbzuxIr[2]).Destination);

        var stbux = Instruction((2u << 21) | (4u << 16) | (5u << 11), "stbux");
        var stbuxIr = Assert.Single(lifter.Lift(new[] { stbux })).Ir;
        Assert.Equal(3, stbuxIr.Count);
        Assert.Equal("addr_stbux_80000000_loc", Assert.IsType<IrBinary>(stbuxIr[0]).Destination);
        Assert.IsType<IrStore>(stbuxIr[1]);
        Assert.Equal("r4", Assert.IsType<IrAssign>(stbuxIr[2]).Destination);

        var invalidLbzux = Instruction((1u << 21) | (0u << 16) | (5u << 11), "lbzux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLbzux }));

        var invalidStbux = Instruction((1u << 21) | (0u << 16) | (5u << 11), "stbux");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidStbux }));
    }

    [Fact]
    public void LiftsChainedCarryAndZeroingVariants()
    {
        var lifter = new PpcLifter();

        var adde = Instruction(0, "adde.", Gpr(7), Gpr(7), Gpr(8));
        var addeIr = Assert.Single(lifter.Lift(new[] { adde })).Ir;
        Assert.Equal("r7_adde_left", Assert.IsType<IrAssign>(addeIr[0]).Destination);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(addeIr[1]).Target);
        Assert.Equal("add", Assert.IsType<IrBinary>(addeIr[2]).Op);
        Assert.Equal("add", Assert.IsType<IrBinary>(addeIr[3]).Op);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(addeIr[4]).Target);
        Assert.IsType<IrSetCrField>(addeIr[5]);

        var addze = Instruction(0, "addze.", Gpr(9), Gpr(9));
        var addzeIr = Assert.Single(lifter.Lift(new[] { addze })).Ir;
        Assert.Equal("r9_addze_src", Assert.IsType<IrAssign>(addzeIr[0]).Destination);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(addzeIr[1]).Target);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(addzeIr[3]).Target);
        Assert.IsType<IrSetCrField>(addzeIr[4]);

        var addme = Instruction(0, "addme.", Gpr(11), Gpr(11));
        var addmeIr = Assert.Single(lifter.Lift(new[] { addme })).Ir;
        Assert.Equal("r11_addme_src", Assert.IsType<IrAssign>(addmeIr[0]).Destination);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(addmeIr[1]).Target);
        Assert.Equal(-1, Assert.IsType<IrBinary>(addmeIr[2]).Right.Constant);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(addmeIr[4]).Target);
        Assert.IsType<IrSetCrField>(addmeIr[5]);

        var subfze = Instruction(0, "subfze.", Gpr(10), Gpr(10));
        var subfzeIr = Assert.Single(lifter.Lift(new[] { subfze })).Ir;
        Assert.Equal("r10_subfze_src", Assert.IsType<IrAssign>(subfzeIr[0]).Destination);
        Assert.Equal("not", Assert.IsType<IrBinary>(subfzeIr[1]).Op);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(subfzeIr[2]).Target);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(subfzeIr[4]).Target);
        Assert.IsType<IrSetCrField>(subfzeIr[5]);

        var subfme = Instruction(0, "subfme.", Gpr(12), Gpr(12));
        var subfmeIr = Assert.Single(lifter.Lift(new[] { subfme })).Ir;
        Assert.Equal("r12_subfme_src", Assert.IsType<IrAssign>(subfmeIr[0]).Destination);
        Assert.Equal("not", Assert.IsType<IrBinary>(subfmeIr[1]).Op);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(subfmeIr[2]).Target);
        Assert.Equal(-1, Assert.IsType<IrBinary>(subfmeIr[3]).Right.Constant);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(subfmeIr[5]).Target);
        Assert.IsType<IrSetCrField>(subfmeIr[6]);
    }

    [Fact]
    public void LiftsBranchCtrCrAndMaskedRegisterForms()
    {
        var lifter = new PpcLifter();

        var bcctr = Instruction((4u << 21) | (9u << 16), "bcctr");
        var bcctrIr = Assert.Single(lifter.Lift(new[] { bcctr })).Ir;
        var bcctrBranch = Assert.IsType<IrBranch>(Assert.Single(bcctrIr));
        Assert.Equal("raw", bcctrBranch.Condition);
        Assert.Equal("indirect_ctr_80000000", bcctrBranch.TrueLabel);
        Assert.Equal("0x80000004", bcctrBranch.FalseLabel);
        Assert.Contains("GetCRBit(ctx, 2, 1)", bcctrBranch.ConditionRegister, StringComparison.Ordinal);

        var bctr = PpcInstruction.Synthetic(
            0x80000010,
            0,
            "bctr",
            Array.Empty<PpcOperand>(),
            branchTargets: new[] { 0x80000030u, 0x80000020u, 0x80000030u });
        var bctrIr = Assert.Single(lifter.Lift(new[] { bctr })).Ir;
        var jumpTable = Assert.IsType<IrJumpTable>(Assert.Single(bctrIr));
        Assert.Equal("ctr", jumpTable.Selector);
        Assert.Equal(new uint[] { 0x80000030u, 0x80000020u }, jumpTable.Cases.Select(c => c.TargetAddress));

        var directBctr = Instruction(0, "bctr");
        var directBctrIr = Assert.Single(lifter.Lift(new[] { directBctr })).Ir;
        Assert.Equal("ctr", Assert.IsType<IrIndirectJump>(Assert.Single(directBctrIr)).Target.RegisterName);

        var crclr = Instruction(0, "crclr", new PpcConditionRegisterOperand("cr7eq", 30));
        var crclrIr = Assert.Single(lifter.Lift(new[] { crclr })).Ir;
        var crclrCall = Assert.IsType<IrCall>(Assert.Single(crclrIr));
        Assert.Equal("PPC_CrSetBit", crclrCall.Target);
        Assert.Equal(new long?[] { 30, 0 }, crclrCall.Arguments.Select(a => a.Constant).ToArray());

        var cror = Instruction(
            0,
            "cror",
            new PpcOperand[]
            {
                new PpcConditionRegisterOperand("crb1", 1),
                new PpcConditionRegisterOperand("crb2", 2),
                new PpcConditionRegisterOperand("crb3", 3)
            });
        var crorIr = Assert.Single(lifter.Lift(new[] { cror })).Ir;
        var crorCall = Assert.IsType<IrCall>(Assert.Single(crorIr));
        Assert.Equal("PPC_CrLogical", crorCall.Target);
        Assert.Equal(new long?[] { 7, 1, 2, 3 }, crorCall.Arguments.Select(a => a.Constant).ToArray());

        var mtcrfNoOp = Instruction(0, "mtcrf", new PpcImmediateOperand(0), Gpr(3));
        var mtcrfNoOpIr = Assert.Single(lifter.Lift(new[] { mtcrfNoOp })).Ir;
        Assert.Contains("no-op", Assert.IsType<IrComment>(Assert.Single(mtcrfNoOpIr)).Text, StringComparison.Ordinal);

        var mtcrfMasked = Instruction(0, "mtcrf", new PpcImmediateOperand(0x80), Gpr(4));
        var mtcrfMaskedIr = Assert.Single(lifter.Lift(new[] { mtcrfMasked })).Ir;
        Assert.Equal(3, mtcrfMaskedIr.Count);
        var maskedSrc = Assert.IsType<IrBinary>(mtcrfMaskedIr[0]);
        Assert.Equal("r4_mtcrf_src", maskedSrc.Destination);
        Assert.Equal(unchecked((int)0xF0000000), maskedSrc.Right.Constant);
        Assert.Equal("cr", Assert.IsType<IrBinary>(mtcrfMaskedIr[2]).Destination);
    }

    [Fact]
    public void LiftsComparisonShiftAndUpdateMemoryForms()
    {
        var lifter = new PpcLifter();

        var cmplwi = Instruction(0, "cmplwi", Gpr(6), new PpcImmediateOperand(0x1234));
        var cmplwiIr = Assert.Single(lifter.Lift(new[] { cmplwi })).Ir;
        var cmplwiSetCr = Assert.IsType<IrSetCrField>(Assert.Single(cmplwiIr));
        Assert.True(cmplwiSetCr.IsUnsigned);
        Assert.Equal("r6", cmplwiSetCr.Left.RegisterName);
        Assert.Equal(0x1234, cmplwiSetCr.Right.Constant);

        var cmpwi = Instruction(0, "cmpwi", new PpcConditionRegisterOperand("cr2", 8), Gpr(6), new PpcImmediateOperand(-1));
        var cmpwiIr = Assert.Single(lifter.Lift(new[] { cmpwi })).Ir;
        var cmpwiSetCr = Assert.IsType<IrSetCrField>(Assert.Single(cmpwiIr));
        Assert.Equal(2, cmpwiSetCr.FieldIndex);
        Assert.False(cmpwiSetCr.IsUnsigned);
        Assert.Equal("r6", cmpwiSetCr.Left.RegisterName);
        Assert.Equal(-1, cmpwiSetCr.Right.Constant);

        var sraw = Instruction(0, "sraw.", Gpr(11), Gpr(12), Gpr(13));
        var srawIr = Assert.Single(lifter.Lift(new[] { sraw })).Ir;
        Assert.Equal("PPC_UpdateCarryShiftRight", Assert.IsType<IrCall>(srawIr[0]).Target);
        Assert.Equal("r13", Assert.IsType<IrCall>(srawIr[0]).Arguments[1].RegisterName);
        Assert.Equal("ppc_sraw", Assert.IsType<IrBinary>(srawIr[1]).Op);
        Assert.IsType<IrSetCrField>(srawIr[2]);

        var srw = Instruction(0, "srw.", Gpr(7), Gpr(8), Gpr(9));
        var srwIr = Assert.Single(lifter.Lift(new[] { srw })).Ir;
        var srwBinary = Assert.IsType<IrBinary>(srwIr[0]);
        Assert.Equal("ppc_srw", srwBinary.Op);
        Assert.Equal("r9", srwBinary.Right.RegisterName);
        Assert.IsType<IrSetCrField>(srwIr[1]);

        var slw = Instruction(0, "slw.", Gpr(10), Gpr(11), Gpr(12));
        var slwIr = Assert.Single(lifter.Lift(new[] { slw })).Ir;
        var slwBinary = Assert.IsType<IrBinary>(slwIr[0]);
        Assert.Equal("ppc_slw", slwBinary.Op);
        Assert.Equal("r12", slwBinary.Right.RegisterName);
        Assert.IsType<IrSetCrField>(slwIr[1]);

        var lhau = Instruction((3u << 21) | (4u << 16) | 0x0010u, "lhau");
        var lhauIr = Assert.Single(lifter.Lift(new[] { lhau })).Ir;
        Assert.Equal("r4", Assert.IsType<IrBinary>(lhauIr[0]).Destination);
        Assert.Equal("r3", Assert.IsType<IrLoad>(lhauIr[1]).Destination);
        Assert.Equal("sar", Assert.IsType<IrBinary>(lhauIr[3]).Op);
        var invalidLhau = Instruction((3u << 21) | (0u << 16) | 0x0010u, "lhau");
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLhau }));

        var stbu = Instruction(0, "stbu", Gpr(7), new PpcDisplacementOperand(4, "r8", 8));
        var stbuIr = Assert.Single(lifter.Lift(new[] { stbu })).Ir;
        Assert.Equal("r8_stbu_ea", Assert.IsType<IrBinary>(stbuIr[0]).Destination);
        Assert.IsType<IrStore>(stbuIr[1]);
        Assert.Equal("r8", Assert.IsType<IrAssign>(stbuIr[2]).Destination);

        var sthu = Instruction(0, "sthu", Gpr(7), new PpcDisplacementOperand(6, "r8", 8));
        var sthuIr = Assert.Single(lifter.Lift(new[] { sthu })).Ir;
        Assert.Equal("r8_sthu_ea", Assert.IsType<IrBinary>(sthuIr[0]).Destination);
        Assert.IsType<IrStore>(sthuIr[1]);
        Assert.Equal("r8", Assert.IsType<IrAssign>(sthuIr[2]).Destination);

        var addic = Instruction(0, "addic.", Gpr(15), Gpr(15), new PpcImmediateOperand(4));
        var addicIr = Assert.Single(lifter.Lift(new[] { addic })).Ir;
        Assert.Equal("r15_addic_src", Assert.IsType<IrAssign>(addicIr[0]).Destination);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(addicIr[2]).Target);
        Assert.IsType<IrSetCrField>(addicIr[3]);

        var subfe = Instruction(0, "subfe.", Gpr(16), Gpr(1), Gpr(16));
        var subfeIr = Assert.Single(lifter.Lift(new[] { subfe })).Ir;
        Assert.Equal("r16_subfe_rb", Assert.IsType<IrAssign>(subfeIr[0]).Destination);
        Assert.Equal("not", Assert.IsType<IrBinary>(subfeIr[1]).Op);
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(subfeIr[2]).Target);
        Assert.Equal("PPC_UpdateCarryAdd", Assert.IsType<IrCall>(subfeIr[5]).Target);
        Assert.IsType<IrSetCrField>(subfeIr[6]);
    }

    [Fact]
    public void LiftsMiscUnaryStatusAndLogicalForms()
    {
        var lifter = new PpcLifter();

        var neg = Instruction(0, "neg.", Gpr(3), Gpr(4));
        var negIr = Assert.Single(lifter.Lift(new[] { neg })).Ir;
        var negBinary = Assert.IsType<IrBinary>(negIr[0]);
        Assert.Equal("sub", negBinary.Op);
        Assert.Equal(0, negBinary.Left.Constant);
        Assert.Equal("r4", negBinary.Right.RegisterName);
        Assert.IsType<IrSetCrField>(negIr[1]);

        var nor = Instruction(0, "nor", Gpr(5), Gpr(6), Gpr(7));
        var norIr = Assert.Single(lifter.Lift(new[] { nor })).Ir;
        Assert.Equal("nor", Assert.IsType<IrBinary>(Assert.Single(norIr)).Op);

        var nand = Instruction(0, "nand.", Gpr(8), Gpr(9), Gpr(10));
        var nandIr = Assert.Single(lifter.Lift(new[] { nand })).Ir;
        Assert.Equal("nand", Assert.IsType<IrBinary>(nandIr[0]).Op);
        Assert.IsType<IrSetCrField>(nandIr[1]);

        var mfdar = Instruction(0, "mfdar", Gpr(11));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfdar })).Ir)).Value.Constant);

        var mfmsr = Instruction(0, "mfmsr", Gpr(12));
        Assert.Equal("msr", Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfmsr })).Ir)).Value.RegisterName);

        var mfpvr = Instruction(0, "mfpvr", Gpr(13));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfpvr })).Ir)).Value.Constant);

        var mfdsisr = Instruction(0, "mfdsisr", Gpr(14));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfdsisr })).Ir)).Value.Constant);

        var blelr = Instruction(0, "blelr");
        var blelrBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { blelr })).Ir));
        Assert.Equal("ble", blelrBranch.Condition);
        Assert.Equal("return", blelrBranch.TrueLabel);
    }

    [Fact]
    public void LiftsUpdateLoadFormsAndCacheNoOps()
    {
        var lifter = new PpcLifter();

        var dcbzR0 = Instruction(0, "dcbz", Gpr(0), Gpr(5));
        var dcbzR0Ir = Assert.Single(lifter.Lift(new[] { dcbzR0 })).Ir;
        Assert.Equal("r5_addr_dcbz", Assert.IsType<IrAssign>(dcbzR0Ir[0]).Destination);
        Assert.Equal("memset_zero_32", Assert.IsType<IrCall>(dcbzR0Ir[2]).Target);

        var dcbz = Instruction(0, "dcbz_l", Gpr(4), Gpr(5));
        var dcbzIr = Assert.Single(lifter.Lift(new[] { dcbz })).Ir;
        Assert.Equal("add", Assert.IsType<IrBinary>(dcbzIr[0]).Op);
        Assert.Equal("and", Assert.IsType<IrBinary>(dcbzIr[1]).Op);

        foreach (var mnemonic in new[] { "icbi", "dcbi", "dcbf" })
        {
            var ins = Instruction(0, mnemonic);
            var ir = Assert.Single(lifter.Lift(new[] { ins })).Ir;
            Assert.Contains(mnemonic, Assert.IsType<IrComment>(Assert.Single(ir)).Text, StringComparison.Ordinal);
        }

        var lfsu = Instruction(0, "lfsu", Gpr(1), new PpcDisplacementOperand(4, "r3", 3));
        var lfsuIr = Assert.Single(lifter.Lift(new[] { lfsu })).Ir;
        Assert.Equal("r3", Assert.IsType<IrBinary>(lfsuIr[0]).Destination);
        Assert.Equal("r1", Assert.IsType<IrLoad>(lfsuIr[1]).Destination);
        var invalidLfsu = Instruction(0, "lfsu", Gpr(1), new PpcDisplacementOperand(4, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLfsu }));

        var lhzu = Instruction(0, "lhzu", Gpr(2), new PpcDisplacementOperand(6, "r4", 4));
        var lhzuIr = Assert.Single(lifter.Lift(new[] { lhzu })).Ir;
        Assert.Equal("r4", Assert.IsType<IrBinary>(lhzuIr[0]).Destination);
        Assert.Equal("r2", Assert.IsType<IrLoad>(lhzuIr[1]).Destination);
        var invalidLhzu = Instruction(0, "lhzu", Gpr(2), new PpcDisplacementOperand(6, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLhzu }));

        var lbzu = Instruction(0, "lbzu", Gpr(3), new PpcDisplacementOperand(8, "r5", 5));
        var lbzuIr = Assert.Single(lifter.Lift(new[] { lbzu })).Ir;
        Assert.Equal("r5", Assert.IsType<IrBinary>(lbzuIr[0]).Destination);
        Assert.Equal("r3", Assert.IsType<IrLoad>(lbzuIr[1]).Destination);
        var invalidLbzu = Instruction(0, "lbzu", Gpr(3), new PpcDisplacementOperand(8, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidLbzu }));

        var stwu = Instruction(0, "stwu", Gpr(6), new PpcDisplacementOperand(0x10, "r7", 7));
        var stwuIr = Assert.Single(lifter.Lift(new[] { stwu })).Ir;
        Assert.IsType<IrStore>(stwuIr[0]);
        Assert.Equal("r7", Assert.IsType<IrBinary>(stwuIr[1]).Destination);
        var invalidStwu = Instruction(0, "stwu", Gpr(6), new PpcDisplacementOperand(0x10, "r0", 0));
        Assert.Throws<InvalidOperationException>(() => lifter.Lift(new[] { invalidStwu }));
    }

    [Fact]
    public void LiftsControlRegisterTableAndDivisionForms()
    {
        var lifter = new PpcLifter();

        var cmplw = Instruction(0, "cmplw", new PpcConditionRegisterOperand("cr4", 16), Gpr(8), Gpr(9));
        var cmplwIr = Assert.Single(lifter.Lift(new[] { cmplw })).Ir;
        var cmplwSetCr = Assert.IsType<IrSetCrField>(Assert.Single(cmplwIr));
        Assert.Equal(4, cmplwSetCr.FieldIndex);
        Assert.True(cmplwSetCr.IsUnsigned);
        Assert.Equal("r8", cmplwSetCr.Left.RegisterName);
        Assert.Equal("r9", cmplwSetCr.Right.RegisterName);

        foreach (var mnemonic in new[] { "mticcr", "mttbu", "mttbl", "mtdar", "mtdsisr" })
        {
            var ins = Instruction(0, mnemonic, Gpr(3));
            Assert.Throws<NotImplementedException>(() => lifter.Lift(new[] { ins }));
        }

        var mfibatu = Instruction(0, "mfibatu", Gpr(4), Gpr(5));
        Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfibatu })).Ir)).Value.Constant);

        foreach (var mnemonic in new[] { "mtibatu", "mtibatl", "mtdbatu", "mtdbatl" })
        {
            var ins = Instruction(0, mnemonic, Gpr(4), Gpr(5));
            Assert.Throws<NotImplementedException>(() => lifter.Lift(new[] { ins }));
        }

        foreach (var mnemonic in new[] { "mfibatl", "mfdbatu", "mfdbatl" })
        {
            var ins = Instruction(0, mnemonic, Gpr(4), Gpr(5));
            var ir = Assert.Single(lifter.Lift(new[] { ins })).Ir;
            Assert.Equal(0, Assert.IsType<IrAssign>(Assert.Single(ir)).Value.Constant);
        }

        var mtxer = Instruction(0, "mtxer", Gpr(6));
        Assert.Equal("xer", Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mtxer })).Ir)).Destination);

        var mfxer = Instruction(0, "mfxer", Gpr(7));
        Assert.Equal("xer", Assert.IsType<IrAssign>(Assert.Single(Assert.Single(lifter.Lift(new[] { mfxer })).Ir)).Value.RegisterName);

        var divw = Instruction(0, "divw.", Gpr(10), Gpr(11), Gpr(12));
        var divwIr = Assert.Single(lifter.Lift(new[] { divw })).Ir;
        Assert.Equal("div", Assert.IsType<IrBinary>(divwIr[0]).Op);
        Assert.IsType<IrSetCrField>(divwIr[1]);
    }

    [Fact]
    public void LiftsExtraCrShiftAndUnsupportedFallbackForms()
    {
        var lifter = new PpcLifter();

        var srw = Instruction(0, "srw.", Gpr(3), Gpr(4), Gpr(5));
        var srwIr = Assert.Single(lifter.Lift(new[] { srw })).Ir;
        Assert.Equal("r3", Assert.IsType<IrBinary>(srwIr[0]).Destination);
        Assert.Equal("ppc_srw", Assert.IsType<IrBinary>(srwIr[0]).Op);
        Assert.IsType<IrSetCrField>(srwIr[1]);

        var divwu = Instruction(0, "divwu.", Gpr(6), Gpr(7), Gpr(8));
        var divwuIr = Assert.Single(lifter.Lift(new[] { divwu })).Ir;
        Assert.Equal("divu", Assert.IsType<IrBinary>(divwuIr[0]).Op);
        Assert.IsType<IrSetCrField>(divwuIr[1]);

        var extsb = Instruction(0, "extsb.", Gpr(9), Gpr(10));
        var extsbIr = Assert.Single(lifter.Lift(new[] { extsb })).Ir;
        var extsbBinary = Assert.IsType<IrBinary>(extsbIr[0]);
        Assert.Equal("sext", extsbBinary.Op);
        Assert.Equal(8, extsbBinary.Right.Constant);
        Assert.IsType<IrSetCrField>(extsbIr[1]);

        var mcrf = Instruction(0, "mcrf", new PpcConditionRegisterOperand("cr7", 28), new PpcConditionRegisterOperand("cr2", 8));
        var mcrfCall = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { mcrf })).Ir));
        Assert.Equal("PPC_Mcrf", mcrfCall.Target);
        Assert.Equal(new long?[] { 7, 2 }, mcrfCall.Arguments.Select(a => a.Constant).ToArray());

        var mcrxr = Instruction(0, "mcrxr", new PpcConditionRegisterOperand("cr7", 28));
        var mcrxrCall = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { mcrxr })).Ir));
        Assert.Equal("PPC_Mcrxr", mcrxrCall.Target);
        Assert.Equal(7, Assert.Single(mcrxrCall.Arguments).Constant);

        var crxor = Instruction(
            0,
            "crxor",
            new PpcConditionRegisterOperand("cr7eq", 30),
            new PpcConditionRegisterOperand("crb2", 2),
            new PpcConditionRegisterOperand("31", 31));
        var crxorCall = Assert.IsType<IrCall>(Assert.Single(Assert.Single(lifter.Lift(new[] { crxor })).Ir));
        Assert.Equal("PPC_CrLogical", crxorCall.Target);
        Assert.Equal(new long?[] { 2, 30, 2, 31 }, crxorCall.Arguments.Select(a => a.Constant).ToArray());

        var slwi = Instruction(0, "slwi.", Gpr(11), Gpr(12), new PpcImmediateOperand(5));
        var slwiIr = Assert.Single(lifter.Lift(new[] { slwi })).Ir;
        Assert.Equal("shl", Assert.IsType<IrBinary>(slwiIr[0]).Op);
        Assert.IsType<IrSetCrField>(slwiIr[1]);

        var unknown = Instruction(0x12345678, "totally_unknown", Gpr(3));
        var undefined = Assert.IsType<IrUndefined>(Assert.Single(Assert.Single(lifter.Lift(new[] { unknown }, allowUnsupported: true)).Ir));
        Assert.Contains("totally_unknown", undefined.Disassembly, StringComparison.Ordinal);
        Assert.Equal(0x12345678u, undefined.RawInstruction);

        var unsupported = Instruction(0, "mticcr", Gpr(3));
        var unsupportedUndefined = Assert.IsType<IrUndefined>(Assert.Single(Assert.Single(lifter.Lift(new[] { unsupported }, allowUnsupported: true)).Ir));
        Assert.Contains("UNIMPLEMENTED", unsupportedUndefined.Reason ?? string.Empty, StringComparison.Ordinal);
    }

    [Fact]
    public void PrivateHelpersParseDisplacementsCrBitsAndBranchConditions()
    {
        Assert.Equal((0, "r3"), InvokePrivate<(int offset, string @base)>("ParseDisplacement", "r3"));
        Assert.Equal((0x1234, "0"), InvokePrivate<(int offset, string @base)>("ParseDisplacement", "0x1234"));
        Assert.Equal((0, "r4"), InvokePrivate<(int offset, string @base)>("ParseDisplacement", "(r4)"));
        Assert.Throws<TargetInvocationException>(() => InvokePrivate<(int offset, string @base)>("ParseDisplacement", "broken("));

        Assert.Equal(30, InvokePrivate<int>("ParseCrBitIndex", "cr7eq"));
        Assert.Equal(2, InvokePrivate<int>("ParseCrBitIndex", "crb2"));
        Assert.Equal(25, InvokePrivate<int>("ParseCrBitIndex", "cr6gt"));
        Assert.Equal(31, InvokePrivate<int>("ParseCrBitIndex", "31"));
        Assert.Equal(0, InvokePrivate<int>("ParseCrBitIndex", "mystery"));

        Assert.Equal(7, InvokePrivate<int>("ParseCrFieldName", "crf7"));
        Assert.Equal(3, InvokePrivate<int>("ParseCrFieldName", "cr3"));
        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldName", "bogus"));

        var absoluteLoad = InvokePrivate<IReadOnlyList<IrInstruction>>("DFormLoad", "r5", "0", 12, 4);
        Assert.Equal("r5_ea", Assert.IsType<IrAssign>(absoluteLoad[0]).Destination);
        Assert.Equal("r5", Assert.IsType<IrLoad>(absoluteLoad[1]).Destination);

        var registerLoad = InvokePrivate<IReadOnlyList<IrInstruction>>("DFormLoad", "r6", "r7", 16, 2);
        var registerLoadIr = Assert.Single(registerLoad);
        var load = Assert.IsType<IrLoad>(registerLoadIr);
        Assert.Equal("r6", load.Destination);
        Assert.Equal("r7", load.Address.Base);
        Assert.Equal(16, load.Address.Offset);
    }

    [Fact]
    public void PrivateCrLogicalHelperMapsAllRemainingOpcodes()
    {
        foreach (var (mnemonic, opcode) in new[]
        {
            ("crnor", 0L),
            ("crandc", 1L),
            ("crnand", 3L),
            ("crand", 4L),
            ("creqv", 5L),
            ("crorc", 6L)
        })
        {
            var ir = Assert.Single(InvokePrivate<IReadOnlyList<IrInstruction>>("LiftCrLogical", mnemonic, "cr7eq", "crb2", "31"));
            var call = Assert.IsType<IrCall>(ir);
            Assert.Equal("PPC_CrLogical", call.Target);
            Assert.Equal(opcode, call.Arguments[0].Constant);
            Assert.Equal(30, call.Arguments[1].Constant);
            Assert.Equal(2, call.Arguments[2].Constant);
            Assert.Equal(31, call.Arguments[3].Constant);
        }
    }

    [Fact]
    public void LiftsLinkRegisterBranchFamiliesAndGenericBcFallbacks()
    {
        var lifter = new PpcLifter();

        var beqlrl = Instruction(0, "beqlrl", new PpcConditionRegisterOperand("cr3", 12));
        var beqlrlBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { beqlrl })).Ir));
        Assert.Equal("beq", beqlrlBranch.Condition);
        Assert.Equal("cr3", beqlrlBranch.ConditionRegister);
        Assert.Contains("call_lr_80000000_80000004", beqlrlBranch.TrueLabel, StringComparison.Ordinal);
        Assert.Equal("link_branch_80000004_80000004", beqlrlBranch.FalseLabel);

        var bgtlrl = Instruction(0, "bgtlrl");
        var bgtlrlBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bgtlrl })).Ir));
        Assert.Equal("bgt", bgtlrlBranch.Condition);
        Assert.Equal("cr0", bgtlrlBranch.ConditionRegister);

        var bnelrCr6 = Instruction(0, "bnelr", new PpcConditionRegisterOperand("cr6", 24));
        var bnelrCr6Branch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bnelrCr6 })).Ir));
        Assert.Equal("bne", bnelrCr6Branch.Condition);
        Assert.Equal("return", bnelrCr6Branch.TrueLabel);
        Assert.Equal("cr6", bnelrCr6Branch.ConditionRegister);

        var bnelrCr7 = Instruction(0, "bnelr", new PpcConditionRegisterOperand("cr7", 28));
        var bnelrCr7Branch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bnelrCr7 })).Ir));
        Assert.Equal("bne", bnelrCr7Branch.Condition);
        Assert.Equal("cr7", bnelrCr7Branch.ConditionRegister);

        var bcImmediate = Instruction(
            0,
            "bc",
            new PpcImmediateOperand(4),
            new PpcImmediateOperand(3),
            new PpcImmediateOperand(unchecked((int)0x80000020)));
        var bcImmediateBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bcImmediate })).Ir));
        Assert.Equal("raw", bcImmediateBranch.Condition);
        Assert.Contains("GetCRBit(ctx, 0, 3)", bcImmediateBranch.ConditionRegister, StringComparison.Ordinal);

        var bcFallback = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "bc",
            new PpcOperand[] { new PpcConditionRegisterOperand("cr4", 16) },
            new[] { 0x80000010u },
            isConditional: true);
        var bcFallbackBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bcFallback })).Ir));
        Assert.Equal("bc", bcFallbackBranch.Condition);
        Assert.Equal("cr4", bcFallbackBranch.ConditionRegister);

        var ctrBc = Assert.Single(lifter.Lift(new[] { Instruction(0, "bc", new PpcImmediateOperand(0), new PpcImmediateOperand(0), new PpcImmediateOperand(4)) })).Ir;
        Assert.Equal("ctr", Assert.IsType<IrBinary>(ctrBc[0]).Destination);
        Assert.Equal("raw", Assert.IsType<IrBranch>(ctrBc[1]).Condition);

        var bclrReturn = Instruction((20u << 21), "bclr");
        var bclrReturnBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bclrReturn })).Ir));
        Assert.Equal("raw", bclrReturnBranch.Condition);
        Assert.Equal("return", bclrReturnBranch.TrueLabel);

        var bclrCr = Instruction((12u << 21) | (2u << 16), "bclr");
        var bclrCrBranch = Assert.IsType<IrBranch>(Assert.Single(Assert.Single(lifter.Lift(new[] { bclrCr })).Ir));
        Assert.Equal("raw", bclrCrBranch.Condition);
        Assert.Equal("return", bclrCrBranch.TrueLabel);

        var bclrCtr = Instruction((16u << 21), "bclr");
        var bclrCtrIr = Assert.Single(lifter.Lift(new[] { bclrCtr })).Ir;
        Assert.Equal("ctr", Assert.IsType<IrBinary>(bclrCtrIr[0]).Destination);
        Assert.Equal("raw", Assert.IsType<IrBranch>(bclrCtrIr[1]).Condition);

        var combinedBclr = Assert.Single(lifter.Lift(new[] { Instruction(0, "bclr") })).Ir;
        Assert.Equal("ctr", Assert.IsType<IrBinary>(combinedBclr[0]).Destination);
        Assert.Equal("raw", Assert.IsType<IrBranch>(combinedBclr[1]).Condition);
    }

    [Fact]
    public void LiftsCacheHintsByteReverseAndRawXoFallbacks()
    {
        var lifter = new PpcLifter();

        foreach (var mnemonic in new[] { "dcbst", "xo_54", "xo_470", "dcbt", "xo_278", "dcbtst", "xo_246", "dcba", "xo_758", "tlbie", "xo_306", "tlbsync", "xo_566" })
        {
            var ir = Assert.Single(lifter.Lift(new[] { Instruction(0, mnemonic) })).Ir;
            Assert.IsType<IrComment>(Assert.Single(ir));
        }

        var stwbrx = Assert.Single(lifter.Lift(new[] { Instruction((3u << 21) | (4u << 16) | (5u << 11), "stwbrx") })).Ir;
        Assert.Equal("PPC_StoreWordByteReverse", Assert.IsType<IrCall>(stwbrx[1]).Target);

        var lwbrx = Assert.Single(lifter.Lift(new[] { Instruction((6u << 21) | (7u << 16) | (8u << 11), "lwbrx") })).Ir;
        Assert.Equal("PPC_LoadWordByteReverse", Assert.IsType<IrCall>(lwbrx[1]).Target);

        var lhbrx = Assert.Single(lifter.Lift(new[] { Instruction((9u << 21) | (10u << 16) | (11u << 11), "lhbrx") })).Ir;
        Assert.Equal("PPC_LoadHalfwordByteReverse", Assert.IsType<IrCall>(lhbrx[1]).Target);

        var sthbrx = Assert.Single(lifter.Lift(new[] { Instruction((12u << 21) | (13u << 16) | (14u << 11), "sthbrx") })).Ir;
        Assert.Equal("PPC_StoreHalfwordByteReverse", Assert.IsType<IrCall>(sthbrx[1]).Target);

        var xoNand = Assert.Single(lifter.Lift(new[] { Instruction((4u << 21) | (5u << 16) | (6u << 11) | 1u, "xo_476") })).Ir;
        Assert.Equal("nand", Assert.IsType<IrBinary>(xoNand[0]).Op);
        Assert.IsType<IrSetCrField>(xoNand[1]);

        var xoMulhwu = Assert.Single(lifter.Lift(new[] { Instruction((7u << 21) | (8u << 16) | (9u << 11), "xo_11") })).Ir;
        Assert.Equal("mulhwu", Assert.IsType<IrBinary>(Assert.Single(xoMulhwu)).Op);

        var xoStwx = Assert.Single(lifter.Lift(new[] { Instruction((10u << 21) | (11u << 16) | (12u << 11), "xo_151") })).Ir;
        Assert.Equal(4, Assert.IsType<IrStore>(xoStwx[1]).SizeBytes);

        var xoStbx = Assert.Single(lifter.Lift(new[] { Instruction((13u << 21) | (14u << 16) | (15u << 11), "xo_215") })).Ir;
        Assert.Equal(1, Assert.IsType<IrStore>(xoStbx[1]).SizeBytes);

        var xoAddze = Assert.Single(lifter.Lift(new[] { Instruction((16u << 21) | (17u << 16) | 1u, "xo_202") })).Ir;
        Assert.Equal("PPC_GetCarry", Assert.IsType<IrCall>(xoAddze[0]).Target);
        Assert.IsType<IrSetCrField>(xoAddze[^1]);

        var xoCmp = Assert.Single(lifter.Lift(new[] { Instruction((3u << 23) | (18u << 16) | (19u << 11), "xo_0") })).Ir;
        var xoCmpSetCr = Assert.IsType<IrSetCrField>(Assert.Single(xoCmp));
        Assert.Equal(3, xoCmpSetCr.FieldIndex);
        Assert.Equal("r18", xoCmpSetCr.Left.RegisterName);
        Assert.Equal("r19", xoCmpSetCr.Right.RegisterName);

        var xoMfsr = Instruction((20u << 21), "xo_595");
        Assert.Throws<NotImplementedException>(() => lifter.Lift(new[] { xoMfsr }));
    }
}
