#include "common.hpp"
#include "../gx/shader_info.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "efb_ram_copy.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gx/pipeline.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "../window.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

#ifdef AURORA_GFX_DEBUG_GROUPS
std::vector<std::string> g_debugGroupStack;
std::vector<std::string> g_debugMarkers;
#endif

constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize + StorageBufferSize +
                                       (UseTextureBuffer ? TextureUploadSize : 0);

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::g_guestWriteGenerationHook = generation;
  aurora::g_guestWriteNotifyHook = notify;
}

namespace aurora {
// For types that we can't ensure are safe to hash with has_unique_object_representations,
// we create specialized methods to handle them. Note that these are highly dependent on
// the structure definition, which could easily change with Dawn updates.
template <>
inline HashType xxh3_hash(const WGPUBindGroupDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(WGPUBindGroupDescriptor, layout); // skip nextInChain, label
  const auto hash = xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                                sizeof(WGPUBindGroupDescriptor) - offset - sizeof(void*) /* skip entries */, seed);
  const size_t entryBytes = sizeof(WGPUBindGroupEntry) * input.entryCount;
  // The entries are hashed unseeded so they stay off XXH3's seeded long path;
  // the descriptor head still carries the caller's seed.
  return hash_combine(hash, xxh3_hash_s(input.entries, entryBytes));
}
template <>
inline HashType xxh3_hash(const wgpu::SamplerDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(wgpu::SamplerDescriptor, addressModeU); // skip nextInChain, label
  return xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                     sizeof(wgpu::SamplerDescriptor) - offset - 2 /* skip padding */, seed);
}
} // namespace aurora

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  wgpu::BindGroup bindGroup;
  uint32_t lastUsedFrame = 0;
};

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
// Bind groups dropped from the cache by clear_caches() while a sealed frame's recorded draws
// still reference them. Ownership moves here and is released at the next seal.
static std::vector<CachedBindGroup> g_retiredBindGroups;
static absl::flat_hash_map<SamplerRef, wgpu::Sampler> g_cachedSamplers;

static ByteBuffer g_verts;
static ByteBuffer g_uniforms;
static ByteBuffer g_indices;
static ByteBuffer g_storage;
static ByteBuffer g_textureUpload;
wgpu::Buffer g_vertexBuffer;
wgpu::Buffer g_uniformBuffer;
wgpu::Buffer g_indexBuffer;
wgpu::Buffer g_storageBuffer;
constexpr size_t FrameSlotCount = 3;
static std::array<wgpu::Buffer, FrameSlotCount> g_stagingBuffers;
static size_t currentStagingBuffer = 0;
enum class BufferMapState {
  Unmapped,
  Mapping,
  Mapped,
};
static std::atomic s_mappingState{BufferMapState::Unmapped};
static wgpu::Limits g_cachedLimits;
// Advanced once per logical frame in the seal prologue, under the renderer GPU mutex and with the
// producer blocked, so every later reader sees a value that no longer moves.
static uint32_t g_frameIndex = UINT32_MAX;
wgpu::BindGroupLayout g_staticBindGroupLayout;
wgpu::BindGroup g_staticBindGroup;
wgpu::BindGroupLayout g_uniformBindGroupLayout;
wgpu::BindGroup g_uniformBindGroup;

// for imgui debug
AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView; // MSAA resolve target; null if msaaSamples == 1
  wgpu::TextureView depthView;
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  wgpu::Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  TextureHandle resolveSourceSnapshot;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  ClipRect resolveSnapshotRect;
  Vec4<float> resolveSourceRect;
  Range resolveUniformRange;
  std::array<u32, 3> resolveCopyFilterCoefficients{0, 64, 0};
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  float clearDepthValue = 1.f;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  // The resolve destination outlives the frame with no re-issue path (one-shot
  // bake), so this pass may not drop draws even in skip-unready-pipelines mode.
  bool requireReadyPipelines = false;
  bool resolveHalfScale = false;
  bool resolveCopyFilterActive = false;
  bool resolveForceOpaqueAlpha = false;
  bool resolveNeedsConversion = false;
  bool resolveNeedsShaderSampling = false;
  bool resolveLinearSampling = false;
  bool snapshotColorResolveSource = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;
};
static std::vector<RenderPass> g_renderPasses;
static u32 g_currentRenderPass = UINT32_MAX;

// Recycle command storage: discarding passes used to free their command lists too, so each frame
// rebuilt hundreds of KB from zero capacity. The passes themselves are cheap to recreate.
using CommandListPool = std::vector<CommandList>;
static CommandListPool g_commandListPool;
static constexpr size_t MaxPooledCommandLists = 32;
// The pool is the only recording storage both sides touch, a few pointer moves per frame, so a
// plain mutex is cheaper than the alternatives and keeps the vector's invariants.
static std::mutex g_commandListPoolMutex;

static CommandList acquire_command_list() noexcept {
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.empty()) {
    return {};
  }
  CommandList list = std::move(g_commandListPool.back());
  g_commandListPool.pop_back();
  list.clear();
  return list;
}

static void release_command_list(CommandList&& list) noexcept {
  if (list.capacity() == 0) {
    return;
  }
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.size() >= MaxPooledCommandLists) {
    return;
  }
  list.clear();
  g_commandListPool.emplace_back(std::move(list));
}

static void release_render_pass(RenderPass& pass) noexcept { release_command_list(std::move(pass.commands)); }

static void recycle_render_passes(std::vector<RenderPass>& passes) noexcept {
  for (auto& pass : passes) {
    release_render_pass(pass);
  }
  passes.clear();
}

struct SealedFrameData {
  std::vector<RenderPass> passes;
};

SealedFrame::SealedFrame() : m_data(std::make_unique<SealedFrameData>()) {}
SealedFrame::~SealedFrame() = default;
SealedFrame::SealedFrame(SealedFrame&&) noexcept = default;
SealedFrame& SealedFrame::operator=(SealedFrame&&) noexcept = default;

static RenderPass& push_render_pass(RenderPass&& pass) {
  auto& out = g_renderPasses.emplace_back(std::move(pass));
  if (out.commands.capacity() == 0) {
    out.commands = acquire_command_list();
  }
  return out;
}
// The producer may build the next frame's GX stream while the worker prepares the target, and
// this is the only renderer-owned property that path queries, so publish it explicitly.
static std::atomic_bool g_inOffscreen{false};
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;

static void discard_suspended_efb_pass() noexcept {
  if (g_suspendedEfbPass) {
    release_render_pass(*g_suspendedEfbPass);
    g_suspendedEfbPass.reset();
  }
}

static bool has_current_render_pass() noexcept { return g_currentRenderPass < g_renderPasses.size(); }
static webgpu::TextureWithSampler g_offscreenColor;
static webgpu::TextureWithSampler g_offscreenDepth;

struct ResolveSourceSnapshotPool {
  TextureHandle entry;
};

// Snapshot resources live in the same safe frame slots as the mapped staging buffers, and copies
// within a frame are ordered, so one grow-only snapshot serves every resolve in that slot.
static std::array<ResolveSourceSnapshotPool, FrameSlotCount> g_resolveSourceSnapshotPools;
static size_t g_recordingSnapshotSlot = 0;

static TextureHandle new_resolve_source_snapshot(wgpu::Extent3D size, wgpu::TextureFormat format) noexcept {
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "GX Copy Source Snapshot",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "GX Copy Source Snapshot view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto textureView = texture.CreateView(&viewDescriptor);
  wgpu::TextureView sampleTextureView = textureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView), std::move(textureView), size,
                                      format, 1, GX_TF_RGBA8);
}

static TextureHandle acquire_resolve_source_snapshot(uint32_t width, uint32_t height) noexcept {
  auto& pool = g_resolveSourceSnapshotPools[g_recordingSnapshotSlot];
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;

  if (pool.entry && pool.entry->size.width >= width && pool.entry->size.height >= height &&
      pool.entry->format == format) {
    return pool.entry;
  }

  const wgpu::Extent3D size{
      std::max(width, pool.entry ? pool.entry->size.width : 0u),
      std::max(height, pool.entry ? pool.entry->size.height : 0u),
      1,
  };
  pool.entry = new_resolve_source_snapshot(size, format);
  return pool.entry;
}

struct ResolveSamplingPlan {
  bool needsConversion = false;
  bool needsShaderSampling = false;
  bool usesLinearSampling = false;
};

static ResolveSamplingPlan make_resolve_sampling_plan(const TextureHandle& target, GXTexFmt format,
                                                       const ClipRect& resolveRect, const Vec4<float>& sourceRect,
                                                       bool halfScale, bool copyFilterActive,
                                                       bool forceOpaqueAlpha) noexcept {
  const auto differs = [](float lhs, float rhs) { return std::abs(lhs - rhs) > 0.01f; };
  const auto differsFromDst = [&](uint32_t dst, float src) {
    return differs(static_cast<float>(dst), src);
  };
  const bool sourceMatchesIntegerRect =
      !differs(sourceRect.x(), static_cast<float>(resolveRect.x)) &&
      !differs(sourceRect.y(), static_cast<float>(resolveRect.y)) &&
      !differs(sourceRect.z(), static_cast<float>(resolveRect.width)) &&
      !differs(sourceRect.w(), static_cast<float>(resolveRect.height));
  const bool dstMatchesSource = target && !differsFromDst(target->size.width, sourceRect.z()) &&
                                !differsFromDst(target->size.height, sourceRect.w());
  const bool needsShaderSampling =
      halfScale || copyFilterActive || forceOpaqueAlpha || !sourceMatchesIntegerRect || !dstMatchesSource;
  return {
      .needsConversion = tex_copy_conv::needs_conversion(format),
      .needsShaderSampling = needsShaderSampling,
      .usesLinearSampling = halfScale || (needsShaderSampling && !copyFilterActive),
  };
}

static ClipRect calculate_resolve_snapshot_rect(const wgpu::Extent3D& targetSize,
                                                const Vec4<float>& sourceRect,
                                                const ResolveSamplingPlan& samplingPlan,
                                                bool copyFilterActive) noexcept {
  const ClipRect fullTarget{
      .x = 0,
      .y = 0,
      .width = static_cast<int32_t>(targetSize.width),
      .height = static_cast<int32_t>(targetSize.height),
  };
  // Shader-sampled and converted copies can feed effects that read outside the copy rectangle
  // (MKW's DOF chain), so keep those full-target. Exact copies have a closed, crop-safe footprint.
  if (samplingPlan.needsConversion ||
      samplingPlan.needsShaderSampling ||
      targetSize.width == 0 || targetSize.height == 0 ||
      !std::isfinite(sourceRect.x()) || !std::isfinite(sourceRect.y()) ||
      !std::isfinite(sourceRect.z()) || !std::isfinite(sourceRect.w()) ||
      sourceRect.z() <= 0.0f || sourceRect.w() <= 0.0f) {
    return fullTarget;
  }

  // Linear sampling can touch one neighbor on every edge. The vertical GX
  // copy filter explicitly samples the previous and next rows.
  const int32_t haloX = samplingPlan.usesLinearSampling ? 1 : 0;
  const int32_t haloY = (samplingPlan.usesLinearSampling || copyFilterActive) ? 1 : 0;
  const int32_t targetWidth = static_cast<int32_t>(targetSize.width);
  const int32_t targetHeight = static_cast<int32_t>(targetSize.height);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(sourceRect.x())) - haloX, 0, targetWidth - 1);
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(sourceRect.y())) - haloY, 0, targetHeight - 1);
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.x() + sourceRect.z())) + haloX,
                                   left + 1, targetWidth);
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.y() + sourceRect.w())) + haloY,
                                    top + 1, targetHeight);
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

static void set_efb_targets(RenderPass& pass) {
  pass.colorView = webgpu::g_frameBuffer.view;
  pass.resolveView = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : nullptr;
  pass.depthView = webgpu::g_depthBuffer.view;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.targetSize = webgpu::g_frameBuffer.size;
  pass.msaaSamples = webgpu::g_graphicsConfig.msaaSamples;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;
std::vector<TextureUpload> g_textureUploads;

static inline void push_command(CommandType type, const Command::Data& data) {
  if (!has_current_render_pass())
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  g_renderPasses[g_currentRenderPass].commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugGroupStack,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= g_renderPasses.size()) {
    return nullptr;
  }
  auto& last = g_renderPasses[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_frame_buffer_size() noexcept {
  if (webgpu::g_frameBuffer.size.width != 0 && webgpu::g_frameBuffer.size.height != 0) {
    return {webgpu::g_frameBuffer.size.width, webgpu::g_frameBuffer.size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

// Render-thread only: the frame worker recycles g_renderPasses in end_frame, so an off-thread
// read is a use-after-free rather than a stale value. Use get_frame_buffer_size() instead.
Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < g_renderPasses.size()) {
    const auto& size = g_renderPasses[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  return get_frame_buffer_size();
}

static Viewport g_cachedViewport;
void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

static ClipRect g_cachedScissor;
void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  if (data.uniformRange.size == 0) {
    const std::array clearUniform{
        std::clamp(data.depth, 0.f, 1.f),
        0.f,
        0.f,
        0.f,
    };
    data.uniformRange = push_uniform(clearUniform);
  }
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale,
                  const std::array<u32, 3>* copyFilterCoefficients, bool forceOpaqueAlpha,
                  float copyFilterRowStride, bool clampTop, bool clampBottom, bool persistentCopy) {
  // Resolve current render pass
  if (!has_current_render_pass()) {
    Log.warn("Dropping resolve pass without an active render pass");
    return;
  }
  auto& prevPass = g_renderPasses[g_currentRenderPass];
  const auto targetWidth = static_cast<int32_t>(prevPass.targetSize.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.targetSize.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    Log.warn("Dropping resolve pass with invalid target size {}x{}", targetWidth, targetHeight);
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  if (targetWidth > 0 && targetHeight > 0) {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
  }
  prevPass.resolveTarget = std::move(texture);
  prevPass.requireReadyPipelines = persistentCopy;
  prevPass.resolveRect = rect;
  prevPass.resolveSourceRect = sourceRect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveHalfScale = halfScale;
  prevPass.resolveForceOpaqueAlpha = forceOpaqueAlpha;
  prevPass.resolveCopyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  prevPass.resolveCopyFilterActive = prevPass.resolveCopyFilterCoefficients[0] != 0 ||
                                     prevPass.resolveCopyFilterCoefficients[1] != 64 ||
                                     prevPass.resolveCopyFilterCoefficients[2] != 0;
  const auto samplingPlan = make_resolve_sampling_plan(
      prevPass.resolveTarget, resolveFormat, rect, sourceRect, halfScale,
      prevPass.resolveCopyFilterActive, forceOpaqueAlpha);
  prevPass.resolveNeedsConversion = samplingPlan.needsConversion;
  prevPass.resolveNeedsShaderSampling = samplingPlan.needsShaderSampling;
  prevPass.resolveLinearSampling = samplingPlan.usesLinearSampling;
  prevPass.snapshotColorResolveSource =
      !gx::is_depth_format(resolveFormat) && (clearColor || clearAlpha || clearDepth);
  Vec4<float> uniformSourceRect = sourceRect;
  float srcW = static_cast<float>(prevPass.targetSize.width);
  float srcH = static_cast<float>(prevPass.targetSize.height);
  if (prevPass.snapshotColorResolveSource) {
    prevPass.resolveSnapshotRect = calculate_resolve_snapshot_rect(
        prevPass.targetSize, sourceRect, samplingPlan, prevPass.resolveCopyFilterActive);
    prevPass.resolveSourceSnapshot = acquire_resolve_source_snapshot(
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.width),
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.height));
    uniformSourceRect = {
        sourceRect.x() - static_cast<float>(prevPass.resolveSnapshotRect.x),
        sourceRect.y() - static_cast<float>(prevPass.resolveSnapshotRect.y),
        sourceRect.z(), sourceRect.w()};
    srcW = static_cast<float>(prevPass.resolveSourceSnapshot->size.width);
    srcH = static_cast<float>(prevPass.resolveSourceSnapshot->size.height);
  }
  // GX's copy-clamp bits pin every vertical filter tap to the first or last source texel; without
  // it a filtered copy at internal resolution samples an unrelated EFB row as a visible border.
  const float clampTopPixels = clampTop ? uniformSourceRect.y() : 0.0f;
  const float clampBottomPixels =
      clampBottom ? uniformSourceRect.y() + uniformSourceRect.w() : srcH;
  const float clampTopUv = (clampTopPixels + 0.5f) / srcH;
  const float clampBottomUv = (clampBottomPixels - 0.5f) / srcH;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const std::array resolveUniform{
      uniformSourceRect.x() / srcW,
      uniformSourceRect.y() / srcH,
      uniformSourceRect.z() / srcW,
      uniformSourceRect.w() / srcH,
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[0]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[1]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[2]),
      prevPass.resolveCopyFilterActive ? 1.0f : 0.0f,
      prevPass.resolveForceOpaqueAlpha ? 1.0f : 0.0f,
      std::max(copyFilterRowStride, 1.0f),
      clampTopUv,
      clampBottomUv,
  };
  prevPass.resolveUniformRange = push_uniform(resolveUniform);
  const bool clearFullTarget = rect.x <= 0 && rect.y <= 0 &&
                               rect.width >= static_cast<int32_t>(prevPass.targetSize.width) &&
                               rect.height >= static_cast<int32_t>(prevPass.targetSize.height);
  const bool useAttachmentColorClear = clearFullTarget && clearColor && clearAlpha;
  const bool useAttachmentDepthClear = clearFullTarget && clearDepth;

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthView = prevPass.depthView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = useAttachmentColorClear,
      .clearDepth = useAttachmentDepthClear,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  if ((!useAttachmentColorClear && (clearColor || clearAlpha)) || (!useAttachmentDepthClear && clearDepth)) {
    // GX copy clears cover the copied EFB rectangle, not always the whole target, so use a scissored
    // clear draw unless the load op can clear all of it.
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = clearDepth,
        }),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .depth = clearDepthValue,
        .useScissor = !clearFullTarget,
        .scissor = rect,
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  if (!has_current_render_pass()) {
    Log.warn("Dropping palette conversion without an active render pass");
    return;
  }
  g_renderPasses[g_currentRenderPass].paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  if (!has_current_render_pass()) {
    return webgpu::g_graphicsConfig.msaaSamples;
  }
  return g_renderPasses[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  // Retire rather than free; see g_retiredBindGroups.
  g_retiredBindGroups.reserve(g_retiredBindGroups.size() + g_cachedBindGroups.size());
  for (auto& entry : g_cachedBindGroups) {
    g_retiredBindGroups.emplace_back(std::move(entry.second));
  }
  g_cachedBindGroups.clear();
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto colorTexture = g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto depthTexture = g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = g_renderPasses[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbPass = std::move(currentPass);
      g_renderPasses.pop_back();
      --g_currentRenderPass;
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .colorView = g_offscreenColor.view,
      .depthView = g_offscreenDepth.view,
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.view,
      .copySourceDepthView = g_offscreenDepth.view,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      .clearDepthValue = 1.f,
      .clearColor = true,
      .clearDepth = true,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    // Keeps its own recorded commands.
    g_renderPasses.emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = push_render_pass(RenderPass{});
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(g_renderPasses[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

void initialize() {
  g_frameIndex = 0;
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_cachedLimits);

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage,
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, UniformBufferSize,
               "Shared Uniform Buffer");
  createBuffer(g_vertexBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, VertexBufferSize,
               "Shared Vertex Buffer");
  createBuffer(g_indexBuffer, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, IndexBufferSize,
               "Shared Index Buffer");
  createBuffer(g_storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
               "Shared Storage Buffer");
  for (int i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, StagingBufferSize,
                 label.c_str());
  }
  currentStagingBuffer = 0;
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
  map_staging_buffer();

  {
    constexpr std::array layoutEntries{
        // Vertex data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        // Storage data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_vertexBuffer,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_storageBuffer,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Static bind group",
        .layout = g_staticBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  {
    constexpr std::array layoutEntries{
        // Uniform buffer (dynamic offset)
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_uniformBuffer,
            .size = gx::MaxUniformSize,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Uniform bind group",
        .layout = g_uniformBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  gx::initialize();
  initialize_pipeline_cache();
}

void shutdown() {
  shutdown_pipeline_cache();
  gx::clear_shader_module_cache();
  efb_ram::shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();

  g_textureUploads.clear();
  g_cachedBindGroups.clear();
  g_retiredBindGroups.clear();
  g_cachedSamplers.clear();
  g_vertexBuffer = {};
  g_uniformBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  g_stagingBuffers.fill({});
  for (auto& pool : g_resolveSourceSnapshotPools) {
    pool.entry.reset();
  }
  discard_suspended_efb_pass();
  g_renderPasses.clear();
  g_commandListPool.clear();
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  g_staticBindGroup = {};
  g_staticBindGroupLayout = {};
  g_uniformBindGroup = {};
  g_uniformBindGroupLayout = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  currentStagingBuffer = 0;
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
}

void map_staging_buffer() {
  auto expected = BufferMapState::Unmapped;
  if (!s_mappingState.compare_exchange_strong(expected, BufferMapState::Mapping, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
    return;
  }

  g_stagingBuffers[currentStagingBuffer].MapAsync(
      wgpu::MapMode::Write, 0, StagingBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
          return;
        }
        ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}", magic_enum::enum_name(status),
               message);
        s_mappingState.store(BufferMapState::Mapped, std::memory_order_release);
      });
}

static bool begin_frame_impl(bool clearEfb) {
  ZoneScoped;
  {
    ZoneScopedN("Wait for buffer map");
    map_staging_buffer();
    while (true) {
      const auto mappingState = s_mappingState.load(std::memory_order_acquire);
      if (mappingState == BufferMapState::Mapped) {
        break;
      }
      if (mappingState == BufferMapState::Unmapped) {
        // Frame begin failed because the staging map was aborted; the caller's retry loop drops the frame
        // and any one-shot bakes recorded into it. Rate-limited.
        static uint32_t s_beginFrameMapFailCount = 0;
        if (s_beginFrameMapFailCount < 64 || (s_beginFrameMapFailCount & 255) == 0) {
          Log.warn("begin_frame aborted: staging buffer unmapped (frame={} occurrences={})", g_frameIndex,
                   s_beginFrameMapFailCount + 1);
        }
        ++s_beginFrameMapFailCount;
        return false;
      }
      g_instance.ProcessEvents();
    }
  }
  g_recordingSnapshotSlot = currentStagingBuffer;
  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[currentStagingBuffer];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(g_verts, VertexBufferSize);
  mapBuffer(g_uniforms, UniformBufferSize);
  mapBuffer(g_indices, IndexBufferSize);
  mapBuffer(g_storage, StorageBufferSize);
  if constexpr (UseTextureBuffer) {
    mapBuffer(g_textureUpload, TextureUploadSize);
  }

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  if (clearEfb) {
    gx::begin_frame_interpolation();
  }
  discard_suspended_efb_pass();
  webgpu::clear_present_source_override();

  push_render_pass(RenderPass{});
  set_efb_targets(g_renderPasses[0]);
  g_renderPasses[0].clearColorValue = gx::g_gxState.clearColor;
  g_renderPasses[0].clearDepthValue = gx::clear_depth_value();
  g_renderPasses[0].clearColor = clearEfb;
  g_renderPasses[0].clearDepth = clearEfb;
  g_currentRenderPass = 0;
  // Refresh paired render viewport/scissor from logical state in case the FB size changed.
  const auto mappedRenderState = gx::map_logical_render_state();
  const bool renderStateChanged = gx::g_gxState.renderViewport != mappedRenderState.viewport ||
                                  gx::g_gxState.renderScissor != mappedRenderState.scissor;
  gx::g_gxState.renderViewport = mappedRenderState.viewport;
  gx::g_gxState.renderScissor = mappedRenderState.scissor;
  gx::g_gxState.stateDirty = gx::g_gxState.stateDirty || renderStateChanged;
  g_cachedViewport = mappedRenderState.viewport;
  g_cachedScissor = mappedRenderState.scissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  return true;
}

bool begin_frame() { return begin_frame_impl(true); }

bool resume_frame() { return begin_frame_impl(false); }

void abort_frame() noexcept {
  efb_ram::cancel();
  efb_ram::abort_async();
  g_verts.release();
  g_uniforms.release();
  g_indices.release();
  g_storage.release();
  if constexpr (UseTextureBuffer) {
    g_textureUploads.clear();
    g_textureUpload.release();
  }
  if (s_mappingState.load(std::memory_order_acquire) == BufferMapState::Mapped) {
    // Pending interpolation tasks hold raw pointers into the mapped staging
    // range; they must be dropped before the buffer is unmapped and rotated.
    gx::drop_pending_frame_interpolation_uniforms();
    g_stagingBuffers[currentStagingBuffer].Unmap();
    s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
    currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
    map_staging_buffer();
  }
  recycle_render_passes(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  discard_suspended_efb_pass();
  g_inOffscreen = false;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  webgpu::clear_present_source_override();
  end_pipeline_frame();
}

static void end_batch_impl(const wgpu::CommandEncoder& cmd, bool advanceFrame) {
  ZoneScoped;
  ASSERT(!g_inOffscreen, "end_frame called while offscreen rendering is active");
  if (advanceFrame) {
    gx::finalize_frame_interpolation();
  } else {
    // Mid-frame batch split: the staging buffer is about to be unmapped and rotated, so pending
    // interpolation tasks pointing into it would dangle. Tie the clear to the rotation itself.
    gx::drop_pending_frame_interpolation_uniforms();
  }
  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer
  uint64_t bufferOffset = 0;
  const auto writeBuffer = [&](ByteBuffer& buf, wgpu::Buffer& out, uint64_t size, std::string_view label) {
    const auto writeSize = buf.size(); // Only need to copy this many bytes
    if (writeSize > 0) {
      cmd.CopyBufferToBuffer(g_stagingBuffers[currentStagingBuffer], bufferOffset, out, 0, AURORA_ALIGN(writeSize, 4));
      buf.release();
    }
    bufferOffset += size;
    return writeSize;
  };
  g_stagingBuffers[currentStagingBuffer].Unmap();
  s_mappingState.store(BufferMapState::Unmapped, std::memory_order_release);
  g_stats.drawCallCount = g_drawCallCount;
  g_stats.mergedDrawCallCount = g_mergedDrawCallCount;
  g_stats.lastVertSize = writeBuffer(g_verts, g_vertexBuffer, VertexBufferSize, "Vertex");
  g_stats.lastUniformSize = writeBuffer(g_uniforms, g_uniformBuffer, UniformBufferSize, "Uniform");
  g_stats.lastIndexSize = writeBuffer(g_indices, g_indexBuffer, IndexBufferSize, "Index");
  g_stats.lastStorageSize = writeBuffer(g_storage, g_storageBuffer, StorageBufferSize, "Storage");
  if constexpr (UseTextureBuffer) {
    g_stats.lastTextureUploadSize = g_textureUpload.size();
    {
      // Perform texture copies
      for (const auto& item : g_textureUploads) {
        const wgpu::TexelCopyBufferInfo buf{
            .layout =
                wgpu::TexelCopyBufferLayout{
                    .offset = item.layout.offset + bufferOffset,
                    .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                    .rowsPerImage = item.layout.rowsPerImage,
                },
            .buffer = g_stagingBuffers[currentStagingBuffer],
        };
        cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
      }
      g_textureUploads.clear();
      g_textureUpload.release();
    }
  }
  currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
  map_staging_buffer();
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  if (advanceFrame) {
    ++g_frameIndex;
  }
}

void end_frame(const wgpu::CommandEncoder& cmd) { end_batch_impl(cmd, true); }

void end_batch(const wgpu::CommandEncoder& cmd) { end_batch_impl(cmd, false); }

uint32_t current_frame() noexcept { return g_frameIndex; }

// The only place that erases from g_cachedBindGroups, whose handles the frame being encoded still
// holds, so it runs in the seal prologue with the renderer mutex held and the producer excluded.
void expire_bind_group_cache() noexcept {
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Debug labels for the render passes. Formatting them per pass per slot costs a heap-allocating
// fmt::format for a name nothing reads outside a capture.
static const char* render_pass_label(u32 index) noexcept {
  static constexpr std::array<const char*, 16> kRenderPassLabels{
      "Render pass 0",  "Render pass 1",  "Render pass 2",  "Render pass 3",
      "Render pass 4",  "Render pass 5",  "Render pass 6",  "Render pass 7",
      "Render pass 8",  "Render pass 9",  "Render pass 10", "Render pass 11",
      "Render pass 12", "Render pass 13", "Render pass 14", "Render pass 15",
  };
  return index < kRenderPassLabels.size() ? kRenderPassLabels[index] : "Render pass";
}

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& passes, u32 idx,
                             int32_t interpolatedFrame);

static void render_impl(std::vector<RenderPass>& renderPasses, wgpu::CommandEncoder& cmd, int32_t interpolatedFrame,
                        bool finalize) {
  ZoneScoped;
  // Palette conversions, MSAA resolves and EFB copies depend on sealed frame state, not on the
  // interpolation weight, so encode them on the native render and let replay slots sample them.
  const bool encodeTextureBakes = interpolatedFrame < 0;
  for (u32 i = 0; i < renderPasses.size(); ++i) {
    const auto& passInfo = renderPasses[i];
    if (encodeTextureBakes) {
      for (const auto& conv : passInfo.paletteConvs) {
        tex_palette_conv::run(cmd, conv);
      }
    }
    const bool hasRenderWork = passInfo.clearColor || passInfo.clearDepth || !passInfo.commands.empty();
    if (i == renderPasses.size() - 1) {
      ASSERT(!passInfo.resolveTarget, "Final render pass must not have resolve target");
    } else if (!(passInfo.resolveTarget && encodeTextureBakes) && !hasRenderWork) {
      // Skip only empty intermediate passes: offscreen and scratch passes with resolves still have to
      // run for later samplers, and on a replay slot a resolve-only pass has nothing to encode.
      continue;
    }

    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = passInfo.colorView,
            .resolveTarget = passInfo.resolveView,
            .loadOp = passInfo.clearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue =
                {
                    .r = passInfo.clearColorValue.x(),
                    .g = passInfo.clearColorValue.y(),
                    .b = passInfo.clearColorValue.z(),
                    .a = passInfo.clearColorValue.w(),
                },
        },
    };
    const wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{
        .view = passInfo.depthView,
        .depthLoadOp = passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
        .depthStoreOp = wgpu::StoreOp::Store,
        .depthClearValue = passInfo.clearDepthValue,
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = render_pass_label(i),
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
        .depthStencilAttachment = &depthStencilAttachment,
    };

    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
    render_pass_impl(pass, renderPasses, i, interpolatedFrame);
    pass.End();

    if (finalize && i == renderPasses.size() - 1) {
      depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView, passInfo.targetSize, passInfo.msaaSamples);
    }

    if (passInfo.resolveTarget) {
      const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
      wgpu::Texture resolveSourceTexture = passInfo.copySourceTexture;
      wgpu::TextureView resolveSourceView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView;
      if (passInfo.snapshotColorResolveSource && passInfo.resolveSourceSnapshot) {
        const wgpu::TexelCopyTextureInfo src{
            .texture = passInfo.copySourceTexture,
            .origin = wgpu::Origin3D{
                .x = static_cast<uint32_t>(passInfo.resolveSnapshotRect.x),
                .y = static_cast<uint32_t>(passInfo.resolveSnapshotRect.y),
            },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveSourceSnapshot->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveSnapshotRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveSnapshotRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
        resolveSourceTexture = passInfo.resolveSourceSnapshot->texture;
        resolveSourceView = passInfo.resolveSourceSnapshot->sampleTextureView;
      }
      if (isDepth && passInfo.msaaSamples > 1) {
        Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
      }
      const tex_copy_conv::ConvRequest convReq{
          .fmt = passInfo.resolveFormat,
          .srcView = resolveSourceView,
          .uniformRange = passInfo.resolveUniformRange,
          .dst = passInfo.resolveTarget,
          .sampleFilter = passInfo.resolveLinearSampling
                              ? tex_copy_conv::SampleFilter::Linear
                              : tex_copy_conv::SampleFilter::Nearest,
          .forceOpaqueAlpha = passInfo.resolveForceOpaqueAlpha,
      };
      if (passInfo.resolveNeedsConversion) {
        tex_copy_conv::run(cmd, convReq);
      } else if (passInfo.resolveNeedsShaderSampling) {
        tex_copy_conv::blit(cmd, convReq);
      } else {
        const wgpu::TexelCopyTextureInfo src{
            .texture = resolveSourceTexture,
            .origin =
                wgpu::Origin3D{
                    .x = static_cast<uint32_t>(passInfo.resolveRect.x -
                                               (passInfo.snapshotColorResolveSource
                                                    ? passInfo.resolveSnapshotRect.x : 0)),
                    .y = static_cast<uint32_t>(passInfo.resolveRect.y -
                                               (passInfo.snapshotColorResolveSource
                                                    ? passInfo.resolveSnapshotRect.y : 0)),
                },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveTarget->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
      }
    }
  }
  if (finalize) {
    recycle_render_passes(renderPasses);
  }

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (finalize && !g_debugGroupStack.empty()) {
    for (auto it = g_debugGroupStack.rbegin(); it != g_debugGroupStack.rend(); ++it) {
      Log.warn("Debug group was not popped at end of frame: {}", *it);
    }
    g_debugGroupStack.clear();
  }

  if (finalize && g_debugMarkers.size() > 0) {
    g_debugMarkers.clear();
  }
#endif
}

void seal_frame(SealedFrame& out) noexcept {
  ZoneScoped;
  // The encode that could still have been holding these has completed: the
  // producer joins the worker's DONE phase before it seals another frame.
  g_retiredBindGroups.clear();
  auto& passes = out.data().passes;
  // The previous cycle already recycled these, so this normally just hands the empty vector, its
  // capacity included, back to the producer.
  recycle_render_passes(passes);
  passes.swap(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
}

void render(SealedFrame& frame, wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize) {
  render_impl(frame.data().passes, cmd, interpolatedFrame, finalize);
}

void render(wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize) {
  render_impl(g_renderPasses, cmd, interpolatedFrame, finalize);
  if (finalize) {
    g_currentRenderPass = UINT32_MAX;
    expire_bind_group_cache();
  }
}

void after_submit() noexcept {
  depth_peek::after_submit();
  efb_ram::after_submit();
  // Retire this frame's completed GPU work. Dawn only reclaims destroyed resources inside a device
  // tick, and a frame that never ticks keeps every released image and its memory for the run.
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& renderPasses, u32 idx,
                             int32_t interpolatedFrame) {
  // Per-invocation, not per-process: two encoders can be recording at once.
  gx::DrawEncodeState encodeState{};
  encodeState.boundTextureBindGroup = gx::g_emptyTextureBindGroup.Get();
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif

  // Bind static bind group for the whole pass
  pass.SetBindGroup(0, g_staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (const auto& cmd : renderPasses[idx].commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      // WebGPU requires 0 <= minDepth <= maxDepth <= 1, and the guest's (near, far) order is already
      // reproduced in clip space. Passing the raw swapped pair diverged per backend in release builds.
      const float minDepth = std::clamp(std::min(vp.znear, vp.zfar), 0.0f, 1.0f);
      const float maxDepth = std::clamp(std::max(vp.znear, vp.zfar), 0.0f, 1.0f);
      pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      const auto& size = renderPasses[idx].targetSize;
      const auto left = std::clamp(sc.x, 0, static_cast<int32_t>(size.width));
      const auto top = std::clamp(sc.y, 0, static_cast<int32_t>(size.height));
      const auto right =
          std::clamp(sc.x + sc.width, left, static_cast<int32_t>(size.width));
      const auto bottom =
          std::clamp(sc.y + sc.height, top, static_cast<int32_t>(size.height));
      pass.SetScissorRect(static_cast<uint32_t>(left), static_cast<uint32_t>(top),
                          static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top));
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::GX: {
        const gfx::Range* uniformOverride = nullptr;
        if (interpolatedFrame >= 0 &&
            static_cast<size_t>(interpolatedFrame) < draw.gx.interpolatedUniformRanges.size() &&
            draw.gx.interpolatedUniformRanges[interpolatedFrame].size != 0) {
          uniformOverride = &draw.gx.interpolatedUniformRanges[interpolatedFrame];
        }
        gx::render(draw.gx, pass, encodeState, renderPasses[idx].requireReadyPipelines, uniformOverride);
      } break;
      case ShaderType::Clear:
        clear::render(draw.clear, pass, renderPasses[idx].targetSize, encodeState.currentPipeline);
        break;
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(g_debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass, PipelineRef& currentPipeline,
                   bool requireReady) {
  if (ref == currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  bool pipelineReady;
  if (!skip_unready_pipelines()) {
    pipelineReady = wait_pipeline(ref, pipeline);
  } else if (requireReady) {
    // The pass resolves into a persistent texture (a one-shot bake such as MKW's minimap), so a
    // skipped draw would never be re-issued. These run behind loads, not mid-race.
    pipelineReady = wait_pipeline_for_persistent_pass(ref, pipeline);
  } else {
    pipelineReady = try_pipeline(ref, pipeline);
  }
  if (!pipelineReady) {
    return false;
  }
  pass.SetPipeline(pipeline);
  currentPipeline = ref;
  return true;
}

static inline Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(alignment);
  } else {
    target.append(data, length);
    if (padding > 0) {
      target.append_zeroes(padding);
    }
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
static inline Range map(ByteBuffer& target, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(length);
  } else {
    // The caller fills [0, length); only the alignment padding needs clearing,
    // otherwise it hands the next frame's readers whatever was there before.
    target.append_uninitialized(length);
    target.append_zeroes(padding);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
Range push_verts(const uint8_t* data, size_t length) { return push(g_verts, data, length, 0); }
Range push_indices(const uint8_t* data, size_t length) { return push(g_indices, data, length, 0); }
Range push_uniform(const uint8_t* data, size_t length) {
  return push(g_uniforms, data, length, g_cachedLimits.minUniformBufferOffsetAlignment);
}
Range push_storage(const uint8_t* data, size_t length) {
  return push(g_storage, data, length, g_cachedLimits.minStorageBufferOffsetAlignment);
}
Range push_texture_data(const uint8_t* data, size_t length, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(g_textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = g_textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}
std::pair<ByteBuffer, Range> map_verts(size_t length) {
  const auto range = map(g_verts, length, 4);
  return {ByteBuffer{g_verts.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_indices(size_t length) {
  const auto range = map(g_indices, length, 4);
  return {ByteBuffer{g_indices.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  const auto range = map(g_uniforms, length, g_cachedLimits.minUniformBufferOffsetAlignment);
  return {ByteBuffer{g_uniforms.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  const auto destination = map(g_uniforms, source.size, g_cachedLimits.minUniformBufferOffsetAlignment);
  std::memcpy(g_uniforms.data() + destination.offset, g_uniforms.data() + source.offset, source.size);
  return {ByteBuffer{g_uniforms.data() + destination.offset, destination.size}, destination};
}
std::pair<ByteBuffer, Range> map_storage(size_t length) {
  const auto range = map(g_storage, length, g_cachedLimits.minStorageBufferOffsetAlignment);
  return {ByteBuffer{g_storage.data() + range.offset, range.size}, range};
}

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    auto bg = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &descriptor));
    g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindGroup = std::move(bg),
                                       .lastUsedFrame = g_frameIndex,
                                   });
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

wgpu::BindGroup& find_bind_group(BindGroupRef id) {
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "get_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup;
}

wgpu::Sampler& sampler_ref(const wgpu::SamplerDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  auto it = g_cachedSamplers.find(id);
  if (it == g_cachedSamplers.end()) {
    it = g_cachedSamplers.try_emplace(id, g_device.CreateSampler(&descriptor)).first;
  }
  return it->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, g_cachedLimits.minUniformBufferOffsetAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugMarkers.size();
  g_debugMarkers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugGroupStack.push_back(std::move(label));
#endif
}
void aurora_push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugGroupStack.emplace_back(label);
#endif
}
void aurora_pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugGroupStack.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }
