// Riivolution overlay loading (host-IO shim). See riivolution.h.
//
// Ported behavior from Dolphin Emulator's DiscIO/RiivolutionParser and
// RiivolutionPatcher (https://github.com/dolphin-emu/dolphin).
// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/storage/riivolution.h"

#include "hle/riivolution_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "nand_path.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct RiivoState {
    std::vector<RuntimeRiivolution::Overlay> overlays;
    std::optional<RuntimeRiivolution::SaveRedirect> saveRedirect;
};

std::once_flag g_riivoOnce;
RiivoState g_riivoState;

std::optional<std::string> RiivoReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Six-character game ID ("RMCP01") from guest low memory, with the same RMCP
// fallback the rest of the storage HLE uses for boots that have not written
// the disc header yet.
std::string RiivoGameId() {
    const uint32_t code = RuntimeHle::CurrentGameCode(0x524D4350u); // "RMCP"
    std::string id(6, '\0');
    id[0] = static_cast<char>((code >> 24) & 0xffu);
    id[1] = static_cast<char>((code >> 16) & 0xffu);
    id[2] = static_cast<char>((code >> 8) & 0xffu);
    id[3] = static_cast<char>(code & 0xffu);

    uint16_t maker = 0;
    if (Memory::Contains(0x80000004u, 2u)) {
        maker = Memory::Read16(0x80000004u);
    }
    const char makerHi = static_cast<char>((maker >> 8) & 0xffu);
    const char makerLo = static_cast<char>(maker & 0xffu);
    if (std::isalnum(static_cast<unsigned char>(makerHi)) &&
        std::isalnum(static_cast<unsigned char>(makerLo))) {
        id[4] = makerHi;
        id[5] = makerLo;
    } else {
        id[4] = '0';
        id[5] = '1';
    }
    return id;
}

// Every narrow path string here is UTF-8, including the ones the XML halves of
// resolved paths are concatenated with.
using RuntimeConfigFile::PathFromUtf8;
using RuntimeConfigFile::PathToUtf8;

std::string RiivoGenericText(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string RiivoComparablePath(const fs::path& path) {
    std::string text = RiivoGenericText(path.lexically_normal());
#ifdef _WIN32
    RuntimeHle::LowerInPlace(text);
#endif
    return text;
}

void RiivoAddRoot(std::vector<RuntimeRiivolution::Overlay>& overlays, fs::path root,
                  const char* source) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        RT_LOG(RT_TAG_RIIVOLUTION) << "rejected overlay root (" << (source ? source : "unknown")
                  << "): " << PathToUtf8(root) << " is not a reachable directory" << std::endl;
        return;
    }

    fs::path normalized = fs::weakly_canonical(root, ec);
    if (ec) {
        normalized = root.lexically_normal();
    }

    const std::string comparable = RiivoComparablePath(normalized);
    const auto duplicate =
        std::find_if(overlays.begin(), overlays.end(), [&](const RuntimeRiivolution::Overlay& overlay) {
            return RiivoComparablePath(overlay.root) == comparable;
        });
    if (duplicate != overlays.end()) {
        return;
    }

    RT_LOG(RT_TAG_RIIVOLUTION) << "overlay root (" << (source ? source : "unknown")
              << "): " << PathToUtf8(normalized) << std::endl;
    overlays.push_back({std::move(normalized), std::nullopt});
}

std::vector<RuntimeRiivolution::Overlay> RiivoDiscoverRoots() {
    std::vector<RuntimeRiivolution::Overlay> overlays;

    // Discovery order is precedence order: DVDInit applies the roots in
    // reverse, so an explicitly configured root (command line first, then
    // Config.toml) always outranks the mod manifest.
    for (const auto& root : RuntimeConfigFile::OverlayRoots()) {
        // A relative overlay root resolves against the config file that named
        // it, never against whatever working directory the process happened
        // to be started in.
        RiivoAddRoot(overlays, RuntimeNandPath::ResolveConfiguredPath(root),
                     "command line or Config.toml");
    }

    // The one canonical Retro Rewind installation setup recorded. Only the
    // Retro Rewind product applies it: overlaying the base product with the
    // pack's menu archives made "base game" boot as a half-Retro-Rewind build.
    if (const auto retroRewindRoot =
            RuntimeProduct::IsRetroRewind() ? RuntimeConfigFile::RetroRewindRoot() : std::string{};
        !retroRewindRoot.empty()) {
        RiivoAddRoot(overlays, RuntimeNandPath::ResolveConfiguredPath(retroRewindRoot),
                     "canonical Retro Rewind installation");
    }

    for (const auto& root : RecompMod::DvdOverlayRoots()) {
        RiivoAddRoot(overlays, root, "recomp mod manifest");
    }

    return overlays;
}

// The Riivolution XMLs describing an overlay root, plus the SD-root directory
// that absolute external paths resolve against.
struct RiivoXmlSet {
    fs::path sdRoot;
    std::vector<fs::path> xmlFiles;
};

std::optional<RiivoXmlSet> RiivoFindXmls(const fs::path& overlayRoot) {
    std::error_code ec;

    // Distribution-pinned XML from recomp.yml: a path relative to the pack
    // folder. The pack folder sits inside the virtual SD root (e.g.
    // <sd>/RetroRewind6), so externals resolve against the root's parent.
    const std::string& configured = RecompMod::RiivolutionXml();
    if (!configured.empty()) {
        const fs::path configuredXml = overlayRoot / PathFromUtf8(configured);
        if (fs::is_regular_file(configuredXml, ec)) {
            return RiivoXmlSet{overlayRoot.parent_path(), {configuredXml}};
        }
    }

    // Dolphin-style discovery: the overlay root is itself a virtual SD root
    // carrying <root>/riivolution/*.xml.
    const fs::path xmlDirectory = overlayRoot / "riivolution";
    if (fs::is_directory(xmlDirectory, ec)) {
        std::vector<fs::path> xmlFiles;
        for (const auto& entry : fs::directory_iterator(xmlDirectory, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file(ec) && entry.path().extension() == ".xml") {
                xmlFiles.push_back(entry.path());
            }
        }
        if (!xmlFiles.empty()) {
            // Sort so the same folder always produces the same disc.
            std::sort(xmlFiles.begin(), xmlFiles.end());
            return RiivoXmlSet{overlayRoot, std::move(xmlFiles)};
        }
    }

    return std::nullopt;
}

std::vector<RiivolutionContract::OptionSelection> RiivoManifestSelections() {
    std::vector<RiivolutionContract::OptionSelection> selections;
    for (const auto& selection : RecompMod::RiivolutionOptionSelections()) {
        selections.push_back({selection.section, selection.option, selection.choice});
    }
    return selections;
}

void RiivoCollectMappings(const RiivolutionContract::Patch& patch, const std::string& sdRootGeneric,
                          const std::string& xmlDirGeneric, RuntimeRiivolution::PatchSet& set) {
    const std::string patchRoot =
        RiivolutionContract::ResolvePatchRoot(sdRootGeneric, xmlDirGeneric, patch.root);
    std::error_code ec;

    // Dolphin applies each patch's file patches first, then its folder
    // patches; with last-registration-wins that means folders shadow files
    // within one patch, and later patches shadow earlier ones.
    for (const auto& file : patch.filePatches) {
        if (file.disc.empty()) {
            ++set.skippedExternals;
            continue;
        }
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, file.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFile = PathFromUtf8(*resolved);
        if (!fs::is_regular_file(hostFile, ec)) {
            ++set.skippedExternals;
            continue;
        }
        set.mappings.push_back(
            {RuntimeRiivolution::Mapping::Kind::File, file.disc, hostFile, true, file.create});
    }

    for (const auto& folder : patch.folderPatches) {
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, folder.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFolder = PathFromUtf8(*resolved);
        if (!fs::is_directory(hostFolder, ec)) {
            ++set.skippedExternals;
            continue;
        }
        const auto kind = folder.disc.empty() ? RuntimeRiivolution::Mapping::Kind::FolderByName
                                              : RuntimeRiivolution::Mapping::Kind::Folder;
        set.mappings.push_back({kind, folder.disc, hostFolder, folder.recursive, folder.create});
    }
}

std::optional<RuntimeRiivolution::PatchSet> RiivoLoadPatchSet(const fs::path& overlayRoot,
                                                              RiivoState& state) {
    const auto xmlSet = RiivoFindXmls(overlayRoot);
    if (!xmlSet) {
        return std::nullopt;
    }

    const std::string gameId = RiivoGameId();
    const std::string sdRootGeneric = RiivoGenericText(xmlSet->sdRoot);
    const auto manifestSelections = RiivoManifestSelections();

    // Dolphin-compatible remembered choices; a recomp.yml pin overrides them.
    std::optional<RiivolutionContract::Config> config;
    const fs::path configXml =
        xmlSet->sdRoot / "riivolution" / "config" / (gameId.substr(0, 4) + ".xml");
    if (const auto configText = RiivoReadFile(configXml)) {
        config = RiivolutionContract::ParseConfigString(*configText);
    }

    RuntimeRiivolution::PatchSet set;
    for (const fs::path& xmlFile : xmlSet->xmlFiles) {
        const auto text = RiivoReadFile(xmlFile);
        if (!text) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: cannot read " << PathToUtf8(xmlFile)
                      << std::endl;
            continue;
        }

        auto disc = RiivolutionContract::ParseString(*text);
        if (!disc) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(xmlFile)
                      << " is not a valid Riivolution XML (version 1 wiidisc); ignoring it"
                      << std::endl;
            continue;
        }
        if (!disc->IsValidForGame(gameId, std::nullopt, std::nullopt)) {
            RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(xmlFile) << ": not valid for " << gameId
                      << ", skipped" << std::endl;
            continue;
        }

        if (config) {
            RiivolutionContract::ApplyConfigDefaults(*disc, *config);
        }
        RiivolutionContract::ApplySelections(*disc, manifestSelections);

        const auto activePatches = disc->GeneratePatches(gameId);
        const std::string xmlDirGeneric = RiivoGenericText(xmlFile.parent_path());

        const size_t before = set.mappings.size();
        for (const auto& patch : activePatches) {
            RiivoCollectMappings(patch, sdRootGeneric, xmlDirGeneric, set);
        }

        // Highest-priority overlay with a <savegame> wins.
        if (!state.saveRedirect) {
            if (const auto* savegame = RiivolutionContract::FindSavegamePatch(activePatches)) {
                if (const auto resolvedSave = RiivolutionContract::MakeAbsoluteFromRelative(
                        sdRootGeneric, xmlDirGeneric, savegame->external)) {
                    state.saveRedirect =
                        RuntimeRiivolution::SaveRedirect{PathFromUtf8(*resolvedSave),
                                                         savegame->clone};
                    RT_LOG(RT_TAG_RIIVOLUTION) << "savegame redirect: "
                              << PathToUtf8(state.saveRedirect->hostDirectory)
                              << (savegame->clone ? " (clone)" : "") << std::endl;
                }
            }
        }

        // A pack whose XML parses but activates nothing is the most confusing
        // failure this layer has: the game boots, plays, and quietly shows
        // vanilla content. Always say what happened.
        RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(xmlFile) << ": " << activePatches.size()
                  << " active patch(es), " << (set.mappings.size() - before) << " mapping(s)"
                  << std::endl;
        if (activePatches.empty()) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(xmlFile)
                      << " has no enabled options for " << gameId
                      << "; check the riivolution option selections (recomp.yml) or "
                      << sdRootGeneric << "/riivolution/config/" << gameId.substr(0, 4) << ".xml"
                      << std::endl;
        }
    }

    if (set.skippedExternals != 0) {
        RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(overlayRoot) << ": skipped "
                  << set.skippedExternals << " mapping(s) whose external path does not exist"
                  << std::endl;
    }

    return set;
}

void RiivoInitialize() {
    g_riivoState.overlays = RiivoDiscoverRoots();
    for (auto& overlay : g_riivoState.overlays) {
        overlay.patches = RiivoLoadPatchSet(overlay.root, g_riivoState);
    }
}

} // namespace

namespace RuntimeRiivolution {

const std::vector<Overlay>& Overlays() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.overlays;
}

const std::optional<SaveRedirect>& GetSaveRedirect() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.saveRedirect;
}

} // namespace RuntimeRiivolution
