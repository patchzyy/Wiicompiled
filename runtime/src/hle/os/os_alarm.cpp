// OSAlarm queue HLE.

#include <cstdint>
#include <iostream>
#include <mutex>

#include "abi_bridge.h"
#include "memory.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "hle/net/network.h"
#include "generated/RuntimeConfig.h"
#include "os_internal.h"

extern "C" void MKW_GUEST_FUNC(801AADE0)(CpuContext* ctx);
extern "C" void MKW_GUEST_FUNC(801A0620)(CpuContext* ctx);

// ============================================================================
// Alarm queue helpers
// ============================================================================
extern bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess);

namespace {
// OSAlarm link layout matches InsertAlarm in the original RVL OS:
//   0x10 = prev
//   0x14 = next
constexpr uint32_t kAlarmPrevOffset = 0x10u;
constexpr uint32_t kAlarmNextOffset = 0x14u;
thread_local int g_alarmProcessDepth = 0;

struct AlarmProcessScope {
    AlarmProcessScope() { ++g_alarmProcessDepth; }
    ~AlarmProcessScope() { --g_alarmProcessDepth; }
};

// Translated code can leave r13 pointing somewhere else, and the alarm queue is
// addressed relative to the SDA1 base, so restore it before touching the queue.
// `logOnce` fires the one-shot notice OS_HLE_ProcessAlarms has always emitted;
// the other call sites have never logged.
void EnsureSda1Base(CpuContext* cpu, bool logOnce = false)
{
    if (cpu->gpr[13] == RuntimeConfig::SDA1_BASE) {
        return;
    }

    if (logOnce) {
        static std::once_flag r13_fix_once;
        std::call_once(r13_fix_once, []() {
            RT_LOG(RT_TAG_OS) << "OS_HLE_ProcessAlarms: fixing r13 to SDA1 base" << std::endl;
        });
    }
    cpu->gpr[13] = RuntimeConfig::SDA1_BASE;
}

void IncrementSchedulerDisableCount()
{
    const uint32_t count = ::Memory::Read32(kSchedulerIdleFlagAddr);
    ::Memory::Write32(kSchedulerIdleFlagAddr, count + 1u);
}

void DecrementSchedulerDisableCount()
{
    const uint32_t count = ::Memory::Read32(kSchedulerIdleFlagAddr);
    if (count == 0) {
        RT_LOG(RT_TAG_OS) << "scheduler disable count underflow during alarm callback" << std::endl;
        return;
    }
    ::Memory::Write32(kSchedulerIdleFlagAddr, count - 1u);
}

void RunDeferredReschedule(CpuContext* cpu)
{
    if (!cpu) {
        return;
    }

    // AdvanceRetrace owns renderer/VI host state across guest callbacks. A
    // completion may make a thread READY here, but switching fibers before the
    // retrace frame unwinds would let unrelated guest work run under the live
    // retrace guard. Leave the pending bit for the next safe scheduler point,
    // matching OSWakeupThread's established behavior below.
    if (VI_HLE_IsAdvancingRetrace()) {
        return;
    }

    if (::Memory::Read32(kSchedulerReschedCounterAddr) == 0) {
        return;
    }

    cpu->gpr[3] = 0;
    SelectThread_801a9c08(cpu);
}

void SanitizeAlarmQueue(CpuContext* cpu)
{
    if (!cpu) {
        return;
    }

    constexpr uint32_t queueBase = kAlarmQueueAddr;
    try {
        const uint32_t head = ::Memory::Read32(queueBase);
        const uint32_t tail = ::Memory::Read32(queueBase + 4u);
        if (head == 0) {
            return;
        }

        const uint32_t prev = ::Memory::Read32(head + kAlarmPrevOffset);
        const uint32_t next = ::Memory::Read32(head + kAlarmNextOffset);

        // Some builds initialize the alarm list as a self-referential node.
        // Treat that as an empty list so InsertAlarm doesn't spin forever.
        if (next == head && prev == head && head == tail) {
            RT_LOG(RT_TAG_OS) << "Alarm queue sentinel detected; resetting to empty before InsertAlarm" << std::endl;
            ::Memory::Write32(queueBase, 0);
            ::Memory::Write32(queueBase + 4u, 0);
            ::Memory::Write32(head + kAlarmPrevOffset, 0);
            ::Memory::Write32(head + kAlarmNextOffset, 0);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "SanitizeAlarmQueue", e);
    }
}

constexpr uint32_t kAlarmHandlerOffset = 0x00u;
constexpr uint32_t kAlarmFireTimeHiOffset = 0x08u;
constexpr uint32_t kAlarmFireTimeLoOffset = 0x0Cu;
constexpr uint32_t kAlarmPeriodHiOffset = 0x18u;
constexpr uint32_t kAlarmPeriodLoOffset = 0x1Cu;

bool IsLikelyCodeAddress(uint32_t addr)
{
    if (addr == 0) {
        return false;
    }
    if (TranslatedFunctionRegistry::FindByAddressPtr(addr)) {
        return true;
    }
    return Memory::Contains(addr, 4);
}
} // namespace

namespace OsHleInternal {
bool ProcessAlarmQueue(CpuContext* cpu, int maxToProcess)
{
    if (!cpu || maxToProcess <= 0) {
        return false;
    }

    if (g_alarmProcessDepth != 0) {
        return false;
    }

    EnsureSda1Base(cpu);

    if (cpu->gpr[13] == 0) {
        return false;
    }

    bool handledAny = false;

    // Keep the alarm recursion guard scoped to alarm dispatch itself. IOS
    // completion may wake and immediately switch into a guest fiber; carrying
    // g_alarmProcessDepth across that switch would make the resumed fiber skip
    // all alarm/network/callback pumping until this host frame ran again.
    {
        AlarmProcessScope alarmProcessScope;
        SanitizeAlarmQueue(cpu);
        constexpr uint32_t queueBase = kAlarmQueueAddr;

        try {
            for (int i = 0; i < maxToProcess; ++i) {
                const uint32_t alarm = ::Memory::Read32(queueBase);
                if (alarm == 0) {
                    break;
                }

                const uint64_t now = ReadSystemTime();
                const uint32_t fireHi = ::Memory::Read32(alarm + kAlarmFireTimeHiOffset);
                const uint32_t fireLo = ::Memory::Read32(alarm + kAlarmFireTimeLoOffset);
                const uint64_t fireTime = (static_cast<uint64_t>(fireHi) << 32) | fireLo;
                const uint32_t handler = ::Memory::Read32(alarm + kAlarmHandlerOffset);
                if (now < fireTime) {
                    break;
                }

                handledAny = true;

                const uint32_t next = ::Memory::Read32(alarm + kAlarmNextOffset);

                ::Memory::Write32(queueBase, next);
                if (next == 0) {
                    ::Memory::Write32(queueBase + 4u, 0);
                } else {
                    ::Memory::Write32(next + kAlarmPrevOffset, 0);
                }

                ::Memory::Write32(alarm + kAlarmNextOffset, 0);
                ::Memory::Write32(alarm + kAlarmPrevOffset, 0);
                ::Memory::Write32(alarm + kAlarmHandlerOffset, 0);

                const uint32_t periodHi = ::Memory::Read32(alarm + kAlarmPeriodHiOffset);
                const uint32_t periodLo = ::Memory::Read32(alarm + kAlarmPeriodLoOffset);
                if ((periodHi | periodLo) != 0) {
                    cpu->gpr[3] = alarm;
                    cpu->gpr[5] = 0;
                    cpu->gpr[6] = 0;
                    cpu->gpr[7] = handler;
                    MKW_GUEST_FUNC(801A0620)(cpu);
                }

                if (handler != 0) {
                    IncrementSchedulerDisableCount();
                    try {
                        cpu->gpr[3] = alarm;
                        cpu->gpr[4] = ::Memory::Read32(kOSCurrentContextAddr);
                        InvokeIndirectCpu(handler, cpu);
                    } catch (...) {
                        DecrementSchedulerDisableCount();
                        throw;
                    }
                    DecrementSchedulerDisableCount();
                    RunDeferredReschedule(cpu);
                }
            }
        } catch (const ::Memory::AccessViolation& e) {
            LogMemoryError(RT_TAG_OS, "ProcessAlarmQueue", e);
        }
    }

    // Host DNS workers never touch guest memory. Commit their output here on
    // the scheduler thread, waking synchronous IOS waiters or queuing async IOS
    // callbacks before the callback drain below.
    bool completionNeedsReschedule = false;
    if (Network_HLE_ProcessCompletions(cpu)) {
        handledAny = true;
        completionNeedsReschedule = true;
    }

    if (NandProcessPendingCallbacks(cpu, maxToProcess)) {
        handledAny = true;
        completionNeedsReschedule = true;
    }
    if (completionNeedsReschedule) {
        RunDeferredReschedule(cpu);
    }

    return handledAny;
}
} // namespace OsHleInternal

extern "C" void OSSetAlarm_HLE_801a0870(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }

    const uint32_t alarm = cpu->gpr[3];
    uint32_t tickHi = cpu->gpr[5];
    uint32_t tickLo = cpu->gpr[6];
    uint32_t handler = cpu->gpr[7];

    const uint32_t altTickHi = cpu->gpr[4];
    const uint32_t altTickLo = cpu->gpr[5];
    const uint32_t altHandler = cpu->gpr[6];

    // Log all OSSetAlarm calls for debugging RFL alarm issues

    if (!IsLikelyCodeAddress(handler) && IsLikelyCodeAddress(altHandler)) {
        handler = altHandler;
        tickHi = altTickHi;
        tickLo = altTickLo;
    }

    if (alarm == 0) {
        return;
    }

    const int32_t level = OS__DisableInterrupts_801a65ac();
    SanitizeAlarmQueue(cpu);

    uint64_t now = 0;
    try {
        now = ReadSystemTime();
    } catch (const ::Memory::AccessViolation&) {
        now = (static_cast<uint64_t>(PPC_Mftbu()) << 32) | PPC_Mftb();
    }

    const uint64_t offset = (static_cast<uint64_t>(tickHi) << 32) | tickLo;
    const uint64_t fireTime = now + offset;

    try {
        ::Memory::Write32(alarm + kAlarmPeriodLoOffset, 0);
        ::Memory::Write32(alarm + kAlarmPeriodHiOffset, 0);
        ::Memory::Write32(alarm + kAlarmHandlerOffset, handler);
        ::Memory::Write32(alarm + kAlarmFireTimeHiOffset, static_cast<uint32_t>(fireTime >> 32));
        ::Memory::Write32(alarm + kAlarmFireTimeLoOffset, static_cast<uint32_t>(fireTime & 0xFFFFFFFFu));
        ::Memory::Write32(alarm + kAlarmNextOffset, 0);
        ::Memory::Write32(alarm + kAlarmPrevOffset, 0);
    } catch (const ::Memory::AccessViolation&) {
        OS__RestoreInterrupts_801a65d4(level);
        return;
    }

    constexpr uint32_t queueBase = kAlarmQueueAddr;
    try {
        const uint32_t head = ::Memory::Read32(queueBase);
        if (head == 0) {
            ::Memory::Write32(queueBase, alarm);
            ::Memory::Write32(queueBase + 4u, alarm);
        } else {
            uint32_t cur = head;
            uint32_t prev = 0;
              while (cur != 0) {
                  const uint32_t curHi = ::Memory::Read32(cur + kAlarmFireTimeHiOffset);
                  const uint32_t curLo = ::Memory::Read32(cur + kAlarmFireTimeLoOffset);
                  const uint64_t curFire = (static_cast<uint64_t>(curHi) << 32) | curLo;
                  if (static_cast<int64_t>(fireTime) < static_cast<int64_t>(curFire)) {
                      break;
                  }
                  prev = cur;
                  cur = ::Memory::Read32(cur + kAlarmNextOffset);
              }


            if (prev == 0) {
                ::Memory::Write32(alarm + kAlarmNextOffset, head);
                ::Memory::Write32(head + kAlarmPrevOffset, alarm);
                ::Memory::Write32(queueBase, alarm);
            } else {
                ::Memory::Write32(alarm + kAlarmPrevOffset, prev);
                ::Memory::Write32(alarm + kAlarmNextOffset, cur);
                ::Memory::Write32(prev + kAlarmNextOffset, alarm);
                if (cur != 0) {
                    ::Memory::Write32(cur + kAlarmPrevOffset, alarm);
                } else {
                    ::Memory::Write32(queueBase + 4u, alarm);
                }
            }
        }
    } catch (const ::Memory::AccessViolation&) {
    }

    OS__RestoreInterrupts_801a65d4(level);
}
PPC_NATIVE_OVERRIDE_VOID(801A0870, OSSetAlarm_HLE_801a0870, (CpuContext* ctx), (ctx));

extern "C" void OS_HLE_ProcessAlarms(int maxToProcess)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    // R13 should always point at the SDA base; force it if translated code clobbered it.
    EnsureSda1Base(cpu, true);
    ProcessAlarmQueue(cpu, maxToProcess);
}

extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess)
{
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled()) {
        return;
    }

    // Alarm handlers reached from here interrupt an arbitrary translated
    // function (GX__Begin's timing service, the host frame loop), so they run
    // on a private copy of that function's register file the way a decrementer
    // interrupt would. Guest memory and the alarm queue stay shared.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();
    EnsureSda1Base(cpu);

    // Host renderer waits are not guest critical sections: on hardware, alarm
    // interrupts continue to run while the GPU is slow. Dispatch callbacks at
    // the timed wait boundary, but keep any resulting guest thread switch
    // deferred until the native GX call has unwound and renderer ownership is
    // safe again.
    IncrementSchedulerDisableCount();
    try {
        ProcessAlarmQueue(cpu, maxToProcess);
    } catch (...) {
        DecrementSchedulerDisableCount();
        throw;
    }
    DecrementSchedulerDisableCount();
}

extern "C" void OS_HLE_BeginDeferredGuestCallbacks()
{
    IncrementSchedulerDisableCount();
}

extern "C" void OS_HLE_EndDeferredGuestCallbacks()
{
    DecrementSchedulerDisableCount();
}

// HLE implementation replacing the translated version. Keeps the original
// field writes and InsertAlarm call but avoids the bad self-linked queue state.
extern "C" void OS__SetPeriodicAlarm_801a08e0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }

    SanitizeAlarmQueue(cpu);

    const uint32_t alarm = cpu->gpr[3];
    const uint32_t startHi = cpu->gpr[5];
    const uint32_t startLo = cpu->gpr[6];
    const uint32_t periodHi = cpu->gpr[7];
    const uint32_t periodLo = cpu->gpr[8];
    const uint32_t handler = cpu->gpr[9];

    const int32_t level = OS__DisableInterrupts_801a65ac();

    Memory::Write32(alarm + kAlarmPeriodLoOffset, periodLo);
    Memory::Write32(alarm + kAlarmPeriodHiOffset, periodHi);

    cpu->gpr[3] = startHi;
    cpu->gpr[4] = startLo;
    MKW_GUEST_FUNC(801AADE0)(cpu);
    Memory::Write32(alarm + 0x20u, cpu->gpr[3]);
    Memory::Write32(alarm + 0x24u, cpu->gpr[4]);

    cpu->gpr[3] = alarm;
    cpu->gpr[5] = 0;
    cpu->gpr[6] = 0;
    cpu->gpr[7] = handler;
    MKW_GUEST_FUNC(801A0620)(cpu);

    cpu->gpr[3] = static_cast<uint32_t>(OS__RestoreInterrupts_801a65d4(level));
}

// Register the function
PPC_NATIVE_OVERRIDE_VOID(801A08E0, OS__SetPeriodicAlarm_801a08e0, (CpuContext* ctx), (ctx));

// RFLiIsWorking (RFL, the Mii library) lives here because its whole job is to
// pump this file's alarm queue: RFLInitRes spins on it waiting for async RFL
// work that only completes when the interrupt-driven alarmCheckCb_ fires.
// Original at 0x800BD860 reads the "working" flag at RFL manager + 0x1B34,
// returning 0 when the manager pointer (0x80386298) is null.
extern "C" uint32_t RFLiIsWorking_HLE_800bd860()
{
    // Pump alarms/callbacks on the current guest thread when available. Using a
    // detached persistent context here can leave the busy loop waiting on work
    // that completed on the wrong scheduling context.
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    EnsureSda1Base(cpu);
    ProcessAlarmQueue(cpu, 32);

    // Now return the actual "working" status
    constexpr uint32_t kRflManagerPtrAddr = MKW_GADDR(80386298);
    constexpr uint32_t kWorkingFlagOffset = 0x1b34u;

    try {
        const uint32_t managerBase = ::Memory::Read32(kRflManagerPtrAddr);
        if (managerBase == 0) {
            return 0;
        }
        return ::Memory::Read32(managerBase + kWorkingFlagOffset);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "RFLiIsWorking", e);
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(800BD860, RFLiIsWorking_HLE_800bd860, uint32_t, (), ());
