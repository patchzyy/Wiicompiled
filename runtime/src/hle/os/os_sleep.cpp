// OSSleepTicks / OSSleepThread parking plus the host-side sleep-timer table.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "timebase_contract.h"
#include "runtime_log.h"
#include "os_internal.h"

namespace OsHleInternal {
std::mutex gSleepTimerMutex;
std::vector<SleepTimerEntry> gSleepTimers;

void CancelSleepTimer(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    std::erase_if(gSleepTimers, [threadPtr](const SleepTimerEntry& entry) {
        return entry.threadPtr == threadPtr;
    });
}

bool SleepTimerIsPending(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    for (const SleepTimerEntry& entry : gSleepTimers) {
        if (entry.threadPtr == threadPtr) {
            return true;
        }
    }
    return false;
}

// Threads with an outstanding OSSleepTicks park (timer armed, wake not yet delivered). If one ends
// up park-shaped with no pending timer, the reconciler in ProcessSleepTimers heals it; nothing
// outside this set is ever force-started.
std::mutex gOutstandingParkMutex;
std::unordered_set<uint32_t> gOutstandingParks;

void MarkParkOutstanding(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.insert(threadPtr);
}

void ClearOutstandingPark(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.erase(threadPtr);
}

void ScheduleSleepTimer(uint32_t threadPtr, uint64_t ticks)
{
    using Clock = std::chrono::steady_clock;

    const auto duration = TimeBaseContract::TicksToDuration(ticks);
    const auto deadline = Clock::now() + duration;

    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    std::erase_if(gSleepTimers, [threadPtr](const SleepTimerEntry& entry) {
        return entry.threadPtr == threadPtr;
    });
    gSleepTimers.push_back({threadPtr, deadline});
}

bool ProcessSleepTimers(CpuContext* cpu)
{
    using Clock = std::chrono::steady_clock;

    std::vector<SleepTimerEntry> dueTimers;
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(gSleepTimerMutex);
        auto it = gSleepTimers.begin();
        while (it != gSleepTimers.end()) {
            if (it->deadline > now) {
                ++it;
                continue;
            }
            dueTimers.push_back(*it);
            it = gSleepTimers.erase(it);
        }
    }

    for (const SleepTimerEntry& timer : dueTimers) {
        const uint32_t threadPtr = timer.threadPtr;
        if (threadPtr == 0 ||
            !Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t))) {
            continue;
        }

        // A sleep park always leaves the thread Ready with a suspension. A Running thread with a due
        // timer is mid-park (fire raced the self-suspend), so defer instead of consuming it there or
        // the sleeper strands. Any other shape means the sleeper already woke; drop the stale timer
        // without touching the thread.
        const uint16_t stateAtFire = Memory::Read16(threadPtr + kThreadStateOffset);
        const int32_t countAtFire =
            static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
        if (stateAtFire == kThreadStateRunning) {
            std::lock_guard<std::mutex> lock(gSleepTimerMutex);
            bool present = false;
            for (const SleepTimerEntry& entry : gSleepTimers) {
                if (entry.threadPtr == threadPtr) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                gSleepTimers.push_back({threadPtr, now + std::chrono::milliseconds(2)});
            }
            continue;
        }
        if (stateAtFire != kThreadStateReady || countAtFire < 1) {
            RT_LOG(RT_TAG_OS) << "sleep-timer stale for thread 0x" << std::hex << threadPtr
                      << std::dec << " (state=" << stateAtFire << " suspends=" << countAtFire
                      << "); dropped without consuming a suspension" << std::endl;
            ClearOutstandingPark(threadPtr);
            continue;
        }

        ClearOutstandingPark(threadPtr);
        cpu->gpr[3] = threadPtr;
        OSResumeThread_HLE_801aa58c(cpu);

        // OSResumeThread only peels one suspension level, so if the sleeper's count is inflated
        // (failed park, overlapping suspend) dropping the timer here would strand it. Keep it armed
        // instead of consuming it; remaining suspensions after the park's own are foreign ones woken
        // by their own OSResumeThread. Rearming here used to steal those foreign suspensions, which
        // is how the DWC re-init thread got stranded.
    }

    // Stranded-sleeper reconciler: heals a thread whose park has no pending wake timer (lost to a
    // race) by resuming it once that shape persists for 100ms, sampled every 50ms so no strand is
    // missed. The atomic CAS below lets exactly one caller run the scan when ProcessSleepTimers
    // executes on more than one host thread; the rest skip it.
    static std::atomic<Clock::rep> strandScanDueAt{0};
    auto strandScanClaim = strandScanDueAt.load(std::memory_order_relaxed);
    const bool runStrandReconciler =
        now.time_since_epoch().count() >= strandScanClaim &&
        strandScanDueAt.compare_exchange_strong(
            strandScanClaim,
            static_cast<Clock::rep>(
                (now + std::chrono::milliseconds(50)).time_since_epoch().count()),
            std::memory_order_relaxed);
    if (runStrandReconciler) {
        static std::mutex strandMutex;
        static std::unordered_map<uint32_t, Clock::time_point> strandFirstSeen;

        std::vector<uint32_t> outstanding;
        {
            std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
            outstanding.assign(gOutstandingParks.begin(), gOutstandingParks.end());
        }
        // Decide who to heal under the lock, but resume OUTSIDE it: the resume
        // re-enters the scheduler and thus this function, and holding the lock
        // across it is a guaranteed self-deadlock.
        std::vector<uint32_t> toHeal;
        {
            std::lock_guard<std::mutex> strandLock(strandMutex);
            // Drop debounce records for threads whose park was satisfied elsewhere.
            for (auto it = strandFirstSeen.begin(); it != strandFirstSeen.end();) {
                if (std::find(outstanding.begin(), outstanding.end(), it->first) ==
                    outstanding.end()) {
                    it = strandFirstSeen.erase(it);
                } else {
                    ++it;
                }
            }
            const auto reconcileNow = Clock::now();
            for (const uint32_t threadPtr : outstanding) {
                if (SleepTimerIsPending(threadPtr)) {
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                if (!Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t)) ||
                    Fiber::GuestFiberManager::IsTerminated(threadPtr)) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const uint16_t state = Memory::Read16(threadPtr + kThreadStateOffset);
                const int32_t count =
                    static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
                if (state != kThreadStateReady || count < 1) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const auto seen = strandFirstSeen.find(threadPtr);
                if (seen == strandFirstSeen.end()) {
                    strandFirstSeen.emplace(threadPtr, reconcileNow);
                    continue;
                }
                if (reconcileNow - seen->second < std::chrono::milliseconds(100)) {
                    continue;
                }
                strandFirstSeen.erase(seen);
                ClearOutstandingPark(threadPtr);
                toHeal.push_back(threadPtr);
            }
        }
        for (const uint32_t threadPtr : toHeal) {
            RT_LOG(RT_TAG_OS) << "reconciler: thread 0x" << std::hex << threadPtr
                      << std::dec << " park-shaped with no pending wake timer for 100ms; "
                      << "resuming lost sleeper" << std::endl;
            cpu->gpr[3] = threadPtr;
            OSResumeThread_HLE_801aa58c(cpu);
        }
    }

    return !dueTimers.empty();
}
} // namespace OsHleInternal

// ---------------------------------------------------------------------------
// OSSleepTicks HLE - mirror the SDK behavior by suspending the current guest
// thread and resuming it from a host-side timer. A host sleep here starves
// lower-priority guest workers because the scheduler never gets control.
// ---------------------------------------------------------------------------
extern "C" void OS__SleepTicks_HLE_801aaca8(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }

    const uint64_t ticks = (static_cast<uint64_t>(cpu->gpr[3]) << 32) | cpu->gpr[4];
    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        if (Fiber::GuestFiberManager::IsInitialized()) {
            const uint32_t fiberThread = Fiber::GuestFiberManager::GetCurrentGuestThread();
            if (fiberThread != 0) {
                currentThread = fiberThread;
            }
        }
        if (currentThread == 0) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        // Parking here is OSSuspendThread plus a one-shot timer that can only peel one suspension.
        // If the scheduler cannot switch away (OSDisableScheduler nesting raised around a deferred
        // guest-callback batch), suspending would inflate the count with nothing left to zero it,
        // parking forever (the silent DWC SOStartupEx hang). Mirror OSSleepThread: refuse to park
        // and let the caller retry.
        const uint32_t sleepIdleFlag = ::Memory::Read32(kSchedulerIdleFlagAddr);
        const uint32_t sleepCurrentContext = ::Memory::Read32(kOSCurrentContextAddr);
        if (sleepIdleFlag != 0 ||
            (sleepCurrentContext != 0 && sleepCurrentContext != currentThread)) {
            thread_local std::unordered_set<uint32_t> reportedUnparkableTickSleeps;
            if (reportedUnparkableTickSleeps.insert(currentThread).second) {
                RT_LOG(RT_TAG_OS) << "OSSleepTicks: thread 0x" << std::hex << currentThread
                          << std::dec << " cannot park (scheduler nesting "
                          << sleepIdleFlag << "); busy-returning so the caller retries."
                          << std::endl;
            }
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        ScheduleSleepTimer(currentThread, ticks);
        MarkParkOutstanding(currentThread);
        cpu->gpr[3] = currentThread;
        OSSuspendThread_HLE_801aa6a8(cpu);

        // Defence in depth: SelectThread has other refusal paths (context mismatch, drained run
        // queues), and there's a fire-before-park race where the timer fires and its resume is
        // consumed before we suspend. Either way we're still executing with a positive suspension
        // count, so revert it fully instead of leaving an inflated counter or a stale timer that
        // could half-wake a future sleep.
        const bool timerStillPending = SleepTimerIsPending(currentThread);
        const int32_t suspendCount =
            static_cast<int32_t>(::Memory::Read32(currentThread + kThreadSuspendOffset));
        if (suspendCount > 0) {
            CancelSleepTimer(currentThread);
            ClearOutstandingPark(currentThread);
            ::Memory::Write32(currentThread + kThreadSuspendOffset,
                              static_cast<uint32_t>(suspendCount - 1));
            if (suspendCount == 1) {
                if (::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateReady &&
                    ::Memory::Read32(currentThread + kThreadQueueOffset) == 0) {
                    ::Memory::Write16(currentThread + kThreadStateOffset, kThreadStateRunning);
                }
                if (Fiber::GuestFiberManager::IsInitialized() &&
                    Fiber::GuestFiberManager::HasFiber(currentThread)) {
                    Fiber::GuestFiberManager::ResumeGuestThread(currentThread);
                }
            }
            thread_local std::unordered_set<uint32_t> reportedFailedTickParks;
            if (reportedFailedTickParks.insert(currentThread).second) {
                RT_LOG(RT_TAG_OS) << "OSSleepTicks: thread 0x" << std::hex << currentThread
                          << std::dec << " failed to switch away while parking; "
                          << "reverted the suspension and busy-returned." << std::endl;
            }
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSleepTicks", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}
REGISTER_NATIVE_FUNCTION(MKW_GADDR(801AACA8), OS__SleepTicks_HLE_801aaca8);

namespace {
// OSSleepThread must reach SelectThread with the scheduler-disable count at zero, or the thread
// relinks into the wait queue a second time and OSWakeupThread livelocks. The SDK guarantees this;
// deferred guest-callback batches (VI retrace, alarms) can raise the count and violate it.
bool SchedulerCanSwitchAway()
{
    return ::Memory::Read32(kSchedulerIdleFlagAddr) == 0;
}

// A blocking call from a non-switchable region can't park, so the caller's retry loop spins; that's
// survivable if host-side work (alarm, IOS completion) eventually satisfies the wait, terminal if
// not. Report once per wait queue and escalate if a site stays wedged long enough to read as a freeze.
void ReportUnparkableSleep(uint32_t queuePtr, uint32_t thread)
{
    using Clock = std::chrono::steady_clock;

    // thread_local rather than static: guest scheduling normally runs on one
    // host thread, but this is reachable from the host frame loop too and a
    // diagnostic must not be the thing that introduces a data race.
    thread_local std::unordered_set<uint32_t> reportedQueues;
    thread_local uint32_t wedgedQueue = 0;
    thread_local Clock::time_point wedgedSince{};
    thread_local bool escalated = false;

    const uint32_t disableCount = ::Memory::Read32(kSchedulerIdleFlagAddr);

    if (reportedQueues.insert(queuePtr).second) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: thread 0x" << std::hex << thread
                  << " tried to block on wait queue 0x" << queuePtr
                  << std::dec << " but the scheduler could not switch away";
        if (disableCount != 0) {
            std::cerr << " (OSDisableScheduler nesting count " << disableCount << ")";
        }
        std::cerr << ". Not parking - the caller retries instead of corrupting"
                     " the wait queue." << std::endl;
    }

    const auto now = Clock::now();
    if (queuePtr != wedgedQueue) {
        wedgedQueue = queuePtr;
        wedgedSince = now;
        escalated = false;
        return;
    }

    constexpr auto kWedgeThreshold = std::chrono::seconds(5);
    if (!escalated && now - wedgedSince >= kWedgeThreshold) {
        escalated = true;
        RT_LOG(RT_TAG_OS) << "OSSleepThread: wait queue 0x" << std::hex << queuePtr
                  << " has been unsatisfiable for " << std::dec
                  << std::chrono::duration_cast<std::chrono::seconds>(now - wedgedSince).count()
                  << "s from a non-switchable region. The guest callback that is"
                     " blocking here must not block, or the deferred-callback"
                     " scope around it is wrong." << std::endl;
    }
}
} // namespace

// OSSleepThread (0x801aa9b8)
// Puts the current thread to sleep on a specified wait queue.
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t queuePtr = cpu->gpr[3];
    
    if (queuePtr == 0) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: null queue pointer!" << std::endl;
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    
    try {
        uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        
        // If no current thread is set but we have a default thread, use that
        // This can happen during early boot before the scheduler has switched threads
        if (currentThread == 0) {
            currentThread = kDefaultThreadContextAddr;
            if (!Memory::Contains(currentThread, 4)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: no current thread and default thread not valid!" << std::endl;
                OS__RestoreInterrupts_801a65d4(irqState);
                return;
            }
            // Set the default thread as current (both running and context)
            ::Memory::Write32(kOSRunningContextAddr, currentThread);
            OS__SetCurrentContext_801a1e70(currentThread);
            
            // Also register as a fiber if not already
            if (Fiber::GuestFiberManager::IsInitialized() && 
                !Fiber::GuestFiberManager::HasFiber(currentThread)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: registering default thread 0x"
                          << std::hex << currentThread << " as fiber" << std::dec << std::endl;
                Fiber::GuestFiberManager::RegisterMainThreadAsFiber(currentThread, cpu);
            }
        }
        
        // See SchedulerCanSwitchAway: parking is only safe when SelectThread is
        // permitted to switch. Otherwise leave the thread RUNNING and the wait
        // queue untouched and let the caller's retry loop re-test its condition.
        if (!SchedulerCanSwitchAway()) {
            ReportUnparkableSleep(queuePtr, currentThread);
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        // Mark thread as WAITING
        ::Memory::Write16(currentThread + 0x2C8u, 4);

        // Store the queue we're waiting on
        ::Memory::Write32(currentThread + 0x2DCu, queuePtr);

        // Insert into wait queue (sorted by priority). The shared helper also
        // re-writes the queue pointer stored above and, for a run-queue
        // address, publishes the pending bit - neither applies to a wait queue.
        const int32_t ourPrio = static_cast<int32_t>(::Memory::Read32(currentThread + 0x2D0u));
        InsertThreadIntoQueueByPriority(queuePtr, currentThread, ourPrio);

        // Mark fiber as waiting
        if (Fiber::GuestFiberManager::IsInitialized()) {
            Fiber::GuestFiberManager::SuspendGuestThread(currentThread);
        }

        // Set reschedule flag and switch threads
        ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        
        // Match the original SDK behavior: sleep yields via SelectThread(0).
        cpu->gpr[3] = 0;
        SelectThread_801a9c08(cpu);

        // Defence in depth: SelectThread has other paths that return without switching (context
        // mismatch, uninitialised thread system, a run queue that drained mid-enqueue). None of
        // them may leave this thread WAITING and linked, or the caller's retry loop inserts the same
        // node twice; a real switch resumes here already RUNNING with the queue pointer cleared, so
        // this block never fires there.
        if (::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateWaiting &&
            ::Memory::Read32(currentThread + kThreadQueueOffset) == queuePtr) {
            // Reverse of the priority-sorted insert above, so a thread is never
            // left WAITING and linked while its caller keeps running on the
            // same stack.
            UnlinkGuestListNode(currentThread, kThreadNextOffset, kThreadPrevOffset, queuePtr,
                                queuePtr + 4u);
            ::Memory::Write32(currentThread + kThreadQueueOffset, 0);
            ::Memory::Write16(currentThread + kThreadStateOffset, kThreadStateRunning);
            if (Fiber::GuestFiberManager::IsInitialized() &&
                Fiber::GuestFiberManager::HasFiber(currentThread)) {
                Fiber::GuestFiberManager::ResumeGuestThread(currentThread);
            }
            ReportUnparkableSleep(queuePtr, currentThread);
        }

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSleepThread", e);
    }
    
    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA9B8, OSSleepThread_HLE_801aa9b8, (CpuContext* ctx), (ctx));
