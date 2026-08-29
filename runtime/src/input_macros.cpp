#include "input_macros.h"

#include "controller_button_names.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <SDL3/SDL_gamepad.h>

namespace InputMacros {
namespace {

// Per-slot pattern state. Lives beside the configuration but is never persisted.
struct RunState {
    bool firing = false;      // emitting a press this frame
    bool triggerHeld = false; // trigger state as of the last Apply
    uint32_t framesInPhase = 0;
    bool waitingForRelease = false; // set while the F10 bar owns input
};

std::mutex g_mutex;
std::array<Slot, kSlotCount> g_slots{};
std::array<RunState, kSlotCount> g_runStates{};

// Read the physical button straight off the pad, bypassing the port's button
// mapping. PADGetSDLGamepadForIndex and PADGetIndexForPort are both public, so
// this needs no change to the pad library itself.
bool NativeButtonHeld(uint32_t port, uint32_t nativeButton) {
    if (nativeButton == PAD_NATIVE_BUTTON_INVALID || nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    const s32 index = PADGetIndexForPort(port);
    if (index < 0) {
        return false;
    }
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
    if (gamepad == nullptr) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

uint32_t ClampFrames(uint32_t frames) {
    return std::clamp(frames, kMinFrames, kMaxFrames);
}

// Turn one Config.toml slot into something the input path can use. Names that do
// not resolve leave the slot disabled instead of half-configured, so a typo is
// inert rather than surprising.
Slot SlotFromConfig(size_t index) {
    const MacroSetting& setting = RuntimeConfigFile::Macro(index);
    Slot slot;
    slot.enabled = setting.enabled;
    slot.port = setting.port < PAD_CHANMAX ? setting.port : 0;
    slot.holdFrames = ClampFrames(setting.holdFrames);
    slot.releaseFrames = ClampFrames(setting.releaseFrames);
    slot.output = ControllerNames::GameCubeMaskFromKeys(setting.output);

    if (!setting.trigger.empty()) {
        if (const auto* native = ControllerNames::FindNativeButton(setting.trigger)) {
            slot.trigger = native->nativeButton;
        } else {
            RT_LOG(RT_TAG_CONFIG) << "macro " << (index + 1) << ": unknown trigger button '"
                                  << setting.trigger << "'" << std::endl;
        }
    }
    if (slot.enabled && !setting.output.empty() && slot.output == 0) {
        RT_LOG(RT_TAG_CONFIG) << "macro " << (index + 1) << ": no usable output button in '"
                              << setting.output << "'" << std::endl;
    }
    return slot;
}

void PersistLocked(size_t index, const Slot& slot) {
    MacroSetting setting;
    setting.enabled = slot.enabled;
    setting.port = slot.port;
    setting.trigger = ControllerNames::NativeButtonForValue(slot.trigger).configName;
    setting.output = ControllerNames::GameCubeKeysFromMask(slot.output);
    setting.holdFrames = slot.holdFrames;
    setting.releaseFrames = slot.releaseFrames;
    RuntimeConfigFile::SetMacro(index, setting);
}

// Move a slot one step along its pattern. One step per guest PADRead, which is
// the game's own once-per-frame input poll. Counting the game's polls rather
// than video frames guarantees the alternation the game actually sees: every
// poll flips the state, so a press is always followed by a release even if the
// renderer stutters or runs ahead.
void StepPhase(const Slot& slot, RunState& state) {
    ++state.framesInPhase;
    const uint32_t phaseLength = state.firing ? slot.holdFrames : slot.releaseFrames;
    if (state.framesInPhase >= phaseLength) {
        state.firing = !state.firing;
        state.framesInPhase = 0;
    }
}

} // namespace

void Reload() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t index = 0; index < kSlotCount; ++index) {
        g_slots[index] = SlotFromConfig(index);
        g_runStates[index] = RunState{};
    }
}

Slot GetSlot(size_t index) noexcept {
    if (index >= kSlotCount) {
        return Slot{};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_slots[index];
}

void ApplySlot(size_t index, const Slot& slot) noexcept {
    if (index >= kSlotCount) {
        return;
    }
    Slot sanitized = slot;
    sanitized.holdFrames = ClampFrames(sanitized.holdFrames);
    sanitized.releaseFrames = ClampFrames(sanitized.releaseFrames);
    if (sanitized.port >= PAD_CHANMAX) {
        sanitized.port = 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_slots[index] = sanitized;
    // Restart the pattern rather than reinterpreting the old phase against new
    // frame counts, which would let a slot come back mid-press.
    g_runStates[index] = RunState{};
}

void PersistSlot(size_t index) noexcept {
    if (index >= kSlotCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    PersistLocked(index, g_slots[index]);
}

void SetSlot(size_t index, const Slot& slot) noexcept {
    ApplySlot(index, slot);
    PersistSlot(index);
}

void Apply(std::array<PADStatus, PAD_CHANMAX>& statuses) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);

    // The F10 bar takes input away from the game; a macro must not sneak past
    // that, and it must not resume mid-pattern the moment the bar closes.
    const bool inputBlocked = PADIsInputBlocked();

    for (size_t index = 0; index < kSlotCount; ++index) {
        const Slot& slot = g_slots[index];
        RunState& state = g_runStates[index];

        if (!slot.IsRunnable() || statuses[slot.port].err != PAD_ERR_NONE) {
            state = RunState{};
            continue;
        }

        if (inputBlocked) {
            state = RunState{};
            state.waitingForRelease = true;
            continue;
        }

        const bool held = NativeButtonHeld(slot.port, slot.trigger);

        // Require the trigger to come back up before a macro re-arms, matching
        // how PADRead suppresses buttons that were already held when the bar
        // closed. Without this, letting go of the mouse re-starts the macro.
        if (state.waitingForRelease) {
            if (held) {
                continue;
            }
            state.waitingForRelease = false;
        }

        if (!held) {
            state = RunState{};
            continue;
        }

        if (!state.triggerHeld) {
            // Fresh press: start on a pressed frame so the first input lands
            // immediately rather than after a release phase.
            state.firing = true;
            state.framesInPhase = 0;
            state.triggerHeld = true;
        } else {
            StepPhase(slot, state);
        }

        if (state.firing) {
            statuses[slot.port].button |= slot.output;
        }
    }
}

bool SlotIsFiring(size_t index) noexcept {
    if (index >= kSlotCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_runStates[index].firing && g_runStates[index].triggerHeld;
}

} // namespace InputMacros
