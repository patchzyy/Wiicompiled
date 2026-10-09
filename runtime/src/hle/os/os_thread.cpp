// Guest thread lifecycle HLE (fiber-backed) plus the thread-list/priority-queue
// helpers shared with the scheduler.

#include <cstdint>
#include <iostream>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "runtime_log.h"
#include "os_internal.h"

namespace OsHleInternal {
void RemoveThreadFromList(uint32_t threadPtr)
{
    UnlinkGuestListNode(threadPtr, kThreadListNextOffset, kThreadListPrevOffset,
                        kThreadListHeadAddr, kThreadListTailAddr);
}

void UpdatePendingMaskForQueue(uint32_t queueEntry)
{
    if (queueEntry < kThreadQueueArrayAddr ||
        queueEntry >= (kThreadQueueArrayAddr + kThreadQueueArrayBytes) ||
        ((queueEntry - kThreadQueueArrayAddr) % 8u) != 0) {
        return;
    }

    if (::Memory::Read32(queueEntry) != 0) {
        return;
    }

    const uint32_t priority = (queueEntry - kThreadQueueArrayAddr) / 8u;
    const uint32_t pending = ::Memory::Read32(kSchedulerPendingFlagAddr);
    ::Memory::Write32(kSchedulerPendingFlagAddr, pending & ~(1u << (31u - priority)));
}

void RemoveThreadFromQueue(uint32_t threadPtr)
{
    const uint32_t queuePtr = ::Memory::Read32(threadPtr + kThreadQueueOffset);
    if (queuePtr == 0) {
        return;
    }

    UnlinkGuestListNode(threadPtr, kThreadNextOffset, kThreadPrevOffset, queuePtr, queuePtr + 4u);
    ::Memory::Write32(threadPtr + kThreadQueueOffset, 0);

    UpdatePendingMaskForQueue(queuePtr);
}

int32_t ComputeThreadEffectivePriority(uint32_t threadPtr)
{
    int32_t priority = static_cast<int32_t>(::Memory::Read32(threadPtr + kThreadBasePriorityOffset));

    for (uint32_t mutexPtr = ::Memory::Read32(threadPtr + kThreadMutexQueueOffset);
         mutexPtr != 0;
         mutexPtr = ::Memory::Read32(mutexPtr + kMutexThreadNextOffset)) {
        const uint32_t waiterThread = ::Memory::Read32(mutexPtr + kMutexWaitQueueHeadOffset);
        if (waiterThread == 0) {
            continue;
        }

        const int32_t waiterPriority =
            static_cast<int32_t>(::Memory::Read32(waiterThread + kThreadPriorityOffset));
        if (waiterPriority < priority) {
            priority = waiterPriority;
        }
    }

    return priority;
}

bool IsThpVideoDecoderEntry(uint32_t entryFunc)
{
    switch (entryFunc) {
    case MKW_GADDR(805529A8):
    case MKW_GADDR(80552A74):
        return true;
    default:
        return false;
    }
}

void InsertThreadIntoQueueByPriority(uint32_t queuePtr, uint32_t threadPtr, int32_t priority)
{
    ::Memory::Write32(threadPtr + kThreadQueueOffset, queuePtr);

    uint32_t insertBefore = ::Memory::Read32(queuePtr);
    while (insertBefore != 0) {
        const int32_t queuedPriority =
            static_cast<int32_t>(::Memory::Read32(insertBefore + kThreadPriorityOffset));
        if (queuedPriority > priority) {
            break;
        }
        insertBefore = ::Memory::Read32(insertBefore + kThreadNextOffset);
    }

    if (insertBefore == 0) {
        const uint32_t tail = ::Memory::Read32(queuePtr + 4u);
        if (tail == 0) {
            ::Memory::Write32(queuePtr, threadPtr);
        } else {
            ::Memory::Write32(tail + kThreadNextOffset, threadPtr);
        }
        ::Memory::Write32(threadPtr + kThreadPrevOffset, tail);
        ::Memory::Write32(threadPtr + kThreadNextOffset, 0);
        ::Memory::Write32(queuePtr + 4u, threadPtr);
    } else {
        ::Memory::Write32(threadPtr + kThreadNextOffset, insertBefore);
        const uint32_t prev = ::Memory::Read32(insertBefore + kThreadPrevOffset);
        ::Memory::Write32(insertBefore + kThreadPrevOffset, threadPtr);
        ::Memory::Write32(threadPtr + kThreadPrevOffset, prev);
        if (prev == 0) {
            ::Memory::Write32(queuePtr, threadPtr);
        } else {
            ::Memory::Write32(prev + kThreadNextOffset, threadPtr);
        }
    }

    if (queuePtr >= kThreadQueueArrayAddr &&
        queuePtr < (kThreadQueueArrayAddr + kThreadQueueArrayBytes) &&
        ((queuePtr - kThreadQueueArrayAddr) % 8u) == 0) {
        const uint32_t queueIndex = (queuePtr - kThreadQueueArrayAddr) / 8u;
        const uint32_t pending = ::Memory::Read32(kSchedulerPendingFlagAddr);
        ::Memory::Write32(kSchedulerPendingFlagAddr, pending | (1u << (31u - queueIndex)));
    }
}

uint32_t SetThreadEffectivePriority(uint32_t threadPtr, int32_t priority)
{
    const uint16_t state = ::Memory::Read16(threadPtr + kThreadStateOffset);
    if (state == 3u) {
        return 0;
    }

    if (state < 3u) {
        if (state == kThreadStateReady) {
            RemoveThreadFromQueue(threadPtr);
            ::Memory::Write32(threadPtr + kThreadPriorityOffset, static_cast<uint32_t>(priority));

            const uint32_t queueEntry =
                kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
            InsertThreadIntoQueueByPriority(queueEntry, threadPtr, priority);
            ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        } else if (state != 0u) {
            ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
            ::Memory::Write32(threadPtr + kThreadPriorityOffset, static_cast<uint32_t>(priority));
        }
        return 0;
    }

    if (state < 5u) {
        const uint32_t queuePtr = ::Memory::Read32(threadPtr + kThreadQueueOffset);
        RemoveThreadFromQueue(threadPtr);
        ::Memory::Write32(threadPtr + kThreadPriorityOffset, static_cast<uint32_t>(priority));
        if (queuePtr != 0) {
            InsertThreadIntoQueueByPriority(queuePtr, threadPtr, priority);
        }

        const uint32_t mutexPtr = ::Memory::Read32(threadPtr + kThreadMutexOffset);
        if (mutexPtr != 0) {
            return ::Memory::Read32(mutexPtr + kMutexOwnerOffset);
        }
    }

    return 0;
}
} // namespace OsHleInternal

namespace {
void PropagateMutexOwnerPriority(uint32_t mutexPtr)
{
    if (mutexPtr == 0) {
        return;
    }

    uint32_t ownerThread = ::Memory::Read32(mutexPtr + kMutexOwnerOffset);
    while (ownerThread != 0 &&
           static_cast<int32_t>(::Memory::Read32(ownerThread + kThreadSuspendOffset)) < 1) {
        const int32_t ownerPriority = ComputeThreadEffectivePriority(ownerThread);
        if (::Memory::Read32(ownerThread + kThreadPriorityOffset) ==
            static_cast<uint32_t>(ownerPriority)) {
            break;
        }
        ownerThread = SetThreadEffectivePriority(ownerThread, ownerPriority);
    }
}

void MarkFiberThreadTerminated(uint32_t threadPtr, uint16_t finalState)
{
    if (!Fiber::GuestFiberManager::IsInitialized()) {
        return;
    }

    Fiber::ThreadState fiberState = Fiber::ThreadState::MORIBUND;
    if (finalState == 0) {
        fiberState = Fiber::ThreadState::WAITING;
    }
    Fiber::GuestFiberManager::ExitGuestThread(threadPtr, fiberState);
}

void WakeThreadJoiners(CpuContext* cpu, uint32_t threadPtr)
{
    if (!cpu) {
        return;
    }
    cpu->gpr[3] = threadPtr + kThreadJoinQueueOffset;
    OSWakeupThread_HLE_801aaaa4(cpu);
}

void UnlockAllThreadMutexes(CpuContext* cpu, uint32_t threadPtr)
{
    if (!cpu) {
        return;
    }
    CpuContextScope scope(cpu);
    cpu->gpr[3] = threadPtr;
    InvokeIndirectCpu(MKW_GADDR(801A8088), cpu); // __OSUnlockAllMutex
}

// Shared tail of OSExitThread/OSCancelThread: clears context, delists if detached, marks
// final state, and wakes joiners/mutex waiters. Only OSExitThread publishes an exit value.
void TerminateThreadCommon(CpuContext* cpu, uint32_t threadPtr, bool publishExitValue,
                           uint32_t exitValue)
{
    OS__ClearContext_801a2098(threadPtr);

    const uint16_t attributes = ::Memory::Read16(threadPtr + kThreadAttrOffset);
    const bool detached = (attributes & 1u) != 0;
    const uint16_t finalState = detached ? 0u : kThreadStateMoribund;

    if (detached) {
        RemoveThreadFromList(threadPtr);
    } else if (publishExitValue) {
        ::Memory::Write32(threadPtr + kThreadExitValueOffset, exitValue);
    }

    ::Memory::Write16(threadPtr + kThreadStateOffset, finalState);
    MarkFiberThreadTerminated(threadPtr, finalState);

    UnlockAllThreadMutexes(cpu, threadPtr);
    WakeThreadJoiners(cpu, threadPtr);
}
} // namespace

// Fiber-based threading HLE: each guest OSThread gets a host fiber for cooperative
// context switching without blocking the main thread.

// OSCreateThread (0x801a9e84)
// Creates a new guest thread and associates a host fiber with it.
extern "C" void OSCreateThread_HLE_801a9e84(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    // r3..r9 = thread struct, entry func, entry arg, stack top, stack size, priority, attributes.

    const uint32_t threadPtr  = cpu->gpr[3];
    const uint32_t entryFunc  = cpu->gpr[4];
    const uint32_t entryArg   = cpu->gpr[5];
    const uint32_t stackTop   = cpu->gpr[6];
    const uint32_t stackSize  = cpu->gpr[7];
    const int32_t  priority   = static_cast<int32_t>(cpu->gpr[8]);
    const uint16_t attributes = static_cast<uint16_t>(cpu->gpr[9]);
    
    // Validate priority range
    if (priority < 0 || priority > 31) {
        RT_LOG(RT_TAG_OS) << "OSCreateThread: invalid priority " << priority << std::endl;
        cpu->gpr[3] = 0; // Return failure
        return;
    }
    
    // Create the guest fiber
    if (Fiber::GuestFiberManager::IsInitialized()) {
        Fiber::GuestFiberManager::CreateGuestFiber(threadPtr, entryFunc, entryArg, stackTop);

        if (Fiber::GuestFiberManager::GetFiber(threadPtr) != nullptr) {
            const uint32_t hid2 = cpu->hid2 != 0 ? cpu->hid2 : 0x10000000u;
            Fiber::GuestFiberManager::GetFiber(threadPtr)->cpuContext.hid2 = hid2;
        }
    }
    
    // Initialize guest thread structure (matching original SDK behavior)
    try {
        const uint32_t alignedStack = stackTop & 0xFFFFFFF8u;
        
        // Thread state and attributes
        ::Memory::Write16(threadPtr + 0x2C8u, 1);              // state = READY
        ::Memory::Write16(threadPtr + 0x2CAu, attributes & 1); // attributes (detached)
        ::Memory::Write32(threadPtr + 0x2D4u, priority);       // base priority
        ::Memory::Write32(threadPtr + 0x2D0u, priority);       // effective priority
        ::Memory::Write32(threadPtr + 0x2CCu, 1);              // suspend count = 1 (created suspended)
        ::Memory::Write32(threadPtr + 0x2D8u, 0xFFFFFFFFu);    // exit value
        
        // Queue pointers
        ::Memory::Write32(threadPtr + 0x2F0u, 0);
        ::Memory::Write32(threadPtr + 0x2ECu, 0);
        ::Memory::Write32(threadPtr + 0x2E8u, 0);
        ::Memory::Write32(threadPtr + 0x2F8u, 0);
        ::Memory::Write32(threadPtr + 0x2F4u, 0);
        
        // Stack setup - write frame markers
        ::Memory::Write32(alignedStack - 8, 0);
        ::Memory::Write32(alignedStack - 4, 0);

        // Let the translated SDK path initialize the guest OSContext exactly
        // like OSInitContext, then apply the OSCreateThread-specific overrides
        // that follow in the original PPC.
        {
            CpuContextScope scope(cpu);
            cpu->gpr[3] = threadPtr;
            cpu->gpr[4] = entryFunc;
            cpu->gpr[5] = alignedStack - 8;
            InvokeIndirectCpu(MKW_GADDR(801A20BC), cpu); // OSInitContext
        }

        if (IsThpVideoDecoderEntry(entryFunc)) {
            // Decoder threads restore their own context via OSLoadContext, so the saved
            // context needs these GQR2-GQR5 values or paired-single THP decode breaks
            // under the all-zero OSInitContext defaults.
            ::Memory::Write32(threadPtr + 0x1ACu, 0x00040004u);
            ::Memory::Write32(threadPtr + 0x1B0u, 0x00050005u);
            ::Memory::Write32(threadPtr + 0x1B4u, 0x00060006u);
            ::Memory::Write32(threadPtr + 0x1B8u, 0x00070007u);

        }

        ::Memory::Write32(threadPtr + 0x84u, MKW_GADDR(801AA0F0)); // LR = OSExitThread
        ::Memory::Write32(threadPtr + 0x0Cu, entryArg);    // r3 = argument

        // Stack info
        ::Memory::Write32(threadPtr + 0x304u, stackTop);
        ::Memory::Write32(threadPtr + 0x308u, stackTop - stackSize);
        ::Memory::Write32(stackTop - stackSize, 0xDEADBABEu);  // Stack guard
        
        // Thread list linkage
        ::Memory::Write32(threadPtr + 0x30Cu, 0);
        ::Memory::Write32(threadPtr + 0x310u, 0);
        ::Memory::Write32(threadPtr + 0x314u, 0);

        // Match the original OSCreateThread slow-path initialization that runs
        // once scheduler globals are live. THP worker threads depend on these
        // queue/list blocks being fully zeroed.
        constexpr uint32_t kSchedulerInitFlagAddr = MKW_GADDR(80347130);
        constexpr uint32_t kThreadAttrSourceAddr = MKW_GADDR(80385AA8);
        if (Memory::Contains(kSchedulerInitFlagAddr, 4) &&
            ::Memory::Read32(kSchedulerInitFlagAddr) != 0) {
            uint32_t srr1 = ::Memory::Read32(threadPtr + 0x19Cu);
            srr1 |= 0x900u;
            ::Memory::Write32(threadPtr + 0x19Cu, srr1);

            uint16_t modeFlags = ::Memory::Read16(threadPtr + 0x1A2u);
            modeFlags = static_cast<uint16_t>(modeFlags | 0x1u);
            ::Memory::Write16(threadPtr + 0x1A2u, modeFlags);

            if (Memory::Contains(kThreadAttrSourceAddr, 4)) {
                const uint32_t attr = (::Memory::Read32(kThreadAttrSourceAddr) & 0xF8u) | 0x4u;
                ::Memory::Write32(threadPtr + 0x194u, attr);
            }

            for (uint32_t offset = 0; offset < 0x80u; offset += 4u) {
                ::Memory::Write32(threadPtr + 0x90u + offset, 0);
                ::Memory::Write32(threadPtr + 0x1C8u + offset, 0);
            }
        }

        // Add to global thread list (matching original SDK logic exactly)
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        
        // Read thread list tail (last added thread)
        const uint32_t tailThread = ::Memory::Read32(kThreadListTailAddr);
        
        // The new thread becomes the head if the queue is empty.
        uint32_t newHead = threadPtr;
        
        if (tailThread != 0) {
            // There's an existing tail - link it to the new thread
            ::Memory::Write32(tailThread + 0x2FCu, threadPtr);  // tail->next = new
            // Keep existing head
            newHead = ::Memory::Read32(kThreadListHeadAddr);
        }
        
        // Update thread list head
        ::Memory::Write32(kThreadListHeadAddr, newHead);
        
        // Link new thread into list
        ::Memory::Write32(threadPtr + 0x300u, tailThread);  // new->prev = old tail
        ::Memory::Write32(threadPtr + 0x2FCu, 0);           // new->next = 0
        
        // New thread becomes the tail
        ::Memory::Write32(kThreadListTailAddr, threadPtr);
        
        OS__RestoreInterrupts_801a65d4(irqState);
        
        cpu->gpr[3] = 1; // Return success
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSCreateThread", e);
        cpu->gpr[3] = 0; // Return failure
    }
}
PPC_NATIVE_OVERRIDE_VOID(801A9E84, OSCreateThread_HLE_801a9e84, (CpuContext* ctx), (ctx));

extern "C" void OSExitThread_HLE_801aa0f0(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t exitValue = cpu->gpr[3];
    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const uint32_t threadPtr = ::Memory::Read32(kOSRunningContextAddr);
        if (threadPtr == 0) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        TerminateThreadCommon(cpu, threadPtr, true, exitValue);

        ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        cpu->gpr[3] = 0;
        SelectThread_801a9c08(cpu);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSExitThread", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA0F0, OSExitThread_HLE_801aa0f0, (CpuContext* ctx), (ctx));

extern "C" void OSCancelThread_HLE_801aa1d4(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t threadPtr = cpu->gpr[3];
    if (threadPtr == 0) {
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const uint16_t state = ::Memory::Read16(threadPtr + kThreadStateOffset);
        if (state == 3 || state == 0 || state > 4) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        if (state == kThreadStateReady) {
            const int32_t suspend = static_cast<int32_t>(::Memory::Read32(threadPtr + kThreadSuspendOffset));
            if (suspend < 1) {
                RemoveThreadFromQueue(threadPtr);
            }
        } else if (state == kThreadStateRunning) {
            ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        } else if (state == kThreadStateWaiting) {
            RemoveThreadFromQueue(threadPtr);
        }

        TerminateThreadCommon(cpu, threadPtr, false, 0);

        if (::Memory::Read32(kSchedulerReschedCounterAddr) != 0) {
            cpu->gpr[3] = 0;
            SelectThread_801a9c08(cpu);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSCancelThread", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA1D4, OSCancelThread_HLE_801aa1d4, (CpuContext* ctx), (ctx));

extern "C" void OSJoinThread_HLE_801aa3ac(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t threadPtr = cpu->gpr[3];
    const uint32_t outExitValue = cpu->gpr[4];
    if (threadPtr == 0) {
        cpu->gpr[3] = 0;
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    uint32_t result = 0;

    try {
        const uint16_t attributes = ::Memory::Read16(threadPtr + kThreadAttrOffset);
        uint16_t state = ::Memory::Read16(threadPtr + kThreadStateOffset);
        const uint32_t joinHead = ::Memory::Read32(threadPtr + kThreadJoinQueueOffset);

        if ((attributes & 1u) == 0 && state != kThreadStateMoribund && joinHead == 0) {
            cpu->gpr[3] = threadPtr + kThreadJoinQueueOffset;
            OSSleepThread_HLE_801aa9b8(cpu);
            state = ::Memory::Read16(threadPtr + kThreadStateOffset);

            bool foundInList = false;
            if (state != 0) {
                for (uint32_t it = ::Memory::Read32(kThreadListHeadAddr); it != 0;
                     it = ::Memory::Read32(it + kThreadListNextOffset)) {
                    if (it == threadPtr) {
                        foundInList = true;
                        break;
                    }
                }
            }

            if (!foundInList && state != kThreadStateMoribund) {
                OS__RestoreInterrupts_801a65d4(irqState);
                cpu->gpr[3] = 0;
                return;
            }
        }

        if (state == kThreadStateMoribund) {
            if (outExitValue != 0) {
                ::Memory::Write32(outExitValue, ::Memory::Read32(threadPtr + kThreadExitValueOffset));
            }
            RemoveThreadFromList(threadPtr);
            ::Memory::Write16(threadPtr + kThreadStateOffset, 0);
            result = 1;
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSJoinThread", e);
        result = 0;
    }

    OS__RestoreInterrupts_801a65d4(irqState);
    cpu->gpr[3] = result;
}
PPC_NATIVE_OVERRIDE_VOID(801AA3AC, OSJoinThread_HLE_801aa3ac, (CpuContext* ctx), (ctx));

extern "C" void OSDetachThread_HLE_801aa4ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t threadPtr = cpu->gpr[3];
    if (threadPtr == 0) {
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const uint16_t attributes = ::Memory::Read16(threadPtr + kThreadAttrOffset);
        ::Memory::Write16(threadPtr + kThreadAttrOffset, attributes | 1u);

        const uint16_t state = ::Memory::Read16(threadPtr + kThreadStateOffset);
        if (state == kThreadStateMoribund) {
            RemoveThreadFromList(threadPtr);
            ::Memory::Write16(threadPtr + kThreadStateOffset, 0);
            MarkFiberThreadTerminated(threadPtr, 0);
        }

        WakeThreadJoiners(cpu, threadPtr);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSDetachThread", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA4EC, OSDetachThread_HLE_801aa4ec, (CpuContext* ctx), (ctx));

extern "C" void OSSuspendThread_HLE_801aa6a8(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t threadPtr = cpu->gpr[3];
    if (threadPtr == 0) {
        cpu->gpr[3] = 0;
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        const int32_t suspendCount =
            static_cast<int32_t>(::Memory::Read32(threadPtr + kThreadSuspendOffset));
        ::Memory::Write32(threadPtr + kThreadSuspendOffset,
                          static_cast<uint32_t>(suspendCount + 1));

        if (suspendCount == 0) {
            const uint16_t state = ::Memory::Read16(threadPtr + kThreadStateOffset);
            if (state < 3u) {
                if (state == kThreadStateReady) {
                    RemoveThreadFromQueue(threadPtr);
                } else if (state != 0u) {
                    ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
                    ::Memory::Write16(threadPtr + kThreadStateOffset, kThreadStateReady);
                }
            } else if (state < 5u) {
                const uint32_t queuePtr = ::Memory::Read32(threadPtr + kThreadQueueOffset);
                RemoveThreadFromQueue(threadPtr);
                ::Memory::Write32(threadPtr + kThreadPriorityOffset, kSuspendedWaitPriority);
                if (queuePtr != 0) {
                    InsertThreadIntoQueueByPriority(queuePtr, threadPtr, kSuspendedWaitPriority);
                }
                PropagateMutexOwnerPriority(::Memory::Read32(threadPtr + kThreadMutexOffset));
            }

            if (Fiber::GuestFiberManager::IsInitialized()) {
                Fiber::GuestFiberManager::SuspendGuestThread(threadPtr);
            }

            if (::Memory::Read32(kSchedulerReschedCounterAddr) != 0) {
                cpu->gpr[3] = 0;
                SelectThread_801a9c08(cpu);
            }
        }

        cpu->gpr[3] = static_cast<uint32_t>(suspendCount);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSuspendThread", e);
        cpu->gpr[3] = 0;
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA6A8, OSSuspendThread_HLE_801aa6a8, (CpuContext* ctx), (ctx));

// OSResumeThread (0x801aa58c)
// Resumes a suspended thread, making it eligible for scheduling.
extern "C" void OSResumeThread_HLE_801aa58c(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t threadPtr = cpu->gpr[3];
    
    if (threadPtr == 0) {
        cpu->gpr[3] = 0;
        return;
    }
    
    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    
    try {
        // Read current suspend count
        const int32_t suspendCount = static_cast<int32_t>(::Memory::Read32(threadPtr + 0x2CCu));
        const int32_t newSuspend = suspendCount - 1;
        
        if (newSuspend < 0) {
            ::Memory::Write32(threadPtr + 0x2CCu, 0);
        } else {
            ::Memory::Write32(threadPtr + 0x2CCu, static_cast<uint32_t>(newSuspend));
            
            if (newSuspend == 0) {
                CancelSleepTimer(threadPtr);
                ClearOutstandingPark(threadPtr);
                const uint16_t state = ::Memory::Read16(threadPtr + 0x2C8u);

                if (state == kThreadStateWaiting) {
                    const uint32_t queuePtr = ::Memory::Read32(threadPtr + kThreadQueueOffset);
                    RemoveThreadFromQueue(threadPtr);

                    const int32_t priority = ComputeThreadEffectivePriority(threadPtr);
                    ::Memory::Write32(threadPtr + kThreadPriorityOffset, static_cast<uint32_t>(priority));

                    if (queuePtr != 0) {
                        InsertThreadIntoQueueByPriority(queuePtr, threadPtr, priority);
                    }

                    if (Fiber::GuestFiberManager::IsInitialized()) {
                        Fiber::GuestFiberManager::SuspendGuestThread(threadPtr);
                    }

                    PropagateMutexOwnerPriority(::Memory::Read32(threadPtr + kThreadMutexOffset));
                } else if (state == kThreadStateReady) {
                    const int32_t priority = ComputeThreadEffectivePriority(threadPtr);
                    ::Memory::Write32(threadPtr + kThreadPriorityOffset, static_cast<uint32_t>(priority));

                    const uint32_t queueEntry =
                        kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
                    InsertThreadIntoQueueByPriority(queueEntry, threadPtr, priority);
                    ::Memory::Write32(kSchedulerReschedCounterAddr, 1);

                    if (Fiber::GuestFiberManager::IsInitialized()) {
                        if (threadPtr == kDefaultThreadContextAddr && !Fiber::GuestFiberManager::HasFiber(threadPtr)) {
                            Fiber::GuestFiberManager::RegisterMainThreadAsFiber(threadPtr, cpu);
                        }
                        Fiber::GuestFiberManager::ResumeGuestThread(threadPtr);
                    }
                } else {
                    // Neither Waiting nor Ready: no path reschedules the fiber, so the thread
                    // would be lost. A thread stuck Running (its park raced the timer pump) is
                    // recovered as Ready; terminated threads stay dead.
                    RT_LOG(RT_TAG_OS) << "OSResumeThread: thread 0x" << std::hex << threadPtr
                              << std::dec << " reached suspend count 0 in state=" << state
                              << (state == kThreadStateRunning ? "; recovering as Ready"
                                                               : "; no wake path - thread lost")
                              << std::endl;
                    if (state == kThreadStateRunning &&
                        !Fiber::GuestFiberManager::IsTerminated(threadPtr)) {
                        ::Memory::Write16(threadPtr + kThreadStateOffset, kThreadStateReady);
                        const int32_t priority = ComputeThreadEffectivePriority(threadPtr);
                        ::Memory::Write32(threadPtr + kThreadPriorityOffset,
                                          static_cast<uint32_t>(priority));
                        const uint32_t queueEntry =
                            kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
                        InsertThreadIntoQueueByPriority(queueEntry, threadPtr, priority);
                        ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
                        if (Fiber::GuestFiberManager::IsInitialized()) {
                            Fiber::GuestFiberManager::ResumeGuestThread(threadPtr);
                        }
                    }
                }

                if (::Memory::Read32(kSchedulerReschedCounterAddr) != 0) {
                    cpu->gpr[3] = 0;
                    SelectThread_801a9c08(cpu);
                }
            }
        }
        
        cpu->gpr[3] = static_cast<uint32_t>(suspendCount);
        
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSResumeThread", e);
        cpu->gpr[3] = 0;
    }
    
    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(801AA58C, OSResumeThread_HLE_801aa58c, (CpuContext* ctx), (ctx));
