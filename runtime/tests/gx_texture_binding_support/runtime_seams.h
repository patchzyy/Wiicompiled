#pragma once

#include "../memory_slow_test_support.h"

#include <deque>

extern "C" const uint8_t kMkwGuestRegion = 0;

namespace TextureBindingTest {
std::array<uint8_t, 4 * 1024 * 1024> memory{};
uint32_t unreadableObject = 0;
uint32_t unreadableObjectAccesses = 0;
uint64_t rangeChecks = 0;
std::vector<std::pair<uint32_t, uint32_t>> retiredRanges;

uint8_t* Pointer(uint32_t address, size_t length) {
    const uint32_t offset = CanonicalizeGxMainRamAddress(address);
    if (unreadableObject != 0 && offset >= unreadableObject && offset < unreadableObject + 32) {
        ++unreadableObjectAccesses;
        throw Memory::AccessViolation(address, length, "test unmapped range");
    }
    if (offset > memory.size() || length > memory.size() - offset) {
        throw Memory::AccessViolation(address, length, "test unmapped range");
    }
    return memory.data() + offset;
}

struct Command {
    std::unique_ptr<std::byte[]> storage;
    void (*run)(void*);
};
std::deque<Command> commands;
} // namespace TextureBindingTest

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error(std::string(reason)), address_(address), length_(length), reason_(reason) {}

bool Memory::Contains(uint32_t address, size_t length) {
    ++TextureBindingTest::rangeChecks;
    try {
        TextureBindingTest::Pointer(address, length);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}
uint8_t* Memory::GetPointer(uint32_t address) { return TextureBindingTest::Pointer(address, 1); }
uint8_t* Memory::GetPointer(uint32_t address, size_t length) { return TextureBindingTest::Pointer(address, length); }

uint8_t MemoryInline::Read8Slow(uint32_t address) { return *TextureBindingTest::Pointer(address, 1); }
uint16_t MemoryInline::Read16Slow(uint32_t address) { return BigEndian::Read16(TextureBindingTest::Pointer(address, 2)); }
uint32_t MemoryInline::Read32Slow(uint32_t address) { return BigEndian::Read32(TextureBindingTest::Pointer(address, 4)); }
uint64_t MemoryInline::Read64Slow(uint32_t address) {
    return (static_cast<uint64_t>(Read32Slow(address)) << 32) | Read32Slow(address + 4);
}
void MemoryInline::Write8Slow(uint32_t address, uint8_t value) { *TextureBindingTest::Pointer(address, 1) = value; }
void MemoryInline::Write16Slow(uint32_t address, uint16_t value) {
    BigEndian::Write16(TextureBindingTest::Pointer(address, 2), value);
}
void MemoryInline::Write32Slow(uint32_t address, uint32_t value) {
    BigEndian::Write32(TextureBindingTest::Pointer(address, 4), value);
}
void MemoryInline::WriteFloat32Slow(uint32_t address, double value) {
    const float narrowed = static_cast<float>(value);
    uint32_t bits;
    std::memcpy(&bits, &narrowed, sizeof(bits));
    Write32Slow(address, bits);
}

namespace GxWorker {
namespace detail {
std::atomic<bool> g_active{false};
constinit thread_local bool t_onWorker = false;
constinit thread_local int t_inlineDepth = 0;
bool g_fifoPending = false;

void* Reserve(size_t size, void (*run)(void*)) {
    auto storage = std::make_unique<std::byte[]>(size);
    void* result = storage.get();
    TextureBindingTest::commands.push_back({std::move(storage), run});
    return result;
}
void Commit() {}
void FlushFifo() { g_fifoPending = false; }
} // namespace detail

void Sync() {
    const bool wasWorker = detail::t_onWorker;
    detail::t_onWorker = true;
    while (!TextureBindingTest::commands.empty()) {
        auto command = std::move(TextureBindingTest::commands.front());
        TextureBindingTest::commands.pop_front();
        command.run(command.storage.get());
    }
    detail::t_onWorker = wasWorker;
}
void BeginInline() { Sync(); ++detail::t_inlineDepth; }
void EndInline() { --detail::t_inlineDepth; }
} // namespace GxWorker

void InvalidateEfbCopyDestinationsForRange(uint32_t address, uint32_t size) {
    TextureBindingTest::retiredRanges.emplace_back(address, size);
}
extern "C" void GxNotifyDisplayListMemoryWrite(uint32_t, uint32_t) {}
