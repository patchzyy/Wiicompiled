#pragma once

#include <SDL3/SDL_events.h>

// Press-to-bind setup for joysticks SDL either doesn't recognize as gamepads or
// recognizes with a mapping that lacks the analog stick (e.g. raphnet adapters).
// The wizard produces a standard SDL gamepad mapping, applies it live, and
// persists it to gamecontrollerdb.txt in the user data directory.
namespace controller_mapping_wizard {

void LoadPersistedMappings();
void HandleSdlEvent(const SDL_Event& event);

// Lists devices that need setup inside the controller settings menu.
void DrawSetupList();
// Draws the wizard window when active; call once per overlay frame.
void Draw();

bool IsActive();

} // namespace controller_mapping_wizard
