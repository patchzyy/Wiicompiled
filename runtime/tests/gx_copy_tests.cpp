#include "gx_test_common.hpp"
#include "../src/hle/gx/gx_internal.h"
#include "gx_sync_support/copy_observation.h"
#include "gx/readback.hpp"
#include "__gx.h"

// Exercise the wrappers without registering guest ABI entry points in the host fixture.
#undef REGISTER_NATIVE_FUNCTION
#define REGISTER_NATIVE_FUNCTION(address, fn)
#include "../src/hle/gx/gx_copy.cpp"

extern "C" void AuroraResetCopyProbesForTesting();

namespace aurora::gfx {
extern uint32_t g_testDrawCount;
namespace testing {
extern std::atomic<uint32_t> resolvePassCount;
extern std::vector<std::shared_ptr<readback::Job>> readbackJobs;
extern std::vector<uint32_t> readbackJobCopies;
std::vector<CopyObservation> copyObservations;

void observe_copy(TextureHandle target, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  GXTexFmt format, const ResolveOptions& options) {
    copyObservations.push_back({std::move(target), rect, clearColor, clearAlpha, clearDepth, format, options,
                                g_testDrawCount});
}
} // namespace testing
} // namespace aurora::gfx

extern "C" {
const uint8_t kMkwGuestRegion = 0;
int g_gxFrameCount = 0;
}
TexCopyState g_texCopyState;

namespace {
constexpr uint32_t kGuestBase = 0x80001000;
std::array<uint8_t, 4096> g_guestBytes{};
std::atomic<uint32_t> g_drawDoneCallbacks{0};
uint32_t g_frameJoins = 0;
uint32_t g_readyXfb = 0;
uint32_t g_presents = 0;
uint32_t g_overlayDraws = 0;

void DrawDoneCallback() { ++g_drawDoneCallbacks; }
} // namespace

void* GuestToHostPtr(uint32_t address, size_t length) {
    const uint32_t canonical = CanonicalizeGxMainRamAddress(address);
    const uint32_t base = CanonicalizeGxMainRamAddress(kGuestBase);
    if (canonical < base || canonical - base >= g_guestBytes.size() ||
        length > g_guestBytes.size() - (canonical - base)) {
        return nullptr;
    }
    return g_guestBytes.data() + canonical - base;
}
void EnsureAuroraFrameActive() {}
void VI_HLE_SetXfbReady(uint32_t address) { g_readyXfb = address; }
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    EXPECT_TRUE(presentedXfb);
    EXPECT_TRUE(paceToRetrace);
    GxWorker::Sync();
    aurora::gx::fifo::drain();
    ++g_presents;
}
extern "C" void aurora_wait_for_frame_worker() { ++g_frameJoins; }
namespace settings_overlay {
void Draw() noexcept { ++g_overlayDraws; }
} // namespace settings_overlay

namespace {
namespace stub = aurora::gfx::testing;

class GxCopyTest : public GXFifoTest, public ::testing::WithParamInterface<bool> {
protected:
    void SetUp() override {
        aurora::gx::fifo::init();
        GXFifoTest::SetUp();
        AuroraResetCopyProbesForTesting();
        AuroraSetGxProducerThread();
        g_texCopyState = {};
        g_gxFrameCount = 0;
        g_drawDoneCallbacks = 0;
        g_frameJoins = g_readyXfb = g_presents = g_overlayDraws = 0;
        g_efbCopyDestinations.clear();
        g_efbCopyBuckets.clear();
        g_largestEfbCopyDestination = 0;
        stub::copyObservations.clear();
        stub::resolvePassCount = 0;
        stub::readbackJobs.clear();
        stub::readbackJobCopies.clear();
        aurora::gfx::g_testDrawCount = 0;
        aurora::gfx::readback::set_supported(true);
        GXSetDrawDoneCallback(DrawDoneCallback);
        aurora::gx::fifo::begin_frame();
        if (GetParam()) {
            GxWorker::Start();
        }
        GxWorker::Run([] {
            GXSetColorUpdate(GX_TRUE);
            GXSetAlphaUpdate(GX_TRUE);
            GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
            GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        });
    }
    void TearDown() override {
        GxWorker::Stop();
        aurora::gx::fifo::drain();
        aurora::gx::fifo::end_frame();
        GXSetDrawDoneCallback(nullptr);
        AuroraResetCopyProbesForTesting();
        aurora::gfx::readback::shutdown();
        aurora::gfx::readback::set_supported(false);
        GXFifoTest::TearDown();
    }
    void Drain() {
        GxWorker::Sync();
        AuroraGXSync();
    }
    void Draw() {
        GxWorker::Run([] {
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
            GXPosition3u8(0, 1, 2);
            GXPosition3u8(3, 4, 5);
            GXPosition3u8(6, 7, 8);
            GXEnd();
        });
    }
    void Copy(uint32_t address, GXBool clear, GXTexFmt format = GX_TF_RGBA8, u16 width = 8, u16 height = 8) {
        GX__SetTexCopySrc_8016f478(2, 4, width, height);
        GX__SetTexCopyDst_8016f4dc(width, height, format, GX_FALSE);
        GX__CopyTex_8016fd74(address, clear);
    }
};

TEST_P(GxCopyTest, InterleavedDrawsClearsAndReusedDestinationsNeedNoCompletionTokens) {
    Draw();
    Copy(kGuestBase, GX_TRUE);
    Draw();
    Copy(kGuestBase + 512, GX_FALSE);
    Copy(kGuestBase, GX_FALSE);
    Drain();
    ASSERT_EQ(stub::copyObservations.size(), 3u);
    EXPECT_EQ(stub::copyObservations[0].draws, 1u);
    EXPECT_EQ(stub::copyObservations[1].draws, 2u);
    EXPECT_EQ(stub::copyObservations[2].draws, 2u);
    const auto& first = stub::copyObservations[0];
    EXPECT_TRUE(first.clearColor);
    EXPECT_TRUE(first.clearAlpha);
    EXPECT_TRUE(first.clearDepth);
    EXPECT_EQ(first.rect.x, 2);
    EXPECT_EQ(first.rect.y, 4);
    EXPECT_EQ(first.rect.width, 8);
    EXPECT_EQ(first.rect.height, 8);
    EXPECT_FALSE(stub::copyObservations[1].clearColor);
    EXPECT_EQ(first.target, stub::copyObservations[2].target);
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
    EXPECT_EQ(g_frameJoins, 0u);
    EXPECT_EQ(g_efbCopyDestinations.size(), 2u);
    GXDrawDone();
    EXPECT_EQ(g_drawDoneCallbacks.load(), 1u);
}

TEST_P(GxCopyTest, ReadbackIsRecordedAfterItsCopyWithoutAnImplicitFence) {
    Draw();
    Copy(kGuestBase, GX_FALSE);
    const auto stream = AuroraReadbackCreateStream();
    ASSERT_NE(stream, 0u);
    const AuroraReadbackCopySrc source{.key = 7, .src = GuestToHostPtr(kGuestBase)};
    ASSERT_TRUE(AuroraReadbackCaptureCopies(stream, &source, 1));
    Drain();
    ASSERT_EQ(stub::readbackJobs.size(), 1u);
    ASSERT_EQ(stub::readbackJobCopies.size(), 1u);
    ASSERT_EQ(stub::copyObservations.size(), 1u);
    EXPECT_EQ(stub::readbackJobCopies[0], 1u);
    ASSERT_EQ(stub::readbackJobs[0]->copies.size(), 1u);
    EXPECT_EQ(stub::readbackJobs[0]->copies[0].texture, stub::copyObservations[0].target);
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
    AuroraReadbackDestroyStream(stream);
}

TEST_P(GxCopyTest, CopyStillFlushesUnpublishedDirtyState) {
    GxWorker::Run([] {
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    });
    GxWorker::Sync();
    ASSERT_NE(__gx->dirtyState, 0u);
    Copy(kGuestBase, GX_FALSE);
    EXPECT_EQ(__gx->dirtyState, 0u);
    Drain();
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
}

TEST_P(GxCopyTest, InvalidationRetiresTheQueuedCopyBeforeLaterConsumers) {
    Copy(kGuestBase, GX_FALSE);
    Copy(kGuestBase + 512, GX_FALSE);
    InvalidateEfbCopyDestinationsForRange(kGuestBase, 256);
    Drain();
    EXPECT_FALSE(aurora::gx::g_gxState.copyTextures.contains(GuestToHostPtr(kGuestBase)));
    EXPECT_TRUE(aurora::gx::g_gxState.copyTextures.contains(GuestToHostPtr(kGuestBase + 512)));
    EXPECT_EQ(g_efbCopyDestinations.size(), 1u);
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
}

TEST_P(GxCopyTest, PersistentCopyPolicyAndDepthProbesSurviveWithoutTheWait) {
    Copy(kGuestBase, GX_FALSE);
    Drain();
    ASSERT_EQ(stub::copyObservations.size(), 1u);
    EXPECT_TRUE(stub::copyObservations.back().options.persistent);
    // The renderer stub retains this field, allowing the real copy policy to see a warm destination.
    aurora::gx::g_gxState.copyTextures[GuestToHostPtr(kGuestBase)].consecutiveFrames = 8;
    Copy(kGuestBase, GX_FALSE, GX_TF_RGBA8, 640, 480);
    Drain();
    EXPECT_FALSE(stub::copyObservations.back().options.persistent);
    const auto probe = kGuestBase + (GetParam() ? 2048u : 3072u);
    Copy(probe, GX_FALSE, GX_TF_Z8, 4, 4);
    Drain();
    EXPECT_FALSE(stub::copyObservations.back().options.persistent);
    EXPECT_FALSE(stub::readbackJobs.empty());
    EXPECT_EQ(g_guestBytes[probe - kGuestBase], 0xff);
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
}

TEST_P(GxCopyTest, DisplayCopyRetainsPresentationAndInlineRenderWorkerJoin) {
    Draw();
    GxWorker::Run([] {
        GXSetDispCopySrc(0, 0, 640, 480);
        GXSetDispCopyDst(640, 480);
    });
    GX__CopyDisp_8016fc38(kGuestBase, GX_TRUE);
    EXPECT_EQ(stub::copyObservations.size(), 1u);
    EXPECT_EQ(g_readyXfb, kGuestBase);
    EXPECT_EQ(g_presents, 1u);
    EXPECT_EQ(g_overlayDraws, 1u);
    EXPECT_EQ(g_gxFrameCount, 1);
    EXPECT_EQ(g_frameJoins, GetParam() ? 0u : 1u);
    EXPECT_EQ(g_drawDoneCallbacks.load(), 0u);
}

INSTANTIATE_TEST_SUITE_P(WorkerDisabledAndEnabled, GxCopyTest, ::testing::Bool());
} // namespace
