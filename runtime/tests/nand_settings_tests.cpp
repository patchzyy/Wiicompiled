#include "nand_settings.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

static void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("NAND settings check failed");
    }
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("wiicomp-nand-settings-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / "title/00000001/00000002/data/setting.txt";
    try {
        Require(!RuntimeNandSettings::Read(root));
        Require(!std::filesystem::exists(root));
        std::filesystem::create_directories(path.parent_path());
        const std::string plain = "AREA=USA\r\n\nCODE=LU\r\nSERNO=987654321\r\nGAME=US\r\n";
        std::array<uint8_t, 256> fixture{};
        for (size_t i = 0; i < fixture.size(); ++i) {
            const unsigned shift = i % 32;
            const uint32_t key = shift == 0 ? 0x73B5DBFAu :
                (0x73B5DBFAu << shift) | (0x73B5DBFAu >> (32 - shift));
            fixture[i] = static_cast<uint8_t>(key) ^ (i < plain.size() ? plain[i] : 0);
        }
        {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(fixture.data()), fixture.size());
        }
        auto settings = RuntimeNandSettings::Read(root);
        Require(settings && RuntimeNandSettings::HasIdentity(*settings));
        Require(settings->at("SERNO") == "987654321" && settings->at("CODE") == "LU");
        Require(settings->at("AREA") == "USA" && settings->at("GAME") == "US");
        std::array<uint8_t, 256> after{};
        {
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char*>(after.data()), after.size());
        }
        Require(after == fixture);
        for (const auto serial : {"", "000000000", "1234567890", "123ABC789"}) {
            (*settings)["SERNO"] = serial;
            Require(!RuntimeNandSettings::HasIdentity(*settings));
        }
        (*settings)["SERNO"] = "012345678";
        Require(RuntimeNandSettings::HasIdentity(*settings));
        (*settings)["CODE"] = "TOOLONG";
        Require(!RuntimeNandSettings::HasIdentity(*settings));
        (*settings)["CODE"] = "LEH";
        settings->erase("GAME");
        Require(!RuntimeNandSettings::HasIdentity(*settings));
        std::filesystem::resize_file(path, 128);
        Require(!RuntimeNandSettings::Read(root));
        std::filesystem::remove_all(root);
        std::cout << "NAND settings checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
