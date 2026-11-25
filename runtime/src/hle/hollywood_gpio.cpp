#include "hle/hollywood_gpio.h"

#include "runtime_log.h"

#include <atomic>

namespace HollywoodGpio {

constexpr uint32_t kSensorBarPower = 0x100;

std::atomic<uint32_t> g_gpioBOut{0};

} // namespace HollywoodGpio

bool HollywoodGpio_Read32(uint32_t address, uint32_t& value) {
    if (address != kHollywoodGpioBOut) {
        return false;
    }
    value = HollywoodGpio::g_gpioBOut.load(std::memory_order_relaxed);
    return true;
}

bool HollywoodGpio_Write32(uint32_t address, uint32_t value) {
    if (address != kHollywoodGpioBOut) {
        return false;
    }
    const uint32_t previous = HollywoodGpio::g_gpioBOut.exchange(value, std::memory_order_relaxed);
    if ((previous ^ value) & HollywoodGpio::kSensorBarPower) {
        RT_LOG(RT_TAG_HLE) << "Sensor bar power " << ((value & HollywoodGpio::kSensorBarPower) ? "on" : "off") << std::endl;
    }
    return true;
}
