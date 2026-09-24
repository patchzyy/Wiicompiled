#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Maps standard Wii setting.txt AREA string to SC area index (RVL SDK SCGetProductArea).
uint32_t LookupProductArea(const std::string& area) {
    static const std::pair<const char*, uint32_t> kAreas[] = {
        {"JPN", 0}, {"USA", 1}, {"EUR", 2}, {"AUS", 3}, {"BRA", 4},
        {"TWN", 5}, {"ROC", 5}, {"KOR", 6}, {"HKG", 7}, {"ASI", 8},
        {"LTN", 9}, {"SAF", 10},
    };
    for (const auto& [name, index] : kAreas) {
        if (area == name) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

// Maps standard Wii setting.txt GAME string to SC game region index (RVL SDK SCGetProductGameRegion).
uint32_t LookupProductGameRegion(const std::string& gameRegion) {
    static const std::pair<const char*, uint32_t> kGameRegions[] = {
        {"JP", 0}, {"US", 1}, {"EU", 2}, {"KR", 3},
    };
    for (const auto& [name, index] : kGameRegions) {
        if (gameRegion == name) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace


// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801B0220 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801B0220, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801B1BE4 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return (RuntimeConfigFile::WidescreenEnabled(true) || MkwForceAspect169Requested()) ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801B1BE4, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801B1CAC -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801B1CAC, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    const std::string& area = RuntimeConsoleIdentity::Current().area;
    return !area.empty() ? LookupProductArea(area) : MKW_REGION_SC_AREA;
}

PPC_NATIVE_OVERRIDE(801B23A0, SCGetProductArea_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original SC storage for the six-byte CODE value (PAL identity 0x803869E0).
    constexpr uint32_t kProductCodeAddress = MKW_GADDR(803869E0);
    const std::string& customCode = RuntimeConsoleIdentity::Current().productCode;
    const char* code = !customCode.empty() ? customCode.c_str() : MKW_REGION_SC_PRODUCT_CODE;
    const size_t size = std::strlen(code) + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size), code, size);
    return kProductCodeAddress;
}

PPC_NATIVE_OVERRIDE(801B2424, SCGetProductCode_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}

PPC_NATIVE_OVERRIDE(801B2460, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    const std::string& gameRegion = RuntimeConsoleIdentity::Current().gameRegion;
    return !gameRegion.empty() ? LookupProductGameRegion(gameRegion) : MKW_REGION_SC_GAME_REGION;
}

PPC_NATIVE_OVERRIDE(801B24C8, SCGetProductGameRegion_HLE, uint32_t, (), ());

// These stubs make the game think all titles are installed; otherwise it checks title ID
// 0x00010004524d4350 ("RMCP", Mario Kart Wii PAL) and reports error code 5.

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (stubbed as installed)\n",
            titleIdHi, titleIdLo);
    return 1; // Always report installed
}

PPC_NATIVE_OVERRIDE(801AE4A0, OS__IsTitleInstalled, uint32_t, (uint32_t titleIdHi, uint32_t titleIdLo), (titleIdHi, titleIdLo));

// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    RT_LOGF(RT_TAG_HLE, "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> success (stubbed)\n",
            requiredBlocks, titleIdHi, titleIdLo);
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, 0x3); // has data + has update = fully installed
    }
    return 0; // Success
}

PPC_NATIVE_OVERRIDE(801AD1D4, OS__CheckInstall, uint32_t, (uint32_t requiredBlocks, uint32_t titleIdHi, uint32_t titleIdLo, uint32_t outFlagsPtr), (requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr));
