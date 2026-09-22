#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace aurora::gfx {
// Byte counts after each allocation's own trailing alignment, in V/U/I/S order.
using StagingSizes = std::array<uint64_t, 4>;
class StagingCapacityError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
struct StagingBatchFull {};
inline uint64_t staging_padded(uint64_t bytes, uint64_t alignment) {
  if (!bytes) return alignment;
  const auto remainder = alignment ? bytes % alignment : 0;
  const auto padding = remainder ? alignment - remainder : 0;
  if (bytes > UINT64_MAX - padding) throw StagingCapacityError("Staging allocation size overflow");
  return bytes + padding;
}
inline bool staging_fits(const StagingSizes& used, const StagingSizes& demand,
                         const StagingSizes& tail, const StagingSizes& capacity) noexcept {
  for (unsigned i = 0; i < used.size(); ++i) {
    const auto limit = capacity[i] < UINT32_MAX ? capacity[i] : UINT32_MAX;
    // The final GPU copy rounds to four bytes. Subtractions avoid wraparound.
    const auto alignedLimit = limit & ~uint64_t(3);
    if (tail[i] > alignedLimit || used[i] > alignedLimit - tail[i] ||
        demand[i] > alignedLimit - tail[i] - used[i]) return false;
  }
  return true;
}
} // namespace aurora::gfx
