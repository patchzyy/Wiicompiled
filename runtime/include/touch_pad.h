#pragma once

#include <array>
#include <cstdint>

struct PADStatus;
namespace WiiRemoteInput { struct KpadSample; }

// On-screen touch controls. Read() fills a status array the caller merges over
// aurora's PADRead result.
//
// Polls SDL's finger list directly rather than using ImGui widgets: steering,
// accelerating and firing happen at once, and ImGui's SDL backend collapses
// touch to a single emulated mouse.
namespace TouchPad {

// True when the host has a touch screen and no physical pad is driving port 0.
bool IsActive();

// Port 0 only. Returns false when touch is not driving input, leaving the
// caller's existing statuses untouched.
bool Read(std::array<PADStatus, 4>& statuses);

// Builds port 0's virtual Wii Remote state while iOS motion controls are on.
// The caller sends it through KPAD instead of the GameCube PAD path.
bool ReadMotionRemote(WiiRemoteInput::KpadSample& sample);
bool MotionRemoteActive();
bool MotionGameCubeActive();
void RecenterMotionRemote();
void ResetMotionRemote();

// Draws the control overlay. Must be called inside a live ImGui frame.
void Draw();

// True when auto-accelerate is enabled in user settings (default true).
bool AutoAccelerateEnabled();

// Enables or disables auto-accelerate and persists the setting.
// Disabling immediately cancels any active gas lock while preserving genuine physical hold.
void SetAutoAccelerate(bool enabled);

// True when gas (button A) is currently latched/locked.
bool IsGasLocked();

// Resets internal touch state and cancels any active latch.
void Reset();

// Core state machine updater, exposed for deterministic testing.
void UpdateAutoAccelerate(bool aPhysicalDown, uint64_t nowMs, bool autoAccelerateEnabled);

} // namespace TouchPad
