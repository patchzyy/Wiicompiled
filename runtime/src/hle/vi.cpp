#include "hle_stubs.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "runtime_log.h"

#include <dolphin/vi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

#include <aurora/aurora.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = MKW_GADDR(80386b38);
constexpr uint32_t kViTvFormatAddr          = MKW_GADDR(80386ba8);
constexpr uint32_t kViRenderWidthAddr       = MKW_GADDR(80350864);
constexpr uint32_t kViRenderHeightAddr      = MKW_GADDR(80350866);
constexpr uint32_t kViXfbWidthAddr          = MKW_GADDR(80350872);
constexpr uint32_t kViXfbHeightAddr         = MKW_GADDR(8035087c);
constexpr uint32_t kViRetraceCountAddr      = MKW_GADDR(80386be4); // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = MKW_GADDR(80386b44);
constexpr uint32_t kViPreRetraceCallback    = MKW_GADDR(80386bb8);
constexpr uint32_t kViPostRetraceCallback   = MKW_GADDR(80386bb4);
constexpr uint32_t kViNextFrameBufferAddr   = MKW_GADDR(80386ba0);
constexpr uint32_t kViNextFrameBufferHwAddr = MKW_GADDR(80350890);
constexpr uint32_t kViRetraceQueueAddr      = MKW_GADDR(80386bc0); // Thread queue for VIWaitForRetrace

// EGG::BaseSystem::sSystem pointer - must be non-null before post-retrace callback is valid
constexpr uint32_t kEggSSystemAddr = MKW_GADDR(80386F60);

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        Memory::Write8(kViTimingGuardAddr, 1);
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }
    
    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        retraceValue = g_vi.retraceCount;
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
    if (ctx) {
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801aaaa4(ctx);
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Guard: only invoke callback if sSystem is initialized
            // The callback dereferences sSystem which must be non-null
            uint32_t sSystemPtr = Memory::Read32(kEggSSystemAddr);
            if (sSystemPtr != 0) {
                InvokeIndirectCpu(postCb, ctx);
            }
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!isBlack || settings_overlay::StartupScreenVisible()) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + g_vi.retraceInterval;
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + g_vi.retraceInterval;
    }
    AdvanceRetrace(ctx, target, true);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)
                    .count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        aurora_report_producer_paced(retracesElapsed <= 1);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
        // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
        uint64_t anchorNanos = baseNanos + intervalNanos;
        if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
            anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
        }
        s_lastPresentAnchorNanos = anchorNanos;
        aurora_set_present_schedule(anchorNanos, intervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    aurora_end_frame();
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    settings_overlay::AdvancePresentedFrame();
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801B94A4) and its lower-level helper __VIInit (0x801B9294) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(801B94A4, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(801B9294, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9934, VIInit_caseD_0_HLE_801b9934, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B993C, VIInit_caseD_1_HLE_801b993c, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9944, VIInit_caseD_2_HLE_801b9944, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801B90F4)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B90F4, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801B9138)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B9138, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BAD38)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(801BAD38, VIGetDTVStatus_HLE_801bad38, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B9F6C, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BA9A4, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAAB8, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB24, VIGetNextFrameBuffer_HLE_801bab24, (CpuContext* ctx), (ctx));

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB2C, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABA4, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABAC, VIGetNextField_HLE_801babac, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::microseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = g_vi.retraceInterval;
        last = g_vi.lastRetrace;
    }
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / interval.count()));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(801BAC48, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            OSSleepThread_HLE_801aa9b8(cpu);

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::microseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = g_vi.retraceInterval;
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B99EC, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));
