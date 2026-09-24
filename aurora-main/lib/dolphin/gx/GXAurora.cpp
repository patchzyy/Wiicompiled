#include "dolphin/gx/GXAurora.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "__gx.h"
#include "gx.hpp"
#include "../../window.hpp"

#include "../../gfx/common.hpp"
#include "../../gx/fifo.hpp"

// Single definition for the `Log` that gx.hpp declares for this directory.
aurora::Module Log("aurora::gx");

static void GXWriteString(const char* label) {
  auto length = strlen(label);

  if (length > std::numeric_limits<u16>::max()) {
    Log.warn("Debug marker size over u16 max, truncating");
    length = std::numeric_limits<u16>::max();
  }

  GX_WRITE_U16(length);
  GX_WRITE_DATA(label, length);
}

void GXPushDebugGroup(const char* label) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_GROUP_PUSH);
  GXWriteString(label);
}

void GXPopDebugGroup() { GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_GROUP_POP); }

void GXInsertDebugMarker(const char* label) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_MARKER_INSERT);
  GXWriteString(label);
}

void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
  const bool changed = g_gxState.viewportPolicy != policy;
  if (changed) {
    // Finish commands using the old framebuffer mapping before changing it.
    aurora::gx::fifo::drain();
  }
  g_gxState.viewportPolicy = policy;
  aurora::window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  aurora::window::set_force_aspect_16_9(policy == AURORA_VIEWPORT_16_9);
  aurora::window::set_present_surface_fill(policy == AURORA_VIEWPORT_STRETCH);
  if (changed) {
    // Reapply the guest viewport and scissor after a resize.
    aurora::gx::set_logical_viewport(g_gxState.logicalViewport);
    aurora::gx::set_logical_scissor(g_gxState.logicalScissor);
  }
}

void AuroraGetRenderSize(u32* width, u32* height) {
  // The guest CPU thread uses this safe render-size value outside render passes.
  const auto renderSize = aurora::gfx::get_frame_buffer_size();
  if (width != nullptr) {
    *width = renderSize.x;
  }
  if (height != nullptr) {
    *height = renderSize.y;
  }
}

void AuroraGetSurfaceSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();
  if (width != nullptr) {
    *width = windowSize.native_fb_width;
  }
  if (height != nullptr) {
    *height = windowSize.native_fb_height;
  }
}

void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_VIEWPORT_RENDER);
  GX_WRITE_F32(left);
  GX_WRITE_F32(top);
  GX_WRITE_F32(wd);
  GX_WRITE_F32(ht);
  GX_WRITE_F32(nearz);
  GX_WRITE_F32(farz);
}

void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_SCISSOR_RENDER);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

namespace {
void WriteMappedRenderState(const aurora::gx::MappedRenderState& mapped) {
  GXSetViewportRender(mapped.viewport.left, mapped.viewport.top, mapped.viewport.width, mapped.viewport.height,
                      mapped.viewport.znear, mapped.viewport.zfar);
  GXSetScissorRender(static_cast<u32>(std::max(mapped.scissor.x, 0)),
                     static_cast<u32>(std::max(mapped.scissor.y, 0)),
                     static_cast<u32>(std::max(mapped.scissor.width, 0)),
                     static_cast<u32>(std::max(mapped.scissor.height, 0)));
}
} // namespace

void GXSetViewportScissorRenderSafeArea(f32 aspect) {
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (targetWidth == 0 || targetHeight == 0 || !std::isfinite(aspect) || aspect <= 0.0f) {
    return;
  }

  // Apply queued viewport changes before direct layout draws use the safe area.
  aurora::gx::fifo::drain();
  auto mapped = aurora::gx::map_logical_render_state();
  const float targetAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);

  float safeLeft = 0.0f;
  float safeTop = 0.0f;
  float safeWidth = static_cast<float>(targetWidth);
  float safeHeight = static_cast<float>(targetHeight);
  if (targetAspect > aspect) {
    safeWidth = safeHeight * aspect;
    safeLeft = (static_cast<float>(targetWidth) - safeWidth) * 0.5f;
  } else if (targetAspect < aspect) {
    safeHeight = safeWidth / aspect;
    safeTop = (static_cast<float>(targetHeight) - safeHeight) * 0.5f;
  }

  const float scaleX = safeWidth / static_cast<float>(targetWidth);
  const float scaleY = safeHeight / static_cast<float>(targetHeight);
  mapped.viewport.left = safeLeft + mapped.viewport.left * scaleX;
  mapped.viewport.top = safeTop + mapped.viewport.top * scaleY;
  mapped.viewport.width *= scaleX;
  mapped.viewport.height *= scaleY;

  const float scissorLeft = safeLeft + static_cast<float>(mapped.scissor.x) * scaleX;
  const float scissorTop = safeTop + static_cast<float>(mapped.scissor.y) * scaleY;
  const float scissorRight =
      safeLeft + static_cast<float>(mapped.scissor.x + mapped.scissor.width) * scaleX;
  const float scissorBottom =
      safeTop + static_cast<float>(mapped.scissor.y + mapped.scissor.height) * scaleY;
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(scissorLeft)), 0,
                                  static_cast<int32_t>(targetWidth));
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(scissorTop)), 0,
                                 static_cast<int32_t>(targetHeight));
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(scissorRight)), left,
                                   static_cast<int32_t>(targetWidth));
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(scissorBottom)), top,
                                    static_cast<int32_t>(targetHeight));
  mapped.scissor = {left, top, right - left, bottom - top};

  WriteMappedRenderState(mapped);
}

void GXRestoreViewportScissorRender() {
  // Run queued GX draws before leaving the direct layout safe area.
  aurora::gx::fifo::drain();
  WriteMappedRenderState(aurora::gx::map_logical_render_state());
}

void GXSetTexCopySrcRender(u16 left, u16 top, u16 wd, u16 ht) {
  aurora::gx::g_gxState.texCopySrc = {left, top, wd, ht};
  aurora::gx::g_gxState.texCopySrcRenderSpace = true;
}

void GXCreateFrameBuffer(u32 width, u32 height) {
  aurora::gx::fifo::drain();
  aurora::gfx::begin_offscreen(width, height);
}

void GXRestoreFrameBuffer() {
  aurora::gx::fifo::drain();
  aurora::gfx::end_offscreen();
}
