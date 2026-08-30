#pragma once

#include "hle_stubs.h"
#include "memory.h"
#include "gx_guest_write.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "gx_texture_binding_contract.h"

#include <algorithm>

#include <dolphin/gx/GXLighting.h>
#include <dolphin/gx/GXFifo.h>
#include <dolphin/gx/GXManage.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/gx/GXDispList.h>
#include <dolphin/gx/GXGet.h>
#include <dolphin/gx/GXTransform.h>
#include <dolphin/gx/GXCull.h>
#include <dolphin/gx/GXPixel.h>
#include <dolphin/gx/GXTev.h>
#include <dolphin/gx/GXBump.h>
#include <dolphin/gx/GXGeometry.h>
#include <dolphin/gx/GXVert.h>
#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <map>
#include <unordered_map>
#include <vector>
#include <type_traits>
#include <memory>
#include <thread>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <cmath>
#include <stdexcept>

#include <aurora/aurora.h>

namespace aurora::gfx {
bool is_offscreen() noexcept;
}

// --- Constants ---
constexpr uint32_t kGXDataPtrAddr = MKW_GADDR(803886C8);
constexpr uint32_t kDlFifoAddr = MKW_GADDR(80344090);
constexpr uint32_t kDlWritePtrAddr = MKW_GADDR(803440A4);
constexpr uint32_t kDlCountAddr = MKW_GADDR(803440AC);
constexpr uint32_t kDlWrapFlagOffset = 0x20;
constexpr uint32_t kMaxTluts = 20;
constexpr uint32_t kGxDrawDoneFlagAddr = MKW_GADDR(803867d8);

// --- External Declarations ---
extern "C" void GXInitTexObjTlut(GXTexObj* obj, u32 tlut);
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);
extern "C" uint32_t __OSSetInterruptHandler_801a65f8_hle(uint32_t interrupt, uint32_t handler);
extern "C" uint32_t __OSUnmaskInterrupts_801a69bc_hle(uint32_t mask);
extern "C" uint32_t OS__GetCurrentThread_801a98b0_hle();
extern "C" void GX__SetCPUFifo_8016c94c(uint32_t fifoAddr);
extern "C" void GX__SetDirtyState_8016ee78();
extern "C" void GX__CallDisplayList_80172f64(uint32_t listAddr, uint32_t nbytes);

extern std::atomic_bool g_auroraFrameActive;
extern std::atomic_bool g_auroraFrameHadWork;

inline void GXMarkFrameWork() {
    g_auroraFrameHadWork.store(true, std::memory_order_release);
}

// --- Global State ---
extern "C" {
extern int g_gxFrameCount;

extern int32_t g_scissorLeft;
extern int32_t g_scissorTop;
extern int32_t g_scissorWidth;
extern int32_t g_scissorHeight;

extern float g_viewportState[6];
extern float g_projectionVector[7];
}

// Decoded guest GXTexObj metadata. The sampler half is
// GxTextureBindingContract::SamplerState, shared with the binding record so the
// two field lists cannot drift; only the bookkeeping fields are added here.
struct TexObjMeta : GxTextureBindingContract::SamplerState {
    uint32_t userData = 0;
    bool needsUpload = true;
};

// --- Host-side texture object storage (audit F9) ---
// One hash-map slot per guest GXTexObj (see TexObjSlot) instead of two parallel maps, so
// GXLoadTexObj no longer pays redundant tree descents to reach state for the same object.
struct HleTexObj {
    alignas(GXTexObj) std::byte storage[sizeof(GXTexObj)]{};
    bool storageLive = false;
    bool constructed = false;

    HleTexObj() = default;
    ~HleTexObj() { Destroy(); }

    GXTexObj* PublicPtr() { return reinterpret_cast<GXTexObj*>(storage); }

    void EnsureStorageLive() {
        if (!storageLive) {
            std::memset(storage, 0, sizeof(storage));
            storageLive = true;
        }
    }

    void Destroy() {
        if (storageLive) {
            GXDestroyTexObj(PublicPtr());
            std::memset(storage, 0, sizeof(storage));
            storageLive = false;
            constructed = false;
        }
    }
};

// One entry of the texobj table, keyed by the guest GXTexObj address.
//
// Derives from TexObjMeta on purpose: `GetTexObjMeta()` hands out a reference to
// the base subobject, so the HLE's `GetTexObjMeta(oa) = meta;` writebacks assign
// only the decoded fields and leave the bookkeeping below intact.
struct TexObjSlot : TexObjMeta {
    // Guest address of the GXTexObj this slot describes (the map key), so the
    // index vectors can carry slot pointers instead of re-looking-up keys.
    uint32_t objAddr = 0;

    // Aurora-side object. Created lazily by CreateHostTexObj.
    std::unique_ptr<HleTexObj> host;

    // Byte-exact shadow of the last-decoded guest GXTexObj; served without a
    // diff only while guest bytes still match it. Games mutate these structs
    // with plain, unflushed CPU stores, so a generation-counter scheme can
    // miss writes; an earlier "freeze after two loads" version caused stale
    // texture binds on track reload. Re-comparing bytes per load is correct.
    uint64_t guestShadow[4] = {0, 0, 0, 0};
    bool hasGuestShadow = false;

    // DCStoreRange interval-index bookkeeping: what this slot currently
    // contributes to g_texObjIntervals, so a pending update that turns out to
    // be a no-op costs a compare instead of an erase + insert.
    uint32_t indexedDataAddr = 0;
    uint32_t indexedSize = 0;
    bool inIntervalIndex = false;
    bool pendingIntervalUpdate = false;
};

struct TlutObjMeta {
    uint32_t dataAddr = 0;
    uint16_t entries = 0;
    uint32_t format = 0;
    bool dirty = true;
};

using BoundTexInfo = GxTextureBindingContract::State;

// CanonicalizeGxMainRamAddress and the guest-write generation table live in
// gx_guest_write.h so the GX caches and the runtime-wide write notifiers share
// one address space and one table.

namespace GxDisplayListScanCache {

inline bool XfPayloadFits(uint32_t pos, uint32_t nbytes, uint16_t count) noexcept {
    const uint32_t payloadBytes = 4u + (static_cast<uint32_t>(count) + 1u) * 4u;
    return pos <= nbytes && payloadBytes <= nbytes - pos;
}

inline uint64_t MakeIdentityKey(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash) noexcept {
    const uint32_t canonicalAddr = CanonicalizeGxMainRamAddress(listAddr);
    uint64_t key = layoutHash;
    key ^= (static_cast<uint64_t>(canonicalAddr) << 32) | static_cast<uint64_t>(nbytes);
    key *= 1099511628211ull;
    key ^= static_cast<uint64_t>(canonicalAddr) * 0x9e3779b97f4a7c15ull;
    return key;
}

// Display-list identity uses a 64-bit content digest (XXH3, computed by the caller in
// gx_dl.cpp) instead of a shadow-copy memcmp, avoiding the multi-megabyte shadow and its extra read pass.
inline bool ContentDigestMatches(uint64_t storedDigest,
                                 uint32_t storedNbytes,
                                 uint64_t currentDigest,
                                 uint32_t currentNbytes) noexcept {
    return storedNbytes == currentNbytes && storedDigest == currentDigest;
}

inline bool CanReuse(uint32_t storedAddr,
                     uint32_t storedNbytes,
                     uint64_t storedLayoutHash,
                     uint64_t storedContentDigest,
                     uint32_t currentAddr,
                     uint32_t currentNbytes,
                     uint64_t currentLayoutHash,
                     uint64_t currentContentDigest) noexcept {
    return storedAddr == CanonicalizeGxMainRamAddress(currentAddr) &&
           storedLayoutHash == currentLayoutHash &&
           ContentDigestMatches(storedContentDigest, storedNbytes, currentContentDigest,
                                currentNbytes);
}

} // namespace GxDisplayListScanCache

struct VtxAttrFmt {
    GXCompCnt cnt = GX_POS_XYZ;
    GXCompType type = GX_F32;
    u8 frac = 0;
};

struct HleGxState {
    GXAttrType vtxDesc[26]{};
    VtxAttrFmt vtxAttrFmt[8][26]{};
    // Display-list scans key their decoded index bounds by the active vertex
    // layout. Most display lists reuse that layout, so recompute its content
    // hash only after a real VCD/VAT mutation.
    uint64_t vtxLayoutHash = 0;
    bool vtxLayoutHashDirty = true;
    // Bumped on every real vtxDesc/vtxAttrFmt mutation. Consumers that mirror
    // this state (gx_dl.cpp's applied-state mirror) use it to detect that a
    // republish would be a no-op.
    uint32_t vtxStateGeneration = 0;
    struct VtxArray {
        uint32_t base = 0;
        uint32_t stride = 0;
    };
    VtxArray vtxArray[26]{};

    GXVtxFmt currentVtxFmt = GX_VTXFMT0;
    GXPrimitive currentPrim = GX_TRIANGLES;
    u32 vertsRemaining = 0;
    bool inBegin = false;
    bool auroraBeginCalled = false;
    bool dirty = false;

    GXAttr currentAttr = GX_VA_NULL;
    int currentComp = 0;
    float compBuffer[9]{}; 
    u32 rawCompBuffer[9]{};
    // Shared byte queue for MMIO FIFO command packets and in-begin vertex payload.
    std::array<uint8_t, 4096> fifoBytes{};
    size_t fifoReadOffset = 0;
    size_t fifoByteCount = 0;

    void ResetVertex();
    void InvalidateVtxLayoutHash() noexcept {
        vtxLayoutHashDirty = true;
        ++vtxStateGeneration;
    }
    GXAttr NextEnabledAttr(int startAttr);
    int GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt);
};

struct TexCopyState {
    uint16_t srcLeft = 0;
    uint16_t srcTop = 0;
    uint16_t srcWidth = 0;
    uint16_t srcHeight = 0;
    uint16_t dstWidth = 0;
    uint16_t dstHeight = 0;
    uint32_t dstFormat = 0;
    uint32_t dstMipmap = 0;
};

// --- Display list recording state (audit F2) ---
// HleFifoWrite owns a shadow cursor instead of re-reading guest memory per FIFO write (was
// 2 reads/write, up to 9 with a list open). Safe only because nothing outside the GX HLE
// touches the guest words it shadows (GXData+0x5F8, kDl*Addr); flushed back to guest memory
// when the list ends so guest-visible state stays correct.
struct GxDisplayListState {
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t writePtr = 0;
    uint32_t count = 0;
    bool active = false;
};
extern GxDisplayListState g_dlRecordState;

inline bool IsDisplayListActive() noexcept { return g_dlRecordState.active; }

// Mirrors GX__BeginDisplayList_80172e00's guest-side initialization.
void BeginDisplayListRecording(uint32_t listAddr, uint32_t sizeBytes);
// Flushes the shadow cursor/count back into guest memory and stops recording.
void EndDisplayListRecording();

// --- Helper Functions ---
void WriteDisplayListData(uint32_t val, uint32_t sizeBytes);
void BeginNextAuroraFrameWithRetry(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
void EnsureAuroraFrameActive();

GXColor DecodeGxColor(uint32_t colorWord);
void WriteGuestFloat(uint32_t addr, float value, const char* label = nullptr);
void* GuestToHostPtr(uint32_t addr, size_t len = 0);
void WriteGuest32(uint32_t addr, uint32_t value, const char* label = nullptr);

static inline bool IsMatrixIndexAttr(GXAttr attr) noexcept {
    return attr == GX_VA_PNMTXIDX ||
           (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX);
}

static inline uint32_t DecodeCpArrayBaseGuestAddress(uint32_t value) noexcept {
    constexpr uint32_t kWiiCpPhysicalAddressMask = 0x1fffffffu;
    const uint32_t physical = value & kWiiCpPhysicalAddressMask;
    if (physical == 0) {
        return 0;
    }
    if (Memory::Contains(physical, 1)) {
        return physical;
    }

    const uint32_t cached = physical | 0x80000000u;
    if (Memory::Contains(cached, 1)) {
        return cached;
    }

    const uint32_t uncached = physical | 0xC0000000u;
    if (Memory::Contains(uncached, 1)) {
        return uncached;
    }

    return cached;
}

bool IsKnownTexFormat(uint32_t fmt);
bool IsPaletteTexFormat(uint32_t fmt);
bool IsKnownTlutFormat(uint32_t fmt);
bool ValidateTlutData(uint32_t objAddr, const TlutObjMeta& meta);

TexObjMeta& GetTexObjMeta(uint32_t addr);
TexObjMeta ExtractTexObjMetaFromGuest(uint32_t addr);
bool TryGetOrExtractTexObjMeta(uint32_t addr, TexObjMeta& outMeta);
TlutObjMeta& GetTlutObjMeta(uint32_t addr);
GXTexObj* GetHostTexObj(uint32_t addr);
GXTexObj* TryGetHostTexObj(uint32_t addr);
GXTexObj* CreateHostTexObj(uint32_t addr);
void MarkHostTexObjConstructed(uint32_t addr);
void MarkTexObjsDirtyForRange(uint32_t addr, uint32_t size);
// Invalidates GPU-only GXCopyTex results when guest CPU writes are made visible
// over their destination. This is the allocation-reuse generation boundary
// for copy textures; it must be called for every data-cache store/flush range.
void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size);
GXTlutObj* CreateHostTlutObj(uint32_t addr);
void MarkHostTlutObjConstructed(uint32_t addr);
GXTlutObj* GetHostTlutObj(uint32_t addr);
void MarkTlutObjsDirtyForRange(uint32_t addr, uint32_t size);
// One-stop invalidation for DMA-class host-side writes into guest RAM (DVD
// reads, DCZeroRange, LC stores): texobjs + TLUTs + EFB copies + display
// lists. extern "C" so HLE modules outside the GX layer can declare it without
// pulling in this header.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);

void HleFifoWrite(u32 val, uint32_t sizeBytes);
void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps = nullptr);
void SubmitIndexedAttribute(GXAttr attr, uint32_t index);

static inline int GetCompSizeBytes(GXCompType type) {
    switch (type) {
    case GX_U8: case GX_S8: return 1;
    case GX_U16: case GX_S16: return 2;
    case GX_F32: return 4;
    default: return 1;
    }
}

// ReadArrayComp (guest vertex array -> float) now lives in gx_stream_common.h
// as GxStream::ReadArrayComp, next to the single fixed-point conversion table
// its display-list and FIFO counterparts share.

extern HleGxState g_hleGxState;
// Set once the guest (or a default-injection site) has published an alpha
// compare to aurora; the only consumer is GxStream::EnsureDefaultGxAlphaCompare.
extern bool g_alphaCompareValid;
extern std::array<BoundTexInfo, 8> g_boundTexMaps;
extern TexCopyState g_texCopyState;

extern std::mutex g_texObjMutex;
extern std::mutex g_tlutObjMutex;
// Guest GXTexObj address -> decoded metadata + host object. std::unordered_map
// rather than absl::flat_hash_map because the runtime target does not link
// absl, and because callers (gx_texture.cpp) hold an iterator across an insert
// that happens on another key: unordered_map keeps references valid across a
// rehash, an open-addressed table would not.
extern std::unordered_map<uint32_t, TexObjSlot> g_TexObjMeta;
