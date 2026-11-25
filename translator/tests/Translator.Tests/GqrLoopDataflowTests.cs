using Translator.Core.Ir;
using Translator.Core.Translation;

namespace Translator.Tests;

public sealed class GqrLoopDataflowTests
{
    private const uint Initial = 0x00070007;
    private const uint Different = 0x00050005;
    private const uint Preserved = 0x3D043D04;
    private const uint Root = 0x80001000;
    private const uint Loop = 0x80002000;
    private const uint Sink = 0x80003000;

    public static IEnumerable<object[]> LoopCases()
    {
        foreach (var shape in new[] { "self", "entry-self", "entry-backedge", "multiple-backedges" })
        foreach (var write in new[] { "same", "different", "unknown" })
        foreach (var reverse in new[] { false, true })
            yield return [shape, write, reverse];
    }

    [Theory]
    [MemberData(nameof(LoopCases))]
    public void OrdinaryFlowMergesEveryLoopEdge(string shape, string write, bool reverse)
    {
        var function = LoopFunction(shape, write, reverse);
        var seed = new Dictionary<string, uint> { ["gqr5"] = Initial, ["gqr6"] = Preserved };
        var masks = new Dictionary<uint, byte> { [Sink] = 0 };
        var flow = GqrConstantPropagation.Analyze(function, seed, masks);
        AssertFacts(flow.In["loop"], write);
        if (write != "unknown")
            Assert.Equal(write == "same" ? Initial : Different, flow.In["exit"]["gqr5"]);
        else
            Assert.False(flow.In["exit"].ContainsKey("gqr5"));

        var specialized = GqrConstantPropagation.Specialize(function, seed, masks);
        var header = specialized.Blocks.Single(block => block.Label == "loop");
        var load = Assert.Single(header.Instructions.OfType<IrResolvedPsqLoad>());
        var store = Assert.Single(header.Instructions.OfType<IrResolvedPsqStore>());
        Assert.Equal(write == "same" ? (uint?)Initial : null, load.KnownGqr);
        Assert.Equal(Preserved, store.KnownGqr);
    }

    [Theory]
    [MemberData(nameof(LoopCases))]
    public void CompactFlowMergesEveryLoopEdgeAtTheCallSite(string shape, string write, bool reverse)
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [Root] = new("root", "entry", [new IrBasicBlock("entry",
            [
                new IrAssign("gqr5", IrValue.Imm(Initial)),
                new IrAssign("gqr6", IrValue.Imm(Preserved)),
                new IrCall(string.Empty, $"0x{Loop:X8}", []),
                new IrReturn(null)
            ])]),
            [Loop] = LoopFunction(shape, write, reverse),
            [Sink] = new("sink", "entry", [new IrBasicBlock("entry", [new IrReturn(null)])])
        };
        var summaries = functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value));
        var result = GqrInterproceduralAnalysis.Analyze(summaries, [Root]);
        Assert.Equal(Initial, result.EntryConstants[Loop]["gqr5"]);
        AssertFacts(result.EntryConstants[Sink], write);
        Assert.Contains(Sink, result.ReachableFunctions);
    }

    [Fact]
    public void EntryBackedgeCannotInventAConstantMissingFromTheEntrySeed()
    {
        var function = LoopFunction("entry-backedge", "same", false);
        var flow = GqrConstantPropagation.Analyze(function, calleeWriteMasks: new Dictionary<uint, byte> { [Sink] = 0 });
        Assert.Empty(flow.In["loop"]);
    }

    [Fact]
    public void CompactEntryBackedgeCannotInventAConstantMissingFromTheEntrySeed()
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [Loop] = LoopFunction("entry-backedge", "same", false),
            [Sink] = new("sink", "entry", [new IrBasicBlock("entry", [new IrReturn(null)])])
        };
        var result = GqrInterproceduralAnalysis.Analyze(
            functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value)), [Loop]);
        Assert.Empty(result.EntryConstants[Sink]);
    }

    private static void AssertFacts(IReadOnlyDictionary<string, uint> facts, string write)
    {
        if (write == "same") Assert.Equal(Initial, facts["gqr5"]);
        else Assert.False(facts.ContainsKey("gqr5"));
        Assert.Equal(Preserved, facts["gqr6"]);
    }

    private static IrFunction LoopFunction(string shape, string write, bool reverse)
    {
        var blocks = new List<IrBasicBlock>();
        var header = new List<IrInstruction>
        {
            new IrResolvedPsqLoad("f1", "range", IrValue.Register("r4"), 0, 0, 5, null),
            new IrResolvedPsqStore("range", IrValue.Register("r4"), 0, IrValue.Register("f1"), 1, 6, null),
            new IrCall(string.Empty, $"0x{Sink:X8}", [])
        };
        var rewritten = write switch
        {
            "same" => IrValue.Imm(Initial),
            "different" => IrValue.Imm(Different),
            _ => IrValue.Register("r3")
        };
        if (shape is "self" or "entry-self")
        {
            header.Add(new IrAssign("gqr5", rewritten));
            header.Add(new IrBranch("beq", "loop", "exit"));
        }
        else if (shape == "entry-backedge")
        {
            header.Add(new IrJump("back"));
            blocks.Add(new IrBasicBlock("back",
                [new IrAssign("gqr5", rewritten), new IrBranch("beq", "loop", "exit")]));
        }
        else
        {
            header.Add(new IrBranch("beq", "left", "right"));
            blocks.Add(new IrBasicBlock("left", [new IrAssign("gqr5", IrValue.Imm(Initial)), new IrJump("loop")]));
            blocks.Add(new IrBasicBlock("right", [new IrAssign("gqr5", rewritten), new IrBranch("beq", "loop", "exit")]));
        }
        blocks.Add(new IrBasicBlock("loop", header));
        blocks.Add(new IrBasicBlock("exit", [new IrReturn(null)]));
        var entry = shape == "self" ? "entry" : "loop";
        if (entry == "entry") blocks.Insert(0, new IrBasicBlock("entry", [new IrJump("loop")]));
        if (reverse) blocks.Reverse();
        return new IrFunction("loop", entry, blocks);
    }
}
