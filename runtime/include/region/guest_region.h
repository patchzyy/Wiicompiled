#pragma once
// Guest-address regioning.
//
// The runtime was written against the PAL executable (RMCP01), and every guest address it names -
// the entry points its native overrides replace, the SDK globals its HLE reads and writes, the
// callback and range constants - is spelled as that PAL address. Those PAL addresses are kept
// exactly as written and treated as the *identity* of the thing they name; a per-region table
// then maps each identity to the address it has in the executable actually being built.
//
// Mechanically: MKW_GADDR(801A9E84) pastes onto MKW_G_801A9E84, which the region header defines
// as the address that identity has in this build (0x801A9DE4u for NTSC-U, 0x801A9E84u for PAL).
// MKW_GUEST_FUNC does the same through MKW_F_ for the translated function's symbol. The
// translator resolves the same table when it scans these sources (tools/region/gen_region_headers.py
// writes the headers; Translator.Core/GuestAddressTable.cs reads them).
//
// Which region header applies is decided by the translation project, not by a build flag: the
// translator writes MKW_GUEST_REGION_HEADER into generated/RuntimeConfig.h from the project's
// runtime.guest_address_table, so the runtime always binds to the executable that generated/ was
// translated from. A RuntimeConfig.h written before that field existed can only be a PAL one.
//
// Any PAL address the region header does not define fails to compile ("use of undeclared
// identifier 'MKW_G_xxxxxxxx'"): a region table is complete or the build does not exist.

#include "generated/RuntimeConfig.h"

#if defined(MKW_GUEST_REGION_HEADER)
#include MKW_GUEST_REGION_HEADER
#else
#include "region/rmcp01.h"
#endif

// A guest address, and the C symbol of its translated function, from a PAL identity. The region
// header carries both forms already written out, so each of these is a single token paste and
// what it expands to is visible in that header.
#define MKW_GADDR(pal_hex) MKW_G_##pal_hex
#define MKW_GUEST_FUNC(pal_hex) MKW_F_##pal_hex

#ifndef MKW_REGION_GAME_ID
#error "The guest region header must define MKW_REGION_GAME_ID and the other MKW_REGION_* facts"
#endif
