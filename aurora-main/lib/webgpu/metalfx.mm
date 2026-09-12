#include "metalfx.hpp"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <dawn/native/MetalBackend.h>

#include <atomic>
#include <string_view>

namespace aurora::webgpu::metalfx {
namespace {
constexpr uint64_t kScheduleTimeoutNs = 1'000'000'000;
// Three current slots plus at most three retiring slots during resize. A busy
// GPU must not allow resize events to allocate unbounded full-resolution images.
constexpr unsigned kMaxLiveResources = 6;
std::atomic<unsigned> g_liveResources{0};

struct SharedImage {
  IOSurfaceRef surface = nullptr;
  id<MTLTexture> metal;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;

  ~SharedImage() { if (surface) CFRelease(surface); }

  bool create(const wgpu::Device& device, id<MTLDevice> native, uint32_t width,
              uint32_t height, wgpu::TextureFormat format, MTLTextureUsage nativeUsage,
              wgpu::TextureUsage usage) {
    const bool bgra = format == wgpu::TextureFormat::BGRA8Unorm;
    const size_t rowBytes = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(width) * 4);
    NSDictionary* properties = @{
      (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
      (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(rowBytes),
      (id)kIOSurfaceAllocSize: @(rowBytes * height),
      (id)kIOSurfacePixelFormat: @(bgra ? 0x42475241u : 0x52474241u)
    };
    surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!surface) return false;
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = nativeUsage;
    metal = [native newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    if (!metal) return false;

    wgpu::SharedTextureMemoryIOSurfaceDescriptor io{};
    io.ioSurface = surface;
    io.allowStorageBinding = false;
    wgpu::SharedTextureMemoryDescriptor importDescriptor{};
    importDescriptor.nextInChain = &io;
    memory = device.ImportSharedTextureMemory(&importDescriptor);
    wgpu::SharedTextureMemoryProperties actual{};
    if (!memory || memory.GetProperties(&actual) != wgpu::Status::Success ||
        actual.format != format || actual.size.width != width || actual.size.height != height ||
        (actual.usage & usage) != usage) return false;
    wgpu::TextureDescriptor textureDescriptor{};
    textureDescriptor.label = "MetalFX shared texture";
    textureDescriptor.size = {width, height, 1};
    textureDescriptor.format = format;
    textureDescriptor.usage = usage;
    texture = memory.CreateTexture(&textureDescriptor);
    if (!texture) return false;
    view = texture.CreateView();
    return view != nullptr;
  }
};

struct API_AVAILABLE(macos(13.0)) Resources {
  SharedImage input, output;
  id<MTLFXSpatialScaler> scaler;
  id<MTLTexture> privateOutput;
  id<MTLCommandQueue> nativeQueue;
  id<MTLSharedEvent> event;
  wgpu::SharedFence fence;
  std::atomic<bool> failed{false};

  Resources() { ++g_liveResources; }
  ~Resources() { --g_liveResources; }
};

class API_AVAILABLE(macos(13.0)) MetalSpatialScaler final : public SpatialScaler {
  wgpu::Instance m_instance;
  wgpu::Queue m_queue;
  std::shared_ptr<Resources> m_resources;
  wgpu::SharedTextureMemoryEndAccessState m_outputReleased{};
  wgpu::Future m_outputScheduled{};
  uint64_t m_value = 0;
  std::string m_error;

  bool fail(const char* reason) {
    m_error = reason;
    m_resources->failed = true;
    return false;
  }

  bool wait_scheduled(wgpu::Future future) {
    return m_instance.WaitAny(future, kScheduleTimeoutNs) == wgpu::WaitStatus::Success ||
           fail("Timed out scheduling MetalFX GPU work");
  }

  bool end_access(SharedImage& image, wgpu::SharedTextureMemoryEndAccessState& state,
                  wgpu::Future& scheduled) {
    wgpu::SharedTextureMemoryMetalEndAccessState metal{};
    state.nextInChain = &metal;
    const auto status = image.memory.EndAccess(image.texture, &state);
    state.nextInChain = nullptr;
    scheduled = metal.commandsScheduledFuture;
    return status == wgpu::Status::Success || fail("MetalFX Dawn EndAccess failed");
  }

  bool begin_access(SharedImage& image, bool initialized, uint64_t value) {
    wgpu::SharedTextureMemoryBeginAccessDescriptor access{};
    access.initialized = initialized;
    if (value) {
      access.fenceCount = 1;
      access.fences = &m_resources->fence;
      access.signaledValueCount = 1;
      access.signaledValues = &value;
    }
    return image.memory.BeginAccess(image.texture, &access) == wgpu::Status::Success ||
           fail("MetalFX Dawn BeginAccess failed");
  }

  bool encode_waits(id<MTLCommandBuffer> commands,
                    const wgpu::SharedTextureMemoryEndAccessState& state) {
    for (size_t i = 0; i < state.fenceCount; ++i) {
      wgpu::SharedFenceMTLSharedEventExportInfo metal{};
      wgpu::SharedFenceExportInfo info{};
      info.nextInChain = &metal;
      state.fences[i].ExportInfo(&info);
      if (info.type != wgpu::SharedFenceType::MTLSharedEvent || !metal.sharedEvent)
        return fail("Dawn did not export a MetalFX shared-event dependency");
      [commands encodeWaitForEvent:(__bridge id<MTLSharedEvent>)metal.sharedEvent
                             value:state.signaledValues[i]];
    }
    return true;
  }

  void retain_until_dawn_done() {
    // A resize/toggle can destroy this wrapper immediately. The last submitted
    // Dawn consumer keeps the IOSurfaces/scaler alive independently of the cache.
    m_queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
        [resources = m_resources](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
          if (status != wgpu::QueueWorkDoneStatus::Success) resources->failed = true;
        });
  }

public:
  MetalSpatialScaler(const wgpu::Instance& instance, const wgpu::Device& device,
                      std::shared_ptr<Resources> resources)
  : m_instance(instance), m_queue(device.GetQueue()), m_resources(std::move(resources)) {}

  const wgpu::TextureView& input_view() const override { return m_resources->input.view; }
  const wgpu::TextureView& output_view() const override { return m_resources->output.view; }
  const wgpu::Texture& output_texture() const override { return m_resources->output.texture; }
  const std::string& error() const override { return m_error; }

  bool begin_input() override {
    if (m_resources->failed) return fail("Previous MetalFX GPU work failed");
    return begin_access(m_resources->input, m_value != 0, m_value);
  }

  bool upscale() override {
    @autoreleasepool {
      // Input has already been submitted. Retain it even if an export or native
      // allocation fails and the caller immediately falls back to normal copy.
      retain_until_dawn_done();
      wgpu::SharedTextureMemoryEndAccessState inputReleased{};
      wgpu::Future inputScheduled{};
      if (!end_access(m_resources->input, inputReleased, inputScheduled) ||
          !wait_scheduled(inputScheduled)) return false;
      if (m_value && !wait_scheduled(m_outputScheduled)) return false;
      id<MTLCommandBuffer> commands = [m_resources->nativeQueue commandBuffer];
      if (!commands) return fail("Could not allocate a MetalFX command buffer");
      commands.label = @"MetalFX spatial upscale and return to Dawn";
      if (!encode_waits(commands, inputReleased) || !encode_waits(commands, m_outputReleased))
        return false;
      [m_resources->scaler encodeToCommandBuffer:commands];
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      if (!blit) return fail("Could not allocate the MetalFX output blit");
      [blit copyFromTexture:m_resources->privateOutput sourceSlice:0 sourceLevel:0
          sourceOrigin:MTLOriginMake(0, 0, 0)
          sourceSize:MTLSizeMake(m_resources->privateOutput.width, m_resources->privateOutput.height, 1)
          toTexture:m_resources->output.metal destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
      ++m_value;
      [commands encodeSignalEvent:m_resources->event value:m_value];
      const auto resources = m_resources;
      [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError) resources->failed = true;
      }];
      [commands commit];
      // Scheduling is required to order independent Metal queues. Completion
      // remains asynchronous; resource reuse is guarded by shared GPU events.
      [commands waitUntilScheduled];
      if (commands.status == MTLCommandBufferStatusError)
        return fail("MetalFX command buffer failed");
      return begin_access(m_resources->output, true, m_value);
    }
  }

  bool end_output() override {
    retain_until_dawn_done();
    m_outputReleased = {};
    return end_access(m_resources->output, m_outputReleased, m_outputScheduled);
  }
};

// Allocation failures must be caught here rather than reaching Aurora's fatal
// uncaptured-error callback. Scope callbacks own their strings even on timeout.
bool pop_scope(const wgpu::Instance& instance, const wgpu::Device& device, std::string& error) {
  auto message = std::make_shared<std::string>();
  auto future = device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
      [message](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView text) {
        if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
          const std::string_view detail{text};
          *message = detail.empty() ? "MetalFX texture allocation failed" : std::string(detail);
        }
      });
  if (instance.WaitAny(future, kScheduleTimeoutNs) != wgpu::WaitStatus::Success) {
    error = "Timed out checking MetalFX texture allocation";
    return false;
  }
  if (!message->empty()) { error = *message; return false; }
  return true;
}
} // namespace

bool supported(const wgpu::Device& device, wgpu::BackendType backend) {
  if (@available(macOS 13.0, *)) {
    if (!device || backend != wgpu::BackendType::Metal ||
        !device.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface) ||
        !device.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent)) return false;
    auto native = dawn::native::metal::GetMTLDevice(device.Get());
    return native && [MTLFXSpatialScalerDescriptor supportsDevice:native];
  }
  return false;
}

std::unique_ptr<SpatialScaler> create(const wgpu::Instance& instance,
                                      const wgpu::Device& device, const Size& size,
                                      std::string& error) {
  error.clear();
  if (@available(macOS 13.0, *)) {
    @autoreleasepool {
      if (!supported(device, wgpu::BackendType::Metal)) {
        error = "MetalFX spatial scaling is unsupported";
        return {};
      }
      wgpu::Limits limits{};
      device.GetLimits(&limits);
      if (!size.inputWidth || !size.inputHeight || size.inputWidth >= size.outputWidth ||
          size.inputHeight >= size.outputHeight || size.outputWidth > limits.maxTextureDimension2D ||
          size.outputHeight > limits.maxTextureDimension2D ||
          (size.format != wgpu::TextureFormat::RGBA8Unorm && size.format != wgpu::TextureFormat::BGRA8Unorm)) {
        error = "MetalFX requires smaller input dimensions and an RGBA8/BGRA8 unorm target";
        return {};
      }
      if (g_liveResources.load() >= kMaxLiveResources) {
        return {};
      }
      auto native = dawn::native::metal::GetMTLDevice(device.Get());
      auto resources = std::make_shared<Resources>();
      auto descriptor = [MTLFXSpatialScalerDescriptor new];
      descriptor.inputWidth = size.inputWidth;
      descriptor.inputHeight = size.inputHeight;
      descriptor.outputWidth = size.outputWidth;
      descriptor.outputHeight = size.outputHeight;
      descriptor.colorTextureFormat = size.format == wgpu::TextureFormat::BGRA8Unorm
          ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
      descriptor.outputTextureFormat = descriptor.colorTextureFormat;
      descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
      resources->scaler = [descriptor newSpatialScalerWithDevice:native];
      resources->nativeQueue = [native newCommandQueue];
      resources->event = [native newSharedEvent];
      if (!resources->scaler || !resources->nativeQueue || !resources->event) {
        error = "Could not create MetalFX spatial resources";
        return {};
      }
      auto outputDescriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:descriptor.outputTextureFormat
          width:size.outputWidth height:size.outputHeight mipmapped:NO];
      outputDescriptor.storageMode = MTLStorageModePrivate;
      outputDescriptor.usage = resources->scaler.outputTextureUsage;
      resources->privateOutput = [native newTextureWithDescriptor:outputDescriptor];
      if (!resources->privateOutput) { error = "Could not allocate MetalFX private output"; return {}; }

      device.PushErrorScope(wgpu::ErrorFilter::Validation);
      device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
      device.PushErrorScope(wgpu::ErrorFilter::Internal);
      bool allocated = resources->input.create(device, native, size.inputWidth, size.inputHeight,
          size.format, resources->scaler.colorTextureUsage, wgpu::TextureUsage::RenderAttachment);
      allocated = allocated && resources->output.create(device, native, size.outputWidth, size.outputHeight,
          size.format, MTLTextureUsageShaderRead, wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc);
      if (allocated) {
        wgpu::SharedFenceMTLSharedEventDescriptor event{};
        event.sharedEvent = (__bridge void*)resources->event;
        wgpu::SharedFenceDescriptor fence{};
        fence.nextInChain = &event;
        resources->fence = device.ImportSharedFence(&fence);
        allocated = resources->fence != nullptr;
      }
      for (int i = 0; i < 3; ++i) {
        if (!pop_scope(instance, device, error)) allocated = false;
      }
      if (!allocated) {
        if (error.empty()) error = "Could not import MetalFX IOSurface textures into Dawn";
        return {};
      }
      resources->scaler.colorTexture = resources->input.metal;
      resources->scaler.outputTexture = resources->privateOutput;
      resources->scaler.inputContentWidth = size.inputWidth;
      resources->scaler.inputContentHeight = size.inputHeight;
      return std::make_unique<MetalSpatialScaler>(instance, device, std::move(resources));
    }
  }
  error = "MetalFX requires macOS 13 or newer";
  return {};
}
} // namespace aurora::webgpu::metalfx
