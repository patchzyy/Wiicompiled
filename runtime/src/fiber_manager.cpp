#include "fiber_manager.h"
#include "memory.h"
#include "abi_bridge.h"
#include "hle_stubs.h"
#include "host_context.h"
#include "runtime_log.h"

// Defined in hle/os/os_sleep.cpp; the sleep-timer table is file-local there.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace Fiber {

std::mutex GuestFiberManager::s_mutex;
std::unordered_map<uint32_t, GuestFiber> GuestFiberManager::s_fibers;
std::vector<void*> GuestFiberManager::s_fibersPendingDelete;
void* GuestFiberManager::s_schedulerFiber = nullptr;
uint32_t GuestFiberManager::s_currentGuestThread = 0;
bool GuestFiberManager::s_initialized = false;
thread_local CpuContext* GuestFiberManager::s_cpuContext = nullptr;

void GuestFiberManager::PurgePendingFibers() {
    std::vector<void*> toDelete;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        toDelete.swap(s_fibersPendingDelete);
    }
    for (void* f : toDelete) {
        if (f && !HostContext::IsCurrent(f)) {
            HostContext::Destroy(f);
        }
    }
}

// Global VI retrace counter
std::atomic<uint32_t> g_viRetracePendingCount{0};

// =============================================================================
// Guest OS memory layout constants
// =============================================================================
namespace {
constexpr uint32_t kOSCurrentContextAddr = 0x800000d4u;
constexpr uint32_t kOSRunningContextAddr = 0x800000e4u;  // OSGetCurrentThread reads this
constexpr uint32_t kThreadQueueArrayAddr = 0x803477b0u;
constexpr uint32_t kSchedulerPendingFlagAddr = 0x80386920u;
constexpr uint32_t kSchedulerReschedCounterAddr = 0x8038691cu;
constexpr uint32_t kSchedulerIdleFlagAddr = 0x80386918u;

// OSThread structure offsets
constexpr uint32_t kThreadStateOffset = 0x2C8u;
constexpr uint32_t kThreadAttrOffset = 0x2CAu;
constexpr uint32_t kThreadSuspendOffset = 0x2CCu;
constexpr uint32_t kThreadPriorityOffset = 0x2D0u;
constexpr uint32_t kThreadExitValueOffset = 0x2D8u;
constexpr uint32_t kThreadNextOffset = 0x2E0u;
constexpr uint32_t kThreadPrevOffset = 0x2E4u;
constexpr uint32_t kThreadQueueOffset = 0x2DCu;
constexpr uint32_t kThreadJoinQueueOffset = 0x2E8u;

// OSContext offsets (for saving/loading fiber context)
constexpr uint32_t kCtxGprOffset = 0x00u;
constexpr uint32_t kCtxCrOffset = 0x80u;
constexpr uint32_t kCtxLrOffset = 0x84u;
constexpr uint32_t kCtxCtrOffset = 0x88u;
constexpr uint32_t kCtxXerOffset = 0x8Cu;
constexpr uint32_t kCtxSrr0Offset = 0x198u;
constexpr uint32_t kCtxSrr1Offset = 0x19Cu;
constexpr uint32_t kCtxGqrOffset = 0x1A8u;

void ClearPendingMaskForEmptyGuestQueue(uint32_t queueEntry)
{
    if (queueEntry < kThreadQueueArrayAddr ||
        ((queueEntry - kThreadQueueArrayAddr) % 8u) != 0) {
        return;
    }

    const uint32_t priority = (queueEntry - kThreadQueueArrayAddr) / 8u;
    if (priority >= 32 || Memory::Read32(queueEntry) != 0) {
        return;
    }

    const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
    Memory::Write32(kSchedulerPendingFlagAddr, pending & ~(1u << (31u - priority)));
}

void RemoveGuestThreadFromQueue(uint32_t threadPtr)
{
    const uint32_t queuePtr = Memory::Read32(threadPtr + kThreadQueueOffset);
    if (queuePtr == 0) {
        return;
    }

    const uint32_t next = Memory::Read32(threadPtr + kThreadNextOffset);
    const uint32_t prev = Memory::Read32(threadPtr + kThreadPrevOffset);

    if (next != 0) {
        Memory::Write32(next + kThreadPrevOffset, prev);
    } else {
        Memory::Write32(queuePtr + 4u, prev);
    }

    if (prev != 0) {
        Memory::Write32(prev + kThreadNextOffset, next);
    } else {
        Memory::Write32(queuePtr, next);
    }

    Memory::Write32(threadPtr + kThreadQueueOffset, 0);
    Memory::Write32(threadPtr + kThreadNextOffset, 0);
    Memory::Write32(threadPtr + kThreadPrevOffset, 0);
    ClearPendingMaskForEmptyGuestQueue(queuePtr);
}

void WakeGuestThreadsOnQueueNoSwitch(uint32_t queueAddr)
{
    constexpr int kMaxWake = 256;

    for (int woke = 0; woke < kMaxWake; ++woke) {
        const uint32_t thread = Memory::Read32(queueAddr);
        if (thread == 0) {
            return;
        }

        const uint32_t next = Memory::Read32(thread + kThreadNextOffset);
        if (next == 0) {
            Memory::Write32(queueAddr + 4u, 0);
        } else {
            Memory::Write32(next + kThreadPrevOffset, 0);
        }
        Memory::Write32(queueAddr, next);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(thread + kThreadPrevOffset, 0);

        const uint16_t state = Memory::Read16(thread + kThreadStateOffset);
        if (state == 0 || state == 8) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        Memory::Write16(thread + kThreadStateOffset, 1);
        const int32_t suspend = static_cast<int32_t>(Memory::Read32(thread + kThreadSuspendOffset));
        if (suspend >= 1) {
            Memory::Write32(thread + kThreadQueueOffset, 0);
            continue;
        }

        int32_t priority = static_cast<int32_t>(Memory::Read32(thread + kThreadPriorityOffset));
        priority = std::clamp(priority, 0, 31);

        const uint32_t runQueue = kThreadQueueArrayAddr + static_cast<uint32_t>(priority) * 8u;
        const uint32_t tail = Memory::Read32(runQueue + 4u);
        if (tail == 0) {
            Memory::Write32(runQueue, thread);
        } else {
            Memory::Write32(tail + kThreadNextOffset, thread);
        }
        Memory::Write32(thread + kThreadPrevOffset, tail);
        Memory::Write32(thread + kThreadNextOffset, 0);
        Memory::Write32(runQueue + 4u, thread);
        Memory::Write32(thread + kThreadQueueOffset, runQueue);

        const uint32_t pending = Memory::Read32(kSchedulerPendingFlagAddr);
        Memory::Write32(kSchedulerPendingFlagAddr, pending | (1u << (31u - static_cast<uint32_t>(priority))));
        Memory::Write32(kSchedulerReschedCounterAddr, 1);

        GuestFiberManager::ResumeGuestThread(thread);
    }

    RT_LOG(RT_TAG_OS) << "WakeGuestThreadsOnQueueNoSwitch: hit safety limit at 0x"
              << std::hex << queueAddr << std::dec << std::endl;
}
} // namespace

// =============================================================================
// GuestFiberManager Implementation
// =============================================================================

void GuestFiberManager::Initialize() {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_initialized) {
        return;
    }
    
    if (!HostContext::InitializeScheduler(&s_schedulerFiber)) {
        RT_LOG(RT_TAG_OS) << "FATAL: Failed to initialize scheduler context!" << std::endl;
        ShowRuntimeFatalPopup("guest scheduler initialization failed",
                              "The host could not create the scheduler context required to run guest threads.");
        std::abort();
    }
    
    s_currentGuestThread = 0;
    s_initialized = true;
}

void GuestFiberManager::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);

    for (auto& [addr, fiber] : s_fibers) {
        if (fiber.fiber && !fiber.isSchedulerFiber) {
            HostContext::Destroy(fiber.fiber);
            fiber.fiber = nullptr;
        }
    }
    s_fibers.clear();

    if (s_schedulerFiber) {
        HostContext::ShutdownScheduler(s_schedulerFiber);
        s_schedulerFiber = nullptr;
    }

    s_initialized = false;
}

bool GuestFiberManager::IsInitialized() {
    return s_initialized;
}

bool GuestFiberManager::CreateGuestFiber(uint32_t guestThreadAddr, uint32_t entryPoint,
                                          uint32_t entryArg, uint32_t stackBase) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "CreateGuestFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if fiber already exists for this thread - if so, reset it
    auto existingIt = s_fibers.find(guestThreadAddr);
    if (existingIt != s_fibers.end()) {
        // Delete the old fiber if it exists and is not the scheduler fiber
        if (existingIt->second.fiber && !existingIt->second.isSchedulerFiber) {
            HostContext::Destroy(existingIt->second.fiber);
        }
        s_fibers.erase(existingIt);
    }
    
    GuestFiber gf;
    gf.entryPoint = entryPoint;
    gf.entryArg = entryArg;
    gf.state = ThreadState::WAITING; // Starts suspended
    gf.terminated = false;
    gf.isSchedulerFiber = false;
    
    // Initialize CPU context with entry point info
    std::memset(&gf.cpuContext, 0, sizeof(CpuContext));
    gf.cpuContext.gpr[1] = stackBase; // Stack pointer
    gf.cpuContext.gpr[3] = entryArg;  // First argument
    gf.cpuContext.lr = 0;             // No return address
    gf.cpuContext.pc = entryPoint;
    gf.cpuContext.srr0 = entryPoint;
    
    // The host stack models only translated host calls; the guest stack starts
    // at stackBase in the CPU context above. 64 KiB is too small for deep
    // translated/HLE call chains (notably NW4R's sound worker), and on macOS
    // it can exhaust the guarded coroutine stack as unrelated host work (such
    // as a window resize) adds a little more nesting. Keep enough headroom for
    // those chains while the guest stack remains separately bounded.
    constexpr size_t kHostStackSize = 1024 * 1024;
    gf.fiber = HostContext::Create(kHostStackSize, FiberProc,
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(guestThreadAddr)));
    
    if (!gf.fiber) {
        RT_LOG(RT_TAG_OS) << "Failed to create host context for thread 0x"
                  << std::hex << guestThreadAddr << std::dec << std::endl;
        return false;
    }

    s_fibers[guestThreadAddr] = gf;
    
    
    return true;
}

void GuestFiberManager::ResumeGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: no fiber for 0x"
                  << std::hex << guestThreadAddr << std::dec << std::endl;
        return;
    }
    
    if (it->second.terminated) {
        RT_LOG(RT_TAG_OS) << "ResumeGuestThread: thread 0x"
                  << std::hex << guestThreadAddr << " already terminated" << std::dec << std::endl;
        return;
    }
    
    it->second.state = ThreadState::READY;

}

void GuestFiberManager::SuspendGuestThread(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = ThreadState::WAITING;
    
}

void GuestFiberManager::ExitGuestThread(uint32_t guestThreadAddr, ThreadState finalState) {
    std::lock_guard<std::mutex> lock(s_mutex);
    
    auto it = s_fibers.find(guestThreadAddr);
    if (it == s_fibers.end()) {
        return;
    }
    
    it->second.state = finalState;
    it->second.terminated = true;
    if (s_currentGuestThread == guestThreadAddr) {
        s_currentGuestThread = 0;
    }
    
    if (it->second.fiber && !it->second.isSchedulerFiber) {
        if (HostContext::IsCurrent(it->second.fiber)) {
            s_fibersPendingDelete.push_back(it->second.fiber);
        } else {
            HostContext::Destroy(it->second.fiber);
        }
        it->second.fiber = nullptr;
    }
}

void GuestFiberManager::SwitchToThread(uint32_t guestThreadAddr, CpuContext* cpu) {
    PurgePendingFibers();
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "SwitchToThread called before initialization!" << std::endl;
        return;
    }
    
    void* fiberHandle = nullptr;
    uint32_t previousThread = 0;
    CpuContext callerContext{};
    const bool haveCallerContext = (cpu != nullptr);
    CpuContext targetContext{};
    bool haveTargetContext = false;

    if (cpu) {
        callerContext = *cpu;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: no fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        fiberHandle = it->second.fiber;
        
        if (!fiberHandle || it->second.terminated) {
            RT_LOG(RT_TAG_OS) << "SwitchToThread: invalid fiber for 0x"
                      << std::hex << guestThreadAddr << std::dec << std::endl;
            return;
        }
        
        // Remember what thread we're switching from
        previousThread = s_currentGuestThread;
        
        // Save current thread's CPU context if switching from a guest thread
        if (s_currentGuestThread != 0 && cpu) {
            auto currentIt = s_fibers.find(s_currentGuestThread);
            if (currentIt != s_fibers.end()) {
                currentIt->second.cpuContext = *cpu;
            }
        }
        
        // Update current thread
        s_currentGuestThread = guestThreadAddr;
        it->second.state = ThreadState::RUNNING;

        if (cpu) {
            targetContext = it->second.cpuContext;
            haveTargetContext = true;
        }
    }
    
    // Store CPU context pointer for the target fiber to use
    s_cpuContext = cpu;
    
    // Check if we're already on the target fiber (e.g., switching to main thread
    // when we're already on the scheduler fiber)
    if (HostContext::IsCurrent(fiberHandle)) {
        // Already executing on the target host fiber. This is common for the
        // default guest thread, which also owns the scheduler fiber. Keep the
        // live CPU context instead of restoring a possibly stale saved copy
        // from before the guest thread slept.
        return;
    }

    if (cpu && haveTargetContext) {
        *cpu = targetContext;
        // FPSCR travels with the guest-thread context, and its NI bit is
        // modeled through the per-host-thread MXCSR; every context restore
        // must re-mirror it.
        MkwApplyHostNiMode(cpu->fpscr);
    }

    // Switch to the target fiber (the target fiber will load its own context)
    HostContext::Switch(fiberHandle);

    // When we return here, the fiber that issued SwitchToThread has resumed.
    // That does not automatically mean the previous guest thread became runnable
    // again; a different thread may simply have yielded back to the scheduler.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        uint32_t runningContext = 0;
        uint32_t currentContext = 0;
        try {
            runningContext = Memory::Read32(kOSRunningContextAddr);
            currentContext = Memory::Read32(kOSCurrentContextAddr);
        } catch (const Memory::AccessViolation&) {
            runningContext = 0;
            currentContext = 0;
        }

        const bool resumedPreviousThread =
            previousThread != 0 &&
            runningContext == previousThread &&
            currentContext == previousThread;

        if (resumedPreviousThread) {
            auto it = s_fibers.find(previousThread);
            if (it != s_fibers.end()) {
                if (cpu) {
                    *cpu = it->second.cpuContext;
                    MkwApplyHostNiMode(cpu->fpscr);
                }
                it->second.state = ThreadState::RUNNING;
            }
            s_currentGuestThread = previousThread;
        } else {
            if (cpu && haveCallerContext) {
                *cpu = callerContext;
                MkwApplyHostNiMode(cpu->fpscr);
            }
            s_currentGuestThread = 0;
        }
    }
}

uint32_t GuestFiberManager::GetCurrentGuestThread() {
    return s_currentGuestThread;
}

GuestFiber* GuestFiberManager::GetFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() ? &it->second : nullptr;
}

bool GuestFiberManager::HasFiber(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_fibers.count(guestThreadAddr) > 0;
}

bool GuestFiberManager::IsTerminated(uint32_t guestThreadAddr) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_fibers.find(guestThreadAddr);
    return it != s_fibers.end() && it->second.terminated;
}

bool GuestFiberManager::RegisterMainThreadAsFiber(uint32_t guestThreadAddr, CpuContext* cpu) {
    if (!s_initialized) {
        RT_LOG(RT_TAG_OS) << "RegisterMainThreadAsFiber called before initialization!" << std::endl;
        return false;
    }
    
    std::lock_guard<std::mutex> lock(s_mutex);
    
    // Check if already registered
    if (s_fibers.count(guestThreadAddr)) {
        return true;
    }
    
    // Register the current host fiber (scheduler fiber) as this guest thread's fiber
    GuestFiber gf;
    gf.fiber = s_schedulerFiber;  // The main fiber IS the scheduler fiber
    gf.entryPoint = 0;
    gf.entryArg = 0;
    gf.state = ThreadState::RUNNING;
    gf.terminated = false;
    gf.isSchedulerFiber = true;  // This is special - it's both scheduler AND main thread
    
    // Copy current CPU context
    if (cpu) {
        gf.cpuContext = *cpu;
    }
    
    s_fibers[guestThreadAddr] = gf;
    s_currentGuestThread = guestThreadAddr;
    
    return true;
}

void GuestFiberManager::ProcessTimerEvents(CpuContext* cpu) {
    constexpr uint32_t kMaxRetracesPerSlice = 16;
    const uint32_t pending = g_viRetracePendingCount.exchange(0, std::memory_order_acq_rel);
    if (pending == 0) {
        return;
    }

    const uint32_t retracesToProcess = std::min(pending, kMaxRetracesPerSlice);
    const uint32_t remaining = pending - retracesToProcess;
    if (remaining != 0) {
        g_viRetracePendingCount.fetch_add(remaining, std::memory_order_release);
    }

    for (uint32_t i = 0; i < retracesToProcess; ++i) {
        VI_HLE_ForceRetrace(cpu);
    }
}

#if defined(_WIN32)
void CALLBACK GuestFiberManager::FiberProc(void* param)
#else
void GuestFiberManager::FiberProc(void* param)
#endif
{
    uint32_t guestThreadAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));


    // Get our fiber info
    GuestFiber* fiber = nullptr;
    uint32_t entryPoint = 0;
    uint32_t entryArg = 0;
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it == s_fibers.end()) {
            RT_LOG(RT_TAG_OS) << "FiberProc: fiber not found!" << std::endl;
            HostContext::Switch(s_schedulerFiber);
            return;
        }
        fiber = &it->second;
        entryPoint = fiber->entryPoint;
        entryArg = fiber->entryArg;
    }
    
    // Get the CPU context
    CpuContext* cpu = s_cpuContext;
    if (!cpu) {
        cpu = &GetPersistentCpuContext();
    }
    
    // Guest OSContext: r2 (TOC/SDA2) at 0x08, r13 (SDA) at 0x34. Both must load correctly or
    // translated code loses access to global/static data.
    try {
        // Load r2 (TOC/SDA2 pointer)
        cpu->gpr[2] = Memory::Read32(guestThreadAddr + 0x08u);
        // Load r13 (SDA pointer) - this is CRITICAL for global data access
        cpu->gpr[13] = Memory::Read32(guestThreadAddr + 0x34u);
        // Load stack pointer from guest context
        cpu->gpr[1] = Memory::Read32(guestThreadAddr + 0x04u);
        // Load saved LR
        cpu->lr = Memory::Read32(guestThreadAddr + 0x84u);
        
    } catch (const Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "Failed to load guest context from 0x" << std::hex << guestThreadAddr
                  << ": " << e.what() << std::dec << std::endl;
    }
    
    // Set up context for thread entry
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;
    
    // Create a CpuContextScope for this fiber
    CpuContextScope scope(cpu);
    
    int startDeferAttempts = 0;
    while (entryPoint == 0x8024373c) { // EGG::Thread::start
        uint32_t vtable = 0;
        uint32_t startFn = 0;
        try {
            vtable = Memory::Read32(entryArg);
            if (vtable >= 0x80000000u) {
                startFn = Memory::Read32(vtable + 0x0Cu);
            }
        } catch (const Memory::AccessViolation&) {
            vtable = 0;
            startFn = 0;
        }


        if (vtable >= 0x80000000u && startFn >= 0x80000000u) {
            break;
        }

        if (startDeferAttempts++ > 50) {
            RT_LOG(RT_TAG_OS) << "EGG::Thread::start target still invalid (vtable=0x" << std::hex << vtable
                      << ", fn=0x" << startFn << ") after retries; continuing anyway." << std::dec << std::endl;
            break;
        }
        HostContext::Switch(s_schedulerFiber);
    }

    // The deferral loop above yields to the scheduler and therefore can resume
    // with registers from a different guest fiber in the shared CpuContext.
    cpu->gpr[3] = entryArg;
    cpu->pc = entryPoint;
    cpu->srr0 = entryPoint;

    
    // Call the translated thread entry function
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(entryPoint);
    if (info) {
        InvokeIndirectCpu(entryPoint, cpu);
    } else {
        RT_LOG(RT_TAG_OS) << "Thread entry 0x" << std::hex << entryPoint
                  << " not found in registry!" << std::dec << std::endl;
    }
    
    // Thread entry functions normally return into OSExitThread on hardware.
    // Our host fiber call boundary observes the return directly, so complete the
    // guest OSThread lifecycle here before handing control back to the scheduler.

    try {
        RemoveGuestThreadFromQueue(guestThreadAddr);
        const uint16_t attributes = Memory::Read16(guestThreadAddr + kThreadAttrOffset);
        const bool detached = (attributes & 1u) != 0;
        const uint16_t finalState = detached ? 0u : static_cast<uint16_t>(ThreadState::MORIBUND);
        if (!detached) {
            Memory::Write32(guestThreadAddr + kThreadExitValueOffset, 0);
        }
        Memory::Write16(guestThreadAddr + kThreadStateOffset, finalState);
        WakeGuestThreadsOnQueueNoSwitch(guestThreadAddr + kThreadJoinQueueOffset);
        if (Memory::Read32(kOSRunningContextAddr) == guestThreadAddr) {
            Memory::Write32(kOSRunningContextAddr, 0);
        }
        if (Memory::Read32(kOSCurrentContextAddr) == guestThreadAddr) {
            Memory::Write32(kOSCurrentContextAddr, 0);
        }
        Memory::Write32(kSchedulerReschedCounterAddr, 1);
    } catch (const Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "Thread return cleanup failed for 0x" << std::hex
                  << guestThreadAddr << " at 0x" << e.address() << std::dec
                  << " (" << e.reason() << ")" << std::endl;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_fibers.find(guestThreadAddr);
        if (it != s_fibers.end()) {
            it->second.terminated = true;
            it->second.state = ThreadState::MORIBUND;
        }
        s_currentGuestThread = 0;
    }
    
    // Return to scheduler
    HostContext::Switch(s_schedulerFiber);
}

} // namespace Fiber
