// Stubs for renderer symbols the GX/FIFO/command_processor code references but that live in the
// full renderer, so the test binary links without the WebGPU runtime.

#include "gx/gx.hpp"
#include "gfx/clear.hpp"
#include "gfx/common.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <fmt/format.h>

namespace {
aurora::Vec2<uint32_t> s_logicalFbSize{640, 480};
aurora::Vec2<uint32_t> s_renderTargetSize{640, 480};
uint32_t s_currentFrame = 0;
std::vector<uint8_t> s_lastPushedVertices;
std::vector<uint16_t> s_lastPushedIndices;
std::optional<aurora::gx::DrawData> s_lastGxDraw;
bool s_trackDrawCommands = false;
bool s_useRealVertexFormatHelpers = false;
std::deque<std::vector<uint8_t>> s_uniformAllocations;
} // namespace

// --- aurora::g_config ---
namespace aurora {
AuroraConfig g_config{};
void wait_for_frame_worker() noexcept {}
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept { return {}; }
bool wait_for_frame_worker_for(std::chrono::microseconds) noexcept { return true; }
std::recursive_mutex& renderer_gpu_mutex() noexcept {
  static std::recursive_mutex mutex;
  return mutex;
}
} // namespace aurora

extern "C" bool aurora_wait_for_frame_worker_for(uint32_t) { return true; }

// --- aurora::log_internal ---
namespace aurora {
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int len) noexcept {
  fprintf(stderr, "[%d] %s: %.*s\n", static_cast<int>(level), module, len, message);
}
void Module::show_fatal_dialog(const char*, std::string_view) noexcept {}
} // namespace aurora

namespace aurora::window {
void set_present_surface_fill(bool) {}
} // namespace aurora::window

// --- fmt::formatter<AuroraLogLevel> ---
auto fmt::formatter<AuroraLogLevel>::format(AuroraLogLevel level, format_context& ctx) const
    -> format_context::iterator {
  return fmt::format_to(ctx.out(), "{}", static_cast<int>(level));
}

// --- GPU buffers (default-constructed, not used in tests) ---
namespace aurora::gfx {
AuroraStats g_stats;
wgpu::Buffer g_vertexBuffer;
wgpu::Buffer g_uniformBuffer;
wgpu::Buffer g_indexBuffer;
wgpu::Buffer g_storageBuffer;
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;
} // namespace aurora::gfx

namespace aurora::webgpu {
GraphicsConfig g_graphicsConfig{};
} // namespace aurora::webgpu

// --- GXState (the real instance -- tests validate this) ---
namespace aurora::gx {
GXState g_gxState{};
void notify_copy_texture_created() noexcept {}
} // namespace aurora::gx

namespace aurora::vi {
Vec2<uint32_t> configured_fb_size() noexcept { return s_logicalFbSize; }
void configure(const GXRenderModeObj*) noexcept {}
} // namespace aurora::vi

// --- Texture uploads ---
namespace aurora::gfx {
std::vector<TextureUpload> g_textureUploads;
} // namespace aurora::gfx

namespace aurora::gfx::efb_ram {
void schedule(void*, uint32_t, uint32_t, GXTexFmt, TextureHandle) noexcept {}
} // namespace aurora::gfx::efb_ram

// --- get_texture ---
namespace aurora::gx {
const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[id]; }
void evict_texture_object(u32 texObjId) noexcept {
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_tlut_object(u32 tlutObjId) noexcept {
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}
void invalidate_static_texture_cache() noexcept {
  for (auto& texture : g_gxState.textures) {
    texture.reset();
  }
  g_gxState.stateDirty = true;
}
void evict_copy_texture(const void* dest) noexcept {
  g_gxState.copyTextures.erase(dest);
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }
}
void set_display_copy_present_source() noexcept {}
void shutdown() noexcept {}
Vec2<uint32_t> logical_fb_size() noexcept { return s_logicalFbSize; }
gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  const float scaleX = static_cast<float>(s_renderTargetSize.x) / static_cast<float>(s_logicalFbSize.x);
  const float scaleY = static_cast<float>(s_renderTargetSize.y) / static_cast<float>(s_logicalFbSize.y);
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
  const auto mapped = map_logical_viewport({
      .left = static_cast<float>(logicalScissor.x),
      .top = static_cast<float>(logicalScissor.y),
      .width = static_cast<float>(logicalScissor.width),
      .height = static_cast<float>(logicalScissor.height),
      .znear = 0.0f,
      .zfar = 1.0f,
  });
  const auto left = static_cast<int32_t>(std::floor(mapped.left));
  const auto top = static_cast<int32_t>(std::floor(mapped.top));
  const auto right = static_cast<int32_t>(std::ceil(mapped.left + mapped.width));
  const auto bottom = static_cast<int32_t>(std::ceil(mapped.top + mapped.height));
  return {left, top, right - left, bottom - top};
}
MappedRenderState map_logical_render_state() noexcept {
  return {
      .viewport = map_logical_viewport(g_gxState.logicalViewport),
      .scissor = map_logical_scissor(g_gxState.logicalScissor),
  };
}
void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}
void set_render_viewport(const gfx::Viewport& viewport) noexcept { g_gxState.renderViewport = viewport; }
void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(scissor));
}
void set_render_scissor(const gfx::ClipRect& scissor) noexcept { g_gxState.renderScissor = scissor; }
} // namespace aurora::gx

// --- Shader/pipeline stubs ---
namespace aurora::gx {
void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  // No-op for tests
}
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept { return {}; }
void resolve_sampled_textures(const ShaderInfo& info) noexcept {}
u8 color_channel(GXChannelID id) noexcept { return 0; }
u8 comp_type_size(GXAttr attr, GXCompType type) noexcept {
  if (!s_useRealVertexFormatHelpers) {
    return 0;
  }
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
    default:
      return 0;
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
      return 0;
    }
  }
}
u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept {
  if (!s_useRealVertexFormatHelpers) {
    return 0;
  }
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
    return cnt == GX_POS_XY ? 2 : cnt == GX_POS_XYZ ? 3 : 0;
  case GX_VA_NRM:
    return cnt == GX_NRM_XYZ ? 3 : (cnt == GX_NRM_NBT || cnt == GX_NRM_NBT3) ? 9 : 0;
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
    return cnt == GX_TEX_S ? 1 : cnt == GX_TEX_ST ? 2 : 0;
  default:
    return 0;
  }
}
} // namespace aurora::gx

// --- Buffer push stubs ---
namespace aurora::gfx {
Range push_verts(const uint8_t* data, size_t length) {
  s_lastPushedVertices.assign(data, data + length);
  return {};
}
Range push_indices(const uint8_t* data, size_t length) {
  CHECK(length % sizeof(uint16_t) == 0, "unaligned test index upload");
  s_lastPushedIndices.resize(length / sizeof(uint16_t));
  std::memcpy(s_lastPushedIndices.data(), data, length);
  return {};
}
Range push_uniform(const uint8_t* data, size_t length) { return {}; }
Range push_storage(const uint8_t* data, size_t length) { return {}; }
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  s_uniformAllocations.emplace_back(length, 0);
  auto& uniformBytes = s_uniformAllocations.back();
  return {ByteBuffer{uniformBytes.data(), uniformBytes.size()},
          Range{static_cast<uint32_t>(s_uniformAllocations.size() - 1),
                static_cast<uint32_t>(length)}};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  return map_uniform(source.size);
}
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
uint64_t staging_uniform_bytes(uint64_t value) { return staging_padded(value, 256); }
uint64_t staging_storage_bytes(uint64_t value) { return staging_padded(value, 256); }
bool staging_has_space(const StagingSizes&) { return true; }
void split_staging_batch() { throw StagingCapacityError("Unexpected split in FIFO unit test"); }

Vec2<uint32_t> get_render_target_size() noexcept { return s_renderTargetSize; }
Vec2<uint32_t> get_frame_buffer_size() noexcept { return s_renderTargetSize; }
uint32_t current_frame() noexcept { return s_currentFrame; }
// The command processor keys its resolved-pipeline memo on this. The test build never changes
// render targets, so a fixed single-sample target matches what pipeline_ref would produce.
uint32_t get_sample_count() noexcept { return 1; }
void set_viewport(const Viewport& viewport) noexcept {}
void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept {}
} // namespace aurora::gfx

namespace aurora::gfx::testing {
void set_current_frame(uint32_t frame) noexcept { s_currentFrame = frame; }

void reset_vertex_push_record() noexcept {
  s_lastPushedVertices.clear();
  s_lastPushedIndices.clear();
  s_lastGxDraw.reset();
  s_trackDrawCommands = false;
  g_mergedDrawCallCount = 0;
}
const std::vector<uint8_t>& last_pushed_vertices() noexcept {
  return s_lastPushedVertices;
}
const std::vector<uint16_t>& last_pushed_indices() noexcept {
  return s_lastPushedIndices;
}
void reset_uniform_allocations() noexcept {
  s_uniformAllocations.clear();
}
const std::vector<uint8_t>& uniform_allocation(size_t index) noexcept {
  CHECK(index < s_uniformAllocations.size(), "uniform test allocation {} out of range {}", index,
        s_uniformAllocations.size());
  return s_uniformAllocations[index];
}
void use_draw_command_tracking(bool enabled) noexcept {
  s_trackDrawCommands = enabled;
  s_lastGxDraw.reset();
}
void use_real_vertex_format_helpers(bool enabled) noexcept {
  s_useRealVertexFormatHelpers = enabled;
}
} // namespace aurora::gfx::testing

// --- Pipeline/draw command stubs ---
namespace aurora::gfx {
template <>
PipelineRef pipeline_ref<clear::PipelineConfig>(const clear::PipelineConfig& config) {
  return 0;
}
template <>
void push_draw_command<clear::DrawData>(clear::DrawData data) {
  // No-op
}
template <>
PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
  return 0;
}
template <>
void push_draw_command<gx::DrawData>(gx::DrawData data) {
  if (s_trackDrawCommands) {
    s_lastGxDraw = std::move(data);
  }
}
template <>
gx::DrawData* get_last_draw_command() {
  return s_trackDrawCommands && s_lastGxDraw.has_value() ? &*s_lastGxDraw : nullptr;
}
} // namespace aurora::gfx

// --- TextureBind::get_descriptor ---
namespace aurora::gfx {
wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept { return wgpu::SamplerDescriptor{}; }
} // namespace aurora::gfx

// --- Texture creation/write/replacement stubs ---
namespace aurora::gfx {
namespace testing {
struct ResolvePassRecord {
  TextureHandle texture;
  ClipRect rect;
  bool clearColor = false;
  bool clearAlpha = false;
  bool clearDepth = false;
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  float clearDepthValue = 0.f;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  std::optional<Vec4<float>> sourceRectPixels;
  bool halfScale = false;
  std::array<u32, 3> copyFilterCoefficients{0, 64, 0};
  bool forceOpaqueAlpha = false;
  float copyFilterRowStride = 1.0f;
  bool clampTop = false;
  bool clampBottom = false;
  bool persistentCopy = false;
};

namespace {
std::vector<ResolvePassRecord> s_resolvePassRecords;
} // namespace

void reset_resolve_pass_records() noexcept { s_resolvePassRecords.clear(); }

const std::vector<ResolvePassRecord>& resolve_pass_records() noexcept { return s_resolvePassRecords; }

void set_framebuffer_sizes(uint32_t logicalWidth, uint32_t logicalHeight,
                           uint32_t targetWidth, uint32_t targetHeight) noexcept {
  s_logicalFbSize = {logicalWidth, logicalHeight};
  s_renderTargetSize = {targetWidth, targetHeight};
}
} // namespace testing

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {
  return {};
}
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  return {};
}
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  return std::make_shared<TextureRef>(wgpu::Texture{}, wgpu::TextureView{}, wgpu::TextureView{},
                                      wgpu::Extent3D{width, height, 1}, wgpu::TextureFormat::RGBA8Unorm, 1, gxFormat);
}
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  return std::make_shared<TextureRef>(wgpu::Texture{}, wgpu::TextureView{}, wgpu::TextureView{},
                                      wgpu::Extent3D{width, height, 1}, wgpu::TextureFormat::RGBA8Unorm, 1, gxFormat);
}
void write_texture(const TextureRef& ref, ArrayRef<uint8_t> data) noexcept {}
void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale,
                  const std::array<u32, 3>* copyFilterCoefficients, bool forceOpaqueAlpha,
                  float copyFilterRowStride, bool clampTop, bool clampBottom, bool persistentCopy) {
  testing::ResolvePassRecord record;
  record.texture = std::move(texture);
  record.rect = rect;
  record.clearColor = clearColor;
  record.clearAlpha = clearAlpha;
  record.clearDepth = clearDepth;
  record.clearColorValue = clearColorValue;
  record.clearDepthValue = clearDepthValue;
  record.resolveFormat = resolveFormat;
  record.sourceRectPixels = sourceRectPixels != nullptr ? std::make_optional(*sourceRectPixels) : std::nullopt;
  record.halfScale = halfScale;
  record.copyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  record.forceOpaqueAlpha = forceOpaqueAlpha;
  record.copyFilterRowStride = copyFilterRowStride;
  record.clampTop = clampTop;
  record.clampBottom = clampBottom;
  record.persistentCopy = persistentCopy;
  testing::s_resolvePassRecords.push_back(std::move(record));
}
void queue_palette_conv(tex_palette_conv::ConvRequest req) {}
void begin_offscreen(uint32_t width, uint32_t height) {}
void end_offscreen() {}
bool is_offscreen() noexcept { return false; }
} // namespace aurora::gfx

namespace aurora::gfx::depth_peek {
namespace {
bool s_snapshotRequested = false;
uint32_t s_width = 0;
uint32_t s_height = 0;
std::vector<uint32_t> s_data;
} // namespace

void initialize() {}
void shutdown() {}
void request_snapshot() noexcept { s_snapshotRequested = true; }
void poll() noexcept {}
void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept {}
void after_submit() noexcept {}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  if (x >= s_width || y >= s_height || s_data.empty()) {
    return false;
  }
  z = s_data[static_cast<size_t>(y) * s_width + x] & 0x00ffffffu;
  return true;
}

namespace testing {
void reset() noexcept {
  s_snapshotRequested = false;
  s_width = 0;
  s_height = 0;
  s_data.clear();
}

bool snapshot_requested() noexcept { return s_snapshotRequested; }

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  s_width = width;
  s_height = height;
  s_data = data;
}
} // namespace testing
} // namespace aurora::gfx::depth_peek

namespace aurora::gfx::tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) { return false; }
} // namespace aurora::gfx::tex_copy_conv

namespace aurora::gfx::tex_palette_conv {
void queue(ConvRequest req) {}
} // namespace aurora::gfx::tex_palette_conv

namespace aurora::gfx::texture_replacement {
u32 compute_texture_upload_size(const GXTexObj_& obj) noexcept { return 0; }
void register_tlut(const GXTlutObj*, const void*, GXTlutFmt, u16) noexcept {}
void load_tlut(const GXTlutObj*, u32) noexcept {}
std::optional<TextureHandle> find_replacement(const GXTexObj_&) noexcept { return std::nullopt; }
} // namespace aurora::gfx::texture_replacement

// --- Window stub ---
#include "../lib/window.hpp"
namespace aurora::window {
AuroraWindowSize get_window_size() { return {640, 480, 640, 480, 640, 480, 1.0f}; }
void set_frame_buffer_aspect_fit(bool) {}
} // namespace aurora::window

// --- WebGPU C API stubs (prevent linker errors from wgpu:: destructors) ---
extern "C" {
void wgpuDeviceRelease(WGPUDevice) {}
void wgpuQueueRelease(WGPUQueue) {}
void wgpuSurfaceRelease(WGPUSurface) {}
void wgpuBufferRelease(WGPUBuffer) {}
void wgpuTextureRelease(WGPUTexture) {}
void wgpuTextureViewRelease(WGPUTextureView) {}
void wgpuSamplerRelease(WGPUSampler) {}
void wgpuShaderModuleRelease(WGPUShaderModule) {}
void wgpuRenderPipelineRelease(WGPURenderPipeline) {}
void wgpuBindGroupRelease(WGPUBindGroup) {}
void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout) {}
void wgpuPipelineLayoutRelease(WGPUPipelineLayout) {}
void wgpuInstanceRelease(WGPUInstance) {}
void wgpuDeviceAddRef(WGPUDevice) {}
void wgpuQueueAddRef(WGPUQueue) {}
void wgpuSurfaceAddRef(WGPUSurface) {}
void wgpuBufferAddRef(WGPUBuffer) {}
void wgpuTextureAddRef(WGPUTexture) {}
void wgpuTextureViewAddRef(WGPUTextureView) {}
void wgpuInstanceAddRef(WGPUInstance) {}
}

void aurora::gfx::push_debug_group(std::string) {}
void aurora_push_debug_group(const char*) {}
void aurora_pop_debug_group() {}
void aurora::gfx::insert_debug_marker(std::string) {}
