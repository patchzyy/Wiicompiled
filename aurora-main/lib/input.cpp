#include "input.hpp"
#include "internal.hpp"

#include "magic_enum.hpp"

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace aurora::input {
Module Log("aurora::input");
absl::flat_hash_map<Uint32, GameController> g_GameControllers;

namespace {
constexpr uint32_t kPortPreferencesMagic = SBIG('CPRT');
constexpr uint32_t kPortPreferencesVersion = 3;
constexpr uint32_t kMaxPersistedStringLength = 256;

enum class PortPreferenceState : uint8_t {
  Unset = 0,
  None = 1,
  Controller = 2,
};

struct ControllerIdentity {
  std::string guid;
  std::string serial;
  std::string path;
  int32_t slot = -1;
};

struct PortPreference {
  PortPreferenceState state = PortPreferenceState::Unset;
  ControllerIdentity identity;
};

std::array<PortPreference, PAD_MAX_CONTROLLERS> g_portPreferences;
bool g_portPreferencesLoaded = false;
// Startup controllers are added one by one via events; capture their SDL player index at init,
// before earlier port reassignments can move it.
absl::flat_hash_map<SDL_JoystickID, int32_t> g_connectSlots;

std::string port_preferences_path() {
  if (g_config.userPath == nullptr) {
    return {};
  }

  std::string path{g_config.userPath};
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += "controller_ports.dat";
  return path;
}

std::string normalize_serial(const char* serial) {
  std::string normalized;
  if (serial == nullptr) {
    return normalized;
  }

  for (const char* c = serial; *c != '\0'; ++c) {
    if (*c == ':' || *c == '-') {
      continue;
    }
    normalized += *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a') : *c;
  }
  return normalized;
}

bool serial_matches(const std::string& saved, const std::string& current) {
  return !saved.empty() && !current.empty() && normalize_serial(saved.c_str()) == normalize_serial(current.c_str());
}

ControllerIdentity controller_identity(const GameController& controller) {
  ControllerIdentity identity;
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(controller.m_controller)), guid, sizeof(guid));
  identity.guid = guid;
  identity.serial = normalize_serial(SDL_GetGamepadSerial(controller.m_controller));
  identity.path = controller.m_path;
  identity.slot = controller.m_slot;
  return identity;
}

bool read_exact(SDL_IOStream* file, void* dst, size_t size) {
  auto* bytes = static_cast<uint8_t*>(dst);
  size_t total = 0;
  while (total < size) {
    const size_t read = SDL_ReadIO(file, bytes + total, size - total);
    if (read == 0) {
      return false;
    }
    total += read;
  }
  return true;
}

bool write_exact(SDL_IOStream* file, const void* src, size_t size) {
  auto* bytes = static_cast<const uint8_t*>(src);
  size_t total = 0;
  while (total < size) {
    const size_t written = SDL_WriteIO(file, bytes + total, size - total);
    if (written == 0) {
      return false;
    }
    total += written;
  }
  return true;
}

template <typename T>
bool read_value(SDL_IOStream* file, T& value) {
  return read_exact(file, &value, sizeof(value));
}

template <typename T>
bool write_value(SDL_IOStream* file, const T& value) {
  return write_exact(file, &value, sizeof(value));
}

bool read_string(SDL_IOStream* file, std::string& value) {
  uint32_t size = 0;
  if (!read_value(file, size) || size > kMaxPersistedStringLength) {
    return false;
  }

  value.resize(size);
  return size == 0 || read_exact(file, value.data(), size);
}

bool write_string(SDL_IOStream* file, const std::string& value) {
  const uint32_t size = static_cast<uint32_t>(std::min<size_t>(value.size(), kMaxPersistedStringLength));
  return write_value(file, size) && (size == 0 || write_exact(file, value.data(), size));
}

bool read_identity(SDL_IOStream* file, uint32_t version, ControllerIdentity& identity) {
  if (!read_string(file, identity.guid) || !read_string(file, identity.serial)) {
    return false;
  }
  return version < 3 || (read_string(file, identity.path) && read_value(file, identity.slot));
}

bool write_identity(SDL_IOStream* file, const ControllerIdentity& identity) {
  return write_string(file, identity.guid) && write_string(file, identity.serial) &&
         write_string(file, identity.path) && write_value(file, identity.slot);
}

bool read_port_preferences_file(std::array<PortPreference, PAD_MAX_CONTROLLERS>& preferences) {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return true;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "rb");
  if (file == nullptr) {
    return true;
  }

  uint32_t magic = 0;
  uint32_t version = 0;
  bool ok = read_value(file, magic) && read_value(file, version) && magic == kPortPreferencesMagic &&
            version >= 2 && version <= kPortPreferencesVersion;

  for (auto& preference : preferences) {
    uint8_t state = 0;
    ControllerIdentity identity;
    ok = ok && read_value(file, state) && state <= static_cast<uint8_t>(PortPreferenceState::Controller) &&
         read_identity(file, version, identity);
    if (ok) {
      preference.state = static_cast<PortPreferenceState>(state);
      preference.identity = std::move(identity);
    }
  }

  SDL_CloseIO(file);
  if (!ok) {
    Log.warn("Ignoring invalid controller port preference file '{}'", path);
  }
  return ok;
}

void ensure_port_preferences_loaded() {
  if (g_portPreferencesLoaded) {
    return;
  }

  std::array<PortPreference, PAD_MAX_CONTROLLERS> preferences;
  if (read_port_preferences_file(preferences)) {
    g_portPreferences = std::move(preferences);
  }
  g_portPreferencesLoaded = true;
}

void save_port_preferences() {
  const auto path = port_preferences_path();
  if (path.empty()) {
    return;
  }

  if (!SDL_CreateDirectory(g_config.userPath)) {
    Log.warn("Failed to create controller port preference directory '{}': {}", g_config.userPath, SDL_GetError());
    return;
  }

  SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "wb");
  if (file == nullptr) {
    Log.warn("Failed to open controller port preference file '{}': {}", path, SDL_GetError());
    return;
  }

  bool ok = write_value(file, kPortPreferencesMagic) && write_value(file, kPortPreferencesVersion);
  for (const auto& preference : g_portPreferences) {
    const auto state = static_cast<uint8_t>(preference.state);
    ok = ok && write_value(file, state) && write_identity(file, preference.identity);
  }

  if (!SDL_FlushIO(file)) {
    ok = false;
  }
  if (!SDL_CloseIO(file)) {
    ok = false;
  }
  if (!ok) {
    Log.warn("Failed to write controller port preference file '{}': {}", path, SDL_GetError());
  }
}

// Zero means no match. Higher scores claim ports first: path+slot > GUID > VID/PID.
constexpr int kScoreVidPid = 1;
constexpr int kScoreGuid = 2;
constexpr int kScoreSlot = 1;
constexpr int kScorePath = 2;

int identity_score(const ControllerIdentity& saved, const ControllerIdentity& current) {
  if (saved.guid.empty()) {
    return 0;
  }
  if (saved.guid == current.guid) {
    if (!saved.serial.empty() && !serial_matches(saved.serial, current.serial)) {
      return 0;
    }
    int score = kScoreGuid;
    if (!saved.path.empty() && saved.path == current.path) {
      score += kScorePath;
    }
    if (saved.slot >= 0 && saved.slot == current.slot) {
      score += kScoreSlot;
    }
    return score;
  }
  if (!serial_matches(saved.serial, current.serial)) {
    return 0;
  }

  // Attempt to match against VID/PID as a fallback if the GUID changes
  uint16_t savedVendor = 0;
  uint16_t savedProduct = 0;
  uint16_t currentVendor = 0;
  uint16_t currentProduct = 0;
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(saved.guid.c_str()), &savedVendor, &savedProduct, nullptr, nullptr);
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(current.guid.c_str()), &currentVendor, &currentProduct, nullptr, nullptr);
  const bool sameDevice =
      savedVendor != 0 && savedVendor == currentVendor && savedProduct != 0 && savedProduct == currentProduct;
  return sameDevice ? kScoreVidPid : 0;
}

void assign_player_index(GameController& controller, int32_t port) {
  SDL_SetGamepadPlayerIndex(controller.m_controller, port);
  controller.m_playerIndex = port;
}

// SDL forgets the index for devices mapped after connect, so player_index() falls
// back to the cached copy; both have to move together or a port looks doubly taken.
int32_t effective_player_index(const GameController& controller) {
  const int32_t player = SDL_GetGamepadPlayerIndex(controller.m_controller);
  return player >= 0 ? player : controller.m_playerIndex;
}

void apply_port_preferences() noexcept {
  ensure_port_preferences_loaded();
  if (!std::any_of(g_portPreferences.begin(), g_portPreferences.end(),
                   [](const auto& preference) { return preference.state != PortPreferenceState::Unset; })) {
    return;
  }

  for (auto& [instance, controller] : g_GameControllers) {
    const int32_t player = effective_player_index(controller);
    if (player >= 0 && player < PAD_MAX_CONTROLLERS && g_portPreferences[player].state != PortPreferenceState::Unset) {
      // Keep SDL's default player assignment from taking explicitly configured ports
      assign_player_index(controller, -1);
    }
  }

  struct Candidate {
    int score;
    uint32_t port;
    Uint32 instance;
  };
  std::vector<Candidate> candidates;
  for (const auto& [instance, controller] : g_GameControllers) {
    const auto identity = controller_identity(controller);
    for (uint32_t port = 0; port < g_portPreferences.size(); ++port) {
      if (g_portPreferences[port].state != PortPreferenceState::Controller) {
        continue;
      }
      if (const int score = identity_score(g_portPreferences[port].identity, identity); score > 0) {
        candidates.push_back({score, port, instance});
      }
    }
  }
  // Best matches claim first so identical controllers each end up on their own port
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.port != b.port ? a.port < b.port : a.instance < b.instance;
  });

  std::array<bool, PAD_MAX_CONTROLLERS> portClaimed{};
  std::array<Uint32, PAD_MAX_CONTROLLERS> claimedInstances{};
  size_t claimedCount = 0;
  bool upgraded = false;
  for (const auto& candidate : candidates) {
    if (portClaimed[candidate.port] ||
        std::find(claimedInstances.begin(), claimedInstances.begin() + claimedCount, candidate.instance) !=
            claimedInstances.begin() + claimedCount) {
      continue;
    }
    portClaimed[candidate.port] = true;
    claimedInstances[claimedCount++] = candidate.instance;
    auto& controller = g_GameControllers[candidate.instance];
    assign_player_index(controller, static_cast<int32_t>(candidate.port));

    // Preferences saved before path and slot existed gain them on their first GUID match
    auto& saved = g_portPreferences[candidate.port].identity;
    if (candidate.score >= kScoreGuid && (saved.path.empty() || saved.slot < 0)) {
      const auto current = controller_identity(controller);
      if (saved.path.empty() && !current.path.empty()) {
        saved.path = current.path;
        upgraded = true;
      }
      if (saved.slot < 0 && current.slot >= 0) {
        saved.slot = current.slot;
        upgraded = true;
      }
    }
  }
  if (upgraded) {
    save_port_preferences();
  }
}

// SDL only hands out a player index when the device already had a gamepad mapping
// at connect time, so anything mapped later (the setup wizard) stays at -1.
void ensure_player_index(GameController& controller) noexcept {
  const int32_t player = SDL_GetGamepadPlayerIndex(controller.m_controller);
  if (player >= 0) {
    controller.m_playerIndex = player;
    return;
  }
  if (controller.m_playerIndex >= 0) {
    return;
  }
  ensure_port_preferences_loaded();
  const auto claim = [&](bool skipConfiguredPorts) {
    for (int32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
      if (skipConfiguredPorts && g_portPreferences[port].state != PortPreferenceState::Unset) {
        continue;
      }
      const bool taken = std::any_of(g_GameControllers.begin(), g_GameControllers.end(), [&](const auto& entry) {
        return entry.second.m_controller != controller.m_controller && effective_player_index(entry.second) == port;
      });
      if (!taken) {
        assign_player_index(controller, port);
        return true;
      }
    }
    return false;
  };
  // Explicitly configured ports are only used as a last resort so a hot-plugged
  // controller cannot steal the port its preferred device will claim.
  if (!claim(true)) {
    claim(false);
  }
}
} // namespace

GameController* get_controller_for_player(uint32_t player) noexcept {
  for (auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return &controller;
    }
  }

  return nullptr;
}

Sint32 get_instance_for_player(uint32_t player) noexcept {
  for (const auto& [which, controller] : g_GameControllers) {
    if (player_index(which) == player) {
      return which;
    }
  }

  return {};
}

SDL_JoystickID add_controller(SDL_JoystickID which) noexcept {
  if (g_GameControllers.contains(which)) {
    return which;
  }
  auto* ctrl = SDL_OpenGamepad(which);
  if (ctrl != nullptr) {
    GameController controller;
    controller.m_controller = ctrl;
    controller.m_index = which;
    controller.m_vid = SDL_GetGamepadVendor(ctrl);
    controller.m_pid = SDL_GetGamepadProduct(ctrl);
    if (controller.m_vid == 0x05ac /* USB_VENDOR_APPLE */ && controller.m_pid == 3) {
      // Ignore Apple TV remote
      SDL_CloseGamepad(ctrl);
      return -1;
    }
    controller.m_isGameCube = controller.m_vid == 0x057E && controller.m_pid == 0x0337;
    if (const char* path = SDL_GetGamepadPath(ctrl); path != nullptr) {
      controller.m_path = path;
    }
    if (const auto slot = g_connectSlots.find(which); slot != g_connectSlots.end()) {
      controller.m_slot = slot->second;
      g_connectSlots.erase(slot);
    } else {
      controller.m_slot = SDL_GetGamepadPlayerIndex(ctrl);
    }
    const char* serial = SDL_GetGamepadSerial(ctrl);
    controller.m_gameCubeUseOrdinaryStop = controller.m_isGameCube && serial && "GCP+"sv == serial;
    if (controller.m_isGameCube ||
        (SDL_GetGamepadType(ctrl) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && controller.m_pid == 0x2073)) {
      controller.m_deadZones.emulateTriggers = false;
    }
    const auto props = SDL_GetGamepadProperties(ctrl);
    controller.m_hasRumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, true);
    controller.m_hasRgbLed = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    SDL_JoystickID instance = SDL_GetJoystickID(SDL_GetGamepadJoystick(ctrl));
    g_GameControllers[instance] = controller;
    ensure_player_index(g_GameControllers[instance]);
    apply_port_preferences();
#if defined(SDL_PLATFORM_MACOS)
    // First-use convenience only: never override a saved assignment or None.
    if (g_portPreferences[0].state == PortPreferenceState::Unset) {
      bool hasOtherPortPreference = false;
      for (size_t port = 1; port < g_portPreferences.size(); ++port) {
        if (g_portPreferences[port].state == PortPreferenceState::Controller &&
            identity_score(g_portPreferences[port].identity, controller_identity(g_GameControllers[instance])) > 0) {
          hasOtherPortPreference = true;
          break;
        }
      }
      if (!hasOtherPortPreference) {
        const auto* p0 = get_controller_for_player(0);
        if (p0 == nullptr) {
          assign_player_index(g_GameControllers[instance], 0);
          persist_controller_for_player(0, &g_GameControllers[instance]);
        } else if (p0 == &g_GameControllers[instance]) {
          persist_controller_for_player(0, &g_GameControllers[instance]);
        }
      }
    }
#endif
    return instance;
  }

  return -1;
}

bool refresh_controller(SDL_JoystickID instance) noexcept {
  const auto it = g_GameControllers.find(instance);
  if (it == g_GameControllers.end()) {
    return false;
  }
  // The SDL mapping changed underneath us; drop the cached PAD bindings so they
  // are rebuilt from the new one.
  it->second.m_mappingLoaded = false;
  ensure_player_index(it->second);
  apply_port_preferences();
  return true;
}

void remove_controller(Uint32 instance) noexcept {
  g_connectSlots.erase(instance);
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_CloseGamepad(it->second.m_controller);
    g_GameControllers.erase(it);
    apply_port_preferences();
  }
}

bool is_gamecube(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_isGameCube;
  }
  return false;
}

int32_t player_index(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    const int player = SDL_GetGamepadPlayerIndex(it->second.m_controller);
    return player >= 0 ? player : it->second.m_playerIndex;
  }
  return -1;
}

void set_player_index(Uint32 instance, Sint32 index) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    SDL_SetGamepadPlayerIndex(it->second.m_controller, index);
    it->second.m_playerIndex = index;
  }
}

std::string controller_name(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    const auto* name = SDL_GetGamepadName(it->second.m_controller);
    if (name != nullptr) {
      return {name};
    }
  }
  return {};
}

bool controller_has_rumble(Uint32 instance) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    return it->second.m_hasRumble;
  }
  return false;
}

void controller_rumble(uint32_t instance, uint16_t low_freq_intensity, uint16_t high_freq_intensity,
                       uint16_t duration_ms) noexcept {
  if (auto it = g_GameControllers.find(instance); it != g_GameControllers.end()) {
    // GC Pocket+ has been observed continuing to vibrate after a hard stop;
    // an ordinary stop cleared it. With GAMECUBE_RUMBLE_BRAKE enabled, SDL
    // encodes (0, 1) as adapter command 0, whereas (0, 0) sends command 2.
    // Apply the workaround here so shutdown uses the same stop as PAD calls.
    if (it->second.m_gameCubeUseOrdinaryStop && low_freq_intensity == 0 && high_freq_intensity == 0) {
      high_freq_intensity = 1;
    }
    SDL_RumbleGamepad(it->second.m_controller, low_freq_intensity, high_freq_intensity, duration_ms);
  }
}

uint32_t controller_count() noexcept { return g_GameControllers.size(); }

void persist_controller_for_player(uint32_t player, const GameController* controller) noexcept {
  if (player >= PAD_MAX_CONTROLLERS) {
    return;
  }

  ensure_port_preferences_loaded();
  if (controller != nullptr) {
    g_portPreferences[player].state = PortPreferenceState::Controller;
    g_portPreferences[player].identity = controller_identity(*controller);
  } else {
    g_portPreferences[player].state = PortPreferenceState::None;
    g_portPreferences[player].identity = {};
  }
  save_port_preferences();
}

void initialize() noexcept {
  /* Make sure we initialize everything input related now, this will automatically add all of the connected controllers
   * as expected */
  ASSERT(SDL_Init(SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD), "Failed to initialize SDL subsystems: {}",
         SDL_GetError());

  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      g_connectSlots[ids[i]] = SDL_GetGamepadPlayerIndexForID(ids[i]);
    }
    SDL_free(ids);
  }
}

struct MouseScrollStatus {
  float scrollX;
  float scrollY;
};

static MouseScrollStatus g_MouseStatus;

void set_mouse_scroll(const float scrollX, const float scrollY) noexcept {
  g_MouseStatus.scrollX = scrollX;
  g_MouseStatus.scrollY = scrollY;
}

void get_mouse_scroll(float* scrollX, float* scrollY) noexcept {
  *scrollX = g_MouseStatus.scrollX;
  *scrollY = g_MouseStatus.scrollY;
}

void shutdown() noexcept {
  // Upon shutdown we want to ensure all controllers are in a default state, so force all rumble supporting controllers
  // to shut off their rumble motors.
  for (const auto& controller : g_GameControllers) {
    if (!controller.second.m_hasRumble) {
      continue;
    }
    controller_rumble(controller.first, 0, 0, 0);
  }
}
} // namespace aurora::input
