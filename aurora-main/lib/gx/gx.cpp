#include "gx.hpp"

#include "pipeline.hpp"
#include "texture_bind_group_cache_key.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../internal.hpp"
#include "../gfx/common.hpp"
#include "../gfx/tex_palette_conv.hpp"
#include "../gfx/texture.hpp"
#include "../gfx/texture_convert.hpp"
#include "../gfx/texture_replacement.hpp"
#include "gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

static aurora::Module Log("aurora::gx");

namespace aurora::gx {
using webgpu::g_device;
using webgpu::g_graphicsConfig;

namespace {






} // namespace

static GXState make_default_gx_state() {
  GXState state{};
  for (size_t i = 0; i < state.tcgs.size(); ++i) {
    state.tcgs[i].src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + i);
  }
  return state;
}

GXState g_gxState = make_default_gx_state();

static wgpu::Sampler sEmptySampler;
static wgpu::Texture sEmptyTexture;
static wgpu::TextureView sEmptyTextureView;
static std::mutex sBindGroupLayoutMutex;
static absl::flat_hash_map<u32, wgpu::BindGroupLayout> sUniformBindGroupLayouts;
static absl::flat_hash_map<u32, std::pair<wgpu::BindGroupLayout, wgpu::BindGroupLayout>> sTextureBindGroupLayouts;
static wgpu::BindGroupLayout sTextureBindGroupLayout;
static wgpu::BindGroupLayout sSamplerBindGroupLayout;
static wgpu::PipelineLayout sPipelineLayout;
wgpu::BindGroup g_emptyTextureBindGroup;
static u64 s_copyTextureStateRevision = 1;
static u64 s_staticTextureCacheValidationRevision = 1;

// GXInvalidateTexAll means "the guest may have rewritten texture memory, so re-check anything cached from it before trusting it again".
static u64 s_textureCacheStamps = 0;

// Marks a cache artifact as reusable at the current validation revision.
static inline void note_texture_cache_stamp() noexcept { ++s_textureCacheStamps; }

static std::array<u64, MaxTextures> s_lastNoCopyResolveRevision{};
static void mark_copy_texture_cache_changed() noexcept;
static size_t static_texture_source_cache_size() noexcept;
static size_t static_palette_texture_source_cache_size() noexcept;
static size_t texture_object_cache_size() noexcept;

struct TextureResolveIdentity {
  const void* data = nullptr;
  u32 mode0 = 0;
  u32 mode1 = 0;
  u32 image0 = 0;
  u32 image3 = 0;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  GXTlut tlut = GX_TLUT0;
  u32 texObjId = 0;
  u32 texDataVersion = 0;
  u8 flags = 0;

  bool operator==(const TextureResolveIdentity& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const TextureResolveIdentity& identity) {
    return H::combine(std::move(h), identity.data, identity.mode0, identity.mode1, identity.image0,
                      identity.image3, identity.width, identity.height, identity.format, identity.tlut,
                      identity.texObjId, identity.texDataVersion, identity.flags);
  }
};
static std::array<TextureResolveIdentity, MaxTextures> s_lastTextureResolveIdentity{};
static std::array<bool, MaxTextures> s_lastTextureResolveIdentityValid{};

struct TextureResolveIdentityCacheEntry {
  bool valid = false;
  size_t hash = 0;
  u64 copyTextureRevision = 0;
  // Revision this binding was last proved good at.
  u64 validationRevision = 0;
  TextureResolveIdentity identity{};
  gfx::TextureBind binding{};
};

// ponytail: A fixed direct-mapped cache avoids allocator/hash-table overhead.
static std::array<TextureResolveIdentityCacheEntry, 2048> s_textureResolveIdentityCache{};

static TextureResolveIdentity make_texture_resolve_identity(const GXTexObj_& obj) noexcept {
  return {
      .data = obj.data,
      .mode0 = obj.mode0,
      .mode1 = obj.mode1,
      .image0 = obj.image0,
      .image3 = obj.image3,
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .tlut = obj.tlut,
      .texObjId = obj.texObjId,
      .texDataVersion = obj.texDataVersion,
      .flags = obj.flags,
  };
}

namespace {
struct DynamicPaletteKey {
  const void* sourceIdentity = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;

  bool operator==(const DynamicPaletteKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const DynamicPaletteKey& key) {
    return H::combine(std::move(h), key.sourceIdentity, key.width, key.height, key.format);
  }
};

struct DynamicPaletteEntry {
  gfx::TextureHandle handle;
  u32 sourceRevision = 0;
  u32 tlutDataVersion = 0;
};

struct CachedTextureEntry {
  gfx::TextureHandle handle;
  u32 texDataVersion = 0;
  u32 tlutObjId = 0;
  u32 tlutDataVersion = 0;
  // Revision the underlying source bytes were last proved good at.
  u64 validationRevision = 0;
};

struct StaticTextureKey {
  const void* data = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 mips = 0;
  u32 format = 0;
  u32 texDataVersion = 0;

  bool operator==(const StaticTextureKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const StaticTextureKey& key) {
    return H::combine(std::move(h), key.data, key.width, key.height, key.mips, key.format, key.texDataVersion);
  }
};

static std::array<StaticTextureKey, MaxTextures> s_lastStaticSourceResolveKey{};
static std::array<bool, MaxTextures> s_lastStaticSourceResolveKeyValid{};
static std::array<u64, MaxTextures> s_lastStaticSourceNoCopyRevision{};

struct StaticPaletteTextureKey {
  StaticTextureKey texture;
  const void* tlutData = nullptr;
  u32 tlutFormat = 0;
  u32 tlutEntries = 0;
  u32 tlutDataVersion = 0;

  bool operator==(const StaticPaletteTextureKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const StaticPaletteTextureKey& key) {
    return H::combine(std::move(h), key.texture, key.tlutData, key.tlutFormat, key.tlutEntries, key.tlutDataVersion);
  }
};

// Revalidation used to keep a full byte-for-byte copy of every cached source and memcmp against it.
struct StaticTextureSourceEntry {
  gfx::TextureHandle handle;
  u64 digest = 0;
  u32 sourceSize = 0;
  u64 validationRevision = 0;
  u64 sourceGeneration = kGuestWriteUntracked;
};

struct StaticPaletteTextureSourceEntry {
  gfx::TextureHandle handle;
  u64 textureDigest = 0;
  u64 tlutDigest = 0;
  u32 textureSize = 0;
  u32 tlutSize = 0;
  u64 validationRevision = 0;
  u64 textureGeneration = kGuestWriteUntracked;
  u64 tlutGeneration = kGuestWriteUntracked;
};

struct CachedTlutTextureEntry {
  gfx::TextureHandle handle;
  u32 tlutDataVersion = 0;
  u64 digest = 0;
  u64 validationRevision = 0;
  u64 generation = kGuestWriteUntracked;
};

struct TlutObjectCache {
  CachedTlutTextureEntry tlutTexture;
  absl::flat_hash_map<DynamicPaletteKey, DynamicPaletteEntry> dynamicPaletteTextures;
  absl::flat_hash_set<u32> staticTextureUsers;
};

absl::flat_hash_map<u32, CachedTextureEntry> s_textureObjectCaches;
absl::flat_hash_map<u32, TlutObjectCache> s_tlutObjectCaches;
absl::flat_hash_map<StaticTextureKey, StaticTextureSourceEntry> s_staticTextureSourceCache;
absl::flat_hash_map<StaticPaletteTextureKey, StaticPaletteTextureSourceEntry> s_staticPaletteTextureSourceCache;
absl::flat_hash_map<u32, StaticTextureKey> s_texObjSourceKeys;
absl::flat_hash_map<u32, StaticPaletteTextureKey> s_texObjPaletteSourceKeys;

// A repeated bind of the same texture object re-derives the same source key and re-stores it into the map above on every resolve.
template <typename Key>
struct TexObjSourceKeyMemo {
  struct Slot {
    u32 texObjId = 0;
    Key key{};
  };
  std::array<Slot, 8> slots{};

  [[nodiscard]] bool matches(u32 texObjId, const Key& key) const {
    const auto& slot = slots[texObjId & (slots.size() - 1)];
    return slot.texObjId == texObjId && slot.key == key;
  }
  void store(u32 texObjId, const Key& key) {
    auto& slot = slots[texObjId & (slots.size() - 1)];
    slot.texObjId = texObjId;
    slot.key = key;
  }
  void forget(u32 texObjId) {
    auto& slot = slots[texObjId & (slots.size() - 1)];
    if (slot.texObjId == texObjId) {
      slot.texObjId = 0;
    }
  }
  void reset() {
    for (auto& slot : slots) {
      slot.texObjId = 0;
    }
  }
};

TexObjSourceKeyMemo<StaticTextureKey> s_texObjSourceKeyMemo;
TexObjSourceKeyMemo<StaticPaletteTextureKey> s_texObjPaletteSourceKeyMemo;

void record_tex_obj_source_key(u32 texObjId, const StaticTextureKey& key) noexcept {
  if (s_texObjSourceKeyMemo.matches(texObjId, key)) {
    return;
  }
  s_texObjSourceKeys[texObjId] = key;
  s_texObjSourceKeyMemo.store(texObjId, key);
}

void record_tex_obj_palette_source_key(u32 texObjId, const StaticPaletteTextureKey& key) noexcept {
  if (s_texObjPaletteSourceKeyMemo.matches(texObjId, key)) {
    return;
  }
  s_texObjPaletteSourceKeys[texObjId] = key;
  s_texObjPaletteSourceKeyMemo.store(texObjId, key);
}

} // namespace

static size_t static_texture_source_cache_size() noexcept { return s_staticTextureSourceCache.size(); }
static size_t static_palette_texture_source_cache_size() noexcept { return s_staticPaletteTextureSourceCache.size(); }
static size_t texture_object_cache_size() noexcept { return s_textureObjectCaches.size(); }

namespace {

struct StaticSourceFrontCacheEntry {
  bool valid = false;
  size_t hash = 0;
  u64 validationRevision = 0;
  StaticTextureKey key{};
  gfx::TextureHandle handle;
};

static std::array<StaticSourceFrontCacheEntry, 256> s_staticSourceFrontCache;

static gfx::TextureHandle lookup_static_source_front_cache(const StaticTextureKey& key, size_t hash) noexcept {
  const auto& entry = s_staticSourceFrontCache[hash & (s_staticSourceFrontCache.size() - 1)];
  if (entry.valid && entry.validationRevision == s_staticTextureCacheValidationRevision && entry.hash == hash &&
      entry.key == key) {
    return entry.handle;
  }
  return {};
}

static void store_static_source_front_cache(const StaticTextureKey& key, size_t hash,
                                            const gfx::TextureHandle& handle) noexcept {
  auto& entry = s_staticSourceFrontCache[hash & (s_staticSourceFrontCache.size() - 1)];
  entry.valid = true;
  entry.hash = hash;
  entry.validationRevision = s_staticTextureCacheValidationRevision;
  entry.key = key;
  entry.handle = handle;
  note_texture_cache_stamp();
}

static void clear_static_source_front_cache() noexcept {
  for (auto& entry : s_staticSourceFrontCache) {
    entry.valid = false;
    entry.handle.reset();
  }
}

static u32 static_texture_data_size(const StaticTextureKey& key) noexcept {
  if (key.data == nullptr || key.width == 0 || key.height == 0 || key.mips == 0) {
    return 0;
  }
  return GXGetTexBufferSize(static_cast<u16>(key.width), static_cast<u16>(key.height), key.format,
                            key.mips > 1 ? GX_TRUE : GX_FALSE, static_cast<u8>(key.mips - 1));
}

static u64 digest_source_bytes(const void* data, size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return 0;
  }
  return static_cast<u64>(xxh3_hash_s(data, size));
}

static bool source_matches_digest(const void* data, size_t size, u64 digest, u32 expectedSize) noexcept {
  if (data == nullptr || size != expectedSize) {
    return false;
  }
  return digest_source_bytes(data, size) == digest;
}

// A source whose generation still matches the one recorded beside its digest cannot have been written since, so the digest is skipped.
static bool validate_static_texture_source_entry(const StaticTextureKey& key,
                                                 StaticTextureSourceEntry& entry) noexcept {
  if (entry.validationRevision == s_staticTextureCacheValidationRevision) {
    return true;
  }
  const u32 dataSize = static_texture_data_size(key);
  const u64 generation = guest_write_generation(key.data, dataSize);
  if (dataSize == entry.sourceSize && guest_write_generation_matches(entry.sourceGeneration, generation)) {
    entry.validationRevision = s_staticTextureCacheValidationRevision;
    note_texture_cache_stamp();
    return true;
  }
  if (!source_matches_digest(key.data, dataSize, entry.digest, entry.sourceSize)) {
    return false;
  }
  entry.sourceGeneration = generation;
  entry.validationRevision = s_staticTextureCacheValidationRevision;
  note_texture_cache_stamp();
  return true;
}

static bool validate_static_palette_texture_source_entry(const StaticPaletteTextureKey& key,
                                                        StaticPaletteTextureSourceEntry& entry) noexcept {
  if (entry.validationRevision == s_staticTextureCacheValidationRevision) {
    return true;
  }
  const u32 textureDataSize = static_texture_data_size(key.texture);
  const size_t tlutDataSize = static_cast<size_t>(key.tlutEntries) * sizeof(u16);
  const u64 textureGeneration = guest_write_generation(key.texture.data, textureDataSize);
  const u64 tlutGeneration = guest_write_generation(key.tlutData, tlutDataSize);
  const bool skippable = textureDataSize == entry.textureSize && tlutDataSize == entry.tlutSize &&
                         guest_write_generation_matches(entry.textureGeneration, textureGeneration) &&
                         guest_write_generation_matches(entry.tlutGeneration, tlutGeneration);
  if (!skippable) {
    const bool textureOk =
        source_matches_digest(key.texture.data, textureDataSize, entry.textureDigest, entry.textureSize);
    const bool tlutOk = source_matches_digest(key.tlutData, tlutDataSize, entry.tlutDigest, entry.tlutSize);
    if (!textureOk || !tlutOk) {
      return false;
    }
    entry.textureGeneration = textureGeneration;
    entry.tlutGeneration = tlutGeneration;
  }
  entry.validationRevision = s_staticTextureCacheValidationRevision;
  note_texture_cache_stamp();
  return true;
}

static StaticTextureSourceEntry make_static_texture_source_entry(const StaticTextureKey& key,
                                                                const gfx::TextureHandle& handle) {
  const u32 dataSize = static_texture_data_size(key);
  const u64 generation = guest_write_generation(key.data, dataSize);
  note_texture_cache_stamp();
  return {
      .handle = handle,
      .digest = digest_source_bytes(key.data, dataSize),
      .sourceSize = dataSize,
      .validationRevision = s_staticTextureCacheValidationRevision,
      .sourceGeneration = generation,
  };
}

static StaticPaletteTextureSourceEntry make_static_palette_texture_source_entry(
    const StaticPaletteTextureKey& key, const gfx::TextureHandle& handle) {
  const u32 textureDataSize = static_texture_data_size(key.texture);
  const size_t tlutDataSize = static_cast<size_t>(key.tlutEntries) * sizeof(u16);
  const u64 textureGeneration = guest_write_generation(key.texture.data, textureDataSize);
  const u64 tlutGeneration = guest_write_generation(key.tlutData, tlutDataSize);
  note_texture_cache_stamp();
  return {
      .handle = handle,
      .textureDigest = digest_source_bytes(key.texture.data, textureDataSize),
      .tlutDigest = digest_source_bytes(key.tlutData, tlutDataSize),
      .textureSize = textureDataSize,
      .tlutSize = static_cast<u32>(tlutDataSize),
      .validationRevision = s_staticTextureCacheValidationRevision,
      .textureGeneration = textureGeneration,
      .tlutGeneration = tlutGeneration,
  };
}

DynamicPaletteKey make_dynamic_palette_key(const GXTexObj_& obj, const GXState::CopyTextureRef& source) {
  return {
      .sourceIdentity = source.handle.get(),
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
  };
}

StaticTextureKey make_static_texture_key(const GXTexObj_& obj) {
  return {
      .data = obj.data,
      .width = obj.width(),
      .height = obj.height(),
      .mips = obj.mip_count(),
      .format = obj.format(),
      .texDataVersion = obj.texDataVersion,
  };
}

StaticPaletteTextureKey make_static_palette_texture_key(const GXTexObj_& obj, const GXTlutObj_& tlut) {
  return {
      .texture = make_static_texture_key(obj),
      .tlutData = tlut.data,
      .tlutFormat = static_cast<u32>(tlut.format),
      .tlutEntries = tlut.numEntries,
      .tlutDataVersion = tlut.tlutDataVersion,
  };
}

bool can_cache_static_texture_upload(const GXTexObj_& obj) noexcept {
  // THP movie frames are decoded into reused guest buffers and uploaded as YUVA.
  return obj.format() != GX_CTF_YUVA8;
}

void clear_texture_dependency(u32 texObjId, u32 tlutObjId) {
  if (texObjId == 0 || tlutObjId == 0) {
    return;
  }
  if (auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    it->second.staticTextureUsers.erase(texObjId);
    if (!it->second.tlutTexture.handle && it->second.dynamicPaletteTextures.empty() &&
        it->second.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it);
    }
  }
}

void store_cached_texture(const GXTexObj_& obj, gfx::TextureHandle handle, u32 tlutObjId = 0, u32 tlutDataVersion = 0) {
  if (obj.texObjId == 0) {
    return;
  }

  auto& entry = s_textureObjectCaches[obj.texObjId];
  if (entry.tlutObjId != tlutObjId) {
    clear_texture_dependency(obj.texObjId, entry.tlutObjId);
  }

  entry.handle = std::move(handle);
  entry.texDataVersion = obj.texDataVersion;
  entry.tlutObjId = tlutObjId;
  entry.tlutDataVersion = tlutDataVersion;
  entry.validationRevision = s_staticTextureCacheValidationRevision;
  note_texture_cache_stamp();

  if (tlutObjId != 0) {
    s_tlutObjectCaches[tlutObjId].staticTextureUsers.insert(obj.texObjId);
  }
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
  const size_t tlutSize = static_cast<size_t>(tlut.numEntries) * sizeof(u16);
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    auto& cached = cache.tlutTexture;
    if (cached.handle && cached.tlutDataVersion == tlut.tlutDataVersion) {
      if (cached.validationRevision == s_staticTextureCacheValidationRevision) {
        return cached.handle;
      }
      // GXInvalidateTexAll: the guest may have rewritten this palette in place without re-running GXInitTlutObj, which is the only thing that moves tlutDataVersion.
      bool stillValid = true;
      if (tlut.data != nullptr) {
        const u64 generation = guest_write_generation(tlut.data, tlutSize);
        if (!guest_write_generation_matches(cached.generation, generation)) {
          stillValid = digest_source_bytes(tlut.data, tlutSize) == cached.digest;
          cached.generation = generation;
        }
      }
      if (stillValid) {
        cached.validationRevision = s_staticTextureCacheValidationRevision;
        note_texture_cache_stamp();
        return cached.handle;
      }
    }
    cache.dynamicPaletteTextures.clear();
    for (const u32 texObjId : cache.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    cache.staticTextureUsers.clear();
  }

  const auto handle =
      gfx::new_static_texture_2d(tlut.numEntries, 1, 1, gfx::tlut_texture_format(tlut.format),
                                 {static_cast<const u8*>(tlut.data), tlutSize}, true, "Loaded TLUT");
  if (tlut.tlutObjId != 0) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    const u64 generation = guest_write_generation(tlut.data, tlutSize);
    cache.tlutTexture.handle = handle;
    cache.tlutTexture.tlutDataVersion = tlut.tlutDataVersion;
    cache.tlutTexture.digest = digest_source_bytes(tlut.data, tlutSize);
    cache.tlutTexture.generation = generation;
    cache.tlutTexture.validationRevision = s_staticTextureCacheValidationRevision;
    note_texture_cache_stamp();
  }
  return handle;
}

gfx::TextureHandle resolve_static_texture(const GXTexObj_& obj) {
  ZoneScoped;
  const bool canCacheUpload = !obj.no_cache() && can_cache_static_texture_upload(obj);

  if (canCacheUpload && obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      const auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == 0 &&
          entry.validationRevision == s_staticTextureCacheValidationRevision) {
        return entry.handle;
      }
    }
  }

  const bool canUseSourceCache = canCacheUpload && obj.data != nullptr;
  const StaticTextureKey sourceKey = canUseSourceCache ? make_static_texture_key(obj) : StaticTextureKey{};
  const size_t sourceHash = canUseSourceCache ? absl::Hash<StaticTextureKey>{}(sourceKey) : 0;
  if (canUseSourceCache) {
    if (auto handle = lookup_static_source_front_cache(sourceKey, sourceHash)) {
      if (obj.texObjId != 0) {
        record_tex_obj_source_key(obj.texObjId, sourceKey);
      }
      return handle;
    }
    if (auto it = s_staticTextureSourceCache.find(sourceKey); it != s_staticTextureSourceCache.end()) {
      if (!validate_static_texture_source_entry(sourceKey, it->second)) {
        s_staticTextureSourceCache.erase(it);
        clear_static_source_front_cache();
      } else {
        const auto& handle = it->second.handle;
        if (obj.texObjId != 0) {
          record_tex_obj_source_key(obj.texObjId, sourceKey);
        }
        store_static_source_front_cache(sourceKey, sourceHash, handle);
        return handle;
      }
    }
  }

  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj); replacement.has_value()) {
    handle = *replacement;
  } else {
#if DEBUG
    const auto name = gfx::texture_replacement::build_texture_replacement_name(obj);
    const auto nameStr = name.c_str();
#else
    const auto nameStr = "GX Static Texture";
#endif
    const u32 dataSize = GXGetTexBufferSize(static_cast<u16>(obj.width()), static_cast<u16>(obj.height()),
                                            obj.format(), obj.has_mips(),
                                            obj.has_mips() ? static_cast<u8>(obj.mip_count() - 1) : 0);
    handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),
                                        {static_cast<const uint8_t*>(obj.data), dataSize}, false, nameStr);
  }
  if (canCacheUpload) {
    store_cached_texture(obj, handle);
    if (canUseSourceCache) {
      if (s_staticTextureSourceCache.size() >= 512) {
        absl::erase_if(s_staticTextureSourceCache, [](const auto& item) { return item.second.handle.use_count() <= 1; });
        clear_static_source_front_cache();
      }
      s_staticTextureSourceCache[sourceKey] = make_static_texture_source_entry(sourceKey, handle);
      store_static_source_front_cache(sourceKey, sourceHash, handle);
      if (obj.texObjId != 0) {
        record_tex_obj_source_key(obj.texObjId, sourceKey);
      }
    }
  }
  return handle;
}

gfx::TextureHandle resolve_static_palette_texture(const GXTexObj_& obj, const GXTlutObj_& tlut) {
  ZoneScoped;

  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      const auto& entry = it->second;
      if (entry.handle && entry.texDataVersion == obj.texDataVersion && entry.tlutObjId == tlut.tlutObjId &&
          entry.tlutDataVersion == tlut.tlutDataVersion &&
          entry.validationRevision == s_staticTextureCacheValidationRevision) {
        return entry.handle;
      }
    }
  }

  const bool canUseSourceCache = !obj.no_cache() && !tlut.no_cache() && obj.data != nullptr && tlut.data != nullptr;
  const StaticPaletteTextureKey sourceKey =
      canUseSourceCache ? make_static_palette_texture_key(obj, tlut) : StaticPaletteTextureKey{};
  if (canUseSourceCache) {
    if (auto it = s_staticPaletteTextureSourceCache.find(sourceKey);
        it != s_staticPaletteTextureSourceCache.end()) {
      if (!validate_static_palette_texture_source_entry(sourceKey, it->second)) {
        s_staticPaletteTextureSourceCache.erase(it);
      } else {
        const auto& handle = it->second.handle;
        if (obj.texObjId != 0) {
          store_cached_texture(obj, handle, tlut.tlutObjId, tlut.tlutDataVersion);
          record_tex_obj_palette_source_key(obj.texObjId, sourceKey);
        }
        return handle;
      }
    }
  }

  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj); replacement.has_value()) {
    handle = *replacement;
  } else {
    const u32 dataSize = GXGetTexBufferSize(static_cast<u16>(obj.width()), static_cast<u16>(obj.height()),
                                            obj.format(), obj.has_mips(),
                                            obj.has_mips() ? static_cast<u8>(obj.mip_count() - 1) : 0);
    auto converted = gfx::convert_texture_palette(
        obj.format(), obj.width(), obj.height(), obj.mip_count(), {static_cast<const u8*>(obj.data), dataSize},
        tlut.format, tlut.numEntries, {static_cast<const u8*>(tlut.data), static_cast<size_t>(tlut.numEntries) * 2});
    if (converted.data.empty()) {
      return {};
    }
    handle =
        gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), GX_TF_RGBA8_PC,
                                   {converted.data.data(), converted.data.size()}, false, "GX Static Palette Texture");
    handle->hasArbitraryMips = converted.hasArbitraryMips;
  }
  if (!obj.no_cache() && !tlut.no_cache()) {
    store_cached_texture(obj, handle, tlut.tlutObjId, tlut.tlutDataVersion);
    if (canUseSourceCache) {
      if (s_staticPaletteTextureSourceCache.size() >= 256) {
        absl::erase_if(s_staticPaletteTextureSourceCache, [](const auto& item) { return item.second.handle.use_count() <= 1; });
      }
      s_staticPaletteTextureSourceCache[sourceKey] = make_static_palette_texture_source_entry(sourceKey, handle);
      if (obj.texObjId != 0) {
        record_tex_obj_palette_source_key(obj.texObjId, sourceKey);
      }
    }
  }
  return handle;
}

gfx::TextureHandle resolve_dynamic_palette_texture(const GXTexObj_& obj, const GXState::CopyTextureRef& source,
                                                   const GXTlutObj_& tlut) {
  ZoneScoped;

  const auto tlutHandle = get_tlut_texture(tlut);
  auto& tlutCache = s_tlutObjectCaches[tlut.tlutObjId];
  if (tlutCache.dynamicPaletteTextures.size() >= 32) {
    absl::erase_if(tlutCache.dynamicPaletteTextures, [](const auto& item) { return item.second.handle.use_count() <= 1; });
  }
  auto& entry = tlutCache.dynamicPaletteTextures[make_dynamic_palette_key(obj, source)];
  if (!entry.handle) {
    // Use source size instead of target (logical) size
    entry.handle = gfx::new_conv_texture(source.handle->size.width, source.handle->size.height, GX_TF_RGBA8,
                                         "GX Dynamic Palette Texture");
  }
  if (entry.sourceRevision != source.revision || entry.tlutDataVersion != tlut.tlutDataVersion) {
    gfx::queue_palette_conv({
        .variant = obj.format() == GX_TF_C4 ? gfx::tex_palette_conv::Variant::FromFloat4
                                            : gfx::tex_palette_conv::Variant::FromFloat8,
        .src = source.handle,
        .dst = entry.handle,
        .tlut = tlutHandle,
    });
    entry.sourceRevision = source.revision;
    entry.tlutDataVersion = tlut.tlutDataVersion;
  }
  return entry.handle;
}

u32 resolved_format_for_handle(const gfx::TextureHandle& handle) {
  if (!handle) {
    return GX_TF_RGBA8;
  }
  if (handle->gxFormat != gfx::InvalidTextureFormat) {
    return handle->gxFormat;
  }
  return GX_TF_RGBA8_PC;
}
} // namespace

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

namespace {
struct ScissorRange {
  s32 offset = 0;
  s32 start = 0;
  s32 end = 0;
};

struct ScissorRect {
  gfx::ClipRect rect{};
  s32 xOff = 0;
  s32 yOff = 0;
};

std::array<ScissorRange, 9> compute_scissor_ranges(s32 start, s32 end, s32 offset, s32 efbDim, u32& count) noexcept {
  std::array<ScissorRange, 9> ranges{};
  count = 0;
  for (s32 extraOff = -4096; extraOff <= 4096; extraOff += 1024) {
    const s32 newOff = offset + extraOff;
    const s32 newStart = std::clamp(start - newOff, 0, efbDim);
    const s32 newEnd = std::clamp(end - newOff + 1, 0, efbDim);
    if (newStart < newEnd && count < ranges.size()) {
      ranges[count++] = {
          .offset = newOff,
          .start = newStart,
          .end = newEnd,
      };
    }
  }
  return ranges;
}

int scissor_viewport_area(const ScissorRect& rect, const gfx::Viewport& viewport) noexcept {
  const float viewportLeft = std::min(viewport.left, viewport.left + viewport.width);
  const float viewportRight = std::max(viewport.left, viewport.left + viewport.width);
  const float viewportTop = std::min(viewport.top, viewport.top + viewport.height);
  const float viewportBottom = std::max(viewport.top, viewport.top + viewport.height);

  const float left = std::clamp(static_cast<float>(rect.rect.x + rect.xOff), viewportLeft, viewportRight);
  const float right =
      std::clamp(static_cast<float>(rect.rect.x + rect.rect.width + rect.xOff), viewportLeft, viewportRight);
  const float top = std::clamp(static_cast<float>(rect.rect.y + rect.yOff), viewportTop, viewportBottom);
  const float bottom =
      std::clamp(static_cast<float>(rect.rect.y + rect.rect.height + rect.yOff), viewportTop, viewportBottom);

  return static_cast<int>(std::max(right - left, 0.0f) * std::max(bottom - top, 0.0f));
}

int scissor_area(const ScissorRect& rect) noexcept {
  return rect.rect.width * rect.rect.height;
}

ScissorRect best_scissor_rect(const gfx::ClipRect& logicalScissor, const gfx::Viewport& logicalViewport) noexcept {
  if (logicalScissor.width <= 0 || logicalScissor.height <= 0) {
    return {
        .rect = {1000, 1000, 1, 1},
        .xOff = 0,
        .yOff = 0,
    };
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const s32 efbWidth = static_cast<s32>(logicalFbWidth);
  const s32 efbHeight = static_cast<s32>(logicalFbHeight);
  const s32 left = logicalScissor.x;
  const s32 right = logicalScissor.x + logicalScissor.width - 1;
  const s32 top = logicalScissor.y;
  const s32 bottom = logicalScissor.y + logicalScissor.height - 1;

  u32 xCount = 0;
  u32 yCount = 0;
  const auto xRanges = compute_scissor_ranges(left, right, g_gxState.scissorOffsetX, efbWidth, xCount);
  const auto yRanges = compute_scissor_ranges(top, bottom, g_gxState.scissorOffsetY, efbHeight, yCount);
  if (xCount == 0 || yCount == 0) {
    return {
        .rect = {1000, 1000, 1, 1},
        .xOff = 0,
        .yOff = 0,
    };
  }

  std::optional<ScissorRect> best;
  for (u32 x = 0; x < xCount; ++x) {
    for (u32 y = 0; y < yCount; ++y) {
      const ScissorRect candidate{
          .rect =
              {
                  .x = xRanges[x].start,
                  .y = yRanges[y].start,
                  .width = xRanges[x].end - xRanges[x].start,
                  .height = yRanges[y].end - yRanges[y].start,
              },
          .xOff = xRanges[x].offset,
          .yOff = yRanges[y].offset,
      };
      if (!best) {
        best = candidate;
        continue;
      }

      const int candidateViewportArea = scissor_viewport_area(candidate, logicalViewport);
      const int bestViewportArea = scissor_viewport_area(*best, logicalViewport);
      if (candidateViewportArea > bestViewportArea ||
          (candidateViewportArea == bestViewportArea && scissor_area(candidate) > scissor_area(*best))) {
        best = candidate;
      }
    }
  }

  return *best;
}

float logical_to_target_x(float value) noexcept {
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return value;
  }
  return value * static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
}

float logical_to_target_y(float value) noexcept {
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return value;
  }
  return value * static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
}

gfx::ClipRect scale_logical_scissor_rect(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  const float left = logical_to_target_x(static_cast<float>(logicalScissor.x));
  const float top = logical_to_target_y(static_cast<float>(logicalScissor.y));
  const float right = logical_to_target_x(static_cast<float>(logicalScissor.x + logicalScissor.width));
  const float bottom = logical_to_target_y(static_cast<float>(logicalScissor.y + logicalScissor.height));

  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));

  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

} // namespace

MappedRenderState map_logical_render_state() noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {
        .viewport = g_gxState.logicalViewport,
        .scissor = g_gxState.logicalScissor,
    };
  }

  const auto bestScissor = best_scissor_rect(g_gxState.logicalScissor, g_gxState.logicalViewport);
  const gfx::Viewport shiftedViewport{
      .left = g_gxState.logicalViewport.left - static_cast<float>(bestScissor.xOff),
      .top = g_gxState.logicalViewport.top - static_cast<float>(bestScissor.yOff),
      .width = g_gxState.logicalViewport.width,
      .height = g_gxState.logicalViewport.height,
      .znear = g_gxState.logicalViewport.znear,
      .zfar = g_gxState.logicalViewport.zfar,
  };

  return {
      .viewport = map_logical_viewport(shiftedViewport),
      .scissor = scale_logical_scissor_rect(bestScissor.rect),
  };
}

namespace {
void apply_logical_render_state() noexcept {
  const auto mapped = map_logical_render_state();
  set_render_viewport(mapped.viewport);
  set_render_scissor(mapped.scissor);
}
} // namespace

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }

  return scale_logical_scissor_rect(logicalScissor);
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  const bool changed = viewport != g_gxState.logicalViewport;
  g_gxState.logicalViewport = viewport;
  g_gxState.stateDirty = g_gxState.stateDirty || changed;
  apply_logical_render_state();
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport == g_gxState.renderViewport) {
    return;
  }
  g_gxState.renderViewport = viewport;
  g_gxState.stateDirty = true;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  apply_logical_render_state();
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

void evict_texture_object(u32 texObjId) noexcept {
  if (const auto it = s_textureObjectCaches.find(texObjId); it != s_textureObjectCaches.end()) {
    clear_texture_dependency(texObjId, it->second.tlutObjId);
    s_textureObjectCaches.erase(it);
  }
  if (const auto it = s_texObjSourceKeys.find(texObjId); it != s_texObjSourceKeys.end()) {
    s_staticTextureSourceCache.erase(it->second);
    s_texObjSourceKeys.erase(it);
    s_texObjSourceKeyMemo.forget(texObjId);
    clear_static_source_front_cache();
  }
  if (const auto it = s_texObjPaletteSourceKeys.find(texObjId); it != s_texObjPaletteSourceKeys.end()) {
    s_staticPaletteTextureSourceCache.erase(it->second);
    s_texObjPaletteSourceKeys.erase(it);
    s_texObjPaletteSourceKeyMemo.forget(texObjId);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}

void evict_tlut_object(u32 tlutObjId) noexcept {
  // Only the conversions that actually depend on this palette are dropped.
  if (const auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    for (const u32 texObjId : it->second.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
      if (const auto keyIt = s_texObjPaletteSourceKeys.find(texObjId); keyIt != s_texObjPaletteSourceKeys.end()) {
        s_staticPaletteTextureSourceCache.erase(keyIt->second);
        s_texObjPaletteSourceKeys.erase(keyIt);
        s_texObjPaletteSourceKeyMemo.forget(texObjId);
      }
    }
    s_tlutObjectCaches.erase(it);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}

// GXInitTexObj mints a fresh object id per call and nothing ever emits DESTROY_TEXOBJ, so the texture object memo used to be bounded only by the fact that GXInvalidateTexAll dropped it six times a frame.
constexpr u64 kTextureCacheIdleRevisions = 128;
constexpr size_t kTextureObjectCacheSoftLimit = 1024;
constexpr size_t kTlutObjectCacheSoftLimit = 512;
constexpr size_t kTexObjSourceKeySoftLimit = 2048;

static void prune_idle_texture_caches() noexcept {
  const u64 revision = s_staticTextureCacheValidationRevision;
  // An idle entry stays idle, so there is nothing to gain from sweeping on every bump (six a frame).
  if (revision % (kTextureCacheIdleRevisions / 2) != 0) {
    return;
  }
  if (s_textureObjectCaches.size() >= kTextureObjectCacheSoftLimit) {
    std::vector<std::pair<u32, u32>> retired;
    for (const auto& [texObjId, entry] : s_textureObjectCaches) {
      if (revision - entry.validationRevision > kTextureCacheIdleRevisions) {
        retired.emplace_back(texObjId, entry.tlutObjId);
      }
    }
    for (const auto& [texObjId, tlutObjId] : retired) {
      clear_texture_dependency(texObjId, tlutObjId);
      s_textureObjectCaches.erase(texObjId);
    }
  }

  if (s_tlutObjectCaches.size() >= kTlutObjectCacheSoftLimit) {
    // Only drop palettes nothing depends on: an entry with users still records which cached conversions have to be thrown away when its bytes change.
    absl::erase_if(s_tlutObjectCaches, [revision](const auto& item) {
      const auto& cache = item.second;
      return cache.staticTextureUsers.empty() && cache.dynamicPaletteTextures.empty() &&
             (!cache.tlutTexture.handle || cache.tlutTexture.handle.use_count() <= 1) &&
             revision - cache.tlutTexture.validationRevision > kTextureCacheIdleRevisions;
    });
  }

  // The source-key maps exist only so evict_texture_object can find the static source entry a texObjId last resolved to, and nothing else ever retires them -- with GXInitTexObj minting a fresh id per call they otherwise grow for the lifetime of the process.
  if (s_texObjSourceKeys.size() >= kTexObjSourceKeySoftLimit) {
    absl::erase_if(s_texObjSourceKeys, [](const auto& item) { return !s_textureObjectCaches.contains(item.first); });
    s_texObjSourceKeyMemo.reset();
  }
  if (s_texObjPaletteSourceKeys.size() >= kTexObjSourceKeySoftLimit) {
    absl::erase_if(s_texObjPaletteSourceKeys,
                   [](const auto& item) { return !s_textureObjectCaches.contains(item.first); });
    s_texObjPaletteSourceKeyMemo.reset();
  }
}

void invalidate_static_texture_cache() noexcept {
  // Nothing has been marked reusable since the last bump, so every entry in every tier is already stale at the current revision.
  if (s_textureCacheStamps == 0) {
    return;
  }

  ++s_staticTextureCacheValidationRevision;
  s_textureCacheStamps = 0;
  if (s_staticTextureCacheValidationRevision == 0) {
    // Unreachable with a 64-bit counter, but if it ever wraps, no stamped revision may survive into the reused revision.
    s_staticTextureCacheValidationRevision = 1;
    s_textureObjectCaches.clear();
    s_tlutObjectCaches.clear();
    s_staticTextureSourceCache.clear();
    s_staticPaletteTextureSourceCache.clear();
    s_texObjSourceKeys.clear();
    s_texObjPaletteSourceKeys.clear();
    s_texObjSourceKeyMemo.reset();
    s_texObjPaletteSourceKeyMemo.reset();
    clear_static_source_front_cache();
    for (auto& entry : s_textureResolveIdentityCache) {
      entry.valid = false;
      entry.binding.reset();
    }
  }

  // Everything else is revision-gated: the texture object memo, the TLUT textures (re-proved by palette digest), the static source front cache and the resolve identity cache all miss on the new revision without being torn down, so the bump costs one increment instead of dropping GPU textures and rebuilding hash tables.
  s_lastTextureResolveIdentityValid.fill(false);
  s_lastNoCopyResolveRevision.fill(0);
  s_lastStaticSourceResolveKeyValid.fill(false);
  s_lastStaticSourceNoCopyRevision.fill(0);
  prune_idle_texture_caches();
  g_gxState.stateDirty = true;
}

void clear_copy_texture_cache() noexcept {
  g_gxState.copyTextures.clear();
  g_gxState.copyTextureCache.clear();
  prune_copy_texture_pool(nullptr);
  mark_copy_texture_cache_changed();
  clear_display_copy_cache();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.dynamicPaletteTextures.clear();
  }
}

void clear_display_copy_cache() noexcept {
  g_gxState.displayCopyTexture.reset();
  g_gxState.displayCopyBindGroup = {};
  g_gxState.displayCopyWidth = 0;
  g_gxState.displayCopyHeight = 0;
}

void set_display_copy_present_source() noexcept {
  if (!g_gxState.displayCopyTexture) {
    return;
  }

  g_gxState.displayCopyBindGroup = webgpu::create_copy_bind_group(g_gxState.displayCopyTexture->sampleTextureView,
                                                                  webgpu::present_source().sampler);
  webgpu::set_present_source_override(g_gxState.displayCopyBindGroup, g_gxState.displayCopyTexture->texture,
                                      g_gxState.displayCopyTexture->size, g_gxState.displayCopyTexture->format);
}

void evict_copy_texture(const void* dest) noexcept {
  // Dynamic palette textures are keyed on the copy texture's raw TextureRef pointer; evicting the copy without them leaves entries that can match a recycled allocation at the same address and serve a stale conversion.
  absl::flat_hash_set<const void*> sourceIdentities;
  bool changed = false;
  if (const auto it = g_gxState.copyTextures.find(dest); it != g_gxState.copyTextures.end()) {
    if (it->second.handle) {
      sourceIdentities.insert(it->second.handle.get());
    }
    g_gxState.copyTextures.erase(it);
    changed = true;
  }
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      if (it->second.handle) {
        sourceIdentities.insert(it->second.handle.get());
      }
      g_gxState.copyTextureCache.erase(it++);
      changed = true;
    } else {
      ++it;
    }
  }
  if (!sourceIdentities.empty()) {
    for (auto& [_, cache] : s_tlutObjectCaches) {
      for (auto it = cache.dynamicPaletteTextures.begin(); it != cache.dynamicPaletteTextures.end();) {
        if (sourceIdentities.contains(it->first.sourceIdentity)) {
          cache.dynamicPaletteTextures.erase(it++);
        } else {
          ++it;
        }
      }
    }
  }
  prune_copy_texture_pool(dest);
  if (changed) {
    mark_copy_texture_cache_changed();
  }
}

static void mark_copy_texture_cache_changed() noexcept {
  ++s_copyTextureStateRevision;
  if (s_copyTextureStateRevision == 0) {
    s_copyTextureStateRevision = 1;
    s_lastNoCopyResolveRevision.fill(0);
  }
}

void mark_copy_texture_sampled(const void* copyDest, GXState::CopyTextureRef& copyRef) noexcept {
  const auto frame = gfx::current_frame();
  copyRef.lastSampledFrame = frame;
  copyRef.sampledThisFrame = true;

  if (auto it = g_gxState.copyTextures.find(copyDest);
      it != g_gxState.copyTextures.end() && it->second.handle == copyRef.handle) {
    it->second.lastSampledFrame = frame;
    it->second.sampledThisFrame = true;
  }

  const GXState::CopyTextureKey key{
      .dest = copyDest,
      .width = copyRef.width,
      .height = copyRef.height,
      .format = copyRef.format,
  };
  if (auto it = g_gxState.copyTextureCache.find(key); it != g_gxState.copyTextureCache.end() &&
      it->second.handle == copyRef.handle) {
    it->second.lastSampledFrame = frame;
    it->second.sampledThisFrame = true;
  }

}

bool copy_ref_matches_texobj(const GXState::CopyTextureRef& copyRef, const GXTexObj_& obj) noexcept {
  return copyRef && copyRef.width == obj.width() && copyRef.height == obj.height() &&
         copy_texture_format_compatible(copyRef.format, obj.format());
}

void notify_copy_texture_created() noexcept {
  mark_copy_texture_cache_changed();
}


GXState::CopyTextureRef* find_cached_copy_texture(const void* copyDest, const GXTexObj_& obj) noexcept {
  const GXState::CopyTextureKey key{
      .dest = copyDest,
      .width = obj.width(),
      .height = obj.height(),
      .format = static_cast<GXTexFmt>(obj.format()),
  };
  auto cached = g_gxState.copyTextureCache.find(key);
  if (cached == g_gxState.copyTextureCache.end() || !cached->second) {
    return nullptr;
  }
  mark_copy_texture_sampled(copyDest, cached->second);
  return &cached->second;
}

GXState::CopyTextureRef* find_copy_texture_for_texobj(const GXTexObj_& obj) noexcept {
  if (obj.data == nullptr) {
    return nullptr;
  }

  if (auto* cached = find_cached_copy_texture(obj.data, obj)) {
    return cached;
  }

  const auto exact = g_gxState.copyTextures.find(obj.data);
  if (exact != g_gxState.copyTextures.end() && copy_ref_matches_texobj(exact->second, obj)) {
    mark_copy_texture_sampled(obj.data, exact->second);
    return &exact->second;
  }
  // A guest allocation can be recycled while an old GPU-only EFB copy still exists.
  return nullptr;
}

void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  ZoneScoped;

  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }

    GXTexObj_ obj = g_gxState.loadedTextures[i];
    auto& textureBind = g_gxState.textures[i];
    const TextureResolveIdentity identity = make_texture_resolve_identity(obj);
    // obj is unchanged between here and the memo checks below, so the two former can_cache_static_texture_upload(obj) calls are one value.
    const bool canCacheUpload = can_cache_static_texture_upload(obj);
    const bool sameResolvedTexture = canCacheUpload && s_lastTextureResolveIdentityValid[i] &&
                                     s_lastTextureResolveIdentity[i] == identity;
    // The primary per-texmap memo below consumes neither the source key nor its hash, so building them ahead of it wasted make_static_texture_key on the hottest path (same texture bound across consecutive draws).
    if (sameResolvedTexture && s_lastNoCopyResolveRevision[i] == s_copyTextureStateRevision) {
      continue;
    }
    const bool canUseStaticSourceKey =
        !is_palette_format(obj.format()) && canCacheUpload && !obj.no_cache() &&
        obj.data != nullptr;
    const StaticTextureKey staticSourceKey =
        canUseStaticSourceKey ? make_static_texture_key(obj) : StaticTextureKey{};
    const size_t identityHash = canUseStaticSourceKey ? absl::Hash<TextureResolveIdentity>{}(identity) : 0;
    if (canUseStaticSourceKey) {
      const auto& entry = s_textureResolveIdentityCache[identityHash & (s_textureResolveIdentityCache.size() - 1)];
      if (entry.valid && entry.hash == identityHash && entry.copyTextureRevision == s_copyTextureStateRevision &&
          entry.validationRevision == s_staticTextureCacheValidationRevision && entry.identity == identity &&
          entry.binding) {
        textureBind = entry.binding;
        note_texture_cache_stamp();
        s_lastTextureResolveIdentity[i] = identity;
        s_lastTextureResolveIdentityValid[i] = true;
        s_lastNoCopyResolveRevision[i] = s_copyTextureStateRevision;
        s_lastStaticSourceResolveKey[i] = staticSourceKey;
        s_lastStaticSourceResolveKeyValid[i] = true;
        s_lastStaticSourceNoCopyRevision[i] = s_copyTextureStateRevision;
        continue;
      }
    }
    if (canUseStaticSourceKey && textureBind.ref && s_lastStaticSourceResolveKeyValid[i] &&
        s_lastStaticSourceResolveKey[i] == staticSourceKey &&
        s_lastStaticSourceNoCopyRevision[i] == s_copyTextureStateRevision) {
      obj.mFormat = resolved_format_for_handle(textureBind.ref);
      textureBind.texObj = obj;
      s_lastTextureResolveIdentity[i] = identity;
      s_lastTextureResolveIdentityValid[i] = true;
      s_lastNoCopyResolveRevision[i] = s_copyTextureStateRevision;
      note_texture_cache_stamp();
      continue;
    }

    GXState::CopyTextureRef* copyRef = find_copy_texture_for_texobj(obj);
    if (sameResolvedTexture) {
      if (copyRef == nullptr) {
        s_lastNoCopyResolveRevision[i] = s_copyTextureStateRevision;
        continue;
      }
      if (!is_palette_format(obj.format()) && textureBind.ref == copyRef->handle) {
        s_lastNoCopyResolveRevision[i] = 0;
        continue;
      }
    }

    gfx::TextureHandle handle;
    if (is_palette_format(obj.format())) {
      const auto tlutIdx = static_cast<size_t>(obj.tlut);
      if (tlutIdx < g_gxState.loadedTluts.size()) {
        const auto& tlut = g_gxState.loadedTluts[tlutIdx];
        if (tlut.data != nullptr) {
          if (copyRef != nullptr) {
            handle = resolve_dynamic_palette_texture(obj, *copyRef, tlut);
          } else if (obj.data != nullptr) {
            handle = resolve_static_palette_texture(obj, tlut);
          }
        }
      }
    } else if (copyRef != nullptr) {
      handle = copyRef->handle;
    } else if (obj.data != nullptr) {
      handle = resolve_static_texture(obj);
    }

    obj.mFormat = resolved_format_for_handle(handle);
    textureBind = gfx::TextureBind{obj, std::move(handle)};
    // The per-texmap memo below lets a later draw reuse this binding without touching guest memory again, including for no_cache textures that never reach any of the stamped caches above, so it has to count as a stamp.
    note_texture_cache_stamp();
    s_lastTextureResolveIdentity[i] = identity;
    s_lastTextureResolveIdentityValid[i] = true;
    s_lastNoCopyResolveRevision[i] = (copyRef == nullptr) ? s_copyTextureStateRevision : 0;
    s_lastStaticSourceResolveKeyValid[i] = canUseStaticSourceKey && copyRef == nullptr;
    if (s_lastStaticSourceResolveKeyValid[i]) {
      s_lastStaticSourceResolveKey[i] = staticSourceKey;
      s_lastStaticSourceNoCopyRevision[i] = s_copyTextureStateRevision;
      auto& entry = s_textureResolveIdentityCache[identityHash & (s_textureResolveIdentityCache.size() - 1)];
      entry.valid = true;
      entry.hash = identityHash;
      entry.copyTextureRevision = s_copyTextureStateRevision;
      entry.validationRevision = s_staticTextureCacheValidationRevision;
      entry.identity = identity;
      entry.binding = textureBind;
      note_texture_cache_stamp();
    } else {
      s_lastStaticSourceNoCopyRevision[i] = 0;
    }
  }
}

static inline wgpu::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst, bool alphaComponent) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return wgpu::BlendFactor::Zero;
  case GX_BL_ONE:
    return wgpu::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (alphaComponent) {
      return isDst ? wgpu::BlendFactor::SrcAlpha : wgpu::BlendFactor::DstAlpha;
    }
    if (isDst) {
      return wgpu::BlendFactor::Src;
    } else {
      return wgpu::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (alphaComponent) {
      return isDst ? wgpu::BlendFactor::OneMinusSrcAlpha : wgpu::BlendFactor::OneMinusDstAlpha;
    }
    if (isDst) {
      return wgpu::BlendFactor::OneMinusSrc;
    } else {
      return wgpu::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return wgpu::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return wgpu::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

static inline GXBlendFactor remove_dst_alpha_usage(GXBlendFactor fac) {
  switch (fac) {
  case GX_BL_DSTALPHA:
    return GX_BL_ONE;
  case GX_BL_INVDSTALPHA:
    return GX_BL_ZERO;
  default:
    return fac;
  }
}

// GX_LEQUAL etc. describe "pass if this pixel is closer than/equal to what's stored" in GX's own
// distance terms, independent of how that distance is encoded as a host depth value. Under
// UseReversedZ the encoding is flipped (near=1, far=0), so "closer" now corresponds to a *larger*
// stored value, not a smaller one - the ordered compare functions (LESS/LEQUAL/GREATER/GEQUAL)
// must invert to match, or the depth test silently runs backwards (verified directly: this was
// the actual cause of a bug report after the projection/shader half of the reverse-Z fix
// eliminated the double-negation that used to accidentally keep the unreversed comparisons
// correct - LEQUAL now needs GreaterEqual, not LessEqual, once the encoding it's testing against
// is genuinely reversed). Matches upstream aurora's to_compare_function exactly.
static inline wgpu::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return wgpu::CompareFunction::Always;
  }
}

static inline wgpu::BlendState to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac,
                                              GXLogicOp op, GXPixelFmt pixelFmt, u32 dstAlpha) {
  wgpu::BlendComponent colorBlendComponent;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
    break;
  case GX_BM_BLEND:
    if (!render_target_has_alpha(pixelFmt)) {
      srcFac = remove_dst_alpha_usage(srcFac);
      dstFac = remove_dst_alpha_usage(dstFac);
    }
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false, false),
        .dstFactor = to_blend_factor(dstFac, true, false),
    };
    break;
  case GX_BM_SUBTRACT:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::ReverseSubtract,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::One,
    };
    break;
  case GX_BM_LOGIC:
    switch (op) {
    default:
      // WebGPU exposes blend factors, but not fixed-function integer logic ops.
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_CLEAR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_AND:
    case GX_LO_REVAND:
    case GX_LO_XOR:
    case GX_LO_NOR:
    case GX_LO_EQUIV:
    case GX_LO_INV:
    case GX_LO_REVOR:
    case GX_LO_INVCOPY:
    case GX_LO_INVOR:
    case GX_LO_NAND:
    case GX_LO_COPY:
    case GX_LO_SET:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_INVAND:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::OneMinusSrc,
      };
      break;
    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    case GX_LO_OR:
      // WebGPU has no fixed-function integer logic op.
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Max,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }
    break;
  }
  wgpu::BlendComponent alphaBlendComponent;
  if (dstAlpha != UINT32_MAX) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::Constant,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
  } else if (mode == GX_BM_BLEND) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false, true),
        .dstFactor = to_blend_factor(dstFac, true, true),
    };
  } else {
    alphaBlendComponent = colorBlendComponent;
  }
  return {
      .color = colorBlendComponent,
      .alpha = alphaBlendComponent,
  };
}

static inline bool effective_alpha_update(GXPixelFmt pixelFmt, bool alphaUpdate) {
  return alphaUpdate && render_target_has_alpha(pixelFmt);
}

static inline u32 effective_dst_alpha(GXPixelFmt pixelFmt, bool alphaUpdate, u32 dstAlpha) {
  return effective_alpha_update(pixelFmt, alphaUpdate) ? dstAlpha : UINT32_MAX;
}

static inline wgpu::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  wgpu::ColorWriteMask writeMask = wgpu::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

static inline wgpu::PrimitiveState to_primitive_state(GXCullMode gx_cullMode) {
  auto cullMode = wgpu::CullMode::None;
  switch (gx_cullMode) {
    DEFAULT_FATAL("unsupported cull mode {}", underlying(gx_cullMode));
  case GX_CULL_FRONT:
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_BACK:
    cullMode = wgpu::CullMode::Back;
    break;
  case GX_CULL_ALL:
    // todo: remove? since all calls get dropped anyways
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_NONE:
    break;
  }
  return {
      .topology = wgpu::PrimitiveTopology::TriangleList,
      .stripIndexFormat = wgpu::IndexFormat::Undefined,
      .frontFace = wgpu::FrontFace::CW,
      .cullMode = cullMode,
  };
}

wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept {
  ZoneScoped;
  const wgpu::DepthStencilState depthStencil{
      .format = g_graphicsConfig.depthFormat,
      .depthWriteEnabled = config.depthUpdate,
      .depthCompare = config.depthCompare ? to_compare_function(config.depthFunc) : wgpu::CompareFunction::Always,
  };
  const auto blendState = to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp,
                                         config.pixelFmt, config.dstAlpha);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .blend = &blendState,
      .writeMask = to_write_mask(config.colorUpdate, config.alphaUpdate),
  }};
  const wgpu::FragmentState fragmentState{
      .module = shader,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = sPipelineLayout,
      .vertex =
          {
              .module = shader,
              .entryPoint = "vs_main",
              .bufferCount = static_cast<uint32_t>(vtxBuffers.size()),
              .buffers = vtxBuffers.data(),
          },
      .primitive = to_primitive_state(config.cullMode),
      .depthStencil = &depthStencil,
      .multisample =
          wgpu::MultisampleState{
              .count = config.msaaSamples,
          },
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

u8 comp_type_size(GXAttr attr, GXCompType type) noexcept {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return 1;
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    switch (type) {
    case GX_RGB565:
    case GX_RGBA4:
      return 2;
    case GX_RGB8:
    case GX_RGBA6:
      return 3;
    case GX_RGBX8:
    case GX_RGBA8:
      return 4;
    }
  default:
    switch (type) {
    case GX_U8:
    case GX_S8:
      return 1;
    case GX_U16:
    case GX_S16:
      return 2;
    case GX_F32:
      return 4;
    default:
      Log.fatal("comp_type_size: Unsupported component type {}", type);
    }
  }
}

u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return 1;
  case GX_VA_POS:
    switch (cnt) {
    case GX_POS_XY:
      return 2;
    case GX_POS_XYZ:
      return 3;
    default:
      break;
    }
    break;
  case GX_VA_NRM:
    switch (cnt) {
    case GX_NRM_XYZ:
      return 3;
    case GX_NRM_NBT:
    case GX_NRM_NBT3:
      return 9;
    default:
      break;
    }
    break;
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return 1;
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7:
    switch (cnt) {
    case GX_TEX_S:
      return 1;
    case GX_TEX_ST:
      return 2;
    default:
      break;
    }
    break;
  default:
    break;
  }
  Log.fatal("comp_cnt_count: Unsupported attr/cnt {} {}", attr, cnt);
}

static u8 index_attr_size(GXAttr attr, GXCompCnt cnt, GXAttrType type) noexcept {
  const u8 indexSize = type == GX_INDEX16 ? 2 : 1;
  if (attr == GX_VA_NRM && cnt == GX_NRM_NBT3) {
    return indexSize * 3;
  }
  return indexSize;
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ZoneScoped;

  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  // GX applies integer logic ops in the PE after fog.
  config.shaderConfig.fogType = effective_pipeline_fog_type(
      g_gxState.fog.type, g_gxState.zTextureOp, g_gxState.zCompLocBeforeTex,
      g_gxState.blendMode, g_gxState.blendOp);
  config.shaderConfig.fogRangeAdjust =
      config.shaderConfig.fogType != GX_FOG_NONE && (g_gxState.fogRange[0] & (1u << 10)) != 0;
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nrmIndexCount = static_cast<u8>(attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1),
    };
    switch (type) {
    case GX_DIRECT: {
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    }
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += index_attr_size(attr, attrFmt.cnt, type);
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += index_attr_size(attr, attrFmt.cnt, type);
      break;
    default:
      Log.fatal("populate_pipeline_config: Invalid vertex type {}", type);
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.dualTexEnabled = (g_gxState.dualTex & 1u) != 0;
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  config.shaderConfig.numTexGens = g_gxState.numTexGens;
  if (g_gxState.zTextureOp != GX_ZT_DISABLE && !g_gxState.zCompLocBeforeTex) {
    config.shaderConfig.zTexture = (g_gxState.zTextureBias & 0x00FFFFFFu) |
                                    ((static_cast<u32>(g_gxState.zTextureFmt) & 0x3u) << 24) |
                                    ((static_cast<u32>(g_gxState.zTextureOp) & 0x3u) << 26);
  }
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < MaxTexCoord; ++i) {
    config.shaderConfig.tcgs[i].src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + i);
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  config.shaderConfig.alphaCompare = {};
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  // The remaining fields are assigned in place.
  config.version = GXPipelineConfigVersion;
  config.msaaSamples = gfx::get_sample_count();
  config.depthFunc = g_gxState.depthFunc;
  config.cullMode = config.shaderConfig.lineMode == 0 ? g_gxState.cullMode : GX_CULL_NONE;
  config.blendMode = g_gxState.blendMode;
  config.blendFacSrc = g_gxState.blendFacSrc;
  config.blendFacDst = g_gxState.blendFacDst;
  config.blendOp = g_gxState.blendOp;
  config.pixelFmt = g_gxState.pixelFmt;
  config.dstAlpha = effective_dst_alpha(g_gxState.pixelFmt, g_gxState.alphaUpdate, g_gxState.dstAlpha);
  config.depthCompare = g_gxState.depthCompare;
  config.depthUpdate = g_gxState.depthUpdate;
  config.alphaUpdate = effective_alpha_update(g_gxState.pixelFmt, g_gxState.alphaUpdate);
  config.colorUpdate = g_gxState.colorUpdate;
}

static TextureBindGroupCacheKey make_texture_bind_group_cache_key(const ShaderInfo& info) noexcept {
  TextureBindGroupCacheKey key{
      .sampledTextures = info.sampledTextures.to_ullong(),
      .sampledIndTextures = info.sampledIndTextures.to_ullong(),
  };
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures[i] && !info.sampledIndTextures[i]) {
      continue;
    }
    const auto& tex = g_gxState.textures[i];
    if (!tex) {
      continue;
    }
    const u32 refState = (tex.ref->isReplacement ? 1u : 0u) | (tex.ref->hasArbitraryMips ? 2u : 0u) |
                         (tex.ref->mipCount << 8);
    set_texture_bind_group_cache_slot(key, i, tex.ref->sampleTextureView.Get(), tex.texObj.mode0,
                                      tex.texObj.mode1, refState);
  }
  return key;
}

static HashType hash_texture_bind_group_cache_key(const TextureBindGroupCacheKey& key) noexcept {
  // The key is a 176-byte POD with no padding, so one hash over the whole thing replaces the six chained XXH3 calls this used to make per draw.
  static_assert(std::has_unique_object_representations_v<TextureBindGroupCacheKey>);
  return xxh3_hash(key);
}

static GXBindGroups build_bind_groups_uncached(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    // Don't bother re-binding anything
    return {};
  }

  // Using C WGPU types instead of C++ wrappers to avoid destructor overhead
  std::array<WGPUBindGroupEntry, MaxTextures * 2> textureEntries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    WGPUBindGroupEntry& textureEntry = textureEntries[i * 2];
    WGPUBindGroupEntry& samplerEntry = textureEntries[i * 2 + 1];
    textureEntry.binding = i * 2;
    samplerEntry.binding = i * 2 + 1;
    if (tex && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      textureEntry.textureView = tex.ref->sampleTextureView.Get();
      samplerEntry.sampler = gfx::sampler_ref(tex.get_descriptor()).Get();
    } else {
      textureEntry.textureView = sEmptyTextureView.Get();
      samplerEntry.sampler = sEmptySampler.Get();
    }
  }
  const WGPUBindGroupDescriptor textureBindGroupDescriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = sTextureBindGroupLayout.Get(),
      .entryCount = textureEntries.size(),
      .entries = textureEntries.data(),
  };
  const auto ref = gfx::bind_group_ref(textureBindGroupDescriptor);
  return {
      .textureBindGroup = ref,
      .resolvedTextureBindGroup = gfx::find_bind_group(ref).Get(),
  };
}

GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    return {};
  }

  constexpr size_t CacheSize = 1024;
  struct Entry {
    bool valid = false;
    u32 frame = 0;
    HashType hash = 0;
    TextureBindGroupCacheKey key{};
    GXBindGroups bindGroups{};
  };
  static std::array<Entry, CacheSize> cache{};

  const auto key = make_texture_bind_group_cache_key(info);
  const HashType hash = hash_texture_bind_group_cache_key(key);
  const u32 frame = gfx::current_frame();
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.frame == frame && entry.hash == hash && entry.key == key) {
    return entry.bindGroups;
  }

  const auto bindGroups = build_bind_groups_uncached(info);
  entry.valid = true;
  entry.frame = frame;
  entry.hash = hash;
  entry.key = key;
  entry.bindGroups = bindGroups;
  return bindGroups;
}

void initialize() noexcept {
  {
    std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      textureEntries[i * 2] = {
          .binding = i * 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      textureEntries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      };
    }
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Texture Bind Group Layout",
        .entryCount = textureEntries.size(),
        .entries = textureEntries.data(),
    };
    sTextureBindGroupLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  {
    constexpr wgpu::SamplerDescriptor descriptor{.label = "Empty sampler"};
    sEmptySampler = gfx::sampler_ref(descriptor);
  }
  {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty texture",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    sEmptyTexture = g_device.CreateTexture(&descriptor);
    sEmptyTextureView = sEmptyTexture.CreateView();
  }
  {
    std::array<wgpu::BindGroupEntry, MaxTextures * 2> entries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      entries[i * 2] = {
          .binding = i * 2,
          .textureView = sEmptyTextureView,
      };
      entries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .sampler = sEmptySampler,
      };
    }
    const wgpu::BindGroupDescriptor desc{
        .label = "GX Empty Texture Bind Group",
        .layout = sTextureBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_emptyTextureBindGroup = g_device.CreateBindGroup(&desc);
  }
  {
    const std::array layouts{
        gfx::g_staticBindGroupLayout,
        gfx::g_uniformBindGroupLayout,
        sTextureBindGroupLayout,
    };
    const wgpu::PipelineLayoutDescriptor desc{
        .label = "GX Pipeline Layout",
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    sPipelineLayout = g_device.CreatePipelineLayout(&desc);
  }
}

void shutdown() noexcept {
  // TODO we should probably store this all in g_state.gx instead
  sSamplerBindGroupLayout = {};
  sTextureBindGroupLayout = {};
  {
    std::lock_guard lock{sBindGroupLayoutMutex};
    sUniformBindGroupLayouts.clear();
    sTextureBindGroupLayouts.clear();
  }
  for (auto& item : g_gxState.textures) {
    item.ref.reset();
  }
  s_textureObjectCaches.clear();
  s_tlutObjectCaches.clear();
  s_staticTextureSourceCache.clear();
  s_staticPaletteTextureSourceCache.clear();
  s_texObjSourceKeys.clear();
  s_texObjPaletteSourceKeys.clear();
  s_texObjSourceKeyMemo.reset();
  s_texObjPaletteSourceKeyMemo.reset();
  clear_static_source_front_cache();
  // The resolve identity cache holds texture references too; it is revision gated rather than cleared during normal operation, so it has to be dropped explicitly here.
  for (auto& entry : s_textureResolveIdentityCache) {
    entry.valid = false;
    entry.binding.reset();
  }
  s_lastTextureResolveIdentityValid.fill(false);
  s_lastStaticSourceResolveKeyValid.fill(false);
  g_gxState.loadedTextures.fill({});
  g_gxState.loadedTluts.fill({});
  clear_copy_texture_cache();
}
} // namespace aurora::gx

static wgpu::AddressMode wgpu_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return wgpu::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  }
}

static std::pair<wgpu::FilterMode, wgpu::MipmapFilterMode> wgpu_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Undefined};
  case GX_LINEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Undefined};
  case GX_NEAR_MIP_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  }
}

static u16 wgpu_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
  case GX_MAX_ANISOTROPY:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}

static bool supports_wgpu_aniso(wgpu::FilterMode magFilter, wgpu::FilterMode minFilter,
                                wgpu::MipmapFilterMode mipFilter) {
  return magFilter == wgpu::FilterMode::Linear && minFilter == wgpu::FilterMode::Linear &&
         mipFilter == wgpu::MipmapFilterMode::Linear;
}

wgpu::SamplerDescriptor aurora::gfx::TextureBind::get_descriptor() const noexcept {
  auto [minFilter, mipFilter] = wgpu_filter_mode(texObj.min_filter());
  const auto [magFilter, _] = wgpu_filter_mode(texObj.mag_filter());
  float minLod = texObj.min_lod();
  float maxLod = texObj.max_lod();
  if (ref && ref->isReplacement) {
    minFilter = wgpu::FilterMode::Linear;
    mipFilter = wgpu::MipmapFilterMode::Linear;
    minLod = 0.f;
    maxLod = static_cast<float>(std::max(ref->mipCount, 1u) - 1u);
  } else if (ref && ref->hasArbitraryMips && mipFilter != wgpu::MipmapFilterMode::Undefined) {
    mipFilter = wgpu::MipmapFilterMode::Linear;
  } else if (mipFilter == wgpu::MipmapFilterMode::Undefined) {
    minLod = 0.f;
    maxLod = 0.f;
  }
  u16 maxAnisotropy = wgpu_aniso(texObj.max_aniso());
  if (maxAnisotropy > 1 && !supports_wgpu_aniso(magFilter, minFilter, mipFilter)) {
    maxAnisotropy = 1;
  }
  return {
      .label = "Generated Filtering Sampler",
      .addressModeU = wgpu_address_mode(texObj.wrap_s()),
      .addressModeV = wgpu_address_mode(texObj.wrap_t()),
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .lodMinClamp = minLod,
      .lodMaxClamp = maxLod,
      .maxAnisotropy = maxAnisotropy,
  };
} // namespace aurora::gx
