// sonic_mkw.h - Sonic (from the player's own Sonic Adventure DX files) as a
// playable driver in Mario Kart Wii. See docs/SONIC.md.
//
// Sonic is a new character who races on foot (see sonic_game.h for the game-side
// parts). Under the hood he plays as a "base" character (Mario by default):
//   * disc layer  - when the DVD index is built, the UI archives are patched
//                   (Sonic's portrait in the roster's unused hammer picture, a new
//                   "Sonic" name message) and the base character's driver models
//                   are fingerprinted;
//   * draw layer  - nw4r::g3d::DrawResMdlDirectly draws Sonic running in races and,
//                   in the menus while Sonic is picked, in place of any model with
//                   one of those fingerprints.
// Nothing from either game is shipped: everything is generated at runtime from
// files the player already has, and cached in <app data>/Cache/sonic.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sonic_mkw {

struct CharacterSlot {
    int id;               // Mario Kart Wii character ID
    const char* name;     // English name
    const char* abbr;     // file abbreviation (Race/Kart/<kart>-<abbr>.szs)
    const char* icon;     // UI texture name (tt_<icon>_64x64.tpl)
};
// Accepts the English name ("luigi", "King Boo"), the file abbreviation ("lg")
// or the icon name ("teresa"); case, spaces and punctuation are ignored.
const CharacterSlot* FindCharacterSlot(const std::string& text);

struct DiscFile {
    std::string dvdPath;               // "/Scene/UI/MenuSingle.szs"
    std::filesystem::path hostPath;
};
using RegisterDiscFile = std::function<void(const std::string& dvdPath, const std::filesystem::path& hostPath,
                                            uint32_t size)>;

// Called once by DVDInit after the disc and overlay files are registered and
// before the FST is published. Registers patched replacements through `reg`.
void PatchDisc(const std::vector<DiscFile>& files, const RegisterDiscFile& reg);

// ---- shared with the draw hook ----------------------------------------------------
// The base character's models were found (the menus can show Sonic over them).
bool ModelSwapActive();
bool IsBaseModelFingerprint(uint64_t fingerprint);
bool DebugLogging();

}  // namespace sonic_mkw
