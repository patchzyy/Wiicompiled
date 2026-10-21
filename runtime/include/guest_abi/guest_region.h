#pragma once

// Guest addresses are named by their PAL (RMCP01) address. Which address that identity has
// depends on the region the game was translated from, which runtime/game supplies at link
// time, so one compiled runtime serves every region.
//
//   MKW_GADDR(801A9E84)       address of that identity in the running region
//   MKW_GADDR_CT(801A9E84)    same, as a constant expression; translated code only
//
// A PAL address missing from guest_address_table.inc does not compile; regenerate the table
// with tools/region/gen_guest_address_table.py.

#include <cstddef>
#include <cstdint>

namespace GuestRegion {

enum class Id : uint8_t {
    Pal = 0,
    NtscU = 1,
    NtscJ = 2,
};
inline constexpr size_t kCount = 3;

struct Row {
    uint32_t address[kCount];

    constexpr uint32_t In(Id region) const { return address[static_cast<size_t>(region)]; }
};

inline constexpr Row kRows[] = {
#define MKW_GUEST_ADDRESS(pal, ntscU, ntscJ) {{0x##pal##u, 0x##ntscU##u, 0x##ntscJ##u}},
#include "guest_abi/guest_address_table.inc"
#undef MKW_GUEST_ADDRESS
};
inline constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

constexpr bool RowsAreSorted() {
    for (size_t i = 1; i < kRowCount; ++i) {
        if (kRows[i - 1].address[0] >= kRows[i].address[0]) {
            return false;
        }
    }
    return true;
}
static_assert(RowsAreSorted(), "guest_address_table.inc must be sorted by PAL address without duplicates");

constexpr const Row* Find(uint32_t pal) {
    size_t lo = 0;
    size_t hi = kRowCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (kRows[mid].address[0] < pal) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < kRowCount && kRows[lo].address[0] == pal ? &kRows[lo] : nullptr;
}

constexpr const Row& RowOf(uint32_t pal) {
    return Find(pal) ? *Find(pal) : throw "PAL address missing from guest_abi/guest_address_table.inc";
}

} // namespace GuestRegion

// Defined by runtime/game for the translated region.
extern "C" const uint8_t kMkwGuestRegion;

namespace GuestRegion {

inline Id Current() noexcept { return static_cast<Id>(kMkwGuestRegion); }

template <uint32_t Pal>
inline uint32_t Address() noexcept {
    constexpr Row row = RowOf(Pal);
    if constexpr (row.address[1] == Pal && row.address[2] == Pal) {
        return Pal;
    } else {
        return row.In(Current());
    }
}

// Folds to false at compile time for a constant target that no region uses for Pal.
template <uint32_t Pal>
inline bool Is(uint32_t target) noexcept {
    constexpr Row row = RowOf(Pal);
    return (target == row.address[0] || target == row.address[1] || target == row.address[2]) &&
           target == row.In(Current());
}

[[noreturn]] void FatalUnknownAddress(uint32_t pal) noexcept;

// For hand-written registrations, which name the PAL address.
inline uint32_t Translate(uint32_t pal) noexcept {
    const Id region = Current();
    if (region == Id::Pal) {
        return pal;
    }
    const Row* row = Find(pal);
    if (!row) {
        FatalUnknownAddress(pal);
    }
    return row->In(region);
}

struct Facts {
    const char* gameId;
    char letter;
    uint32_t gameCode;
    uint32_t viTvFormat;
    uint32_t mem1ArenaLo;
    // setting.txt defaults for a new NAND
    const char* area;
    const char* model;
    const char* productCode;
    const char* video;
    const char* gameRegion;
};

inline constexpr Facts kFacts[kCount] = {
    {"RMCP01", 'P', 0x524D4350u, 1u, 0x80399180u, "EUR", "RVL-001(EUR)", "LEH", "PAL", "EU"},
    {"RMCE01", 'E', 0x524D4345u, 0u, 0x80394E00u, "USA", "RVL-001(USA)", "LU", "NTSC", "US"},
    {"RMCJ01", 'J', 0x524D434Au, 0u, 0x80398B00u, "JPN", "RVL-001(JPN)", "LJ", "NTSC", "JP"},
};

inline const Facts& CurrentFacts() noexcept { return kFacts[static_cast<size_t>(Current())]; }

} // namespace GuestRegion

#define MKW_GADDR(pal) (::GuestRegion::Address<0x##pal##u>())

#if defined(MKW_TRANSLATED_GUEST_REGION)
#define MKW_GADDR_CT(pal) \
    (::GuestRegion::RowOf(0x##pal##u).In(static_cast<::GuestRegion::Id>(MKW_TRANSLATED_GUEST_REGION)))
#endif
