#pragma once

#include <webgpu/webgpu_cpp.h>

#include <memory>
#include <string>

namespace aurora::webgpu::metalfx {
struct Size {
  uint32_t inputWidth;
  uint32_t inputHeight;
  uint32_t outputWidth;
  uint32_t outputHeight;
  wgpu::TextureFormat format;

  bool operator==(const Size&) const = default;
};

// All methods except supported() belong to the serialized frame encoder.
// GPU ownership is explicit: begin_input -> submit input -> upscale ->
// submit output consumption -> end_output. Neither texture may be used by
// Dawn outside its access interval. Destruction retires in-flight resources.
class SpatialScaler {
public:
  virtual ~SpatialScaler() = default;
  virtual const wgpu::TextureView& input_view() const = 0;
  virtual const wgpu::TextureView& output_view() const = 0;
  virtual const wgpu::Texture& output_texture() const = 0;
  virtual bool begin_input() = 0;
  virtual bool upscale() = 0;
  virtual bool end_output() = 0;
  virtual const std::string& error() const = 0;
};

bool supported(const wgpu::Device& device, wgpu::BackendType backend);
// A null result with no error means the bounded retirement pool is busy;
// skip upscaling for this frame and retry at a later frame boundary.
std::unique_ptr<SpatialScaler> create(const wgpu::Instance& instance,
                                      const wgpu::Device& device, const Size& size,
                                      std::string& error);
} // namespace aurora::webgpu::metalfx
