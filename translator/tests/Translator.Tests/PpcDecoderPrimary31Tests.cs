using System;
using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class PpcDecoderPrimary31Tests
{
    private static uint EncodeXoForm(uint rs, uint ra, uint rb, uint xo, bool rc = false, bool oe = false)
        => (31u << 26) | (rs << 21) | (ra << 16) | (rb << 11) | ((oe ? 1u : 0u) << 10) | (xo << 1) | (rc ? 1u : 0u);

    private static uint EncodeXForm(uint rs, uint ra, uint rb, uint xo10, bool rc = false)
        => (31u << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo10 << 1) | (rc ? 1u : 0u);

    private static uint EncodeSprWord(uint rs, uint spr, uint xo)
    {
        var sprLo = spr & 0x1F;
        var sprHi = (spr >> 5) & 0x1F;
        return (31u << 26) | (rs << 21) | (sprLo << 16) | (sprHi << 11) | (xo << 1);
    }

    private static PpcInstruction Decode(uint word) => PpcDecoder.Decode(0x80000000, word);

    private static void AssertGpr(PpcOperand operand, string expected)
        => Assert.Equal(expected, Assert.IsType<PpcRegisterOperand>(operand).Name);

    private static void AssertFpr(PpcOperand operand, string expected)
        => Assert.Equal(expected, Assert.IsType<PpcRegisterOperand>(operand).Name);

    [Fact]
    public void DecodesXoFormArithmeticAndCompareMnemonics()
    {
        var cmpw = Decode((31u << 26) | (2u << 23) | (3u << 16) | (5u << 11));
        Assert.Equal("cmpw", cmpw.Mnemonic);
        Assert.True(cmpw.IsConditionalBranch);
        Assert.Collection(
            cmpw.Operands,
            op => Assert.Equal("cr2", Assert.IsType<PpcConditionRegisterOperand>(op).Name),
            op => AssertGpr(op, "r3"),
            op => AssertGpr(op, "r5"));

        var invalidCmp = Decode((31u << 26) | (1u << 21));
        Assert.Equal("invalid_cmp", invalidCmp.Mnemonic);

        var tw = Decode(EncodeXoForm(31, 4, 5, 4));
        Assert.Equal("tw", tw.Mnemonic);
        Assert.Collection(
            tw.Operands,
            op => Assert.Equal(31, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => AssertGpr(op, "r4"),
            op => AssertGpr(op, "r5"));

        var addc = Decode(EncodeXoForm(3, 4, 5, 10, rc: true, oe: true));
        Assert.Equal("addco.", addc.Mnemonic);
        Assert.Collection(addc.Operands, op => AssertGpr(op, "r3"), op => AssertGpr(op, "r4"), op => AssertGpr(op, "r5"));

        var subfe = Decode(EncodeXoForm(6, 7, 8, 136, oe: true));
        Assert.Equal("subfeo", subfe.Mnemonic);

        var addze = Decode(EncodeXoForm(9, 10, 0, 202, rc: true));
        Assert.Equal("addze.", addze.Mnemonic);
        Assert.Collection(addze.Operands, op => AssertGpr(op, "r9"), op => AssertGpr(op, "r10"));

        var addme = Decode(EncodeXoForm(9, 10, 0, 234, rc: true));
        Assert.Equal("addme.", addme.Mnemonic);
        Assert.Collection(addme.Operands, op => AssertGpr(op, "r9"), op => AssertGpr(op, "r10"));

        var subfme = Decode(EncodeXoForm(9, 10, 0, 232, rc: true));
        Assert.Equal("subfme.", subfme.Mnemonic);
        Assert.Collection(subfme.Operands, op => AssertGpr(op, "r9"), op => AssertGpr(op, "r10"));

        var neg = Decode(EncodeXoForm(11, 12, 0, 104, rc: true));
        Assert.Equal("neg.", neg.Mnemonic);

        var mullw = Decode(EncodeXoForm(13, 14, 15, 235, oe: true));
        Assert.Equal("mullwo", mullw.Mnemonic);

        var add = Decode(EncodeXoForm(16, 17, 18, 266, rc: true));
        Assert.Equal("add.", add.Mnemonic);

        var divwu = Decode(EncodeXoForm(19, 20, 21, 459, oe: true));
        Assert.Equal("divwuo", divwu.Mnemonic);

        var divw = Decode(EncodeXoForm(22, 23, 24, 491, rc: true));
        Assert.Equal("divw.", divw.Mnemonic);
    }

    [Fact]
    public void DecodesSprAndLogicalRegisterForms()
    {
        var mfcr = Decode(EncodeXoForm(4, 0, 0, 19));
        Assert.Equal("mfcr", mfcr.Mnemonic);
        AssertGpr(Assert.Single(mfcr.Operands), "r4");

        var and = Decode(EncodeXoForm(5, 6, 7, 28, rc: true));
        Assert.Equal("and.", and.Mnemonic);
        Assert.Collection(and.Operands, op => AssertGpr(op, "r6"), op => AssertGpr(op, "r5"), op => AssertGpr(op, "r7"));

        var andc = Decode(EncodeXoForm(5, 6, 7, 60, rc: true));
        Assert.Equal("andc.", andc.Mnemonic);

        var mfmsr = Decode(EncodeXoForm(8, 0, 0, 83));
        Assert.Equal("mfmsr", mfmsr.Mnemonic);
        AssertGpr(Assert.Single(mfmsr.Operands), "r8");

        var mtmsr = Decode(EncodeXoForm(9, 0, 0, 146));
        Assert.Equal("mtmsr", mtmsr.Mnemonic);
        AssertGpr(Assert.Single(mtmsr.Operands), "r9");

        var mtcrf = Decode((31u << 26) | (10u << 21) | (0x80u << 12) | (144u << 1));
        Assert.Equal("mtcrf", mtcrf.Mnemonic);
        Assert.Collection(
            mtcrf.Operands,
            op => Assert.Equal(0x80, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => AssertGpr(op, "r10"));

        var nor = Decode(EncodeXoForm(10, 11, 12, 124, rc: true));
        Assert.Equal("nor.", nor.Mnemonic);

        var nand = Decode(EncodeXoForm(13, 14, 15, 476));
        Assert.Equal("nand", nand.Mnemonic);

        var eqv = Decode(EncodeXoForm(13, 14, 15, 284, rc: true));
        Assert.Equal("eqv.", eqv.Mnemonic);

        var xor = Decode(EncodeXoForm(16, 17, 18, 316, rc: true));
        Assert.Equal("xor.", xor.Mnemonic);

        var mflr = Decode(EncodeSprWord(3, 8, 339));
        Assert.Equal("mflr", mflr.Mnemonic);
        AssertGpr(Assert.Single(mflr.Operands), "r3");

        var mfctr = Decode(EncodeSprWord(4, 9, 339));
        Assert.Equal("mfctr", mfctr.Mnemonic);

        var mfxer = Decode(EncodeSprWord(5, 1, 339));
        Assert.Equal("mfxer", mfxer.Mnemonic);

        var mfspr = Decode(EncodeSprWord(6, 912, 339));
        Assert.Equal("mfspr", mfspr.Mnemonic);
        Assert.Collection(
            mfspr.Operands,
            op => AssertGpr(op, "r6"),
            op => Assert.Equal(912, Assert.IsType<PpcImmediateOperand>(op).Value));

        var mtlr = Decode(EncodeSprWord(7, 8, 467));
        Assert.Equal("mtlr", mtlr.Mnemonic);
        AssertGpr(Assert.Single(mtlr.Operands), "r7");

        var mtctr = Decode(EncodeSprWord(8, 9, 467));
        Assert.Equal("mtctr", mtctr.Mnemonic);

        var mtxer = Decode(EncodeSprWord(9, 1, 467));
        Assert.Equal("mtxer", mtxer.Mnemonic);

        var mtspr = Decode(EncodeSprWord(10, 913, 467));
        Assert.Equal("mtspr", mtspr.Mnemonic);
        Assert.Collection(
            mtspr.Operands,
            op => Assert.Equal(913, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => AssertGpr(op, "r10"));

        var mtsr = Decode(EncodeXoForm(11, 3, 0, 210));
        Assert.Equal("mtsr", mtsr.Mnemonic);
        Assert.Collection(
            mtsr.Operands,
            op => Assert.Equal(3, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => AssertGpr(op, "r11"));

        Assert.Equal("mtsrin", Decode(EncodeXForm(11, 0, 12, 242)).Mnemonic);
    }

    [Fact]
    public void DecodesIndexedMemoryAndByteReverseForms()
    {
        Assert.Equal("lwzx", Decode(EncodeXForm(3, 4, 5, 23)).Mnemonic);
        Assert.Equal("lwzux", Decode(EncodeXForm(6, 7, 8, 55)).Mnemonic);
        Assert.Equal("lbzx", Decode(EncodeXForm(9, 10, 11, 87)).Mnemonic);
        Assert.Equal("stwx", Decode(EncodeXForm(12, 13, 14, 151)).Mnemonic);
        Assert.Equal("stwux", Decode(EncodeXForm(15, 16, 17, 183)).Mnemonic);
        Assert.Equal("stbx", Decode(EncodeXForm(18, 19, 20, 215)).Mnemonic);
        Assert.Equal("stbux", Decode(EncodeXForm(21, 22, 23, 247)).Mnemonic);
        Assert.Equal("lhzx", Decode(EncodeXForm(24, 25, 26, 279)).Mnemonic);
        Assert.Equal("lhzux", Decode(EncodeXForm(24, 25, 26, 311)).Mnemonic);
        Assert.Equal("lhax", Decode(EncodeXForm(24, 25, 26, 343)).Mnemonic);
        Assert.Equal("lhaux", Decode(EncodeXForm(24, 25, 26, 375)).Mnemonic);
        Assert.Equal("sthx", Decode(EncodeXForm(27, 28, 29, 407)).Mnemonic);
        Assert.Equal("sthux", Decode(EncodeXForm(27, 28, 29, 439)).Mnemonic);

        var stwcx = Decode(EncodeXForm(3, 4, 5, 150, rc: true));
        Assert.Equal("stwcx.", stwcx.Mnemonic);
        Assert.True(stwcx.IsConditionalBranch);

        Assert.Equal("lwbrx", Decode(EncodeXForm(3, 4, 5, 534)).Mnemonic);
        Assert.Equal("stwbrx", Decode(EncodeXForm(6, 7, 8, 662)).Mnemonic);
        Assert.Equal("lhbrx", Decode(EncodeXForm(9, 10, 11, 790)).Mnemonic);
        Assert.Equal("sthbrx", Decode(EncodeXForm(12, 13, 14, 918)).Mnemonic);

        var stfiwx = Decode(EncodeXForm(2, 3, 4, 983));
        Assert.Equal("stfiwx", stfiwx.Mnemonic);
        Assert.Collection(stfiwx.Operands, op => AssertFpr(op, "f2"), op => AssertGpr(op, "r3"), op => AssertGpr(op, "r4"));
    }

    [Fact]
    public void DecodesShiftFloatingAndCacheForms()
    {
        Assert.Equal("slw.", Decode(EncodeXForm(5, 6, 7, 24, rc: true)).Mnemonic);
        Assert.Equal("dcbst", Decode(EncodeXForm(0, 6, 7, 54)).Mnemonic);
        Assert.Equal("dcbf", Decode(EncodeXForm(0, 6, 7, 86)).Mnemonic);
        Assert.Equal("dcbtst", Decode(EncodeXForm(0, 6, 7, 246)).Mnemonic);
        Assert.Equal("dcbt", Decode(EncodeXForm(0, 6, 7, 278)).Mnemonic);
        Assert.Equal("tlbie", Decode(EncodeXForm(0, 0, 7, 306)).Mnemonic);
        Assert.Equal("dcbi", Decode(EncodeXForm(0, 6, 7, 470)).Mnemonic);
        Assert.Equal("mcrxr", Decode(EncodeXForm(28, 0, 0, 512)).Mnemonic);
        Assert.Equal("srw.", Decode(EncodeXForm(8, 9, 10, 536, rc: true)).Mnemonic);
        Assert.Equal("tlbsync", Decode(EncodeXForm(0, 0, 0, 566)).Mnemonic);

        var mfsr = Decode(EncodeXForm(8, 9, 0, 595));
        Assert.Equal("mfsr", mfsr.Mnemonic);
        Assert.Collection(
            mfsr.Operands,
            op => AssertGpr(op, "r8"),
            op => Assert.Equal(9, Assert.IsType<PpcImmediateOperand>(op).Value));

        Assert.Equal("mfsrin", Decode(EncodeXForm(8, 0, 9, 659)).Mnemonic);
        Assert.Equal("sync", Decode(EncodeXForm(0, 0, 0, 598)).Mnemonic);
        Assert.Equal("sraw.", Decode(EncodeXForm(11, 12, 13, 792, rc: true)).Mnemonic);

        var srawi = Decode(EncodeXForm(14, 15, 7, 824, rc: true));
        Assert.Equal("srawi.", srawi.Mnemonic);
        Assert.Collection(
            srawi.Operands,
            op => AssertGpr(op, "r15"),
            op => AssertGpr(op, "r14"),
            op => Assert.Equal(7, Assert.IsType<PpcImmediateOperand>(op).Value));

        var lfsx = Decode(EncodeXForm(1, 2, 3, 535));
        Assert.Equal("lfsx", lfsx.Mnemonic);
        Assert.Collection(lfsx.Operands, op => AssertFpr(op, "f1"), op => AssertGpr(op, "r2"), op => AssertGpr(op, "r3"));

        Assert.Equal("lfsux", Decode(EncodeXForm(1, 2, 3, 567)).Mnemonic);

        var lfdx = Decode(EncodeXForm(4, 5, 6, 599));
        Assert.Equal("lfdx", lfdx.Mnemonic);

        Assert.Equal("lfdux", Decode(EncodeXForm(4, 5, 6, 631)).Mnemonic);

        var stfsx = Decode(EncodeXForm(7, 8, 9, 663));
        Assert.Equal("stfsx", stfsx.Mnemonic);

        Assert.Equal("stfsux", Decode(EncodeXForm(7, 8, 9, 695)).Mnemonic);

        var stfdx = Decode(EncodeXForm(10, 11, 12, 727));
        Assert.Equal("stfdx", stfdx.Mnemonic);

        Assert.Equal("dcba", Decode(EncodeXForm(0, 11, 12, 758)).Mnemonic);

        Assert.Equal("stfdux", Decode(EncodeXForm(10, 11, 12, 759)).Mnemonic);

        Assert.Equal("eieio", Decode(EncodeXForm(0, 0, 0, 854)).Mnemonic);

        Assert.Equal("mftb", Decode(EncodeSprWord(13, 268, 371)).Mnemonic);
        Assert.Equal("mftbu", Decode(EncodeSprWord(14, 269, 371)).Mnemonic);

        var icbi = Decode(EncodeXForm(0, 13, 14, 982));
        Assert.Equal("icbi", icbi.Mnemonic);
        Assert.Collection(icbi.Operands, op => AssertGpr(op, "r13"), op => AssertGpr(op, "r14"));

        var dcbz = Decode(EncodeXForm(0, 15, 16, 1014));
        Assert.Equal("dcbz", dcbz.Mnemonic);

        var extsh = Decode(EncodeXForm(17, 18, 0, 922, rc: true));
        Assert.Equal("extsh.", extsh.Mnemonic);
        Assert.Collection(extsh.Operands, op => AssertGpr(op, "r18"), op => AssertGpr(op, "r17"));

        var extsb = Decode(EncodeXForm(19, 20, 0, 954, rc: true));
        Assert.Equal("extsb.", extsb.Mnemonic);
        Assert.Collection(extsb.Operands, op => AssertGpr(op, "r20"), op => AssertGpr(op, "r19"));
    }

    [Fact]
    public void FallsBackToXoMnemonicForUnknownCodes()
    {
        var instruction = Decode(EncodeXForm(1, 2, 3, 321));

        Assert.Equal("xo_321", instruction.Mnemonic);
        Assert.Empty(instruction.Operands);
        Assert.False(instruction.IsConditionalBranch);
    }
}
