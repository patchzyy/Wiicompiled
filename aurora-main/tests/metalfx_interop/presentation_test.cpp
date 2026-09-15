// Explicitly opted-in windowed test of Aurora's real frame/presentation path.
// No Wii game data is needed; the source override supplies a synthetic image.
#include <aurora/aurora.h>
#include <imgui.h>
#include <SDL3/SDL_timer.h>

#include "webgpu/gpu.hpp"
#include "window.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
std::atomic<unsigned> g_errors{0};

void log_message(AuroraLogLevel level, const char* module, const char* message, unsigned len) {
  if (level >= LOG_ERROR) ++g_errors;
  if (level >= LOG_WARNING || std::string_view(message, len).find("MetalFX") != std::string_view::npos)
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(len), message);
  if (level == LOG_FATAL) std::abort();
}

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void draw_frames(uint32_t width, uint32_t height, AuroraMetalFXStatus expected) {
  using namespace aurora::webgpu;
  auto source = create_render_texture(width, height, false);
  auto bindGroup = create_copy_bind_group(source);
  std::vector<uint32_t> pixels(size_t(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      pixels[size_t(y) * width + x] = 0xff000000u | ((x / 16 % 2) ? 0x00bb55u : 0xbb5500u);
    }
  }
  wgpu::TexelCopyTextureInfo target{};
  target.texture = source.texture;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = width * 4;
  layout.rowsPerImage = height;
  g_queue.WriteTexture(&target, pixels.data(), pixels.size() * sizeof(uint32_t), &layout, &source.size);

  unsigned rendered = 0;
  unsigned matchingStatus = 0;
  for (unsigned attempt = 0; attempt < 300 && rendered < 12; ++attempt) {
    aurora_update();
    if (!aurora_begin_frame()) { SDL_Delay(5); continue; }
    set_present_source_override(bindGroup, source.texture, source.size, source.format);
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::Begin("MetalFX presentation test", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Output-resolution overlay after game upscaling");
    ImGui::Text("Source: %u x %u", width, height);
    ImGui::End();
    aurora_end_frame();
    aurora_wait_for_frame_worker();
    const auto status = aurora_get_metalfx_status();
    require(status != AURORA_METALFX_ERROR, "MetalFX reported a presentation error");
    if (status == expected) ++matchingStatus;
    ++rendered;
  }
  require(rendered == 12 && matchingStatus >= 9, "Presentation did not reach the expected MetalFX state");
  require(g_errors.load() == 0, "Aurora reported an error");
  std::printf("PASS presentation source=%ux%u status=%d frames=%u\n", width, height, expected, rendered);
}
} // namespace

int main(int argc, char** argv) {
  const auto cache = std::filesystem::temp_directory_path() /
      ("aurora-metalfx-presentation-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(cache);
  const auto path = cache.string();
  AuroraConfig config{};
  config.appName = "MetalFX presentation test";
  config.userPath = path.c_str();
  config.cachePath = path.c_str();
  config.resourcesPath = path.c_str();
  config.desiredBackend = BACKEND_METAL;
  config.windowWidth = 640;
  config.windowHeight = 480;
  config.msaa = 1;
  config.maxTextureAnisotropy = 1;
  config.logCallback = log_message;
  config.logLevel = LOG_INFO;
  aurora_initialize(argc, argv, &config);
  int result = 0;
  try {
    if (!aurora_is_metalfx_spatial_supported()) {
      std::puts("SKIP: MetalFX spatial scaling is unavailable");
      result = 77;
    } else {
      aurora::window::lock_present_aspect_ratio(4, 3);
      aurora_set_metalfx_spatial(false);
      require(!aurora_get_metalfx_spatial(), "Disable request was not retained");
      draw_frames(320, 240, AURORA_METALFX_DISABLED);
      aurora_set_metalfx_spatial(true);
      require(aurora_get_metalfx_spatial(), "Enable request was not retained");
      draw_frames(320, 240, AURORA_METALFX_ACTIVE);
      aurora::window::set_window_size(800, 500);
      draw_frames(320, 240, AURORA_METALFX_ACTIVE);
      aurora::window::lock_present_aspect_ratio(16, 9);
      draw_frames(320, 240, AURORA_METALFX_ACTIVE);
      const auto output = aurora::window::get_window_size();
      draw_frames(output.native_fb_width, output.native_fb_height, AURORA_METALFX_NOT_UPSCALING);
      aurora_set_metalfx_spatial(false);
      draw_frames(320, 240, AURORA_METALFX_DISABLED);
      aurora_set_metalfx_spatial(true);
      draw_frames(320, 240, AURORA_METALFX_ACTIVE);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    result = 1;
  }
  aurora_shutdown();
  std::error_code cleanupError;
  std::filesystem::remove_all(cache, cleanupError);
  return result;
}
