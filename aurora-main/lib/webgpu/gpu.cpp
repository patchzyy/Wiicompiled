#include "gpu.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/render_size_limits.hpp>
#include <magic_enum.hpp>
#include <webgpu/webgpu_cpp.h>

#include "../gfx/common.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "../dolphin/vi/vi_internal.hpp"

#if defined(WEBGPU_DAWN) && !defined(__MINGW32__)
#include "../dawn/BackendBinding.hpp"
#include <dawn/native/DawnNative.h>
#elif defined(WEBGPU_DAWN)
#include "../dawn/BackendBinding.hpp"
#endif

#if defined(WEBGPU_DAWN) && defined(_WIN32)
#include <windows.h>
#endif

namespace aurora::gx {
void clear_display_copy_cache() noexcept;
} // namespace aurora::gx
namespace aurora::gfx {
void clear_offscreen_cache();
} // namespace aurora::gfx

namespace aurora::webgpu {
static Module Log("aurora::gpu");

wgpu::Device g_device;
wgpu::Queue g_queue;
wgpu::Surface g_surface;
wgpu::BackendType g_backendType;
GraphicsConfig g_graphicsConfig;
TextureWithSampler g_frameBuffer;
TextureWithSampler g_frameBufferResolved;
TextureWithSampler g_depthBuffer;

// EFB -> XFB copy pipeline
static wgpu::BindGroupLayout g_CopyBindGroupLayout;
wgpu::RenderPipeline g_CopyPipeline;
wgpu::BindGroup g_CopyBindGroup;
static bool g_presentSourceOverrideActive = false;
static wgpu::BindGroup g_presentSourceOverrideBindGroup;
static wgpu::Texture g_presentSourceOverrideTexture;
static wgpu::Extent3D g_presentSourceOverrideSize{};
static wgpu::TextureFormat g_presentSourceOverrideFormat = wgpu::TextureFormat::Undefined;

static wgpu::Adapter g_adapter;
wgpu::Instance g_instance;
static wgpu::AdapterInfo g_adapterInfo;
static wgpu::SurfaceCapabilities g_surfaceCapabilities;
bool g_bcTexturesSupported;
// Written by Dawn's device-loss callback and consumed at ordered frame boundaries. Keep the
// callback free of logging, allocation, teardown and renderer state mutation.
static std::atomic_bool g_deviceLost{false};
static std::atomic<wgpu::DeviceLostReason> g_deviceLostReason{wgpu::DeviceLostReason::Unknown};
// The reason enum is almost always `Unknown`, while Dawn's message carries the real cause, so
// keep a truncated copy. Written with a plain memcpy, published by the g_deviceLost store.
static std::array<char, 256> g_deviceLostMessage{};
// Errors raised before initialize() completes must not be fatal: the backend fallback loop retries
// the next backend, and a broken ICD can raise uncaptured errors mid-probe.
static std::atomic_bool g_initialized{false};

namespace {

struct RenderTargetSize {
  uint32_t width;
  uint32_t height;
};

RenderTargetSize clamp_render_target_size(uint32_t width, uint32_t height) noexcept {
  const uint32_t maxDimension = g_graphicsConfig.maxTextureDimension2D;
  if (width == 0 || height == 0 || maxDimension == 0 || maxDimension == WGPU_LIMIT_U32_UNDEFINED ||
      (width <= maxDimension && height <= maxDimension)) {
    return {width, height};
  }

  // Keep the requested aspect while fitting both axes inside the adapter's maximum 2D texture size.
  // 64-bit arithmetic so a large window cannot wrap during the scale.
  if (width > maxDimension) {
    height = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(height) * maxDimension) / width));
    width = maxDimension;
  }
  if (height > maxDimension) {
    width = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(width) * maxDimension) / height));
    height = maxDimension;
  }
  return {width, height};
}

RenderTargetSize clamp_frame_buffer_size(uint32_t width, uint32_t height) noexcept {
  const auto adapterClamped = clamp_render_target_size(width, height);
  const auto budgeted =
      render_size_limits::fit_framebuffer_to_budget(adapterClamped.width, adapterClamped.height,
                                                    g_graphicsConfig.maxTextureDimension2D);
  return {budgeted.width, budgeted.height};
}

// V-Sync is never enabled: the guest drives its own pacing, and blocking in Present() couples the
// whole machine to the monitor (a 120 FPS target on a 75 Hz display runs in slow motion).
wgpu::PresentMode best_present_mode() {
  const auto supports = [](const wgpu::PresentMode candidate) {
    for (size_t i = 0; i < g_surfaceCapabilities.presentModeCount; ++i) {
      if (g_surfaceCapabilities.presentModes[i] == candidate) {
        return true;
      }
    }
    return false;
  };
  // Vulkan prefers Mailbox, every other backend Immediate. Under window capture the Vulkan driver
  // cannot flip and Immediate leaks about a megabyte per present until the device is lost.
  const bool preferMailbox = g_backendType == wgpu::BackendType::Vulkan;
  if (preferMailbox && supports(wgpu::PresentMode::Mailbox)) {
    return wgpu::PresentMode::Mailbox;
  }
  if (supports(wgpu::PresentMode::Immediate)) {
    return wgpu::PresentMode::Immediate;
  }
  if (g_backendType != wgpu::BackendType::Metal && supports(wgpu::PresentMode::Mailbox)) {
    return wgpu::PresentMode::Mailbox;
  }
  // Mailbox is preferred over Fifo because Fifo caps presentation at the refresh rate and every slot
  // deadline after the first is missed. Reaching this means neither is offered, so say so loudly.
  Log.warn("Surface supports neither Immediate nor Mailbox; falling back to Fifo. Presentation "
           "is capped at the display refresh rate, so the game may run slower than its own "
           "pace and frame interpolation cannot exceed the refresh rate.");
  return wgpu::PresentMode::Fifo;
}

wgpu::TextureFormat to_linear(wgpu::TextureFormat format) {
  if (format == wgpu::TextureFormat::RGBA8UnormSrgb) {
    return wgpu::TextureFormat::RGBA8Unorm;
  }
  if (format == wgpu::TextureFormat::BGRA8UnormSrgb) {
    return wgpu::TextureFormat::BGRA8Unorm;
  }
  return format;
}

wgpu::TextureFormat best_surface_format() {
  if (g_surfaceCapabilities.formatCount == 0) {
    return wgpu::TextureFormat::Undefined;
  }
  for (size_t i = 0; i < g_surfaceCapabilities.formatCount; ++i) {
    const auto format = to_linear(g_surfaceCapabilities.formats[i]);
    if (format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::BGRA8Unorm) {
      return format;
    }
  }
  return g_surfaceCapabilities.formats[0];
}

} // namespace

TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled) {
  const auto renderTargetSize = clamp_render_target_size(width, height);
  const wgpu::Extent3D size{
      .width = renderTargetSize.width,
      .height = renderTargetSize.height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  uint32_t sampleCount = 1;
  if (multisampled) {
    sampleCount = g_graphicsConfig.msaaSamples;
  }
  if (width == 0 || height == 0) {
    Log.fatal("Invalid render texture size! {}x{}, multisampled {}, format {}", width, height, static_cast<uint32_t>(format), multisampled);
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Render texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = sampleCount,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Render texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Render sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

const TextureWithSampler& present_source() noexcept {
  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
}

PresentSource current_present_source() noexcept {
  if (g_presentSourceOverrideActive && g_presentSourceOverrideBindGroup != nullptr) {
    return {
        .bindGroup = g_presentSourceOverrideBindGroup,
        .texture = g_presentSourceOverrideTexture,
        .size = g_presentSourceOverrideSize,
        .format = g_presentSourceOverrideFormat,
    };
  }

  return {
      .bindGroup = g_CopyBindGroup,
      .texture = present_source().texture,
      .size = present_source().size,
      .format = present_source().format,
  };
}

void set_present_source_override(wgpu::BindGroup bindGroup, wgpu::Texture texture, wgpu::Extent3D size,
                                 wgpu::TextureFormat format) noexcept {
  g_presentSourceOverrideBindGroup = std::move(bindGroup);
  g_presentSourceOverrideTexture = std::move(texture);
  g_presentSourceOverrideSize = size;
  g_presentSourceOverrideFormat = format;
  g_presentSourceOverrideActive = true;
}

void clear_present_source_override() noexcept {
  g_presentSourceOverrideActive = false;
  g_presentSourceOverrideBindGroup = {};
  g_presentSourceOverrideTexture = {};
  g_presentSourceOverrideSize = {};
  g_presentSourceOverrideFormat = wgpu::TextureFormat::Undefined;
}

Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept {
  if (surface_width == 0 || surface_height == 0 || !(content_aspect > 0.f)) {
    return {};
  }

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = std::min<uint32_t>(
      surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) *
                                                                               static_cast<double>(1.f / content_aspect)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(
        surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) *
                                                                                static_cast<double>(content_aspect)))));
  }

  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept {
  if (content_width == 0 || content_height == 0) {
    return {};
  }
  return calculate_present_viewport_for_aspect(
      surface_width, surface_height, static_cast<float>(content_width) / static_cast<float>(content_height));
}

static TextureWithSampler create_depth_texture(uint32_t width, uint32_t height) {
  const auto renderTargetSize = clamp_render_target_size(width, height);
  const wgpu::Extent3D size{
      .width = renderTargetSize.width,
      .height = renderTargetSize.height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Depth texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = g_graphicsConfig.msaaSamples,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Depth texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Depth sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

void create_copy_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = R"""(
@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(color.rgb, 1.0);
}
)""";
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "XFB Copy Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .writeMask = wgpu::ColorWriteMask::All,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_CopyBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_CopyBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .multisample =
          wgpu::MultisampleState{
              .count = 1,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  g_CopyPipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);
}

wgpu::BindGroup create_copy_bind_group(wgpu::TextureView sourceView, wgpu::Sampler sampler) {
  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .sampler = sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .textureView = sourceView,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_CopyBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  return g_device.CreateBindGroup(&bindGroupDescriptor);
}

wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source) {
  return create_copy_bind_group(source.view, source.sampler);
}

static wgpu::BackendType to_wgpu_backend(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_WEBGPU:
    return wgpu::BackendType::WebGPU;
  case BACKEND_D3D11:
    return wgpu::BackendType::D3D11;
  case BACKEND_D3D12:
    return wgpu::BackendType::D3D12;
  case BACKEND_METAL:
    return wgpu::BackendType::Metal;
  case BACKEND_VULKAN:
    return wgpu::BackendType::Vulkan;
  case BACKEND_OPENGL:
    return wgpu::BackendType::OpenGL;
  case BACKEND_OPENGLES:
    return wgpu::BackendType::OpenGLES;
  default:
    return wgpu::BackendType::Null;
  }
}

static bool create_surface() {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    Log.error("Failed to create surface: no window");
    return false;
  }
  const auto chainedDescriptor = utils::SetupWindowAndGetSurfaceDescriptor(window);
  if (!chainedDescriptor) {
    Log.error("Failed to create surface descriptor for current window");
    return false;
  }
  const wgpu::SurfaceDescriptor surfaceDescriptor{
      .nextInChain = chainedDescriptor.get(),
      .label = "Surface",
  };
  release_surface();
  g_surface = g_instance.CreateSurface(&surfaceDescriptor);
  if (!g_surface) {
    Log.error("Failed to create surface");
    return false;
  }
  return true;
}

bool initialize(AuroraBackend auroraBackend) {
  if (!g_instance) {
    Log.info("Creating WebGPU instance");
    const std::array requiredInstanceFeatures{
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor instanceDescriptor{
        .requiredFeatureCount = requiredInstanceFeatures.size(),
        .requiredFeatures = requiredInstanceFeatures.data(),
    };
#if defined(WEBGPU_DAWN) && !defined(__MINGW32__)
    // DawnNative.h's C++ constructor has an MSVC ABI that cannot cross into llvm-mingw, and the
    // descriptor only restates Dawn's defaults, so use the public WebGPU descriptor here.
    dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor;
    dawnInstanceDescriptor.backendValidationLevel = dawn::native::BackendValidationLevel::Disabled;
    instanceDescriptor.nextInChain = &dawnInstanceDescriptor;
#endif
    g_instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!g_instance) {
      Log.error("Failed to create WebGPU instance");
      return false;
    }
  }
  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);
  Log.info("Attempting to initialize {}", magic_enum::enum_name(backend));
  // One call is one backend attempt. aurora::initialize() retries without calling shutdown(), so a
  // leftover adapter would pass the `if (!g_adapter)` guard and mismatch adapter with device.
  g_queue = {};
  g_device = {};
  g_deviceLostReason.store(wgpu::DeviceLostReason::Unknown, std::memory_order_relaxed);
  g_deviceLost.store(false, std::memory_order_release);
  g_adapter = {};
  g_backendType = wgpu::BackendType::Undefined;
  {
    window::SurfaceLock surfaceLock;
    if (!create_surface()) {
      return false;
    }
  }
  {
    const wgpu::RequestAdapterOptions options{
        .powerPreference = wgpu::PowerPreference::HighPerformance,
        .backendType = backend,
        .compatibleSurface = g_surface,
    };
    const auto future = g_instance.RequestAdapter(
        &options, wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
          if (status == wgpu::RequestAdapterStatus::Success) {
            g_adapter = std::move(adapter);
          } else {
            Log.warn("Adapter request failed: {}", message);
          }
        });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      Log.error("Failed to create {} adapter: {}", magic_enum::enum_name(backend),
                magic_enum::enum_name(status));
      return false;
    }
    if (!g_adapter) {
      Log.error("No {} adapter is available on this system", magic_enum::enum_name(backend));
      return false;
    }
  }
  g_adapter.GetInfo(&g_adapterInfo);
  g_backendType = g_adapterInfo.backendType;
  const auto backendName = magic_enum::enum_name(g_backendType);
  auto adapterName = g_adapterInfo.device;
  if (adapterName.IsUndefined()) {
    adapterName = wgpu::StringView("Unknown");
  }
  auto description = g_adapterInfo.description;
  if (description.IsUndefined()) {
    description = wgpu::StringView("Unknown");
  }
  Log.info("Graphics adapter information\n  API: {}\n  Device: {} ({})\n  Driver: {}", backendName, adapterName,
           magic_enum::enum_name(g_adapterInfo.adapterType), description);

  uint32_t maxTextureDimension2D = 0;
  {
    wgpu::Limits supportedLimits{};
    g_adapter.GetLimits(&supportedLimits);
    maxTextureDimension2D = supportedLimits.maxTextureDimension2D;
    const wgpu::Limits requiredLimits{
        // Use "best" supported limits
        .maxTextureDimension1D = supportedLimits.maxTextureDimension1D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension1D,
        .maxTextureDimension2D = supportedLimits.maxTextureDimension2D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension2D,
        .maxTextureDimension3D = supportedLimits.maxTextureDimension3D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension3D,
        .maxTextureArrayLayers = supportedLimits.maxTextureArrayLayers == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureArrayLayers,
        .maxDynamicStorageBuffersPerPipelineLayout = supportedLimits.maxDynamicStorageBuffersPerPipelineLayout == 0
                                                         ? WGPU_LIMIT_U32_UNDEFINED
                                                         : supportedLimits.maxDynamicStorageBuffersPerPipelineLayout,
        .maxStorageBuffersPerShaderStage = supportedLimits.maxStorageBuffersPerShaderStage == 0
                                               ? WGPU_LIMIT_U32_UNDEFINED
                                               : supportedLimits.maxStorageBuffersPerShaderStage,
        .minUniformBufferOffsetAlignment =
            supportedLimits.minUniformBufferOffsetAlignment < 64 ? 64 : supportedLimits.minUniformBufferOffsetAlignment,
        .minStorageBufferOffsetAlignment =
            supportedLimits.minStorageBufferOffsetAlignment < 16 ? 16 : supportedLimits.minStorageBufferOffsetAlignment,
    };
    Log.info(
        "Using limits:"
        "\n  maxTextureDimension1D: {}"
        "\n  maxTextureDimension2D: {}"
        "\n  maxTextureDimension3D: {}"
        "\n  maxTextureArrayLayers: {}"
        "\n  maxDynamicStorageBuffersPerPipelineLayout: {}"
        "\n  maxStorageBuffersPerShaderStage: {}"
        "\n  minUniformBufferOffsetAlignment: {}"
        "\n  minStorageBufferOffsetAlignment: {}",
        requiredLimits.maxTextureDimension1D, requiredLimits.maxTextureDimension2D,
        requiredLimits.maxTextureDimension3D, requiredLimits.maxTextureArrayLayers,
        requiredLimits.maxDynamicStorageBuffersPerPipelineLayout, requiredLimits.maxStorageBuffersPerShaderStage,
        requiredLimits.minUniformBufferOffsetAlignment, requiredLimits.minStorageBufferOffsetAlignment);
    std::vector<wgpu::FeatureName> requiredFeatures;
    // Optional native sharing for MetalFX. Devices without either feature keep
    // the normal renderer; the upscaler checks the enabled pair at runtime.
    if (backend == wgpu::BackendType::Metal &&
        g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface) &&
        g_adapter.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent)) {
      requiredFeatures.push_back(wgpu::FeatureName::SharedTextureMemoryIOSurface);
      requiredFeatures.push_back(wgpu::FeatureName::SharedFenceMTLSharedEvent);
    }
    bool implicitDeviceSynchronizationSupported = false;
    wgpu::SupportedFeatures supportedFeatures;
    g_adapter.GetFeatures(&supportedFeatures);
    for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
      const auto feature = supportedFeatures.features[i];
      if (feature == wgpu::FeatureName::TextureCompressionBC) {
        g_bcTexturesSupported = true;
        requiredFeatures.push_back(feature);
      }
      // The presenter calls device and queue methods while the frame worker encodes, which Dawn only
      // supports with this feature; without it the two race inside the device's dynamic uploader.
      if (feature == wgpu::FeatureName::ImplicitDeviceSynchronization) {
        implicitDeviceSynchronizationSupported = true;
        requiredFeatures.push_back(feature);
      }
    }
    if (!implicitDeviceSynchronizationSupported) {
      Log.warn(
          "Adapter does not support ImplicitDeviceSynchronization; multi-threaded presentation is "
          "not safe on this device.");
    }
#ifdef WEBGPU_DAWN
    wgpu::DawnCacheDeviceDescriptor cacheDescriptor({
        .isolationKey = nullptr,
        .loadDataFunction = load_from_cache,
        .storeDataFunction = store_to_cache,
        .functionUserdata = nullptr,
    });

    std::vector<const char*> enableToggles{
    /* clang-format off */
#if _WIN32
      "use_dxc",
#ifndef NDEBUG
      "emit_hlsl_debug_symbols",
#endif
#endif
#ifndef ANDROID
      "use_user_defined_labels_in_backend",
#endif
      "disable_symbol_renaming",
      "enable_immediate_error_handling",
        /* clang-format on */
    };
#ifdef NDEBUG
    enableToggles.push_back("skip_validation");
    enableToggles.push_back("disable_robustness");
#endif
    if (g_backendType == wgpu::BackendType::Vulkan) {
      enableToggles.push_back("vulkan_monolithic_pipeline_cache");
    }
    const wgpu::DawnTogglesDescriptor togglesDescriptor({
        .nextInChain = &cacheDescriptor,
        .enabledToggleCount = enableToggles.size(),
        .enabledToggles = enableToggles.data(),
    });
#endif
    wgpu::DeviceDescriptor deviceDescriptor;
#ifdef WEBGPU_DAWN
    deviceDescriptor.nextInChain = &togglesDescriptor;
#endif
    deviceDescriptor.requiredFeatureCount = requiredFeatures.size();
    deviceDescriptor.requiredFeatures = requiredFeatures.data();
    deviceDescriptor.requiredLimits = &requiredLimits;
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
          if (g_initialized.load(std::memory_order_acquire)) {
            FATAL("WebGPU error {}: {}", underlying(type), message);
          } else {
            Log.warn("WebGPU error {}: {}", underlying(type), message);
          }
        });
    deviceDescriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
                                           [](const wgpu::Device& device, wgpu::DeviceLostReason reason,
                                              wgpu::StringView message) {
                                             (void)device;
                                             // Shutdown and backend retry release the final
                                             // device reference here too, not a real failure.
                                             if (reason == wgpu::DeviceLostReason::Destroyed) {
                                               return;
                                             }
                                             // Via string_view, so Dawn resolves a
                                             // WGPU_STRLEN length instead of SIZE_MAX.
                                             const std::string_view text{message};
                                             const size_t copied =
                                                 std::min(text.size(), g_deviceLostMessage.size() - 1);
                                             if (copied > 0) {
                                               std::memcpy(g_deviceLostMessage.data(), text.data(), copied);
                                             }
                                             g_deviceLostMessage[copied] = '\0';
                                             g_deviceLostReason.store(reason, std::memory_order_relaxed);
                                             g_deviceLost.store(true, std::memory_order_release);
                                           });
    const auto future =
        g_adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                  if (status == wgpu::RequestDeviceStatus::Success) {
                                    g_device = std::move(device);
                                  } else {
                                    Log.warn("Device request failed: {}", message);
                                  }
                                });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      Log.error("Failed to create device: {}", magic_enum::enum_name(status));
      return false;
    }
    if (!g_device) {
      return false;
    }
    g_device.SetLoggingCallback([](wgpu::LoggingType type, wgpu::StringView message) {
      AuroraLogLevel level = LOG_FATAL;
      switch (type) {
      case wgpu::LoggingType::Verbose:
        level = LOG_DEBUG;
        break;
      case wgpu::LoggingType::Info:
        level = LOG_INFO;
        break;
      case wgpu::LoggingType::Warning:
        level = LOG_WARNING;
        break;
      case wgpu::LoggingType::Error:
        level = LOG_ERROR;
        break;
      default:
        break;
      }
      Log.report(level, "WebGPU message: {}", message);
    });
  }
  g_queue = g_device.GetQueue();

  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);
  if (status != wgpu::Status::Success) {
    Log.error("Failed to get surface capabilities: {}", magic_enum::enum_name(status));
    return false;
  }
  if (g_surfaceCapabilities.formatCount == 0) {
    Log.error("Surface has no formats");
    return false;
  }
  if (g_surfaceCapabilities.presentModeCount == 0) {
    Log.error("Surface has no present modes");
    return false;
  }
  auto surfaceFormat = best_surface_format();
  auto presentMode = best_present_mode();
  Log.info("Using surface format {}, present mode {}", magic_enum::enum_name(surfaceFormat),
           magic_enum::enum_name(presentMode));
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .surfaceConfiguration =
          wgpu::SurfaceConfiguration{
              .format = surfaceFormat,
              .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
              .width = size.native_fb_width,
              .height = size.native_fb_height,
              .presentMode = presentMode,
          },
      .depthFormat = wgpu::TextureFormat::Depth32Float,
      .msaaSamples = g_config.msaa,
      .textureAnisotropy = g_config.maxTextureAnisotropy,
      .maxTextureDimension2D = maxTextureDimension2D,
  };
  create_copy_pipeline();
  {
    window::SurfaceLock surfaceLock;
    resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  }
  g_initialized.store(true, std::memory_order_release);
  return true;
}

void fail_if_device_lost() noexcept {
  if (!g_deviceLost.load(std::memory_order_acquire)) {
    return;
  }

  // Several frame-owning threads can observe loss, so serialize escalation: one thread logs and the
  // rest wait for termination instead of submitting more work to a lost device.
  static std::mutex fatalMutex;
  const std::lock_guard lock(fatalMutex);
  const auto reason = g_deviceLostReason.load(std::memory_order_relaxed);
  const char* const detail = g_deviceLostMessage.data();
  if (detail[0] != '\0') {
    Log.fatal("WebGPU device was lost ({}: {}). Rendering cannot continue safely; restart the application.",
              magic_enum::enum_name(reason), detail);
  } else {
    Log.fatal("WebGPU device was lost ({}). Rendering cannot continue safely; restart the application.",
              magic_enum::enum_name(reason));
  }
}

void serialize_pipeline_caches() noexcept {
#if defined(WEBGPU_DAWN) && defined(_WIN32)
  if (!g_device || g_backendType != wgpu::BackendType::Vulkan) {
    return;
  }
  using PerformIdleTasksFn = void(*)(const wgpu::Device*);
  static const auto performIdleTasks = []() -> PerformIdleTasksFn {
    const HMODULE dawnModule = GetModuleHandleW(L"webgpu_dawn.dll");
    if (dawnModule == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<PerformIdleTasksFn>(reinterpret_cast<void*>(
        GetProcAddress(dawnModule, "?PerformIdleTasks@native@dawn@@YAXAEBVDevice@wgpu@@@Z")));
  }();
  if (performIdleTasks != nullptr) {
    performIdleTasks(&g_device);
  }
#endif
}

void shutdown() {
  serialize_pipeline_caches();
  g_initialized.store(false, std::memory_order_release);
  g_CopyBindGroupLayout = {};
  g_CopyPipeline = {};
  g_CopyBindGroup = {};
  g_frameBuffer = {};
  g_frameBufferResolved = {};
  g_depthBuffer = {};
  g_queue = {};
  g_surface = {};
  g_device = {};
  g_adapter = {};
  g_instance = {};

  cache_shutdown();
}

void release_surface() noexcept {
  const bool hadSurface = static_cast<bool>(g_surface);
  if (g_surface) {
    g_surface.Unconfigure();
  }
  g_surface = {};
  if (hadSurface && g_instance && g_device && g_queue) {
    const auto future = g_queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
    g_instance.WaitAny(future, 1000000000);
  }
}

bool refresh_surface(bool recreate) {
  if (!g_instance || !g_device) {
    return false;
  }
  if (!window::is_presentable()) {
    release_surface();
    return false;
  }
  if ((!g_surface || recreate) && !create_surface()) {
    return false;
  }
  uint32_t width = g_graphicsConfig.surfaceConfiguration.width;
  uint32_t height = g_graphicsConfig.surfaceConfiguration.height;
  uint32_t native_width = width;
  uint32_t native_height = height;
  if (window::get_sdl_window() != nullptr) {
    const auto size = window::get_window_size();
    width = size.fb_width;
    height = size.fb_height;
    native_width = size.native_fb_width;
    native_height = size.native_fb_height;
  }
  if (width != 0 && height != 0) {
    resize_swapchain(width, height, native_width, native_height, true);
  }
  return true;
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height, bool force) {
  if (!g_surface || !g_device || width == 0 || height == 0 || native_height == 0 || native_width == 0) {
    return;
  }

  uint32_t render_width = width;
  uint32_t render_height = height;
  const auto [efbWidth, efbHeight] = vi::configured_fb_size();
  if (efbWidth != 0 && efbHeight != 0) {
    render_width = std::max(render_width, efbWidth);
    render_height = std::max(render_height, efbHeight);
  }

  const auto requestedRenderSize = RenderTargetSize{render_width, render_height};
  const auto clampedRenderSize = clamp_frame_buffer_size(render_width, render_height);
  render_width = clampedRenderSize.width;
  render_height = clampedRenderSize.height;
  if (requestedRenderSize.width != render_width || requestedRenderSize.height != render_height) {
    Log.warn(
        "Render target {}x{} exceeds the safe framebuffer budget (adapter max {}, practical max {} / {} "
        "pixels); clamping to {}x{}",
        requestedRenderSize.width, requestedRenderSize.height, g_graphicsConfig.maxTextureDimension2D,
        render_size_limits::kMaxFramebufferDimension, render_size_limits::kMaxFramebufferPixels, render_width,
        render_height);
  }

  const bool sizeChanged = g_graphicsConfig.surfaceConfiguration.width != native_width ||
                           g_graphicsConfig.surfaceConfiguration.height != native_height ||
                           g_frameBuffer.size.width != render_width || g_frameBuffer.size.height != render_height;
  if (!force && !sizeChanged) {
    return;
  }
  if (sizeChanged) {
    gx::clear_display_copy_cache();
    gfx::clear_caches();
    clear_present_source_override();
  }
  g_graphicsConfig.surfaceConfiguration.width = native_width;
  g_graphicsConfig.surfaceConfiguration.height = native_height;
  auto surfaceConfiguration = g_graphicsConfig.surfaceConfiguration;
  surfaceConfiguration.device = g_device;
  g_surface.Configure(&surfaceConfiguration);
  if (!sizeChanged) {
    // Forced reconfigure at an unchanged size (present-mode change or recreated surface). The
    // offscreen targets are not swapchain images, so reallocating them would only stall the frame.
    return;
  }
  g_frameBuffer = create_render_texture(render_width, render_height, true);
  g_frameBufferResolved = create_render_texture(render_width, render_height, false);
  g_depthBuffer = create_depth_texture(render_width, render_height);
  g_CopyBindGroup = create_copy_bind_group(present_source());
}
} // namespace aurora::webgpu
