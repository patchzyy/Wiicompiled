using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private static void EmitInstruction(string currentLabel, int directCallOrdinal, IrInstruction ins, StringBuilder sb, int bufferBaseLength, int indent,
        IrCfg cfg, Dictionary<string, string> labelNames, RepresentationEnvironment types, FunctionAbiClassification signature,
        Dictionary<string, bool> localPaired, IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<string, uint> knownConstants,
        IReadOnlyDictionary<string, uint> localConstants,
        LinkedAddressRemap? linkedAddressRemap,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets,
        StackAddressFacts stackFacts,
        bool inlineGuestThunkStackBase,
        uint? localFallthroughLr,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlyDictionary<uint, GuestAbiContract> stateFreeAbiContracts,
        IReadOnlyDictionary<uint, string> stateFreeCallSymbols,
        IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant> stateFreeCallSiteVariants,
        IReadOnlySet<uint> modOverridableCallTargets,
        bool shareLrContinuationDispatch)
    {
        if (ins is IrPhi)
        {
            return; // Phi nodes are eliminated before codegen.
        }

        var pad = IndentPad(indent);
        switch (ins)
        {
            case IrAssign assign:
                {
                    var dest = ToDestination(assign.Destination, types);
                    var src = ToRegisterAssignmentExpression(assign.Destination, assign.Value, types);
                    sb.AppendLine($"{pad}{dest} = {src};");
                    if (IsFloatRegister(assign.Destination))
                    {
                        var paired = false;
                        if (assign.Value.Kind == "register" &&
                            assign.Value.RegisterName != null &&
                            IsFloatRegister(assign.Value.RegisterName))
                        {
                            var srcBase = GetRegisterBaseName(assign.Value.RegisterName);
                            paired = localPaired.TryGetValue(srcBase, out var isPaired) && isPaired;
                        }
                        localPaired[GetRegisterBaseName(assign.Destination)] = paired;
                    }
                }
                break;
            case IrBinary bin:
                {
                    var dest = ToDestination(bin.Destination, types);
                    var scalarFloatOperation = IsFloatRegister(bin.Destination);
                    var left = scalarFloatOperation
                        ? ToScalarFloatExpression(bin.Left, types, localPaired)
                        : ToExpression(bin.Left, types);
                    var right = scalarFloatOperation
                        ? ToScalarFloatExpression(bin.Right, types, localPaired)
                        : ToExpression(bin.Right, types);
                    var expr = BinaryExpressionWithOperands(bin, left, right);
                    sb.AppendLine($"{pad}{dest} = {expr};");
                    if (IsFloatRegister(bin.Destination))
                    {
                        localPaired[GetRegisterBaseName(bin.Destination)] = false;
                    }
                }
                break;
            case IrLoad load:
                {
                    var dest = ToDestination(load.Destination, types);
                    var destType = types.Get(load.Destination);
                    var isFloatDest = (destType is ValueRepresentation pt && pt.IsFloat) ||
                                      IsFloatRegister(load.Destination);

                    if (TryEmitStackLoad(load, dest, isFloatDest, sb, pad, types, stackFacts))
                    {
                    }
                    else if (isFloatDest && load.SizeBytes == 4)
                    {
                        sb.AppendLine($"{pad}{dest} = {GuestLoadHelper(4, isFloat: true)}({RemapAddress(load.Address, types, localConstants, linkedAddressRemap)});");
                    }
                    else if (isFloatDest && load.SizeBytes == 8)
                    {
                        sb.AppendLine($"{pad}{dest} = {GuestLoadHelper(8, isFloat: true)}({RemapAddress(load.Address, types, localConstants, linkedAddressRemap)});");
                    }
                    else
                    {
                        sb.AppendLine($"{pad}{dest} = {GuestLoadHelper(load.SizeBytes, isFloat: false)}({RemapAddress(load.Address, types, localConstants, linkedAddressRemap)});");
                    }
                    if (IsFloatRegister(load.Destination))
                    {
                        localPaired[GetRegisterBaseName(load.Destination)] = false;
                    }
                }
                break;
            case IrStore store:
                {
                    var srcExpr = ToExpression(store.Source, types);
                    var isFloatStore = store.Source.Kind == "register" && 
                                       store.Source.RegisterName != null &&
                                       IsFloatRegister(store.Source.RegisterName);
                    if (isFloatStore &&
                        store.SizeBytes == 4 &&
                        store.Source.RegisterName != null &&
                        localPaired.TryGetValue(GetRegisterBaseName(store.Source.RegisterName), out var sourceIsPaired) &&
                        sourceIsPaired)
                    {
                        srcExpr = $"PPC_PsToScalarInline({srcExpr})";
                    }

                    if (TryEmitKnownGpuFifoStore(store, srcExpr, isFloatStore, sb, pad, knownConstants))
                    {
                        break;
                    }

                    // A translate-time-constant address outside the MMIO window (gather-pipe already handled
                    // above) always takes the flat helper's plain-store arm, so the runtime MMIO test is provably dead.
                    var provenRamStore =
                        TryGetKnownEffectiveAddress(store.Address, knownConstants, out var knownStoreAddress) &&
                        IsProvenRamStore(knownStoreAddress, store.SizeBytes);
                    string StoreHelper(int sizeBytes, bool isFloat) => provenRamStore
                        ? FlatWriteRamHelper(Bits(sizeBytes), isFloat)
                        : GuestStoreHelper(sizeBytes, isFloat);

                    if (TryEmitStackStore(store, srcExpr, isFloatStore, sb, pad, types, stackFacts))
                    {
                    }
                    else if (isFloatStore && store.SizeBytes == 4)
                    {
                        sb.AppendLine($"{pad}{StoreHelper(4, isFloat: true)}({RemapAddress(store.Address, types, localConstants, linkedAddressRemap)}, {srcExpr});");
                    }
                    else if (isFloatStore && store.SizeBytes == 8)
                    {
                        sb.AppendLine($"{pad}{StoreHelper(8, isFloat: true)}({RemapAddress(store.Address, types, localConstants, linkedAddressRemap)}, {srcExpr});");
                    }
                    else
                    {
                        // Memory::Write8/Write16 take uint8_t/uint16_t, so cast to
                        // the exact parameter type instead of widening to
                        // uint32_t and letting the call narrow it again. The
                        // stored bytes are identical either way. Write64 keeps a
                        // 32-bit cast: it has no 64-bit integer producer here.
                        var integerSource = store.SizeBytes switch
                        {
                            4 => srcExpr,
                            1 or 2 => $"static_cast<uint{Bits(store.SizeBytes)}_t>({srcExpr})",
                            _ => $"static_cast<uint32_t>({srcExpr})"
                        };
                        sb.AppendLine($"{pad}{StoreHelper(store.SizeBytes, isFloat: false)}({RemapAddress(store.Address, types, localConstants, linkedAddressRemap)}, {integerSource});");
                    }
                }
                break;
            case IrResolveGuestMemoryRange resolve:
                {
                    var start = RemapAddress(
                        new IrAddress(resolve.Base.RegisterName!, resolve.MinOffset),
                        types, localConstants, linkedAddressRemap);
                    sb.AppendLine($"{pad}{resolve.Destination} = MemoryInline::ResolveRangeHost({start}, 0, {resolve.Length}u, {(resolve.NeedsReadAccess ? "true" : "false")}, {(resolve.NeedsWriteAccess ? "true" : "false")});");
                }
                break;
            case IrResolvedLoad load:
                {
                    var dest = ToDestination(load.Destination, types);
                    var destType = types.Get(load.Destination);
                    var isFloatDest = (destType is ValueRepresentation pt && pt.IsFloat) || IsFloatRegister(load.Destination);
                    var address = RemapAddress(load.OriginalAddress, types, localConstants, linkedAddressRemap);
                    var helper = isFloatDest ? $"Float{Bits(load.SizeBytes)}" : Bits(load.SizeBytes).ToString();
                    sb.AppendLine($"{pad}{dest} = MemoryInline::ReadResolved{helper}({load.Range}, {load.RangeOffset}u, {address});");
                    if (IsFloatRegister(load.Destination))
                        localPaired[GetRegisterBaseName(load.Destination)] = false;
                }
                break;
            case IrResolvedStore store:
                {
                    var srcExpr = ToExpression(store.Source, types);
                    var isFloatStore = store.Source.Kind == "register" && store.Source.RegisterName is not null &&
                                       IsFloatRegister(store.Source.RegisterName);
                    if (isFloatStore && store.SizeBytes == 4 && store.Source.RegisterName is not null &&
                        localPaired.TryGetValue(GetRegisterBaseName(store.Source.RegisterName), out var sourceIsPaired) && sourceIsPaired)
                        srcExpr = $"PPC_PsToScalarInline({srcExpr})";
                    var address = RemapAddress(store.OriginalAddress, types, localConstants, linkedAddressRemap);
                    var helper = isFloatStore ? $"Float{Bits(store.SizeBytes)}" : Bits(store.SizeBytes).ToString();
                    var value = isFloatStore || store.SizeBytes == 4 ? srcExpr : $"static_cast<uint{Bits(store.SizeBytes)}_t>({srcExpr})";
                    sb.AppendLine($"{pad}MemoryInline::WriteResolved{helper}({store.Range}, {store.RangeOffset}u, {address}, {value});");
                }
                break;
            case IrResolvedPsqLoad load:
                {
                    var helper = load.KnownGqr.HasValue
                        ? "PPC_PsqLKnownResolvedInline"
                        : "PPC_PsqLResolvedInline";
                    var known = load.KnownGqr.HasValue ? $", 0x{load.KnownGqr.Value:X8}u" : string.Empty;
                    var template = $"<{load.W}u, {load.I}u{known}>";
                    var address = ToExpression(load.OriginalAddress, types);
                    var knownCall = $"{helper}{template}(ctx, {load.Range}, {load.RangeOffset}u, {address})";
                    if (load.GuardKnownGqr && load.KnownGqr.HasValue)
                    {
                        var fallback = $"PPC_PsqLResolvedInline<{load.W}u, {load.I}u>(ctx, {load.Range}, {load.RangeOffset}u, {address})";
                        sb.AppendLine($"{pad}if ({GqrEntryGuardName(load.I, load.KnownGqr.Value)}) {{");
                        sb.AppendLine($"{pad}    {PairedAssignment(load.Destination, knownCall, types)}");
                        sb.AppendLine($"{pad}}} else {{");
                        sb.AppendLine($"{pad}    {PairedAssignment(load.Destination, fallback, types)}");
                        sb.AppendLine($"{pad}}}");
                    }
                    else sb.AppendLine($"{pad}{PairedAssignment(load.Destination, knownCall, types)}");
                    if (IsFloatRegister(load.Destination))
                        localPaired[GetRegisterBaseName(load.Destination)] = true;
                }
                break;
            case IrResolvedPsqStore store:
                {
                    var helper = store.KnownGqr.HasValue
                        ? "PPC_PsqStKnownResolvedInline"
                        : "PPC_PsqStResolvedInline";
                    var known = store.KnownGqr.HasValue ? $", 0x{store.KnownGqr.Value:X8}u" : string.Empty;
                    var template = $"<{store.W}u, {store.I}u{known}>";
                    var value = ToPairedFloatExpression(store.Source, types, localPaired);
                    var address = ToExpression(store.OriginalAddress, types);
                    var knownCall = $"{helper}{template}(ctx, {store.Range}, {store.RangeOffset}u, {address}, {value})";
                    if (store.GuardKnownGqr && store.KnownGqr.HasValue)
                    {
                        sb.AppendLine($"{pad}if ({GqrEntryGuardName(store.I, store.KnownGqr.Value)}) {{");
                        sb.AppendLine($"{pad}    {knownCall};");
                        sb.AppendLine($"{pad}}} else {{");
                        sb.AppendLine($"{pad}    PPC_PsqStResolvedInline<{store.W}u, {store.I}u>(ctx, {store.Range}, {store.RangeOffset}u, {address}, {value});");
                        sb.AppendLine($"{pad}}}");
                    }
                    else sb.AppendLine($"{pad}{knownCall};");
                }
                break;
            case IrResolvedLoadPair pair:
                {
                    bool IsFloat(string destination) =>
                        (types.Get(destination) is ValueRepresentation primitive && primitive.IsFloat) || IsFloatRegister(destination);
                    var firstFloat = IsFloat(pair.FirstDestination);
                    var secondFloat = IsFloat(pair.SecondDestination);
                    var firstAddress = RemapAddress(pair.FirstOriginalAddress, types, localConstants, linkedAddressRemap);
                    var secondAddress = RemapAddress(pair.SecondOriginalAddress, types, localConstants, linkedAddressRemap);
                    var pairHelper = pair.ElementSizeBytes == 2
                        ? $"MemoryInline::ReadResolvedPair16({pair.Range}, {pair.RangeOffset}u)"
                        : $"MemoryInline::ReadResolvedPair32({pair.Range}, {pair.RangeOffset}u)";
                    string FastValue(string member, bool isFloat) => isFloat ? $"PpcBitCastToFloatInline(resolved_pair.{member})" : $"resolved_pair.{member}";
                    string SlowHelper(bool isFloat) => isFloat ? $"Float{Bits(pair.ElementSizeBytes)}" : Bits(pair.ElementSizeBytes).ToString();
                    var firstMember = pair.Descending ? "second" : "first";
                    var secondMember = pair.Descending ? "first" : "second";
                    var firstOffset = pair.Descending ? pair.RangeOffset + pair.ElementSizeBytes : pair.RangeOffset;
                    var secondOffset = pair.Descending ? pair.RangeOffset : pair.RangeOffset + pair.ElementSizeBytes;
                    sb.AppendLine($"{pad}{{");
                    sb.AppendLine($"{pad}    const auto resolved_pair = {pairHelper};");
                    sb.AppendLine($"{pad}    if (resolved_pair.valid) {{");
                    sb.AppendLine($"{pad}        {ToDestination(pair.FirstDestination, types)} = {FastValue(firstMember, firstFloat)};");
                    sb.AppendLine($"{pad}        {ToDestination(pair.SecondDestination, types)} = {FastValue(secondMember, secondFloat)};");
                    sb.AppendLine($"{pad}    }} else {{");
                    sb.AppendLine($"{pad}        {ToDestination(pair.FirstDestination, types)} = MemoryInline::ReadResolved{SlowHelper(firstFloat)}({pair.Range}, {firstOffset}u, {firstAddress});");
                    sb.AppendLine($"{pad}        {ToDestination(pair.SecondDestination, types)} = MemoryInline::ReadResolved{SlowHelper(secondFloat)}({pair.Range}, {secondOffset}u, {secondAddress});");
                    sb.AppendLine($"{pad}    }}");
                    sb.AppendLine($"{pad}}}");
                    if (IsFloatRegister(pair.FirstDestination)) localPaired[GetRegisterBaseName(pair.FirstDestination)] = false;
                    if (IsFloatRegister(pair.SecondDestination)) localPaired[GetRegisterBaseName(pair.SecondDestination)] = false;
                }
                break;
            case IrResolvedStorePair pair:
                {
                    bool IsFloat(IrValue source) => source.RegisterName is { } name && IsFloatRegister(name);
                    string Source(IrValue source)
                    {
                        var expression = ToExpression(source, types);
                        if (pair.ElementSizeBytes == 4 && source.RegisterName is { } name && IsFloatRegister(name) &&
                            localPaired.TryGetValue(GetRegisterBaseName(name), out var paired) && paired)
                            expression = $"PPC_PsToScalarInline({expression})";
                        return expression;
                    }
                    var firstFloat = IsFloat(pair.FirstSource);
                    var secondFloat = IsFloat(pair.SecondSource);
                    var first = Source(pair.FirstSource);
                    var second = Source(pair.SecondSource);
                    string Raw(string value, bool isFloat) => isFloat
                        ? $"PpcBitCastToU32Inline(static_cast<float>({value}))"
                        : $"static_cast<uint32_t>({value})";
                    var lowSource = pair.Descending ? second : first;
                    var highSource = pair.Descending ? first : second;
                    var lowFloat = pair.Descending ? secondFloat : firstFloat;
                    var highFloat = pair.Descending ? firstFloat : secondFloat;
                    var packed = pair.ElementSizeBytes == 2
                        ? $"((static_cast<uint32_t>(static_cast<uint16_t>({lowSource})) << 16) | static_cast<uint16_t>({highSource}))"
                        : $"((static_cast<uint64_t>({Raw(lowSource, lowFloat)}) << 32) | {Raw(highSource, highFloat)})";
                    var firstAddress = RemapAddress(pair.FirstOriginalAddress, types, localConstants, linkedAddressRemap);
                    var secondAddress = RemapAddress(pair.SecondOriginalAddress, types, localConstants, linkedAddressRemap);
                    var fast = pair.ElementSizeBytes == 2
                        ? $"MemoryInline::WriteResolvedPair16({pair.Range}, {pair.RangeOffset}u, {packed})"
                        : $"MemoryInline::WriteResolvedPair32({pair.Range}, {pair.RangeOffset}u, {packed})";
                    string Helper(bool isFloat) => isFloat ? $"Float{Bits(pair.ElementSizeBytes)}" : Bits(pair.ElementSizeBytes).ToString();
                    var firstOffset = pair.Descending ? pair.RangeOffset + pair.ElementSizeBytes : pair.RangeOffset;
                    var secondOffset = pair.Descending ? pair.RangeOffset : pair.RangeOffset + pair.ElementSizeBytes;
                    sb.AppendLine($"{pad}if (!{fast}) {{");
                    sb.AppendLine($"{pad}    MemoryInline::WriteResolved{Helper(firstFloat)}({pair.Range}, {firstOffset}u, {firstAddress}, {first});");
                    sb.AppendLine($"{pad}    MemoryInline::WriteResolved{Helper(secondFloat)}({pair.Range}, {secondOffset}u, {secondAddress}, {second});");
                    sb.AppendLine($"{pad}}}");
                }
                break;
            case IrCall call:
                if (TryParseAddress(call.Target, out var addr))
                {
                    if (TryEmitInlineGuestThunk(addr, sb, pad, types, inlineGuestThunkStackBase))
                    {
                        break;
                    }

                    // Guest call boundary: CpuContext becomes authoritative for the callee. Argument normalization
                    // and state-free marshalling both read/write CpuContext directly, so the flush choice below
                    // can't be narrowed when either is present; normalization is built into its own buffer first
                    // so the emit order stays fixed.
                    var argumentNormalization = new StringBuilder();
                    EmitNormalizeGuestCallFloatArguments(
                        call.Target, call.Arguments, argumentNormalization, pad, localPaired, guestAbiProvider);
                    var callSiteKey = new GuestStateFreeCallSiteKey(currentLabel, addr, directCallOrdinal);
                    var hasCallSiteVariant = stateFreeCallSiteVariants.TryGetValue(callSiteKey, out var callSiteVariant);
                    var stateFreeSymbol = hasCallSiteVariant
                        ? callSiteVariant!.Symbol
                        : stateFreeCallSymbols.GetValueOrDefault(addr);
                    var stateFreeContract = hasCallSiteVariant
                        ? callSiteVariant!.Contract
                        : stateFreeAbiContracts.GetValueOrDefault(addr);
                    var usesStateFreeCall = stateFreeSymbol is not null && stateFreeContract is not null &&
                                            IsStateFreeCallSafe(stateFreeContract);
                    var resumesThroughLinkRegister =
                        nonReturningCallTargets.Contains(addr) ||
                        (lrContinuationCallTargets.Contains(addr) && localFallthroughLr.HasValue);
                    // The explicit-state signature carries everything the callee reads/writes, so a resident caller
                    // can pass host registers instead of spilling to CpuContext. Excluded: paired-single arg/return
                    // normalization rewrites ctx->fpr[N] directly, and LR-continuation sites dispatch on ctx->lr
                    // after the callee writes it, so those keep the full boundary.
                    var marshalsStateFreeThroughResidency =
                        usesStateFreeCall &&
                        _activeResidency is not null &&
                        argumentNormalization.Length == 0 &&
                        !resumesThroughLinkRegister &&
                        !IsGuestPairedScalarFloatReturn(guestAbiProvider, call.Target);
                    if (marshalsStateFreeThroughResidency)
                    {
                        EmitResidentStateFreeCall(
                            sb, pad, bufferBaseLength, addr, stateFreeSymbol!, stateFreeContract!,
                            localFallthroughLr, stateFreeAbiContracts, stateFreeCallSymbols);
                        // Emits nothing for this shape (the paired-scalar return
                        // case is excluded above); it still owns the f1
                        // representation tag every guest call boundary applies.
                        EmitNormalizeGuestCallFloatReturn(call.Target, sb, pad, localPaired, guestAbiProvider);
                        break;
                    }
                    var boundarySync = usesStateFreeCall || argumentNormalization.Length != 0
                        ? ResidencyBoundarySync.Full
                        : ResolveDirectCallBoundarySync(addr, guestAbiContracts, modOverridableCallTargets);
                    if (!boundarySync.IsFull &&
                        IsGuestPairedScalarFloatReturn(guestAbiProvider, call.Target))
                    {
                        // The return normalization below rewrites ctx->fpr[1]
                        // directly, so f1 has to be part of this boundary even if
                        // the callee contract does not mention it.
                        boundarySync = boundarySync.WithFpr(1);
                    }
                    AppendFlush(sb, pad, boundarySync);
                    sb.Append(argumentNormalization);
                    if (usesStateFreeCall)
                    {
                        sb.AppendLine($"{pad}if ({StateFreeAvailabilityCondition(addr, stateFreeAbiContracts, stateFreeCallSymbols)}) {{");
                        if (StateFreeHasOutputs(stateFreeContract!))
                        {
                            // Uniquified by the call site's position in the
                            // translation unit. The body is built in its own
                            // buffer, so the offset of that buffer inside the
                            // unit has to be added back.
                            var stateFreeResult = $"state_free_result_{addr:X8}_{bufferBaseLength + sb.Length:X}";
                            sb.AppendLine($"{pad}    const auto {stateFreeResult} = {stateFreeSymbol}({StateFreeCallArguments(stateFreeContract!, localFallthroughLr)});");
                            AppendStateFreeResultStores(sb, pad + "    ", addr, stateFreeContract!, stateFreeResult);
                        }
                        else
                        {
                            sb.AppendLine($"{pad}    {stateFreeSymbol}({StateFreeCallArguments(stateFreeContract!, localFallthroughLr)});");
                        }
                        sb.AppendLine($"{pad}}} else {{");
                        RecordEmittedDirectCallTarget(addr);
                        sb.AppendLine($"{pad}    InvokeDirectCpu<0x{addr:X8}u>(ctx);");
                        sb.AppendLine($"{pad}}}");
                    }
                    else
                    {
                        RecordEmittedDirectCallTarget(addr);
                        sb.AppendLine($"{pad}InvokeDirectCpu<0x{addr:X8}u>(ctx);");
                    }
                    if (resumesThroughLinkRegister)
                    {
                        // The callee may resume this body through a local LR
                        // continuation label, so the locals must be refreshed
                        // from the context it just wrote. The exit paths below
                        // return without writing locals back.
                        AppendReload(sb, pad, boundarySync);
                        var fallbackPad = pad;
                        if (localFallthroughLr.HasValue)
                        {
                            sb.AppendLine($"{pad}if (ctx->lr != 0x{localFallthroughLr.Value:X8}u) {{");
                            fallbackPad = IndentPad(indent + 1);
                        }

                        if (shareLrContinuationDispatch)
                        {
                            sb.AppendLine($"{fallbackPad}goto lr_continuation_dispatch;");
                        }
                        else
                        {
                            EmitLocalLrContinuationDispatch(sb, fallbackPad, labelNames);
                            sb.AppendLine($"{fallbackPad}if (TranslatedFunctionRegistry::FindByAddressPtr(ctx->lr) != nullptr) {{");
                            sb.AppendLine($"{fallbackPad}    InvokeIndirectCpu(ctx->lr, ctx);");
                            sb.AppendLine($"{fallbackPad}}}");
                            sb.AppendLine($"{fallbackPad}return;");
                        }
                        if (localFallthroughLr.HasValue)
                        {
                            sb.AppendLine($"{pad}}}");
                        }
                        break;
                    }
                    EmitNormalizeGuestCallFloatReturn(call.Target, sb, pad, localPaired, guestAbiProvider);
                    AppendReload(sb, pad, boundarySync);
                }
                else if (IsGuestCallTarget(call.Target))
                {
                    var targetSymbol = FormatSymbol(call.Target);
                    AppendFlush(sb, pad);
                    EmitNormalizeGuestCallFloatArguments(call.Target, call.Arguments, sb, pad, localPaired, guestAbiProvider);
                    sb.AppendLine($"{pad}{targetSymbol}(ctx);");
                    EmitNormalizeGuestCallFloatReturn(call.Target, sb, pad, localPaired, guestAbiProvider);
                    AppendReload(sb, pad);
                }
                else
                {
                    // Try inline expansion first for performance
                    if (TryEmitInlinePpc(call, sb, pad, types, localPaired, stackFacts))
                    {
                        // Inline code was emitted - update paired tracking
                        if (!string.IsNullOrWhiteSpace(call.Destination) && IsFloatRegister(call.Destination))
                        {
                            localPaired[GetRegisterBaseName(call.Destination)] = IsPairedProducerTarget(call.Target);
                        }
                        break;
                    }

                    // Helper function call (e.g., PPC_Ps*, memory helpers)
                    var isPairedConsumer = IsPairedConsumerTarget(call.Target);
                    var isSingleConsumer = IsSinglePrecisionConsumerTarget(call.Target);
                    var isScalarConsumer = IsScalarFloatConsumerTarget(call.Target);
                    var args = new List<string>(call.Arguments.Count);
                    for (var argIndex = 0; argIndex < call.Arguments.Count; argIndex++)
                    {
                        var argVal = call.Arguments[argIndex];
                        var expr = isPairedConsumer
                            ? ToPairedFloatExpression(argVal, types, localPaired)
                            : (isSingleConsumer || isScalarConsumer)
                                ? ToScalarFloatExpression(argVal, types, localPaired)
                                : ToExpression(argVal, types);
                        args.Add(expr);
                    }
                    // Helpers can implicitly touch guest state through CpuContext (PPC_Stwcx writes CR0, PPC_Lswi
                    // writes GPRs, OSSystemCall is a full boundary). Resident locals can't observe that, so the
                    // cataloged effect becomes a mini boundary: flush what it may read, reload what it may write.
                    var helperSync = _activeResidency is not null
                        ? ResidencyBoundarySync.FromHelperEffect(AnalyzeGuestHelperEffect(call))
                        : null;
                    if (helperSync is not null && !helperSync.IsEmpty)
                    {
                        AppendFlush(sb, pad, helperSync);
                    }
                    var callExpr = $"{FormatSymbol(call.Target)}({string.Join(", ", args)})";
                    if (!string.IsNullOrWhiteSpace(call.Destination))
                    {
                        sb.AppendLine(IsPairedProducerTarget(call.Target)
                            ? $"{pad}{PairedAssignment(call.Destination, callExpr, types)}"
                            : $"{pad}{ToDestination(call.Destination, types)} = {callExpr};");
                    }
                    else
                    {
                        sb.AppendLine($"{pad}{callExpr};");
                    }
                    if (helperSync is not null && !helperSync.IsEmpty)
                    {
                        // An explicit destination carries the helper's return
                        // value in the local; reloading it from CpuContext would
                        // clobber that result with stale state.
                        var reloadSync = ExcludeDestinationFromReload(helperSync, call.Destination);
                        AppendReload(sb, pad, reloadSync);
                    }
                }
                if (!string.IsNullOrWhiteSpace(call.Destination) && IsFloatRegister(call.Destination))
                {
                    localPaired[GetRegisterBaseName(call.Destination)] = IsPairedProducerTarget(call.Target);
                }
                break;
            case IrIndirectCall icall:
                {
                    var indirectTarget = ToExpression(icall.Target, types);
                    AppendFlush(sb, pad);
                    sb.AppendLine($"{pad}InvokeIndirectCpu({indirectTarget}, ctx);");
                    AppendReload(sb, pad);
                    // f1 is the conventional scalar floating-point return.
                    // Do not retag the other volatile FPRs: a callee that leaves
                    // one untouched also leaves its packed payload untouched.
                    localPaired["f1"] = false;
                }
                break;
            case IrIndirectJump ijump:
                {
                    var jumpTarget = ToExpression(ijump.Target, types);
                    // Tail dispatch: the callee inherits authoritative context
                    // and this frame never observes registers again, so the
                    // locals must not be written back afterwards.
                    AppendFlush(sb, pad);
                    sb.AppendLine($"{pad}InvokeIndirectJump({jumpTarget}, ctx);");
                    sb.AppendLine($"{pad}return;");
                }
                break;
            case IrComment comment:
                {
                    // Emit as a C++ comment - these are legitimate comments like nop, mtcrf no-op, psq operations
                    sb.AppendLine($"{pad}// {EscapeForCxxLiteral(comment.Text)}");
                }
                break;
            case IrTracePpc trace:
                {
                }
                break;
            case IrUndefined undef:
                {
                    var details = undef.Reason == null
                        ? $"{undef.Disassembly}"
                        : $"{undef.Disassembly} | {undef.Reason}";
                    var escaped = EscapeForCxxLiteral(details);
                    sb.AppendLine($"{pad}UNDEFINED(0x{undef.Address:X8}u, 0x{undef.RawInstruction:X8}u, \"{escaped}\");");
                }
                break;
            case IrSetCrField setCr:
                {
                    var leftExpr = ToExpression(setCr.Left, types);
                    var rightExpr = ToExpression(setCr.Right, types);
                    // The resident helpers take the CR by reference and XER by
                    // value. Both are localized, so the summary-overflow copy
                    // reads a host register instead of reloading CpuContext
                    // after every potentially aliasing guest store.
                    if (IsFloatValue(setCr.Left, types) || IsFloatValue(setCr.Right, types))
                    {
                        leftExpr = ToScalarFloatExpression(setCr.Left, types, localPaired);
                        rightExpr = ToScalarFloatExpression(setCr.Right, types, localPaired);
                        sb.AppendLine(_activeResidency is null
                            ? $"{pad}SetCRFloat(ctx, {setCr.FieldIndex}, {leftExpr}, {rightExpr});"
                            : $"{pad}SetCRFloatResident({_activeResidency.Cr(written: true)}, {setCr.FieldIndex}, {leftExpr}, {rightExpr});");
                    }
                    else
                    {
                        var leftCast = setCr.IsUnsigned
                            ? $"static_cast<uint32_t>({leftExpr})"
                            : $"static_cast<int32_t>({leftExpr})";
                        var rightCast = setCr.IsUnsigned
                            ? $"static_cast<uint32_t>({rightExpr})"
                            : $"static_cast<int32_t>({rightExpr})";
                        sb.AppendLine(_activeResidency is null
                            ? $"{pad}SetCR(ctx, {setCr.FieldIndex}, {leftCast}, {rightCast});"
                            : $"{pad}SetCRResident({_activeResidency.Cr(written: true)}, {_activeResidency.Xer(written: false)}, {setCr.FieldIndex}, {leftCast}, {rightCast});");
                    }
                }
                break;
        }
    }

    private static bool TryEmitStackLoad(
        IrLoad load,
        string dest,
        bool isFloatDest,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        StackAddressFacts stackFacts)
    {
        if (!stackFacts.TryResolve(load.Address, out _))
        {
            return false;
        }

        var address = Address(load.Address, types);
        if (isFloatDest && load.SizeBytes == 4)
        {
            sb.AppendLine($"{pad}{dest} = {StackLoadHelper(4, isFloat: true)}({address});");
            return true;
        }
        if (isFloatDest && load.SizeBytes == 8)
        {
            sb.AppendLine($"{pad}{dest} = {StackLoadHelper(8, isFloat: true)}({address});");
            return true;
        }

        sb.AppendLine($"{pad}{dest} = {StackLoadHelper(load.SizeBytes, isFloat: false)}({address});");
        return true;
    }

    private static bool TryEmitStackStore(
        IrStore store,
        string srcExpr,
        bool isFloatStore,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        StackAddressFacts stackFacts)
    {
        if (!stackFacts.TryResolve(store.Address, out _))
        {
            return false;
        }

        var address = Address(store.Address, types);
        if (isFloatStore && store.SizeBytes == 4)
        {
            sb.AppendLine($"{pad}{StackStoreHelper(4, isFloat: true)}({address}, {srcExpr});");
            return true;
        }
        if (isFloatStore && store.SizeBytes == 8)
        {
            sb.AppendLine($"{pad}{StackStoreHelper(8, isFloat: true)}({address}, {srcExpr});");
            return true;
        }

        var bits = Bits(store.SizeBytes);
        var integerSource = bits == 32 ? srcExpr : $"static_cast<uint{bits}_t>({srcExpr})";
        sb.AppendLine($"{pad}{StackStoreHelper(store.SizeBytes, isFloat: false)}({address}, {integerSource});");
        return true;
    }

    private static bool TryEmitKnownGpuFifoStore(
        IrStore store,
        string srcExpr,
        bool isFloatStore,
        StringBuilder sb,
        string pad,
        IReadOnlyDictionary<string, uint> knownConstants)
    {
        if (!TryGetKnownGpuFifoStore(store, isFloatStore, knownConstants, out var fifoStore))
        {
            return false;
        }

        if (isFloatStore)
        {
            if (_activeGpuFifoBurstSlot is { } floatSlot)
            {
                EmitGpuFifoBurstSlotStore(sb, pad, floatSlot, srcExpr);
                return true;
            }

            sb.AppendLine($"{pad}GX_HLE_FIFO_WriteFloat(static_cast<float>({srcExpr}));");
            return true;
        }

        if (_activeGpuFifoBurstSlot is { } slot)
        {
            EmitGpuFifoBurstSlotStore(sb, pad, slot, srcExpr);
            return true;
        }

        var helper = fifoStore.ByteLength switch
        {
            1 => "GX_HLE_FIFO_Write8",
            2 => "GX_HLE_FIFO_Write16",
            4 => "GX_HLE_FIFO_Write32",
            _ => throw new InvalidOperationException($"Unexpected GPU FIFO width {fifoStore.ByteLength}.")
        };
        sb.AppendLine($"{pad}{helper}(static_cast<uint{fifoStore.ByteLength * 8}_t>({srcExpr}));");
        return true;
    }

    private static void EmitLocalLrContinuationDispatch(
        StringBuilder sb,
        string pad,
        IReadOnlyDictionary<string, string> labelNames)
    {
        var localContinuations = labelNames
            .Where(static pair => TryParseAddress(pair.Key, out _))
            .Select(static pair =>
            {
                TryParseAddress(pair.Key, out var address);
                return (Address: address, Label: pair.Value);
            })
            .OrderBy(static item => item.Address)
            .ToArray();

        if (localContinuations.Length == 0)
        {
            return;
        }

        sb.AppendLine($"{pad}switch (ctx->lr) {{");
        foreach (var (address, label) in localContinuations)
        {
            RecordEmittedLocalLabel(label);
            sb.AppendLine($"{pad}case 0x{address:X8}u:");
            sb.AppendLine($"{pad}    goto {label};");
        }

        sb.AppendLine($"{pad}default:");
        sb.AppendLine($"{pad}    break;");
        sb.AppendLine($"{pad}}}");
    }

    private static bool TryGetKnownEffectiveAddress(
        IrAddress address,
        IReadOnlyDictionary<string, uint> knownConstants,
        out uint effectiveAddress)
    {
        if (knownConstants.TryGetValue(address.Base, out var baseAddress))
        {
            effectiveAddress = unchecked(baseAddress + (uint)address.Offset);
            return true;
        }

        effectiveAddress = 0;
        return false;
    }

    private static bool IsGpuFifoAddress(uint address) =>
        address >= 0xCC008000u && address < 0xCC008100u;

    private static void EmitNormalizeGuestCallFloatArguments(
        string target,
        IReadOnlyList<IrValue> arguments,
        StringBuilder sb,
        string pad,
        Dictionary<string, bool> localPaired,
        IGuestFunctionAbiProvider guestAbiProvider)
    {
        foreach (var arg in arguments)
        {
            if (arg.Kind != "register" || arg.RegisterName == null)
            {
                continue;
            }

            var baseName = GetRegisterBaseName(arg.RegisterName);
            if (!localPaired.TryGetValue(baseName, out var isPaired) ||
                !isPaired ||
                !IsGuestScalarFloatArgument(guestAbiProvider, target, baseName))
            {
                continue;
            }

            var regIndex = ParseFloatRegisterIndex(baseName);
            sb.AppendLine($"{pad}ctx->fpr[{regIndex}].d = PPC_PsToScalarInline(ctx->fpr[{regIndex}].d);");
            localPaired[baseName] = false;
        }
    }

    private static void EmitNormalizeGuestCallFloatReturn(
        string target,
        StringBuilder sb,
        string pad,
        Dictionary<string, bool> localPaired,
        IGuestFunctionAbiProvider guestAbiProvider)
    {
        if (IsGuestPairedScalarFloatReturn(guestAbiProvider, target))
        {
            sb.AppendLine($"{pad}ctx->fpr[1].d = PPC_PsToScalarInline(ctx->fpr[1].d);");
        }

        // f1 is the conventional scalar floating-point return. Preserve the
        // representation tags for f2-f13: ABI volatility does not mean that an
        // untouched packed payload has magically become a scalar value.
        localPaired["f1"] = false;
    }
}
