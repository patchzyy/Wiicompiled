#include "boot_race.h"

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr std::string_view kOption = "--race";
constexpr uint32_t kLuigiCircuit = 0x08;
constexpr uint32_t kLastRaceCourse = 0x1F;

// Racedata (menu scenario, copied into the race scenario by Racedata::InitRace)
constexpr uint32_t kMenuScenarioOffset = 0xC10;
constexpr uint32_t kPlayersOffset = 0x8;
constexpr uint32_t kPlayerSize = 0xF0;
constexpr uint32_t kPlayerKartOffset = 0x8;
constexpr uint32_t kPlayerCharacterOffset = 0xC;
constexpr uint32_t kPlayerTypeOffset = 0x10;
constexpr uint32_t kPlayerControllerChannelOffset = 0x6;
constexpr uint32_t kPlayerCount = 12;
constexpr uint32_t kSettingsOffset = 0xB48;

constexpr uint32_t kSectionPadOffset = 0x34;
constexpr uint32_t kFirstGcnControllerId = 0x124;
constexpr uint32_t kSectionP1Vs = 0x20;

enum : uint32_t { kPlayerRealLocal = 0, kPlayerCpu = 1 };
enum : uint32_t { kGameModeVs = 1, kCc150 = 2, kCpuHard = 2, kItemsBalanced = 0 };

struct Racer {
    uint32_t character;
    uint32_t kart;
};

// Standard kart matching each character's weight class.
constexpr std::array<Racer, 12> kRacers{{
    {0x00, 0x01}, // Mario
    {0x07, 0x01}, // Luigi
    {0x10, 0x01}, // Peach
    {0x0A, 0x01}, // Yoshi
    {0x0F, 0x01}, // Daisy
    {0x08, 0x00}, // Toad
    {0x06, 0x00}, // Baby Mario
    {0x0E, 0x00}, // Koopa Troopa
    {0x03, 0x02}, // Bowser
    {0x09, 0x02}, // Donkey Kong
    {0x0B, 0x02}, // Wario
    {0x16, 0x02}, // Funky Kong
}};

std::optional<uint32_t> g_course;
bool g_cpuDriven = false;

uint32_t ParseCourse(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    uint32_t course = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), course, base);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || course > kLastRaceCourse) {
        throw std::invalid_argument("--race expects a race course id from 0 to 0x1F");
    }
    return course;
}

void SetUpMenuScenario(uint32_t course) {
    const uint32_t racedata = Memory::Read32(MKW_GADDR(809BD728));
    if (racedata == 0) {
        throw std::runtime_error("--race: Racedata does not exist yet at SectionMgr::Init");
    }
    const uint32_t scenario = racedata + kMenuScenarioOffset;
    for (uint32_t i = 0; i < kRacers.size(); ++i) {
        const uint32_t player = scenario + kPlayersOffset + i * kPlayerSize;
        Memory::Write32(player + kPlayerKartOffset, kRacers[i].kart);
        Memory::Write32(player + kPlayerCharacterOffset, kRacers[i].character);
        Memory::Write32(player + kPlayerTypeOffset, i == 0 ? kPlayerRealLocal : kPlayerCpu);
    }
    const uint32_t settings = scenario + kSettingsOffset;
    Memory::Write32(settings + 0x00, course);
    Memory::Write32(settings + 0x04, kCc150);
    Memory::Write32(settings + 0x08, kGameModeVs);
    Memory::Write32(settings + 0x0C, 0);
    Memory::Write32(settings + 0x14, kCpuHard);
    Memory::Write32(settings + 0x18, kItemsBalanced);
    Memory::Write8(settings + 0x24, 0);
    Memory::Write8(settings + 0x25, 3);
    Memory::Write32(settings + 0x28, 0);
}

} // namespace

namespace BootRace {

bool ParseArgument(std::string_view arg) {
    if (arg == "--race-cpu") {
        g_cpuDriven = true;
        if (!g_course) {
            g_course = kLuigiCircuit;
        }
        return true;
    }
    if (!arg.starts_with(kOption)) {
        return false;
    }
    const std::string_view rest = arg.substr(kOption.size());
    if (rest.empty()) {
        g_course = kLuigiCircuit;
    } else if (rest.front() == '=') {
        g_course = ParseCourse(rest.substr(1));
    } else {
        return false;
    }
    return true;
}

bool Requested() {
    return g_course.has_value();
}

} // namespace BootRace

// ArgumentParser::parse, picks the boot section in SectionMgr::Init.
extern "C" void ArgumentParser_Parse_805b9344(CpuContext* ctx) {
    static bool consumed = false;
    if (!g_course || consumed) {
        MKW_GUEST_FUNC(805B9344)(ctx);
        return;
    }
    consumed = true;
    SetUpMenuScenario(*g_course);

    if (!g_cpuDriven) {
        const uint32_t sectionMgr = Memory::Read32(MKW_GADDR(809C1E38));
        if (sectionMgr == 0) {
            throw std::runtime_error("--race: SectionMgr instance is not set at SectionMgr::Init");
        }
        ctx->gpr[3] = sectionMgr + kSectionPadOffset;
        ctx->gpr[4] = 0;
        ctx->gpr[5] = kFirstGcnControllerId;
        MKW_GUEST_FUNC(8061B490)(ctx);
    }

    RT_LOGF("boot-race", "booting into 1P VS on course 0x%02X%s\n", *g_course, g_cpuDriven ? " (CPU driven)" : "");
    ctx->gpr[3] = kSectionP1Vs;
}
REGISTER_NATIVE_FUNCTION_AS(0x805B9344, ArgumentParser_Parse_805b9344, "ArgumentParser_Parse_805b9344");

// --race-cpu hands the local racer to the CPU driver while it keeps its camera and HUD.

// RacedataScenario::InitScreens; local racers keep no real controller channel.
extern "C" void RacedataScenario_InitScreens_8052f4e8(CpuContext* ctx) {
    if (!g_cpuDriven) {
        MKW_GUEST_NEXT(8052F4E8)(ctx);
        return;
    }
    const uint32_t players = ctx->gpr[3] + kPlayersOffset;
    std::array<uint8_t, kPlayerCount> channels{};
    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        channels[i] = Memory::Read8(players + i * kPlayerSize + kPlayerControllerChannelOffset);
    }
    MKW_GUEST_NEXT(8052F4E8)(ctx);
    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        const uint32_t player = players + i * kPlayerSize;
        if (Memory::Read32(player + kPlayerTypeOffset) == kPlayerRealLocal) {
            Memory::Write8(player + kPlayerControllerChannelOffset, channels[i]);
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8052F4E8, RacedataScenario_InitScreens_8052f4e8, "RacedataScenario_InitScreens_8052f4e8");

// KartAIController::IsCPU
extern "C" void KartAIController_IsCPU_8072624c(CpuContext* ctx) {
    MKW_GUEST_FUNC(8072624C)(ctx);
    if (g_cpuDriven) {
        ctx->gpr[3] = 1;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8072624C, KartAIController_IsCPU_8072624c, "KartAIController_IsCPU_8072624c");

// Kart::Link::IsCPU; bit 0 marks a CPU racer, bit 1 the local one.
extern "C" void KartLink_IsCPU_80590664(CpuContext* ctx) {
    MKW_GUEST_FUNC(80590664)(ctx);
    if (g_cpuDriven) {
        ctx->gpr[3] = ctx->gpr[0] & 3;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80590664, KartLink_IsCPU_80590664, "KartLink_IsCPU_80590664");

// RacedataScenario::InitRNG; fixed seeds keep --race-cpu runs repeatable for benchmarking.
extern "C" void RacedataScenario_InitRng_8052f924(CpuContext* ctx) {
    const uint32_t scenario = ctx->gpr[3];
    MKW_GUEST_FUNC(8052F924)(ctx);
    if (g_cpuDriven) {
        Memory::Write32(scenario + kSettingsOffset + 0x2C, 0x74A1B095u);
        Memory::Write32(scenario + kSettingsOffset + 0x30, 0x1234567u);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8052F924, RacedataScenario_InitRng_8052f924, "RacedataScenario_InitRng_8052f924");

// SectionPad::GetFirstDeactivatedPad; a CPU driven racer needs no controller.
extern "C" void SectionPad_GetFirstDeactivatedPad_8061b9bc(CpuContext* ctx) {
    MKW_GUEST_FUNC(8061B9BC)(ctx);
    if (g_cpuDriven) {
        ctx->gpr[3] = static_cast<uint32_t>(-1);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8061B9BC, SectionPad_GetFirstDeactivatedPad_8061b9bc,
                            "SectionPad_GetFirstDeactivatedPad_8061b9bc");
