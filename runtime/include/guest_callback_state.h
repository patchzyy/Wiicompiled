#pragma once

namespace GuestCallbacks {
// Saved and restored with the executing host fiber.
inline thread_local bool nandDraining = false;
}
