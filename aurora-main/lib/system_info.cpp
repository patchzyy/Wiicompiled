#include "system_info.hpp"
#include "internal.hpp"

#if _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <dxgi.h>

template<typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ~ComPtr() { reset(); }

  T* Get() const { return value; }
  T* operator->() const { return value; }
  T** Put() {
    reset();
    return &value;
  }

private:
  void reset() {
    if (value != nullptr) {
      value->Release();
      value = nullptr;
    }
  }

  T* value = nullptr;
};
typedef LONG NTSTATUS, *PNTSTATUS;
extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOEXW lpVersionInformation);
#elif __APPLE__
#include "sys/sysctl.h"
#elif linux
#include <ranges>
#include <fstream>
#include <filesystem>
#include <sys/sysinfo.h>
#endif


using namespace std::string_literals;

namespace aurora {

static constexpr auto Unknown = "Unknown";

static Module Log("aurora::system_info");

static std::string GetOSVersion();
static std::string GetCpuModel();
static uint64_t GetMemoryAmount();
static void LogMisc();

void log_system_information() {
  Log.info("CPU model: {}", GetCpuModel());
  const auto memSize = GetMemoryAmount() / 1024 / 1024;
  Log.info("Memory: {} MiB", memSize);

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_X64)
  Log.info("Architecture: x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
  Log.info("Architecture: aarch64");
#else
  Log.info("Architecture: Unknown");
#endif

  Log.info("OS: {}", GetOSVersion());
  LogMisc();
}

#if _WIN32
static std::string wideStringToUtf8(std::wstring_view str) {
  const auto size = WideCharToMultiByte(
    CP_UTF8,
    0,
    str.data(),
    static_cast<int>(str.size()),
    nullptr,
    0,
    nullptr,
    nullptr
    );

  std::string result{};
  result.resize(size);

  WideCharToMultiByte(
    CP_UTF8,
    0,
    str.data(),
    static_cast<int>(str.size()),
    result.data(),
    static_cast<int>(result.size()),
    nullptr,
    nullptr
    );

  return result;
}

std::string GetCpuModel() {
  HKEY key = nullptr;
  const auto opened = RegOpenKeyExW(
    HKEY_LOCAL_MACHINE,
    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
    0,
    KEY_QUERY_VALUE,
    &key);
  if (opened != ERROR_SUCCESS) {
    Log.error("Unable to open the processor registry key");
    return Unknown;
  }

  wchar_t value[256]{};
  DWORD type = 0;
  DWORD size = sizeof(value);
  const auto queried = RegQueryValueExW(
    key,
    L"ProcessorNameString",
    nullptr,
    &type,
    reinterpret_cast<BYTE*>(value),
    &size);
  RegCloseKey(key);
  if (queried != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    Log.error("Unable to read the processor name");
    return Unknown;
  }

  const auto length = wcsnlen(value, sizeof(value) / sizeof(value[0]));
  return wideStringToUtf8(std::wstring_view(value, length));
}

uint64_t GetMemoryAmount() {
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  GlobalMemoryStatusEx(&status);

  return status.ullTotalPhys;
}

std::string GetOSVersion() {
  RTL_OSVERSIONINFOEXW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  auto result = RtlGetVersion(&info);
  if (result != 0) {
    Log.error("RtlGetVersion failed");
    return Unknown;
  }

  return fmt::format(
    "Microsoft Windows {}.{} build {}",
    info.dwMajorVersion,
    info.dwMinorVersion,
    info.dwBuildNumber);
}

static void LogGpus() {
  ComPtr<IDXGIFactory1> factory;
  auto result = CreateDXGIFactory1(
    __uuidof(IDXGIFactory1),
    reinterpret_cast<void**>(factory.Put()));
  if (FAILED(result)) {
    Log.error("Unable to create IDXGIFactory1");
    return;
  }

  for (UINT i = 0;;i++) {
    ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(i, adapter.Put());
    if (result == DXGI_ERROR_NOT_FOUND)
      break;

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);

    std::wstring descName(desc.Description, wcsnlen(desc.Description, 128));
    Log.info("Detected GPU: {}", wideStringToUtf8(descName));
  }
}

void LogMisc() {
  LogGpus();
}

#elif __APPLE__

static std::string sysCtlToString(const char* name) {
  size_t length;
  auto err = sysctlbyname(name, nullptr, &length, nullptr, 0);
  if (err) {
    Log.error("sysctlbyname failed: %d", err);
    return Unknown;
  }

  std::string value;
  value.resize(length);
  err = sysctlbyname(name, value.data(), &length, nullptr, 0);
  if (err) {
    Log.error("second sysctlbyname failed: %d", err);
    return Unknown;
  }

  if (value[length - 1] == '\0')
    value.pop_back();

  return value;
}

std::string GetCpuModel() {
  return sysCtlToString("machdep.cpu.brand_string");
}

uint64_t GetMemoryAmount() {
  uint64_t result;
  size_t size = sizeof(result);
  const auto err = sysctlbyname("hw.memsize", &result, &size, nullptr, 0);
  if (err) {
    Log.error("sysctlbyname failed: %d", err);
    return 0;
  }

  return result;
}

std::string GetOSVersion() {
#if TARGET_OS_MAC
  constexpr auto name = "macOS";
#elif TARGET_OS_IOS
  constexpr auto name = "iOS";
#elif TARGET_OS_TV
  constexpr auto name = "tvOS";
#else
  constexpr auto name = Unknown;
#endif

  return fmt::format("{} {}", name, system_info::getSystemVersionString());
}

void LogMisc() {
  // Nada.
}
#elif linux

// https://stackoverflow.com/questions/216823/how-can-i-trim-a-stdstring
static void ltrim(std::string &s) {
  s.erase(s.begin(), std::ranges::find_if(s.begin(), s.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
}
static void rtrim(std::string &s) {
    s.erase(std::ranges::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}
inline std::string trim(std::string str) {
  ltrim(str);
  rtrim(str);
  return std::move(str);
}

std::string GetCpuModel() {
  std::ifstream cpuInfo("/proc/cpuinfo");
  if (!cpuInfo)
  {
    Log.error("Failed to open /proc/cpuinfo");
    return Unknown;
  }

  while (!cpuInfo.bad() && !cpuInfo.eof()) {
    std::string line;
    std::getline(cpuInfo, line);

    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    auto left = trim(line.substr(0, colon));
    auto right = trim(line.substr(colon + 1));

    if (left == "model name") {
      return right;
    }
  }

  return Unknown;
}

uint64_t GetMemoryAmount() {
  struct sysinfo info{};
  sysinfo(&info);

  return info.totalram;
}

std::string GetOSVersion() {
  auto path = "/etc/os-release";
  if (!std::filesystem::exists(path)) {
    path = "/usr/lib/os-release";
  }

  std::ifstream releaseInfo(path);
  if (!releaseInfo)
  {
    Log.error("Failed to open /etc/os-release or /usr/lib/os-release");
    return Unknown;
  }

  std::string name, version;

  while (!releaseInfo.bad() && !releaseInfo.eof()) {
    std::string line;
    std::getline(releaseInfo, line);

    const auto split = line.find('=');
    if (split == std::string::npos) {
      continue;
    }

    auto left = trim(line.substr(0, split));
    auto right = trim(line.substr(split + 1));

    if (right[0] == '"' && right[right.size()-1] == '"') {
      right = right.substr(1, right.size()-2);
    }

    if (left == "NAME") {
      name = right;
    } else if (left == "VERSION") {
      version = right;
    }
  }

  if (name.empty()) {
    return Unknown;
  }

  return fmt::format("{} {}", name, version);
}

void LogMisc() {

}
#else
std::string GetCpuModel() {
  return Unknown;
}

uint64_t GetMemoryAmount() {
  return 0;
}

std::string GetOSVersion() {
  return Unknown;
}

void LogMisc() {

}
#endif

} // namespace aurora
