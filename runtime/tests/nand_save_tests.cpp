#include "nand_save_probe.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using RuntimeNandSave::ReadAction;
using RuntimeNandSave::Contents;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
    output.close();
    Require(static_cast<bool>(output), "Fixture write failed");
}

static std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// A disk error after zero-filled blocks must not look like a blank file's EOF.
class FailingDisk : public std::streambuf {
    int blocks;
public:
    explicit FailingDisk(int zeroBlocks) : blocks(zeroBlocks) {}
    std::streamsize xsgetn(char* buffer, std::streamsize length) override {
        if (blocks-- <= 0) throw std::runtime_error("injected read failure");
        std::fill(buffer, buffer + length, '\0');
        return length;
    }
};

int main() {
    const auto root = fs::temp_directory_path() / ("wiicomp-save-scenarios-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto save = root / "title/00010004/524d4350/data/rksys.dat";
        const auto shadow = fs::path(save.native() + fs::path(".nandsafe.tmp").native());
        // Save inspection must leave unrelated NAND data alone. Settings
        // initialization is covered separately by nand_settings_tests.
        const auto settingsPath = root / "title/00000001/00000002/data/setting.txt";
        const std::string identity(256, '\x5a');
        Write(settingsPath, identity);
        Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::Proceed, "Fresh profile follows normal missing-file handling");
        Require(!fs::exists(save), "Probing fresh profile must not create a save");

        // First launch interrupted before save initialization, including block
        // boundaries and a full-sized synthetic zero-filled allocation.
        for (const size_t size : {size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097), size_t(3 * 1024 * 1024)}) {
            const std::string bytes(size, '\0');
            Write(save, bytes);
            Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::Missing, "Blank save should be offered first-save recovery");
            Require(Read(save) == bytes, "Blank-save detection must not modify the file");
            for (int mode : {2, 3}) {
                Require(RuntimeNandSave::CheckRead(save, mode) == ReadAction::Proceed, "Write opens must remain available for initialization");
            }
        }

        // Existing saves, imported saves, partial/corrupt saves, and a zero
        // prefix with data only in the final byte are all left to the game.
        std::string existing(3 * 1024 * 1024, '\0');
        existing.replace(0, 8, "RKSD0006");
        existing[10000] = 42;
        for (const std::string& bytes : {existing, std::string("RKSD"), std::string("damaged-header"),
                 std::string(8192, '\0') + "x", std::string(8191, '\0') + "x"}) {
            Write(save, bytes);
            Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::Proceed, "Never hide a save containing any data");
            Require(Read(save) == bytes, "Existing/partial save must be byte-identical after inspection");
        }

        // Interrupted replacement: retain a committed original regardless of
        // whether the shadow is blank, partial, or contains a complete header.
        Write(save, existing);
        for (const std::string& bytes : {std::string(), std::string(4096, '\0'), std::string("RKSD"), existing}) {
            Write(shadow, bytes);
            Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::Proceed, "Committed original takes precedence over write shadow");
            Require(Read(save) == existing && Read(shadow) == bytes, "Probe must preserve both sides of an interrupted write");
        }
        // No usable original: do not let missing-save recovery discard the
        // only possible recovery source, and do not auto-promote that shadow.
        for (const bool mainExists : {false, true}) {
            fs::remove(save);
            if (mainExists) Write(save, std::string(4096, '\0'));
            Write(shadow, existing);
            Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::RecoveryNeeded, "Preserve recovery candidate when original is missing or blank");
            Require(Read(shadow) == existing, "Recovery candidate must remain unchanged");
            Require(fs::exists(save) == mainExists, "Do not promote shadow automatically");
        }
        Write(shadow, std::string(4096, '\0'));
        Require(RuntimeNandSave::CheckRead(save, 1) == ReadAction::Missing, "Two blank files may use first-save recovery");
        fs::remove(shadow);

        for (const char* name : {"rksys.dat.bak", "rksys.dat.backup", "rksys.dat2", "banner.bin", "setting.txt"}) {
            const auto unrelated = save.parent_path() / name;
            Write(unrelated, std::string(4096, '\0'));
            Require(RuntimeNandSave::CheckRead(unrelated, 1) == ReadAction::Proceed, "Do not classify backups or unrelated files as missing saves");
        }
        for (int blocks : {0, 1, 2}) {
            FailingDisk disk(blocks);
            std::istream input(&disk);
            Require(RuntimeNandSave::InspectStream(input) == Contents::Error, "Read failure must remain an error, including after zero-filled blocks");
        }
        std::istringstream badEof;
        badEof.setstate(std::ios::badbit | std::ios::eofbit);
        Require(RuntimeNandSave::InspectStream(badEof) == Contents::Error, "Badbit plus EOF must not imply a blank save");

#ifdef _WIN32
        Write(save, existing);
        const HANDLE locked = CreateFileW(save.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "Could not lock fixture");
        const auto lockedResult = RuntimeNandSave::CheckRead(save, 1);
        CloseHandle(locked);
        Require(lockedResult == ReadAction::Error, "Sharing/access failure must not report a missing save");
        Require(Read(save) == existing, "Locked save must survive inspection unchanged");
        Require(SetFileAttributesW(save.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "Set fixture read-only");
        const auto readOnlyResult = RuntimeNandSave::CheckRead(save, 1);
        SetFileAttributesW(save.c_str(), FILE_ATTRIBUTE_NORMAL);
        Require(readOnlyResult == ReadAction::Proceed && Read(save) == existing, "Readable read-only save remains available");
#endif
        Require(Read(settingsPath) == identity, "Save inspection must not change NAND settings");
        fs::remove_all(root);
        std::cout << "NAND save startup, preservation, interrupted-write and I/O failure scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (fixtures retained at " << root << ")\n";
        return 1;
    }
}
