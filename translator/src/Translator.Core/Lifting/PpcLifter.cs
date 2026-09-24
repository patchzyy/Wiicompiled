using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

/// <summary> 
/// Very small lifter...
/// </summary>
public sealed partial class PpcLifter
{
    public IReadOnlyList<LiftedInstruction> Lift(
        IReadOnlyList<PpcInstruction> instructions,
        bool allowUnsupported = false,
        uint? functionEntryPoint = null,
        IReadOnlySet<uint>? knownFunctionEntryPoints = null)
    {
        var validAddresses = new HashSet<uint>(instructions.Select(i => i.Address));
        var instructionsByAddress = instructions.ToDictionary(i => i.Address);
        var result = new List<LiftedInstruction>(instructions.Count);
        foreach (var ins in instructions)
        {
            var ir = LiftInstruction(
                ins,
                validAddresses,
                instructionsByAddress,
                allowUnsupported,
                functionEntryPoint,
                knownFunctionEntryPoints);
            result.Add(new LiftedInstruction(ins, ir));
        }
        return result;
    }

    private static string NormalizeRegister(string name)
    {
        // Capstone sometimes reports condition register fields as the starting bit (e.g., cr28)
        // instead of the field index (cr7). Normalize these so SSA sees consistent names.
        if (name.StartsWith("cr", StringComparison.OrdinalIgnoreCase))
        {
            var rest = name.AsSpan(2);
            if (int.TryParse(rest, NumberStyles.Integer, CultureInfo.InvariantCulture, out var val) && val >= 8)
            {
                return $"cr{val / 4}";
            }
        }

        return name;
    }

    private static string RegisterOperandName(IReadOnlyList<PpcOperand> ops, int idx) =>
        ops[idx] is PpcRegisterOperand r
            ? NormalizeRegister(r.Name)
            : throw new FormatException($"Operand {idx} is not a register");

    private static int ImmediateOperandValue(IReadOnlyList<PpcOperand> ops, int idx) => ops[idx] switch
    {
        PpcImmediateOperand imm => imm.Value,
        _ => throw new FormatException($"Operand {idx} is not an immediate")
    };

    /// <summary>
    /// EABI argument registers in call order. Shared across every call site since
    /// <see cref="IrValue"/> is immutable and <c>IrCall.Arguments</c> is read-only.
    /// </summary>
    internal static readonly IrValue[] AbiCallArguments = BuildAbiCallArguments();

    private static IrValue[] BuildAbiCallArguments()
    {
        var args = new IrValue[8 + 13];
        var next = 0;
        for (var i = 3; i <= 10; i++) args[next++] = IrValue.Register($"r{i}");
        for (var i = 1; i <= 13; i++) args[next++] = IrValue.Register($"f{i}");
        return args;
    }

    private static IReadOnlyList<IrInstruction> LiftInstruction(
        PpcInstruction ins,
        HashSet<uint> validAddresses,
        IReadOnlyDictionary<uint, PpcInstruction> instructionsByAddress,
        bool allowUnsupported,
        uint? functionEntryPoint,
        IReadOnlySet<uint>? knownFunctionEntryPoints)
    {
        var mnemonic = ins.Mnemonic.ToLowerInvariant();
        var ops = ins.Operands ?? Array.Empty<PpcOperand>();

        // Local functions, not delegates: keeps the captured operand list in a by-ref struct
        // closure instead of heap-allocating a closure object plus two delegates per instruction.
        string Reg(int idx) => RegisterOperandName(ops, idx);
        int Imm(int idx) => ImmediateOperandValue(ops, idx);

        // Legacy string view to minimize churn in the existing switch body. The numeric helpers
        // above should be used for anything that needs values, not parsing these strings.
        var operands = ops.Select(o => NormalizeRegister(o.ToOperandString())).ToList();

        var pairedSingle = TryLiftPairedSingle(ins, mnemonic, ops, operands);
        if (pairedSingle != null)
        {
            return pairedSingle;
        }

        var floating = TryLiftFloating(ins, mnemonic, ops, operands);
        if (floating != null)
        {
            return floating;
        }

        try
        {
            switch (mnemonic)
            {
            case "nop" when ops.Count == 0:
                {
                    //pass / ignore
                    return Array.Empty<IrInstruction>();
                }

            case "li" when ops.Count == 2:
                return new[] { new IrAssign(Reg(0), IrValue.Imm(SignExtend16(Imm(1)))) };

            case "lis" when ops.Count == 2:
                {
                    // lis rD, SIMM -> rD = SIMM << 16
                    var simm = Imm(1);
                    return new[] { new IrAssign(Reg(0), IrValue.Imm(simm << 16)) };
                }

            case "addi" when ops.Count == 3:
                return new[] { new IrBinary(Reg(0), BaseOrZero(Reg(1)), IrValue.Imm(SignExtend16(Imm(2))), "add") };

            case "mr":
            case "or" when operands.Count == 3 && operands[1] == operands[2]:
                return new[] { new IrAssign(Reg(0), IrValue.Register(Reg(1))) };

            case "or" when operands.Count == 3:
            case "or." when operands.Count == 3:
                return BinWithCrFlag(Reg(0), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), "or", mnemonic.EndsWith('.'));

            case "orc" when operands.Count == 3:
            case "orc." when operands.Count == 3:
                return BinWithCrFlag(Reg(0), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), "orc", mnemonic.EndsWith('.'));

            case "oris" when operands.Count == 3:
                {
                    var uimm = MaskUimm(Imm(2)) << 16;
                    return new[] { new IrBinary(Reg(0), IrValue.Register(Reg(1)), IrValue.Imm(uimm), "or") };
                }

            case "xoris" when operands.Count == 3:
                {
                    var uimm = MaskUimm(Imm(2)) << 16;
                    return new[] { new IrBinary(Reg(0), IrValue.Register(Reg(1)), IrValue.Imm(uimm), "xor") };
                }

            case "xori" when operands.Count == 3:
                {
                    var uimm = Imm(2) & 0xFFFF;
                    return new[] { new IrBinary(Reg(0), IrValue.Register(Reg(1)), IrValue.Imm(uimm), "xor") };
                }

            case "xori." when operands.Count == 3:
                {
                    var uimm = Imm(2) & 0xFFFF;
                    return BinWithCrFlag(Reg(0), IrValue.Register(Reg(1)), IrValue.Imm(uimm), "xor", true);
                }

            case "mfcr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("cr")) };

            case "sync":
            case "isync":
            case "eieio":
            case "xo_598": // sync
                // Memory barrier/sync instructions - no-op in HLE (no real cache)
                return new[] { new IrComment($"{mnemonic} @ 0x{ins.Address:X8} (no-op)") };

            case "addc" when operands.Count == 3:
            case "addc." when operands.Count == 3:
                {
                    var dest = Reg(0);
                    var left = Reg(1);
                    var right = Reg(2);
                    var instructions = new List<IrInstruction>();
                    var leftValue = PreserveIfAliased(dest, left, "addc_left", instructions);
                    var rightValue = PreserveIfAliased(dest, right, "addc_right", instructions);

                    instructions.Add(new IrBinary(dest, IrValue.Register(leftValue), IrValue.Register(rightValue), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(leftValue),
                        IrValue.Register(rightValue),
                        IrValue.Imm(0)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "addco" when operands.Count == 3:
            case "addco." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Addco", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "subfc" when operands.Count == 3:
            case "subfc." when operands.Count == 3:
                {
                    // subfc rD, rA, rB -> rD = rB - rA, CA = carry from subtraction (1 when no borrow)
                    var dest = Reg(0);
                    var minuend = Reg(2);   // rB
                    var subtrahend = Reg(1); // rA
                    var instructions = new List<IrInstruction>();
                    var minuendValue = PreserveIfAliased(dest, minuend, "subfc_min", instructions);
                    var subtrahendValue = PreserveIfAliased(dest, subtrahend, "subfc_sub", instructions);

                    instructions.Add(new IrBinary(dest, IrValue.Register(minuendValue), IrValue.Register(subtrahendValue), "sub"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarrySub", new[]
                    {
                        IrValue.Register(minuendValue),
                        IrValue.Register(subtrahendValue)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "subfco" when operands.Count == 3:
            case "subfco." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Subfco", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "lmw" when operands.Count == 2:
                {
                    var startReg = ParseRegisterNumber(operands[0]);
                    var (offset, baseReg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    for (int i = startReg; i <= 31; i++)
                    {
                        instructions.AddRange(DFormLoad($"r{i}", baseReg, offset + (i - startReg) * 4, 4));
                    }
                    return instructions;
                }

            case "srw" when operands.Count == 3:
            case "srw." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var shReg = operands[2];

                    var list = new List<IrInstruction>
                    {
                        new IrBinary(dest, IrValue.Register(src), IrValue.Register(shReg), "ppc_srw")
                    };

                    if (mnemonic.EndsWith('.'))
                        list.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));

                    return list;
                }

            case "divwu" when operands.Count == 3:
            case "divwu." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "divu", mnemonic.EndsWith('.'));

            case "divwuo" when operands.Count == 3:
            case "divwuo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Divwuo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "mullw" when operands.Count == 3:
            case "mullw." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mul", mnemonic.EndsWith('.'));

            case "mullwo" when operands.Count == 3:
            case "mullwo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Mullwo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "lbzx" when operands.Count == 3:
                {
                    var rD = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    return BuildIndexedAddress(rA, rB, addr, a => new[] { new IrLoad(rD, new IrAddress(a, 0), 1) });
                }

            case "lwarx" when operands.Count == 3:
                {
                    var addr = $"addr_lwarx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(Reg(0), "PPC_Lwarx", new[] { IrValue.Register(a) })
                    });
                }

            case "lswx" when operands.Count == 3:
                {
                    var rD = ParseRegisterNumber(Reg(0));
                    var addr = $"addr_lswx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(string.Empty, "PPC_Lswx", new[] { IrValue.Imm(rD), IrValue.Register(a) })
                    });
                }

            case "lswi" when operands.Count == 3:
                {
                    var rD = ParseRegisterNumber(Reg(0));
                    var rA = Reg(1);
                    var nb = ParseImmediate(operands[2]);
                    var addr = $"addr_lswi_{ins.Address:X8}_loc";
                    IrInstruction address = rA == "r0"
                        ? new IrAssign(addr, IrValue.Imm(0))
                        : new IrAssign(addr, IrValue.Register(rA));
                    return new IrInstruction[]
                    {
                        address,
                        new IrCall(string.Empty, "PPC_Lswi", new[] { IrValue.Imm(rD), IrValue.Register(addr), IrValue.Imm(nb) })
                    };
                }

            case "eciwx" when operands.Count == 3:
                {
                    var addr = $"addr_eciwx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(Reg(0), "PPC_Eciwx", new[] { IrValue.Register(a) })
                    });
                }

            case "lbzux":
            case "xo_119":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"lbzux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_lbzux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"r{rD}", new IrAddress(addr, 0), 1)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "stbx" when operands.Count == 3:
                {
                    var rS = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    // FIX: Use BuildIndexedAddress to handle r0 correctly
                    return BuildIndexedAddress(rA, rB, addr, a => new[] { new IrStore(new IrAddress(a, 0), IrValue.Register(rS), 1) });
                }

            case "stwcx." when operands.Count == 3:
                {
                    var addr = $"addr_stwcx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(string.Empty, "PPC_Stwcx", new[] { IrValue.Register(a), IrValue.Register(Reg(0)) })
                    });
                }

            case "stswx" when operands.Count == 3:
                {
                    var rS = ParseRegisterNumber(Reg(0));
                    var addr = $"addr_stswx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(string.Empty, "PPC_Stswx", new[] { IrValue.Imm(rS), IrValue.Register(a) })
                    });
                }

            case "stswi" when operands.Count == 3:
                {
                    var rS = ParseRegisterNumber(Reg(0));
                    var rA = Reg(1);
                    var nb = ParseImmediate(operands[2]);
                    var addr = $"addr_stswi_{ins.Address:X8}_loc";
                    IrInstruction address = rA == "r0"
                        ? new IrAssign(addr, IrValue.Imm(0))
                        : new IrAssign(addr, IrValue.Register(rA));
                    return new IrInstruction[]
                    {
                        address,
                        new IrCall(string.Empty, "PPC_Stswi", new[] { IrValue.Imm(rS), IrValue.Register(addr), IrValue.Imm(nb) })
                    };
                }

            case "ecowx" when operands.Count == 3:
                {
                    var addr = $"addr_ecowx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress(Reg(1), Reg(2), addr, a => new[]
                    {
                        new IrCall(string.Empty, "PPC_Ecowx", new[] { IrValue.Register(a), IrValue.Register(Reg(0)) })
                    });
                }

            case "stbux":
            case "xo_247":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    
                    if (rA == 0)
                    {
                        throw new InvalidOperationException($"stbux with rA=0 is invalid @ 0x{ins.Address:X8}");
                    }

                    string addrReg = $"addr_stbux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"r{rS}"), 1)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "extsb" when operands.Count == 2:
            case "extsb." when operands.Count == 2:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Imm(8), "sext", mnemonic.EndsWith('.'));

            case "adde" when operands.Count == 3:
            case "adde." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var left = operands[1];
                    var right = operands[2];
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var leftValue = PreserveIfAliased(dest, left, "adde_left", instructions);
                    var rightValue = PreserveIfAliased(dest, right, "adde_right", instructions);

                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(leftValue), IrValue.Register(rightValue), "add"));
                    instructions.Add(new IrBinary(dest, IrValue.Register(dest), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(leftValue),
                        IrValue.Register(rightValue),
                        IrValue.Register(carry)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "addeo" when operands.Count == 3:
            case "addeo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Addeo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "addze" when operands.Count == 2:
            case "addze." when operands.Count == 2:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased(dest, src, "addze_src", instructions);

                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(srcValue), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(srcValue),
                        IrValue.Imm(0),
                        IrValue.Register(carry)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "addzeo" when operands.Count == 2:
            case "addzeo." when operands.Count == 2:
                return CallUnaryWithCr(Reg(0), "PPC_Addzeo", Reg(1), mnemonic.EndsWith('.'));

            case "addme" when operands.Count == 2:
            case "addme." when operands.Count == 2:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased(dest, src, "addme_src", instructions);

                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(srcValue), IrValue.Imm(-1), "add"));
                    instructions.Add(new IrBinary(dest, IrValue.Register(dest), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(srcValue),
                        IrValue.Imm(-1),
                        IrValue.Register(carry)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "addmeo" when operands.Count == 2:
            case "addmeo." when operands.Count == 2:
                return CallUnaryWithCr(Reg(0), "PPC_Addmeo", Reg(1), mnemonic.EndsWith('.'));

            case "subfze" when operands.Count == 2:
            case "subfze." when operands.Count == 2:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var tmpNot = $"{dest}_not";
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased(dest, src, "subfze_src", instructions);

                    instructions.Add(new IrBinary(tmpNot, IrValue.Register(srcValue), IrValue.Imm(0), "not"));
                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(tmpNot), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(tmpNot),
                        IrValue.Register(carry),
                        IrValue.Imm(0)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "subfzeo" when operands.Count == 2:
            case "subfzeo." when operands.Count == 2:
                return CallUnaryWithCr(Reg(0), "PPC_Subfzeo", Reg(1), mnemonic.EndsWith('.'));

            case "subfme" when operands.Count == 2:
            case "subfme." when operands.Count == 2:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var tmpNot = $"{dest}_not";
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased(dest, src, "subfme_src", instructions);

                    instructions.Add(new IrBinary(tmpNot, IrValue.Register(srcValue), IrValue.Imm(0), "not"));
                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(tmpNot), IrValue.Imm(-1), "add"));
                    instructions.Add(new IrBinary(dest, IrValue.Register(dest), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(tmpNot),
                        IrValue.Imm(-1),
                        IrValue.Register(carry)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "subfmeo" when operands.Count == 2:
            case "subfmeo." when operands.Count == 2:
                return CallUnaryWithCr(Reg(0), "PPC_Subfmeo", Reg(1), mnemonic.EndsWith('.'));

            case "dcbz_l" when operands.Count == 2:
            case "dcbz" when operands.Count == 2:
                {
                    var rA = operands[0];
                    var rB = operands[1];
                    var addrReg = $"{rB}_addr_dcbz";

                    // If rA is r0, EA = rB, otherwise EA = rA + rB
                    IrInstruction addrCalc = rA == "r0"
                        ? new IrAssign(addrReg, IrValue.Register(rB))
                        : new IrBinary(addrReg, IrValue.Register(rA), IrValue.Register(rB), "add");

                    var aligned = $"{addrReg}_al";
                    return new IrInstruction[]
                    {
                        addrCalc,
                        new IrBinary(aligned, IrValue.Register(addrReg), IrValue.Imm(unchecked((int)~31)), "and"),
                        new IrCall(string.Empty, "memset_zero_32", new[] { IrValue.Register(aligned) })
                    };
                }

            case "icbi":
            case "dcbi":
            case "dcbf":
                // Cache invalidate/flush - no-op in HLE (no real cache)
                return new[] { new IrComment($"{mnemonic} @ 0x{ins.Address:X8} (no-op)") };

            case "lfd" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormLoad(operands[0], reg, offset, 8);
                }

            case "lfdu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var addr = $"{reg}_addr";
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException($"lfdu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(addr, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrLoad(operands[0], new IrAddress(addr, 0), 8));
                    instructions.Add(new IrAssign(reg, IrValue.Register(addr)));
                    return instructions;
                }

            case "lfs" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormLoad(operands[0], reg, offset, 4);
                }

            case "lfsu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException($"lfsu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrLoad(operands[0], new IrAddress(reg, 0), 4));
                    return instructions;
                }


            case "rfi":
                // Return From Interrupt: in HLE, treat as return since exception vectors
                // are typically stubbed. The OS would restore state from context.
                return new[] { new IrReturn(null) };

            case "neg" when operands.Count == 2:
            case "neg." when operands.Count == 2:
                return BinWithCrFlag(operands[0], IrValue.Imm(0), IrValue.Register(operands[1]), "sub", mnemonic.EndsWith('.'));

            case "nego" when operands.Count == 2:
            case "nego." when operands.Count == 2:
                return CallUnaryWithCr(Reg(0), "PPC_Nego", Reg(1), mnemonic.EndsWith('.'));

            case "cmpw" or "cmpd" when operands.Count == 2:
                return LiftCompare("cr0", IrValue.Register(operands[0]), IrValue.Register(operands[1]), "sub", isUnsigned: false);

            case "cmpw" or "cmpd" when operands.Count == 3:
                return LiftCompare(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "sub", isUnsigned: false);

            case "bcctr":
            case "bcctrl":
                {
                    var rawInstr = ReadRawInstruction(ins);
                    var bo = (int)((rawInstr >> 21) & 0x1F);
                    var bi = (int)((rawInstr >> 16) & 0x1F);
                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    var targetLabel = mnemonic == "bcctr"
                        ? $"indirect_ctr_{ins.Address:X8}"
                        : $"call_ctr_{ins.Address:X8}_{ins.EndAddress:X8}";

                    return new[] { new IrBranch("raw", targetLabel, fallthrough, BuildBoConditionExpression(bo, bi, allowCtr: false)) };
                }

            case "bctr":
                if (ins.BranchTargets.Count > 0)
                {
                    var cases = ins.BranchTargets
                        .Distinct()
                        .Select(addr => new IrJumpTableCase(addr, $"0x{addr:X8}"))
                        .ToList();
                    return new IrInstruction[] { new IrJumpTable("ctr", cases) };
                }

                // bctr (without link) is an indirect JUMP, not a call.
                // Typically used for switch statements jumping within the same function.
                return new[] { new IrIndirectJump(IrValue.Register("ctr")) };

            case "ori" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(MaskUimm(ParseImmediate(operands[2]))), "or") };

            case "nor" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "nor") };

            case "nand" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "nand") };

            case "nand." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "nand", true);

            case "eqv" when operands.Count == 3:
            case "eqv." when operands.Count == 3:
                return BinWithCrFlag(Reg(0), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), "eqv", mnemonic.EndsWith('.'));

            case "mfdar" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };

            case "mfmsr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("msr")) };

            case "mfpvr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };

            case "mfdsisr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };

            case "mtcrf" when operands.Count == 2:
                {
                    var crm = ParseImmediate(operands[0]);
                    return LiftMtcrfMasked(crm, operands[1]);
                }

            case "mcrxr" when operands.Count == 1:
                return new[] { new IrCall("cr", "PPC_Mcrxr", new[] { IrValue.Imm(ParseCrFieldName(operands[0])) }) };

            case "mcrfs" when operands.Count == 2:
                return new[] { new IrCall("cr", "PPC_Mcrfs", new[] { IrValue.Imm(ParseCrFieldName(operands[0])), IrValue.Imm(ParseCrFieldName(operands[1])) }) };

            case "extsh" when operands.Count == 2:
            case "extsh." when operands.Count == 2:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Imm(16), "sext", mnemonic.EndsWith('.'));

            case "sraw" when operands.Count == 3:
            case "sraw." when operands.Count == 3:
                {
                    var rA = operands[0];
                    var rS = operands[1];
                    var rB = operands[2];
                    var instructions = new List<IrInstruction>();
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryShiftRight", new[]
                    {
                        IrValue.Register(rS),
                        IrValue.Register(rB)
                    }));
                    instructions.Add(new IrBinary(rA, IrValue.Register(rS), IrValue.Register(rB), "ppc_sraw"));
                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(rA), IrValue.Imm(0), false));
                    }
                    return instructions;
                }

            case "crclr" when operands.Count == 1:
                {
                    var bit = ParseCrBitIndex(operands[0]);
                    return new[]
                    {
                        new IrCall("cr", "PPC_CrSetBit", new[]
                        {
                            IrValue.Imm(bit),
                            IrValue.Imm(0)
                        })
                    };
                }

            case "mcrf" when operands.Count == 2:
                {
                    var destField = ParseCrFieldName(operands[0]);
                    var srcField = ParseCrFieldName(operands[1]);
                    return new[]
                    {
                        new IrCall("cr", "PPC_Mcrf", new[]
                        {
                            IrValue.Imm(destField),
                            IrValue.Imm(srcField)
                        })
                    };
                }

            case "crnor":
            case "crandc":
            case "crxor":
            case "crnand":
            case "crand":
            case "creqv":
            case "crorc":
            case "cror":
                return LiftCrLogical(mnemonic, operands[0], operands[1], operands[2]);

            case "lwz" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormLoad(operands[0], reg, offset, 4);
                }

            case "stw" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormStore(reg, offset, IrValue.Register(operands[0]), 4);
                }

            case "stwu" when operands.Count == 2:
                {
                    // stwu rS, d(rA) -> Store rS at d(rA), then rA = rA + d
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>(DFormStore(reg, offset, IrValue.Register(operands[0]), 4));
                    if (reg == "r0")
                        throw new InvalidOperationException($"stwu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    return instructions;
                }

            case "cmplwi" when operands.Count >= 2:
                {
                    var dest = operands.Count == 3 ? operands[0] : "cr0";
                    var rA = operands.Count == 3 ? operands[1] : operands[0];
                    var uimm = operands.Count == 3 ? operands[2] : operands[1];
                    return LiftCompare(dest, IrValue.Register(rA), IrValue.Imm(MaskUimm(ParseImmediate(uimm))), "sub_u", isUnsigned: true);
                }

            case "cmpwi" when operands.Count >= 2:
                {
                    var dest = operands.Count == 3 ? operands[0] : "cr0";
                    var rA = operands.Count == 3 ? operands[1] : operands[0];
                    var simm = operands.Count == 3 ? operands[2] : operands[1];
                    return LiftCompare(dest, IrValue.Register(rA), IrValue.Imm(SignExtend16(ParseImmediate(simm))), "sub", isUnsigned: false);
                }

            case "cmplw" when operands.Count == 2:
                return LiftCompare("cr0", IrValue.Register(operands[0]), IrValue.Register(operands[1]), "sub_u", isUnsigned: true);

            case "cmplw" when operands.Count == 3:
                return LiftCompare(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "sub_u", isUnsigned: true);

            case "mficcr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("iccr")) };

            case "mticcr" when operands.Count == 1:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}' @ 0x{ins.Address:X8}");

            case "mttbu" when operands.Count == 1:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}' @ 0x{ins.Address:X8}");

            case "mttbl" when operands.Count == 1:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}' @ 0x{ins.Address:X8}");

            case "mtdar" when operands.Count == 1:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}' @ 0x{ins.Address:X8}");

            case "mtdsisr" when operands.Count == 1:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}' @ 0x{ins.Address:X8}");

            case "mfibatu" when operands.Count == 2:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };

            case "mtibatu" when operands.Count == 2:
            case "mtibatl" when operands.Count == 2:
            case "mtdbatu" when operands.Count == 2:
            case "mtdbatl" when operands.Count == 2:
                throw new NotImplementedException($"UNIMPLEMENTED instruction '{mnemonic} {operands[0]}, {operands[1]}' @ 0x{ins.Address:X8}");

            case "mfibatl" when operands.Count == 2:
            case "mfdbatu" when operands.Count == 2:
            case "mfdbatl" when operands.Count == 2:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };

            case "mtxer" when operands.Count == 1:
                return new[] { new IrAssign("xer", IrValue.Register(operands[0])) };

            case "mfxer" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("xer")) };

            case "divw" when operands.Count == 3:
            case "divw." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "div", mnemonic.EndsWith('.'));

            case "divwo" when operands.Count == 3:
            case "divwo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Divwo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "lha" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>(DFormLoad(operands[0], reg, offset, 2))
                    {
                        new IrBinary(operands[0], IrValue.Register(operands[0]), IrValue.Imm(16), "shl"),
                        new IrBinary(operands[0], IrValue.Register(operands[0]), IrValue.Imm(16), "sar")
                    };
                    return instructions;
                }

            case "lhau":
            case "opc_43":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    short d = (short)(rawInstr & 0xFFFF);

                    if (rA == 0)
                        throw new InvalidOperationException($"lhau with rA=0 is invalid @ 0x{ins.Address:X8}");

                    // EA = (rA) + d -> update rA
                    // rD = EXTS(MEM(EA, 2))
                    
                    return new IrInstruction[] 
                    {
                        new IrBinary($"r{rA}", IrValue.Register($"r{rA}"), IrValue.Imm(d), "add"),
                        new IrLoad($"r{rD}", new IrAddress($"r{rA}", 0), 2),
                        new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "shl"),
                        new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "sar")
                    };
                }

            case "lhz" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormLoad(operands[0], reg, offset, 2);
                }

            case "lhzu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException($"lhzu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrLoad(operands[0], new IrAddress(reg, 0), 2));
                    return instructions;
                }

            case "lbz" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormLoad(operands[0], reg, offset, 1);
                }

            case "lbzu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException($"lbzu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrLoad(operands[0], new IrAddress(reg, 0), 1));
                    return instructions;
                }

            case "stb" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormStore(reg, offset, IrValue.Register(operands[0]), 1);
                }

            case "stbu" when operands.Count == 2:
                {
                    // stbu rS, d(rA) -> EA = (rA) + d, MEM(EA, 1) = rS, rA = EA
                    // Fix: compute EA into temp first to preserve rS if rS == rA
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    if (reg == "r0")
                        throw new InvalidOperationException($"stbu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    var ea = $"{reg}_stbu_ea";
                    return new IrInstruction[]
                    {
                        new IrBinary(ea, IrValue.Register(reg), IrValue.Imm(offset), "add"),
                        new IrStore(new IrAddress(ea, 0), IrValue.Register(operands[0]), 1),
                        new IrAssign(reg, IrValue.Register(ea))
                    };
                }

            case "mtmsr" when operands.Count == 1:
                return new[] { new IrAssign("msr", IrValue.Register(operands[0])) };

            case "sth" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormStore(reg, offset, IrValue.Register(operands[0]), 2);
                }

            case "sthu" when operands.Count == 2:
                {
                    // sthu rS, d(rA) -> EA = (rA) + d, MEM(EA, 2) = rS, rA = EA
                    // Fix: compute EA into temp first to preserve rS if rS == rA
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    if (reg == "r0")
                        throw new InvalidOperationException($"sthu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    var ea = $"{reg}_sthu_ea";
                    return new IrInstruction[]
                    {
                        new IrBinary(ea, IrValue.Register(reg), IrValue.Imm(offset), "add"),
                        new IrStore(new IrAddress(ea, 0), IrValue.Register(operands[0]), 2),
                        new IrAssign(reg, IrValue.Register(ea))
                    };
                }

            case "stmw" when operands.Count == 2:
                {
                    var startReg = ParseRegisterNumber(operands[0]);
                    var (offset, baseReg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    for (int i = startReg; i <= 31; i++)
                    {
                        instructions.AddRange(DFormStore(baseReg, offset + (i - startReg) * 4, IrValue.Register($"r{i}"), 4));
                    }
                    return instructions;
                }

            case "opc_29": // andis. (opcode 0x1D) not decoded by disassembler
                {
                    // andis. rA, rS, UIMM -> rA = rS & (UIMM << 16), sets CR0
                    uint rawInstr29 = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr29 >> 21) & 0x1F);
                    int rA = (int)((rawInstr29 >> 16) & 0x1F);
                    int uimm = (int)(rawInstr29 & 0xFFFF);
                    int shifted = uimm << 16;

                    return BinWithCrFlag($"r{rA}", IrValue.Register($"r{rS}"), IrValue.Imm(shifted), "and", true);
                }

            case "opc_45": // sthu (opcode 0x2D) not decoded by disassembler
                {
                    // sthu rS, d(rA) -> EA = (rA) + d, MEM(EA, 2) = rS, rA = EA
                    // Fix: compute EA into temp first to preserve rS if rS == rA
                    uint rawInstr45 = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr45 >> 21) & 0x1F);
                    int rA = (int)((rawInstr45 >> 16) & 0x1F);
                    int d = (short)(rawInstr45 & 0xFFFF);

                    string baseReg = $"r{rA}";
                    string ea = $"{baseReg}_sthu_ea";
                    return new IrInstruction[]
                    {
                        new IrBinary(ea, IrValue.Register(baseReg), IrValue.Imm(d), "add"),
                        new IrStore(new IrAddress(ea, 0), IrValue.Register($"r{rS}"), 2),
                        new IrAssign(baseReg, IrValue.Register(ea))
                    };
                }

            case "slwi" when operands.Count == 3:
            case "slwi." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var shift = ParseImmediate(operands[2]);
                    return BinWithCrFlag(dest, IrValue.Register(src), IrValue.Imm(shift), "shl", mnemonic.EndsWith('.'));
                }

            case "addic" when operands.Count == 3:
            case "addic." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var imm = SignExtend16(ParseImmediate(operands[2]));

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased(dest, src, "addic_src", instructions);

                    instructions.Add(new IrBinary(dest, IrValue.Register(srcValue), IrValue.Imm(imm), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(srcValue),
                        IrValue.Imm(imm),
                        IrValue.Imm(0)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "subfe" when operands.Count == 3:
            case "subfe." when operands.Count == 3:
                // subfe rD, rA, rB -> rD = rB + ~rA + CA
                {
                    var dest = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var tmpNot = $"{dest}_not";
                    var carry = $"{dest}_ca";

                    var instructions = new List<IrInstruction>();
                    var rBValue = PreserveIfAliased(dest, rB, "subfe_rb", instructions);

                    instructions.Add(new IrBinary(tmpNot, IrValue.Register(rA), IrValue.Imm(0), "not"));
                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary(dest, IrValue.Register(tmpNot), IrValue.Register(rBValue), "add"));
                    instructions.Add(new IrBinary(dest, IrValue.Register(dest), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(tmpNot),
                        IrValue.Register(rBValue),
                        IrValue.Register(carry)
                    }));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "subfeo" when operands.Count == 3:
            case "subfeo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Subfeo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "sthx" when operands.Count == 3:
                {
                    var rS = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    // FIX: Use BuildIndexedAddress to handle r0 correctly
                    return BuildIndexedAddress(rA, rB, addr, a => new[] { new IrStore(new IrAddress(a, 0), IrValue.Register(rS), 2) });
                }


            case "mfctr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("ctr")) };

            case "sc":
                // System call; side effects only, no value.
                return new[] { new IrCall(string.Empty, "OSSystemCall", Array.Empty<IrValue>()) };

            case "addis" when operands.Count == 3:
                {
                    var imm = SignExtend16(ParseImmediate(operands[2])) << 16;
                    return new[] { new IrBinary(operands[0], BaseOrZero(operands[1]), IrValue.Imm(imm), "add") };
                }

            case "lfdx":
            case "xo_599":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    // lfdx frD, rA, rB -> Load double into float register D from address (rA|0) + rB
                    // Use a unique temp variable to avoid corrupting rB if it's also used as the address reg
                    string addrReg = $"addr_lfdx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[] 
                    {
                        new IrLoad($"f{rD}", new IrAddress(addr, 0), 8)
                    });
                }

            case "lfdux":
            case "xo_631":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"lfdux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_lfdux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"f{rD}", new IrAddress(addr, 0), 8)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "lfsx":
            case "xo_535":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    string addrReg = $"addr_lfsx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[] 
                    {
                        new IrLoad($"f{rD}", new IrAddress(addr, 0), 4)
                    });
                }

            case "lfsux":
            case "xo_567":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"lfsux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    // For update instructions, we need to use a temp then update rA
                    string addrReg = $"addr_lfsux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"f{rD}", new IrAddress(addr, 0), 4)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "stfdx":
            case "xo_727":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    string addrReg = $"addr_stfdx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[] 
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"f{rS}"), 8)
                    });
                }

            case "stfdux":
            case "xo_759":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"stfdux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_stfdux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"f{rS}"), 8)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "stfsx":
            case "xo_663":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    string addrReg = $"addr_stfsx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[] 
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"f{rS}"), 4)
                    });
                }

            case "stfsux":
            case "xo_695":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"stfsux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    // EA = (rA) + (rB) -> update rA
                    // MEM(EA, 4) = fS
                    
                    return new IrInstruction[] 
                    {
                        new IrBinary($"r{rA}", IrValue.Register($"r{rA}"), IrValue.Register($"r{rB}"), "add"),
                        new IrStore(new IrAddress($"r{rA}", 0), IrValue.Register($"f{rS}"), 4)
                    };
                }

            case "sthux":
            case "xo_439":
                {
                    // sthux rS, rA, rB -> EA = (rA) + (rB), MEM(EA, 2) = rS, rA = EA
                    // Fix: compute EA into temp first to preserve rS if rS == rA
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"sthux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_sthux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"r{rS}"), 2)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "stfiwx":
            case "xo_983":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    string addrReg = $"addr_stfiwx_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[] 
                    {
                        // Use dedicated helper that writes raw lower 32 bits, NOT float conversion
                        new IrCall(string.Empty, "PPC_Stfiwx", new[] { IrValue.Register(addr), IrValue.Register($"f{rS}") })
                    });
                }
                
            case "add" when operands.Count == 3:
            case "add." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "add", mnemonic.EndsWith('.'));

            case "addo" when operands.Count == 3:
            case "addo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Addo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "clrlwi" when operands.Count == 3:
            case "clrlwi." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var shift = ParseImmediate(operands[2]);
                    var mask = unchecked((int)GetMask(shift, 31));

                    var instructions = new List<IrInstruction>
                    {
                        // clrlwi clears the leftmost `shift` bits; no rotation occurs.
                        new IrBinary(dest, IrValue.Register(src), IrValue.Imm(mask), "and")
                    };

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "rlwinm" when operands.Count == 5:
            case "rlwinm." when operands.Count == 5:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var sh = ParseImmediate(operands[2]);
                    var mb = ParseImmediate(operands[3]);
                    var me = ParseImmediate(operands[4]);
                    var mask = GetMask(mb, me);

                    var instructions = new List<IrInstruction>();
                    var val = src;

                    if (sh != 0)
                    {
                        var rot = $"{dest}_rot";
                        instructions.Add(new IrBinary(rot, IrValue.Register(src), IrValue.Imm(sh), "rotl"));
                        val = rot;
                    }

                    instructions.Add(new IrBinary(dest, IrValue.Register(val), IrValue.Imm(unchecked((int)mask)), "and"));
                    
                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "rlwimi" when operands.Count == 5:
            case "rlwimi." when operands.Count == 5:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var sh = ParseImmediate(operands[2]);
                    var mb = ParseImmediate(operands[3]);
                    var me = ParseImmediate(operands[4]);
                    var mask = unchecked((int)GetMask(mb, me));

                    var instructions = new List<IrInstruction>();
                    var val = src;

                    if (sh != 0)
                    {
                        var rot = $"{dest}_rot";
                        instructions.Add(new IrBinary(rot, IrValue.Register(src), IrValue.Imm(sh), "rotl"));
                        val = rot;
                    }

                    // (rA & ~mask) | (rot & mask)
                    var maskedRot = $"{dest}_mrot";
                    var maskedDest = $"{dest}_mdest";
                    instructions.Add(new IrBinary(maskedRot, IrValue.Register(val), IrValue.Imm(mask), "and"));
                    instructions.Add(new IrBinary(maskedDest, IrValue.Register(dest), IrValue.Imm(~mask), "and"));
                    instructions.Add(new IrBinary(dest, IrValue.Register(maskedDest), IrValue.Register(maskedRot), "or"));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "rlwnm" when operands.Count == 5:
            case "rlwnm." when operands.Count == 5:
                {
                    // rlwnm rA, rS, rB, MB, ME
                    // rA = rotl32(rS, rB[27:31]) & MASK(MB, ME)
                    // Uses register rB for rotation amount (only low 5 bits used)
                    var dest = operands[0];
                    var src = operands[1];
                    var shiftReg = operands[2];  // Register containing shift amount
                    var mb = ParseImmediate(operands[3]);
                    var me = ParseImmediate(operands[4]);
                    var mask = unchecked((int)GetMask(mb, me));

                    var instructions = new List<IrInstruction>();

                    var rot = $"{dest}_rot";
                    instructions.Add(new IrBinary(rot, IrValue.Register(src), IrValue.Register(shiftReg), "rotl"));

                    // Apply mask
                    instructions.Add(new IrBinary(dest, IrValue.Register(rot), IrValue.Imm(mask), "and"));

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "mfspr" when operands.Count == 2:
                {
                    var dest = operands[0];
                    var spr = ParseImmediate(operands[1]);

                    switch (spr)
                    {
                        case 8: return new[] { new IrAssign(dest, IrValue.Register("lr")) };
                        case 9: return new[] { new IrAssign(dest, IrValue.Register("ctr")) };
                        case 1: return new[] { new IrAssign(dest, IrValue.Register("xer")) };
                        case >= 912 and <= 919: return new[] { new IrAssign(dest, IrValue.Register($"gqr{spr - 912}")) };
                        case 920: return new[] { new IrAssign(dest, IrValue.Register("hid2")) };
                        case 1008: return new[] { new IrAssign(dest, IrValue.Register("hid0")) };
                        case 1009: return new[] { new IrAssign(dest, IrValue.Register("hid1")) };
                        case 26: return new[] { new IrAssign(dest, IrValue.Register("srr0")) };
                        case 27: return new[] { new IrAssign(dest, IrValue.Register("srr1")) };
                    }

                    return new IrInstruction[]
                    {
                        new IrComment($"mfspr {spr} unsupported @ 0x{ins.Address:X8}"),
                        new IrCall(dest, "PPC_ReadSpr", new[] { IrValue.Imm(spr) })
                    };
                }

            case "mtspr" when operands.Count == 2:
                {
                    var spr = ParseImmediate(operands[0]);
                    var src = operands[1];

                    switch (spr)
                    {
                        case 8: return new[] { new IrAssign("lr", IrValue.Register(src)) };
                        case 9: return new[] { new IrAssign("ctr", IrValue.Register(src)) };
                        case 1: return new[] { new IrAssign("xer", IrValue.Register(src)) };
                        case >= 912 and <= 919: return new[] { new IrAssign($"gqr{spr - 912}", IrValue.Register(src)) };
                        case 26: return new[] { new IrAssign("srr0", IrValue.Register(src)) };
                        case 27: return new[] { new IrAssign("srr1", IrValue.Register(src)) };
                        // HID registers (hardware implementation dependent)
                        case 920: return new[] { new IrAssign("hid2", IrValue.Register(src)) };
                        case 1008: return new[] { new IrAssign("hid0", IrValue.Register(src)) };
                        case 1009: return new[] { new IrAssign("hid1", IrValue.Register(src)) };
                        // Debug/Breakpoint registers - delegate to runtime helper (no-op in HLE)
                        case 22:   // DEC (Decrementer)
                        case 1010: // IABR (Instruction Address Breakpoint Register)
                        case 1011: // HID4 / L2CR
                        case 1013: // DABR (Data Address Breakpoint Register)
                        case 1017: // L2CR/DMA ancillary
                        case 952:  // PMC1-4 Performance counters
                        case 953:
                        case 954:
                        case 956:
                        case 957:
                        case 958:
                        // SPRG0-7 (Software Program Registers - OS scratch space)
                        case >= 272 and <= 279:
                        // MSR (Machine State Register)
                        case 16:
                            return new IrInstruction[]
                            {
                                new IrComment($"mtspr {spr} (privileged/OS register) @ 0x{ins.Address:X8}"),
                                new IrCall(string.Empty, "PPC_WriteSpr", new[] { IrValue.Imm(spr), IrValue.Register(src) })
                            };
                    }

                    // Any other SPR write is delegated to the runtime helper.
                    // The runtime will log a warning and shadow the value for reads.
                    return new IrInstruction[]
                    {
                        new IrComment($"mtspr {spr} (unknown SPR) @ 0x{ins.Address:X8}"),
                        new IrCall(string.Empty, "PPC_WriteSpr", new[] { IrValue.Imm(spr), IrValue.Register(src) })
                    };
                }

            case "mtsr" when operands.Count == 2:
                {
                    var sr = ParseImmediate(operands[0]);
                    // Move To Segment Register - no-op in HLE (no real MMU)
                    return new[] { new IrComment($"mtsr {sr} @ 0x{ins.Address:X8} (no-op)") };
                }

            case "mtsrin" when operands.Count == 2:
                return new[] { new IrComment($"mtsrin @ 0x{ins.Address:X8} (no-op)") };

            case "mfsr" when operands.Count == 2:
                {
                    var dest = operands[0];
                    var sr = ParseImmediate(operands[1]);
                    return new[] { new IrAssign(dest, IrValue.Imm(0)) };
                }

            case "mfsrin" when operands.Count == 2:
                return new[] { new IrAssign(operands[0], IrValue.Imm(0)) };


            case "stfd" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormStore(reg, offset, IrValue.Register(operands[0]), 8);
                }

            case "stfdu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException("stfdu with rA=0 is invalid");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrStore(new IrAddress(reg, 0), IrValue.Register(operands[0]), 8));
                    return instructions;
                }

            case "stfs" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return DFormStore(reg, offset, IrValue.Register(operands[0]), 4);
                }

            case "stfsu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException("stfsu with rA=0 is invalid");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrStore(new IrAddress(reg, 0), IrValue.Register(operands[0]), 4));
                    return instructions;
                }

            case "stwux" when operands.Count == 3:
                {
                    var rS = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    if (rA == "r0")
                        throw new InvalidOperationException($"stwux with rA=0 is invalid @ 0x{ins.Address:X8}");
                    return new IrInstruction[]
                    {
                        new IrBinary(addr, IrValue.Register(rA), IrValue.Register(rB), "add"),
                        new IrStore(new IrAddress(addr, 0), IrValue.Register(rS), 4),
                        new IrAssign(rA, IrValue.Register(addr))
                    };
                }

            case "stwx" when operands.Count == 3:
                {
                    var rS = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    return BuildIndexedAddress(rA, rB, addr, a => new[]
                    {
                        new IrStore(new IrAddress(a, 0), IrValue.Register(rS), 4)
                    });
                }

            case "lwzux" when operands.Count == 3:
                {
                    var rD = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    if (rA == "r0")
                        throw new InvalidOperationException($"lwzux with rA=0 is invalid @ 0x{ins.Address:X8}");
                    return new IrInstruction[]
                    {
                        new IrBinary(addr, IrValue.Register(rA), IrValue.Register(rB), "add"),
                        new IrLoad(rD, new IrAddress(addr, 0), 4),
                        new IrAssign(rA, IrValue.Register(addr))
                    };
                }

            case "lwzx" when operands.Count == 3:
                {
                    var rD = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    // FIX: Use BuildIndexedAddress to handle r0 correctly
                    return BuildIndexedAddress(rA, rB, addr, a => new[] { new IrLoad(rD, new IrAddress(a, 0), 4) });
                }

            case "lhzx" when operands.Count == 3:
                {
                    var rD = operands[0];
                    var rA = operands[1];
                    var rB = operands[2];
                    var addr = $"{rA}_addr";
                    // FIX: Use BuildIndexedAddress to handle r0 correctly
                    return BuildIndexedAddress(rA, rB, addr, a => new[] { new IrLoad(rD, new IrAddress(a, 0), 2) });
                }

            case "lhax":
            case "xo_343":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    string addrReg = $"addr_lhax_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"r{rD}", new IrAddress(addr, 0), 2)
                    }));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "shl"));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "sar"));
                    return instructions;
                }

            case "lhaux":
            case "xo_375":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"lhaux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_lhaux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"r{rD}", new IrAddress(addr, 0), 2)
                    }));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "shl"));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Imm(16), "sar"));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "lhzux":
            case "xo_311":
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);

                    if (rA == 0)
                        throw new InvalidOperationException($"lhzux with rA=0 is invalid @ 0x{ins.Address:X8}");

                    string addrReg = $"addr_lhzux_{ins.Address:X8}_loc";
                    var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrLoad($"r{rD}", new IrAddress(addr, 0), 2)
                    }));
                    instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
                    return instructions;
                }

            case "mulli" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(SignExtend16(ParseImmediate(operands[2]))), "mul") };

            case "mulhwu" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mulhwu") };

            case "mulhwu." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mulhwu", true);

            case "mulhw" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mulhw") };

            case "mulhw." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mulhw", true);

            case "srawi" when operands.Count == 3:
            case "srawi." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var sh = ParseImmediate(operands[2]) & 0x1F;

                    var instructions = new List<IrInstruction>
                    {
                        new IrCall("xer", "PPC_UpdateCarryShiftRight", new[]
                        {
                            IrValue.Register(src),
                            IrValue.Imm(sh)
                        }),
                        new IrBinary(dest, IrValue.Register(src), IrValue.Imm(sh), "sar")
                    };

                    if (mnemonic.EndsWith('.'))
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "bdnz" when operands.Count == 1:
            case "bdz" when operands.Count == 1:
                {
                    var target = operands[0];
                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    return new IrInstruction[]
                    {
                        new IrBinary("ctr", IrValue.Register("ctr"), IrValue.Imm(-1), "add"),
                        // Use the mnemonic as the condition type to indicate CTR-based branch
                        new IrBranch(mnemonic, target, fallthrough, "ctr")
                    };
                }

            case "mtctr" when operands.Count == 1:
                return new[] { new IrAssign("ctr", IrValue.Register(operands[0])) };

            case "mflr" when operands.Count == 1:
                return new[] { new IrAssign(operands[0], IrValue.Register("lr")) };

            case "mtlr" when operands.Count == 1:
                return new[] { new IrAssign("lr", IrValue.Register(operands[0])) };

            case "srwi" when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(ParseImmediate(operands[2])), "shr") };

            case "slw" when operands.Count == 3:
            case "slw." when operands.Count == 3:
                {
                    var dest = operands[0];
                    var src = operands[1];
                    var shReg = operands[2];

                    var list = new List<IrInstruction>
                    {
                        new IrBinary(dest, IrValue.Register(src), IrValue.Register(shReg), "ppc_slw")
                    };

                    if (mnemonic.EndsWith('.'))
                        list.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));

                    return list;
                }

            case "andi." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Imm(MaskUimm(ParseImmediate(operands[2]))), "and", true);

            case "andis." when operands.Count == 3:
                {
                    // andis. rA, rS, UIMM -> rA = rS & (UIMM << 16), sets CR0
                    var uimm = MaskUimm(ParseImmediate(operands[2])) << 16;
                    return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Imm(uimm), "and", true);
                }

            case "and" when operands.Count == 3:
            case "and." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "and", mnemonic.EndsWith('.'));

            case "andc" when operands.Count == 3:
            case "andc." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "andc", mnemonic.EndsWith('.'));

            case "xor" when operands.Count == 3:
            case "xor." when operands.Count == 3:
                return BinWithCrFlag(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "xor", mnemonic.EndsWith('.'));

            case "subfic" when operands.Count == 3:
                {
                    // subfic rD, rA, IMM -> rD = IMM - rA, CA set if no borrow (IMM >= rA)
                    var dest = Reg(0);
                    var immVal = SignExtend16(ParseImmediate(operands[2]));
                    var rA = Reg(1);
                    var instructions = new List<IrInstruction>();
                    var rAValue = PreserveIfAliased(dest, rA, "subfic_ra", instructions);

                    instructions.Add(new IrBinary(dest, IrValue.Imm(immVal), IrValue.Register(rAValue), "sub"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarrySub", new[]
                    {
                        IrValue.Imm(immVal),
                        IrValue.Register(rAValue)
                    }));
                    return instructions;
                }

            case "subf" when operands.Count == 3:
            case "subf." when operands.Count == 3:
                return BinWithCrFlag(
                    operands[0],
                    IrValue.Register(operands[2]),
                    IrValue.Register(operands[1]),
                    "sub",
                    mnemonic.EndsWith('.'));

            case "subfo" when operands.Count == 3:
            case "subfo." when operands.Count == 3:
                return CallBinaryWithCr(Reg(0), "PPC_Subfo", Reg(1), Reg(2), mnemonic.EndsWith('.'));

            case "beqlr":
            case "bnelr":
            case "bltlr":
            case "bgtlr":
            case "bgelr":
            case "blelr":
            case "bsolr":
            case "bnslr":
                {
                    var crField = "cr0";
                    if (ops.Count > 0 && ops[0] is PpcConditionRegisterOperand crOp)
                    {
                        crField = NormalizeRegister(crOp.Name);
                    }

                    var cond = mnemonic.Replace("lr", string.Empty, StringComparison.OrdinalIgnoreCase);
                    return new[] { new IrBranch(cond, "return", $"0x{ins.EndAddress:X8}", crField) };
                }

            case "bctrl":
                // PowerPC EABI: pass argument registers for indirect calls too
                var bctrlArgs = AbiCallArguments;

                return new IrInstruction[] {
                    new IrAssign("lr", IrValue.Imm((int)ins.EndAddress)),
                    new IrIndirectCall(string.Empty, IrValue.Register("ctr"), bctrlArgs) 
                };

            case "cntlzw" when operands.Count == 2:
            case "cntlzw." when operands.Count == 2:
            case "cntlz" when operands.Count == 2:  // Keep for compatibility
                {
                    var instructions = new List<IrInstruction> { new IrCall(operands[0], "PPC_Cntlzw", new[] { IrValue.Register(operands[1]) }) };
                    if (mnemonic.EndsWith('.'))
                        instructions.Add(new IrSetCrField(0, IrValue.Register(operands[0]), IrValue.Imm(0), false));
                    return instructions;
                }

            case "lwzu" when operands.Count == 2:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    var instructions = new List<IrInstruction>();
                    if (reg == "r0")
                        throw new InvalidOperationException($"lwzu with rA=0 is invalid @ 0x{ins.Address:X8}");
                    instructions.Add(new IrBinary(reg, IrValue.Register(reg), IrValue.Imm(offset), "add"));
                    instructions.Add(new IrLoad(operands[0], new IrAddress(reg, 0), 4));
                    return instructions;
                }

            case "b":
                {
                    var targetAddr = ins.BranchTargets.FirstOrDefault();
                    if (validAddresses.Contains(targetAddr))
                    {
                        return new[] { new IrJump(TargetLabel(ins, validAddresses)) };
                    }

                    var branchesToKnownFunction =
                        functionEntryPoint.HasValue &&
                        targetAddr != functionEntryPoint.Value &&
                        knownFunctionEntryPoints?.Contains(targetAddr) == true;

                    // Tail call to external function
                    var tailCallArgs = AbiCallArguments;
                    return new IrInstruction[]
                    {
                        new IrCall(string.Empty, TargetLabel(ins, validAddresses, preferFallthrough: false), tailCallArgs),
                        new IrReturn(null)
                    };
                }

            case "bl":
                // PowerPC EABI passes arguments in r3-r10, f1-f13
                var blTargetAddr = ins.BranchTargets.FirstOrDefault();
                if (blTargetAddr == ins.EndAddress &&
                    instructionsByAddress.TryGetValue(blTargetAddr, out var fallthroughIns) &&
                    string.Equals(fallthroughIns.Mnemonic, "mflr", StringComparison.OrdinalIgnoreCase))
                {
                    return new IrInstruction[] {
                        new IrAssign("lr", IrValue.Imm((int)ins.EndAddress))
                    };
                }

                var blArgs = AbiCallArguments;

                return new IrInstruction[] {
                    new IrAssign("lr", IrValue.Imm((int)ins.EndAddress)),
                    new IrCall(string.Empty, TargetLabel(ins, validAddresses, preferFallthrough: false), blArgs) 
                };

            case "bltl":
            case "bcl":
                {
                    var rawInstr = ReadRawInstruction(ins);
                    var bo = (int)((rawInstr >> 21) & 0x1F);
                    var bi = (int)((rawInstr >> 16) & 0x1F);
                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    var targetAddr = ins.BranchTargets.FirstOrDefault();
                    var linkedTarget = $"link_branch_{ins.EndAddress:X8}_{targetAddr:X8}";
                    var instructions = new List<IrInstruction>();
                    if ((bo & 0x04) == 0)
                    {
                        instructions.Add(new IrBinary("ctr", IrValue.Register("ctr"), IrValue.Imm(-1), "add"));
                    }
                    instructions.Add(new IrAssign("lr", IrValue.Imm((int)ins.EndAddress)));
                    instructions.Add(new IrBranch("raw", linkedTarget, fallthrough, BuildBoConditionExpression(bo, bi, allowCtr: true)));
                    return instructions;
                }

            case "blrl":
                // Branch to address in LR and update LR with the return address.
                var blrlArgs = AbiCallArguments;

                // Save current LR (target) to temp, update LR, then call temp
                var blrlTemp = $"addr_blrl_{ins.Address:X8}_loc";
                return new IrInstruction[] { 
                    new IrAssign(blrlTemp, IrValue.Register("lr")),
                    new IrAssign("lr", IrValue.Imm((int)ins.EndAddress)),
                    new IrIndirectCall(string.Empty, IrValue.Register(blrlTemp), blrlArgs) 
                };

            case "bclrl":
                {
                    var rawInstr = ReadRawInstruction(ins);
                    var bo = (int)((rawInstr >> 21) & 0x1F);
                    var bi = (int)((rawInstr >> 16) & 0x1F);
                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    var targetLabel = $"call_lr_{ins.Address:X8}_{ins.EndAddress:X8}";
                    var instructions = new List<IrInstruction>();
                    if ((bo & 0x04) == 0)
                    {
                        instructions.Add(new IrBinary("ctr", IrValue.Register("ctr"), IrValue.Imm(-1), "add"));
                    }
                    instructions.Add(new IrBranch("raw", targetLabel, fallthrough, BuildBoConditionExpression(bo, bi, allowCtr: true)));
                    return instructions;
                }

            case "beqlrl":
            case "bnelrl":
            case "bltlrl":
            case "bgtlrl":
            case "bgelrl":
            case "blelrl":
            case "bsolrl":
            case "bnslrl":
                {
                    var crField = "cr0";
                    if (ops.Count > 0 && ops[0] is PpcConditionRegisterOperand crOp)
                    {
                        crField = NormalizeRegister(crOp.Name);
                    }

                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    var targetLabel = $"call_lr_{ins.Address:X8}_{ins.EndAddress:X8}";
                    var cond = mnemonic.Replace("lrl", string.Empty, StringComparison.OrdinalIgnoreCase);
                    return new[] { new IrBranch(cond, targetLabel, fallthrough, crField) };
                }

            case "bne":
            case "beq":
            case "blt":
            case "bgt":
            case "ble":
            case "bge":
                {
                    // Extract CR field from operand if present (for cr1-cr7), otherwise default to cr0
                    var crField = "cr0";
                    if (ops.Count > 0 && ops[0] is PpcConditionRegisterOperand crOp)
                    {
                        crField = NormalizeRegister(crOp.Name);
                    }
                    return new[] { new IrBranch(mnemonic, TargetLabel(ins, validAddresses, preferFallthrough: false), $"0x{ins.EndAddress:X8}", crField) };
                }

            case "bc":
                {
                    // Generic bc instruction.
                    // If we have BO and BI as operands, we can lift it correctly.
                    if (ops.Count >= 3 && ops[0] is PpcImmediateOperand boOp && ops[1] is PpcImmediateOperand biOp)
                    {
                        var bo = boOp.Value;
                        var bi = biOp.Value;
                        var crFieldIdx = bi / 4;
                        var crBitIdx = bi % 4;
                        var target = operands[2];
                        var fallthrough = $"0x{ins.EndAddress:X8}";

                        var instructions = new List<IrInstruction>();
                        if ((bo & 0x04) == 0)
                        {
                            instructions.Add(new IrBinary("ctr", IrValue.Register("ctr"), IrValue.Imm(-1), "add"));
                        }
                        instructions.Add(new IrBranch("raw", target, fallthrough, BuildBoConditionExpression(bo, bi, allowCtr: true)));
                        return instructions;
                    }

                    // Extract CR field from operand if present, otherwise default to cr0
                    var crField = "cr0";
                    if (ops.Count > 0 && ops[0] is PpcConditionRegisterOperand crOp)
                    {
                        crField = NormalizeRegister(crOp.Name);
                    }
                    return new[] { new IrBranch("bc", TargetLabel(ins, validAddresses, preferFallthrough: false), $"0x{ins.EndAddress:X8}", crField) };
                }


            case "blr":
                return new[] { new IrReturn(null) };

            case "bclr":
                {
                    var rawInstr = ReadRawInstruction(ins);
                    var bo = (int)((rawInstr >> 21) & 0x1F);
                    var bi = (int)((rawInstr >> 16) & 0x1F);
                    var fallthrough = $"0x{ins.EndAddress:X8}";
                    var bo2 = (bo & 0x04) != 0;
                    var instructions = new List<IrInstruction>();
                    if (!bo2)
                    {
                        instructions.Add(new IrBinary("ctr", IrValue.Register("ctr"), IrValue.Imm(-1), "add"));
                    }
                    instructions.Add(new IrBranch("raw", "return", fallthrough, BuildBoConditionExpression(bo, bi, allowCtr: true)));
                    return instructions;
                }

            case "xo_54": // dcbst
            case "dcbst":
                // Data Cache Block Store - no-op in HLE (no real cache)
                return new[] { new IrComment($"dcbst @ 0x{ins.Address:X8} (no-op)") };

            case "xo_138": // adde
                // adde rD, rA, rB
                // Opcode 31, XO 138
                {
                    uint rawInstrAdde = ins.Word;
                    int rD = (int)((rawInstrAdde >> 21) & 0x1F);
                    int rA = (int)((rawInstrAdde >> 16) & 0x1F);
                    int rB = (int)((rawInstrAdde >> 11) & 0x1F);
                    bool rc = (rawInstrAdde & 1) != 0;

                    var carry = $"r{rD}_ca";
                    var instructions = new List<IrInstruction>();
                    var leftValue = PreserveIfAliased($"r{rD}", $"r{rA}", "adde_left", instructions);
                    var rightValue = PreserveIfAliased($"r{rD}", $"r{rB}", "adde_right", instructions);

                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register(leftValue), IrValue.Register(rightValue), "add"));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register($"r{rD}"), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(leftValue),
                        IrValue.Register(rightValue),
                        IrValue.Register(carry)
                    }));

                    if (rc)
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register($"r{rD}"), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "xo_202": // addze
                // addze rD, rA
                // Opcode 31, XO 202
                {
                    uint rawInstrAddze = ins.Word;
                    int rD = (int)((rawInstrAddze >> 21) & 0x1F);
                    int rA = (int)((rawInstrAddze >> 16) & 0x1F);
                    bool rc = (rawInstrAddze & 1) != 0;

                    var carry = $"r{rD}_ca";
                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased($"r{rD}", $"r{rA}", "addze_src", instructions);

                    instructions.Add(new IrCall(carry, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register(srcValue), IrValue.Register(carry), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(srcValue),
                        IrValue.Imm(0),
                        IrValue.Register(carry)
                    }));

                    if (rc)
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register($"r{rD}"), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "xo_0": // cmp fallback
                // cmp crfD, L, rA, rB
                // Opcode 31, XO 0
                {
                    uint rawInstrCmp = ins.Word;
                    int crfD = (int)((rawInstrCmp >> 23) & 0x7);
                    int rA = (int)((rawInstrCmp >> 16) & 0x1F);
                    int rB = (int)((rawInstrCmp >> 11) & 0x1F);
                    
                    return LiftCompare($"cr{crfD}", IrValue.Register($"r{rA}"), IrValue.Register($"r{rB}"), "sub", isUnsigned: false);
                }

            case "xo_470": // dcbi
                // Data Cache Block Invalidate - no-op in HLE (no real cache)
                return new[] { new IrComment($"dcbi @ 0x{ins.Address:X8} (no-op)") };

            case "dcbt":
            case "xo_278": // dcbt - Data Cache Block Touch (prefetch hint)
            case "dcbtst":
            case "xo_246": // dcbtst - Data Cache Block Touch for Store
            case "dcba":
            case "xo_758": // dcba - Data Cache Block Allocate
            case "tlbie":
            case "xo_306": // tlbie - no software TLB in this runtime
            case "tlbsync":
            case "xo_566": // tlbsync - no software TLB in this runtime
                // Cache prefetch hint - no-op in HLE
                return new[] { new IrComment($"{mnemonic} @ 0x{ins.Address:X8} (no-op)") };

            case "stwbrx":
            case "xo_662": // stwbrx - Store Word Byte-Reverse Indexed
                {
                    // stwbrx rS, rA, rB -> stores rS byte-reversed to address (rA|0)+rB
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrCall("", "PPC_StoreWordByteReverse", new[] 
                        { 
                            IrValue.Register(addr), 
                            IrValue.Register($"r{rS}") 
                        })
                    });
                }

            case "lwbrx":
            case "xo_534": // lwbrx - Load Word Byte-Reverse Indexed
                {
                    // lwbrx rD, rA, rB -> loads byte-reversed word from address (rA|0)+rB into rD
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrCall($"r{rD}", "PPC_LoadWordByteReverse", new[] { IrValue.Register(addr) })
                    });
                }

            case "lhbrx":
            case "xo_790": // lhbrx - Load Halfword Byte-Reverse Indexed
                {
                    // lhbrx rD, rA, rB -> loads byte-reversed halfword from address (rA|0)+rB into rD
                    uint rawInstr = ReadRawInstruction(ins);
                    int rD = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrCall($"r{rD}", "PPC_LoadHalfwordByteReverse", new[] { IrValue.Register(addr) })
                    });
                }

            case "sthbrx":
            case "xo_918": // sthbrx - Store Halfword Byte-Reverse Indexed
                {
                    // sthbrx rS, rA, rB -> stores rS byte-reversed (halfword) to address (rA|0)+rB
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrCall("", "PPC_StoreHalfwordByteReverse", new[] 
                        { 
                            IrValue.Register(addr), 
                            IrValue.Register($"r{rS}") 
                        })
                    });
                }

            case "nand":
            case "nand.":
                return BinWithCrFlag(Reg(0), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), "nand", mnemonic.EndsWith('.'));

            case "xo_476": // nand
                {
                    uint rawInstr = ReadRawInstruction(ins);
                    int rS = (int)((rawInstr >> 21) & 0x1F);
                    int rA = (int)((rawInstr >> 16) & 0x1F);
                    int rB = (int)((rawInstr >> 11) & 0x1F);
                    bool rc = (rawInstr & 1) != 0;
                    
                    // nand RA, RS, RB -> RA = ~(RS & RB)
                    return BinWithCrFlag($"r{rA}", IrValue.Register($"r{rS}"), IrValue.Register($"r{rB}"), "nand", rc);
                }

            case "xo_11": // mulhwu
                // mulhwu rD, rA, rB
                // Opcode 31, XO 11
                {
                    uint rawInstrMulhwu = ins.Word;
                    int rD = (int)((rawInstrMulhwu >> 21) & 0x1F);
                    int rA = (int)((rawInstrMulhwu >> 16) & 0x1F);
                    int rB = (int)((rawInstrMulhwu >> 11) & 0x1F);
                    
                    return new[] { new IrBinary($"r{rD}", IrValue.Register($"r{rA}"), IrValue.Register($"r{rB}"), "mulhwu") };
                }
    

            case "xo_595": // mfsr
            case "mfsr":
                // mfsr rD, SR
                // Opcode 31, XO 595
                {
                    uint rawInstrMfsr = ins.Word;
                    int rD = (int)((rawInstrMfsr >> 21) & 0x1F);
                    
                    throw new NotImplementedException($"UNIMPLEMENTED instruction 'mfsr r{rD}, SR' @ 0x{ins.Address:X8}");
                }
            case "xo_151": // stwx
                // stwx rS, rA, rB
                // Opcode 31, XO 151
                {
                    uint rawInstrStwx = ins.Word;
                    int rS = (int)((rawInstrStwx >> 21) & 0x1F);
                    int rA = (int)((rawInstrStwx >> 16) & 0x1F);
                    int rB = (int)((rawInstrStwx >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"r{rS}"), 4)
                    });
                }

            case "xo_215": // stbx
                // stbx rS, rA, rB
                // Opcode 31, XO 215
                {
                    uint rawInstrStbx = ins.Word;
                    int rS = (int)((rawInstrStbx >> 21) & 0x1F);
                    int rA = (int)((rawInstrStbx >> 16) & 0x1F);
                    int rB = (int)((rawInstrStbx >> 11) & 0x1F);
                    
                    string addrReg = $"addr_{ins.Address:X8}_loc";
                    return BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => new[]
                    {
                        new IrStore(new IrAddress(addr, 0), IrValue.Register($"r{rS}"), 1)
                    });
                }
            case "mftb":
            case "xo_371":
                {
                    string dest;
                    bool isUpper = false;

                    // xo_371 often loses its operands in the disassembler; decode manually. rD is bits 6-10,
                    // and the TBR id splits as high5(bits 16-20) << 5 | low5(bits 11-15). TBR 269 = TBU.
                    if (operands.Count == 0)
                    {
                        uint raw1 = ReadRawInstruction(ins);
                        int rD = (int)((raw1 >> 21) & 0x1F);
                        dest = $"r{rD}";

                        uint sprLow5 = (raw1 >> 16) & 0x1F;
                        uint sprHigh5 = (raw1 >> 11) & 0x1F;

                        int tbr = (int)((sprHigh5 << 5) | sprLow5);
                        
                        if (tbr == 269) isUpper = true;
                    }
                    else
                    {
                        dest = operands[0];
                        // Check if explicit TBR provided in operands
                        if (operands.Count > 1)
                        {
                            int tbr = ParseImmediate(operands[1]);
                            if (tbr == 269) isUpper = true;
                        }
                    }

                    string helper = isUpper ? "PPC_Mftbu" : "PPC_Mftb";
                    return new[] { new IrCall(dest, helper, Array.Empty<IrValue>()) };
                }

            case "mftbu":
                // mftbu rD
                // Be defensive: if operands missing, read rD from raw instruction
                if (operands.Count == 0)
                {
                    uint rawInstrMftbu = ins.Word;
                    int rD = (int)((rawInstrMftbu >> 21) & 0x1F);
                    return new[] { new IrCall($"r{rD}", "PPC_Mftbu", Array.Empty<IrValue>()) };
                }
                return new[] { new IrCall(operands[0], "PPC_Mftbu", Array.Empty<IrValue>()) };

            case "xo_954": // extsb
                // extsb rA, rS
                // Opcode 31, XO 954
                {
                    uint rawInstrExtsb = ins.Word;
                    int rS = (int)((rawInstrExtsb >> 21) & 0x1F);
                    int rA = (int)((rawInstrExtsb >> 16) & 0x1F);
                    bool rc = (rawInstrExtsb & 1) != 0;
                    
                    return BinWithCrFlag($"r{rA}", IrValue.Register($"r{rS}"), IrValue.Imm(8), "sext", rc);
                }

            case "xo_200": // subfze
                // subfze rD, rA
                // Opcode 31, XO 200
                {
                    uint rawInstrSubfze = ins.Word;
                    int rD = (int)((rawInstrSubfze >> 21) & 0x1F);
                    int rA = (int)((rawInstrSubfze >> 16) & 0x1F);

                    bool rc = (rawInstrSubfze & 1) != 0;
                    var tmpNot = $"r{rD}_not";
                    var tmpCa = $"r{rD}_ca";

                    var instructions = new List<IrInstruction>();
                    var srcValue = PreserveIfAliased($"r{rD}", $"r{rA}", "subfze_src", instructions);

                    instructions.Add(new IrBinary(tmpNot, IrValue.Register(srcValue), IrValue.Imm(0), "not"));
                    instructions.Add(new IrCall(tmpCa, "PPC_GetCarry", Array.Empty<IrValue>()));
                    instructions.Add(new IrBinary($"r{rD}", IrValue.Register(tmpNot), IrValue.Register(tmpCa), "add"));
                    instructions.Add(new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                    {
                        IrValue.Register(tmpNot),
                        IrValue.Register(tmpCa),
                        IrValue.Imm(0)
                    }));

                    if (rc)
                    {
                        instructions.Add(new IrSetCrField(0, IrValue.Register($"r{rD}"), IrValue.Imm(0), false));
                    }

                    return instructions;
                }

            case "xo_144": // mtcrf
                // mtcrf CRM, rS
                // Opcode 31, XO 144
                {
                    uint rawInstrMtcrf = ins.Word;
                    int rS = (int)((rawInstrMtcrf >> 21) & 0x1F);
                    int crm = (int)((rawInstrMtcrf >> 12) & 0xFF);
                    
                    return LiftMtcrfMasked(crm, $"r{rS}");
                }

            case "xo_210": // mtsr
            case "mtsr":
                // Move To Segment Register - no-op in HLE (no real MMU)
                return new[] { new IrComment($"mtsr @ 0x{ins.Address:X8} (no-op)") };

            case "xo_86": // dcbf
                // Data Cache Block Flush - no-op in HLE (no real cache)
                return new[] { new IrComment($"dcbf @ 0x{ins.Address:X8} (no-op)") };

            case "opc_17": // sc
                return new[] { new IrCall(string.Empty, "OSSystemCall", Array.Empty<IrValue>()) };

            case "twi":
            case "opc_3": // twi (Trap Word Immediate)
                {
                    uint raw1 = ReadRawInstruction(ins);
                    int to = (int)((raw1 >> 21) & 0x1F);  // Trap condition
                    int rA = (int)((raw1 >> 16) & 0x1F);  // Register A
                    int si = (int)(raw1 & 0xFFFF);        // Signed immediate
                    if ((si & 0x8000) != 0) si |= unchecked((int)0xFFFF0000); // Sign extend
                    
                    return new[]
                    {
                        new IrCall(string.Empty, "PPC_TrapWord", new[]
                        {
                            IrValue.Imm(to), IrValue.Register($"r{rA}"), IrValue.Imm(si)
                        })
                    };
                }

            case "tw":
                {
                    uint raw1 = ReadRawInstruction(ins);
                    int to = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rB = (int)((raw1 >> 11) & 0x1F);
                    return new[]
                    {
                        new IrCall(string.Empty, "PPC_TrapWord", new[]
                        {
                            IrValue.Imm(to), IrValue.Register($"r{rA}"), IrValue.Register($"r{rB}")
                        })
                    };
                }

            case "xo_75": // mulhw
                {
                    uint raw1 = ReadRawInstruction(ins);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rB = (int)((raw1 >> 11) & 0x1F);
                    bool rc = (raw1 & 1) != 0;

                    return BinWithCrFlag($"r{rD}", IrValue.Register($"r{rA}"), IrValue.Register($"r{rB}"), "mulhw", rc);
                }

            // Catch-all for misidentified Double Precision instructions (Opcode 63)
            // The disassembler often labels these as fp_{Value}, where Value = (rC << 5) | XO.
            case "nop":
            case "opc_0":
                return new[] { new IrComment("nop") };
        }
        }
        catch (NotImplementedException ex)
        {
            if (allowUnsupported)
            {
                return LiftUndefined(ins, ex.Message);
            }

            throw;
        }

        // BitConverter.ToString over the big-endian bytes produced exactly this
        // uppercase eight-digit form.
        var raw = ins.Word.ToString("X8", CultureInfo.InvariantCulture);
        var message = $"UNIMPLEMENTED 0x{ins.Address:X8}: {ins.Mnemonic} {ins.OperandText} (0x{raw})".Trim();
        if (allowUnsupported)
        {
            return LiftUndefined(ins, message);
        }

        throw new InvalidOperationException(message);
    }

    private static IReadOnlyList<IrInstruction> LiftUndefined(PpcInstruction ins, string reason)
    {
        var operandText = ins.OperandText;
        var disasm = string.IsNullOrWhiteSpace(operandText)
            ? ins.Mnemonic
            : $"{ins.Mnemonic} {operandText}";
        var raw = ReadRawInstruction(ins);
        return new IrInstruction[] { new IrUndefined(ins.Address, raw, disasm, reason) };
    }

    private static uint ReadRawInstruction(PpcInstruction ins) => ins.Word;

    private static IReadOnlyList<IrInstruction> LiftMtfsf(PpcInstruction ins)
    {
        var raw = ReadRawInstruction(ins);
        var flm = (int)((raw >> 17) & 0xFF);
        var frb = (int)((raw >> 11) & 0x1F);
        return new[] { new IrCall(string.Empty, "PPC_Mtfsf", new[] { IrValue.Imm(flm), IrValue.Register($"f{frb}") }) };
    }

    private static IReadOnlyList<IrInstruction> LiftMtfsfi(PpcInstruction ins)
    {
        var raw = ReadRawInstruction(ins);
        var field = (int)((raw >> 23) & 0x7);
        var value = (int)((raw >> 12) & 0xF);
        return new[] { new IrCall(string.Empty, "PPC_Mtfsfi", new[] { IrValue.Imm(field), IrValue.Imm(value) }) };
    }

    private static IReadOnlyList<IrInstruction> LiftMtfsb1(PpcInstruction ins)
    {
        var raw = ReadRawInstruction(ins);
        var bt = (int)((raw >> 21) & 0x1F);
        return new[] { new IrCall(string.Empty, "PPC_Mtfsb1", new[] { IrValue.Imm(bt) }) };
    }

    private static IReadOnlyList<IrInstruction> LiftMtfsb0(PpcInstruction ins)
    {
        var raw = ReadRawInstruction(ins);
        var bt = (int)((raw >> 21) & 0x1F);
        return new[] { new IrCall(string.Empty, "PPC_Mtfsb0", new[] { IrValue.Imm(bt) }) };
    }

    private static IReadOnlyList<IrInstruction> LiftMffs(PpcInstruction ins)
    {
        var raw = ReadRawInstruction(ins);
        var fD = (int)((raw >> 21) & 0x1F);
        return new[] { new IrCall($"f{fD}", "PPC_Mffs", Array.Empty<IrValue>()) };
    }

    private static IReadOnlyList<IrInstruction> LiftCompare(string destination, IrValue left, IrValue right, string op, bool isUnsigned)
    {
        var field = ParseCrFieldIndex(destination);
        return new IrInstruction[]
        {
            new IrSetCrField(field, left, right, isUnsigned)
        };
    }

    private static int ParseCrFieldIndex(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return 0;
        }

        var baseName = name;
        var underscore = name.IndexOf('_');
        if (underscore >= 0)
        {
            baseName = name[..underscore];
        }

        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase))
        {
            return 0;
        }

        if (baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(baseName.AsSpan(2), out var parsed) && parsed is >= 0 and < 8)
        {
            return parsed;
        }

        return 0;
    }

    private static IReadOnlyList<IrInstruction> BinWithCrFlag(string dest, IrValue left, IrValue right, string op, bool setsCr)
    {
        var ops = new List<IrInstruction>
        {
            new IrBinary(dest, left, right, op)
        };

        if (setsCr)
        {
            ops.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
        }

        return ops;
    }

    private static IReadOnlyList<IrInstruction> CallBinaryWithCr(string dest, string target, string left, string right, bool setsCr)
    {
        var ops = new List<IrInstruction>
        {
            new IrCall(dest, target, new[] { IrValue.Register(left), IrValue.Register(right) })
        };
        if (setsCr)
        {
            ops.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
        }
        return ops;
    }

    private static IReadOnlyList<IrInstruction> CallUnaryWithCr(string dest, string target, string source, bool setsCr)
    {
        var ops = new List<IrInstruction>
        {
            new IrCall(dest, target, new[] { IrValue.Register(source) })
        };
        if (setsCr)
        {
            ops.Add(new IrSetCrField(0, IrValue.Register(dest), IrValue.Imm(0), false));
        }
        return ops;
    }

    private static int ParseImmediate(string text)
    {
        var trimmed = text.Trim();
        var negative = trimmed.StartsWith("-", StringComparison.Ordinal);
        if (negative)
        {
            trimmed = trimmed[1..];
        }

        int value = trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? int.Parse(trimmed[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : int.Parse(trimmed, CultureInfo.InvariantCulture);

        return negative ? -value : value;
    }

private static int SignExtend16(int value)
    {
        return (short)(value & 0xFFFF);
    }

    private static int MaskUimm(int value)
    {
        return value & 0xFFFF;
    }

    private static IrValue BaseOrZero(string reg) => reg == "r0" || reg == "0" ? IrValue.Imm(0) : IrValue.Register(reg);

    private static string PreserveIfAliased(string dest, string source, string tag, List<IrInstruction> instructions)
    {
        if (!string.Equals(dest, source, StringComparison.OrdinalIgnoreCase))
        {
            return source;
        }

        var temp = $"{source}_{tag}";
        instructions.Add(new IrAssign(temp, IrValue.Register(source)));
        return temp;
    }

    private static IReadOnlyList<IrInstruction> DFormLoad(string dest, string baseReg, int offset, int size)
    {
        if (baseReg == "r0" || baseReg == "0")
        {
            var ea = $"{dest}_ea";
            return new IrInstruction[]
            {
                new IrAssign(ea, IrValue.Imm(offset)),
                new IrLoad(dest, new IrAddress(ea, 0), size)
            };
        }

        return new IrInstruction[] { new IrLoad(dest, new IrAddress(baseReg, offset), size) };
    }

    private static IReadOnlyList<IrInstruction> DFormStore(string baseReg, int offset, IrValue src, int size)
    {
        if (baseReg == "r0" || baseReg == "0")
        {
            var ea = $"{src.RegisterName ?? "addr"}_ea";
            return new IrInstruction[]
            {
                new IrAssign(ea, IrValue.Imm(offset)),
                new IrStore(new IrAddress(ea, 0), src, size)
            };
        }

        return new IrInstruction[] { new IrStore(new IrAddress(baseReg, offset), src, size) };
    }

    private static IReadOnlyList<IrInstruction> LiftMtcrfMasked(int crm, string sourceReg)
    {
        // CRM bit 7 maps to CR0 (bits 28-31), bit 0 maps to CR7 (bits 0-3).
        uint mask = 0;
        for (int i = 0; i < 8; i++)
        {
            if ((crm & (1 << i)) != 0)
            {
                mask |= 0xFu << (i * 4);
            }
        }

        if (mask == 0)
        {
            return new IrInstruction[] { new IrComment("mtcrf crm=0 (no-op)") };
        }

        var maskedSrc = $"{sourceReg}_mtcrf_src";
        var maskedCr = $"{sourceReg}_mtcrf_preserve";
        return new IrInstruction[]
        {
            new IrBinary(maskedSrc, IrValue.Register(sourceReg), IrValue.Imm(unchecked((int)mask)), "and"),
            new IrBinary(maskedCr, IrValue.Register("cr"), IrValue.Imm(unchecked((int)~mask)), "and"),
            new IrBinary("cr", IrValue.Register(maskedCr), IrValue.Register(maskedSrc), "or")
        };
    }

    private static (int offset, string @base) ParseDisplacement(string text)
    {
        // pattern: 0x10(r1) or -4(r31)
        var open = text.IndexOf('(');
        var close = text.IndexOf(')');

        if (open == -1 && close == -1)
        {
            // No parentheses. Could be "0x1234" (absolute) or "r3" (implied 0 offset?)
            // If it's a register name, assume 0(reg).
            if (text.StartsWith("r") || text == "sp" || text == "rtoc")
            {
                return (0, text);
            }
            // Assume immediate (absolute address)
            return (ParseImmediate(text), "0");
        }

        if (open == 0 && close > 0)
        {
            // (r3) -> 0(r3)
            var reg = text.Substring(1, close - 1);
            return (0, reg);
        }

        if (open <= 0 || close <= open)
        {
            throw new FormatException($"Unexpected displacement syntax '{text}'.");
        }

        var offsetText = text.Substring(0, open);
        var reg2 = text.Substring(open + 1, close - open - 1);
        return (SignExtend16(ParseImmediate(offsetText)), reg2);
    }

    private static IReadOnlyList<IrInstruction> EmitPairedSingleLoad(string destination, string baseReg, int offset, int w, int quant, bool updateBase, uint address)
    {
        // PPC rule: if rA=0 in D-form, EA = signext(d), not r0 + d
        bool isBaseZero = baseReg == "r0" || baseReg == "0";

        // Update forms (psq_lu) with rA=0 are invalid
        if (updateBase && isBaseZero)
        {
            throw new InvalidOperationException($"psq_lu with rA=0 is invalid (cannot update r0) @ 0x{address:X8}");
        }

        var instructions = new List<IrInstruction>
        {
            new IrComment($"psq_load w={w} quant={quant} (using PPC_PsqL)")
        };

        string addrReg;
        
        if (isBaseZero)
        {
            // EA = signext(d) - just use the offset as the absolute address
            addrReg = $"{destination}_psq_ea";
            instructions.Add(new IrAssign(addrReg, IrValue.Imm(offset)));
            instructions.Add(new IrCall(destination, "PPC_PsqL", new[] { 
                IrValue.Register(addrReg),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
        }
        else if (updateBase)
        {
            addrReg = $"{baseReg}_psq_addr";
            instructions.Add(new IrBinary(addrReg, IrValue.Register(baseReg), IrValue.Imm(offset), "add"));
            // Call helper with w and quant (GQR index)
            instructions.Add(new IrCall(destination, "PPC_PsqL", new[] { 
                IrValue.Register(addrReg),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
            instructions.Add(new IrAssign(baseReg, IrValue.Register(addrReg)));
        }
        else
        {
            // We need a temporary address calculation if offset != 0
            if (offset != 0)
            {
                addrReg = $"{baseReg}_psq_tmp";
                instructions.Add(new IrBinary(addrReg, IrValue.Register(baseReg), IrValue.Imm(offset), "add"));
            }
            else
            {
                addrReg = baseReg;
            }
            instructions.Add(new IrCall(destination, "PPC_PsqL", new[] { 
                IrValue.Register(addrReg),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
        }

        return instructions;
    }

    private static IReadOnlyList<IrInstruction> EmitPairedSingleStore(string source, string baseReg, int offset, int w, int quant, bool updateBase, uint address)
    {
        // PPC rule: if rA=0 in D-form, EA = signext(d), not r0 + d
        bool isBaseZero = baseReg == "r0" || baseReg == "0";

        // Update forms (psq_stu) with rA=0 are invalid
        if (updateBase && isBaseZero)
        {
            throw new InvalidOperationException($"psq_stu with rA=0 is invalid (cannot update r0) @ 0x{address:X8}");
        }

        var instructions = new List<IrInstruction>
        {
            new IrComment($"psq_store w={w} quant={quant} (using PPC_PsqSt)")
        };

        string addrReg;

        if (isBaseZero)
        {
            // EA = signext(d) - just use the offset as the absolute address
            addrReg = $"{source}_psq_ea";
            instructions.Add(new IrAssign(addrReg, IrValue.Imm(offset)));
            instructions.Add(new IrCall(string.Empty, "PPC_PsqSt", new[] { 
                IrValue.Register(addrReg),
                IrValue.Register(source),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
        }
        else if (updateBase)
        {
            // Calculate EA into temp, Store, then Assign EA to baseReg.
            // Manual says: EA = (RA) + d. MEM(EA, ...) = ... RA <- EA.
            var ea = $"{baseReg}_psq_ea";
            instructions.Add(new IrBinary(ea, IrValue.Register(baseReg), IrValue.Imm(offset), "add"));
            
            // Call PPC_PsqSt with W and I (quant/GQR index)
            instructions.Add(new IrCall(string.Empty, "PPC_PsqSt", new[] { 
                IrValue.Register(ea),
                IrValue.Register(source),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
            instructions.Add(new IrAssign(baseReg, IrValue.Register(ea)));
        }
        else
        {
            if (offset != 0)
            {
                addrReg = $"{baseReg}_psq_tmp";
                instructions.Add(new IrBinary(addrReg, IrValue.Register(baseReg), IrValue.Imm(offset), "add"));
            }
            else
            {
                addrReg = baseReg;
            }
            // Call PPC_PsqSt with W and I (quant/GQR index)
            instructions.Add(new IrCall(string.Empty, "PPC_PsqSt", new[] { 
                IrValue.Register(addrReg),
                IrValue.Register(source),
                IrValue.Imm(w),
                IrValue.Imm(quant)
            }));
        }

        return instructions;
    }

    private static readonly Dictionary<string, int> CrBitAliasLookup = new(StringComparer.OrdinalIgnoreCase)
    {
        ["cr0lt"] = 0, ["cr0gt"] = 1, ["cr0eq"] = 2, ["cr0so"] = 3,
        ["cr1lt"] = 4, ["cr1gt"] = 5, ["cr1eq"] = 6, ["cr1so"] = 7,
        ["cr2lt"] = 8, ["cr2gt"] = 9, ["cr2eq"] = 10, ["cr2so"] = 11,
        ["cr3lt"] = 12, ["cr3gt"] = 13, ["cr3eq"] = 14, ["cr3so"] = 15,
        ["cr4lt"] = 16, ["cr4gt"] = 17, ["cr4eq"] = 18, ["cr4so"] = 19,
        ["cr5lt"] = 20, ["cr5gt"] = 21, ["cr5eq"] = 22, ["cr5so"] = 23,
        ["cr6lt"] = 24, ["cr6gt"] = 25, ["cr6eq"] = 26, ["cr6so"] = 27,
        ["cr7lt"] = 28, ["cr7gt"] = 29, ["cr7eq"] = 30, ["cr7so"] = 31
    };

    private static int ParseCrBitIndex(string operand)
    {
        if (string.IsNullOrWhiteSpace(operand))
        {
            return 0;
        }

        if (CrBitAliasLookup.TryGetValue(operand, out var aliasBit))
        {
            return aliasBit;
        }

        var trimmed = operand.Trim();
        
        // Handle the crb{N} format (explicit CR bit number)
        if (trimmed.StartsWith("crb", StringComparison.OrdinalIgnoreCase))
        {
            var bitSuffix = trimmed[3..];
            if (int.TryParse(bitSuffix, NumberStyles.Integer, CultureInfo.InvariantCulture, out var bitNum))
            {
                return Math.Clamp(bitNum, 0, 31);
            }
        }
        
        // Handle cr{N} format (could be CR field or CR bit depending on context)
        if (trimmed.StartsWith("cr", StringComparison.OrdinalIgnoreCase))
        {
            var suffix = trimmed[2..];
            if (int.TryParse(suffix, NumberStyles.Integer, CultureInfo.InvariantCulture, out var direct))
            {
                return Math.Clamp(direct, 0, 31);
            }

            var digits = new string(suffix.TakeWhile(char.IsDigit).ToArray());
            if (digits.Length > 0 && int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out var field))
            {
                var remainder = suffix[digits.Length..].ToLowerInvariant();
                var bitInField = remainder switch
                {
                    "lt" => 0,
                    "gt" => 1,
                    "eq" => 2,
                    "so" => 3,
                    _ => 0
                };
                return Math.Clamp(field * 4 + bitInField, 0, 31);
            }
        }

        if (int.TryParse(trimmed, NumberStyles.Integer, CultureInfo.InvariantCulture, out var numeric))
        {
            return Math.Clamp(numeric, 0, 31);
        }

        return 0;
    }

    private static int ParseCrFieldName(string operand)
    {
        if (string.IsNullOrWhiteSpace(operand))
        {
            return 0;
        }

        var trimmed = operand.Trim();
        if (trimmed.StartsWith("crf", StringComparison.OrdinalIgnoreCase))
        {
            trimmed = trimmed[3..];
        }
        else if (trimmed.StartsWith("cr", StringComparison.OrdinalIgnoreCase))
        {
            trimmed = trimmed[2..];
        }

        if (int.TryParse(trimmed, NumberStyles.Integer, CultureInfo.InvariantCulture, out var field))
        {
            return Math.Clamp(field, 0, 7);
        }

        return 0;
    }

    private static IReadOnlyList<IrInstruction> LiftCrLogical(string mnemonic, string targetBit, string leftBit, string rightBit)
    {
        var opcode = mnemonic switch
        {
            "crnor" => 0,
            "crandc" => 1,
            "crxor" => 2,
            "crnand" => 3,
            "crand" => 4,
            "creqv" => 5,
            "crorc" => 6,
            "cror" => 7,
            _ => 0
        };

        return new[]
        {
            new IrCall("cr", "PPC_CrLogical", new[]
            {
                IrValue.Imm(opcode),
                IrValue.Imm(ParseCrBitIndex(targetBit)),
                IrValue.Imm(ParseCrBitIndex(leftBit)),
                IrValue.Imm(ParseCrBitIndex(rightBit))
            })
        };
    }

private static string TargetLabel(PpcInstruction ins, HashSet<uint> validAddresses, bool preferFallthrough = true)
{
    // FIX: Check if any targets exist rather than checking if the value is 0
    if (ins.BranchTargets == null || ins.BranchTargets.Count == 0)
    {
        throw new InvalidOperationException($"Branch target missing for instruction at 0x{ins.Address:X8}");
    }
    
    var target = ins.BranchTargets[0];
    if (!validAddresses.Contains(target) && preferFallthrough)
    {
        // If the target wasn't decoded in this slice, prefer the fallthrough so SSA stays consistent.
        return $"0x{ins.EndAddress:X8}";
    }
    return $"0x{target:X8}";
}

    private static string BuildBoConditionExpression(int bo, int bi, bool allowCtr)
    {
        var crFieldIdx = bi / 4;
        var crBitIdx = bi % 4;
        var branchIfTrue = ((bo >> 3) & 1) != 0;
        var dontCheckCtr = ((bo >> 2) & 1) != 0;
        var dontCheckCondition = ((bo >> 4) & 1) != 0;
        var ctrZero = ((bo >> 1) & 1) != 0;

        var ctrExpr = !allowCtr || dontCheckCtr
            ? "true"
            : $"(((ctx->ctr != 0) ^ {(ctrZero ? "true" : "false")}))";
        var crExpr = dontCheckCondition
            ? "true"
            : $"(GetCRBit(ctx, {crFieldIdx}, {crBitIdx}) == {(branchIfTrue ? "true" : "false")})";
        return $"(({ctrExpr}) && ({crExpr}))";
    }

    private static uint GetMask(int mb, int me)
    {
        uint mask = 0;
        if (mb <= me)
        {
            for (int i = mb; i <= me; i++) mask |= (0x80000000u >> i);
        }
        else
        {
            for (int i = mb; i <= 31; i++) mask |= (0x80000000u >> i);
            for (int i = 0; i <= me; i++) mask |= (0x80000000u >> i);
        }
        return mask;
    }

    private static int ParseRegisterNumber(string reg)
    {
        if (reg.StartsWith("r", StringComparison.OrdinalIgnoreCase) && int.TryParse(reg.Substring(1), out var num))
        {
            return num;
        }
        throw new FormatException($"Invalid register: {reg}");
    }

    private static IrInstruction[] BuildIndexedAddress(string rA, string rB, string addrReg, Func<string, IrInstruction[]> buildOps)
{
    var ops = new List<IrInstruction>();
    
    if (string.Equals(rA, "r0", StringComparison.OrdinalIgnoreCase))
    {
        // (rA|0) + rB => base is 0 when rA is r0
        ops.Add(new IrAssign(addrReg, IrValue.Register(rB)));
    }
    else
    {
        ops.Add(new IrBinary(addrReg, IrValue.Register(rA), IrValue.Register(rB), "add"));
    }
    
    ops.AddRange(buildOps(addrReg));
    return ops.ToArray();
}

    private static IReadOnlyList<IrInstruction> LiftPairedSingleIndexedLoad(PpcInstruction ins, uint rawInstr, bool updateBase)
    {
        int rD = (int)((rawInstr >> 21) & 0x1F);
        int rA = (int)((rawInstr >> 16) & 0x1F);
        int rB = (int)((rawInstr >> 11) & 0x1F);
        int w = (int)((rawInstr >> 10) & 1);
        int i = (int)((rawInstr >> 7) & 7);

        // EA = (rA|0) + rB
        string addrReg = $"addr_psqx_{ins.Address:X8}_loc";
        var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => 
        {
            // Call PPC_PsqL with w and i (GQR index)
            return new[] 
            { 
               new IrCall($"f{rD}", "PPC_PsqL", new[] { 
                   IrValue.Register(addr),
                   IrValue.Imm(w),
                   IrValue.Imm(i)
               })
            };
        }));
        
        if (updateBase)
        {
             if (rA == 0) throw new InvalidOperationException($"psq_lux/stux with rA=0 is invalid @ 0x{ins.Address:X8}");
             instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
        }
        
        return instructions;
    }

    private static IReadOnlyList<IrInstruction> LiftPairedSingleIndexedStore(PpcInstruction ins, uint rawInstr, bool updateBase)
    {
        int rS = (int)((rawInstr >> 21) & 0x1F);
        int rA = (int)((rawInstr >> 16) & 0x1F);
        int rB = (int)((rawInstr >> 11) & 0x1F);
        int w = (int)((rawInstr >> 10) & 1);
        int i = (int)((rawInstr >> 7) & 7);

        string addrReg = $"addr_psqx_{ins.Address:X8}_loc";
        var instructions = new List<IrInstruction>(BuildIndexedAddress($"r{rA}", $"r{rB}", addrReg, addr => 
        {
            // Call PPC_PsqSt with w and i (GQR index)
            return new[] 
            { 
               new IrCall(string.Empty, "PPC_PsqSt", new[] { 
                   IrValue.Register(addr), 
                   IrValue.Register($"f{rS}"),
                   IrValue.Imm(w),
                   IrValue.Imm(i)
               })
            };
        }));
        
        if (updateBase)
        {
             if (rA == 0) throw new InvalidOperationException($"psq_lux/stux with rA=0 is invalid @ 0x{ins.Address:X8}");
             instructions.Add(new IrAssign($"r{rA}", IrValue.Register(addrReg)));
        }

        return instructions;
    }
}
