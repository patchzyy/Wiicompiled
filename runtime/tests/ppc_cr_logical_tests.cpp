#include "isa/ppc_isa_cr.h"

#include <array>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckLogical() {
    constexpr uint32_t truth[8][4] = {
        {1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {1, 1, 1, 0},
        {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 1, 1}, {0, 1, 1, 1}
    };
    for (uint32_t op = 0; op < 8; ++op)
    for (uint32_t ba = 0; ba < 32; ++ba)
    for (uint32_t bb = 0; bb < 32; ++bb)
    for (uint32_t bt = 0; bt < 32; ++bt)
    for (uint32_t seed : {0u, 0xFFFFFFFFu, 0xA5C39678u, 0x5A3C6987u})
    for (uint32_t bits = 0; bits < 4; ++bits) {
        const uint32_t aMask = 1u << (31u - ba);
        const uint32_t bMask = 1u << (31u - bb);
        uint32_t initial = (seed & ~aMask) | ((bits & 2u) ? aMask : 0u);
        initial = (initial & ~bMask) | ((bits & 1u) ? bMask : 0u);
        const uint32_t a = (initial >> (31u - ba)) & 1u;
        const uint32_t b = (initial >> (31u - bb)) & 1u;
        const uint32_t mask = 1u << (31u - bt);
        const uint32_t expected = (initial & ~mask) | (truth[op][a * 2u + b] ? mask : 0u);
        Require(PpcCrLogicalResident(initial, op, bt, ba, bb) == expected, "resident CR logical truth table");
    }
}

void CheckNorCr0() {
    struct Case { uint32_t left, right, result, cr0; };
    constexpr Case cases[] = {
        {0u, 0u, 0xFFFFFFFFu, 8u},
        {0x80000000u, 0u, 0x7FFFFFFFu, 4u},
        {0xFFFFFFFFu, 0u, 0u, 2u},
        {0x7FFFFFFFu, 0u, 0x80000000u, 8u},
        {0x80000000u, 0x7FFFFFFEu, 1u, 4u},
        {0xAAAAAAAAu, 0x55555555u, 0u, 2u}
    };
    for (const auto& test : cases)
    for (uint32_t xer : {0u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
        const uint32_t result = ~(test.left | test.right);
        Require(result == test.result, "nor result");
        uint32_t cr = 0xF5A39678u;
        SetCRResident(cr, xer, 0, static_cast<int32_t>(result), int32_t{0});
        const uint32_t expected = ((test.cr0 | (xer >> 31)) << 28) | 0x05A39678u;
        Require(cr == expected, "nor CR0 LT/GT/EQ/SO and other fields");
        Require(GetCRBitResident(cr, 0, 0) == (test.cr0 == 8u), "nor LT branch");
        Require(GetCRBitResident(cr, 0, 1) == (test.cr0 == 4u), "nor GT branch");
        Require(GetCRBitResident(cr, 0, 2) == (test.cr0 == 2u), "nor EQ branch");
        Require(GetCRBitResident(cr, 0, 3) == ((xer >> 31) != 0), "nor SO branch");
    }
}

}

int main() {
    try {
        CheckLogical();
        CheckNorCr0();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
