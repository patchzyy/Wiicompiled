#include "guest_abi/guest_region.h"

#if !defined(MKW_TRANSLATED_GUEST_REGION)
#error "MKW_TRANSLATED_GUEST_REGION comes from the translator's shard manifest (emit-build-shards)"
#endif

static_assert(MKW_TRANSLATED_GUEST_REGION >= 0 && MKW_TRANSLATED_GUEST_REGION < GuestRegion::kCount);

extern "C" const uint8_t kMkwGuestRegion = MKW_TRANSLATED_GUEST_REGION;
