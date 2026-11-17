#include "abi_bridge.h"
#include "system_bridge.h"
#include "memory_slow_test_support.h"
#include "recomp_mod_loader.h"
#include "runtime_product.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
CpuContext g_cpu{};
std::unordered_map<uint32_t, uint32_t> g_words;
std::vector<uint32_t> g_calls;
unsigned g_unwound = 0;
unsigned g_caught = 0;
bool g_invalidContext = false;
constexpr uint32_t kCtorBase = 0x80100000;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct UnwindCounter {
    ~UnwindCounter() { ++g_unwound; }
};
}

Memory::Config Memory::Config::WiiDefaults() { return {}; }
void Memory::Init(const Config&) { g_words.clear(); }
Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error(std::string(reason)), address_(address), length_(length), reason_(reason) {}
bool Memory::Contains(uint32_t, size_t) { return false; }
uint8_t* Memory::GetPointer(uint32_t address, size_t length) {
    throw AccessViolation(address, length, "unmapped test memory");
}
uint32_t MemoryInline::Read32Slow(uint32_t address) {
    const auto found = g_words.find(address);
    if (found == g_words.end()) throw Memory::AccessViolation(address, 4, "unmapped test word");
    return found->second;
}
void MemoryInline::Write32Slow(uint32_t address, uint32_t value) { g_words[address] = value; }

namespace RecompMod {
const std::vector<MemoryReservation>& MemoryReservations() {
    static const std::vector<MemoryReservation> empty;
    return empty;
}
uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
}
namespace RuntimeProduct {
const Descriptor& Active() noexcept {
    static const Descriptor descriptor{Kind::BaseGame, "test"};
    return descriptor;
}
}
extern "C" {
extern const uint8_t kMkwGuestRegion = 0;
extern const uint32_t kGuestMapSymbolCount = 0;
extern const uint32_t kGuestMapSymbolAddresses[] = {0};
extern const char* const kGuestMapSymbolNames[] = {""};
void InitializeDataSections() {
    for (const uint32_t start : {MKW_GADDR(80244DE0), MKW_GADDR(8088f400)}) {
        for (uint32_t i = 0; i < 6; ++i) g_words[start + 4 * i] = kCtorBase + 4 * i;
        g_words[start + 24] = 0;
        g_words[start + 28] = 0xffffffff;
        g_words[start + 32] = 0xdeadbeef;
    }
}
}
void InitializePersistentCpuContext() { g_cpu = {}; }
CpuContext& GetPersistentCpuContext() { return g_cpu; }
std::optional<TranslatedFunctionInfo> FindNearestTranslatedFunction(uint32_t) { return std::nullopt; }
const TranslatedFunctionInfo* TranslatedFunctionRegistry::FindByAddressPtr(uint32_t address) {
    static const TranslatedFunctionInfo info{kCtorBase, "test ctor"};
    return address >= kCtorBase && address < kCtorBase + 24 ? &info : nullptr;
}
void InvokeIndirectCpu(uint32_t address, CpuContext* cpu) {
    g_invalidContext |= cpu != &g_cpu || g_currentCpuContext != cpu ||
        cpu->gpr[1] != 0x81700000 || !g_suppressSehReporting || g_sehJumpTarget == nullptr;
    g_calls.push_back(address);
    cpu->gpr[1] = 0;
    const uint32_t index = (address - kCtorBase) / 4;
    if (index == 4) {
#ifdef _WIN32
        longjmp(*g_sehJumpTarget, 1);
#else
        siglongjmp(*g_sehJumpTarget, 1);
#endif
    }
    UnwindCounter counter;
    switch (index) {
    case 0: throw std::runtime_error("escaped constructor");
    case 1: throw Memory::AccessViolation(0xdeadbeef, 4, "escaped memory fault");
    case 2: throw 42;
    case 3:
        try { throw std::runtime_error("handled inside constructor"); }
        catch (const std::runtime_error&) { ++g_caught; }
        break;
    default: break;
    }
}

int main() {
    std::ostringstream log;
    auto* saved = std::cerr.rdbuf(log.rdbuf());
    try {
        CpuContext caller{};
        g_currentCpuContext = &caller;
        SystemBridge::Initialize();
        Require(!g_invalidContext, "constructor context or recovery target was not prepared");
        Require(g_calls.size() == 12, "a constructor failure skipped later constructors");
        for (size_t i = 0; i < g_calls.size(); ++i)
            Require(g_calls[i] == kCtorBase + 4 * (i % 6), "constructor ordering changed");
        Require(g_unwound == 10 && g_caught == 2, "C++ exceptions did not unwind or remain locally catchable");
        Require(!g_suppressSehReporting && g_sehJumpTarget == nullptr, "constructor recovery state leaked");
        Require(g_currentCpuContext == &caller, "constructor scope did not restore the caller context");
        Require(log.str().find("Executed 2 main DOL static constructors.") != std::string::npos &&
                log.str().find("Executed 2 static constructors.") != std::string::npos,
                "failed constructors were counted as successful");
        g_currentCpuContext = nullptr;
    } catch (const std::exception& error) {
        std::cerr.rdbuf(saved);
        std::cerr << error.what() << '\n' << log.str();
        return 1;
    }
    std::cerr.rdbuf(saved);
}
