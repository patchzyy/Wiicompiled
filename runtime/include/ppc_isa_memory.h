#pragma once

// Host memory seam for isa/ppc_isa_quantized.h. That header includes
// "ppc_isa_memory.h" by plain quoted name and expects whatever it resolves to
// to supply the Memory / MemoryInline APIs and the flat-guest-base macros its
// psq fast paths use. Part of the guest ABI, so it resolves to guest_abi/ only.

#include "guest_abi/memory.h"
