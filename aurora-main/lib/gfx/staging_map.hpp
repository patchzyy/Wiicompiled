#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace aurora::gfx {

enum class BufferMapState { Unmapped, Mapping, Mapped };

// The renderer owns request/reset; Dawn may complete a request on another thread.
// An old callback must never publish readiness for a different staging slot.
class StagingMapState {
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t generation_ = 0;
  BufferMapState state_ = BufferMapState::Unmapped;

public:
  uint64_t request() {
    std::lock_guard lock(mutex_);
    if (state_ != BufferMapState::Unmapped) return 0;
    state_ = BufferMapState::Mapping;
    return ++generation_;
  }

  bool complete(uint64_t generation, BufferMapState state) {
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || state_ != BufferMapState::Mapping) return false;
      state_ = state;
    }
    changed_.notify_all();
    return true;
  }

  void reset() {
    {
      std::lock_guard lock(mutex_);
      ++generation_;
      state_ = BufferMapState::Unmapped;
    }
    changed_.notify_all();
  }

  BufferMapState state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

  void wait_for_progress() {
    std::unique_lock lock(mutex_);
    // ProcessEvents is still serviced between waits for implementations that
    // need it. A spontaneous completion wakes immediately, without polling.
    changed_.wait_for(lock, std::chrono::milliseconds(1),
                      [&] { return state_ != BufferMapState::Mapping; });
  }
};

} // namespace aurora::gfx
