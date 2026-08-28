// Riivolution patch-XML parsing and patch selection.
//
// Ported from Dolphin Emulator's DiscIO/RiivolutionParser.h/cpp and the
// external-path resolution rules of DiscIO/RiivolutionPatcher.cpp
// (https://github.com/dolphin-emu/dolphin).
// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Deviations from Dolphin, all deliberate:
//  - <memory> patches are parsed but never applied here: guest code patching
//    belongs to the translator's Code.pul/lowmem pipeline, not the runtime.
//  - Riivolution "macros" are not supported

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

namespace RiivolutionContract {

// ============================================================================
// Minimal XML document model
// ============================================================================

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;

    const std::string* FindAttribute(std::string_view attributeName) const {
        for (const auto& attribute : attributes) {
            if (attribute.first == attributeName) {
                return &attribute.second;
            }
        }
        return nullptr;
    }

    std::string Attribute(std::string_view attributeName, std::string_view fallback = {}) const {
        const std::string* value = FindAttribute(attributeName);
        return value ? *value : std::string(fallback);
    }

    bool AttributeBool(std::string_view attributeName, bool fallback) const {
        const std::string* value = FindAttribute(attributeName);
        if (!value) {
            return fallback;
        }
        if (*value == "true" || *value == "1" || *value == "yes") {
            return true;
        }
        if (*value == "false" || *value == "0" || *value == "no") {
            return false;
        }
        return fallback;
    }

    // Accepts decimal and 0x-prefixed hex, matching pugixml's number parsing
    // (Riivolution XMLs write memory offsets as 0x........).
    uint32_t AttributeUint(std::string_view attributeName, uint32_t fallback) const {
        const std::string* value = FindAttribute(attributeName);
        if (!value || value->empty()) {
            return fallback;
        }
        const std::string& text = *value;
        size_t index = 0;
        uint32_t base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            index = 2;
        }
        uint64_t result = 0;
        for (; index < text.size(); ++index) {
            const char c = text[index];
            uint32_t digit;
            if (c >= '0' && c <= '9') {
                digit = static_cast<uint32_t>(c - '0');
            } else if (base == 16 && c >= 'a' && c <= 'f') {
                digit = static_cast<uint32_t>(c - 'a' + 10);
            } else if (base == 16 && c >= 'A' && c <= 'F') {
                digit = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fallback;
            }
            result = result * base + digit;
            if (result > 0xffffffffull) {
                return fallback;
            }
        }
        return index > (base == 16 ? 2u : 0u) ? static_cast<uint32_t>(result) : fallback;
    }

    int AttributeInt(std::string_view attributeName, int fallback) const {
        const std::string* value = FindAttribute(attributeName);
        if (!value || value->empty()) {
            return fallback;
        }
        const bool negative = (*value)[0] == '-';
        const uint32_t magnitude =
            AttributeUintFromText(negative ? value->substr(1) : *value, 0x80000000u);
        if (magnitude == 0x80000000u && !negative) {
            return fallback;
        }
        return negative ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
    }

private:
    static uint32_t AttributeUintFromText(const std::string& text, uint32_t fallback) {
        XmlNode probe;
        probe.attributes.push_back({"v", text});
        return probe.AttributeUint("v", fallback);
    }
};

namespace XmlDetail {

inline bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

} // namespace XmlDetail

// Converts only element and attribute data from pugixml. Riivolution carries
// its data in attributes, so text, declarations, comments, and CDATA do not
// need to become part of the contract's data model.
// depth guards against unbounded native-stack recursion: riivolution.xml is a user-authored file
// (SD card / mod pack), and a real pack never nests more than a handful of levels deep, so a
// crafted pack with deliberately deep nesting is a stack-overflow DoS without this cap. Nodes
// beyond the cap are kept (with their attributes) but their own children are dropped rather than
// failing the whole parse over one malformed branch.
inline XmlNode CopyXmlNode(const pugi::xml_node& source, int depth = 0) {
    XmlNode destination;
    destination.name = source.name();
    for (const pugi::xml_attribute& attribute : source.attributes()) {
        destination.attributes.emplace_back(attribute.name(), attribute.value());
    }
    constexpr int kMaxXmlDepth = 64;
    if (depth < kMaxXmlDepth) {
        for (const pugi::xml_node& child : source.children()) {
            if (child.type() == pugi::node_element) {
                destination.children.push_back(CopyXmlNode(child, depth + 1));
            }
        }
    }
    return destination;
}

// Parses a document with pugixml and returns its root element, or nullopt when
// malformed. load_buffer accepts the UTF-8 BOM used by some Riivolution packs.
inline std::optional<XmlNode> ParseXml(std::string_view text) {
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(
        text.data(), text.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        return std::nullopt;
    }

    const pugi::xml_node root = document.document_element();
    if (!root) {
        return std::nullopt;
    }
    return CopyXmlNode(root);
}

// ============================================================================
// Riivolution data model (mirrors Dolphin's DiscIO::Riivolution)
// ============================================================================

struct GameFilter {
    std::optional<std::string> game;
    std::optional<std::string> developer;
    std::optional<int> disc;
    std::optional<int> version;
    std::optional<std::vector<std::string>> regions;
};

struct PatchReference {
    std::string id;
    std::map<std::string, std::string> params;
};

struct Choice {
    std::string name;
    std::vector<PatchReference> patchReferences;
};

struct Option {
    std::string name;
    std::string id;
    std::vector<Choice> choices;

    // 1-based index into choices; 0 means disabled.
    uint32_t selectedChoice = 0;
};

struct Section {
    std::string name;
    std::vector<Option> options;
};

struct File {
    std::string disc;
    std::string external;
    bool resize = true;
    bool create = false;
    uint32_t offset = 0;
    uint32_t fileoffset = 0;
    uint32_t length = 0;
};

struct Folder {
    std::string disc;
    std::string external;
    bool resize = true;
    bool create = false;
    bool recursive = true;
    uint32_t length = 0;
};

struct Savegame {
    std::string external;
    bool clone = true;
};

// Parsed for completeness; the runtime never applies these (guest code and
// lowmem patching is the translator pipeline's job).
struct MemoryPatch {
    uint32_t offset = 0;
    std::string value;
    std::string valuefile;
    std::string original;
    bool ocarina = false;
    bool search = false;
    uint32_t align = 1;
};

struct Patch {
    std::string id;
    std::string root;
    std::vector<File> filePatches;
    std::vector<Folder> folderPatches;
    std::vector<Savegame> savegamePatches;
    std::vector<MemoryPatch> memoryPatches;
};

struct Disc {
    int version = 0;
    GameFilter gameFilter;
    std::vector<Section> sections;
    std::vector<Patch> patches;

    bool IsValidForGame(const std::string& gameId, std::optional<uint16_t> revision,
                        std::optional<uint8_t> discNumber) const;
    std::vector<Patch> GeneratePatches(const std::string& gameId) const;
};

// riivolution/config/<GameID4>.xml - remembered option choices.
struct ConfigOption {
    std::string id;
    uint32_t defaultChoice = 0;
};

struct Config {
    int version = 0;
    std::vector<ConfigOption> options;
};

// An option choice pinned by the distribution manifest (recomp.yml).
struct OptionSelection {
    std::string section; // empty = match any section
    std::string option;  // matches Option::id first, then Option::name
    uint32_t choice = 0;
};

// ============================================================================
// Parsing
// ============================================================================

namespace Detail {

inline std::map<std::string, std::string> ReadParams(const XmlNode& node,
                                                     std::map<std::string, std::string> params = {}) {
    for (const XmlNode& paramNode : node.children) {
        if (paramNode.name != "param") {
            continue;
        }
        params[paramNode.Attribute("name")] = paramNode.Attribute("value");
    }
    return params;
}

} // namespace Detail

inline std::optional<Disc> ParseString(std::string_view xml) {
    const std::optional<XmlNode> root = ParseXml(xml);
    if (!root || root->name != "wiidisc") {
        return std::nullopt;
    }

    Disc disc;
    disc.version = root->AttributeInt("version", -1);
    if (disc.version != 1) {
        return std::nullopt;
    }
    const std::string defaultRoot = root->Attribute("root");

    for (const XmlNode& node : root->children) {
        if (node.name == "id") {
            for (const auto& attribute : node.attributes) {
                if (attribute.first == "game") {
                    disc.gameFilter.game = attribute.second;
                } else if (attribute.first == "developer") {
                    disc.gameFilter.developer = attribute.second;
                } else if (attribute.first == "disc") {
                    disc.gameFilter.disc = node.AttributeInt("disc", -1);
                } else if (attribute.first == "version") {
                    disc.gameFilter.version = node.AttributeInt("version", -1);
                }
            }
            std::vector<std::string> regions;
            for (const XmlNode& regionNode : node.children) {
                if (regionNode.name == "region") {
                    regions.push_back(regionNode.Attribute("type"));
                }
            }
            if (!regions.empty()) {
                disc.gameFilter.regions = std::move(regions);
            }
            continue;
        }

        if (node.name == "options") {
            for (const XmlNode& sectionNode : node.children) {
                if (sectionNode.name != "section") {
                    continue;
                }
                Section section;
                section.name = sectionNode.Attribute("name");
                for (const XmlNode& optionNode : sectionNode.children) {
                    if (optionNode.name != "option") {
                        continue;
                    }
                    Option option;
                    option.id = optionNode.Attribute("id");
                    option.name = optionNode.Attribute("name");
                    option.selectedChoice = optionNode.AttributeUint("default", 0);
                    auto optionParams = Detail::ReadParams(optionNode);
                    for (const XmlNode& choiceNode : optionNode.children) {
                        if (choiceNode.name != "choice") {
                            continue;
                        }
                        Choice choice;
                        choice.name = choiceNode.Attribute("name");
                        auto choiceParams = Detail::ReadParams(choiceNode, optionParams);
                        for (const XmlNode& patchRefNode : choiceNode.children) {
                            if (patchRefNode.name != "patch") {
                                continue;
                            }
                            PatchReference patchReference;
                            patchReference.id = patchRefNode.Attribute("id");
                            patchReference.params = Detail::ReadParams(patchRefNode, choiceParams);
                            choice.patchReferences.push_back(std::move(patchReference));
                        }
                        option.choices.push_back(std::move(choice));
                    }
                    section.options.push_back(std::move(option));
                }
                disc.sections.push_back(std::move(section));
            }
            continue;
        }

        if (node.name == "patch") {
            Patch patch;
            patch.id = node.Attribute("id");
            patch.root = node.Attribute("root");
            if (patch.root.empty()) {
                patch.root = defaultRoot;
            }

            for (const XmlNode& patchNode : node.children) {
                if (patchNode.name == "file") {
                    File file;
                    file.disc = patchNode.Attribute("disc");
                    file.external = patchNode.Attribute("external");
                    file.resize = patchNode.AttributeBool("resize", true);
                    file.create = patchNode.AttributeBool("create", false);
                    file.offset = patchNode.AttributeUint("offset", 0);
                    file.fileoffset = patchNode.AttributeUint("fileoffset", 0);
                    file.length = patchNode.AttributeUint("length", 0);
                    patch.filePatches.push_back(std::move(file));
                } else if (patchNode.name == "folder") {
                    Folder folder;
                    folder.disc = patchNode.Attribute("disc");
                    folder.external = patchNode.Attribute("external");
                    folder.resize = patchNode.AttributeBool("resize", true);
                    folder.create = patchNode.AttributeBool("create", false);
                    folder.recursive = patchNode.AttributeBool("recursive", true);
                    folder.length = patchNode.AttributeUint("length", 0);
                    patch.folderPatches.push_back(std::move(folder));
                } else if (patchNode.name == "savegame") {
                    Savegame savegame;
                    savegame.external = patchNode.Attribute("external");
                    savegame.clone = patchNode.AttributeBool("clone", true);
                    patch.savegamePatches.push_back(std::move(savegame));
                } else if (patchNode.name == "memory") {
                    MemoryPatch memory;
                    memory.offset = patchNode.AttributeUint("offset", 0);
                    memory.value = patchNode.Attribute("value");
                    memory.valuefile = patchNode.Attribute("valuefile");
                    memory.original = patchNode.Attribute("original");
                    memory.ocarina = patchNode.AttributeBool("ocarina", false);
                    memory.search = patchNode.AttributeBool("search", false);
                    memory.align = patchNode.AttributeUint("align", 1);
                    patch.memoryPatches.push_back(std::move(memory));
                }
            }
            disc.patches.push_back(std::move(patch));
        }
    }

    return disc;
}

inline std::optional<Config> ParseConfigString(std::string_view xml) {
    const std::optional<XmlNode> root = ParseXml(xml);
    if (!root || root->name != "riivolution") {
        return std::nullopt;
    }

    Config config;
    config.version = root->AttributeInt("version", -1);
    if (config.version != 2) {
        return std::nullopt;
    }

    for (const XmlNode& optionNode : root->children) {
        if (optionNode.name != "option") {
            continue;
        }
        ConfigOption option;
        option.id = optionNode.Attribute("id");
        option.defaultChoice = optionNode.AttributeUint("default", 0);
        config.options.push_back(std::move(option));
    }

    return config;
}

// ============================================================================
// Game matching and patch generation
// ============================================================================

inline bool Disc::IsValidForGame(const std::string& gameId, std::optional<uint16_t> revision,
                                 std::optional<uint8_t> discNumber) const {
    if (gameId.size() != 6) {
        return false;
    }

    const std::string_view gameIdFull(gameId);
    const std::string_view gameRegion = gameIdFull.substr(3, 1);
    const std::string_view gameDeveloper = gameIdFull.substr(4, 2);
    const int discNumberInt = discNumber ? static_cast<int>(*discNumber) : -1;
    const int revisionInt = revision ? static_cast<int>(*revision) : -1;

    if (gameFilter.game && !XmlDetail::StartsWith(gameIdFull, *gameFilter.game)) {
        return false;
    }
    if (gameFilter.developer && gameDeveloper != *gameFilter.developer) {
        return false;
    }
    if (gameFilter.disc && discNumberInt != *gameFilter.disc) {
        return false;
    }
    if (gameFilter.version && revisionInt != *gameFilter.version) {
        return false;
    }
    if (gameFilter.regions) {
        const auto& regions = *gameFilter.regions;
        if (!regions.empty() &&
            std::find(regions.begin(), regions.end(), std::string(gameRegion)) == regions.end()) {
            return false;
        }
    }

    return true;
}

inline std::vector<Patch> Disc::GeneratePatches(const std::string& gameId) const {
    const std::string_view gameIdFull(gameId);
    const std::string_view gameIdNoRegion = gameIdFull.substr(0, 3);
    const std::string_view gameRegion = gameIdFull.substr(3, 1);
    const std::string_view gameDeveloper = gameIdFull.size() >= 6 ? gameIdFull.substr(4, 2) : std::string_view();

    const auto replaceVariables =
        [](std::string_view sv, const std::vector<std::pair<std::string, std::string_view>>& replacements) {
            std::string result;
            result.reserve(sv.size());
            while (!sv.empty()) {
                bool replaced = false;
                for (const auto& replacement : replacements) {
                    if (XmlDetail::StartsWith(sv, replacement.first)) {
                        result.append(replacement.second.data(), replacement.second.size());
                        sv = sv.substr(replacement.first.size());
                        replaced = true;
                        break;
                    }
                }
                if (replaced) {
                    continue;
                }
                result.push_back(sv[0]);
                sv = sv.substr(1);
            }
            return result;
        };

    // Take only selected patches, replace placeholders in all strings, and
    // return them.
    std::vector<Patch> activePatches;
    for (const Section& section : sections) {
        for (const Option& option : section.options) {
            const uint32_t selected = option.selectedChoice;
            if (selected == 0 || selected > option.choices.size()) {
                continue;
            }
            const Choice& choice = option.choices[selected - 1];
            for (const PatchReference& patchReference : choice.patchReferences) {
                const auto patch = std::find_if(patches.begin(), patches.end(), [&](const Patch& candidate) {
                    return candidate.id == patchReference.id;
                });
                if (patch == patches.end()) {
                    continue;
                }

                std::vector<std::pair<std::string, std::string_view>> replacements;
                replacements.emplace_back("{$__gameid}", gameIdNoRegion);
                replacements.emplace_back("{$__region}", gameRegion);
                replacements.emplace_back("{$__maker}", gameDeveloper);
                for (const auto& param : patchReference.params) {
                    replacements.emplace_back("{$" + param.first + "}", param.second);
                }

                Patch newPatch = *patch;
                newPatch.root = replaceVariables(newPatch.root, replacements);
                for (File& file : newPatch.filePatches) {
                    file.disc = replaceVariables(file.disc, replacements);
                    file.external = replaceVariables(file.external, replacements);
                }
                for (Folder& folder : newPatch.folderPatches) {
                    folder.disc = replaceVariables(folder.disc, replacements);
                    folder.external = replaceVariables(folder.external, replacements);
                }
                for (Savegame& savegame : newPatch.savegamePatches) {
                    savegame.external = replaceVariables(savegame.external, replacements);
                }
                for (MemoryPatch& memory : newPatch.memoryPatches) {
                    memory.valuefile = replaceVariables(memory.valuefile, replacements);
                }
                activePatches.push_back(std::move(newPatch));
            }
        }
    }

    return activePatches;
}

// ============================================================================
// Option selection
// ============================================================================

// Dolphin's config identifier: an option is addressed by its id when it has
// one, otherwise by the concatenation of section name and option name.
inline void ApplyConfigDefaults(Disc& disc, const Config& config) {
    for (const ConfigOption& configOption : config.options) {
        for (Section& section : disc.sections) {
            for (Option& option : section.options) {
                const bool matches = option.id.empty()
                    ? (section.name + option.name) == configOption.id
                    : option.id == configOption.id;
                if (matches) {
                    option.selectedChoice = configOption.defaultChoice;
                }
            }
        }
    }
}

// Applies distribution-pinned selections. Runs after ApplyConfigDefaults so a
// pin always wins over the user's remembered choice.
inline void ApplySelections(Disc& disc, const std::vector<OptionSelection>& selections) {
    for (const OptionSelection& selection : selections) {
        for (Section& section : disc.sections) {
            if (!selection.section.empty() && section.name != selection.section) {
                continue;
            }
            for (Option& option : section.options) {
                const bool matches = (!option.id.empty() && option.id == selection.option) ||
                                     option.name == selection.option;
                if (matches && selection.choice <= option.choices.size()) {
                    option.selectedChoice = selection.choice;
                }
            }
        }
    }
}

// External path resolution (Dolphin FileDataLoaderHostFS semantics). A leading '/' is absolute
// (relative to the SD card root); otherwise relative to the patch root (the XML's folder, or
// its 'root' attribute override). All paths use '/' separators (callers convert host paths via
// generic_string() first); returns nullopt for ".." traversal or a backslash, which Riivolution
// treats as a filename character that Windows paths can't replicate.

inline std::optional<std::string> MakeAbsoluteFromRelative(std::string_view sdRoot,
                                                           std::string_view patchRoot,
                                                           std::string_view externalRelativePath) {
    if (externalRelativePath.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }

    const bool absolute = !externalRelativePath.empty() && externalRelativePath[0] == '/';
    std::string result(absolute ? sdRoot : patchRoot);
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }

    std::string_view work = externalRelativePath;
    while (!work.empty() && work.front() == '/') {
        work.remove_prefix(1);
    }
    while (!work.empty() && work.back() == '/') {
        work.remove_suffix(1);
    }

    size_t depth = 0;
    while (!work.empty()) {
        const size_t separator = work.find('/');
        const std::string_view element = work.substr(0, separator);

        if (element == ".") {
            // Harmless, changes nothing.
        } else if (element == "..") {
            // Going up a level; never above the root.
            if (depth == 0) {
                return std::nullopt;
            }
            --depth;
            const size_t lastSlash = result.rfind('/');
            if (lastSlash == std::string::npos) {
                return std::nullopt;
            }
            result.resize(lastSlash);
        } else if (!element.empty()) {
            ++depth;
            result.push_back('/');
            result.append(element.data(), element.size());
        }

        if (separator == std::string_view::npos) {
            break;
        }
        work.remove_prefix(separator + 1);
    }

    return result;
}

// Computes a patch's effective root directory from the XML file's directory
// and the patch's 'root' attribute.
inline std::string ResolvePatchRoot(std::string_view sdRoot, std::string_view xmlDirectory,
                                    std::string_view rootAttribute) {
    std::string patchRoot(xmlDirectory);
    if (!rootAttribute.empty()) {
        if (auto resolved = MakeAbsoluteFromRelative(sdRoot, xmlDirectory, rootAttribute)) {
            patchRoot = std::move(*resolved);
        }
    }
    return patchRoot;
}

// First <savegame> across the active patches, in order (Dolphin
// ExtractSavegameRedirect).
inline const Savegame* FindSavegamePatch(const std::vector<Patch>& activePatches) {
    for (const Patch& patch : activePatches) {
        if (!patch.savegamePatches.empty()) {
            return &patch.savegamePatches[0];
        }
    }
    return nullptr;
}

} // namespace RiivolutionContract
