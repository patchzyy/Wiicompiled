#pragma once

#include <cstdint>

// HW_GPIOB_OUT, the GPIO outputs the PPC owns. WPAD sets bit 8 to power the
// sensor bar; there is nothing behind it on a PC, so the register only latches.
inline constexpr uint32_t kHollywoodGpioBOut = 0xCD0000C0u;

bool HollywoodGpio_Read32(uint32_t address, uint32_t& value);
bool HollywoodGpio_Write32(uint32_t address, uint32_t value);
