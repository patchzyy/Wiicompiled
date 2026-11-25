#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "ppc_runtime.h"

// Forward declarations
struct CpuContext;

// GuestFiberManager: each guest OSThread maps to a host context. A scheduler context picks
// which guest fiber runs; a real timer thread queues VI retraces at the VI cadence. Guest
// threads only switch at explicit yield points (OSSleepThread, OSYieldThread, ...), matching
// Wii cooperative semantics exactly.

namespace Fiber {

// Thread states matching guest OS
enum class ThreadState : uint32_t {
    READY = 1,
    RUNNING = 2,
    WAITING = 4,
    MORIBUND = 8,
};

// Information about a guest fiber
struct GuestFiber {
    void* fiber = nullptr;              // Host context handle
    uint32_t entryPoint = 0;            // Thread entry function
    uint32_t entryArg = 0;              // Argument to entry function
    CpuContext cpuContext{};            // Saved CPU context for this fiber
    ThreadState state = ThreadState::READY;
    bool isSchedulerFiber = false;      // True for the main scheduler fiber
    bool terminated = false;            // Thread has exited
};

class GuestFiberManager {
public:
    // Initialize the fiber system - must be called from main thread
    static void Initialize();
    
    // Shutdown the fiber system
    static void Shutdown();
    
    // Check if fiber system is initialized
    static bool IsInitialized();
    
    // Create a fiber for a guest thread (called from OSCreateThread HLE).
    // OSCreateThread's stackSize/priority are not passed: the host fiber models
    // neither, only the guest SP seeded from stackBase. Returns true on success.
    static bool CreateGuestFiber(uint32_t guestThreadAddr, uint32_t entryPoint,
                                  uint32_t entryArg, uint32_t stackBase);
    
    // Resume a guest thread (called from OSResumeThread HLE)
    // This marks the fiber as runnable
    static void ResumeGuestThread(uint32_t guestThreadAddr);
    
    // Suspend a guest thread (called from OSSuspendThread HLE)
    static void SuspendGuestThread(uint32_t guestThreadAddr);
    
    // Called when a guest thread exits or is canceled
    static void ExitGuestThread(uint32_t guestThreadAddr, ThreadState finalState);
    
    // Switch to a specific guest thread's fiber (called from OSLoadContext)
    // This saves the current fiber's state and switches to the target
    static void SwitchToThread(uint32_t guestThreadAddr, CpuContext* cpu);
    
    // Get the currently running guest thread address (0 if in scheduler)
    static uint32_t GetCurrentGuestThread();
    
    // Get fiber info for a guest thread (may be null)
    static GuestFiber* GetFiber(uint32_t guestThreadAddr);
    
    // Check for VI retrace and process it (called from scheduler idle)
    static void ProcessTimerEvents(CpuContext* cpu);
    
    // Register the current host fiber as a guest thread fiber
    // This is used for the default/main thread that exists before OSCreateThread
    static bool RegisterMainThreadAsFiber(uint32_t guestThreadAddr, CpuContext* cpu);
    
    // Check if a fiber exists for a guest thread
    static bool HasFiber(uint32_t guestThreadAddr);
    static bool IsTerminated(uint32_t guestThreadAddr);
    // May be captured before the default thread is registered as a fiber.
    static uint64_t GetThreadGeneration(uint32_t guestThreadAddr);

private:
    // The fiber entry point wrapper
#if defined(_WIN32)
    static void CALLBACK FiberProc(void* param);
#else
    static void FiberProc(void* param);
#endif
    // Yield to the scheduler while preserving this host frame's registers and ambient state.
    static void SwitchToScheduler();

    // Internal state
    static std::mutex s_mutex;
    static std::unordered_map<uint32_t, GuestFiber> s_fibers;
    static std::unordered_map<uint32_t, uint64_t> s_threadGenerations;
    static uint64_t s_nextThreadGeneration;
    static std::vector<void*> s_fibersPendingDelete;
    // The scheduler's own host context. Its opaque handle is supplied by the
    // active HostContext backend, so one field serves every supported host.
    static void* s_schedulerFiber;
    static uint32_t s_currentGuestThread;
    static bool s_initialized;
    static void PurgePendingFibers();
};

// Global pending retrace count. GuestFiberManager::ProcessTimerEvents drains
// this on the guest thread so VI callbacks still run with guest state/locking
// expectations.
extern std::atomic<uint32_t> g_viRetracePendingCount;

} // namespace Fiber
