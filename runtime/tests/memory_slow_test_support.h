#pragma once

#include "guest_abi/memory.h"

// Leave fast mappings empty so scalar accesses use each harness's bounded store.
namespace MemoryInline {
PageEntry g_pageTable[kPageCount]{};
uintptr_t g_fullPageBias[kPageCount]{};
uintptr_t g_fullReadablePageBias[kPageCount]{};
uintptr_t g_fullWritablePageBias[kPageCount]{};
const SparseWritablePageTable* g_sparseWritablePageTables[kPageCount]{};
uint8_t g_deferredReadCoveredPages[kPageCount]{};
}

namespace RecompMod {
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
}
