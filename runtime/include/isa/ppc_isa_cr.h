#pragma once

#include <cmath>
#include <cstdint>

inline void SetCRResident(uint32_t& cr, uint32_t xer, int field, int32_t a, int32_t b) noexcept {
    uint32_t value = (a < b ? 0x8u : 0u) | (a > b ? 0x4u : 0u) | (a == b ? 0x2u : 0u) | ((xer >> 31) & 1u);
    const int shift = (7 - field) * 4;
    cr = (cr & ~(0xFu << shift)) | (value << shift);
}
inline void SetCRResident(uint32_t& cr, uint32_t xer, int field, uint32_t a, uint32_t b) noexcept {
    uint32_t value = (a < b ? 0x8u : 0u) | (a > b ? 0x4u : 0u) | (a == b ? 0x2u : 0u) | ((xer >> 31) & 1u);
    const int shift = (7 - field) * 4;
    cr = (cr & ~(0xFu << shift)) | (value << shift);
}
inline void SetCRFloatResident(uint32_t& cr, int field, double a, double b) noexcept {
    uint32_t value = (std::isnan(a) || std::isnan(b)) ? 0x1u :
        ((a < b ? 0x8u : 0u) | (a > b ? 0x4u : 0u) | (a == b ? 0x2u : 0u));
    const int shift = (7 - field) * 4;
    cr = (cr & ~(0xFu << shift)) | (value << shift);
}


inline bool GetCRBitResident(uint32_t cr, int field, int bit) noexcept {
    const int shift = (7 - field) * 4 + (3 - bit);
    return ((cr >> shift) & 1u) != 0u;
}

inline uint32_t PpcCrSetBitResident(uint32_t cr, uint32_t bitIndex, uint32_t value) noexcept {
    const uint32_t mask = 1u << (31u - (bitIndex & 31u));
    return (value & 1u) != 0 ? (cr | mask) : (cr & ~mask);
}

inline uint32_t PpcCrLogicalResident(
    uint32_t cr, uint32_t op, uint32_t bt, uint32_t ba, uint32_t bb) noexcept {
    const auto readBit = [cr](uint32_t index) noexcept {
        return (cr >> (31u - (index & 31u))) & 1u;
    };
    const uint32_t a = readBit(ba);
    const uint32_t b = readBit(bb);
    uint32_t result = 0;
    switch (op & 7u) {
        case 0: result = ~(a | b) & 1u; break;
        case 1: result = a & (~b & 1u); break;
        case 2: result = a ^ b; break;
        case 3: result = ~(a & b) & 1u; break;
        case 4: result = a & b; break;
        case 5: result = ~(a ^ b) & 1u; break;
        case 6: result = a | (~b & 1u); break;
        case 7: result = a | b; break;
    }
    return PpcCrSetBitResident(cr, bt, result);
}

inline uint32_t PpcMcrfResident(uint32_t cr, uint32_t dstField, uint32_t srcField) noexcept {
    dstField &= 7u;
    srcField &= 7u;
    const uint32_t dstShift = (7u - dstField) * 4u;
    const uint32_t srcShift = (7u - srcField) * 4u;
    const uint32_t field = (cr >> srcShift) & 0xFu;
    return (cr & ~(0xFu << dstShift)) | (field << dstShift);
}
