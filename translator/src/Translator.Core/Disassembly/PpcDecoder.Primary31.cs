using System.Buffers.Binary;

namespace Translator.Core.Disassembly;

public static partial class PpcDecoder
{
    private static void DecodePrimary31(uint address, uint word, List<PpcOperand> operands, List<uint> branches,
        ref string mnemonic, ref bool isReturn, ref bool isCall, ref bool isCond)
    {
        // For XO-form arithmetic instructions, bit 21 (our bit 10) is the OE bit.
        // We extract both the full 10-bit XO and the 9-bit XO with separate OE.
        var xo10 = GetField(word, 1, 10);  // Full 10-bit XO for X-form instructions
        var xo = GetField(word, 1, 9);     // 9-bit XO for XO-form instructions
        var oe = ((word >> 10) & 1) != 0;  // OE bit (bit 21)
        var rc = (word & 1) != 0;
        var rs = GetField(word, 21);
        var ra = GetField(word, 16);
        var rb = GetField(word, 11);

        // Helper for arithmetic mnemonics with OE and Rc support
        string ArithMnemonic(string baseName) => baseName + (oe ? "o" : "") + (rc ? "." : "");

        switch (xo)
        {
            case 0: // cmp (cmpw)
            {
                if (xo10 == 512)
                {
                    mnemonic = "mcrxr";
                    operands.Add(CrField(rs / 4));
                    break;
                }

                var crfD = GetField(word, 23, 3);
                var lBit = GetField(word, 21, 1);
                if (lBit != 0)
                    mnemonic = "invalid_cmp";
                else
                    mnemonic = "cmpw";

                operands.Add(CrField(crfD));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                isCond = true;
                break;
            }

            case 4: // tw
                mnemonic = "tw";
                operands.Add(new PpcImmediateOperand(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 8: // subfc rD,rA,rB
                mnemonic = ArithMnemonic("subfc");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 10: // addc rD,rA,rB
                mnemonic = ArithMnemonic("addc");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 19: // mfcr rD
                mnemonic = "mfcr";
                operands.Add(Reg(rs));
                break;

            case 20: // lwarx rD,rA,rB
                mnemonic = "lwarx";
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;





            case 28: // and rA,rS,rB
                mnemonic = rc ? "and." : "and";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 26: // cntlzw rA,rS
                mnemonic = rc ? "cntlzw." : "cntlzw";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                break;

            case 32: // cmplw
            {
                var crfD = GetField(word, 23, 3);
                mnemonic = "cmplw";
                if (crfD != 0)
                    operands.Add(CrField(crfD));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                isCond = true;
                break;
            }

            case 60: // andc rA,rS,rB
                mnemonic = rc ? "andc." : "andc";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;



            case 83: // mfmsr rD
                if (xo10 == 595)
                {
                    mnemonic = "mfsr";
                    operands.Add(Reg(rs));
                    operands.Add(new PpcImmediateOperand(ra & 0xF));
                }
                else
                {
                    mnemonic = "mfmsr";
                    operands.Add(Reg(rs));
                }
                break;

            case 146: // mtmsr rS
                mnemonic = "mtmsr";
                operands.Add(Reg(rs));
                break;

            case 144: // mtcrf CRM,rS
                mnemonic = "mtcrf";
                operands.Add(new PpcImmediateOperand((int)((word >> 12) & 0xFF)));
                operands.Add(Reg(rs));
                break;



            case 124: // nor rA,rS,rB
                mnemonic = rc ? "nor." : "nor";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 476: // nand rA,rS,rB
                mnemonic = rc ? "nand." : "nand";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 104: // neg rD,rA
                mnemonic = ArithMnemonic("neg");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                break;

            case 136: // subfe rD,rA,rB
                mnemonic = ArithMnemonic("subfe");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;



            case 40: // subf rD,rA,rB
                mnemonic = ArithMnemonic("subf");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 232: // subfme rD,rA
                mnemonic = ArithMnemonic("subfme");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                break;





            case 202: // addze rD,rA
                mnemonic = ArithMnemonic("addze");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                break;

            case 234: // addme rD,rA
                mnemonic = ArithMnemonic("addme");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                break;

            case 235: // mullw rD,rA,rB
                mnemonic = ArithMnemonic("mullw");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 266: // add rD,rA,rB
                mnemonic = ArithMnemonic("add");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;



            case 316: // xor rA,rS,rB
                mnemonic = rc ? "xor." : "xor";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 284: // eqv rA,rS,rB
                mnemonic = rc ? "eqv." : "eqv";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 339: // mfspr rD,SPR
                var spr = DecodeSpr(word);
                mnemonic = spr switch
                {
                    8 => "mflr",
                    9 => "mfctr",
                    1 => "mfxer",
                    _ => "mfspr"
                };
                operands.Add(Reg(rs));
                if (mnemonic == "mfspr")
                {
                    operands.Add(new PpcImmediateOperand(spr));
                }
                break;





            case 412: // orc rA,rS,rB
                mnemonic = rc ? "orc." : "orc";
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;



            case 444: // or / mr alias
                mnemonic = (!rc && rs == rb) ? "mr" : (rc ? "or." : "or");
                operands.Add(Reg(ra));
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 11: // mulhwu rD,rA,rB
                mnemonic = rc ? "mulhwu." : "mulhwu";
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 75: // mulhw rD,rA,rB (signed multiply high word)
                mnemonic = rc ? "mulhw." : "mulhw";
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 459: // divwu rD,rA,rB
                mnemonic = ArithMnemonic("divwu");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            case 467: // mtspr SPR,rS
                spr = DecodeSpr(word);
                mnemonic = spr switch
                {
                    8 => "mtlr",
                    9 => "mtctr",
                    1 => "mtxer",
                    _ => "mtspr"
                };
                if (mnemonic == "mtspr")
                {
                    operands.Add(new PpcImmediateOperand(spr));
                    operands.Add(Reg(rs));
                }
                else
                {
                    operands.Add(Reg(rs));
                }
                break;

            case 210: // mtsr SR,rS
                mnemonic = "mtsr";
                operands.Add(new PpcImmediateOperand(ra & 0xF));
                operands.Add(Reg(rs));
                break;

            case 242: // mtsrin rS,rB
                mnemonic = "mtsrin";
                operands.Add(Reg(rs));
                operands.Add(Reg(rb));
                break;

            case 491: // divw rD,rA,rB
                mnemonic = ArithMnemonic("divw");
                operands.Add(Reg(rs));
                operands.Add(Reg(ra));
                operands.Add(Reg(rb));
                break;

            default:
                // X-form instructions use the full 10-bit XO (no OE bit)
                // Handle them here with a secondary switch on xo10
                switch (xo10)
                {
                    case 150 when rc: // stwcx. rS,rA,rB
                        mnemonic = "stwcx.";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        isCond = true;
                        break;

                    case 23: // lwzx rD,rA,rB
                        mnemonic = "lwzx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 24: // slw rA,rS,rB
                        mnemonic = rc ? "slw." : "slw";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        operands.Add(Reg(rb));
                        break;

                    case 54: // dcbst rA,rB
                        mnemonic = "dcbst";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 86: // dcbf rA,rB
                        mnemonic = "dcbf";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 55: // lwzux rD,rA,rB
                        mnemonic = "lwzux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 87: // lbzx rD,rA,rB
                        mnemonic = "lbzx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 151: // stwx rS,rA,rB
                        mnemonic = "stwx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 183: // stwux rS,rA,rB
                        mnemonic = "stwux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 246: // dcbtst rA,rB
                        mnemonic = "dcbtst";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 215: // stbx rS,rA,rB
                        mnemonic = "stbx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 247: // stbux rS,rA,rB
                        mnemonic = "stbux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 278: // dcbt rA,rB
                        mnemonic = "dcbt";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 310: // eciwx rD,rA,rB
                        mnemonic = "eciwx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 306: // tlbie rB
                        mnemonic = "tlbie";
                        operands.Add(Reg(rb));
                        break;

                    case 279: // lhzx rD,rA,rB
                        mnemonic = "lhzx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 311: // lhzux rD,rA,rB
                        mnemonic = "lhzux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 343: // lhax rD,rA,rB
                        mnemonic = "lhax";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 375: // lhaux rD,rA,rB
                        mnemonic = "lhaux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 407: // sthx rS,rA,rB
                        mnemonic = "sthx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 438: // ecowx rS,rA,rB
                        mnemonic = "ecowx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 439: // sthux rS,rA,rB
                        mnemonic = "sthux";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 470: // dcbi rA,rB
                        mnemonic = "dcbi";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 536: // srw rA,rS,rB
                        mnemonic = rc ? "srw." : "srw";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        operands.Add(Reg(rb));
                        break;

                    case 598: // sync
                        mnemonic = "sync";
                        break;

                    case 566: // tlbsync
                        mnemonic = "tlbsync";
                        break;

                    case 533: // lswx rD,rA,rB
                        mnemonic = "lswx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 597: // lswi rD,rA,NB
                        mnemonic = "lswi";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(new PpcImmediateOperand((int)rb));
                        break;

                    case 792: // sraw rA,rS,rB
                        mnemonic = rc ? "sraw." : "sraw";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        operands.Add(Reg(rb));
                        break;

                    case 824: // srawi rA,rS,SH
                        var sh5 = GetField(word, 11, 5);
                        mnemonic = rc ? "srawi." : "srawi";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        operands.Add(new PpcImmediateOperand(sh5));
                        break;

                    case 535: // lfsx frD,rA,rB
                        mnemonic = "lfsx";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 567: // lfsux frD,rA,rB
                        mnemonic = "lfsux";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 599: // lfdx frD,rA,rB
                        mnemonic = "lfdx";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 631: // lfdux frD,rA,rB
                        mnemonic = "lfdux";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 663: // stfsx frS,rA,rB
                        mnemonic = "stfsx";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 661: // stswx rS,rA,rB
                        mnemonic = "stswx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 695: // stfsux frS,rA,rB
                        mnemonic = "stfsux";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 725: // stswi rS,rA,NB
                        mnemonic = "stswi";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(new PpcImmediateOperand((int)rb));
                        break;

                    case 727: // stfdx frS,rA,rB
                        mnemonic = "stfdx";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 758: // dcba rA,rB
                        mnemonic = "dcba";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 759: // stfdux frS,rA,rB
                        mnemonic = "stfdux";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 854: // eieio
                        mnemonic = "eieio";
                        break;

                    case 371: // mftb rD,TBR
                        var tbr = DecodeSpr(word);
                        mnemonic = tbr == 269 ? "mftbu" : "mftb";
                        operands.Add(Reg(rs));
                        if (tbr is not (268 or 269))
                        {
                            operands.Add(new PpcImmediateOperand(tbr));
                        }
                        break;

                    case 534: // lwbrx rD,rA,rB
                        mnemonic = "lwbrx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 662: // stwbrx rS,rA,rB
                        mnemonic = "stwbrx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 659: // mfsrin rD,rB
                        mnemonic = "mfsrin";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(rb));
                        break;

                    case 790: // lhbrx rD,rA,rB
                        mnemonic = "lhbrx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 918: // sthbrx rS,rA,rB
                        mnemonic = "sthbrx";
                        operands.Add(Reg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 982: // icbi rA,rB
                        mnemonic = "icbi";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 983: // stfiwx frS,rA,rB
                        mnemonic = "stfiwx";
                        operands.Add(FReg(rs));
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 1014: // dcbz rA,rB
                        mnemonic = "dcbz";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rb));
                        break;

                    case 922: // extsh rA,rS
                        mnemonic = rc ? "extsh." : "extsh";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        break;

                    case 954: // extsb rA,rS
                        mnemonic = rc ? "extsb." : "extsb";
                        operands.Add(Reg(ra));
                        operands.Add(Reg(rs));
                        break;

                    default:
                        mnemonic = $"xo_{xo10}";
                        break;
                }
                break;
        }
    }
}
