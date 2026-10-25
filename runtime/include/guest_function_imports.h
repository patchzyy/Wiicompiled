#pragma once

// Base translations the runtime calls directly, named by PAL identity and resolved through the
// function registry on first use.
//   MKW_GUEST_FUNC(801A0620)(ctx);
// A native registered with REGISTER_NATIVE_WRAPPER_AS calls through with MKW_GUEST_NEXT instead,
// so a mod overlay at the same address still runs.

#include "guest_abi/cpu_context.h"
#include "guest_abi/guest_region.h"

namespace GuestFunctions {
using Entry = void (*)(CpuContext*);

// The base translation at a guest address, also where a native or mod overrides it.
Entry BaseTranslation(uint32_t address);

// The translation a native wrapper sits on top of: the winning mod overlay, else the base
// translation.
Entry NextTranslation(uint32_t address);
} // namespace GuestFunctions

#define MKW_GUEST_FUNC(pal)                                                                        \
    (*[] {                                                                                         \
        static const ::GuestFunctions::Entry entry = ::GuestFunctions::BaseTranslation(MKW_GADDR(pal)); \
        return entry;                                                                              \
    }())

#define MKW_GUEST_NEXT(pal)                                                                        \
    (*[] {                                                                                         \
        static const ::GuestFunctions::Entry entry = ::GuestFunctions::NextTranslation(MKW_GADDR(pal)); \
        return entry;                                                                              \
    }())
