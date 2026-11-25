#pragma once

// Instrumented ISA memory seam: no guest VM reservation or GPU is needed.
#include "big_endian.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace PsqTestMemory {
inline std::array<uint8_t, 512> bytes{};
inline uint32_t address = 0;
inline size_t width = 0;
inline unsigned accesses = 0;
inline bool directStack = false;
inline uint8_t* Pointer(uint32_t addr) { return bytes.data() + (addr & 255u); }
inline void Record(uint32_t addr, size_t size) { address = addr; width = size; ++accesses; }
template <typename T> T ReadHost(const uint8_t* host) {
    if constexpr (sizeof(T) == 1) return *host;
    else if constexpr (sizeof(T) == 2) return BigEndian::Read16(host);
    else if constexpr (sizeof(T) == 4) return BigEndian::Read32(host);
    else return (uint64_t(BigEndian::Read32(host)) << 32) | BigEndian::Read32(host + 4);
}
template <typename T> void WriteHost(uint8_t* host, T value) {
    if constexpr (sizeof(T) == 1) *host = value;
    else if constexpr (sizeof(T) == 2) BigEndian::Write16(host, value);
    else if constexpr (sizeof(T) == 4) BigEndian::Write32(host, value);
    else BigEndian::Write64(host, value);
}
template <typename T> T Read(uint32_t addr) {
    Record(addr, sizeof(T)); return ReadHost<T>(Pointer(addr));
}
template <typename T> void Write(uint32_t addr, T value) {
    Record(addr, sizeof(T)); WriteHost(Pointer(addr), value);
}
}

namespace GuestFlat {
inline bool RequiresCheckedAccess() noexcept { return true; }
}
#define MKW_FLAT_GUEST_BASE (PsqTestMemory::bytes.data())

class Memory {
public:
    static uint8_t Read8(uint32_t a) { return PsqTestMemory::Read<uint8_t>(a); }
    static uint16_t Read16(uint32_t a) { return PsqTestMemory::Read<uint16_t>(a); }
    static uint32_t Read32(uint32_t a) { return PsqTestMemory::Read<uint32_t>(a); }
    static uint64_t Read64(uint32_t a) { return PsqTestMemory::Read<uint64_t>(a); }
    static void Write8(uint32_t a, uint8_t v) { PsqTestMemory::Write(a, v); }
    static void Write16(uint32_t a, uint16_t v) { PsqTestMemory::Write(a, v); }
    static void Write32(uint32_t a, uint32_t v) { PsqTestMemory::Write(a, v); }
    static void Write64(uint32_t a, uint64_t v) { PsqTestMemory::Write(a, v); }
};

namespace MemoryInline {
inline bool FlatWriteNeedsPolicy(uint32_t) { return true; }
inline bool TryGetPointerFast(uint32_t a, size_t, uint8_t*& host) {
    host = PsqTestMemory::directStack ? PsqTestMemory::Pointer(a) : nullptr;
    return host != nullptr;
}
inline bool TryGetWritablePointerFast(uint32_t a, size_t n, uint8_t*& host) {
    return TryGetPointerFast(a, n, host);
}
template <typename T> T ReadResolvedFallback(uint32_t a) { return PsqTestMemory::Read<T>(a); }
template <typename T> void WriteResolvedFallback(uint32_t a, T v) { PsqTestMemory::Write(a, v); }
template <typename T> T ReadResolved(uint8_t* host, uint32_t o, uint32_t a) {
    return host ? PsqTestMemory::ReadHost<T>(host + o) : ReadResolvedFallback<T>(a);
}
template <typename T> void WriteResolved(uint8_t* host, uint32_t o, uint32_t a, T v) {
    if (host) PsqTestMemory::WriteHost(host + o, v); else WriteResolvedFallback(a, v);
}
#define PSQ_TEST_MEMORY_WIDTH(Bits, Type) \
inline Type ReadStack##Bits(uint32_t a) { \
    return PsqTestMemory::directStack ? PsqTestMemory::ReadHost<Type>(PsqTestMemory::Pointer(a)) : PsqTestMemory::Read<Type>(a); } \
inline void WriteStack##Bits(uint32_t a, Type v) { \
    if (PsqTestMemory::directStack) PsqTestMemory::WriteHost(PsqTestMemory::Pointer(a), v); else PsqTestMemory::Write(a, v); } \
inline Type ReadResolved##Bits(uint8_t* h, uint32_t o, uint32_t a) { return ReadResolved<Type>(h, o, a); } \
inline void WriteResolved##Bits(uint8_t* h, uint32_t o, uint32_t a, Type v) { WriteResolved(h, o, a, v); }
PSQ_TEST_MEMORY_WIDTH(8, uint8_t)
PSQ_TEST_MEMORY_WIDTH(16, uint16_t)
PSQ_TEST_MEMORY_WIDTH(32, uint32_t)
PSQ_TEST_MEMORY_WIDTH(64, uint64_t)
#undef PSQ_TEST_MEMORY_WIDTH
}
