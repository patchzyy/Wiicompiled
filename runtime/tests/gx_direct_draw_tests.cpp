#include "gx_test_common.hpp"
#include "../src/hle/gx/gx_internal.h"
#include "../src/hle/gx/gx_stream_common.h"
#include "isa/big_endian.h"

#include <chrono>
#include <cstdio>
#include <numeric>

namespace {
class RuntimeDirectDrawTest : public GXFifoTest {
protected:
    void SetUp() override {
        GXFifoTest::SetUp();
        reset_runtime();
    }

    void reset_runtime() {
        GXInit(nullptr, 0);
        aurora::gx::fifo::clear_buffer();
        g_hleGxState = {};
        g_dlRecordState = {};
        g_alphaCompareValid = false;
        GxWorker::detail::g_active = false;
        GxWorker::detail::t_onWorker = false;
    }

    void position(GXCompCnt cnt = GX_POS_XYZ, GXCompType type = GX_U8, u8 frac = 0) {
        g_hleGxState.vtxDesc[GX_VA_POS] = GX_DIRECT;
        g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_POS] = {cnt, type, frac};
        g_hleGxState.InvalidateVtxLayoutHash();
    }

    void begin(u16 count, bool direct = true) {
        auto& g = g_hleGxState;
        GxStream::PublishAuroraVtxState(GX_VTXFMT0, {false, GX_VA_POS, GX_VA_TEX7});
        g.currentVtxFmt = GX_VTXFMT0;
        g.currentPrim = GX_TRIANGLES;
        g.vertsRemaining = count;
        g.inBegin = count != 0;
        g.auroraBeginCalled = false;
        g.fifoReadOffset = 0;
        g.fifoByteCount = 0;
        g.fifoPendingDrawBytes = 0;
        g.ResetVertex();
        GxStream::PrepareImmediateDirectDraw();
        if (!direct) {
            g.fifoDirectBytesRemaining = 0;
        }
    }

    void write(const std::vector<u8>& bytes, u32 width) {
        size_t pos = 0;
        for (; pos + width <= bytes.size(); pos += width) {
            const u32 value = width == 4 ? BigEndian::Read32(bytes.data() + pos)
                : width == 2 ? BigEndian::Read16(bytes.data() + pos) : bytes[pos];
            HleFifoWrite(value, width);
        }
        for (; pos < bytes.size(); ++pos) {
            HleFifoWrite(bytes[pos], 1);
        }
    }
};

TEST_F(RuntimeDirectDrawTest, DirectNumericPayloadMatchesIncrementalForEveryWriteWidth) {
    for (GXCompType type : {GX_U8, GX_S8, GX_U16, GX_S16, GX_F32}) {
        for (GXCompCnt cnt : {GX_POS_XY, GX_POS_XYZ}) {
            for (u32 width : {1u, 2u, 4u}) {
                const u32 bytesPerVertex = (cnt == GX_POS_XY ? 2u : 3u) * GetCompSizeBytes(type);
                std::vector<u8> payload(bytesPerVertex * 3);
                if (type == GX_F32) {
                    for (size_t i = 0; i < payload.size() / sizeof(float); ++i) {
                        const float value = static_cast<float>(i + 1) / 7.0f;
                        BigEndian::WriteFloat32(payload.data(), i * sizeof(float), value);
                    }
                } else {
                    for (size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<u8>(i * 17u + 11u);
                    }
                }
                reset_runtime();
                position(cnt, type, 7);
                begin(3, false);
                write(payload, width);
                const auto reference = capture_fifo();
                reset_runtime();
                position(cnt, type, 7);
                begin(3);
                ASSERT_EQ(g_hleGxState.fifoDirectBytesRemaining, payload.size());
                write(payload, width);
                EXPECT_EQ(capture_fifo(), reference) << "type=" << type << " count=" << cnt << " width=" << width;
                EXPECT_FALSE(g_hleGxState.inBegin);
                EXPECT_EQ(g_hleGxState.vertsRemaining, 0u);
            }
        }
    }
}

TEST_F(RuntimeDirectDrawTest, MixedMatrixColorsAndAllTextureCoordinatesMatchIncremental) {
    constexpr u32 bytesPerVertex = 45;
    std::vector<u8> payload(bytesPerVertex * 3);
    std::iota(payload.begin(), payload.end(), u8{64});
    for (u8 vertex = 0; vertex < 3; ++vertex) {
        payload[vertex * bytesPerVertex] = vertex * 6;
        payload[vertex * bytesPerVertex + 1] = vertex * 6 + 3;
    }
    std::vector<u8> reference;
    for (bool direct : {false, true}) {
        reset_runtime();
        position(GX_POS_XY, GX_S16, 11);
        auto& g = g_hleGxState;
        g.vtxDesc[GX_VA_PNMTXIDX] = GX_DIRECT;
        g.vtxDesc[GX_VA_TEX7MTXIDX] = GX_DIRECT;
        g.vtxDesc[GX_VA_CLR0] = GX_DIRECT;
        g.vtxAttrFmt[0][GX_VA_CLR0] = {GX_CLR_RGB, GX_RGB8, 0};
        g.vtxDesc[GX_VA_CLR1] = GX_DIRECT;
        g.vtxAttrFmt[0][GX_VA_CLR1] = {GX_CLR_RGBA, GX_RGBA8, 0};
        for (int tex = GX_VA_TEX0; tex <= GX_VA_TEX7; ++tex) {
            g.vtxDesc[tex] = GX_DIRECT;
            g.vtxAttrFmt[0][tex] = {GX_TEX_ST, GX_S16, static_cast<u8>(tex)};
        }
        begin(3, direct);
        ASSERT_EQ(g.fifoDirectBytesRemaining, direct ? payload.size() : 0u);
        GX_HLE_FIFO_WriteBurst(payload.data(), static_cast<u32>(payload.size()));
        if (!direct) {
            reference = capture_fifo();
        } else {
            EXPECT_EQ(capture_fifo(), reference);
        }
    }
}

TEST_F(RuntimeDirectDrawTest, NormalAliasesIndexedAndReencodedColorsKeepFallback) {
    for (GXCompCnt cnt : {GX_NRM_XYZ, GX_NRM_NBT, GX_NRM_NBT3}) {
        reset_runtime();
        position();
        g_hleGxState.vtxDesc[GX_VA_NRM] = GX_DIRECT;
        g_hleGxState.vtxAttrFmt[0][GX_VA_NRM] = {cnt, GX_S16, 14};
        begin(3);
        EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    }
    for (GXCompType type : {GX_RGB565, GX_RGBX8, GX_RGBA4, GX_RGBA6}) {
        reset_runtime();
        position();
        g_hleGxState.vtxDesc[GX_VA_CLR0] = GX_DIRECT;
        g_hleGxState.vtxAttrFmt[0][GX_VA_CLR0] = {GX_CLR_RGBA, type, 0};
        begin(3);
        EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    }
    for (GXAttrType type : {GX_INDEX8, GX_INDEX16}) {
        reset_runtime();
        position();
        g_hleGxState.vtxDesc[GX_VA_POS] = type;
        begin(3);
        EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    }
}

TEST_F(RuntimeDirectDrawTest, CompletingWordAndBurstTailAreParsedImmediately) {
    for (bool burst : {false, true}) {
        reset_runtime();
        position();
        begin(1);
        const std::vector<u8> stream{0x11, 0x22, 0x33, 0x48};
        if (burst) {
            GX_HLE_FIFO_WriteBurst(stream.data(), static_cast<u32>(stream.size()));
        } else {
            HleFifoWrite(0x11223348, 4);
        }
        const auto bytes = capture_fifo();
        ASSERT_FALSE(bytes.empty());
        EXPECT_EQ(bytes.back(), 0x48);
        EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
        EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    }
}

TEST_F(RuntimeDirectDrawTest, RawPacketsReserveCompletingWordTailAtBufferBoundary) {
    for (u16 packetBytes : {4093, 4094, 4095, 4096}) {
        reset_runtime();
        g_hleGxState.vtxDesc[GX_VA_PNMTXIDX] = GX_DIRECT;
        const u16 count = packetBytes - 3;
        HleFifoWrite(0x90, 1);
        HleFifoWrite(count, 2);
        EXPECT_EQ(g_hleGxState.fifoPendingDrawBytes, packetBytes == 4093 ? packetBytes : 0u);
        for (u32 i = 0; i < count - 1u; ++i) {
            HleFifoWrite(0x12, 1);
        }
        HleFifoWrite(0x12480000, 4);
        const auto bytes = capture_fifo();
        ASSERT_GE(bytes.size(), count + 4u);
        const auto drawStart = bytes.size() - count - 4u;
        EXPECT_EQ(bytes[drawStart], 0x90);
        EXPECT_EQ(BigEndian::Read16(bytes.data() + drawStart + 1u), count);
        for (size_t i = drawStart + 3u; i < drawStart + count + 3u; ++i) {
            ASSERT_EQ(bytes[i], 0x12) << "packet=" << packetBytes;
        }
        EXPECT_EQ(bytes.back(), 0x48);
        EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
        EXPECT_EQ(g_hleGxState.fifoPendingDrawBytes, 0u);
        EXPECT_FALSE(g_hleGxState.inBegin);
    }
}

TEST_F(RuntimeDirectDrawTest, PendingRawLengthIsRecomputedAfterLayoutInvalidation) {
    position();
    HleFifoWrite(0x90, 1);
    HleFifoWrite(1, 2);
    ASSERT_EQ(g_hleGxState.fifoPendingDrawBytes, 6u);
    position(GX_POS_XY);
    HleFifoWrite(0x1122, 2);
    EXPECT_EQ(g_hleGxState.fifoPendingDrawBytes, 0u);
    EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
    EXPECT_FALSE(g_hleGxState.inBegin);
}

TEST_F(RuntimeDirectDrawTest, CompletingWordCanStartPartialCpPacket) {
    position(GX_POS_XY);
    begin(1);
    HleFifoWrite(0x11220850, 4);
    EXPECT_FALSE(g_hleGxState.inBegin);
    EXPECT_EQ(g_hleGxState.fifoByteCount, 2u);
    HleFifoWrite(3u << 9, 4);
    EXPECT_EQ(g_hleGxState.vtxDesc[GX_VA_POS], GX_INDEX16);
    EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
}

TEST_F(RuntimeDirectDrawTest, LayoutChangeBeforeFirstPayloadRechecksDirectEligibility) {
    position();
    begin(1);
    g_hleGxState.vtxDesc[GX_VA_NRM] = GX_DIRECT;
    g_hleGxState.vtxAttrFmt[0][GX_VA_NRM] = {GX_NRM_XYZ, GX_S16, 14};
    g_hleGxState.InvalidateVtxLayoutHash();
    HleFifoWrite(0x11, 1);
    EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    EXPECT_TRUE(g_hleGxState.auroraBeginCalled);
    HleFifoWrite(0x2233, 2);
    HleFifoWrite(0x44556677, 4);
    HleFifoWrite(0x8899, 2);
    EXPECT_FALSE(g_hleGxState.inBegin);
    EXPECT_FALSE(g_hleGxState.auroraBeginCalled);
}

TEST_F(RuntimeDirectDrawTest, DirectPayloadLargerThanQueueStillMatchesIncremental) {
    std::vector<u8> reference;
    const std::vector<u8> payload(4096u * 3u, 0x32);
    for (bool direct : {false, true}) {
        reset_runtime();
        position();
        begin(4096, direct);
        GX_HLE_FIFO_WriteBurst(payload.data(), static_cast<u32>(payload.size()));
        EXPECT_FALSE(g_hleGxState.inBegin);
        EXPECT_EQ(g_hleGxState.fifoByteCount, 0u);
        if (!direct) {
            reference = capture_fifo();
        } else {
            EXPECT_EQ(capture_fifo(), reference);
        }
    }
}

TEST_F(RuntimeDirectDrawTest, RecordingBypassesPreparedDirectPayload) {
    position();
    begin(1);
    g_dlRecordState = {0x80001000, 32, 0x80001000, 0, true};
    HleFifoWrite(0x11223344, 4);
    EXPECT_EQ(g_dlRecordState.count, 4u);
    EXPECT_EQ(Memory::Read32(0x80001000), 0x11223344u);
    EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 3u);
    EXPECT_TRUE(capture_fifo().empty());
    g_dlRecordState.active = false;
}

TEST_F(RuntimeDirectDrawTest, ZeroCountDoesNotPrepareDirectPayload) {
    position();
    begin(0);
    EXPECT_EQ(g_hleGxState.fifoDirectBytesRemaining, 0u);
    EXPECT_FALSE(g_hleGxState.inBegin);
}

TEST_F(RuntimeDirectDrawTest, DISABLED_SubmissionBenchmark) {
    constexpr u32 draws = 20000;
    const std::vector<u8> payload(3u * 4u, 0x32);
    for (bool direct : {false, true, true, false}) {
        reset_runtime();
        position();
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < draws; ++i) {
            begin(4, direct);
            write(payload, 4);
        }
        const auto end = std::chrono::steady_clock::now();
        const auto bytes = capture_fifo();
        std::printf("direct=%d draws=%u elapsed_us=%lld fifo_bytes=%zu\n", direct, draws,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()),
                    bytes.size());
    }
}
} // namespace
