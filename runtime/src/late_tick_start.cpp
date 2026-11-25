#include "late_tick_start.h"

#include <algorithm>
#include <cmath>

namespace LateTickStart {

uint64_t LeadNanos(double workP99Ms, double missPenaltyMs, uint64_t intervalNanos) {
    constexpr double kMarginMs = 2.0;
    constexpr uint64_t kFloorNanos = 3'000'000;
    constexpr uint64_t kHeadroomNanos = 2'000'000;
    if (intervalNanos <= kFloorNanos + kHeadroomNanos) {
        return 0;
    }
    const double leadMs = workP99Ms + missPenaltyMs + kMarginMs;
    const uint64_t lead = leadMs > 0.0 ? static_cast<uint64_t>(leadMs * 1e6 + 0.5) : 0;
    return std::clamp(lead, kFloorNanos, intervalNanos - kHeadroomNanos);
}

double NextMissPenalty(double currentMs, bool missedRetrace) {
    return missedRetrace ? std::min(currentMs + 2.0, 8.0) : std::max(currentMs - 0.05, 0.0);
}

void WorkMeter::OnTickStart(uint64_t ns) {
    m_tickStart = ns;
    m_measured = false;
}

void WorkMeter::OnWaitStart(uint64_t ns) {
    if (m_measured || ns < m_tickStart) {
        return;
    }
    m_measured = true;
    m_samples[m_write] = static_cast<double>(ns - m_tickStart) / 1e6;
    m_write = (m_write + 1) % kWindow;
    m_count = std::min(m_count + 1, kWindow);
}

bool WorkMeter::P99(double& ms) const {
    if (m_count < 10) {
        return false;
    }
    double sorted[kWindow];
    std::copy(m_samples, m_samples + m_count, sorted);
    std::sort(sorted, sorted + m_count);
    ms = sorted[static_cast<int>(std::ceil((m_count - 1) * 0.99))];
    return true;
}

} // namespace LateTickStart
