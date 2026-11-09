using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Guest register residency for one emitted function body: C++ locals are authoritative between call boundaries (host regs), synced via flush/reload at each one.
    /// Emitted twice, a recording pass then the real pass, so any register reference outside the recorded set fails loud instead of silently reading stale <c>ctx-&gt;</c> state.
    /// </summary>
    private sealed class RegisterResidency
    {
        private readonly bool _recording;
        private readonly SortedSet<int> _gprs = new();
        private readonly SortedSet<int> _fprs = new();
        private readonly SortedSet<int> _writtenGprs = new();
        private readonly SortedSet<int> _writtenFprs = new();
        private bool _cr;
        private bool _ctr;
        private bool _xer;
        private bool _writesCr;
        private bool _writesCtr;
        private bool _writesXer;

        // Dirty-register tracking (CpuContext write-back only for registers whose local changed since the last sync).
        // A register is dirty while its local may differ from CpuContext: set by a write, cleared by an entry load,
        // a reload after a boundary, or a write-back. The recording pass summarizes every block as a gen/kill
        // transfer plus its outgoing edge writes; Snapshot() solves the forward union dataflow over them; the
        // real pass replays the same transfer from the solved block-entry state at every flush point.
        // A function that can resume through a local LR-continuation label has edges this analysis does not
        // see, so it keeps the old write-back-everything-written behaviour.
        private readonly string? _entryLabel;
        private readonly bool _conservative;
        private Dictionary<string, BlockSummary>? _blocks;
        private Dictionary<string, DirtySet>? _blockIn;
        private BlockSummary? _activeBlock;
        private DirtySet _activeIn;
        private string? _edgeTarget;
        private DirtySet _edgeGen;
        private readonly Stack<(Transfer Base, List<Transfer> Done)> _alternatives = new();

        private const byte CrBit = 1;
        private const byte CtrBit = 2;
        private const byte XerBit = 4;

        // What a guest callback may leave changed in CpuContext under the PowerPC EABI (r0, r3-r12,
        // f0-f13, CR, CTR, XER), plus r13, which OS_HLE_ProcessAlarms resets to the SDA base. A runtime
        // native can run guest handlers on the caller's live context behind a contract that declares no
        // writes (OS_HLE_ProcessAlarms from GKI_delay / PPCMtdec), so after any boundary these stay dirty
        // unless it reloaded them; the callbacks preserve the rest.
        private static readonly DirtySet CallbackClobberable = new()
        {
            Gpr = 0x00003FF9u,
            Fpr = 0x00003FFFu,
            Flags = CrBit | CtrBit | XerBit
        };

        internal struct DirtySet : IEquatable<DirtySet>
        {
            public uint Gpr;
            public uint Fpr;
            public byte Flags;

            public static DirtySet Or(DirtySet a, DirtySet b) =>
                new() { Gpr = a.Gpr | b.Gpr, Fpr = a.Fpr | b.Fpr, Flags = (byte)(a.Flags | b.Flags) };

            public static DirtySet And(DirtySet a, DirtySet b) =>
                new() { Gpr = a.Gpr & b.Gpr, Fpr = a.Fpr & b.Fpr, Flags = (byte)(a.Flags & b.Flags) };

            public static DirtySet AndNot(DirtySet a, DirtySet b) =>
                new() { Gpr = a.Gpr & ~b.Gpr, Fpr = a.Fpr & ~b.Fpr, Flags = (byte)(a.Flags & ~b.Flags) };

            public bool Equals(DirtySet other) => Gpr == other.Gpr && Fpr == other.Fpr && Flags == other.Flags;
            public override bool Equals(object? obj) => obj is DirtySet other && Equals(other);
            public override int GetHashCode() => HashCode.Combine(Gpr, Fpr, Flags);
        }

        /// <summary>out = (in minus Kill) plus Gen. Closed under sequencing and under join of alternatives.</summary>
        internal struct Transfer
        {
            public DirtySet Gen;
            public DirtySet Kill;

            public readonly DirtySet Apply(DirtySet input) => DirtySet.Or(DirtySet.AndNot(input, Kill), Gen);

            public void Write(DirtySet written)
            {
                Gen = DirtySet.Or(Gen, written);
                Kill = DirtySet.AndNot(Kill, written);
            }

            public void Clean(DirtySet cleaned)
            {
                Gen = DirtySet.AndNot(Gen, cleaned);
                Kill = DirtySet.Or(Kill, cleaned);
            }

            public static Transfer Join(Transfer a, Transfer b) => new()
            {
                Gen = DirtySet.Or(a.Gen, b.Gen),
                Kill = DirtySet.And(a.Kill, b.Kill)
            };
        }

        private sealed class BlockSummary
        {
            public Transfer Transfer;
            public readonly List<(string Target, DirtySet Gen)> Edges = new();
        }

        private RegisterResidency(bool recording, string? entryLabel = null, bool conservative = false)
        {
            _recording = recording;
            _entryLabel = entryLabel;
            _conservative = conservative;
        }

        public static RegisterResidency CreateRecorder(string? entryLabel = null, bool conservative = false) =>
            new(true, entryLabel, conservative) { _blocks = new Dictionary<string, BlockSummary>(StringComparer.OrdinalIgnoreCase) };

        public RegisterResidency Snapshot()
        {
            var snapshot = new RegisterResidency(false, _entryLabel, _conservative);
            snapshot._gprs.UnionWith(_gprs);
            snapshot._fprs.UnionWith(_fprs);
            snapshot._writtenGprs.UnionWith(_writtenGprs);
            snapshot._writtenFprs.UnionWith(_writtenFprs);
            snapshot._cr = _cr;
            snapshot._ctr = _ctr;
            snapshot._xer = _xer;
            snapshot._writesCr = _writesCr;
            snapshot._writesCtr = _writesCtr;
            snapshot._writesXer = _writesXer;
            snapshot._blocks = _blocks;
            snapshot._blockIn = SolveDirtyEntryStates(snapshot.WrittenSet());
            return snapshot;
        }

        private DirtySet WrittenSet()
        {
            var set = new DirtySet();
            foreach (var gpr in _writtenGprs) set.Gpr |= 1u << gpr;
            foreach (var fpr in _writtenFprs) set.Fpr |= 1u << fpr;
            if (_writesCr) set.Flags |= CrBit;
            if (_writesCtr) set.Flags |= CtrBit;
            if (_writesXer) set.Flags |= XerBit;
            return set;
        }

        /// <summary>
        /// Forward union dataflow over the recorded blocks. The entry block starts clean (entry loads); a block no
        /// recorded edge reaches (and that is not the entry) starts with everything dirty, the old behaviour.
        /// </summary>
        private Dictionary<string, DirtySet>? SolveDirtyEntryStates(DirtySet all)
        {
            if (_blocks is null || _conservative || _entryLabel is null || !_blocks.ContainsKey(_entryLabel))
                return null;

            var ins = new Dictionary<string, DirtySet>(StringComparer.OrdinalIgnoreCase);
            var reached = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var block in _blocks.Values)
                foreach (var edge in block.Edges)
                    reached.Add(edge.Target);
            foreach (var label in _blocks.Keys)
                ins[label] = !string.Equals(label, _entryLabel, StringComparison.OrdinalIgnoreCase) && !reached.Contains(label)
                    ? all
                    : default;

            bool changed;
            do
            {
                changed = false;
                foreach (var (label, block) in _blocks)
                {
                    var output = block.Transfer.Apply(ins[label]);
                    foreach (var (target, edgeGen) in block.Edges)
                    {
                        if (!ins.TryGetValue(target, out var current)) continue;
                        var merged = DirtySet.Or(current, DirtySet.Or(output, edgeGen));
                        if (!merged.Equals(current))
                        {
                            ins[target] = merged;
                            changed = true;
                        }
                    }
                }
            }
            while (changed);

            return ins;
        }

        /// <summary>Starts a block: the running transfer is empty and, in the real pass, its entry state is the solved one.</summary>
        public void BeginBlock(string label)
        {
            _activeBlock = new BlockSummary();
            _edgeTarget = null;
            _alternatives.Clear();
            if (_recording)
            {
                _blocks![label] = _activeBlock;
                _activeIn = default;
            }
            else
            {
                _activeIn = _blockIn is not null && _blockIn.TryGetValue(label, out var known) ? known : WrittenSet();
            }
        }

        /// <summary>Writes made while a CFG edge's normalization is emitted belong to that edge only.</summary>
        public void BeginEdge(string target)
        {
            if (_activeBlock is null) return;
            _edgeTarget = target;
            _edgeGen = default;
        }

        public void EndEdge()
        {
            if (_activeBlock is null || _edgeTarget is null) return;
            if (_recording) _activeBlock.Edges.Add((_edgeTarget, _edgeGen));
            _edgeTarget = null;
        }

        /// <summary>
        /// Brackets code whose arms are alternatives within one block (a state-free call's fast and slow arm):
        /// each arm starts from the state at <see cref="BeginAlternatives"/>; afterwards the state is their join.
        /// </summary>
        public void BeginAlternatives()
        {
            if (_activeBlock is null) return;
            _alternatives.Push((_activeBlock.Transfer, new List<Transfer>()));
        }

        public void NextAlternative()
        {
            if (_activeBlock is null) return;
            var (baseTransfer, done) = _alternatives.Peek();
            done.Add(_activeBlock.Transfer);
            _activeBlock.Transfer = baseTransfer;
        }

        public void EndAlternatives()
        {
            if (_activeBlock is null) return;
            var (_, done) = _alternatives.Pop();
            var joined = _activeBlock.Transfer;
            foreach (var arm in done) joined = Transfer.Join(joined, arm);
            _activeBlock.Transfer = joined;
        }

        private DirtySet CurrentDirty()
        {
            if (_activeBlock is null || _conservative) return WrittenSet();
            return _activeBlock.Transfer.Apply(_activeIn);
        }

        private void NoteWrite(DirtySet written)
        {
            if (_activeBlock is null) return;
            if (_edgeTarget is not null) _edgeGen = DirtySet.Or(_edgeGen, written);
            else _activeBlock.Transfer.Write(written);
        }

        private void NoteClean(DirtySet cleaned)
        {
            if (_activeBlock is null || _edgeTarget is not null) return;
            _activeBlock.Transfer.Clean(cleaned);
        }

        public bool IsEmpty => _gprs.Count == 0 && _fprs.Count == 0 && !_cr && !_ctr && !_xer;

        public static string GprLocal(int index) => GprRegisterName(index);
        public static string FprLocal(int index) => FprRegisterName(index);
        public const string CrLocal = "cr";
        public const string CtrLocal = "ctr";
        public const string XerLocal = "xer";

        public IEnumerable<string> DeclaredLocalNames
        {
            get
            {
                foreach (var gpr in _gprs) yield return GprLocal(gpr);
                foreach (var fpr in _fprs) yield return FprLocal(fpr);
                if (_cr) yield return CrLocal;
                if (_ctr) yield return CtrLocal;
                if (_xer) yield return XerLocal;
            }
        }

        public string Gpr(int index, bool written)
        {
            Track(_gprs, _writtenGprs, index, written, "r");
            return GprLocal(index);
        }

        /// <summary>Scalar (PS0) view of a localized FPR.</summary>
        public string FprScalar(int index, bool written)
        {
            Track(_fprs, _writtenFprs, index, written, "f");
            return FprScalarLocalName(index);
        }

        /// <summary>Whole <c>PPC_FPR</c> value of a localized FPR.</summary>
        public string FprStorage(int index, bool written)
        {
            Track(_fprs, _writtenFprs, index, written, "f");
            return FprLocal(index);
        }

        public string Cr(bool written)
        {
            if (written) NoteWrite(new DirtySet { Flags = CrBit });
            if (_recording)
            {
                _cr = true;
                _writesCr |= written;
            }
            else if (!_cr || (written && !_writesCr))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize cr for this function body.");
            }

            return CrLocal;
        }

        public string Ctr(bool written)
        {
            if (written) NoteWrite(new DirtySet { Flags = CtrBit });
            if (_recording)
            {
                _ctr = true;
                _writesCtr |= written;
            }
            else if (!_ctr || (written && !_writesCtr))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize ctr for this function body.");
            }

            return CtrLocal;
        }

        /// <summary>
        /// XER, localized as one value (not just summary-overflow) because the carry helpers
        /// read/modify/write it and every integer compare reads XER.SO into its CR field.
        /// </summary>
        public string Xer(bool written)
        {
            if (written) NoteWrite(new DirtySet { Flags = XerBit });
            if (_recording)
            {
                _xer = true;
                _writesXer |= written;
            }
            else if (!_xer || (written && !_writesXer))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize xer for this function body.");
            }

            return XerLocal;
        }

        private void Track(SortedSet<int> referenced, SortedSet<int> written, int index, bool isWrite, string prefix)
        {
            if (isWrite)
            {
                var bit = 1u << index;
                NoteWrite(ReferenceEquals(referenced, _gprs) ? new DirtySet { Gpr = bit } : new DirtySet { Fpr = bit });
            }

            if (_recording)
            {
                referenced.Add(index);
                if (isWrite) written.Add(index);
                return;
            }

            if (!referenced.Contains(index) || (isWrite && !written.Contains(index)))
            {
                throw new InvalidOperationException(
                    $"Register residency did not localize {prefix}{index} for this function body.");
            }
        }

        /// <summary>
        /// Entry loads. Every referenced register is loaded unconditionally:
        /// liveness-narrowed initialization is a later optimization and reading
        /// an architectural register is always well defined.
        /// </summary>
        public void AppendEntryLoads(StringBuilder sb)
        {
            if (_recording) return;
            foreach (var gpr in _gprs)
                sb.Append("    uint32_t ").Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");
            foreach (var fpr in _fprs)
                sb.Append("    PPC_FPR ").Append(FprLocal(fpr)).Append(" = ctx->fpr[").Append(fpr).AppendLine("];");
            if (_cr) sb.AppendLine($"    uint32_t {CrLocal} = ctx->cr;");
            if (_ctr) sb.AppendLine($"    uint32_t {CtrLocal} = ctx->ctr;");
            if (_xer) sb.AppendLine($"    uint32_t {XerLocal} = ctx->xer;");
        }

        /// <summary>
        /// Stores the possibly-written localized registers back into
        /// <c>CpuContext</c>. Storing an unmodified register writes the same
        /// value back, so the conservative write set stays correct.
        /// </summary>
        public void AppendFlush(StringBuilder sb, string pad) => AppendFlush(sb, pad, ResidencyBoundarySync.Full);

        /// <summary>
        /// Contract-narrowed flush. Only the registers the boundary can observe
        /// or overwrite are published; see <see cref="ResidencyBoundarySync"/>
        /// for why the write set has to participate in the flush mask.
        /// </summary>
        public void AppendFlush(StringBuilder sb, string pad, ResidencyBoundarySync sync) =>
            AppendFlush(sb, pad, sync, leavesFrame: false);

        /// <summary>
        /// Writes back the registers that are both dirty and in the boundary's flush set. A flush that stays in
        /// this frame (<paramref name="leavesFrame"/> false) leaves them clean afterwards; one that returns or
        /// tail-dispatches must not change the state the sibling paths of the block see.
        /// </summary>
        public void AppendFlush(StringBuilder sb, string pad, ResidencyBoundarySync sync, bool leavesFrame)
        {
            var dirty = CurrentDirty();
            if (!_recording)
            {
                foreach (var gpr in _writtenGprs)
                    if (sync.FlushesGpr(gpr) && (dirty.Gpr & (1u << gpr)) != 0)
                        sb.Append(pad).Append("ctx->gpr[").Append(gpr).Append("] = ").Append(GprLocal(gpr)).AppendLine(";");
                foreach (var fpr in _writtenFprs)
                    if (sync.FlushesFpr(fpr) && (dirty.Fpr & (1u << fpr)) != 0)
                        sb.Append(pad).Append("ctx->fpr[").Append(fpr).Append("] = ").Append(FprLocal(fpr)).AppendLine(";");
                if (_writesCr && sync.FlushesCr && (dirty.Flags & CrBit) != 0) sb.Append(pad).AppendLine($"ctx->cr = {CrLocal};");
                if (_writesCtr && sync.FlushesCtr && (dirty.Flags & CtrBit) != 0) sb.Append(pad).AppendLine($"ctx->ctr = {CtrLocal};");
                if (_writesXer && sync.FlushesXer && (dirty.Flags & XerBit) != 0) sb.Append(pad).AppendLine($"ctx->xer = {XerLocal};");
            }

            if (leavesFrame) return;
            var flushed = new DirtySet { Gpr = sync.FlushGprMask, Fpr = sync.FlushFprMask };
            if (sync.FlushesCr) flushed.Flags |= CrBit;
            if (sync.FlushesCtr) flushed.Flags |= CtrBit;
            if (sync.FlushesXer) flushed.Flags |= XerBit;
            NoteClean(flushed);
        }

        /// <summary>
        /// Reloads the full localized set after a boundary returns, unconditionally: a callee can
        /// change any volatile register and a nonvolatile one too if it restores it itself.
        /// </summary>
        public void AppendReload(StringBuilder sb, string pad) => AppendReload(sb, pad, ResidencyBoundarySync.Full);

        /// <summary>Contract-narrowed reload.</summary>
        public void AppendReload(StringBuilder sb, string pad, ResidencyBoundarySync sync)
        {
            var reloaded = new DirtySet { Gpr = sync.ReloadGprMask, Fpr = sync.ReloadFprMask };
            if (sync.ReloadsCr) reloaded.Flags |= CrBit;
            if (sync.ReloadsCtr) reloaded.Flags |= CtrBit;
            if (sync.ReloadsXer) reloaded.Flags |= XerBit;
            NoteWrite(DirtySet.AndNot(CallbackClobberable, reloaded));
            NoteClean(reloaded);
            if (_recording) return;
            foreach (var gpr in _gprs)
                if (sync.ReloadsGpr(gpr))
                    sb.Append(pad).Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");
            foreach (var fpr in _fprs)
                if (sync.ReloadsFpr(fpr))
                    sb.Append(pad).Append(FprLocal(fpr)).Append(" = ctx->fpr[").Append(fpr).AppendLine("];");
            if (_cr && sync.ReloadsCr) sb.Append(pad).AppendLine($"{CrLocal} = ctx->cr;");
            if (_ctr && sync.ReloadsCtr) sb.Append(pad).AppendLine($"{CtrLocal} = ctx->ctr;");
            if (_xer && sync.ReloadsXer) sb.Append(pad).AppendLine($"{XerLocal} = ctx->xer;");
        }
    }

    /// <summary>
    /// Which localized registers one call boundary must sync. Flush mask is <c>read-before-write | possible-write</c>, not just callee reads, or an unflushed possibly-written register reloads stale.
    /// Reload mask is the callee's possible-write mask (transitive union, not net effect); safe even for a spill/restore, which just re-reads a value already restored to what this frame flushed.
    /// Falls back to <see cref="Full"/> without a precise contract (missing, full-fence, indirect, or CpuContext-touching marshalling).
    /// </summary>
    internal sealed record ResidencyBoundarySync(
        uint FlushGprMask,
        uint FlushFprMask,
        bool FlushesCr,
        bool FlushesCtr,
        uint ReloadGprMask,
        uint ReloadFprMask,
        bool ReloadsCr,
        bool ReloadsCtr,
        bool FlushesXer = false,
        bool ReloadsXer = false)
    {
        public static ResidencyBoundarySync Full { get; } = new(
            uint.MaxValue, uint.MaxValue, true, true,
            uint.MaxValue, uint.MaxValue, true, true,
            FlushesXer: true, ReloadsXer: true);

        /// <summary>
        /// Fast arm of a state-free call site (explicit-state ABI, locals carry args/results) still flushes r1 and XER unconditionally, since a guest gather-pipe store can trigger GX HLE mid-call servicing a VI retrace or OS alarm whose handlers read them architecturally.
        /// Flush-only; the slow arm (<c>InvokeDirectCpu</c>) keeps a full <see cref="Full"/> pair.
        /// </summary>
        public static ResidencyBoundarySync StateFreeResidentOuter { get; } = new(
            FlushGprMask: 1u << 1,
            FlushFprMask: 0u,
            FlushesCr: false,
            FlushesCtr: false,
            ReloadGprMask: 0u,
            ReloadFprMask: 0u,
            ReloadsCr: false,
            ReloadsCtr: false,
            FlushesXer: true,
            ReloadsXer: false);

        public bool FlushesGpr(int index) => (FlushGprMask & (1u << index)) != 0;
        public bool FlushesFpr(int index) => (FlushFprMask & (1u << index)) != 0;
        public bool ReloadsGpr(int index) => (ReloadGprMask & (1u << index)) != 0;
        public bool ReloadsFpr(int index) => (ReloadFprMask & (1u << index)) != 0;

        public bool IsFull =>
            FlushGprMask == uint.MaxValue && FlushFprMask == uint.MaxValue && FlushesCr && FlushesCtr &&
            ReloadGprMask == uint.MaxValue && ReloadFprMask == uint.MaxValue && ReloadsCr && ReloadsCtr &&
            FlushesXer && ReloadsXer;

        /// <summary>
        /// Adds a register class back to both masks, for call sites that emit their own architectural
        /// <c>CpuContext</c> traffic (paired-single return normalization) the callee contract omits.
        /// </summary>
        public ResidencyBoundarySync WithFpr(int index) => this with
        {
            FlushFprMask = FlushFprMask | (1u << index),
            ReloadFprMask = ReloadFprMask | (1u << index)
        };

        public bool IsEmpty =>
            FlushGprMask == 0 && FlushFprMask == 0 && !FlushesCr && !FlushesCtr &&
            ReloadGprMask == 0 && ReloadFprMask == 0 && !ReloadsCr && !ReloadsCtr &&
            !FlushesXer && !ReloadsXer;

        /// <summary>
        /// Boundary set for a non-guest helper's implicit <c>CpuContext</c> effects, from
        /// <see cref="Translator.Core.Analysis.GuestHelperEffectCatalog"/>. Explicit argument/destination registers
        /// need no sync; unknown helpers catalog as complete boundaries and arrive as full masks; a pure helper is empty.
        /// </summary>
        public static ResidencyBoundarySync FromHelperEffect(Translator.Core.Analysis.GuestHelperEffect effect) => new(
            FlushGprMask: effect.GprReadMask | effect.GprWriteMask,
            FlushFprMask: effect.FprReadMask | effect.FprWriteMask,
            FlushesCr: (effect.CrReadMask | effect.CrWriteMask) != 0,
            FlushesCtr: effect.ReadsCtr || effect.WritesCtr,
            ReloadGprMask: effect.GprWriteMask,
            ReloadFprMask: effect.FprWriteMask,
            ReloadsCr: effect.CrWriteMask != 0,
            ReloadsCtr: effect.WritesCtr,
            FlushesXer: effect.ReadsXer || effect.WritesXer,
            ReloadsXer: effect.WritesXer);

        /// <summary>
        /// r1 flushes unconditionally: typed native stubs derive their contract from a host signature and can't see the body re-enter guest code via HLE interrupt delivery, whose callback chains build frames from <c>ctx-&gt;gpr[1]</c>.
        /// A stale r1 once let such a callback overwrite a live frame's save area (return address landed in nw4r::ef's saved r29 at postVRetrace, 0x80210050, as a wild read). Flush-only; caller keeps its stack pointer.
        /// </summary>
        private const uint StackPointerFlushMask = 1u << 1;

        public static ResidencyBoundarySync FromCalleeContract(GuestAbiContract contract)
        {
            if (contract.HasFullSynchronizationFence) return Full;
            return new ResidencyBoundarySync(
                FlushGprMask: contract.GprReadBeforeWriteMask | contract.GprPossibleWriteMask |
                              StackPointerFlushMask,
                FlushFprMask: contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask,
                // The contract tracks CR per field while the local is the whole
                // packed register, so any intersecting field forces the sync.
                FlushesCr: (contract.CrReadBeforeWriteMask | contract.CrPossibleWriteMask) != 0,
                FlushesCtr: contract.ReadsCtrBeforeWrite || contract.MayWriteCtr,
                ReloadGprMask: contract.GprPossibleWriteMask,
                ReloadFprMask: contract.FprPossibleWriteMask,
                ReloadsCr: contract.CrPossibleWriteMask != 0,
                ReloadsCtr: contract.MayWriteCtr,
                // Flushed unconditionally for the same reason as r1: interrupt handlers read XER.SO out
                // of the architectural copy. Reload stays contract-driven so a handler can't clobber it.
                FlushesXer: true,
                ReloadsXer: contract.MayWriteXer);
        }
    }

    /// <summary>
    /// Ambient residency for the body being emitted. Thread-local, not an explicit parameter, since threading one through 30+ static formatting helpers risks a missed path silently reading stale <c>ctx-&gt;gpr[N]</c>; also keeps parallel per-function emission independent.
    /// </summary>
    [ThreadStatic]
    private static RegisterResidency? _activeResidency;

    private static void AppendFlush(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendFlush(sb, pad);
    }

    private static void AppendReload(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendReload(sb, pad);
    }

    private static void AppendFlush(StringBuilder sb, string pad, ResidencyBoundarySync sync)
    {
        if (_activeResidency is { } residency) residency.AppendFlush(sb, pad, sync);
    }

    /// <summary>Write-back on a path that leaves the frame (return, tail dispatch): publishes the dirty registers, tracks nothing.</summary>
    private static void AppendExitFlush(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendFlush(sb, pad, ResidencyBoundarySync.Full, leavesFrame: true);
    }

    private static void AppendReload(StringBuilder sb, string pad, ResidencyBoundarySync sync)
    {
        if (_activeResidency is { } residency) residency.AppendReload(sb, pad, sync);
    }

    /// <summary>
    /// Resolves the boundary sync set for a direct guest call, falling back to <see cref="ResidencyBoundarySync.Full"/> when narrowing is disabled, residency is inactive, the callee has no contract or a full-sync fence, or a mod could replace the callee with a re-translation this body's contract doesn't describe.
    /// </summary>
    private static ResidencyBoundarySync ResolveDirectCallBoundarySync(
        uint target,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets)
    {
        if (_activeResidency is null ||
            modOverridableCallTargets.Contains(target) ||
            !guestAbiContracts.TryGetValue(target, out var contract))
        {
            return ResidencyBoundarySync.Full;
        }

        return ResidencyBoundarySync.FromCalleeContract(contract);
    }

    /// <summary>
    /// Removes an explicit helper destination register from a helper-effect
    /// reload mask. The destination local holds the helper's return value; the
    /// implicit-context reload must not overwrite it.
    /// </summary>
    private static ResidencyBoundarySync ExcludeDestinationFromReload(
        ResidencyBoundarySync sync,
        string? destination)
    {
        if (string.IsNullOrWhiteSpace(destination))
        {
            return sync;
        }

        var baseName = GetRegisterBaseName(destination);
        if (baseName.Length > 1 && baseName[0] is 'r' or 'R' &&
            int.TryParse(baseName.AsSpan(1), out var gpr) && gpr is >= 0 and < 32)
        {
            return sync with { ReloadGprMask = sync.ReloadGprMask & ~(1u << gpr) };
        }

        if (baseName.Length > 1 && baseName[0] is 'f' or 'F' &&
            int.TryParse(baseName.AsSpan(1), out var fpr) && fpr is >= 0 and < 32)
        {
            return sync with { ReloadFprMask = sync.ReloadFprMask & ~(1u << fpr) };
        }

        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsCr = false };
        }

        if (baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsCtr = false };
        }

        if (baseName.Equals("xer", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsXer = false };
        }

        return sync;
    }

    /// <summary>
    /// Rejects a residency whose local names would shadow an emitted temporary. A guard against a
    /// future naming change, not an expected condition, since <see cref="IsCpuRegisterNoLocalNeeded"/>
    /// already keeps machine register names out of the ordinary local set.
    /// </summary>
    private static void RequireResidencyNamesAreFree(
        RegisterResidency residency,
        uint entryPoint,
        IEnumerable<string> emittedLocalNames)
    {
        var taken = new HashSet<string>(emittedLocalNames, StringComparer.Ordinal);
        var collisions = residency.DeclaredLocalNames.Where(taken.Contains).ToArray();
        if (collisions.Length != 0)
        {
            throw new InvalidOperationException(
                $"Register residency local name(s) {string.Join(", ", collisions)} collide with an existing " +
                $"local in 0x{entryPoint:X8}.");
        }
    }
}
