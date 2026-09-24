#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

struct SDL_Window;
struct SDL_Renderer;

namespace aurora::window {

enum class CustomEvent {
  FutureResize,
  RefreshSurface,
  End,
};
static Uint32 operator+(Uint32 lhs, CustomEvent rhs) { return lhs + static_cast<Uint32>(rhs); }

// On Android in particular, we need to hold a mutex around critical areas like surface creation
// and presentation, so that the SDLActivity doesn't destroy the surface out from underneath us.
class SurfaceLock {
public:
  SurfaceLock() noexcept;
  ~SurfaceLock();

  SurfaceLock(const SurfaceLock&) = delete;
  SurfaceLock& operator=(const SurfaceLock&) = delete;
};

bool initialize();
bool initialize_event_watch();
bool push_custom_event(CustomEvent eventType);
void shutdown();
bool create_window(AuroraBackend backend);
bool create_renderer();
void destroy_window();
void show_window();
AuroraWindowSize get_window_size();
const AuroraEvent* poll_events();
SDL_Window* get_sdl_window();
SDL_Renderer* get_sdl_renderer();
bool is_paused() noexcept;
bool is_presentable() noexcept;
void pump_events() noexcept;
bool native_resize_pending() noexcept;
bool native_window_size_matches(uint32_t width, uint32_t height) noexcept;
void set_surface_ready(bool ready) noexcept;
void set_title(const char* title);
void set_fullscreen(bool fullscreen);
bool get_fullscreen();
void set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode get_display_mode();
void set_window_size(uint32_t width, uint32_t height);
void set_window_position(uint32_t x, uint32_t y);
void center_window();
void sync_frame_buffer_size() noexcept;
void request_frame_buffer_resize();
void set_frame_buffer_scale(float scale);
void set_frame_buffer_aspect_fit(bool fit);
void set_force_aspect_16_9(bool force);
void set_present_surface_fill(bool fill);
void lock_present_aspect_ratio(int width, int height);
void unlock_present_aspect_ratio();
bool get_present_aspect_ratio(float& aspect) noexcept;
void set_background_input(bool value);
}; // namespace aurora::window
