#pragma once

// Base translations the runtime calls directly, named by PAL identity. runtime/game binds each
// to the translated region's symbol.
//   MKW_GUEST_FUNC(801A0620)(ctx);

#include "guest_abi/cpu_context.h"

#define MKW_GUEST_FUNC(pal) (*::mkw_guest_function_##pal)

#define MKW_GUEST_FUNCTION_IMPORT(pal, ntscU, ntscJ) extern "C" void (*const mkw_guest_function_##pal)(CpuContext*);
#include "guest_function_imports.inc"
#undef MKW_GUEST_FUNCTION_IMPORT
