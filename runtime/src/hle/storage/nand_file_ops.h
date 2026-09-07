#pragma once

#include <filesystem>
#include <system_error>

// Move without replacing an existing entry. Cross-filesystem regular files are
// staged at the destination before removing the source. On failure the source
// remains; a failed source removal can leave both complete copies.
void NandMove(const std::filesystem::path& source, const std::filesystem::path& destination,
              std::error_code& ec);
