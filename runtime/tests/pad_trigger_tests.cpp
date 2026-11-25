#include "pad_trigger_support/hle_stubs.h"
#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

#include <array>
#include <vector>

namespace {
struct TestPort {
    bool assigned = true;
    bool connected = true;
    std::vector<PADButtonMapping> primary;
    std::vector<PADButtonMapping> alternate;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> held{};
};
std::array<TestPort, PAD_CHANMAX> g_ports;
bool g_blocked = false;

bool TestGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) {
    const auto port = reinterpret_cast<uintptr_t>(gamepad) - 1;
    return g_ports.at(port).held.at(button);
}
}

#define SDL_GetGamepadButton TestGamepadButton
#include "../src/hle/input/pad.cpp"
#undef SDL_GetGamepadButton

namespace InputBindings {
bool InputBlocked() noexcept { return g_blocked; }
void Apply(PADStatus*) noexcept {}
}

namespace MotionTrick {
void Apply(PADStatus*) noexcept {}
}

BOOL PADInit() { return TRUE; }
BOOL PADReset(u32) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }
void PADControlMotor(u32, u32) {}
u32 PADRead(PADStatus*) { return 0; }
s32 PADGetIndexForPort(u32 port) { return g_ports.at(port).assigned ? static_cast<s32>(port) : -1; }
SDL_Gamepad* PADGetSDLGamepadForIndex(u32 index) {
    return g_ports.at(index).connected ? reinterpret_cast<SDL_Gamepad*>(static_cast<uintptr_t>(index) + 1) : nullptr;
}
PADButtonMapping* PADGetButtonMappings(u32 port, u32* count) {
    auto& mappings = g_ports.at(port).primary;
    *count = static_cast<u32>(mappings.size());
    return mappings.empty() ? nullptr : mappings.data();
}
PADButtonMapping* PADGetAltButtonMappings(u32 port, u32* count) {
    auto& mappings = g_ports.at(port).alternate;
    *count = static_cast<u32>(mappings.size());
    return mappings.empty() ? nullptr : mappings.data();
}
uint8_t* Memory::GetPointer(uint32_t, size_t) {
    static std::array<uint8_t, PAD_CHANMAX * PadStatusContract::kGuestStatusSize> bytes{};
    return bytes.data();
}

namespace {
int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::array<PADStatus, PAD_CHANMAX> ReadTriggers() {
    std::array<PADStatus, PAD_CHANMAX> statuses{};
    FillTriggersHeldByButtons(statuses.data());
    return statuses;
}
}

int main() {
    g_ports[0].primary = {{SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
                          {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_L}};
    g_ports[0].alternate = {{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_R}};
    g_ports[0].held[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;
    g_ports[0].held[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = true;
    auto status = ReadTriggers();
    Check(status[0].triggerLeft == 255, "primary mapping count reaches scanner");
    Check(status[0].triggerRight == 255, "alternate mapping count reaches scanner independently");

    g_ports[0].held[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = false;
    status = ReadTriggers();
    Check(status[0].triggerLeft == 0 && status[0].triggerRight == 255, "released buttons do not pull triggers");
    g_ports[0].held[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;

    g_ports[0].primary.clear();
    g_ports[0].alternate = {{SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
                            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_L},
                            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_R}};
    status = ReadTriggers();
    Check(status[0].triggerLeft == 255 && status[0].triggerRight == 255,
          "nonempty alternate mappings work with an empty primary set");

    g_ports[0].alternate = {{PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_L},
                            {SDL_GAMEPAD_BUTTON_COUNT, PAD_TRIGGER_R},
                            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BUTTON_A}};
    status = ReadTriggers();
    Check(status[0].triggerLeft == 0 && status[0].triggerRight == 0,
          "invalid native buttons and unrelated bindings do not pull triggers");

    for (auto& port : g_ports) {
        port.primary = {{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_L}};
        port.alternate.clear();
        port.held[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;
    }
    g_ports[1].assigned = false;
    g_ports[2].connected = false;
    status = ReadTriggers();
    Check(status[0].triggerLeft == 255 && status[3].triggerLeft == 255, "connected ports are scanned");
    Check(status[1].triggerLeft == 0 && status[2].triggerLeft == 0, "unassigned and disconnected ports are skipped");

    status = {};
    status[0].err = PAD_ERR_NO_CONTROLLER;
    status[0].triggerLeft = 17;
    FillTriggersHeldByButtons(status.data());
    Check(status[0].triggerLeft == 17, "error statuses retain their original trigger values");
    g_blocked = true;
    status = ReadTriggers();
    Check(status[0].triggerLeft == 0 && status[3].triggerLeft == 0, "blocked input is withheld");
    return g_failures == 0 ? 0 : 1;
}
