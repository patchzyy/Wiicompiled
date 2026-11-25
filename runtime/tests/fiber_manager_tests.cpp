#include "abi_bridge.h"
#include "fiber_manager.h"
#include "guest_function_imports.h"
#include "hle/os/os_internal.h"
#include "hle_stubs.h"
#include "memory_slow_test_support.h"

// Count native stack lifetimes while exercising the platform's real handoff.
#define HostContext FiberTestBackend
#include "../src/host_context.cpp"
#undef HostContext

#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

extern "C" void OSExitThread_HLE_801aa0f0(CpuContext*);
extern "C" void OSCancelThread_HLE_801aa1d4(CpuContext*);
extern "C" void OSJoinThread_HLE_801aa3ac(CpuContext*);
void NandQueueIosCallback(uint32_t, int32_t, uint32_t);
bool NandProcessPendingCallbacks(CpuContext*, int);
void LogNandError(const char*, const char*, ...) { std::abort(); }

extern "C" const uint8_t kMkwGuestRegion = 0;
extern "C" uint32_t PPC_Cntlzw(uint32_t value) { return PPC_CntlzwInline(value); }

namespace {
void Check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::fprintf(stderr, "fiber regression at line %d: %s\n", line, expression);
        std::_Exit(1);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

std::unordered_set<void*> g_liveStacks;
size_t g_created = 0;
size_t g_destroyed = 0;
std::array<uint8_t, 4 * 1024 * 1024> g_memory{};
CpuContext g_cpu{};
std::unordered_map<uint32_t, TranslatedFunctionInfo> g_entries;
int g_unlockCalls = 0;
int g_stage = 0;
bool g_reuseReturns = false;

constexpr uint32_t kMain = 0x80010000;
constexpr uint32_t kWorker = 0x80011000;
constexpr uint32_t kOther = 0x80012000;
constexpr uint32_t kJoiner = 0x80013000;
constexpr uint32_t kMutexWaiter = 0x80014000;
constexpr uint32_t kMutex = 0x80018000;
constexpr uint32_t kSecondMutex = 0x80018100;
constexpr uint32_t kOutput = 0x80019000;
constexpr uint32_t kObject = 0x8001a000;
constexpr uint32_t kVtable = 0x8001b000;
constexpr uint32_t kEntry = 0x80020000;
constexpr uint32_t kOtherEntry = 0x80020100;
constexpr uint32_t kCallback = 0x80020200;
constexpr uint32_t kExitValue = 0x1234abcd;

using Manager = Fiber::GuestFiberManager;

void Register(uint32_t address, void (*entry)(CpuContext*))
{
    g_entries[address] = {entry};
}

void SetIdentity(uint32_t thread)
{
    Memory::Write32(kOSCurrentContextAddr, thread);
    Memory::Write32(kOSRunningContextAddr, thread);
    Memory::Write16(thread + kThreadStateOffset, kThreadStateRunning);
}

void Switch(uint32_t thread, CpuContext* cpu)
{
    SetIdentity(thread);
    Manager::SwitchToThread(thread, cpu);
}

void SeedThread(uint32_t thread, uint32_t priority, uint32_t srr1 = 0x9032)
{
    std::memset(Memory::GetPointer(thread, 0x318), 0, 0x318);
    Memory::Write32(thread + 4, 0x80100000 - thread % 0x10000);
    Memory::Write32(thread + 8, 0x8038efa0);
    Memory::Write32(thread + 0x34, 0x8038cc00);
    Memory::Write32(thread + 0x84, 0x801aa0f0);
    Memory::Write32(thread + 0x19c, srr1);
    Memory::Write32(thread + kThreadPriorityOffset, priority);
    Memory::Write32(thread + kThreadBasePriorityOffset, priority);
    Memory::Write16(thread + kThreadStateOffset, kThreadStateReady);
}

void Create(uint32_t thread, uint32_t entry, uint32_t argument, uint32_t priority,
            uint32_t srr1 = 0x9032)
{
    SeedThread(thread, priority, srr1);
    CHECK(Manager::CreateGuestFiber(thread, entry, argument, Memory::Read32(thread + 4)));
}

void Enqueue(uint32_t thread)
{
    Memory::Write16(thread + kThreadStateOffset, kThreadStateReady);
    const uint32_t priority = Memory::Read32(thread + kThreadPriorityOffset);
    InsertThreadIntoQueueByPriority(kThreadQueueArrayAddr + priority * 8, thread, priority);
    Manager::ResumeGuestThread(thread);
}

void WaitOn(uint32_t thread, uint32_t queue)
{
    Memory::Write16(thread + kThreadStateOffset, kThreadStateWaiting);
    Memory::Write32(thread + kThreadSuspendOffset, 1);
    Memory::Write32(thread + kThreadQueueOffset, queue);
    Memory::Write32(queue, thread);
    Memory::Write32(queue + 4, thread);
}

void UnlockAll(CpuContext* cpu)
{
    ++g_unlockCalls;
    const uint32_t thread = cpu->gpr[3];
    for (uint32_t mutex = Memory::Read32(thread + kThreadMutexQueueOffset); mutex != 0;) {
        const uint32_t next = Memory::Read32(mutex + kMutexThreadNextOffset);
        CHECK(Memory::Read32(mutex + kMutexOwnerOffset) == thread);
        Memory::Write32(thread + kThreadMutexQueueOffset, next);
        if (next == 0) {
            Memory::Write32(thread + kThreadMutexTailOffset, 0);
        } else {
            Memory::Write32(next + kMutexThreadPrevOffset, 0);
        }
        Memory::Write32(mutex + kMutexOwnerOffset, 0);
        Memory::Write32(mutex + kMutexCountOffset, 0);
        cpu->gpr[3] = mutex;
        OSWakeupThread_HLE_801aaaa4(cpu);
        mutex = next;
    }
    Memory::Write32(thread + kThreadMutexQueueOffset, 0);
    Memory::Write32(thread + kThreadMutexTailOffset, 0);
}

struct Fixture {
    Fixture()
    {
        CHECK(g_liveStacks.empty());
        g_memory.fill(0);
        g_cpu = {};
        g_cpu.gpr[1] = 0x80100000;
        g_cpu.gpr[29] = 0x29292929;
        g_cpu.gpr[31] = 0x31313131;
        g_entries.clear();
        Register(0x801a8088, UnlockAll);
        g_unlockCalls = 0;
        g_stage = 0;
        g_currentCpuContext = &g_cpu;
        MkwApplyHostNiMode(0);
        g_interrupts_enabled.store(true);
        SeedThread(kMain, 10);
        SetIdentity(kMain);
        Manager::Initialize();
        const auto generation = Manager::GetThreadGeneration(kMain);
        CHECK(Manager::RegisterMainThreadAsFiber(kMain, &g_cpu));
        CHECK(Manager::GetThreadGeneration(kMain) == generation);
    }

    ~Fixture()
    {
        Manager::Shutdown();
        CHECK(g_liveStacks.empty());
        CHECK(g_created == g_destroyed);
        g_currentCpuContext = nullptr;
    }
};

void ScratchWorker(CpuContext* cpu)
{
    CHECK(cpu == &GetPersistentCpuContext());
    CHECK(TryGetCpuContext() == cpu);
    CHECK(cpu->gpr[3] == 73);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK((Memory::Read16(kWorker + 0x1a2) & 2) != 0);
    cpu->gpr[29] = 0xabcdef29;
    cpu->fpscr = 4;
    MkwApplyHostNiMode(cpu->fpscr);
    {
        CpuContext scratch = *cpu;
        scratch.gpr[29] = 0xcafe0029;
        scratch.fpscr = 0;
        CpuContextScope scope(&scratch);
        OS__DisableInterrupts_801a65ac();
        Switch(kMain, &scratch);
        CHECK(TryGetCpuContext() == &scratch);
        CHECK(scratch.gpr[29] == 0xcafe0029);
        CHECK(cpu->gpr[29] == 0xabcdef29);
        CHECK(!OS_HLE_InterruptsEnabled());
        CHECK(!g_mkwHostNiActive);
        scratch.gpr[31] = 0xfeed0031;
        CHECK(TryGetCpuContext()->gpr[31] == 0xfeed0031);
    }
    CHECK(TryGetCpuContext() == cpu);
    CHECK(g_mkwHostNiActive);
    cpu->gpr[3] = kExitValue;
}

void TestNestedScratch()
{
    Fixture fixture;
    Register(kEntry, ScratchWorker);
    Create(kWorker, kEntry, 73, 3);
    {
        CpuContext outer = g_cpu;
        outer.gpr[29] = 0xaaaa0029;
        outer.fpscr = 4;
        CpuContextScope outerScope(&outer);
        {
            CpuContext inner = outer;
            inner.gpr[29] = 0xbbbb0029;
            inner.fpscr = 0;
            CpuContextScope innerScope(&inner);
            OS__DisableInterrupts_801a65ac();
            Switch(kWorker, &inner);
            CHECK(TryGetCpuContext() == &inner);
            CHECK(inner.gpr[29] == 0xbbbb0029);
            CHECK(g_cpu.gpr[29] == 0x29292929);
            CHECK(g_cpu.gpr[31] == 0x31313131);
            CHECK(!OS_HLE_InterruptsEnabled());
            CHECK((Memory::Read16(kMain + 0x1a2) & 2) == 0);
            CHECK(!g_mkwHostNiActive);
        }
        CHECK(TryGetCpuContext() == &outer);
        CHECK(outer.gpr[29] == 0xaaaa0029);
        CHECK(g_mkwHostNiActive);
    }
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(!g_mkwHostNiActive);
    OS__RestoreInterrupts_801a65d4(1);
    Enqueue(kMain);
    Switch(kWorker, &g_cpu);
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(g_cpu.gpr[29] == 0x29292929);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK(!g_mkwHostNiActive);
    CHECK(Manager::IsTerminated(kWorker));
    CHECK(g_liveStacks.empty());
}

void SwitchCallback(CpuContext* cpu)
{
    CHECK(cpu != &GetPersistentCpuContext());
    CHECK(TryGetCpuContext() == cpu);
    CHECK(cpu->gpr[3] == kWorker || cpu->gpr[3] == kOther);
    CHECK(cpu->gpr[4] == kOther || cpu->gpr[4] == kMain);
    cpu->gpr[3] = 0xcccc0003;
    cpu->gpr[4] = 0xcccc0004;
    cpu->gpr[29] = 0xcccc0029;
    cpu->gpr[31] = 0xcccc0031;
    cpu->fpscr = 4;
    MkwApplyHostNiMode(4);
}

void ReturningWorker(CpuContext* cpu)
{
    CHECK(TryGetCpuContext() == &GetPersistentCpuContext());
    CHECK(cpu->gpr[3] == 41);
    CHECK(OS_HLE_InterruptsEnabled());
    cpu->gpr[29] = 0x11110029;
    g_stage = 1;
    cpu->gpr[3] = kExitValue;
}

void FollowingWorker(CpuContext* cpu)
{
    CHECK(g_stage == 1);
    CHECK(Manager::IsTerminated(kWorker));
    CHECK(Memory::Read32(kWorker + kThreadExitValueOffset) == kExitValue);
    CHECK(Memory::Read32(kMutex + kMutexOwnerOffset) == 0);
    CHECK(Memory::Read32(kMutex + kMutexCountOffset) == 0);
    CHECK(Memory::Read16(kJoiner + kThreadStateOffset) == kThreadStateReady);
    CHECK(Memory::Read16(kMutexWaiter + kThreadStateOffset) == kThreadStateReady);
    CHECK(Memory::Read16(kMain + kThreadStateOffset) == kThreadStateWaiting);
    CHECK(!OS_HLE_InterruptsEnabled());
    CHECK(!g_mkwHostNiActive);
    CHECK(cpu->gpr[29] != 0xcccc0029);
    g_stage = 2;
    Enqueue(kMain);
    cpu->gpr[3] = 0x55;
}

void TestExitSchedulingAndCleanup()
{
    Fixture fixture;
    Register(kEntry, ReturningWorker);
    Register(kOtherEntry, FollowingWorker);
    Register(kCallback, SwitchCallback);
    Memory::Write32(kSwitchThreadCallbackPtrAddr, kCallback);
    Create(kWorker, kEntry, 41, 3);
    Create(kOther, kOtherEntry, 0, 5, 0x1032);
    SeedThread(kJoiner, 8);
    SeedThread(kMutexWaiter, 8);
    WaitOn(kJoiner, kWorker + kThreadJoinQueueOffset);
    WaitOn(kMutexWaiter, kMutex);
    Memory::Write32(kMutex + kMutexOwnerOffset, kWorker);
    Memory::Write32(kMutex + kMutexCountOffset, 2);
    Memory::Write32(kWorker + kThreadMutexQueueOffset, kMutex);
    Memory::Write32(kWorker + kThreadMutexTailOffset, kMutex);
    Memory::Write16(kOther + kThreadAttrOffset, 1);
    Memory::Write32(kThreadListHeadAddr, kMain);
    Memory::Write32(kThreadListTailAddr, kOther);
    Memory::Write32(kMain + kThreadListNextOffset, kWorker);
    Memory::Write32(kWorker + kThreadListPrevOffset, kMain);
    Memory::Write32(kWorker + kThreadListNextOffset, kOther);
    Memory::Write32(kOther + kThreadListPrevOffset, kWorker);
    Memory::Write16(kMain + kThreadStateOffset, kThreadStateWaiting);
    Enqueue(kOther);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 2);
    CHECK(g_unlockCalls == 2);
    CHECK(Memory::Read16(kOther + kThreadStateOffset) == 0);
    CHECK(Memory::Read32(kThreadListTailAddr) == kWorker);
    CHECK(Memory::Read32(kWorker + kThreadListNextOffset) == 0);
    CHECK(g_cpu.gpr[29] == 0x29292929);
    CHECK(g_cpu.gpr[31] == 0x31313131);
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK(!g_mkwHostNiActive);
    CHECK(g_liveStacks.empty());
    g_cpu.gpr[3] = kWorker;
    g_cpu.gpr[4] = kOutput;
    OSJoinThread_HLE_801aa3ac(&g_cpu);
    CHECK(g_cpu.gpr[3] == 1);
    CHECK(Memory::Read32(kOutput) == kExitValue);
    CHECK(Memory::Read16(kWorker + kThreadStateOffset) == 0);
}

void FirstMutexWaiter(CpuContext* cpu)
{
    CHECK(Memory::Read32(kSchedulerIdleFlagAddr) == 0);
    CHECK(Memory::Read32(kMutex + kMutexOwnerOffset) == 0);
    CHECK(Memory::Read32(kSecondMutex + kMutexOwnerOffset) == kWorker);
    CHECK(Memory::Read32(kWorker + kThreadMutexQueueOffset) == kSecondMutex);
    CHECK(Memory::Read32(kWorker + kThreadJoinQueueOffset) == kJoiner);
    CHECK(Memory::Read16(kJoiner + kThreadStateOffset) == kThreadStateWaiting);
    CHECK(Memory::Read16(kMutexWaiter + kThreadStateOffset) == kThreadStateWaiting);
    CHECK(Manager::IsTerminated(kWorker));
    CHECK(g_stage == 1);
    ++g_stage;
    if (cpu->gpr[3] == 0) Enqueue(kMain);
    cpu->gpr[3] = kExitValue;
}

void SeedCleanupWaiters(bool cancelOther)
{
    Register(kEntry, ReturningWorker);
    Register(kOtherEntry, FirstMutexWaiter);
    Create(kWorker, kEntry, 41, 3);
    Create(kOther, kOtherEntry, cancelOther ? 1 : 0, 1);
    SeedThread(kJoiner, 2);
    SeedThread(kMutexWaiter, 4);
    WaitOn(kOther, kMutex);
    WaitOn(kMutexWaiter, kSecondMutex);
    WaitOn(kJoiner, kWorker + kThreadJoinQueueOffset);
    for (uint32_t thread : {kOther, kJoiner, kMutexWaiter}) {
        Memory::Write32(thread + kThreadSuspendOffset, 0);
    }
    Memory::Write32(kMutex + kMutexOwnerOffset, kWorker);
    Memory::Write32(kMutex + kMutexCountOffset, 1);
    Memory::Write32(kMutex + kMutexThreadNextOffset, kSecondMutex);
    Memory::Write32(kSecondMutex + kMutexOwnerOffset, kWorker);
    Memory::Write32(kSecondMutex + kMutexCountOffset, 1);
    Memory::Write32(kSecondMutex + kMutexThreadPrevOffset, kMutex);
    Memory::Write32(kWorker + kThreadMutexQueueOffset, kMutex);
    Memory::Write32(kWorker + kThreadMutexTailOffset, kSecondMutex);
}

void TestExitPreemptsAtFirstWakeup()
{
    Fixture fixture;
    SeedCleanupWaiters(false);
    Memory::Write16(kMain + kThreadStateOffset, kThreadStateWaiting);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 2);
    CHECK(g_unlockCalls == 2);
    CHECK(Memory::Read32(kWorker + kThreadExitValueOffset) == kExitValue);
    CHECK(Memory::Read32(kSecondMutex + kMutexOwnerOffset) == kWorker);
    CHECK(Memory::Read16(kJoiner + kThreadStateOffset) == kThreadStateWaiting);
    CHECK(Memory::Read16(kMutexWaiter + kThreadStateOffset) == kThreadStateWaiting);
    CHECK(g_liveStacks.empty());
}

void TestCancelOtherPreemptsDuringCleanup()
{
    Fixture fixture;
    SeedCleanupWaiters(true);
    Memory::Write32(kJoiner + kThreadSuspendOffset, 1);
    Memory::Write32(kMutexWaiter + kThreadSuspendOffset, 1);
    Memory::Write32(kWorker + kThreadSuspendOffset, 1);
    Memory::Write32(kWorker + kThreadExitValueOffset, 0x5678);
    g_stage = 1;
    g_cpu.gpr[3] = kWorker;
    OSCancelThread_HLE_801aa1d4(&g_cpu);
    CHECK(g_stage == 2);
    CHECK(g_unlockCalls == 2);
    CHECK(Memory::Read32(kSecondMutex + kMutexOwnerOffset) == 0);
    CHECK(Memory::Read32(kWorker + kThreadMutexQueueOffset) == 0);
    CHECK(Memory::Read32(kWorker + kThreadJoinQueueOffset) == 0);
    CHECK(Memory::Read32(kWorker + kThreadExitValueOffset) == 0x5678);
    CHECK(Memory::Read16(kJoiner + kThreadStateOffset) == kThreadStateReady);
    CHECK(Memory::Read16(kMutexWaiter + kThreadStateOffset) == kThreadStateReady);
    CHECK(Manager::GetCurrentGuestThread() == kMain);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK(g_liveStacks.empty());
}

void ExplicitExit(CpuContext* cpu)
{
    cpu->gpr[3] = kExitValue;
    OSExitThread_HLE_801aa0f0(cpu);
    CHECK(false);
}

void TestRepeatedExplicitExits()
{
    Fixture fixture;
    Register(kEntry, ExplicitExit);
    const size_t before = g_destroyed;
    for (int i = 0; i < 32; ++i) {
        Create(kWorker, kEntry, 0, 3);
        Enqueue(kMain);
        Switch(kWorker, &g_cpu);
        CHECK(g_liveStacks.empty());
        CHECK(g_destroyed == before + i + 1);
        CHECK(Manager::IsTerminated(kWorker));
        CHECK(Manager::GetFiber(kWorker)->fiber == nullptr);
        CHECK(Memory::Read32(kWorker + kThreadExitValueOffset) == kExitValue);
    }
}

void TestInactiveReplacementAndCancel()
{
    Fixture fixture;
    Register(kEntry, ExplicitExit);
    const size_t before = g_destroyed;
    Create(kWorker, kEntry, 0, 3);
    const auto originalGeneration = Manager::GetThreadGeneration(kWorker);
    CHECK(Manager::CreateGuestFiber(kWorker, kEntry, 0, 0x80101000));
    const auto replacedGeneration = Manager::GetThreadGeneration(kWorker);
    CHECK(replacedGeneration != originalGeneration);
    CHECK(g_destroyed == before + 1);
    CHECK(g_liveStacks.size() == 1);
    Memory::Write32(kSchedulerIdleFlagAddr, 3);
    g_cpu.gpr[3] = kWorker;
    OSCancelThread_HLE_801aa1d4(&g_cpu);
    CHECK(g_destroyed == before + 2);
    CHECK(g_liveStacks.empty());
    CHECK(Manager::IsTerminated(kWorker));
    CHECK(Manager::GetThreadGeneration(kWorker) != replacedGeneration);
    CHECK(Memory::Read32(kSchedulerIdleFlagAddr) == 3);
    CHECK(Manager::GetCurrentGuestThread() == kMain);
}

void ExitInsideCallback(CpuContext* cpu)
{
    CpuContext scratch = *cpu;
    scratch.gpr[29] = 0xdead0029;
    scratch.gpr[3] = kExitValue;
    CpuContextScope scope(&scratch);
    OSExitThread_HLE_801aa0f0(&scratch);
    CHECK(false);
}

void AfterCallbackOwnerExit(CpuContext* cpu)
{
    CHECK(g_liveStacks.size() == 1);
    CHECK(TryGetCpuContext() == cpu);
    CHECK(cpu == &GetPersistentCpuContext());
    CHECK(cpu->gpr[29] != 0xdead0029);
    TryGetCpuContext()->gpr[31] = 0x56780031;
    CHECK(cpu->gpr[31] == 0x56780031);
    g_stage = 1;
    Enqueue(kMain);
    cpu->gpr[3] = kExitValue;
}

void RecordNandCallback(CpuContext* cpu)
{
    CHECK(cpu->gpr[3] == 23 && cpu->gpr[4] == 42);
    CHECK(!NandProcessPendingCallbacks(cpu, 1));
    ++g_stage;
}

void DrainNandAndExit(CpuContext* cpu)
{
    NandProcessPendingCallbacks(cpu, 1);
    CHECK(false);
}

void DrainAfterOwnerExit(CpuContext* cpu)
{
    CHECK(NandProcessPendingCallbacks(cpu, 1));
    CHECK(g_stage == 1);
    CHECK(!NandProcessPendingCallbacks(cpu, 1));
    Enqueue(kMain);
}

void TestNandCallbackOwnerExit()
{
    Fixture fixture;
    Register(kEntry, DrainNandAndExit);
    Register(kOtherEntry, DrainAfterOwnerExit);
    Register(kCallback, ExitInsideCallback);
    Register(kCallback + 4, RecordNandCallback);
    NandQueueIosCallback(kCallback, 0, 0);
    NandQueueIosCallback(kCallback + 4, 23, 42);
    Create(kWorker, kEntry, 0, 3);
    Create(kOther, kOtherEntry, 0, 5);
    Memory::Write16(kMain + kThreadStateOffset, kThreadStateWaiting);
    Enqueue(kOther);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 1 && g_liveStacks.empty());
}

void YieldNandCallback(CpuContext* cpu)
{
    CHECK(!NandProcessPendingCallbacks(cpu, 1));
    Switch(kMain, cpu);
    CHECK(!NandProcessPendingCallbacks(cpu, 1));
    CHECK(g_stage == 1);
    ++g_stage;
}

void DrainYieldingNand(CpuContext* cpu)
{
    CHECK(NandProcessPendingCallbacks(cpu, 1));
    CHECK(g_stage == 2);
    Enqueue(kMain);
}

void TestYieldingNandCallback()
{
    Fixture fixture;
    Register(kEntry, DrainYieldingNand);
    Register(kCallback, YieldNandCallback);
    Register(kCallback + 4, RecordNandCallback);
    NandQueueIosCallback(kCallback, 0, 0);
    NandQueueIosCallback(kCallback + 4, 23, 42);
    Create(kWorker, kEntry, 0, 3);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 0);
    CHECK(NandProcessPendingCallbacks(&g_cpu, 1));
    CHECK(g_stage == 1);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 2 && g_liveStacks.empty());
}

void TestCallbackOwnerExit()
{
    Fixture fixture;
    Register(kEntry, ExitInsideCallback);
    Register(kOtherEntry, AfterCallbackOwnerExit);
    Create(kWorker, kEntry, 0, 3);
    Create(kOther, kOtherEntry, 0, 5);
    Memory::Write16(kMain + kThreadStateOffset, kThreadStateWaiting);
    Enqueue(kOther);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 1);
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(g_cpu.gpr[29] == 0x29292929);
    CHECK(g_cpu.gpr[31] == 0x31313131);
    CHECK(g_liveStacks.empty());
}

void Replacement(CpuContext* cpu)
{
    CHECK(cpu->gpr[3] == 91);
    CHECK(TryGetCpuContext() == &GetPersistentCpuContext());
    g_stage = 2;
    cpu->gpr[3] = kExitValue;
}

void ReusingWorker(CpuContext* cpu)
{
    CHECK(Manager::CreateGuestFiber(kWorker, kOtherEntry, 91, 0x80101000));
    CHECK(!g_liveStacks.empty());
    CHECK(!Manager::IsTerminated(kWorker));
    CHECK(Manager::GetFiber(kWorker)->cpuContext.gpr[3] == 91);
    g_stage = 1;
    cpu->gpr[29] = 0xdead0029;
    if (!g_reuseReturns) {
        Switch(kMain, cpu);
        CHECK(false);
    }
}

void TestActiveReuse(bool returnFromEntry)
{
    Fixture fixture;
    g_reuseReturns = returnFromEntry;
    Register(kEntry, ReusingWorker);
    Register(kOtherEntry, Replacement);
    Create(kWorker, kEntry, 0, 3);
    const size_t before = g_destroyed;
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 1);
    CHECK(g_destroyed == before + 1);
    CHECK(!Manager::IsTerminated(kWorker));
    CHECK(Manager::GetFiber(kWorker)->cpuContext.gpr[3] == 91);
    CHECK(Manager::GetFiber(kWorker)->cpuContext.gpr[29] == 0);
    CHECK(g_cpu.gpr[29] == 0x29292929);
    SetIdentity(kMain);
    Enqueue(kMain);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 2);
    CHECK(g_destroyed == before + 2);
    CHECK(g_liveStacks.empty());
}

void DeferredEntry(CpuContext* cpu)
{
    CHECK(cpu == &GetPersistentCpuContext());
    CHECK(TryGetCpuContext() == cpu);
    CHECK(cpu->gpr[3] == kObject);
    CHECK(OS_HLE_InterruptsEnabled());
    g_stage = 1;
    cpu->gpr[3] = kExitValue;
}

void TestDirectSchedulerYield()
{
    Fixture fixture;
    Register(0x8024373c, DeferredEntry);
    Create(kWorker, 0x8024373c, kObject, 3);
    OS__DisableInterrupts_801a65ac();
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 0);
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(g_cpu.gpr[29] == 0x29292929);
    CHECK(!OS_HLE_InterruptsEnabled());
    CHECK((Memory::Read16(kWorker + 0x1a2) & 2) != 0);
    Memory::Write32(kObject, kVtable);
    Memory::Write32(kVtable + 0xc, kEntry);
    SetIdentity(kMain);
    Enqueue(kMain);
    Switch(kWorker, &g_cpu);
    CHECK(g_stage == 1);
    CHECK(!OS_HLE_InterruptsEnabled());
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(g_liveStacks.empty());
}

void DisabledSchedulerEntry(CpuContext* cpu)
{
    Memory::Write32(kSchedulerIdleFlagAddr, 1);
    cpu->gpr[3] = kExitValue;
}

void DisabledSchedulerExplicitEntry(CpuContext* cpu)
{
    DisabledSchedulerEntry(cpu);
    OSExitThread_HLE_801aa0f0(cpu);
}

void TestDisabledSchedulerFallback(bool explicitExit)
{
    Fixture fixture;
    Register(kEntry, explicitExit ? DisabledSchedulerExplicitEntry : DisabledSchedulerEntry);
    Create(kWorker, kEntry, 0, 3);
    Switch(kWorker, &g_cpu);
    CHECK(g_unlockCalls == 1);
    CHECK(Manager::IsTerminated(kWorker));
    CHECK(Memory::Read32(kWorker + kThreadExitValueOffset) == kExitValue);
    CHECK(Memory::Read32(kOSRunningContextAddr) == 0);
    CHECK(Memory::Read32(kOSCurrentContextAddr) == 0);
    CHECK(TryGetCpuContext() == &g_cpu);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK(g_liveStacks.empty());
}

void TestNullAmbientAndSameFiber()
{
    Fixture fixture;
    Register(kEntry, ExplicitExit);
    Create(kWorker, kEntry, 0, 3, 0x1032);
    g_currentCpuContext = nullptr;
    MkwApplyHostNiMode(4);
    const uint32_t control = MkwGetHostFpControl();
    Manager::SwitchToThread(kMain, &g_cpu);
    CHECK(g_currentCpuContext == nullptr);
    CHECK(MkwGetHostFpControl() == control);
    Enqueue(kMain);
    Switch(kWorker, &g_cpu);
    CHECK(g_currentCpuContext == nullptr);
    CHECK(MkwGetHostFpControl() == control);
    CHECK(g_mkwHostNiActive);
    CHECK(OS_HLE_InterruptsEnabled());
    CHECK(g_liveStacks.empty());
}
} // namespace

namespace HostContext {
using Handle = void*;
using Entry = void (*)(void*);
bool InitializeScheduler(Handle* scheduler) { return FiberTestBackend::InitializeScheduler(scheduler); }
void ShutdownScheduler(Handle scheduler) { FiberTestBackend::ShutdownScheduler(scheduler); }
Handle Create(size_t size, Entry entry, void* argument)
{
    Handle handle = FiberTestBackend::Create(size, entry, argument);
    if (handle) {
        CHECK(g_liveStacks.insert(handle).second);
        ++g_created;
    }
    return handle;
}
bool IsCurrent(Handle handle) { return FiberTestBackend::IsCurrent(handle); }
void Destroy(Handle handle)
{
    CHECK(!IsCurrent(handle));
    CHECK(g_liveStacks.erase(handle) == 1);
    ++g_destroyed;
    FiberTestBackend::Destroy(handle);
}
void Switch(Handle target) { FiberTestBackend::Switch(target); }
} // namespace HostContext

CpuContext& GetPersistentCpuContext() { return g_cpu; }
const TranslatedFunctionInfo* TranslatedFunctionRegistry::FindByAddressPtr(uint32_t address)
{
    const auto it = g_entries.find(address);
    return it == g_entries.end() ? nullptr : &it->second;
}
void InvokeIndirectCpu(uint32_t address, CpuContext* cpu)
{
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
    CHECK(info != nullptr);
    if (TryGetCpuContext() == cpu) {
        info->entry(cpu);
    } else {
        CpuContextScope scope(cpu);
        info->entry(cpu);
    }
}
[[noreturn]] void InvokeIndirectJump(uint32_t, CpuContext*) { std::abort(); }
void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::abort(); }

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error(std::string(reason)), address_(address), length_(length), reason_(reason) {}
bool Memory::Contains(uint32_t address, size_t length)
{
    return address >= 0x80000000 && address - 0x80000000 < g_memory.size() &&
           length <= g_memory.size() - (address - 0x80000000);
}
uint8_t* Memory::GetPointer(uint32_t address, size_t length)
{
    if (!Contains(address, length)) throw AccessViolation(address, length, "test memory bounds");
    return g_memory.data() + address - 0x80000000;
}
uint8_t* Memory::GetPointer(uint32_t address) { return GetPointer(address, 1); }
uint16_t MemoryInline::Read16Slow(uint32_t address)
{
    const uint8_t* p = Memory::GetPointer(address, 2);
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t MemoryInline::Read32Slow(uint32_t address)
{
    const uint8_t* p = Memory::GetPointer(address, 4);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void MemoryInline::Write16Slow(uint32_t address, uint16_t value)
{
    uint8_t* p = Memory::GetPointer(address, 2);
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}
void MemoryInline::Write32Slow(uint32_t address, uint32_t value)
{
    uint8_t* p = Memory::GetPointer(address, 4);
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

namespace OsHleInternal {
std::atomic<bool> g_interrupts_enabled{true};
bool ProcessSleepTimers(CpuContext*) { return false; }
bool ProcessAlarmQueue(CpuContext*, int) { return false; }
void CancelSleepTimer(uint32_t) {}
void ClearOutstandingPark(uint32_t) {}
} // namespace OsHleInternal

bool OS_HLE_InterruptsEnabled() noexcept { return g_interrupts_enabled.load(); }
void OS_HLE_RestoreInterruptMirror(bool enabled) noexcept { g_interrupts_enabled.store(enabled); }
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level)
{
    const bool previous = g_interrupts_enabled.exchange(level != 0);
    const uint32_t current = Memory::Read32(kOSCurrentContextAddr);
    if (current != 0) {
        const uint16_t flags = Memory::Read16(current + 0x1a2);
        Memory::Write16(current + 0x1a2, static_cast<uint16_t>(level ? flags | 2 : flags & ~2));
    }
    return previous ? 1 : 0;
}
extern "C" int32_t OS__DisableInterrupts_801a65ac() { return OS__RestoreInterrupts_801a65d4(0); }
extern "C" int32_t OS__EnableInterrupts_801a65c0() { return OS__RestoreInterrupts_801a65d4(1); }
extern "C" void OS__SetCurrentContext_801a1e70(uint32_t context)
{
    Memory::Write32(kOSCurrentContextAddr, context);
}
extern "C" void OS__ClearContext_801a2098(uint32_t context)
{
    Memory::Write16(context + 0x1a0, 0);
    Memory::Write16(context + 0x1a2, 0);
}
extern "C" [[noreturn]] void OS__LoadContext_801a1f58(CpuContext*) { std::abort(); }
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext*) { std::abort(); }

namespace GuestFunctions {
Entry BaseTranslation(uint32_t address)
{
    CHECK(address == 0x801a1ed8);
    return +[](CpuContext* cpu) { cpu->gpr[3] = 0; };
}
} // namespace GuestFunctions
void VI_HLE_ForceRetrace(CpuContext*) {}
void VI_HLE_PollRetrace(CpuContext*) {}
void VI_HLE_WaitForNextRetracePoll() { std::abort(); }
bool VI_HLE_IsAdvancingRetrace() { return false; }
void Audio_HLE_Poll(CpuContext*) {}

int main()
{
    const auto previousMainGeneration = Manager::GetThreadGeneration(kMain);
    TestNestedScratch();
    CHECK(Manager::GetThreadGeneration(kMain) != previousMainGeneration);
    TestExitSchedulingAndCleanup();
    TestExitPreemptsAtFirstWakeup();
    TestCancelOtherPreemptsDuringCleanup();
    TestRepeatedExplicitExits();
    TestInactiveReplacementAndCancel();
    TestCallbackOwnerExit();
    TestNandCallbackOwnerExit();
    TestYieldingNandCallback();
    TestActiveReuse(false);
    TestActiveReuse(true);
    TestDirectSchedulerYield();
    TestDisabledSchedulerFallback(false);
    TestDisabledSchedulerFallback(true);
    TestNullAmbientAndSameFiber();
}
