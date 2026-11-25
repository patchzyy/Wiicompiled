#pragma once

#include <aurora/event.h>

#include <cstdint>

// Outside a Steam launch, Steam applies its desktop controller layout, which binds
// controller buttons to mouse and keyboard actions. While the game window has focus,
// a running Steam client is asked to apply the given app's game layout instead.
namespace SteamInputLayout {

void Initialize(bool enabled, uint32_t appId);
void HandleEvents(const AuroraEvent* events);
// Restores the desktop layout. Idempotent; safe to call from any exit path.
void Release() noexcept;

} // namespace SteamInputLayout
