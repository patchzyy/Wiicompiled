#pragma once

#include "common.hpp"
#include <dolphin/gx/GXAurora.h>

#include <vector>

namespace aurora::gfx::depth_peek {

void initialize();
void shutdown();

void request_snapshot() noexcept;
bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept;
void poll() noexcept;

// Captured before SEALED; the producer may configure the next frame during encode.
struct FrameMapping {
  Vec2<uint32_t> logicalSize{};
  AuroraViewportPolicy viewportPolicy = AURORA_VIEWPORT_FIT;
};
FrameMapping capture_frame_mapping() noexcept;

void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples, const FrameMapping& mapping) noexcept;
void after_submit() noexcept;

namespace testing {
void reset() noexcept;
bool snapshot_requested() noexcept;
void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data);
} // namespace testing

} // namespace aurora::gfx::depth_peek
