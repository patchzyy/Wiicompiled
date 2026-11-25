#pragma once

#include <aurora/event.h>

// Lets the keyboard type into Retro Rewind's friend code entry page.
namespace FriendCodeKeyboard {
void HandleEvents(const AuroraEvent* events) noexcept;
} // namespace FriendCodeKeyboard
