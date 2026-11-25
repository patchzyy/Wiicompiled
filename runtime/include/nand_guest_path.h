#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace RuntimeNandGuestPath {

// A guest path component may only ever name an entry directly inside the directory it is
// appended to. ':' is refused on every host so a profile behaves the same everywhere: on
// Windows it forms a drive prefix ("C:"), which std::filesystem appends by *replacing* the
// path built so far, and an NTFS stream name ("file:stream").
inline bool IsSafeComponent(std::string_view component) {
    if (component.empty() || component == "." || component == "..") {
        return false;
    }
    if (component.find_first_of(":/\\") != std::string_view::npos ||
        component.find('\0') != std::string_view::npos) {
        return false;
    }
    // Belt and braces for any other host spelling of a root.
    const std::filesystem::path part{std::string(component)};
    return !part.has_root_name() && !part.has_root_directory();
}

// Appends a '/'-separated guest path below `root`, one component at a time. Empty
// components (leading, trailing or doubled slashes) are skipped; nullopt when any
// component could leave `root`.
inline std::optional<std::filesystem::path> AppendBelowRoot(const std::filesystem::path& root,
                                                            std::string_view guestPath) {
    std::filesystem::path hostPath = root;
    size_t cursor = 0;
    while (cursor < guestPath.size()) {
        const size_t slash = guestPath.find('/', cursor);
        const size_t end = slash == std::string_view::npos ? guestPath.size() : slash;
        if (end != cursor) {
            const std::string_view component = guestPath.substr(cursor, end - cursor);
            if (!IsSafeComponent(component)) {
                return std::nullopt;
            }
            hostPath /= std::filesystem::path{std::string(component)};
        }
        cursor = end + 1;
    }
    return hostPath;
}

} // namespace RuntimeNandGuestPath
