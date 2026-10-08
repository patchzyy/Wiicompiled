// SonicCore - Sonic Adventure DX's Sonic as a reusable C++ module.
// Licensed under the MIT license. See LICENSE.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace sonic {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

enum class LogLevel { Info, Warn, Error, Debug };
void logf(LogLevel lvl, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
// Route SonicCore's log lines into the host engine (default: stdout).
void setLogHandler(std::function<void(LogLevel, const char*)> handler);

#define SONIC_LOGI(...) ::sonic::logf(::sonic::LogLevel::Info, __VA_ARGS__)
#define SONIC_LOGW(...) ::sonic::logf(::sonic::LogLevel::Warn, __VA_ARGS__)
#define SONIC_LOGE(...) ::sonic::logf(::sonic::LogLevel::Error, __VA_ARGS__)
#define SONIC_LOGD(...) ::sonic::logf(::sonic::LogLevel::Debug, __VA_ARGS__)

template <typename T>
inline T rd(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
inline u16 be16(const u8* p) { return u16((p[0] << 8) | p[1]); }
inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

std::string toLower(std::string s);
std::string toUpper(std::string s);

// files
bool readFile(const std::string& path, std::vector<u8>& out);
bool fileExists(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
// Case-insensitive lookup of `name` inside `dir` (game folders differ in case). "" if absent.
std::string findFileNoCase(const std::string& dir, const std::string& name);

}  // namespace sonic
