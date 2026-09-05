#pragma once

#include <filesystem>
#include <string>

#include <imgui.h>

// Icons are CC BY 3.0 by Zacksly - see runtime/assets/touch/ATTRIBUTION.txt.
namespace TouchArt {

// By file stem, e.g. "a" or "d-pad". Null when the file is missing; callers
// fall back to drawing the shape.
ImTextureID Get(const char* name);

// Call once, after aurora is up.
void Preload();

} // namespace TouchArt
