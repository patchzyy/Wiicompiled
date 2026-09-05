#include "touch_art.h"

#include <aurora/imgui.h>
#include <png.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "runtime_config.h"

// Icons are CC BY 3.0 by Zacksly; see runtime/assets/touch/ATTRIBUTION.txt.
// Textures are owned by aurora once handed over and live for the process.
namespace TouchArt {
namespace {

std::unordered_map<std::string, ImTextureID> g_cache;

// libpng's error path longjmps, so everything it can touch must be released
// through this struct rather than by falling off the end of the function.
struct PngReader {
    std::FILE* file = nullptr;
    png_structp png = nullptr;
    png_infop info = nullptr;
    std::vector<png_bytep> rows;
    ~PngReader() {
        if (png != nullptr) png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, nullptr);
        if (file != nullptr) std::fclose(file);
    }
};

bool DecodeRgba8(const std::filesystem::path& path, std::vector<uint8_t>& out, uint32_t& width,
                 uint32_t& height) {
    PngReader r;
    r.file = std::fopen(path.string().c_str(), "rb");
    if (r.file == nullptr) {
        return false;
    }
    r.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (r.png == nullptr) return false;
    r.info = png_create_info_struct(r.png);
    if (r.info == nullptr) return false;
    if (setjmp(png_jmpbuf(r.png))) return false;

    png_init_io(r.png, r.file);
    png_read_info(r.png, r.info);

    png_set_expand(r.png);
    png_set_strip_16(r.png);
    png_set_gray_to_rgb(r.png);
    png_set_filler(r.png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(r.png, r.info);

    width = png_get_image_width(r.png, r.info);
    height = png_get_image_height(r.png, r.info);
    if (width == 0 || height == 0) return false;

    out.assign(static_cast<size_t>(width) * height * 4u, 0);
    r.rows.resize(height);
    for (uint32_t y = 0; y < height; ++y) {
        r.rows[y] = out.data() + static_cast<size_t>(y) * width * 4u;
    }
    png_read_image(r.png, r.rows.data());
    return true;
}

} // namespace

ImTextureID Get(const char* name) {
    if (name == nullptr || *name == '\0') {
        return ImTextureID{};
    }
    if (const auto it = g_cache.find(name); it != g_cache.end()) {
        return it->second;
    }

    ImTextureID id{};
    if (const auto dir = RuntimeConfigFile::ExecutableDirectory()) {
        const std::filesystem::path path = *dir / "touch" / (std::string(name) + ".png");
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        if (DecodeRgba8(path, rgba, w, h)) {
            id = aurora_imgui_add_texture(w, h, rgba.data());
        } else {
            // Cached null so this reports once rather than every frame.
            std::cout << "[touch] missing button art: " << path << std::endl;
        }
    }
    g_cache.emplace(name, id);
    return id;
}

void Preload() {
    // Must match the names used in touch_pad.cpp; a miss costs a hitch, not a
    // failure.
    static constexpr const char* kNames[] = {
        "a", "b", "right_bumper", "l_analog", "r_analog", "start_pause", "d-pad", "control_stick",
    };
    for (const char* name : kNames) {
        Get(name);
        Get((std::string(name) + "_fill").c_str());
    }
}

} // namespace TouchArt
