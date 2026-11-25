#pragma once

#include <algorithm>
#include <aurora/render_size_limits.hpp>
#include <cmath>
#include <cstdint>

// EGG::Screen picks a canvas record at 0x802A3EE8 via SCGetAspectRatio, squeezed into the fixed
// 608x456 framebuffer by xScale=fbWidth/width; only gScreenScaleX/Y (0x80386F20/24) feed vertical
// FOV, so records can only widen horizontal FOV. Solving for k = surfaceAspect/(16:9): record
// width = base*max(k,1), gScreenScaleY = max(1/k,1), giving Hor+ above 16:9 and Vert+ below it
// without cropping either axis. Verified bit-exact against the reference Gecko patch at 2560x1080.
namespace MkwDynamicAspect {

inline constexpr float kPresentationAspect169 = 16.0f / 9.0f;
inline constexpr float kPresentationAspect43 = 4.0f / 3.0f;
inline constexpr float kEggFramebufferWidth = 608.0f;
inline constexpr uint32_t kEggScreenHeight = 456u;
inline constexpr uint32_t kEggRecordWidth43 = 608u;
inline constexpr uint32_t kEggRecordWidth169 = 832u;
// Guard against a degenerate window turning the world inside out: 6.0 is a
// ~145 degree vertical FOV at MKW's stock 45 degree base, past which the
// 10.0 near plane and the game's own culling stop behaving.
inline constexpr float kMaxVerticalExpansion =
    aurora::render_size_limits::kMaxDynamicPortraitExpansion;

inline float SurfaceAspect(uint32_t width, uint32_t height) noexcept {
    if (width == 0 || height == 0) {
        return kPresentationAspect169;
    }
    return aurora::render_size_limits::clamp_dynamic_aspect(
        static_cast<float>(width) / static_cast<float>(height));
}

// surfaceAspect / referenceAspect. Above 1 the surface is wider than the
// reference, below 1 it is tighter (4:3, portrait).
inline float AspectRatioFactor(uint32_t width, uint32_t height,
                               float referenceAspect = kPresentationAspect169) noexcept {
    return SurfaceAspect(width, height) / referenceAspect;
}

// Hor+ factor, clamped at 1 so a tighter surface never narrows the canvas
// (that would crop away scene the reference view showed).
inline float HorizontalExpansion(uint32_t width, uint32_t height,
                                 float referenceAspect = kPresentationAspect169) noexcept {
    return std::max(1.0f, AspectRatioFactor(width, height, referenceAspect));
}

// Vert+ factor, clamped at 1 so a wider surface never shortens the frustum.
// Exactly one of the two expansions is ever greater than 1.
inline float VerticalExpansion(uint32_t width, uint32_t height,
                               float referenceAspect = kPresentationAspect169) noexcept {
    const float factor = AspectRatioFactor(width, height, referenceAspect);
    if (!(factor > 0.0f)) {
        return 1.0f;
    }
    return std::clamp(1.0f / factor, 1.0f, kMaxVerticalExpansion);
}

inline uint32_t EggRecordWidth(uint32_t baseWidth, uint32_t surfaceWidth, uint32_t surfaceHeight,
                               float referenceAspect = kPresentationAspect169) noexcept {
    const float scaledWidth = static_cast<float>(baseWidth) *
                              HorizontalExpansion(surfaceWidth, surfaceHeight, referenceAspect);
    return std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround(scaledWidth / 4.0f)) * 4u, 4u, 0xFFFCu);
}

inline float EggHorizontalScale(uint32_t recordWidth) noexcept {
    return kEggFramebufferWidth / static_cast<float>(std::max(recordWidth, 1u));
}

} // namespace MkwDynamicAspect
