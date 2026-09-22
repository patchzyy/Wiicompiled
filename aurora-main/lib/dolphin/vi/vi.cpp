#include <dolphin/vi.h>

#include "../../window.hpp"
#include "aurora/math.hpp"
#include "vi_internal.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <mutex>

namespace aurora::vi {
std::optional<GXRenderModeObj> g_renderMode;
namespace {
std::atomic<float> g_presentAspectCorrection{1.f};
std::mutex g_renderModeMutex;

float calculate_present_aspect_correction(const GXRenderModeObj& rm) noexcept {
  if (rm.viWidth == 0 || rm.viHeight == 0) {
    return 1.f;
  }

  // Dolphin derives the displayed aspect from the VI active region instead of assuming an exact
  // 4:3 or 16:9 signal. These are its BT.601 active sample and line counts.
  const auto format = (static_cast<uint32_t>(rm.viTVmode) >> 2) & 0x7u;
  const bool pal = format == VI_PAL || format == VI_DEBUG_PAL;
  const float nominalActiveWidth =
      pal ? 864.f * (52.f / 64.f) : 858.f * (52.655555f / 63.555555f);
  const float nominalActiveHeight = pal ? 576.f : 486.f;
  const float horizontalFill = static_cast<float>(rm.viWidth) / nominalActiveWidth;
  const float verticalFill = static_cast<float>(rm.viHeight) / nominalActiveHeight;
  return horizontalFill / verticalFill;
}

Vec2<uint32_t> render_mode_size_locked() noexcept {
  if (!g_renderMode) {
    return {640, 528};
  }
  // GX has a fixed 640x528 EFB workspace. Games can configure a narrower visible framebuffer, but
  // effects such as MKW's DOF use the remaining EFB area as scratch.
  return {std::max<uint32_t>(g_renderMode->fbWidth, 640), std::max<uint32_t>(g_renderMode->efbHeight, 528)};
}

} // namespace

Vec2<uint32_t> render_mode_size() noexcept {
  std::lock_guard lock(g_renderModeMutex);
  return render_mode_size_locked();
}

void configure(const GXRenderModeObj* rm) noexcept {
  bool sizeChanged = false;
  {
    std::lock_guard lock(g_renderModeMutex);
    const auto oldSize = render_mode_size_locked();
    if (rm == nullptr) {
      g_renderMode.reset();
    } else {
      g_renderMode = *rm;
      g_presentAspectCorrection.store(calculate_present_aspect_correction(*rm), std::memory_order_release);
    }
    if (rm == nullptr) {
      g_presentAspectCorrection.store(1.f, std::memory_order_release);
    }
    sizeChanged = render_mode_size_locked() != oldSize;
  }
  // Never hold the mode lock across a resize request or a renderer callback.
  if (sizeChanged) {
    window::request_frame_buffer_resize();
  }
}

Vec2<uint32_t> configured_fb_size() noexcept {
  return render_mode_size();
}

Vec2<uint32_t> visible_fb_size() noexcept {
  std::lock_guard lock(g_renderModeMutex);
  if (!g_renderMode) {
    return {640, 528};
  }
  return {g_renderMode->fbWidth, g_renderMode->efbHeight};
}

float present_aspect_correction() noexcept {
  return g_presentAspectCorrection.load(std::memory_order_acquire);
}
} // namespace aurora::vi

extern "C" {
void VIInit() {}
void VIConfigure(const GXRenderModeObj* rm) { aurora::vi::configure(rm); }
void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {
  (void)xOrg;
  (void)yOrg;
  (void)width;
  (void)height;
}
u32 VIGetTvFormat() { return 0; }
void VIFlush() {}

void VISetWindowTitle(const char* title) { aurora::window::set_title(title); }
void VISetWindowFullscreen(bool fullscreen) { aurora::window::set_fullscreen(fullscreen); }
bool VIGetWindowFullscreen() { return aurora::window::get_fullscreen(); }
void VISetWindowSize(uint32_t width, uint32_t height) { aurora::window::set_window_size(width, height); }
void VISetWindowPosition(uint32_t x, uint32_t y) { aurora::window::set_window_position(x, y); }
void VICenterWindow() { aurora::window::center_window(); }
void VISetFrameBufferScale(float scale) { aurora::window::set_frame_buffer_scale(scale); }
void VILockAspectRatio(int width, int height) { aurora::window::lock_present_aspect_ratio(width, height); }
void VIUnlockAspectRatio() { aurora::window::unlock_present_aspect_ratio(); }
}
