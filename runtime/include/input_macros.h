#pragma once

// Frame-synced input macros.
//
// A macro watches one physical button on a host pad and, while it is held,
// stamps a set of GameCube buttons into the status the game is about to read,
// alternating pressed and released on a frame pattern. Holding LB to send D-pad
// Up as fast as the game can register it is the motivating case.
//
// The pattern is counted in guest input polls, which the game performs once per
// game frame. That is deliberately not wall-clock time and not presented frames,
// so the pattern behaves identically with frame interpolation on or off, and
// every press the pattern emits is separated by a release the game can see.
//
// Why a press cannot land on *every* frame: the game recognises a new press by
// seeing the button go from released to pressed. A button reported as held on
// consecutive frames is one long press, not many. The tightest real spam is
// therefore one frame pressed, one frame released, which is what hold=1,
// release=1 produces: 30 distinct presses per second at 60 fps.

#include <array>
#include <cstddef>
#include <cstdint>

#include <dolphin/pad.h>

namespace InputMacros {

inline constexpr size_t kSlotCount = 4;

// Lowest and highest frame counts a slot may use for either half of its pattern.
inline constexpr uint32_t kMinFrames = 1;
inline constexpr uint32_t kMaxFrames = 60;

struct Slot {
    bool enabled = false;
    uint32_t port = 0;                                // zero-based game port
    uint32_t trigger = PAD_NATIVE_BUTTON_INVALID;     // physical SDL button
    uint16_t output = 0;                              // OR of PAD_* button bits
    uint32_t holdFrames = 1;
    uint32_t releaseFrames = 1;

    bool IsRunnable() const {
        return enabled && output != 0 && trigger != PAD_NATIVE_BUTTON_INVALID && port < PAD_CHANMAX;
    }
};

// Rebuild the engine's view of the [controller] macro_N_* keys in Config.toml.
// Safe to call at any time; running patterns are reset.
void Reload() noexcept;

// Read one slot. Returns a disabled slot for an out-of-range index.
Slot GetSlot(size_t index) noexcept;

// Replace one slot in the running engine without touching Config.toml. Use this
// for controls that change continuously, such as a slider being dragged; each
// persist rewrites the whole file, which is far too much for a per-frame edit.
void ApplySlot(size_t index, const Slot& slot) noexcept;

// Write the slot the engine is currently running to Config.toml.
void PersistSlot(size_t index) noexcept;

// Replace one slot and persist it. The right call for discrete edits.
void SetSlot(size_t index, const Slot& slot) noexcept;

// Mix macro output into a freshly read status set. Call exactly once per guest
// PADRead, after every other input source has been merged in, so a macro can add
// to what the player is physically holding rather than being overwritten by it.
void Apply(std::array<PADStatus, PAD_CHANMAX>& statuses) noexcept;

// True while the slot is emitting a press. Drives the activity dot in the F10
// bar so a misconfigured macro is obvious without leaving the menu.
bool SlotIsFiring(size_t index) noexcept;

} // namespace InputMacros
