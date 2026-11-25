#include "late_tick_start.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
constexpr uint64_t kUs = 1'000;
constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kInterval = 16'666 * kUs;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool Near(double actual, double expected) { return std::abs(actual - expected) < 1e-6; }

void LeadCoversTheWorkWithAMargin() {
    using LateTickStart::LeadNanos;
    Check(LeadNanos(1.2, 0.0, kInterval) == 3'200 * kUs, "lead = work + 2 ms");
    Check(LeadNanos(0.1, 0.0, kInterval) == 3'000 * kUs, "lead floor is 3 ms");
    Check(LeadNanos(20.0, 0.0, kInterval) == kInterval - 2'000 * kUs, "lead ceiling is the interval less 2 ms");
    Check(LeadNanos(1.2, 2.0, kInterval) == 5'200 * kUs, "a miss penalty widens the lead");
    Check(LeadNanos(1.0, 0.0, 0) == 0, "no interval, no hold");
}

void MissPenaltyGrowsFastAndDecaysSlowly() {
    using LateTickStart::NextMissPenalty;
    Check(Near(NextMissPenalty(0.0, true), 2.0), "a missed retrace adds 2 ms");
    Check(Near(NextMissPenalty(7.0, true), 8.0), "the penalty stops at 8 ms");
    Check(Near(NextMissPenalty(2.0, false), 1.95), "each frame on time takes 0.05 ms off");
    Check(Near(NextMissPenalty(0.01, false), 0.0), "it never goes negative");
}

void MeterTimesTickStartToTheFirstWait() {
    LateTickStart::WorkMeter meter;
    double p99 = 0.0;
    for (uint64_t tick = 0; tick < 9; ++tick) {
        const uint64_t start = tick * kInterval;
        meter.OnTickStart(start);
        meter.OnWaitStart(start + 1 * kMs);
        meter.OnWaitStart(start + 9 * kMs);   // later waits in the same tick don't count
    }
    Check(!meter.P99(p99), "fewer than 10 ticks give no estimate");
    meter.OnTickStart(9 * kInterval);
    meter.OnWaitStart(9 * kInterval + 1 * kMs);
    Check(meter.P99(p99) && Near(p99, 1.0), "ten 1 ms ticks give 1 ms");
}

void MeterUsesTheRecentSlowTicks() {
    LateTickStart::WorkMeter meter;
    double p99 = 0.0;
    // 100 ticks: one slow 5 ms tick among 1 ms ones. The window keeps the newest 60.
    for (uint64_t tick = 0; tick < 100; ++tick) {
        const uint64_t start = tick * kInterval;
        meter.OnTickStart(start);
        meter.OnWaitStart(start + (tick == 90 ? 5 : 1) * kMs);
    }
    Check(meter.P99(p99) && Near(p99, 5.0), "a recent slow tick sets the p99");
    for (uint64_t tick = 100; tick < 200; ++tick) {
        const uint64_t start = tick * kInterval;
        meter.OnTickStart(start);
        meter.OnWaitStart(start + 1 * kMs);
    }
    Check(meter.P99(p99) && Near(p99, 1.0), "it ages out of the window");
}

void TickWithoutAWaitIsNotMeasured() {
    LateTickStart::WorkMeter meter;
    double p99 = 0.0;
    for (uint64_t tick = 0; tick < 20; ++tick) {
        meter.OnTickStart(tick * kInterval);
    }
    Check(!meter.P99(p99), "a game that never waits gives no estimate, so nothing is held");
}

} // namespace

int main() {
    LeadCoversTheWorkWithAMargin();
    MissPenaltyGrowsFastAndDecaysSlowly();
    MeterTimesTickStartToTheFirstWait();
    MeterUsesTheRecentSlowTicks();
    TickWithoutAWaitIsNotMeasured();
    if (g_failures != 0) {
        std::printf("late_tick_start_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("late_tick_start_tests: all passed\n");
    return 0;
}
