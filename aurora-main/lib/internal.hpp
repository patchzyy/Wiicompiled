#pragma once

#include "logging.hpp" // IWYU pragma: keep

#include <aurora/aurora.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

using namespace std::string_view_literals;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#ifndef SBIG
#define SBIG(q)                                                                                                        \
  (((q) & 0x000000FF) << 24 | ((q) & 0x0000FF00) << 8 | ((q) & 0x00FF0000) >> 8 | ((q) & 0xFF000000) >> 24)
#endif
#else
#ifndef SBIG
#define SBIG(q) (q)
#endif
#endif

template <typename T>
  requires(sizeof(T) == sizeof(uint16_t) && std::is_arithmetic_v<T>)
constexpr T bswap(T val) noexcept {
  union {
    uint16_t u;
    T t;
  } v{.t = val};
#if __GNUC__
  v.u = __builtin_bswap16(v.u);
#elif _WIN32
  v.u = _byteswap_ushort(v.u);
#else
  v.u = (v.u << 8) | ((v.u >> 8) & 0xFF);
#endif
  return v.t;
}

template <typename T>
  requires(sizeof(T) == sizeof(uint32_t) && std::is_arithmetic_v<T>)
constexpr T bswap(T val) noexcept {
  union {
    uint32_t u;
    T t;
  } v{.t = val};
#if __GNUC__
  v.u = __builtin_bswap32(v.u);
#elif _WIN32
  v.u = _byteswap_ulong(v.u);
#else
  v.u = ((v.u & 0x0000FFFF) << 16) | ((v.u & 0xFFFF0000) >> 16) | ((v.u & 0x00FF00FF) << 8) | ((v.u & 0xFF00FF00) >> 8);
#endif
  return v.t;
}

template <typename T>
  requires(sizeof(T) == sizeof(uint64_t) && std::is_arithmetic_v<T>)
constexpr T bswap(T val) noexcept {
  union {
    uint64_t u;
    T t;
  } v{.t = val};
#if __GNUC__
  v.u = __builtin_bswap64(v.u);
#elif _WIN32
  v.u = _byteswap_uint64(v.u);
#else
  static_assert(false, "bswap 64bit not implemented on this target");
#endif
  return v.t;
}

template <typename T>
  requires(std::is_enum_v<T>)
auto underlying(T value) -> std::underlying_type_t<T> {
  return static_cast<std::underlying_type_t<T>>(value);
}

#define AURORA_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define POINTER_ADD_TYPE(type_, ptr_, offset_) ((type_)((uintptr_t)(ptr_) + (uintptr_t)(offset_)))
#define POINTER_ADD(ptr_, offset_) POINTER_ADD_TYPE(decltype(ptr_), ptr_, offset_)

#if !defined(__has_cpp_attribute)
#define __has_cpp_attribute(name) 0
#endif
#if __has_cpp_attribute(unlikely)
#define UNLIKELY [[unlikely]]
#else
#define UNLIKELY
#endif
#if __has_cpp_attribute(likely)
#define LIKELY [[likely]]
#else
#define LIKELY
#endif
#define FATAL(msg, ...) Log.fatal(msg, ##__VA_ARGS__);
#define ASSERT(cond, msg, ...)                                                                                         \
  if (!(cond))                                                                                                         \
  UNLIKELY FATAL(msg, ##__VA_ARGS__)
#ifdef NDEBUG
#define CHECK(cond, msg, ...)
#else
#define CHECK(cond, msg, ...) ASSERT(cond, msg, ##__VA_ARGS__)
#endif
#define DEFAULT_FATAL(msg, ...) UNLIKELY default : FATAL(msg, ##__VA_ARGS__)
#define TRY(cond, msg, ...)                                                                                            \
  if (!(cond))                                                                                                         \
    UNLIKELY {                                                                                                         \
      Log.error(msg, ##__VA_ARGS__);                                                                                   \
      return false;                                                                                                    \
    }
#define TRY_WARN(cond, msg, ...)                                                                                       \
  if (!(cond))                                                                                                         \
    UNLIKELY { Log.warn(msg, ##__VA_ARGS__); }

#define UNIMPLEMENTED() FATAL("UNIMPLEMENTED: {}", __FUNCTION__)

namespace wgpu { class CommandBuffer; }

namespace aurora {
void submit_staging_commands(const wgpu::CommandBuffer& commands);
extern AuroraConfig g_config;
extern uint32_t g_sdlCustomEventsStart;
extern char g_gameName[4];

// wait_for_frame_worker() joins the DONE phase (ImGui, surface, sealing another frame need this).
// wait_for_frame_worker_sealed() joins only SEALED, which is what the FIFO drain uses.
void wait_for_frame_worker() noexcept;
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept;
bool wait_for_frame_worker_for(std::chrono::microseconds timeout) noexcept;
std::recursive_mutex& renderer_gpu_mutex() noexcept;

template <typename T>
class ArrayRef {
public:
  using value_type = std::remove_cvref_t<T>;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using reference = value_type&;
  using const_reference = const value_type&;
  using iterator = const_pointer;
  using const_iterator = const_pointer;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  ArrayRef() = default;
  explicit ArrayRef(const T& one) : ptr(&one), length(1) {}
  ArrayRef(const T* data, size_t length) : ptr(data), length(length) {}
  ArrayRef(const T* begin, const T* end) : ptr(begin), length(end - begin) {}
  template <size_t N>
  constexpr ArrayRef(const T (&arr)[N]) : ptr(arr), length(N) {}
  template <size_t N>
  constexpr ArrayRef(const std::array<T, N>& arr) : ptr(arr.data()), length(arr.size()) {}
  ArrayRef(const std::vector<T>& vec) : ptr(vec.data()), length(vec.size()) {}

  const T* data() const { return ptr; }
  size_t size() const { return length; }
  bool empty() const { return length == 0; }

  const T& front() const {
    assert(!empty());
    return ptr[0];
  }
  const T& back() const {
    assert(!empty());
    return ptr[length - 1];
  }
  const T& operator[](size_t i) const {
    assert(i < length && "Invalid index!");
    return ptr[i];
  }

  iterator begin() const { return ptr; }
  iterator end() const { return ptr + length; }

  reverse_iterator rbegin() const { return reverse_iterator(end()); }
  reverse_iterator rend() const { return reverse_iterator(begin()); }

  /// Disallow accidental assignment from a temporary.
  template <typename U>
  std::enable_if_t<std::is_same<U, T>::value, ArrayRef<T>>& operator=(U&& Temporary) = delete;

  /// Disallow accidental assignment from a temporary.
  template <typename U>
  std::enable_if_t<std::is_same<U, T>::value, ArrayRef<T>>& operator=(std::initializer_list<U>) = delete;

private:
  const T* ptr = nullptr;
  size_t length = 0;
};
} // namespace aurora
