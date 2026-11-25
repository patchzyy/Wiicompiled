#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

class Memory {
public:
    class AccessViolation : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    static uint8_t* GetPointer(uint32_t address, size_t length);
};
