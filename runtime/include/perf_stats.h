#pragma once

#include <chrono>

// MKW_PERF_STATS=1: logs the game thread's busy time per presented frame, i.e. wall time minus the
// time it spends blocked on VI pacing, the GX worker or the audio mixer.
namespace PerfStats {

bool Enabled() noexcept;
void AddWait(std::chrono::steady_clock::duration wait) noexcept;
void OnFramePresented() noexcept;

class WaitScope {
public:
    WaitScope() noexcept : m_start(Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~WaitScope() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            AddWait(std::chrono::steady_clock::now() - m_start);
        }
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    std::chrono::steady_clock::time_point m_start;
};

} // namespace PerfStats
