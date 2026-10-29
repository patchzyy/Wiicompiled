#include "runtime_config.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
std::optional<toml::ordered_value> Update(const std::string& original, const char* section, const char* key,
                                          const char* value, std::string* text = nullptr) {
    const auto updated = RuntimeConfigFile::UpdateSettingText(original, section, key, value);
    if (!updated) return std::nullopt;
    if (text) *text = *updated;
    return toml::parse_str<toml::ordered_type_config>(*updated);
}
bool SetsFps(const std::string& original, const char* message) {
    const auto document = Update(original, "video", "show_fps", "true");
    return Require(document && document->at("video").at("show_fps").as_boolean() &&
                       document->at("video").as_table().size() == 1,
                   message);
}
bool SetsOffset(const std::string& original, const char* message) {
    const auto document = Update(original, "controller.wii_accel_offsets", "x", "0.5");
    return Require(document && document->at("controller").at("wii_accel_offsets").at("x").as_floating() == 0.5,
                   message);
}
} // namespace

int main() {
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    {
        std::istringstream input("[video]\nwidescreen = false\nforce_16_9 = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "legacy.toml");
        if (!Require(config.forceAspect == std::optional<bool>{true} &&
                         config.widescreen == std::optional<bool>{true} && config.legacyForceAspect,
                     "legacy force_16_9 did not force a 16:9 game")) return 1;
    }
    {
        std::istringstream input("[video]\nwidescreen = false\nforce_aspect = true\nforce_16_9 = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "migrated.toml");
        if (!Require(config.forceAspect == std::optional<bool>{true} &&
                         config.widescreen == std::optional<bool>{false} && !config.legacyForceAspect,
                     "legacy force_16_9 overrode force_aspect")) return 1;
    }
    {
        std::istringstream input("[controller]\nmotion_trick = true\nmotion_trick_sensitivity = 5\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "motion.toml");
        if (!Require(config.motionTrick == std::optional<bool>{true} &&
                         config.motionTrickSensitivity == std::optional<int32_t>{5},
                     "shake to trick settings were not loaded")) return 1;
    }
    {
        std::istringstream input("[controller]\nmotion_trick_sensitivity = 9\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "motion-range.toml");
        if (!Require(!config.motionTrick && !config.motionTrickSensitivity,
                     "out of range shake to trick sensitivity was accepted")) return 1;
    }
    {
        std::istringstream input("[controller]\nsteam_input_override = false\nsteam_input_app_id = 1234\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "steam.toml");
        if (!Require(config.steamInputOverride == std::optional<bool>{false} &&
                         config.steamInputAppId == std::optional<uint32_t>{1234},
                     "Steam Input settings were not loaded")) return 1;
    }
    {
        std::istringstream input("[controller]\nsteam_input_app_id = 0\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "steam-zero.toml");
        if (!Require(!config.steamInputAppId, "zero Steam Input app id was accepted")) return 1;
    }
    if (!SetsFps("[video]\nshow_fps = false\n", "bare key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show_fps\" = false\n", "basic-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n'show_fps'=false\n", "literal-quoted key was not replaced")) return 1;
    if (!SetsFps("[video]\n\"show\\u005Ffps\" = false\n", "escaped key was not replaced")) return 1;
    if (!SetsFps("[\"video\"]\n  show_fps = false\n", "quoted table header was not matched")) return 1;
    if (!SetsFps("[video]\nshow_fps = [\n  1,\n]\n", "multi-line value was not replaced")) return 1;
    if (!SetsFps("[video]\n", "missing key was not created")) return 1;
    if (!SetsFps("", "missing table was not created")) return 1;
    {
        std::string text;
        const auto document = Update("# top\nfoo = 1\n\n[video]\nzzz = 2 # keep\nshow_fps = false # note\naaa = 3\n\n"
                                     "[audio]\nshow_fps = 1\n",
                                     "video", "show_fps", "true", &text);
        if (!Require(document && document->at("foo").as_integer() == 1 &&
                         document->at("audio").at("show_fps").as_integer() == 1 &&
                         document->at("video").at("zzz").as_integer() == 2 &&
                         document->at("video").at("aaa").as_integer() == 3 &&
                         document->at("video").at("show_fps").as_boolean(),
                     "unrelated settings were not preserved")) return 1;
        const size_t zzz = text.find("zzz"), fps = text.find("show_fps"), aaa = text.find("aaa");
        if (!Require(text.find("# top") != std::string::npos && text.find("# keep") != std::string::npos &&
                         text.find("# note") != std::string::npos && zzz < fps && fps < aaa,
                     "comments or key order were not preserved")) return 1;
    }
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video]\nshow_fps = 1\nshow_fps = 2\n", "video", "show_fps", "true"),
                 "a document with a duplicate key was rewritten")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("[video\nshow_fps = 1\n", "video", "show_fps", "true"),
                 "an unparseable document was rewritten")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n\n[controller.wii_accel_offsets]\nx = 0.1\ny = 2\n",
                    "key in an existing nested table was not replaced")) return 1;
    if (!SetsOffset("[controller.wii_accel_offsets]\ny = 2\n", "key was not added to a nested table")) return 1;
    if (!SetsOffset("[controller]\nrumble = true\n", "missing nested table was not created")) return 1;
    if (!SetsOffset("[controller.\"wii_accel_offsets\"]\nx = 0.1\n", "quoted nested header was not matched")) return 1;
    if (!SetsOffset("[controller]\nwii_accel_offsets.y = 2\n", "dotted-key table was not extended")) return 1;
    if (!SetsOffset("controller.wii_accel_offsets.y = 2\n", "root-level dotted table was not extended")) return 1;
    if (!Require(!RuntimeConfigFile::UpdateSettingText("controller = \"custom\"\n", "controller.wii_accel_offsets",
                                                       "x", "0.5"),
                 "a scalar was replaced by a table")) return 1;
    {
        const auto document = Update("[video.advanced]\nfoo = 1\n", "video", "show_fps", "true");
        if (!Require(document && document->at("video").at("show_fps").as_boolean() &&
                         document->at("video").at("advanced").at("foo").as_integer() == 1,
                     "key was not added to an implicit table")) return 1;
    }
    return 0;
}
