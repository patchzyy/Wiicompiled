using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;
using Translator.Core.Translation;

namespace Translator.Core.CodeGen;

/// <summary>
/// Conservative C++ emitter that writes guest registers directly through CpuContext.
/// Non-register temporaries still use ordinary local variables.
/// </summary>
public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// <c>MKW_RESTRICT</c> tells the host compiler guest memory via <c>Memory::</c> cannot alias ctx, so
    /// resident locals stay in host registers. Restrict isn't part of the type, so forward decls use plain <c>CpuContext* ctx</c>.
    /// </summary>
    internal const string CpuContextDefinitionParameter = "CpuContext* MKW_RESTRICT ctx";

    internal static string FunctionDefinitionSignature(string name) =>
        $"extern \"C\" void {name}({CpuContextDefinitionParameter})";

    private readonly IGuestFunctionAbiProvider _guestAbiProvider;

    /// <summary>
    /// Reused scratch buffer for <c>EmitFunctionBody</c>, which emits a body up to three times
    /// (discovery, public entry, state-free fallback); checked out while in use so nested emission allocates its own.
    /// </summary>
    [ThreadStatic]
    private static StringBuilder? _reusableFunctionBodyBuilder;

    private const int FunctionBodyBuilderCapacity = 16 * 1024;

    public CxxLinearCodeGenerator(IGuestFunctionAbiProvider? guestAbiProvider = null)
    {
        _guestAbiProvider = guestAbiProvider ?? EmptyGuestFunctionAbiProvider.Instance;
    }

    /// <summary>
    /// Direct-call boundaries materialized by the body currently being emitted. The emitter is
    /// the authority here, replacing scanning emitted text for <c>InvokeDirectCpu</c>; recording
    /// is idempotent since a body can emit more than once (discovery, public entry, state-free fallback).
    /// </summary>
    [ThreadStatic]
    private static SortedSet<uint>? _emittedDirectCallTargets;

    private static void RecordEmittedDirectCallTarget(uint target) =>
        _emittedDirectCallTargets?.Add(target);

    /// <summary>
    /// Guest addresses appearing as an emitted <c>loc_XXXXXXXX</c> label (definition or goto
    /// target) in the body currently being emitted; replaces scanning emitted text to find a
    /// translated function's interior resume points.
    /// </summary>
    [ThreadStatic]
    private static SortedSet<uint>? _emittedLocalLabelAddresses;

    private static void RecordEmittedLocalLabel(string cxxLabel)
    {
        if (_emittedLocalLabelAddresses is null) return;
        const string prefix = "loc_";
        if (!cxxLabel.StartsWith(prefix, StringComparison.Ordinal)) return;
        if (uint.TryParse(
                cxxLabel.AsSpan(prefix.Length),
                System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture,
                out var address))
        {
            _emittedLocalLabelAddresses.Add(address);
        }
    }


    public CxxEmissionResult EmitWithFacts(uint entryPoint,
        SsaResult ssa,
        FunctionAbiClassification signature,
        RepresentationEnvironment types,
        bool emitModRegistration = false,
        uint modRegistrationPriority = 100,
        ulong modRegistrationModuleId = 1,
        IReadOnlySet<uint>? nonReturningCallTargets = null,
        IReadOnlySet<uint>? lrContinuationCallTargets = null,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        bool emitStateFreeLeafVariant = false,
        IReadOnlyDictionary<uint, GuestAbiContract>? stateFreeAbiContracts = null,
        IReadOnlyDictionary<uint, string>? stateFreeCallSymbols = null,
        IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>? stateFreeCallSiteVariants = null,
        IReadOnlyList<GuestStateFreeCallVariant>? stateFreeEntryVariants = null,
        uint? moduleLinkBase = null,
        uint? moduleGuestBase = null,
        uint moduleLinkedCodeSize = 0,
        IReadOnlyDictionary<string, uint>? gqrEntryConstants = null,
        IReadOnlyDictionary<uint, byte>? gqrCalleeWriteMasks = null,
        bool gqrConstantsRequireRuntimeGuard = false,
        bool enableLeafAbiSpillElision = false,
        IReadOnlySet<uint>? modOverridableCallTargets = null,
        bool enableGpuFifoBurstCoalescing = true)
    {
        modOverridableCallTargets ??= new HashSet<uint>();
        nonReturningCallTargets ??= new HashSet<uint>();
        lrContinuationCallTargets ??= new HashSet<uint>();
        guestAbiContracts ??= new Dictionary<uint, GuestAbiContract>();
        stateFreeAbiContracts ??= new Dictionary<uint, GuestAbiContract>();
        stateFreeCallSymbols ??= new Dictionary<uint, string>();
        stateFreeCallSiteVariants ??= new Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>();
        stateFreeEntryVariants ??= Array.Empty<GuestStateFreeCallVariant>();
        // Register-class selection is not a proof that guest ABI stack traffic
        // can be removed. Keep that semantic transform independently gated.
        var elideLeafAbiSpills = enableLeafAbiSpillElision;
        // GPR spill elision is not enabled in the C++ backend. Public indirect
        // entry can flow into resident/state-free bodies, so locally owning a
        // cached register is insufficient to prove that the complete public
        // call boundary preserves the PPC nonvolatile value.
        var leafGprStackFunction = ssa.Function;
        var leafStackFunction = elideLeafAbiSpills
            ? ElideLeafStackFprSaveRestore(leafGprStackFunction, types)
            : leafGprStackFunction;
        var func = ElideDeadPureLocalTemps(
            ElideFctiwStackLowWordLoads(
                ElideOverwrittenPsqStackLoads(leafStackFunction),
                types));
        var isGuestCallFree = FunctionEligibleForLeafRegisterCache(func);
        var hasProvenKernelEntry = isGuestCallFree && gqrEntryConstants is { Count: > 0 };
        func = GqrConstantPropagation.Specialize(
            func, gqrEntryConstants, gqrCalleeWriteMasks,
            gqrConstantsRequireRuntimeGuard);
        // Call-free proven kernels amortize very small ranges. Ordinary functions
        // use the epoch-safe scalar form and a higher threshold to avoid adding
        // pointer locals for incidental field pairs.
        func = GuestMemoryRangeLowering.Lower(
            func, minimumAccesses: hasProvenKernelEntry ? 2 : 8, ignoreDisabledPcTraces: true,
            scalarOnly: !hasProvenKernelEntry);
        var architecturalFunc = func;
        var guestAbiContract = GuestAbiContractAnalyzer.Analyze(architecturalFunc, guestAbiContracts);
        var cfg = IrCfg.Build(func);
        // Condition-flag elision is planned once against the final IR so every
        // emission of this body (recording pass, public entry, state-free
        // fallback) suppresses exactly the same instruction positions.
        var flagElision = PlanFlagElision(
            func, cfg, types, guestAbiContracts, modOverridableCallTargets,
            nonReturningCallTargets, lrContinuationCallTargets);
        // Flattened once per function: the emitter used to hash a (block, index)
        // tuple for every instruction of every emission of this body.
        var suppressedInstructionMasks = BuildSuppressedInstructionMasks(func, flagElision);
        var cxxName = SanitizeIdentifier(signature.Name);
        var pairedFlow = ComputePairedFlowStates(func, cfg, _guestAbiProvider);
        var pairedIn = pairedFlow.In;
        var pairedOut = pairedFlow.Out;
        var pairedEdgeLiveness = ComputeRegisterLiveness(
            func, cfg,
            new LeafRegisterCache(
                new SortedSet<int>(), new SortedSet<int>(Enumerable.Range(0, 32)),
                new SortedSet<int>(), new SortedSet<int>()),
            _guestAbiProvider);
        var leafPreservedFprMask = LeafStackPreservedFprMask(architecturalFunc);
        var nonvolatileFprWriteMask = FunctionNonvolatileFprWriteMask(architecturalFunc) & ~leafPreservedFprMask;
        var preservesNonvolatileFprs = nonvolatileFprWriteMask == 0;
        var stackFacts = StackAddressFacts.Build(func);
        // Register residency supersedes the leaf register cache (localizes state via the
        // expression layer instead of rewriting text, and covers non-leaf bodies too);
        // residency owns every primary body, so the text-rewriting cache survives only for the state-free clone below.
        var residencyOwnsPrimaryBody = !emitStateFreeLeafVariant;
        var cachedRegisters = emitStateFreeLeafVariant
            ? CollectLeafRegisterCacheRegisters(func, _guestAbiProvider, guestAbiContracts)
            : LeafRegisterCache.Empty;
        var registerCacheInitializers = !cachedRegisters.IsEmpty
            ? BuildLeafRegisterCacheInitializers(
                func, cfg, cachedRegisters, _guestAbiProvider,
                emitStateFreeLeafVariant ? guestAbiContracts : null)
            : LeafRegisterCacheInitializers.Empty;
        var sb = new StringBuilder();
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine("#include \"ppc_runtime.h\"");
        sb.AppendLine("#include \"abi_bridge.h\"");
        sb.AppendLine("#include \"memory.h\"");
        sb.AppendLine("#include \"recomp_mod_loader.h\"");
        sb.AppendLine();

        var forwardDecls = CollectCallTargets(func)
            .Select(FormatSymbol)
            .Where(t => t.StartsWith("func_", StringComparison.OrdinalIgnoreCase) && !string.Equals(t, cxxName, StringComparison.Ordinal) && !TryParseAddress(t, out _))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (var target in forwardDecls.OrderBy(t => t, StringComparer.OrdinalIgnoreCase))
        {
            sb.AppendLine($"extern \"C\" void {target}(CpuContext* ctx);");
        }
        if (forwardDecls.Count > 0)
        {
            sb.AppendLine();
        }

        var implementationName = cxxName;
        foreach (var target in guestAbiContract.DirectCallTargets.Where(stateFreeCallSymbols.ContainsKey))
        {
            if (!stateFreeAbiContracts.TryGetValue(target, out var stateFreeContract))
                continue;
            AppendStateFreeResultDeclaration(sb, target, stateFreeContract);
            sb.AppendLine($"extern \"C\" {StateFreeReturnType(target, stateFreeContract)} {StateFreeCallingConvention(stateFreeContract)}{stateFreeCallSymbols[target]}({StateFreeParameterList(stateFreeContract, includeNames: false)});");
        }
        foreach (var variant in stateFreeCallSiteVariants.Values
                     .DistinctBy(static variant => variant.Symbol, StringComparer.Ordinal)
                     .OrderBy(static variant => variant.Symbol, StringComparer.Ordinal))
        {
            sb.AppendLine($"extern \"C\" {StateFreeReturnType(variant.Target, variant.Contract)} {StateFreeCallingConvention(variant.Contract)}{variant.Symbol}({StateFreeParameterList(variant.Contract, includeNames: false)});");
        }
        if (guestAbiContract.DirectCallTargets.Any(stateFreeCallSymbols.ContainsKey)) sb.AppendLine();
        // Collect and declare only non-register local variables (temporaries)
        var locals = CollectNonRegisterLocals(func);
        var orderedLocals = locals.OrderBy(x => x, StringComparer.Ordinal).ToArray();
        var guestRangeResolves = func.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>()
            .GroupBy(static resolve => resolve.Destination, StringComparer.Ordinal)
            .Select(static group => group.First())
            .OrderBy(static resolve => resolve.Destination, StringComparer.Ordinal)
            .ToArray();
        var gqrEntryGuards = CollectGqrEntryGuards(func);

        var labelNames = func.Blocks.ToDictionary(b => b.Label, b => SanitizeLabel(b.Label), StringComparer.OrdinalIgnoreCase);
        var instructionContinuationLabels = new Dictionary<uint, string>();
        var needsInstructionContinuationLabels = func.Blocks
            .SelectMany(static block => block.Instructions)
            .OfType<IrCall>()
            .Any(call =>
                TryParseAddress(call.Target, out var target) &&
                (nonReturningCallTargets.Contains(target) || lrContinuationCallTargets.Contains(target)));
        if (needsInstructionContinuationLabels)
        {
            foreach (var trace in func.Blocks.SelectMany(static block => block.Instructions).OfType<IrTracePpc>())
            {
                var irLabel = $"0x{trace.Address:X8}";
                if (!labelNames.ContainsKey(irLabel))
                {
                    var cxxLabel = SanitizeLabel(irLabel);
                    labelNames.Add(irLabel, cxxLabel);
                    instructionContinuationLabels.TryAdd(trace.Address, cxxLabel);
                }
            }
        }
        var nextBlockLabels = BuildNextBlockLabelMap(func);
        var knownConstants = BuildKnownUIntConstants(func);
        // Burst coalescing is planned once against the final IR so every
        // emission of this body sees the same runs and the same buffer names.
        var gpuFifoBurstPlan = BuildGpuFifoBurstPlan(func, knownConstants, enableGpuFifoBurstCoalescing);
        // The state-free clone is derived by rewriting this body's text, and it
        // rewrites the ctx-carrying PSQ helper spellings into the explicit-state
        // ones. Hoisting changes those spellings, so leave it off for the bodies
        // that clone is made from.
        var hoistedGqrIndices = CollectHoistableGqrIndices(
            func, stackFacts, !emitStateFreeLeafVariant);
        var linkedAddressRemap = moduleLinkBase.HasValue && moduleGuestBase.HasValue && moduleLinkedCodeSize != 0
            ? new LinkedAddressRemap(moduleLinkBase.Value, moduleGuestBase.Value, moduleLinkedCodeSize)
            : null;
        // The complete function body is produced by this local function so it
        // can be emitted more than once with a different residency mode. The
        // inputs above are all deterministic and immutable from here on, so two
        // invocations with the same residency produce identical text.
        var emittedDirectCallTargets = new SortedSet<uint>();
        var emittedLocalLabelAddresses = new SortedSet<uint>();
        string EmitFunctionBody(RegisterResidency? residency, int bufferBaseLength, bool discardResult = false)
        {
            var previousResidency = _activeResidency;
            var previousDirectCallTargets = _emittedDirectCallTargets;
            var previousLocalLabels = _emittedLocalLabelAddresses;
            var previousFifoBurstSlot = _activeGpuFifoBurstSlot;
            var previousHoistedGqrIndices = _hoistedGqrIndices;
            _activeResidency = residency;
            _emittedDirectCallTargets = emittedDirectCallTargets;
            _emittedLocalLabelAddresses = emittedLocalLabelAddresses;
            _activeGpuFifoBurstSlot = null;
            _hoistedGqrIndices = hoistedGqrIndices;
            var body = _reusableFunctionBodyBuilder ?? new StringBuilder(FunctionBodyBuilderCapacity);
            _reusableFunctionBodyBuilder = null;
            try
            {
                body.AppendLine("{");
                foreach (var local in orderedLocals)
                {
                    var representation = types.Get(local);
                    var type = RepresentationFormatter.ToCxx(representation);
                    var initValue = RepresentationFormatter.DefaultValue(representation);
                    var valueSuffix = string.IsNullOrWhiteSpace(initValue) ? string.Empty : $" = {initValue}";
                    // ABI preservation locals stay in the native stack frame on purpose: a
                    // callee-saved host register would raise pressure in exactly the call-heavy
                    // paths this targets, and a volatile slot still beats guest address translation + byte swapping.
                    var qualifier = local.StartsWith("abi_saved_lr_", StringComparison.Ordinal)
                        ? "volatile "
                        : string.Empty;
                    body.AppendLine($"    {qualifier}{type} {local}{valueSuffix};");
                }
                foreach (var resolve in guestRangeResolves)
                {
                    body.AppendLine($"    uint8_t* {resolve.Destination} = nullptr;");
                }
                foreach (var guard in gqrEntryGuards)
                {
                    body.AppendLine($"    const bool {GqrEntryGuardName(guard.Index, guard.Value)} = ctx->gqr[{guard.Index}u] == 0x{guard.Value:X8}u;");
                }
                foreach (var run in gpuFifoBurstPlan.Runs)
                {
                    body.AppendLine($"    uint8_t {run.BufferName}[{run.TotalBytes}];");
                }
                if (orderedLocals.Length > 0 || guestRangeResolves.Length > 0 ||
                    gqrEntryGuards.Count > 0 || gpuFifoBurstPlan.Runs.Count > 0)
                {
                    body.AppendLine();
                }
                if (residency is not null)
                {
                    var beforeEntryLoads = body.Length;
                    residency.AppendEntryLoads(body);
                    if (body.Length != beforeEntryLoads)
                    {
                        body.AppendLine();
                    }
                }
                EmitHoistedGqrPrologue(body, hoistedGqrIndices);

                body.AppendLine($"    goto {labelNames[func.EntryLabel]};");
                body.AppendLine();

                foreach (var block in func.Blocks)
                {
                    var localConstants = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
                    var label = labelNames[block.Label];
                    RecordEmittedLocalLabel(label);
                    body.AppendLine($"{label}:");
                    body.AppendLine("{");
                    var localPaired = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
                    if (pairedIn.TryGetValue(block.Label, out var pairedAtEntry))
                    {
                        foreach (var reg in pairedAtEntry)
                        {
                            localPaired[reg] = true;
                        }
                    }

                    var term = block.Instructions.LastOrDefault();
                    var nonTerminatorCount = term is IrBranch or IrJump or IrReturn or IrJumpTable or IrUndefined
                        ? Math.Max(0, block.Instructions.Count - 1)
                        : block.Instructions.Count;
                    // Per-target call counter for this block. It replaces an
                    // O(n^2) rescan of the prefix and must therefore advance for
                    // every call position the rescan used to see, including the
                    // ones the flag-elision plan suppresses below.
                    var directCallOrdinals = new Dictionary<string, int>(StringComparer.Ordinal);
                    var blockSuppressed = suppressedInstructionMasks.TryGetValue(block.Label, out var suppressedMask)
                        ? suppressedMask
                        : null;

                    for (var i = 0; i < nonTerminatorCount; i++)
                    {
                        var directCallOrdinal = -1;
                        if (block.Instructions[i] is IrCall ordinalCall)
                        {
                            directCallOrdinals.TryGetValue(ordinalCall.Target, out var priorCalls);
                            directCallOrdinals[ordinalCall.Target] = priorCalls + 1;
                            directCallOrdinal = priorCalls;
                        }

                        if (blockSuppressed is not null && i < blockSuppressed.Length && blockSuppressed[i])
                        {
                            // Provably dead condition-flag or link-register
                            // traffic. The constant tracker still observes it so
                            // address folding is unchanged.
                            UpdateKnownUIntConstants(block.Instructions[i], localConstants);
                            continue;
                        }

                        if (block.Instructions[i] is IrTracePpc trace &&
                            instructionContinuationLabels.TryGetValue(trace.Address, out var continuationLabel))
                        {
                            RecordEmittedLocalLabel(continuationLabel);
                            body.AppendLine($"{continuationLabel}:");
                        }
                        var localFallthroughLr = TryGetLocalFallthroughLr(block.Instructions, i, nonReturningCallTargets, lrContinuationCallTargets);
                        // State-free bodies are cloned after register caching and lose their
                        // direct-call fallbacks, so this initial emission (which must stay a
                        // genuine CpuContext fallback) only requests cache-boundary sync for the legacy resident ABI.
                        var inlineGuestThunkStackBase = block.Instructions[i] is IrCall &&
                            HasLocallyProvenStackFrameBase(block.Instructions, i, stackFacts);
                        _activeGpuFifoBurstSlot = gpuFifoBurstPlan.Slot(block.Label, i);
                        try
                        {
                            EmitInstruction(block.Label, directCallOrdinal, block.Instructions[i], body, bufferBaseLength, 1, cfg, labelNames, types, signature, localPaired, _guestAbiProvider, knownConstants, localConstants, linkedAddressRemap, nonReturningCallTargets, lrContinuationCallTargets, stackFacts, inlineGuestThunkStackBase, localFallthroughLr, guestAbiContracts, stateFreeAbiContracts, stateFreeCallSymbols, stateFreeCallSiteVariants, modOverridableCallTargets);
                        }
                        finally
                        {
                            _activeGpuFifoBurstSlot = null;
                        }
                        UpdateKnownUIntConstants(block.Instructions[i], localConstants);
                        if (gpuFifoBurstPlan.Completion(block.Label, i) is { } completedBurst)
                        {
                            body.AppendLine(
                                $"    GX_HLE_FIFO_WriteBurst({completedBurst.BufferName}, {completedBurst.TotalBytes}u);");
                        }
                        EmitHoistedGqrReloads(body, "    ", block.Instructions[i], hoistedGqrIndices, gqrCalleeWriteMasks);
                    }

                    switch (term)
                    {
                        case IrUndefined undef:
                            EmitInstruction(block.Label, -1, undef, body, bufferBaseLength, 1, cfg, labelNames, types, signature, localPaired, _guestAbiProvider, knownConstants, localConstants, linkedAddressRemap, nonReturningCallTargets, lrContinuationCallTargets, stackFacts, inlineGuestThunkStackBase: false, localFallthroughLr: null, guestAbiContracts, stateFreeAbiContracts, stateFreeCallSymbols, stateFreeCallSiteVariants, modOverridableCallTargets);
                            AppendFlush(body, "    ");
                            body.AppendLine("    return;");
                            break;

                        case IrReturn ret:
                            AppendFlush(body, "    ");
                            body.AppendLine("    return;");
                            break;

                        case IrJump jump:
                            EmitNormalizePairedStateOnEdge(body, "    ", block.Label, jump.TargetLabel, pairedOut, pairedIn, pairedEdgeLiveness.In);
                            EmitGotoUnlessFallthrough(body, "    ", block.Label, jump.TargetLabel, nextBlockLabels, labelNames, signature.Name);
                            break;

                        case IrBranch br:
                            var trueLabel = RequireLabel(br.TrueLabel, labelNames, block.Label, signature.Name);
                            var falseLabel = RequireLabel(br.FalseLabel, labelNames, block.Label, signature.Name);
                            var cond = flagElision.FusedBranches.TryGetValue(block.Label, out var fusedCompare)
                                ? FusedConditionExpression(fusedCompare, types, localPaired)
                                : ToCondition(br.Condition, br.ConditionRegister, types);
                            // The false edge is often pure fallthrough with no
                            // representation normalization, which used to emit an
                            // empty "} else {}". Build it first and keep the else
                            // only when it actually carries something.
                            var falseEdge = new StringBuilder();
                            EmitNormalizePairedStateOnEdge(falseEdge, "        ", block.Label, br.FalseLabel, pairedOut, pairedIn, pairedEdgeLiveness.In);
                            EmitGotoUnlessFallthrough(falseEdge, "        ", block.Label, br.FalseLabel, nextBlockLabels, labelNames, signature.Name);
                            body.AppendLine($"    if ({cond}) {{");
                            EmitNormalizePairedStateOnEdge(body, "        ", block.Label, br.TrueLabel, pairedOut, pairedIn, pairedEdgeLiveness.In);
                            EmitGotoUnlessFallthrough(body, "        ", block.Label, br.TrueLabel, nextBlockLabels, labelNames, signature.Name);
                            if (falseEdge.Length != 0)
                            {
                                body.AppendLine("    } else {");
                                body.Append(falseEdge);
                            }
                            body.AppendLine("    }");
                            break;

                        case IrJumpTable table:
                            var selectorExpr = ToExpression(IrValue.Register(table.Selector), types);
                            body.AppendLine($"    switch (static_cast<uint32_t>({selectorExpr})) {{");
                            foreach (var jt in table.Cases)
                            {
                                var caseLabel = RequireLabel(jt.TargetLabel, labelNames, block.Label, signature.Name);
                                body.AppendLine($"    case 0x{jt.TargetAddress:X8}u:");
                                EmitNormalizePairedStateOnEdge(body, "        ", block.Label, jt.TargetLabel, pairedOut, pairedIn, pairedEdgeLiveness.In);
                                body.AppendLine($"        goto {caseLabel};");
                                body.AppendLine("        break;");
                            }
                            body.AppendLine("    default:");
                            // Unlisted selectors leave this frame through the
                            // dispatcher: publish the locals and never write
                            // them back afterwards.
                            AppendFlush(body, "        ");
                            body.AppendLine($"        InvokeIndirectJump({selectorExpr}, ctx);");
                            body.AppendLine("        return;");
                            body.AppendLine("    }");
                            break;

                        default:
                            // Fallthrough to next block if present.
                            var succ = cfg.Successors(block.Label).FirstOrDefault();
                            if (!string.IsNullOrWhiteSpace(succ))
                            {
                                EmitNormalizePairedStateOnEdge(body, "    ", block.Label, succ!, pairedOut, pairedIn, pairedEdgeLiveness.In);
                                EmitGotoUnlessFallthrough(body, "    ", block.Label, succ!, nextBlockLabels, labelNames, signature.Name);
                            }
                            else
                            {
                                // An indirect jump already published the locals
                                // and returned; the trailing return below is
                                // unreachable and must not write them back.
                                if (term is not IrIndirectJump)
                                {
                                    AppendFlush(body, "    ");
                                }
                                body.AppendLine("    return;");
                            }
                            break;
                    }

                    body.AppendLine("}");
                    body.AppendLine();
                }

                body.Append('}');
                // The residency discovery pass only exists for its side effects
                // on the recorder; materializing its text costs a full copy of
                // the largest string this translator builds.
                return discardResult ? string.Empty : body.ToString();
            }
            finally
            {
                body.Clear();
                _reusableFunctionBodyBuilder = body;
                _activeResidency = previousResidency;
                _emittedDirectCallTargets = previousDirectCallTargets;
                _emittedLocalLabelAddresses = previousLocalLabels;
                _activeGpuFifoBurstSlot = previousFifoBurstSlot;
                _hoistedGqrIndices = previousHoistedGqrIndices;
            }
        }

        // Discovery pass: emit the body once with a recording residency to observe every register the emitter
        // really formats (inline EABI thunks read r11 with no IR operand; raw branch conditions reference CR/CTR
        // as plain text), then discard the result. The buffer offset below matters because call-site temporaries
        // uniquify themselves by position in the translation unit.
        var bodyBufferBaseLength = sb.Length +
            FunctionDefinitionSignature(implementationName).Length +
            Environment.NewLine.Length;
        RegisterResidency? residency = null;
        {
            var recorder = RegisterResidency.CreateRecorder();
            EmitFunctionBody(recorder, bodyBufferBaseLength, discardResult: true);
            var discovered = recorder.Snapshot();
            if (!discovered.IsEmpty)
            {
                RequireResidencyNamesAreFree(
                    discovered,
                    entryPoint,
                    orderedLocals
                        .Concat(guestRangeResolves.Select(static resolve => resolve.Destination))
                        .Concat(gqrEntryGuards.Select(static guard => GqrEntryGuardName(guard.Index, guard.Value))));
                residency = discovered;
            }
        }

        sb.AppendLine(FunctionDefinitionSignature(implementationName));
        sb.AppendLine(EmitFunctionBody(residencyOwnsPrimaryBody ? residency : null, bodyBufferBaseLength));

        var aliasName = $"func_{entryPoint:X8}";
        if (!emitModRegistration && !cxxName.Equals(aliasName, StringComparison.OrdinalIgnoreCase))
        {
            sb.AppendLine();
            sb.AppendLine(FunctionDefinitionSignature(aliasName));
            sb.AppendLine("{");
            sb.AppendLine($"    {cxxName}(ctx);");
            sb.AppendLine("}");
        }

        sb.AppendLine();
        var guestAbiMarker = GeneratedMarkers.GuestAbi(guestAbiContract);
        sb.AppendLine($"// {guestAbiMarker}");
        // Registration is translator-internal, not code: real registration happens via the shard emitter's bulk
        // registration TUs. This survives only as a marker comment (matching RECOMP_GUEST_ABI's shape) that the
        // shard emitter and dispatch profile parse out, then strip when composing a compiled shard.
        CxxBuildRegistrationFacts? registration = null;
        if (emitModRegistration)
        {
            var prettyName = EscapeCxxStringLiteral(signature.Name);
            sb.AppendLine(GeneratedMarkers.ModRegistration(
                entryPoint,
                cxxName,
                prettyName,
                preservesNonvolatileFprs,
                nonvolatileFprWriteMask,
                modRegistrationPriority,
                modRegistrationModuleId));
        }
        else
        {
            sb.AppendLine(GeneratedMarkers.BaseRegistration(
                entryPoint, cxxName, preservesNonvolatileFprs, nonvolatileFprWriteMask));
            registration = new CxxBuildRegistrationFacts(
                cxxName, preservesNonvolatileFprs, nonvolatileFprWriteMask);
        }

        var code = sb.ToString();
        GuestStateFreeEmissionFacts? stateFreeFacts = null;
        var stateFreeVariantFacts = new List<GuestStateFreeEmissionFacts>();
        // The state-free clone is derived by rewriting this register-cached body's text, so the ordinary
        // CpuContext body must be captured here, before caching is applied, since it's also the mandatory
        // fallback for mod overrides and unavailable descendants. It's re-emitted with residency and swapped
        // in after the clone is produced.
        var ordinaryStateFreeFallbackBody = emitStateFreeLeafVariant
            ? (residency is not null
                ? EmitFunctionBody(residency, bodyBufferBaseLength)
                : ExtractFunctionBody(code, implementationName))
            : null;
        if (!cachedRegisters.IsEmpty)
        {
            code = ApplyLeafRegisterCache(
                code,
                implementationName,
                cachedRegisters,
                registerCacheInitializers);
        }
        if (emitStateFreeLeafVariant)
        {
            if (!stateFreeCallSymbols.TryGetValue(entryPoint, out var stateFreeSymbol))
                throw new InvalidOperationException($"Missing state-free symbol for 0x{entryPoint:X8}.");
            static uint StateFreeRegisterMask(IEnumerable<int> values) =>
                values.Aggregate(0u, static (mask, value) => mask | (1u << value));
            var exactContract = guestAbiContract with
            {
                GprReadBeforeWriteMask = StateFreeRegisterMask(registerCacheInitializers.Gprs),
                GprPossibleWriteMask = StateFreeRegisterMask(cachedRegisters.WrittenGprs),
                // Full FPR caching can include values read only to preserve the
                // public nonvolatile ABI. They are not semantic inputs to a
                // state-free call and must not inflate its native signature.
                FprReadBeforeWriteMask = StateFreeRegisterMask(registerCacheInitializers.Fprs) &
                                         guestAbiContract.FprReadBeforeWriteMask,
                // Native paired lowering can replace cached_fN completely with
                FprPossibleWriteMask = guestAbiContract.FprPossibleWriteMask,
                // An unlinked direct branch is a tail call.  Its callee returns
                // through this function's incoming LR even when LR never appears
                // as an ordinary register initializer in the caller body.
                ReadsLrBeforeWrite = guestAbiContract.ReadsLrBeforeWrite ||
                    func.Blocks.SelectMany(static block => block.Instructions)
                        .OfType<IrCall>()
                        .Any(static call => string.IsNullOrWhiteSpace(call.Destination))
            };
            if (stateFreeAbiContracts.TryGetValue(entryPoint, out var plannedContract))
            {
                exactContract = exactContract with
                {
                    GprPossibleWriteMask = exactContract.GprPossibleWriteMask & plannedContract.GprPossibleWriteMask,
                    FprPossibleWriteMask = exactContract.FprPossibleWriteMask & plannedContract.FprPossibleWriteMask,
                    CrPossibleWriteMask = (byte)(exactContract.CrPossibleWriteMask & plannedContract.CrPossibleWriteMask),
                    MayWriteXer = exactContract.MayWriteXer && plannedContract.MayWriteXer,
                    MayWriteCtr = exactContract.MayWriteCtr && plannedContract.MayWriteCtr,
                    MayWriteLr = exactContract.MayWriteLr && plannedContract.MayWriteLr,
                    MayWriteFpscr = exactContract.MayWriteFpscr && plannedContract.MayWriteFpscr,
                    GqrPossibleWriteMask = (byte)(exactContract.GqrPossibleWriteMask & plannedContract.GqrPossibleWriteMask),
                    HidPossibleWriteMask = (byte)(exactContract.HidPossibleWriteMask & plannedContract.HidPossibleWriteMask)
                };
            }
            // Clone before GQR template versioning. The public CpuContext entry
            // may retain its guarded template wrapper, while this clone owns
            // the original cached register values and receives GQRs explicitly.
            code = AppendStateFreeLeafVariant(
                code, implementationName, stateFreeSymbol, entryPoint, exactContract,
                out var primaryStateFreeFacts,
                boundaryContextTargets: new HashSet<uint>(),
                guestAbiContracts: guestAbiContracts);
            stateFreeFacts = primaryStateFreeFacts;
            foreach (var variant in stateFreeEntryVariants)
            {
                code = AppendStateFreeLeafVariant(
                    code, implementationName, variant.Symbol, entryPoint, variant.Contract,
                    out var variantFacts, preferInline: true);
                stateFreeVariantFacts.Add(variantFacts);
            }
            code = ReplaceFunctionBody(
                code,
                implementationName,
                ordinaryStateFreeFallbackBody ?? throw new InvalidOperationException(
                    $"Missing ordinary state-free fallback body for 0x{entryPoint:X8}."));
        }
        if (CanVersionGuardedGqrFunction(func, gqrEntryGuards))
        {
            code = ApplyGuardedGqrFunctionVersioning(code, implementationName, gqrEntryGuards);
        }

        return new CxxEmissionResult(
            code,
            guestAbiContract,
            guestAbiMarker,
            registration,
            emittedDirectCallTargets.ToArray(),
            emittedLocalLabelAddresses.ToArray(),
            stateFreeFacts,
            stateFreeVariantFacts);
    }

    private static string EscapeCxxStringLiteral(string value) => CxxStringLiteralEscaping.Escape(value);

    private static uint? TryGetLocalFallthroughLr(
        IReadOnlyList<IrInstruction> instructions,
        int index,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets)
    {
        if (index <= 0 ||
            instructions[index] is not IrCall ||
            instructions[index - 1] is not IrAssign { Value.Kind: "const", Value.Constant: { } lrValue } lrAssign ||
            !string.Equals(GetRegisterBaseName(lrAssign.Destination), "lr", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        return unchecked((uint)lrValue);
    }

    // Shared with GuestMemoryRangeLowering: the range hoister has to decide
    // whether a store is one the emitter will turn into a direct
    // GX_HLE_FIFO_Write*, and the only way to guarantee that decision agrees is
    // to consult the exact same constant map the emitter consults.
    internal static Dictionary<string, uint> BuildKnownUIntConstants(IrFunction func)
    {
        var constants = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);

        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrAssign assign when TryGetUIntConstant(assign.Value, constants, out var value):
                        constants[assign.Destination] = value;
                        break;

                    case IrBinary binary when TryEvaluateUIntBinary(binary, constants, out var value):
                        constants[binary.Destination] = value;
                        break;
                }
            }
        }

        return constants;
    }

    private static void UpdateKnownUIntConstants(IrInstruction instruction, Dictionary<string, uint> constants)
    {
        string? destination = instruction switch
        {
            IrAssign assign => assign.Destination,
            IrBinary binary => binary.Destination,
            IrLoad load => load.Destination,
            IrCall call => call.Destination,
            IrIndirectCall call => call.Destination,
            _ => null
        };

        if (destination is null)
        {
            return;
        }

        if (instruction is IrAssign constantAssign && TryGetUIntConstant(constantAssign.Value, constants, out var assigned))
        {
            constants[destination] = assigned;
            constants[GetRegisterBaseName(destination)] = assigned;
        }
        else if (instruction is IrBinary constantBinary && TryEvaluateUIntBinary(constantBinary, constants, out var calculated))
        {
            constants[destination] = calculated;
            constants[GetRegisterBaseName(destination)] = calculated;
        }
        else
        {
            constants.Remove(destination);
            constants.Remove(GetRegisterBaseName(destination));
        }
    }

    private sealed record LinkedAddressRemap(uint LinkBase, uint GuestBase, uint CodeSize);

    private static bool TryEvaluateUIntBinary(
        IrBinary binary,
        IReadOnlyDictionary<string, uint> constants,
        out uint value)
    {
        value = 0;
        if (!TryGetUIntConstant(binary.Left, constants, out var left) ||
            !TryGetUIntConstant(binary.Right, constants, out var right))
        {
            return false;
        }

        value = binary.Op switch
        {
            "add" => unchecked(left + right),
            "sub" => unchecked(left - right),
            "and" => left & right,
            "or" => left | right,
            "xor" => left ^ right,
            "shl" => unchecked(left << (int)(right & 31)),
            "rotl" => unchecked((left << (int)(right & 31)) | (left >> (int)((32 - right) & 31))),
            "shr" => left >> (int)(right & 31),
            _ => 0
        };

        return binary.Op is "add" or "sub" or "and" or "or" or "xor" or "shl" or "shr" or "rotl";
    }

    private static bool TryGetUIntConstant(
        IrValue value,
        IReadOnlyDictionary<string, uint> constants,
        out uint result)
    {
        if (value.Kind == "const" && value.Constant.HasValue)
        {
            result = unchecked((uint)value.Constant.Value);
            return true;
        }

        if (value.Kind == "register" &&
            value.RegisterName != null &&
            constants.TryGetValue(value.RegisterName, out result))
        {
            return true;
        }

        result = 0;
        return false;
    }

    private static Dictionary<string, string?> BuildNextBlockLabelMap(IrFunction func)
    {
        var nextBlockLabels = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < func.Blocks.Count; i++)
        {
            nextBlockLabels[func.Blocks[i].Label] = i + 1 < func.Blocks.Count
                ? func.Blocks[i + 1].Label
                : null;
        }

        return nextBlockLabels;
    }

    private static void EmitGotoUnlessFallthrough(
        StringBuilder sb,
        string pad,
        string currentLabel,
        string targetLabel,
        IReadOnlyDictionary<string, string?> nextBlockLabels,
        Dictionary<string, string> labelNames,
        string functionName)
    {
        if (nextBlockLabels.TryGetValue(currentLabel, out var nextLabel) &&
            string.Equals(nextLabel, targetLabel, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        var cxxLabel = RequireLabel(targetLabel, labelNames, currentLabel, functionName);
        sb.AppendLine($"{pad}goto {cxxLabel};");
    }

    private static uint FunctionNonvolatileFprWriteMask(IrFunction func)
    {
        uint mask = 0;
        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                mask |= InstructionNonvolatileFprWriteMask(instruction);
            }
        }

        return mask;
    }

    private static uint LeafStackPreservedFprMask(IrFunction func)
    {
        if (!FunctionEligibleForLeafRegisterCache(func))
            return 0;

        var order = 0;
        var returns = new List<int>();
        var saves = new Dictionary<int, int>();
        var restores = new Dictionary<int, int>();
        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is IrReturn)
                    returns.Add(order);
                if (instruction is IrAssign assign)
                {
                    if (block.Label.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase) &&
                        assign.Value.Kind == "register" &&
                        TryParseNonvolatileFpr(assign.Value.RegisterName, out var savedFpr) &&
                        assign.Destination.Equals(LeafStackSavedFprName(savedFpr), StringComparison.OrdinalIgnoreCase))
                    {
                        saves[savedFpr] = order;
                    }

                    if (TryParseNonvolatileFpr(assign.Destination, out var restoredFpr) &&
                        assign.Value.Kind == "register" &&
                        assign.Value.RegisterName is { } source &&
                        source.Equals(LeafStackSavedFprName(restoredFpr), StringComparison.OrdinalIgnoreCase))
                    {
                        restores[restoredFpr] = order;
                    }
                }
                ++order;
            }
        }

        if (returns.Count != 1)
            return 0;

        uint mask = 0;
        foreach (var fpr in saves.Keys)
        {
            if (restores.TryGetValue(fpr, out var restoreOrder) &&
                saves[fpr] < restoreOrder && restoreOrder < returns[0])
            {
                mask |= 1u << fpr;
            }
        }
        return mask;
    }

    private static uint InstructionNonvolatileFprWriteMask(IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrAssign assign:
                return NonvolatileFprMask(assign.Destination);

            case IrBinary binary:
                return NonvolatileFprMask(binary.Destination);

            case IrLoad load:
                return NonvolatileFprMask(load.Destination);

            case IrCall call:
                return InlineRestoreNonvolatileFprMask(call) |
                       (string.IsNullOrWhiteSpace(call.Destination) ? 0 : NonvolatileFprMask(call.Destination));

            case IrIndirectCall indirectCall:
                return string.IsNullOrWhiteSpace(indirectCall.Destination)
                    ? 0
                    : NonvolatileFprMask(indirectCall.Destination);

            case IrPhi phi:
                return NonvolatileFprMask(phi.Destination);

            default:
                return 0;
        }
    }

    private static uint InlineRestoreNonvolatileFprMask(IrCall call)
    {
        if (!TryParseAddress(call.Target, out var address) ||
            !TryGetInlineGuestThunkSpec(address, out var spec) ||
            spec.Kind != InlineGuestThunkKind.RestFpr)
        {
            return 0;
        }

        uint mask = 0;
        for (var fpr = Math.Max(14, spec.StartRegister); fpr <= 31; fpr++)
        {
            mask |= 1u << fpr;
        }
        return mask;
    }

    private static uint NonvolatileFprMask(string registerName)
    {
        if (string.IsNullOrWhiteSpace(registerName) ||
            !IsFloatRegister(registerName))
        {
            return 0;
        }

        var index = ParseFloatRegisterIndex(registerName);
        return index is >= 14 and <= 31 ? 1u << index : 0;
    }

    private static void EmitNormalizePairedStateOnEdge(
        StringBuilder sb,
        string pad,
        string fromLabel,
        string toLabel,
        IReadOnlyDictionary<string, HashSet<string>> pairedOut,
        IReadOnlyDictionary<string, HashSet<string>> pairedIn,
        IReadOnlyDictionary<string, HashSet<string>> liveIn)
    {
        pairedOut.TryGetValue(fromLabel, out var fromSet);
        pairedIn.TryGetValue(toLabel, out var toSet);

        fromSet ??= new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        toSet ??= new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        var candidateRegs = new HashSet<string>(fromSet, StringComparer.OrdinalIgnoreCase);
        candidateRegs.UnionWith(toSet);
        if (liveIn.TryGetValue(toLabel, out var targetLive))
            candidateRegs.IntersectWith(targetLive);
        else
            candidateRegs.Clear();

        foreach (var reg in candidateRegs
                     .Where(IsFloatRegister)
                     .OrderBy(ParseFloatRegisterIndex)
                     .ThenBy(r => r, StringComparer.OrdinalIgnoreCase))
        {
            var fromPaired = fromSet.Contains(reg);
            var toPaired = toSet.Contains(reg);
            if (fromPaired == toPaired)
                continue;

            var regIndex = ParseFloatRegisterIndex(reg);
            // A CFG edge is not a call boundary: the resident local is the
            // authoritative representation that has to be normalized here.
            var regExpr = _activeResidency is null
                ? $"ctx->fpr[{regIndex}].d"
                : _activeResidency.FprScalar(regIndex, written: true);
            sb.AppendLine(toPaired
                ? $"{pad}{regExpr} = PPC_PsFromScalarInline({regExpr});"
                : $"{pad}{regExpr} = PPC_PsToScalarInline({regExpr});");
        }
    }

    private static int ParseFloatRegisterIndex(string name)
    {
        var baseName = GetRegisterBaseName(name);
        if (baseName.Length > 1 &&
            baseName[0] == 'f' &&
            int.TryParse(baseName.AsSpan(1), out var index))
        {
            return index;
        }

        return int.MaxValue;
    }
}
