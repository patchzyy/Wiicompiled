#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include "webgpu/metalfx.hpp"

#include <dawn/native/MetalBackend.h>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
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

void runCase(const wgpu::Instance& instance, const wgpu::Device& device,
             bool bgra, uint32_t width, uint32_t height,
             uint32_t outWidth, uint32_t outHeight) {
  const auto format = bgra ? wgpu::TextureFormat::BGRA8Unorm : wgpu::TextureFormat::RGBA8Unorm;
  using namespace aurora::webgpu::metalfx;
  std::array<std::unique_ptr<SpatialScaler>, 3> slots;
  for (auto& slot : slots) {
    std::string error;
    slot = create(instance, device, {width, height, outWidth, outHeight, format}, error);
    if (!slot) throw std::runtime_error(error);
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

  for (unsigned frame = 0; frame < kFrames; ++frame) {
    auto& slot = slots[frame % slots.size()];
    require(slot->begin_input(), "Production MetalFX begin_input failed");
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
    attachment.view = slot->input_view();
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
    if (!slot->upscale()) throw std::runtime_error(slot->error());

    wgpu::BufferDescriptor readbackDescriptor{};
    readbackDescriptor.size = readbackSize;
    readbackDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto readback = device.CreateBuffer(&readbackDescriptor);
    encoder = device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo copySource{};
    copySource.texture = slot->output_texture();
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = readback;
    destination.layout.bytesPerRow = bytesPerRow;
    destination.layout.rowsPerImage = outHeight;
    const wgpu::Extent3D extent{outWidth, outHeight, 1};
    encoder.CopyTextureToBuffer(&copySource, &destination, &extent);
    auto copy = encoder.Finish();
    dawnQueue.Submit(1, &copy);
    if (!slot->end_output()) throw std::runtime_error(slot->error());
    readbacks.push_back(std::move(readback));
  }

  // Model toggle/resize immediately after submission, while either queue may
  // still be consuming these textures. Production completion callbacks must
  // keep the resources alive after the cache drops its wrappers.
  slots = {};

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
  if (!aurora::webgpu::metalfx::supported(device, wgpu::BackendType::Metal)) {
    std::cout << "SKIP: GPU does not support MetalFX spatial scaling\n";
    return 77;
  }
  require(!aurora::webgpu::metalfx::supported(device, wgpu::BackendType::Vulkan),
          "MetalFX must reject non-Metal backends");
  std::string error;
  require(!aurora::webgpu::metalfx::create(instance, device,
              {128, 96, 128, 96, wgpu::TextureFormat::RGBA8Unorm}, error) && !error.empty(),
          "MetalFX must reject equal-size input/output");
  require(!aurora::webgpu::metalfx::create(instance, device,
              {128, 96, 256, 192, wgpu::TextureFormat::RGBA8UnormSrgb}, error),
          "MetalFX must reject implicit sRGB conversion");
  {
    using namespace aurora::webgpu::metalfx;
    std::array<std::unique_ptr<SpatialScaler>, 6> resources;
    for (auto& scaler : resources) {
      scaler = create(instance, device, {64, 48, 128, 96, wgpu::TextureFormat::RGBA8Unorm}, error);
      require(scaler != nullptr, "Could not fill the MetalFX resource pool");
    }
    require(!create(instance, device, {64, 48, 128, 96, wgpu::TextureFormat::RGBA8Unorm}, error)
                && error.empty(), "A full retirement pool must defer allocation without a fatal error");
  }
  for (bool bgra : {false, true}) {
    runCase(instance, device, bgra, 64, 48, 128, 96);
    runCase(instance, device, bgra, 320, 180, 480, 270);
    runCase(instance, device, bgra, 960, 540, 1920, 1080);
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
