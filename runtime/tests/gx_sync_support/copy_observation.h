#pragma once

#include "gfx/recording.hpp"

#include <vector>

namespace aurora::gfx::testing {
struct CopyObservation {
    TextureHandle target;
    ClipRect rect;
    bool clearColor;
    bool clearAlpha;
    bool clearDepth;
    GXTexFmt format;
    ResolveOptions options;
    uint32_t draws;
};

extern std::vector<CopyObservation> copyObservations;
void observe_copy(TextureHandle target, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  GXTexFmt format, const ResolveOptions& options);
} // namespace aurora::gfx::testing
