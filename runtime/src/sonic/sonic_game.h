// sonic_game.h - state shared by the game-side parts of the Sonic integration.
//
// Sonic is a new character on top of an existing one, the "base" character
// (Mario by default): the game itself sees a player picking the base character,
// so weight class, item odds, online and every menu keep working. Which players
// actually picked Sonic is tracked here:
//   * sonic_roster.cpp    - the extra Sonic button on the character select grid;
//   * sonic_race_hook.cpp - Sonic players race on foot (SonicRacer), the game's
//                           kart shrinks to an invisible stand-in;
//   * sonic_draw_hook.cpp - draws Sonic (racing on foot, or in the menus over the
//                           base character's model while Sonic is picked).
#pragma once

#include <cstdint>
#include <vector>

#include "soniccore/core/math.h"

namespace sonic_mkw {

struct PosedSonic;

namespace game {

// Character select/race identity.
constexpr const char* kSonicIconPane = "cha_21_hammer";  // unused roster picture pane, Sonic's icon
constexpr uint32_t kSonicNameMessage = 9025;             // BMG message added by the disc patcher
int BaseCharacter();                                     // Mario Kart Wii character id Sonic plays as

// ---- character select (sonic_roster.cpp) ----------------------------------------
constexpr int kHuds = 4;
void SetChoice(int hud, bool sonic);  // the player confirmed Sonic (true) or someone else
bool ChoseSonic(int hud);
void SetHover(int hud, int hover);    // -1 nothing, 0 another character, 1 Sonic
void ClearHovers();
// The menus show Sonic instead of the base character's model.
bool MenuShowsSonic();

// While > 0, the game's per-character icon and name lookups answer with Sonic's
// for the base character (set around the calls that build Sonic's button, name
// and model).
void PushIdentity();
void PopIdentity();
bool IdentityActive();

// ---- race (sonic_race_hook.cpp) --------------------------------------------------
struct RacerDraw {
    sonic::Vec3 kartPos;           // where Sonic put the game's (shrunk) kart, Mario Kart units
    sonic::Vec3 gamePos;           // where the game's own update had it this frame
    const PosedSonic* posed = nullptr;
    int id = 0;
    // Models whose origin is this close (view space) belong to Sonic's stand-in
    // kart; with hideAll they are all skipped (full-size kart), otherwise only
    // the base character's driver (the shrunk kart is too small to see).
    float hideRadius = 40.0f;
    bool hideAll = false;
};
// Sonic racers to draw this frame (empty outside races).
void RacersForDraw(std::vector<RacerDraw>& out);
bool RaceActive();
// True while a race has Sonic players and nobody else uses the base character, so
// race-wide lookups (position icons, result names) can show Sonic for it.
bool RaceBaseIsSonic();
uint32_t FrameNumber();

}  // namespace game
}  // namespace sonic_mkw
