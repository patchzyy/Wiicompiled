#pragma once

#include <cstdint>

inline constexpr uint32_t SDL_INIT_AUDIO = 0x10u;
extern "C" bool SDL_InitSubSystem(uint32_t);
extern "C" void SDL_QuitSubSystem(uint32_t);
