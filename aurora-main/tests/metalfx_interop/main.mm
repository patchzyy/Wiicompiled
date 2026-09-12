#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <dawn/native/MetalBackend.h>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr uint64_t kTimeoutNs = 10'000'000'000;
constexpr unsigned kFrames = 24;
std::atomic<unsigned> g_errors{0};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void wait(const wgpu::Instance& instance, wgpu::Future future) {
  require(instance.WaitAny(future, kTimeoutNs) == wgpu::WaitStatus::Success,
          "Dawn operation timed out or failed");
}

struct SharedImage {
  IOSurfaceRef surface = nullptr;
  id<MTLTexture> metal;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;

  SharedImage() = default;
  SharedImage(const SharedImage&) = delete;
  SharedImage& operator=(const SharedImage&) = delete;
  ~SharedImage() { if (surface) CFRelease(surface); }

  void create(const wgpu::Device& device, id<MTLDevice> native, uint32_t width,
              uint32_t height, bool bgra, MTLTextureUsage metalUsage,
              wgpu::TextureUsage usage) {
    const size_t rowBytes = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4);
    NSDictionary* properties = @{
      (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
      (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(rowBytes),
      (id)kIOSurfaceAllocSize: @(rowBytes * height),
      (id)kIOSurfacePixelFormat: @(bgra ? 0x42475241u : 0x52474241u)
    };
    surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    require(surface != nullptr, "IOSurface allocation failed");
    auto descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = metalUsage;
    metal = [native newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    require(metal != nil, "Metal IOSurface texture creation failed");

    wgpu::SharedTextureMemoryIOSurfaceDescriptor io{};
    io.ioSurface = surface;
    io.allowStorageBinding = false;
    wgpu::SharedTextureMemoryDescriptor import{};
    import.nextInChain = &io;
    memory = device.ImportSharedTextureMemory(&import);
    wgpu::SharedTextureMemoryProperties actual{};
    require(memory.GetProperties(&actual) == wgpu::Status::Success,
            "Dawn IOSurface import failed");
    const auto format = bgra ? wgpu::TextureFormat::BGRA8Unorm : wgpu::TextureFormat::RGBA8Unorm;
    require(actual.format == format && actual.size.width == width && actual.size.height == height,
            "Unexpected imported IOSurface format or dimensions");
    require((actual.usage & usage) == usage, "Imported texture lacks required Dawn usages");
    wgpu::TextureDescriptor textureDescriptor{};
    textureDescriptor.size = {width, height, 1};
    textureDescriptor.format = format;
    textureDescriptor.usage = usage;
    texture = memory.CreateTexture(&textureDescriptor);
    require(texture != nullptr, "Dawn shared texture creation failed");
  }
};

// EndAccess exports both ownership and GPU completion dependencies. Scheduling
// is a separate requirement on Metal: submit the producer before a queue waits
// for its event, without waiting for the GPU to finish the frame.
wgpu::SharedTextureMemoryEndAccessState endAccess(const wgpu::Instance& instance,
                                                 SharedImage& image) {
  wgpu::SharedTextureMemoryMetalEndAccessState metal{};
  wgpu::SharedTextureMemoryEndAccessState state{};
  state.nextInChain = &metal;
  require(image.memory.EndAccess(image.texture, &state) == wgpu::Status::Success,
          "Dawn EndAccess failed");
  wait(instance, metal.commandsScheduledFuture);
  state.nextInChain = nullptr;
  return state;
}

void encodeWaits(id<MTLCommandBuffer> commands,
                 const wgpu::SharedTextureMemoryEndAccessState& state) {
  for (size_t i = 0; i < state.fenceCount; ++i) {
    wgpu::SharedFenceMTLSharedEventExportInfo metal{};
    wgpu::SharedFenceExportInfo info{};
    info.nextInChain = &metal;
    state.fences[i].ExportInfo(&info);
    require(info.type == wgpu::SharedFenceType::MTLSharedEvent && metal.sharedEvent,
            "Dawn did not export a Metal shared event");
    [commands encodeWaitForEvent:(__bridge id<MTLSharedEvent>)metal.sharedEvent
                           value:state.signaledValues[i]];
  }
}

void beginAccess(SharedImage& image, bool initialized, const wgpu::SharedFence& fence,
                 uint64_t value) {
  wgpu::SharedTextureMemoryBeginAccessDescriptor access{};
  access.initialized = initialized;
  if (value) {
    access.fenceCount = 1;
    access.fences = &fence;
    access.signaledValueCount = 1;
    access.signaledValues = &value;
  }
  require(image.memory.BeginAccess(image.texture, &access) == wgpu::Status::Success,
          "Dawn BeginAccess failed");
}

struct Slot {
  SharedImage input, output;
  id<MTLFXSpatialScaler> scaler;
  id<MTLTexture> privateOutput;
  id<MTLSharedEvent> event;
  wgpu::SharedFence fence;
  uint64_t value = 0;
  wgpu::SharedTextureMemoryEndAccessState outputReleased{};
};

void runCase(const wgpu::Instance& instance, const wgpu::Device& device,
             id<MTLDevice> native, bool bgra, uint32_t width, uint32_t height,
             uint32_t outWidth, uint32_t outHeight) {
  const auto format = bgra ? wgpu::TextureFormat::BGRA8Unorm : wgpu::TextureFormat::RGBA8Unorm;
  const auto metalFormat = bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
  id<MTLCommandQueue> queue = [native newCommandQueue];
  require(queue != nil, "Metal command queue creation failed");
  std::array<Slot, 3> slots;
  for (auto& slot : slots) {
    auto descriptor = [MTLFXSpatialScalerDescriptor new];
    descriptor.inputWidth = width;
    descriptor.inputHeight = height;
    descriptor.outputWidth = outWidth;
    descriptor.outputHeight = outHeight;
    descriptor.colorTextureFormat = metalFormat;
    descriptor.outputTextureFormat = metalFormat;
    descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    slot.scaler = [descriptor newSpatialScalerWithDevice:native];
    require(slot.scaler != nil, "MetalFX spatial scaler creation failed");
    slot.input.create(device, native, width, height, bgra, slot.scaler.colorTextureUsage,
                      wgpu::TextureUsage::RenderAttachment);
    slot.output.create(device, native, outWidth, outHeight, bgra, MTLTextureUsageShaderRead,
                       wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::TextureBinding);
    auto outputDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalFormat
        width:outWidth height:outHeight mipmapped:NO];
    // MetalFX requires private output; IOSurface-backed shared storage cannot
    // be passed as outputTexture. Return it to Dawn with a GPU-only blit.
    outputDescriptor.storageMode = MTLStorageModePrivate;
    outputDescriptor.usage = slot.scaler.outputTextureUsage;
    slot.privateOutput = [native newTextureWithDescriptor:outputDescriptor];
    require(slot.privateOutput != nil, "Private MetalFX output allocation failed");
    slot.scaler.colorTexture = slot.input.metal;
    slot.scaler.outputTexture = slot.privateOutput;
    slot.scaler.inputContentWidth = width;
    slot.scaler.inputContentHeight = height;
    slot.event = [native newSharedEvent];
    require(slot.event != nil, "Metal shared event creation failed");
    wgpu::SharedFenceMTLSharedEventDescriptor shared{};
    shared.sharedEvent = (__bridge void*)slot.event;
    wgpu::SharedFenceDescriptor fenceDescriptor{};
    fenceDescriptor.nextInChain = &shared;
    slot.fence = device.ImportSharedFence(&fenceDescriptor);
    require(slot.fence != nullptr, "Dawn shared event import failed");
  }

  // Asymmetric quadrants expose channel swaps, vertical flips, and stale frames.
  wgpu::ShaderSourceWGSL source{};
  source.code = R"(
    @group(0) @binding(0) var<uniform> params: vec4f;
    @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
      let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
      return vec4f(p[i], 0, 1);
    }
    @fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
      return vec4f(params.x, select(0.2, 0.8, p.x >= params.y / 2),
                   select(0.3, 0.7, p.y >= params.z / 2), 1);
    }
  )";
  wgpu::ShaderModuleDescriptor shaderDescriptor{};
  shaderDescriptor.nextInChain = &source;
  auto shader = device.CreateShaderModule(&shaderDescriptor);
  wgpu::ColorTargetState target{};
  target.format = format;
  wgpu::FragmentState fragment{};
  fragment.module = shader;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor pipelineDescriptor{};
  pipelineDescriptor.vertex.module = shader;
  pipelineDescriptor.vertex.entryPoint = "vs";
  pipelineDescriptor.fragment = &fragment;
  auto pipeline = device.CreateRenderPipeline(&pipelineDescriptor);
  auto dawnQueue = device.GetQueue();
  const uint32_t bytesPerRow = (outWidth * 4 + 255) & ~255u;
  const uint64_t readbackSize = uint64_t(bytesPerRow) * outHeight;
  std::vector<wgpu::Buffer> readbacks;
  std::vector<id<MTLCommandBuffer>> nativeCommands;

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    auto& slot = slots[frame % slots.size()];
    beginAccess(slot.input, slot.value != 0, slot.fence, slot.value);
    const std::array<float, 4> params{0.2f + float(frame % 5) * 0.1f,
                                     float(width), float(height), 0};
    wgpu::BufferDescriptor uniformDescriptor{};
    uniformDescriptor.size = sizeof(params);
    uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    auto uniform = device.CreateBuffer(&uniformDescriptor);
    dawnQueue.WriteBuffer(uniform, 0, params.data(), sizeof(params));
    wgpu::BindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = uniform;
    entry.size = sizeof(params);
    wgpu::BindGroupDescriptor bindDescriptor{};
    bindDescriptor.layout = pipeline.GetBindGroupLayout(0);
    bindDescriptor.entryCount = 1;
    bindDescriptor.entries = &entry;
    auto bindGroup = device.CreateBindGroup(&bindDescriptor);
    auto encoder = device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = slot.input.texture.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor passDescriptor{};
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &attachment;
    auto pass = encoder.BeginRenderPass(&passDescriptor);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.Draw(3);
    pass.End();
    auto render = encoder.Finish();
    dawnQueue.Submit(1, &render);
    auto inputReleased = endAccess(instance, slot.input);

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    require(commands != nil, "Metal command buffer creation failed");
    encodeWaits(commands, inputReleased);
    encodeWaits(commands, slot.outputReleased);
    [slot.scaler encodeToCommandBuffer:commands];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    require(blit != nil, "Metal blit encoder creation failed");
    [blit copyFromTexture:slot.privateOutput sourceSlice:0 sourceLevel:0
        sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(outWidth, outHeight, 1)
        toTexture:slot.output.metal destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    ++slot.value;
    [commands encodeSignalEvent:slot.event value:slot.value];
    [commands commit];
    // Scheduling, not completion: avoid cross-queue scheduling inversions.
    [commands waitUntilScheduled];
    nativeCommands.push_back(commands);

    beginAccess(slot.output, true, slot.fence, slot.value);
    wgpu::BufferDescriptor readbackDescriptor{};
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto readback = device.CreateBuffer(&readbackDescriptor);
    encoder = device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo copySource{};
    copySource.texture = slot.output.texture;
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = readback;
    destination.layout.bytesPerRow = bytesPerRow;
    destination.layout.rowsPerImage = outHeight;
    const wgpu::Extent3D extent{outWidth, outHeight, 1};
    encoder.CopyTextureToBuffer(&copySource, &destination, &extent);
    auto copy = encoder.Finish();
    dawnQueue.Submit(1, &copy);
    slot.outputReleased = endAccess(instance, slot.output);
    readbacks.push_back(std::move(readback));
  }

  // Readback is only the test oracle. No CPU image transfer or GPU completion
  // wait occurs between Dawn rendering, MetalFX, and Dawn consumption above.
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    bool mapped = false;
    auto& readback = readbacks[frame];
    wait(instance, readback.MapAsync(wgpu::MapMode::Read, 0, readbackSize,
        wgpu::CallbackMode::WaitAnyOnly, [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
          mapped = status == wgpu::MapAsyncStatus::Success;
        }));
    require(mapped, "Output readback mapping failed");
    const auto* bytes = static_cast<const uint8_t*>(readback.GetConstMappedRange());
    require(bytes != nullptr, "Output readback pointer is null");
    for (unsigned y = 0; y < 4; ++y) {
      for (unsigned x = 0; x < 4; ++x) {
        const unsigned px = (2 * x + 1) * outWidth / 8;
        const unsigned py = (2 * y + 1) * outHeight / 8;
        const auto* pixel = bytes + py * bytesPerRow + px * 4;
        const std::array<float, 4> expected{
          0.2f + float(frame % 5) * 0.1f, x >= 2 ? 0.8f : 0.2f,
          y >= 2 ? 0.7f : 0.3f, 1};
        for (unsigned c = 0; c < 4; ++c) {
          const unsigned channel = bgra && c != 1 && c != 3 ? 2 - c : c;
          if (std::abs(int(pixel[channel]) - int(std::lround(expected[c] * 255))) > 5) {
            std::cerr << "Pixel mismatch: frame=" << frame << " x=" << px << " y=" << py
                      << " channel=" << c << " actual=" << int(pixel[channel])
                      << " expected=" << std::lround(expected[c] * 255) << '\n';
            throw std::runtime_error("MetalFX output failed image validation");
          }
        }
      }
    }
    readback.Unmap();
  }
  for (id<MTLCommandBuffer> commands : nativeCommands) {
    // Event signaling can precede the CPU-visible completed status. This wait
    // belongs to final verification, never the frame handoff above.
    [commands waitUntilCompleted];
    if (commands.error) std::cerr << "Metal error: " << commands.error.description.UTF8String << '\n';
    require(commands.status == MTLCommandBufferStatusCompleted, "Metal command buffer failed");
  }
  require(g_errors.load() == 0, "Dawn reported validation errors or device loss");
  std::cout << "PASS " << (bgra ? "BGRA8" : "RGBA8") << ' ' << width << 'x' << height
            << " -> " << outWidth << 'x' << outHeight << ": " << kFrames
            << " frames, 3 reused slots, 16 pixel samples/frame\n";
}

int run() {
  const wgpu::InstanceFeatureName timedWait = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instanceDescriptor{};
  instanceDescriptor.requiredFeatureCount = 1;
  instanceDescriptor.requiredFeatures = &timedWait;
  auto instance = wgpu::CreateInstance(&instanceDescriptor);
  require(instance != nullptr, "Dawn instance creation failed");
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{};
  options.backendType = wgpu::BackendType::Metal;
  wait(instance, instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
      [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message) {
        if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(result);
        else std::cerr << "Adapter: " << std::string_view(message) << '\n';
      }));
  if (!adapter) { std::cout << "SKIP: no Dawn Metal adapter\n"; return 77; }
  const std::array features{wgpu::FeatureName::SharedTextureMemoryIOSurface,
                            wgpu::FeatureName::SharedFenceMTLSharedEvent};
  for (auto feature : features) {
    if (!adapter.HasFeature(feature)) {
      std::cout << "SKIP: Dawn adapter lacks IOSurface/shared-event interoperability\n";
      return 77;
    }
  }
  wgpu::DeviceDescriptor descriptor{};
  descriptor.requiredFeatureCount = features.size();
  descriptor.requiredFeatures = features.data();
  descriptor.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
        ++g_errors;
        std::cerr << "Dawn error: " << std::string_view(message) << '\n';
      });
  descriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        if (reason != wgpu::DeviceLostReason::Destroyed) {
          ++g_errors;
          std::cerr << "Device lost: " << std::string_view(message) << '\n';
        }
      });
  wgpu::Device device;
  wait(instance, adapter.RequestDevice(&descriptor, wgpu::CallbackMode::WaitAnyOnly,
      [&device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message) {
        if (status == wgpu::RequestDeviceStatus::Success) device = std::move(result);
        else std::cerr << "Device: " << std::string_view(message) << '\n';
      }));
  require(device != nullptr, "Dawn device creation failed");
  id<MTLDevice> native = dawn::native::metal::GetMTLDevice(device.Get());
  require(native != nil, "Dawn native Metal device is unavailable");
  std::cout << "GPU: " << native.name.UTF8String << '\n';
  if (![MTLFXSpatialScalerDescriptor supportsDevice:native]) {
    std::cout << "SKIP: GPU does not support MetalFX spatial scaling\n";
    return 77;
  }
  for (bool bgra : {false, true}) {
    runCase(instance, device, native, bgra, 64, 48, 128, 96);
    runCase(instance, device, native, bgra, 320, 180, 480, 270);
    runCase(instance, device, native, bgra, 960, 540, 1920, 1080);
  }
  return 0;
}
} // namespace

int main() {
  @autoreleasepool {
    try { return run(); }
    catch (const std::exception& error) {
      std::cerr << "FAIL: " << error.what() << '\n';
      return 1;
    }
  }
}
