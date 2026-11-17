#include "nand_internal.h"
#include "guest_callback_state.h"

// Guest callback dispatch: host NAND work completes synchronously, so an "async" call just
// queues its guest completion callback here and an HLE pump dispatches it later.

struct PendingNandCallback {
    uint32_t callbackPtr = 0;
    int32_t result = 0;
    uint32_t commandBlock = 0;
};

static std::mutex g_pendingNandCallbackMutex;
static std::deque<PendingNandCallback> g_pendingNandCallbacks;

static void DispatchNandCallback(CpuContext* cpu, uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    // The callback signature is: void callback(int result, void* commandBlock)
    // We need to invoke it through the translated function system
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        LogNandError("InvokeNandCallback", "callback 0x%08X not found in registry", callbackPtr);
        return;
    }

    if (cpu) {
        // IOS/NAND completions are asynchronous side calls. Run them on a
        // scratch guest CPU context so callback argument setup and any
        // transient LR/CTR/nonvolatile register changes do not corrupt the
        // interrupted thread that happened to pump the completion queue.
        CpuContext callbackCpu = *cpu;
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        CpuContextScope callbackScope(&callbackCpu);
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    } else {
        auto& callbackCpu = GetPersistentCpuContext();
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    }
}

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
    g_pendingNandCallbacks.push_back({callbackPtr, result, commandBlock});
}

// Guest NAND callbacks can re-enter this drain: RFL's hidden-Mii loader chains one async
// load per Mii as its own completion callback, and a database fattened by online play
// nested hundreds of frames deep and crashed at boot. Hardware is iterative via the IPC
// interrupt, so a nested drain refuses here and the outermost loop dispatches the queue flat.
bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess) {
    if (maxToProcess <= 0) {
        return false;
    }
    if (GuestCallbacks::nandDraining) {
        return false;
    }
    struct DrainScope {
        DrainScope() { GuestCallbacks::nandDraining = true; }
        ~DrainScope() { GuestCallbacks::nandDraining = false; }
    } drainScope;

    // Host-side NAND work finishes immediately. Drain a generous chunk of the guest
    // callback queue in one pump so multi-step chains (open -> read -> close -> user cb)
    // do not get stranded across many retrace ticks and trip the RFL/Mii error path.
    const size_t maxCallbacksThisPump = std::max<size_t>(static_cast<size_t>(maxToProcess), 64);
    size_t processed = 0;
    while (processed < maxCallbacksThisPump) {
        PendingNandCallback cb{};
        {
            std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
            if (g_pendingNandCallbacks.empty()) {
                break;
            }
            cb = g_pendingNandCallbacks.front();
            g_pendingNandCallbacks.pop_front();
        }
        DispatchNandCallback(cpu, cb.callbackPtr, cb.result, cb.commandBlock);
        ++processed;
    }
    return processed != 0;
}
