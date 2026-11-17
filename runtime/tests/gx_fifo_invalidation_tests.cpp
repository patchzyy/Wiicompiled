#include "gx_test_common.hpp"
#include "../src/hle/gx/gx_internal.h"
#include "isa/big_endian.h"
#include "memory_slow_test_support.h"

#include <algorithm>

namespace aurora::gfx::testing {
extern std::vector<uint8_t> storageData;
}

namespace {
constexpr uint32_t kGuestBase = 0x80001000;
std::array<uint8_t, 4096> g_guestBytes{};
std::vector<std::vector<uint8_t>> g_deferredBursts;
}

// Host seams; the runtime parser and Aurora encoder/decoder are linked unchanged.
extern "C" const uint8_t kMkwGuestRegion = 0;
GxDisplayListState g_dlRecordState;
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};
bool g_alphaCompareValid = false;

namespace GxWorker {
namespace detail {
std::atomic<bool> g_active{false};
constinit thread_local bool t_onWorker = false;
constinit thread_local int t_inlineDepth = 0;
}
void FifoBurst(const uint8_t* bytes, uint32_t size) { g_deferredBursts.emplace_back(bytes, bytes + size); }
}

bool Memory::Contains(uint32_t address, size_t length) {
    return address >= kGuestBase && address - kGuestBase <= g_guestBytes.size() &&
           length <= g_guestBytes.size() - (address - kGuestBase);
}
uint8_t MemoryInline::Read8Slow(uint32_t address) { return g_guestBytes.at(address - kGuestBase); }
uint16_t MemoryInline::Read16Slow(uint32_t address) { return BigEndian::Read16(&g_guestBytes.at(address - kGuestBase)); }
uint32_t MemoryInline::Read32Slow(uint32_t address) { return BigEndian::Read32(&g_guestBytes.at(address - kGuestBase)); }
void MemoryInline::Write8Slow(uint32_t address, uint8_t value) { g_guestBytes.at(address - kGuestBase) = value; }
void MemoryInline::Write16Slow(uint32_t address, uint16_t value) {
    BigEndian::Write16(&g_guestBytes.at(address - kGuestBase), value);
}
void MemoryInline::Write32Slow(uint32_t address, uint32_t value) {
    BigEndian::Write32(&g_guestBytes.at(address - kGuestBase), value);
}
void* GuestToHostPtr(uint32_t address, size_t length) {
    return Memory::Contains(address, length) ? &g_guestBytes.at(address - kGuestBase) : nullptr;
}
void EnsureAuroraFrameActive() {}
void EnsureAuroraFrameForFifo() {}
void WriteDisplayListData(uint32_t value, uint32_t size) {
    size = size == 0 ? 4 : size;
    for (uint32_t i = 0; i < size; ++i) {
        Memory::Write8(g_dlRecordState.writePtr++, static_cast<uint8_t>(value >> (8 * (size - i - 1))));
        ++g_dlRecordState.count;
    }
}
extern "C" void GX__CallDisplayList_80172f64(uint32_t address, uint32_t size) {
    GXCallDisplayList(GuestToHostPtr(address, size), size);
}

extern void GxWorkerReplayFifoBurst(const uint8_t* bytes, uint32_t size);

namespace {
class RuntimeFifoInvalidationTest : public GXFifoTest {
protected:
    void SetUp() override {
        GXFifoTest::SetUp();
        g_hleGxState = {};
        g_dlRecordState = {};
        g_deferredBursts.clear();
        GxWorker::detail::g_active = false;
        GxWorker::detail::t_onWorker = false;
        g_alphaCompareValid = false;
    }

    void TearDown() override {
        GxWorker::detail::g_active = false;
        GxWorker::detail::t_onWorker = false;
        GXFifoTest::TearDown();
    }

    void seed_cache() {
        for (auto& array : gxState().arrays) {
            array.cachedRange = {32, 64};
        }
        gxState().dirty = 0;
    }

    void expect_invalidated() {
        for (const auto& array : gxState().arrays) {
            EXPECT_EQ(array.cachedRange.size, 0u);
        }
        EXPECT_NE(gxState().dirty & aurora::gx::DirtyImmediates, 0u);
    }
};

TEST_F(RuntimeFifoInvalidationTest, ScalarCommandClearsEveryCachedArray) {
    seed_cache();
    HleFifoWrite(0x48, 1);
    const auto bytes = capture_fifo();
    ASSERT_EQ(bytes, (std::vector<uint8_t>{0x48}));
    EXPECT_EQ(gxState().arrays[GX_VA_POS].cachedRange.size, 64u);
    decode_fifo(bytes);
    expect_invalidated();
}

TEST_F(RuntimeFifoInvalidationTest, BurstFastPrefixAndFallbackKeepCommandOrder) {
    const std::vector<uint8_t> stream{0x61, 0x41, 0, 0, 1, 0x48, 0x61, 0x42, 0, 0, 2, 0x48};
    GX_HLE_FIFO_WriteBurst(stream.data(), stream.size());
    EXPECT_EQ(capture_fifo(), stream);
    EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
}

TEST_F(RuntimeFifoInvalidationTest, PartialPacketPayloadDoesNotBecomeAnInvalidation) {
    const std::array<uint8_t, 4> prefix{0x08, 0x50, 0, 0};
    const std::array<uint8_t, 3> tail{0x48, 0, 0x48};
    GX_HLE_FIFO_WriteBurst(prefix.data(), prefix.size());
    EXPECT_TRUE(capture_fifo().empty());
    GX_HLE_FIFO_WriteBurst(tail.data(), tail.size());
    EXPECT_EQ(capture_fifo(), (std::vector<uint8_t>{0x48}));
    EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
}

TEST_F(RuntimeFifoInvalidationTest, DeferredBurstOwnsBytesUntilWorkerReplay) {
    GxWorker::detail::g_active = true;
    std::array<uint8_t, 1> stream{0x48};
    GX_HLE_FIFO_WriteBurst(stream.data(), stream.size());
    stream[0] = 0;
    EXPECT_TRUE(capture_fifo().empty());
    ASSERT_EQ(g_deferredBursts.size(), 1u);
    GxWorker::detail::t_onWorker = true;
    const auto& copy = g_deferredBursts.front();
    GxWorkerReplayFifoBurst(copy.data(), copy.size());
    EXPECT_EQ(capture_fifo(), (std::vector<uint8_t>{0x48}));
    EXPECT_EQ(g_deferredBursts.size(), 1u);
}

TEST_F(RuntimeFifoInvalidationTest, RecordingDefersInvalidationUntilPlayback) {
    g_dlRecordState = {kGuestBase, 32, kGuestBase, 0, true};
    HleFifoWrite(0x48, 1);
    EXPECT_TRUE(capture_fifo().empty());
    EXPECT_EQ(g_guestBytes[0], 0x48);
    EXPECT_EQ(g_dlRecordState.count, 1u);
    g_dlRecordState.active = false;
    GX_HLE_FIFO_WriteBurst(g_guestBytes.data(), 1);
    seed_cache();
    decode_fifo(capture_fifo());
    expect_invalidated();
}

TEST_F(RuntimeFifoInvalidationTest, CallPacketForwardsChildInvalidation) {
    g_guestBytes[0] = 0x48;
    const std::array<uint8_t, 9> call{0x40, 0x80, 0, 0x10, 0, 0, 0, 0, 1};
    GX_HLE_FIFO_WriteBurst(call.data(), call.size());
    seed_cache();
    decode_fifo(capture_fifo());
    expect_invalidated();
}

TEST_F(RuntimeFifoInvalidationTest, SameAddressUploadsNewVerticesAfterInvalidation) {
    auto& storage = aurora::gfx::testing::storageData;
    storage.clear();
    std::array<uint8_t, 9> vertices{1, 2, 3, 4, 5, 6, 7, 8, 9};
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
    GXSetArray(GX_VA_POS, vertices.data(), vertices.size(), 3, false);
    const auto draw = [&] {
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition1x8(0);
        GXPosition1x8(1);
        GXPosition1x8(2);
        GXEnd();
        decode_fifo(capture_fifo());
    };
    draw();
    const auto first = gxState().arrays[GX_VA_POS].cachedRange;
    ASSERT_EQ(first.size, vertices.size());
    ASSERT_LE(first.offset + first.size, storage.size());
    EXPECT_TRUE(std::equal(vertices.begin(), vertices.end(), storage.begin() + first.offset));

    vertices.fill(42);
    draw();
    EXPECT_EQ(gxState().arrays[GX_VA_POS].cachedRange.offset, first.offset);
    EXPECT_NE(storage[first.offset], 42);

    HleFifoWrite(0x48, 1);
    draw();
    const auto second = gxState().arrays[GX_VA_POS].cachedRange;
    ASSERT_EQ(second.size, vertices.size());
    ASSERT_LE(second.offset + second.size, storage.size());
    EXPECT_NE(second.offset, first.offset);
    EXPECT_TRUE(std::equal(vertices.begin(), vertices.end(), storage.begin() + second.offset));
}
} // namespace
