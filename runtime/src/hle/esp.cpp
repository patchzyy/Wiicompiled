// ESP HLE Stubs
// ESP functions communicate with /dev/es to get title information.
// We stub these to return Mario Kart Wii's title ID directly.

#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>

static constexpr uint32_t MKW_TITLE_ID_HI = 0x00010004;
static constexpr uint32_t MKW_TITLE_ID_LO = MKW_REGION_GAME_CODE; // game code fallback

static bool IsValidEspTitleCode(uint32_t code) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char ch = static_cast<char>((code >> shift) & 0xffu);
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

static uint32_t CurrentTitleIdLo() {
    if (Memory::Contains(0x80000000u, 4u)) {
        const uint32_t code = Memory::Read32(0x80000000u);
        if (IsValidEspTitleCode(code)) {
            return code;
        }
    }

    return MKW_TITLE_ID_LO;
}

// The guest asked for its title ID or data directory through a pointer we cannot
// honour. Nothing downstream explains why the save-data path then fails, so say
// so - once per site, because the caller may retry every frame.
static void ReportEspArgError(bool& logged, const char* who, const char* what)
{
    if (logged) {
        return;
    }
    logged = true;
    RT_LOGF(RT_TAG_HLE, "%s: %s\n", who, what);
    std::fflush(stderr);
}

// 0x801671d0 -> ESP_InitLib
// Opens /dev/es. We just pretend it succeeded.
extern "C" int32_t ESP_InitLib_stub(void)
{
    return 0;
}
PPC_NATIVE_OVERRIDE(801671d0, ESP_InitLib_stub, int32_t, (void), ());

// 0x80167224 -> ESP_CloseLib  
// Closes /dev/es. No-op for us.
extern "C" int32_t ESP_CloseLib_stub(void)
{
    return 0;
}
PPC_NATIVE_OVERRIDE(80167224, ESP_CloseLib_stub, int32_t, (void), ());

// 0x8016799c -> ESP_GetTitleId
// Gets the 64-bit title ID. param_1 is pointer to 8 bytes.
// Decompiled signature: int ESP_GetTitleId(undefined4 *param_1)
extern "C" int32_t ESP_GetTitleId_stub(uint32_t outPtr)
{
    if (outPtr == 0) {
        static bool logged = false;
        ReportEspArgError(logged, "ESP_GetTitleId", "null output pointer, returning error");
        return -0x3f9; // Same error as original for null
    }

    // Write title ID (big-endian)
    Memory::Write32(outPtr, MKW_TITLE_ID_HI);
    Memory::Write32(outPtr + 4, CurrentTitleIdLo());
    return 0;
}
PPC_NATIVE_OVERRIDE(8016799c, ESP_GetTitleId_stub, int32_t, (uint32_t outPtr), (outPtr));

// 0x80167904 -> ESP_GetDataDir(titleHi, titleLo, outPathPtr)
// Gets the NAND data directory path for a title.
extern "C" int32_t ESP_GetDataDir_stub(uint32_t titleHi, uint32_t titleLo, uint32_t outPathPtr)
{
    if (outPathPtr == 0) {
        static bool loggedNull = false;
        ReportEspArgError(loggedNull, "ESP_GetDataDir", "null output pointer, returning error");
        return -0x3f9;
    }

    // Check alignment (original requires 32-byte alignment)
    if ((outPathPtr & 0x1f) != 0) {
        static bool loggedUnaligned = false;
        ReportEspArgError(loggedUnaligned, "ESP_GetDataDir",
                          "output pointer is not 32-byte aligned, returning error");
        return -0x3f9;
    }


    // Build the path: /title/XXXXXXXX/YYYYYYYY/data
    // This is what the game expects for its save data location
    char path[64];
    std::sprintf(path, "/title/%08x/%08x/data", titleHi, titleLo);
    
    // Copy path to game memory
    const char* src = path;
    uint32_t dst = outPathPtr;
    while (*src) {
        Memory::Write8(dst++, static_cast<uint8_t>(*src++));
    }
    Memory::Write8(dst, 0); // null terminator
    return 0;
}
PPC_NATIVE_OVERRIDE(80167904, ESP_GetDataDir_stub, int32_t, (uint32_t titleHi, uint32_t titleLo, uint32_t outPathPtr), (titleHi, titleLo, outPathPtr));
