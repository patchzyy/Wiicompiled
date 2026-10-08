#include "common.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace sonic {

static std::function<void(LogLevel, const char*)> g_handler;
static std::mutex g_logMutex;

void setLogHandler(std::function<void(LogLevel, const char*)> handler) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_handler = std::move(handler);
}

void logf(LogLevel lvl, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_handler) {
        g_handler(lvl, buf);
        return;
    }
    const char* tag = lvl == LogLevel::Info ? "[sonic] " : lvl == LogLevel::Warn ? "[sonic warn] " : lvl == LogLevel::Error ? "[sonic error] " : "[sonic debug] ";
    fprintf(stdout, "%s%s\n", tag, buf);
    fflush(stdout);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

// Paths are UTF-8 everywhere (u8path), so folders with non-ASCII names work on Windows too.
bool readFile(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize(size_t(n));
    return n == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(path), ec);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string findFileNoCase(const std::string& dir, const std::string& name) {
    std::string direct = joinPath(dir, name);
    if (fileExists(direct)) return direct;
    std::error_code ec;
    std::string want = toLower(name);
    for (auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (toLower(e.path().filename().u8string()) == want) return e.path().u8string();
    }
    return "";
}

}  // namespace sonic
