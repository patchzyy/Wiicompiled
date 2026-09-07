#pragma once

#include <filesystem>
#include <system_error>

// Move without replacing an existing entry. Cross-filesystem regular files are
// staged at the destination before removing the source. On failure the source
// remains; a failed source removal can leave both complete copies.
// The caller must keep the source stable for the duration of the move; NAND HLE
// calls run synchronously on the cooperative guest thread without yielding.
void NandMove(const std::filesystem::path& source, const std::filesystem::path& destination,
              std::error_code& ec);
