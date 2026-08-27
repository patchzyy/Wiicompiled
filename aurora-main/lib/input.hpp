#pragma once

#include <string>
#include <array>
#include "dolphin/pad.h" // For PADDeaZones and PADButtonMapping
#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_keyboard.h"
#include "SDL3/SDL_keycode.h"
#include "SDL3/SDL_mouse.h"
#include "logging.hpp"

#include <absl/container/flat_hash_map.h>

namespace aurora::input {
extern Module Log;

struct GameController {
  SDL_Gamepad* m_controller = nullptr;
  bool m_isGameCube = false;
  Sint32 m_index = -1;
  Sint32 m_playerIndex = -1;
  bool m_hasRumble = false;
  PADDeadZones m_deadZones{
      .emulateTriggers = true,
      .useDeadzones = true,
      .stickDeadZone = 8000,
      .substickDeadZone = 8000,
      .leftTriggerActivationZone = 31150,
      .rightTriggerActivationZone = 31150,
  };
  uint16_t m_vid = 0;
  uint16_t m_pid = 0;
  std::array<PADButtonMapping, PAD_BUTTON_COUNT> m_buttonMapping{};
  // Secondary binding per GC button; not persisted in .controller files, the
  // runtime re-applies it from its own config whenever a controller attaches.
  std::array<PADButtonMapping, PAD_BUTTON_COUNT> m_altButtonMapping{};
  std::array<PADAxisMapping, PAD_AXIS_COUNT> m_axisMapping{};
  uint16_t m_rumbleIntensityLow = 32767;
  uint16_t m_rumbleIntensityHigh = 32767;
  bool m_mappingLoaded = false;
  constexpr bool operator==(const GameController& other) const {
    return m_controller == other.m_controller && m_index == other.m_index;
  }
  uint8_t m_ledRed = 0xFF;
  uint8_t m_ledGreen = 0xFF;
  uint8_t m_ledBlue = 0xFF;
  bool m_isColorDirty = true;
  bool m_hasRgbLed = false;
};

GameController* get_controller_for_player(uint32_t player) noexcept;
Sint32 get_instance_for_player(uint32_t player) noexcept;
SDL_JoystickID add_controller(SDL_JoystickID which) noexcept;
bool refresh_controller(SDL_JoystickID instance) noexcept;
void remove_controller(Uint32 instance) noexcept;
Sint32 player_index(Uint32 instance) noexcept;
void set_player_index(Uint32 instance, Sint32 index) noexcept;
std::string controller_name(Uint32 instance) noexcept;
bool is_gamecube(Uint32 instance) noexcept;
bool controller_has_rumble(Uint32 instance) noexcept;
void controller_rumble(uint32_t instance, uint16_t low_freq_intensity, uint16_t high_freq_intensity,
                       uint16_t duration_ms) noexcept;
uint32_t controller_count() noexcept;
void initialize() noexcept;
void persist_controller_for_player(uint32_t player, const GameController* controller) noexcept;
extern absl::flat_hash_map<Uint32, GameController> g_GameControllers;

void set_mouse_scroll(float scrollX, float scrollY) noexcept;
void get_mouse_scroll(float* scrollX, float* scrollY) noexcept;

void shutdown() noexcept;
} // namespace aurora::input
