#include "gx_internal.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <vector>

// --- Texture and TLUT Objects ---

namespace aurora::gfx {
struct TextureRef;
using TextureHandle = std::shared_ptr<TextureRef>;
} 

// HleTexObj now lives in gx_internal.h: it is embedded in TexObjSlot so the
// metadata and the Aurora object share one hash-map entry.

struct HostGXTlutObj {
    alignas(GXTlutObj) std::byte publicStorage[sizeof(GXTlutObj)]{};
    aurora::gfx::TextureHandle ref;
};

struct HleTlutObj {
    static constexpr size_t kTlutObjStorageSize =
        (sizeof(GXTlutObj) >= sizeof(HostGXTlutObj)) ? sizeof(GXTlutObj) : sizeof(HostGXTlutObj);
    static constexpr size_t kTlutObjStorageAlign =
        (alignof(GXTlutObj) >= alignof(HostGXTlutObj)) ? alignof(GXTlutObj) : alignof(HostGXTlutObj);
    using Storage = std::aligned_storage_t<kTlutObjStorageSize, kTlutObjStorageAlign>;

    Storage storage{};
    bool storageLive = false;
    bool constructed = false;

    HleTlutObj() = default;
    ~HleTlutObj() { Destroy(); }

    HostGXTlutObj* HostObj() { return reinterpret_cast<HostGXTlutObj*>(&storage); }
    GXTlutObj* PublicPtr() { return reinterpret_cast<GXTlutObj*>(HostObj()->publicStorage); }

    void EnsureStorageLive() {
        if (!storageLive) {
            new (&storage) HostGXTlutObj();
            storageLive = true;
        }
    }

    void Destroy() {
        if (storageLive) {
            GXDestroyTlutObj(PublicPtr());
            std::destroy_at(HostObj());
            std::memset(&storage, 0, sizeof(storage));
            storageLive = false;
            constructed = false;
        }
    }
};

std::mutex g_texObjMutex;
std::map<uint32_t, std::unique_ptr<HleTlutObj>> g_HostTlutObjMap;
std::mutex g_tlutObjMutex;

// The texobj table. One slot per guest GXTexObj address holds both the decoded
// metadata and the Aurora object, so GXLoadTexObj resolves everything it needs
// with a single hash lookup instead of six tree descents across two maps.
std::unordered_map<uint32_t, TexObjSlot> g_TexObjMeta;
std::map<uint32_t, TlutObjMeta> g_TlutObjMeta;
std::array<BoundTexInfo, 8> g_boundTexMaps{};

namespace {

// A whole GXLoadTexObj call resolves the same guest address several times, so memoising the last
// slot turns all but the first lookup into a pointer compare. Safe because g_TexObjMeta is
// node-based and never erased from, so element references stay valid across rehashes.
uint32_t g_texObjSlotMemoAddr = 0;
TexObjSlot* g_texObjSlotMemo = nullptr;

constexpr size_t kTexObjTableReserve = 1024;

} // namespace

struct TexObjIntervalEntry {
    uint32_t dataAddr = 0;
    uint32_t size = 0;
    uint32_t objAddr = 0;
    TexObjSlot* slot = nullptr;
};

// DCStoreRange invalidation index: every indexed slot is listed in each 64 KiB bucket of guest RAM
// its texture data overlaps (slot.indexedDataAddr/indexedSize), so a flushed range only looks at
// the slots in its own buckets. It replaced a vector sorted by data address, which shifted the whole
// index whenever a texture object moved (memmove was 0.7% of the game thread on a slow Android device, in
// courses), and walked back over every entry within the largest texture's size of each flush.
// Mutations queue their slot (an O(1) dedupe flag on the slot) for the next lookup to apply.
constexpr uint32_t kTexObjBucketShift = 16;
std::unordered_map<uint32_t, std::vector<TexObjSlot*>> g_texObjBuckets;
std::vector<TexObjSlot*> g_texObjIntervalsPending;
bool g_texObjIntervalsRebuildAll = true;

// Address bounds of every registered TLUT payload, so a DCStoreRange over an
// unrelated buffer can skip the g_TlutObjMeta scan entirely. Recomputed lazily
// because TLUT registration is far rarer than cache flushes.
uint64_t g_tlutObjBoundsStart = 0;
uint64_t g_tlutObjBoundsEnd = 0;
bool g_tlutObjBoundsDirty = true;

// Guest GXTexObj is 32 bytes: word0 wrap/filter bits, word1 LOD bits, word2 width/height/format,
// word3 data address (<<5), word4 user data, word5 texFmt, word6 tlut, byte 0x1F flags. Exact
// bit positions are decoded inline below.

namespace {
thread_local uint32_t t_texObjSnapshotAddr = 0;
thread_local const uint8_t* t_texObjSnapshotBytes = nullptr;

const uint8_t* TexObjSnapshotFor(uint32_t addr) noexcept {
    return (t_texObjSnapshotBytes != nullptr && addr == t_texObjSnapshotAddr) ? t_texObjSnapshotBytes : nullptr;
}

uint32_t ReadTexObj32(uint32_t addr, uint32_t offset) {
    if (const uint8_t* bytes = TexObjSnapshotFor(addr)) return BigEndian::Read32(bytes + offset);
    return Memory::Read32(addr + offset);
}

uint8_t ReadTexObj8(uint32_t addr, uint32_t offset) {
    if (const uint8_t* bytes = TexObjSnapshotFor(addr)) return bytes[offset];
    return Memory::Read8(addr + offset);
}

uint64_t ReadTexObj64(uint32_t addr, uint32_t offset) {
    if (const uint8_t* bytes = TexObjSnapshotFor(addr)) {
        return (static_cast<uint64_t>(BigEndian::Read32(bytes + offset)) << 32) | BigEndian::Read32(bytes + offset + 4);
    }
    return Memory::Read64(addr + offset);
}
} // namespace

TexObjBytes CaptureTexObjBytes(uint32_t addr) {
    TexObjBytes bytes{};
    for (uint32_t offset = 0; offset < bytes.size(); offset += 4) {
        const uint32_t word = Memory::Read32(addr + offset);
        bytes[offset + 0] = static_cast<uint8_t>(word >> 24);
        bytes[offset + 1] = static_cast<uint8_t>(word >> 16);
        bytes[offset + 2] = static_cast<uint8_t>(word >> 8);
        bytes[offset + 3] = static_cast<uint8_t>(word);
    }
    return bytes;
}

TexObjSnapshotScope::TexObjSnapshotScope(uint32_t addr, const TexObjBytes& bytes) noexcept {
    t_texObjSnapshotAddr = addr;
    t_texObjSnapshotBytes = bytes.data();
}

TexObjSnapshotScope::~TexObjSnapshotScope() {
    t_texObjSnapshotAddr = 0;
    t_texObjSnapshotBytes = nullptr;
}

TexObjMeta ExtractTexObjMetaFromGuest(uint32_t addr) {
    TexObjMeta meta{};
    try {
        uint32_t word0 = ReadTexObj32(addr, 0x00);
        uint32_t word1 = ReadTexObj32(addr, 0x04);
        uint32_t word2 = ReadTexObj32(addr, 0x08);
        uint32_t word3 = ReadTexObj32(addr, 0x0C);
        uint32_t word4 = ReadTexObj32(addr, 0x10);
        uint32_t word5 = ReadTexObj32(addr, 0x14);
        uint32_t word6 = ReadTexObj32(addr, 0x18);
        uint8_t flags = ReadTexObj8(addr, 0x1F);
        
        meta.wrapS = word0 & 0x3;
        meta.wrapT = (word0 >> 2) & 0x3;
        meta.width = (word2 & 0x3FF) + 1;
        meta.height = ((word2 >> 10) & 0x3FF) + 1;
        meta.dataAddr = CanonicalizeGxMainRamAddress((word3 & 0xFFFFFF) << 5);
        meta.userData = word4;
        meta.mipmap = (flags & 1) != 0;

        const uint32_t formatFromWord2 = (word2 >> 20) & 0xF;
        uint32_t resolvedFormat = word5;
        if (!IsKnownTexFormat(resolvedFormat) && IsKnownTexFormat(formatFromWord2)) {
            resolvedFormat = formatFromWord2;
        }
        meta.format = resolvedFormat;
        meta.tlut = word6;
        
        // LOD parameters from GXGetTexObjLODAll
        // minLod = ((word1 & 0xff) / 16.0f)
        // maxLod = (((word1 >> 8) & 0xff) / 16.0f)
        meta.minLod = (float)(word1 & 0xFF) / 16.0f;
        meta.maxLod = (float)((word1 >> 8) & 0xFF) / 16.0f;
        
        // lodBias from bits [17:9] as signed 8-bit, scaled by 1/32
        int8_t biasRaw = (int8_t)((word0 >> 9) & 0xFF);
        meta.lodBias = (float)biasRaw / 32.0f;
        
        // biasClamp from bit 21
        meta.biasClamp = ((word0 >> 21) & 1) != 0;
        
        // edgeLod from countLeadingZeros((word0 >> 8) & 1) >> 5
        // If bit 8 is set, edgeLod=0, else edgeLod=1
        meta.edgeLod = ((word0 >> 8) & 1) == 0;
        
        // maxAniso from bits [20:19]
        meta.maxAniso = (word0 >> 19) & 0x3;
        
        // Filter modes - bits [7:5] store the encoded HW value written by
        // GX2HWFiltConv. This is the inverse of kGxToHwMinFilter in gx_texture.cpp.
        static const uint8_t filterLut[] = {0, 2, 4, 0, 1, 3, 5, 0};
        meta.minFilter = filterLut[(word0 >> 5) & 7];
        meta.magFilter = (word0 >> 4) & 1;

        // GXInitTexObj sets flags bit 1 for non-CI formats, but GXInitTexObjCI clears it.
        // Accept CI textures even when bit 1 is cleared, as long as the payload is sane.
        bool isValid = (flags & 0x02) != 0;
        if (!isValid) {
            if (IsKnownTexFormat(meta.format) && meta.dataAddr != 0) {
                isValid = true;
            }
        }
        if (!isValid) {
            return TexObjMeta{};
        }
        
    } catch (...) {
        // If we can't read the guest memory, return empty meta
    }
    return meta;
}

namespace {

bool IsUsableTexObjMeta(const TexObjMeta& meta) {
    return meta.dataAddr != 0 &&
           meta.width != 0 &&
           meta.height != 0 &&
           IsKnownTexFormat(meta.format);
}

bool TryBuildTexObjIntervalEntry(TexObjSlot& slot, TexObjIntervalEntry& outEntry) {
    if (!IsUsableTexObjMeta(slot)) {
        return false;
    }

    const uint8_t maxLod = slot.maxLod > 0.0f ? static_cast<uint8_t>(std::min(slot.maxLod, 255.0f)) : 0u;
    const uint32_t texSize =
        GXGetTexBufferSize(slot.width, slot.height, slot.format, static_cast<GXBool>(slot.mipmap), maxLod);
    if (texSize == 0) {
        return false;
    }

    outEntry = TexObjIntervalEntry{
        .dataAddr = CanonicalizeGxMainRamAddress(slot.dataAddr),
        .size = texSize,
        .objAddr = slot.objAddr,
        .slot = &slot,
    };
    return true;
}

// --- Slot lookup -------------------------------------------------------------

TexObjSlot* FindTexObjSlot(uint32_t addr) {
    if (g_texObjSlotMemo != nullptr && g_texObjSlotMemoAddr == addr) {
        return g_texObjSlotMemo;
    }
    const auto it = g_TexObjMeta.find(addr);
    if (it == g_TexObjMeta.end()) {
        return nullptr;
    }
    g_texObjSlotMemoAddr = addr;
    g_texObjSlotMemo = &it->second;
    return g_texObjSlotMemo;
}

TexObjSlot& FindOrCreateTexObjSlot(uint32_t addr) {
    if (TexObjSlot* existing = FindTexObjSlot(addr)) {
        return *existing;
    }
    if (g_TexObjMeta.empty()) {
        g_TexObjMeta.reserve(kTexObjTableReserve);
    }
    TexObjSlot& slot = g_TexObjMeta[addr];
    slot.objAddr = addr;
    g_texObjSlotMemoAddr = addr;
    g_texObjSlotMemo = &slot;
    return slot;
}

// --- DCStoreRange interval index --------------------------------------------

// The 64 KiB buckets [first, last] that bytes [start, start + size) touch (size > 0).
inline uint32_t FirstTexObjBucket(uint64_t start) { return static_cast<uint32_t>(start >> kTexObjBucketShift); }
inline uint32_t LastTexObjBucket(uint64_t start, uint64_t size) {
    return static_cast<uint32_t>((start + size - 1u) >> kTexObjBucketShift);
}

void IndexTexObjSlotLocked(TexObjSlot& slot, uint32_t dataAddr, uint32_t size) {
    const uint32_t last = LastTexObjBucket(dataAddr, size);
    for (uint64_t bucket = FirstTexObjBucket(dataAddr); bucket <= last; ++bucket) {
        g_texObjBuckets[static_cast<uint32_t>(bucket)].push_back(&slot);
    }
    slot.indexedDataAddr = dataAddr;
    slot.indexedSize = size;
    slot.inIntervalIndex = true;
}

void UnindexTexObjSlotLocked(TexObjSlot& slot) {
    if (slot.inIntervalIndex) {
        const uint32_t last = LastTexObjBucket(slot.indexedDataAddr, slot.indexedSize);
        for (uint64_t bucket = FirstTexObjBucket(slot.indexedDataAddr); bucket <= last; ++bucket) {
            const auto it = g_texObjBuckets.find(static_cast<uint32_t>(bucket));
            if (it == g_texObjBuckets.end()) {
                continue;
            }
            auto& list = it->second;
            const auto pos = std::find(list.begin(), list.end(), &slot);
            if (pos != list.end()) {
                *pos = list.back();
                list.pop_back();
            }
        }
    }
    slot.indexedDataAddr = 0;
    slot.indexedSize = 0;
    slot.inIntervalIndex = false;
}

void RebuildTexObjIntervalsLocked() {
    for (auto& [bucket, list] : g_texObjBuckets) {
        (void)bucket;
        list.clear();
    }
    for (auto& entry : g_TexObjMeta) {
        TexObjSlot& slot = entry.second;
        slot.pendingIntervalUpdate = false;
        slot.inIntervalIndex = false;
        TexObjIntervalEntry interval{};
        if (TryBuildTexObjIntervalEntry(slot, interval)) {
            IndexTexObjSlotLocked(slot, interval.dataAddr, interval.size);
        } else {
            slot.indexedDataAddr = 0;
            slot.indexedSize = 0;
        }
    }
    g_texObjIntervalsRebuildAll = false;
    g_texObjIntervalsPending.clear();
}

// Records that the caller is about to mutate the metadata of `slot`. The slot
// remembers what it currently contributes to the index, so this only has to be
// an O(1) enqueue with an O(1) dedupe - no linear scan and, critically, no
// capacity limit that silently degrades into a full rebuild.
void QueueTexObjIntervalUpdate(TexObjSlot& slot) {
    if (g_texObjIntervalsRebuildAll || slot.pendingIntervalUpdate) {
        return;
    }
    slot.pendingIntervalUpdate = true;
    g_texObjIntervalsPending.push_back(&slot);
}

// Metadata writebacks that leave the interval untouched - the overwhelming
// majority, since GXLoadTexObj stores its metadata back twice per call - cost
// one GXGetTexBufferSize and two compares; a move touches only its buckets.
void PatchTexObjIntervalEntryLocked(TexObjSlot& slot) {
    TexObjIntervalEntry fresh{};
    const bool hasFresh = TryBuildTexObjIntervalEntry(slot, fresh);
    if (slot.inIntervalIndex && hasFresh && slot.indexedDataAddr == fresh.dataAddr &&
        slot.indexedSize == fresh.size) {
        return;
    }
    UnindexTexObjSlotLocked(slot);
    if (hasFresh) {
        IndexTexObjSlotLocked(slot, fresh.dataAddr, fresh.size);
    }
}

void EnsureTexObjIntervalsLocked() {
    if (g_texObjIntervalsRebuildAll) {
        RebuildTexObjIntervalsLocked();
        return;
    }
    for (TexObjSlot* slot : g_texObjIntervalsPending) {
        slot->pendingIntervalUpdate = false;
        PatchTexObjIntervalEntryLocked(*slot);
    }
    g_texObjIntervalsPending.clear();
}

void EnsureTlutObjBoundsLocked() {
    if (!g_tlutObjBoundsDirty) {
        return;
    }

    uint64_t boundsStart = std::numeric_limits<uint64_t>::max();
    uint64_t boundsEnd = 0;
    for (const auto& [objAddr, meta] : g_TlutObjMeta) {
        (void)objAddr;
        if (meta.dataAddr == 0 || meta.entries == 0 || !IsKnownTlutFormat(meta.format)) {
            continue;
        }
        const uint64_t tlutStart = CanonicalizeGxMainRamAddress(meta.dataAddr);
        const uint64_t tlutEnd = tlutStart + static_cast<uint64_t>(meta.entries) * 2u;
        boundsStart = std::min(boundsStart, tlutStart);
        boundsEnd = std::max(boundsEnd, tlutEnd);
    }
    if (boundsStart >= boundsEnd) {
        boundsStart = 0;
        boundsEnd = 0;
    }

    g_tlutObjBoundsStart = boundsStart;
    g_tlutObjBoundsEnd = boundsEnd;
    g_tlutObjBoundsDirty = false;
}

bool TexObjBackingDiffers(const TexObjMeta& lhs, const TexObjMeta& rhs) {
    return CanonicalizeGxMainRamAddress(lhs.dataAddr) != CanonicalizeGxMainRamAddress(rhs.dataAddr) ||
           lhs.width != rhs.width ||
           lhs.height != rhs.height ||
           lhs.format != rhs.format ||
           lhs.mipmap != rhs.mipmap ||
           lhs.maxLod != rhs.maxLod;
}

bool TexObjMetaDiffers(const TexObjMeta& lhs, const TexObjMeta& rhs) {
    return CanonicalizeGxMainRamAddress(lhs.dataAddr) != CanonicalizeGxMainRamAddress(rhs.dataAddr) ||
           lhs.width != rhs.width ||
           lhs.height != rhs.height ||
           lhs.format != rhs.format ||
           lhs.wrapS != rhs.wrapS ||
           lhs.wrapT != rhs.wrapT ||
           lhs.mipmap != rhs.mipmap ||
           lhs.minFilter != rhs.minFilter ||
           lhs.magFilter != rhs.magFilter ||
           lhs.minLod != rhs.minLod ||
           lhs.maxLod != rhs.maxLod ||
           lhs.lodBias != rhs.lodBias ||
           lhs.biasClamp != rhs.biasClamp ||
           lhs.edgeLod != rhs.edgeLod ||
           lhs.maxAniso != rhs.maxAniso ||
           lhs.tlut != rhs.tlut ||
           lhs.userData != rhs.userData;
}

TexObjMeta MergeGuestTexObjMeta(const TexObjMeta& cached, const TexObjMeta& guest) {
    TexObjMeta merged = guest;
    merged.needsUpload = cached.needsUpload;
    if (TexObjBackingDiffers(cached, guest)) {
        merged.needsUpload = true;
    }
    return merged;
}

// Reads the raw 32 guest bytes of a GXTexObj struct. Returns false (and the
// caller must treat the shadow as absent) when the address is unreadable.
bool ReadTexObjShadow(uint32_t addr, uint64_t (&out)[4]) noexcept {
    try {
        out[0] = ReadTexObj64(addr, 0x00);
        out[1] = ReadTexObj64(addr, 0x08);
        out[2] = ReadTexObj64(addr, 0x10);
        out[3] = ReadTexObj64(addr, 0x18);
    } catch (...) {
        return false;
    }
    return true;
}

bool ShadowEquals(const uint64_t (&a)[4], const uint64_t (&b)[4]) noexcept {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// Captures the current guest bytes as the slot's shadow so the cache is marked in sync. HLE
// GXInitTexObj* callers fetch the meta reference before rewriting guest memory, so their capture
// is intentionally stale; the next lookup just re-decodes once, which is the safe direction.
void MarkTexObjSyncedLocked(TexObjSlot& slot) noexcept {
    slot.hasGuestShadow = ReadTexObjShadow(slot.objAddr, slot.guestShadow);
}

} // namespace

bool TryGetOrExtractTexObjMeta(uint32_t addr, TexObjMeta& outMeta) {
    TexObjSlot* slot = FindTexObjSlot(addr);
    if (slot != nullptr) {
        const bool cachedUsable = IsUsableTexObjMeta(*slot);
        if (cachedUsable) {
            // Fast path: guest bytes are byte-identical to what the cache was decoded from. The
            // full 32-byte compare (not a generation stamp) is what catches plain guest stores
            // over the struct that never issue a cache op; see TexObjSlot::guestShadow.
            uint64_t shadow[4];
            if (ReadTexObjShadow(addr, shadow) && slot->hasGuestShadow &&
                ShadowEquals(slot->guestShadow, shadow)) {
                outMeta = *slot;
                return true;
            }

            const TexObjMeta extracted = ExtractTexObjMetaFromGuest(addr);
            if (IsUsableTexObjMeta(extracted) && TexObjMetaDiffers(*slot, extracted)) {
                // The caller writes the merged result back through
                // GetTexObjMeta, which is what re-establishes the sync.
                outMeta = MergeGuestTexObjMeta(*slot, extracted);
                return true;
            }
            MarkTexObjSyncedLocked(*slot);
            outMeta = *slot;
            return true;
        }
        // Cached metadata can be created early (e.g., by LOD/filter calls) before
        // full InitTexObj has populated dimensions/address. Re-extract from guest
        // memory in that case instead of propagating the stale zeroed entry.
        TexObjMeta extracted = ExtractTexObjMetaFromGuest(addr);
        const bool extractedUsable = IsUsableTexObjMeta(extracted);
        if (extractedUsable) {
            QueueTexObjIntervalUpdate(*slot);
            static_cast<TexObjMeta&>(*slot) = extracted;
            MarkTexObjSyncedLocked(*slot);
            outMeta = extracted;
            return true;
        }
        // Keep legacy behavior for callers that still want to inspect invalid meta.
        outMeta = *slot;
        return true;
    }
    // No slot yet: decode the guest struct without creating one, exactly as
    // before. The first GXLoadTexObj writeback is what materialises the slot.
    outMeta = ExtractTexObjMetaFromGuest(addr);
    if (outMeta.width > 0 && outMeta.height > 0) {
        return true;
    }
    return false;
}

// Single choke point for texobj metadata writes: callers mutate the returned reference, so this
// queues the interval-index patch and re-captures the guest shadow before that happens.
TexObjMeta& GetTexObjMeta(uint32_t addr) {
    TexObjSlot& slot = FindOrCreateTexObjSlot(addr);
    QueueTexObjIntervalUpdate(slot);
    MarkTexObjSyncedLocked(slot);
    return slot;
}

// Same contract as GetTexObjMeta: the caller mutates the returned reference, so
// the cached TLUT address bounds have to be recomputed before the next flush.
TlutObjMeta& GetTlutObjMeta(uint32_t addr) {
    g_tlutObjBoundsDirty = true;
    return g_TlutObjMeta[addr];
}

GXTexObj* GetHostTexObj(uint32_t addr) {
    TexObjSlot* slot = FindTexObjSlot(addr);
    if (slot == nullptr || !slot->host || !slot->host->constructed) {
        const CpuContext* cpu = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX,
                "GXTex: invalid GXTexObj @0x%08X (PC=0x%08X, LR=0x%08X, CTR=0x%08X)\n",
                addr, cpu ? cpu->pc : 0u, cpu ? cpu->lr : 0u, cpu ? cpu->ctr : 0u);
        std::fflush(stderr);
        GXTexObj* obj = CreateHostTexObj(addr);
        MarkHostTexObjConstructed(addr);
        return obj;
    }
    return slot->host->PublicPtr();
}

GXTexObj* CreateHostTexObj(uint32_t addr) {
    TexObjSlot& slot = FindOrCreateTexObjSlot(addr);
    // GXInitTexObj/GXInitTexObjCI land here and rewrite the entire guest struct,
    // resetting the LOD/filter words the HLE does not mirror into this cache.
    // The shadow captured before that rewrite mismatches afterwards, so the
    // next lookup takes exactly one guest re-decode and picks those defaults
    // (and the computed mipmap maxLod) up.
    if (!slot.host) {
        slot.host = std::make_unique<HleTexObj>();
    } else {
        slot.host->Destroy();
    }
    slot.host->EnsureStorageLive();
    return slot.host->PublicPtr();
}

void MarkHostTexObjConstructed(uint32_t addr) {
    TexObjSlot* slot = FindTexObjSlot(addr);
    if (slot != nullptr && slot->host) {
        slot->host->constructed = true;
    }
}

GXTexObj* TryGetHostTexObj(uint32_t addr) {
    TexObjSlot* slot = FindTexObjSlot(addr);
    if (slot != nullptr && slot->host && slot->host->constructed) {
        return slot->host->PublicPtr();
    }
    return nullptr;
}

void MarkTexObjsDirtyForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    // Do this even when no GXTexObj currently describes the range. Games
    // commonly fill and flush a newly recycled heap block before constructing
    // its texture object; the flush is the only authoritative indication that
    // an older GPU-only EFB copy at that address is no longer the source.
    InvalidateEfbCopyDestinationsForRange(addr, size);

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::lock_guard<std::mutex> guard(g_texObjMutex);

    EnsureTexObjIntervalsLocked();
    // Every slot overlapping the range is in at least one of the range's buckets.
    const auto markOverlapping = [&](const std::vector<TexObjSlot*>& list) {
        for (TexObjSlot* slot : list) {
            const uint64_t texStart = slot->indexedDataAddr;
            if (texStart < dirtyEnd && texStart + slot->indexedSize > dirtyStart) {
                slot->needsUpload = true;
            }
        }
    };
    const uint32_t first = FirstTexObjBucket(dirtyStart);
    const uint32_t last = LastTexObjBucket(dirtyStart, size);
    if (static_cast<uint64_t>(last) - first + 1u > g_texObjBuckets.size()) {
        // A range wider than the index (a whole-heap flush): visit the buckets there are instead.
        for (const auto& [bucket, list] : g_texObjBuckets) {
            if (bucket >= first && bucket <= last) {
                markOverlapping(list);
            }
        }
        return;
    }
    for (uint64_t bucket = first; bucket <= last; ++bucket) {
        const auto it = g_texObjBuckets.find(static_cast<uint32_t>(bucket));
        if (it != g_texObjBuckets.end()) {
            markOverlapping(it->second);
        }
    }
}

// Defined in gx_dl.cpp.
extern "C" void GxNotifyDisplayListMemoryWrite(uint32_t addr, uint32_t size);

// Single entry point for DMA-class host writes into guest RAM (DVD reads, DCZeroRange, locked
// cache stores) that the SDK brackets with DCInvalidateRange rather than a flush, so the normal
// DC flush/store interceptors never see them. Without this call, GX caches keyed on guest bytes
// keep serving stale contents and a reloaded track reuses old texture pixels at the same address.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    // Ordered with recorded GX work: retiring EFB copy textures emits aurora commands.
    GxWorker::Run([addr, size] {
        MarkTexObjsDirtyForRange(addr, size);
        MarkTlutObjsDirtyForRange(addr, size);
    });
    GxNotifyDisplayListMemoryWrite(addr, size);
}

GXTlutObj* CreateHostTlutObj(uint32_t addr) {
    auto& slot = g_HostTlutObjMap[addr];
    if (!slot) {
        slot = std::make_unique<HleTlutObj>();
    } else {
        slot->Destroy();
    }
    slot->EnsureStorageLive();
    return slot->PublicPtr();
}

void MarkHostTlutObjConstructed(uint32_t addr) {
    auto it = g_HostTlutObjMap.find(addr);
    if (it != g_HostTlutObjMap.end() && it->second) {
        it->second->constructed = true;
    }
}

GXTlutObj* GetHostTlutObj(uint32_t addr) {
    auto it = g_HostTlutObjMap.find(addr);
    if (it == g_HostTlutObjMap.end() || !it->second || !it->second->constructed) {
        const CpuContext* cpu = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX,
                "GXTex: invalid GXTlutObj @0x%08X (PC=0x%08X, LR=0x%08X, CTR=0x%08X)\n",
                addr, cpu ? cpu->pc : 0u, cpu ? cpu->lr : 0u, cpu ? cpu->ctr : 0u);
        std::fflush(stderr);
        GXTlutObj* obj = CreateHostTlutObj(addr);
        MarkHostTlutObjConstructed(addr);
        return obj;
    }
    return it->second->PublicPtr();
}

void MarkTlutObjsDirtyForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::lock_guard<std::mutex> guard(g_tlutObjMutex);
    EnsureTlutObjBoundsLocked();
    // Most DCStoreRange calls flush small material/vertex buffers that live
    // nowhere near a palette, so reject them without walking the map.
    if (g_tlutObjBoundsStart >= g_tlutObjBoundsEnd ||
        dirtyEnd <= g_tlutObjBoundsStart ||
        dirtyStart >= g_tlutObjBoundsEnd) {
        return;
    }
    for (auto& [objAddr, meta] : g_TlutObjMeta) {
        if (meta.dataAddr == 0 || meta.entries == 0 || !IsKnownTlutFormat(meta.format)) {
            continue;
        }

        const uint64_t tlutStart = CanonicalizeGxMainRamAddress(meta.dataAddr);
        const uint64_t tlutEnd = tlutStart + static_cast<uint64_t>(meta.entries) * 2u;
        if (dirtyEnd <= tlutStart || dirtyStart >= tlutEnd) {
            continue;
        }

        meta.dirty = true;
    }
}
