#include "perf_stats.h"

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace PerfStats {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kWindow = 240;

thread_local Clock::duration t_wait{};

struct FrameWindow {
    Clock::time_point last{};
    std::array<double, kWindow> busyMs{};
    size_t count = 0;
};

FrameWindow g_window;

} // namespace

bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_PERF_STATS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void AddWait(Clock::duration wait) noexcept { t_wait += wait; }

void OnFramePresented() noexcept {
    if (!Enabled()) {
        return;
    }
    const auto now = Clock::now();
    const Clock::duration wait = t_wait;
    t_wait = {};
    if (g_window.last == Clock::time_point{}) {
        g_window.last = now;
        return;
    }
    const auto busy = (now - g_window.last) - wait;
    g_window.last = now;
    g_window.busyMs[g_window.count++] = std::chrono::duration<double, std::milli>(busy).count();
    if (g_window.count < kWindow) {
        return;
    }
    std::array<double, kWindow> sorted = g_window.busyMs;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double ms : sorted) {
        sum += ms;
    }
    RT_LOGF("perf", "busy ms/frame over %zu frames: avg %.3f median %.3f p95 %.3f max %.3f\n", kWindow,
            sum / kWindow, sorted[kWindow / 2], sorted[kWindow * 95 / 100], sorted[kWindow - 1]);
    g_window.count = 0;
}

} // namespace PerfStats
