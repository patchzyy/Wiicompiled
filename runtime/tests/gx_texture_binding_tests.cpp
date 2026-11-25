#include "gx_test_common.hpp"
#include "../src/hle/gx/gx_internal.h"
#include "isa/big_endian.h"

#include <bit>

// Keep the production loaders and metadata writers, without a guest dispatch registry.
#undef PPC_NATIVE_OVERRIDE
#undef PPC_NATIVE_OVERRIDE_VOID
#define PPC_NATIVE_OVERRIDE(...)
#define PPC_NATIVE_OVERRIDE_VOID(...)
#include "../src/hle/gx/gx_utils.cpp"
#include "../src/hle/gx/gx_objects.cpp"
#include "../src/hle/gx/gx_texture.cpp"
#include "gx_texture_binding_support/runtime_seams.h"

namespace {
constexpr uint32_t kObject = 0x80001000;
constexpr uint32_t kOtherObject = kObject + 32;
constexpr uint32_t kPixels = 0x80010000;
constexpr uint32_t kOtherPixels = kPixels + 4096;
constexpr uint32_t kGuestGxData = 0x80003000;

class RuntimeTextureBindingTest : public GXFifoTest {
protected:
    void SetUp() override {
        GXFifoTest::SetUp();
        AuroraSetGxProducerThread();
        GxWorker::detail::g_active = false;
        GxWorker::detail::t_inlineDepth = 0;
        GxWorker::detail::t_onWorker = false;
        TextureBindingTest::memory.fill(0);
        TextureBindingTest::unreadableObject = 0;
        TextureBindingTest::unreadableObjectAccesses = 0;
        TextureBindingTest::rangeChecks = 0;
        TextureBindingTest::retiredRanges.clear();
        InvalidateTexObjLoads();
        // The production table never erases slots; reset its memo before fixture teardown.
        g_texObjSlotMemo = nullptr;
        g_texObjSlotMemoAddr = 0;
        g_texObjIntervals.clear();
        g_texObjIntervalsPending.clear();
        g_texObjIntervalsRebuildAll = true;
        g_TexObjMeta.clear();
        g_TlutObjMeta.clear();
        g_HostTlutObjMap.clear();
        g_tlutObjBoundsDirty = true;
        Memory::Write32(kGXDataPtrAddr, kGuestGxData);
    }

    void TearDown() override {
        GxWorker::Sync();
        GxWorker::detail::g_active = false;
        g_texObjSlotMemo = nullptr;
        g_texObjSlotMemoAddr = 0;
        g_texObjIntervals.clear();
        g_texObjIntervalsPending.clear();
        g_TexObjMeta.clear();
        g_HostTlutObjMap.clear();
        GXFifoTest::TearDown();
    }

    void Init(uint32_t object = kObject, uint32_t pixels = kPixels, uint32_t format = GX_TF_RGBA8) {
        if (IsPaletteTexFormat(format)) {
            GX__InitTexObjCI_80170a04(object, pixels, 8, 8, format, GX_REPEAT, GX_CLAMP, 0, 0);
        } else {
            GX__InitTexObj_801707f8(object, pixels, 8, 8, format, GX_REPEAT, GX_CLAMP, 0);
        }
    }

    void Load(uint32_t object = kObject, uint32_t map = 0) { GX__LoadTexObj_80170f2c(object, map); }

    void Warm(uint32_t object = kObject, uint32_t map = 0) {
        Load(object, map);
        GxWorker::Sync();
        EnsureTexObjIntervalsLocked();
        capture_fifo();
    }

    void ExpectFast(uint32_t object = kObject, uint32_t map = 0) {
        const auto checks = TextureBindingTest::rangeChecks;
        Load(object, map);
        GxWorker::Sync();
        EXPECT_EQ(TextureBindingTest::rangeChecks, checks);
        EXPECT_TRUE(g_texObjIntervalsPending.empty());
    }

    void ExpectFull(uint32_t object = kObject, uint32_t map = 0) {
        const auto checks = TextureBindingTest::rangeChecks;
        Load(object, map);
        GxWorker::Sync();
        EXPECT_GT(TextureBindingTest::rangeChecks, checks);
    }
};

TEST_F(RuntimeTextureBindingTest, RepeatedBindingSkipsMetadataAndExistingHostLoad) {
    Init();
    Warm();
    Memory::Write32(kGuestGxData + 0x5FC, 0x20);
    Memory::Write16(kGuestGxData + 2, 0x1234);
    ExpectFast();
    EXPECT_TRUE(capture_fifo().empty());
    EXPECT_EQ(Memory::Read32(kGuestGxData + 0x5FC), 0x21u);
    EXPECT_EQ(Memory::Read16(kGuestGxData + 2), 0);
}

TEST_F(RuntimeTextureBindingTest, AlternatingObjectsAndMapsStillEmitRequiredBindings) {
    Init();
    Init(kOtherObject, kOtherPixels);
    Warm();
    Warm(kOtherObject, 1);
    ExpectFast(kObject, 1);
    auto bytes = capture_fifo();
    ASSERT_FALSE(bytes.empty());
    decode_fifo(bytes);
    EXPECT_EQ(gxState().loadedTextures[1].data, GuestToHostPtr(kPixels));
    ExpectFast(kOtherObject, 0);
    bytes = capture_fifo();
    ASSERT_FALSE(bytes.empty());
    decode_fifo(bytes);
    EXPECT_EQ(gxState().loadedTextures[0].data, GuestToHostPtr(kOtherPixels));
}

TEST_F(RuntimeTextureBindingTest, BindOnlyAndFullPathsEmitIdenticalPackets) {
    for (uint32_t format : {GX_TF_I4, GX_TF_RGBA8, GX_TF_CMPR, GX_TF_Z8, GX_TF_Z16, GX_TF_Z24X8}) {
        Init(kObject, kPixels, format);
        // Include values the full path has to sanitize before memoizing them.
        Memory::Write32(kObject, Memory::Read32(kObject) | 3u | (3u << 19));
        Memory::Write32(kObject + 4, 0xFFFF);
        Warm();
        g_boundTexMaps[2] = {};
        ExpectFast(kObject, 2);
        const auto fast = capture_fifo();
        ASSERT_FALSE(fast.empty());
        g_boundTexMaps[2] = {};
        g_TexObjMeta.at(kObject).validatedLoad.Invalidate();
        ExpectFull(kObject, 2);
        EXPECT_EQ(capture_fifo(), fast) << format;
    }
}

TEST_F(RuntimeTextureBindingTest, EverySnapshotByteParticipatesInValidation) {
    Init();
    Warm();
    const auto original = CaptureTexObjBytes(kObject);
    for (uint32_t offset = 0; offset < original.size(); ++offset) {
        {
            TexObjSnapshotScope before(kObject, original);
            ASSERT_NE(TryGetValidatedTexObjSlot(kObject), nullptr) << offset;
        }
        const uint8_t changed = original[offset] ^ 0x80;
        Memory::Write8(kObject + offset, changed);
        const auto snapshot = CaptureTexObjBytes(kObject);
        TexObjSnapshotScope scope(kObject, snapshot);
        EXPECT_EQ(TryGetValidatedTexObjSlot(kObject), nullptr) << offset;
        Memory::Write8(kObject + offset, original[offset]);
        // Re-arm through a real successful load, using the original bytes.
        {
            TexObjSnapshotScope restored(kObject, original);
            LoadTexObjImpl(kObject, 0);
        }
    }
}

TEST_F(RuntimeTextureBindingTest, GetterShadowCannotValidateAnotherSnapshot) {
    Init();
    Warm();
    auto newer = CaptureTexObjBytes(kObject);
    newer[0x1C] ^= 1;
    Memory::Write8(kObject + 0x1C, newer[0x1C]);
    GX__GetTexObjAll_80170bf8(kObject, 0, 0, 0, 0, 0, 0, 0);
    // This getter refreshes the decode shadow without doing a full texture load.
    ExpectFull();
    Warm();
    ExpectFast();
}

TEST_F(RuntimeTextureBindingTest, ValidatedLoadRestoresClampedLodAfterGetterRefresh) {
    constexpr uint32_t lodOutput = kGuestGxData + 0x600;
    const auto getMaxLod = [&] {
        GX__GetTexObjLODAll_80170cbc(kObject, 0, 0, 0, lodOutput, 0, 0, 0, 0);
        return std::bit_cast<float>(Memory::Read32(lodOutput));
    };

    Init();
    Memory::Write32(kObject + 4, 0x5000);
    Warm();
    ASSERT_EQ(getMaxLod(), 3.0f);
    Memory::Write32(kObject + 4, 0x3000);
    ASSERT_EQ(getMaxLod(), 3.0f);
    Memory::Write32(kObject + 4, 0x5000);

    ExpectFast();
    EXPECT_TRUE(capture_fifo().empty());
    EXPECT_EQ(getMaxLod(), 3.0f);
}

TEST_F(RuntimeTextureBindingTest, ProducerBacklogUsesExecutedBytes) {
    Init();
    Warm();
    const auto oldBytes = CaptureTexObjBytes(kObject);
    GxWorker::detail::g_active = true;
    Load(kObject, 1);
    Memory::Write32(kObject + 0x0C, CanonicalizeGxMainRamAddress(kOtherPixels) >> 5);
    Load(kObject, 2);
    // Producer has already moved on again before either queued load executes.
    Memory::Write32(kObject + 0x0C, BigEndian::Read32(oldBytes.data() + 0x0C));
    GxWorker::Sync();
    auto bytes = capture_fifo();
    decode_fifo(bytes);
    EXPECT_EQ(gxState().loadedTextures[1].data, GuestToHostPtr(kPixels));
    EXPECT_EQ(gxState().loadedTextures[2].data, GuestToHostPtr(kOtherPixels));
    EXPECT_EQ(g_TexObjMeta.at(kObject).dataAddr, CanonicalizeGxMainRamAddress(kOtherPixels));
    ExpectFull(kObject, 2);
    Warm(kObject, 2);
    ExpectFast(kObject, 2);
}

TEST_F(RuntimeTextureBindingTest, MetadataAndHostWritersRequireFullLoad) {
    Init();
    Warm();
    GX__InitTexObjLOD_80170a4c(kObject, GX_NEAR, GX_LINEAR, 0, 0, 0.5f, 1, 0, GX_ANISO_2);
    ExpectFull();
    Warm();
    GX__InitTexObjWrapMode_80170b50(kObject, GX_MIRROR, GX_REPEAT);
    ExpectFull();
    Warm();
    GX__InitTexObjTlut_80170b64(kObject, 3);
    ExpectFull();
    Warm();
    GX__InitTexObjFilter_80170b6c(kObject, GX_LINEAR, GX_NEAR);
    ExpectFull();
    Warm();
    GX__InitTexObjLODBias_80170b94(kObject, -0.5f);
    ExpectFull();
    Warm();
    GX__InitTexObjUserData_80170be8(kObject, kOtherPixels);
    ExpectFull();
    Warm();
    // Even an identical descriptor receives a new host texture identity.
    Init();
    ExpectFull();
    EXPECT_FALSE(capture_fifo().empty());
    Warm();
    auto& meta = GetTexObjMeta(kObject);
    meta.wrapT = GX_MIRROR;
    ExpectFull();
}

TEST_F(RuntimeTextureBindingTest, PixelDmaBetweenQueuedLoadsCannotBeSkipped) {
    Init();
    Warm();
    const auto* host = TryGetHostTexObj(kObject);
    const auto oldVersion = reinterpret_cast<const GXTexObj_*>(host)->texDataVersion;
    GxWorker::detail::g_active = true;
    Load(kObject, 1);
    GxNotifyGuestRamDmaWrite(kPixels, 256);
    Load(kObject, 1);
    GxWorker::Sync();
    EXPECT_FALSE(g_TexObjMeta.at(kObject).needsUpload);
    EXPECT_GT(reinterpret_cast<const GXTexObj_*>(host)->texDataVersion, oldVersion);
    EXPECT_EQ(TextureBindingTest::retiredRanges.size(), 1u);
    auto bytes = capture_fifo();
    decode_fifo(bytes);
    EXPECT_GT(gxState().loadedTextures[1].texDataVersion, oldVersion);
}

TEST_F(RuntimeTextureBindingTest, PixelDmaRequiresRebindingEachLoadedMap) {
    Init();
    for (uint32_t map : {0u, 1u, 2u}) {
        Load(kObject, map);
    }
    decode_fifo(capture_fifo());
    const auto oldVersion = gxState().loadedTextures[0].texDataVersion;
    ASSERT_NE(oldVersion, 0u);
    ASSERT_EQ(gxState().loadedTextures[1].texDataVersion, oldVersion);
    ASSERT_EQ(gxState().loadedTextures[2].texDataVersion, oldVersion);

    GxWorker::detail::g_active = true;
    GxNotifyGuestRamDmaWrite(kPixels, 256);
    ExpectFull(kObject, 0);
    decode_fifo(capture_fifo());
    const auto newVersion = gxState().loadedTextures[0].texDataVersion;
    ASSERT_GT(newVersion, oldVersion);
    ASSERT_FALSE(g_TexObjMeta.at(kObject).needsUpload);
    ASSERT_EQ(TextureBindingTest::retiredRanges.size(), 1u);
    EnsureTexObjIntervalsLocked();

    ExpectFast(kObject, 1);
    auto bytes = capture_fifo();
    EXPECT_FALSE(bytes.empty());
    if (!bytes.empty()) {
        decode_fifo(bytes);
    }
    EXPECT_EQ(gxState().loadedTextures[1].texDataVersion, newVersion);
    ExpectFast(kObject, 1);
    EXPECT_TRUE(capture_fifo().empty());

    g_TexObjMeta.at(kObject).validatedLoad.Invalidate();
    ExpectFull(kObject, 2);
    bytes = capture_fifo();
    EXPECT_FALSE(bytes.empty());
    if (!bytes.empty()) {
        decode_fifo(bytes);
    }
    EXPECT_EQ(gxState().loadedTextures[2].texDataVersion, newVersion);
}

TEST_F(RuntimeTextureBindingTest, HostReinitializationRequiresRebindingOtherMaps) {
    Init();
    Load();
    Load(kObject, 1);
    decode_fifo(capture_fifo());
    const auto oldId = gxState().loadedTextures[1].texObjId;
    const auto oldVersion = gxState().loadedTextures[1].texDataVersion;

    Init();
    Load();
    decode_fifo(capture_fifo());
    const auto newId = gxState().loadedTextures[0].texObjId;
    ASSERT_NE(newId, oldId);
    ASSERT_EQ(gxState().loadedTextures[0].texDataVersion, oldVersion);
    EnsureTexObjIntervalsLocked();

    ExpectFast(kObject, 1);
    const auto bytes = capture_fifo();
    ASSERT_FALSE(bytes.empty());
    decode_fifo(bytes);
    EXPECT_EQ(gxState().loadedTextures[1].texObjId, newId);
}

TEST_F(RuntimeTextureBindingTest, DirtyAliasesAndAddressReuseUploadBeforeRebinding) {
    Init();
    Warm();
    const auto oldVersion = reinterpret_cast<const GXTexObj_*>(TryGetHostTexObj(kObject))->texDataVersion;
    MarkTexObjsDirtyForRange(0xC0010000, 256);
    EXPECT_TRUE(g_TexObjMeta.at(kObject).needsUpload);
    ExpectFull();
    EXPECT_GT(reinterpret_cast<const GXTexObj_*>(TryGetHostTexObj(kObject))->texDataVersion, oldVersion);
    Warm();
    Memory::Write32(kObject + 0x0C, CanonicalizeGxMainRamAddress(kOtherPixels) >> 5);
    ExpectFull();
    EXPECT_EQ(g_TexObjMeta.at(kObject).dataAddr, CanonicalizeGxMainRamAddress(kOtherPixels));
    auto bytes = capture_fifo();
    decode_fifo(bytes);
    EXPECT_EQ(gxState().loadedTextures[0].data, GuestToHostPtr(kOtherPixels));
}

TEST_F(RuntimeTextureBindingTest, PaletteFormatsAlwaysKeepFullPathAcrossTlutChanges) {
    for (uint32_t format : {GX_TF_C4, GX_TF_C8, GX_TF_C14X2}) {
        Init(kObject, kPixels, format);
        Warm();
        ExpectFull();
        GX__InitTlutObj_80170f80(kOtherObject, kOtherPixels, GX_TL_RGB565, 16);
        GX__LoadTlut_80170fa8(kOtherObject, 0);
        MarkTlutObjsDirtyForRange(kOtherPixels, 32);
        GX__LoadTlut_80170fa8(kOtherObject, 0);
        ExpectFull();
    }
}

TEST_F(RuntimeTextureBindingTest, InvalidationAndCopyEvictionPreserveMetadataValidation) {
    Init();
    Load();
    EnsureTexObjIntervalsLocked();
    auto bind = capture_fifo();
    ASSERT_FALSE(bind.empty());
    decode_fifo(bind);
    ASSERT_EQ(gxState().loadedTextures[0].data, GuestToHostPtr(kPixels));
    GX__InvalidateTexAll_80171110();
    auto invalidate = capture_fifo();
    ASSERT_FALSE(invalidate.empty());
    decode_fifo(invalidate);
    ExpectFast();
    EXPECT_TRUE(capture_fifo().empty());
    void* copyDest = GuestToHostPtr(kPixels);
    ASSERT_NE(copyDest, nullptr);
    gxState().copyTextures[copyDest] = {.revision = 1};
    GXDestroyCopyTex(copyDest);
    decode_fifo(capture_fifo());
    EXPECT_FALSE(gxState().copyTextures.contains(copyDest));
    ExpectFast();
    EXPECT_TRUE(capture_fifo().empty());
}

TEST_F(RuntimeTextureBindingTest, ResetAndHostDestructionCannotReuseCompletedLoad) {
    Init();
    Warm();
    InvalidateTexObjLoads();
    ExpectFull();
    EXPECT_FALSE(capture_fifo().empty());
    Warm();
    g_TexObjMeta.at(kObject).host->Destroy();
    ExpectFull();
    EXPECT_TRUE(g_TexObjMeta.at(kObject).host->constructed);
}

TEST_F(RuntimeTextureBindingTest, UnreadableSnapshotDoesNotBecomeValidated) {
    Init();
    Warm();
    TextureBindingTest::unreadableObject = CanonicalizeGxMainRamAddress(kObject);
    ExpectFull();
    EXPECT_FALSE(g_TexObjMeta.at(kObject).validatedLoad.valid);
    TextureBindingTest::unreadableObject = 0;
    ExpectFull();
    EXPECT_TRUE(g_TexObjMeta.at(kObject).validatedLoad.valid);
}

TEST_F(RuntimeTextureBindingTest, InvalidMapDoesNotCaptureOrValidateDescriptor) {
    Init();
    for (bool defer : {false, true}) {
        SCOPED_TRACE(defer);
        GxWorker::detail::g_active = defer;
        auto& validation = g_TexObjMeta.at(kObject).validatedLoad;
        validation.Invalidate();
        Load(kObject, 8);
        GxWorker::Sync();
        EXPECT_FALSE(validation.valid);
        EXPECT_TRUE(capture_fifo().empty());

        TextureBindingTest::unreadableObject = CanonicalizeGxMainRamAddress(kObject);
        const auto accesses = TextureBindingTest::unreadableObjectAccesses;
        Memory::Write32(kGuestGxData + 0x5FC, 0x20);
        Memory::Write16(kGuestGxData + 2, 0x1234);

        Load(kObject, 8);
        EXPECT_EQ(Memory::Read32(kGuestGxData + 0x5FC), 0x21u);
        EXPECT_EQ(Memory::Read16(kGuestGxData + 2), 0u);
        GxWorker::Sync();
        EXPECT_EQ(TextureBindingTest::unreadableObjectAccesses, accesses);
        EXPECT_FALSE(validation.valid);
        EXPECT_TRUE(capture_fifo().empty());

        TextureBindingTest::unreadableObject = 0;
        ExpectFull();
        EXPECT_TRUE(validation.valid);
        capture_fifo();
    }
}

TEST_F(RuntimeTextureBindingTest, InvalidGuestDescriptorKeepsPlaceholderFallback) {
    Init();
    Load();
    EnsureTexObjIntervalsLocked();
    decode_fifo(capture_fifo());
    auto& loaded = gxState().loadedTextures[0];
    ASSERT_EQ(loaded.data, GuestToHostPtr(kPixels));
    const auto validId = loaded.texObjId;

    WriteGuestTexObj(kOtherObject, kOtherPixels, 8, 8, GX_CTF_R4, GX_CLAMP, GX_CLAMP, false, false, 0);
    Load(kOtherObject);
    auto bytes = capture_fifo();
    ASSERT_FALSE(bytes.empty());
    decode_fifo(bytes);
    EXPECT_EQ(loaded.width(), 1u);
    EXPECT_EQ(loaded.height(), 1u);
    EXPECT_NE(loaded.texObjId, validId);
    EXPECT_EQ(g_boundTexMaps[0].objAddr, 0u);

    EnsureTexObjIntervalsLocked();
    ExpectFast();
    bytes = capture_fifo();
    ASSERT_FALSE(bytes.empty());
    decode_fifo(bytes);
    EXPECT_EQ(loaded.texObjId, validId);
    EXPECT_EQ(loaded.data, GuestToHostPtr(kPixels));
}

TEST_F(RuntimeTextureBindingTest, DISABLED_BenchmarkRepeatedLoads) {
    Init();
    Warm();
    constexpr uint32_t iterations = 250000;
    auto& validation = g_TexObjMeta.at(kObject).validatedLoad;
    for (bool forceFull : {true, false}) {
        const auto checks = TextureBindingTest::rangeChecks;
        const auto begin = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < iterations; ++i) {
            if (forceFull) validation.Invalidate();
            Load();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("texture-load full=%d iterations=%u ns=%lld range-checks=%llu\n", forceFull, iterations,
                    static_cast<long long>(elapsed),
                    static_cast<unsigned long long>(TextureBindingTest::rangeChecks - checks));
    }
}
} // namespace
