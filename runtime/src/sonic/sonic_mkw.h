// sonic_mkw.h - Sonic (from the player's own Sonic Adventure DX files) as a
// playable driver in Mario Kart Wii. See docs/SONIC.md.
//
// Sonic takes over one roster slot (Luigi by default):
//   * disc layer  - when the DVD index is built, the UI archives are patched
//                   (roster icons rendered from Sonic's model, the character's
//                   name) and the slot's driver models are fingerprinted;
//   * draw layer  - nw4r::g3d::DrawResMdlDirectly draws Sonic, posed and scaled
//                   from the replaced driver's skeleton, instead of any model
//                   with one of those fingerprints.
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
bool ModelSwapActive();
bool IsSonicFingerprint(uint64_t fingerprint);
bool DebugLogging();

}  // namespace sonic_mkw
