#include "metalfx.hpp"

namespace aurora::webgpu::metalfx {
bool supported(const wgpu::Device&, wgpu::BackendType) { return false; }

std::unique_ptr<SpatialScaler> create(const wgpu::Instance&, const wgpu::Device&,
                                      const Size&, std::string& error) {
  error = "MetalFX is not available in this build";
  return {};
}
} // namespace aurora::webgpu::metalfx
