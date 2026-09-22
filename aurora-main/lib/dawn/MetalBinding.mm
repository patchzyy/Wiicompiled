#include "BackendBinding.hpp"

#import <Foundation/Foundation.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

namespace aurora::webgpu::utils {
namespace {
constexpr const char* MetalViewProperty = "aurora.window.metal_view";

void SDLCALL DestroyMetalView(void*, void* value) {
  SDL_Metal_DestroyView(value);
}
} // namespace

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptorCocoa(SDL_Window* window) {
  const auto properties = SDL_GetWindowProperties(window);
  if (!properties) {
    return nullptr;
  }
  auto view = SDL_GetPointerProperty(properties, MetalViewProperty, nullptr);
  if (!view) {
    view = SDL_Metal_CreateView(window);
    if (!view) {
      return nullptr;
    }
    // Own one view per window, not per WebGPU surface. Surface recovery must
    // preserve the UIKit root and its controls (and the Cocoa Metal subview).
    // SDL cleans window properties before destroying its native window.
    // The cleanup callback also runs if setting the property fails.
    if (!SDL_SetPointerPropertyWithCleanup(properties, MetalViewProperty, view, DestroyMetalView, nullptr)) {
      return nullptr;
    }
  }
  auto desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = SDL_Metal_GetLayer(view);
  if (!desc->layer) {
    SDL_ClearProperty(properties, MetalViewProperty);
    return nullptr;
  }
  return desc;
}
} // namespace aurora::webgpu::utils
