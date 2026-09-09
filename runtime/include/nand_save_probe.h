#pragma once

#include <filesystem>
#include <fstream>
#include <istream>

namespace RuntimeNandSave {

enum class Contents { Missing, Blank, Nonzero, Error };
enum class ReadAction { Proceed, Missing, Error, RecoveryNeeded };

// A failed read is not evidence that a save is blank. Check badbit before EOF:
// an I/O failure may set both, whereas a successful short final read sets EOF.
inline Contents InspectStream(std::istream& input) {
    if (!input) return Contents::Error;
    char block[4096];
    for (;;) {
        input.read(block, sizeof(block));
        if (input.bad() || (input.fail() && !input.eof())) return Contents::Error;
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            if (block[i] != 0) return Contents::Nonzero;
        }
        if (input.eof()) return Contents::Blank;
    }
}

inline Contents InspectFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return Contents::Error;
    if (!std::filesystem::exists(status)) return Contents::Missing;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return Contents::Error;
    std::ifstream input(path, std::ios::binary);
    return InspectStream(input);
}

// Probe only read-only opens of the actual save and its exact write shadow.
// No probe writes, removes, or repairs data, and backups are not save aliases.
inline ReadAction CheckRead(const std::filesystem::path& path, int mode) {
    const auto name = path.filename();
    const bool isMain = name == "rksys.dat";
    if (mode != 1 || (!isMain && name != "rksys.dat.nandsafe.tmp")) return ReadAction::Proceed;
    const auto contents = InspectFile(path);
    if (contents == Contents::Error) return ReadAction::Error;
    if (contents == Contents::Nonzero) return ReadAction::Proceed;
    if (isMain) {
        auto shadow = path;
        shadow += ".nandsafe.tmp";
        const auto shadowContents = InspectFile(shadow);
        if (shadowContents == Contents::Error) return ReadAction::Error;
        // The next write normally discards an old shadow. Preserve a possible
        // recovery source when there is no usable original, without promoting
        // an uncommitted (and potentially incomplete) shadow to the real save.
        if (shadowContents == Contents::Nonzero) return ReadAction::RecoveryNeeded;
    }
    return contents == Contents::Blank ? ReadAction::Missing : ReadAction::Proceed;
}

} // namespace RuntimeNandSave
