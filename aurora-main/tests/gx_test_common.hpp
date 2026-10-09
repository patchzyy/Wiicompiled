#pragma once

#include <gtest/gtest.h>

#include <dolphin/gx.h>

#include "gx/gx.hpp"
#include "gx/fifo.hpp"
#include "gx/command_processor.hpp"
#include "gfx/depth_peek.hpp"
#include "internal.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

// Forward declare dirty state flush
extern "C" void __GXSetDirtyState();

namespace aurora::gfx::testing {
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

void reset_resolve_pass_records() noexcept;
const std::vector<ResolvePassRecord>& resolve_pass_records() noexcept;
void set_current_frame(uint32_t frame) noexcept;
void set_framebuffer_sizes(uint32_t logicalWidth, uint32_t logicalHeight,
                           uint32_t targetWidth, uint32_t targetHeight) noexcept;
void reset_vertex_push_record() noexcept;
const std::vector<uint8_t>& last_pushed_vertices() noexcept;
const std::vector<uint16_t>& last_pushed_indices() noexcept;
void reset_uniform_allocations() noexcept;
const std::vector<uint8_t>& uniform_allocation(size_t index) noexcept;
void use_draw_command_tracking(bool enabled) noexcept;
void use_real_vertex_format_helpers(bool enabled) noexcept;
// Admit this many draws, then refuse one as if the staging batch were full.
void refuse_staging_admission_after(uint32_t admissions) noexcept;
void reset_staging_capacity() noexcept;
} // namespace aurora::gfx::testing

class GXFifoTest : public ::testing::Test {
protected:
  void SetUp() override {
    aurora::gfx::testing::set_current_frame(0);
    aurora::gfx::testing::set_framebuffer_sizes(640, 480, 640, 480);
    GXInit(nullptr, 0);
    aurora::gx::fifo::clear_buffer();
    aurora::gx::g_gxState = aurora::gx::GXState{};
    aurora::g_config.disableCopyFilter = false;
    aurora::gfx::depth_peek::testing::reset();
    aurora::gfx::testing::reset_resolve_pass_records();
    aurora::gfx::testing::reset_vertex_push_record();
    aurora::gfx::testing::use_real_vertex_format_helpers(false);
    aurora::gfx::testing::reset_staging_capacity();
  }

  // Copy the internal FIFO buffer contents and clear it
  std::vector<u8> capture_fifo() {
    auto size = aurora::gx::fifo::get_buffer_size();
    auto* data = aurora::gx::fifo::get_buffer_data();
    std::vector<u8> bytes(data, data + size);
    aurora::gx::fifo::clear_buffer();
    return bytes;
  }

  // Reset g_gxState to default-constructed state
  void reset_gx_state() { aurora::gx::g_gxState = aurora::gx::GXState{}; }

  // Decode a captured FIFO byte stream through the command processor
  void decode_fifo(const std::vector<u8>& bytes) {
    aurora::gx::fifo::process(bytes.data(), static_cast<u32>(bytes.size()), true);
  }

  // Flush dirty state, then capture the FIFO buffer
  std::vector<u8> flush_and_capture() {
    __GXSetDirtyState();
    return capture_fifo();
  }

  // Convenience reference to g_gxState
  aurora::gx::GXState& gxState() { return aurora::gx::g_gxState; }
};
