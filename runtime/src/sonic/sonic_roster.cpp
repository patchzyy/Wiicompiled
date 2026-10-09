// sonic_roster.cpp - the Sonic button on the character select screen.
//
// CtrlMenuCharacterSelect (the grid of character buttons) builds one ButtonDriver
// per character in Load (0x807E25A8) through LoadButton(idx) (0x807E2928). After
// the game's Load, a 27th ButtonDriver is made for Sonic with the game's own
// code: it is constructed, then LoadButton is run for the base character's index
// with the button array pointer temporarily shifted so that "button idx" is the
// new one. It therefore gets the base character's layout, handlers and button ID
// (the game sees a click on it as picking the base character), and is then moved
// to a free cell of the grid and appended to the grid's control group.
//
// Which button was used is what makes a player Sonic: the click/select hooks
// record it per HUD slot, and while the game builds the button, its name and its
// model for Sonic, the character icon and name lookups answer with Sonic's (the
// "cha_21_hammer" picture pane, which the disc patcher gives Sonic's portrait,
// and BMG message 9025, "Sonic").
//
// Guest layouts (PAL):
//   CtrlMenuCharacterSelect  children ControlGroup +0x68 (array +0, z-sorted array
//                            +4, count +0x10), driverButtonsArray +0x1B0
//   ButtonDriver (0x260)     UIControl positions: programmer-relative position
//                            +0x04, artist position +0x1C; zIdx +0x84;
//                            ControlManipulator +0x174 (parentManager +0x3C);
//                            PushButton buttonId +0x240
#include "abi_bridge.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "sonic/sonic_formats.h"
#include "sonic/sonic_game.h"
#include "sonic/sonic_guest.h"
#include "sonic/sonic_mkw.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#ifndef RT_TAG_SONIC
#define RT_TAG_SONIC "sonic"
#endif

extern "C" void func_807e25a8(CpuContext* ctx);  // CtrlMenuCharacterSelect::Load
extern "C" void func_807e2928(CpuContext* ctx);  // CtrlMenuCharacterSelect::LoadButton
extern "C" void func_807e35fc(CpuContext* ctx);  // ::OnButtonClick
extern "C" void func_807e36f4(CpuContext* ctx);  // ::OnButtonSelect
extern "C" void func_807e37d4(CpuContext* ctx);  // ::OnButtonDeselect
extern "C" void func_807e3880(CpuContext* ctx);  // ::OnMiiButtonClick
extern "C" void func_807e39cc(CpuContext* ctx);  // ::OnMiiButtonSelect
extern "C" void func_80627008(CpuContext* ctx);  // CtrlMenuCharacterSelect::~CtrlMenuCharacterSelect
extern "C" void func_807e3e10(CpuContext* ctx);  // ButtonDriver::InitSelf
extern "C" void func_80860acc(CpuContext* ctx);  // GetCharacterIconPaneName(CharacterId)
extern "C" void func_80833774(CpuContext* ctx);  // GetCharacterMessageId(CharacterId, ...)

namespace sonic_mkw {
namespace roster_detail {

using sonic::Vec3;
namespace g = guest;

constexpr uint32_t kOperatorNew = 0x80229DCCu;
constexpr uint32_t kOperatorNewArray = 0x80229DF0u;
constexpr uint32_t kButtonDriverCtor = 0x807E2808u;
constexpr uint32_t kButtonDriverDtor = 0x807E2844u;
constexpr uint32_t kGetButtonDriver = 0x807E35B0u;
constexpr uint32_t kLoadButton = 0x807E2928u;
constexpr uint32_t kControlGroupInsert = 0x805C27DCu;  // ControlGroup::SetControl(u8 idx, UIControl&, u32 zIdx)
constexpr uint32_t kAddControlManipulator = 0x805F0D44u;
constexpr uint32_t kButtonSize = 0x260u;
constexpr int kGridButtons = 26;  // 24 characters + 2 Mii outfits

struct Grid {
    uint32_t ctrl = 0;
    uint32_t sonic = 0;  // our ButtonDriver
    uint32_t base = 0;   // the base character's ButtonDriver
    int baseIndex = -1;
    bool baseInitialised = false;
    bool sonicInitialised = false;
    bool manipulatorChecked = false;
};

struct State {
    std::map<uint32_t, Grid> grids;       // by CtrlMenuCharacterSelect
    uint32_t loading = 0;                 // ctrl whose Load is running
    std::map<int, uint32_t> loadButtonR5;  // LoadButton's second argument, by index
    bool choseSonic[game::kHuds] = {};
    bool choseBase[game::kHuds] = {};     // picked the real base character
    int hover[game::kHuds] = {-1, -1, -1, -1};
    std::atomic<int> identity{0};          // > 0: lookups answer with Sonic
    std::atomic<int> notSonic{0};          // > 0: lookups answer with the base character
    int logs = 0;
};

State& S() {
    static State state;
    return state;
}

Grid* GridFor(uint32_t ctrl) {
    auto it = S().grids.find(ctrl);
    return it == S().grids.end() ? nullptr : &it->second;
}

bool IsSonicButton(uint32_t button) {
    for (auto& [ctrl, grid] : S().grids) {
        if (grid.sonic && grid.sonic == button) return true;
    }
    return false;
}

// Identity rules for the icon/name lookups (see sonic_game.h).
bool AnswerWithSonic() {
    State& s = S();
    if (s.identity.load() > 0) return true;
    if (s.notSonic.load() > 0) return false;  // the real base character's button, name or model
    if (game::RaceActive()) return game::RaceBaseIsSonic();
    bool sonic = false, base = false;
    for (int h = 0; h < game::kHuds; ++h) {
        sonic = sonic || s.choseSonic[h];
        base = base || s.choseBase[h];
    }
    return sonic && !base;
}

// Around the game's work for one grid button: Sonic's button answers with
// Sonic, every other button with its own character.
struct IdentityScope {
    explicit IdentityScope(bool sonic) : sonic_(sonic) {
        if (sonic_) game::PushIdentity();
        else ++S().notSonic;
    }
    ~IdentityScope() {
        if (sonic_) game::PopIdentity();
        else --S().notSonic;
    }
    bool sonic_;
};

Vec3 Position(uint32_t control) { return g::Vec(control + 0x04); }

// A free cell of the grid for Sonic: the grid's own spacing, first free cell in
// reading order (top row first), or a new column right of the last row.
Vec3 FreeCell(uint32_t buttons, int count, uint32_t base) {
    std::vector<Vec3> pos;
    for (int i = 0; i < count; ++i) pos.push_back(Position(buttons + uint32_t(i) * kButtonSize));
    auto unique = [](std::vector<float> v) {
        std::sort(v.begin(), v.end());
        std::vector<float> out;
        for (float x : v) {
            if (out.empty() || std::fabs(x - out.back()) > 2.0f) out.push_back(x);
        }
        return out;
    };
    std::vector<float> xs, ys;
    for (const Vec3& p : pos) {
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    const std::vector<float> ux = unique(xs), uy = unique(ys);
    auto occupied = [&](float x, float y) {
        for (const Vec3& p : pos) {
            if (std::fabs(p.x - x) < 2.0f && std::fabs(p.y - y) < 2.0f) return true;
        }
        return false;
    };
    // layout Y grows upwards: top row first
    for (auto y = uy.rbegin(); y != uy.rend(); ++y) {
        for (float x : ux) {
            if (!occupied(x, *y)) return Vec3(x, *y, Position(base).z);
        }
    }
    float dx = 0;
    for (size_t i = 1; i < ux.size(); ++i) {
        const float d = ux[i] - ux[i - 1];
        if (d > 2.0f && (dx == 0 || d < dx)) dx = d;
    }
    if (dx == 0) dx = 60.0f;
    const float bottom = uy.empty() ? Position(base).y : uy.front();
    float right = -1e30f;
    for (const Vec3& p : pos) {
        if (std::fabs(p.y - bottom) < 2.0f) right = std::max(right, p.x);
    }
    return Vec3(right + dx, bottom, Position(base).z);
}

void LogGrid(uint32_t buttons, int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        const uint32_t b = buttons + uint32_t(i) * kButtonSize;
        const Vec3 p = Position(b);
        char item[64];
        std::snprintf(item, sizeof(item), " %d:(%.0f,%.0f)", int(g::U32(b + 0x240)), p.x, p.y);
        text += item;
    }
    RT_LOG(RT_TAG_SONIC) << "character grid buttons (id:(x,y)):" << text << std::endl;
}

void AddSonicButton(CpuContext* ctx, uint32_t ctrl) {
    State& s = S();
    const int baseCharacter = game::BaseCharacter();
    const uint32_t buttons = g::Ptr(ctrl + 0x1B0);
    if (!buttons) {
        RT_LOG(RT_TAG_SONIC) << "character select: no button array; no Sonic button" << std::endl;
        return;
    }
    const uint32_t base = g::Call(ctx, kGetButtonDriver, {ctrl, uint32_t(baseCharacter)});
    if (!base || base < buttons || base >= buttons + kGridButtons * kButtonSize ||
        (base - buttons) % kButtonSize != 0) {
        RT_LOGF(RT_TAG_SONIC, "character select: no button for character %d (got 0x%08X); no Sonic button\n",
                baseCharacter, base);
        return;
    }
    const int index = int((base - buttons) / kButtonSize);
    const uint32_t group = ctrl + 0x68;
    const uint32_t count = g::U32(group + 0x10);
    if (count == 0 || count > 64 || !g::Ptr(group) || !g::Ptr(group + 4)) {
        RT_LOGF(RT_TAG_SONIC, "character select: unexpected control group (%u controls); no Sonic button\n", count);
        return;
    }
    if (DebugLogging()) LogGrid(buttons, kGridButtons);

    // 1. a new ButtonDriver, built by the game
    const uint32_t button = g::Call(ctx, kOperatorNew, {kButtonSize});
    if (!g::Valid(button, kButtonSize)) {
        RT_LOG(RT_TAG_SONIC) << "character select: out of memory for the Sonic button" << std::endl;
        return;
    }
    g::Call(ctx, kButtonDriverCtor, {button});

    // 2. loaded as button `index` (the base character's): the array pointer is
    //    shifted so that the game's LoadButton(index) lands on the new button
    const auto r5 = s.loadButtonR5.find(index);
    g::SetU32(ctrl + 0x1B0, button - uint32_t(index) * kButtonSize);
    {
        IdentityScope sonic(true);
        g::Call(ctx, kLoadButton, {ctrl, uint32_t(index), r5 != s.loadButtonR5.end() ? r5->second : 1u});
    }
    g::SetU32(ctrl + 0x1B0, buttons);

    // 3. LoadButton may have put it in the base button's slot of the control group:
    //    give the slot back, then append it as a new control.
    const uint32_t oldArray = g::Ptr(group), oldSorted = g::Ptr(group + 4);
    for (uint32_t i = 0; i < count; ++i) {
        if (g::U32(oldArray + 4 * i) == button) g::SetU32(oldArray + 4 * i, base);
        if (g::U32(oldSorted + 4 * i) == button) g::SetU32(oldSorted + 4 * i, base);
    }
    const uint32_t newArray = g::Call(ctx, kOperatorNewArray, {(count + 1) * 4});
    const uint32_t newSorted = g::Call(ctx, kOperatorNewArray, {(count + 1) * 4});
    if (!g::Valid(newArray, (count + 1) * 4) || !g::Valid(newSorted, (count + 1) * 4)) {
        RT_LOG(RT_TAG_SONIC) << "character select: out of memory for the control group" << std::endl;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        g::SetU32(newArray + 4 * i, g::U32(oldArray + 4 * i));
        g::SetU32(newSorted + 4 * i, g::U32(oldSorted + 4 * i));
    }
    g::SetU32(newArray + 4 * count, 0);
    g::SetU32(newSorted + 4 * count, 0);
    g::SetU32(group, newArray);
    g::SetU32(group + 4, newSorted);
    g::SetU32(group + 0x10, count + 1);
    g::Call(ctx, kControlGroupInsert, {group, count, button, g::U32(base + 0x84)});

    // 4. its own cell in the grid
    const Vec3 cell = FreeCell(buttons, kGridButtons, base);
    const Vec3 delta = cell - Position(base);
    g::SetVec(button + 0x04, Position(button) + delta);
    g::SetVec(button + 0x1C, g::Vec(button + 0x1C) + delta);

    Grid grid;
    grid.ctrl = ctrl;
    grid.sonic = button;
    grid.base = base;
    grid.baseIndex = index;
    grid.baseInitialised = s.grids.count(ctrl) ? s.grids[ctrl].baseInitialised : false;
    s.grids[ctrl] = grid;
    RT_LOGF(RT_TAG_SONIC,
            "Sonic button added to the character select grid at (%.0f, %.0f) (base %d, button index %d, 0x%08X)\n",
            cell.x, cell.y, baseCharacter, index, button);

    // The grid's controls may already be initialised (then ours is done here) or
    // not (then the game initialises it with the others).
    if (grid.baseInitialised) {
        const uint32_t vtable = g::Ptr(button);
        const uint32_t init = vtable ? g::U32(vtable + 0x0C) : 0;
        if (init) g::Call(ctx, init, {button});
    }
}

// The new button must be in the page's list of selectable controls. LoadButton or
// the page may already have done that; otherwise register it with the manager the
// base button is in.
void CheckManipulator(CpuContext* ctx, Grid& grid) {
    if (grid.manipulatorChecked) return;
    const uint32_t manager = g::Ptr(grid.base + 0x174 + 0x3C);
    if (!manager) return;  // not registered yet: try again later
    grid.manipulatorChecked = true;
    if (g::Ptr(grid.sonic + 0x174 + 0x3C)) return;
    g::Call(ctx, kAddControlManipulator, {manager, grid.sonic + 0x174});
    RT_LOGF(RT_TAG_SONIC, "Sonic button registered with the page's controls (0x%08X)\n", manager);
}

bool Enabled() {
    return RuntimeConfigFile::SonicEnabled() && ToLowerAscii(RuntimeConfigFile::SonicSelect()) != "base";
}

}  // namespace roster_detail

namespace game {

int BaseCharacter() {
    static int cached = -2;
    if (cached == -2) {
        const CharacterSlot* slot = FindCharacterSlot(RuntimeConfigFile::SonicBase());
        if (!slot) {
            RT_LOG(RT_TAG_SONIC) << "base = \"" << RuntimeConfigFile::SonicBase()
                                 << "\" is not a Mario Kart Wii character; using Mario" << std::endl;
        }
        cached = slot ? slot->id : 0;
    }
    return cached;
}

void SetChoice(int hud, bool sonic) {
    if (hud < 0 || hud >= kHuds) return;
    roster_detail::S().choseSonic[hud] = sonic;
}
bool ChoseSonic(int hud) { return hud >= 0 && hud < kHuds && roster_detail::S().choseSonic[hud]; }
void SetHover(int hud, int hover) {
    if (hud >= 0 && hud < kHuds) roster_detail::S().hover[hud] = hover;
}
void ClearHovers() {
    for (int& h : roster_detail::S().hover) h = -1;
}
bool MenuShowsSonic() {
    auto& s = roster_detail::S();
    for (int h = 0; h < kHuds; ++h) {
        if (s.hover[h] == 1) return true;
        if (s.hover[h] == -1 && s.choseSonic[h]) return true;
    }
    return false;
}
void PushIdentity() { ++roster_detail::S().identity; }
void PopIdentity() { --roster_detail::S().identity; }
bool IdentityActive() { return roster_detail::S().identity.load() > 0; }

}  // namespace game
}  // namespace sonic_mkw


extern "C" void CtrlMenuCharacterSelect_Load_Sonic_807e25a8(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const uint32_t ctrl = ctx->gpr[3];
    if (!sonic_mkw::roster_detail::Enabled()) {
        func_807e25a8(ctx);
        return;
    }
    s.loading = ctrl;
    s.loadButtonR5.clear();
    sonic_mkw::game::ClearHovers();
    {
        sonic_mkw::roster_detail::IdentityScope scope(false);  // the real buttons keep their own icons
        func_807e25a8(ctx);
    }
    s.loading = 0;
    try {
        sonic_mkw::roster_detail::AddSonicButton(ctx, ctrl);
    } catch (const std::exception& error) {
        RT_LOGF(RT_TAG_SONIC, "Sonic button failed: %s\n", error.what());
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x807E25A8, CtrlMenuCharacterSelect_Load_Sonic_807e25a8,
                            "CtrlMenuCharacterSelect_Load_Sonic_807e25a8");

extern "C" void CtrlMenuCharacterSelect_LoadButton_Sonic_807e2928(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    if (s.loading == ctx->gpr[3]) s.loadButtonR5[int(ctx->gpr[4])] = ctx->gpr[5];
    func_807e2928(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E2928, CtrlMenuCharacterSelect_LoadButton_Sonic_807e2928,
                            "CtrlMenuCharacterSelect_LoadButton_Sonic_807e2928");

extern "C" void CtrlMenuCharacterSelect_ButtonDriver_InitSelf_Sonic_807e3e10(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const uint32_t button = ctx->gpr[3];
    bool sonic = false;
    for (auto& [ctrl, grid] : s.grids) {
        if (button == grid.base) grid.baseInitialised = true;
        if (button == grid.sonic) {
            sonic = true;
            grid.sonicInitialised = true;
        }
    }
    // A grid still being loaded has no entry yet: remember its base initialised.
    if (s.loading && !s.grids.count(s.loading)) {
        const uint32_t buttons = sonic_mkw::guest::Ptr(s.loading + 0x1B0);
        if (buttons && button >= buttons && button < buttons + 26 * 0x260u) {
            sonic_mkw::roster_detail::Grid& grid = s.grids[s.loading];
            grid.ctrl = s.loading;
            grid.baseInitialised = true;  // conservatively: the grid initialises during Load
        }
    }
    sonic_mkw::roster_detail::IdentityScope scope(sonic);
    func_807e3e10(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E3E10, CtrlMenuCharacterSelect_ButtonDriver_InitSelf_Sonic_807e3e10,
                            "CtrlMenuCharacterSelect_ButtonDriver_InitSelf_Sonic_807e3e10");

extern "C" void CtrlMenuCharacterSelect_OnButtonClick_Sonic_807e35fc(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const uint32_t button = ctx->gpr[4];
    const int hud = int(ctx->gpr[5]);
    const bool sonic = sonic_mkw::roster_detail::IsSonicButton(button);
    if (!s.grids.empty() && hud >= 0 && hud < sonic_mkw::game::kHuds) {
        s.choseSonic[hud] = sonic;
        sonic_mkw::roster_detail::Grid* grid = sonic_mkw::roster_detail::GridFor(ctx->gpr[3]);
        s.choseBase[hud] = !sonic && grid && button == grid->base;
        if (sonic_mkw::DebugLogging() || sonic) {
            RT_LOGF(RT_TAG_SONIC, "character select: player %d picked %s\n", hud + 1,
                    sonic ? "SONIC" : "another character");
        }
    }
    sonic_mkw::roster_detail::IdentityScope scope(sonic);
    func_807e35fc(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E35FC, CtrlMenuCharacterSelect_OnButtonClick_Sonic_807e35fc,
                            "CtrlMenuCharacterSelect_OnButtonClick_Sonic_807e35fc");

extern "C" void CtrlMenuCharacterSelect_OnButtonSelect_Sonic_807e36f4(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const uint32_t button = ctx->gpr[4];
    const int hud = int(ctx->gpr[5]);
    const bool sonic = sonic_mkw::roster_detail::IsSonicButton(button);
    if (sonic_mkw::roster_detail::Grid* grid = sonic_mkw::roster_detail::GridFor(ctx->gpr[3])) sonic_mkw::roster_detail::CheckManipulator(ctx, *grid);
    if (!s.grids.empty()) sonic_mkw::game::SetHover(hud, sonic ? 1 : 0);
    sonic_mkw::roster_detail::IdentityScope scope(sonic);
    func_807e36f4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E36F4, CtrlMenuCharacterSelect_OnButtonSelect_Sonic_807e36f4,
                            "CtrlMenuCharacterSelect_OnButtonSelect_Sonic_807e36f4");

extern "C" void CtrlMenuCharacterSelect_OnButtonDeselect_Sonic_807e37d4(CpuContext* ctx) {
    const bool sonic = sonic_mkw::roster_detail::IsSonicButton(ctx->gpr[4]);
    sonic_mkw::roster_detail::IdentityScope scope(sonic);
    func_807e37d4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E37D4, CtrlMenuCharacterSelect_OnButtonDeselect_Sonic_807e37d4,
                            "CtrlMenuCharacterSelect_OnButtonDeselect_Sonic_807e37d4");

extern "C" void CtrlMenuCharacterSelect_OnMiiButtonClick_Sonic_807e3880(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const int hud = int(ctx->gpr[5]);
    if (hud >= 0 && hud < sonic_mkw::game::kHuds) {
        s.choseSonic[hud] = false;
        s.choseBase[hud] = false;
    }
    func_807e3880(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E3880, CtrlMenuCharacterSelect_OnMiiButtonClick_Sonic_807e3880,
                            "CtrlMenuCharacterSelect_OnMiiButtonClick_Sonic_807e3880");

extern "C" void CtrlMenuCharacterSelect_OnMiiButtonSelect_Sonic_807e39cc(CpuContext* ctx) {
    if (!sonic_mkw::roster_detail::S().grids.empty()) sonic_mkw::game::SetHover(int(ctx->gpr[5]), 0);
    func_807e39cc(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x807E39CC, CtrlMenuCharacterSelect_OnMiiButtonSelect_Sonic_807e39cc,
                            "CtrlMenuCharacterSelect_OnMiiButtonSelect_Sonic_807e39cc");

extern "C" void CtrlMenuCharacterSelect_dtor_Sonic_80627008(CpuContext* ctx) {
    auto& s = sonic_mkw::roster_detail::S();
    const uint32_t ctrl = ctx->gpr[3];
    auto it = s.grids.find(ctrl);
    const uint32_t button = it != s.grids.end() ? it->second.sonic : 0;
    if (it != s.grids.end()) {
        s.grids.erase(it);
        sonic_mkw::game::ClearHovers();
    }
    func_80627008(ctx);
    if (button) {
        // ~ButtonDriver with "delete" (the control group never owned it)
        CpuContext call = *ctx;
        sonic_mkw::guest::Call(&call, sonic_mkw::roster_detail::kButtonDriverDtor, {button, 1u});
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80627008, CtrlMenuCharacterSelect_dtor_Sonic_80627008,
                            "CtrlMenuCharacterSelect_dtor_Sonic_80627008");

extern "C" void GetCharacterIconPaneName_Sonic_80860acc(CpuContext* ctx) {
    const int character = int(ctx->gpr[3]);
    func_80860acc(ctx);
    if (character == sonic_mkw::game::BaseCharacter() && sonic_mkw::roster_detail::AnswerWithSonic()) {
        const uint32_t name = sonic_mkw::guest::InternString(sonic_mkw::game::kSonicIconPane);
        if (name) ctx->gpr[3] = name;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80860ACC, GetCharacterIconPaneName_Sonic_80860acc,
                            "GetCharacterIconPaneName_Sonic_80860acc");

extern "C" void GetCharacterMessageId_Sonic_80833774(CpuContext* ctx) {
    const int character = int(ctx->gpr[3]);
    func_80833774(ctx);
    if (character == sonic_mkw::game::BaseCharacter() && sonic_mkw::roster_detail::AnswerWithSonic()) {
        ctx->gpr[3] = sonic_mkw::game::kSonicNameMessage;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80833774, GetCharacterMessageId_Sonic_80833774, "GetCharacterMessageId_Sonic_80833774");
