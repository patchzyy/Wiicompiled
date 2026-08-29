#pragma once

#include <filesystem>
#include <string>
#include <string_view>

/**
 * Narrow path strings crossing the aurora boundary are UTF-8. path::string() and the
 * char path constructor go through the ANSI codepage on Windows, so they must not be
 * used for anything the host handed us or hands back to SDL, sqlite or ImGui.
 */
inline std::string fs_path_to_string(const std::filesystem::path& path) {
  const auto u8str = path.u8string();
  return { reinterpret_cast<const char*>(u8str.c_str()), u8str.size() };
}

inline std::filesystem::path fs_path_from_string(std::string_view utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
