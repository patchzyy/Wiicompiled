#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_macros.h"
#include "wup028_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace {

// Read on the guest thread every time the game asks for rumble, written from
// the overlay thread when the setting changes, so it cannot live in the config
// struct like settings that are only ever read at startup.
std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R (a bumper, or a GameCube trigger's click
// switch) sets the button bit but leaves the analog value at rest, because the
// button has no travel of its own. On real hardware the click only engages at
// full depression, so report the trigger as fully pulled; otherwise code that
// reads the analog value sees a press worth nothing.
void FillTriggersHeldByButtons(std::array<PADStatus, PAD_CHANMAX>& statuses) {
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }

        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };

        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

// Called by the F10 settings bar. Declared where it is used rather than in a
// header, since this is the only caller.
extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

extern "C" uint32_t PAD__Init_HLE()
{
    Wup028Adapter::Initialize();
    return PADInit() ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF2F0, PAD__Init_HLE, uint32_t, (), ());

extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    std::array<PADStatus, PAD_CHANMAX> statuses{};
    std::array<PADStatus, PAD_CHANMAX> adapterStatuses{};
    uint32_t rumbleMask = PADRead(statuses.data());
    if (Wup028Adapter::Read(adapterStatuses) && !PADIsInputBlocked()) {
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            if (adapterStatuses[port].err == PAD_ERR_NONE) {
                statuses[port] = adapterStatuses[port];
                rumbleMask |= PAD_CHAN0_BIT >> port;
            }
        }
    }

    FillTriggersHeldByButtons(statuses);

    // Last, so a macro adds to whatever the player is physically holding rather
    // than being overwritten by a later input source. This is also the game's
    // own once-per-frame input poll, which is the cadence macro patterns use.
    InputMacros::Apply(statuses);

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
PPC_NATIVE_OVERRIDE(801AF44C, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF0DC, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF1E4, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    // Downgrade to a stop rather than dropping the call: the game tracks motor
    // state and expects its transitions honoured, and a stop also kills
    // anything still running from before the setting was turned off.
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    if (!Wup028Adapter::SetRumble(static_cast<uint32_t>(chan), command == PAD_MOTOR_RUMBLE)) {
        PADControlMotor(chan, command);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
