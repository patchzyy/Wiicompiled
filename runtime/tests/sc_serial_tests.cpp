#include "sc_serial_contract.h"
#include "nand_settings.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static uint32_t ReadWord(const unsigned char* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

int main(int argc, char** argv) {
    try {
        // Feed real generator + SC ABI outputs to the upstream bot decoder.
        // Usage: mkw_sc_serial_tests --timestamp-vectors <unix-seconds> ...
        if (argc > 1 && std::string(argv[1]) == "--timestamp-vectors") {
            for (int i = 2; i < argc; ++i) {
                const auto timestamp = std::stoll(argv[i]);
                const auto serial = RuntimeNandSettings::GenerateSerial(static_cast<std::time_t>(timestamp));
                std::array<unsigned char, 4> output{};
                Require(RuntimeScSerial::Write(serial, 4,
                    [](uint32_t address, size_t size) { return address == 4 && size == 4; },
                    [&](uint32_t, uint32_t value) {
                        for (unsigned j = 0; j < 4; ++j)
                            output[j] = static_cast<unsigned char>(value >> (24 - 8 * j));
                    }) == 1, "Generated serial must pass SC ABI");
                std::cout << timestamp << '\t' << serial << "\tLEH"
                          << std::setfill('0') << std::setw(9) << ReadWord(output.data()) << '\n';
            }
            return 0;
        }
        // Reproduce the reported csnums from the old string-writing override.
        const unsigned char old7886[] = {'7', '8', '8', '6'};
        const unsigned char old7618[] = {'7', '6', '1', '8'};
        Require(ReadWord(old7886) == 926431286, "Reproduce shared LEH926431286");
        Require(ReadWord(old7618) == 926298424, "Reproduce shared LEH926298424");

        std::array<unsigned char, 16> memory;
        size_t available = 4;
        unsigned writes = 0;
        const auto contains = [&](uint32_t address, size_t size) {
            return address == 4 && size <= available;
        };
        const auto write32 = [&](uint32_t address, uint32_t value) {
            ++writes;
            for (unsigned i = 0; i < 4; ++i)
                memory[address + i] = static_cast<unsigned char>(value >> (24 - 8 * i));
        };
        for (const auto& pair : {std::pair{"788600001", 788600001u}, {"788699999", 788699999u},
                 {"761800001", 761800001u}, {"761899999", 761899999u},
                 {"012345678", 12345678u}, {"000000001", 1u}, {"999999999", 999999999u}}) {
            memory.fill(0xa5);
            writes = 0;
            Require(RuntimeScSerial::Write(pair.first, 4, contains, write32) == 1, "Accept an exactly four-byte output buffer");
            Require(writes == 1 && ReadWord(memory.data() + 4) == pair.second, "Return full numeric serial, including digits after common prefix");
            for (size_t i = 0; i < memory.size(); ++i)
                if (i < 4 || i >= 8) Require(memory[i] == 0xa5, "Do not overwrite adjacent guest stack data");
        }
        for (const char* serial : {"", "1234567890", "7886x1234", "-12345678", "+12345678"}) {
            writes = 0;
            Require(RuntimeScSerial::Write(serial, 4, contains, write32) == 0 && writes == 0, "Reject malformed serial without a write");
        }
        writes = 0;
        Require(RuntimeScSerial::Write("788600001", 0, contains, write32) == 0 && writes == 0, "Reject null output");
        available = 3;
        Require(RuntimeScSerial::Write("788600001", 4, contains, write32) == 0 && writes == 0, "Reject undersized output");
        std::cout << "SC serial collision reproduction, numeric output and memory-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
