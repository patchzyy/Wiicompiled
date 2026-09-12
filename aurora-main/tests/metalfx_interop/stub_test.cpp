#include "webgpu/metalfx.hpp"

int main() {
  using namespace aurora::webgpu::metalfx;
  if (supported({}, wgpu::BackendType::Vulkan) || supported({}, wgpu::BackendType::Metal)) return 1;
  std::string error;
  if (create({}, {}, {640, 480, 1280, 960, wgpu::TextureFormat::RGBA8Unorm}, error)) return 1;
  return error.empty() ? 1 : 0;
}
