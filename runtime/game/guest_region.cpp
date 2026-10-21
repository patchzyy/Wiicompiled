#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"

#if !defined(MKW_TRANSLATED_GUEST_REGION)
#error "MKW_TRANSLATED_GUEST_REGION comes from the translator's shard manifest (emit-build-shards)"
#endif

static_assert(MKW_TRANSLATED_GUEST_REGION >= 0 && MKW_TRANSLATED_GUEST_REGION < GuestRegion::kCount);

extern "C" const uint8_t kMkwGuestRegion = MKW_TRANSLATED_GUEST_REGION;

#define MKW_DEFINE_GUEST_FUNCTION_IMPORT(pal, symbol)                                             \
    extern "C" void func_##symbol(CpuContext*);                                                  \
    extern "C" void (*const mkw_guest_function_##pal)(CpuContext*) = &func_##symbol;             \
    static_assert(MKW_GADDR_CT(pal) == 0x##symbol##u, "guest_function_imports.inc disagrees with " \
                                                       "guest_address_table.inc");

#if MKW_TRANSLATED_GUEST_REGION == 0
#define MKW_GUEST_FUNCTION_IMPORT(pal, ntscU, ntscJ) MKW_DEFINE_GUEST_FUNCTION_IMPORT(pal, pal)
#elif MKW_TRANSLATED_GUEST_REGION == 1
#define MKW_GUEST_FUNCTION_IMPORT(pal, ntscU, ntscJ) MKW_DEFINE_GUEST_FUNCTION_IMPORT(pal, ntscU)
#else
#define MKW_GUEST_FUNCTION_IMPORT(pal, ntscU, ntscJ) MKW_DEFINE_GUEST_FUNCTION_IMPORT(pal, ntscJ)
#endif
#include "guest_function_imports.inc"
#undef MKW_GUEST_FUNCTION_IMPORT
