#include "guest_abi/guest_region.h"

#include <cstdio>
#include <cstdlib>

namespace GuestRegion {

void FatalUnknownAddress(uint32_t pal) noexcept {
    std::fprintf(stderr, "Guest region: PAL address 0x%08X has no %s address in guest_address_table.inc\n",
                 pal, CurrentFacts().gameId);
    std::fflush(stderr);
    std::abort();
}

} // namespace GuestRegion
