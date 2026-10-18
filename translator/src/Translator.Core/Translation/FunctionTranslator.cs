using System;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using Translator.Core.Analysis.BasicBlocks;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Translator.Core.Loading;
using Translator.Core.Representation;
using Translator.Core;

namespace Translator.Core.Translation;

/// <summary>
/// Options that control how a single function is translated.
/// </summary>
public sealed record TranslationOptions(
    string? PreferredName = null,
    int MaxInstructions = 8192,
    int MaxBytes = 0x10000,
    bool AllowUnsupportedInstructions = false,
    bool EmitModRegistration = false,
    uint ModRegistrationPriority = 100,
    ulong ModRegistrationModuleId = 1,
    IReadOnlySet<uint>? NonReturningCallTargets = null,
    IReadOnlySet<uint>? LrContinuationCallTargets = null,
    IReadOnlyDictionary<uint, uint>? LinkedCallFallthroughLrOverrides = null,
    IReadOnlySet<uint>? KnownFunctionEntryPoints = null,
    IReadOnlyDictionary<uint, GuestAbiContract>? GuestAbiContracts = null,
    bool EmitStateFreeLeafVariant = false,
    IReadOnlyDictionary<uint, GuestAbiContract>? StateFreeAbiContracts = null,
    IReadOnlyDictionary<uint, string>? StateFreeCallSymbols = null,
    IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>? StateFreeCallSiteVariants = null,
    IReadOnlyList<GuestStateFreeCallVariant>? StateFreeEntryVariants = null,
    uint? ModuleLinkBase = null,
    uint? ModuleGuestBase = null,
    uint ModuleLinkedCodeSize = 0,
    IReadOnlyDictionary<string, uint>? GqrEntryConstants = null,
    IReadOnlyDictionary<uint, byte>? GqrCalleeWriteMasks = null,
    bool GqrConstantsRequireRuntimeGuard = false,
    bool EnableLeafAbiSpillElision = false,
    IReadOnlySet<uint>? ModOverridableCallTargets = null,
    bool GenerateCxx = true,
    bool DiscoveryOnly = false,
    /// <summary>
    /// Splices tiny, call-free leaf callees into their direct callers at the IR level; the callee
    /// is still translated separately, only the caller's call edge disappears. Always on: the
    /// inliner clears it on callee-decode options to stop a splice from recursing into another.
    /// </summary>
    bool EnableLeafInlining = true,
    /// <summary>
    /// Addresses whose translated body is not the runtime winner - native
    /// registrations, base-translation exclusions and mod patch sites. These are
    /// never inlined, because the splice would bypass the replacement.
    /// </summary>
    IReadOnlySet<uint>? LeafInliningBlockedTargets = null,
    int LeafInliningMaxCalleeInstructions = 56,
    int LeafInliningMaxCallerGrowthPercent = 50,
    /// <summary>
    /// Admit a callee whose blocks are not a straight line, provided its CFG is
    /// acyclic and every exit is a plain return. Emergency opt-out; the
    /// straight-line shape keeps working with this off.
    /// </summary>
    bool LeafInliningAllowMultiBlockCallees = true)
{
    public static TranslationOptions Default { get; } = new();
}

/// <summary>
/// Lightweight profiling and quality metrics captured during translation.
/// </summary>
public sealed class TranslationMetrics
{
    public TimeSpan Total { get; internal set; }
    public TimeSpan Disassembly { get; internal set; }
    public TimeSpan BasicBlocks { get; internal set; }
    public TimeSpan Lifting { get; internal set; }
    public TimeSpan Ssa { get; internal set; }
    public TimeSpan RepresentationClassification { get; internal set; }
    public TimeSpan CodeGen { get; internal set; }

    public int PpcInstructionCount { get; internal set; }
    public int BasicBlockCount { get; internal set; }
    public int IrInstructionCount { get; internal set; }
    public int IrBlockCount { get; internal set; }
    public int UnsupportedInstructionCount { get; internal set; }

    /// <summary>Direct call sites replaced by a spliced leaf body.</summary>
    public int InlinedCallSites { get; internal set; }

    /// <summary>Guest instructions contributed by those spliced bodies.</summary>
    public int InlinedGuestInstructions { get; internal set; }
}

/// <summary>
/// Full set of artifacts emitted by a translation of a single function.
/// </summary>
public sealed record FunctionTranslationResult(
    uint EntryPoint,
    string Name,
    IReadOnlyList<PpcInstruction> Instructions,
    IReadOnlyList<BasicBlock> Blocks,
    IrFunction LinearIr,
    SsaResult Ssa,
    RepresentationEnvironment Representations,
    FunctionAbiClassification AbiClassification,
    string CxxCode,
    TranslationMetrics Metrics,
    /// <summary>
    /// Facts the emitter derived while producing <see cref="CxxCode"/>. Null when
    /// C++ generation was skipped. Consumers must read emission facts from here
    /// rather than re-parsing <see cref="CxxCode"/>.
    /// </summary>
    CxxEmissionResult? Emission = null);

/// <summary>
/// Front-end artifacts needed while recursively discovering a program. This
/// deliberately excludes representation and return-ABI classification, ABI contracts, and C++ output.
/// </summary>
public sealed record FunctionDiscoveryResult(
    uint EntryPoint,
    string Name,
    IReadOnlyList<PpcInstruction> Instructions,
    IReadOnlyList<BasicBlock> Blocks,
    IrFunction LinearIr,
    SsaResult Ssa,
    TranslationMetrics Metrics);

/// <summary>
/// End-to-end pipeline that takes a RAM image and produces classified C++ for a
/// single PowerPC function while recording per-stage metrics.
/// </summary>
public sealed class FunctionTranslator
{
    private readonly ProgramImage _image;
    private readonly IGuestFunctionAbiProvider _guestAbiProvider;

    /// <summary>
    /// Program-wide memo of speculative leaf-inline decodes, keyed by front-end context, so a
    /// callee (a pure function of image/boundary answers/policy) is never redecoded per caller.
    /// </summary>
    private readonly ConcurrentDictionary<LeafInlineMemoContext, ConcurrentDictionary<uint, LeafInlineMemoEntry>>
        _leafInlineMemo = new();

    public FunctionTranslator(
        ProgramImage image,
        IGuestFunctionAbiProvider? guestAbiProvider = null)
    {
        _image = image;
        _guestAbiProvider = guestAbiProvider ?? new InferredGuestFunctionAbiProvider(_image);
    }

    /// <summary>
    /// Everything besides the callee address that a speculative decode depends on. Compared by
    /// identity since contents mutate during a run; content drift is handled by the probe log instead.
    /// </summary>
    private sealed record LeafInlineMemoContext(
        IReadOnlySet<uint>? KnownFunctionEntryPoints,
        IReadOnlySet<uint>? BlockedTargets,
        int MaxCalleeGuestInstructions,
        int MaxCalleeBlocks,
        bool AllowAcyclicMultiBlockCallees,
        int MaxCalleeAcyclicBlocks,
        int MaxInstructions,
        int MaxBytes,
        bool AllowUnsupportedInstructions);

    /// <summary>
    /// A memoized decision plus the boundary questions it was derived from.
    /// Replaying the decode is equivalent to reusing this entry exactly while
    /// every one of those questions still has the same answer.
    /// </summary>
    private sealed record LeafInlineMemoEntry(LeafInlineCandidate? Candidate, BoundaryProbeLog Probes);

    public FunctionTranslationResult Translate(uint entryPoint, TranslationOptions? options = null)
    {
        options ??= TranslationOptions.Default;
        var metrics = new TranslationMetrics();
        var total = Stopwatch.StartNew();

        // Disassemble
        using var disassembler = new PpcDisassembler();
        // The full pipeline never tolerates a budget overrun, so the result is
        // non-null here.
        var irFunction = BuildLinearIr(
            disassembler, entryPoint, options, metrics, out var instructions, out var basicBlocks)!;
        irFunction = ApplyLeafInlining(disassembler, entryPoint, irFunction, options, metrics);
        metrics.IrInstructionCount = irFunction.Blocks.Sum(static b => b.Instructions.Count);
        metrics.IrBlockCount = irFunction.Blocks.Count;

        // SSA + validation.
        var ssaSw = Stopwatch.StartNew();
        SsaResult ssa;
        try
        {
            ssa = new SsaTransformer().Convert(irFunction);
            ssa.ValidateUseDef();
        }
        finally
        {
            ssaSw.Stop();
        }
        metrics.Ssa = ssaSw.Elapsed;

        var functionName = irFunction.Name;
        return FinishTranslation(
            entryPoint, functionName, instructions, basicBlocks, irFunction, ssa, options, metrics, total);
    }

    /// <summary>Front-end stages (decode, blocks, lifting, linear IR), reused so a leaf-inlining
    /// candidate goes through the exact same pipeline as the caller.</summary>
    /// <param name="boundaryProbes">Records every "not a boundary" answer so the result can later be
    /// proven still current.</param>
    /// <param name="toleratesBudgetOverrun">Speculative decoding treats an oversized body as null
    /// instead of throwing.</param>
    private IrFunction? BuildLinearIr(
        PpcDisassembler disassembler,
        uint entryPoint,
        TranslationOptions options,
        TranslationMetrics? metrics,
        out IReadOnlyList<PpcInstruction> instructions,
        out IReadOnlyList<BasicBlock> basicBlocks,
        BoundaryProbeLog? boundaryProbes = null,
        bool toleratesBudgetOverrun = false)
    {
        var disSw = Stopwatch.StartNew();
        if (toleratesBudgetOverrun)
        {
            var decoded = disassembler.TryDisassembleFunction(
                _image,
                entryPoint,
                options.MaxInstructions,
                options.MaxBytes,
                options.KnownFunctionEntryPoints,
                boundaryProbes);
            if (decoded is null)
            {
                instructions = Array.Empty<PpcInstruction>();
                basicBlocks = Array.Empty<BasicBlock>();
                return null;
            }

            instructions = decoded;
        }
        else
        {
            instructions = disassembler.DisassembleFunction(
                _image,
                entryPoint,
                options.MaxInstructions,
                options.MaxBytes,
                options.KnownFunctionEntryPoints,
                boundaryProbes);
        }
        disSw.Stop();
        if (metrics is not null)
        {
            metrics.Disassembly = disSw.Elapsed;
            metrics.PpcInstructionCount = instructions.Count;
        }

        // Build CFG-ready basic blocks.
        var bbSw = Stopwatch.StartNew();
        basicBlocks = BasicBlockBuilder.Build(instructions, new[] { entryPoint });
        bbSw.Stop();
        if (metrics is not null)
        {
            metrics.BasicBlocks = bbSw.Elapsed;
            metrics.BasicBlockCount = basicBlocks.Count;
        }

        // Lift to linear IR.
        var lifter = new PpcLifter();
        var liftSw = Stopwatch.StartNew();
        var lifted = lifter.Lift(
            instructions,
            options.AllowUnsupportedInstructions,
            entryPoint,
            options.KnownFunctionEntryPoints);
        if (metrics is not null)
        {
            var unsupported = 0;
            foreach (var instruction in lifted)
            {
                foreach (var ir in instruction.Ir)
                {
                    if (ir is IrUndefined)
                    {
                        unsupported++;
                    }
                }
            }

            metrics.UnsupportedInstructionCount = unsupported;
        }
        liftSw.Stop();
        if (metrics is not null)
        {
            metrics.Lifting = liftSw.Elapsed;
        }

        var liftedByAddress = new Dictionary<uint, IReadOnlyList<IrInstruction>>(lifted.Count);
        foreach (var entry in lifted)
        {
            liftedByAddress[entry.Origin.Address] = entry.Ir;
        }

        var decodedAddresses = new HashSet<uint>(instructions.Count);
        foreach (var instruction in instructions)
        {
            decodedAddresses.Add(instruction.Address);
        }

        var irBlocks = new List<IrBasicBlock>(basicBlocks.Count);
        foreach (var block in basicBlocks)
        {
            var ir = new List<IrInstruction>();
            foreach (var ins in block.Instructions)
            {
                if (!liftedByAddress.TryGetValue(ins.Address, out var lowered))
                {
                    throw new InvalidOperationException($"Failed to lift instruction at 0x{ins.Address:X8}");
                }

                AppendLoweredWithTrace(ir, lowered, CreateTrace(ins));
            }

            var last = block.Instructions[^1];
            if (!last.IsReturn &&
                !last.IsConditionalBranch &&
                !last.IsUnconditionalBranch &&
                !decodedAddresses.Contains(last.EndAddress))
            {
                var fallthroughAddress = last.EndAddress;
                var fallthroughIsBoundary = options.KnownFunctionEntryPoints?.Contains(fallthroughAddress) == true;
                if (fallthroughIsBoundary)
                {
                    ir.Add(new IrJump(LabelFor(fallthroughAddress)));
                }
                else
                {
                    boundaryProbes?.RecordAbsent(fallthroughAddress);
                }
            }

            var label = LabelFor(block.StartAddress);
            irBlocks.Add(new IrBasicBlock(label, ir));
        }

        // Ensure every referenced label has a basic block so SSA construction cannot fail on missing targets.
        var existingLabels = new HashSet<string>();
        foreach (var block in irBlocks)
        {
            existingLabels.Add(block.Label);
        }

        // Branch labels first, then jump labels - the order synthetic blocks get
        // appended in depends on it. Collected in one traversal, and the
        // return-block question is answered by the same pass because no
        // synthetic block below ever contributes an IrBranch.
        var branchLabels = new List<string>();
        var jumpLabels = new List<string>();
        var needsReturnBlock = false;
        foreach (var block in irBlocks)
        {
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrBranch branch:
                        if (!string.IsNullOrWhiteSpace(branch.TrueLabel))
                        {
                            branchLabels.Add(branch.TrueLabel);
                        }

                        if (!string.IsNullOrWhiteSpace(branch.FalseLabel))
                        {
                            branchLabels.Add(branch.FalseLabel);
                        }

                        if (string.Equals(branch.TrueLabel, "return", StringComparison.OrdinalIgnoreCase) ||
                            string.Equals(branch.FalseLabel, "return", StringComparison.OrdinalIgnoreCase))
                        {
                            needsReturnBlock = true;
                        }

                        break;
                    case IrJump jump:
                        if (!string.IsNullOrWhiteSpace(jump.TargetLabel))
                        {
                            jumpLabels.Add(jump.TargetLabel);
                        }

                        break;
                }
            }
        }

        var referencedLabels = new List<string>(branchLabels.Count + jumpLabels.Count);
        referencedLabels.AddRange(branchLabels);
        referencedLabels.AddRange(jumpLabels);

        foreach (var label in referencedLabels)
        {
            if (!existingLabels.Contains(label))
            {
                if (string.Equals(label, "return", StringComparison.OrdinalIgnoreCase))
                {
                    // Defer creation of the return block so the dedicated return-creation logic below can
                    // insert a proper block that emits an IrReturn instruction with the ABI return register.
                    continue;
                }

                if (TryBuildSyntheticBlock(label, existingLabels, out var synth))
                {
                    irBlocks.Add(synth);
                    existingLabels.Add(label);
                    continue;
                }

                var missing = new IrBasicBlock(label, new IrInstruction[] { new IrComment("synthetic missing target") });
                irBlocks.Add(missing);
                existingLabels.Add(label);
            }
        }

        // Some branch forms (e.g., beqlr) target the link register; model these as jumps
        // to a synthetic return block if referenced.
        if (needsReturnBlock)
        {
            var hasReturnBlock = false;
            foreach (var block in irBlocks)
            {
                if (string.Equals(block.Label, "return", StringComparison.OrdinalIgnoreCase))
                {
                    hasReturnBlock = true;
                    break;
                }
            }

            if (!hasReturnBlock)
            {
                irBlocks.Add(new IrBasicBlock("return", new IrInstruction[] { new IrReturn(null) }));
            }
        }
        if (options.LinkedCallFallthroughLrOverrides is { Count: > 0 })
        {
            irBlocks = ApplyLinkedCallFallthroughLrOverrides(irBlocks, options.LinkedCallFallthroughLrOverrides);
        }

        var functionName = options.PreferredName ?? $"func_{entryPoint:X8}";
        var entryLabel = LabelFor(entryPoint);
        var hasEntryBlock = false;
        foreach (var block in irBlocks)
        {
            if (string.Equals(block.Label, entryLabel, StringComparison.OrdinalIgnoreCase))
            {
                hasEntryBlock = true;
                break;
            }
        }

        if (!hasEntryBlock)
        {
            entryLabel = irBlocks.Count > 0 ? irBlocks[0].Label : "entry";
        }
        return new IrFunction(functionName, entryLabel, irBlocks);
    }

    /// <summary>
    /// Replaces admitted direct calls with the callee's IR before SSA runs, so
    /// the spliced guest instructions become ordinary instructions of the
    /// caller for every later analysis and lowering.
    /// </summary>
    private IrFunction ApplyLeafInlining(
        PpcDisassembler disassembler,
        uint entryPoint,
        IrFunction function,
        TranslationOptions options,
        TranslationMetrics metrics)
    {
        if (!IsLeafInliningEligibleCaller(options))
        {
            return function;
        }

        var policy = new LeafInliningPolicy(
            MaxCalleeGuestInstructions: Math.Max(1, options.LeafInliningMaxCalleeInstructions),
            MaxCallerGrowthPercent: Math.Max(0, options.LeafInliningMaxCallerGrowthPercent),
            BlockedTargets: options.LeafInliningBlockedTargets,
            AllowAcyclicMultiBlockCallees: options.LeafInliningAllowMultiBlockCallees);

        // Shared across callers: a callee decode is a pure function of (image, boundary answers,
        // policy). Each entry carries the boundary questions it asked, so it gets recomputed the
        // moment discovery adds an entry point that would answer one of them differently.
        var memoContext = new LeafInlineMemoContext(
            options.KnownFunctionEntryPoints,
            policy.BlockedTargets,
            policy.MaxCalleeGuestInstructions,
            policy.MaxCalleeBlocks,
            policy.AllowAcyclicMultiBlockCallees,
            policy.MaxCalleeAcyclicBlocks,
            options.MaxInstructions,
            options.MaxBytes,
            options.AllowUnsupportedInstructions);
        var candidates = _leafInlineMemo.GetOrAdd(memoContext, static _ => new ConcurrentDictionary<uint, LeafInlineMemoEntry>());
        var knownEntryPoints = options.KnownFunctionEntryPoints;

        LeafInlineCandidate? Resolve(uint target)
        {
            // A self-call is a caller-local fact and must never enter the
            // shared memo.
            if (target == entryPoint)
            {
                return null;
            }

            if (candidates.TryGetValue(target, out var cached) && cached.Probes.IsStillValid(knownEntryPoints))
            {
                return cached.Candidate;
            }

            var probes = new BoundaryProbeLog();
            LeafInlineCandidate? candidate = null;
            try
            {
                // Decoding is capped one instruction past the admissible
                // size, so an oversized callee is refused by the decoder
                // instead of being fully lifted.
                var maximum = policy.MaxCalleeGuestInstructions + 1;
                var calleeOptions = options with
                {
                    PreferredName = null,
                    MaxInstructions = Math.Min(options.MaxInstructions, maximum),
                    MaxBytes = Math.Min(options.MaxBytes, maximum * 4),
                    EnableLeafInlining = false,
                    LinkedCallFallthroughLrOverrides = null
                };
                var calleeIr = BuildLinearIr(
                    disassembler,
                    target,
                    calleeOptions,
                    null,
                    out var calleeInstructions,
                    out _,
                    probes,
                    toleratesBudgetOverrun: true);
                candidate = calleeIr is null
                    ? null
                    : LeafFunctionInliner.TryCreateCandidate(
                        target, calleeIr, calleeInstructions, policy, out _);
            }
            catch (Exception ex) when (ex is InvalidOperationException or ArgumentException
                                          or IndexOutOfRangeException or NotSupportedException
                                          or OverflowException)
            {
                // Speculative decoding is extra work the caller's own translation never depended
                // on, so a failure here must never abort that translation; treat it as "not a
                // candidate" and keep the ordinary call. Probes recorded before the failure remain valid.
                candidate = null;
            }

            candidates[target] = new LeafInlineMemoEntry(candidate, probes);
            return candidate;
        }

        var inlined = LeafFunctionInliner.Inline(function, policy, Resolve);
        metrics.InlinedCallSites = inlined.InlinedCallSites;
        metrics.InlinedGuestInstructions = inlined.InlinedGuestInstructions;
        return inlined.Function;
    }

    /// <summary>
    /// Excludes bodies whose call sites carry extra structure (continuation/non-returning dispatch
    /// labels keyed by guest address, module link-address remapping) that can't survive duplication.
    /// </summary>
    private static bool IsLeafInliningEligibleCaller(TranslationOptions options) =>
        options.EnableLeafInlining &&
        options.LrContinuationCallTargets is not { Count: > 0 } &&
        options.NonReturningCallTargets is not { Count: > 0 } &&
        options.LinkedCallFallthroughLrOverrides is not { Count: > 0 } &&
        options.ModuleLinkBase is null &&
        options.ModuleGuestBase is null &&
        !options.EmitModRegistration;

    private FunctionTranslationResult FinishTranslation(
        uint entryPoint,
        string functionName,
        IReadOnlyList<PpcInstruction> instructions,
        IReadOnlyList<BasicBlock> basicBlocks,
        IrFunction irFunction,
        SsaResult ssa,
        TranslationOptions options,
        TranslationMetrics metrics,
        Stopwatch total)
    {
        if (options.DiscoveryOnly)
        {
            total.Stop();
            metrics.Total = total.Elapsed;
            return new FunctionTranslationResult(
                entryPoint,
                functionName,
                instructions,
                basicBlocks,
                irFunction,
                ssa,
                new RepresentationEnvironment(),
                new FunctionAbiClassification(functionName, ValueRepresentation.Void),
                string.Empty,
                metrics);
        }

        // Coarse host representation and return-ABI classification.
        var classificationSw = Stopwatch.StartNew();
        var types = new RepresentationClassifier().Classify(ssa.Function);
        classificationSw.Stop();
        metrics.RepresentationClassification = classificationSw.Elapsed;

        var signature = new FunctionAbiClassifier().Classify(functionName, ssa.Function, types);

        CxxEmissionResult? emission = null;
        if (options.GenerateCxx)
        {
            var codeGenSw = Stopwatch.StartNew();
            emission = EmitCxx(entryPoint, ssa, signature, types, options);
            codeGenSw.Stop();
            metrics.CodeGen = codeGenSw.Elapsed;
        }

        total.Stop();
        metrics.Total = total.Elapsed;

        return new FunctionTranslationResult(
            entryPoint,
            functionName,
            instructions,
            basicBlocks,
            irFunction,
            ssa,
            types,
            signature,
             emission?.Code ?? string.Empty,
             metrics,
             emission);
    }

    /// <summary>
    /// Runs only the front-end stages required for recursive target discovery
    /// and compact whole-program summaries.
    /// </summary>
    public FunctionDiscoveryResult Discover(uint entryPoint, TranslationOptions? options = null)
    {
        options ??= TranslationOptions.Default;
        var result = Translate(entryPoint, options with { GenerateCxx = false, DiscoveryOnly = true });
        return new FunctionDiscoveryResult(
            result.EntryPoint,
            result.Name,
            result.Instructions,
            result.Blocks,
            result.LinearIr,
            result.Ssa,
            result.Metrics);
    }

    /// <summary>
    /// Performs only the post-SSA lowering stages. Production discovery stores
    /// this canonical graph once, so final whole-program facts do not force a
    /// second disassembly/CFG/lifting/SSA pass.
    /// </summary>
    public FunctionTranslationResult LowerCanonical(
        uint entryPoint,
        IrFunction canonicalSsa,
        TranslationOptions options)
    {
        var metrics = new TranslationMetrics
        {
            IrInstructionCount = canonicalSsa.Blocks.Sum(static block => block.Instructions.Count),
            IrBlockCount = canonicalSsa.Blocks.Count
        };
        var total = Stopwatch.StartNew();
        var ssa = new SsaResult(canonicalSsa, IrCfg.Build(canonicalSsa));
        var classificationSw = Stopwatch.StartNew();
        var types = new RepresentationClassifier().Classify(canonicalSsa);
        var signature = new FunctionAbiClassifier().Classify(canonicalSsa.Name, canonicalSsa, types);
        classificationSw.Stop();
        metrics.RepresentationClassification = classificationSw.Elapsed;
        var codegen = Stopwatch.StartNew();
        var emission = options.GenerateCxx
            ? EmitCxx(entryPoint, ssa, signature, types, options)
            : null;
        codegen.Stop();
        metrics.CodeGen = codegen.Elapsed;
        total.Stop();
        metrics.Total = total.Elapsed;
        return new FunctionTranslationResult(
            entryPoint,
            canonicalSsa.Name,
            Array.Empty<PpcInstruction>(),
            Array.Empty<BasicBlock>(),
            canonicalSsa,
            ssa,
            types,
            signature,
             emission?.Code ?? string.Empty,
             metrics,
             emission);
    }

    private CxxEmissionResult EmitCxx(
        uint entryPoint,
        SsaResult ssa,
        FunctionAbiClassification signature,
        RepresentationEnvironment types,
        TranslationOptions options)
    {
        return new CxxLinearCodeGenerator(_guestAbiProvider).EmitWithFacts(
            entryPoint,
            ssa,
            signature,
            types,
            options.EmitModRegistration,
            options.ModRegistrationPriority,
            options.ModRegistrationModuleId,
            options.NonReturningCallTargets,
            options.LrContinuationCallTargets,
            options.GuestAbiContracts,
            options.EmitStateFreeLeafVariant,
            options.StateFreeAbiContracts,
            options.StateFreeCallSymbols,
            options.StateFreeCallSiteVariants,
            options.StateFreeEntryVariants,
            options.ModuleLinkBase,
            options.ModuleGuestBase,
            options.ModuleLinkedCodeSize,
            gqrEntryConstants: options.GqrEntryConstants,
            gqrCalleeWriteMasks: options.GqrCalleeWriteMasks,
            gqrConstantsRequireRuntimeGuard: options.GqrConstantsRequireRuntimeGuard,
            enableLeafAbiSpillElision: options.EnableLeafAbiSpillElision,
            modOverridableCallTargets: options.ModOverridableCallTargets);
    }

    private static IrTracePpc CreateTrace(PpcInstruction instruction)
    {
        var operandText = instruction.OperandText;
        var disasm = string.IsNullOrWhiteSpace(operandText)
            ? instruction.Mnemonic
            : $"{instruction.Mnemonic} {operandText}";
        // PowerPC instructions are exactly one big-endian word, which is the
        // form the old byte-array path always took.
        var rawHex = $"0x{instruction.Word:X8}";
        return new IrTracePpc(instruction.Address, disasm, rawHex);
    }

    /// <summary>Appends a guest instruction's lowered IR to <paramref name="destination"/>, trace placed
    /// before the first terminator (or last if none), written in place with no per-instruction copy.</summary>
    private static void AppendLoweredWithTrace(
        List<IrInstruction> destination,
        IReadOnlyList<IrInstruction> instructions,
        IrTracePpc trace)
    {
        var count = instructions.Count;
        if (count == 0)
        {
            destination.Add(trace);
            return;
        }

        var terminatorIndex = -1;
        for (var i = 0; i < count; i++)
        {
            if (instructions[i] is IrBranch or IrJump or IrReturn or IrIndirectJump or IrJumpTable or IrUndefined)
            {
                terminatorIndex = i;
                break;
            }
        }

        if (terminatorIndex < 0)
        {
            for (var i = 0; i < count; i++)
            {
                destination.Add(instructions[i]);
            }

            destination.Add(trace);
            return;
        }

        for (var i = 0; i < terminatorIndex; i++)
        {
            destination.Add(instructions[i]);
        }

        destination.Add(trace);
        for (var i = terminatorIndex; i < count; i++)
        {
            destination.Add(instructions[i]);
        }
    }

    private static string Base(string name) => RegisterNameUtils.StripNumericSuffix(name);

    private static string LabelFor(uint address) => $"0x{address:X8}";

    private static List<IrBasicBlock> ApplyLinkedCallFallthroughLrOverrides(
        IReadOnlyList<IrBasicBlock> blocks,
        IReadOnlyDictionary<uint, uint> overrides)
    {
        var rewritten = new List<IrBasicBlock>(blocks.Count);
        foreach (var block in blocks)
        {
            var instructions = block.Instructions.ToArray();
            var changed = false;
            for (var i = 0; i + 1 < instructions.Length; i++)
            {
                if (instructions[i] is IrAssign { Value.Kind: "const", Value.Constant: { } lrValue } assign &&
                    string.Equals(Base(assign.Destination), "lr", StringComparison.OrdinalIgnoreCase) &&
                    instructions[i + 1] is IrCall &&
                    overrides.TryGetValue(unchecked((uint)lrValue), out var overrideLr))
                {
                    instructions[i] = assign with { Value = IrValue.Imm(unchecked((int)overrideLr)) };
                    changed = true;
                }
            }

            rewritten.Add(changed ? new IrBasicBlock(block.Label, instructions) : block);
        }

        return rewritten;
    }

    private static bool TryBuildSyntheticBlock(string label, IReadOnlySet<string> localLabels, out IrBasicBlock block)
    {
        const string IndirectCtrPrefix = "indirect_ctr_";
        const string CallCtrPrefix = "call_ctr_";
        const string CallLrPrefix = "call_lr_";
        const string LinkBranchPrefix = "link_branch_";

        if (label.StartsWith(IndirectCtrPrefix, StringComparison.OrdinalIgnoreCase))
        {
            block = new IrBasicBlock(label, new IrInstruction[]
            {
                new IrIndirectJump(IrValue.Register("ctr"))
            });
            return true;
        }

        if (TryParseCallLabel(label, CallCtrPrefix, out _, out var fallthrough))
        {
            var args = BuildAbiCallArgs();
            block = new IrBasicBlock(label, new IrInstruction[]
            {
                new IrAssign("lr", IrValue.Imm((int)fallthrough)),
                new IrIndirectCall(string.Empty, IrValue.Register("ctr"), args),
                new IrJump($"0x{fallthrough:X8}")
            });
            return true;
        }

        if (TryParseCallLabel(label, CallLrPrefix, out var callAddr, out fallthrough))
        {
            var args = BuildAbiCallArgs();
            var temp = $"addr_bclrl_{callAddr:X8}_loc";
            block = new IrBasicBlock(label, new IrInstruction[]
            {
                new IrAssign(temp, IrValue.Register("lr")),
                new IrAssign("lr", IrValue.Imm((int)fallthrough)),
                new IrIndirectCall(string.Empty, IrValue.Register(temp), args),
                new IrJump($"0x{fallthrough:X8}")
            });
            return true;
        }

        if (TryParseLinkBranchLabel(label, LinkBranchPrefix, out fallthrough, out var target))
        {
            block = new IrBasicBlock(label, target == fallthrough || localLabels.Contains($"0x{target:X8}")
                ? new IrInstruction[]
                {
                    new IrAssign("lr", IrValue.Imm((int)fallthrough)),
                    new IrJump($"0x{target:X8}")
                }
                : new IrInstruction[]
                {
                    new IrAssign("lr", IrValue.Imm((int)fallthrough)),
                    new IrCall(string.Empty, $"0x{target:X8}", BuildAbiCallArgs()),
                    new IrJump($"0x{fallthrough:X8}")
                });
            return true;
        }

        if (TryParseHex(label, out var absoluteTarget))
        {
            block = new IrBasicBlock(label, new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{absoluteTarget:X8}", BuildAbiCallArgs()),
                new IrReturn(null)
            });
            return true;
        }

        block = null!;
        return false;
    }

    private static bool TryParseCallLabel(string label, string prefix, out uint addr, out uint fallthrough)
    {
        addr = 0;
        fallthrough = 0;

        if (!label.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var rest = label[prefix.Length..];
        var parts = rest.Split('_', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 2)
        {
            return false;
        }

        if (!TryParseHex(parts[0], out addr) || !TryParseHex(parts[1], out fallthrough))
        {
            return false;
        }

        return true;
    }

    private static bool TryParseLinkBranchLabel(string label, string prefix, out uint fallthrough, out uint target)
    {
        fallthrough = 0;
        target = 0;

        if (!label.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var rest = label[prefix.Length..];
        var parts = rest.Split('_', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 2)
        {
            return false;
        }

        if (!TryParseHex(parts[0], out fallthrough) || !TryParseHex(parts[1], out target))
        {
            return false;
        }

        return true;
    }

    private static bool TryParseHex(string text, out uint value) =>
        GuestTargetParser.TryParseHexAddress(text, out value);

    /// <summary>
    /// The EABI argument registers in the order the lifter passes them. Shares
    /// the lifter's single immutable instance instead of rebuilding an
    /// identical list per synthetic call block.
    /// </summary>
    private static IReadOnlyList<IrValue> BuildAbiCallArgs() => PpcLifter.AbiCallArguments;

}
