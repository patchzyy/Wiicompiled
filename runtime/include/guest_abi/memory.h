#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Memory {
public:
    using DeferredReadCallback = bool (*)(void* user);

    static constexpr size_t kMem1Size = 24u * 1024u * 1024u;
    static constexpr size_t kMem2Size = 128u * 1024u * 1024u;
    static constexpr uint32_t kMem1PhysicalBase = 0x00000000u;
    static constexpr uint32_t kMem1CachedBase = 0x80000000u;
    static constexpr uint32_t kMem1UncachedBase = 0xC0000000u;
    static constexpr uint32_t kMem2PhysicalBase = 0x10000000u;
    static constexpr uint32_t kMem2CachedBase = 0x90000000u;
    static constexpr uint32_t kMem2UncachedBase = 0xD0000000u;
    static constexpr uint32_t kMem2PhysicalEnd =
        kMem2PhysicalBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2CachedEnd =
        kMem2CachedBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2UncachedEnd =
        kMem2UncachedBase + static_cast<uint32_t>(kMem2Size);

    struct RegionConfig {
        std::string name;
        uint32_t baseAddress = 0;
        size_t sizeBytes = 0;
    };

    struct Config {
        std::vector<RegionConfig> regions;
        static Config WiiDefaults();
    };

    class AccessViolation : public std::runtime_error {
    public:
        AccessViolation(uint32_t address, size_t length, std::string_view reason);

        uint32_t address() const noexcept { return address_; }
        size_t length() const noexcept { return length_; }
        std::string_view reason() const noexcept { return reason_; }

    private:
        uint32_t address_ = 0;
        size_t length_ = 0;
        std::string reason_;
    };

    static void Init(const Config& config);
    // Single flat MEM1 region. Not used by the shipped runtime, but the
    // translator integration-test harnesses emit calls to it.
    static void Init(size_t mem1Size);
    static void Reset();
    // Executable ranges are normally registered during startup. Rebuild the
    // writable fast-path classification after each registration so a page
    // previously classified as ordinary data cannot retain a stale direct
    // write bias.
    static void RefreshWritableFastPathsForExecutableRanges();

    static uint8_t Read8(uint32_t addr);
    static uint16_t Read16(uint32_t addr);
    static uint32_t Read32(uint32_t addr);
    static uint64_t Read64(uint32_t addr);
    static float ReadFloat32(uint32_t addr);
    static double ReadFloat64(uint32_t addr);
    static void Write8(uint32_t addr, uint8_t val);
    static void Write16(uint32_t addr, uint16_t val);
    static void Write32(uint32_t addr, uint32_t val);
    static void Write64(uint32_t addr, uint64_t val);
    static void WriteFloat32(uint32_t addr, double val);
    static void WriteFloat64(uint32_t addr, double val);

    // Exception-safe scalar access for HLE code. These keep Read32/Write32's
    // full mapping behavior and only convert an unmapped address into a failure
    // result; they are deliberately not MemoryInline::Try*GuestScalar, which is
    // the translated-code fast path over the page table.
    static bool TryRead32(uint32_t addr, uint32_t& value) noexcept {
        try {
            value = Read32(addr);
            return true;
        } catch (const AccessViolation&) {
            return false;
        }
    }

    static bool TryWrite32(uint32_t addr, uint32_t value) noexcept {
        try {
            Write32(addr, value);
            return true;
        } catch (const AccessViolation&) {
            return false;
        }
    }

    static uint8_t* GetPointer(uint32_t addr);
    static uint8_t* GetPointer(uint32_t addr, size_t length);
    static bool Contains(uint32_t addr, size_t length = 1);

    static uint64_t RegisterDeferredRead(uint32_t addr, size_t length,
                                         DeferredReadCallback callback, void* user);
    static void ClearDeferredReads();

    // sizeBytes reports the storage actually allocated, which is not always the
    // configured size (aliased MEM1/MEM2 windows are clamped to what is behind
    // them). Used by the crash dump.
    static std::vector<RegionConfig> DescribeRegions();
};

#include "guest_abi/memory_access.h"
