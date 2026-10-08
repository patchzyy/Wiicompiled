#pragma once
#include "../core/common.h"

namespace sonic {
// Sega PRS (LZ77 variant) decompression. Used by *.PRS files and some
// compressed archives.
bool prsDecompress(const u8* src, size_t srcLen, std::vector<u8>& out);
}
