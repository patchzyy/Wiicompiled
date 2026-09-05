#pragma once

#include <array>
#include <cstdint>

struct PADStatus;

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

// Draws the control overlay. Must be called inside a live ImGui frame.
void Draw();


} // namespace TouchPad
